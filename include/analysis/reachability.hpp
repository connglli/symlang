#pragma once

#include <string>
#include "analysis/pass_manager.hpp"

namespace symir {

  class ReachabilityAnalysis : public symir::FunctionPass {
  public:
    std::string name() const override { return "ReachabilityAnalysis"; }

    symir::PassResult run(FunDecl &f, DiagBag &diags) override;
  };

} // namespace symir
