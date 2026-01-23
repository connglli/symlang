#pragma once

#include <unordered_map>
#include "ast/ast.hpp"
#include "frontend/diagnostics.hpp"

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

class TypeChecker {
public:
  explicit TypeChecker(const Program &p) : prog_(p) {}

  std::unordered_map<std::string, TypeAnnotations> runAll(DiagBag &diags);

private:
  const Program &prog_;

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

  void collectStructs(DiagBag &diags);
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
