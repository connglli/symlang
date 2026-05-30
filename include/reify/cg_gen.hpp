#pragma once

// [v0.2.2] cg_gen — random DAG call-graph generator for rylink.
//
// Nodes are numbered [0, N). Node 0 is the entry. Edges (i, j) require
// i < j so the graph is necessarily acyclic (no recursion). Edge density
// is controlled by hyperparameters::kPEdge and capped by kMaxOutDegree.

#include <cstddef>
#include <random>
#include <vector>

namespace symir::reify {

  struct CallGraph {
    int nNodes = 0;
    // Adjacency list: outEdges[i] = nodes that node i calls. Always sorted
    // ascending, all entries > i.
    std::vector<std::vector<int>> outEdges;

    int entry() const { return 0; }
  };

  struct CGGenConfig {
    int nNodes = 4;
    double pEdge = 0.45;
    int maxOutDegree = 3;
  };

  CallGraph genCallGraph(std::mt19937 &rng, const CGGenConfig &cfg);

} // namespace symir::reify
