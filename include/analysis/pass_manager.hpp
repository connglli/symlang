#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "ast/ast.hpp"
#include "frontend/diagnostics.hpp"

namespace symir {

  enum class PassResult {
    Success,
    Error, // Stop pipeline
  };

  class Pass {
  public:
    virtual ~Pass() = default;
    virtual std::string name() const = 0;
  };

  class ModulePass : public Pass {
  public:
    virtual PassResult run(Program &prog, DiagBag &diags) = 0;
  };

  class FunctionPass : public Pass {
  public:
    virtual PassResult run(FunDecl &fun, DiagBag &diags) = 0;
  };

  class PassManager {
  public:
    explicit PassManager(DiagBag &diags) : diags_(diags) {}

    void addModulePass(std::unique_ptr<ModulePass> pass) {
      modulePasses_.push_back(std::move(pass));
    }

    // Helper to add function passes wrapped in a module pass that iterates over functions
    void addFunctionPass(std::unique_ptr<FunctionPass> pass);

    PassResult run(Program &prog);

  private:
    DiagBag &diags_;
    std::vector<std::unique_ptr<ModulePass>> modulePasses_;
  };

} // namespace symir
