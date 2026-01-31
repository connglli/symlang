#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "ast/ast.hpp"
#include "frontend/diagnostics.hpp"

namespace symir {

  /**
   * Represents the Control Flow Graph of a function.
   *
   * The CFG indexes basic blocks and tracks successor/predecessor relationships
   * to facilitate various program analyses and optimizations.
   */
  struct CFG {
    // List of block labels in the order they appear in the function.
    std::vector<std::string> blocks;
    // Mapping from block label to its index in the 'blocks' vector.
    std::unordered_map<std::string, std::size_t> indexOf;

    // Adjacency lists representing the edges between blocks (using indices).
    std::vector<std::vector<std::size_t>> succ;
    std::vector<std::vector<std::size_t>> pred;

    // Index of the entry block (defaults to 0).
    std::size_t entry = 0;

    /**
     * Helper to get a string key for a block label.
     */
    static std::string labelKey(const BlockLabel &b) { return b.name; }

    /**
     * Builds the CFG for a given function declaration.
     * Reports errors (like invalid branch targets) to the provided DiagBag.
     */
    static CFG build(const FunDecl &f, DiagBag &diags);

    /**
     * Computes the Reverse Postorder traversal of the CFG.
     * RPO is essential for efficient forward dataflow analysis.
     */
    std::vector<std::size_t> rpo() const;

    /**
     * Computes the shortest path to any block that ends with a 'ret' terminator.
     * Returns a map from block index to the index of the next block in the shortest path.
     */
    std::unordered_map<std::size_t, std::size_t> shortestPathToRet(const FunDecl &f) const;
  };

} // namespace symir
