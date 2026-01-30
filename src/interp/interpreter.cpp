#include "interp/interpreter.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include "analysis/cfg.hpp"
#include "analysis/type_utils.hpp"
#include "frontend/diagnostics.hpp"

namespace symir {

  Interpreter::Interpreter(const Program &prog) : prog_(prog) {
    for (const auto &s: prog_.structs) {
      structs_[s.name.name] = &s;
    }
  }

  void Interpreter::run(
      const std::string &entryFuncName, const SymBindings &symBindings, bool dumpExec
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
      case RuntimeValue::Kind::Float:
        return std::to_string(rv.floatVal);
      case RuntimeValue::Kind::Undef:
        return "undef";
      case RuntimeValue::Kind::Array:
        return "[...]";
      case RuntimeValue::Kind::Struct:
        return "{...}";
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
    if (auto at = TypeUtils::asArray(t)) {
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
      } else {
        res.kind = RuntimeValue::Kind::Undef;
        res.bits = 64;
      }
    }
    return res;
  }

  Interpreter::RuntimeValue Interpreter::broadcast(const TypePtr &t, const RuntimeValue &v) {
    if (auto at = TypeUtils::asArray(t)) {
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

    if (iv.kind == InitVal::Kind::Aggregate) {
      const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
      if (auto at = TypeUtils::asArray(t)) {
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
    }

    if (TypeUtils::asArray(t) || TypeUtils::asStruct(t)) {
      return broadcast(t, v);
    }
    return v;
  }

  void Interpreter::execFunction(
      const FunDecl &f, const std::vector<RuntimeValue> &args, const SymBindings &symBindings
  ) {
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
      if (it != symBindings.end()) {
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
                if (res.kind == RuntimeValue::Kind::Int)
                  std::cout << "Result: " << res.intVal << "\n";
                else
                  std::cout << "Result: " << res.floatVal << "\n";
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

      // Promote Int to Float if needed (Literal inference support)
      if (v.kind == RuntimeValue::Kind::Float && right.kind == RuntimeValue::Kind::Int) {
        right.floatVal = static_cast<double>(right.intVal);
        right.kind = RuntimeValue::Kind::Float;
      } else if (v.kind == RuntimeValue::Kind::Int && right.kind == RuntimeValue::Kind::Float) {
        v.floatVal = static_cast<double>(v.intVal);
        v.kind = RuntimeValue::Kind::Float;
      }

      if (v.kind == RuntimeValue::Kind::Int && right.kind == RuntimeValue::Kind::Int) {
        if (tail.op == AddOp::Plus) {
          if (__builtin_add_overflow(v.intVal, right.intVal, &v.intVal))
            throw std::runtime_error("UB: Signed integer overflow in addition");
        } else {
          if (__builtin_sub_overflow(v.intVal, right.intVal, &v.intVal))
            throw std::runtime_error("UB: Signed integer overflow in subtraction");
        }
        v.intVal = canonicalize(v.intVal, v.bits);
      } else if (v.kind == RuntimeValue::Kind::Float && right.kind == RuntimeValue::Kind::Float) {
        if (tail.op == AddOp::Plus)
          v.floatVal += right.floatVal;
        else
          v.floatVal -= right.floatVal;
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
            if (c.kind == RuntimeValue::Kind::Undef || r.kind == RuntimeValue::Kind::Undef)
              throw std::runtime_error("UB: Reading undef in op");

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
              } else if (arg.op == AtomOpKind::And) {
                res.intVal = c.intVal & r.intVal;
              } else if (arg.op == AtomOpKind::Or) {
                res.intVal = c.intVal | r.intVal;
              } else if (arg.op == AtomOpKind::Xor) {
                res.intVal = c.intVal ^ r.intVal;
              } else if (arg.op == AtomOpKind::Shl || arg.op == AtomOpKind::Shr || arg.op == AtomOpKind::LShr) {
                if (r.intVal < 0 || (uint64_t) r.intVal >= (uint64_t) res.bits) {
                  throw std::runtime_error("UB: Overshift");
                }
                if (arg.op == AtomOpKind::Shl) {
                  res.intVal = c.intVal << r.intVal;
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
              res.bits = c.bits;
              if (arg.op == AtomOpKind::Mul)
                res.floatVal = c.floatVal * r.floatVal;
              else if (arg.op == AtomOpKind::Div)
                res.floatVal = c.floatVal / r.floatVal;
              else if (arg.op == AtomOpKind::Mod)
                res.floatVal = std::fmod(c.floatVal, r.floatVal);
              else
                throw std::runtime_error("Unsupported op for floats");
              return res;
            }
            throw std::runtime_error("OpAtom requires same scalar kinds");
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            RuntimeValue r = evalLValue(arg.rval, store);
            if (r.kind == RuntimeValue::Kind::Undef)
              throw std::runtime_error("UB: Reading undef in unary op");
            if (r.kind != RuntimeValue::Kind::Int)
              throw std::runtime_error("Unary op requires int");
            RuntimeValue res;
            res.kind = RuntimeValue::Kind::Int;
            if (arg.op == UnaryOpKind::Not) {
              res.intVal = ~r.intVal;
            }
            return res;
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            return evalCond(*arg.cond, store) ? evalSelectVal(arg.vtrue, store)
                                              : evalSelectVal(arg.vfalse, store);
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            return evalCoef(arg.coef, store);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            return evalLValue(arg.rval, store);
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
              throw std::runtime_error("UB: Reading undef in cast");

            RuntimeValue res;
            auto dstBits = TypeUtils::getBitWidth(arg.dstType);
            if (dstBits) {
              res.kind = RuntimeValue::Kind::Int;
              res.bits = *dstBits;
              if (v.kind == RuntimeValue::Kind::Int)
                res.intVal = canonicalize(v.intVal, res.bits);
              else
                res.intVal = static_cast<int64_t>(v.floatVal); // Float to Int
            } else if (arg.dstType && std::holds_alternative<FloatType>(arg.dstType->v)) {
              res.kind = RuntimeValue::Kind::Float;
              res.bits =
                  (std::get<FloatType>(arg.dstType->v).kind == FloatType::Kind::F32) ? 32 : 64;
              if (v.kind == RuntimeValue::Kind::Int)
                res.floatVal = static_cast<double>(v.intVal); // Int to Float
              else
                res.floatVal = v.floatVal; // Float to Float (resize)
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

    // Enforce bitwidth of destination if it's an integer
    if (val.kind == RuntimeValue::Kind::Int) {
      val.bits = cur->bits;
      val.intVal = canonicalize(val.intVal, val.bits);
    }
    *cur = val;
  }

  bool Interpreter::evalCond(const Cond &c, const Store &store) {
    RuntimeValue l = evalExpr(c.lhs, store);
    RuntimeValue r = evalExpr(c.rhs, store);

    if (l.kind == RuntimeValue::Kind::Undef || r.kind == RuntimeValue::Kind::Undef)
      throw std::runtime_error("UB: Reading undef in condition");

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
    throw std::runtime_error("Cond operands must be same scalar kind");
  }

} // namespace symir
