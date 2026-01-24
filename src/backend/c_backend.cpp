#include "backend/c_backend.hpp"
#include <algorithm>
#include <functional>
#include <iostream>

namespace symir {

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
          } else if constexpr (std::is_same_v<T, StructType>) {
            out_ << "struct " << mangleName(arg.name.name);
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            // Arrays are emitted as C arrays in context, but here we emit the base type
            emitType(arg.elem);
          }
        },
        type->v
    );
  }

  void CBackend::emit(const Program &prog) {
    out_ << "#include <stdint.h>\n";
    out_ << "#include <stdbool.h>\n";
    out_ << "#include <assert.h>\n\n";

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
      varWidths_.clear();
      for (const auto &p: f.params)
        varWidths_[p.name.name] = getWidth(p.type);
      for (const auto &s: f.syms)
        varWidths_[s.name.name] = getWidth(s.type);
      for (const auto &l: f.lets)
        varWidths_[l.name.name] = getWidth(l.type);

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
          emitInitVal(*l.init);
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
                    emitInitVal(*l.init);
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
            emitInitVal(*l.init);
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
              [this, &f](auto &&arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, AssignInstr>) {
                  emitLValue(arg.lhs);
                  out_ << " = ";
                  emitExpr(arg.rhs);
                  out_ << ";\n";
                } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                  out_ << "// assume ";
                  emitCond(arg.cond);
                  out_ << "\n";
                } else if constexpr (std::is_same_v<T, RequireInstr>) {
                  out_ << "assert(";
                  emitCond(arg.cond);
                  if (arg.message)
                    out_ << " && \"" << *arg.message << "\"";
                  out_ << ");\n";
                }
              },
              ins
          );
        }
        indent();
        std::visit(
            [this, &f](auto &&arg) {
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
                  out_ << " % ";
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
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            out_ << "(";
            emitType(arg.dstType);
            out_ << ")(";
            std::visit(
                [&](auto &&src) {
                  using S = std::decay_t<decltype(src)>;
                  if constexpr (std::is_same_v<S, IntLit>) {
                    out_ << src.value;
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

  void CBackend::emitInitVal(const InitVal &iv) {
    switch (iv.kind) {
      case InitVal::Kind::Int:
        out_ << std::get<IntLit>(iv.value).value;
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
      case InitVal::Kind::Aggregate: {
        out_ << "{";
        const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
        for (size_t i = 0; i < elements.size(); ++i) {
          emitInitVal(*elements[i]);
          if (i + 1 < elements.size())
            out_ << ", ";
        }
        out_ << "}";
        break;
      }
    }
  }

} // namespace symir
