#include "analysis/reachability.hpp"
#include <queue>
#include <unordered_set>
#include "analysis/cfg.hpp"

namespace symir {

  symir::PassResult ReachabilityAnalysis::run(FunDecl &f, DiagBag &diags) {
    CFG cfg = CFG::build(f, diags);
    if (diags.hasErrors())
      return symir::PassResult::Error;

    std::unordered_set<size_t> visited;
    std::queue<size_t> worklist;

    worklist.push(cfg.entry);
    visited.insert(cfg.entry);

    while (!worklist.empty()) {
      size_t curr = worklist.front();
      worklist.pop();

      for (size_t next: cfg.succ[curr]) {
        if (visited.find(next) == visited.end()) {
          visited.insert(next);
          worklist.push(next);
        }
      }
    }

    if (visited.size() < cfg.blocks.size()) {
      for (size_t i = 0; i < cfg.blocks.size(); ++i) {
        if (visited.find(i) == visited.end()) {
          diags.warn("Unreachable basic block: " + cfg.blocks[i], f.blocks[i].label.span);
        }
      }
    }

    return symir::PassResult::Success;
  }

} // namespace symir
