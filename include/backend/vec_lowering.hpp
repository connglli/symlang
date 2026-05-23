#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>
#include "ast/ast.hpp"

namespace symir {

  /**
   * VecLowering — abstract strategy that controls how the C backend lowers
   * `<N> T` vector locals and operations. Four built-in strategies are
   * provided via `makeVecLowering`; external tools may subclass for custom
   * lowerings (the interface is stable enough for rysmith mutation).
   *
   * See `tmp/vecplan.md` for the plan and per-strategy notes.
   */
  class VecLowering {
  public:
    virtual ~VecLowering() = default;

    /// Human-readable name (`"vecext"`, `"struct"`, ...). Stamped into the
    /// emitted C file as a traceability comment.
    virtual std::string name() const = 0;

    /// Emit any typedefs / helpers the strategy needs at the top of the
    /// file. `usedShapes` is the deduplicated set of (N, T) shapes that
    /// appear in the program; some strategies emit one typedef per shape.
    virtual void emitPreamble(std::ostream &out, const std::vector<VecType> &usedShapes) = 0;

    /// C type-string for a vector type (e.g., for use in declarations and
    /// function signatures). Strategies that don't have a single C type
    /// (e.g., `scalars`) return an empty string; callers route those via
    /// `emitLocalDecl` instead.
    virtual std::string typeString(const VecType &vt) = 0;

    /// Emit a local variable declaration for a vector. Some strategies emit
    /// one line (vecext / struct / array); `scalars` emits N lines.
    virtual void emitLocalDecl(std::ostream &out, const std::string &name, const VecType &vt) = 0;

    /// Emit an initializer following the local declaration. iv may be
    /// Aggregate (brace), Scalar (broadcast), or Undef.
    virtual void
    emitInit(std::ostream &out, const std::string &name, const VecType &vt, const InitVal &iv) = 0;

    /// Lane read in expression position: returns a C expression string.
    /// `idxExpr` is already the C-level index expression (lexed/lowered by
    /// the caller).
    virtual std::string
    emitLaneRead(const std::string &name, const VecType &vt, const std::string &idxExpr) = 0;

    /// Lane write `name[idx] = val;` (or the strategy's equivalent).
    virtual void emitLaneWrite(
        std::ostream &out, const std::string &name, const VecType &vt, const std::string &idxExpr,
        const std::string &valExpr
    ) = 0;

    /// Whole-vector copy `lhs = rhs;`. For vecext/struct: one assignment.
    /// For scalars/array: per-lane copies.
    virtual void emitWholeCopy(
        std::ostream &out, const std::string &lhs, const std::string &rhs, const VecType &vt
    ) = 0;

    /// Lane-wise binary op `dst = lhs <op> rhs;`. Coef-broadcast is handled
    /// by the caller — both `lhs` and `rhs` already reference vector
    /// values at this point.
    virtual void emitBinOp(
        std::ostream &out, const std::string &dst, AtomOpKind op, const std::string &lhs,
        const std::string &rhs, const VecType &vt
    ) = 0;

    /// Lane-wise unary `~`.
    virtual void emitUnaryNot(
        std::ostream &out, const std::string &dst, const std::string &src, const VecType &vt
    ) = 0;

    /// cmp -> mask vector. The output is `<N> i1` (lane scalar `i1`); the
    /// strategy decides how `i1` is represented (vecext: -1/0 lanes;
    /// others: 1/0 lanes).
    virtual void emitCmp(
        std::ostream &out, const std::string &dst, RelOp op, const std::string &lhs,
        const std::string &rhs, const VecType &operandVt
    ) = 0;

    /// Mask-based select per lane.
    virtual void emitMaskSelect(
        std::ostream &out, const std::string &dst, const std::string &mask,
        const std::string &vtArg, const std::string &vfArg, const VecType &operandVt
    ) = 0;

    /// Lane-wise cast.
    virtual void emitCast(
        std::ostream &out, const std::string &dst, const std::string &src, const VecType &srcVt,
        const VecType &dstVt
    ) = 0;

    /// May a vector cross a C function boundary? `false` for `scalars`
    /// (multiple identifiers can't be one return value) and `array` (C
    /// decays arrays to pointers); the C backend refuses to emit a vector
    /// param or return if this is false.
    virtual bool canCrossFnBoundary() const = 0;
  };

  /**
   * Factory by name. Returns nullptr for unknown names. The default in
   * `symirc` is "vecext".
   *
   * Built-in names:
   *   "vecext"  — GCC/Clang vector_size attribute (Phase 1)
   *   "struct"  — packed struct { T lanes[N]; }   (Phase 2)
   *   "scalars" — N separate scalars               (Phase 2)
   *   "array"   — T[N]                              (Phase 2)
   */
  std::unique_ptr<VecLowering> makeVecLowering(const std::string &name);

} // namespace symir
