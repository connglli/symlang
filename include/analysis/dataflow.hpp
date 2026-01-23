#pragma once

#include <vector>
#include "analysis/cfg.hpp"

namespace symir {

  // Generic Forward Dataflow Problem
  template<typename State>
  class DataflowProblem {
  public:
    virtual ~DataflowProblem() = default;

    // Initial value for all blocks (usually Top or Bottom)
    virtual State bottom() = 0;

    // Initial value for entry block
    virtual State entryState() = 0;

    // Meet/Join operator (e.g., AND for definite init, OR for liveness)
    virtual State meet(const State &lhs, const State &rhs) = 0;

    // Transfer function for a block
    virtual State transfer(const Block &block, const State &in) = 0;

    // Check if states are identical
    virtual bool equal(const State &lhs, const State &rhs) = 0;
  };

  template<typename State>
  class DataflowSolver {
  public:
    struct Result {
      std::vector<State> in;
      std::vector<State> out;
    };

    static Result solve(const FunDecl &f, const CFG &cfg, DataflowProblem<State> &problem) {
      size_t numBlocks = cfg.blocks.size();
      Result res;
      res.in.assign(numBlocks, problem.bottom());
      res.out.assign(numBlocks, problem.bottom());

      res.in[cfg.entry] = problem.entryState();

      std::vector<size_t> rpo = cfg.rpo();
      bool changed = true;
      while (changed) {
        changed = false;
        for (size_t idx: rpo) {
          if (idx != cfg.entry && !cfg.pred[idx].empty()) {
            State meetState = res.out[cfg.pred[idx][0]];
            for (size_t i = 1; i < cfg.pred[idx].size(); ++i) {
              meetState = problem.meet(meetState, res.out[cfg.pred[idx][i]]);
            }
            res.in[idx] = meetState;
          }

          State newOut = problem.transfer(f.blocks[idx], res.in[idx]);
          if (!problem.equal(res.out[idx], newOut)) {
            res.out[idx] = newOut;
            changed = true;
          }
        }
      }
      return res;
    }
  };

} // namespace symir
