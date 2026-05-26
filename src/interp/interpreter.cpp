#include "interp/interpreter.hpp"
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include "analysis/cfg.hpp"
#include "analysis/type_utils.hpp"
#include "error.hpp"
#include "frontend/diagnostics.hpp"

namespace symir {

  // Enforce IEEE 754 finite-only semantics (spec §7.4 rules 6–7):
  // truncate to f32 if needed, then reject infinity or NaN.
  static double checkFPResult(double val, uint32_t bits) {
    if (bits == 32)
      val = static_cast<double>(static_cast<float>(val));
    if (std::isinf(val))
      throw UndefinedBehaviorError("UB: Floating-point result is infinity");
    if (std::isnan(val))
      throw UndefinedBehaviorError("UB: Floating-point result is NaN");
    return val;
  }

  Interpreter::Interpreter(const Program &prog) : prog_(prog) {
    for (const auto &s: prog_.structs) {
      structs_[s.name.name] = &s;
    }
  }

  // ---- Memory helpers ----

  std::uint64_t Interpreter::sizeofType(const TypePtr &t) const {
    if (!t)
      return 8;
    if (auto it = std::get_if<IntType>(&t->v)) {
      uint32_t bits = it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64);
      return (bits + 7) / 8;
    }
    if (auto ft = std::get_if<FloatType>(&t->v)) {
      return ft->kind == FloatType::Kind::F32 ? 4 : 8;
    }
    if (std::holds_alternative<PtrType>(t->v))
      return 8;
    if (auto at = std::get_if<ArrayType>(&t->v)) {
      return at->size * sizeofType(at->elem);
    }
    if (auto st = std::get_if<StructType>(&t->v)) {
      auto it = structs_.find(st->name.name);
      if (it == structs_.end())
        return 8;
      uint64_t total = 0;
      for (const auto &f: it->second->fields)
        total += sizeofType(f.type);
      return total;
    }
    return 8;
  }

  // Allocate (or return existing) base address for varName with type t.
  // Syncs the current store value into the heap.
  std::uint64_t
  Interpreter::allocObject(const std::string &varName, const TypePtr &t, const Store &store) {
    auto it = addrMap_.find(varName);
    if (it != addrMap_.end())
      return it->second;

    uint64_t base = nextAddr_;
    uint64_t totalSize = sizeofType(t);
    // Align allocation to 8 bytes
    nextAddr_ += (totalSize + 7) & ~7ULL;

    uint64_t elemSize =
        sizeofType(std::get_if<ArrayType>(&t->v) ? std::get<ArrayType>(t->v).elem : t);
    uint64_t count = std::get_if<ArrayType>(&t->v) ? std::get<ArrayType>(t->v).size : 1;
    objects_.push_back(ObjectInfo{varName, "", base, base + totalSize, elemSize, count});
    addrMap_[varName] = base;

    // Sync current store value into heap. For arrays-of-structs the
    // element storage isn't itself a flat scalar — we recurse so that
    // every leaf field has its own per-byte heap entry and ObjectInfo.
    // This lets `ptrfield %p, f` for `%p = addr %arr[k]` resolve to a
    // valid object-bound load address even when %p has been advanced
    // via pointer arithmetic.
    auto sit = store.find(varName);
    if (sit != store.end()) {
      const RuntimeValue &sv = sit->second;
      if (sv.kind == RuntimeValue::Kind::Array) {
        auto at = std::get_if<ArrayType>(&t->v);
        auto elemTy = at ? at->elem : t;
        bool elemIsStruct = elemTy && std::holds_alternative<StructType>(elemTy->v);
        const StructDecl *sd = nullptr;
        if (elemIsStruct) {
          auto sIt = structs_.find(std::get<StructType>(elemTy->v).name.name);
          if (sIt != structs_.end())
            sd = sIt->second;
        }
        for (std::size_t i = 0; i < sv.arrayVal.size(); ++i) {
          uint64_t elemBase = base + i * elemSize;
          if (sd) {
            // Per-element struct: create a whole-element ObjectInfo (for
            // ptrfield provenance = the struct) plus per-field ObjectInfos.
            uint64_t off = 0;
            uint64_t minFS = elemSize;
            for (const auto &f: sd->fields) {
              uint64_t fs = sizeofType(f.type);
              if (fs < minFS)
                minFS = fs;
            }
            objects_.push_back(
                ObjectInfo{varName, "", elemBase, elemBase + elemSize, minFS, elemSize / minFS, i}
            );
            for (const auto &f: sd->fields) {
              uint64_t fSize = sizeofType(f.type);
              objects_.push_back(
                  ObjectInfo{varName, f.name, elemBase + off, elemBase + off + fSize, fSize, 1, i}
              );
              if (sv.arrayVal[i].kind == RuntimeValue::Kind::Struct) {
                auto fit = sv.arrayVal[i].structVal.find(f.name);
                if (fit != sv.arrayVal[i].structVal.end())
                  heap_[elemBase + off] = fit->second;
              }
              off += fSize;
            }
          } else {
            heap_[elemBase] = sv.arrayVal[i];
          }
        }
      } else {
        heap_[base] = sv;
      }
    }
    return base;
  }

  const Interpreter::ObjectInfo *Interpreter::findObject(std::uint64_t addr) const {
    const ObjectInfo *best = nullptr;
    for (const auto &o: objects_) {
      if (addr >= o.base && addr < o.end) {
        if (!best) {
          best = &o;
        } else {
          uint64_t oSize = o.end - o.base;
          uint64_t bestSize = best->end - best->base;
          if (oSize < bestSize) {
            best = &o;
          } else if (oSize == bestSize && best->fieldName.empty() && !o.fieldName.empty()) {
            best = &o;
          }
        }
      }
    }
    return best;
  }

  // Like findObject, but also matches the one-past-the-end address (valid for arithmetic).
  // Two-pass: interior membership wins over one-past-the-end so that consecutive
  // allocations (where src.end == dst.base) are unambiguous.
  const Interpreter::ObjectInfo *Interpreter::findObjectForArith(std::uint64_t addr) const {
    for (const auto &o: objects_)
      if (addr >= o.base && addr < o.end)
        return &o;
    for (const auto &o: objects_)
      if (addr == o.end)
        return &o;
    return nullptr;
  }

  // Find an ObjectInfo by exact base address — used for provenance-based lookups.
  const Interpreter::ObjectInfo *Interpreter::findObjectByBase(std::uint64_t base) const {
    for (const auto &o: objects_)
      if (o.base == base)
        return &o;
    return nullptr;
  }

  // Byte offset of named field within struct s (sequential layout, no padding).
  std::uint64_t Interpreter::fieldOffset(const StructDecl &s, const std::string &fieldName) const {
    uint64_t offset = 0;
    for (const auto &f: s.fields) {
      if (f.name == fieldName)
        return offset;
      offset += sizeofType(f.type);
    }
    throw std::runtime_error("Internal: field '" + fieldName + "' not found in struct");
  }

  // Materialize a struct variable into the heap: allocate one ObjectInfo per field
  // with provenance [fieldBase, fieldBase+sizeof(T)).  Idempotent (no-op if already done).
  std::uint64_t Interpreter::materializeStruct(
      const std::string &varName, const StructDecl &s, const Store &store
  ) {
    auto it = addrMap_.find(varName);
    if (it != addrMap_.end())
      return it->second;

    // Compute total size (sequential field layout, no padding).
    uint64_t totalSize = 0;
    for (const auto &f: s.fields)
      totalSize += sizeofType(f.type);

    uint64_t base = nextAddr_;
    nextAddr_ += (totalSize + 7) & ~7ULL;
    addrMap_[varName] = base;

    // [v0.2.1] Create a whole-struct ObjectInfo so that ptrfield-derived
    // pointers (whose provenance = the struct per rule 15) can roam over
    // the entire struct range. The elemSize is set to the smallest field
    // size so ptr arith steps by the right granularity.
    uint64_t minFieldSize = totalSize;
    for (const auto &f: s.fields) {
      uint64_t fs = sizeofType(f.type);
      if (fs < minFieldSize)
        minFieldSize = fs;
    }
    objects_.push_back(
        ObjectInfo{varName, "", base, base + totalSize, minFieldSize, totalSize / minFieldSize}
    );

    // Create one ObjectInfo per field and sync its value into the heap.
    uint64_t offset = 0;
    auto sv = store.find(varName);
    for (const auto &f: s.fields) {
      uint64_t fBase = base + offset;
      uint64_t fElemSize;
      uint64_t fCount;
      if (auto at = std::get_if<ArrayType>(&f.type->v)) {
        fCount = at->size;
        fElemSize = sizeofType(at->elem);
      } else {
        fCount = 1;
        fElemSize = sizeofType(f.type);
      }
      uint64_t fSize = fCount * fElemSize;
      objects_.push_back(ObjectInfo{varName, f.name, fBase, fBase + fSize, fElemSize, fCount});

      if (sv != store.end() && sv->second.kind == RuntimeValue::Kind::Struct) {
        auto fit = sv->second.structVal.find(f.name);
        if (fit != sv->second.structVal.end()) {
          if (fit->second.kind == RuntimeValue::Kind::Array) {
            for (size_t i = 0; i < fit->second.arrayVal.size(); ++i)
              heap_[fBase + i * fElemSize] = fit->second.arrayVal[i];
          } else {
            heap_[fBase] = fit->second;
          }
        }
      }
      offset += fSize;
    }
    return base;
  }

  void Interpreter::run(
      const std::string &entryFuncName, const SymBindings &symBindings, bool dumpExec
  ) {
    // Ensure IEEE 754 RNE rounding mode regardless of process FP environment.
    std::fesetround(FE_TONEAREST);
    dumpExec_ = dumpExec;
    const FunDecl *entry = nullptr;
    for (const auto &f: prog_.funs) {
      if (f.name.name == entryFuncName) {
        entry = &f;
        break;
      }
    }
    if (!entry) {
      throw std::runtime_error("Entry function not found: " + entryFuncName);
    }

    std::vector<RuntimeValue> args;
    execFunction(*entry, args, symBindings);
  }

  std::string Interpreter::rvToString(const RuntimeValue &rv) const {
    switch (rv.kind) {
      case RuntimeValue::Kind::Int:
        return std::to_string(rv.intVal);
      case RuntimeValue::Kind::Float:
        return std::to_string(rv.floatVal);
      case RuntimeValue::Kind::Undef:
        return "undef";
      case RuntimeValue::Kind::Array:
        return "[...]";
      case RuntimeValue::Kind::Struct:
        return "{...}";
      case RuntimeValue::Kind::Ptr:
        return "ptr(0x" + std::to_string(rv.ptrVal) + ")";
      case RuntimeValue::Kind::Vec: {
        std::string s = "<";
        for (size_t i = 0; i < rv.arrayVal.size(); ++i) {
          if (i)
            s += ", ";
          s += rvToString(rv.arrayVal[i]);
        }
        s += ">";
        return s;
      }
    }
    return "?";
  }

  static std::int64_t canonicalize(std::int64_t val, std::uint32_t bits) {
    if (bits >= 64)
      return val;
    std::uint64_t mask = (1ULL << bits) - 1;
    std::uint64_t sign_bit = 1ULL << (bits - 1);
    std::uint64_t uval = static_cast<std::uint64_t>(val) & mask;
    if (uval & sign_bit)
      uval |= ~mask;
    return static_cast<std::int64_t>(uval);
  }

  Interpreter::RuntimeValue Interpreter::makeUndef(const TypePtr &t) {
    RuntimeValue res;
    if (auto vt = TypeUtils::asVec(t)) {
      // [v0.2.1] Undef vector: every lane is undef. A subsequent lane
      // write produces a defined value at that lane; remaining lanes
      // stay undef until a whole-vector copy assigns them (rule 22).
      res.kind = RuntimeValue::Kind::Vec;
      for (size_t i = 0; i < vt->size; ++i)
        res.arrayVal.push_back(makeUndef(vt->elem));
    } else if (auto at = TypeUtils::asArray(t)) {
      res.kind = RuntimeValue::Kind::Array;
      for (size_t i = 0; i < at->size; ++i)
        res.arrayVal.push_back(makeUndef(at->elem));
    } else if (auto st = TypeUtils::asStruct(t)) {
      res.kind = RuntimeValue::Kind::Struct;
      auto it = structs_.find(st->name.name);
      if (it != structs_.end()) {
        for (const auto &f: it->second->fields)
          res.structVal[f.name] = makeUndef(f.type);
      }
    } else {
      auto bits = TypeUtils::getBitWidth(t);
      if (bits) {
        res.kind = RuntimeValue::Kind::Undef;
        res.bits = *bits;
      } else if (t && std::holds_alternative<FloatType>(t->v)) {
        res.kind = RuntimeValue::Kind::Undef;
        res.bits = (std::get<FloatType>(t->v).kind == FloatType::Kind::F32) ? 32 : 64;
      } else if (t && std::holds_alternative<PtrType>(t->v)) {
        res.kind = RuntimeValue::Kind::Undef;
        res.bits = 64;
      } else {
        res.kind = RuntimeValue::Kind::Undef;
        res.bits = 64;
      }
    }
    return res;
  }

  Interpreter::RuntimeValue Interpreter::broadcast(const TypePtr &t, const RuntimeValue &v) {
    if (auto vt = TypeUtils::asVec(t)) {
      // [v0.2.1] Broadcast init for vector: each lane gets a copy of `v`
      // canonicalized to the lane scalar type.
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Vec;
      for (size_t i = 0; i < vt->size; ++i)
        res.arrayVal.push_back(broadcast(vt->elem, v));
      return res;
    } else if (auto at = TypeUtils::asArray(t)) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Array;
      for (size_t i = 0; i < at->size; ++i)
        res.arrayVal.push_back(broadcast(at->elem, v));
      return res;
    } else if (auto st = TypeUtils::asStruct(t)) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Struct;
      auto it = structs_.find(st->name.name);
      if (it != structs_.end()) {
        for (const auto &f: it->second->fields)
          res.structVal[f.name] = broadcast(f.type, v);
      }
      return res;
    } else {
      RuntimeValue res = v;
      auto bits = TypeUtils::getBitWidth(t);
      if (bits) {
        res.bits = *bits;
        res.intVal = canonicalize(res.intVal, res.bits);
      } else if (t && std::holds_alternative<FloatType>(t->v)) {
        res.bits = (std::get<FloatType>(t->v).kind == FloatType::Kind::F32) ? 32 : 64;
      }
      return res;
    }
  }

  Interpreter::RuntimeValue
  Interpreter::evalInit(const InitVal &iv, const TypePtr &t, const Store &store) {
    if (iv.kind == InitVal::Kind::Undef)
      return makeUndef(t);

    if (iv.kind == InitVal::Kind::Null) {
      RuntimeValue rv;
      rv.kind = RuntimeValue::Kind::Ptr;
      rv.ptrVal = 0; // null = address 0
      rv.bits = 64;
      return rv;
    }

    if (iv.kind == InitVal::Kind::Aggregate) {
      const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
      if (auto vt = TypeUtils::asVec(t)) {
        // [v0.2.1] Brace init for vector: each lane init is a scalar.
        RuntimeValue res;
        res.kind = RuntimeValue::Kind::Vec;
        for (size_t i = 0; i < elements.size(); ++i)
          res.arrayVal.push_back(evalInit(*elements[i], vt->elem, store));
        return res;
      } else if (auto at = TypeUtils::asArray(t)) {
        RuntimeValue res;
        res.kind = RuntimeValue::Kind::Array;
        for (size_t i = 0; i < elements.size(); ++i)
          res.arrayVal.push_back(evalInit(*elements[i], at->elem, store));
        return res;
      } else if (auto st = TypeUtils::asStruct(t)) {
        RuntimeValue res;
        res.kind = RuntimeValue::Kind::Struct;
        auto sit = structs_.find(st->name.name);
        if (sit != structs_.end()) {
          for (size_t i = 0; i < sit->second->fields.size(); ++i) {
            res.structVal[sit->second->fields[i].name] =
                evalInit(*elements[i], sit->second->fields[i].type, store);
          }
        }
        return res;
      }
    }

    // Scalar
    RuntimeValue v;
    if (iv.kind == InitVal::Kind::Int) {
      v.kind = RuntimeValue::Kind::Int;
      v.intVal = std::get<IntLit>(iv.value).value;
    } else if (iv.kind == InitVal::Kind::Float) {
      v.kind = RuntimeValue::Kind::Float;
      v.floatVal = std::get<FloatLit>(iv.value).value;
    } else if (iv.kind == InitVal::Kind::Sym) {
      v = store.at(std::get<SymId>(iv.value).name);
    } else if (iv.kind == InitVal::Kind::Atom) {
      // [v0.2.1] §3.4.2 atom-form init — evaluate the atom against the
      // partially-built store. Inits are processed in declaration order
      // so any local the atom references must already be in the store.
      v = evalAtom(*std::get<AtomPtr>(iv.value), store);
    } else {
      v = store.at(std::get<LocalId>(iv.value).name);
    }

    if (v.kind == RuntimeValue::Kind::Int) {
      v.bits = TypeUtils::getBitWidth(t).value_or(64);
      v.intVal = canonicalize(v.intVal, v.bits);
    } else if (v.kind == RuntimeValue::Kind::Float) {
      v.bits = (t && std::holds_alternative<FloatType>(t->v))
                   ? (std::get<FloatType>(t->v).kind == FloatType::Kind::F32 ? 32 : 64)
                   : 64;
      // SPEC §6.4: an init value for an f32 local must take its f32 precision.
      // Without this, a non-exactly-representable literal like
      // `let %a: f32 = 16777217.0` keeps the f64 image (exact 16777217.0)
      // while claiming bits=32, so later arithmetic diverges from the lowered
      // C/WASM (where the literal would round to 16777216.0 under RNE).
      if (v.bits == 32)
        v.floatVal = static_cast<double>(static_cast<float>(v.floatVal));
    }

    if (TypeUtils::asArray(t) || TypeUtils::asStruct(t) || TypeUtils::asVec(t)) {
      return broadcast(t, v);
    }
    return v;
  }

  void Interpreter::execFunction(
      const FunDecl &f, const std::vector<RuntimeValue> &args, const SymBindings &symBindings
  ) {
    // Reset per-function memory state
    heap_.clear();
    objects_.clear();
    addrMap_.clear();
    typeMap_.clear();
    nextAddr_ = 4096; // leave address 0 for null

    // Build name→type map for addr provenance lookups.
    for (const auto &p: f.params)
      typeMap_[p.name.name] = p.type;
    for (const auto &s: f.syms)
      typeMap_[s.name.name] = s.type;
    for (const auto &l: f.lets)
      typeMap_[l.name.name] = l.type;

    Store store;
    DiagBag diags;

    for (size_t i = 0; i < f.params.size(); ++i) {
      RuntimeValue v = args[i];
      v.bits = TypeUtils::getBitWidth(f.params[i].type).value_or(64);
      v.intVal = canonicalize(v.intVal, v.bits);
      store[f.params[i].name.name] = v;
    }

    for (const auto &s: f.syms) {
      auto it = symBindings.find(s.name.name);
      if (it == symBindings.end()) {
        throw std::runtime_error(
            "Symbol " + s.name.name + " has no binding (provide --sym " + s.name.name + "=<value>)"
        );
      }
      {
        RuntimeValue v;
        std::visit(
            [&](auto &&val) {
              using T = std::decay_t<decltype(val)>;
              if (std::holds_alternative<IntType>(s.type->v)) {
                v.kind = RuntimeValue::Kind::Int;
                if constexpr (std::is_same_v<T, std::int64_t>) {
                  v.intVal = val;
                } else {
                  v.intVal = static_cast<std::int64_t>(val);
                }
                v.bits = TypeUtils::getBitWidth(s.type).value_or(64);
                v.intVal = canonicalize(v.intVal, v.bits);
              } else if (std::holds_alternative<FloatType>(s.type->v)) {
                v.kind = RuntimeValue::Kind::Float;
                if constexpr (std::is_same_v<T, double>) {
                  v.floatVal = val;
                } else {
                  v.floatVal = static_cast<double>(val);
                }
                v.bits = (std::get<FloatType>(s.type->v).kind == FloatType::Kind::F32) ? 32 : 64;
              }
            },
            it->second
        );
        store[s.name.name] = v;
      }
    }

    // Init locals
    for (const auto &l: f.lets) {
      if (l.init) {
        store[l.name.name] = evalInit(*l.init, l.type, store);
      } else {
        store[l.name.name] = makeUndef(l.type);
      }
    }

    CFG cfg = CFG::build(f, diags);
    if (diags.hasErrors())
      throw std::runtime_error("CFG Build failed during interp");

    std::size_t pc = cfg.entry;

    while (true) {
      const Block &block = f.blocks[pc];
      if (dumpExec_) {
        std::cout << block.label.name << ":\n";
      }

      for (const auto &ins: block.instrs) {
        std::visit(
            [&](auto &&i) {
              using T = std::decay_t<decltype(i)>;
              if constexpr (std::is_same_v<T, AssignInstr>) {
                RuntimeValue rhs = evalExpr(i.rhs, store);
                if (dumpExec_) {
                  std::cout << "  " << i.lhs.base.name;
                  for (const auto &acc: i.lhs.accesses) {
                    if (auto ai = std::get_if<AccessIndex>(&acc)) {
                      std::cout << "[";
                      if (auto ilit = std::get_if<IntLit>(&ai->index)) {
                        std::cout << ilit->value;
                      } else {
                        auto id = std::get<LocalOrSymId>(ai->index);
                        std::visit(
                            [&](auto &&id_val) {
                              auto it = store.find(id_val.name);
                              if (it != store.end())
                                std::cout << it->second.intVal;
                              else
                                std::cout << id_val.name;
                            },
                            id
                        );
                      }
                      std::cout << "]";
                    } else if (auto af = std::get_if<AccessField>(&acc)) {
                      std::cout << "." << af->field;
                    }
                  }
                  std::cout << " = " << rvToString(rhs) << "\n";
                }
                setLValue(i.lhs, rhs, store);
              } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                if (!evalCond(i.cond, store))
                  throw std::runtime_error("Assumption failed");
              } else if constexpr (std::is_same_v<T, RequireInstr>) {
                if (!evalCond(i.cond, store)) {
                  std::string msg = i.message.value_or("Requirement failed");
                  throw RequireViolationError(msg);
                }
              } else if constexpr (std::is_same_v<T, StoreInstr>) {
                RuntimeValue ptrVal = evalExpr(i.ptr, store);
                RuntimeValue val = evalExpr(i.val, store);
                if (ptrVal.kind == RuntimeValue::Kind::Undef)
                  throw UndefinedBehaviorError("UB: Store through undef pointer");
                if (ptrVal.kind != RuntimeValue::Kind::Ptr)
                  throw std::runtime_error("Store requires a pointer operand");
                if (ptrVal.ptrVal == 0)
                  throw UndefinedBehaviorError("UB: Null pointer dereference in store");
                // Provenance-based bounds check.
                const ObjectInfo *obj = findObjectByBase(ptrVal.ptrBase);
                if (!obj)
                  throw UndefinedBehaviorError("UB: Store to unknown address");
                if (ptrVal.ptrVal < obj->base || ptrVal.ptrVal >= obj->end)
                  throw UndefinedBehaviorError("UB: Store out of bounds");
                // [v0.2.1] Rule 15b: typed-access mismatch on store.
                const ObjectInfo *cellObj = findObject(ptrVal.ptrVal);
                if (cellObj && cellObj->fieldName.size() > 0) {
                  auto tit = typeMap_.find(
                      std::get_if<RValueAtom>(&i.ptr.first.v)
                          ? std::get_if<RValueAtom>(&i.ptr.first.v)->rval.base.name
                          : ""
                  );
                  if (tit != typeMap_.end()) {
                    TypePtr ptrTy = tit->second;
                    if (auto pt = std::get_if<PtrType>(&ptrTy->v)) {
                      uint64_t ptrElemSize = sizeofType(pt->pointee);
                      if (ptrElemSize != cellObj->elemSize)
                        throw UndefinedBehaviorError(
                            "UB: Typed-access mismatch (rule 15b) on store"
                        );
                    }
                  }
                }
                // SPEC §6.4: enforce destination precision. The pointee object
                // has an elemSize that reflects the pointee type; for f32 this
                // is 4, for f64 it is 8. Round float values to f32 if the
                // pointee is f32-sized so the stored bits match what C/WASM
                // would compute. (Int canonicalization is already handled in
                // AssignInstr's setLValue and is left to the store-side mirror
                // for now since the heap holds full int64 bits.)
                if (val.kind == RuntimeValue::Kind::Float && obj->elemSize == 4) {
                  val.bits = 32;
                  val.floatVal = static_cast<double>(static_cast<float>(val.floatVal));
                }
                heap_[ptrVal.ptrVal] = val;
                // Sync back to store for Store/Heap consistency.
                // Strategy: find the most specific ObjectInfo that
                // contains the store address. If it's a field-level
                // ObjectInfo, sync to that field. If it's a whole-struct
                // ObjectInfo (from ptrfield provenance), find the field
                // ObjectInfo that contains the address and sync there.
                const ObjectInfo *syncObj = findObject(ptrVal.ptrVal);
                if (syncObj && !syncObj->fieldName.empty()) {
                  if (syncObj->arrayIdx != static_cast<std::uint64_t>(-1) &&
                      store.count(syncObj->varName) &&
                      store.at(syncObj->varName).kind == RuntimeValue::Kind::Array) {
                    auto &arr = store[syncObj->varName].arrayVal;
                    if (syncObj->arrayIdx < arr.size() &&
                        arr[syncObj->arrayIdx].kind == RuntimeValue::Kind::Struct)
                      arr[syncObj->arrayIdx].structVal[syncObj->fieldName] = val;
                  } else if (store.count(syncObj->varName) &&
                             store.at(syncObj->varName).kind == RuntimeValue::Kind::Struct) {
                    store[syncObj->varName].structVal[syncObj->fieldName] = val;
                  }
                } else if (obj->count == 1 && obj->fieldName.empty() &&
                           obj->arrayIdx == static_cast<std::uint64_t>(-1) &&
                           store.count(obj->varName) &&
                           store.at(obj->varName).kind != RuntimeValue::Kind::Array &&
                           store.at(obj->varName).kind != RuntimeValue::Kind::Vec) {
                  store[obj->varName] = val;
                } else {
                  uint64_t idx = (ptrVal.ptrVal - obj->base) / obj->elemSize;
                  if (store.count(obj->varName) &&
                      store.at(obj->varName).kind == RuntimeValue::Kind::Array)
                    store[obj->varName].arrayVal[idx] = val;
                }
              }
            },
            ins
        );
      }

      bool jumped = false;
      std::visit(
          [&](auto &&t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, BrTerm>) {
              if (t.isConditional) {
                if (evalCond(*t.cond, store))
                  pc = cfg.indexOf[t.thenLabel.name];
                else
                  pc = cfg.indexOf[t.elseLabel.name];
              } else {
                pc = cfg.indexOf[t.dest.name];
              }
              jumped = true;
            } else if constexpr (std::is_same_v<T, RetTerm>) {
              if (t.value) {
                RuntimeValue res = evalExpr(*t.value, store);
                if (res.kind == RuntimeValue::Kind::Undef)
                  throw UndefinedBehaviorError("UB: Reading undef in ret");
                if (res.kind == RuntimeValue::Kind::Int)
                  std::cout << "Result: " << res.intVal << "\n";
                else if (res.kind == RuntimeValue::Kind::Float) {
                  // Print floats as IEEE 754 hex (printf %a) so the output is
                  // bit-exact: round-trips losslessly, distinguishes +0/-0,
                  // and handles subnormals correctly. This is the format used
                  // for interp ⇄ compiled-C cross-validation in the xval
                  // tests; decimal would silently lose bits at the boundary.
                  char buf[64];
                  std::snprintf(buf, sizeof(buf), "%a", res.floatVal);
                  std::cout << "Result: " << buf << "\n";
                } else if (res.kind == RuntimeValue::Kind::Ptr)
                  std::cout << "Result: ptr(0x" << std::hex << res.ptrVal << std::dec << ")\n";
              } else {
                std::cout << "Result: void\n";
              }
              return;
            } else if constexpr (std::is_same_v<T, UnreachableTerm>) {
              throw std::runtime_error("Reached unreachable");
            }
          },
          block.term
      );

      if (!jumped)
        break;
    }
  }

  Interpreter::RuntimeValue Interpreter::evalExpr(const Expr &e, const Store &store) {
    RuntimeValue v = evalAtom(e.first, store);
    for (const auto &tail: e.rest) {
      RuntimeValue right = evalAtom(tail.atom, store);
      // [v0.2.1] Vector chain: lane-wise +/-. The other operand may be
      // another Vec OR a scalar literal that broadcasts (matches the
      // typechecker rule allowing literal broadcast in vec chains).
      if (v.kind == RuntimeValue::Kind::Vec &&
          (right.kind == RuntimeValue::Kind::Int || right.kind == RuntimeValue::Kind::Float)) {
        RuntimeValue bvec;
        bvec.kind = RuntimeValue::Kind::Vec;
        bvec.arrayVal.reserve(v.arrayVal.size());
        for (auto &laneL: v.arrayVal) {
          RuntimeValue lane = right;
          lane.bits = laneL.bits;
          if (lane.kind == RuntimeValue::Kind::Int)
            lane.intVal = canonicalize(lane.intVal, lane.bits);
          else if (lane.bits == 32)
            lane.floatVal = static_cast<double>(static_cast<float>(lane.floatVal));
          bvec.arrayVal.push_back(std::move(lane));
        }
        right = std::move(bvec);
      }
      if (v.kind == RuntimeValue::Kind::Vec && right.kind == RuntimeValue::Kind::Vec) {
        if (v.arrayVal.size() != right.arrayVal.size())
          throw std::runtime_error("Vector lane count mismatch in +/-");
        for (size_t k = 0; k < v.arrayVal.size(); ++k) {
          auto &lhsLane = v.arrayVal[k];
          auto &rhsLane = right.arrayVal[k];
          if (lhsLane.kind == RuntimeValue::Kind::Undef ||
              rhsLane.kind == RuntimeValue::Kind::Undef)
            throw UndefinedBehaviorError("UB: Reading undef vector lane");
          if (lhsLane.kind == RuntimeValue::Kind::Int) {
            __int128 a = lhsLane.intVal, b = rhsLane.intVal;
            __int128 r = (tail.op == AddOp::Plus) ? (a + b) : (a - b);
            int64_t smax =
                (lhsLane.bits == 64) ? INT64_MAX : ((INT64_C(1) << (lhsLane.bits - 1)) - 1);
            int64_t smin = (lhsLane.bits == 64) ? INT64_MIN : (-(INT64_C(1) << (lhsLane.bits - 1)));
            if (r > (__int128) smax || r < (__int128) smin)
              throw UndefinedBehaviorError("UB: vector lane overflow in +/-");
            lhsLane.intVal = canonicalize(static_cast<int64_t>(r), lhsLane.bits);
          } else if (lhsLane.kind == RuntimeValue::Kind::Float) {
            uint32_t opBits = std::min(lhsLane.bits, rhsLane.bits);
            if (tail.op == AddOp::Plus)
              lhsLane.floatVal = checkFPResult(lhsLane.floatVal + rhsLane.floatVal, opBits);
            else
              lhsLane.floatVal = checkFPResult(lhsLane.floatVal - rhsLane.floatVal, opBits);
            lhsLane.bits = opBits;
          } else {
            throw std::runtime_error("Unsupported lane kind in vector +/-");
          }
        }
        continue;
      }
      if (v.kind == RuntimeValue::Kind::Undef || right.kind == RuntimeValue::Kind::Undef)
        throw UndefinedBehaviorError("UB: Reading undef in expr");

      // Promote Int to Float if needed (Literal inference support)
      if (v.kind == RuntimeValue::Kind::Float && right.kind == RuntimeValue::Kind::Int) {
        right.floatVal = static_cast<double>(right.intVal);
        right.kind = RuntimeValue::Kind::Float;
      } else if (v.kind == RuntimeValue::Kind::Int && right.kind == RuntimeValue::Kind::Float) {
        v.floatVal = static_cast<double>(v.intVal);
        v.kind = RuntimeValue::Kind::Float;
      }

      if (v.kind == RuntimeValue::Kind::Int && right.kind == RuntimeValue::Kind::Int) {
        // Check overflow against the *declared* bitwidth, not int64.
        // Use __int128 so the intermediate result never overflows before the check.
        __int128 a = v.intVal, b = right.intVal;
        __int128 result = (tail.op == AddOp::Plus) ? (a + b) : (a - b);
        int64_t smax = (v.bits == 64) ? INT64_MAX : ((INT64_C(1) << (v.bits - 1)) - 1);
        int64_t smin = (v.bits == 64) ? INT64_MIN : (-(INT64_C(1) << (v.bits - 1)));
        if (result > (__int128) smax || result < (__int128) smin) {
          if (tail.op == AddOp::Plus)
            throw UndefinedBehaviorError("UB: Signed integer overflow in addition");
          else
            throw UndefinedBehaviorError("UB: Signed integer overflow in subtraction");
        }
        v.intVal = static_cast<int64_t>(result);
        v.intVal = canonicalize(v.intVal, v.bits);
      } else if (v.kind == RuntimeValue::Kind::Float && right.kind == RuntimeValue::Kind::Float) {
        // SPEC §6.7: FP expressions are homogeneous, but evalCoef tags
        // FloatLit with bits=64 (it has no context). Take the narrower of the
        // two operands so an Expr like `0.125 + (-268435449 as f32)` rounds
        // at f32 precision instead of inheriting the literal's bits=64. The
        // result's effective type also narrows for downstream chain steps.
        uint32_t opBits = std::min(v.bits, right.bits);
        if (tail.op == AddOp::Plus)
          v.floatVal = checkFPResult(v.floatVal + right.floatVal, opBits);
        else
          v.floatVal = checkFPResult(v.floatVal - right.floatVal, opBits);
        v.bits = opBits;
      } else if (v.kind == RuntimeValue::Kind::Ptr && right.kind == RuntimeValue::Kind::Int) {
        // ptr ± int: scale offset by element size; result must stay in [base, end].
        // One-past-the-end (== end) is valid for arithmetic but not for dereference.
        // Use ptrBase to find the exact provenance object (enforces field-level boundaries).
        const ObjectInfo *obj = findObjectByBase(v.ptrBase);
        if (!obj)
          throw UndefinedBehaviorError("UB: Pointer arithmetic on out-of-bounds pointer");
        uint64_t elemSize = obj->elemSize;
        int64_t delta = right.intVal * static_cast<int64_t>(elemSize);
        int64_t newAddr = (tail.op == AddOp::Plus) ? static_cast<int64_t>(v.ptrVal) + delta
                                                   : static_cast<int64_t>(v.ptrVal) - delta;
        if (newAddr < static_cast<int64_t>(obj->base) || newAddr > static_cast<int64_t>(obj->end))
          throw UndefinedBehaviorError("UB: Pointer arithmetic out of bounds");
        v.ptrVal = static_cast<uint64_t>(newAddr);
        // ptrBase preserved: arithmetic does not change provenance.
      } else if (v.kind == RuntimeValue::Kind::Ptr && right.kind == RuntimeValue::Kind::Ptr) {
        // ptr - ptr: element count distance (only subtraction makes sense)
        if (tail.op != AddOp::Minus)
          throw std::runtime_error("Cannot add two pointers");
        if (v.ptrBase != right.ptrBase)
          throw UndefinedBehaviorError("UB: Pointer subtraction across different objects");
        const ObjectInfo *obj = findObjectByBase(v.ptrBase);
        uint64_t elemSize = obj ? obj->elemSize : 1;
        int64_t diff = static_cast<int64_t>(v.ptrVal) - static_cast<int64_t>(right.ptrVal);
        v.kind = RuntimeValue::Kind::Int;
        v.intVal = diff / static_cast<int64_t>(elemSize);
        v.bits = 64;
      } else {
        throw std::runtime_error("Expr ops only on same scalar kinds (Int/Float)");
      }
    }
    return v;
  }

  Interpreter::RuntimeValue Interpreter::evalAtom(const Atom &a, const Store &store) {
    return std::visit(
        [&](auto &&arg) -> RuntimeValue {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            RuntimeValue c = evalCoef(arg.coef, store);
            RuntimeValue r = evalLValue(arg.rval, store);
            // [v0.2.1] Vector OpAtom: rval is Vec; coef is either Vec or a
            // scalar literal that broadcasts. Build a per-lane Coef-LValue
            // and recurse into a synthetic OpAtom evaluation for each lane.
            if (r.kind == RuntimeValue::Kind::Vec) {
              RuntimeValue res;
              res.kind = RuntimeValue::Kind::Vec;
              res.arrayVal.reserve(r.arrayVal.size());
              for (size_t k = 0; k < r.arrayVal.size(); ++k) {
                auto &rL = r.arrayVal[k];
                RuntimeValue cL = (c.kind == RuntimeValue::Kind::Vec) ? c.arrayVal[k] : c;
                if (rL.kind == RuntimeValue::Kind::Undef || cL.kind == RuntimeValue::Kind::Undef)
                  throw UndefinedBehaviorError("UB: Reading undef vector lane");
                // Inherit the lane's bits for the coef literal (broadcast).
                if (c.kind != RuntimeValue::Kind::Vec) {
                  if (cL.kind == RuntimeValue::Kind::Int)
                    cL.bits = rL.bits;
                  if (cL.kind == RuntimeValue::Kind::Float)
                    cL.bits = rL.bits;
                }
                // Build a fake one-atom Atom { OpAtom with these scalar
                // operands } and reuse evalAtom — too involved; inline.
                RuntimeValue laneRes;
                if (rL.kind == RuntimeValue::Kind::Int) {
                  laneRes.kind = RuntimeValue::Kind::Int;
                  laneRes.bits = rL.bits;
                  int64_t bw_smax =
                      (rL.bits == 64) ? INT64_MAX : ((INT64_C(1) << (rL.bits - 1)) - 1);
                  int64_t bw_smin = (rL.bits == 64) ? INT64_MIN : (-(INT64_C(1) << (rL.bits - 1)));
                  if (arg.op == AtomOpKind::Mul) {
                    __int128 p = (__int128) cL.intVal * rL.intVal;
                    if (p > (__int128) bw_smax || p < (__int128) bw_smin)
                      throw UndefinedBehaviorError("UB: vector lane overflow in *");
                    laneRes.intVal = static_cast<int64_t>(p);
                  } else if (arg.op == AtomOpKind::Div) {
                    if (rL.intVal == 0)
                      throw UndefinedBehaviorError("UB: vector lane division by zero");
                    if (cL.intVal == bw_smin && rL.intVal == -1)
                      throw UndefinedBehaviorError("UB: vector lane overflow in /");
                    laneRes.intVal = cL.intVal / rL.intVal;
                  } else if (arg.op == AtomOpKind::Mod) {
                    if (rL.intVal == 0)
                      throw UndefinedBehaviorError("UB: vector lane modulo by zero");
                    if (cL.intVal == bw_smin && rL.intVal == -1)
                      throw UndefinedBehaviorError("UB: vector lane overflow in %");
                    laneRes.intVal = cL.intVal % rL.intVal;
                  } else if (arg.op == AtomOpKind::And)
                    laneRes.intVal = cL.intVal & rL.intVal;
                  else if (arg.op == AtomOpKind::Or)
                    laneRes.intVal = cL.intVal | rL.intVal;
                  else if (arg.op == AtomOpKind::Xor)
                    laneRes.intVal = cL.intVal ^ rL.intVal;
                  else if (arg.op == AtomOpKind::Shl || arg.op == AtomOpKind::Shr ||
                           arg.op == AtomOpKind::LShr) {
                    if (rL.intVal < 0 || (uint64_t) rL.intVal >= (uint64_t) laneRes.bits)
                      throw UndefinedBehaviorError("UB: vector lane overshift");
                    if (arg.op == AtomOpKind::Shl) {
                      if (cL.intVal < 0)
                        throw UndefinedBehaviorError("UB: vector lane left shift of negative");
                      __int128 p = (__int128) cL.intVal << rL.intVal;
                      if (p > (__int128) bw_smax || p < (__int128) bw_smin)
                        throw UndefinedBehaviorError("UB: vector lane overflow in <<");
                      laneRes.intVal = static_cast<int64_t>(p);
                    } else if (arg.op == AtomOpKind::Shr) {
                      laneRes.intVal = cL.intVal >> rL.intVal;
                    } else {
                      uint64_t mask = (laneRes.bits >= 64) ? ~0ULL : (1ULL << laneRes.bits) - 1;
                      laneRes.intVal =
                          (int64_t) ((static_cast<uint64_t>(cL.intVal) & mask) >> rL.intVal);
                    }
                  }
                  laneRes.intVal = canonicalize(laneRes.intVal, laneRes.bits);
                } else if (rL.kind == RuntimeValue::Kind::Float) {
                  laneRes.kind = RuntimeValue::Kind::Float;
                  laneRes.bits = std::min(cL.bits, rL.bits);
                  if (arg.op == AtomOpKind::Mul)
                    laneRes.floatVal = checkFPResult(cL.floatVal * rL.floatVal, laneRes.bits);
                  else if (arg.op == AtomOpKind::Div)
                    laneRes.floatVal = checkFPResult(cL.floatVal / rL.floatVal, laneRes.bits);
                  else if (arg.op == AtomOpKind::Mod)
                    laneRes.floatVal =
                        checkFPResult(std::fmod(cL.floatVal, rL.floatVal), laneRes.bits);
                  else
                    throw std::runtime_error("Unsupported op for float vector lane");
                } else {
                  throw std::runtime_error("Unsupported vector lane kind in OpAtom");
                }
                res.arrayVal.push_back(std::move(laneRes));
              }
              return res;
            }
            if (c.kind == RuntimeValue::Kind::Undef || r.kind == RuntimeValue::Kind::Undef)
              throw UndefinedBehaviorError("UB: Reading undef in op");

            // Promote Int to Float if needed (Literal inference support)
            if (c.kind == RuntimeValue::Kind::Float && r.kind == RuntimeValue::Kind::Int) {
              r.floatVal = static_cast<double>(r.intVal);
              r.kind = RuntimeValue::Kind::Float;
            } else if (c.kind == RuntimeValue::Kind::Int && r.kind == RuntimeValue::Kind::Float) {
              c.floatVal = static_cast<double>(c.intVal);
              c.kind = RuntimeValue::Kind::Float;
            }

            if (c.kind == RuntimeValue::Kind::Int && r.kind == RuntimeValue::Kind::Int) {
              RuntimeValue res;
              res.kind = RuntimeValue::Kind::Int;
              res.bits = c.bits;

              // Compute bitwidth-specific signed min/max for overflow detection.
              int64_t bw_smax = (c.bits == 64) ? INT64_MAX : ((INT64_C(1) << (c.bits - 1)) - 1);
              int64_t bw_smin = (c.bits == 64) ? INT64_MIN : (-(INT64_C(1) << (c.bits - 1)));

              if (arg.op == AtomOpKind::Mul) {
                __int128 prod = (__int128) c.intVal * (__int128) r.intVal;
                if (prod > (__int128) bw_smax || prod < (__int128) bw_smin)
                  throw UndefinedBehaviorError("UB: Signed integer overflow in multiplication");
                res.intVal = static_cast<int64_t>(prod);
              } else if (arg.op == AtomOpKind::Div) {
                if (r.intVal == 0)
                  throw UndefinedBehaviorError("UB: Division by zero");
                if (c.intVal == bw_smin && r.intVal == -1)
                  throw UndefinedBehaviorError("UB: Signed integer overflow in division");
                res.intVal = c.intVal / r.intVal;
              } else if (arg.op == AtomOpKind::Mod) {
                if (r.intVal == 0)
                  throw UndefinedBehaviorError("UB: Modulo by zero");
                if (c.intVal == bw_smin && r.intVal == -1)
                  throw UndefinedBehaviorError("UB: Signed integer overflow in modulo");
                res.intVal = c.intVal % r.intVal;
              } else if (arg.op == AtomOpKind::And) {
                res.intVal = c.intVal & r.intVal;
              } else if (arg.op == AtomOpKind::Or) {
                res.intVal = c.intVal | r.intVal;
              } else if (arg.op == AtomOpKind::Xor) {
                res.intVal = c.intVal ^ r.intVal;
              } else if (arg.op == AtomOpKind::Shl || arg.op == AtomOpKind::Shr ||
                         arg.op == AtomOpKind::LShr) {
                if (r.intVal < 0 || (uint64_t) r.intVal >= (uint64_t) res.bits) {
                  throw UndefinedBehaviorError("UB: Overshift");
                }
                if (arg.op == AtomOpKind::Shl) {
                  // SPEC §7.1 rule 4: SHL on a negative operand is UB, and
                  // result overflow on a non-negative operand is also UB
                  // (signed-integer overflow, same footing as +/-/*).
                  if (c.intVal < 0)
                    throw UndefinedBehaviorError("UB: Left shift of negative");
                  __int128 prod = (__int128) c.intVal << r.intVal;
                  if (prod > (__int128) bw_smax || prod < (__int128) bw_smin)
                    throw UndefinedBehaviorError("UB: Signed integer overflow in shift");
                  res.intVal = static_cast<int64_t>(prod);
                } else if (arg.op == AtomOpKind::Shr) {
                  res.intVal = c.intVal >> r.intVal;
                } else {
                  // Logical shift right: mask to width first
                  uint64_t mask = (res.bits >= 64) ? ~0ULL : (1ULL << res.bits) - 1;
                  res.intVal = (int64_t) ((static_cast<uint64_t>(c.intVal) & mask) >> r.intVal);
                }
              }
              res.intVal = canonicalize(res.intVal, res.bits);
              return res;
            } else if (c.kind == RuntimeValue::Kind::Float && r.kind == RuntimeValue::Kind::Float) {
              RuntimeValue res;
              res.kind = RuntimeValue::Kind::Float;
              // SPEC §6.7: float expressions are homogeneous — all atoms must
              // have the same FP type. evalCoef always tags FloatLit with
              // bits=64 (it has no context), so use the rval's precision (the
              // typed variable) to recover the expression's true type. Without
              // this, e.g. `-4.0 * %v0` for %v0:f32 would yield bits=64 and
              // the surrounding chain would round at f64 precision.
              res.bits = std::min(c.bits, r.bits);
              if (arg.op == AtomOpKind::Mul)
                res.floatVal = checkFPResult(c.floatVal * r.floatVal, res.bits);
              else if (arg.op == AtomOpKind::Div)
                res.floatVal = checkFPResult(c.floatVal / r.floatVal, res.bits);
              else if (arg.op == AtomOpKind::Mod)
                res.floatVal = checkFPResult(std::fmod(c.floatVal, r.floatVal), res.bits);
              else
                throw std::runtime_error("Unsupported op for floats");
              return res;
            }
            throw std::runtime_error("OpAtom requires same scalar kinds");
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            RuntimeValue r = evalLValue(arg.rval, store);
            // [v0.2.1] Vector unary ~: lane-wise.
            if (r.kind == RuntimeValue::Kind::Vec) {
              RuntimeValue res;
              res.kind = RuntimeValue::Kind::Vec;
              res.arrayVal.reserve(r.arrayVal.size());
              for (auto &lane: r.arrayVal) {
                if (lane.kind == RuntimeValue::Kind::Undef)
                  throw UndefinedBehaviorError("UB: Reading undef lane in unary");
                if (lane.kind != RuntimeValue::Kind::Int)
                  throw std::runtime_error("Vector unary ~ requires integer lanes");
                RuntimeValue laneRes;
                laneRes.kind = RuntimeValue::Kind::Int;
                laneRes.bits = lane.bits;
                laneRes.intVal = canonicalize(~lane.intVal, lane.bits);
                res.arrayVal.push_back(std::move(laneRes));
              }
              return res;
            }
            if (r.kind == RuntimeValue::Kind::Undef)
              throw UndefinedBehaviorError("UB: Reading undef in unary op");
            if (r.kind != RuntimeValue::Kind::Int)
              throw std::runtime_error("Unary op requires int");
            RuntimeValue res;
            res.kind = RuntimeValue::Kind::Int;
            if (arg.op == UnaryOpKind::Not) {
              res.intVal = ~r.intVal;
            }
            return res;
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            // [v0.2.1] Two forms. Mask form requires lane-wise (or scalar
            // i1) blend; Cond form is the existing scalar boolean select.
            if (arg.cond) {
              return evalCond(*arg.cond, store) ? evalSelectVal(arg.vtrue, store)
                                                : evalSelectVal(arg.vfalse, store);
            }
            // Mask form
            RuntimeValue mask = evalExpr(*arg.maskExpr, store);
            if (mask.kind == RuntimeValue::Kind::Vec) {
              RuntimeValue vt = evalSelectVal(arg.vtrue, store);
              RuntimeValue vf = evalSelectVal(arg.vfalse, store);
              if (vt.kind != RuntimeValue::Kind::Vec || vf.kind != RuntimeValue::Kind::Vec ||
                  vt.arrayVal.size() != mask.arrayVal.size() ||
                  vf.arrayVal.size() != mask.arrayVal.size())
                throw std::runtime_error("Mask-based select: lane count mismatch");
              RuntimeValue res;
              res.kind = RuntimeValue::Kind::Vec;
              res.arrayVal.reserve(mask.arrayVal.size());
              for (size_t k = 0; k < mask.arrayVal.size(); ++k) {
                const auto &mL = mask.arrayVal[k];
                if (mL.kind == RuntimeValue::Kind::Undef)
                  throw UndefinedBehaviorError("UB: undef mask lane");
                bool pick = (mL.intVal != 0);
                const auto &chosen = pick ? vt.arrayVal[k] : vf.arrayVal[k];
                if (chosen.kind == RuntimeValue::Kind::Undef)
                  throw UndefinedBehaviorError("UB: undef lane selected by mask");
                res.arrayVal.push_back(chosen);
              }
              return res;
            }
            // Scalar i1 mask: all-or-nothing.
            if (mask.kind == RuntimeValue::Kind::Undef)
              throw UndefinedBehaviorError("UB: undef scalar mask");
            return (mask.intVal != 0) ? evalSelectVal(arg.vtrue, store)
                                      : evalSelectVal(arg.vfalse, store);
          } else if constexpr (std::is_same_v<T, CmpAtom>) {
            // [v0.2.1] Reified comparison. Both operands are SelectVal.
            RuntimeValue lv = evalSelectVal(arg.lhs, store);
            RuntimeValue rv = evalSelectVal(arg.rhs, store);
            auto cmpScalar = [&](const RuntimeValue &a, const RuntimeValue &b) -> bool {
              if (a.kind == RuntimeValue::Kind::Undef || b.kind == RuntimeValue::Kind::Undef)
                throw UndefinedBehaviorError("UB: undef in cmp");
              // [v0.2.1] Rule 14: relational compare of pointers from
              // different objects (or null vs non-null in a relational
              // op) is UB. Equality / inequality remain legal.
              if (a.kind == RuntimeValue::Kind::Ptr || b.kind == RuntimeValue::Kind::Ptr) {
                bool relational = arg.op == RelOp::LT || arg.op == RelOp::LE ||
                                  arg.op == RelOp::GT || arg.op == RelOp::GE;
                if (relational) {
                  uint64_t aBase = (a.kind == RuntimeValue::Kind::Ptr) ? a.ptrBase : 0;
                  uint64_t bBase = (b.kind == RuntimeValue::Kind::Ptr) ? b.ptrBase : 0;
                  if (a.kind != b.kind)
                    throw UndefinedBehaviorError(
                        "UB: relational compare between pointer and non-pointer"
                    );
                  if (aBase != bBase)
                    throw UndefinedBehaviorError("UB: relational compare of cross-object pointers");
                }
                bool eq = a.ptrVal == b.ptrVal;
                switch (arg.op) {
                  case RelOp::EQ:
                    return eq;
                  case RelOp::NE:
                    return !eq;
                  case RelOp::LT:
                    return a.ptrVal < b.ptrVal;
                  case RelOp::LE:
                    return a.ptrVal <= b.ptrVal;
                  case RelOp::GT:
                    return a.ptrVal > b.ptrVal;
                  case RelOp::GE:
                    return a.ptrVal >= b.ptrVal;
                }
                return false;
              }
              double af = (a.kind == RuntimeValue::Kind::Float) ? a.floatVal
                                                                : static_cast<double>(a.intVal);
              double bf = (b.kind == RuntimeValue::Kind::Float) ? b.floatVal
                                                                : static_cast<double>(b.intVal);
              int64_t ai = a.intVal, bi = b.intVal;
              bool isFP =
                  (a.kind == RuntimeValue::Kind::Float || b.kind == RuntimeValue::Kind::Float);
              switch (arg.op) {
                case RelOp::EQ:
                  return isFP ? af == bf : ai == bi;
                case RelOp::NE:
                  return isFP ? af != bf : ai != bi;
                case RelOp::LT:
                  return isFP ? af < bf : ai < bi;
                case RelOp::LE:
                  return isFP ? af <= bf : ai <= bi;
                case RelOp::GT:
                  return isFP ? af > bf : ai > bi;
                case RelOp::GE:
                  return isFP ? af >= bf : ai >= bi;
              }
              return false;
            };
            if (lv.kind == RuntimeValue::Kind::Vec) {
              RuntimeValue res;
              res.kind = RuntimeValue::Kind::Vec;
              res.arrayVal.reserve(lv.arrayVal.size());
              for (size_t k = 0; k < lv.arrayVal.size(); ++k) {
                bool b = cmpScalar(lv.arrayVal[k], rv.arrayVal[k]);
                RuntimeValue lane;
                lane.kind = RuntimeValue::Kind::Int;
                lane.bits = 1;
                lane.intVal = b ? 1 : 0;
                res.arrayVal.push_back(std::move(lane));
              }
              return res;
            }
            RuntimeValue res;
            res.kind = RuntimeValue::Kind::Int;
            res.bits = 1;
            res.intVal = cmpScalar(lv, rv) ? 1 : 0;
            return res;
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            return evalCoef(arg.coef, store);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            return evalLValue(arg.rval, store);
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            // addr <lv>: return the address of the lvalue's storage with provenance.
            const std::string &varName = arg.lv.base.name;
            if (!store.count(varName))
              throw std::runtime_error("UB: addr of unknown variable " + varName);

            // Allocate / materialize storage.  Use typeMap_ when available so
            // that structs get per-field ObjectInfos and scalars/arrays get a
            // typed ObjectInfo with the correct elemSize and count.
            uint64_t base;
            auto tit = typeMap_.find(varName);
            if (tit != typeMap_.end()) {
              if (auto st = std::get_if<StructType>(&tit->second->v)) {
                auto sit = structs_.find(st->name.name);
                if (sit == structs_.end())
                  throw std::runtime_error("Unknown struct: " + st->name.name);
                base = materializeStruct(varName, *sit->second, store);
              } else {
                base = allocObject(varName, tit->second, store);
              }
            } else {
              // Fallback for variables without type info: infer from RuntimeValue shape.
              auto ait = addrMap_.find(varName);
              if (ait != addrMap_.end()) {
                base = ait->second;
              } else {
                const RuntimeValue &sv = store.at(varName);
                uint64_t elemSize = 8, count = 1;
                if (sv.kind == RuntimeValue::Kind::Array) {
                  count = sv.arrayVal.size();
                  elemSize = sv.arrayVal.empty() ? 4
                             : (sv.arrayVal[0].kind == RuntimeValue::Kind::Int)
                                 ? (sv.arrayVal[0].bits + 7) / 8
                             : (sv.arrayVal[0].kind == RuntimeValue::Kind::Float)
                                 ? sv.arrayVal[0].bits / 8
                                 : 8;
                } else if (sv.kind == RuntimeValue::Kind::Int) {
                  elemSize = (sv.bits + 7) / 8;
                } else if (sv.kind == RuntimeValue::Kind::Float) {
                  elemSize = sv.bits / 8;
                }
                base = nextAddr_;
                nextAddr_ += (elemSize * count + 7) & ~7ULL;
                objects_.push_back(
                    ObjectInfo{varName, "", base, base + elemSize * count, elemSize, count}
                );
                addrMap_[varName] = base;
                if (sv.kind == RuntimeValue::Kind::Array) {
                  for (size_t i = 0; i < sv.arrayVal.size(); ++i)
                    heap_[base + i * elemSize] = sv.arrayVal[i];
                } else {
                  heap_[base] = sv;
                }
              }
            }

            // Walk accesses to compute final address and provenance base.
            uint64_t addr = base;
            uint64_t ptrBase = base; // updated by field accesses (rule 15)
            for (const auto &acc: arg.lv.accesses) {
              if (auto ai = std::get_if<AccessIndex>(&acc)) {
                const ObjectInfo *obj = findObjectByBase(ptrBase);
                if (!obj)
                  throw std::runtime_error("Internal: no ObjectInfo at ptrBase for addr");
                int64_t idx = 0;
                if (std::holds_alternative<IntLit>(ai->index)) {
                  idx = std::get<IntLit>(ai->index).value;
                } else {
                  const auto &id = std::get<LocalOrSymId>(ai->index);
                  RuntimeValue idxRv = std::get_if<LocalId>(&id)
                                           ? store.at(std::get_if<LocalId>(&id)->name)
                                           : store.at(std::get_if<SymId>(&id)->name);
                  idx = idxRv.intVal;
                }
                if (idx < 0 || (uint64_t) idx >= obj->count)
                  throw UndefinedBehaviorError("UB: addr of out-of-bounds array element");
                addr = ptrBase + idx * obj->elemSize;
                // ptrBase stays at the array's base (arithmetic walks the array).
              } else if (auto af = std::get_if<AccessField>(&acc)) {
                // addr lv.f: per spec rule 15, provenance = the immediate
                // containing struct. The result pointer can roam over the
                // whole struct's byte range.
                auto tit2 = typeMap_.find(varName);
                if (tit2 == typeMap_.end())
                  throw std::runtime_error("No type info for field access on " + varName);
                auto st = std::get_if<StructType>(&tit2->second->v);
                if (!st)
                  throw std::runtime_error("Field access on non-struct variable " + varName);
                auto sit = structs_.find(st->name.name);
                if (sit == structs_.end())
                  throw std::runtime_error("Unknown struct for field access");
                // materializeStruct is idempotent; ensures per-field ObjectInfos exist.
                uint64_t structBase = materializeStruct(varName, *sit->second, store);
                addr = structBase + fieldOffset(*sit->second, af->field);
                // Provenance = the struct's base (whole-struct ObjectInfo).
                ptrBase = structBase;
              }
            }

            RuntimeValue rv;
            rv.kind = RuntimeValue::Kind::Ptr;
            rv.ptrVal = addr;
            rv.ptrBase = ptrBase;
            rv.bits = 64;
            return rv;
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            // load <rval>: dereference the pointer
            RuntimeValue ptrRv = evalLValue(arg.rval, store);
            if (ptrRv.kind == RuntimeValue::Kind::Undef)
              throw UndefinedBehaviorError("UB: Load through undef pointer");
            if (ptrRv.kind != RuntimeValue::Kind::Ptr)
              throw std::runtime_error("load requires a pointer operand");
            if (ptrRv.ptrVal == 0)
              throw UndefinedBehaviorError("UB: Null pointer dereference in load");
            // Provenance-based bounds check: use ptrBase to locate the exact object.
            const ObjectInfo *obj = findObjectByBase(ptrRv.ptrBase);
            if (!obj)
              throw UndefinedBehaviorError("UB: Load from unknown address");
            if (ptrRv.ptrVal < obj->base || ptrRv.ptrVal >= obj->end)
              throw UndefinedBehaviorError("UB: Load out of bounds");
            // [v0.2.1] Rule 15b: typed-access mismatch. The load address
            // must coincide with the start of a cell whose declared type
            // matches the pointer's static type. We check by finding the
            // most specific (field-level) ObjectInfo at the address; if
            // the pointer's elemSize doesn't match that ObjectInfo's
            // elemSize, the types disagree.
            const ObjectInfo *cellObj = findObject(ptrRv.ptrVal);
            if (cellObj && cellObj->fieldName.size() > 0) {
              // The pointer's element size comes from the provenance obj.
              // If the field cell's elemSize differs, it's a type mismatch.
              // (e.g., ptr i32 landing on an i64 field cell.)
              auto tit = typeMap_.find(arg.rval.base.name);
              if (tit != typeMap_.end()) {
                TypePtr ptrTy = tit->second;
                if (auto pt = std::get_if<PtrType>(&ptrTy->v)) {
                  uint64_t ptrElemSize = sizeofType(pt->pointee);
                  if (ptrElemSize != cellObj->elemSize)
                    throw UndefinedBehaviorError(
                        "UB: Typed-access mismatch (rule 15b): pointer element size " +
                        std::to_string(ptrElemSize) + " != cell size " +
                        std::to_string(cellObj->elemSize)
                    );
                }
              }
            }
            auto hit = heap_.find(ptrRv.ptrVal);
            if (hit == heap_.end())
              throw UndefinedBehaviorError("UB: Load from uninitialized memory");
            return hit->second;
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            // [v0.2.1] §6.8.9: ptrindex <ptr>, <index> navigates a ptr [N] T
            // to ptr T at element `index`. Strict UB at the navigation site
            // for null (rule 17), undef (rule 18), or one-past-end (rule 19)
            // sources, plus out-of-bounds index (rule 16, index in [0, N]).
            RuntimeValue ptrRv = evalLValue(arg.rval, store);
            if (ptrRv.kind == RuntimeValue::Kind::Undef)
              throw UndefinedBehaviorError("UB: 'ptrindex' through undef pointer");
            if (ptrRv.kind != RuntimeValue::Kind::Ptr)
              throw std::runtime_error("ptrindex requires a pointer operand");
            if (ptrRv.ptrVal == 0)
              throw UndefinedBehaviorError("UB: 'ptrindex' through null pointer");
            const ObjectInfo *obj = findObjectByBase(ptrRv.ptrBase);
            if (!obj)
              throw UndefinedBehaviorError("UB: 'ptrindex' on pointer to unknown object");
            if (ptrRv.ptrVal == obj->end)
              throw UndefinedBehaviorError("UB: 'ptrindex' from a one-past-the-end pointer");
            // Resolve the index (literal or local/sym).
            int64_t idx = 0;
            if (auto il = std::get_if<IntLit>(&arg.index)) {
              idx = il->value;
            } else {
              const auto &id = std::get<LocalOrSymId>(arg.index);
              RuntimeValue idxRv = std::get_if<LocalId>(&id)
                                       ? store.at(std::get_if<LocalId>(&id)->name)
                                       : store.at(std::get_if<SymId>(&id)->name);
              if (idxRv.kind == RuntimeValue::Kind::Undef)
                throw UndefinedBehaviorError("UB: 'ptrindex' index is undef");
              idx = idxRv.intVal;
            }
            // Rule 16: index in [0, N] (N is one-past-end, valid for arithmetic).
            // obj->count is N.
            if (idx < 0 || (uint64_t) idx > obj->count)
              throw UndefinedBehaviorError("UB: 'ptrindex' index out of bounds");
            RuntimeValue rv;
            rv.kind = RuntimeValue::Kind::Ptr;
            rv.bits = 64;
            rv.ptrVal = obj->base + (uint64_t) idx * obj->elemSize;
            // Provenance: arithmetic on the result roams over the array
            // (spec rule 15, §7.5). ptrBase stays at the array's base so
            // ptr/load checks continue to use the whole-array range.
            rv.ptrBase = obj->base;
            return rv;
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            // [v0.2.1] §6.8.10: ptrfield <ptr>, <fld> navigates ptr @S to
            // ptr FieldType at the field's static offset.
            RuntimeValue ptrRv = evalLValue(arg.rval, store);
            if (ptrRv.kind == RuntimeValue::Kind::Undef)
              throw UndefinedBehaviorError("UB: 'ptrfield' through undef pointer");
            if (ptrRv.kind != RuntimeValue::Kind::Ptr)
              throw std::runtime_error("ptrfield requires a pointer operand");
            if (ptrRv.ptrVal == 0)
              throw UndefinedBehaviorError("UB: 'ptrfield' through null pointer");
            const ObjectInfo *obj = findObjectByBase(ptrRv.ptrBase);
            if (!obj)
              throw UndefinedBehaviorError("UB: 'ptrfield' on pointer to unknown object");
            if (ptrRv.ptrVal == obj->end)
              throw UndefinedBehaviorError("UB: 'ptrfield' from a one-past-the-end pointer");
            // Resolve the pointee struct decl. The rval is an LValue whose
            // base must be a `ptr @S` local; chase that through typeMap_.
            auto tit = typeMap_.find(arg.rval.base.name);
            if (tit == typeMap_.end())
              throw std::runtime_error("ptrfield: no type info for " + arg.rval.base.name);
            TypePtr cur = tit->second;
            for (const auto &acc: arg.rval.accesses) {
              if (auto af = std::get_if<AccessField>(&acc)) {
                if (auto st = std::get_if<StructType>(&cur->v)) {
                  auto sit = structs_.find(st->name.name);
                  if (sit == structs_.end())
                    throw std::runtime_error("Unknown struct in ptrfield rval chain");
                  bool found = false;
                  for (const auto &f: sit->second->fields)
                    if (f.name == af->field) {
                      cur = f.type;
                      found = true;
                      break;
                    }
                  if (!found)
                    throw std::runtime_error("Unknown field in ptrfield rval chain");
                } else {
                  cur = nullptr;
                  break;
                }
              } else if (auto ai = std::get_if<AccessIndex>(&acc)) {
                (void) ai;
                if (auto at = std::get_if<ArrayType>(&cur->v))
                  cur = at->elem;
                else
                  cur = nullptr;
                if (!cur)
                  break;
              }
            }
            if (!cur || !std::holds_alternative<PtrType>(cur->v))
              throw std::runtime_error("ptrfield: rval is not pointer-typed");
            const auto &pt = std::get<PtrType>(cur->v);
            if (!std::holds_alternative<StructType>(pt.pointee->v))
              throw std::runtime_error("ptrfield: pointee is not a struct");
            auto &st = std::get<StructType>(pt.pointee->v);
            auto sit = structs_.find(st.name.name);
            if (sit == structs_.end())
              throw std::runtime_error("Unknown struct in ptrfield: " + st.name.name);
            uint64_t off = fieldOffset(*sit->second, arg.field);
            RuntimeValue rv;
            rv.kind = RuntimeValue::Kind::Ptr;
            rv.bits = 64;
            // The field address is relative to the *current* struct the
            // pointer is pointing into. For `addr %arr[k]; %p = %p + n;
            // ptrfield %p, f`, that's `ptrVal + off`, not `ptrBase + off`
            // — ptrBase still refers to the containing array's start.
            rv.ptrVal = ptrRv.ptrVal + off;
            // Provenance: field pointer's provenance is the *immediate
            // containing struct* (spec rule 15). Arithmetic on the result
            // can roam over the whole struct's byte range. We set ptrBase
            // to the struct's start address so the bounds check in ptr
            // arith / load / store uses the struct's ObjectInfo.
            rv.ptrBase = ptrRv.ptrVal;
            return rv;
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            RuntimeValue v = std::visit(
                [&](auto &&src) -> RuntimeValue {
                  using S = std::decay_t<decltype(src)>;
                  if constexpr (std::is_same_v<S, IntLit>) {
                    RuntimeValue rv;
                    rv.kind = RuntimeValue::Kind::Int;
                    rv.intVal = src.value;
                    rv.bits = 64;
                    return rv;
                  } else if constexpr (std::is_same_v<S, FloatLit>) {
                    RuntimeValue rv;
                    rv.kind = RuntimeValue::Kind::Float;
                    rv.floatVal = src.value;
                    rv.bits = 64;
                    return rv;
                  } else if constexpr (std::is_same_v<S, SymId>) {
                    return store.at(src.name);
                  } else {
                    return evalLValue(src, store);
                  }
                },
                arg.src
            );
            if (v.kind == RuntimeValue::Kind::Undef)
              throw UndefinedBehaviorError("UB: Reading undef in cast");

            // [v0.2.1] Vector cast: lane-wise. v is a Vec; dstType is <N> U.
            if (v.kind == RuntimeValue::Kind::Vec) {
              auto vt = TypeUtils::asVec(arg.dstType);
              if (!vt)
                throw std::runtime_error("Vector cast requires vector dst");
              if (v.arrayVal.size() != vt->size)
                throw std::runtime_error("Vector cast: lane count mismatch");
              RuntimeValue res;
              res.kind = RuntimeValue::Kind::Vec;
              res.arrayVal.reserve(vt->size);
              auto laneBits = TypeUtils::getBitWidth(vt->elem);
              bool isFp = vt->elem && std::holds_alternative<FloatType>(vt->elem->v);
              bool isF32 = isFp && std::get<FloatType>(vt->elem->v).kind == FloatType::Kind::F32;
              uint32_t fpBits = isFp ? (isF32 ? 32u : 64u) : 0u;
              for (auto &lane: v.arrayVal) {
                if (lane.kind == RuntimeValue::Kind::Undef)
                  throw UndefinedBehaviorError("UB: undef lane in cast");
                RuntimeValue r;
                if (laneBits) {
                  r.kind = RuntimeValue::Kind::Int;
                  r.bits = *laneBits;
                  if (lane.kind == RuntimeValue::Kind::Int) {
                    r.intVal = canonicalize(lane.intVal, r.bits);
                  } else {
                    double lo = -std::ldexp(1.0, static_cast<int>(r.bits) - 1);
                    double hi = std::ldexp(1.0, static_cast<int>(r.bits) - 1);
                    if (std::isnan(lane.floatVal) || std::isinf(lane.floatVal) ||
                        lane.floatVal < lo || lane.floatVal >= hi)
                      throw UndefinedBehaviorError("UB: vector lane float->int OOR");
                    r.intVal = static_cast<int64_t>(lane.floatVal);
                  }
                } else if (isFp) {
                  r.kind = RuntimeValue::Kind::Float;
                  r.bits = fpBits;
                  double raw = (lane.kind == RuntimeValue::Kind::Int)
                                   ? static_cast<double>(lane.intVal)
                                   : lane.floatVal;
                  r.floatVal = isF32 ? static_cast<double>(static_cast<float>(raw)) : raw;
                  if (isF32 && std::isinf(r.floatVal))
                    throw UndefinedBehaviorError("UB: vector lane f32 overflow to inf");
                } else {
                  throw std::runtime_error("Vector cast: unsupported lane dst type");
                }
                res.arrayVal.push_back(std::move(r));
              }
              return res;
            }

            RuntimeValue res;
            auto dstBits = TypeUtils::getBitWidth(arg.dstType);
            if (dstBits) {
              res.kind = RuntimeValue::Kind::Int;
              res.bits = *dstBits;
              if (v.kind == RuntimeValue::Kind::Int) {
                res.intVal = canonicalize(v.intVal, res.bits);
              } else {
                // Float to Int: check for out-of-range (spec §7.4 rule 8).
                // lo = -2^(bits-1), hi = 2^(bits-1); valid range is [lo, hi).
                double lo = -std::ldexp(1.0, static_cast<int>(res.bits) - 1);
                double hi = std::ldexp(1.0, static_cast<int>(res.bits) - 1);
                if (std::isnan(v.floatVal) || std::isinf(v.floatVal) || v.floatVal < lo ||
                    v.floatVal >= hi)
                  throw UndefinedBehaviorError("UB: Float-to-integer cast out of range");
                res.intVal = static_cast<int64_t>(v.floatVal);
              }
            } else if (arg.dstType && std::holds_alternative<FloatType>(arg.dstType->v)) {
              res.kind = RuntimeValue::Kind::Float;
              bool isF32 = std::get<FloatType>(arg.dstType->v).kind == FloatType::Kind::F32;
              res.bits = isF32 ? 32 : 64;
              double raw =
                  (v.kind == RuntimeValue::Kind::Int) ? static_cast<double>(v.intVal) : v.floatVal;
              // For f32 destination, round to f32 precision so the stored
              // value matches what C/WASM lowering would compute. Without
              // this, e.g. `268435457 as f32` would stay as 268435457.0 in
              // double precision instead of being rounded to 268435456.0f
              // (round-to-nearest-even at the f32 boundary).
              res.floatVal = isF32 ? static_cast<double>(static_cast<float>(raw)) : raw;
              // SPEC §6.4 / §7.4 rule 6: f64 -> f32 narrowing is UB if the
              // result overflows to ±∞ (e.g. 1e40 as f32). Trap here for any
              // FP -> f32 that produced ±∞ after rounding (integer sources
              // can't overflow per spec, but the check covers them harmlessly).
              if (isF32 && std::isinf(res.floatVal))
                throw UndefinedBehaviorError("UB: Float narrowing cast overflows to infinity");
            }
            return res;
          }
          return RuntimeValue{};
        },
        a.v
    );
  }

  Interpreter::RuntimeValue Interpreter::evalCoef(const Coef &c, const Store &store) {
    if (std::holds_alternative<IntLit>(c)) {
      RuntimeValue rv;
      rv.kind = RuntimeValue::Kind::Int;
      rv.intVal = std::get<IntLit>(c).value;
      rv.bits = 64;
      return rv;
    }
    if (std::holds_alternative<FloatLit>(c)) {
      RuntimeValue rv;
      rv.kind = RuntimeValue::Kind::Float;
      rv.floatVal = std::get<FloatLit>(c).value;
      rv.bits = 64;
      return rv;
    }
    if (std::holds_alternative<NullLit>(c)) {
      RuntimeValue rv;
      rv.kind = RuntimeValue::Kind::Ptr;
      rv.ptrVal = 0;
      rv.bits = 64;
      return rv;
    }
    const auto &id = std::get<LocalOrSymId>(c);
    if (auto lid = std::get_if<LocalId>(&id))
      return store.at(lid->name);
    auto sid = std::get_if<SymId>(&id);
    if (store.count(sid->name))
      return store.at(sid->name);
    throw std::runtime_error("Internal error: Unbound symbol " + sid->name);
  }

  Interpreter::RuntimeValue Interpreter::evalSelectVal(const SelectVal &sv, const Store &store) {
    if (std::holds_alternative<RValue>(sv))
      return evalLValue(std::get<RValue>(sv), store);
    return evalCoef(std::get<Coef>(sv), store);
  }

  Interpreter::RuntimeValue Interpreter::evalLValue(const LValue &lv, const Store &store) {
    const RuntimeValue *cur = &store.at(lv.base.name);

    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        if (cur->kind == RuntimeValue::Kind::Undef)
          throw UndefinedBehaviorError("UB: Reading field of undef");
        if (cur->kind != RuntimeValue::Kind::Array && cur->kind != RuntimeValue::Kind::Vec)
          throw std::runtime_error("Indexing non-array");

        // Eval index
        RuntimeValue idxVal;
        const auto &idx = ai->index;
        if (std::holds_alternative<IntLit>(idx))
          idxVal.intVal = std::get<IntLit>(idx).value;
        else {
          const auto &id = std::get<LocalOrSymId>(idx);
          if (auto lid = std::get_if<LocalId>(&id))
            idxVal = store.at(lid->name);
          else { // SymId
            auto sid = std::get_if<SymId>(&id);
            idxVal = store.at(sid->name);
          }
        }
        if (idxVal.kind == RuntimeValue::Kind::Undef)
          throw UndefinedBehaviorError("UB: Undef index");

        if (idxVal.intVal < 0 || (size_t) idxVal.intVal >= cur->arrayVal.size())
          throw UndefinedBehaviorError(
              cur->kind == RuntimeValue::Kind::Vec ? "UB: Vector lane index out of bounds"
                                                   : "UB: Array index out of bounds"
          );

        cur = &cur->arrayVal[idxVal.intVal];
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (cur->kind == RuntimeValue::Kind::Undef)
          throw UndefinedBehaviorError("UB: Reading field of undef");
        if (cur->kind != RuntimeValue::Kind::Struct)
          throw std::runtime_error("Accessing field of non-struct");
        auto it = cur->structVal.find(af->field);
        if (it == cur->structVal.end())
          throw UndefinedBehaviorError("UB: Uninitialized field read");
        cur = &it->second;
      }
    }
    if (cur->kind == RuntimeValue::Kind::Undef)
      throw UndefinedBehaviorError("UB: Reading undef value");
    return *cur;
  }

  void Interpreter::setLValue(const LValue &lv, RuntimeValue val, Store &store) {
    RuntimeValue *cur = &store.at(lv.base.name);

    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        if (cur->kind != RuntimeValue::Kind::Array && cur->kind != RuntimeValue::Kind::Vec)
          throw std::runtime_error("Indexing non-array");
        RuntimeValue idxVal;
        const auto &idx = ai->index;
        if (std::holds_alternative<IntLit>(idx))
          idxVal.intVal = std::get<IntLit>(idx).value;
        else {
          const auto &id = std::get<LocalOrSymId>(idx);
          if (auto lid = std::get_if<LocalId>(&id))
            idxVal = store.at(lid->name);
          else {
            auto sid = std::get_if<SymId>(&id);
            idxVal = store.at(sid->name);
          }
        }
        if (idxVal.intVal < 0 || (size_t) idxVal.intVal >= cur->arrayVal.size())
          throw UndefinedBehaviorError(
              cur->kind == RuntimeValue::Kind::Vec ? "UB: Vector lane index out of bounds"
                                                   : "UB: Array index out of bounds"
          );
        cur = &cur->arrayVal[idxVal.intVal];
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (cur->kind != RuntimeValue::Kind::Struct)
          throw std::runtime_error("Accessing field of non-struct");
        cur = &cur->structVal[af->field];
      }
    }

    // Enforce destination precision.
    //   Int:   canonicalize to bit width.
    //   Float: round to declared precision (f32 destinations must store the
    //          f32 image, not the wider double the RHS evaluator may have
    //          produced — evalCoef tags FloatLit with bits=64 by default).
    if (val.kind == RuntimeValue::Kind::Int) {
      val.bits = cur->bits;
      val.intVal = canonicalize(val.intVal, val.bits);
    } else if (val.kind == RuntimeValue::Kind::Float) {
      val.bits = cur->bits;
      if (val.bits == 32)
        val.floatVal = static_cast<double>(static_cast<float>(val.floatVal));
    }
    *cur = val;

    // Spec §9.4.7 / interp Store↔Heap consistency: when an addr-taken local
    // is updated via a direct assignment, the heap-side mirror must reflect
    // the new value so subsequent loads through `load <ptr>` see it.
    auto ait = addrMap_.find(lv.base.name);
    if (ait == addrMap_.end())
      return;
    uint64_t base = ait->second;
    const RuntimeValue &top = store.at(lv.base.name);
    if (lv.accesses.empty()) {
      heap_[base] = top;
      return;
    }
    // For nested accesses, recompute the offset relative to base.
    uint64_t addr = base;
    const RuntimeValue *walk = &top;
    auto typeIt = typeMap_.find(lv.base.name);
    TypePtr curType = (typeIt != typeMap_.end()) ? typeIt->second : nullptr;
    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        if (walk->kind != RuntimeValue::Kind::Array)
          return;
        int64_t idx = 0;
        if (std::holds_alternative<IntLit>(ai->index))
          idx = std::get<IntLit>(ai->index).value;
        else {
          const auto &id = std::get<LocalOrSymId>(ai->index);
          idx = std::visit([&](auto &&v) { return store.at(v.name).intVal; }, id);
        }
        if (idx < 0 || (size_t) idx >= walk->arrayVal.size())
          return;
        uint64_t elemSize = 0;
        if (curType) {
          if (auto at = std::get_if<ArrayType>(&curType->v)) {
            auto bw = TypeUtils::getBitWidth(at->elem);
            elemSize = bw ? (*bw + 7) / 8
                       : std::holds_alternative<FloatType>(at->elem->v)
                           ? (std::get<FloatType>(at->elem->v).kind == FloatType::Kind::F32 ? 4 : 8)
                           : 8;
            curType = at->elem;
          }
        }
        if (elemSize == 0)
          return; // no type info — give up syncing nested case
        addr += idx * elemSize;
        walk = &walk->arrayVal[idx];
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (walk->kind != RuntimeValue::Kind::Struct)
          return;
        if (!curType)
          return;
        auto st = std::get_if<StructType>(&curType->v);
        if (!st)
          return;
        auto sit = structs_.find(st->name.name);
        if (sit == structs_.end())
          return;
        addr += fieldOffset(*sit->second, af->field);
        for (const auto &f: sit->second->fields)
          if (f.name == af->field) {
            curType = f.type;
            break;
          }
        auto sfit = walk->structVal.find(af->field);
        if (sfit == walk->structVal.end())
          return;
        walk = &sfit->second;
      }
    }
    heap_[addr] = *walk;
  }

  bool Interpreter::evalCond(const Cond &c, const Store &store) {
    RuntimeValue l = evalExpr(c.lhs, store);
    RuntimeValue r = evalExpr(c.rhs, store);

    if (l.kind == RuntimeValue::Kind::Undef || r.kind == RuntimeValue::Kind::Undef)
      throw UndefinedBehaviorError("UB: Reading undef in condition");

    // Promote Int to Float if needed (Literal inference support)
    if (l.kind == RuntimeValue::Kind::Float && r.kind == RuntimeValue::Kind::Int) {
      r.floatVal = static_cast<double>(r.intVal);
      r.kind = RuntimeValue::Kind::Float;
    } else if (l.kind == RuntimeValue::Kind::Int && r.kind == RuntimeValue::Kind::Float) {
      l.floatVal = static_cast<double>(l.intVal);
      l.kind = RuntimeValue::Kind::Float;
    }

    if (l.kind == RuntimeValue::Kind::Int && r.kind == RuntimeValue::Kind::Int) {
      switch (c.op) {
        case RelOp::EQ:
          return l.intVal == r.intVal;
        case RelOp::NE:
          return l.intVal != r.intVal;
        case RelOp::LT:
          return l.intVal < r.intVal;
        case RelOp::LE:
          return l.intVal <= r.intVal;
        case RelOp::GT:
          return l.intVal > r.intVal;
        case RelOp::GE:
          return l.intVal >= r.intVal;
      }
    } else if (l.kind == RuntimeValue::Kind::Float && r.kind == RuntimeValue::Kind::Float) {
      switch (c.op) {
        case RelOp::EQ:
          return l.floatVal == r.floatVal;
        case RelOp::NE:
          return l.floatVal != r.floatVal;
        case RelOp::LT:
          return l.floatVal < r.floatVal;
        case RelOp::LE:
          return l.floatVal <= r.floatVal;
        case RelOp::GT:
          return l.floatVal > r.floatVal;
        case RelOp::GE:
          return l.floatVal >= r.floatVal;
      }
    }
    if (l.kind == RuntimeValue::Kind::Ptr && r.kind == RuntimeValue::Kind::Ptr) {
      switch (c.op) {
        case RelOp::EQ:
          return l.ptrVal == r.ptrVal;
        case RelOp::NE:
          return l.ptrVal != r.ptrVal;
        case RelOp::LT:
        case RelOp::LE:
        case RelOp::GT:
        case RelOp::GE: {
          // Relational comparison requires same provenance object (spec §7.5 rule 14).
          // ptrBase==0 implies null or invalid pointer — always UB for relational ops.
          if (l.ptrBase == 0 || r.ptrBase == 0 || l.ptrBase != r.ptrBase)
            throw UndefinedBehaviorError(
                "UB: Relational pointer comparison across different objects"
            );
          if (c.op == RelOp::LT)
            return l.ptrVal < r.ptrVal;
          if (c.op == RelOp::LE)
            return l.ptrVal <= r.ptrVal;
          if (c.op == RelOp::GT)
            return l.ptrVal > r.ptrVal;
          return l.ptrVal >= r.ptrVal;
        }
      }
    }
    throw std::runtime_error("Cond operands must be same scalar kind");
  }

} // namespace symir
