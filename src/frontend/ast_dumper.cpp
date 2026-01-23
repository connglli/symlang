#include "ast/ast_dumper.hpp"

namespace symir {

  void ASTDumper::indent() {
    for (int i = 0; i < indent_level_; ++i)
      out_ << "  ";
  }

  void ASTDumper::dump(const Program &p) {
    out_ << "Program\n";
    indent_level_++;
    for (const auto &s: p.structs) {
      indent();
      out_ << "StructDecl: " << s.name.name << "\n";
      indent_level_++;
      for (const auto &f: s.fields) {
        indent();
        out_ << "Field: " << f.name << " : ";
        dumpType(f.type);
        out_ << "\n";
      }
      indent_level_--;
    }

    for (const auto &f: p.funs) {
      indent();
      out_ << "FunDecl: " << f.name.name << " : ";
      dumpType(f.retType);
      out_ << "\n";
      indent_level_++;

      if (!f.params.empty()) {
        indent();
        out_ << "Params:\n";
        indent_level_++;
        for (const auto &p: f.params) {
          indent();
          out_ << p.name.name << " : ";
          dumpType(p.type);
          out_ << "\n";
        }
        indent_level_--;
      }

      if (!f.syms.empty()) {
        indent();
        out_ << "Symbols:\n";
        indent_level_++;
        for (const auto &s: f.syms) {
          indent();
          out_ << "sym " << s.name.name << " : ";
          switch (s.kind) {
            case SymKind::Value:
              out_ << "value ";
              break;
            case SymKind::Coef:
              out_ << "coef ";
              break;
            case SymKind::Index:
              out_ << "index ";
              break;
          }
          dumpType(s.type);
          if (s.domain) {
            out_ << " ";
            dumpDomain(*s.domain);
          }
          out_ << "\n";
        }
        indent_level_--;
      }

      if (!f.lets.empty()) {
        indent();
        out_ << "Locals:\n";
        indent_level_++;
        for (const auto &l: f.lets) {
          indent();
          out_ << (l.isMutable ? "let mut " : "let ") << l.name.name << " : ";
          dumpType(l.type);
          if (l.init) {
            out_ << " = ";
            dumpInitVal(*l.init);
          }
          out_ << "\n";
        }
        indent_level_--;
      }

      for (const auto &b: f.blocks) {
        indent();
        out_ << "Block: " << b.label.name << "\n";
        indent_level_++;
        for (const auto &ins: b.instrs) {
          indent();
          std::visit(
              [this](auto &&arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, AssignInstr>) {
                  dumpLValue(arg.lhs);
                  out_ << " = ";
                  dumpExpr(arg.rhs);
                } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                  out_ << "assume ";
                  dumpCond(arg.cond);
                } else if constexpr (std::is_same_v<T, RequireInstr>) {
                  out_ << "require ";
                  dumpCond(arg.cond);
                  if (arg.message)
                    out_ << ", \"" << *arg.message << "\"";
                }
                out_ << "\n";
              },
              ins
          );
        }
        indent();
        out_ << "Terminator: ";
        std::visit(
            [this](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, BrTerm>) {
                out_ << "br ";
                if (arg.isConditional) {
                  dumpCond(*arg.cond);
                  out_ << ", " << arg.thenLabel.name << ", " << arg.elseLabel.name;
                } else {
                  out_ << arg.dest.name;
                }
              } else if constexpr (std::is_same_v<T, RetTerm>) {
                out_ << "ret";
                if (arg.value) {
                  out_ << " ";
                  dumpExpr(*arg.value);
                }
              } else if constexpr (std::is_same_v<T, UnreachableTerm>) {
                out_ << "unreachable";
              }
              out_ << "\n";
            },
            b.term
        );
        indent_level_--;
      }
      indent_level_--;
    }
    indent_level_--;
  }

  void ASTDumper::dumpType(const TypePtr &t) {
    if (!t) {
      out_ << "NULL_TYPE";
      return;
    }
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntType>) {
            switch (arg.kind) {
              case IntType::Kind::I32:
                out_ << "i32";
                break;
              case IntType::Kind::I64:
                out_ << "i64";
                break;
              case IntType::Kind::ICustom:
                out_ << "i" << (arg.bits ? std::to_string(*arg.bits) : "?");
                break;
              case IntType::Kind::IntKeyword:
                out_ << "int";
                break;
            }
          } else if constexpr (std::is_same_v<T, StructType>) {
            out_ << arg.name.name;
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            out_ << "[" << arg.size << "] ";
            dumpType(arg.elem);
          }
        },
        t->v
    );
  }

  void ASTDumper::dumpExpr(const Expr &e) {
    out_ << "(";
    dumpAtom(e.first);
    for (const auto &t: e.rest) {
      out_ << (t.op == AddOp::Plus ? " + " : " - ");
      dumpAtom(t.atom);
    }
    out_ << ")";
  }

  void ASTDumper::dumpAtom(const Atom &a) {
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            dumpCoef(arg.coef);
            out_ << " " << atomOpToString(arg.op) << " ";
            dumpLValue(arg.rval);
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            out_ << "select ";
            dumpCond(*arg.cond);
            out_ << ", ";
            dumpSelectVal(arg.vtrue);
            out_ << ", ";
            dumpSelectVal(arg.vfalse);
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            dumpCoef(arg.coef);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            dumpLValue(arg.rval);
          }
        },
        a.v
    );
  }

  void ASTDumper::dumpCond(const Cond &c) {
    dumpExpr(c.lhs);
    out_ << " " << relOpToString(c.op) << " ";
    dumpExpr(c.rhs);
  }

  void ASTDumper::dumpLValue(const LValue &lv) {
    out_ << lv.base.name;
    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        out_ << "[";
        dumpIndex(ai->index);
        out_ << "]";
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        out_ << "." << af->field;
      }
    }
  }

  void ASTDumper::dumpCoef(const Coef &c) {
    std::visit(
        [this, &c](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            out_ << arg.value;
          } else {
            std::visit([this](auto &&id) { out_ << id.name; }, arg);
          }
        },
        c
    );
  }

  void ASTDumper::dumpSelectVal(const SelectVal &sv) {
    if (std::holds_alternative<RValue>(sv)) {
      dumpLValue(std::get<RValue>(sv));
    } else {
      dumpCoef(std::get<Coef>(sv));
    }
  }

  void ASTDumper::dumpIndex(const Index &idx) {
    std::visit(
        [this, &idx](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            out_ << arg.value;
          } else {
            std::visit([this](auto &&id) { out_ << id.name; }, arg);
          }
        },
        idx
    );
  }

  void ASTDumper::dumpInitVal(const InitVal &iv) {
    switch (iv.kind) {
      case InitVal::Kind::Int:
        out_ << std::get<IntLit>(iv.value).value;
        break;
      case InitVal::Kind::Sym:
        out_ << std::get<SymId>(iv.value).name;
        break;
      case InitVal::Kind::Local:
        out_ << std::get<LocalId>(iv.value).name;
        break;
      case InitVal::Kind::Undef:
        out_ << "undef";
        break;
    }
  }

  void ASTDumper::dumpDomain(const Domain &d) {
    out_ << "in ";
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, DomainInterval>) {
            out_ << "[" << arg.lo << ", " << arg.hi << "]";
          } else if constexpr (std::is_same_v<T, DomainSet>) {
            out_ << "{";
            for (size_t i = 0; i < arg.values.size(); ++i) {
              out_ << arg.values[i] << (i + 1 < arg.values.size() ? ", " : "");
            }
            out_ << "}";
          }
        },
        d
    );
  }

  std::string ASTDumper::relOpToString(RelOp op) {
    switch (op) {
      case RelOp::EQ:
        return "==";
      case RelOp::NE:
        return "!=";
      case RelOp::LT:
        return "<";
      case RelOp::LE:
        return "<=";
      case RelOp::GT:
        return ">";
      case RelOp::GE:
        return ">=";
    }
    return "?";
  }

  std::string ASTDumper::atomOpToString(AtomOpKind op) {
    switch (op) {
      case AtomOpKind::Mul:
        return "*";
      case AtomOpKind::Div:
        return "/";
      case AtomOpKind::Mod:
        return "% ";
    }
    return "?";
  }

} // namespace symir
