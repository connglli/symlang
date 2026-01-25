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

  void WasmBackend::emitMask(std::uint32_t bitwidth, std::uint32_t wasmWidth) {
    if (bitwidth >= wasmWidth)
      return;
    if (wasmWidth == 32) {
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
    if (toWidth <= 32) {
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
      emitSignExtend(targetWidth, (targetWidth <= 32 ? 32 : 64));
    }
  }

  void WasmBackend::emitAtom(const Atom &atom, std::uint32_t targetWidth) {
    uint32_t wasmWidth = (targetWidth <= 32 ? 32 : 64);
    std::visit(
        [this, targetWidth, wasmWidth](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, CoefAtom>) {
            emitCoef(arg.coef, targetWidth);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            emitLValue(arg.rval, false);
            if (locals_.count(arg.rval.base.name)) {
              std::uint32_t srcWidth = getIntWidth(locals_.at(arg.rval.base.name).symirType);
              if (srcWidth <= 32 && targetWidth > 32) {
                indent();
                out_ << "i64.extend_i32_s\n";
              } else if (srcWidth > 32 && targetWidth <= 32) {
                indent();
                out_ << "i32.wrap_i64\n";
              }
            }
          } else if constexpr (std::is_same_v<T, OpAtom>) {
            if (arg.op == AtomOpKind::LShr) {
              emitCoef(arg.coef, targetWidth);
              emitMask(targetWidth, wasmWidth);
              emitLValue(arg.rval, false);
              indent();
              out_ << (wasmWidth == 32 ? "i32.shr_u\n" : "i64.shr_u\n");
            } else {
              emitCoef(arg.coef, targetWidth);
              emitLValue(arg.rval, false);
              if (locals_.count(arg.rval.base.name)) {
                std::uint32_t rWidth = getIntWidth(locals_.at(arg.rval.base.name).symirType);
                if (rWidth <= 32 && targetWidth > 32) {
                  indent();
                  out_ << "i64.extend_i32_s\n";
                } else if (rWidth > 32 && targetWidth <= 32) {
                  indent();
                  out_ << "i32.wrap_i64\n";
                }
              }

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
            }
            emitSignExtend(targetWidth, wasmWidth);
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
            if (locals_.count(arg.rval.base.name)) {
              std::uint32_t srcWidth = getIntWidth(locals_.at(arg.rval.base.name).symirType);
              if (srcWidth <= 32 && targetWidth > 32) {
                indent();
                out_ << "i64.extend_i32_s\n";
              } else if (srcWidth > 32 && targetWidth <= 32) {
                indent();
                out_ << "i32.wrap_i64\n";
              }
            }

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
            emitSignExtend(targetWidth, wasmWidth);
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
                    if (syms_.count(src.name)) {
                      srcWidth = getIntWidth(syms_.at(src.name));
                    }
                  } else {
                    emitLValue(src, false);
                    if (locals_.count(src.base.name))
                      srcWidth = getIntWidth(locals_.at(src.base.name).symirType);
                  }
                },
                arg.src
            );
            std::uint32_t dstWidth = getIntWidth(arg.dstType);
            // Handle cross-width conversions
            if (srcWidth <= 32 && (targetWidth > 32 || dstWidth > 32)) {
              // Upcast: extend if targeting 64
              if (wasmWidth == 64) {
                indent();
                out_ << "i64.extend_i32_s\n";
              }
            } else if (srcWidth > 32 && (targetWidth <= 32 || dstWidth <= 32)) {
              // Downcast: wrap if targeting 32
              if (wasmWidth == 32) {
                indent();
                out_ << "i32.wrap_i64\n";
              }
            }
            emitSignExtend(targetWidth, wasmWidth);
          }
        },
        atom.v
    );
  }

  void WasmBackend::emitCond(const Cond &cond) {
    std::uint32_t width = 32;
    auto needs64 = [&](const Expr &e) {
      auto atomNeeds64 = [&](const Atom &a) {
        if (std::holds_alternative<CastAtom>(a.v)) {
          if (getIntWidth(std::get<CastAtom>(a.v).dstType) > 32)
            return true;
        } else if (std::holds_alternative<RValueAtom>(a.v)) {
          auto &lv = std::get<RValueAtom>(a.v).rval;
          if (locals_.count(lv.base.name) && getIntWidth(locals_.at(lv.base.name).symirType) > 32)
            return true;
        } else if (std::holds_alternative<CoefAtom>(a.v)) {
          auto &coef = std::get<CoefAtom>(a.v).coef;
          if (std::holds_alternative<LocalOrSymId>(coef)) {
            auto &id = std::get<LocalOrSymId>(coef);
            if (std::holds_alternative<LocalId>(id)) {
              auto &lid = std::get<LocalId>(id);
              if (locals_.count(lid.name) && getIntWidth(locals_.at(lid.name).symirType) > 32)
                return true;
            }
          }
        }
        return false;
      };
      if (atomNeeds64(e.first))
        return true;
      for (const auto &t: e.rest)
        if (atomNeeds64(t.atom))
          return true;
      return false;
    };
    if (needs64(cond.lhs) || needs64(cond.rhs))
      width = 64;

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
    if (!locals_.count(lv.base.name))
      return;
    const auto &info = locals_.at(lv.base.name);

    if (info.isParam) {
      indent();
      out_ << "local.get " << mangleName(lv.base.name) << "\n";
    } else {
      indent();
      out_ << "local.get $__old_sp\n";
      indent();
      out_ << "i32.const " << info.offset << "\n";
      indent();
      out_ << "i32.sub\n";
    }

    TypePtr curType = info.symirType;
    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        if (auto at = std::get_if<ArrayType>(&curType->v)) {
          std::uint32_t elemSize = getTypeSize(at->elem);
          emitIndex(ai->index);
          indent();
          out_ << "i32.const " << elemSize << "\n";
          indent();
          out_ << "i32.mul\n";
          indent();
          out_ << "i32.add\n";
          curType = at->elem;
        }
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (auto st = std::get_if<StructType>(&curType->v)) {
          if (structLayouts_.count(st->name.name)) {
            const auto &sinfo = structLayouts_.at(st->name.name);
            if (sinfo.fields.count(af->field)) {
              const auto &finfo = sinfo.fields.at(af->field);
              indent();
              out_ << "i32.const " << finfo.offset << "\n";
              indent();
              out_ << "i32.add\n";
              curType = finfo.type;
            }
          }
        }
      }
    }
  }

  void WasmBackend::emitLValue(const LValue &lv, bool isStore) {
    if (!locals_.count(lv.base.name))
      return;
    const auto &info = locals_.at(lv.base.name);
    if (info.isAggregate || !lv.accesses.empty()) {
      if (!isStore) {
        emitAddress(lv);
        indent();
        TypePtr curType = info.symirType;
        for (const auto &acc: lv.accesses) {
          if (std::get_if<AccessIndex>(&acc)) {
            if (auto at = std::get_if<ArrayType>(&curType->v))
              curType = at->elem;
          } else if (auto af = std::get_if<AccessField>(&acc)) {
            if (auto st = std::get_if<StructType>(&curType->v)) {
              auto &fld = af->field;
              if (structLayouts_.count(st->name.name) &&
                  structLayouts_.at(st->name.name).fields.count(fld))
                curType = structLayouts_.at(st->name.name).fields.at(fld).type;
            }
          }
        }
        std::uint32_t width = getIntWidth(curType);
        out_ << (width <= 8
                     ? "i32.load8_s"
                     : (width <= 16 ? "i32.load16_s" : (width <= 32 ? "i32.load" : "i64.load")))
             << "\n";
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
    uint32_t wasmWidth = (targetWidth <= 32 ? 32 : 64);
    std::visit(
        [this, targetWidth, wasmWidth](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            indent();
            out_ << (wasmWidth == 32 ? "i32.const " : "i64.const ") << arg.value << "\n";
          } else {
            std::visit(
                [this, targetWidth, wasmWidth](auto &&id) {
                  using ID = std::decay_t<decltype(id)>;
                  if constexpr (std::is_same_v<ID, SymId>) {
                    indent();
                    out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, id.name))
                         << "\n";
                    std::uint32_t srcWidth = 32;
                    if (syms_.count(id.name)) {
                      srcWidth = getIntWidth(syms_.at(id.name));
                    }
                    if (srcWidth <= 32 && targetWidth > 32) {
                      indent();
                      out_ << "i64.extend_i32_s\n";
                    } else if (srcWidth > 32 && targetWidth <= 32) {
                      indent();
                      out_ << "i32.wrap_i64\n";
                    }
                  } else {
                    indent();
                    out_ << "local.get " << mangleName(id.name) << "\n";
                    if (locals_.count(id.name)) {
                      std::uint32_t srcWidth = getIntWidth(locals_.at(id.name).symirType);
                      if (srcWidth <= 32 && targetWidth > 32) {
                        indent();
                        out_ << "i64.extend_i32_s\n";
                      } else if (srcWidth > 32 && targetWidth <= 32) {
                        indent();
                        out_ << "i32.wrap_i64\n";
                      }
                    }
                  }
                },
                arg
            );
          }
        },
        coef
    );
    emitSignExtend(targetWidth, wasmWidth);
  }

  void WasmBackend::emitSelectVal(const SelectVal &sv, std::uint32_t targetWidth) {
    if (std::holds_alternative<RValue>(sv)) {
      const auto &lv = std::get<RValue>(sv);
      emitLValue(lv, false);
      if (locals_.count(lv.base.name)) {
        std::uint32_t srcWidth = getIntWidth(locals_.at(lv.base.name).symirType);
        if (srcWidth <= 32 && targetWidth > 32) {
          indent();
          out_ << "i64.extend_i32_s\n";
        } else if (srcWidth > 32 && targetWidth <= 32) {
          indent();
          out_ << "i32.wrap_i64\n";
        }
      }
    } else {
      emitCoef(std::get<Coef>(sv), targetWidth);
    }
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
                  if (locals_.count(id.name)) {
                    std::uint32_t srcWidth = getIntWidth(locals_.at(id.name).symirType);
                    if (srcWidth > 32) {
                      indent();
                      out_ << "i32.wrap_i64\n";
                    }
                  }
                },
                arg
            );
          }
        },
        idx
    );
  }

  void WasmBackend::emitInitVal(const InitVal &iv, const TypePtr &type, std::uint32_t baseOffset) {
    if (iv.kind == InitVal::Kind::Int || iv.kind == InitVal::Kind::Sym) {
      if (auto at = std::get_if<ArrayType>(&type->v)) {
        std::uint32_t elemSize = getTypeSize(at->elem);
        for (std::uint64_t i = 0; i < at->size; ++i) {
          emitInitVal(iv, at->elem, baseOffset - i * elemSize);
        }
      } else if (auto st = std::get_if<StructType>(&type->v)) {
        if (structLayouts_.count(st->name.name)) {
          const auto &sinfo = structLayouts_.at(st->name.name);
          for (const auto &fname: sinfo.fieldNames) {
            const auto &finfo = sinfo.fields.at(fname);
            emitInitVal(iv, finfo.type, baseOffset - finfo.offset);
          }
        }
      } else {
        indent();
        out_ << "local.get $__old_sp\n";
        indent();
        out_ << "i32.const " << baseOffset << "\n";
        indent();
        out_ << "i32.sub\n";

        if (iv.kind == InitVal::Kind::Int) {
          indent();
          out_ << (getIntWidth(type) <= 32 ? "i32.const " : "i64.const ")
               << std::get<IntLit>(iv.value).value << "\n";
        } else {
          // Symbol
          const auto &sid = std::get<SymId>(iv.value);
          indent();
          out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, sid.name)) << "\n";
          std::uint32_t srcWidth = 32;
          if (syms_.count(sid.name)) {
            srcWidth = getIntWidth(syms_.at(sid.name));
          }
          if (srcWidth <= 32 && getIntWidth(type) > 32) {
            indent();
            out_ << "i64.extend_i32_s\n";
          } else if (srcWidth > 32 && getIntWidth(type) <= 32) {
            indent();
            out_ << "i32.wrap_i64\n";
          }
        }

        uint32_t w = getIntWidth(type);
        indent();
        out_ << (w <= 8 ? "i32.store8"
                        : (w <= 16 ? "i32.store16" : (w <= 32 ? "i32.store" : "i64.store")))
             << "\n";
      }
    } else if (iv.kind == InitVal::Kind::Aggregate) {
      const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
      if (auto at = std::get_if<ArrayType>(&type->v)) {
        std::uint32_t elemSize = getTypeSize(at->elem);
        for (size_t i = 0; i < elements.size(); ++i) {
          emitInitVal(*elements[i], at->elem, baseOffset - i * elemSize);
        }
      } else if (auto st = std::get_if<StructType>(&type->v)) {
        if (structLayouts_.count(st->name.name)) {
          const auto &sinfo = structLayouts_.at(st->name.name);
          for (size_t i = 0; i < elements.size() && i < sinfo.fieldNames.size(); ++i) {
            const auto &fname = sinfo.fieldNames[i];
            const auto &finfo = sinfo.fields.at(fname);
            emitInitVal(*elements[i], finfo.type, baseOffset - finfo.offset);
          }
        }
      }
    }
  }

  void WasmBackend::emit(const Program &prog) {
    computeLayouts(prog);
    if (!noModuleTags_) {
      out_ << "(module\n";
      indent_level_++;
    }

    for (const auto &f: prog.funs) {
      for (const auto &s: f.syms) {
        indent();
        out_ << "(import \"" << stripSigil(f.name.name) << "\" \"" << stripSigil(s.name.name)
             << "\" (func " << mangleName(getMangledSymbolName(f.name.name, s.name.name))
             << " (result " << getWasmType(s.type) << ")))\n";
      }
    }

    indent();
    out_ << "(memory 16)\n"; // 1MB
    indent();
    out_ << "(global $__stack_pointer (mut i32) (i32.const 1048576))\n";

    for (const auto &f: prog.funs) {
      curFuncName_ = f.name.name;
      locals_.clear();
      syms_.clear();
      stackSize_ = 0;

      for (const auto &s: f.syms) {
        syms_[s.name.name] = s.type;
      }

      for (const auto &p: f.params) {
        locals_[p.name.name] = {getWasmType(p.type), true, getIntWidth(p.type), false, 0, p.type};
      }
      for (const auto &l: f.lets) {
        bool isAgg = std::holds_alternative<StructType>(l.type->v) ||
                     std::holds_alternative<ArrayType>(l.type->v);
        if (isAgg) {
          std::uint32_t size = getTypeSize(l.type);
          if (stackSize_ % 8 != 0)
            stackSize_ += 8 - (stackSize_ % 8);
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
            emitSignExtend(getIntWidth(l.type), (locals_[l.name.name].wasmType == "i32" ? 32 : 64));
            indent();
            out_ << "local.set " << mangleName(l.name.name) << "\n";
          } else if (l.init->kind == InitVal::Kind::Local) {
            emitLValue({std::get<LocalId>(l.init->value), {}, l.init->span}, false);
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
      out_ << "(loop $__symir_dispatch_loop\n";
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
                  if (locals_.count(arg.lhs.base.name)) {
                    const auto &info = locals_.at(arg.lhs.base.name);
                    if (info.isAggregate || !arg.lhs.accesses.empty()) {
                      emitAddress(arg.lhs);
                      TypePtr curType = info.symirType;
                      for (const auto &acc: arg.lhs.accesses) {
                        if (std::holds_alternative<AccessIndex>(acc)) {
                          if (auto at = std::get_if<ArrayType>(&curType->v))
                            curType = at->elem;
                        } else if (auto af = std::get_if<AccessField>(&acc)) {
                          if (auto st = std::get_if<StructType>(&curType->v)) {
                            auto &fld = af->field;
                            if (structLayouts_.count(st->name.name) &&
                                structLayouts_.at(st->name.name).fields.count(fld))
                              curType = structLayouts_.at(st->name.name).fields.at(fld).type;
                          }
                        }
                      }
                      std::uint32_t width = getIntWidth(curType);
                      emitExpr(arg.rhs, width);
                      indent();
                      if (width <= 8)
                        out_ << "i32.store8\n";
                      else if (width <= 16)
                        out_ << "i32.store16\n";
                      else if (width <= 32)
                        out_ << "i32.store\n";
                      else
                        out_ << "i64.store\n";
                    } else {
                      emitExpr(arg.rhs, info.bitwidth);
                      indent();
                      out_ << "local.set " << mangleName(arg.lhs.base.name) << "\n";
                    }
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
                  out_ << "br $__symir_dispatch_loop\n";
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
                  out_ << "br $__symir_dispatch_loop\n";
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
      out_ << ") ;; dispatch loop\n";

      if (f.retType && !f.blocks.empty()) {
        indent();
        out_ << (getIntWidth(f.retType) <= 32 ? "i32.const 0\n" : "i64.const 0\n");
      }

      indent_level_--;
      indent();
      out_ << ")\n\n";
      indent();
      std::string exportedName = stripSigil(f.name.name);
      if (exportedName == "main")
        exportedName = "symir_main";
      out_ << "(export \"" << exportedName << "\" (func " << mangleName(f.name.name) << "))\n";
    }

    if (!noModuleTags_) {
      indent_level_--;
      out_ << ")\n";
    }
  }

} // namespace symir
