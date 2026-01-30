#include "solver/solver.hpp"
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include "analysis/cfg.hpp"

namespace symir {

  SymbolicExecutor::SymbolicExecutor(
      const Program &prog, const Config &config, SolverFactory solverFactory
  ) :
      prog_(prog),
      config_(config), solverFactory_(solverFactory) {
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
    throw std::runtime_error("Aggregate types do not have a single SMT sort in this encoding");
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
    if (iv.kind == InitVal::Kind::Int) {
      auto lit = std::get<IntLit>(iv.value);
      val = solver.make_bv_value(getSort(t, solver), std::to_string(lit.value), 10);
    } else if (iv.kind == InitVal::Kind::Float) {
      auto lit = std::get<FloatLit>(iv.value);
      // Using standard RNE
      val = solver.make_fp_value(
          getSort(t, solver), std::to_string(lit.value), smt::RoundingMode::RNE
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
                auto lo = solver.make_bv_value(getSort(s.type, solver), std::to_string(d.lo), 10);
                auto hi = solver.make_bv_value(getSort(s.type, solver), std::to_string(d.hi), 10);
                pathConstraints.push_back(solver.make_term(smt::Kind::BV_SLE, {lo, sv.term}));
                pathConstraints.push_back(solver.make_term(smt::Kind::BV_SLE, {sv.term, hi}));
              } else if constexpr (std::is_same_v<T, DomainSet>) {
                std::vector<smt::Term> or_terms;
                for (auto v: d.values) {
                  auto vt = solver.make_bv_value(getSort(s.type, solver), std::to_string(v), 10);
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
        auto val = solver.make_bv_value(
            getSort(s.type, solver), std::to_string(fixedSyms.at(s.name.name)), 10
        );
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
          finalRes.model[s.name.name] = parseIntegerLiteral(val_str);
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
      for (size_t i = 1; i < elements.size(); ++i) {
        auto i_term = solver.make_bv_value(solver.get_sort(idx), std::to_string(i), 10);
        auto cond = solver.make_term(smt::Kind::EQUAL, {idx, i_term});
        res = solver.make_term(smt::Kind::ITE, {cond, elements[i].term, res});
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
          res = mergeAggregate(res.arrayVal, idx, solver);
        }
        // Strict UB: bounds check
        auto size_term = solver.make_bv_value(solver.get_sort(idx), std::to_string(array_size), 10);
        auto zero = solver.make_bv_zero(solver.get_sort(idx));
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
      res.term = solver.make_term(smt::Kind::ITE, {cond, t.term, f.term});
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
      }

      // Bounds check UB
      size_t size = cur.arrayVal.size();
      if (size == 0)
        throw std::runtime_error("Indexing empty array");

      auto size_term = solver.make_bv_value(solver.get_sort(idx), std::to_string(size), 10);
      auto zero = solver.make_bv_zero(solver.get_sort(idx));

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
        for (size_t k = 0; k < newCur.arrayVal.size(); ++k) {
          auto k_term = solver.make_bv_value(solver.get_sort(idx), std::to_string(k), 10);
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

  smt::Term SymbolicExecutor::evalExpr(
      const Expr &e, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
      std::optional<smt::Sort> expectedSort
  ) {
    smt::Term res = evalAtom(e.first, solver, store, pc, expectedSort);
    for (const auto &tail: e.rest) {
      smt::Term right = evalAtom(tail.atom, solver, store, pc, expectedSort);
      if (tail.op == AddOp::Plus) {
        if (solver.is_fp_sort(solver.get_sort(res))) {
          res = solver.make_term(smt::Kind::FP_ADD, {res, right});
        } else {
          auto overflow = solver.make_term(smt::Kind::BV_SADD_OVERFLOW, {res, right});
          pc.push_back(solver.make_term(smt::Kind::NOT, {overflow}));
          res = solver.make_term(smt::Kind::BV_ADD, {res, right});
        }
      } else {
        if (solver.is_fp_sort(solver.get_sort(res))) {
          res = solver.make_term(smt::Kind::FP_SUB, {res, right});
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

            if (solver.is_fp_sort(solver.get_sort(c))) {
              if (arg.op == AtomOpKind::Mul)
                return solver.make_term(smt::Kind::FP_MUL, {c, r});
              if (arg.op == AtomOpKind::Div)
                return solver.make_term(smt::Kind::FP_DIV, {c, r});
              if (arg.op == AtomOpKind::Mod)
                return solver.make_term(smt::Kind::FP_REM, {c, r});
              return {};
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
            if (srcIsFp && !dstIsFp) { // FP -> BV
              uint32_t width = solver.get_bv_width(dstSort);
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

    if (solver.is_fp_sort(solver.get_sort(lhs))) {
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

} // namespace symir
