#include <unordered_map>
#include "analysis/dataflow.hpp"
#include "analysis/pass_manager.hpp"

class DefiniteInitAnalysis : public symir::FunctionPass {
public:
  std::string name() const override { return "DefiniteInitAnalysis"; }

  symir::PassResult run(FunDecl &f, DiagBag &diags) override;

private:
  using InitSet = std::unordered_map<std::string, bool>;

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
