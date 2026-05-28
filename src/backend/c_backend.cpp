#include "backend/c_backend.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include "analysis/type_utils.hpp"

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
          } else if constexpr (std::is_same_v<T, VecType>) {
            // [v0.2.1] Vector type — delegated to the lowering strategy.
            // VecLowering produces a C type-string (a typedef name for
            // vecext, a struct name for structscalars / structarray, etc.).
            out_ << vecLowering_->typeString(arg);
          }
        },
        type->v
    );
  }

  // [v0.2.1] Walk the program collecting every (N, T) vector shape used so
  // the lowering strategy can emit its preamble (typedefs / struct decls).
  static void collectVecShapesInType(const TypePtr &t, std::vector<VecType> &out) {
    if (!t)
      return;
    std::visit(
        [&](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, VecType>) {
            out.push_back(arg);
            collectVecShapesInType(arg.elem, out);
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            collectVecShapesInType(arg.elem, out);
          } else if constexpr (std::is_same_v<T, PtrType>) {
            collectVecShapesInType(arg.pointee, out);
          }
        },
        t->v
    );
  }

  static std::vector<VecType> collectVecShapes(const Program &prog) {
    std::vector<VecType> out;
    for (const auto &s: prog.structs)
      for (const auto &f: s.fields)
        collectVecShapesInType(f.type, out);
    for (const auto &f: prog.funs) {
      collectVecShapesInType(f.retType, out);
      for (const auto &p: f.params)
        collectVecShapesInType(p.type, out);
      for (const auto &s: f.syms)
        collectVecShapesInType(s.type, out);
      for (const auto &l: f.lets)
        collectVecShapesInType(l.type, out);
    }
    return out;
  }

  std::string CBackend::intrinsicHelperName(const std::string &intrName, uint32_t bits) const {
    // intrName begins with '@'; drop it.
    std::string base = intrName;
    if (!base.empty() && base[0] == '@')
      base.erase(0, 1);
    return "_symir_" + base + "_i" + std::to_string(bits);
  }

  // [v0.2.2] §11.5 widening-and-mask lowering. We emit one static inline
  // helper per (intrinsic, bit-width). Each helper widens to the next
  // larger machine width, performs the operation, then sign-extends /
  // masks the result back to N bits. UB-preconditions abort via
  // __builtin_trap (matches the `assert`-on-violation pattern other UB
  // sites use).
  void CBackend::emitIntrinsicHelper(const IntrinsicDecl &intr) {
    auto rb = TypeUtils::getBitWidth(intr.retType);
    if (!rb)
      return; // non-integer intrinsics aren't supported in v0.2.2
    uint32_t N = *rb;
    uint32_t W = (N <= 8) ? 8 : (N <= 16) ? 16 : (N <= 32) ? 32 : 64;
    std::string sty = "int" + std::to_string(W) + "_t";
    std::string uty = "uint" + std::to_string(W) + "_t";
    std::string outTy = "int" + std::to_string(W) + "_t"; // matches emitType for iN
    std::string name = intrinsicHelperName(intr.name.name, N);

    out_ << "static inline " << outTy << " " << name << "(";
    for (size_t i = 0; i < intr.params.size(); ++i) {
      if (i)
        out_ << ", ";
      out_ << sty << " a" << i;
    }
    out_ << ") {\n";

    // Sign-mask helper to N bits.
    auto sextN = [&](const std::string &expr) -> std::string {
      if (N == W)
        return expr;
      // (sty)((uty)expr << (W-N)) >> (W-N)  — arithmetic shift restores sign.
      return "(" + sty + ")((" + uty + ")(" + expr + ") << " + std::to_string(W - N) + ") >> " +
             std::to_string(W - N);
    };
    auto maskU = [&](const std::string &expr) -> std::string {
      if (N == W)
        return "(" + uty + ")(" + expr + ")";
      // (uty)expr & ((1<<N) - 1)
      return "((" + uty + ")(" + expr + ") & ((" + uty + ")1 << " + std::to_string(N) + ") - 1)";
    };

    const std::string &n = intr.name.name;
    if (n == "@abs") {
      int64_t int_min_N = (N == 64) ? INT64_MIN : -(INT64_C(1) << (N - 1));
      out_ << "  if (a0 == (" << sty << ")" << int_min_N << "LL) __builtin_trap();\n";
      out_ << "  " << sty << " r = a0 < 0 ? -a0 : a0;\n";
      out_ << "  return " << sextN("r") << ";\n";
    } else if (n == "@min") {
      out_ << "  " << sty << " r = a0 < a1 ? a0 : a1;\n";
      out_ << "  return " << sextN("r") << ";\n";
    } else if (n == "@max") {
      out_ << "  " << sty << " r = a0 > a1 ? a0 : a1;\n";
      out_ << "  return " << sextN("r") << ";\n";
    } else if (n == "@popcount") {
      out_ << "  " << uty << " u = " << maskU("a0") << ";\n";
      if (W <= 32)
        out_ << "  " << sty << " r = (" << sty << ")__builtin_popcount(u);\n";
      else
        out_ << "  " << sty << " r = (" << sty << ")__builtin_popcountll((uint64_t)u);\n";
      out_ << "  return " << sextN("r") << ";\n";
    } else if (n == "@clz") {
      out_ << "  " << uty << " u = " << maskU("a0") << ";\n";
      out_ << "  if (u == 0) __builtin_trap();\n";
      if (W <= 32) {
        out_ << "  " << sty << " r = (" << sty << ")__builtin_clz((uint32_t)u) - "
             << std::to_string(32 - N) << ";\n";
      } else {
        out_ << "  " << sty << " r = (" << sty << ")__builtin_clzll((uint64_t)u) - "
             << std::to_string(64 - N) << ";\n";
      }
      out_ << "  return " << sextN("r") << ";\n";
    } else if (n == "@ctz") {
      out_ << "  " << uty << " u = " << maskU("a0") << ";\n";
      out_ << "  if (u == 0) __builtin_trap();\n";
      if (W <= 32)
        out_ << "  " << sty << " r = (" << sty << ")__builtin_ctz((uint32_t)u);\n";
      else
        out_ << "  " << sty << " r = (" << sty << ")__builtin_ctzll((uint64_t)u);\n";
      out_ << "  return " << sextN("r") << ";\n";
    } else {
      out_ << "  __builtin_trap(); /* unknown intrinsic */\n";
    }
    out_ << "}\n\n";
  }

  // [v0.2.2] §11.2 lowering for `decl`. Link-form decls become `extern`
  // prototypes (linker resolves them). Contract-form decls also emit an
  // `extern` plus a `// contract:` summary comment.
  void CBackend::emitExtDecl(const ExtDecl &d) {
    if (d.contract) {
      out_ << "// contract: " << d.name.name << "\n";
      for (const auto &pre: d.contract->pres)
        out_ << "//   pre  " << (pre.message ? *pre.message : "") << "\n";
      for (const auto &post: d.contract->posts)
        out_ << "//   post " << (post.message ? *post.message : "") << "\n";
    }
    out_ << "extern ";
    emitType(d.retType);
    out_ << " " << mangleName(d.name.name) << "(";
    if (d.params.empty()) {
      out_ << "void";
    } else {
      for (size_t i = 0; i < d.params.size(); ++i) {
        if (i)
          out_ << ", ";
        emitType(d.params[i].type);
        out_ << " " << mangleName(d.params[i].name.name);
      }
    }
    out_ << ");\n\n";
  }

  void CBackend::emit(const Program &prog) {
    prog_ = &prog;
    out_ << "#include <stdint.h>\n";
    out_ << "#include <stddef.h>\n";
    out_ << "#include <stdbool.h>\n";
    out_ << "#include <float.h>\n";
    out_ << "#include <math.h>\n";
    out_ << "#include <string.h>\n";
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

    // [v0.2.1] Vector-lowering strategy. Default to vecext.
    if (!vecLowering_) {
      vecLowering_ = makeVecLowering("vecext");
    }
    out_ << "// vec-lowering: " << vecLowering_->name() << "\n";
    auto vecShapes = collectVecShapes(prog);
    if (!vecShapes.empty()) {
      // Validate fn-boundary capability before emitting anything.
      if (!vecLowering_->canCrossFnBoundary()) {
        for (const auto &f: prog.funs) {
          if (f.retType && std::holds_alternative<VecType>(f.retType->v))
            throw std::runtime_error(
                "vec-lowering '" + vecLowering_->name() +
                "' cannot cross function boundaries: function '" + f.name.name +
                "' returns a vector"
            );
          for (const auto &p: f.params)
            if (p.type && std::holds_alternative<VecType>(p.type->v))
              throw std::runtime_error(
                  "vec-lowering '" + vecLowering_->name() +
                  "' cannot cross function boundaries: function '" + f.name.name +
                  "' has a vector parameter"
              );
        }
      }
      vecLowering_->emitPreamble(out_, vecShapes);
    }

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

    // 1b. [v0.2.2] Emit intrinsic helpers and extern decls for link-form
    //     `decl`s. Contract-form `decl`s lower with extern + a structured
    //     comment summarizing the contract (§11.2).
    for (const auto &intr: prog.intrinsics) {
      emitIntrinsicHelper(intr);
    }
    for (const auto &d: prog.extDecls) {
      emitExtDecl(d);
    }

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

        // [v0.2.1] Vector locals route through the strategy: emit the
        // declaration (which can be `T v[N]`, `T v_0, v_1, …` for scalars,
        // or `struct ... v`), then emit per-lane initializers as separate
        // statements so every strategy converges on the same code shape.
        if (std::holds_alternative<VecType>(l.type->v)) {
          auto &vt = std::get<VecType>(l.type->v);
          std::string vName = mangleName(l.name.name);
          indent();
          vecLowering_->emitLocalDecl(out_, vName, vt);
          out_ << ";\n";
          if (l.init) {
            if (l.init->kind == InitVal::Kind::Aggregate) {
              const auto &elems = std::get<std::vector<InitValPtr>>(l.init->value);
              for (std::uint64_t k = 0; k < vt.size && k < elems.size(); ++k) {
                indent();
                std::string lane = vecLowering_->emitLaneRead(vName, vt, std::to_string(k));
                out_ << lane << " = ";
                emitInitVal(*elems[k], vt.elem);
                out_ << ";\n";
              }
            } else if (l.init->kind == InitVal::Kind::Undef) {
              // undef: no init. Reading is UB by spec (caught by definite-init).
            } else {
              TypePtr initType = getInitValType(*l.init);
              if (initType && std::holds_alternative<VecType>(initType->v)) {
                if (l.init->kind == InitVal::Kind::Local || l.init->kind == InitVal::Kind::Sym) {
                  std::string srcName;
                  if (l.init->kind == InitVal::Kind::Local) {
                    srcName = mangleName(std::get<LocalId>(l.init->value).name);
                  } else {
                    srcName = mangleName(std::get<SymId>(l.init->value).name);
                  }
                  indent();
                  vecLowering_->emitWholeCopy(out_, vName, srcName, vt);
                  out_ << ";\n";
                } else if (l.init->kind == InitVal::Kind::Atom) {
                  if (vecLowering_->needsLaneUnroll()) {
                    for (std::uint64_t k = 0; k < vt.size; ++k) {
                      indent();
                      std::string dstLane =
                          vecLowering_->emitLaneRead(vName, vt, std::to_string(k));
                      out_ << dstLane << " = "
                           << emitVecAtomLane(*std::get<AtomPtr>(l.init->value), vt, k) << ";\n";
                    }
                  } else {
                    indent();
                    out_ << vName << " = ";
                    emitInitVal(*l.init, l.type);
                    out_ << ";\n";
                  }
                }
              } else {
                // Broadcast scalar.
                for (std::uint64_t k = 0; k < vt.size; ++k) {
                  indent();
                  std::string lane = vecLowering_->emitLaneRead(vName, vt, std::to_string(k));
                  out_ << lane << " = ";
                  emitInitVal(*l.init, vt.elem);
                  out_ << ";\n";
                }
              }
            }
          }
          continue;
        }

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
          TypePtr initType = getInitValType(*l.init);
          bool isWholeCopy = initType && TypeUtils::areTypesEqual(initType, l.type);
          if (isWholeCopy) {
            if (!dims.empty()) {
              out_ << " = {0};\n";
              indent();
              out_ << "memcpy(&" << mangleName(l.name.name) << ", &";
              emitInitVal(*l.init, l.type);
              out_ << ", sizeof(" << mangleName(l.name.name) << "));\n";
            } else {
              out_ << " = ";
              emitInitVal(*l.init, l.type);
              out_ << ";\n";
            }
          } else if (!dims.empty() || std::holds_alternative<StructType>(l.type->v)) {
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
                  // [v0.2.1] Vector LHS goes through strategy-aware paths.
                  auto lhsTy = getLValueType(arg.lhs);
                  if (lhsTy && std::holds_alternative<VecType>(lhsTy->v) &&
                      arg.lhs.accesses.empty()) {
                    auto &vt = std::get<VecType>(lhsTy->v);
                    // CmpAtom and mask-form SelectAtom are always lane-unroll.
                    if (arg.rhs.rest.empty()) {
                      if (auto cmpA = std::get_if<CmpAtom>(&arg.rhs.first.v)) {
                        emitVecCmpAssign(arg.lhs, *cmpA, vt);
                        return;
                      }
                      if (auto sel = std::get_if<SelectAtom>(&arg.rhs.first.v)) {
                        if (sel->maskExpr) {
                          emitVecMaskSelectAssign(arg.lhs, *sel, vt);
                          return;
                        }
                      }
                    }
                    // [v0.2.1] Logical right shift (LShr) is the only OpAtom
                    // that can't be expressed inline on a vec-ext type — the
                    // signed-to-unsigned reinterpret is illegal on GCC vector
                    // types. Force lane-unroll for that op regardless of
                    // strategy.
                    bool isVecLShr = false;
                    if (arg.rhs.rest.empty()) {
                      if (auto op = std::get_if<OpAtom>(&arg.rhs.first.v))
                        isVecLShr = (op->op == AtomOpKind::LShr);
                    }
                    if (vecLowering_->needsLaneUnroll() || isVecLShr) {
                      emitVecAssign(arg.lhs, arg.rhs, vt);
                      return;
                    }
                    // vecext: fall through to the inline expression path.
                  }
                  // [v0.2.1] Scalar `cmp` assignment: emit `(l op r) ? 1 : 0`.
                  // CmpAtom can't lower as an inline expression (see emitAtom),
                  // so we special-case it at AssignInstr level for scalars too.
                  if (arg.rhs.rest.empty()) {
                    if (auto cmpA = std::get_if<CmpAtom>(&arg.rhs.first.v)) {
                      emitLValue(arg.lhs);
                      out_ << " = ((";
                      emitSelectVal(cmpA->lhs);
                      const char *op = "==";
                      switch (cmpA->op) {
                        case RelOp::EQ:
                          op = "==";
                          break;
                        case RelOp::NE:
                          op = "!=";
                          break;
                        case RelOp::LT:
                          op = "<";
                          break;
                        case RelOp::LE:
                          op = "<=";
                          break;
                        case RelOp::GT:
                          op = ">";
                          break;
                        case RelOp::GE:
                          op = ">=";
                          break;
                      }
                      out_ << ") " << op << " (";
                      emitSelectVal(cmpA->rhs);
                      out_ << ")) ? 1 : 0;\n";
                      return;
                    }
                  }
                  if (lhsTy && std::holds_alternative<ArrayType>(lhsTy->v)) {
                    out_ << "memcpy(&(";
                    emitLValue(arg.lhs);
                    out_ << "), &(";
                    emitExpr(arg.rhs);
                    out_ << "), sizeof(";
                    emitLValue(arg.lhs);
                    out_ << "));\n";
                  } else {
                    emitLValue(arg.lhs);
                    out_ << " = ";
                    emitExpr(arg.rhs);
                    out_ << ";\n";
                  }
                  // [v0.2.1] FP vector lanes: per-lane finite check (rule 21
                  // lifted to FP rules 6/7 — any lane producing ±∞ or NaN
                  // is UB). The native vec-ext division won't trap on its
                  // own and UBSan doesn't catch SIMD div-by-zero, so we
                  // emit an explicit check.
                  if (lhsTy && std::holds_alternative<VecType>(lhsTy->v) &&
                      arg.lhs.accesses.empty()) {
                    auto &vtFp = std::get<VecType>(lhsTy->v);
                    if (vtFp.elem && std::holds_alternative<FloatType>(vtFp.elem->v)) {
                      std::string base = mangleName(arg.lhs.base.name);
                      for (std::uint64_t k = 0; k < vtFp.size; ++k) {
                        indent();
                        std::string lane =
                            vecLowering_->emitLaneRead(base, vtFp, std::to_string(k));
                        out_ << "if (!__builtin_isfinite(" << lane << ")) __builtin_trap();\n";
                      }
                    }
                  }
                  // [v0.2.1] Scalar FP UB: any ±∞ or NaN result is UB
                  // (rules 6/7). UBSan catches some FP issues but not NaN
                  // from 0.0/0.0; emit an explicit `isfinite` check after
                  // FP assignments so the spec's semantics are enforced.
                  // Also fires for FP element writes (array element / struct
                  // field / vector lane) — the check uses the LHS in place.
                  bool lhsIsFp = lhsTy && std::holds_alternative<FloatType>(lhsTy->v);
                  if (lhsIsFp) {
                    indent();
                    out_ << "if (!__builtin_isfinite(";
                    emitLValue(arg.lhs);
                    out_ << ")) __builtin_trap();\n";
                  }
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
            // [v0.2.1] Cond form lowers to C's `?:` directly. The mask form
            // with a vector mask requires per-lane emission, special-cased
            // at AssignInstr level; with a scalar i1 mask it lowers to the
            // same `?:` after a `!= 0` predicate (so we don't depend on the
            // mask producing an exact `1` bit pattern).
            if (arg.cond) {
              out_ << "(";
              emitCond(*arg.cond);
              out_ << " ? ";
              emitSelectVal(arg.vtrue);
              out_ << " : ";
              emitSelectVal(arg.vfalse);
              out_ << ")";
            } else {
              // Mask form. Vector masks are out of expression-position scope.
              auto maskTy = getExprType(*arg.maskExpr);
              if (maskTy && std::holds_alternative<VecType>(maskTy->v)) {
                throw std::runtime_error(
                    "vector mask-form select must be the RHS of an assignment "
                    "(inline use not yet supported in this codegen)"
                );
              }
              out_ << "((";
              emitExpr(*arg.maskExpr);
              out_ << ") != 0 ? ";
              emitSelectVal(arg.vtrue);
              out_ << " : ";
              emitSelectVal(arg.vfalse);
              out_ << ")";
            }
          } else if constexpr (std::is_same_v<T, CmpAtom>) {
            // [v0.2.1] Same restriction: cmp returns a vector value that
            // needs lane-wise emission, handled at AssignInstr level.
            throw std::runtime_error(
                "cmp must be the RHS of an assignment (inline use not yet supported)"
            );
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
            // [v0.2.1] Rule 15b (typed-access mismatch): load through a
            // pointer that doesn't have enough room for the pointee. Use
            // __builtin_object_size so we trap when the pointer has been
            // walked past its originating field/array.
            out_ << "({ __typeof__(";
            emitLValue(arg.rval);
            out_ << ") _pl = ";
            emitLValue(arg.rval);
            out_ << "; if (!_pl) __builtin_trap();"
                 << " if (__builtin_object_size(_pl, 0) != (size_t)-1 &&"
                 << "     __builtin_object_size(_pl, 0) < sizeof(*_pl)) __builtin_trap();"
                 << " *_pl; })";
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            // [v0.2.1] ptrindex p, i → element pointer. Rule 17 (null nav),
            // rule 18 (undef nav — relies on UBSan), rule 16 (index bounds).
            // We emit:  ({ if (!p) __builtin_trap();
            //              if ((uint64_t)i > N) __builtin_trap();
            //              p + i; })
            // The N comes from the source pointer's array pointee.
            uint64_t arrSize = 0;
            auto rvTy = getLValueType(arg.rval);
            if (rvTy) {
              if (auto pt = std::get_if<PtrType>(&rvTy->v))
                if (auto at = std::get_if<ArrayType>(&pt->pointee->v))
                  arrSize = at->size;
            }
            out_ << "({ __typeof__(";
            emitLValue(arg.rval);
            out_ << ") _pi = ";
            emitLValue(arg.rval);
            out_ << "; int64_t _ii = (int64_t)(";
            emitIndex(arg.index);
            out_ << "); if (!_pi) __builtin_trap();";
            if (arrSize > 0)
              out_ << " if (_ii < 0 || (uint64_t)_ii > " << arrSize << "ULL) __builtin_trap();";
            // The pointer p has C type "T *" (pointee array decayed), so
            // (p + i) is the element-pointer of type T *.
            out_ << " _pi + _ii; })";
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            // [v0.2.1] ptrfield p, f → field pointer. Rule 17 (null nav)
            // and rule 19 (one-past-end nav). The one-past-end check uses
            // __builtin_object_size: a valid struct pointer has at least
            // sizeof(struct) bytes of object remaining; one-past-end has
            // zero bytes (and GCC reports 0).
            out_ << "({ __typeof__(";
            emitLValue(arg.rval);
            out_ << ") _pf = ";
            emitLValue(arg.rval);
            out_ << "; if (!_pf) __builtin_trap();"
                 << " if (__builtin_object_size(_pf, 0) < sizeof(*_pf)) __builtin_trap();"
                 << " &(_pf->" << arg.field << "); })";
          } else if constexpr (std::is_same_v<T, CallAtom>) {
            // [v0.2.2] Lower `call @f(args...)` to a C call expression.
            // For intrinsics, dispatch to the helper emitted in emit() §1b.
            // For other targets, use the mangled name directly (link-form
            // decls have extern prototypes; fun bodies are emitted later).
            const IntrinsicDecl *intr = nullptr;
            for (const auto &i: prog_->intrinsics)
              if (i.name.name == arg.callee.name) {
                intr = &i;
                break;
              }
            if (intr) {
              auto rb = TypeUtils::getBitWidth(intr->retType);
              out_ << intrinsicHelperName(arg.callee.name, rb.value_or(32));
            } else {
              out_ << mangleName(arg.callee.name);
            }
            out_ << "(";
            for (size_t i = 0; i < arg.args.size(); ++i) {
              if (i)
                out_ << ", ";
              emitExpr(*arg.args[i]);
            }
            out_ << ")";
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            // [v0.2.1] Vector cast: a C-style `(target_vec_t)(src_vec)`
            // is a *bitcast* in GCC vec-ext, not a per-lane conversion.
            // The right primitive is `__builtin_convertvector`.
            if (arg.dstType && std::holds_alternative<VecType>(arg.dstType->v)) {
              out_ << "__builtin_convertvector(";
              std::visit(
                  [&](auto &&src) {
                    using S = std::decay_t<decltype(src)>;
                    if constexpr (std::is_same_v<S, LValue>) {
                      emitLValue(src);
                    } else {
                      out_ << "/*unsupported vec cast src*/";
                    }
                  },
                  arg.src
              );
              out_ << ", ";
              emitType(arg.dstType);
              out_ << ")";
              return;
            }
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

  // [v0.2.1] cmp on vector operands lowers to a lane-wise loop. We need a
  // C-expression string for each side; SelectVal's parts can be either an
  // RValue (local name) or a Coef (literal / local / sym).
  // [v0.2.1] Per-lane C expression for a SelectVal. Delegates lane access
  // to the active VecLowering so each strategy picks its lane syntax.
  std::string
  CBackend::sirSelectValLane(const SelectVal &sv, const VecType &vt, const std::string &kExpr) {
    if (auto rv = std::get_if<RValue>(&sv)) {
      return vecLowering_->emitLaneRead(mangleName(rv->base.name), vt, kExpr);
    }
    if (auto cf = std::get_if<Coef>(&sv)) {
      if (auto i = std::get_if<IntLit>(cf))
        return std::to_string(i->value);
      if (auto f = std::get_if<FloatLit>(cf)) {
        std::ostringstream os;
        os.precision(17);
        os << f->value;
        return os.str();
      }
      if (auto id = std::get_if<LocalOrSymId>(cf)) {
        std::string nm = std::visit([](auto &&x) { return x.name; }, *id);
        return mangleName(nm);
      }
    }
    return "/*?*/";
  }

  void CBackend::emitVecCmpAssign(const LValue &lhs, const CmpAtom &c, const VecType &vt) {
    std::string dst = mangleName(lhs.base.name);
    const char *op = nullptr;
    switch (c.op) {
      case RelOp::EQ:
        op = "==";
        break;
      case RelOp::NE:
        op = "!=";
        break;
      case RelOp::LT:
        op = "<";
        break;
      case RelOp::LE:
        op = "<=";
        break;
      case RelOp::GT:
        op = ">";
        break;
      case RelOp::GE:
        op = ">=";
        break;
    }
    VecType operandVt = vt;
    if (auto rv = std::get_if<RValue>(&c.lhs)) {
      auto t = getLValueType(*rv);
      if (t && std::holds_alternative<VecType>(t->v))
        operandVt = std::get<VecType>(t->v);
    }
    for (std::uint64_t k = 0; k < vt.size; ++k) {
      std::string kS = std::to_string(k);
      std::string dstLane = vecLowering_->emitLaneRead(dst, vt, kS);
      std::string l = sirSelectValLane(c.lhs, operandVt, kS);
      std::string r = sirSelectValLane(c.rhs, operandVt, kS);
      if (k) {
        out_ << ";\n";
        indent();
      }
      out_ << dstLane << " = ((" << l << ") " << op << " (" << r << ")) ? 1 : 0";
    }
    out_ << ";\n";
  }

  void
  CBackend::emitVecMaskSelectAssign(const LValue &lhs, const SelectAtom &s, const VecType &vt) {
    std::string dst = mangleName(lhs.base.name);
    if (!s.maskExpr->rest.empty())
      throw std::runtime_error("Mask-form select: complex mask expressions not yet lowered to C");
    auto maskRv = std::get_if<RValueAtom>(&s.maskExpr->first.v);
    if (!maskRv)
      throw std::runtime_error("Mask-form select: mask must be a vector local for codegen");
    auto maskTy = getLValueType(maskRv->rval);
    if (!maskTy || !std::holds_alternative<VecType>(maskTy->v))
      throw std::runtime_error("Mask-form select: mask must have vector type");
    auto &maskVt = std::get<VecType>(maskTy->v);
    std::string maskName = mangleName(maskRv->rval.base.name);

    VecType armVt = vt;
    if (auto rv = std::get_if<RValue>(&s.vtrue)) {
      auto t = getLValueType(*rv);
      if (t && std::holds_alternative<VecType>(t->v))
        armVt = std::get<VecType>(t->v);
    }

    for (std::uint64_t k = 0; k < vt.size; ++k) {
      std::string kS = std::to_string(k);
      std::string dstLane = vecLowering_->emitLaneRead(dst, vt, kS);
      std::string maskLane = vecLowering_->emitLaneRead(maskName, maskVt, kS);
      std::string vtLane = sirSelectValLane(s.vtrue, armVt, kS);
      std::string vfLane = sirSelectValLane(s.vfalse, armVt, kS);
      if (k) {
        out_ << ";\n";
        indent();
      }
      out_ << dstLane << " = (" << maskLane << ") ? (" << vtLane << ") : (" << vfLane << ")";
    }
    out_ << ";\n";
  }

  // [v0.2.1] emitVecAtomLane: return a C expression for lane k of an Atom
  // that yields a vector value. Used by the lane-unroll path when the
  // active strategy can't lower vector ops as native C operators.
  std::string CBackend::emitVecAtomLane(const Atom &a, const VecType &vt, std::uint64_t k) {
    std::string kS = std::to_string(k);
    return std::visit(
        [&](auto &&arg) -> std::string {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, RValueAtom>) {
            // Vector lvalue → lane k via strategy.
            std::string base = mangleName(arg.rval.base.name);
            if (arg.rval.accesses.empty()) {
              return vecLowering_->emitLaneRead(base, vt, kS);
            }
            // Path with accesses (struct fields, etc.) — fall back to
            // emitLValue + lane subscript. Not exercised by our tests.
            return base + "/* nested */" + "[" + kS + "]";
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            // Literal broadcasts.
            if (auto i = std::get_if<IntLit>(&arg.coef))
              return std::to_string(i->value);
            if (auto f = std::get_if<FloatLit>(&arg.coef)) {
              std::ostringstream os;
              os.precision(17);
              os << f->value;
              return os.str();
            }
            if (auto id = std::get_if<LocalOrSymId>(&arg.coef)) {
              std::string nm = std::visit([](auto &&x) { return x.name; }, *id);
              // If it names a vector local/sym, take lane k; else broadcast.
              auto vinfo = varTypes_.find(nm);
              if (vinfo != varTypes_.end() && std::holds_alternative<VecType>(vinfo->second->v)) {
                auto &vvt = std::get<VecType>(vinfo->second->v);
                return vecLowering_->emitLaneRead(mangleName(nm), vvt, kS);
              }
              return mangleName(nm);
            }
            return "/*?coef*/";
          } else if constexpr (std::is_same_v<T, OpAtom>) {
            // coef <op> rval, lane-wise. coef may be a literal (broadcast)
            // or a vector lvalue.
            std::string coefLane;
            if (auto i = std::get_if<IntLit>(&arg.coef))
              coefLane = std::to_string(i->value);
            else if (auto f = std::get_if<FloatLit>(&arg.coef)) {
              std::ostringstream os;
              os.precision(17);
              os << f->value;
              coefLane = os.str();
            } else if (auto id = std::get_if<LocalOrSymId>(&arg.coef)) {
              std::string nm = std::visit([](auto &&x) { return x.name; }, *id);
              auto vinfo = varTypes_.find(nm);
              if (vinfo != varTypes_.end() && std::holds_alternative<VecType>(vinfo->second->v)) {
                auto &vvt = std::get<VecType>(vinfo->second->v);
                coefLane = vecLowering_->emitLaneRead(mangleName(nm), vvt, kS);
              } else {
                coefLane = mangleName(nm);
              }
            } else {
              coefLane = "/*?coef*/";
            }
            std::string rvalLane =
                vecLowering_->emitLaneRead(mangleName(arg.rval.base.name), vt, kS);
            const char *op = nullptr;
            switch (arg.op) {
              case AtomOpKind::Mul:
                op = "*";
                break;
              case AtomOpKind::Div:
                op = "/";
                break;
              case AtomOpKind::Mod:
                op = "%";
                break;
              case AtomOpKind::And:
                op = "&";
                break;
              case AtomOpKind::Or:
                op = "|";
                break;
              case AtomOpKind::Xor:
                op = "^";
                break;
              case AtomOpKind::Shl:
                op = "<<";
                break;
              case AtomOpKind::Shr:
                op = ">>";
                break;
              case AtomOpKind::LShr:
                op = ">>";
                break;
            }
            // Float % needs fmod / fmodf.
            bool isFloat = vt.elem && std::holds_alternative<FloatType>(vt.elem->v);
            if (arg.op == AtomOpKind::Mod && isFloat) {
              const char *fn =
                  (std::get<FloatType>(vt.elem->v).kind == FloatType::Kind::F32) ? "fmodf" : "fmod";
              return std::string(fn) + "((" + coefLane + "), (" + rvalLane + "))";
            }
            if (arg.op == AtomOpKind::LShr) {
              // Logical (unsigned) right-shift: cast lane to unsigned at
              // the lane's actual bit width so we don't accidentally
              // sign-extend through a wider unsigned type.
              const char *u = "uint32_t";
              if (auto it = std::get_if<IntType>(&vt.elem->v)) {
                int bits = it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64);
                if (bits <= 8)
                  u = "uint8_t";
                else if (bits <= 16)
                  u = "uint16_t";
                else if (bits <= 32)
                  u = "uint32_t";
                else
                  u = "uint64_t";
              }
              return std::string("((") + u + ")(" + coefLane + ") >> (" + rvalLane + "))";
            }
            return "((" + coefLane + ") " + op + " (" + rvalLane + "))";
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            std::string rvalLane =
                vecLowering_->emitLaneRead(mangleName(arg.rval.base.name), vt, kS);
            return "(~(" + rvalLane + "))";
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            // src is LValue (or literal/sym, but those aren't vectors).
            auto lv = std::get_if<LValue>(&arg.src);
            if (!lv)
              return "/*?cast*/";
            auto srcTy = getLValueType(*lv);
            if (!srcTy || !std::holds_alternative<VecType>(srcTy->v))
              return "/*?cast2*/";
            auto &srcVt = std::get<VecType>(srcTy->v);
            std::string srcLane = vecLowering_->emitLaneRead(mangleName(lv->base.name), srcVt, kS);
            // C cast on scalar lane.
            std::ostringstream os;
            os << "((";
            // Element C type from vt:
            if (auto it = std::get_if<IntType>(&vt.elem->v)) {
              int bits = it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64);
              if (bits <= 8)
                os << "int8_t";
              else if (bits <= 16)
                os << "int16_t";
              else if (bits <= 32)
                os << "int32_t";
              else
                os << "int64_t";
            } else if (auto ft = std::get_if<FloatType>(&vt.elem->v)) {
              os << (ft->kind == FloatType::Kind::F32 ? "float" : "double");
            }
            os << ")(" << srcLane << "))";
            return os.str();
          } else {
            // Other atoms (cmp / mask-select / load / addr) aren't handled
            // by the lane-unroll path — they're special-cased at
            // AssignInstr level above.
            return "/*?atom*/";
          }
        },
        a.v
    );
  }

  std::string CBackend::emitVecExprLane(const Expr &e, const VecType &vt, std::uint64_t k) {
    std::string result = emitVecAtomLane(e.first, vt, k);
    for (const auto &tail: e.rest) {
      std::string rhs = emitVecAtomLane(tail.atom, vt, k);
      const char *op = (tail.op == AddOp::Plus) ? "+" : "-";
      result = "(" + result + " " + op + " " + rhs + ")";
    }
    return result;
  }

  void CBackend::emitVecAssign(const LValue &lhs, const Expr &rhs, const VecType &vt) {
    std::string dst = mangleName(lhs.base.name);
    // Whole-vector copy fast path: RHS is a single bare RValueAtom of
    // the same vector type — let the strategy emit one copy statement.
    if (rhs.rest.empty()) {
      if (auto rv = std::get_if<RValueAtom>(&rhs.first.v)) {
        if (rv->rval.accesses.empty()) {
          auto srcTy = getLValueType(rv->rval);
          if (srcTy && std::holds_alternative<VecType>(srcTy->v)) {
            vecLowering_->emitWholeCopy(out_, dst, mangleName(rv->rval.base.name), vt);
            out_ << ";\n";
            return;
          }
        }
      }
    }
    // Unroll: emit one statement per lane.
    for (std::uint64_t k = 0; k < vt.size; ++k) {
      if (k) {
        indent();
      }
      std::string dstLane = vecLowering_->emitLaneRead(dst, vt, std::to_string(k));
      out_ << dstLane << " = " << emitVecExprLane(rhs, vt, k) << ";\n";
    }
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
    // [v0.2.1] Vector lane access through the LValue path goes through the
    // strategy's emitLaneRead so each strategy controls its lane syntax
    // (vecext: `name[k]`; scalars: `name_k`; structarray: `name.lanes[k]`;
    // structscalars: `name.l<k>`).
    if (lv.accesses.size() == 1) {
      if (auto ai = std::get_if<AccessIndex>(&lv.accesses[0])) {
        auto baseTy = getLValueType(LValue{lv.base, {}, lv.span});
        if (baseTy && std::holds_alternative<VecType>(baseTy->v)) {
          auto &vt = std::get<VecType>(baseTy->v);
          // Render the index into a string; reuse emitIndex via a tmp stream.
          std::ostringstream tmp;
          std::ostream &orig = out_;
          std::streambuf *origBuf = orig.rdbuf(tmp.rdbuf());
          emitIndex(ai->index);
          orig.rdbuf(origBuf);
          std::string idxStr = tmp.str();
          // [v0.2.1] Dynamic lane indices need a runtime bounds check —
          // GCC vec-ext lane access doesn't trap on OOB by itself. For
          // an IntLit index the parser already pinned it, so skip the
          // check (the typechecker may also have rejected it).
          bool isLit = std::holds_alternative<IntLit>(ai->index);
          if (!isLit) {
            // Wrap with a GCC statement-expression: evaluate idx once,
            // trap if out of bounds, then read the lane.
            std::string wrapped = "({ int64_t _vi = (" + idxStr +
                                  "); if ((uint64_t)_vi >= (uint64_t)" + std::to_string(vt.size) +
                                  "ull) __builtin_trap(); _vi; })";
            out_ << vecLowering_->emitLaneRead(mangleName(lv.base.name), vt, wrapped);
          } else {
            out_ << vecLowering_->emitLaneRead(mangleName(lv.base.name), vt, idxStr);
          }
          return;
        }
      }
    }
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
      case InitVal::Kind::Atom: {
        // [v0.2.1] §3.4.2 atom-form init — emit the atom inline (the
        // typechecker has already verified the atom's type matches the
        // target).
        emitAtom(*std::get<AtomPtr>(iv.value));
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
        else if (auto vt = std::get_if<VecType>(&cur->v))
          cur = vt->elem;
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
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            // ptrindex p, i : ptr [N] T → ptr T
            auto pt = getLValueType(arg.rval);
            if (pt) {
              if (auto ptr = std::get_if<PtrType>(&pt->v)) {
                if (auto at = std::get_if<ArrayType>(&ptr->pointee->v)) {
                  auto resPtr = std::make_shared<Type>();
                  resPtr->v = PtrType{at->elem, {}};
                  return resPtr;
                }
              }
            }
            return nullptr;
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            // ptrfield p, f : ptr @S → ptr FieldType
            auto pt = getLValueType(arg.rval);
            if (pt) {
              if (auto ptr = std::get_if<PtrType>(&pt->v)) {
                if (auto st = std::get_if<StructType>(&ptr->pointee->v)) {
                  auto sit = structFields_.find(st->name.name);
                  if (sit != structFields_.end()) {
                    for (const auto &[name, ty]: sit->second) {
                      if (name == arg.field) {
                        auto resPtr = std::make_shared<Type>();
                        resPtr->v = PtrType{ty, {}};
                        return resPtr;
                      }
                    }
                  }
                }
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

  TypePtr CBackend::getInitValType(const InitVal &iv) {
    switch (iv.kind) {
      case InitVal::Kind::Int: {
        auto t = std::make_shared<Type>();
        t->v = IntType{IntType::Kind::I64, {}, {}};
        return t;
      }
      case InitVal::Kind::Float: {
        auto t = std::make_shared<Type>();
        t->v = FloatType{isDoubleCtx_ ? FloatType::Kind::F64 : FloatType::Kind::F32, {}};
        return t;
      }
      case InitVal::Kind::Sym: {
        auto it = varTypes_.find(std::get<SymId>(iv.value).name);
        return (it != varTypes_.end()) ? it->second : nullptr;
      }
      case InitVal::Kind::Local: {
        auto it = varTypes_.find(std::get<LocalId>(iv.value).name);
        return (it != varTypes_.end()) ? it->second : nullptr;
      }
      case InitVal::Kind::Atom: {
        return getAtomType(*std::get<AtomPtr>(iv.value));
      }
      default:
        return nullptr;
    }
  }

} // namespace symir
