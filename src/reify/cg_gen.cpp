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

    // Connect any node without an incoming edge by force. Without
    // this, a node j > 0 that no i < j happened to draw stays
    // unreachable from the entry: its body still gets emitted into
    // the bundle but no rewrite ever runs against it, so the bundled
    // program carries dead-function code. Pick a uniformly random
    // predecessor i in [0, j) that has room under maxOutDegree; if
    // every candidate is already saturated we relax the cap by one
    // for this single edge (better than leaving a dead node).
    std::vector<bool> hasIncoming(g.nNodes, false);
    for (int i = 0; i < g.nNodes; ++i)
      for (int j: g.outEdges[i])
        hasIncoming[j] = true;
    for (int j = 1; j < g.nNodes; ++j) {
      if (hasIncoming[j])
        continue;
      std::vector<int> cands;
      for (int i = 0; i < j; ++i)
        if ((int) g.outEdges[i].size() < cfg.maxOutDegree)
          cands.push_back(i);
      // Fall back to any predecessor when none has room — keeping the
      // graph connected is more important than the soft out-degree
      // cap. This only fires in tiny edge cases (maxOutDegree=1 with
      // an early high-fanout block) and only ever exceeds the cap by 1.
      if (cands.empty())
        for (int i = 0; i < j; ++i)
          cands.push_back(i);
      std::uniform_int_distribution<int> pick(0, (int) cands.size() - 1);
      int i = cands[pick(rng)];
      g.outEdges[i].push_back(j);
      std::sort(g.outEdges[i].begin(), g.outEdges[i].end());
      hasIncoming[j] = true;
    }
    return g;
  }

} // namespace symir::reify
