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
    std::unordered_map<std::string, bool> varIsFloat_; // true if f32 or f64
    std::unordered_map<std::string, bool> varIsF32_;   // true if f32, false if f64
    TypePtr curFuncRetType_;
    bool isDoubleCtx_ = false;
    std::unordered_map<std::string, TypePtr> varTypes_;
    std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>> structFields_;
    std::unordered_map<std::string, std::vector<TypePtr>> structFieldTypesOrder_;

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
    TypePtr getLValueType(const LValue &lv);
    TypePtr getAtomType(const Atom &atom);
    TypePtr getExprType(const Expr &expr);
    TypePtr getCoefType(const Coef &coef);

    // --- Mangling and naming helpers ---
    std::string getMangledSymbolName(const std::string &funcName, const std::string &symName);
    std::string mangleName(const std::string &name);
    std::string stripSigil(const std::string &name);
  };

} // namespace symir
