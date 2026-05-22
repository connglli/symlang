#include "backend/c_backend.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace symir {

  // Format a double literal with enough precision to round-trip exactly,
  // i.e. `static_cast<double>(parseDouble(emitFloat(v))) == v` for every
  // finite v. The default `operator<<` uses 6 decimal digits which is far
  // short of double's ~17 needed for exactness — it would silently turn
  // `16777216.0` into `1.67772e+07` (= 16777200), changing the program's
  // observable behavior.
  static std::string formatFloatLit(double v) {
    std::ostringstream os;
    os << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    std::string s = os.str();
    // Ensure the literal parses as a floating-point constant in C — append
    // ".0" if no decimal/exponent character is present (so an integer-valued
    // double doesn't pick up integer type).
    if (s.find_first_of(".eEnN") == std::string::npos)
      s += ".0";
    return s;
  }

  static bool isOrContainsF64(const TypePtr &t) {
    if (!t)
      return false;
    return std::visit(
        [](auto &&arg) -> bool {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, FloatType>) {
            return arg.kind == FloatType::Kind::F64;
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            return isOrContainsF64(arg.elem);
          } else if constexpr (std::is_same_v<T, PtrType>) {
            return isOrContainsF64(arg.pointee);
          }
          return false;
        },
        t->v
    );
  }

  // RAII guard for ``isDoubleCtx_``: sets it to ``newValue`` on entry and
  // restores the prior value when it goes out of scope. Used at every
  // emission entry point that establishes a fresh evaluation context
  // (assign, store, return, cond, cast-from-float-lit, init).
  struct CtxGuard {
    bool &flag;
    bool saved;

    CtxGuard(bool &f, bool newValue) : flag(f), saved(f) { flag = newValue; }

    ~CtxGuard() { flag = saved; }

    CtxGuard(const CtxGuard &) = delete;
    CtxGuard &operator=(const CtxGuard &) = delete;
  };

  void CBackend::indent() {
    for (int i = 0; i < indent_level_; ++i)
      out_ << "  ";
  }

  std::string CBackend::mangleName(const std::string &name) {
    if (name.empty())
      return name;
    size_t start = 0;
    if (name[0] == '@' || name[0] == '%' || name[0] == '^') {
      start = 1;
      if (name.size() > 1 && name[1] == '?')
        start = 2;
    }
    // Prefix with symir_ to avoid C keywords and collisions for internal identifiers
    return "symir_" + name.substr(start);
  }

  std::string CBackend::stripSigil(const std::string &name) {
    if (name.empty())
      return name;
    size_t start = 0;
    if (name[0] == '@' || name[0] == '%' || name[0] == '^') {
      start = 1;
      if (name.size() > 1 && name[1] == '?')
        start = 2;
    }
    return name.substr(start);
  }

  std::string
  CBackend::getMangledSymbolName(const std::string &funcName, const std::string &symName) {
    // Follow docs/symirc.md format: <func>__<sym> with sigils removed
    return stripSigil(funcName) + "__" + stripSigil(symName);
  }

  void CBackend::emitType(const TypePtr &type) {
    if (!type) {
      out_ << "void";
      return;
    }
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntType>) {
            if (arg.kind == IntType::Kind::I32)
              out_ << "int32_t";
            else if (arg.kind == IntType::Kind::I64)
              out_ << "int64_t";
            else if (arg.kind == IntType::Kind::ICustom) {
              int b = arg.bits.value_or(32);
              if (b <= 8)
                out_ << "int8_t";
              else if (b <= 16)
                out_ << "int16_t";
              else if (b <= 32)
                out_ << "int32_t";
              else
                out_ << "int64_t";
            }
          } else if constexpr (std::is_same_v<T, FloatType>) {
            out_ << (arg.kind == FloatType::Kind::F32 ? "float" : "double");
          } else if constexpr (std::is_same_v<T, StructType>) {
            out_ << "struct " << mangleName(arg.name.name);
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            // Arrays are emitted as C arrays in context, but here we emit the base type
            emitType(arg.elem);
          } else if constexpr (std::is_same_v<T, PtrType>) {
            emitType(arg.pointee);
            out_ << " *";
          }
        },
        type->v
    );
  }

  void CBackend::emit(const Program &prog) {
    out_ << "#include <stdint.h>\n";
    out_ << "#include <stddef.h>\n";
    out_ << "#include <stdbool.h>\n";
    out_ << "#include <float.h>\n";
    out_ << "#include <math.h>\n";
    if (!noRequire_)
      out_ << "#include <assert.h>\n";
    out_ << "\n";

    // SPEC §2.9 conformance. C doesn't mandate IEEE 754 — the implementation
    // declares conformance by predefining __STDC_IEC_559__ (C99 §F.1). We
    // cannot define it ourselves; we MUST refuse to compile on a platform
    // that doesn't conform, because SymIR's FP semantics (RNE rounding,
    // finite-only domain, deterministic NaN/inf handling) all rest on
    // IEC 60559 / IEEE 754. Also disable FP contraction — without this,
    // GCC may fuse `a*b + c` into a single-rounding `fma`, which violates
    // §2.9 "no contraction" and would diverge from the interpreter and WASM.
    out_ << "#if !defined(__STDC_IEC_559__) || __STDC_IEC_559__ != 1\n";
    out_ << "# error \"SymIR-lowered C requires an IEC 60559 / IEEE 754 conforming \"\\\n";
    out_ << "          \"implementation (compiler must predefine __STDC_IEC_559__ to 1)\"\n";
    out_ << "#endif\n";
    out_ << "#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0\n";
    out_
        << "# error \"SymIR-lowered C requires an implementation with FLT_EVAL_METHOD == 0, \"\\\n";
    out_ << "          \"i.e., do not promote float into double or long double for evaluation\"\n";
    out_ << "#endif\n";
    out_ << "#pragma STDC FP_CONTRACT OFF\n";
    out_ << "\n";

    // 0. Populate struct fields map (name + type in declaration order).
    structFields_.clear();
    for (const auto &s: prog.structs) {
      std::vector<std::pair<std::string, TypePtr>> fields;
      fields.reserve(s.fields.size());
      for (const auto &f: s.fields)
        fields.emplace_back(f.name, f.type);
      structFields_[s.name.name] = std::move(fields);
    }

    // 1. Forward decls for structs
    for (const auto &s: prog.structs) {
      out_ << "struct " << mangleName(s.name.name) << ";\n";
    }
    out_ << "\n";

    // 2. Struct definitions
    for (const auto &s: prog.structs) {
      out_ << "struct " << mangleName(s.name.name) << " {\n";
      indent_level_++;
      for (const auto &f: s.fields) {
        indent();
        // Handle array fields
        TypePtr cur = f.type;
        std::vector<uint64_t> dims;
        while (auto at = std::get_if<ArrayType>(&cur->v)) {
          dims.push_back(at->size);
          cur = at->elem;
        }
        emitType(cur);
        out_ << " " << f.name;
        for (auto d: dims)
          out_ << "[" << d << "]";
        out_ << ";\n";
      }
      indent_level_--;
      out_ << "};\n\n";
    }

    auto getWidth = [](const TypePtr &t) -> std::uint32_t {
      if (auto it = std::get_if<IntType>(&t->v)) {
        switch (it->kind) {
          case IntType::Kind::I32:
            return 32;
          case IntType::Kind::I64:
            return 64;
          case IntType::Kind::ICustom:
            return it->bits.value_or(32);
        }
      }
      return 64;
    };

    // 3. Functions
    for (const auto &f: prog.funs) {
      curFuncName_ = f.name.name;
      curFuncRetType_ = f.retType;
      varWidths_.clear();
      varTypes_.clear();
      auto recordVar = [&](const std::string &name, const TypePtr &t) {
        if (!t)
          return;
        varWidths_[name] = getWidth(t);
        varTypes_[name] = t;
      };
      for (const auto &p: f.params)
        recordVar(p.name.name, p.type);
      for (const auto &s: f.syms)
        recordVar(s.name.name, s.type);
      for (const auto &l: f.lets)
        recordVar(l.name.name, l.type);

      // 3a. Extern symbols
      for (const auto &s: f.syms) {
        out_ << "extern ";
        emitType(s.type);
        out_ << " " << getMangledSymbolName(f.name.name, s.name.name) << "(void);\n";
      }
      if (!f.syms.empty())
        out_ << "\n";

      // 3b. Function signature
      emitType(f.retType);
      out_ << " " << mangleName(f.name.name) << "(";
      if (f.params.empty()) {
        out_ << "void";
      } else {
        for (size_t i = 0; i < f.params.size(); ++i) {
          const auto &p = f.params[i];
          TypePtr cur = p.type;
          std::vector<uint64_t> dims;
          while (auto at = std::get_if<ArrayType>(&cur->v)) {
            dims.push_back(at->size);
            cur = at->elem;
          }
          emitType(cur);
          out_ << " " << mangleName(p.name.name);
          for (auto d: dims)
            out_ << "[" << d << "]";
          if (i + 1 < f.params.size())
            out_ << ", ";
        }
      }
      out_ << ") {\n";
      indent_level_++;

      // 3c. Locals and their initializations
      for (const auto &l: f.lets) {
        CtxGuard ctx(isDoubleCtx_, isOrContainsF64(l.type));

        indent();
        TypePtr cur = l.type;
        std::vector<uint64_t> dims;
        while (auto at = std::get_if<ArrayType>(&cur->v)) {
          dims.push_back(at->size);
          cur = at->elem;
        }
        emitType(cur);
        out_ << " " << mangleName(l.name.name);
        for (auto d: dims)
          out_ << "[" << d << "]";

        if (l.init && l.init->kind == InitVal::Kind::Aggregate) {
          out_ << " = ";
          emitInitVal(*l.init, l.type);
          out_ << ";\n";
        } else if (l.init) {
          // Broadcast init
          if (!dims.empty() || std::holds_alternative<StructType>(l.type->v)) {
            // Aggregate broadcast
            out_ << " = {0};\n";
            // Check if we need a loop for non-zero init
            bool isZero = false;
            if (l.init->kind == InitVal::Kind::Int && std::get<IntLit>(l.init->value).value == 0)
              isZero = true;

            if (!isZero) {
              if (!dims.empty()) {
                std::function<void(size_t, std::string)> genLoops = [&](size_t dim,
                                                                        std::string access) {
                  if (dim == dims.size()) {
                    indent();
                    out_ << mangleName(l.name.name) << access << " = ";
                    emitInitVal(*l.init, cur);
                    out_ << ";\n";
                    return;
                  }
                  indent();
                  out_ << "for (int i" << dim << " = 0; i" << dim << " < " << dims[dim] << "; ++i"
                       << dim << ") {\n";
                  indent_level_++;
                  genLoops(dim + 1, access + "[i" + std::to_string(dim) + "]");
                  indent_level_--;
                  indent();
                  out_ << "}\n";
                };
                genLoops(0, "");
              } else {
                indent();
                out_ << "/* Warning: non-zero broadcast init for struct not fully supported */\n";
              }
            }
          } else {
            // Scalar broadcast
            out_ << " = ";
            emitInitVal(*l.init, l.type);
            out_ << ";\n";
          }
        } else {
          out_ << ";\n";
        }
      }

      // 3d. Blocks
      for (const auto &b: f.blocks) {
        out_ << mangleName(b.label.name) << ": ;\n"; // semicolon for empty label case

        for (const auto &ins: b.instrs) {
          indent();
          std::visit(
              [this](auto &&arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, AssignInstr>) {
                  CtxGuard ctx(isDoubleCtx_, isOrContainsF64(getLValueType(arg.lhs)));
                  emitLValue(arg.lhs);
                  out_ << " = ";
                  emitExpr(arg.rhs);
                  out_ << ";\n";
                } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                  out_ << "// assume ";
                  emitCond(arg.cond);
                  out_ << "\n";
                } else if constexpr (std::is_same_v<T, RequireInstr>) {
                  if (!noRequire_) {
                    out_ << "assert(";
                    emitCond(arg.cond);
                    if (arg.message)
                      out_ << " && \"" << *arg.message << "\"";
                    out_ << ");\n";
                  }
                } else if constexpr (std::is_same_v<T, StoreInstr>) {
                  TypePtr pointeeTy = nullptr;
                  if (auto ptrTy = getExprType(arg.ptr)) {
                    if (auto pt = std::get_if<PtrType>(&ptrTy->v))
                      pointeeTy = pt->pointee;
                  }
                  CtxGuard ctx(isDoubleCtx_, isOrContainsF64(pointeeTy));
                  out_ << "*";
                  emitExpr(arg.ptr);
                  out_ << " = ";
                  emitExpr(arg.val);
                  out_ << ";\n";
                }
              },
              ins
          );
        }
        indent();
        std::visit(
            [this](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, BrTerm>) {
                if (arg.isConditional) {
                  out_ << "if (";
                  emitCond(*arg.cond);
                  out_ << ") goto " << mangleName(arg.thenLabel.name) << ";\n";
                  indent();
                  out_ << "else goto " << mangleName(arg.elseLabel.name) << ";\n";
                } else {
                  out_ << "goto " << mangleName(arg.dest.name) << ";\n";
                }
              } else if constexpr (std::is_same_v<T, RetTerm>) {
                out_ << "return";
                if (arg.value) {
                  CtxGuard ctx(isDoubleCtx_, isOrContainsF64(curFuncRetType_));
                  out_ << " ";
                  emitExpr(*arg.value);
                }
                out_ << ";\n";
              } else if constexpr (std::is_same_v<T, UnreachableTerm>) {
                out_ << "// unreachable\n";
              }
            },
            b.term
        );
      }

      indent_level_--;
      out_ << "}\n\n";
    }
  }

  void CBackend::emitExpr(const Expr &expr) {
    out_ << "(";
    emitAtom(expr.first);
    for (const auto &t: expr.rest) {
      out_ << (t.op == AddOp::Plus ? " + " : " - ");
      emitAtom(t.atom);
    }
    out_ << ")";
  }

  void CBackend::emitAtom(const Atom &atom) {
    out_ << "(";
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            if (arg.op == AtomOpKind::LShr) {
              // LShr is the logical (unsigned) right shift. C's signed `>>`
              // is implementation-defined for negative LHS, so we cast through
              // unsigned to get well-defined zero-fill semantics matching the
              // SymIR interpreter. (Shl/Shr deliberately use raw signed shifts:
              // SymIR spec §7.1 rule 4 treats SHL result overflow as UB, so
              // any program reaching here with overflowing SHL is on an
              // infeasible path — UBSan-trap is the correct surfacing.)
              std::uint32_t bits = 32;
              if (std::holds_alternative<LocalOrSymId>(arg.coef)) {
                auto name =
                    std::visit([](auto &&v) { return v.name; }, std::get<LocalOrSymId>(arg.coef));
                if (varWidths_.count(name))
                  bits = varWidths_.at(name);
              } else {
                int64_t val = std::get<IntLit>(arg.coef).value;
                if (val < INT32_MIN || val > INT32_MAX)
                  bits = 64;
              }

              out_ << "(";
              if (bits <= 8)
                out_ << "int8_t)((uint8_t)";
              else if (bits <= 16)
                out_ << "int16_t)((uint16_t)";
              else if (bits <= 32)
                out_ << "int32_t)((uint32_t)";
              else
                out_ << "int64_t)((uint64_t)";

              emitCoef(arg.coef);
              out_ << " >> ";
              emitLValue(arg.rval);
              out_ << ")";
            } else if (arg.op == AtomOpKind::Mod) {
              // C fmod (truncated quotient): same semantics as integer %, consistent
              // with SymIR spec which aligns float % with integer % (truncate-toward-zero).
              // Detect float operand via coef type; for a literal coef, the rval
              // pins f32 vs f64 (operands share a type in SymIR).
              auto floatKindOf = [this](const TypePtr &t) -> FloatType::Kind * {
                if (!t)
                  return nullptr;
                if (auto ft = std::get_if<FloatType>(&t->v))
                  return &const_cast<FloatType::Kind &>(ft->kind);
                return nullptr;
              };
              bool isFloat = std::holds_alternative<FloatLit>(arg.coef);
              bool isF32 = false;
              if (auto *lid = std::get_if<LocalOrSymId>(&arg.coef)) {
                auto name = std::visit([](auto &&v) { return v.name; }, *lid);
                auto it = varTypes_.find(name);
                if (it != varTypes_.end()) {
                  if (auto *k = floatKindOf(it->second)) {
                    isFloat = true;
                    isF32 = (*k == FloatType::Kind::F32);
                  }
                }
              }
              if (isFloat && !isF32) {
                if (auto *k = floatKindOf(getLValueType(arg.rval)))
                  isF32 = (*k == FloatType::Kind::F32);
              }
              if (isFloat) {
                out_ << (isF32 ? "fmodf(" : "fmod(");
                emitCoef(arg.coef);
                out_ << ", ";
                emitLValue(arg.rval);
                out_ << ")";
              } else {
                emitCoef(arg.coef);
                out_ << " % ";
                emitLValue(arg.rval);
              }
            } else {
              emitCoef(arg.coef);
              switch (arg.op) {
                case AtomOpKind::Mul:
                  out_ << " * ";
                  break;
                case AtomOpKind::Div:
                  out_ << " / ";
                  break;
                case AtomOpKind::Mod:
                  // unreachable: handled above
                  break;
                case AtomOpKind::And:
                  out_ << " & ";
                  break;
                case AtomOpKind::Or:
                  out_ << " | ";
                  break;
                case AtomOpKind::Xor:
                  out_ << " ^ ";
                  break;
                case AtomOpKind::Shl:
                  out_ << " << ";
                  break;
                case AtomOpKind::Shr:
                  out_ << " >> ";
                  break;
                default:
                  break;
              }
              emitLValue(arg.rval);
            }
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            out_ << "(";
            emitCond(*arg.cond);
            out_ << " ? ";
            emitSelectVal(arg.vtrue);
            out_ << " : ";
            emitSelectVal(arg.vfalse);
            out_ << ")";
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            emitCoef(arg.coef);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            emitLValue(arg.rval);
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            if (arg.op == UnaryOpKind::Not)
              out_ << "~";
            emitLValue(arg.rval);
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            out_ << "&";
            emitLValue(arg.lv);
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            out_ << "*";
            emitLValue(arg.rval);
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            out_ << "(";
            emitType(arg.dstType);
            out_ << ")(";
            std::visit(
                [&](auto &&src) {
                  using S = std::decay_t<decltype(src)>;
                  if constexpr (std::is_same_v<S, IntLit>) {
                    out_ << src.value;
                  } else if constexpr (std::is_same_v<S, FloatLit>) {
                    CtxGuard ctx(isDoubleCtx_, isOrContainsF64(arg.dstType));
                    out_ << formatFloatLit(src.value);
                    if (!isDoubleCtx_)
                      out_ << "f";
                  } else if constexpr (std::is_same_v<S, SymId>) {
                    out_ << getMangledSymbolName(curFuncName_, src.name) << "()";
                  } else {
                    emitLValue(src);
                  }
                },
                arg.src
            );
            out_ << ")";
          }
        },
        atom.v
    );
    out_ << ")";
  }

  void CBackend::emitCond(const Cond &cond) {
    // Take the type from either operand: SymIR requires lhs and rhs to share
    // a type, so the disjunction is just defensive against missing lookups.
    CtxGuard ctx(
        isDoubleCtx_,
        isOrContainsF64(getExprType(cond.lhs)) || isOrContainsF64(getExprType(cond.rhs))
    );
    emitExpr(cond.lhs);
    switch (cond.op) {
      case RelOp::EQ:
        out_ << " == ";
        break;
      case RelOp::NE:
        out_ << " != ";
        break;
      case RelOp::LT:
        out_ << " < ";
        break;
      case RelOp::LE:
        out_ << " <= ";
        break;
      case RelOp::GT:
        out_ << " > ";
        break;
      case RelOp::GE:
        out_ << " >= ";
        break;
    }
    emitExpr(cond.rhs);
  }

  void CBackend::emitLValue(const LValue &lv) {
    out_ << mangleName(lv.base.name);
    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        out_ << "[";
        emitIndex(ai->index);
        out_ << "]";
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        out_ << "." << af->field;
      }
    }
  }

  void CBackend::emitCoef(const Coef &coef) {
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            out_ << arg.value;
          } else if constexpr (std::is_same_v<T, FloatLit>) {
            out_ << formatFloatLit(arg.value);
            if (!isDoubleCtx_)
              out_ << "f";
          } else if constexpr (std::is_same_v<T, NullLit>) {
            out_ << "NULL";
          } else {
            std::visit(
                [this](auto &&id) {
                  if constexpr (std::is_same_v<std::decay_t<decltype(id)>, SymId>) {
                    out_ << getMangledSymbolName(curFuncName_, id.name) << "()";
                  } else {
                    out_ << mangleName(id.name);
                  }
                },
                arg
            );
          }
        },
        coef
    );
  }

  void CBackend::emitSelectVal(const SelectVal &sv) {
    if (std::holds_alternative<RValue>(sv))
      emitLValue(std::get<RValue>(sv));
    else
      emitCoef(std::get<Coef>(sv));
  }

  void CBackend::emitIndex(const Index &idx) {
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            out_ << arg.value;
          } else {
            std::visit(
                [this](auto &&id) {
                  if constexpr (std::is_same_v<std::decay_t<decltype(id)>, SymId>) {
                    out_ << getMangledSymbolName(curFuncName_, id.name) << "()";
                  } else {
                    out_ << mangleName(id.name);
                  }
                },
                arg
            );
          }
        },
        idx
    );
  }

  void CBackend::emitInitVal(const InitVal &iv, TypePtr expectedType) {
    // When ``expectedType`` is unknown (nullptr), leave the existing context
    // alone — the caller already established it (e.g., from the surrounding
    // let.type). Only override when we know the destination type locally.
    CtxGuard ctx(isDoubleCtx_, expectedType ? isOrContainsF64(expectedType) : isDoubleCtx_);
    switch (iv.kind) {
      case InitVal::Kind::Int:
        out_ << std::get<IntLit>(iv.value).value;
        break;
      case InitVal::Kind::Float:
        out_ << formatFloatLit(std::get<FloatLit>(iv.value).value);
        if (!isDoubleCtx_)
          out_ << "f";
        break;
      case InitVal::Kind::Sym:
        out_ << getMangledSymbolName(curFuncName_, std::get<SymId>(iv.value).name) << "()";
        break;
      case InitVal::Kind::Local:
        out_ << mangleName(std::get<LocalId>(iv.value).name);
        break;
      case InitVal::Kind::Undef:
        out_ << "0";
        break;
      case InitVal::Kind::Null:
        out_ << "NULL";
        break;
      case InitVal::Kind::Aggregate: {
        out_ << "{";
        const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
        for (size_t i = 0; i < elements.size(); ++i) {
          TypePtr elemType = nullptr;
          if (expectedType) {
            if (auto at = std::get_if<ArrayType>(&expectedType->v))
              elemType = at->elem;
            else if (auto st = std::get_if<StructType>(&expectedType->v))
              elemType = getStructFieldTypeAt(st->name.name, i);
          }
          emitInitVal(*elements[i], elemType);
          if (i + 1 < elements.size())
            out_ << ", ";
        }
        out_ << "}";
        break;
      }
    }
  }

  TypePtr CBackend::getLValueType(const LValue &lv) {
    auto it = varTypes_.find(lv.base.name);
    // Every let-local, param, and sym is recorded in ``recordVar`` at the
    // top of each function; reaching this assert means a code path emitted
    // an lvalue whose base was never declared — that would silently mis-flag
    // the float-evaluation context and produce a wrong result.
    assert(it != varTypes_.end() && "lvalue base not in varTypes_");
    if (it == varTypes_.end())
      return nullptr;
    TypePtr cur = it->second;
    for (const auto &acc: lv.accesses) {
      if (!cur)
        return nullptr;
      if (std::holds_alternative<AccessIndex>(acc)) {
        if (auto at = std::get_if<ArrayType>(&cur->v))
          cur = at->elem;
        else if (auto pt = std::get_if<PtrType>(&cur->v))
          cur = pt->pointee;
        else
          return nullptr;
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (auto st = std::get_if<StructType>(&cur->v))
          cur = findStructFieldType(st->name.name, af->field);
        else
          return nullptr;
      }
    }
    return cur;
  }

  TypePtr
  CBackend::findStructFieldType(const std::string &structName, const std::string &fieldName) const {
    auto it = structFields_.find(structName);
    if (it == structFields_.end())
      return nullptr;
    for (const auto &[name, type]: it->second)
      if (name == fieldName)
        return type;
    return nullptr;
  }

  TypePtr CBackend::getStructFieldTypeAt(const std::string &structName, size_t idx) const {
    auto it = structFields_.find(structName);
    if (it == structFields_.end() || idx >= it->second.size())
      return nullptr;
    return it->second[idx].second;
  }

  TypePtr CBackend::getCoefType(const Coef &coef) {
    return std::visit(
        [this](auto &&arg) -> TypePtr {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            // Int literals are polymorphic in SymIR; the actual width is
            // pinned by the surrounding context. We return *some* integer
            // type because all callers consume this only via
            // ``isOrContainsF64`` (which is false for any int).
            auto t = std::make_shared<Type>();
            t->v = IntType{IntType::Kind::I64, {}, {}};
            return t;
          } else if constexpr (std::is_same_v<T, FloatLit>) {
            // Float literals inherit the surrounding ``isDoubleCtx_`` —
            // see the header note: callers must invoke this BEFORE setting
            // their own context, so the answer reflects the outer scope.
            auto t = std::make_shared<Type>();
            t->v = FloatType{isDoubleCtx_ ? FloatType::Kind::F64 : FloatType::Kind::F32, {}};
            return t;
          } else if constexpr (std::is_same_v<T, NullLit>) {
            auto t = std::make_shared<Type>();
            t->v = PtrType{nullptr, {}};
            return t;
          } else {
            return std::visit(
                [this](auto &&id) -> TypePtr {
                  auto it = varTypes_.find(id.name);
                  if (it != varTypes_.end()) {
                    return it->second;
                  }
                  return nullptr;
                },
                arg
            );
          }
        },
        coef
    );
  }

  TypePtr CBackend::getAtomType(const Atom &atom) {
    return std::visit(
        [this](auto &&arg) -> TypePtr {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            return getLValueType(arg.rval);
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            return getLValueType(arg.rval);
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            if (std::holds_alternative<RValue>(arg.vtrue)) {
              return getLValueType(std::get<RValue>(arg.vtrue));
            } else {
              return getCoefType(std::get<Coef>(arg.vtrue));
            }
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            return getCoefType(arg.coef);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            return getLValueType(arg.rval);
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            return arg.dstType;
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            auto t = std::make_shared<Type>();
            t->v = PtrType{getLValueType(arg.lv), {}};
            return t;
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            auto pt = getLValueType(arg.rval);
            if (pt) {
              if (auto ptr = std::get_if<PtrType>(&pt->v)) {
                return ptr->pointee;
              }
            }
            return nullptr;
          }
          return nullptr;
        },
        atom.v
    );
  }

  // SymIR requires every atom in an Expr to share a single type, so the
  // first atom's type is the whole expression's type.
  TypePtr CBackend::getExprType(const Expr &expr) { return getAtomType(expr.first); }

} // namespace symir
