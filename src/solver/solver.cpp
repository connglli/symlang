#include "solver/solver.hpp"
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include "analysis/cfg.hpp"

namespace symir {

  thread_local const FunDecl *SymbolicExecutor::currentFun_ = nullptr;

  // Pointers are encoded as 64-bit BV tags identifying the addressed local.
  // Tag 0 is reserved for null. Tags are derived deterministically from the
  // local name via FNV-1a so they remain stable across solver invocations.
  static constexpr uint32_t kPtrBits = 64;

  static uint64_t tagOfLocal(const std::string &name) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
    for (unsigned char c: name) {
      h ^= c;
      h *= 1099511628211ULL; // FNV-1a 64-bit prime
    }
    if (h == 0)
      h = 1; // never collide with null
    return h;
  }

  // Map RelOp to the appropriate SMT comparison kind, choosing BV or FP
  // variants based on isFp.
  static smt::Kind relOpToSmtKind(RelOp op, bool isFp) {
    switch (op) {
      case RelOp::EQ:
        return smt::Kind::EQUAL;
      case RelOp::NE:
        return smt::Kind::DISTINCT;
      case RelOp::LT:
        return isFp ? smt::Kind::FP_LT : smt::Kind::BV_SLT;
      case RelOp::LE:
        return isFp ? smt::Kind::FP_LEQ : smt::Kind::BV_SLE;
      case RelOp::GT:
        return isFp ? smt::Kind::FP_GT : smt::Kind::BV_SGT;
      case RelOp::GE:
        return isFp ? smt::Kind::FP_GEQ : smt::Kind::BV_SGE;
    }
    return smt::Kind::EQUAL;
  }

  // Size of a type measured in BV-tag units (one unit per scalar leaf).
  // Used by the pointer encoding so that:
  //   * `addr %arr[k]` on `[N] T` advances by `k * sizeofTagUnits(T)`
  //   * pointer arithmetic on `ptr T` scales by `sizeofTagUnits(T)`
  //   * `ptrfield`/`ptrindex` adds the right offset for nested aggregates
  // Forward-declared so other helpers can refer to it; defined below.
  static std::uint64_t sizeofTagUnits(
      const TypePtr &t, const std::unordered_map<std::string, const StructDecl *> &structs
  ) {
    if (!t)
      return 1;
    if (auto at = std::get_if<ArrayType>(&t->v))
      return at->size * sizeofTagUnits(at->elem, structs);
    if (auto st = std::get_if<StructType>(&t->v)) {
      auto sIt = structs.find(st->name.name);
      if (sIt == structs.end())
        return 1;
      std::uint64_t sum = 0;
      for (const auto &f: sIt->second->fields)
        sum += sizeofTagUnits(f.type, structs);
      return sum;
    }
    return 1; // scalar / ptr / vec
  }

  // Byte offset (in tag units) of the named struct field.
  static std::uint64_t fieldOffsetTagUnits(
      const StructDecl &s, const std::string &field,
      const std::unordered_map<std::string, const StructDecl *> &structs
  ) {
    std::uint64_t off = 0;
    for (const auto &f: s.fields) {
      if (f.name == field)
        return off;
      off += sizeofTagUnits(f.type, structs);
    }
    return off;
  }

  // Compare SymIR types for structural equality at the level we care about
  // (matters when enumerating candidate ptr targets in load/store dispatch).
  static bool typeMatch(const TypePtr &a, const TypePtr &b) {
    if (!a || !b)
      return a == b;
    if (a->v.index() != b->v.index())
      return false;
    if (auto pa = std::get_if<IntType>(&a->v)) {
      auto pb = std::get_if<IntType>(&b->v);
      auto width = [](const IntType &t) -> uint32_t {
        switch (t.kind) {
          case IntType::Kind::I32:
            return 32;
          case IntType::Kind::I64:
            return 64;
          case IntType::Kind::ICustom:
            return t.bits.value_or(32);
        }
        return 32;
      };
      return width(*pa) == width(*pb);
    }
    if (auto pa = std::get_if<FloatType>(&a->v)) {
      return pa->kind == std::get<FloatType>(b->v).kind;
    }
    if (auto pa = std::get_if<PtrType>(&a->v)) {
      return typeMatch(pa->pointee, std::get<PtrType>(b->v).pointee);
    }
    if (auto pa = std::get_if<VecType>(&a->v)) {
      auto pb = std::get_if<VecType>(&b->v);
      return pa->size == pb->size && typeMatch(pa->elem, pb->elem);
    }
    // Aggregate types as pointees are not supported in current solver dispatch.
    return false;
  }

  SymbolicExecutor::SymbolicExecutor(
      const Program &prog, const Config &config, SolverFactory solverFactory
  ) : prog_(prog), config_(config), solverFactory_(solverFactory) {
    for (const auto &s: prog_.structs) {
      structs_[s.name.name] = &s;
    }
  }

  smt::Sort SymbolicExecutor::getSort(const TypePtr &t, smt::ISolver &solver) {
    if (auto it = std::get_if<IntType>(&t->v)) {
      uint32_t bits = 32;
      switch (it->kind) {
        case IntType::Kind::I32:
          bits = 32;
          break;
        case IntType::Kind::I64:
          bits = 64;
          break;
        case IntType::Kind::ICustom:
          bits = it->bits.value_or(32);
          break;
      }
      return solver.make_bv_sort(bits);
    }
    if (auto ft = std::get_if<FloatType>(&t->v)) {
      if (ft->kind == FloatType::Kind::F32)
        return solver.make_fp_sort(8, 24);
      else
        return solver.make_fp_sort(11, 53);
    }
    if (auto at = std::get_if<ArrayType>(&t->v)) {
      return getSort(at->elem, solver);
    }
    if (auto st = std::get_if<StructType>(&t->v)) {
      auto it = structs_.find(st->name.name);
      if (it != structs_.end() && !it->second->fields.empty()) {
        return getSort(it->second->fields[0].type, solver);
      }
    }
    if (std::holds_alternative<PtrType>(t->v)) {
      return solver.make_bv_sort(kPtrBits);
    }
    if (auto vt = std::get_if<VecType>(&t->v)) {
      // [v0.2.1] Vectors aren't a single SMT sort; lanes are held as N
      // independent terms in SymbolicValue::arrayVal. getSort returns the
      // lane sort so any downstream caller that wants "what kind of term
      // is in each lane?" gets the right answer.
      return getSort(vt->elem, solver);
    }
    throw std::runtime_error("Unknown type or empty struct in getSort");
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::createSymbolicValue(
      const TypePtr &t, const std::string &name, smt::ISolver &solver, bool
  ) {
    SymbolicValue res;
    if (auto at = std::get_if<ArrayType>(&t->v)) {
      res.kind = SymbolicValue::Kind::Array;
      for (size_t i = 0; i < at->size; ++i) {
        res.arrayVal.push_back(
            createSymbolicValue(at->elem, name + "[" + std::to_string(i) + "]", solver)
        );
      }
    } else if (auto vt = std::get_if<VecType>(&t->v)) {
      // [v0.2.1] Vector sym: N independent lane-symbolic constants
      // (§9.5.1). Same shape as Array but tagged Vec so downstream
      // dispatch picks the lane-wise UB path.
      res.kind = SymbolicValue::Kind::Vec;
      for (size_t i = 0; i < vt->size; ++i) {
        res.arrayVal.push_back(
            createSymbolicValue(vt->elem, name + "[" + std::to_string(i) + "]", solver)
        );
      }
    } else if (auto st = std::get_if<StructType>(&t->v)) {
      res.kind = SymbolicValue::Kind::Struct;
      auto it = structs_.find(st->name.name);
      if (it != structs_.end()) {
        for (const auto &f: it->second->fields) {
          res.structVal[f.name] = createSymbolicValue(f.type, name + "." + f.name, solver);
        }
      }
    } else {
      res.kind = SymbolicValue::Kind::Int;
      res.term = solver.make_const(getSort(t, solver), name);
      res.is_defined = solver.make_true();
    }
    return res;
  }

  SymbolicExecutor::SymbolicValue
  SymbolicExecutor::makeUndef(const TypePtr &t, smt::ISolver &solver) {
    SymbolicValue res;
    if (auto at = std::get_if<ArrayType>(&t->v)) {
      res.kind = SymbolicValue::Kind::Array;
      for (size_t i = 0; i < at->size; ++i)
        res.arrayVal.push_back(makeUndef(at->elem, solver));
    } else if (auto vt = std::get_if<VecType>(&t->v)) {
      res.kind = SymbolicValue::Kind::Vec;
      for (size_t i = 0; i < vt->size; ++i)
        res.arrayVal.push_back(makeUndef(vt->elem, solver));
    } else if (auto st = std::get_if<StructType>(&t->v)) {
      res.kind = SymbolicValue::Kind::Struct;
      auto it = structs_.find(st->name.name);
      if (it != structs_.end()) {
        for (const auto &f: it->second->fields)
          res.structVal[f.name] = makeUndef(f.type, solver);
      }
    } else {
      res.kind = SymbolicValue::Kind::Int;
      res.term = solver.make_const(getSort(t, solver), "undef");
      res.is_defined = solver.make_false();
    }
    return res;
  }

  SymbolicExecutor::SymbolicValue
  SymbolicExecutor::broadcast(const TypePtr &t, smt::Term val, smt::ISolver &solver) {
    if (std::holds_alternative<ArrayType>(t->v)) {
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Array;
      const auto &at = std::get<ArrayType>(t->v);
      for (size_t i = 0; i < at.size; ++i)
        res.arrayVal.push_back(broadcast(at.elem, val, solver));
      return res;
    } else if (std::holds_alternative<VecType>(t->v)) {
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Vec;
      const auto &vt = std::get<VecType>(t->v);
      for (size_t i = 0; i < vt.size; ++i)
        res.arrayVal.push_back(broadcast(vt.elem, val, solver));
      return res;
    } else if (std::holds_alternative<StructType>(t->v)) {
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Struct;
      auto it = structs_.find(std::get<StructType>(t->v).name.name);
      if (it != structs_.end()) {
        for (const auto &f: it->second->fields)
          res.structVal[f.name] = broadcast(f.type, val, solver);
      }
      return res;
    } else {
      // Scalar leaf (IntType, FloatType, PtrType).
      // The incoming `val` term may have been constructed for a *different*
      // scalar type (e.g., BV[8] when this leaf is i32) because evalInit
      // derives the zero from getSort(struct), which returns the sort of
      // the first field's element — not this particular field's sort.
      // Resize BV terms to the correct width so every field gets a
      // correctly-sorted SMT constant.  All callers that broadcast an
      // integer literal zero just want "zero of the right width", so
      // sign-extension (= zero-extension for 0) is always correct here.
      smt::Term leaf = val;
      auto targetSort = getSort(t, solver);
      auto valSort = solver.get_sort(val);
      if (solver.is_bv_sort(valSort) && solver.is_bv_sort(targetSort)) {
        uint32_t vw = solver.get_bv_width(valSort);
        uint32_t tw = solver.get_bv_width(targetSort);
        if (vw < tw)
          leaf = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {val}, {tw - vw});
        else if (vw > tw)
          // Truncate: re-create the constant at the narrower sort.
          // We only reach here for literal broadcasts (= 0), so the
          // numeric value already fits in tw bits.
          leaf = solver.make_bv_value(targetSort, "0", 10);
      }
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Int;
      res.term = leaf;
      res.is_defined = solver.make_true();
      return res;
    }
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::evalInit(
      const InitVal &iv, const TypePtr &t, smt::ISolver &solver, SymbolicStore &store,
      std::vector<smt::Term> &pc
  ) {
    if (iv.kind == InitVal::Kind::Undef)
      return makeUndef(t, solver);

    // [v0.2.1] Atom-form initializer (addr, load, cmp, ptrindex, etc.)
    if (iv.kind == InitVal::Kind::Atom) {
      const auto &atom = *std::get<AtomPtr>(iv.value);
      if (auto vt = std::get_if<VecType>(&t->v)) {
        return evalVecExprAtom(atom, *vt, solver, store, pc);
      }
      return evalAtom(atom, solver, store, pc, getSort(t, solver));
    }

    if (iv.kind == InitVal::Kind::Aggregate) {
      const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
      if (auto at = std::get_if<ArrayType>(&t->v)) {
        SymbolicValue res;
        res.kind = SymbolicValue::Kind::Array;
        for (size_t i = 0; i < elements.size(); ++i)
          res.arrayVal.push_back(evalInit(*elements[i], at->elem, solver, store, pc));
        return res;
      } else if (auto vt = std::get_if<VecType>(&t->v)) {
        SymbolicValue res;
        res.kind = SymbolicValue::Kind::Vec;
        for (size_t i = 0; i < elements.size(); ++i)
          res.arrayVal.push_back(evalInit(*elements[i], vt->elem, solver, store, pc));
        return res;
      } else if (auto st = std::get_if<StructType>(&t->v)) {
        SymbolicValue res;
        res.kind = SymbolicValue::Kind::Struct;
        auto it = structs_.find(st->name.name);
        if (it != structs_.end()) {
          for (size_t i = 0; i < elements.size(); ++i)
            res.structVal[it->second->fields[i].name] =
                evalInit(*elements[i], it->second->fields[i].type, solver, store, pc);
        }
        return res;
      }
    }

    // Scalar
    smt::Term val;
    if (iv.kind == InitVal::Kind::Null) {
      // null pointer: BV64 zero.
      val = solver.make_bv_value(solver.make_bv_sort(kPtrBits), "0", 10);
      return broadcast(t, val, solver);
    }
    if (iv.kind == InitVal::Kind::Int) {
      auto lit = std::get<IntLit>(iv.value);
      TypePtr leafType = t;
      while (auto *at = std::get_if<ArrayType>(&leafType->v)) {
        leafType = at->elem;
      }
      val = solver.make_bv_value(getSort(leafType, solver), std::to_string(lit.value), 10);
    } else if (iv.kind == InitVal::Kind::Float) {
      auto lit = std::get<FloatLit>(iv.value);
      TypePtr leafType = t;
      while (auto *at = std::get_if<ArrayType>(&leafType->v)) {
        leafType = at->elem;
      }
      // Using standard RNE
      val = solver.make_fp_value(
          getSort(leafType, solver), std::to_string(lit.value), smt::RoundingMode::RNE
      );
    } else if (iv.kind == InitVal::Kind::Sym) {
      return store.at(std::get<SymId>(iv.value).name);
    } else if (iv.kind == InitVal::Kind::Local) {
      return store.at(std::get<LocalId>(iv.value).name);
    }
    return broadcast(t, val, solver);
  }

  SymbolicExecutor::Result SymbolicExecutor::solve(
      const std::string &funcName, const std::vector<std::string> &path,
      const std::unordered_map<std::string, int64_t> &fixedSyms
  ) {

    const FunDecl *entry = nullptr;
    for (const auto &f: prog_.funs) {
      if (f.name.name == funcName) {
        entry = &f;
        break;
      }
    }
    if (!entry)
      throw std::runtime_error("Function not found: " + funcName);

    // Make the current FunDecl visible to evalAtom/StoreInstr handlers via
    // thread_local storage. Restored on scope exit so nested or concurrent
    // solve() invocations on different threads see their own value.
    struct FunGuard {
      const FunDecl *prev;

      ~FunGuard() { SymbolicExecutor::currentFun_ = prev; }
    } funGuard{currentFun_};

    currentFun_ = entry;

    auto solverPtr = solverFactory_(config_);
    smt::ISolver &solver = *solverPtr;

    SymbolicStore store;
    std::vector<smt::Term> pathConstraints;
    std::vector<smt::Term> requirements;

    // [v0.2.1] Reset per-solve provenance tracking. Each call to solve()
    // walks a fresh CFG path, so prior provenance state must not leak.
    ptrProv_.clear();

    // 1. Declare symbols and fix values if requested
    for (const auto &s: entry->syms) {
      auto sv = createSymbolicValue(s.type, s.name.name, solver, true);
      store[s.name.name] = sv;

      // [v0.2.1] Vector sym: collect the per-lane terms so the domain/fix
      // logic below can apply constraints per lane. For scalar sym this
      // is just `{sv.term}` (one element).
      std::vector<smt::Term> symLaneTerms;
      TypePtr symLaneType;
      if (sv.kind == SymbolicValue::Kind::Vec) {
        for (const auto &lane: sv.arrayVal)
          symLaneTerms.push_back(lane.term);
        if (auto vt = std::get_if<VecType>(&s.type->v))
          symLaneType = vt->elem;
      } else {
        symLaneTerms.push_back(sv.term);
        symLaneType = s.type;
      }

      // Add domain constraints
      if (s.domain) {
        std::visit(
            [&](auto &&d) {
              using T = std::decay_t<decltype(d)>;
              // Domain constraints apply per-lane for vector syms (§3.4.1
              // says each lane gets the same domain).
              auto laneSort = getSort(symLaneType, solver);
              if constexpr (std::is_same_v<T, DomainInterval>) {
                uint32_t bits = 64;
                if (auto *it = std::get_if<IntType>(&symLaneType->v)) {
                  switch (it->kind) {
                    case IntType::Kind::I32:
                      bits = 32;
                      break;
                    case IntType::Kind::I64:
                      bits = 64;
                      break;
                    case IntType::Kind::ICustom:
                      bits = it->bits.value_or(32);
                      break;
                  }
                }
                int64_t effLo = d.lo, effHi = d.hi;
                if (bits < 64) {
                  int64_t typeLo = -(1LL << (bits - 1));
                  int64_t typeHi = (1LL << (bits - 1)) - 1;
                  effLo = std::max(effLo, typeLo);
                  effHi = std::min(effHi, typeHi);
                }
                if (effLo <= effHi) {
                  auto lo = solver.make_bv_value_int64(laneSort, effLo);
                  auto hi = solver.make_bv_value_int64(laneSort, effHi);
                  for (const auto &t: symLaneTerms) {
                    pathConstraints.push_back(solver.make_term(smt::Kind::BV_SLE, {lo, t}));
                    pathConstraints.push_back(solver.make_term(smt::Kind::BV_SLE, {t, hi}));
                  }
                }
              } else if constexpr (std::is_same_v<T, DomainSet>) {
                for (const auto &t: symLaneTerms) {
                  std::vector<smt::Term> or_terms;
                  for (auto v: d.values) {
                    auto vt = solver.make_bv_value_int64(laneSort, v);
                    or_terms.push_back(solver.make_term(smt::Kind::EQUAL, {t, vt}));
                  }
                  if (!or_terms.empty()) {
                    smt::Term or_all = or_terms[0];
                    for (size_t i = 1; i < or_terms.size(); ++i)
                      or_all = solver.make_term(smt::Kind::OR, {or_all, or_terms[i]});
                    pathConstraints.push_back(or_all);
                  }
                }
              }
            },
            *s.domain
        );
      }

      if (fixedSyms.count(s.name.name)) {
        auto val =
            solver.make_bv_value_int64(getSort(symLaneType, solver), fixedSyms.at(s.name.name));
        for (const auto &t: symLaneTerms)
          pathConstraints.push_back(solver.make_term(smt::Kind::EQUAL, {t, val}));
      }
    }

    // 2. Declare locals (parameters are also in store)
    for (const auto &p: entry->params) {
      store[p.name.name] = createSymbolicValue(p.type, p.name.name, solver);
    }
    for (const auto &l: entry->lets) {
      if (l.init) {
        store[l.name.name] = evalInit(*l.init, l.type, solver, store, pathConstraints);
      } else {
        store[l.name.name] = makeUndef(l.type, solver);
      }
    }

    // 3. CFG Build for label mapping
    DiagBag diags;
    CFG cfg = CFG::build(*entry, diags);
    if (diags.hasErrors())
      throw std::runtime_error("CFG build failed");

    // 4. Path traversal
    for (size_t i = 0; i < path.size(); ++i) {
      const std::string &label = path[i];
      if (cfg.indexOf.find(label) == cfg.indexOf.end())
        throw std::runtime_error("Invalid block label in path: " + label);

      const Block &block = entry->blocks[cfg.indexOf.at(label)];

      for (const auto &ins: block.instrs) {
        std::visit(
            [&](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, AssignInstr>) {
                auto lhsVal =
                    evalLValue(arg.lhs, solver, store, pathConstraints, /*forWrite=*/true);
                // [v0.2.1] Vector LHS: evaluate RHS as a SymbolicValue::Vec.
                if (lhsVal.kind == SymbolicValue::Kind::Vec && arg.lhs.accesses.empty()) {
                  // Find the LHS local's declared VecType.
                  TypePtr lhsType;
                  if (currentFun_) {
                    for (const auto &l: currentFun_->lets)
                      if (l.name.name == arg.lhs.base.name) {
                        lhsType = l.type;
                        break;
                      }
                    if (!lhsType)
                      for (const auto &p: currentFun_->params)
                        if (p.name.name == arg.lhs.base.name) {
                          lhsType = p.type;
                          break;
                        }
                  }
                  if (lhsType && std::holds_alternative<VecType>(lhsType->v)) {
                    auto &vt = std::get<VecType>(lhsType->v);
                    SymbolicValue rhsV = evalVecExpr(arg.rhs, vt, solver, store, pathConstraints);
                    setLValue(arg.lhs, rhsV, solver, store, pathConstraints);
                    return;
                  }
                }
                auto rhs = evalExpr(
                    arg.rhs, solver, store, pathConstraints,
                    lhsVal.kind == SymbolicValue::Kind::Int
                        ? std::optional(solver.get_sort(lhsVal.term))
                        : std::nullopt
                );
                setLValue(arg.lhs, rhs, solver, store, pathConstraints);
                // [v0.2.1] Track ptr provenance for cross-object and one-
                // past-end UB checks. The LHS is either a whole-local ptr
                // or a ptr-typed struct field path (`%s.p1`); we mirror
                // the RHS atom's provenance (addr / ptrindex / ptrfield
                // set a known provenance, pointer arithmetic preserves
                // it, load-derived ptrs are unknown).
                if (currentFun_) {
                  // Resolve the LHS type by walking accesses.
                  auto resolveLhsType = [&]() -> TypePtr {
                    TypePtr cur;
                    for (const auto &l: currentFun_->lets)
                      if (l.name.name == arg.lhs.base.name) {
                        cur = l.type;
                        break;
                      }
                    if (!cur)
                      for (const auto &p: currentFun_->params)
                        if (p.name.name == arg.lhs.base.name) {
                          cur = p.type;
                          break;
                        }
                    for (const auto &acc: arg.lhs.accesses) {
                      if (!cur)
                        return nullptr;
                      if (auto af = std::get_if<AccessField>(&acc)) {
                        auto st = std::get_if<StructType>(&cur->v);
                        if (!st)
                          return nullptr;
                        auto sIt = structs_.find(st->name.name);
                        if (sIt == structs_.end())
                          return nullptr;
                        cur = nullptr;
                        for (const auto &f: sIt->second->fields)
                          if (f.name == af->field) {
                            cur = f.type;
                            break;
                          }
                      } else if (auto ai = std::get_if<AccessIndex>(&acc)) {
                        (void) ai;
                        if (auto at = std::get_if<ArrayType>(&cur->v))
                          cur = at->elem;
                        else if (auto vt = std::get_if<VecType>(&cur->v))
                          cur = vt->elem;
                        else
                          return nullptr;
                      }
                    }
                    return cur;
                  };
                  auto isPtr = [&]() {
                    auto t = resolveLhsType();
                    return t && std::holds_alternative<PtrType>(t->v);
                  };
                  auto buildLhsKey = [&]() -> std::string {
                    // Only field-keyed accesses (no dynamic indices).
                    std::string key = arg.lhs.base.name;
                    for (const auto &acc: arg.lhs.accesses) {
                      if (auto af = std::get_if<AccessField>(&acc)) {
                        key += "." + af->field;
                      } else {
                        return {};
                      }
                    }
                    return key;
                  };
                  std::string lhsKey = isPtr() ? buildLhsKey() : "";
                  if (!lhsKey.empty()) {
                    auto provFromName =
                        [&](const std::string &src) -> std::optional<PtrProvenance> {
                      auto it = ptrProv_.find(src);
                      if (it != ptrProv_.end())
                        return it->second;
                      return std::nullopt;
                    };
                    auto compute = [&]() -> std::optional<PtrProvenance> {
                      if (arg.rhs.rest.empty()) {
                        const auto &a = arg.rhs.first.v;
                        if (auto addr = std::get_if<AddrAtom>(&a)) {
                          // Provenance = the addressed local; size is the
                          // immediate containing object's total tag-unit
                          // span (spec rule 15). For `addr %arr[k]` that
                          // remains the whole array; for `addr %s.f` the
                          // whole struct.
                          uint64_t baseTag = tagOfLocal(addr->lv.base.name);
                          TypePtr ty;
                          for (const auto &l: currentFun_->lets)
                            if (l.name.name == addr->lv.base.name) {
                              ty = l.type;
                              break;
                            }
                          if (!ty)
                            for (const auto &p: currentFun_->params)
                              if (p.name.name == addr->lv.base.name) {
                                ty = p.type;
                                break;
                              }
                          std::uint64_t size = sizeofTagUnits(ty, structs_);
                          return PtrProvenance{baseTag, size};
                        }
                        if (auto pi = std::get_if<PtrIndexAtom>(&a)) {
                          // [v0.2.1 fix] Narrow provenance for ptrindex.
                          // The result pointer's provenance = the array the
                          // source points to. Compute the narrowed sub-array
                          // range from the source's type (ptr [N] T).
                          auto srcProv = provFromName(buildLValueKey(pi->rval));
                          if (srcProv && currentFun_) {
                            TypePtr baseType = resolveLValueType(pi->rval);
                            if (baseType) {
                              if (auto pt = std::get_if<PtrType>(&baseType->v)) {
                                if (auto at = std::get_if<ArrayType>(&pt->pointee->v)) {
                                  // Narrowed size = N * sizeofTagUnits(T)
                                  std::uint64_t elemUnits = sizeofTagUnits(at->elem, structs_);
                                  std::uint64_t narrowSize = at->size * elemUnits;
                                  // The narrowed base = source ptrVal at
                                  // assignment time. We don't have the
                                  // concrete tag here, so approximate:
                                  // baseTag stays at the source's base but
                                  // size is narrowed.
                                  return PtrProvenance{srcProv->baseTag, narrowSize};
                                }
                              }
                            }
                          }
                          return srcProv;
                        }
                        if (auto pf = std::get_if<PtrFieldAtom>(&a))
                          return provFromName(buildLValueKey(pf->rval));
                        if (auto ca = std::get_if<CoefAtom>(&a)) {
                          if (auto id = std::get_if<LocalOrSymId>(&ca->coef))
                            if (auto lid = std::get_if<LocalId>(id))
                              return provFromName(lid->name);
                        }
                        if (auto rv = std::get_if<RValueAtom>(&a)) {
                          return provFromName(buildLValueKey(rv->rval));
                        }
                        // LoadAtom-derived ptrs: provenance unknown.
                      } else {
                        // `%p = %q + i` style: provenance carries from %q.
                        const auto &a = arg.rhs.first.v;
                        if (auto ca = std::get_if<CoefAtom>(&a)) {
                          if (auto id = std::get_if<LocalOrSymId>(&ca->coef))
                            if (auto lid = std::get_if<LocalId>(id))
                              return provFromName(lid->name);
                        }
                        if (auto rv = std::get_if<RValueAtom>(&a)) {
                          return provFromName(buildLValueKey(rv->rval));
                        }
                      }
                      return std::nullopt;
                    };
                    auto newProv = compute();
                    if (newProv)
                      ptrProv_[lhsKey] = *newProv;
                    else
                      ptrProv_.erase(lhsKey);
                  }
                }
              } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                pathConstraints.push_back(evalCond(arg.cond, solver, store, pathConstraints));
              } else if constexpr (std::is_same_v<T, RequireInstr>) {
                requirements.push_back(evalCond(arg.cond, solver, store, pathConstraints));
              } else if constexpr (std::is_same_v<T, StoreInstr>) {
                // store %p, %v — mux-update every candidate target's value:
                //   for each %t of pointee type: %t := ite(p == tag_t, v, %t)
                if (!currentFun_)
                  throw std::runtime_error("store encountered without active FunDecl");

                // Evaluate ptr term (BV64) and stored value term.
                SymbolicValue ptrVal = evalExpr(arg.ptr, solver, store, pathConstraints);
                smt::Term ptrTerm = ptrVal.term;

                // [v0.2.1] Rule 9/11: store through null or OOB is UB.
                // The evalExpr above already pushes rule-10 OOB constraints
                // for ptr-arith expressions, but a bare `store %pa, v`
                // where %pa was set earlier still needs a null check.
                auto bv64Store = solver.make_bv_sort(kPtrBits);
                auto nullStore = solver.make_bv_value_int64(bv64Store, 0);
                pathConstraints.push_back(
                    solver.make_term(smt::Kind::DISTINCT, {ptrTerm, nullStore})
                );

                if (ptrVal.prov_base.internal && ptrVal.prov_size.internal) {
                  auto zero = solver.make_bv_value_int64(bv64Store, 0);
                  auto hasProv = solver.make_term(smt::Kind::DISTINCT, {ptrVal.prov_base, zero});
                  auto inBoundsLower =
                      solver.make_term(smt::Kind::BV_ULE, {ptrVal.prov_base, ptrTerm});
                  auto endAddr =
                      solver.make_term(smt::Kind::BV_ADD, {ptrVal.prov_base, ptrVal.prov_size});
                  auto inBoundsUpper = solver.make_term(smt::Kind::BV_ULT, {ptrTerm, endAddr});
                  auto cond = solver.make_term(
                      smt::Kind::IMPLIES,
                      {hasProv, solver.make_term(smt::Kind::AND, {inBoundsLower, inBoundsUpper})}
                  );
                  pathConstraints.push_back(cond);
                }

                // Determine pointee type from the ptr expression's first atom.
                TypePtr pointeeType;
                if (auto *rv = std::get_if<RValueAtom>(&arg.ptr.first.v)) {
                  TypePtr rvalType = resolveLValueType(rv->rval);
                  if (rvalType) {
                    if (auto pt = std::get_if<PtrType>(&rvalType->v)) {
                      pointeeType = pt->pointee;
                    }
                  }
                }
                if (!pointeeType)
                  throw std::runtime_error(
                      "store: cannot derive pointee type (only `store %p, ...` "
                      "with a ptr-typed local or parameter %p is currently supported)"
                  );

                auto pointeeSort = getSort(pointeeType, solver);
                SymbolicValue valVal =
                    evalExpr(arg.val, solver, store, pathConstraints, std::optional(pointeeSort));
                smt::Term valTerm = valVal.term;

                auto bv64 = solver.make_bv_sort(kPtrBits);
                // Mirror the load enumeration: recurse over (type, value,
                // offset) so a store can target any scalar leaf of a
                // nested aggregate — array-of-structs, struct-of-arrays.
                std::function<void(const TypePtr &, SymbolicValue &, std::uint64_t, std::uint64_t)>
                    enumStore;
                std::vector<smt::Term> storeMatchConds;
                enumStore = [&](const TypePtr &ty, SymbolicValue &sv, std::uint64_t baseTag,
                                std::uint64_t off) {
                  if (!ty)
                    return;
                  if (typeMatch(ty, pointeeType)) {
                    auto tagTerm =
                        solver.make_bv_value_int64(bv64, static_cast<int64_t>(baseTag + off));
                    auto cond = solver.make_term(smt::Kind::EQUAL, {ptrTerm, tagTerm});
                    storeMatchConds.push_back(cond);
                    sv.term = solver.make_term(smt::Kind::ITE, {cond, valTerm, sv.term});
                    return;
                  }
                  if (auto at = std::get_if<ArrayType>(&ty->v)) {
                    std::uint64_t stride = sizeofTagUnits(at->elem, structs_);
                    for (std::uint64_t k = 0; k < at->size && k < sv.arrayVal.size(); ++k)
                      enumStore(at->elem, sv.arrayVal[k], baseTag, off + k * stride);
                    return;
                  }
                  if (auto st = std::get_if<StructType>(&ty->v)) {
                    auto sIt = structs_.find(st->name.name);
                    if (sIt == structs_.end())
                      return;
                    std::uint64_t fOff = 0;
                    for (const auto &f: sIt->second->fields) {
                      auto fIt = sv.structVal.find(f.name);
                      if (fIt != sv.structVal.end())
                        enumStore(f.type, fIt->second, baseTag, off + fOff);
                      fOff += sizeofTagUnits(f.type, structs_);
                    }
                    return;
                  }
                };
                for (const auto &l: currentFun_->lets) {
                  std::uint64_t baseTag = tagOfLocal(l.name.name);
                  enumStore(l.type, store.at(l.name.name), baseTag, 0);
                }
                // [v0.2.1] Rule 11/15b: the store must land on a valid
                // T-typed cell — same as load's anyMatch constraint.
                if (!storeMatchConds.empty()) {
                  smt::Term anyMatch = storeMatchConds[0];
                  for (size_t j = 1; j < storeMatchConds.size(); ++j)
                    anyMatch = solver.make_term(smt::Kind::OR, {anyMatch, storeMatchConds[j]});
                  pathConstraints.push_back(anyMatch);
                }
              }
            },
            ins
        );
      }

      // Evaluate the terminator. For a conditional br on a non-final block
      // we also pick a side (then/else) per the path; the cond is evaluated
      // either way so that any UB triggered by computing the cond (e.g.
      // rule 14 cross-object pointer compare) is captured as a path
      // constraint even when the br is the final block.
      const std::string *nextLabel = (i + 1 < path.size()) ? &path[i + 1] : nullptr;
      std::visit(
          [&](auto &&term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, BrTerm>) {
              if (term.isConditional) {
                auto cond = evalCond(*term.cond, solver, store, pathConstraints);
                if (nextLabel) {
                  if (term.thenLabel.name == *nextLabel) {
                    pathConstraints.push_back(cond);
                  } else if (term.elseLabel.name == *nextLabel) {
                    pathConstraints.push_back(solver.make_term(smt::Kind::NOT, {cond}));
                  } else {
                    throw std::runtime_error(
                        "Path edge not in CFG: " + label + " -> " + *nextLabel
                    );
                  }
                }
                // No next block: cond was evaluated for UB side-effects only.
              } else if (nextLabel) {
                if (term.dest.name != *nextLabel)
                  throw std::runtime_error("Path edge not in CFG: " + label + " -> " + *nextLabel);
              }
            } else if constexpr (std::is_same_v<T, RetTerm>) {
              if (nextLabel)
                throw std::runtime_error(
                    "Block " + label + " ends with ret but path has more blocks"
                );
              // [v0.2.1] Evaluate the return value's expression so that
              // any UB checks it raises (div/mod by zero, signed overflow,
              // load OOB, read of undef, etc.) become path constraints.
              if (term.value) {
                (void) evalExpr(*term.value, solver, store, pathConstraints);
              }
            } else {
              if (nextLabel)
                throw std::runtime_error(
                    "Block " + label + " ends with non-branch terminator but path has more blocks"
                );
            }
          },
          block.term
      );
    }

    // 5. Solve
    for (auto c: pathConstraints)
      solver.assert_formula(c);
    for (auto r: requirements)
      solver.assert_formula(r);

    smt::Result res = solver.check_sat();
    Result finalRes;
    if (res == smt::Result::SAT) {
      finalRes.sat = true;
      // Local helper: extract one BV/FP lane's concrete value into a
      // ModelVal. Pulled out so the vector branch below can reuse it
      // without duplicating the FP bit-pattern reconstruction.
      auto extractValue = [&](const smt::Term &term) -> Result::ModelVal {
        auto val_term = solver.get_value(term);
        if (solver.is_fp_sort(solver.get_sort(term))) {
          std::string bin = solver.get_fp_value_string(val_term);
          uint64_t bits = 0;
          for (char c: bin)
            bits = (bits << 1) | (c - '0');
          double d;
          if (bin.size() <= 32) {
            uint32_t b32 = (uint32_t) bits;
            float f;
            std::memcpy(&f, &b32, sizeof(f));
            d = f;
          } else {
            std::memcpy(&d, &bits, sizeof(d));
          }
          return d;
        } else {
          auto val_str = solver.get_bv_value_string(val_term, 10);
          uint64_t uraw = std::stoull(val_str, nullptr, 10);
          uint32_t width = solver.get_bv_width(solver.get_sort(term));
          int64_t raw;
          if (width < 64) {
            uint64_t mask = (uint64_t(1) << width) - 1;
            uraw &= mask;
            if (uraw >= (uint64_t(1) << (width - 1)))
              raw = static_cast<int64_t>(uraw) - static_cast<int64_t>(mask + 1);
            else
              raw = static_cast<int64_t>(uraw);
          } else {
            raw = static_cast<int64_t>(uraw);
          }
          return raw;
        }
      };

      for (const auto &s: entry->syms) {
        const auto &sv = store.at(s.name.name);
        // [v0.2.1] Vector sym: extract one model value per lane.
        if (sv.kind == SymbolicValue::Kind::Vec) {
          std::vector<Result::ModelVal> lanes;
          lanes.reserve(sv.arrayVal.size());
          for (const auto &lane: sv.arrayVal)
            lanes.push_back(extractValue(lane.term));
          finalRes.vecModel[s.name.name] = std::move(lanes);
          continue;
        }
        finalRes.model[s.name.name] = extractValue(sv.term);
      }
    } else if (res == smt::Result::UNSAT) {
      finalRes.unsat = true;
    } else {
      finalRes.unknown = true;
    }
    return finalRes;
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::mergeAggregate(
      const std::vector<SymbolicValue> &elements, smt::Term idx, smt::ISolver &solver
  ) {
    if (elements.empty())
      return {SymbolicValue::Kind::Undef};

    if (elements[0].kind == SymbolicValue::Kind::Int) {
      smt::Term res = elements[0].term;
      smt::Term defined = elements[0].is_defined;
      auto targetSort = solver.get_sort(res);
      auto idxSort = solver.get_sort(idx);
      for (size_t i = 1; i < elements.size(); ++i) {
        auto i_term = solver.make_bv_value(idxSort, std::to_string(i), 10);
        auto cond = solver.make_term(smt::Kind::EQUAL, {idx, i_term});

        smt::Term nextTerm = elements[i].term;
        auto nextSort = solver.get_sort(nextTerm);
        if (solver.is_bv_sort(targetSort) && solver.is_bv_sort(nextSort)) {
          auto targetWidth = solver.get_bv_width(targetSort);
          auto nextWidth = solver.get_bv_width(nextSort);
          if (nextWidth < targetWidth) {
            nextTerm =
                solver.make_term(smt::Kind::BV_SIGN_EXTEND, {nextTerm}, {targetWidth - nextWidth});
          } else if (nextWidth > targetWidth) {
            res = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {res}, {nextWidth - targetWidth});
            targetSort = solver.get_sort(res);
          }
        }

        res = solver.make_term(smt::Kind::ITE, {cond, nextTerm, res});
        defined = solver.make_term(smt::Kind::ITE, {cond, elements[i].is_defined, defined});
      }
      return SymbolicValue(SymbolicValue::Kind::Int, res, defined);
    } else if (elements[0].kind == SymbolicValue::Kind::Array) {
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Array;
      size_t inner_size = elements[0].arrayVal.size();
      for (size_t j = 0; j < inner_size; ++j) {
        std::vector<SymbolicValue> inner_elements;
        for (size_t i = 0; i < elements.size(); ++i)
          inner_elements.push_back(elements[i].arrayVal[j]);
        res.arrayVal.push_back(mergeAggregate(inner_elements, idx, solver));
      }
      return res;
    } else if (elements[0].kind == SymbolicValue::Kind::Struct) {
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Struct;
      for (const auto &[fld, _]: elements[0].structVal) {
        std::vector<SymbolicValue> inner_elements;
        for (size_t i = 0; i < elements.size(); ++i)
          inner_elements.push_back(elements[i].structVal.at(fld));
        res.structVal[fld] = mergeAggregate(inner_elements, idx, solver);
      }
      return res;
    }
    return {SymbolicValue::Kind::Undef};
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::evalLValue(
      const LValue &lv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
      bool forWrite
  ) {
    SymbolicValue res = store.at(lv.base.name);
    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        if (res.kind != SymbolicValue::Kind::Array && res.kind != SymbolicValue::Kind::Vec)
          throw std::runtime_error("Indexing non-array");
        size_t array_size = res.arrayVal.size();
        smt::Term idx;
        if (auto lit = std::get_if<IntLit>(&ai->index)) {
          idx = solver.make_bv_value(solver.make_bv_sort(32), std::to_string(lit->value), 10);
          // [v0.2.1] Out-of-range literal index is UB at compile time —
          // emit the bounds constraint (it'll be false → UNSAT) without
          // crashing in arrayVal.at().
          if (lit->value < 0 || static_cast<uint64_t>(lit->value) >= array_size) {
            pc.push_back(solver.make_false());
            // Use the last element as a dummy value (we won't be used).
            if (!res.arrayVal.empty())
              res = res.arrayVal.back();
            else
              res = SymbolicValue{SymbolicValue::Kind::Undef};
          } else {
            SymbolicValue next = res.arrayVal.at(lit->value);
            res = std::move(next);
          }
        } else {
          auto id = std::get<LocalOrSymId>(ai->index);
          idx = std::visit([&](auto &&v) { return store.at(v.name).term; }, id);
          auto idxSort = solver.get_sort(idx);
          if (solver.get_bv_width(idxSort) != 32) {
            idx = solver.make_term(
                smt::Kind::BV_SIGN_EXTEND, {idx}, {32 - solver.get_bv_width(idxSort)}
            );
          }
          res = mergeAggregate(res.arrayVal, idx, solver);
        }
        // Strict UB: bounds check
        auto size_term =
            solver.make_bv_value(solver.make_bv_sort(32), std::to_string(array_size), 10);
        auto zero = solver.make_bv_zero(solver.make_bv_sort(32));
        pc.push_back(solver.make_term(smt::Kind::BV_SLE, {zero, idx}));
        pc.push_back(solver.make_term(smt::Kind::BV_SLT, {idx, size_term}));
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (res.kind != SymbolicValue::Kind::Struct)
          throw std::runtime_error("Field access on non-struct");
        SymbolicValue next = res.structVal.at(af->field);
        res = std::move(next);
      }
    }
    // [v0.2.1] Strict UB rule 3: reading an `undef` scalar is UB. Add
    // is_defined as a path constraint. Suppressed on the LHS-eval of
    // an AssignInstr (the caller is about to overwrite the value).
    if (!forWrite && res.kind == SymbolicValue::Kind::Int && res.is_defined.internal)
      pc.push_back(res.is_defined);
    return res;
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::muxSymbolicValue(
      smt::Term cond, const SymbolicValue &t, const SymbolicValue &f, smt::ISolver &solver
  ) {
    if (t.kind != f.kind) {
      throw std::runtime_error("Muxing different kinds of SymbolicValues");
    }

    SymbolicValue res;
    res.kind = t.kind;

    if (t.kind == SymbolicValue::Kind::Int) {
      auto tSort = solver.get_sort(t.term);
      auto fSort = solver.get_sort(f.term);
      smt::Term tTerm = t.term;
      smt::Term fTerm = f.term;
      if (solver.is_bv_sort(tSort) && solver.is_bv_sort(fSort)) {
        auto tWidth = solver.get_bv_width(tSort);
        auto fWidth = solver.get_bv_width(fSort);
        if (tWidth < fWidth) {
          tTerm = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {tTerm}, {fWidth - tWidth});
        } else if (fWidth < tWidth) {
          fTerm = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {fTerm}, {tWidth - fWidth});
        }
      }
      res.term = solver.make_term(smt::Kind::ITE, {cond, tTerm, fTerm});
      res.is_defined = solver.make_term(smt::Kind::ITE, {cond, t.is_defined, f.is_defined});
      auto bv64 = solver.make_bv_sort(64);
      auto zero = solver.make_bv_value_int64(bv64, 0);
      auto tBase = t.prov_base.internal ? t.prov_base : zero;
      auto fBase = f.prov_base.internal ? f.prov_base : zero;
      auto tSize = t.prov_size.internal ? t.prov_size : zero;
      auto fSize = f.prov_size.internal ? f.prov_size : zero;
      res.prov_base = solver.make_term(smt::Kind::ITE, {cond, tBase, fBase});
      res.prov_size = solver.make_term(smt::Kind::ITE, {cond, tSize, fSize});
    } else if (t.kind == SymbolicValue::Kind::Array || t.kind == SymbolicValue::Kind::Vec) {
      if (t.arrayVal.size() != f.arrayVal.size())
        throw std::runtime_error("Muxing arrays/vectors of different sizes");
      for (size_t i = 0; i < t.arrayVal.size(); ++i) {
        res.arrayVal.push_back(muxSymbolicValue(cond, t.arrayVal[i], f.arrayVal[i], solver));
      }
    } else if (t.kind == SymbolicValue::Kind::Struct) {
      for (const auto &[key, val]: t.structVal) {
        if (f.structVal.find(key) == f.structVal.end())
          throw std::runtime_error("Muxing structs with mismatching keys: " + key);
        res.structVal[key] = muxSymbolicValue(cond, val, f.structVal.at(key), solver);
      }
    }
    return res;
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::updateLValueRec(
      const SymbolicValue &cur, std::span<const Access> accesses, const SymbolicValue &val,
      smt::Term pathCond, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
      int depth
  ) {
    if (depth > 100)
      throw std::runtime_error("Recursion depth exceeded in updateLValueRec");

    if (accesses.empty()) {
      return muxSymbolicValue(pathCond, val, cur, solver);
    }

    const Access &acc = accesses[0];
    auto nextAccesses = accesses.subspan(1);
    SymbolicValue newCur = cur; // Copy

    if (auto ai = std::get_if<AccessIndex>(&acc)) {
      if (cur.kind != SymbolicValue::Kind::Array && cur.kind != SymbolicValue::Kind::Vec)
        throw std::runtime_error("Indexing non-array in setLValue");

      smt::Term idx;
      if (auto lit = std::get_if<IntLit>(&ai->index)) {
        idx = solver.make_bv_value(solver.make_bv_sort(32), std::to_string(lit->value), 10);
      } else {
        auto id = std::get<LocalOrSymId>(ai->index);
        idx = std::visit([&](auto &&v) { return store.at(v.name).term; }, id);
        if (solver.get_bv_width(solver.get_sort(idx)) != 32) {
          idx = solver.make_term(
              smt::Kind::BV_SIGN_EXTEND, {idx}, {32 - solver.get_bv_width(solver.get_sort(idx))}
          );
        }
      }

      // Bounds check UB
      size_t size = cur.arrayVal.size();
      if (size == 0)
        throw std::runtime_error("Indexing empty array");

      auto size_term = solver.make_bv_value(solver.make_bv_sort(32), std::to_string(size), 10);
      auto zero = solver.make_bv_zero(solver.make_bv_sort(32));

      pc.push_back(solver.make_term(
          smt::Kind::IMPLIES, {pathCond, solver.make_term(smt::Kind::BV_SLE, {zero, idx})}
      ));
      pc.push_back(solver.make_term(
          smt::Kind::IMPLIES, {pathCond, solver.make_term(smt::Kind::BV_SLT, {idx, size_term})}
      ));

      if (auto lit = std::get_if<IntLit>(&ai->index)) {
        // Constant index
        uint64_t k = lit->value;
        if (k < newCur.arrayVal.size()) {
          newCur.arrayVal[k] = updateLValueRec(
              cur.arrayVal[k], nextAccesses, val, pathCond, solver, store, pc, depth + 1
          );
        }
      } else {
        // Symbolic index
        auto idxSort = solver.get_sort(idx);
        for (size_t k = 0; k < newCur.arrayVal.size(); ++k) {
          auto k_term = solver.make_bv_value(idxSort, std::to_string(k), 10);
          auto match = solver.make_term(smt::Kind::EQUAL, {idx, k_term});
          auto cond = solver.make_term(smt::Kind::AND, {pathCond, match});
          newCur.arrayVal[k] = updateLValueRec(
              cur.arrayVal[k], nextAccesses, val, cond, solver, store, pc, depth + 1
          );
        }
      }
    } else {
      auto &af = std::get<AccessField>(acc);
      if (cur.kind != SymbolicValue::Kind::Struct)
        throw std::runtime_error("Field access on non-struct in setLValue");
      if (cur.structVal.find(af.field) == cur.structVal.end())
        throw std::runtime_error("Field not found: " + af.field);

      newCur.structVal[af.field] = updateLValueRec(
          cur.structVal.at(af.field), nextAccesses, val, pathCond, solver, store, pc, depth + 1
      );
    }
    return newCur;
  }

  void SymbolicExecutor::setLValue(
      const LValue &lv, const SymbolicValue &val, smt::ISolver &solver, SymbolicStore &store,
      std::vector<smt::Term> &pc
  ) {
    SymbolicValue &root = store.at(lv.base.name);
    // Use true condition because the instruction itself is unconditional *at this point in the
    // trace* The path constraints handle the reachability of the instruction.
    root = updateLValueRec(root, lv.accesses, val, solver.make_true(), solver, store, pc, 0);
  }

  // Enforce spec §7.4 rules 6–7: add NOT(fp.isInfinite(t)) AND NOT(fp.isNaN(t)) to pc.
  static void assertFPFinite(smt::Term t, smt::ISolver &solver, std::vector<smt::Term> &pc) {
    auto notInf = solver.make_term(smt::Kind::NOT, {solver.make_term(smt::Kind::FP_IS_INF, {t})});
    auto notNaN = solver.make_term(smt::Kind::NOT, {solver.make_term(smt::Kind::FP_IS_NAN, {t})});
    pc.push_back(notInf);
    pc.push_back(notNaN);
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::evalExpr(
      const Expr &e, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
      std::optional<smt::Sort> expectedSort
  ) {
    SymbolicValue res = evalAtom(e.first, solver, store, pc, expectedSort);
    // The typechecker requires the +/- chain to be homogeneous in type. If the
    // caller didn't supply an expected sort, propagate the first atom's sort
    // to subsequent atoms.
    std::optional<smt::Sort> chainSort = expectedSort;
    if (!chainSort && res.term.internal)
      chainSort = solver.get_sort(res.term);

    // Detect pointer arithmetic or pointer subtraction dynamically.
    TypePtr firstTy = resolveAtomType(e.first);
    bool isPtrExpr = firstTy && std::holds_alternative<PtrType>(firstTy->v);
    uint64_t ptrStep = 0;
    if (isPtrExpr) {
      auto pointeeTy = std::get<PtrType>(firstTy->v).pointee;
      ptrStep = sizeofTagUnits(pointeeTy, structs_);
    }

    for (const auto &tail: e.rest) {
      TypePtr rightTy = resolveAtomType(tail.atom);
      bool rhsIsPtr = rightTy && std::holds_alternative<PtrType>(rightTy->v);

      SymbolicValue right = evalAtom(tail.atom, solver, store, pc, chainSort);

      auto lSort = solver.get_sort(res.term);
      auto rSort = solver.get_sort(right.term);

      if (solver.is_fp_sort(lSort)) {
        auto rmRNE = solver.make_rm_value(smt::RoundingMode::RNE);
        if (tail.op == AddOp::Plus) {
          res.term = solver.make_term(smt::Kind::FP_ADD, {rmRNE, res.term, right.term});
        } else {
          res.term = solver.make_term(smt::Kind::FP_SUB, {rmRNE, res.term, right.term});
        }
        assertFPFinite(res.term, solver, pc);
      } else if (solver.is_bv_sort(lSort) && solver.is_bv_sort(rSort)) {
        auto lWidth = solver.get_bv_width(lSort);
        auto rWidth = solver.get_bv_width(rSort);
        if (lWidth < rWidth) {
          res.term = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {res.term}, {rWidth - lWidth});
        } else if (rWidth < lWidth) {
          right.term = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {right.term}, {lWidth - rWidth});
        }

        bool isPtrIntArith = isPtrExpr && !rhsIsPtr;
        bool isPtrSub = isPtrExpr && rhsIsPtr && tail.op == AddOp::Minus;

        if (isPtrIntArith && ptrStep != 1) {
          auto stepTerm = solver.make_bv_value_int64(
              solver.get_sort(right.term), static_cast<int64_t>(ptrStep)
          );
          right.term = solver.make_term(smt::Kind::BV_MUL, {right.term, stepTerm});
        }

        if (tail.op == AddOp::Plus) {
          auto overflow = solver.make_term(smt::Kind::BV_SADD_OVERFLOW, {res.term, right.term});
          pc.push_back(solver.make_term(smt::Kind::NOT, {overflow}));
          res.term = solver.make_term(smt::Kind::BV_ADD, {res.term, right.term});
        } else {
          auto overflow = solver.make_term(smt::Kind::BV_SSUB_OVERFLOW, {res.term, right.term});
          pc.push_back(solver.make_term(smt::Kind::NOT, {overflow}));
          if (isPtrSub) {
            // Pointer subtraction:
            // 1. Rule 12 dynamic assertion (matching bases, non-zero)
            auto bv64 = solver.make_bv_sort(64);
            auto zero = solver.make_bv_value_int64(bv64, 0);
            if (res.prov_base.internal && right.prov_base.internal) {
              auto eqBase = solver.make_term(smt::Kind::EQUAL, {res.prov_base, right.prov_base});
              auto nonZeroBase = solver.make_term(smt::Kind::DISTINCT, {res.prov_base, zero});
              pc.push_back(solver.make_term(smt::Kind::AND, {eqBase, nonZeroBase}));
            } else {
              // One or both have no provenance, which is UB for ptr subtraction
              pc.push_back(solver.make_false());
            }

            // 2. Subtract addresses
            res.term = solver.make_term(smt::Kind::BV_SUB, {res.term, right.term});
            // 3. Divide by element size to get distance
            if (ptrStep > 1) {
              auto stepTerm = solver.make_bv_value_int64(
                  solver.get_sort(res.term), static_cast<int64_t>(ptrStep)
              );
              res.term = solver.make_term(smt::Kind::BV_SDIV, {res.term, stepTerm});
            }
            // 4. Result is an integer, so clear provenance
            res.prov_base = {};
            res.prov_size = {};
          } else {
            res.term = solver.make_term(smt::Kind::BV_SUB, {res.term, right.term});
          }
        }

        // [v0.2.1] Rule 10 (ptr arith OOB): for ptr ± int, result must stay in [base, base + size].
        if (isPtrIntArith) {
          if (res.prov_base.internal && res.prov_size.internal) {
            auto end = solver.make_term(smt::Kind::BV_ADD, {res.prov_base, res.prov_size});
            pc.push_back(solver.make_term(smt::Kind::BV_ULE, {res.prov_base, res.term}));
            pc.push_back(solver.make_term(smt::Kind::BV_ULE, {res.term, end}));
          }
        }
      }
    }
    return res;
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::evalAtom(
      const Atom &a, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
      std::optional<smt::Sort> expectedSort
  ) {
    return std::visit(
        [&](auto &&arg) -> SymbolicValue {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            smt::Term c = evalCoef(arg.coef, solver, store, expectedSort);
            smt::Term r = evalLValue(arg.rval, solver, store, pc).term;

            auto cSort = solver.get_sort(c);
            auto rSort = solver.get_sort(r);

            if (solver.is_fp_sort(cSort)) {
              // SPEC §2.9: all FP ops use RNE. The fmod encoding additionally
              // uses RTZ for the quotient-to-integer step.
              auto rmRNE = solver.make_rm_value(smt::RoundingMode::RNE);
              smt::Term fpRes;
              if (arg.op == AtomOpKind::Mul)
                fpRes = solver.make_term(smt::Kind::FP_MUL, {rmRNE, c, r});
              else if (arg.op == AtomOpKind::Div)
                fpRes = solver.make_term(smt::Kind::FP_DIV, {rmRNE, c, r});
              else if (arg.op == AtomOpKind::Mod) {
                // fmod(x,y) = x - trunc(x/y)*y  (truncated-quotient, matches integer %)
                // Encode as: fp.sub(x, fp.mul(fp.roundToIntegral[RTZ](fp.div(x,y)), y))
                auto rmRTZ = solver.make_rm_value(smt::RoundingMode::RTZ);
                auto q = solver.make_term(smt::Kind::FP_DIV, {rmRNE, c, r});
                auto qi = solver.make_term(smt::Kind::FP_RTI, {rmRTZ, q});
                auto prod = solver.make_term(smt::Kind::FP_MUL, {rmRNE, qi, r});
                fpRes = solver.make_term(smt::Kind::FP_SUB, {rmRNE, c, prod});
              } else
                return {};
              assertFPFinite(fpRes, solver, pc);
              return SymbolicValue(SymbolicValue::Kind::Int, fpRes, solver.make_true());
            }

            if (solver.is_bv_sort(cSort) && solver.is_bv_sort(rSort)) {
              auto cWidth = solver.get_bv_width(cSort);
              auto rWidth = solver.get_bv_width(rSort);
              if (cWidth < rWidth) {
                c = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {c}, {rWidth - cWidth});
                cSort = solver.get_sort(c);
              } else if (rWidth < cWidth) {
                r = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {r}, {cWidth - rWidth});
                rSort = solver.get_sort(r);
              }
            }

            if (arg.op == AtomOpKind::Div || arg.op == AtomOpKind::Mod) {
              auto zero = solver.make_bv_zero(solver.get_sort(r));
              pc.push_back(solver.make_term(smt::Kind::DISTINCT, {r, zero}));
            }
            if (arg.op == AtomOpKind::Mul) {
              auto overflow = solver.make_term(smt::Kind::BV_SMUL_OVERFLOW, {c, r});
              pc.push_back(solver.make_term(smt::Kind::NOT, {overflow}));
              return SymbolicValue(
                  SymbolicValue::Kind::Int, solver.make_term(smt::Kind::BV_MUL, {c, r}),
                  solver.make_true()
              );
            }
            if (arg.op == AtomOpKind::Div) {
              // Check overflow: c == INT_MIN && r == -1
              auto min_signed = solver.make_bv_min_signed(solver.get_sort(c));
              auto minus_one = solver.make_bv_value_int64(solver.get_sort(r), -1);
              auto is_min = solver.make_term(smt::Kind::EQUAL, {c, min_signed});
              auto is_minus_one = solver.make_term(smt::Kind::EQUAL, {r, minus_one});
              auto div_overflow = solver.make_term(smt::Kind::AND, {is_min, is_minus_one});
              pc.push_back(solver.make_term(smt::Kind::NOT, {div_overflow}));
              return SymbolicValue(
                  SymbolicValue::Kind::Int, solver.make_term(smt::Kind::BV_SDIV, {c, r}),
                  solver.make_true()
              );
            }
            if (arg.op == AtomOpKind::Mod) {
              // Check overflow for mod
              auto min_signed = solver.make_bv_min_signed(solver.get_sort(c));
              auto minus_one = solver.make_bv_value_int64(solver.get_sort(r), -1);
              auto is_min = solver.make_term(smt::Kind::EQUAL, {c, min_signed});
              auto is_minus_one = solver.make_term(smt::Kind::EQUAL, {r, minus_one});
              auto mod_overflow = solver.make_term(smt::Kind::AND, {is_min, is_minus_one});
              pc.push_back(solver.make_term(smt::Kind::NOT, {mod_overflow}));
              return SymbolicValue(
                  SymbolicValue::Kind::Int, solver.make_term(smt::Kind::BV_SREM, {c, r}),
                  solver.make_true()
              );
            }
            if (arg.op == AtomOpKind::And) {
              return SymbolicValue(
                  SymbolicValue::Kind::Int, solver.make_term(smt::Kind::BV_AND, {c, r}),
                  solver.make_true()
              );
            }
            if (arg.op == AtomOpKind::Or) {
              return SymbolicValue(
                  SymbolicValue::Kind::Int, solver.make_term(smt::Kind::BV_OR, {c, r}),
                  solver.make_true()
              );
            }
            if (arg.op == AtomOpKind::Xor) {
              return SymbolicValue(
                  SymbolicValue::Kind::Int, solver.make_term(smt::Kind::BV_XOR, {c, r}),
                  solver.make_true()
              );
            }
            if (arg.op == AtomOpKind::Shl || arg.op == AtomOpKind::Shr ||
                arg.op == AtomOpKind::LShr) {
              // Overshift UB: amount in [0, width). Negative amounts and
              // amounts >= width are both UB (rule 5). We assert `r` is in
              // range by reading it as signed (>= 0) AND unsigned (< width)
              // — the conjunction covers both cases regardless of sort.
              uint32_t width = solver.get_bv_width(solver.get_sort(c));
              auto width_term = solver.make_bv_value(solver.get_sort(r), std::to_string(width), 10);
              auto zero = solver.make_bv_zero(solver.get_sort(r));
              pc.push_back(solver.make_term(smt::Kind::BV_SLE, {zero, r}));
              pc.push_back(solver.make_term(smt::Kind::BV_ULT, {r, width_term}));

              if (arg.op == AtomOpKind::Shl) {
                // SPEC §7.1 rule 4: SHL on a negative operand is UB, and
                // result overflow is UB. The result-overflow path was
                // already checked via `(ashr (shl c, r), r) == c`; we now
                // also require `c >= 0` to catch `-1 << 1` which the
                // overflow check otherwise accepts (ashr-shl round-trips
                // on `-1` since the sign bit dominates).
                pc.push_back(solver.make_term(smt::Kind::BV_SLE, {zero, c}));
                auto shifted = solver.make_term(smt::Kind::BV_SHL, {c, r});
                auto unshifted = solver.make_term(smt::Kind::BV_ASHR, {shifted, r});
                pc.push_back(solver.make_term(smt::Kind::EQUAL, {unshifted, c}));
                return SymbolicValue(SymbolicValue::Kind::Int, shifted, solver.make_true());
              }
              if (arg.op == AtomOpKind::Shr)
                return SymbolicValue(
                    SymbolicValue::Kind::Int, solver.make_term(smt::Kind::BV_ASHR, {c, r}),
                    solver.make_true()
                );
              if (arg.op == AtomOpKind::LShr)
                return SymbolicValue(
                    SymbolicValue::Kind::Int, solver.make_term(smt::Kind::BV_SHR, {c, r}),
                    solver.make_true()
                );
            }
            return {};
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            auto r = evalLValue(arg.rval, solver, store, pc).term;
            if (arg.op == UnaryOpKind::Not) {
              return SymbolicValue(
                  SymbolicValue::Kind::Int, solver.make_term(smt::Kind::BV_NOT, {r}),
                  solver.make_true()
              );
            }
            return {};
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            // Two forms: cond form (relational predicate) or mask form
            // (scalar i1 expression here; vector masks go through the
            // vector path).
            smt::Term cond;
            if (arg.cond) {
              cond = evalCond(*arg.cond, solver, store, pc);
            } else if (arg.maskExpr) {
              SymbolicValue m = evalExpr(*arg.maskExpr, solver, store, pc);
              auto mSort = solver.get_sort(m.term);
              if (solver.is_fp_sort(mSort))
                throw std::runtime_error("scalar select: mask must be integral");
              auto zero = solver.make_bv_zero(mSort);
              cond = solver.make_term(smt::Kind::DISTINCT, {m.term, zero});
            } else {
              throw std::runtime_error("SelectAtom: neither cond nor maskExpr set");
            }
            // [v0.2.1] §6.4 select is lazy: only the chosen arm's UB
            // constraints participate in the path condition. Evaluate
            // each arm into a private constraint list, then gate them
            // with the appropriate side of `cond` before pushing to pc.
            std::vector<smt::Term> tPc, fPc;
            SymbolicValue vt = evalSelectVal(arg.vtrue, solver, store, tPc, expectedSort);
            SymbolicValue vf = evalSelectVal(arg.vfalse, solver, store, fPc, expectedSort);
            auto notCond = solver.make_term(smt::Kind::NOT, {cond});
            for (auto &t: tPc)
              pc.push_back(solver.make_term(smt::Kind::IMPLIES, {cond, t}));
            for (auto &t: fPc)
              pc.push_back(solver.make_term(smt::Kind::IMPLIES, {notCond, t}));

            return muxSymbolicValue(cond, vt, vf, solver);
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            smt::Term term = evalCoef(arg.coef, solver, store, expectedSort);
            auto bv64 = solver.make_bv_sort(64);
            auto zero = solver.make_bv_value_int64(bv64, 0);
            return SymbolicValue(SymbolicValue::Kind::Int, term, solver.make_true(), zero, zero);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            return evalLValue(arg.rval, solver, store, pc);
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            smt::Term src = std::visit(
                [&](auto &&s) -> smt::Term {
                  using S = std::decay_t<decltype(s)>;
                  if constexpr (std::is_same_v<S, IntLit>) {
                    return solver.make_bv_value(
                        solver.make_bv_sort(32), std::to_string(s.value), 10
                    );
                  } else if constexpr (std::is_same_v<S, FloatLit>) {
                    // Default to f32 if implied
                    return solver.make_fp_value(
                        solver.make_fp_sort(8, 24), std::to_string(s.value), smt::RoundingMode::RNE
                    );
                  } else if constexpr (std::is_same_v<S, SymId>) {
                    return store.at(s.name).term;
                  } else {
                    return evalLValue(s, solver, store, pc).term;
                  }
                },
                arg.src
            );
            auto dstSort = getSort(arg.dstType, solver);

            bool srcIsFp = solver.is_fp_sort(solver.get_sort(src));
            bool dstIsFp = solver.is_fp_sort(dstSort);

            smt::Term casted;
            if (srcIsFp && !dstIsFp) { // FP -> BV (spec §7.4 rule 8: range check)
              uint32_t width = solver.get_bv_width(dstSort);
              auto srcSort = solver.get_sort(src);
              assertFPFinite(src, solver, pc);
              double lo = -std::ldexp(1.0, static_cast<int>(width) - 1);
              double hi = std::ldexp(1.0, static_cast<int>(width) - 1);
              auto loFp = solver.make_fp_value_from_real(srcSort, lo, smt::RoundingMode::RNE);
              auto hiFp = solver.make_fp_value_from_real(srcSort, hi, smt::RoundingMode::RNE);
              pc.push_back(solver.make_term(smt::Kind::FP_GEQ, {src, loFp}));
              pc.push_back(solver.make_term(smt::Kind::FP_LT, {src, hiFp}));
              auto rmRTZ = solver.make_rm_value(smt::RoundingMode::RTZ);
              casted = solver.make_term(smt::Kind::FP_TO_SBV, {rmRTZ, src}, {width});
            } else if (!srcIsFp && dstIsFp) { // BV -> FP
              auto [exp, sig] = solver.get_fp_dims(dstSort);
              auto rmRNE = solver.make_rm_value(smt::RoundingMode::RNE);
              casted = solver.make_term(smt::Kind::FP_TO_FP_FROM_SBV, {rmRNE, src}, {exp, sig});
              assertFPFinite(casted, solver, pc);
            } else if (srcIsFp && dstIsFp) { // FP -> FP
              auto [exp, sig] = solver.get_fp_dims(dstSort);
              auto rmRNE = solver.make_rm_value(smt::RoundingMode::RNE);
              casted = solver.make_term(smt::Kind::FP_TO_FP_FROM_FP, {rmRNE, src}, {exp, sig});
              assertFPFinite(casted, solver, pc);
            } else {
              // BV -> BV resizing
              uint32_t srcWidth = solver.get_bv_width(solver.get_sort(src));
              uint32_t dstWidth = solver.get_bv_width(dstSort);
              if (srcWidth == dstWidth)
                casted = src;
              else if (srcWidth < dstWidth) {
                casted = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {src}, {dstWidth - srcWidth});
              } else {
                casted = solver.make_term(smt::Kind::BV_EXTRACT, {src}, {dstWidth - 1, 0});
              }
            }
            return SymbolicValue(SymbolicValue::Kind::Int, casted, solver.make_true());
          } else if constexpr (std::is_same_v<T, CmpAtom>) {
            auto getOperandSort = [&]() -> std::optional<smt::Sort> {
              if (auto rv = std::get_if<RValue>(&arg.lhs))
                return solver.get_sort(evalLValue(*rv, solver, store, pc).term);
              if (auto rv = std::get_if<RValue>(&arg.rhs))
                return solver.get_sort(evalLValue(*rv, solver, store, pc).term);
              return std::nullopt;
            };
            auto opSort = getOperandSort();
            auto loadOperand = [&](const SelectVal &sv) -> SymbolicValue {
              return evalSelectVal(sv, solver, store, pc, opSort);
            };

            SymbolicValue lVal = loadOperand(arg.lhs);
            SymbolicValue rVal = loadOperand(arg.rhs);

            TypePtr lhsTy = resolveSelectValType(arg.lhs);
            TypePtr rhsTy = resolveSelectValType(arg.rhs);
            bool isLhsPtr = lhsTy && std::holds_alternative<PtrType>(lhsTy->v);
            bool isRhsPtr = rhsTy && std::holds_alternative<PtrType>(rhsTy->v);
            if (isLhsPtr || isRhsPtr) {
              if (arg.op == RelOp::LT || arg.op == RelOp::LE || arg.op == RelOp::GT ||
                  arg.op == RelOp::GE) {
                auto bv64 = solver.make_bv_sort(64);
                auto zero = solver.make_bv_value_int64(bv64, 0);
                if (lVal.prov_base.internal && rVal.prov_base.internal) {
                  auto eqBase =
                      solver.make_term(smt::Kind::EQUAL, {lVal.prov_base, rVal.prov_base});
                  auto nonZeroBase = solver.make_term(smt::Kind::DISTINCT, {lVal.prov_base, zero});
                  pc.push_back(solver.make_term(smt::Kind::AND, {eqBase, nonZeroBase}));
                } else {
                  pc.push_back(solver.make_false());
                }
              }
            }

            smt::Term l = lVal.term;
            smt::Term r = rVal.term;

            auto lSort = solver.get_sort(l);
            auto rSort = solver.get_sort(r);
            if (solver.is_fp_sort(lSort) != solver.is_fp_sort(rSort))
              throw std::runtime_error("scalar cmp: mixed FP/BV operands");
            if (!solver.is_fp_sort(lSort)) {
              uint32_t lw = solver.get_bv_width(lSort);
              uint32_t rw = solver.get_bv_width(rSort);
              if (lw != rw) {
                uint32_t target = std::max(lw, rw);
                if (lw < target)
                  l = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {l}, {target - lw});
                if (rw < target)
                  r = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {r}, {target - rw});
              }
            }
            bool isFp = solver.is_fp_sort(solver.get_sort(l));
            smt::Kind k = relOpToSmtKind(arg.op, isFp);
            smt::Term cond = solver.make_term(k, {l, r});
            smt::Sort i1 = solver.make_bv_sort(1);
            smt::Term one = solver.make_bv_value(i1, "1", 10);
            smt::Term zero = solver.make_bv_value(i1, "0", 10);
            return SymbolicValue(
                SymbolicValue::Kind::Int, solver.make_term(smt::Kind::ITE, {cond, one, zero}),
                solver.make_true()
            );
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            const std::string targetName = arg.lv.base.name;
            auto bv64 = solver.make_bv_sort(kPtrBits);
            smt::Term tag =
                solver.make_bv_value_int64(bv64, static_cast<int64_t>(tagOfLocal(targetName)));
            TypePtr cur;
            if (currentFun_) {
              for (const auto &l: currentFun_->lets)
                if (l.name.name == targetName) {
                  cur = l.type;
                  break;
                }
              if (!cur)
                for (const auto &p: currentFun_->params)
                  if (p.name.name == targetName) {
                    cur = p.type;
                    break;
                  }
            }
            smt::Term prov_base = tag;
            std::uint64_t initial_size = sizeofTagUnits(cur, structs_);
            smt::Term prov_size =
                solver.make_bv_value_int64(bv64, static_cast<int64_t>(initial_size));

            for (const auto &acc: arg.lv.accesses) {
              if (auto ai = std::get_if<AccessIndex>(&acc)) {
                auto at = std::get_if<ArrayType>(&cur->v);
                if (!at)
                  throw std::runtime_error("addr: index on non-array");
                std::uint64_t stride = sizeofTagUnits(at->elem, structs_);
                prov_base = tag;
                prov_size =
                    solver.make_bv_value_int64(bv64, static_cast<int64_t>(at->size * stride));

                if (auto il = std::get_if<IntLit>(&ai->index)) {
                  std::uint64_t off = static_cast<std::uint64_t>(il->value) * stride;
                  if (off != 0) {
                    auto ofT = solver.make_bv_value_int64(bv64, static_cast<int64_t>(off));
                    tag = solver.make_term(smt::Kind::BV_ADD, {tag, ofT});
                  }
                  cur = at->elem;
                  continue;
                }
                if (auto id = std::get_if<LocalOrSymId>(&ai->index)) {
                  smt::Term idxT = std::visit([&](auto &&v) { return store.at(v.name).term; }, *id);
                  auto idxSort = solver.get_sort(idxT);
                  uint32_t iw = solver.get_bv_width(idxSort);
                  if (iw < kPtrBits)
                    idxT = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {idxT}, {kPtrBits - iw});
                  if (stride != 1) {
                    auto stT = solver.make_bv_value_int64(bv64, static_cast<int64_t>(stride));
                    idxT = solver.make_term(smt::Kind::BV_MUL, {idxT, stT});
                  }
                  tag = solver.make_term(smt::Kind::BV_ADD, {tag, idxT});
                  cur = at->elem;
                  continue;
                }
              }
              if (auto af = std::get_if<AccessField>(&acc)) {
                auto st = std::get_if<StructType>(&cur->v);
                if (!st)
                  throw std::runtime_error("'addr' field access on non-struct base: " + targetName);
                auto sIt = structs_.find(st->name.name);
                if (sIt == structs_.end())
                  throw std::runtime_error("unknown struct in addr field access");
                prov_base = tag;
                std::uint64_t stSize = sizeofTagUnits(cur, structs_);
                prov_size = solver.make_bv_value_int64(bv64, static_cast<int64_t>(stSize));

                std::uint64_t off = fieldOffsetTagUnits(*sIt->second, af->field, structs_);
                TypePtr fieldTy;
                for (const auto &f: sIt->second->fields)
                  if (f.name == af->field) {
                    fieldTy = f.type;
                    break;
                  }
                if (!fieldTy)
                  throw std::runtime_error("addr: unknown field " + af->field);
                if (off != 0) {
                  auto ofT = solver.make_bv_value_int64(bv64, static_cast<int64_t>(off));
                  tag = solver.make_term(smt::Kind::BV_ADD, {tag, ofT});
                }
                cur = fieldTy;
                continue;
              }
              throw std::runtime_error("'addr' with this access kind not yet supported in solver");
            }
            return SymbolicValue(
                SymbolicValue::Kind::Int, tag, solver.make_true(), prov_base, prov_size
            );
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            auto bv64 = solver.make_bv_sort(kPtrBits);
            SymbolicValue ptrVal = evalLValue(arg.rval, solver, store, pc);
            smt::Term ptrTerm = ptrVal.term;

            auto nullTerm = solver.make_bv_value_int64(bv64, 0);
            pc.push_back(solver.make_term(smt::Kind::DISTINCT, {ptrTerm, nullTerm}));

            if (ptrVal.prov_base.internal && ptrVal.prov_size.internal) {
              auto endAddr =
                  solver.make_term(smt::Kind::BV_ADD, {ptrVal.prov_base, ptrVal.prov_size});
              pc.push_back(solver.make_term(smt::Kind::DISTINCT, {ptrTerm, endAddr}));
            }

            smt::Term idxT;
            if (auto il = std::get_if<IntLit>(&arg.index)) {
              idxT = solver.make_bv_value_int64(bv64, il->value);
            } else if (auto id = std::get_if<LocalOrSymId>(&arg.index)) {
              smt::Term raw = std::visit([&](auto &&v) { return store.at(v.name).term; }, *id);
              auto rawSort = solver.get_sort(raw);
              uint32_t rw = solver.get_bv_width(rawSort);
              if (rw < kPtrBits)
                raw = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {raw}, {kPtrBits - rw});
              idxT = raw;
            }

            std::uint64_t elemUnits = 1;
            std::uint64_t arrSize = 0;
            if (currentFun_) {
              TypePtr baseType = resolveLValueType(arg.rval);
              if (baseType) {
                if (auto pt = std::get_if<PtrType>(&baseType->v)) {
                  if (auto at = std::get_if<ArrayType>(&pt->pointee->v)) {
                    elemUnits = sizeofTagUnits(at->elem, structs_);
                    arrSize = at->size;
                  }
                }
              }
            }

            if (arrSize > 0) {
              auto zero = solver.make_bv_value_int64(bv64, 0);
              auto N = solver.make_bv_value_int64(bv64, static_cast<int64_t>(arrSize));
              pc.push_back(solver.make_term(smt::Kind::BV_SLE, {zero, idxT}));
              pc.push_back(solver.make_term(smt::Kind::BV_SLE, {idxT, N}));
            }

            if (elemUnits != 1) {
              auto stride = solver.make_bv_value_int64(bv64, static_cast<int64_t>(elemUnits));
              idxT = solver.make_term(smt::Kind::BV_MUL, {idxT, stride});
            }
            smt::Term newAddr = solver.make_term(smt::Kind::BV_ADD, {ptrTerm, idxT});

            smt::Term prov_base = ptrTerm;
            smt::Term prov_size =
                solver.make_bv_value_int64(bv64, static_cast<int64_t>(arrSize * elemUnits));
            return SymbolicValue(
                SymbolicValue::Kind::Int, newAddr, solver.make_true(), prov_base, prov_size
            );
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            if (!currentFun_)
              throw std::runtime_error("ptrfield encountered without active FunDecl");
            auto bv64 = solver.make_bv_sort(kPtrBits);
            SymbolicValue ptrVal = evalLValue(arg.rval, solver, store, pc);
            smt::Term ptrTerm = ptrVal.term;

            auto nullTerm = solver.make_bv_value_int64(bv64, 0);
            pc.push_back(solver.make_term(smt::Kind::DISTINCT, {ptrTerm, nullTerm}));

            if (ptrVal.prov_base.internal && ptrVal.prov_size.internal) {
              auto endAddr =
                  solver.make_term(smt::Kind::BV_ADD, {ptrVal.prov_base, ptrVal.prov_size});
              pc.push_back(solver.make_term(smt::Kind::DISTINCT, {ptrTerm, endAddr}));
            }

            TypePtr cur = resolveLValueType(arg.rval);
            if (!cur || !std::holds_alternative<PtrType>(cur->v))
              throw std::runtime_error("ptrfield: rval is not pointer-typed");
            auto &pt = std::get<PtrType>(cur->v);
            if (!std::holds_alternative<StructType>(pt.pointee->v))
              throw std::runtime_error("ptrfield: pointee is not a struct");
            auto &st = std::get<StructType>(pt.pointee->v);
            auto sIt = structs_.find(st.name.name);
            if (sIt == structs_.end())
              throw std::runtime_error("ptrfield: unknown struct " + st.name.name);
            bool found = false;
            for (const auto &f: sIt->second->fields)
              if (f.name == arg.field) {
                found = true;
                break;
              }
            if (!found)
              throw std::runtime_error("ptrfield: unknown field " + arg.field);
            std::uint64_t off = fieldOffsetTagUnits(*sIt->second, arg.field, structs_);
            smt::Term newAddr = ptrTerm;
            if (off != 0) {
              auto offT = solver.make_bv_value_int64(bv64, static_cast<int64_t>(off));
              newAddr = solver.make_term(smt::Kind::BV_ADD, {ptrTerm, offT});
            }

            TypePtr fieldTy;
            for (const auto &f: sIt->second->fields)
              if (f.name == arg.field) {
                fieldTy = f.type;
                break;
              }
            smt::Term prov_base = ptrTerm;
            std::uint64_t structSize = sizeofTagUnits(pt.pointee, structs_);
            smt::Term prov_size =
                solver.make_bv_value_int64(bv64, static_cast<int64_t>(structSize));
            return SymbolicValue(
                SymbolicValue::Kind::Int, newAddr, solver.make_true(), prov_base, prov_size
            );
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            if (!currentFun_)
              throw std::runtime_error("load encountered without active FunDecl");

            SymbolicValue ptrVal = evalLValue(arg.rval, solver, store, pc);
            smt::Term ptrTerm = ptrVal.term;

            const TypePtr baseType = resolveLValueType(arg.rval);
            if (!baseType || !std::holds_alternative<PtrType>(baseType->v))
              throw std::runtime_error("load target is not ptr-typed: " + arg.rval.base.name);
            const TypePtr pointeeType = std::get<PtrType>(baseType->v).pointee;

            auto pointeeSort = getSort(pointeeType, solver);
            smt::Term result;
            if (solver.is_fp_sort(pointeeSort)) {
              auto [exp, sig] = solver.get_fp_dims(pointeeSort);
              auto zeroBv = solver.make_bv_value(solver.make_bv_sort(exp + sig), "0", 10);
              result = solver.make_term(smt::Kind::FP_TO_FP_FROM_SBV, {zeroBv}, {exp, sig});
            } else {
              result = solver.make_bv_value(pointeeSort, "0", 10);
            }
            auto bv64 = solver.make_bv_sort(kPtrBits);
            auto zero = solver.make_bv_value_int64(bv64, 0);

            if (ptrVal.prov_base.internal && ptrVal.prov_size.internal) {
              auto hasProv = solver.make_term(smt::Kind::DISTINCT, {ptrVal.prov_base, zero});
              auto inBoundsLower = solver.make_term(smt::Kind::BV_ULE, {ptrVal.prov_base, ptrTerm});
              auto endAddr =
                  solver.make_term(smt::Kind::BV_ADD, {ptrVal.prov_base, ptrVal.prov_size});
              auto inBoundsUpper = solver.make_term(smt::Kind::BV_ULT, {ptrTerm, endAddr});
              auto cond = solver.make_term(
                  smt::Kind::IMPLIES,
                  {hasProv, solver.make_term(smt::Kind::AND, {inBoundsLower, inBoundsUpper})}
              );
              pc.push_back(cond);
            }

            smt::Term res_prov_base = zero;
            smt::Term res_prov_size = zero;

            std::vector<smt::Term> matchConds;
            std::function<void(const TypePtr &, SymbolicValue &, std::uint64_t, std::uint64_t)>
                enumLoad;
            enumLoad = [&](const TypePtr &ty, SymbolicValue &sv, std::uint64_t baseTag,
                           std::uint64_t off) {
              if (!ty)
                return;
              if (typeMatch(ty, pointeeType)) {
                auto tagTerm =
                    solver.make_bv_value_int64(bv64, static_cast<int64_t>(baseTag + off));
                auto cond = solver.make_term(smt::Kind::EQUAL, {ptrTerm, tagTerm});
                matchConds.push_back(cond);
                result = solver.make_term(smt::Kind::ITE, {cond, sv.term, result});

                auto svBase = sv.prov_base.internal ? sv.prov_base : zero;
                auto svSize = sv.prov_size.internal ? sv.prov_size : zero;
                res_prov_base = solver.make_term(smt::Kind::ITE, {cond, svBase, res_prov_base});
                res_prov_size = solver.make_term(smt::Kind::ITE, {cond, svSize, res_prov_size});
                return;
              }
              if (auto at = std::get_if<ArrayType>(&ty->v)) {
                std::uint64_t stride = sizeofTagUnits(at->elem, structs_);
                for (std::uint64_t k = 0; k < at->size && k < sv.arrayVal.size(); ++k)
                  enumLoad(at->elem, sv.arrayVal[k], baseTag, off + k * stride);
                return;
              }
              if (auto st = std::get_if<StructType>(&ty->v)) {
                auto sIt = structs_.find(st->name.name);
                if (sIt == structs_.end())
                  return;
                std::uint64_t fOff = 0;
                for (const auto &f: sIt->second->fields) {
                  auto fIt = sv.structVal.find(f.name);
                  if (fIt != sv.structVal.end())
                    enumLoad(f.type, fIt->second, baseTag, off + fOff);
                  fOff += sizeofTagUnits(f.type, structs_);
                }
                return;
              }
            };
            for (const auto &l: currentFun_->lets) {
              std::uint64_t baseTag = tagOfLocal(l.name.name);
              enumLoad(l.type, store.at(l.name.name), baseTag, 0);
            }
            auto nullPtr = solver.make_bv_value_int64(bv64, 0);
            pc.push_back(solver.make_term(smt::Kind::DISTINCT, {ptrTerm, nullPtr}));
            if (!matchConds.empty()) {
              smt::Term anyMatch = matchConds[0];
              for (size_t i = 1; i < matchConds.size(); ++i)
                anyMatch = solver.make_term(smt::Kind::OR, {anyMatch, matchConds[i]});
              pc.push_back(anyMatch);
            }
            return SymbolicValue(
                SymbolicValue::Kind::Int, result, solver.make_true(), res_prov_base, res_prov_size
            );
          }
          return SymbolicValue(SymbolicValue::Kind::Undef);
        },
        a.v
    );
  }

  // [v0.2.1] evalVecExpr — evaluate an Expr whose value is a vector,
  // returning a SymbolicValue with Kind::Vec. Single-atom Expr only
  // (chains involving vectors aren't exercised by our v0.2.1 tests; the
  // typechecker already permits them, so this is the natural follow-up
  // when those patterns appear in user code).
  SymbolicExecutor::SymbolicValue SymbolicExecutor::evalVecExpr(
      const Expr &e, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
      std::vector<smt::Term> &pc
  ) {
    auto buildVec = [&](std::vector<SymbolicValue> lanes) {
      SymbolicValue r;
      r.kind = SymbolicValue::Kind::Vec;
      r.arrayVal = std::move(lanes);
      return r;
    };

    auto laneSort = getSort(vt.elem, solver);

    // Fast path: chained +/- on vector lanes. We resolve each atom to its
    // N per-lane terms by lowering as a single-atom Expr — the dispatcher
    // below handles the supported atom kinds.
    if (!e.rest.empty()) {
      bool fpLane = solver.is_fp_sort(laneSort);
      // Atom isn't copy-assignable (SelectAtom holds unique_ptr), so we
      // call into the single-atom dispatcher in place via a tiny helper
      // that reuses the existing branch by visiting the Atom variant.
      auto evalAtomToLanes = [&](const Atom &a, auto &dispatch) -> std::vector<smt::Term> {
        SymbolicValue v = dispatch(a);
        std::vector<smt::Term> out(vt.size);
        if (v.kind == SymbolicValue::Kind::Vec) {
          for (std::uint64_t k = 0; k < vt.size; ++k)
            out[k] = v.arrayVal[k].term;
        } else {
          for (std::uint64_t k = 0; k < vt.size; ++k)
            out[k] = v.term;
        }
        return out;
      };
      // Recurse into evalVecExprAtom (declared below) for each atom in the
      // chain. We dispatch by constructing a one-atom view through the
      // overloaded helper.
      auto dispatchAtom = [&](const Atom &a) -> SymbolicValue {
        return evalVecExprAtom(a, vt, solver, store, pc);
      };
      auto rmRNE = fpLane ? solver.make_rm_value(smt::RoundingMode::RNE) : smt::Term();
      std::vector<smt::Term> acc = evalAtomToLanes(e.first, dispatchAtom);
      for (const auto &term: e.rest) {
        auto next = evalAtomToLanes(term.atom, dispatchAtom);
        for (std::uint64_t k = 0; k < vt.size; ++k) {
          smt::Kind opK;
          if (fpLane)
            opK = term.op == AddOp::Plus ? smt::Kind::FP_ADD : smt::Kind::FP_SUB;
          else {
            // [v0.2.1] Per-lane signed BV add/sub overflow is UB
            // (rule 1 / 2 lifted to lanes).
            auto ovK =
                term.op == AddOp::Plus ? smt::Kind::BV_SADD_OVERFLOW : smt::Kind::BV_SSUB_OVERFLOW;
            auto ov = solver.make_term(ovK, {acc[k], next[k]});
            pc.push_back(solver.make_term(smt::Kind::NOT, {ov}));
            opK = term.op == AddOp::Plus ? smt::Kind::BV_ADD : smt::Kind::BV_SUB;
          }
          acc[k] = fpLane ? solver.make_term(opK, {rmRNE, acc[k], next[k]})
                          : solver.make_term(opK, {acc[k], next[k]});
          // [v0.2.1] Per-lane FP result must be finite (rule 6/7 lifted).
          if (fpLane)
            assertFPFinite(acc[k], solver, pc);
        }
      }
      std::vector<SymbolicValue> lanes;
      lanes.reserve(vt.size);
      for (std::uint64_t k = 0; k < vt.size; ++k)
        lanes.emplace_back(SymbolicValue::Kind::Int, acc[k], solver.make_true());
      return buildVec(std::move(lanes));
    }

    return evalVecExprAtom(e.first, vt, solver, store, pc);
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::evalVecExprAtom(
      const Atom &a, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
      std::vector<smt::Term> &pc
  ) {
    auto buildVec = [&](std::vector<SymbolicValue> lanes) {
      SymbolicValue r;
      r.kind = SymbolicValue::Kind::Vec;
      r.arrayVal = std::move(lanes);
      return r;
    };
    return std::visit(
        [&](auto &&arg) -> SymbolicValue {
          using T = std::decay_t<decltype(arg)>;
          auto sortFor = [&]() { return getSort(vt.elem, solver); };
          if constexpr (std::is_same_v<T, CoefAtom>) {
            // Vector sym or local — look up the store entry.
            if (auto id = std::get_if<LocalOrSymId>(&arg.coef)) {
              std::string nm = std::visit([](auto &&x) { return x.name; }, *id);
              auto it = store.find(nm);
              if (it != store.end() && it->second.kind == SymbolicValue::Kind::Vec)
                return it->second;
            }
            // Literal broadcast (rare; vector init handled elsewhere).
            smt::Term bv = evalCoef(arg.coef, solver, store, sortFor());
            std::vector<SymbolicValue> lanes;
            lanes.reserve(vt.size);
            for (std::uint64_t k = 0; k < vt.size; ++k)
              lanes.emplace_back(SymbolicValue::Kind::Int, bv, solver.make_true());
            return buildVec(std::move(lanes));
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            return evalLValue(arg.rval, solver, store, pc);
          } else if constexpr (std::is_same_v<T, CmpAtom>) {
            // Per-lane comparison → <N> i1.
            // Infer operand lane sort from whichever side is a vec RValue.
            auto getOperandLaneSort = [&]() -> std::optional<smt::Sort> {
              auto getValSort = [&](const SelectVal &sv) -> std::optional<smt::Sort> {
                if (auto rv = std::get_if<RValue>(&sv)) {
                  auto val = evalLValue(*rv, solver, store, pc);
                  if (val.kind == SymbolicValue::Kind::Vec && !val.arrayVal.empty())
                    return solver.get_sort(val.arrayVal[0].term);
                }
                if (auto cf = std::get_if<Coef>(&sv)) {
                  if (auto id = std::get_if<LocalOrSymId>(cf)) {
                    std::string nm = std::visit([](auto &&x) { return x.name; }, *id);
                    auto it = store.find(nm);
                    if (it != store.end() && it->second.kind == SymbolicValue::Kind::Vec &&
                        !it->second.arrayVal.empty())
                      return solver.get_sort(it->second.arrayVal[0].term);
                  }
                }
                return std::nullopt;
              };
              if (auto s = getValSort(arg.lhs))
                return s;
              if (auto s = getValSort(arg.rhs))
                return s;
              return std::nullopt;
            };
            auto opLaneSort = getOperandLaneSort();
            auto getVecOperand = [&](const SelectVal &sv) -> SymbolicValue {
              if (auto rv = std::get_if<RValue>(&sv))
                return evalLValue(*rv, solver, store, pc);
              if (auto cf = std::get_if<Coef>(&sv)) {
                if (auto id = std::get_if<LocalOrSymId>(cf)) {
                  std::string nm = std::visit([](auto &&x) { return x.name; }, *id);
                  auto it = store.find(nm);
                  if (it != store.end() && it->second.kind == SymbolicValue::Kind::Vec)
                    return it->second;
                }
                smt::Term bv = evalCoef(*cf, solver, store, opLaneSort);
                std::vector<SymbolicValue> lanes;
                lanes.reserve(vt.size);
                for (std::uint64_t k = 0; k < vt.size; ++k)
                  lanes.emplace_back(SymbolicValue::Kind::Int, bv, solver.make_true());
                return buildVec(std::move(lanes));
              }
              throw std::runtime_error("Vec cmp: unsupported SelectVal");
            };
            auto lhsV = getVecOperand(arg.lhs);
            auto rhsV = getVecOperand(arg.rhs);
            std::vector<SymbolicValue> lanes;
            lanes.reserve(vt.size);
            smt::Sort i1 = solver.make_bv_sort(1);
            smt::Term one = solver.make_bv_value(i1, "1", 10);
            smt::Term zero = solver.make_bv_value(i1, "0", 10);
            // Detect FP lanes — BV_SLT/etc only work on BV sorts; for FP
            // operands we need FP_LT/FP_LEQ/FP_GT/FP_GEQ. The CmpAtom's
            // result type is i1 (so `vt.elem` is i1 here, an integer),
            // so we have to probe the *operand* sort to pick the right
            // SMT op. EQUAL/DISTINCT are polymorphic across both
            // theories.
            bool fpLane =
                !lhsV.arrayVal.empty() && solver.is_fp_sort(solver.get_sort(lhsV.arrayVal[0].term));
            for (std::uint64_t k = 0; k < vt.size; ++k) {
              const auto &l = lhsV.arrayVal[k].term;
              const auto &r = rhsV.arrayVal[k].term;
              smt::Kind opKind = relOpToSmtKind(arg.op, fpLane);
              smt::Term cond = solver.make_term(opKind, {l, r});
              smt::Term laneBit = solver.make_term(smt::Kind::ITE, {cond, one, zero});
              lanes.emplace_back(SymbolicValue::Kind::Int, laneBit, solver.make_true());
            }
            return buildVec(std::move(lanes));
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            // Mask form (cond form is scalar — wouldn't yield a vector).
            if (!arg.maskExpr)
              throw std::runtime_error("Vec SelectAtom: expected mask form");
            // Mask is a vector value: either an lvalue (RValueAtom) or a
            // bare sym/local reference (CoefAtom holding LocalOrSymId).
            SymbolicValue maskV;
            if (auto maskRv = std::get_if<RValueAtom>(&arg.maskExpr->first.v)) {
              maskV = evalLValue(maskRv->rval, solver, store, pc);
            } else if (auto maskCf = std::get_if<CoefAtom>(&arg.maskExpr->first.v)) {
              auto id = std::get_if<LocalOrSymId>(&maskCf->coef);
              if (!id)
                throw std::runtime_error("Vec SelectAtom: mask coef must be local/sym identifier");
              std::string nm = std::visit([](auto &&x) { return x.name; }, *id);
              auto it = store.find(nm);
              if (it == store.end() || it->second.kind != SymbolicValue::Kind::Vec)
                throw std::runtime_error("Vec SelectAtom: mask must resolve to a vector");
              maskV = it->second;
            } else {
              throw std::runtime_error(
                  "Vec SelectAtom: mask must be a vector lvalue or identifier"
              );
            }
            auto loadArm = [&](const SelectVal &sv) -> SymbolicValue {
              if (auto rv = std::get_if<RValue>(&sv))
                return evalLValue(*rv, solver, store, pc);
              if (auto cf = std::get_if<Coef>(&sv)) {
                if (auto id = std::get_if<LocalOrSymId>(cf)) {
                  std::string nm = std::visit([](auto &&x) { return x.name; }, *id);
                  auto it = store.find(nm);
                  if (it != store.end() && it->second.kind == SymbolicValue::Kind::Vec)
                    return it->second;
                }
                smt::Term bv = evalCoef(*cf, solver, store, sortFor());
                std::vector<SymbolicValue> lanes;
                lanes.reserve(vt.size);
                for (std::uint64_t k = 0; k < vt.size; ++k)
                  lanes.emplace_back(SymbolicValue::Kind::Int, bv, solver.make_true());
                return buildVec(std::move(lanes));
              }
              throw std::runtime_error("Vec select arm: unsupported");
            };
            SymbolicValue vtArm = loadArm(arg.vtrue);
            SymbolicValue vfArm = loadArm(arg.vfalse);
            std::vector<SymbolicValue> lanes;
            lanes.reserve(vt.size);
            smt::Sort i1 = solver.make_bv_sort(1);
            smt::Term oneI1 = solver.make_bv_value(i1, "1", 10);
            for (std::uint64_t k = 0; k < vt.size; ++k) {
              smt::Term cond = solver.make_term(smt::Kind::EQUAL, {maskV.arrayVal[k].term, oneI1});
              smt::Term chosen = solver.make_term(
                  smt::Kind::ITE, {cond, vtArm.arrayVal[k].term, vfArm.arrayVal[k].term}
              );
              // [v0.2.1] Propagate per-lane is_defined: select on an undef
              // lane is still undef. Use the lane's source is_defined if
              // present; default to "defined" otherwise.
              smt::Term chosenDef;
              if (vtArm.arrayVal[k].is_defined.internal && vfArm.arrayVal[k].is_defined.internal) {
                chosenDef = solver.make_term(
                    smt::Kind::ITE,
                    {cond, vtArm.arrayVal[k].is_defined, vfArm.arrayVal[k].is_defined}
                );
              } else {
                chosenDef = solver.make_true();
              }
              lanes.emplace_back(SymbolicValue::Kind::Int, chosen, chosenDef);
            }
            return buildVec(std::move(lanes));
          } else if constexpr (std::is_same_v<T, OpAtom>) {
            // Per-lane scalar op with coef broadcast or coef-as-vector.
            std::vector<smt::Term> coefLanes;
            auto laneSort = getSort(vt.elem, solver);
            // Resolve coef as a per-lane sequence.
            if (auto i = std::get_if<IntLit>(&arg.coef)) {
              smt::Term bv = solver.make_bv_value(laneSort, std::to_string(i->value), 10);
              for (std::uint64_t k = 0; k < vt.size; ++k)
                coefLanes.push_back(bv);
            } else if (auto fl = std::get_if<FloatLit>(&arg.coef)) {
              if (!solver.is_fp_sort(laneSort))
                throw std::runtime_error(
                    "Vec OpAtom: float-literal coef on non-FP vector lane sort"
                );
              smt::Term fp =
                  solver.make_fp_value(laneSort, std::to_string(fl->value), smt::RoundingMode::RNE);
              for (std::uint64_t k = 0; k < vt.size; ++k)
                coefLanes.push_back(fp);
            } else if (auto id = std::get_if<LocalOrSymId>(&arg.coef)) {
              std::string nm = std::visit([](auto &&x) { return x.name; }, *id);
              auto it = store.find(nm);
              if (it == store.end())
                throw std::runtime_error("Vec OpAtom: coef name not in store: " + nm);
              if (it->second.kind == SymbolicValue::Kind::Vec) {
                for (std::uint64_t k = 0; k < vt.size; ++k)
                  coefLanes.push_back(it->second.arrayVal[k].term);
              } else {
                // Scalar local/sym → broadcast.
                for (std::uint64_t k = 0; k < vt.size; ++k)
                  coefLanes.push_back(it->second.term);
              }
            } else {
              throw std::runtime_error("Vec OpAtom: unsupported coef shape");
            }
            // Rval is a vector lvalue.
            SymbolicValue rvalV = evalLValue(arg.rval, solver, store, pc);
            std::vector<SymbolicValue> lanes;
            lanes.reserve(vt.size);
            bool fpLane = solver.is_fp_sort(laneSort);
            auto rmRNE = fpLane ? solver.make_rm_value(smt::RoundingMode::RNE) : smt::Term();
            for (std::uint64_t k = 0; k < vt.size; ++k) {
              smt::Term c = coefLanes[k];
              smt::Term r = rvalV.arrayVal[k].term;
              smt::Term out;
              if (fpLane) {
                // Float lanes: SMT-LIB FP theory.
                switch (arg.op) {
                  case AtomOpKind::Mul:
                    out = solver.make_term(smt::Kind::FP_MUL, {rmRNE, c, r});
                    break;
                  case AtomOpKind::Div:
                    out = solver.make_term(smt::Kind::FP_DIV, {rmRNE, c, r});
                    break;
                  default:
                    throw std::runtime_error("Vec FP OpAtom: only mul/div supported on FP lanes");
                }
                // [v0.2.1] Per-lane FP rule 6/7: each lane must be finite.
                assertFPFinite(out, solver, pc);
              } else {
                // [v0.2.1] Per-lane shift UB: amount in [0, width) and Shl
                // requires non-negative operand (rule 4/5).
                auto sortR = solver.get_sort(r);
                auto sortC = solver.get_sort(c);
                auto bvZeroR = solver.make_bv_zero(sortR);
                auto bvZeroC = solver.make_bv_zero(sortC);
                uint32_t laneWidth = solver.get_bv_width(sortC);
                auto widthTerm = solver.make_bv_value(sortR, std::to_string(laneWidth), 10);
                auto addShiftBounds = [&]() {
                  pc.push_back(solver.make_term(smt::Kind::BV_SLE, {bvZeroR, r}));
                  pc.push_back(solver.make_term(smt::Kind::BV_ULT, {r, widthTerm}));
                };
                switch (arg.op) {
                  case AtomOpKind::Mul: {
                    auto ov = solver.make_term(smt::Kind::BV_SMUL_OVERFLOW, {c, r});
                    pc.push_back(solver.make_term(smt::Kind::NOT, {ov}));
                    out = solver.make_term(smt::Kind::BV_MUL, {c, r});
                    break;
                  }
                  case AtomOpKind::Div: {
                    pc.push_back(solver.make_term(smt::Kind::DISTINCT, {r, bvZeroR}));
                    // [v0.2.1 fix] Per-lane INT_MIN / -1 overflow is UB
                    // (rule 4 lifted to lanes by rule 21).
                    auto min_signed = solver.make_bv_min_signed(solver.get_sort(c));
                    auto minus_one = solver.make_bv_value_int64(solver.get_sort(r), -1);
                    auto is_min = solver.make_term(smt::Kind::EQUAL, {c, min_signed});
                    auto is_minus_one = solver.make_term(smt::Kind::EQUAL, {r, minus_one});
                    auto div_overflow = solver.make_term(smt::Kind::AND, {is_min, is_minus_one});
                    pc.push_back(solver.make_term(smt::Kind::NOT, {div_overflow}));
                    out = solver.make_term(smt::Kind::BV_SDIV, {c, r});
                    break;
                  }
                  case AtomOpKind::Mod: {
                    pc.push_back(solver.make_term(smt::Kind::DISTINCT, {r, bvZeroR}));
                    // [v0.2.1 fix] Per-lane INT_MIN % -1 overflow is UB
                    // (rule 4 lifted to lanes by rule 21).
                    auto min_signed_m = solver.make_bv_min_signed(solver.get_sort(c));
                    auto minus_one_m = solver.make_bv_value_int64(solver.get_sort(r), -1);
                    auto is_min_m = solver.make_term(smt::Kind::EQUAL, {c, min_signed_m});
                    auto is_minus_one_m = solver.make_term(smt::Kind::EQUAL, {r, minus_one_m});
                    auto mod_overflow =
                        solver.make_term(smt::Kind::AND, {is_min_m, is_minus_one_m});
                    pc.push_back(solver.make_term(smt::Kind::NOT, {mod_overflow}));
                    out = solver.make_term(smt::Kind::BV_SREM, {c, r});
                    break;
                  }
                  case AtomOpKind::And:
                    out = solver.make_term(smt::Kind::BV_AND, {c, r});
                    break;
                  case AtomOpKind::Or:
                    out = solver.make_term(smt::Kind::BV_OR, {c, r});
                    break;
                  case AtomOpKind::Xor:
                    out = solver.make_term(smt::Kind::BV_XOR, {c, r});
                    break;
                  case AtomOpKind::Shl: {
                    addShiftBounds();
                    pc.push_back(solver.make_term(smt::Kind::BV_SLE, {bvZeroC, c}));
                    auto shifted = solver.make_term(smt::Kind::BV_SHL, {c, r});
                    auto unshifted = solver.make_term(smt::Kind::BV_ASHR, {shifted, r});
                    pc.push_back(solver.make_term(smt::Kind::EQUAL, {unshifted, c}));
                    out = shifted;
                    break;
                  }
                  case AtomOpKind::Shr:
                    addShiftBounds();
                    out = solver.make_term(smt::Kind::BV_ASHR, {c, r});
                    break;
                  case AtomOpKind::LShr:
                    addShiftBounds();
                    out = solver.make_term(smt::Kind::BV_SHR, {c, r});
                    break;
                }
              }
              lanes.emplace_back(SymbolicValue::Kind::Int, out, solver.make_true());
            }
            return buildVec(std::move(lanes));
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            // Per-lane cast. Src is a vector lvalue or sym; dst is the
            // outer vt (already known to be vector by AssignInstr).
            SymbolicValue srcV;
            std::visit(
                [&](auto &&s) {
                  using S = std::decay_t<decltype(s)>;
                  if constexpr (std::is_same_v<S, LValue>)
                    srcV = evalLValue(s, solver, store, pc);
                  else if constexpr (std::is_same_v<S, SymId>) {
                    auto it = store.find(s.name);
                    if (it != store.end())
                      srcV = it->second;
                  } else {
                    throw std::runtime_error("Vec cast: unsupported src kind");
                  }
                },
                arg.src
            );
            if (srcV.kind != SymbolicValue::Kind::Vec)
              throw std::runtime_error("Vec cast: src must be a vector value");
            auto dstSort = getSort(vt.elem, solver);
            bool dstIsFp = solver.is_fp_sort(dstSort);
            std::vector<SymbolicValue> lanes;
            lanes.reserve(vt.size);
            for (std::uint64_t k = 0; k < vt.size; ++k) {
              smt::Term lane = srcV.arrayVal[k].term;
              auto srcSort = solver.get_sort(lane);
              bool srcIsFp = solver.is_fp_sort(srcSort);
              smt::Term out;
              if (srcIsFp && !dstIsFp) { // FP -> BV (RTZ + range check per lane)
                uint32_t width = solver.get_bv_width(dstSort);
                assertFPFinite(lane, solver, pc);
                double lo = -std::ldexp(1.0, static_cast<int>(width) - 1);
                double hi = std::ldexp(1.0, static_cast<int>(width) - 1);
                auto loFp = solver.make_fp_value_from_real(srcSort, lo, smt::RoundingMode::RNE);
                auto hiFp = solver.make_fp_value_from_real(srcSort, hi, smt::RoundingMode::RNE);
                pc.push_back(solver.make_term(smt::Kind::FP_GEQ, {lane, loFp}));
                pc.push_back(solver.make_term(smt::Kind::FP_LT, {lane, hiFp}));
                auto rmRTZ = solver.make_rm_value(smt::RoundingMode::RTZ);
                out = solver.make_term(smt::Kind::FP_TO_SBV, {rmRTZ, lane}, {width});
              } else if (!srcIsFp && dstIsFp) {
                auto [exp, sig] = solver.get_fp_dims(dstSort);
                auto rmRNE = solver.make_rm_value(smt::RoundingMode::RNE);
                out = solver.make_term(smt::Kind::FP_TO_FP_FROM_SBV, {rmRNE, lane}, {exp, sig});
                // [v0.2.1] Per-lane finite (rule 6/7 lifted).
                assertFPFinite(out, solver, pc);
              } else if (srcIsFp && dstIsFp) {
                auto [exp, sig] = solver.get_fp_dims(dstSort);
                auto rmRNE = solver.make_rm_value(smt::RoundingMode::RNE);
                out = solver.make_term(smt::Kind::FP_TO_FP_FROM_FP, {rmRNE, lane}, {exp, sig});
                // [v0.2.1] Per-lane finite (rule 6/7 lifted) — catches
                // f64→f32 overflow producing ±inf.
                assertFPFinite(out, solver, pc);
              } else { // BV -> BV
                uint32_t sw = solver.get_bv_width(srcSort);
                uint32_t dw = solver.get_bv_width(dstSort);
                if (sw == dw)
                  out = lane;
                else if (sw < dw)
                  out = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {lane}, {dw - sw});
                else
                  out = solver.make_term(smt::Kind::BV_EXTRACT, {lane}, {dw - 1, 0});
              }
              lanes.emplace_back(SymbolicValue::Kind::Int, out, solver.make_true());
            }
            return buildVec(std::move(lanes));
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            // Per-lane bitwise NOT for vector operand.
            SymbolicValue srcV = evalLValue(arg.rval, solver, store, pc);
            if (srcV.kind != SymbolicValue::Kind::Vec)
              throw std::runtime_error("Vec UnaryAtom: src must be vector");
            std::vector<SymbolicValue> lanes;
            lanes.reserve(vt.size);
            for (std::uint64_t k = 0; k < vt.size; ++k) {
              smt::Term lane = srcV.arrayVal[k].term;
              smt::Term out = solver.make_term(smt::Kind::BV_NOT, {lane});
              lanes.emplace_back(SymbolicValue::Kind::Int, out, solver.make_true());
            }
            return buildVec(std::move(lanes));
          } else {
            throw std::runtime_error("Solver: this vector RHS atom kind isn't yet lowered");
          }
        },
        a.v
    );
  }

  smt::Term SymbolicExecutor::evalCoef(
      const Coef &c, smt::ISolver &solver, SymbolicStore &store,
      std::optional<smt::Sort> expectedSort
  ) {
    if (auto lit = std::get_if<IntLit>(&c)) {
      smt::Sort s = expectedSort.value_or(solver.make_bv_sort(32));
      return solver.make_bv_value(s, std::to_string(lit->value), 10);
    }
    if (auto flit = std::get_if<FloatLit>(&c)) {
      smt::Sort s = expectedSort.value_or(solver.make_fp_sort(8, 24));
      return solver.make_fp_value(s, std::to_string(flit->value), smt::RoundingMode::RNE);
    }
    if (std::get_if<NullLit>(&c)) {
      // null = 0 as BV64 (pointer width)
      return solver.make_bv_value(solver.make_bv_sort(64), "0", 10);
    }
    auto id = std::get<LocalOrSymId>(c);
    return std::visit([&](auto &&v) { return store.at(v.name).term; }, id);
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::evalSelectVal(
      const SelectVal &sv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
      std::optional<smt::Sort> expectedSort
  ) {
    if (auto rv = std::get_if<RValue>(&sv))
      return evalLValue(*rv, solver, store, pc);
    smt::Term t = evalCoef(std::get<Coef>(sv), solver, store, expectedSort);
    return SymbolicValue(SymbolicValue::Kind::Int, t, solver.make_true());
  }

  smt::Term SymbolicExecutor::evalCond(
      const Cond &c, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc
  ) {
    SymbolicValue lhsVal = evalExpr(c.lhs, solver, store, pc);
    smt::Term lhs = lhsVal.term;
    SymbolicValue rhsVal = evalExpr(c.rhs, solver, store, pc, solver.get_sort(lhs));
    smt::Term rhs = rhsVal.term;

    // [v0.2.1] Dynamic Rule 14 Relational Comparison check
    TypePtr lhsType = resolveExprType(c.lhs);
    TypePtr rhsType = resolveExprType(c.rhs);
    bool isLhsPtr = lhsType && std::holds_alternative<PtrType>(lhsType->v);
    bool isRhsPtr = rhsType && std::holds_alternative<PtrType>(rhsType->v);
    bool isRelational =
        (c.op == RelOp::LT || c.op == RelOp::LE || c.op == RelOp::GT || c.op == RelOp::GE);
    if (isRelational && isLhsPtr && isRhsPtr) {
      auto bv64 = solver.make_bv_sort(64);
      auto zero = solver.make_bv_value_int64(bv64, 0);
      if (lhsVal.prov_base.internal && rhsVal.prov_base.internal) {
        auto eqBase = solver.make_term(smt::Kind::EQUAL, {lhsVal.prov_base, rhsVal.prov_base});
        auto nonZeroBase = solver.make_term(smt::Kind::DISTINCT, {lhsVal.prov_base, zero});
        pc.push_back(solver.make_term(smt::Kind::AND, {eqBase, nonZeroBase}));
      } else {
        pc.push_back(solver.make_false());
      }
    }

    auto lSort = solver.get_sort(lhs);
    auto rSort = solver.get_sort(rhs);

    if (solver.is_fp_sort(lSort)) {
      switch (c.op) {
        case RelOp::EQ:
          return solver.make_term(smt::Kind::FP_EQUAL, {lhs, rhs});
        case RelOp::NE:
          return solver.make_term(
              smt::Kind::NOT, {solver.make_term(smt::Kind::FP_EQUAL, {lhs, rhs})}
          );
        case RelOp::LT:
          return solver.make_term(smt::Kind::FP_LT, {lhs, rhs});
        case RelOp::LE:
          return solver.make_term(smt::Kind::FP_LEQ, {lhs, rhs});
        case RelOp::GT:
          return solver.make_term(smt::Kind::FP_GT, {lhs, rhs});
        case RelOp::GE:
          return solver.make_term(smt::Kind::FP_GEQ, {lhs, rhs});
      }
    }

    if (solver.is_bv_sort(lSort) && solver.is_bv_sort(rSort)) {
      auto lWidth = solver.get_bv_width(lSort);
      auto rWidth = solver.get_bv_width(rSort);
      if (lWidth < rWidth) {
        lhs = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {lhs}, {rWidth - lWidth});
      } else if (rWidth < lWidth) {
        rhs = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {rhs}, {lWidth - rWidth});
      }
    }

    switch (c.op) {
      case RelOp::EQ:
        return solver.make_term(smt::Kind::EQUAL, {lhs, rhs});
      case RelOp::NE:
        return solver.make_term(smt::Kind::DISTINCT, {lhs, rhs});
      case RelOp::LT:
        return solver.make_term(smt::Kind::BV_SLT, {lhs, rhs});
      case RelOp::LE:
        return solver.make_term(smt::Kind::BV_SLE, {lhs, rhs});
      case RelOp::GT:
        return solver.make_term(smt::Kind::BV_SGT, {lhs, rhs});
      case RelOp::GE:
        return solver.make_term(smt::Kind::BV_SGE, {lhs, rhs});
    }
    return {};
  }

  SymbolicExecutor::Result SymbolicExecutor::sample(
      const std::string &funcName, uint32_t n, uint32_t maxPathLen, bool requireTerminal,
      const std::vector<std::string> &prefixPath,
      const std::unordered_map<std::string, int64_t> &fixedSyms
  ) {
    const FunDecl *entry = nullptr;
    for (const auto &f: prog_.funs) {
      if (f.name.name == funcName) {
        entry = &f;
        break;
      }
    }
    if (!entry)
      throw std::runtime_error("Function not found: " + funcName);

    DiagBag diags;
    CFG cfg = CFG::build(*entry, diags);
    if (diags.hasErrors())
      throw std::runtime_error("CFG build failed");

    auto nextToRet = cfg.shortestPathToRet(*entry);

    // Determine number of threads
    uint32_t num_threads = config_.num_threads;
    if (num_threads == 0) {
      num_threads = std::thread::hardware_concurrency();
      if (num_threads == 0)
        num_threads = 1; // Fallback if hardware_concurrency returns 0
    }

    // Lambda to generate a random path and attempt to solve it
    // Returns optional Result: nullopt if path should be skipped, otherwise the solve result
    auto tryOneSample = [&](std::mt19937 &rng) -> std::optional<Result> {
      std::vector<std::string> path = prefixPath;
      if (path.empty()) {
        path.push_back(cfg.blocks[cfg.entry]);
      }

      std::size_t currentIdx = cfg.indexOf.at(path.back());
      bool terminated = std::holds_alternative<RetTerm>(entry->blocks[currentIdx].term) ||
                        std::holds_alternative<UnreachableTerm>(entry->blocks[currentIdx].term);

      // Random walk
      while (!terminated && path.size() < maxPathLen) {
        const auto &successors = cfg.succ[currentIdx];
        if (successors.empty())
          break;

        std::uniform_int_distribution<std::size_t> dist(0, successors.size() - 1);
        std::size_t nextIdx = successors[dist(rng)];
        path.push_back(cfg.blocks[nextIdx]);
        currentIdx = nextIdx;
        terminated = std::holds_alternative<RetTerm>(entry->blocks[currentIdx].term) ||
                     std::holds_alternative<UnreachableTerm>(entry->blocks[currentIdx].term);
      }

      // Handle non-terminated paths
      if (!terminated) {
        if (requireTerminal) {
          // Append shortest path to ret
          while (!std::holds_alternative<RetTerm>(entry->blocks[currentIdx].term)) {
            auto it = nextToRet.find(currentIdx);
            if (it == nextToRet.end()) {
              // Cannot reach ret from here - skip this sample
              return std::nullopt;
            }
            currentIdx = it->second;
            path.push_back(cfg.blocks[currentIdx]);
          }
        } else {
          // Discard if not terminated
          return std::nullopt;
        }
      }

      // Try to solve this path
      try {
        return solve(funcName, path, fixedSyms);
      } catch (const std::exception &e) {
        Result errRes;
        errRes.unknown = true;
        errRes.message = e.what();
        return errRes;
      }
    };

    // Single-threaded execution
    if (num_threads == 1) {
      std::mt19937 rng(config_.seed);
      Result lastRes;
      lastRes.unknown = true;

      for (uint32_t i = 0; i < n; ++i) {
        auto res = tryOneSample(rng);
        if (!res)
          continue; // Path was skipped

        if (res->sat)
          return *res;
        lastRes = std::move(*res);
      }

      return lastRes;
    }

    // Multi-threaded execution
    std::atomic<bool> found(false);
    std::atomic<uint32_t> samplesProcessed(0);
    std::mutex resultMutex;
    Result satResult;
    Result lastRes;
    lastRes.unknown = true;

    auto workerFunc = [&](uint32_t threadId) {
      std::mt19937 rng(config_.seed + threadId);
      Result threadLastRes;
      threadLastRes.unknown = true;

      while (samplesProcessed.fetch_add(1) < n && !found.load()) {
        if (found.load())
          break;

        auto res = tryOneSample(rng);
        if (!res)
          continue; // Path was skipped

        if (res->sat) {
          std::lock_guard<std::mutex> lock(resultMutex);
          if (!found.load()) {
            satResult = std::move(*res);
            found.store(true);
          }
          return;
        }
        threadLastRes = std::move(*res);
      }

      // Update last result
      std::lock_guard<std::mutex> lock(resultMutex);
      if (!threadLastRes.message.empty() ||
          (!threadLastRes.sat && !threadLastRes.unsat && threadLastRes.unknown)) {
        lastRes = std::move(threadLastRes);
      }
    };

    // Launch worker threads
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < num_threads; ++i) {
      threads.emplace_back(workerFunc, i);
    }

    // Wait for all threads to complete
    for (auto &t: threads) {
      t.join();
    }

    if (found.load()) {
      return satResult;
    }

    return lastRes;
  }

  TypePtr SymbolicExecutor::resolveLValueType(const LValue &lv) const {
    if (!currentFun_)
      throw std::runtime_error("resolveLValueType: no active FunDecl");
    const std::string baseName = lv.base.name;
    TypePtr cur;
    for (const auto &l: currentFun_->lets) {
      if (l.name.name == baseName) {
        cur = l.type;
        break;
      }
    }
    if (!cur) {
      for (const auto &p: currentFun_->params) {
        if (p.name.name == baseName) {
          cur = p.type;
          break;
        }
      }
    }
    if (!cur)
      throw std::runtime_error("resolveLValueType: base local not found: " + baseName);

    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        (void) ai;
        if (auto at = std::get_if<ArrayType>(&cur->v)) {
          cur = at->elem;
        } else if (auto vt = std::get_if<VecType>(&cur->v)) {
          cur = vt->elem;
        } else {
          throw std::runtime_error("resolveLValueType: indexing non-array/vector type");
        }
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (auto st = std::get_if<StructType>(&cur->v)) {
          auto sIt = structs_.find(st->name.name);
          if (sIt == structs_.end())
            throw std::runtime_error("resolveLValueType: unknown struct type: " + st->name.name);
          bool found = false;
          for (const auto &f: sIt->second->fields) {
            if (f.name == af->field) {
              cur = f.type;
              found = true;
              break;
            }
          }
          if (!found)
            throw std::runtime_error("resolveLValueType: field not found in struct: " + af->field);
        } else {
          throw std::runtime_error("resolveLValueType: field access on non-struct type");
        }
      }
    }
    return cur;
  }

  std::string SymbolicExecutor::buildLValueKey(const LValue &lv) const {
    std::string key = lv.base.name;
    for (const auto &acc: lv.accesses) {
      if (auto af = std::get_if<AccessField>(&acc)) {
        key += "." + af->field;
      } else {
        return {};
      }
    }
    return key;
  }

  TypePtr SymbolicExecutor::resolveExprType(const Expr &e) const {
    if (!currentFun_)
      return nullptr;
    return resolveAtomType(e.first);
  }

  TypePtr SymbolicExecutor::resolveAtomType(const Atom &a) const {
    return std::visit(
        [&](auto &&arg) -> TypePtr {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, RValueAtom>) {
            return resolveLValueType(arg.rval);
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            auto pointeeTy = resolveLValueType(arg.lv);
            return std::make_shared<Type>(PtrType{pointeeTy, SourceSpan{}});
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            auto srcTy = resolveLValueType(arg.rval);
            if (srcTy) {
              if (auto pt = std::get_if<PtrType>(&srcTy->v)) {
                if (auto at = std::get_if<ArrayType>(&pt->pointee->v)) {
                  return std::make_shared<Type>(PtrType{at->elem, SourceSpan{}});
                }
              }
            }
            return nullptr;
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            auto srcTy = resolveLValueType(arg.rval);
            if (srcTy) {
              if (auto pt = std::get_if<PtrType>(&srcTy->v)) {
                if (auto st = std::get_if<StructType>(&pt->pointee->v)) {
                  auto sIt = structs_.find(st->name.name);
                  if (sIt != structs_.end()) {
                    for (const auto &f: sIt->second->fields) {
                      if (f.name == arg.field) {
                        return std::make_shared<Type>(PtrType{f.type, SourceSpan{}});
                      }
                    }
                  }
                }
              }
            }
            return nullptr;
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            auto srcTy = resolveLValueType(arg.rval);
            if (srcTy) {
              if (auto pt = std::get_if<PtrType>(&srcTy->v)) {
                return pt->pointee;
              }
            }
            return nullptr;
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            return resolveSelectValType(arg.vtrue);
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            return arg.dstType;
          } else {
            if constexpr (std::is_same_v<T, CoefAtom>) {
              if (std::holds_alternative<NullLit>(arg.coef)) {
                return std::make_shared<Type>(PtrType{nullptr, SourceSpan{}});
              }
            }
            return nullptr;
          }
        },
        a.v
    );
  }

  TypePtr SymbolicExecutor::resolveSelectValType(const SelectVal &sv) const {
    if (auto rv = std::get_if<RValue>(&sv)) {
      return resolveLValueType(*rv);
    }
    auto coef = std::get<Coef>(sv);
    if (std::holds_alternative<NullLit>(coef)) {
      return std::make_shared<Type>(PtrType{nullptr, SourceSpan{}});
    }
    return nullptr;
  }

} // namespace symir
