#pragma once

#include <string>
#include <unordered_set>
#include "analysis/pass_manager.hpp"

namespace symir {

  class SemChecker : public symir::ModulePass {
  public:
    std::string name() const override { return "SemChecker"; }

    symir::PassResult run(Program &prog, DiagBag &diags) override;

  private:
    void checkStruct(const StructDecl &s, DiagBag &diags);
    void checkFunction(const FunDecl &f, DiagBag &diags);
    void checkSigils(const FunDecl &f, DiagBag &diags);
    void checkDuplicates(const FunDecl &f, DiagBag &diags);
  };

} // namespace symir
