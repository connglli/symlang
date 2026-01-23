#pragma once

#include <string>
#include <unordered_set>
#include "ast/ast.hpp"
#include "frontend/diagnostics.hpp"

class SemChecker {
public:
  explicit SemChecker(const Program &p) : prog_(p) {}

  void run(DiagBag &diags);

private:
  const Program &prog_;

  void checkStruct(const StructDecl &s, DiagBag &diags);
  void checkFunction(const FunDecl &f, DiagBag &diags);
  void checkSigils(const FunDecl &f, DiagBag &diags);
  void checkDuplicates(const FunDecl &f, DiagBag &diags);
};
