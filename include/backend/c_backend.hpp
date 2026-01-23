#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include "ast/ast.hpp"

namespace symir {

  class CBackend {
  public:
    explicit CBackend(std::ostream &out) : out_(out) {}

    void emit(const Program &prog);

  private:
    std::ostream &out_;
    int indent_level_ = 0;
    std::string curFuncName_;

    void indent();
    void emitType(const TypePtr &type);
    void emitExpr(const Expr &expr);
    void emitAtom(const Atom &atom);
    void emitCond(const Cond &cond);
    void emitLValue(const LValue &lv);
    void emitCoef(const Coef &coef);
    void emitSelectVal(const SelectVal &sv);
    void emitIndex(const Index &idx);

    std::string getMangledSymbolName(const std::string &funcName, const std::string &symName);
    std::string stripSigil(const std::string &name);
  };

} // namespace symir
