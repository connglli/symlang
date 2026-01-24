#pragma once

#include <vector>
#include "analysis/cfg.hpp"

namespace symir {

  /**
   * Generic interface for a Forward Dataflow Problem.
   * @tparam State The type representing the dataflow information (e.g., bitset, map).
   */
  template<typename State>
  class DataflowProblem {
  public:
    virtual ~DataflowProblem() = default;

    /**
     * The 'bottom' value of the lattice, used for initializing non-entry blocks.
     */
    virtual State bottom() = 0;

    /**
     * The state at the start of the entry block.
     */
    virtual State entryState() = 0;

    /**
     * The meet (or join) operator that combines information from multiple predecessors.
     */
    virtual State meet(const State &lhs, const State &rhs) = 0;

    /**
     * The transfer function that computes the 'out' state of a block given its 'in' state.
     */
    virtual State transfer(const Block &block, const State &in) = 0;

    /**
     * Checks if two dataflow states are equal.
     */
    virtual bool equal(const State &lhs, const State &rhs) = 0;
  };

  /**
   * Worklist-based iterative solver for forward dataflow problems.
   */
  template<typename State>
  class DataflowSolver {
  public:
    struct Result {
      std::vector<State> in;
      std::vector<State> out;
    };

    /**
     * Solves the dataflow problem on a given function.
     */
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
