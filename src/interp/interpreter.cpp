#include "interp/interpreter.hpp"
#include <iostream>
#include <stdexcept>
#include "analysis/cfg.hpp"
#include "frontend/diagnostics.hpp"

namespace symir {

  Interpreter::Interpreter(const Program &prog) : prog_(prog) {
    for (const auto &s: prog_.structs) {
      structs_[s.name.name] = &s;
    }
  }

  void Interpreter::run(
      const std::string &entryFuncName,
      const std::unordered_map<std::string, std::int64_t> &symBindings, bool dumpExec
  ) {
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
      case RuntimeValue::Kind::Undef:
        return "undef";
      case RuntimeValue::Kind::Array:
        return "[...]";
      case RuntimeValue::Kind::Struct:
        return "{...}";
    }
    return "?";
  }

  Interpreter::RuntimeValue Interpreter::makeUndef(const TypePtr &t) {
    RuntimeValue res;
    if (auto at = std::get_if<ArrayType>(&t->v)) {
      res.kind = RuntimeValue::Kind::Array;
      for (size_t i = 0; i < at->size; ++i)
        res.arrayVal.push_back(makeUndef(at->elem));
    } else if (auto st = std::get_if<StructType>(&t->v)) {
      res.kind = RuntimeValue::Kind::Struct;
      auto it = structs_.find(st->name.name);
      if (it != structs_.end()) {
        for (const auto &f: it->second->fields)
          res.structVal[f.name] = makeUndef(f.type);
      }
    } else {
      res.kind = RuntimeValue::Kind::Undef;
    }
    return res;
  }

  Interpreter::RuntimeValue Interpreter::broadcast(const TypePtr &t, const RuntimeValue &v) {
    if (std::holds_alternative<ArrayType>(t->v)) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Array;
      const auto &at = std::get<ArrayType>(t->v);
      for (size_t i = 0; i < at.size; ++i)
        res.arrayVal.push_back(broadcast(at.elem, v));
      return res;
    } else if (std::holds_alternative<StructType>(t->v)) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Struct;
      auto it = structs_.find(std::get<StructType>(t->v).name.name);
      if (it != structs_.end()) {
        for (const auto &f: it->second->fields)
          res.structVal[f.name] = broadcast(f.type, v);
      }
      return res;
    } else {
      return v;
    }
  }

  Interpreter::RuntimeValue
  Interpreter::evalInit(const InitVal &iv, const TypePtr &t, const Store &store) {
    if (iv.kind == InitVal::Kind::Undef)
      return makeUndef(t);

    if (iv.kind == InitVal::Kind::Aggregate) {
      const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
      if (auto at = std::get_if<ArrayType>(&t->v)) {
        RuntimeValue res;
        res.kind = RuntimeValue::Kind::Array;
        for (size_t i = 0; i < elements.size(); ++i)
          res.arrayVal.push_back(evalInit(*elements[i], at->elem, store));
        return res;
      } else if (auto st = std::get_if<StructType>(&t->v)) {
        RuntimeValue res;
        res.kind = RuntimeValue::Kind::Struct;
        auto it = structs_.find(st->name.name);
        if (it != structs_.end()) {
          for (size_t i = 0; i < elements.size(); ++i)
            res.structVal[it->second->fields[i].name] =
                evalInit(*elements[i], it->second->fields[i].type, store);
        }
        return res;
      }
    }

    // Scalar
    RuntimeValue scalar;
    if (iv.kind == InitVal::Kind::Int) {
      scalar.kind = RuntimeValue::Kind::Int;
      scalar.intVal = std::get<IntLit>(iv.value).value;
    } else if (iv.kind == InitVal::Kind::Sym) {
      scalar = store.at(std::get<SymId>(iv.value).name);
    } else if (iv.kind == InitVal::Kind::Local) {
      scalar = store.at(std::get<LocalId>(iv.value).name);
    }
    return broadcast(t, scalar);
  }

  void Interpreter::execFunction(
      const FunDecl &f, const std::vector<RuntimeValue> &args,
      const std::unordered_map<std::string, std::int64_t> &symBindings
  ) {
    Store store;
    DiagBag diags;

    // Init params
    for (size_t i = 0; i < f.params.size(); ++i) {
      if (i < args.size())
        store[f.params[i].name.name] = args[i];
      else
        store[f.params[i].name.name] = RuntimeValue{RuntimeValue::Kind::Int, 0, {}, {}};
    }

    // Init Symbols
    for (const auto &s: f.syms) {
      auto it = symBindings.find(s.name.name);
      if (it == symBindings.end()) {
        throw std::runtime_error("Unbound symbol: " + s.name.name);
      }
      store[s.name.name] = RuntimeValue{RuntimeValue::Kind::Int, it->second, {}, {}};
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
                  throw std::runtime_error("Requirement failed: " + msg);
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
                  throw std::runtime_error("UB: Reading undef in ret");
                std::cout << "Result: " << res.intVal << "\n";
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
      if (v.kind == RuntimeValue::Kind::Undef || right.kind == RuntimeValue::Kind::Undef)
        throw std::runtime_error("UB: Reading undef in expr");
      if (v.kind != RuntimeValue::Kind::Int || right.kind != RuntimeValue::Kind::Int)
        throw std::runtime_error("Expr ops only on ints");

      if (tail.op == AddOp::Plus) {
        if (__builtin_add_overflow(v.intVal, right.intVal, &v.intVal))
          throw std::runtime_error("UB: Signed integer overflow in addition");
      } else {
        if (__builtin_sub_overflow(v.intVal, right.intVal, &v.intVal))
          throw std::runtime_error("UB: Signed integer overflow in subtraction");
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
            if (c.kind == RuntimeValue::Kind::Undef || r.kind == RuntimeValue::Kind::Undef)
              throw std::runtime_error("UB: Reading undef in op");
            if (c.kind != RuntimeValue::Kind::Int || r.kind != RuntimeValue::Kind::Int)
              throw std::runtime_error("OpAtom requires ints");

            RuntimeValue res;
            res.kind = RuntimeValue::Kind::Int;
            if (arg.op == AtomOpKind::Mul) {
              if (__builtin_mul_overflow(c.intVal, r.intVal, &res.intVal))
                throw std::runtime_error("UB: Signed integer overflow in multiplication");
            } else if (arg.op == AtomOpKind::Div) {
              if (r.intVal == 0)
                throw std::runtime_error("UB: Division by zero");
              if (c.intVal == INT64_MIN && r.intVal == -1)
                throw std::runtime_error("UB: Signed integer overflow in division");
              res.intVal = c.intVal / r.intVal;
            } else if (arg.op == AtomOpKind::Mod) {
              if (r.intVal == 0)
                throw std::runtime_error("UB: Modulo by zero");
              if (c.intVal == INT64_MIN && r.intVal == -1)
                throw std::runtime_error("UB: Signed integer overflow in modulo");
              res.intVal = c.intVal % r.intVal;
            }
            return res;
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            return evalCond(*arg.cond, store) ? evalSelectVal(arg.vtrue, store)
                                              : evalSelectVal(arg.vfalse, store);
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            return evalCoef(arg.coef, store);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            return evalLValue(arg.rval, store);
          }
          return RuntimeValue{};
        },
        a.v
    );
  }

  Interpreter::RuntimeValue Interpreter::evalCoef(const Coef &c, const Store &store) {
    if (std::holds_alternative<IntLit>(c)) {
      return RuntimeValue{RuntimeValue::Kind::Int, std::get<IntLit>(c).value, {}, {}};
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
          throw std::runtime_error("UB: Reading field of undef");
        if (cur->kind != RuntimeValue::Kind::Array)
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
          throw std::runtime_error("UB: Undef index");

        if (idxVal.intVal < 0 || (size_t) idxVal.intVal >= cur->arrayVal.size())
          throw std::runtime_error("UB: Array index out of bounds");

        cur = &cur->arrayVal[idxVal.intVal];
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (cur->kind == RuntimeValue::Kind::Undef)
          throw std::runtime_error("UB: Reading field of undef");
        if (cur->kind != RuntimeValue::Kind::Struct)
          throw std::runtime_error("Accessing field of non-struct");
        auto it = cur->structVal.find(af->field);
        if (it == cur->structVal.end())
          throw std::runtime_error("UB: Uninitialized field read");
        cur = &it->second;
      }
    }
    if (cur->kind == RuntimeValue::Kind::Undef)
      throw std::runtime_error("UB: Reading undef value");
    return *cur;
  }

  void Interpreter::setLValue(const LValue &lv, RuntimeValue val, Store &store) {
    RuntimeValue *cur = &store.at(lv.base.name);

    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        if (cur->kind != RuntimeValue::Kind::Array)
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
          throw std::runtime_error("UB: Array index out of bounds");
        cur = &cur->arrayVal[idxVal.intVal];
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (cur->kind != RuntimeValue::Kind::Struct)
          throw std::runtime_error("Accessing field of non-struct");
        cur = &cur->structVal[af->field];
      }
    }
    *cur = val;
  }

  bool Interpreter::evalCond(const Cond &c, const Store &store) {
    RuntimeValue l = evalExpr(c.lhs, store);
    RuntimeValue r = evalExpr(c.rhs, store);

    if (l.kind == RuntimeValue::Kind::Undef || r.kind == RuntimeValue::Kind::Undef)
      throw std::runtime_error("UB: Reading undef in cond");
    if (l.kind != RuntimeValue::Kind::Int || r.kind != RuntimeValue::Kind::Int)
      throw std::runtime_error("Cond operands must be int");

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
    return false;
  }

} // namespace symir
