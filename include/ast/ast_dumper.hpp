#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include "ast/ast.hpp"

namespace symir {

  class ASTDumper {
  public:
    explicit ASTDumper(std::ostream &out) : out_(out) {}

    explicit ASTDumper(std::ostream &out, const std::unordered_map<std::string, int64_t> &model) :
        out_(out), model_(model) {}

    void dump(const Program &p);

  private:
    std::ostream &out_;
    std::unordered_map<std::string, int64_t> model_;
    int indent_level_ = 0;

    void indent();
    void dumpType(const TypePtr &t);
    void dumpExpr(const Expr &e);
    void dumpAtom(const Atom &a);
    void dumpCond(const Cond &c);
    void dumpLValue(const LValue &lv);
    void dumpCoef(const Coef &c);
    void dumpSelectVal(const SelectVal &sv);
    void dumpIndex(const Index &idx);
    void dumpInitVal(const InitVal &iv);
    void dumpDomain(const Domain &d);

    std::string relOpToString(RelOp op);
    std::string atomOpToString(AtomOpKind op);
  };

} // namespace symir
