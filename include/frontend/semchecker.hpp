#pragma once

#include <string>
#include <unordered_set>
#include "analysis/pass_manager.hpp"

namespace symir {

  /**
   * Performs semantic analysis on the SymIR program.
   * Checks for duplicate declarations, invalid sigils, and other
   * well-formedness constraints not captured by the grammar or type checker.
   */
  class SemChecker : public symir::ModulePass {
  public:
    std::string name() const override { return "SemChecker"; }

    /**
     * Executes the semantic checker on the program.
     */
    symir::PassResult run(Program &prog, DiagBag &diags) override;

  private:
    void checkStruct(const StructDecl &s, DiagBag &diags);
    void checkFunction(const FunDecl &f, DiagBag &diags);
    void checkSigils(const FunDecl &f, DiagBag &diags);
    void checkDuplicates(const FunDecl &f, DiagBag &diags);
  };

} // namespace symir
