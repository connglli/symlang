#include "analysis/pass_manager.hpp"

namespace symir {

  class FunctionPassWrapper : public ModulePass {
  public:
    explicit FunctionPassWrapper(std::unique_ptr<FunctionPass> pass) : pass_(std::move(pass)) {}

    std::string name() const override { return pass_->name(); }

    PassResult run(Program &prog, DiagBag &diags) override {
      bool failed = false;
      for (auto &f: prog.funs) {
        if (pass_->run(f, diags) == PassResult::Error) {
          failed = true;
        }
      }
      return failed ? PassResult::Error : PassResult::Success;
    }

  private:
    std::unique_ptr<FunctionPass> pass_;
  };

  void PassManager::addFunctionPass(std::unique_ptr<FunctionPass> pass) {
    addModulePass(std::make_unique<FunctionPassWrapper>(std::move(pass)));
  }

  PassResult PassManager::run(Program &prog) {
    for (auto &pass: modulePasses_) {
      if (pass->run(prog, diags_) == PassResult::Error) {
        return PassResult::Error;
      }
    }
    return PassResult::Success;
  }

} // namespace symir
