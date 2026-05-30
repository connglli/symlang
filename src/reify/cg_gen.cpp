#include "reify/cg_gen.hpp"

#include <algorithm>

namespace symir::reify {

  CallGraph genCallGraph(std::mt19937 &rng, const CGGenConfig &cfg) {
    CallGraph g;
    g.nNodes = std::max(1, cfg.nNodes);
    g.outEdges.assign(g.nNodes, {});

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
