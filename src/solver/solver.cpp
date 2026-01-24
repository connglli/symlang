#include "solver/solver.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>
#include "analysis/cfg.hpp"

namespace symir {

  SymbolicExecutor::SymbolicExecutor(const Program &prog, const Config &config) :
      prog_(prog), config_(config) {
    for (const auto &s: prog_.structs) {
      structs_[s.name.name] = &s;
    }
  }

  bitwuzla::Sort SymbolicExecutor::getSort(const TypePtr &t, bitwuzla::TermManager &tm) {
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
      return tm.mk_bv_sort(bits);
    }
    throw std::runtime_error("Aggregate types do not have a single SMT sort in this encoding");
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::createSymbolicValue(
      const TypePtr &t, const std::string &name, bitwuzla::TermManager &tm, bool
  ) {
    SymbolicValue res;
    if (auto at = std::get_if<ArrayType>(&t->v)) {
      res.kind = SymbolicValue::Kind::Array;
      for (size_t i = 0; i < at->size; ++i) {
        res.arrayVal.push_back(
            createSymbolicValue(at->elem, name + "[" + std::to_string(i) + "]", tm)
        );
      }
    } else if (auto st = std::get_if<StructType>(&t->v)) {
      res.kind = SymbolicValue::Kind::Struct;
      auto it = structs_.find(st->name.name);
      if (it != structs_.end()) {
        for (const auto &f: it->second->fields) {
          res.structVal[f.name] = createSymbolicValue(f.type, name + "." + f.name, tm);
        }
      }
    } else {
      res.kind = SymbolicValue::Kind::Int;
      res.term = tm.mk_const(getSort(t, tm), name);
      res.is_defined = tm.mk_true();
    }
    return res;
  }

  SymbolicExecutor::SymbolicValue
  SymbolicExecutor::makeUndef(const TypePtr &t, bitwuzla::TermManager &tm) {
    SymbolicValue res;
    if (auto at = std::get_if<ArrayType>(&t->v)) {
      res.kind = SymbolicValue::Kind::Array;
      for (size_t i = 0; i < at->size; ++i)
        res.arrayVal.push_back(makeUndef(at->elem, tm));
    } else if (auto st = std::get_if<StructType>(&t->v)) {
      res.kind = SymbolicValue::Kind::Struct;
      auto it = structs_.find(st->name.name);
      if (it != structs_.end()) {
        for (const auto &f: it->second->fields)
          res.structVal[f.name] = makeUndef(f.type, tm);
      }
    } else {
      res.kind = SymbolicValue::Kind::Int;
      res.term = tm.mk_const(getSort(t, tm), "undef");
      res.is_defined = tm.mk_false();
    }
    return res;
  }

  SymbolicExecutor::SymbolicValue
  SymbolicExecutor::broadcast(const TypePtr &t, bitwuzla::Term val, bitwuzla::TermManager &tm) {
    if (std::holds_alternative<ArrayType>(t->v)) {
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Array;
      const auto &at = std::get<ArrayType>(t->v);
      for (size_t i = 0; i < at.size; ++i)
        res.arrayVal.push_back(broadcast(at.elem, val, tm));
      return res;
    } else if (std::holds_alternative<StructType>(t->v)) {
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Struct;
      auto it = structs_.find(std::get<StructType>(t->v).name.name);
      if (it != structs_.end()) {
        for (const auto &f: it->second->fields)
          res.structVal[f.name] = broadcast(f.type, val, tm);
      }
      return res;
    } else {
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Int;
      res.term = val;
      res.is_defined = tm.mk_true();
      return res;
    }
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::evalInit(
      const InitVal &iv, const TypePtr &t, bitwuzla::TermManager &tm, SymbolicStore &store
  ) {
    if (iv.kind == InitVal::Kind::Undef)
      return makeUndef(t, tm);

    if (iv.kind == InitVal::Kind::Aggregate) {
      const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
      if (auto at = std::get_if<ArrayType>(&t->v)) {
        SymbolicValue res;
        res.kind = SymbolicValue::Kind::Array;
        for (size_t i = 0; i < elements.size(); ++i)
          res.arrayVal.push_back(evalInit(*elements[i], at->elem, tm, store));
        return res;
      } else if (auto st = std::get_if<StructType>(&t->v)) {
        SymbolicValue res;
        res.kind = SymbolicValue::Kind::Struct;
        auto it = structs_.find(st->name.name);
        if (it != structs_.end()) {
          for (size_t i = 0; i < elements.size(); ++i)
            res.structVal[it->second->fields[i].name] =
                evalInit(*elements[i], it->second->fields[i].type, tm, store);
        }
        return res;
      }
    }

    // Scalar
    bitwuzla::Term val;
    if (iv.kind == InitVal::Kind::Int) {
      auto lit = std::get<IntLit>(iv.value);
      val = tm.mk_bv_value(getSort(t, tm), std::to_string(lit.value), 10);
    } else if (iv.kind == InitVal::Kind::Sym) {
      val = store.at(std::get<SymId>(iv.value).name).term;
    } else if (iv.kind == InitVal::Kind::Local) {
      val = store.at(std::get<LocalId>(iv.value).name).term;
    }
    return broadcast(t, val, tm);
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

    bitwuzla::TermManager tm;
    bitwuzla::Options options;
    options.set(bitwuzla::Option::PRODUCE_MODELS, true);
    if (config_.timeout_ms > 0)
      options.set(bitwuzla::Option::TIME_LIMIT_PER, (uint64_t) config_.timeout_ms);
    if (config_.seed > 0)
      options.set(bitwuzla::Option::SEED, (uint64_t) config_.seed);
    bitwuzla::Bitwuzla solver(tm, options);

    SymbolicStore store;
    std::vector<bitwuzla::Term> pathConstraints;
    std::vector<bitwuzla::Term> requirements;

    // 1. Declare symbols and fix values if requested
    for (const auto &s: entry->syms) {
      auto sv = createSymbolicValue(s.type, s.name.name, tm, true);
      store[s.name.name] = sv;

      // Add domain constraints
      if (s.domain) {
        std::visit(
            [&](auto &&d) {
              using T = std::decay_t<decltype(d)>;
              if constexpr (std::is_same_v<T, DomainInterval>) {
                auto lo = tm.mk_bv_value(getSort(s.type, tm), std::to_string(d.lo), 10);
                auto hi = tm.mk_bv_value(getSort(s.type, tm), std::to_string(d.hi), 10);
                pathConstraints.push_back(tm.mk_term(bitwuzla::Kind::BV_SLE, {lo, sv.term}));
                pathConstraints.push_back(tm.mk_term(bitwuzla::Kind::BV_SLE, {sv.term, hi}));
              } else if constexpr (std::is_same_v<T, DomainSet>) {
                std::vector<bitwuzla::Term> or_terms;
                for (auto v: d.values) {
                  auto vt = tm.mk_bv_value(getSort(s.type, tm), std::to_string(v), 10);
                  or_terms.push_back(tm.mk_term(bitwuzla::Kind::EQUAL, {sv.term, vt}));
                }
                if (!or_terms.empty()) {
                  bitwuzla::Term or_all = or_terms[0];
                  for (size_t i = 1; i < or_terms.size(); ++i)
                    or_all = tm.mk_term(bitwuzla::Kind::OR, {or_all, or_terms[i]});
                  pathConstraints.push_back(or_all);
                }
              }
            },
            *s.domain
        );
      }

      if (fixedSyms.count(s.name.name)) {
        auto val =
            tm.mk_bv_value(getSort(s.type, tm), std::to_string(fixedSyms.at(s.name.name)), 10);
        pathConstraints.push_back(tm.mk_term(bitwuzla::Kind::EQUAL, {sv.term, val}));
      }
    }

    // 2. Declare locals (parameters are also in store)
    for (const auto &p: entry->params) {
      store[p.name.name] = createSymbolicValue(p.type, p.name.name, tm);
    }
    for (const auto &l: entry->lets) {
      if (l.init) {
        store[l.name.name] = evalInit(*l.init, l.type, tm, store);
      } else {
        store[l.name.name] = makeUndef(l.type, tm);
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
                auto lhsVal = evalLValue(arg.lhs, tm, solver, store, pathConstraints);
                auto rhs = evalExpr(
                    arg.rhs, tm, solver, store, pathConstraints,
                    lhsVal.kind == SymbolicValue::Kind::Int ? std::optional(lhsVal.term.sort())
                                                            : std::nullopt
                );
                SymbolicValue val(SymbolicValue::Kind::Int, rhs, tm.mk_true());
                setLValue(arg.lhs, val, tm, solver, store, pathConstraints);
              } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                pathConstraints.push_back(evalCond(arg.cond, tm, solver, store, pathConstraints));
              } else if constexpr (std::is_same_v<T, RequireInstr>) {
                requirements.push_back(evalCond(arg.cond, tm, solver, store, pathConstraints));
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
                  auto cond = evalCond(*term.cond, tm, solver, store, pathConstraints);
                  if (term.thenLabel.name == nextLabel) {
                    pathConstraints.push_back(cond);
                  } else if (term.elseLabel.name == nextLabel) {
                    pathConstraints.push_back(tm.mk_term(bitwuzla::Kind::NOT, {cond}));
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

    bitwuzla::Result res = solver.check_sat();
    Result finalRes;
    if (res == bitwuzla::Result::SAT) {
      finalRes.sat = true;
      for (const auto &s: entry->syms) {
        auto term = store.at(s.name.name).term;
        auto val_term = solver.get_value(term);
        auto val_str = val_term.value<std::string>(10);
        finalRes.model[s.name.name] = parseIntegerLiteral(val_str);
      }
    } else if (res == bitwuzla::Result::UNSAT) {
      finalRes.unsat = true;
    } else {
      finalRes.unknown = true;
    }
    return finalRes;
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::mergeAggregate(
      const std::vector<SymbolicValue> &elements, bitwuzla::Term idx, bitwuzla::TermManager &tm
  ) {
    if (elements.empty())
      return {SymbolicValue::Kind::Undef};

    if (elements[0].kind == SymbolicValue::Kind::Int) {
      bitwuzla::Term res = elements[0].term;
      bitwuzla::Term defined = elements[0].is_defined;
      for (size_t i = 1; i < elements.size(); ++i) {
        auto i_term = tm.mk_bv_value(idx.sort(), std::to_string(i), 10);
        auto cond = tm.mk_term(bitwuzla::Kind::EQUAL, {idx, i_term});
        res = tm.mk_term(bitwuzla::Kind::ITE, {cond, elements[i].term, res});
        defined = tm.mk_term(bitwuzla::Kind::ITE, {cond, elements[i].is_defined, defined});
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
        res.arrayVal.push_back(mergeAggregate(inner_elements, idx, tm));
      }
      return res;
    } else if (elements[0].kind == SymbolicValue::Kind::Struct) {
      SymbolicValue res;
      res.kind = SymbolicValue::Kind::Struct;
      for (const auto &[fld, _]: elements[0].structVal) {
        std::vector<SymbolicValue> inner_elements;
        for (size_t i = 0; i < elements.size(); ++i)
          inner_elements.push_back(elements[i].structVal.at(fld));
        res.structVal[fld] = mergeAggregate(inner_elements, idx, tm);
      }
      return res;
    }
    return {SymbolicValue::Kind::Undef};
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::evalLValue(
      const LValue &lv, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &, SymbolicStore &store,
      std::vector<bitwuzla::Term> &pc
  ) {
    SymbolicValue res = store.at(lv.base.name);
    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        if (res.kind != SymbolicValue::Kind::Array)
          throw std::runtime_error("Indexing non-array");
        size_t array_size = res.arrayVal.size();
        bitwuzla::Term idx;
        if (auto lit = std::get_if<IntLit>(&ai->index)) {
          idx = tm.mk_bv_value(tm.mk_bv_sort(32), std::to_string(lit->value), 10);
          SymbolicValue next = res.arrayVal.at(lit->value);
          res = std::move(next);
        } else {
          auto id = std::get<LocalOrSymId>(ai->index);
          idx = std::visit([&](auto &&v) { return store.at(v.name).term; }, id);
          res = mergeAggregate(res.arrayVal, idx, tm);
        }
        // Strict UB: bounds check
        auto size_term = tm.mk_bv_value(idx.sort(), std::to_string(array_size), 10);
        auto zero = tm.mk_bv_zero(idx.sort());
        pc.push_back(tm.mk_term(bitwuzla::Kind::BV_SLE, {zero, idx}));
        pc.push_back(tm.mk_term(bitwuzla::Kind::BV_SLT, {idx, size_term}));
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
      bitwuzla::Term cond, const SymbolicValue &t, const SymbolicValue &f, bitwuzla::TermManager &tm
  ) {
    if (t.kind != f.kind) {
      throw std::runtime_error("Muxing different kinds of SymbolicValues");
    }

    SymbolicValue res;
    res.kind = t.kind;

    if (t.kind == SymbolicValue::Kind::Int) {
      res.term = tm.mk_term(bitwuzla::Kind::ITE, {cond, t.term, f.term});
      res.is_defined = tm.mk_term(bitwuzla::Kind::ITE, {cond, t.is_defined, f.is_defined});
    } else if (t.kind == SymbolicValue::Kind::Array) {
      if (t.arrayVal.size() != f.arrayVal.size())
        throw std::runtime_error("Muxing arrays of different sizes");
      for (size_t i = 0; i < t.arrayVal.size(); ++i) {
        res.arrayVal.push_back(muxSymbolicValue(cond, t.arrayVal[i], f.arrayVal[i], tm));
      }
    } else if (t.kind == SymbolicValue::Kind::Struct) {
      for (const auto &[key, val]: t.structVal) {
        if (f.structVal.find(key) == f.structVal.end())
          throw std::runtime_error("Muxing structs with mismatching keys: " + key);
        res.structVal[key] = muxSymbolicValue(cond, val, f.structVal.at(key), tm);
      }
    }
    return res;
  }

  SymbolicExecutor::SymbolicValue SymbolicExecutor::updateLValueRec(
      const SymbolicValue &cur, std::span<const Access> accesses, const SymbolicValue &val,
      bitwuzla::Term pathCond, bitwuzla::TermManager &tm, SymbolicStore &store,
      std::vector<bitwuzla::Term> &pc, int depth
  ) {
    if (depth > 100)
      throw std::runtime_error("Recursion depth exceeded in updateLValueRec");

    if (accesses.empty()) {
      return muxSymbolicValue(pathCond, val, cur, tm);
    }

    const Access &acc = accesses[0];
    auto nextAccesses = accesses.subspan(1);
    SymbolicValue newCur = cur; // Copy

    if (auto ai = std::get_if<AccessIndex>(&acc)) {
      if (cur.kind != SymbolicValue::Kind::Array)
        throw std::runtime_error("Indexing non-array in setLValue");

      bitwuzla::Term idx;
      if (auto lit = std::get_if<IntLit>(&ai->index)) {
        idx = tm.mk_bv_value(tm.mk_bv_sort(32), std::to_string(lit->value), 10);
      } else {
        auto id = std::get<LocalOrSymId>(ai->index);
        idx = std::visit([&](auto &&v) { return store.at(v.name).term; }, id);
      }

      // Bounds check UB
      size_t size = cur.arrayVal.size();
      if (size == 0)
        throw std::runtime_error("Indexing empty array");

      auto size_term = tm.mk_bv_value(idx.sort(), std::to_string(size), 10);
      auto zero = tm.mk_bv_zero(idx.sort());

      pc.push_back(tm.mk_term(
          bitwuzla::Kind::IMPLIES, {pathCond, tm.mk_term(bitwuzla::Kind::BV_SLE, {zero, idx})}
      ));
      pc.push_back(tm.mk_term(
          bitwuzla::Kind::IMPLIES, {pathCond, tm.mk_term(bitwuzla::Kind::BV_SLT, {idx, size_term})}
      ));

      if (auto lit = std::get_if<IntLit>(&ai->index)) {
        // Constant index
        uint64_t k = lit->value;
        if (k < newCur.arrayVal.size()) {
          newCur.arrayVal[k] = updateLValueRec(
              cur.arrayVal[k], nextAccesses, val, pathCond, tm, store, pc, depth + 1
          );
        }
      } else {
        // Symbolic index
        for (size_t k = 0; k < newCur.arrayVal.size(); ++k) {
          auto k_term = tm.mk_bv_value(idx.sort(), std::to_string(k), 10);
          auto match = tm.mk_term(bitwuzla::Kind::EQUAL, {idx, k_term});
          auto cond = tm.mk_term(bitwuzla::Kind::AND, {pathCond, match});
          newCur.arrayVal[k] =
              updateLValueRec(cur.arrayVal[k], nextAccesses, val, cond, tm, store, pc, depth + 1);
        }
      }
    } else {
      auto &af = std::get<AccessField>(acc);
      if (cur.kind != SymbolicValue::Kind::Struct)
        throw std::runtime_error("Field access on non-struct in setLValue");
      if (cur.structVal.find(af.field) == cur.structVal.end())
        throw std::runtime_error("Field not found: " + af.field);

      newCur.structVal[af.field] = updateLValueRec(
          cur.structVal.at(af.field), nextAccesses, val, pathCond, tm, store, pc, depth + 1
      );
    }
    return newCur;
  }

  void SymbolicExecutor::setLValue(
      const LValue &lv, const SymbolicValue &val, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &,
      SymbolicStore &store, std::vector<bitwuzla::Term> &pc
  ) {
    SymbolicValue &root = store.at(lv.base.name);
    // Use true condition because the instruction itself is unconditional *at this point in the
    // trace* The path constraints handle the reachability of the instruction.
    root = updateLValueRec(root, lv.accesses, val, tm.mk_true(), tm, store, pc, 0);
  }

  bitwuzla::Term SymbolicExecutor::evalExpr(
      const Expr &e, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store,
      std::vector<bitwuzla::Term> &pc, std::optional<bitwuzla::Sort> expectedSort
  ) {
    bitwuzla::Term res = evalAtom(e.first, tm, solver, store, pc, expectedSort);
    for (const auto &tail: e.rest) {
      bitwuzla::Term right = evalAtom(tail.atom, tm, solver, store, pc, expectedSort);
      if (tail.op == AddOp::Plus) {
        auto overflow = tm.mk_term(bitwuzla::Kind::BV_SADD_OVERFLOW, {res, right});
        pc.push_back(tm.mk_term(bitwuzla::Kind::NOT, {overflow}));
        res = tm.mk_term(bitwuzla::Kind::BV_ADD, {res, right});
      } else {
        auto overflow = tm.mk_term(bitwuzla::Kind::BV_SSUB_OVERFLOW, {res, right});
        pc.push_back(tm.mk_term(bitwuzla::Kind::NOT, {overflow}));
        res = tm.mk_term(bitwuzla::Kind::BV_SUB, {res, right});
      }
    }
    return res;
  }

  bitwuzla::Term SymbolicExecutor::evalAtom(
      const Atom &a, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store,
      std::vector<bitwuzla::Term> &pc, std::optional<bitwuzla::Sort> expectedSort
  ) {
    return std::visit(
        [&](auto &&arg) -> bitwuzla::Term {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            bitwuzla::Term c = evalCoef(arg.coef, tm, solver, store, expectedSort);
            bitwuzla::Term r = evalLValue(arg.rval, tm, solver, store, pc).term;
            if (arg.op == AtomOpKind::Div || arg.op == AtomOpKind::Mod) {
              auto zero = tm.mk_bv_zero(r.sort());
              pc.push_back(tm.mk_term(bitwuzla::Kind::DISTINCT, {r, zero}));
            }
            if (arg.op == AtomOpKind::Mul) {
              auto overflow = tm.mk_term(bitwuzla::Kind::BV_SMUL_OVERFLOW, {c, r});
              pc.push_back(tm.mk_term(bitwuzla::Kind::NOT, {overflow}));
              return tm.mk_term(bitwuzla::Kind::BV_MUL, {c, r});
            }
            if (arg.op == AtomOpKind::Div) {
              // Check overflow: c == INT_MIN && r == -1
              auto min_signed = tm.mk_bv_min_signed(c.sort());
              auto minus_one = tm.mk_bv_value_int64(r.sort(), -1);
              auto is_min = tm.mk_term(bitwuzla::Kind::EQUAL, {c, min_signed});
              auto is_minus_one = tm.mk_term(bitwuzla::Kind::EQUAL, {r, minus_one});
              auto div_overflow = tm.mk_term(bitwuzla::Kind::AND, {is_min, is_minus_one});
              pc.push_back(tm.mk_term(bitwuzla::Kind::NOT, {div_overflow}));
              return tm.mk_term(bitwuzla::Kind::BV_SDIV, {c, r});
            }
            if (arg.op == AtomOpKind::Mod) {
              // Check overflow for mod? Yes, if div overflows, mod is problematic/UB in C.
              auto min_signed = tm.mk_bv_min_signed(c.sort());
              auto minus_one = tm.mk_bv_value_int64(r.sort(), -1);
              auto is_min = tm.mk_term(bitwuzla::Kind::EQUAL, {c, min_signed});
              auto is_minus_one = tm.mk_term(bitwuzla::Kind::EQUAL, {r, minus_one});
              auto mod_overflow = tm.mk_term(bitwuzla::Kind::AND, {is_min, is_minus_one});
              pc.push_back(tm.mk_term(bitwuzla::Kind::NOT, {mod_overflow}));
              return tm.mk_term(bitwuzla::Kind::BV_SREM, {c, r});
            }
            if (arg.op == AtomOpKind::And) {
              return tm.mk_term(bitwuzla::Kind::BV_AND, {c, r});
            }
            if (arg.op == AtomOpKind::Or) {
              return tm.mk_term(bitwuzla::Kind::BV_OR, {c, r});
            }
            if (arg.op == AtomOpKind::Xor) {
              return tm.mk_term(bitwuzla::Kind::BV_XOR, {c, r});
            }
            if (arg.op == AtomOpKind::Shl || arg.op == AtomOpKind::Shr ||
                arg.op == AtomOpKind::LShr) {
              // Overshift UB: amount < width
              uint32_t width = c.sort().bv_size();
              auto width_term = tm.mk_bv_value(r.sort(), std::to_string(width), 10);
              pc.push_back(tm.mk_term(bitwuzla::Kind::BV_ULT, {r, width_term}));

              if (arg.op == AtomOpKind::Shl)
                return tm.mk_term(bitwuzla::Kind::BV_SHL, {c, r});
              if (arg.op == AtomOpKind::Shr)
                return tm.mk_term(bitwuzla::Kind::BV_ASHR, {c, r});
              if (arg.op == AtomOpKind::LShr)
                return tm.mk_term(bitwuzla::Kind::BV_SHR, {c, r});
            }
            return {};
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            auto r = evalLValue(arg.rval, tm, solver, store, pc).term;
            if (arg.op == UnaryOpKind::Not) {
              return tm.mk_term(bitwuzla::Kind::BV_NOT, {r});
            }
            return {};
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            bitwuzla::Term cond = evalCond(*arg.cond, tm, solver, store, pc);
            bitwuzla::Term vt = evalSelectVal(arg.vtrue, tm, solver, store, pc, expectedSort);
            bitwuzla::Term vf = evalSelectVal(arg.vfalse, tm, solver, store, pc, expectedSort);
            return tm.mk_term(bitwuzla::Kind::ITE, {cond, vt, vf});
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            return evalCoef(arg.coef, tm, solver, store, expectedSort);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            return evalLValue(arg.rval, tm, solver, store, pc).term;
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            bitwuzla::Term src = std::visit(
                [&](auto &&s) -> bitwuzla::Term {
                  using S = std::decay_t<decltype(s)>;
                  if constexpr (std::is_same_v<S, IntLit>) {
                    return tm.mk_bv_value(tm.mk_bv_sort(32), std::to_string(s.value), 10);
                  } else if constexpr (std::is_same_v<S, SymId>) {
                    return store.at(s.name).term;
                  } else {
                    return evalLValue(s, tm, solver, store, pc).term;
                  }
                },
                arg.src
            );
            auto dstSort = getSort(arg.dstType, tm);
            uint32_t srcWidth = src.sort().bv_size();
            uint32_t dstWidth = dstSort.bv_size();
            if (srcWidth == dstWidth)
              return src;
            if (srcWidth < dstWidth) {
              return tm.mk_term(bitwuzla::Kind::BV_SIGN_EXTEND, {src}, {dstWidth - srcWidth});
            } else {
              return tm.mk_term(bitwuzla::Kind::BV_EXTRACT, {src}, {dstWidth - 1, 0});
            }
          }
        },
        a.v
    );
  }

  bitwuzla::Term SymbolicExecutor::evalCoef(
      const Coef &c, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &, SymbolicStore &store,
      std::optional<bitwuzla::Sort> expectedSort
  ) {
    if (auto lit = std::get_if<IntLit>(&c)) {
      bitwuzla::Sort s = expectedSort.value_or(tm.mk_bv_sort(32));
      return tm.mk_bv_value(s, std::to_string(lit->value), 10);
    }
    auto id = std::get<LocalOrSymId>(c);
    return std::visit([&](auto &&v) { return store.at(v.name).term; }, id);
  }

  bitwuzla::Term SymbolicExecutor::evalSelectVal(
      const SelectVal &sv, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver,
      SymbolicStore &store, std::vector<bitwuzla::Term> &pc,
      std::optional<bitwuzla::Sort> expectedSort
  ) {
    if (auto rv = std::get_if<RValue>(&sv))
      return evalLValue(*rv, tm, solver, store, pc).term;
    return evalCoef(std::get<Coef>(sv), tm, solver, store, expectedSort);
  }

  bitwuzla::Term SymbolicExecutor::evalCond(
      const Cond &c, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store,
      std::vector<bitwuzla::Term> &pc
  ) {
    bitwuzla::Term lhs = evalExpr(c.lhs, tm, solver, store, pc);
    bitwuzla::Term rhs = evalExpr(c.rhs, tm, solver, store, pc, lhs.sort());
    switch (c.op) {
      case RelOp::EQ:
        return tm.mk_term(bitwuzla::Kind::EQUAL, {lhs, rhs});
      case RelOp::NE:
        return tm.mk_term(bitwuzla::Kind::DISTINCT, {lhs, rhs});
      case RelOp::LT:
        return tm.mk_term(bitwuzla::Kind::BV_SLT, {lhs, rhs});
      case RelOp::LE:
        return tm.mk_term(bitwuzla::Kind::BV_SLE, {lhs, rhs});
      case RelOp::GT:
        return tm.mk_term(bitwuzla::Kind::BV_SGT, {lhs, rhs});
      case RelOp::GE:
        return tm.mk_term(bitwuzla::Kind::BV_SGE, {lhs, rhs});
    }
    return {};
  }

} // namespace symir
