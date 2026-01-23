#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include "ast/ast.hpp"

namespace symir {

  class SIRPrinter {
  public:
    explicit SIRPrinter(std::ostream &out) : out_(out) {}

    explicit SIRPrinter(std::ostream &out, const std::unordered_map<std::string, int64_t> &model) :
        out_(out), model_(model) {}

    void print(const Program &p);

  private:
    std::ostream &out_;
    std::unordered_map<std::string, int64_t> model_;
    int indent_level_ = 0;

    void indent();
    void printType(const TypePtr &t);
    void printExpr(const Expr &e);
    void printAtom(const Atom &a);
    void printCond(const Cond &c);
    void printLValue(const LValue &lv);
    void printCoef(const Coef &c);
    void printSelectVal(const SelectVal &sv);
    void printIndex(const Index &idx);
    void printInitVal(const InitVal &iv);
    void printDomain(const Domain &d);

    std::string relOpToString(RelOp op);
    std::string atomOpToString(AtomOpKind op);
  };

} // namespace symir
