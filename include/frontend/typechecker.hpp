#include <unordered_map>
#include "analysis/pass_manager.hpp"

struct Ty {
  struct BoolTy {};

  struct BVTy {
    std::uint32_t bits;
  };

  using Variant = std::variant<BoolTy, BVTy, std::monostate>;
  Variant v;

  bool isBool() const { return std::holds_alternative<BoolTy>(v); }

  bool isBV() const { return std::holds_alternative<BVTy>(v); }

  std::uint32_t bvBits() const { return std::get<BVTy>(v).bits; }
};

struct TypeAnnotations {
  std::unordered_map<NodeId, Ty> nodeTy;
};

class TypeChecker : public symir::ModulePass {
public:
  std::string name() const override { return "TypeChecker"; }

  symir::PassResult run(Program &prog, DiagBag &diags) override;

private:
  struct StructInfo {
    std::unordered_map<std::string, TypePtr> fields;
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

  void collectStructs(const Program &prog, DiagBag &diags);
  void checkFunction(const FunDecl &f, TypeAnnotations &ann, DiagBag &diags);

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

  std::optional<std::uint32_t> getBVWidth(const TypePtr &t, DiagBag &diags, SourceSpan sp);
  bool typeEquals(const TypePtr &a, const TypePtr &b);
};
