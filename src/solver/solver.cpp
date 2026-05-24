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
      } else if (auto vt = std::get_if<VecType>(&t->v)) {
        SymbolicValue res;
        res.kind = SymbolicValue::Kind::Vec;
        for (size_t i = 0; i < elements.size(); ++i)
          res.arrayVal.push_back(evalInit(*elements[i], vt->elem, solver, store));
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
      const LValue &lv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc
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

  smt::Term SymbolicExecutor::evalExpr(
      const Expr &e, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
      std::optional<smt::Sort> expectedSort
  ) {
    smt::Term res = evalAtom(e.first, solver, store, pc, expectedSort);
    // The typechecker requires the +/- chain to be homogeneous in type. If the
    // caller didn't supply an expected sort, propagate the first atom's sort
    // to subsequent atoms so untyped literals (e.g. SelectVal `1` inside an
    // i16 chain) come out at the right width — otherwise the solver would
    // sign-extend to BV32 and miss the i16 overflow that the interpreter
    // detects.
    std::optional<smt::Sort> chainSort = expectedSort;
    if (!chainSort)
      chainSort = solver.get_sort(res);

    for (const auto &tail: e.rest) {
      smt::Term right = evalAtom(tail.atom, solver, store, pc, chainSort);
      auto lSort = solver.get_sort(res);
      auto rSort = solver.get_sort(right);

      if (solver.is_fp_sort(lSort)) {
        // SPEC §2.9: all FP ops use RNE (no dynamic rounding modes).
        auto rmRNE = solver.make_rm_value(smt::RoundingMode::RNE);
        if (tail.op == AddOp::Plus) {
          res = solver.make_term(smt::Kind::FP_ADD, {rmRNE, res, right});
        } else {
          res = solver.make_term(smt::Kind::FP_SUB, {rmRNE, res, right});
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

              if (arg.op == AtomOpKind::Shl) {
                // SPEC §7.1 rule 4: SHL result overflow is UB. There is no
                // built-in BV_SHL_OVERFLOW, so we construct it: overflow iff
                // arithmetic-right-shifting the result by n doesn't recover
                // the original c (i.e., bits shifted past the sign bit
                // disagreed with the sign).
                auto shifted = solver.make_term(smt::Kind::BV_SHL, {c, r});
                auto unshifted = solver.make_term(smt::Kind::BV_ASHR, {shifted, r});
                pc.push_back(solver.make_term(smt::Kind::EQUAL, {unshifted, c}));
                return shifted;
              }
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

            // FP cast handling. SMT-LIB FP theory requires a rounding-mode
            // operand. SPEC §6.4: fp->int is RTZ (truncation toward zero),
            // int->fp and fp->fp are RNE. Omitting the RM (as the previous
            // code did) made bitwuzla default to its own choice and silently
            // produced models where the solver disagreed with the interp on
            // the cast result — surfaced by seed 13579 / func38_2 where the
            // solver computed `v0 = -7.5f as i32 = -8` (RNE) while interp
            // truncated to -7 (RTZ), causing the saddo chain to admit an
            // overflowing model.
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
              auto rmRTZ = solver.make_rm_value(smt::RoundingMode::RTZ);
              return solver.make_term(smt::Kind::FP_TO_SBV, {rmRTZ, src}, {width});
            }
            if (!srcIsFp && dstIsFp) { // BV -> FP
              auto [exp, sig] = solver.get_fp_dims(dstSort);
              auto rmRNE = solver.make_rm_value(smt::RoundingMode::RNE);
              return solver.make_term(smt::Kind::FP_TO_FP_FROM_SBV, {rmRNE, src}, {exp, sig});
            }
            if (srcIsFp && dstIsFp) { // FP -> FP
              auto [exp, sig] = solver.get_fp_dims(dstSort);
              auto rmRNE = solver.make_rm_value(smt::RoundingMode::RNE);
              return solver.make_term(smt::Kind::FP_TO_FP_FROM_FP, {rmRNE, src}, {exp, sig});
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
            // `addr %arr[0]` is equivalent to `addr %arr` (same first-element
            // address). Other element indices and field paths require true
            // offset tracking and are still left to a future revision.
            for (const auto &acc: arg.lv.accesses) {
              if (auto ai = std::get_if<AccessIndex>(&acc)) {
                auto il = std::get_if<IntLit>(&ai->index);
                if (il && il->value == 0)
                  continue; // benign: same as base address
              }
              throw std::runtime_error(
                  "'addr' with non-zero field/index accesses not yet supported in solver"
              );
            }
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
          else
            opK = term.op == AddOp::Plus ? smt::Kind::BV_ADD : smt::Kind::BV_SUB;
          acc[k] = fpLane ? solver.make_term(opK, {rmRNE, acc[k], next[k]})
                          : solver.make_term(opK, {acc[k], next[k]});
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
                smt::Term bv = evalCoef(*cf, solver, store, sortFor());
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
              smt::Kind opKind = smt::Kind::EQUAL;
              switch (arg.op) {
                case RelOp::EQ:
                  opKind = smt::Kind::EQUAL;
                  break;
                case RelOp::NE:
                  opKind = smt::Kind::DISTINCT;
                  break;
                case RelOp::LT:
                  opKind = fpLane ? smt::Kind::FP_LT : smt::Kind::BV_SLT;
                  break;
                case RelOp::LE:
                  opKind = fpLane ? smt::Kind::FP_LEQ : smt::Kind::BV_SLE;
                  break;
                case RelOp::GT:
                  opKind = fpLane ? smt::Kind::FP_GT : smt::Kind::BV_SGT;
                  break;
                case RelOp::GE:
                  opKind = fpLane ? smt::Kind::FP_GEQ : smt::Kind::BV_SGE;
                  break;
              }
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
              lanes.emplace_back(SymbolicValue::Kind::Int, chosen, solver.make_true());
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
              } else {
                switch (arg.op) {
                  case AtomOpKind::Mul: {
                    auto ov = solver.make_term(smt::Kind::BV_SMUL_OVERFLOW, {c, r});
                    pc.push_back(solver.make_term(smt::Kind::NOT, {ov}));
                    out = solver.make_term(smt::Kind::BV_MUL, {c, r});
                    break;
                  }
                  case AtomOpKind::Div: {
                    auto zero = solver.make_bv_zero(solver.get_sort(r));
                    pc.push_back(solver.make_term(smt::Kind::DISTINCT, {r, zero}));
                    out = solver.make_term(smt::Kind::BV_SDIV, {c, r});
                    break;
                  }
                  case AtomOpKind::Mod: {
                    auto zero = solver.make_bv_zero(solver.get_sort(r));
                    pc.push_back(solver.make_term(smt::Kind::DISTINCT, {r, zero}));
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
                  case AtomOpKind::Shl:
                    out = solver.make_term(smt::Kind::BV_SHL, {c, r});
                    break;
                  case AtomOpKind::Shr:
                    out = solver.make_term(smt::Kind::BV_ASHR, {c, r});
                    break;
                  case AtomOpKind::LShr:
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
              } else if (srcIsFp && dstIsFp) {
                auto [exp, sig] = solver.get_fp_dims(dstSort);
                auto rmRNE = solver.make_rm_value(smt::RoundingMode::RNE);
                out = solver.make_term(smt::Kind::FP_TO_FP_FROM_FP, {rmRNE, lane}, {exp, sig});
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
