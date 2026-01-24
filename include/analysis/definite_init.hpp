#pragma once

#include <unordered_map>
#include "analysis/dataflow.hpp"
#include "analysis/pass_manager.hpp"

namespace symir {

  /**
   * Performs definite initialization analysis on a function.
   * Ensures that every local variable is assigned a value before it is read.
   * Uses a forward dataflow analysis (Must-Init).
   */
  class DefiniteInitAnalysis : public symir::FunctionPass {
  public:
    std::string name() const override { return "DefiniteInitAnalysis"; }

    /**
     * Executes the analysis on the function.
     */
    symir::PassResult run(FunDecl &f, DiagBag &diags) override;

  private:
    using InitSet = std::unordered_map<std::string, bool>;

    /**
     * Dataflow problem definition for definite initialization.
     * The state is a map from variable name to a boolean (is initialized).
     */
    class Problem : public symir::DataflowProblem<InitSet> {
    public:
      Problem(const FunDecl &f, DiagBag &diags);

      InitSet bottom() override;
      InitSet entryState() override;
      InitSet meet(const InitSet &lhs, const InitSet &rhs) override;
      InitSet transfer(const Block &block, const InitSet &in) override;
      bool equal(const InitSet &lhs, const InitSet &rhs) override;

    private:
      const FunDecl &f_;
      DiagBag &diags_;
    };
  };

} // namespace symir
