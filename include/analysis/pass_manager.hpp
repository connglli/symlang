#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "ast/ast.hpp"
#include "frontend/diagnostics.hpp"

namespace symir {

  /**
   * Represents the outcome of a compiler pass.
   */
  enum class PassResult {
    Success,
    Error, // Stop pipeline execution
  };

  /**
   * Base class for all compiler passes.
   */
  class Pass {
  public:
    virtual ~Pass() = default;
    virtual std::string name() const = 0;
  };

  /**
   * A pass that operates on the entire SymIR module (Program).
   */
  class ModulePass : public Pass {
  public:
    virtual PassResult run(Program &prog, DiagBag &diags) = 0;
  };

  /**
   * A pass that operates on a single function declaration.
   */
  class FunctionPass : public Pass {
  public:
    virtual PassResult run(FunDecl &fun, DiagBag &diags) = 0;
  };

  /**
   * Orchestrates the execution of a series of compiler passes.
   */
  class PassManager {
  public:
    explicit PassManager(DiagBag &diags) : diags_(diags) {}

    /**
     * Registers a module-level pass.
     */
    void addModulePass(std::unique_ptr<ModulePass> pass) {
      modulePasses_.push_back(std::move(pass));
    }

    /**
     * Registers a function-level pass.
     * The manager will automatically run this pass on every function in the program.
     */
    void addFunctionPass(std::unique_ptr<FunctionPass> pass);

    /**
     * Executes all registered passes on the program in the order they were added.
     */
    PassResult run(Program &prog);

  private:
    DiagBag &diags_;
    std::vector<std::unique_ptr<ModulePass>> modulePasses_;
  };

} // namespace symir
