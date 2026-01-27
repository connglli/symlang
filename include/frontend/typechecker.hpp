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

    using Variant = std::variant<BoolTy, BVTy, FloatTy, std::monostate>;
    Variant v;

    bool isBool() const { return std::holds_alternative<BoolTy>(v); }

    bool isBV() const { return std::holds_alternative<BVTy>(v); }

    std::uint32_t bvBits() const { return std::get<BVTy>(v).bits; }

    bool isFloat() const { return std::holds_alternative<FloatTy>(v); }

    std::uint32_t floatBits() const { return std::get<FloatTy>(v).bits; }
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
    void checkFunction(const FunDecl &f, TypeAnnotations &ann, DiagBag &diags);

    void checkInitVal(
        const InitVal &iv, const TypePtr &targetType,
        const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, DiagBag &diags
    );

    Ty typeOfExpr(
        const Expr &e, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags,
        std::optional<std::uint32_t> expectedBits
    );

    Ty typeOfAtom(
        const Atom &a, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags,
        std::optional<std::uint32_t> expectedBits
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
        std::optional<std::uint32_t> expectedBits
    );

    Ty typeOfSelectVal(
        const SelectVal &sv, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags,
        std::optional<std::uint32_t> expectedBits
    );

    void checkCond(
        const Cond &c, const std::unordered_map<std::string, VarInfo> &vars,
        const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags
    );

    void checkLiteralRange(int64_t val, std::uint32_t bits, SourceSpan sp, DiagBag &diags);
  };

} // namespace symir
