#pragma once

#include <iostream>
#include <string>
#include "ast/ast.hpp"

class ASTDumper {
public:
  explicit ASTDumper(std::ostream &out) : out_(out) {}

  void dump(const Program &p);

private:
  std::ostream &out_;
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
