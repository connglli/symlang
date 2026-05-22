#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include "ast/ast.hpp"

namespace symir {

  /**
   * Generates C code from a SymIR program.
   * Maps SymIR constructs to their C equivalents, handling bitwidths
   * and symbolic variables (as externs).
   */
  class CBackend {
  public:
    explicit CBackend(std::ostream &out) : out_(out) {}

    /**
     * Translates the entire program to C and writes it to the output stream.
     */
    void emit(const Program &prog);

    void setNoRequire(bool val) { noRequire_ = val; }

  private:
    std::ostream &out_;
    int indent_level_ = 0;
    bool noRequire_ = false;
    std::string curFuncName_;
    std::unordered_map<std::string, std::uint32_t> varWidths_;
    TypePtr curFuncRetType_;
    // ``isDoubleCtx_`` is the lowering-time evaluation context for float
    // literals: when false, literals emit with the ``f`` suffix so an
    // expression like ``-3.0 * %f32_var`` stays in single precision under
    // FLT_EVAL_METHOD == 0. Mutated transiently around assign/store/ret/
    // cond/init/cast emission via ``CtxGuard``.
    bool isDoubleCtx_ = false;
    std::unordered_map<std::string, TypePtr> varTypes_;
    // Struct field name+type in declaration order. Linear lookup by name
    // for AccessField, indexed lookup for InitVal aggregates. Structs are
    // tiny (handful of fields), so linear scan is fine.
    std::unordered_map<std::string, std::vector<std::pair<std::string, TypePtr>>> structFields_;

    // --- Emission helpers ---
    void indent();
    void emitType(const TypePtr &type);
    void emitExpr(const Expr &expr);
    void emitAtom(const Atom &atom);
    void emitCond(const Cond &cond);
    void emitLValue(const LValue &lv);
    void emitCoef(const Coef &coef);
    void emitSelectVal(const SelectVal &sv);
    void emitIndex(const Index &idx);
    void emitInitVal(const InitVal &iv, TypePtr expectedType = nullptr);

    // --- Type query helpers ---
    // Resolve the static type of an lvalue / atom / expression. Used to
    // decide whether float-literal emission needs the ``f`` suffix. For
    // expressions, only the first atom is examined: SymIR requires every
    // operand in an Expr to share a single type, so this is sound.
    //
    // NOTE on impurity: ``getCoefType`` returns a type for a bare float
    // literal that depends on the *current* ``isDoubleCtx_``. Callers
    // therefore see the literal as inheriting the outer context — which
    // is what every existing call site (emitCond / emitStore) wants. If
    // you add a new caller, be aware the answer for ``FloatLit`` reflects
    // the surrounding context, not the literal in isolation.
    TypePtr getLValueType(const LValue &lv);
    TypePtr getAtomType(const Atom &atom);
    TypePtr getExprType(const Expr &expr);
    TypePtr getCoefType(const Coef &coef);

    // Look up a struct field by name; returns nullptr if either the
    // struct or the field is unknown.
    TypePtr findStructFieldType(const std::string &structName, const std::string &fieldName) const;
    // Look up a struct field by declaration-order index; returns nullptr
    // if the struct is unknown or the index is out of range.
    TypePtr getStructFieldTypeAt(const std::string &structName, size_t idx) const;

    // --- Mangling and naming helpers ---
    std::string getMangledSymbolName(const std::string &funcName, const std::string &symName);
    std::string mangleName(const std::string &name);
    std::string stripSigil(const std::string &name);
  };

} // namespace symir
