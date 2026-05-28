#pragma once

#include <unordered_map>
#include "analysis/pass_manager.hpp"

namespace symir {

  /**
   * Represents the result of type inference for an expression or atom.
   */
  struct Ty {
    struct BoolTy {};

    struct BVTy {
      std::uint32_t bits;
    };

    struct FloatTy {
      std::uint32_t bits;
    };

    /// Pointer type: carries the full ptr T TypePtr for downstream use.
    struct PtrTy {
      TypePtr type; // always a PtrType node
    };

    /// [v0.2.1] Vector type: carries the full <N> T TypePtr.
    struct VecTy {
      TypePtr type; // always a VecType node
    };

    struct ArrayTy {
      TypePtr type; // always an ArrayType node
    };

    struct StructTy {
      TypePtr type; // always a StructType node
    };

    using Variant =
        std::variant<BoolTy, BVTy, FloatTy, PtrTy, VecTy, ArrayTy, StructTy, std::monostate>;
    Variant v;

    bool isBool() const { return std::holds_alternative<BoolTy>(v); }

    bool isBV() const { return std::holds_alternative<BVTy>(v); }

    std::uint32_t bvBits() const { return std::get<BVTy>(v).bits; }

    bool isFloat() const { return std::holds_alternative<FloatTy>(v); }

    std::uint32_t floatBits() const { return std::get<FloatTy>(v).bits; }

    bool isPtr() const { return std::holds_alternative<PtrTy>(v); }

    TypePtr ptrType() const { return std::get<PtrTy>(v).type; }

    bool isVec() const { return std::holds_alternative<VecTy>(v); }

    TypePtr vecType() const { return std::get<VecTy>(v).type; }

    bool isArray() const { return std::holds_alternative<ArrayTy>(v); }

    TypePtr arrayType() const { return std::get<ArrayTy>(v).type; }

    bool isStruct() const { return std::holds_alternative<StructTy>(v); }

    TypePtr structType() const { return std::get<StructTy>(v).type; }
  };

  /**
   * Stores type information for AST nodes after type checking.
   */
  struct TypeAnnotations {
    std::unordered_map<NodeId, Ty> nodeTy;
  };

  /**
   * Performs bitwidth-aware type checking on the SymIR AST.
   * Ensures that bitwidths match across assignments and operations.
   */
  class TypeChecker : public symir::ModulePass {
  public:
    std::string name() const override { return "TypeChecker"; }

    /**
     * Executes the type checker on the program.
     */
    symir::PassResult run(Program &prog, DiagBag &diags) override;

  private:
    struct StructInfo {
      std::unordered_map<std::string, TypePtr> fields;
      std::vector<std::pair<std::string, TypePtr>> fieldList;
      SourceSpan declSpan;
    };

    std::unordered_map<std::string, StructInfo> structs_;

    // [v0.2.2] Callable registry — populated up-front from all `fun`,
    // `decl`, and `intrinsic` declarations. Used by `call @name(...)` to
    // resolve the callee, validate arity/types, and produce the result
    // type without depending on declaration order.
    struct CalleeInfo {
      enum class Kind { Fun, ExtLink, ExtContract, Intrinsic } kind;
      std::vector<TypePtr> paramTypes;
      TypePtr retType;
      SourceSpan declSpan;
      // Non-owning back-pointers for downstream phases.
      const FunDecl *fun = nullptr;
      const ExtDecl *ext = nullptr;
      const IntrinsicDecl *intr = nullptr;
    };

    std::unordered_map<std::string, CalleeInfo> callees_;

    struct VarInfo {
      TypePtr type;
      bool isMutable;
      bool isParam;
      SourceSpan declSpan;
    };

    struct SymInfo {
      TypePtr type;
      SymKind kind;
      SourceSpan declSpan;
    };

    // --- Internal type checking helpers ---
    void collectStructs(const Program &prog, DiagBag &diags);
    // [v0.2.2] Build the callable registry; check ext/intrinsic signatures.
    void collectCallees(const Program &prog, DiagBag &diags);
    // [v0.2.2] Validate contract well-formedness for one ExtDecl.
    void checkContract(const ExtDecl &d, TypeAnnotations &ann, DiagBag &diags);
    // [v0.2.2] Build the call graph and reject any cycle (no recursion).
    void checkNoRecursion(const Program &prog, DiagBag &diags);
    void checkFunction(const FunDecl &f, TypeAnnotations &ann, DiagBag &diags);

    void checkInitVal(
        const InitVal &iv, const TypePtr &targetType,
        const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, DiagBag &diags
    );

    Ty typeOfExpr(
        const Expr &e, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags,
        std::optional<std::uint32_t> expectedBits, TypePtr ptrCtx = nullptr
    );

    Ty typeOfAtom(
        const Atom &a, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags,
        std::optional<std::uint32_t> expectedBits, TypePtr ptrCtx = nullptr
    );

    TypePtr typeOfLValue(
        const LValue &lv, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, DiagBag &diags
    );

    void checkIndex(
        const Index &idx, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, DiagBag &diags
    );

    TypePtr typeOfCoef(
        const Coef &c, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, DiagBag &diags,
        std::optional<std::uint32_t> expectedBits,
        TypePtr ptrCtx = nullptr // non-null only when null literal needs ptr type resolution
    );

    Ty typeOfSelectVal(
        const SelectVal &sv, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags,
        std::optional<std::uint32_t> expectedBits, TypePtr ptrCtx = nullptr
    );

    void checkCond(
        const Cond &c, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags
    );

    void checkLiteralRange(int64_t val, std::uint32_t bits, SourceSpan sp, DiagBag &diags);
  };

} // namespace symir
