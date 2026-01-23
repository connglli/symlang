#pragma once

#include <unordered_map>
#include "analysis/cfg.hpp"
#include "ast/ast.hpp"
#include "frontend/diagnostics.hpp"

class DefiniteInitAnalysis {
public:
  static void run(const FunDecl &f, const CFG &cfg, DiagBag &diags);

private:
  using InitSet = std::unordered_map<std::string, bool>;

  struct Context {
    const FunDecl &f;
    const CFG &cfg;
    DiagBag &diags;
    std::unordered_map<std::string, bool> isParam;
    std::unordered_map<std::string, bool> hasInit;
  };

  static InitSet makeAllFalse(const FunDecl &f);
  static InitSet makeAllTrue(const FunDecl &f);
  static bool sameInitSet(const InitSet &a, const InitSet &b);
  static InitSet meetAND(const InitSet &a, const InitSet &b);
  static InitSet transferBlock(const Block &b, InitSet state, Context &ctx);
};
