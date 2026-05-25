#include "ast/sir_printer.hpp"

namespace symir {

  void SIRPrinter::indent() {
    for (int i = 0; i < indent_level_; ++i)
      out_ << "  ";
  }

  std::string SIRPrinter::vecSymLocalName(const std::string &symName) const {
    // Strip the `?` after the sigil and append a fixed suffix.
    //   "%?v"  → "%v__solved"
    //   "@?x"  → "%x__solved"  (force local-scope; vec syms are function-scoped)
    if (symName.size() >= 2 && symName[1] == '?')
      return "%" + symName.substr(2) + "__solved";
    return symName;
  }

  static void printDouble(std::ostream &out, double d) {
    std::string s = std::to_string(d);
    // Remove redundant trailing zeros, but keep at least one after '.' if it's not scientific
    // notation
    if (s.find('.') != std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos) {
      while (s.size() > 2 && s.back() == '0' && s[s.size() - 2] != '.') {
        s.pop_back();
      }
    }
    out << s;
  }

  void SIRPrinter::print(const Program &p) {
    for (const auto &s: p.structs) {
      out_ << "struct " << s.name.name << " {\n";
      indent_level_++;
      for (const auto &f: s.fields) {
        indent();
        out_ << f.name << ": ";
        printType(f.type);
        out_ << ";\n";
      }
      indent_level_--;
      out_ << "} \n\n";
    }

    for (const auto &f: p.funs) {
      out_ << "fun " << f.name.name << "(";
      for (size_t i = 0; i < f.params.size(); ++i) {
        out_ << f.params[i].name.name << ": ";
        printType(f.params[i].type);
        if (i + 1 < f.params.size())
          out_ << ", ";
      }
      out_ << ") : ";
      printType(f.retType);
      out_ << " {\n";
      indent_level_++;

      // 1. Symbols. If a model is present, scalar syms get substituted
      //    inline at use sites and are dropped here; vec syms get
      //    materialized as synthetic let-decls (see step 1b).
      bool hasModel = !model_.empty() || !vecModel_.empty();
      if (!hasModel) {
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
          printType(s.type);
          if (s.domain) {
            out_ << " ";
            printDomain(*s.domain);
          }
          out_ << ";\n";
        }
      }

      // 1b. Synthetic let-decls for each vec sym in vecModel. Vector
      //     sym references can't be substituted inline (Atom grammar has
      //     no vector literal), so we lower them to a constant let.
      for (const auto &s: f.syms) {
        auto it = vecModel_.find(s.name.name);
        if (it == vecModel_.end())
          continue;
        indent();
        out_ << "let " << vecSymLocalName(s.name.name) << ": ";
        printType(s.type);
        out_ << " = {";
        for (size_t k = 0; k < it->second.size(); ++k) {
          const auto &v = it->second[k];
          if (std::holds_alternative<int64_t>(v))
            out_ << std::get<int64_t>(v);
          else
            printDouble(out_, std::get<double>(v));
          if (k + 1 < it->second.size())
            out_ << ", ";
        }
        out_ << "};\n";
      }

      // 2. Locals
      for (const auto &l: f.lets) {
        indent();
        out_ << "let " << (l.isMutable ? "mut " : "") << l.name.name << ": ";
        printType(l.type);
        if (l.init) {
          out_ << " = ";
          printInitVal(*l.init);
        }
        out_ << ";\n";
      }

      // 3. Blocks
      for (const auto &b: f.blocks) {
        out_ << b.label.name << ":\n";
        for (const auto &ins: b.instrs) {
          indent();
          std::visit(
              [this](auto &&arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, AssignInstr>) {
                  printLValue(arg.lhs);
                  out_ << " = ";
                  printExpr(arg.rhs);
                } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                  out_ << "assume ";
                  printCond(arg.cond);
                } else if constexpr (std::is_same_v<T, RequireInstr>) {
                  out_ << "require ";
                  printCond(arg.cond);
                  if (arg.message)
                    out_ << ", \"" << *arg.message << "\"";
                } else if constexpr (std::is_same_v<T, StoreInstr>) {
                  out_ << "store ";
                  printExpr(arg.ptr);
                  out_ << ", ";
                  printExpr(arg.val);
                }
                out_ << ";\n";
              },
              ins
          );
        }
        indent();
        std::visit(
            [this](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, BrTerm>) {
                out_ << "br ";
                if (arg.isConditional) {
                  printCond(*arg.cond);
                  out_ << ", " << arg.thenLabel.name << ", " << arg.elseLabel.name;
                } else {
                  out_ << arg.dest.name;
                }
              } else if constexpr (std::is_same_v<T, RetTerm>) {
                out_ << "ret";
                if (arg.value) {
                  out_ << " ";
                  printExpr(*arg.value);
                }
              } else if constexpr (std::is_same_v<T, UnreachableTerm>) {
                out_ << "unreachable";
              }
              out_ << ";\n";
            },
            b.term
        );
      }

      indent_level_--;
      out_ << "} \n\n";
    }
  }

  void SIRPrinter::printType(const TypePtr &t) {
    if (!t)
      return;
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
            }
          } else if constexpr (std::is_same_v<T, FloatType>) {
            switch (arg.kind) {
              case FloatType::Kind::F32:
                out_ << "f32";
                break;
              case FloatType::Kind::F64:
                out_ << "f64";
                break;
            }
          } else if constexpr (std::is_same_v<T, StructType>) {
            out_ << arg.name.name;
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            out_ << "[" << arg.size << "] ";
            printType(arg.elem);
          } else if constexpr (std::is_same_v<T, PtrType>) {
            out_ << "ptr ";
            printType(arg.pointee);
          } else if constexpr (std::is_same_v<T, VecType>) {
            out_ << "<" << arg.size << "> ";
            printType(arg.elem);
          }
        },
        t->v
    );
  }

  void SIRPrinter::printExpr(const Expr &e) {
    printAtom(e.first);
    for (const auto &t: e.rest) {
      out_ << (t.op == AddOp::Plus ? " + " : " - ");
      printAtom(t.atom);
    }
  }

  void SIRPrinter::printAtom(const Atom &a) {
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            printCoef(arg.coef);
            out_ << " " << atomOpToString(arg.op) << " ";
            printLValue(arg.rval);
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            out_ << "select ";
            if (arg.cond)
              printCond(*arg.cond);
            else if (arg.maskExpr)
              printExpr(*arg.maskExpr); // [v0.2.1] mask form
            out_ << ", ";
            printSelectVal(arg.vtrue);
            out_ << ", ";
            printSelectVal(arg.vfalse);
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            printCoef(arg.coef);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            printLValue(arg.rval);
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            if (arg.op == UnaryOpKind::Not) {
              out_ << "~";
            }
            printLValue(arg.rval);
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            std::visit(
                [&](auto &&src) {
                  using S = std::decay_t<decltype(src)>;
                  if constexpr (std::is_same_v<S, IntLit>) {
                    out_ << src.value;
                  } else if constexpr (std::is_same_v<S, FloatLit>) {
                    printDouble(out_, src.value);
                  } else if constexpr (std::is_same_v<S, SymId>) {
                    if (vecModel_.count(src.name)) {
                      // Vec sym — substitute with the synthetic local.
                      out_ << vecSymLocalName(src.name);
                    } else if (model_.count(src.name)) {
                      auto val = model_.at(src.name);
                      if (std::holds_alternative<int64_t>(val))
                        out_ << std::get<int64_t>(val);
                      else
                        printDouble(out_, std::get<double>(val));
                    } else {
                      out_ << src.name;
                    }
                  } else {
                    printLValue(src);
                  }
                },
                arg.src
            );
            out_ << " as ";
            printType(arg.dstType);
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            out_ << "addr ";
            printLValue(arg.lv);
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            out_ << "load ";
            printLValue(arg.rval);
          } else if constexpr (std::is_same_v<T, CmpAtom>) {
            out_ << "cmp " << relOpToString(arg.op) << " ";
            printSelectVal(arg.lhs);
            out_ << ", ";
            printSelectVal(arg.rhs);
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            out_ << "ptrindex ";
            printLValue(arg.rval);
            out_ << ", ";
            printIndex(arg.index);
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            out_ << "ptrfield ";
            printLValue(arg.rval);
            out_ << ", " << arg.field;
          }
        },
        a.v
    );
  }

  void SIRPrinter::printCond(const Cond &c) {
    printExpr(c.lhs);
    out_ << " " << relOpToString(c.op) << " ";
    printExpr(c.rhs);
  }

  void SIRPrinter::printLValue(const LValue &lv) {
    if (model_.count(lv.base.name) && lv.accesses.empty()) {
      auto val = model_.at(lv.base.name);
      if (std::holds_alternative<int64_t>(val))
        out_ << std::get<int64_t>(val);
      else
        printDouble(out_, std::get<double>(val));
      return;
    }
    out_ << lv.base.name;
    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        out_ << "[";
        printIndex(ai->index);
        out_ << "]";
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        out_ << "." << af->field;
      }
    }
  }

  void SIRPrinter::printCoef(const Coef &c) {
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            out_ << arg.value;
          } else if constexpr (std::is_same_v<T, FloatLit>) {
            printDouble(out_, arg.value);
          } else if constexpr (std::is_same_v<T, NullLit>) {
            out_ << "null";
          } else {
            std::visit(
                [this](auto &&id) {
                  if (vecModel_.count(id.name)) {
                    out_ << vecSymLocalName(id.name);
                  } else if (model_.count(id.name)) {
                    auto val = model_.at(id.name);
                    if (std::holds_alternative<int64_t>(val))
                      out_ << std::get<int64_t>(val);
                    else
                      printDouble(out_, std::get<double>(val));
                  } else {
                    out_ << id.name;
                  }
                },
                arg
            );
          }
        },
        c
    );
  }

  void SIRPrinter::printSelectVal(const SelectVal &sv) {
    if (std::holds_alternative<RValue>(sv)) {
      printLValue(std::get<RValue>(sv));
    } else {
      printCoef(std::get<Coef>(sv));
    }
  }

  void SIRPrinter::printIndex(const Index &idx) {
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            out_ << arg.value;
          } else {
            std::visit(
                [this](auto &&id) {
                  if (vecModel_.count(id.name)) {
                    out_ << vecSymLocalName(id.name);
                  } else if (model_.count(id.name)) {
                    auto val = model_.at(id.name);
                    if (std::holds_alternative<int64_t>(val))
                      out_ << std::get<int64_t>(val);
                    else
                      printDouble(out_, std::get<double>(val));
                  } else {
                    out_ << id.name;
                  }
                },
                arg
            );
          }
        },
        idx
    );
  }

  void SIRPrinter::printInitVal(const InitVal &iv) {
    switch (iv.kind) {
      case InitVal::Kind::Int:
        out_ << std::get<IntLit>(iv.value).value;
        break;
      case InitVal::Kind::Float:
        printDouble(out_, std::get<FloatLit>(iv.value).value);
        break;
      case InitVal::Kind::Sym: {
        auto name = std::get<SymId>(iv.value).name;
        if (vecModel_.count(name)) {
          out_ << vecSymLocalName(name);
        } else if (model_.count(name)) {
          auto val = model_.at(name);
          if (std::holds_alternative<int64_t>(val))
            out_ << std::get<int64_t>(val);
          else
            printDouble(out_, std::get<double>(val));
        } else {
          out_ << name;
        }
        break;
      }
      case InitVal::Kind::Local:
        out_ << std::get<LocalId>(iv.value).name;
        break;
      case InitVal::Kind::Undef:
        out_ << "undef";
        break;
      case InitVal::Kind::Null:
        out_ << "null";
        break;
      case InitVal::Kind::Aggregate: {
        out_ << "{";
        const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
        for (size_t i = 0; i < elements.size(); ++i) {
          printInitVal(*elements[i]);
          if (i + 1 < elements.size())
            out_ << ", ";
        }
        out_ << "}";
        break;
      }
    }
  }

  void SIRPrinter::printDomain(const Domain &d) {
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

  std::string SIRPrinter::relOpToString(RelOp op) {
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

  std::string SIRPrinter::atomOpToString(AtomOpKind op) {
    switch (op) {
      case AtomOpKind::Mul:
        return "*";
      case AtomOpKind::Div:
        return "/";
      case AtomOpKind::Mod:
        return "%";
      case AtomOpKind::And:
        return "&";
      case AtomOpKind::Or:
        return "|";
      case AtomOpKind::Xor:
        return "^";
      case AtomOpKind::Shl:
        return "<<";
      case AtomOpKind::Shr:
        return ">>";
      case AtomOpKind::LShr:
        return ">>>";
    }
    return "?";
  }

} // namespace symir
