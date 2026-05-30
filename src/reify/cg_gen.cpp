#include "reify/cg_gen.hpp"

#include <algorithm>

namespace symir::reify {

  CallGraph genCallGraph(std::mt19937 &rng, const CGGenConfig &cfg) {
    CallGraph g;
    g.nNodes = std::max(1, cfg.nNodes);
    g.outEdges.assign(g.nNodes, {});

    // We walk successor indices in ascending order and stop once the
    // out-degree cap is hit. This biases edges toward low-numbered
    // successors — node i is more likely to call i+1 than i+5. The
    // skew is acceptable for the random whole-program generator (the
    // CG only needs to be a non-trivial DAG; perfect uniformity isn't
    // required) and keeps the inner loop O(N²) in the simple form.
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    for (int i = 0; i < g.nNodes; ++i) {
      for (int j = i + 1; j < g.nNodes; ++j) {
        if ((int) g.outEdges[i].size() >= cfg.maxOutDegree)
          break;
        if (uni(rng) < cfg.pEdge)
          g.outEdges[i].push_back(j);
      }
    }
    return g;
  }

} // namespace symir::reify
