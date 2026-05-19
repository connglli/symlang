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
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Int;
      res.term = val;
      res.is_defined = solver.make_true();
      return res;
    }
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::evalInit(
      const InitVal &iv, const TypePtr &t, smt::ISolver &solver, SymbolicStore &store
  ) {
    if (iv.kind == InitVal::Kind::Undef)
      return makeUndef(t, solver);

    if (iv.kind == InitVal::Kind::Aggregate) {
      const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
      if (auto at = std::get_if<ArrayType>(&t->v)) {
        SymbolicValue res;
        res.kind = SymbolicValue::Kind::Array;
        for (size_t i = 0; i < elements.size(); ++i)
          res.arrayVal.push_back(evalInit(*elements[i], at->elem, solver, store));
        return res;
      } else if (auto st = std::get_if<StructType>(&t->v)) {
        SymbolicValue res;
        res.kind = SymbolicValue::Kind::Struct;
        auto it = structs_.find(st->name.name);
        if (it != structs_.end()) {
          for (size_t i = 0; i < elements.size(); ++i)
            res.structVal[it->second->fields[i].name] =
                evalInit(*elements[i], it->second->fields[i].type, solver, store);
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
      val = store.at(std::get<SymId>(iv.value).name).term;
    } else if (iv.kind == InitVal::Kind::Local) {
      val = store.at(std::get<LocalId>(iv.value).name).term;
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

    // 1. Declare symbols and fix values if requested
    for (const auto &s: entry->syms) {
      auto sv = createSymbolicValue(s.type, s.name.name, solver, true);
      store[s.name.name] = sv;

      // Add domain constraints
      if (s.domain) {
        std::visit(
            [&](auto &&d) {
              using T = std::decay_t<decltype(d)>;
              if constexpr (std::is_same_v<T, DomainInterval>) {
                auto loSort = getSort(s.type, solver);
                // Clamp [lo, hi] to the type's actual signed range so a wide domain
                // (e.g. full-i32) applied to a narrow type (i8, i16) doesn't produce
                // an unsatisfiable constraint after BV truncation.
                uint32_t bits = 64;
                if (auto *it = std::get_if<IntType>(&s.type->v)) {
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
                  auto lo = solver.make_bv_value_int64(loSort, effLo);
                  auto hi = solver.make_bv_value_int64(loSort, effHi);
                  pathConstraints.push_back(solver.make_term(smt::Kind::BV_SLE, {lo, sv.term}));
                  pathConstraints.push_back(solver.make_term(smt::Kind::BV_SLE, {sv.term, hi}));
                }
              } else if constexpr (std::is_same_v<T, DomainSet>) {
                std::vector<smt::Term> or_terms;
                for (auto v: d.values) {
                  auto vt = solver.make_bv_value_int64(getSort(s.type, solver), v);
                  or_terms.push_back(solver.make_term(smt::Kind::EQUAL, {sv.term, vt}));
                }
                if (!or_terms.empty()) {
                  smt::Term or_all = or_terms[0];
                  for (size_t i = 1; i < or_terms.size(); ++i)
                    or_all = solver.make_term(smt::Kind::OR, {or_all, or_terms[i]});
                  pathConstraints.push_back(or_all);
                }
              }
            },
            *s.domain
        );
      }

      if (fixedSyms.count(s.name.name)) {
        auto val = solver.make_bv_value_int64(getSort(s.type, solver), fixedSyms.at(s.name.name));
        pathConstraints.push_back(solver.make_term(smt::Kind::EQUAL, {sv.term, val}));
      }
    }

    // 2. Declare locals (parameters are also in store)
    for (const auto &p: entry->params) {
      store[p.name.name] = createSymbolicValue(p.type, p.name.name, solver);
    }
    for (const auto &l: entry->lets) {
      if (l.init) {
        store[l.name.name] = evalInit(*l.init, l.type, solver, store);
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
                auto lhsVal = evalLValue(arg.lhs, solver, store, pathConstraints);
                auto rhs = evalExpr(
                    arg.rhs, solver, store, pathConstraints,
                    lhsVal.kind == SymbolicValue::Kind::Int
                        ? std::optional(solver.get_sort(lhsVal.term))
                        : std::nullopt
                );
                SymbolicValue val(SymbolicValue::Kind::Int, rhs, solver.make_true());
                setLValue(arg.lhs, val, solver, store, pathConstraints);
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
                smt::Term ptrTerm = evalExpr(arg.ptr, solver, store, pathConstraints);

                // Determine pointee type from the ptr expression's first atom.
                // The first atom is an RValueAtom of a ptr-typed local in the
                // common case (rysmith generates `store %p, ...`).
                TypePtr pointeeType;
                if (auto *rv = std::get_if<RValueAtom>(&arg.ptr.first.v)) {
                  const std::string baseName = rv->rval.base.name;
                  // Look in lets and params for the ptr-typed binding.
                  for (const auto &l: currentFun_->lets) {
                    if (l.name.name == baseName) {
                      if (auto pt = std::get_if<PtrType>(&l.type->v))
                        pointeeType = pt->pointee;
                      break;
                    }
                  }
                  if (!pointeeType) {
                    for (const auto &p: currentFun_->params) {
                      if (p.name.name == baseName) {
                        if (auto pt = std::get_if<PtrType>(&p.type->v))
                          pointeeType = pt->pointee;
                        break;
                      }
                    }
                  }
                }
                if (!pointeeType)
                  throw std::runtime_error(
                      "store: cannot derive pointee type (only `store %p, ...` "
                      "with a ptr-typed local or parameter %p is currently supported)"
                  );

                auto pointeeSort = getSort(pointeeType, solver);
                smt::Term valTerm =
                    evalExpr(arg.val, solver, store, pathConstraints, std::optional(pointeeSort));

                for (const auto &l: currentFun_->lets) {
                  if (!typeMatch(l.type, pointeeType))
                    continue;
                  auto tagTerm = solver.make_bv_value_int64(
                      solver.make_bv_sort(kPtrBits), static_cast<int64_t>(tagOfLocal(l.name.name))
                  );
                  auto cond = solver.make_term(smt::Kind::EQUAL, {ptrTerm, tagTerm});
                  auto &sv = store.at(l.name.name);
                  sv.term = solver.make_term(smt::Kind::ITE, {cond, valTerm, sv.term});
                }
              }
            },
            ins
        );
      }

      // Branch condition if not last block
      if (i + 1 < path.size()) {
        const std::string &nextLabel = path[i + 1];
        std::visit(
            [&](auto &&term) {
              using T = std::decay_t<decltype(term)>;
              if constexpr (std::is_same_v<T, BrTerm>) {
                if (term.isConditional) {
                  auto cond = evalCond(*term.cond, solver, store, pathConstraints);
                  if (term.thenLabel.name == nextLabel) {
                    pathConstraints.push_back(cond);
                  } else if (term.elseLabel.name == nextLabel) {
                    pathConstraints.push_back(solver.make_term(smt::Kind::NOT, {cond}));
                  } else {
                    throw std::runtime_error("Path edge not in CFG: " + label + " -> " + nextLabel);
                  }
                } else {
                  if (term.dest.name != nextLabel)
                    throw std::runtime_error("Path edge not in CFG: " + label + " -> " + nextLabel);
                }
              } else {
                throw std::runtime_error(
                    "Block " + label + " ends with non-branch terminator but path has more blocks"
                );
              }
            },
            block.term
        );
      }
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
      for (const auto &s: entry->syms) {
        auto term = store.at(s.name.name).term;
        auto val_term = solver.get_value(term);

        if (solver.is_fp_sort(solver.get_sort(term))) {
          std::string bin = solver.get_fp_value_string(val_term);
          uint64_t bits = 0;
          for (char c: bin) {
            bits = (bits << 1) | (c - '0');
          }
          double d;
          if (bin.size() <= 32) {
            uint32_t b32 = (uint32_t) bits;
            float f;
            std::memcpy(&f, &b32, sizeof(f));
            d = f;
          } else {
            std::memcpy(&d, &bits, sizeof(d));
          }
          finalRes.model[s.name.name] = d;
        } else {
          auto val_str = solver.get_bv_value_string(val_term, 10);
          // Bitwuzla returns the unsigned bit-pattern as decimal.
          // Use stoull so values in (INT64_MAX, UINT64_MAX] don't throw out_of_range.
          uint64_t uraw = std::stoull(val_str, nullptr, 10);
          uint32_t width = solver.get_bv_width(solver.get_sort(term));
          int64_t raw;
          if (width < 64) {
            // Mask to bitwidth then sign-extend.
            uint64_t mask = (uint64_t(1) << width) - 1;
            uraw &= mask;
            if (uraw >= (uint64_t(1) << (width - 1)))
              raw = static_cast<int64_t>(uraw) - static_cast<int64_t>(uint64_t(1) << width);
            else
              raw = static_cast<int64_t>(uraw);
          } else {
            // width == 64: reinterpret unsigned bit-pattern as signed two's complement.
            raw = static_cast<int64_t>(uraw);
          }
          finalRes.model[s.name.name] = raw;
        }
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
      const LValue &lv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc
  ) {
    SymbolicValue res = store.at(lv.base.name);
    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        if (res.kind != SymbolicValue::Kind::Array)
          throw std::runtime_error("Indexing non-array");
        size_t array_size = res.arrayVal.size();
        smt::Term idx;
        if (auto lit = std::get_if<IntLit>(&ai->index)) {
          idx = solver.make_bv_value(solver.make_bv_sort(32), std::to_string(lit->value), 10);
          SymbolicValue next = res.arrayVal.at(lit->value);
          res = std::move(next);
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
    } else if (t.kind == SymbolicValue::Kind::Array) {
      if (t.arrayVal.size() != f.arrayVal.size())
        throw std::runtime_error("Muxing arrays of different sizes");
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
      if (cur.kind != SymbolicValue::Kind::Array)
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

  smt::Term SymbolicExecutor::evalExpr(
      const Expr &e, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
      std::optional<smt::Sort> expectedSort
  ) {
    smt::Term res = evalAtom(e.first, solver, store, pc, expectedSort);
    for (const auto &tail: e.rest) {
      smt::Term right = evalAtom(tail.atom, solver, store, pc, expectedSort);
      auto lSort = solver.get_sort(res);
      auto rSort = solver.get_sort(right);

      if (solver.is_fp_sort(lSort)) {
        if (tail.op == AddOp::Plus) {
          res = solver.make_term(smt::Kind::FP_ADD, {res, right});
        } else {
          res = solver.make_term(smt::Kind::FP_SUB, {res, right});
        }
        assertFPFinite(res, solver, pc);
      } else if (solver.is_bv_sort(lSort) && solver.is_bv_sort(rSort)) {
        auto lWidth = solver.get_bv_width(lSort);
        auto rWidth = solver.get_bv_width(rSort);
        if (lWidth < rWidth) {
          res = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {res}, {rWidth - lWidth});
        } else if (rWidth < lWidth) {
          right = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {right}, {lWidth - rWidth});
        }

        if (tail.op == AddOp::Plus) {
          auto overflow = solver.make_term(smt::Kind::BV_SADD_OVERFLOW, {res, right});
          pc.push_back(solver.make_term(smt::Kind::NOT, {overflow}));
          res = solver.make_term(smt::Kind::BV_ADD, {res, right});
        } else {
          auto overflow = solver.make_term(smt::Kind::BV_SSUB_OVERFLOW, {res, right});
          pc.push_back(solver.make_term(smt::Kind::NOT, {overflow}));
          res = solver.make_term(smt::Kind::BV_SUB, {res, right});
        }
      }
    }
    return res;
  }

  smt::Term SymbolicExecutor::evalAtom(
      const Atom &a, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
      std::optional<smt::Sort> expectedSort
  ) {
    return std::visit(
        [&](auto &&arg) -> smt::Term {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            smt::Term c = evalCoef(arg.coef, solver, store, expectedSort);
            smt::Term r = evalLValue(arg.rval, solver, store, pc).term;

            auto cSort = solver.get_sort(c);
            auto rSort = solver.get_sort(r);

            if (solver.is_fp_sort(cSort)) {
              smt::Term fpRes;
              if (arg.op == AtomOpKind::Mul)
                fpRes = solver.make_term(smt::Kind::FP_MUL, {c, r});
              else if (arg.op == AtomOpKind::Div)
                fpRes = solver.make_term(smt::Kind::FP_DIV, {c, r});
              else if (arg.op == AtomOpKind::Mod) {
                // fmod(x,y) = x - trunc(x/y)*y  (truncated-quotient, matches integer %)
                // Encode as: fp.sub(x, fp.mul(fp.roundToIntegral[RTZ](fp.div(x,y)), y))
                auto rmRTZ = solver.make_rm_value(smt::RoundingMode::RTZ);
                auto q = solver.make_term(smt::Kind::FP_DIV, {c, r});      // div with default RNE
                auto qi = solver.make_term(smt::Kind::FP_RTI, {rmRTZ, q}); // truncate to integer
                auto prod = solver.make_term(smt::Kind::FP_MUL, {qi, r});  // qi * y
                fpRes = solver.make_term(smt::Kind::FP_SUB, {c, prod});    // x - qi*y
              } else
                return {};
              assertFPFinite(fpRes, solver, pc);
              return fpRes;
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
              return solver.make_term(smt::Kind::BV_MUL, {c, r});
            }
            if (arg.op == AtomOpKind::Div) {
              // Check overflow: c == INT_MIN && r == -1
              auto min_signed = solver.make_bv_min_signed(solver.get_sort(c));
              auto minus_one = solver.make_bv_value_int64(solver.get_sort(r), -1);
              auto is_min = solver.make_term(smt::Kind::EQUAL, {c, min_signed});
              auto is_minus_one = solver.make_term(smt::Kind::EQUAL, {r, minus_one});
              auto div_overflow = solver.make_term(smt::Kind::AND, {is_min, is_minus_one});
              pc.push_back(solver.make_term(smt::Kind::NOT, {div_overflow}));
              return solver.make_term(smt::Kind::BV_SDIV, {c, r});
            }
            if (arg.op == AtomOpKind::Mod) {
              // Check overflow for mod
              auto min_signed = solver.make_bv_min_signed(solver.get_sort(c));
              auto minus_one = solver.make_bv_value_int64(solver.get_sort(r), -1);
              auto is_min = solver.make_term(smt::Kind::EQUAL, {c, min_signed});
              auto is_minus_one = solver.make_term(smt::Kind::EQUAL, {r, minus_one});
              auto mod_overflow = solver.make_term(smt::Kind::AND, {is_min, is_minus_one});
              pc.push_back(solver.make_term(smt::Kind::NOT, {mod_overflow}));
              return solver.make_term(smt::Kind::BV_SREM, {c, r});
            }
            if (arg.op == AtomOpKind::And) {
              return solver.make_term(smt::Kind::BV_AND, {c, r});
            }
            if (arg.op == AtomOpKind::Or) {
              return solver.make_term(smt::Kind::BV_OR, {c, r});
            }
            if (arg.op == AtomOpKind::Xor) {
              return solver.make_term(smt::Kind::BV_XOR, {c, r});
            }
            if (arg.op == AtomOpKind::Shl || arg.op == AtomOpKind::Shr ||
                arg.op == AtomOpKind::LShr) {
              // Overshift UB: amount < width
              uint32_t width = solver.get_bv_width(solver.get_sort(c));
              auto width_term = solver.make_bv_value(solver.get_sort(r), std::to_string(width), 10);
              pc.push_back(solver.make_term(smt::Kind::BV_ULT, {r, width_term}));

              if (arg.op == AtomOpKind::Shl)
                return solver.make_term(smt::Kind::BV_SHL, {c, r});
              if (arg.op == AtomOpKind::Shr)
                return solver.make_term(smt::Kind::BV_ASHR, {c, r});
              if (arg.op == AtomOpKind::LShr)
                return solver.make_term(smt::Kind::BV_SHR, {c, r});
            }
            return {};
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            auto r = evalLValue(arg.rval, solver, store, pc).term;
            if (arg.op == UnaryOpKind::Not) {
              return solver.make_term(smt::Kind::BV_NOT, {r});
            }
            return {};
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            smt::Term cond = evalCond(*arg.cond, solver, store, pc);
            smt::Term vt = evalSelectVal(arg.vtrue, solver, store, pc, expectedSort);
            smt::Term vf = evalSelectVal(arg.vfalse, solver, store, pc, expectedSort);
            auto vtSort = solver.get_sort(vt);
            auto vfSort = solver.get_sort(vf);
            if (solver.is_bv_sort(vtSort) && solver.is_bv_sort(vfSort)) {
              auto vtWidth = solver.get_bv_width(vtSort);
              auto vfWidth = solver.get_bv_width(vfSort);
              if (vtWidth < vfWidth) {
                vt = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {vt}, {vfWidth - vtWidth});
              } else if (vfWidth < vtWidth) {
                vf = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {vf}, {vtWidth - vfWidth});
              }
            }
            return solver.make_term(smt::Kind::ITE, {cond, vt, vf});
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            return evalCoef(arg.coef, solver, store, expectedSort);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            return evalLValue(arg.rval, solver, store, pc).term;
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

            // Correct handling for FP casts requiring indices
            if (srcIsFp && !dstIsFp) { // FP -> BV (spec §7.4 rule 8: range check)
              uint32_t width = solver.get_bv_width(dstSort);
              auto srcSort = solver.get_sort(src);
              // Add PC constraints: src is finite and in [-2^(w-1), 2^(w-1)).
              assertFPFinite(src, solver, pc);
              double lo = -std::ldexp(1.0, static_cast<int>(width) - 1);
              double hi = std::ldexp(1.0, static_cast<int>(width) - 1);
              auto loFp = solver.make_fp_value_from_real(srcSort, lo, smt::RoundingMode::RNE);
              auto hiFp = solver.make_fp_value_from_real(srcSort, hi, smt::RoundingMode::RNE);
              pc.push_back(solver.make_term(smt::Kind::FP_GEQ, {src, loFp}));
              pc.push_back(solver.make_term(smt::Kind::FP_LT, {src, hiFp}));
              return solver.make_term(smt::Kind::FP_TO_SBV, {src}, {width});
            }
            if (!srcIsFp && dstIsFp) { // BV -> FP
              auto [exp, sig] = solver.get_fp_dims(dstSort);
              return solver.make_term(smt::Kind::FP_TO_FP_FROM_SBV, {src}, {exp, sig});
            }
            if (srcIsFp && dstIsFp) { // FP -> FP
              auto [exp, sig] = solver.get_fp_dims(dstSort);
              return solver.make_term(smt::Kind::FP_TO_FP_FROM_FP, {src}, {exp, sig});
            }

            // BV -> BV resizing
            uint32_t srcWidth = solver.get_bv_width(solver.get_sort(src));
            uint32_t dstWidth = solver.get_bv_width(dstSort);
            if (srcWidth == dstWidth)
              return src;
            if (srcWidth < dstWidth) {
              return solver.make_term(smt::Kind::BV_SIGN_EXTEND, {src}, {dstWidth - srcWidth});
            } else {
              return solver.make_term(smt::Kind::BV_EXTRACT, {src}, {dstWidth - 1, 0});
            }
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            // addr %v — encode as a BV64 tag identifying the target local.
            // Only whole-local addresses are currently supported; struct/array
            // field addressing (spec rule 15) is left to a future revision.
            if (!arg.lv.accesses.empty())
              throw std::runtime_error(
                  "'addr' with field/index accesses not yet supported in solver"
              );
            const std::string targetName = arg.lv.base.name;
            return solver.make_bv_value_int64(
                solver.make_bv_sort(kPtrBits), static_cast<int64_t>(tagOfLocal(targetName))
            );
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            // load %p — dispatch over candidate targets of the pointee type.
            // Build a chain of ites: ite(p == tag_v, value_of_v, fallback).
            // Fallback (no target matches) is zero; defenseable since hitting
            // it means a UB null-deref under the path.
            if (!currentFun_)
              throw std::runtime_error("load encountered without active FunDecl");

            // Evaluate the pointer expression to a BV64 term.
            smt::Term ptrTerm = evalLValue(arg.rval, solver, store, pc).term;

            // Identify pointee type from the pointer-typed lvalue.
            // arg.rval is an LValue; resolve the base local's type and walk
            // any accesses (none expected in the rysmith use case).
            const std::string baseName = arg.rval.base.name;
            TypePtr baseType;
            for (const auto &l: currentFun_->lets) {
              if (l.name.name == baseName) {
                baseType = l.type;
                break;
              }
            }
            if (!baseType) {
              for (const auto &p: currentFun_->params)
                if (p.name.name == baseName) {
                  baseType = p.type;
                  break;
                }
            }
            if (!baseType || !std::holds_alternative<PtrType>(baseType->v))
              throw std::runtime_error("load target is not ptr-typed: " + baseName);
            const TypePtr pointeeType = std::get<PtrType>(baseType->v).pointee;

            // Default fallback: zero of pointee sort.
            auto pointeeSort = getSort(pointeeType, solver);
            smt::Term result;
            if (solver.is_fp_sort(pointeeSort)) {
              auto [exp, sig] = solver.get_fp_dims(pointeeSort);
              auto zeroBv = solver.make_bv_value(solver.make_bv_sort(exp + sig), "0", 10);
              result = solver.make_term(smt::Kind::FP_TO_FP_FROM_SBV, {zeroBv}, {exp, sig});
            } else {
              result = solver.make_bv_value(pointeeSort, "0", 10);
            }

            for (const auto &l: currentFun_->lets) {
              if (!typeMatch(l.type, pointeeType))
                continue;
              auto tagTerm = solver.make_bv_value_int64(
                  solver.make_bv_sort(kPtrBits), static_cast<int64_t>(tagOfLocal(l.name.name))
              );
              auto cond = solver.make_term(smt::Kind::EQUAL, {ptrTerm, tagTerm});
              auto &sv = store.at(l.name.name);
              result = solver.make_term(smt::Kind::ITE, {cond, sv.term, result});
            }
            return result;
          }
          // Unreachable
          return solver.make_bv_value(solver.make_bv_sort(32), "0", 10);
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

  smt::Term SymbolicExecutor::evalSelectVal(
      const SelectVal &sv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
      std::optional<smt::Sort> expectedSort
  ) {
    if (auto rv = std::get_if<RValue>(&sv))
      return evalLValue(*rv, solver, store, pc).term;
    return evalCoef(std::get<Coef>(sv), solver, store, expectedSort);
  }

  smt::Term SymbolicExecutor::evalCond(
      const Cond &c, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc
  ) {
    smt::Term lhs = evalExpr(c.lhs, solver, store, pc);
    smt::Term rhs = evalExpr(c.rhs, solver, store, pc, solver.get_sort(lhs));

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

} // namespace symir
