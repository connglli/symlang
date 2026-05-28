#include "backend/wasm_backend.hpp"
#include <algorithm>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include "analysis/type_utils.hpp"

namespace symir {

  // Format a double literal with enough precision to round-trip exactly.
  // Default operator<< uses 6 digits which silently truncates large floats
  // (e.g. 16777216 → "1.67772e+07" = 16777200). 17 digits is sufficient
  // for double per IEEE-754; the WAT parser accepts both fixed and
  // exponent forms.
  static std::string formatFloatLit(double v) {
    std::ostringstream os;
    os << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    std::string s = os.str();
    if (s.find_first_of(".eEnN") == std::string::npos)
      s += ".0";
    return s;
  }

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
          } else if constexpr (std::is_same_v<T, FloatType>) {
            return arg.kind == FloatType::Kind::F32 ? "f32" : "f64";
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
    if (auto ft = std::get_if<FloatType>(&type->v)) {
      return (ft->kind == FloatType::Kind::F32) ? 32 : 64;
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
          } else if constexpr (std::is_same_v<T, FloatType>) {
            return arg.kind == FloatType::Kind::F32 ? 4 : 8;
          } else if constexpr (std::is_same_v<T, StructType>) {
            if (structLayouts_.count(arg.name.name))
              return structLayouts_.at(arg.name.name).totalSize;
            return 0;
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            return arg.size * getTypeSize(arg.elem);
          } else if constexpr (std::is_same_v<T, PtrType>) {
            return 4; // WASM pointers are 32-bit (4 bytes)
          } else if constexpr (std::is_same_v<T, VecType>) {
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

  void WasmBackend::emitExpr(const Expr &expr, std::uint32_t targetWidth, bool isFloat) {
    emitAtom(expr.first, targetWidth, isFloat);

    for (const auto &t: expr.rest) {
      emitAtom(t.atom, targetWidth, isFloat);
      indent();
      if (isFloat) {
        out_ << (targetWidth <= 32 ? "f32." : "f64.") << (t.op == AddOp::Plus ? "add\n" : "sub\n");
      } else {
        if (targetWidth <= 32)
          out_ << (t.op == AddOp::Plus ? "i32.add\n" : "i32.sub\n");
        else
          out_ << (t.op == AddOp::Plus ? "i64.add\n" : "i64.sub\n");
        emitSignExtend(targetWidth, (targetWidth <= 32 ? 32 : 64));
      }
    }
  }

  void WasmBackend::emitAtom(const Atom &atom, std::uint32_t targetWidth, bool isFloat) {
    uint32_t wasmWidth = (targetWidth <= 32 ? 32 : 64);
    std::visit(
        [this, targetWidth, wasmWidth, isFloat](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, CoefAtom>) {
            emitCoef(arg.coef, targetWidth, isFloat);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            emitLValue(arg.rval, false);
            if (locals_.count(arg.rval.base.name)) {
              auto const &li = locals_.at(arg.rval.base.name);
              // Walk accesses to determine the actual loaded type, not the base
              // local's type (e.g. %s.tag where tag is i64 inside an aggregate %s).
              TypePtr srcType = li.symirType;
              for (const auto &acc: arg.rval.accesses) {
                if (std::get_if<AccessIndex>(&acc)) {
                  if (auto at = std::get_if<ArrayType>(&srcType->v))
                    srcType = at->elem;
                  else if (auto vt = std::get_if<VecType>(&srcType->v))
                    srcType = vt->elem;
                } else if (auto af = std::get_if<AccessField>(&acc)) {
                  if (auto st = std::get_if<StructType>(&srcType->v)) {
                    if (structLayouts_.count(st->name.name) &&
                        structLayouts_.at(st->name.name).fields.count(af->field))
                      srcType = structLayouts_.at(st->name.name).fields.at(af->field).type;
                  }
                }
              }
              std::uint32_t srcWidth = 32;
              bool srcIsFloat = false;
              if (auto bits = TypeUtils::getBitWidth(srcType)) {
                srcWidth = *bits;
              } else if (srcType && std::holds_alternative<FloatType>(srcType->v)) {
                srcIsFloat = true;
                srcWidth = (std::get<FloatType>(srcType->v).kind == FloatType::Kind::F32) ? 32 : 64;
              }
              if (srcIsFloat) {
                if (srcWidth == 32 && targetWidth == 64) {
                  indent();
                  out_ << "f64.promote_f32\n";
                }
              } else {
                if (srcWidth <= 32 && targetWidth > 32) {
                  indent();
                  out_ << "i64.extend_i32_s\n";
                } else if (srcWidth > 32 && targetWidth <= 32) {
                  indent();
                  out_ << "i32.wrap_i64\n";
                }
              }
            }
          } else if constexpr (std::is_same_v<T, OpAtom>) {
            if (isFloat) {
              emitCoef(arg.coef, targetWidth, isFloat);
              emitLValue(arg.rval, false);
              if (locals_.count(arg.rval.base.name)) {
                auto const &li = locals_.at(arg.rval.base.name);
                if (std::holds_alternative<FloatType>(li.symirType->v)) {
                  if (li.bitwidth == 32 && targetWidth == 64) {
                    indent();
                    out_ << "f64.promote_f32\n";
                  }
                }
              }
              std::string prefix = (targetWidth <= 32 ? "f32." : "f64.");
              if (arg.op == AtomOpKind::Mod) {
                // WebAssembly MVP has no f32.rem / f64.rem instruction.
                // Emit fmod inline: x - trunc(x/y) * y
                // Stack after emitCoef+emitLValue above: [x, y] — but we need
                // x and y each twice, so emit them fresh here.
                // Re-emit y (with optional f32→f64 promotion) as a helper.
                auto emitY = [&]() {
                  emitLValue(arg.rval, false);
                  if (locals_.count(arg.rval.base.name)) {
                    auto const &li = locals_.at(arg.rval.base.name);
                    if (std::holds_alternative<FloatType>(li.symirType->v)) {
                      if (li.bitwidth == 32 && targetWidth == 64) {
                        indent();
                        out_ << "f64.promote_f32\n";
                      }
                    }
                  }
                };
                // The preceding emitCoef/emitLValue already pushed [x, y] onto
                // the stack (used for the initial x below). Drop those now and
                // re-emit cleanly so the stack is deterministic.
                // Actually, since we need x twice we restructure entirely:
                // emitCoef/emitLValue above left [x, y] on stack; pop y with
                // local.set into a drop, but simpler: the two emits above are
                // effectively wasted — in WASM we must balance the stack.
                // Instead we emit a `drop` for y and `drop` for x (they were
                // pushed by the standard path above), then emit the full fmod.
                indent();
                out_ << "drop\n"; // drop y (emitLValue result)
                indent();
                out_ << "drop\n"; // drop x (emitCoef result)
                // Now emit fmod(x, y) = x - trunc(x/y) * y
                emitCoef(arg.coef, targetWidth, isFloat); // x
                emitCoef(arg.coef, targetWidth, isFloat); // x  (for division)
                emitY();                                  // y
                indent();
                out_ << prefix << "div\n"; // x/y
                indent();
                out_ << prefix << "trunc\n"; // trunc(x/y)
                emitY();                     // y
                indent();
                out_ << prefix << "mul\n"; // trunc(x/y)*y
                indent();
                out_ << prefix << "sub\n"; // x - trunc(x/y)*y
              } else {
                indent();
                switch (arg.op) {
                  case AtomOpKind::Mul:
                    out_ << prefix << "mul\n";
                    break;
                  case AtomOpKind::Div:
                    out_ << prefix << "div\n";
                    break;
                  default:
                    break;
                }
              }
            } else if (arg.op == AtomOpKind::LShr) {
              emitCoef(arg.coef, targetWidth, isFloat);
              emitMask(targetWidth, wasmWidth);
              emitLValue(arg.rval, false);
              indent();
              out_ << (wasmWidth == 32 ? "i32.shr_u\n" : "i64.shr_u\n");
            } else {
              emitCoef(arg.coef, targetWidth, isFloat);
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
            if (!isFloat)
              emitSignExtend(targetWidth, wasmWidth);
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            if (arg.maskExpr) {
              emitExpr(*arg.maskExpr, 32, false);
            } else {
              emitCond(*arg.cond);
            }
            indent();
            std::string typePrefix;
            if (isFloat)
              typePrefix = (targetWidth <= 32 ? "f32" : "f64");
            else
              typePrefix = (targetWidth <= 32 ? "i32" : "i64");

            out_ << "if (result " << typePrefix << ")\n";
            indent_level_++;
            emitSelectVal(arg.vtrue, targetWidth, isFloat);
            indent_level_--;
            indent();
            out_ << "else\n";
            indent_level_++;
            emitSelectVal(arg.vfalse, targetWidth, isFloat);
            indent_level_--;
            indent();
            out_ << "end\n";
          } else if constexpr (std::is_same_v<T, CmpAtom>) {
            TypePtr lhsType = getSelectValType(arg.lhs);
            TypePtr rhsType = getSelectValType(arg.rhs);
            bool isFloat = (lhsType && std::holds_alternative<FloatType>(lhsType->v)) ||
                           (rhsType && std::holds_alternative<FloatType>(rhsType->v));
            bool is64 = false;
            if (isFloat) {
              is64 = (lhsType && std::get_if<FloatType>(&lhsType->v) &&
                      std::get<FloatType>(lhsType->v).kind == FloatType::Kind::F64) ||
                     (rhsType && std::get_if<FloatType>(&rhsType->v) &&
                      std::get<FloatType>(rhsType->v).kind == FloatType::Kind::F64);
            } else {
              auto getWidth = [](const TypePtr &t) -> uint32_t {
                if (!t)
                  return 32;
                if (auto it = std::get_if<IntType>(&t->v)) {
                  if (it->kind == IntType::Kind::I64 || (it->bits && *it->bits > 32))
                    return 64;
                }
                return 32;
              };
              is64 = (getWidth(lhsType) > 32 || getWidth(rhsType) > 32);
            }
            uint32_t operandWidth = is64 ? 64 : 32;
            emitSelectVal(arg.lhs, operandWidth, isFloat);
            emitSelectVal(arg.rhs, operandWidth, isFloat);

            indent();
            std::string opStr;
            if (isFloat) {
              std::string prefix = (operandWidth <= 32 ? "f32." : "f64.");
              switch (arg.op) {
                case RelOp::EQ:
                  opStr = prefix + "eq";
                  break;
                case RelOp::NE:
                  opStr = prefix + "ne";
                  break;
                case RelOp::LT:
                  opStr = prefix + "lt";
                  break;
                case RelOp::LE:
                  opStr = prefix + "le";
                  break;
                case RelOp::GT:
                  opStr = prefix + "gt";
                  break;
                case RelOp::GE:
                  opStr = prefix + "ge";
                  break;
              }
            } else {
              std::string prefix = (operandWidth <= 32 ? "i32." : "i64.");
              switch (arg.op) {
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
            }
            out_ << opStr << "\n";
            if (targetWidth > 32) {
              indent();
              out_ << "i64.extend_i32_s\n";
            }
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            uint64_t arrSize = 0;
            TypePtr elemType = nullptr;
            auto rvTy = getLValueType(arg.rval);
            if (rvTy) {
              if (auto pt = std::get_if<PtrType>(&rvTy->v)) {
                if (auto at = std::get_if<ArrayType>(&pt->pointee->v)) {
                  arrSize = at->size;
                  elemType = at->elem;
                }
              }
            }
            uint32_t elemSize = elemType ? getTypeSize(elemType) : 1;

            emitLValue(arg.rval, false);
            indent();
            out_ << "local.tee $__ptr_temp\n";
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

            emitIndex(arg.index);
            indent();
            out_ << "local.tee $__idx_temp\n";
            indent();
            out_ << "local.get $__idx_temp\n";
            indent();
            out_ << "i32.const 0\n";
            indent();
            out_ << "i32.lt_s\n";
            indent();
            out_ << "local.get $__idx_temp\n";
            indent();
            out_ << "i32.const " << arrSize << "\n";
            indent();
            out_ << "i32.gt_s\n";
            indent();
            out_ << "i32.or\n";
            indent();
            out_ << "if\n";
            indent_level_++;
            indent();
            out_ << "unreachable\n";
            indent_level_--;
            indent();
            out_ << "end\n";

            indent();
            out_ << "local.get $__ptr_temp\n";
            indent();
            out_ << "local.get $__idx_temp\n";
            if (elemSize > 1) {
              indent();
              out_ << "i32.const " << elemSize << "\n";
              indent();
              out_ << "i32.mul\n";
            }
            indent();
            out_ << "i32.add\n";
          } else if constexpr (std::is_same_v<T, CallAtom>) {
            // [v0.2.2] Push arguments left-to-right then `call $name`.
            const IntrinsicDecl *intr = nullptr;
            if (prog_) {
              for (const auto &i: prog_->intrinsics)
                if (i.name.name == arg.callee.name) {
                  intr = &i;
                  break;
                }
            }
            // Determine the parameter types so we can pass argWidth correctly.
            std::vector<TypePtr> ptypes;
            if (intr) {
              for (const auto &p: intr->params)
                ptypes.push_back(p.type);
            } else if (prog_) {
              for (const auto &f: prog_->funs)
                if (f.name.name == arg.callee.name) {
                  for (const auto &p: f.params)
                    ptypes.push_back(p.type);
                  break;
                }
              if (ptypes.empty()) {
                for (const auto &d: prog_->extDecls)
                  if (d.name.name == arg.callee.name) {
                    for (const auto &p: d.params)
                      ptypes.push_back(p.type);
                    break;
                  }
              }
            }
            for (size_t i = 0; i < arg.args.size(); ++i) {
              uint32_t pw = 32;
              bool pf = false;
              if (i < ptypes.size() && ptypes[i]) {
                pw = getIntWidth(ptypes[i]);
                if (pw == 0) {
                  if (std::holds_alternative<FloatType>(ptypes[i]->v)) {
                    auto &ft = std::get<FloatType>(ptypes[i]->v);
                    pw = (ft.kind == FloatType::Kind::F32) ? 32 : 64;
                    pf = true;
                  } else {
                    pw = 32;
                  }
                }
              }
              emitExpr(*arg.args[i], pw, pf);
            }
            indent();
            if (intr) {
              auto rb = getIntWidth(intr->retType);
              out_ << "call " << intrinsicHelperName(arg.callee.name, rb) << "\n";
            } else {
              out_ << "call " << mangleName(arg.callee.name) << "\n";
            }
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            uint32_t fieldOffset = 0;
            auto rvTy = getLValueType(arg.rval);
            if (rvTy) {
              if (auto pt = std::get_if<PtrType>(&rvTy->v)) {
                if (auto st = std::get_if<StructType>(&pt->pointee->v)) {
                  if (structLayouts_.count(st->name.name)) {
                    const auto &sinfo = structLayouts_.at(st->name.name);
                    if (sinfo.fields.count(arg.field)) {
                      fieldOffset = sinfo.fields.at(arg.field).offset;
                    }
                  }
                }
              }
            }

            emitLValue(arg.rval, false);
            indent();
            out_ << "local.tee $__ptr_temp\n";
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

            if (fieldOffset > 0) {
              indent();
              out_ << "local.get $__ptr_temp\n";
              indent();
              out_ << "i32.const " << fieldOffset << "\n";
              indent();
              out_ << "i32.add\n";
            } else {
              indent();
              out_ << "local.get $__ptr_temp\n";
            }
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            emitLValue(arg.rval, false);
            if (isFloat) {
              indent();
              out_ << (targetWidth <= 32 ? "f32.neg\n" : "f64.neg\n");
            } else {
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
            }
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            // Return the WASM memory address of the lvalue (must be an aggregate/spilled local)
            emitAddress(arg.lv);
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            // Load through pointer: *ptr
            // Push ptr twice — first copy stays for the actual load,
            // second copy is used for the null check.
            const std::string &pname = arg.rval.base.name;
            // If pname was address-taken, it lives on the shadow stack rather
            // than as a WASM local; fetch its value from there.
            auto emitPushPtr = [&]() {
              if (locals_.count(pname) && locals_.at(pname).isAggregate) {
                const auto &pinfo = locals_.at(pname);
                indent();
                out_ << "local.get $__old_sp\n";
                indent();
                out_ << "i32.const " << pinfo.offset << "\n";
                indent();
                out_ << "i32.sub\n";
                indent();
                out_ << "i32.load\n";
              } else {
                indent();
                out_ << "local.get " << mangleName(pname) << "\n";
              }
            };
            // Null check: push ptr for null test, then keep first for load
            emitPushPtr();
            emitPushPtr();
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
            // stack: [ptr_for_load]; determine pointee type for load instruction
            uint32_t loadWidth = targetWidth;
            bool loadIsFloat = isFloat;
            if (locals_.count(pname)) {
              const auto &info = locals_.at(pname);
              if (auto pt = std::get_if<PtrType>(&info.symirType->v)) {
                if (auto bits = TypeUtils::getBitWidth(pt->pointee)) {
                  loadWidth = *bits;
                  loadIsFloat = false;
                } else if (pt->pointee && std::holds_alternative<FloatType>(pt->pointee->v)) {
                  loadIsFloat = true;
                  loadWidth =
                      (std::get<FloatType>(pt->pointee->v).kind == FloatType::Kind::F32) ? 32 : 64;
                }
              }
            }
            indent();
            if (loadIsFloat) {
              out_ << (loadWidth <= 32 ? "f32.load\n" : "f64.load\n");
            } else {
              out_
                  << (loadWidth <= 8    ? "i32.load8_s\n"
                      : loadWidth <= 16 ? "i32.load16_s\n"
                      : loadWidth <= 32 ? "i32.load\n"
                                        : "i64.load\n");
            }
            // Sign-extend if needed
            if (!loadIsFloat && loadWidth <= 32 && targetWidth > 32) {
              indent();
              out_ << "i64.extend_i32_s\n";
            }
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            std::uint32_t srcWidth = 32;
            bool srcIsFloat = false;
            std::visit(
                [&](auto &&src) {
                  using S = std::decay_t<decltype(src)>;
                  if constexpr (std::is_same_v<S, IntLit>) {
                    srcWidth = (src.value > INT32_MAX || src.value < INT32_MIN) ? 64 : 32;
                    indent();
                    out_ << (srcWidth <= 32 ? "i32.const " : "i64.const ") << src.value << "\n";
                  } else if constexpr (std::is_same_v<S, FloatLit>) {
                    srcWidth = 64;
                    srcIsFloat = true;
                    indent();
                    out_ << "f64.const " << formatFloatLit(src.value) << "\n";
                  } else if constexpr (std::is_same_v<S, SymId>) {
                    indent();
                    out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, src.name))
                         << "\n";
                    srcWidth = 32;
                    if (syms_.count(src.name)) {
                      srcWidth = getIntWidth(syms_.at(src.name));
                      if (std::holds_alternative<FloatType>(syms_.at(src.name)->v)) {
                        srcIsFloat = true;
                        srcWidth = (std::get<FloatType>(syms_.at(src.name)->v).kind ==
                                    FloatType::Kind::F32)
                                       ? 32
                                       : 64;
                      }
                    }
                  } else {
                    emitLValue(src, false);
                    if (locals_.count(src.base.name)) {
                      auto const &li = locals_.at(src.base.name);
                      // Walk accesses so cast-from-field uses the field's
                      // type, not the base local's (e.g. %s.tag where tag is i64).
                      TypePtr at_type = li.symirType;
                      for (const auto &acc: src.accesses) {
                        if (std::get_if<AccessIndex>(&acc)) {
                          if (auto at = std::get_if<ArrayType>(&at_type->v))
                            at_type = at->elem;
                          else if (auto vt = std::get_if<VecType>(&at_type->v))
                            at_type = vt->elem;
                        } else if (auto af = std::get_if<AccessField>(&acc)) {
                          if (auto st = std::get_if<StructType>(&at_type->v)) {
                            if (structLayouts_.count(st->name.name) &&
                                structLayouts_.at(st->name.name).fields.count(af->field))
                              at_type = structLayouts_.at(st->name.name).fields.at(af->field).type;
                          }
                        }
                      }
                      if (auto bits = TypeUtils::getBitWidth(at_type)) {
                        srcWidth = *bits;
                      } else if (at_type && std::holds_alternative<FloatType>(at_type->v)) {
                        srcIsFloat = true;
                        srcWidth = (std::get<FloatType>(at_type->v).kind == FloatType::Kind::F32)
                                       ? 32
                                       : 64;
                      }
                    }
                  }
                },
                arg.src
            );

            bool dstIsFloat = std::holds_alternative<FloatType>(arg.dstType->v);
            std::uint32_t dstWidth = 32;
            if (dstIsFloat) {
              dstWidth =
                  (std::get<FloatType>(arg.dstType->v).kind == FloatType::Kind::F32) ? 32 : 64;
            } else {
              dstWidth = getIntWidth(arg.dstType);
            }

            indent();
            if (srcIsFloat && dstIsFloat) {
              if (srcWidth == 32 && dstWidth == 64)
                out_ << "f64.promote_f32\n";
              else if (srcWidth == 64 && dstWidth == 32)
                out_ << "f32.demote_f64\n";
            } else if (srcIsFloat && !dstIsFloat) {
              if (dstWidth <= 32)
                out_ << (srcWidth == 32 ? "i32.trunc_f32_s\n" : "i32.trunc_f64_s\n");
              else
                out_ << (srcWidth == 32 ? "i64.trunc_f32_s\n" : "i64.trunc_f64_s\n");
            } else if (!srcIsFloat && dstIsFloat) {
              if (srcWidth <= 32)
                out_ << (dstWidth == 32 ? "f32.convert_i32_s\n" : "f64.convert_i32_s\n");
              else
                out_ << (dstWidth == 32 ? "f32.convert_i64_s\n" : "f64.convert_i64_s\n");
            } else {
              // BV -> BV
              if (srcWidth <= 32 && (targetWidth > 32 || dstWidth > 32)) {
                if (wasmWidth == 64) {
                  out_ << "i64.extend_i32_s\n";
                }
              } else if (srcWidth > 32 && (targetWidth <= 32 || dstWidth <= 32)) {
                if (wasmWidth == 32) {
                  out_ << "i32.wrap_i64\n";
                }
              }
            }
            if (!dstIsFloat)
              emitSignExtend(targetWidth, wasmWidth);
          }
        },
        atom.v
    );
  }

  void WasmBackend::emitCond(const Cond &cond) {
    std::uint32_t width = 32;
    bool isFloat = false;
    auto needs64 = [&](const Expr &e) {
      auto atomNeeds64 = [&](const Atom &a) {
        if (std::holds_alternative<CastAtom>(a.v)) {
          auto const &ca = std::get<CastAtom>(a.v);
          if (std::holds_alternative<FloatType>(ca.dstType->v)) {
            isFloat = true;
            if (std::get<FloatType>(ca.dstType->v).kind == FloatType::Kind::F64)
              return true;
          }
          if (getIntWidth(ca.dstType) > 32)
            return true;
        } else if (std::holds_alternative<RValueAtom>(a.v)) {
          auto &lv = std::get<RValueAtom>(a.v).rval;
          if (locals_.count(lv.base.name)) {
            auto const &li = locals_.at(lv.base.name);
            if (std::holds_alternative<FloatType>(li.symirType->v)) {
              isFloat = true;
              if (std::get<FloatType>(li.symirType->v).kind == FloatType::Kind::F64)
                return true;
            }
            if (li.bitwidth > 32)
              return true;
          }
        } else if (std::holds_alternative<CoefAtom>(a.v)) {
          auto &coef = std::get<CoefAtom>(a.v).coef;
          if (std::holds_alternative<FloatLit>(coef)) {
            isFloat = true;
            return true; // assume f64 for lit if 64 bits target?
          }
          if (std::holds_alternative<LocalOrSymId>(coef)) {
            auto &id = std::get<LocalOrSymId>(coef);
            auto name = std::visit([](auto &&v) { return v.name; }, id);
            if (locals_.count(name)) {
              auto const &li = locals_.at(name);
              if (std::holds_alternative<FloatType>(li.symirType->v)) {
                isFloat = true;
                if (std::get<FloatType>(li.symirType->v).kind == FloatType::Kind::F64)
                  return true;
              }
              if (li.bitwidth > 32)
                return true;
            } else if (syms_.count(name)) {
              auto const &st = syms_.at(name);
              if (std::holds_alternative<FloatType>(st->v)) {
                isFloat = true;
                if (std::get<FloatType>(st->v).kind == FloatType::Kind::F64)
                  return true;
              }
              if (getIntWidth(st) > 32)
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

    emitExpr(cond.lhs, width, isFloat);
    emitExpr(cond.rhs, width, isFloat);
    indent();
    std::string opStr;
    if (isFloat) {
      std::string prefix = (width <= 32 ? "f32." : "f64.");
      switch (cond.op) {
        case RelOp::EQ:
          opStr = prefix + "eq";
          break;
        case RelOp::NE:
          opStr = prefix + "ne";
          break;
        case RelOp::LT:
          opStr = prefix + "lt";
          break;
        case RelOp::LE:
          opStr = prefix + "le";
          break;
        case RelOp::GT:
          opStr = prefix + "gt";
          break;
        case RelOp::GE:
          opStr = prefix + "ge";
          break;
      }
    } else {
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
        } else if (auto vt = std::get_if<VecType>(&curType->v)) {
          std::uint32_t elemSize = getTypeSize(vt->elem);
          emitIndex(ai->index);
          indent();
          out_ << "i32.const " << elemSize << "\n";
          indent();
          out_ << "i32.mul\n";
          indent();
          out_ << "i32.add\n";
          curType = vt->elem;
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
            else if (auto vt = std::get_if<VecType>(&curType->v))
              curType = vt->elem;
          } else if (auto af = std::get_if<AccessField>(&acc)) {
            if (auto st = std::get_if<StructType>(&curType->v)) {
              auto &fld = af->field;
              if (structLayouts_.count(st->name.name) &&
                  structLayouts_.at(st->name.name).fields.count(fld))
                curType = structLayouts_.at(st->name.name).fields.at(fld).type;
            }
          }
        }
        std::uint32_t width = 0;
        bool valIsFloat = false;
        if (auto bits = TypeUtils::getBitWidth(curType)) {
          width = *bits;
        } else if (curType && std::holds_alternative<FloatType>(curType->v)) {
          valIsFloat = true;
          width = (std::get<FloatType>(curType->v).kind == FloatType::Kind::F32) ? 32 : 64;
        }

        if (valIsFloat) {
          out_ << (width == 32 ? "f32.load" : "f64.load") << "\n";
        } else {
          out_ << (width <= 8
                       ? "i32.load8_s"
                       : (width <= 16 ? "i32.load16_s" : (width <= 32 ? "i32.load" : "i64.load")))
               << "\n";
        }
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

  void WasmBackend::emitCoef(const Coef &coef, std::uint32_t targetWidth, bool isFloat) {
    uint32_t wasmWidth = (targetWidth <= 32 ? 32 : 64);
    std::visit(
        [this, targetWidth, wasmWidth, isFloat](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            indent();
            if (isFloat) {
              out_ << (wasmWidth == 32 ? "f32.const " : "f64.const ") << arg.value << ".0\n";
            } else {
              out_ << (wasmWidth == 32 ? "i32.const " : "i64.const ") << arg.value << "\n";
            }
          } else if constexpr (std::is_same_v<T, FloatLit>) {
            indent();
            out_ << (wasmWidth == 32 ? "f32.const " : "f64.const ") << formatFloatLit(arg.value)
                 << "\n";
          } else if constexpr (std::is_same_v<T, NullLit>) {
            // null pointer = 0 as i32 (WASM pointers are 32-bit)
            indent();
            out_ << "i32.const 0\n";
          } else {
            std::visit(
                [this, targetWidth](auto &&id) {
                  using ID = std::decay_t<decltype(id)>;
                  if constexpr (std::is_same_v<ID, SymId>) {
                    indent();
                    out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, id.name))
                         << "\n";
                    std::uint32_t srcWidth = 32;
                    bool srcIsFloat = false;
                    if (syms_.count(id.name)) {
                      srcWidth = getIntWidth(syms_.at(id.name));
                      if (std::holds_alternative<FloatType>(syms_.at(id.name)->v))
                        srcIsFloat = true;
                    }
                    if (srcIsFloat) {
                      if (srcWidth == 32 && targetWidth == 64) {
                        indent();
                        out_ << "f64.promote_f32\n";
                      }
                    } else {
                      if (srcWidth <= 32 && targetWidth > 32) {
                        indent();
                        out_ << "i64.extend_i32_s\n";
                      } else if (srcWidth > 32 && targetWidth <= 32) {
                        indent();
                        out_ << "i32.wrap_i64\n";
                      }
                    }
                  } else {
                    indent();
                    out_ << "local.get " << mangleName(id.name) << "\n";
                    if (locals_.count(id.name)) {
                      auto const &li = locals_.at(id.name);
                      std::uint32_t srcWidth = li.bitwidth;
                      if (std::holds_alternative<FloatType>(li.symirType->v)) {
                        if (srcWidth == 32 && targetWidth == 64) {
                          indent();
                          out_ << "f64.promote_f32\n";
                        }
                      } else {
                        if (srcWidth <= 32 && targetWidth > 32) {
                          indent();
                          out_ << "i64.extend_i32_s\n";
                        } else if (srcWidth > 32 && targetWidth <= 32) {
                          indent();
                          out_ << "i32.wrap_i64\n";
                        }
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
    if (!isFloat)
      emitSignExtend(targetWidth, wasmWidth);
  }

  void WasmBackend::emitSelectVal(const SelectVal &sv, std::uint32_t targetWidth, bool isFloat) {
    if (std::holds_alternative<RValue>(sv)) {
      const auto &lv = std::get<RValue>(sv);
      emitLValue(lv, false);
      TypePtr ty = getLValueType(lv);
      if (ty) {
        if (std::holds_alternative<FloatType>(ty->v)) {
          auto ft = std::get<FloatType>(ty->v);
          uint32_t srcWidth = (ft.kind == FloatType::Kind::F32) ? 32 : 64;
          if (srcWidth == 32 && targetWidth > 32) {
            indent();
            out_ << "f64.promote_f32\n";
          } else if (srcWidth == 64 && targetWidth <= 32) {
            indent();
            out_ << "f32.demote_f64\n";
          }
        } else {
          std::uint32_t srcWidth = getIntWidth(ty);
          if (srcWidth <= 32 && targetWidth > 32) {
            indent();
            out_ << "i64.extend_i32_s\n";
          } else if (srcWidth > 32 && targetWidth <= 32) {
            indent();
            out_ << "i32.wrap_i64\n";
          }
        }
      }
    } else {
      emitCoef(std::get<Coef>(sv), targetWidth, isFloat);
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
                  using ID = std::decay_t<decltype(id)>;
                  if constexpr (std::is_same_v<ID, SymId>) {
                    indent();
                    out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, id.name))
                         << "\n";
                    // Indices must be i32. If sym is i64, wrap it.
                    std::uint32_t srcWidth = 32;
                    if (syms_.count(id.name)) {
                      srcWidth = getIntWidth(syms_.at(id.name));
                    }
                    if (srcWidth > 32) {
                      indent();
                      out_ << "i32.wrap_i64\n";
                    }
                  } else {
                    indent();
                    out_ << "local.get " << mangleName(id.name) << "\n";
                    if (locals_.count(id.name)) {
                      std::uint32_t srcWidth = getIntWidth(locals_.at(id.name).symirType);
                      if (srcWidth > 32) {
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
        idx
    );
  }

  void WasmBackend::emitInitVal(const InitVal &iv, const TypePtr &type, std::uint32_t baseOffset) {
    if (auto at = std::get_if<ArrayType>(&type->v)) {
      std::uint32_t elemSize = getTypeSize(at->elem);
      if (iv.kind == InitVal::Kind::Aggregate) {
        const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
        for (size_t i = 0; i < elements.size() && i < at->size; ++i) {
          emitInitVal(*elements[i], at->elem, baseOffset - i * elemSize);
        }
      } else if (iv.kind == InitVal::Kind::Local) {
        const auto &lid = std::get<LocalId>(iv.value);
        const auto &srcInfo = locals_.at(lid.name);
        emitCopy(type, baseOffset, lid.name, srcInfo.offset);
      } else {
        for (std::uint64_t i = 0; i < at->size; ++i) {
          emitInitVal(iv, at->elem, baseOffset - i * elemSize);
        }
      }
    } else if (auto vt = std::get_if<VecType>(&type->v)) {
      std::uint32_t elemSize = getTypeSize(vt->elem);
      if (iv.kind == InitVal::Kind::Aggregate) {
        const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
        for (size_t i = 0; i < elements.size() && i < vt->size; ++i) {
          emitInitVal(*elements[i], vt->elem, baseOffset - i * elemSize);
        }
      } else if (iv.kind == InitVal::Kind::Sym) {
        const auto &sid = std::get<SymId>(iv.value);
        for (std::uint64_t i = 0; i < vt->size; ++i) {
          indent();
          out_ << "local.get $__old_sp\n";
          indent();
          out_ << "i32.const " << (baseOffset - i * elemSize) << "\n";
          indent();
          out_ << "i32.sub\n";

          indent();
          out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, sid.name)) << "__" << i
               << "\n";

          std::uint32_t dstWidth = getIntWidth(vt->elem);
          bool valIsFloat = std::holds_alternative<FloatType>(vt->elem->v);

          indent();
          if (valIsFloat) {
            out_ << (dstWidth == 32 ? "f32.store\n" : "f64.store\n");
          } else {
            out_ << (dstWidth <= 8
                         ? "i32.store8"
                         : (dstWidth <= 16 ? "i32.store16"
                                           : (dstWidth <= 32 ? "i32.store" : "i64.store")))
                 << "\n";
          }
        }
      } else if (iv.kind == InitVal::Kind::Local) {
        const auto &lid = std::get<LocalId>(iv.value);
        const auto &srcInfo = locals_.at(lid.name);
        emitCopy(type, baseOffset, lid.name, srcInfo.offset);
      } else {
        for (std::uint64_t i = 0; i < vt->size; ++i) {
          emitInitVal(iv, vt->elem, baseOffset - i * elemSize);
        }
      }
    } else if (auto st = std::get_if<StructType>(&type->v)) {
      if (structLayouts_.count(st->name.name)) {
        const auto &sinfo = structLayouts_.at(st->name.name);
        if (iv.kind == InitVal::Kind::Aggregate) {
          const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
          for (size_t i = 0; i < elements.size() && i < sinfo.fieldNames.size(); ++i) {
            const auto &fname = sinfo.fieldNames[i];
            const auto &finfo = sinfo.fields.at(fname);
            emitInitVal(*elements[i], finfo.type, baseOffset - finfo.offset);
          }
        } else if (iv.kind == InitVal::Kind::Local) {
          const auto &lid = std::get<LocalId>(iv.value);
          const auto &srcInfo = locals_.at(lid.name);
          emitCopy(type, baseOffset, lid.name, srcInfo.offset);
        } else {
          for (const auto &fname: sinfo.fieldNames) {
            const auto &finfo = sinfo.fields.at(fname);
            emitInitVal(iv, finfo.type, baseOffset - finfo.offset);
          }
        }
      }
    } else {
      // Leaf types (scalar or pointer stack slots)
      indent();
      out_ << "local.get $__old_sp\n";
      indent();
      out_ << "i32.const " << baseOffset << "\n";
      indent();
      out_ << "i32.sub\n";

      if (iv.kind == InitVal::Kind::Int) {
        indent();
        if (std::holds_alternative<FloatType>(type->v)) {
          out_ << (getIntWidth(type) <= 32 ? "f32.const " : "f64.const ")
               << std::get<IntLit>(iv.value).value << ".0\n";
        } else {
          out_ << (getIntWidth(type) <= 32 ? "i32.const " : "i64.const ")
               << std::get<IntLit>(iv.value).value << "\n";
        }
      } else if (iv.kind == InitVal::Kind::Float) {
        indent();
        out_ << (getIntWidth(type) <= 32 ? "f32.const " : "f64.const ")
             << formatFloatLit(std::get<FloatLit>(iv.value).value) << "\n";
      } else if (iv.kind == InitVal::Kind::Sym) {
        const auto &sid = std::get<SymId>(iv.value);
        indent();
        out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, sid.name)) << "\n";
        std::uint32_t srcWidth = 32;
        bool srcIsFloat = false;
        if (syms_.count(sid.name)) {
          srcWidth = getIntWidth(syms_.at(sid.name));
          if (std::holds_alternative<FloatType>(syms_.at(sid.name)->v))
            srcIsFloat = true;
        }
        if (!srcIsFloat) {
          if (srcWidth <= 32 && getIntWidth(type) > 32) {
            indent();
            out_ << "i64.extend_i32_s\n";
          } else if (srcWidth > 32 && getIntWidth(type) <= 32) {
            indent();
            out_ << "i32.wrap_i64\n";
          }
        } else {
          if (srcWidth == 32 && getIntWidth(type) == 64) {
            indent();
            out_ << "f64.promote_f32\n";
          }
        }
      } else if (iv.kind == InitVal::Kind::Null) {
        indent();
        out_ << "i32.const 0\n";
      } else if (iv.kind == InitVal::Kind::Local) {
        const auto &lid = std::get<LocalId>(iv.value);
        const auto &srcInfo = locals_.at(lid.name);
        if (srcInfo.isAggregate) {
          indent();
          out_ << "local.get $__old_sp\n";
          indent();
          out_ << "i32.const " << srcInfo.offset << "\n";
          indent();
          out_ << "i32.sub\n";
          std::uint32_t width = 32;
          bool valIsFloat = false;
          if (auto bits = TypeUtils::getBitWidth(type)) {
            width = *bits;
          } else if (type && std::holds_alternative<FloatType>(type->v)) {
            valIsFloat = true;
            width = (std::get<FloatType>(type->v).kind == FloatType::Kind::F32) ? 32 : 64;
          }
          indent();
          if (valIsFloat) {
            out_ << (width == 32 ? "f32.load\n" : "f64.load\n");
          } else {
            out_ << (width <= 8
                         ? "i32.load8_u"
                         : (width <= 16 ? "i32.load16_u" : (width <= 32 ? "i32.load" : "i64.load")))
                 << "\n";
          }
        } else {
          indent();
          out_ << "local.get " << mangleName(lid.name) << "\n";
        }
      } else if (iv.kind == InitVal::Kind::Atom) {
        const auto &atom = std::get<AtomPtr>(iv.value);
        std::uint32_t width = 32;
        bool valIsFloat = false;
        if (auto bits = TypeUtils::getBitWidth(type)) {
          width = *bits;
        } else if (type && std::holds_alternative<FloatType>(type->v)) {
          valIsFloat = true;
          width = (std::get<FloatType>(type->v).kind == FloatType::Kind::F32) ? 32 : 64;
        }
        emitAtom(*atom, width, valIsFloat);
      } else {
        // Undef / default
        indent();
        if (std::holds_alternative<FloatType>(type->v)) {
          out_ << (getIntWidth(type) <= 32 ? "f32.const 0.0\n" : "f64.const 0.0\n");
        } else {
          out_ << (getIntWidth(type) <= 32 ? "i32.const 0\n" : "i64.const 0\n");
        }
      }

      std::uint32_t width = 0;
      bool valIsFloat = false;
      if (auto bits = TypeUtils::getBitWidth(type)) {
        width = *bits;
      } else if (type && std::holds_alternative<FloatType>(type->v)) {
        valIsFloat = true;
        width = (std::get<FloatType>(type->v).kind == FloatType::Kind::F32) ? 32 : 64;
      } else if (type && std::holds_alternative<PtrType>(type->v)) {
        width = 32;
      }

      indent();
      if (valIsFloat) {
        out_ << (width == 32 ? "f32.store\n" : "f64.store\n");
      } else {
        out_ << (width <= 8
                     ? "i32.store8"
                     : (width <= 16 ? "i32.store16" : (width <= 32 ? "i32.store" : "i64.store")))
             << "\n";
      }
    }
  }

  void WasmBackend::emitCopy(
      const TypePtr &type, std::uint32_t dstOffset, const std::string &srcName,
      std::uint32_t srcOffset
  ) {
    if (auto at = std::get_if<ArrayType>(&type->v)) {
      std::uint32_t elemSize = getTypeSize(at->elem);
      for (std::uint64_t i = 0; i < at->size; ++i) {
        emitCopy(at->elem, dstOffset - i * elemSize, srcName, srcOffset - i * elemSize);
      }
    } else if (auto vt = std::get_if<VecType>(&type->v)) {
      std::uint32_t elemSize = getTypeSize(vt->elem);
      for (std::uint64_t i = 0; i < vt->size; ++i) {
        emitCopy(vt->elem, dstOffset - i * elemSize, srcName, srcOffset - i * elemSize);
      }
    } else if (auto st = std::get_if<StructType>(&type->v)) {
      if (structLayouts_.count(st->name.name)) {
        const auto &sinfo = structLayouts_.at(st->name.name);
        for (const auto &fname: sinfo.fieldNames) {
          const auto &finfo = sinfo.fields.at(fname);
          emitCopy(finfo.type, dstOffset - finfo.offset, srcName, srcOffset - finfo.offset);
        }
      }
    } else {
      // Leaf copy
      indent();
      out_ << "local.get $__old_sp\n";
      indent();
      out_ << "i32.const " << dstOffset << "\n";
      indent();
      out_ << "i32.sub\n";

      // Load source
      const auto &srcInfo = locals_.at(srcName);
      if (srcInfo.isAggregate) {
        indent();
        out_ << "local.get $__old_sp\n";
        indent();
        out_ << "i32.const " << srcOffset << "\n";
        indent();
        out_ << "i32.sub\n";

        std::uint32_t width = 0;
        bool valIsFloat = false;
        if (auto bits = TypeUtils::getBitWidth(type)) {
          width = *bits;
        } else if (type && std::holds_alternative<FloatType>(type->v)) {
          valIsFloat = true;
          width = (std::get<FloatType>(type->v).kind == FloatType::Kind::F32) ? 32 : 64;
        } else if (type && std::holds_alternative<PtrType>(type->v)) {
          width = 32;
        }

        indent();
        if (valIsFloat) {
          out_ << (width == 32 ? "f32.load\n" : "f64.load\n");
        } else {
          out_ << (width <= 8
                       ? "i32.load8_u"
                       : (width <= 16 ? "i32.load16_u" : (width <= 32 ? "i32.load" : "i64.load")))
               << "\n";
        }
      } else {
        indent();
        out_ << "local.get " << mangleName(srcName) << "\n";
      }

      // Store destination
      std::uint32_t width = 0;
      bool valIsFloat = false;
      if (auto bits = TypeUtils::getBitWidth(type)) {
        width = *bits;
      } else if (type && std::holds_alternative<FloatType>(type->v)) {
        valIsFloat = true;
        width = (std::get<FloatType>(type->v).kind == FloatType::Kind::F32) ? 32 : 64;
      } else if (type && std::holds_alternative<PtrType>(type->v)) {
        width = 32;
      }

      indent();
      if (valIsFloat) {
        out_ << (width == 32 ? "f32.store\n" : "f64.store\n");
      } else {
        out_ << (width <= 8
                     ? "i32.store8"
                     : (width <= 16 ? "i32.store16" : (width <= 32 ? "i32.store" : "i64.store")))
             << "\n";
      }
    }
  }

  std::string WasmBackend::intrinsicHelperName(const std::string &intrName, uint32_t bits) const {
    std::string base = intrName;
    if (!base.empty() && base[0] == '@')
      base.erase(0, 1);
    return "$_symir_" + base + "_i" + std::to_string(bits);
  }

  // [v0.2.2] §11.5 widening-and-mask. WASM has native i32/i64 popcnt/clz/ctz
  // but no abs/min/max for ints — those are emitted via select / branches.
  // The helper widens iN to i32 or i64, computes, and sign-masks back to N.
  void WasmBackend::emitIntrinsicHelper(const IntrinsicDecl &intr) {
    auto rb = getIntWidth(intr.retType);
    if (rb == 0)
      return;
    uint32_t N = rb;
    uint32_t W = (N <= 32) ? 32 : 64;
    std::string ity = (W == 32) ? "i32" : "i64";
    std::string name = intrinsicHelperName(intr.name.name, N);

    indent();
    out_ << "(func " << name;
    for (size_t i = 0; i < intr.params.size(); ++i) {
      out_ << " (param $a" << i << " " << ity << ")";
    }
    out_ << " (result " << ity << ")\n";
    indent_level_++;
    // Declare a scratch local for @clz/@ctz (UB-check on zero).
    const std::string &intrN = intr.name.name;
    if (intrN == "@clz" || intrN == "@ctz") {
      indent();
      out_ << "(local $tmp0 " << ity << ")\n";
    }

    auto pushArg = [&](size_t i) {
      indent();
      out_ << "local.get $a" << i << "\n";
    };
    // Mask top bits back to N (sign-extended).
    auto sextN = [&]() {
      if (N == W)
        return;
      indent();
      out_ << ity << ".const " << (W - N) << "\n";
      indent();
      out_ << ity << ".shl\n";
      indent();
      out_ << ity << ".const " << (W - N) << "\n";
      indent();
      out_ << ity << ".shr_s\n";
    };
    // Lower N-bit mask: (1 << N) - 1, only needed when N < W.
    auto pushMask = [&]() {
      if (N == W) {
        indent();
        out_ << ity << ".const -1\n";
      } else {
        indent();
        out_ << ity << ".const " << ((uint64_t(1) << N) - 1) << "\n";
      }
    };

    const std::string &n = intr.name.name;
    if (n == "@abs") {
      // if (a0 == INT_MIN_N) unreachable; r = a0 < 0 ? -a0 : a0;
      int64_t int_min_N = (N == 64) ? INT64_MIN : -(INT64_C(1) << (N - 1));
      pushArg(0);
      indent();
      out_ << ity << ".const " << int_min_N << "\n";
      indent();
      out_ << ity << ".eq\n";
      indent();
      out_ << "if\n";
      indent_level_++;
      indent();
      out_ << "unreachable\n";
      indent_level_--;
      indent();
      out_ << "end\n";
      // r = a0 < 0 ? -a0 : a0  →  push -a0, a0, (a0 < 0), select
      indent();
      out_ << ity << ".const 0\n";
      pushArg(0);
      indent();
      out_ << ity << ".sub\n";
      pushArg(0);
      pushArg(0);
      indent();
      out_ << ity << ".const 0\n";
      indent();
      out_ << ity << ".lt_s\n";
      indent();
      out_ << "select\n";
      sextN();
    } else if (n == "@min" || n == "@max") {
      bool isMin = (n == "@min");
      pushArg(0);
      pushArg(1);
      pushArg(0);
      pushArg(1);
      indent();
      out_ << ity << "." << (isMin ? "lt_s" : "gt_s") << "\n";
      indent();
      out_ << "select\n";
      sextN();
    } else if (n == "@popcount") {
      pushArg(0);
      pushMask();
      indent();
      out_ << ity << ".and\n";
      indent();
      out_ << ity << ".popcnt\n";
      sextN();
    } else if (n == "@clz" || n == "@ctz") {
      pushArg(0);
      pushMask();
      indent();
      out_ << ity << ".and\n";
      indent();
      out_ << "local.tee $tmp0\n";
      indent();
      out_ << ity << ".eqz\n";
      indent();
      out_ << "if\n";
      indent_level_++;
      indent();
      out_ << "unreachable\n";
      indent_level_--;
      indent();
      out_ << "end\n";
      indent();
      out_ << "local.get $tmp0\n";
      indent();
      out_ << ity << "." << (n == "@clz" ? "clz" : "ctz") << "\n";
      // For clz, subtract (W-N) so leading zeros above bit N-1 aren't counted.
      if (n == "@clz" && N != W) {
        indent();
        out_ << ity << ".const " << (W - N) << "\n";
        indent();
        out_ << ity << ".sub\n";
      }
      sextN();
    } else {
      indent();
      out_ << "unreachable\n";
    }

    indent_level_--;
    indent();
    out_ << ")\n";
  }

  void WasmBackend::emit(const Program &prog) {
    prog_ = &prog;
    computeLayouts(prog);
    if (!noModuleTags_) {
      out_ << "(module\n";
      indent_level_++;
    }

    for (const auto &f: prog.funs) {
      for (const auto &s: f.syms) {
        if (auto vt = std::get_if<VecType>(&s.type->v)) {
          for (std::uint64_t i = 0; i < vt->size; ++i) {
            indent();
            out_ << "(import \"" << stripSigil(f.name.name) << "\" \"" << stripSigil(s.name.name)
                 << "__" << i << "\" (func "
                 << mangleName(getMangledSymbolName(f.name.name, s.name.name)) << "__" << i
                 << " (result " << getWasmType(vt->elem) << ")))\n";
          }
        } else {
          indent();
          out_ << "(import \"" << stripSigil(f.name.name) << "\" \"" << stripSigil(s.name.name)
               << "\" (func " << mangleName(getMangledSymbolName(f.name.name, s.name.name))
               << " (result " << getWasmType(s.type) << ")))\n";
        }
      }
    }

    indent();
    out_ << "(memory 16)\n"; // 1MB
    indent();
    out_ << "(global $__stack_pointer (mut i32) (i32.const 1048576))\n";

    // [v0.2.2] Emit intrinsic helper functions and import link-form decls.
    for (const auto &intr: prog.intrinsics) {
      emitIntrinsicHelper(intr);
    }
    for (const auto &d: prog.extDecls) {
      indent();
      out_ << "(import \"\" \"" << stripSigil(d.name.name) << "\" (func "
           << mangleName(d.name.name);
      for (const auto &p: d.params) {
        out_ << " (param " << getWasmType(p.type) << ")";
        (void) p;
      }
      out_ << " (result " << getWasmType(d.retType) << ")))\n";
    }

    for (const auto &f: prog.funs) {
      curFuncName_ = f.name.name;
      locals_.clear();
      syms_.clear();
      stackSize_ = 0;

      // Pre-scan: collect variables whose address is taken (must be spilled to shadow stack)
      std::unordered_set<std::string> addrTaken;
      std::function<void(const Expr &)> scanExpr;
      std::function<void(const Atom &)> scanAtom;

      scanAtom = [&](const Atom &a) {
        std::visit(
            [&](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, AddrAtom>) {
                addrTaken.insert(arg.lv.base.name);
              } else if constexpr (std::is_same_v<T, SelectAtom>) {
                if (arg.cond) {
                  scanExpr(arg.cond->lhs);
                  scanExpr(arg.cond->rhs);
                }
                if (arg.maskExpr) {
                  scanExpr(*arg.maskExpr);
                }
              }
            },
            a.v
        );
      };

      scanExpr = [&](const Expr &e) {
        scanAtom(e.first);
        for (const auto &t: e.rest) {
          scanAtom(t.atom);
        }
      };

      auto scanInitVal = [&](auto &self, const InitVal &iv) -> void {
        if (iv.kind == InitVal::Kind::Atom) {
          scanAtom(*std::get<AtomPtr>(iv.value));
        } else if (iv.kind == InitVal::Kind::Aggregate) {
          const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
          for (const auto &el: elements) {
            self(self, *el);
          }
        }
      };

      for (const auto &l: f.lets) {
        if (l.init) {
          scanInitVal(scanInitVal, *l.init);
        }
      }

      for (const auto &b: f.blocks) {
        for (const auto &ins: b.instrs) {
          std::visit(
              [&](auto &&instr) {
                using IT = std::decay_t<decltype(instr)>;
                if constexpr (std::is_same_v<IT, AssignInstr>) {
                  scanExpr(instr.rhs);
                } else if constexpr (std::is_same_v<IT, StoreInstr>) {
                  scanExpr(instr.ptr);
                  scanExpr(instr.val);
                }
              },
              ins
          );
        }
        if (auto rt = std::get_if<RetTerm>(&b.term)) {
          if (rt->value) {
            scanExpr(*rt->value);
          }
        }
      }

      for (const auto &s: f.syms) {
        syms_[s.name.name] = s.type;
      }

      for (const auto &p: f.params) {
        locals_[p.name.name] = {getWasmType(p.type), true, getIntWidth(p.type), false, 0, p.type};
      }
      for (const auto &l: f.lets) {
        // Mark as aggregate if it's struct/array/vector OR if its address is taken (needs memory
        // slot)
        bool isAgg = std::holds_alternative<StructType>(l.type->v) ||
                     std::holds_alternative<ArrayType>(l.type->v) ||
                     std::holds_alternative<VecType>(l.type->v) || addrTaken.count(l.name.name);
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
      out_ << "(local $__pc i32)\n";
      indent();
      out_ << "(local $__old_sp i32)\n";
      indent();
      out_ << "(local $__ptr_temp i32)\n"; // scratch register for null-checked ptr ops
      indent();
      out_ << "(local $__idx_temp i32)\n"; // scratch register for index bounds checks
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
            bool isTargetFloat = std::holds_alternative<FloatType>(l.type->v);
            if (isTargetFloat) {
              out_ << (locals_[l.name.name].bitwidth <= 32 ? "f32.const " : "f64.const ")
                   << std::get<IntLit>(l.init->value).value << ".0\n";
            } else {
              out_ << (locals_[l.name.name].bitwidth <= 32 ? "i32.const " : "i64.const ")
                   << std::get<IntLit>(l.init->value).value << "\n";
              emitSignExtend(
                  getIntWidth(l.type), (locals_[l.name.name].wasmType == "i32" ? 32 : 64)
              );
            }
            indent();
            out_ << "local.set " << mangleName(l.name.name) << "\n";
          } else if (l.init->kind == InitVal::Kind::Float) {
            indent();
            out_ << (locals_[l.name.name].wasmType == "f32" ? "f32.const " : "f64.const ")
                 << formatFloatLit(std::get<FloatLit>(l.init->value).value) << "\n";
            indent();
            out_ << "local.set " << mangleName(l.name.name) << "\n";
          } else if (l.init->kind == InitVal::Kind::Null) {
            // null pointer = i32 0 in WASM
            indent();
            out_ << "i32.const 0\n";
            indent();
            out_ << "local.set " << mangleName(l.name.name) << "\n";
          } else if (l.init->kind == InitVal::Kind::Local) {
            emitLValue({std::get<LocalId>(l.init->value), {}, l.init->span}, false);
            indent();
            out_ << "local.set " << mangleName(l.name.name) << "\n";
          } else if (l.init->kind == InitVal::Kind::Sym) {
            const auto &sid = std::get<SymId>(l.init->value);
            indent();
            out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, sid.name)) << "\n";
            // Handle int extension if needed
            std::uint32_t srcWidth = 32;
            bool srcIsFloat = false;
            if (syms_.count(sid.name)) {
              srcWidth = getIntWidth(syms_.at(sid.name));
              if (std::holds_alternative<FloatType>(syms_.at(sid.name)->v))
                srcIsFloat = true;
            }
            if (!srcIsFloat) {
              if (srcWidth <= 32 && getIntWidth(l.type) > 32) {
                indent();
                out_ << "i64.extend_i32_s\n";
              } else if (srcWidth > 32 && getIntWidth(l.type) <= 32) {
                indent();
                out_ << "i32.wrap_i64\n";
              }
            } else {
              // Handle float promotion if needed
              if (srcWidth == 32 && getIntWidth(l.type) == 64) {
                indent();
                out_ << "f64.promote_f32\n";
              }
            }
            indent();
            out_ << "local.set " << mangleName(l.name.name) << "\n";
          } else if (l.init->kind == InitVal::Kind::Atom) {
            const auto &atom = std::get<AtomPtr>(l.init->value);
            bool isFloat = std::holds_alternative<FloatType>(l.type->v);
            emitAtom(*atom, locals_[l.name.name].bitwidth, isFloat);
            indent();
            out_ << "local.set " << mangleName(l.name.name) << "\n";
          }
        }
      }

      indent();
      out_ << "i32.const 0\n";
      indent();
      out_ << "local.set $__pc\n";

      indent();
      out_ << "(loop $__symir_dispatch_loop\n";
      indent_level_++;

      for (size_t i = 0; i < f.blocks.size(); ++i) {
        indent();
        out_ << "(block " << mangleName(f.blocks[i].label.name) << "\n";
        indent_level_++;
      }

      indent();
      out_ << "local.get $__pc\n";
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
                    TypePtr lhsTy = getLValueType(arg.lhs);
                    if (lhsTy && std::holds_alternative<VecType>(lhsTy->v) &&
                        arg.lhs.accesses.empty()) {
                      auto &vt = std::get<VecType>(lhsTy->v);
                      std::uint32_t elemSize = getTypeSize(vt.elem);
                      bool valIsFloat = std::holds_alternative<FloatType>(vt.elem->v);
                      uint32_t width = getIntWidth(vt.elem);
                      for (uint64_t i = 0; i < vt.size; ++i) {
                        indent();
                        out_ << "local.get $__old_sp\n";
                        indent();
                        out_ << "i32.const " << (info.offset - i * elemSize) << "\n";
                        indent();
                        out_ << "i32.sub\n";

                        emitVecExprLane(arg.rhs, vt, i, width, valIsFloat);

                        indent();
                        if (valIsFloat) {
                          out_ << (width == 32 ? "f32.store\n" : "f64.store\n");
                        } else {
                          out_ << (width <= 8
                                       ? "i32.store8"
                                       : (width <= 16 ? "i32.store16"
                                                      : (width <= 32 ? "i32.store" : "i64.store")))
                               << "\n";
                        }
                      }
                    } else if (info.isAggregate || !arg.lhs.accesses.empty()) {
                      emitAddress(arg.lhs);
                      TypePtr curType = info.symirType;
                      for (const auto &acc: arg.lhs.accesses) {
                        if (std::holds_alternative<AccessIndex>(acc)) {
                          if (auto at = std::get_if<ArrayType>(&curType->v))
                            curType = at->elem;
                          else if (auto vt = std::get_if<VecType>(&curType->v))
                            curType = vt->elem;
                        } else if (auto af = std::get_if<AccessField>(&acc)) {
                          if (auto st = std::get_if<StructType>(&curType->v)) {
                            auto &fld = af->field;
                            if (structLayouts_.count(st->name.name) &&
                                structLayouts_.at(st->name.name).fields.count(fld))
                              curType = structLayouts_.at(st->name.name).fields.at(fld).type;
                          }
                        }
                      }
                      std::uint32_t width = 0;
                      bool valIsFloat = false;
                      if (auto bits = TypeUtils::getBitWidth(curType)) {
                        width = *bits;
                      } else if (curType && std::holds_alternative<FloatType>(curType->v)) {
                        valIsFloat = true;
                        width = (std::get<FloatType>(curType->v).kind == FloatType::Kind::F32) ? 32
                                                                                               : 64;
                      } else if (curType && std::holds_alternative<PtrType>(curType->v)) {
                        // WASM pointers are 32-bit (one i32 cell).
                        width = 32;
                      }

                      emitExpr(arg.rhs, width, valIsFloat);
                      indent();
                      if (valIsFloat) {
                        out_ << (width == 32 ? "f32.store\n" : "f64.store\n");
                      } else {
                        if (width <= 8)
                          out_ << "i32.store8\n";
                        else if (width <= 16)
                          out_ << "i32.store16\n";
                        else if (width <= 32)
                          out_ << "i32.store\n";
                        else
                          out_ << "i64.store\n";
                      }
                    } else {
                      bool isFloat = std::holds_alternative<FloatType>(info.symirType->v);
                      bool isPtr = std::holds_alternative<PtrType>(info.symirType->v);
                      if (isPtr) {
                        emitPtrExpr(arg.rhs, info.symirType);
                      } else if (isPtrDiff(arg.rhs)) {
                        // ptr - ptr → i64 element distance. Emit byte diff, then /sizeof.
                        emitPtrDiff(arg.rhs);
                      } else {
                        emitExpr(arg.rhs, info.bitwidth, isFloat);
                      }
                      indent();
                      out_ << "local.set " << mangleName(arg.lhs.base.name) << "\n";
                    }
                  }
                } else if constexpr (std::is_same_v<T, RequireInstr>) {
                  if (!noRequire_) {
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
                } else if constexpr (std::is_same_v<T, StoreInstr>) {
                  // *ptr = val — with null-pointer trap
                  // Determine pointee type from the pointer expression
                  uint32_t storeWidth = 32;
                  bool storeIsFloat = false;
                  if (auto rva = std::get_if<RValueAtom>(&arg.ptr.first.v)) {
                    if (locals_.count(rva->rval.base.name)) {
                      const auto &pinfo = locals_.at(rva->rval.base.name);
                      if (auto pt = std::get_if<PtrType>(&pinfo.symirType->v)) {
                        if (auto bits = TypeUtils::getBitWidth(pt->pointee)) {
                          storeWidth = *bits;
                        } else if (pt->pointee &&
                                   std::holds_alternative<FloatType>(pt->pointee->v)) {
                          storeIsFloat = true;
                          storeWidth =
                              (std::get<FloatType>(pt->pointee->v).kind == FloatType::Kind::F32)
                                  ? 32
                                  : 64;
                        }
                      }
                    }
                  }
                  // Emit ptr expr → save to $__ptr_temp, null check, then store
                  emitExpr(arg.ptr, 32, false);
                  indent();
                  out_ << "local.tee $__ptr_temp\n";
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
                  indent();
                  out_ << "local.get $__ptr_temp\n";
                  emitExpr(arg.val, storeWidth, storeIsFloat);
                  indent();
                  if (storeIsFloat) {
                    out_ << (storeWidth <= 32 ? "f32.store\n" : "f64.store\n");
                  } else {
                    out_
                        << (storeWidth <= 8    ? "i32.store8\n"
                            : storeWidth <= 16 ? "i32.store16\n"
                            : storeWidth <= 32 ? "i32.store\n"
                                               : "i64.store\n");
                  }
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
                  out_ << "local.set $__pc\n";
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
                  out_ << "local.set $__pc\n";
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
                  out_ << "local.set $__pc\n";
                  indent();
                  out_ << "br $__symir_dispatch_loop\n";
                }
              } else if constexpr (std::is_same_v<T, RetTerm>) {
                if (arg.value) {
                  bool isFloat = std::holds_alternative<FloatType>(f.retType->v);
                  emitExpr(*arg.value, getIntWidth(f.retType), isFloat);
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
        bool isFloat = std::holds_alternative<FloatType>(f.retType->v);
        if (isFloat) {
          out_ << (getIntWidth(f.retType) <= 32 ? "f32.const 0.0\n" : "f64.const 0.0\n");
        } else {
          out_ << (getIntWidth(f.retType) <= 32 ? "i32.const 0\n" : "i64.const 0\n");
        }
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

  void WasmBackend::emitPtrExpr(const Expr &expr, const TypePtr &ptrType) {
    // Emit a pointer-valued expression as an i32 WASM address.
    // For ptr ± int, scale the integer offset by the pointee element size.
    const auto &pt = std::get<PtrType>(ptrType->v);
    uint32_t elemSize = getTypeSize(pt.pointee);

    emitAtom(expr.first, 32, false);

    for (const auto &t: expr.rest) {
      emitAtom(t.atom, 32, false);
      // Scale integer offset by element size (pointer arithmetic)
      if (elemSize > 1) {
        indent();
        out_ << "i32.const " << elemSize << "\n";
        indent();
        out_ << "i32.mul\n";
      }
      indent();
      out_ << (t.op == AddOp::Plus ? "i32.add\n" : "i32.sub\n");
    }
  }

  bool WasmBackend::isPtrDiff(const Expr &expr) const {
    // Match `ptr_lvalue - ptr_lvalue` as the entire expression. Per spec §6.8.6
    // this is the only mixed form that yields a non-pointer result (i64).
    if (expr.rest.size() != 1)
      return false;
    if (expr.rest[0].op != AddOp::Minus)
      return false;
    auto firstRv = std::get_if<RValueAtom>(&expr.first.v);
    auto secondRv = std::get_if<RValueAtom>(&expr.rest[0].atom.v);
    if (!firstRv || !secondRv)
      return false;
    auto firstIt = locals_.find(firstRv->rval.base.name);
    auto secondIt = locals_.find(secondRv->rval.base.name);
    if (firstIt == locals_.end() || secondIt == locals_.end())
      return false;
    return std::holds_alternative<PtrType>(firstIt->second.symirType->v) &&
           std::holds_alternative<PtrType>(secondIt->second.symirType->v);
  }

  void WasmBackend::emitPtrDiff(const Expr &expr) {
    // (q - p) in bytes, then divide by sizeof(pointee) to yield element distance.
    // Result is left on the stack as i64 (spec §6.8.6: ptr T - ptr T → i64).
    auto firstRv = std::get<RValueAtom>(expr.first.v);
    const auto &firstInfo = locals_.at(firstRv.rval.base.name);
    const auto &pt = std::get<PtrType>(firstInfo.symirType->v);
    uint32_t elemSize = getTypeSize(pt.pointee);

    emitAtom(expr.first, 32, false);
    emitAtom(expr.rest[0].atom, 32, false);
    indent();
    out_ << "i32.sub\n";
    if (elemSize > 1) {
      indent();
      out_ << "i32.const " << elemSize << "\n";
      indent();
      out_ << "i32.div_s\n";
    }
    indent();
    out_ << "i64.extend_i32_s\n";
  }

  TypePtr WasmBackend::getLValueType(const LValue &lv) {
    if (!locals_.count(lv.base.name))
      return nullptr;
    const auto &info = locals_.at(lv.base.name);
    TypePtr curType = info.symirType;
    for (const auto &acc: lv.accesses) {
      if (std::get_if<AccessIndex>(&acc)) {
        if (auto at = std::get_if<ArrayType>(&curType->v))
          curType = at->elem;
        else if (auto vt = std::get_if<VecType>(&curType->v))
          curType = vt->elem;
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (auto st = std::get_if<StructType>(&curType->v)) {
          auto &fld = af->field;
          if (structLayouts_.count(st->name.name) &&
              structLayouts_.at(st->name.name).fields.count(fld))
            curType = structLayouts_.at(st->name.name).fields.at(fld).type;
        }
      }
    }
    return curType;
  }

  TypePtr WasmBackend::getSelectValType(const SelectVal &sv) {
    if (std::holds_alternative<RValue>(sv)) {
      return getLValueType(std::get<RValue>(sv));
    } else {
      const auto &coef = std::get<Coef>(sv);
      return std::visit(
          [this](auto &&arg) -> TypePtr {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, IntLit>) {
              return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, 32, {}}, {}});
            } else if constexpr (std::is_same_v<T, FloatLit>) {
              return std::make_shared<Type>(Type{FloatType{FloatType::Kind::F64, {}}, {}});
            } else if constexpr (std::is_same_v<T, NullLit>) {
              return std::make_shared<Type>(Type{PtrType{nullptr, {}}, {}});
            } else {
              return std::visit(
                  [this](auto &&id) -> TypePtr {
                    using ID = std::decay_t<decltype(id)>;
                    if constexpr (std::is_same_v<ID, SymId>) {
                      if (syms_.count(id.name))
                        return syms_.at(id.name);
                    } else {
                      if (locals_.count(id.name))
                        return locals_.at(id.name).symirType;
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
  }

  void WasmBackend::emitVecExprLane(
      const Expr &expr, const VecType &vt, std::uint64_t lane, std::uint32_t targetWidth,
      bool isFloat
  ) {
    emitVecAtomLane(expr.first, vt, lane, targetWidth, isFloat);

    for (const auto &t: expr.rest) {
      emitVecAtomLane(t.atom, vt, lane, targetWidth, isFloat);
      indent();
      if (isFloat) {
        out_ << (targetWidth <= 32 ? "f32." : "f64.") << (t.op == AddOp::Plus ? "add\n" : "sub\n");
      } else {
        if (targetWidth <= 32)
          out_ << (t.op == AddOp::Plus ? "i32.add\n" : "i32.sub\n");
        else
          out_ << (t.op == AddOp::Plus ? "i64.add\n" : "i64.sub\n");
        emitSignExtend(targetWidth, (targetWidth <= 32 ? 32 : 64));
      }
    }
  }

  void WasmBackend::emitVecAtomLane(
      const Atom &atom, const VecType &vt, std::uint64_t lane, std::uint32_t targetWidth,
      bool isFloat
  ) {
    uint32_t wasmWidth = (targetWidth <= 32 ? 32 : 64);
    std::visit(
        [this, &vt, lane, targetWidth, wasmWidth, isFloat](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, CoefAtom>) {
            emitVecCoefLane(arg.coef, vt, lane, targetWidth, isFloat);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            emitVecLValueLane(arg.rval, vt, lane, targetWidth, isFloat);
          } else if constexpr (std::is_same_v<T, OpAtom>) {
            emitVecCoefLane(arg.coef, vt, lane, targetWidth, isFloat);
            emitVecLValueLane(arg.rval, vt, lane, targetWidth, isFloat);

            std::string prefix = (targetWidth <= 32 ? "i32." : "i64.");
            if (isFloat) {
              prefix = (targetWidth <= 32 ? "f32." : "f64.");
              if (arg.op == AtomOpKind::Mod) {
                auto emitX = [&]() { emitVecCoefLane(arg.coef, vt, lane, targetWidth, isFloat); };
                auto emitY = [&]() { emitVecLValueLane(arg.rval, vt, lane, targetWidth, isFloat); };

                indent();
                out_ << "drop\n";
                indent();
                out_ << "drop\n";

                emitX();
                emitX();
                emitY();
                indent();
                out_ << prefix << "div\n";
                indent();
                out_ << prefix << "trunc\n";
                emitY();
                indent();
                out_ << prefix << "mul\n";
                indent();
                out_ << prefix << "sub\n";
              } else {
                indent();
                switch (arg.op) {
                  case AtomOpKind::Mul:
                    out_ << prefix << "mul\n";
                    break;
                  case AtomOpKind::Div:
                    out_ << prefix << "div\n";
                    break;
                  default:
                    break;
                }
              }
            } else if (arg.op == AtomOpKind::LShr) {
              emitMask(targetWidth, wasmWidth);
              indent();
              out_ << (wasmWidth == 32 ? "i32.shr_u\n" : "i64.shr_u\n");
            } else {
              indent();
              std::string opStr;
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
            if (!isFloat)
              emitSignExtend(targetWidth, wasmWidth);
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            if (arg.maskExpr) {
              VecType maskVt{
                  vt.size,
                  std::make_shared<Type>(Type{IntType{IntType::Kind::ICustom, 1, {}}, {}}),
                  {}
              };
              emitVecExprLane(*arg.maskExpr, maskVt, lane, 32, false);
            } else {
              emitCond(*arg.cond);
            }
            indent();
            std::string typePrefix =
                isFloat ? (targetWidth <= 32 ? "f32" : "f64") : (targetWidth <= 32 ? "i32" : "i64");
            out_ << "if (result " << typePrefix << ")\n";
            indent_level_++;
            emitVecSelectValLane(arg.vtrue, vt, lane, targetWidth, isFloat);
            indent_level_--;
            indent();
            out_ << "else\n";
            indent_level_++;
            emitVecSelectValLane(arg.vfalse, vt, lane, targetWidth, isFloat);
            indent_level_--;
            indent();
            out_ << "end\n";
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            emitVecLValueLane(arg.rval, vt, lane, targetWidth, isFloat);
            if (isFloat) {
              indent();
              out_ << (targetWidth <= 32 ? "f32.neg\n" : "f64.neg\n");
            } else {
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
            }
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            bool srcIsFloat = false;
            uint32_t srcWidth = 32;
            std::visit(
                [&](auto &&src) {
                  using S = std::decay_t<decltype(src)>;
                  if constexpr (std::is_same_v<S, IntLit>) {
                    srcWidth = (src.value > INT32_MAX || src.value < INT32_MIN) ? 64 : 32;
                    indent();
                    out_ << (srcWidth <= 32 ? "i32.const " : "i64.const ") << src.value << "\n";
                  } else if constexpr (std::is_same_v<S, FloatLit>) {
                    srcWidth = 64;
                    srcIsFloat = true;
                    indent();
                    out_ << "f64.const " << formatFloatLit(src.value) << "\n";
                  } else if constexpr (std::is_same_v<S, SymId>) {
                    indent();
                    out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, src.name))
                         << "__" << lane << "\n";
                    srcWidth = 32;
                    srcIsFloat = false;
                    if (syms_.count(src.name)) {
                      auto symTy = syms_.at(src.name);
                      if (auto vt = std::get_if<VecType>(&symTy->v)) {
                        symTy = vt->elem;
                      }
                      if (std::holds_alternative<FloatType>(symTy->v)) {
                        srcIsFloat = true;
                        srcWidth =
                            (std::get<FloatType>(symTy->v).kind == FloatType::Kind::F32) ? 32 : 64;
                      } else if (auto it = std::get_if<IntType>(&symTy->v)) {
                        srcWidth = it->bits.value_or(32);
                      }
                    }
                  } else {
                    TypePtr srcTy = getLValueType(src);
                    uint32_t realSrcWidth = 32;
                    bool realSrcIsFloat = false;
                    if (srcTy) {
                      if (auto vt = std::get_if<VecType>(&srcTy->v)) {
                        srcTy = vt->elem;
                      }
                      realSrcIsFloat = std::holds_alternative<FloatType>(srcTy->v);
                      if (realSrcIsFloat) {
                        realSrcWidth =
                            (std::get<FloatType>(srcTy->v).kind == FloatType::Kind::F32) ? 32 : 64;
                      } else if (auto it = std::get_if<IntType>(&srcTy->v)) {
                        realSrcWidth = it->bits.value_or(32);
                      }
                    }
                    emitVecLValueLane(src, vt, lane, realSrcWidth, realSrcIsFloat);
                    srcWidth = realSrcWidth;
                    srcIsFloat = realSrcIsFloat;
                  }
                },
                arg.src
            );

            TypePtr dstTy = arg.dstType;
            if (auto vt = std::get_if<VecType>(&dstTy->v)) {
              dstTy = vt->elem;
            }
            bool dstIsFloat = std::holds_alternative<FloatType>(dstTy->v);
            uint32_t dstWidth = getIntWidth(dstTy);
            if (dstIsFloat) {
              dstWidth = (std::get<FloatType>(dstTy->v).kind == FloatType::Kind::F32) ? 32 : 64;
            }

            indent();
            if (srcIsFloat && dstIsFloat) {
              if (srcWidth == 32 && dstWidth == 64)
                out_ << "f64.promote_f32\n";
              else if (srcWidth == 64 && dstWidth == 32)
                out_ << "f32.demote_f64\n";
            } else if (srcIsFloat && !dstIsFloat) {
              if (dstWidth <= 32)
                out_ << (srcWidth == 32 ? "i32.trunc_f32_s\n" : "i32.trunc_f64_s\n");
              else
                out_ << (srcWidth == 32 ? "i64.trunc_f32_s\n" : "i64.trunc_f64_s\n");
            } else if (!srcIsFloat && dstIsFloat) {
              if (srcWidth <= 32)
                out_ << (dstWidth == 32 ? "f32.convert_i32_s\n" : "f64.convert_i32_s\n");
              else
                out_ << (dstWidth == 32 ? "f32.convert_i64_s\n" : "f64.convert_i64_s\n");
            } else {
              if (srcWidth <= 32 && dstWidth > 32)
                out_ << "i64.extend_i32_s\n";
              else if (srcWidth > 32 && dstWidth <= 32)
                out_ << "i32.wrap_i64\n";
            }
            if (!dstIsFloat)
              emitSignExtend(targetWidth, wasmWidth);
          } else if constexpr (std::is_same_v<T, CmpAtom>) {
            TypePtr opTy = getSelectValType(arg.lhs);
            TypePtr elemTy = opTy;
            VecType opVt = vt;
            if (opTy && std::holds_alternative<VecType>(opTy->v)) {
              opVt = std::get<VecType>(opTy->v);
              elemTy = opVt.elem;
            }
            bool opIsFloat = elemTy && std::holds_alternative<FloatType>(elemTy->v);
            uint32_t opWidth = getIntWidth(elemTy);

            emitVecSelectValLane(arg.lhs, opVt, lane, opWidth, opIsFloat);
            emitVecSelectValLane(arg.rhs, opVt, lane, opWidth, opIsFloat);

            indent();
            std::string opStr;
            if (opIsFloat) {
              std::string prefix = (opWidth <= 32 ? "f32." : "f64.");
              switch (arg.op) {
                case RelOp::EQ:
                  opStr = prefix + "eq";
                  break;
                case RelOp::NE:
                  opStr = prefix + "ne";
                  break;
                case RelOp::LT:
                  opStr = prefix + "lt";
                  break;
                case RelOp::LE:
                  opStr = prefix + "le";
                  break;
                case RelOp::GT:
                  opStr = prefix + "gt";
                  break;
                case RelOp::GE:
                  opStr = prefix + "ge";
                  break;
              }
            } else {
              std::string prefix = (opWidth <= 32 ? "i32." : "i64.");
              switch (arg.op) {
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
            }
            out_ << opStr << "\n";
            if (targetWidth > 32) {
              indent();
              out_ << "i64.extend_i32_s\n";
            }
          }
        },
        atom.v
    );
  }

  void WasmBackend::emitVecCoefLane(
      const Coef &coef, const VecType &vt, std::uint64_t lane, std::uint32_t targetWidth,
      bool isFloat
  ) {
    uint32_t wasmWidth = (targetWidth <= 32 ? 32 : 64);
    std::visit(
        [this, &vt, lane, targetWidth, wasmWidth, isFloat](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            indent();
            if (isFloat) {
              out_ << (wasmWidth == 32 ? "f32.const " : "f64.const ") << arg.value << ".0\n";
            } else {
              out_ << (wasmWidth == 32 ? "i32.const " : "i64.const ") << arg.value << "\n";
            }
          } else if constexpr (std::is_same_v<T, FloatLit>) {
            indent();
            out_ << (wasmWidth == 32 ? "f32.const " : "f64.const ") << formatFloatLit(arg.value)
                 << "\n";
          } else if constexpr (std::is_same_v<T, NullLit>) {
            indent();
            out_ << "i32.const 0\n";
          } else {
            std::visit(
                [this, &vt, lane, targetWidth](auto &&id) {
                  using ID = std::decay_t<decltype(id)>;
                  if constexpr (std::is_same_v<ID, SymId>) {
                    indent();
                    out_ << "call " << mangleName(getMangledSymbolName(curFuncName_, id.name))
                         << "__" << lane << "\n";
                    std::uint32_t srcWidth = getIntWidth(vt.elem);
                    if (srcWidth <= 32 && targetWidth > 32) {
                      indent();
                      out_ << "i64.extend_i32_s\n";
                    } else if (srcWidth > 32 && targetWidth <= 32) {
                      indent();
                      out_ << "i32.wrap_i64\n";
                    }
                  } else {
                    emitVecLValueLane({id, {}, id.span}, vt, lane, targetWidth, false);
                  }
                },
                arg
            );
          }
        },
        coef
    );
    if (!isFloat)
      emitSignExtend(targetWidth, wasmWidth);
  }

  void WasmBackend::emitVecLValueLane(
      const LValue &lv, const VecType &vt, std::uint64_t lane, std::uint32_t targetWidth,
      bool isFloat
  ) {
    (void) isFloat;
    if (!locals_.count(lv.base.name))
      return;
    const auto &info = locals_.at(lv.base.name);

    if (!info.isAggregate || !lv.accesses.empty()) {
      emitLValue(lv, false);
      TypePtr ty = getLValueType(lv);
      if (ty) {
        bool valIsFloat = std::holds_alternative<FloatType>(ty->v);
        std::uint32_t width = 32;
        if (valIsFloat) {
          width = (std::get<FloatType>(ty->v).kind == FloatType::Kind::F32) ? 32 : 64;
        } else if (auto it = std::get_if<IntType>(&ty->v)) {
          width = it->bits.value_or(32);
        }
        if (!valIsFloat) {
          if (width <= 32 && targetWidth > 32) {
            indent();
            out_ << "i64.extend_i32_s\n";
          } else if (width > 32 && targetWidth <= 32) {
            indent();
            out_ << "i32.wrap_i64\n";
          }
        } else {
          if (width == 32 && targetWidth == 64) {
            indent();
            out_ << "f64.promote_f32\n";
          } else if (width == 64 && targetWidth == 32) {
            indent();
            out_ << "f32.demote_f64\n";
          }
        }
      }
      return;
    }

    TypePtr elemTy = vt.elem;
    if (auto localVt = std::get_if<VecType>(&info.symirType->v)) {
      elemTy = localVt->elem;
    }
    std::uint32_t elemSize = getTypeSize(elemTy);
    indent();
    out_ << "local.get $__old_sp\n";
    indent();
    out_ << "i32.const " << (info.offset - lane * elemSize) << "\n";
    indent();
    out_ << "i32.sub\n";

    std::uint32_t width = getIntWidth(elemTy);
    bool valIsFloat = std::holds_alternative<FloatType>(elemTy->v);

    indent();
    if (valIsFloat) {
      out_ << (width == 32 ? "f32.load\n" : "f64.load\n");
    } else {
      out_
          << (width <= 8
                  ? "i32.load8_s\n"
                  : (width <= 16 ? "i32.load16_s\n" : (width <= 32 ? "i32.load\n" : "i64.load\n")));
    }

    if (!valIsFloat) {
      if (width <= 32 && targetWidth > 32) {
        indent();
        out_ << "i64.extend_i32_s\n";
      } else if (width > 32 && targetWidth <= 32) {
        indent();
        out_ << "i32.wrap_i64\n";
      }
    } else {
      if (width == 32 && targetWidth == 64) {
        indent();
        out_ << "f64.promote_f32\n";
      } else if (width == 64 && targetWidth == 32) {
        indent();
        out_ << "f32.demote_f64\n";
      }
    }
  }

  void WasmBackend::emitVecSelectValLane(
      const SelectVal &sv, const VecType &vt, std::uint64_t lane, std::uint32_t targetWidth,
      bool isFloat
  ) {
    if (std::holds_alternative<RValue>(sv)) {
      emitVecLValueLane(std::get<RValue>(sv), vt, lane, targetWidth, isFloat);
    } else {
      emitVecCoefLane(std::get<Coef>(sv), vt, lane, targetWidth, isFloat);
    }
  }

} // namespace symir
