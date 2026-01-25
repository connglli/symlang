#include "backend/wasm_backend.hpp"
#include <algorithm>
#include <iomanip>

namespace symir {

  void WasmBackend::indent() {
    for (int i = 0; i < indent_level_; ++i)
      out_ << "  ";
  }

  std::string WasmBackend::mangleName(const std::string &name) {
    if (name.empty())
      return name;
    size_t start = 0;
    if (name[0] == '@' || name[0] == '%' || name[0] == '^') {
      start = 1;
      if (name.size() > 1 && name[1] == '?')
        start = 2;
    }
    return "$" + name.substr(start);
  }

  std::string WasmBackend::stripSigil(const std::string &name) {
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
  WasmBackend::getMangledSymbolName(const std::string &funcName, const std::string &symName) {
    return stripSigil(funcName) + "__" + stripSigil(symName);
  }

  std::string WasmBackend::getWasmType(const TypePtr &type) {
    if (!type)
      return "";
    return std::visit(
        [](auto &&arg) -> std::string {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntType>) {
            if (arg.kind == IntType::Kind::I64 || (arg.bits.has_value() && *arg.bits > 32))
              return "i64";
            return "i32";
          } else {
            return "i32";
          }
        },
        type->v
    );
  }

  std::uint32_t WasmBackend::getIntWidth(const TypePtr &type) {
    if (!type)
      return 32;
    if (auto it = std::get_if<IntType>(&type->v)) {
      if (it->kind == IntType::Kind::I32)
        return 32;
      if (it->kind == IntType::Kind::I64)
        return 64;
      return it->bits.value_or(32);
    }
    return 32;
  }

  std::uint32_t WasmBackend::getTypeSize(const TypePtr &type) {
    if (!type)
      return 0;
    return std::visit(
        [this](auto &&arg) -> std::uint32_t {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntType>) {
            std::uint32_t bits = 32;
            if (arg.kind == IntType::Kind::I32)
              bits = 32;
            else if (arg.kind == IntType::Kind::I64)
              bits = 64;
            else
              bits = arg.bits.value_or(32);
            if (bits <= 8)
              return 1;
            if (bits <= 16)
              return 2;
            if (bits <= 32)
              return 4;
            return 8;
          } else if constexpr (std::is_same_v<T, StructType>) {
            if (structLayouts_.count(arg.name.name))
              return structLayouts_.at(arg.name.name).totalSize;
            return 0;
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            return arg.size * getTypeSize(arg.elem);
          }
          return 0;
        },
        type->v
    );
  }

  void WasmBackend::computeLayouts(const Program &prog) {
    for (const auto &s: prog.structs) {
      StructInfo info;
      std::uint32_t offset = 0;
      for (const auto &f: s.fields) {
        std::uint32_t size = getTypeSize(f.type);
        if (size >= 8 && offset % 8 != 0)
          offset += 8 - (offset % 8);
        else if (size >= 4 && offset % 4 != 0)
          offset += 4 - (offset % 4);

        info.fields[f.name] = {offset, size, f.type};
        info.fieldNames.push_back(f.name);
        offset += size;
      }
      if (offset % 8 != 0)
        offset += 8 - (offset % 8);
      info.totalSize = offset;
      structLayouts_[s.name.name] = info;
    }
  }

  void WasmBackend::emitMask(std::uint32_t bitwidth) {
    if (bitwidth == 32 || bitwidth == 64)
      return;
    if (bitwidth < 32) {
      uint32_t mask = (1ULL << bitwidth) - 1;
      indent();
      out_ << "i32.const " << mask << "\n";
      indent();
      out_ << "i32.and\n";
    } else {
      uint64_t mask = (1ULL << bitwidth) - 1;
      indent();
      out_ << "i64.const " << mask << "\n";
      indent();
      out_ << "i64.and\n";
    }
  }

  void WasmBackend::emitSignExtend(std::uint32_t fromWidth, std::uint32_t toWidth) {
    if (fromWidth == toWidth)
      return;
    if (toWidth == 32) {
      indent();
      out_ << "i32.const " << (32 - fromWidth) << "\n";
      indent();
      out_ << "i32.shl\n";
      indent();
      out_ << "i32.const " << (32 - fromWidth) << "\n";
      indent();
      out_ << "i32.shr_s\n";
    } else {
      if (fromWidth < 32) {
        emitSignExtend(fromWidth, 32);
        indent();
        out_ << "i64.extend_i32_s\n";
      } else if (fromWidth == 32) {
        indent();
        out_ << "i64.extend_i32_s\n";
      } else {
        indent();
        out_ << "i64.const " << (64 - fromWidth) << "\n";
        indent();
        out_ << "i64.shl\n";
        indent();
        out_ << "i64.const " << (64 - fromWidth) << "\n";
        indent();
        out_ << "i64.shr_s\n";
      }
    }
  }

  void WasmBackend::emitExpr(const Expr &expr, std::uint32_t targetWidth) {
    emitAtom(expr.first, targetWidth);
    for (const auto &t: expr.rest) {
      emitAtom(t.atom, targetWidth);
      indent();
      if (targetWidth <= 32)
        out_ << (t.op == AddOp::Plus ? "i32.add\n" : "i32.sub\n");
      else
        out_ << (t.op == AddOp::Plus ? "i64.add\n" : "i64.sub\n");
      emitMask(targetWidth);
    }
  }

  void WasmBackend::emitAtom(const Atom &atom, std::uint32_t targetWidth) {
    std::visit(
        [this, targetWidth](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, CoefAtom>) {
            emitCoef(arg.coef, targetWidth);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            emitLValue(arg.rval, false);
          } else if constexpr (std::is_same_v<T, OpAtom>) {
            emitCoef(arg.coef, targetWidth);
            emitLValue(arg.rval, false);
            indent();
            std::string opStr;
            std::string prefix = (targetWidth <= 32 ? "i32." : "i64.");
            switch (arg.op) {
              case AtomOpKind::Mul:
                opStr = prefix + "mul";
                break;
              case AtomOpKind::Div:
                opStr = prefix + "div_s";
                break;
              case AtomOpKind::Mod:
                opStr = prefix + "rem_s";
                break;
              case AtomOpKind::And:
                opStr = prefix + "and";
                break;
              case AtomOpKind::Or:
                opStr = prefix + "or";
                break;
              case AtomOpKind::Xor:
                opStr = prefix + "xor";
                break;
              case AtomOpKind::Shl:
                opStr = prefix + "shl";
                break;
              case AtomOpKind::Shr:
                opStr = prefix + "shr_s";
                break;
              case AtomOpKind::LShr:
                opStr = prefix + "shr_u";
                break;
            }
            out_ << opStr << "\n";
            emitMask(targetWidth);
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            emitCond(*arg.cond);
            indent();
            out_ << "if (result " << (targetWidth <= 32 ? "i32" : "i64") << ")\n";
            indent_level_++;
            emitSelectVal(arg.vtrue, targetWidth);
            indent_level_--;
            indent();
            out_ << "else\n";
            indent_level_++;
            emitSelectVal(arg.vfalse, targetWidth);
            indent_level_--;
            indent();
            out_ << "end\n";
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            emitLValue(arg.rval, false);
            indent();
            if (targetWidth <= 32) {
              out_ << "i32.const -1\n";
              indent();
              out_ << "i32.xor\n";
            } else {
              out_ << "i64.const -1\n";
              indent();
              out_ << "i64.xor\n";
            }
            emitMask(targetWidth);
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            std::uint32_t srcWidth = 32;
            std::visit(
                [&](auto &&src) {
                  using S = std::decay_t<decltype(src)>;
                  if constexpr (std::is_same_v<S, IntLit>) {
                    srcWidth = (src.value > INT32_MAX || src.value < INT32_MIN) ? 64 : 32;
                    indent();
                    out_ << (srcWidth <= 32 ? "i32.const " : "i64.const ") << src.value << "\n";
                  } else if constexpr (std::is_same_v<S, SymId>) {
                    indent();
                    out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, src.name))
                         << "\n";
                    srcWidth = 32;
                  } else {
                    emitLValue(src, false);
                    srcWidth = 32;
                  }
                },
                arg.src
            );
            std::uint32_t dstWidth = getIntWidth(arg.dstType);
            if (srcWidth <= 32 && dstWidth > 32) {
              indent();
              out_ << "i64.extend_i32_s\n";
            } else if (srcWidth > 32 && dstWidth <= 32) {
              indent();
              out_ << "i32.wrap_i64\n";
            }
            emitMask(dstWidth);
          }
        },
        atom.v
    );
  }

  void WasmBackend::emitCond(const Cond &cond) {
    std::uint32_t width = 32;
    emitExpr(cond.lhs, width);
    emitExpr(cond.rhs, width);
    indent();
    std::string opStr;
    std::string prefix = (width <= 32 ? "i32." : "i64.");
    switch (cond.op) {
      case RelOp::EQ:
        opStr = prefix + "eq";
        break;
      case RelOp::NE:
        opStr = prefix + "ne";
        break;
      case RelOp::LT:
        opStr = prefix + "lt_s";
        break;
      case RelOp::LE:
        opStr = prefix + "le_s";
        break;
      case RelOp::GT:
        opStr = prefix + "gt_s";
        break;
      case RelOp::GE:
        opStr = prefix + "ge_s";
        break;
    }
    out_ << opStr << "\n";
  }

  void WasmBackend::emitAddress(const LValue &lv) {
    const auto &info = locals_.at(lv.base.name);
    indent();
    out_ << "local.get $__old_sp\n";
    indent();
    out_ << "i32.const " << info.offset << "\n";
    indent();
    out_ << "i32.sub\n";

    TypePtr curType = info.symirType;
    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        auto at = std::get<ArrayType>(curType->v);
        std::uint32_t elemSize = getTypeSize(at.elem);
        emitIndex(ai->index);
        indent();
        out_ << "i32.const " << elemSize << "\n";
        indent();
        out_ << "i32.mul\n";
        indent();
        out_ << "i32.add\n";
        curType = at.elem;
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        auto st = std::get<StructType>(curType->v);
        const auto &sinfo = structLayouts_.at(st.name.name);
        const auto &finfo = sinfo.fields.at(af->field);
        indent();
        out_ << "i32.const " << finfo.offset << "\n";
        indent();
        out_ << "i32.add\n";
        curType = finfo.type;
      }
    }
  }

  void WasmBackend::emitLValue(const LValue &lv, bool isStore) {
    const auto &info = locals_.at(lv.base.name);
    if (info.isAggregate || !lv.accesses.empty()) {
      if (!isStore) {
        emitAddress(lv);
        indent();
        out_ << (info.bitwidth <= 32 ? "i32.load\n" : "i64.load\n");
      }
    } else {
      if (isStore) {
        indent();
        out_ << "local.set " << mangleName(lv.base.name) << "\n";
      } else {
        indent();
        out_ << "local.get " << mangleName(lv.base.name) << "\n";
      }
    }
  }

  void WasmBackend::emitCoef(const Coef &coef, std::uint32_t targetWidth) {
    std::visit(
        [this, targetWidth](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            indent();
            out_ << (targetWidth <= 32 ? "i32.const " : "i64.const ") << arg.value << "\n";
          } else {
            std::visit(
                [this, targetWidth](auto &&id) {
                  using ID = std::decay_t<decltype(id)>;
                  if constexpr (std::is_same_v<ID, SymId>) {
                    indent();
                    out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, id.name))
                         << "\n";
                  } else {
                    indent();
                    out_ << "local.get " << mangleName(id.name) << "\n";
                  }
                },
                arg
            );
          }
        },
        coef
    );
  }

  void WasmBackend::emitSelectVal(const SelectVal &sv, std::uint32_t targetWidth) {
    if (std::holds_alternative<RValue>(sv))
      emitLValue(std::get<RValue>(sv), false);
    else
      emitCoef(std::get<Coef>(sv), targetWidth);
  }

  void WasmBackend::emitIndex(const Index &idx) {
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            indent();
            out_ << "i32.const " << arg.value << "\n";
          } else {
            std::visit(
                [this](auto &&id) {
                  indent();
                  out_ << "local.get " << mangleName(id.name) << "\n";
                },
                arg
            );
          }
        },
        idx
    );
  }

  void WasmBackend::emitInitVal(const InitVal &iv, const TypePtr &type, std::uint32_t offset) {
    if (iv.kind == InitVal::Kind::Int) {
      if (auto at = std::get_if<ArrayType>(&type->v)) {
        std::uint32_t elemSize = getTypeSize(at->elem);
        for (std::uint64_t i = 0; i < at->size; ++i) {
          emitInitVal(iv, at->elem, offset - i * elemSize);
        }
      } else if (auto st = std::get_if<StructType>(&type->v)) {
        const auto &sinfo = structLayouts_.at(st->name.name);
        for (const auto &fname: sinfo.fieldNames) {
          const auto &finfo = sinfo.fields.at(fname);
          emitInitVal(iv, finfo.type, offset - finfo.offset);
        }
      } else {
        indent();
        out_ << "local.get $__old_sp\n";
        indent();
        out_ << "i32.const " << offset << "\n";
        indent();
        out_ << "i32.sub\n";
        indent();
        out_ << (getIntWidth(type) <= 32 ? "i32.const " : "i64.const ")
             << std::get<IntLit>(iv.value).value << "\n";
        indent();
        out_ << (getIntWidth(type) <= 32 ? "i32.store\n" : "i64.store\n");
      }
    } else if (iv.kind == InitVal::Kind::Aggregate) {
      const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
      if (auto at = std::get_if<ArrayType>(&type->v)) {
        std::uint32_t elemSize = getTypeSize(at->elem);
        for (size_t i = 0; i < elements.size(); ++i) {
          emitInitVal(*elements[i], at->elem, offset + i * elemSize);
        }
      } else if (auto st = std::get_if<StructType>(&type->v)) {
        const auto &sinfo = structLayouts_.at(st->name.name);
        for (size_t i = 0; i < elements.size() && i < sinfo.fieldNames.size(); ++i) {
          const auto &fname = sinfo.fieldNames[i];
          const auto &finfo = sinfo.fields.at(fname);
          emitInitVal(*elements[i], finfo.type, offset + finfo.offset);
        }
      }
    }
  }

  void WasmBackend::emit(const Program &prog) {
    computeLayouts(prog);
    out_ << "(module\n";
    indent_level_++;

    for (const auto &f: prog.funs) {
      for (const auto &s: f.syms) {
        indent();
        out_ << "(import \"" << stripSigil(f.name.name) << "\" \"" << stripSigil(s.name.name)
             << "\" (func " << mangleName(getMangledSymbolName(f.name.name, s.name.name))
             << " (result " << getWasmType(s.type) << ")))\n";
      }
    }

    indent();
    out_ << "(memory 1)\n";
    indent();
    out_ << "(global $__stack_pointer (mut i32) (i32.const 65536))\n";

    for (const auto &f: prog.funs) {
      curFuncName_ = f.name.name;
      locals_.clear();
      stackSize_ = 0;

      for (const auto &p: f.params) {
        locals_[p.name.name] = {getWasmType(p.type), true, getIntWidth(p.type), false, 0, p.type};
      }
      for (const auto &l: f.lets) {
        bool isAgg = std::holds_alternative<StructType>(l.type->v) ||
                     std::holds_alternative<ArrayType>(l.type->v);
        if (isAgg) {
          std::uint32_t size = getTypeSize(l.type);
          stackSize_ += size;
          locals_[l.name.name] = {"i32", false, getIntWidth(l.type), true, stackSize_, l.type};
        } else {
          locals_[l.name.name] = {
              getWasmType(l.type), false, getIntWidth(l.type), false, 0, l.type
          };
        }
      }

      indent();
      out_ << "(func " << mangleName(f.name.name);
      for (const auto &p: f.params) {
        out_ << " (param " << mangleName(p.name.name) << " " << getWasmType(p.type) << ")";
      }
      if (f.retType) {
        out_ << " (result " << getWasmType(f.retType) << ")";
      }
      out_ << "\n";
      indent_level_++;

      indent();
      out_ << "(local $pc i32)\n";
      indent();
      out_ << "(local $__old_sp i32)\n";
      for (const auto &l: f.lets) {
        if (!locals_[l.name.name].isAggregate) {
          indent();
          out_ << "(local " << mangleName(l.name.name) << " " << locals_[l.name.name].wasmType
               << ")\n";
        }
      }

      if (stackSize_ > 0) {
        indent();
        out_ << "global.get $__stack_pointer\n";
        indent();
        out_ << "local.set $__old_sp\n";
        indent();
        out_ << "global.get $__stack_pointer\n";
        indent();
        out_ << "i32.const " << stackSize_ << "\n";
        indent();
        out_ << "i32.sub\n";
        indent();
        out_ << "global.set $__stack_pointer\n";
      }

      for (const auto &l: f.lets) {
        if (l.init) {
          if (locals_[l.name.name].isAggregate) {
            emitInitVal(*l.init, l.type, locals_[l.name.name].offset);
          } else if (l.init->kind == InitVal::Kind::Int) {
            indent();
            out_ << (locals_[l.name.name].bitwidth <= 32 ? "i32.const " : "i64.const ")
                 << std::get<IntLit>(l.init->value).value << "\n";
            indent();
            out_ << "local.set " << mangleName(l.name.name) << "\n";
          }
        }
      }

      indent();
      out_ << "i32.const 0\n";
      indent();
      out_ << "local.set $pc\n";

      indent();
      out_ << "(loop $loop\n";
      indent_level_++;

      for (size_t i = 0; i < f.blocks.size(); ++i) {
        indent();
        out_ << "(block " << mangleName(f.blocks[i].label.name) << "\n";
        indent_level_++;
      }

      indent();
      out_ << "local.get $pc\n";
      indent();
      out_ << "br_table";
      for (int i = f.blocks.size() - 1; i >= 0; --i) {
        out_ << " " << i;
      }
      out_ << " 0\n";

      for (int i = f.blocks.size() - 1; i >= 0; --i) {
        indent_level_--;
        indent();
        out_ << ") ;; " << f.blocks[i].label.name << "\n";

        const auto &b = f.blocks[i];
        for (const auto &ins: b.instrs) {
          std::visit(
              [this](auto &&arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, AssignInstr>) {
                  const auto &info = locals_.at(arg.lhs.base.name);
                  if (info.isAggregate || !arg.lhs.accesses.empty()) {
                    emitAddress(arg.lhs);
                    emitExpr(arg.rhs, info.bitwidth);
                    indent();
                    out_ << (info.bitwidth <= 32 ? "i32.store\n" : "i64.store\n");
                  } else {
                    emitExpr(arg.rhs, info.bitwidth);
                    indent();
                    out_ << "local.set " << mangleName(arg.lhs.base.name) << "\n";
                  }
                } else if constexpr (std::is_same_v<T, RequireInstr>) {
                  emitCond(arg.cond);
                  indent();
                  out_ << "i32.eqz\n";
                  indent();
                  out_ << "if\n";
                  indent_level_++;
                  indent();
                  out_ << "unreachable\n";
                  indent_level_--;
                  indent();
                  out_ << "end\n";
                }
              },
              ins
          );
        }

        std::visit(
            [this, &f, &f_blocks = f.blocks](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, BrTerm>) {
                if (arg.isConditional) {
                  emitCond(*arg.cond);
                  indent();
                  out_ << "if\n";
                  indent_level_++;
                  int thenIdx = -1;
                  for (size_t j = 0; j < f_blocks.size(); ++j)
                    if (f_blocks[j].label.name == arg.thenLabel.name)
                      thenIdx = j;
                  indent();
                  out_ << "i32.const " << thenIdx << "\n";
                  indent();
                  out_ << "local.set $pc\n";
                  indent_level_--;
                  indent();
                  out_ << "else\n";
                  indent_level_++;
                  int elseIdx = -1;
                  for (size_t j = 0; j < f_blocks.size(); ++j)
                    if (f_blocks[j].label.name == arg.elseLabel.name)
                      elseIdx = j;
                  indent();
                  out_ << "i32.const " << elseIdx << "\n";
                  indent();
                  out_ << "local.set $pc\n";
                  indent_level_--;
                  indent();
                  out_ << "end\n";
                  indent();
                  out_ << "br $loop\n";
                } else {
                  int destIdx = -1;
                  for (size_t j = 0; j < f_blocks.size(); ++j)
                    if (f_blocks[j].label.name == arg.dest.name)
                      destIdx = j;
                  indent();
                  out_ << "i32.const " << destIdx << "\n";
                  indent();
                  out_ << "local.set $pc\n";
                  indent();
                  out_ << "br $loop\n";
                }
              } else if constexpr (std::is_same_v<T, RetTerm>) {
                if (arg.value) {
                  emitExpr(*arg.value, getIntWidth(f.retType));
                }
                if (stackSize_ > 0) {
                  indent();
                  out_ << "local.get $__old_sp\n";
                  indent();
                  out_ << "global.set $__stack_pointer\n";
                }
                indent();
                out_ << "return\n";
              } else if constexpr (std::is_same_v<T, UnreachableTerm>) {
                indent();
                out_ << "unreachable\n";
              }
            },
            b.term
        );
      }

      indent_level_--;
      indent();
      out_ << ") ;; loop\n";

      if (f.retType && !f.blocks.empty()) {
        indent();
        out_ << (getIntWidth(f.retType) <= 32 ? "i32.const 0\n" : "i64.const 0\n");
      }

      indent_level_--;
      indent();
      out_ << ")\n\n";
      indent();
      out_ << "(export \"" << stripSigil(f.name.name) << "\" (func " << mangleName(f.name.name)
           << "))\n";
    }

    indent_level_--;
    out_ << ")\n";
  }

} // namespace symir
