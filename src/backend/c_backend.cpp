#include "backend/c_backend.hpp"
#include <algorithm>

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
            if (arg.kind == IntType::Kind::I32 || arg.kind == IntType::Kind::IntKeyword)
              out_ << "int32_t";
            else if (arg.kind == IntType::Kind::I64)
              out_ << "int64_t";
            else if (arg.kind == IntType::Kind::ICustom) {
              int bits = arg.bits.value_or(32);
              if (bits <= 8)
                out_ << "int8_t";
              else if (bits <= 16)
                out_ << "int16_t";
              else if (bits <= 32)
                out_ << "int32_t";
              else
                out_ << "int64_t";
            }
          } else if constexpr (std::is_same_v<T, StructType>) {
            out_ << "struct " << mangleName(arg.name.name);
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            // This is tricky in C for function params or fields.
            // We'll emit the element type here, and handle brackets elsewhere if needed.
            // But for local decls, we can do: T name[N].
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
        out_ << " "
             << f.name; // Field names don't need mangling usually, but could collide with keywords?
        // Let's not mangle fields for now as they are scoped to struct.
        // But wait, if field is "int", it's fine. If field is "else", problem.
        // Let's mangle fields too? No, usually struct access is `s.field`.
        // But definition `int else;` is invalid.
        // Let's not risk it and skip mangling fields for now or manually escape keywords?
        // Simpler: assume fields are safe or user avoids keywords.
        // If I mangle, I must mangle access too.
        for (auto d: dims)
          out_ << "[" << d << "]";
        out_ << ";\n";
      }
      indent_level_--;
      out_ << "};\n\n";
    }

    // 3. Functions
    for (const auto &f: prog.funs) {
      // 3a. Extern symbols
      for (const auto &s: f.syms) {
        out_ << "extern ";
        emitType(s.type);
        out_ << " " << getMangledSymbolName(f.name.name, s.name.name) << "(void);\n";
      }
      if (!f.syms.empty())
        out_ << "\n";

      // 3b. Function signature
      curFuncName_ = f.name.name;
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
        } else if (l.init && dims.empty()) {
          out_ << " = ";
          emitInitVal(*l.init);
          out_ << ";\n";
        } else if (!l.init && dims.empty()) {
          out_ << " = 0;\n";
        } else {
          // Aggregate target with Scalar Init (broadcast) or no init
          out_ << ";\n";
          uint64_t total_size = 1;
          for (auto d: dims)
            total_size *= d;

          indent();
          out_ << "for (int i = 0; i < " << total_size << "; ++i) ((";
          emitType(cur);
          out_ << "*)" << mangleName(l.name.name) << ")[i] = ";
          if (l.init)
            emitInitVal(*l.init);
          else
            out_ << "0";
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
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
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
            }
            emitLValue(arg.rval);
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
          }
        },
        atom.v
    );
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
            // LocalOrSymId.
            std::visit(
                [this](auto &&id) {
                  using IDT = std::decay_t<decltype(id)>;
                  if constexpr (std::is_same_v<IDT, SymId>) {
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
    if (auto rv = std::get_if<RValue>(&sv))
      emitLValue(*rv);
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
