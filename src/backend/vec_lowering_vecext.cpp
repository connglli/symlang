// VecLowering strategy: GCC/Clang vector extensions.
//
// Maps `<N> T` to a typedef of the form
//   typedef T __attribute__((vector_size(N * sizeof(T)))) <name>;
// where <name> is `_vec_<N>_<elem>` (e.g. `_vec_4_i32`). The compiler
// natively supports lane subscript and binary operators on this type,
// which makes most emissions trivially one-liner.
//
// This strategy supports `canCrossFnBoundary` (parameters and returns
// pass the vector by value in SIMD registers per the platform ABI).

#include <set>
#include <sstream>
#include "backend/vec_lowering.hpp"

namespace symir {

  namespace {

    std::string elemSuffix(const TypePtr &elem) {
      if (auto it = std::get_if<IntType>(&elem->v)) {
        int bits = it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64);
        return "i" + std::to_string(bits);
      }
      if (auto ft = std::get_if<FloatType>(&elem->v)) {
        return ft->kind == FloatType::Kind::F32 ? "f32" : "f64";
      }
      return "u"; // unknown — shouldn't happen for well-typed programs
    }

    std::string elemCType(const TypePtr &elem) {
      if (auto it = std::get_if<IntType>(&elem->v)) {
        int bits = it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64);
        if (bits == 1)
          return "int8_t"; // i1 stored as int8 lane (vecext can't have 1-bit lanes)
        if (bits <= 8)
          return "int8_t";
        if (bits <= 16)
          return "int16_t";
        if (bits <= 32)
          return "int32_t";
        return "int64_t";
      }
      if (auto ft = std::get_if<FloatType>(&elem->v)) {
        return ft->kind == FloatType::Kind::F32 ? "float" : "double";
      }
      return "void";
    }

    std::uint64_t elemBytes(const TypePtr &elem) {
      if (auto it = std::get_if<IntType>(&elem->v)) {
        int bits = it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64);
        if (bits == 1)
          return 1; // stored as i8
        if (bits <= 8)
          return 1;
        if (bits <= 16)
          return 2;
        if (bits <= 32)
          return 4;
        return 8;
      }
      if (auto ft = std::get_if<FloatType>(&elem->v)) {
        return ft->kind == FloatType::Kind::F32 ? 4 : 8;
      }
      return 0;
    }

    std::string typeName(const VecType &vt) {
      return "_vec_" + std::to_string(vt.size) + "_" + elemSuffix(vt.elem);
    }

    const char *opStr(AtomOpKind k) {
      switch (k) {
        case AtomOpKind::Mul:
          return "*";
        case AtomOpKind::Div:
          return "/";
        case AtomOpKind::Mod:
          return "%"; // float lanes use fmod handled by caller; vec-ext doesn't have a fmod op
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
          return ">>"; // for unsigned cast inside, see emitBinOp
      }
      return "?";
    }

    const char *relStr(RelOp op) {
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

  } // namespace

  class VecExtLowering : public VecLowering {
  public:
    std::string name() const override { return "vecext"; }

    void emitPreamble(std::ostream &out, const std::vector<VecType> &usedShapes) override {
      // De-duplicate by (N, elem-suffix).
      std::set<std::string> emitted;
      for (const auto &vt: usedShapes) {
        std::string tn = typeName(vt);
        if (!emitted.insert(tn).second)
          continue;
        std::uint64_t bytes = vt.size * elemBytes(vt.elem);
        out << "typedef " << elemCType(vt.elem) << " " << tn << " __attribute__((vector_size("
            << bytes << ")));\n";
      }
      if (!emitted.empty())
        out << "\n";
    }

    std::string typeString(const VecType &vt) override { return typeName(vt); }

    void emitLocalDecl(std::ostream &out, const std::string &name, const VecType &vt) override {
      out << typeName(vt) << " " << name;
    }

    void emitInit(
        std::ostream &out, const std::string &name, const VecType &vt, const InitVal &iv
    ) override {
      (void) name;
      // Vecext supports both brace and broadcast init via C's `(T){a,b,...}`
      // compound-literal syntax — but the caller (c_backend.cpp) already
      // walks the InitVal and emits the C braces around individual scalar
      // values. To keep this strategy thin and reuse the existing scalar
      // emitInitVal machinery, we just let the caller drive: emitInit is
      // a no-op for VecExt; the caller emits `= { … }` after the local
      // declaration. (Other strategies will override this.)
      (void) out;
      (void) vt;
      (void) iv;
    }

    std::string
    emitLaneRead(const std::string &name, const VecType &vt, const std::string &idxExpr) override {
      (void) vt;
      return name + "[" + idxExpr + "]";
    }

    void emitLaneWrite(
        std::ostream &out, const std::string &name, const VecType &vt, const std::string &idxExpr,
        const std::string &valExpr
    ) override {
      (void) vt;
      out << name << "[" << idxExpr << "] = " << valExpr;
    }

    void emitWholeCopy(
        std::ostream &out, const std::string &lhs, const std::string &rhs, const VecType &vt
    ) override {
      (void) vt;
      out << lhs << " = " << rhs;
    }

    void emitBinOp(
        std::ostream &out, const std::string &dst, AtomOpKind op, const std::string &lhs,
        const std::string &rhs, const VecType &vt
    ) override {
      bool isFloat = vt.elem && std::holds_alternative<FloatType>(vt.elem->v);
      if (op == AtomOpKind::Mod && isFloat) {
        // GCC vec-ext doesn't have a vector fmod. Fall back to a small
        // per-lane loop using fmodf/fmod. Emit as a comma-sep block.
        // We emit a do-while(0) so the macro-style block is statement-safe.
        const char *fn =
            (std::get<FloatType>(vt.elem->v).kind == FloatType::Kind::F32) ? "fmodf" : "fmod";
        out << "do { for (int _i = 0; _i < " << vt.size << "; ++_i) (" << dst << ")[_i] = " << fn
            << "((" << lhs << ")[_i], (" << rhs << ")[_i]); } while(0)";
        return;
      }
      if (op == AtomOpKind::LShr) {
        // Logical shift: cast lhs to unsigned vector type, shift, cast back.
        // We do this lane-wise to avoid needing a second vec-ext typedef.
        out << "do { for (int _i = 0; _i < " << vt.size << "; ++_i) (" << dst << ")[_i] = (typeof(("
            << lhs << ")[0]))((unsigned long long)(" << lhs << ")[_i] >> (" << rhs
            << ")[_i]); } while(0)";
        return;
      }
      out << dst << " = " << lhs << " " << opStr(op) << " " << rhs;
    }

    void emitUnaryNot(
        std::ostream &out, const std::string &dst, const std::string &src, const VecType &vt
    ) override {
      (void) vt;
      out << dst << " = ~" << src;
    }

    void emitCmp(
        std::ostream &out, const std::string &dst, RelOp op, const std::string &lhs,
        const std::string &rhs, const VecType &operandVt
    ) override {
      // GCC vec-ext compare returns a vector of all-ones (true) / 0 (false)
      // of the same lane width as the operands. The interpreter uses 0/1
      // for <N> i1, so we mask the result with `& 1` per lane to land on
      // the same representation. Since the dst is `<N> i1` (stored as i8
      // lanes per our `i1 -> int8_t` mapping), we also need to truncate
      // to i8 widths. We do all of that in one per-lane loop.
      const char *cmp = relStr(op);
      out << "do { for (int _i = 0; _i < " << operandVt.size << "; ++_i) (" << dst << ")[_i] = (("
          << lhs << ")[_i] " << cmp << " (" << rhs << ")[_i]) ? 1 : 0; } while(0)";
    }

    void emitMaskSelect(
        std::ostream &out, const std::string &dst, const std::string &mask,
        const std::string &vtArg, const std::string &vfArg, const VecType &operandVt
    ) override {
      // Per-lane blend. We rely on the interpreter's 0/1 mask representation.
      out << "do { for (int _i = 0; _i < " << operandVt.size << "; ++_i) (" << dst << ")[_i] = (("
          << mask << ")[_i]) ? (" << vtArg << ")[_i] : (" << vfArg << ")[_i]; } while(0)";
    }

    void emitCast(
        std::ostream &out, const std::string &dst, const std::string &src, const VecType &srcVt,
        const VecType &dstVt
    ) override {
      (void) srcVt;
      // Per-lane scalar cast. The C compiler picks RNE for int->float and
      // truncation toward zero for float->int (matches our spec §6.4).
      out << "do { for (int _i = 0; _i < " << dstVt.size << "; ++_i) (" << dst << ")[_i] = ("
          << elemCType(dstVt.elem) << ")(" << src << ")[_i]; } while(0)";
    }

    bool canCrossFnBoundary() const override { return true; }
  };

  std::unique_ptr<VecLowering> makeVecLowering(const std::string &name) {
    if (name == "vecext")
      return std::make_unique<VecExtLowering>();
    // struct / scalars / array — Phase 2.
    return nullptr;
  }

} // namespace symir
