#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "ast/ast.hpp"
#include "frontend/diagnostics.hpp"

struct CFG {
  // Block order as in function
  std::vector<std::string> blocks; // "^entry", "^b1", ...
  std::unordered_map<std::string, std::size_t> indexOf;

  // Adjacency by indices
  std::vector<std::vector<std::size_t>> succ;
  std::vector<std::vector<std::size_t>> pred;

  std::size_t entry = 0; // index into blocks

  static std::string labelKey(const BlockLabel &b) { return b.name; }

  static CFG build(const FunDecl &f, DiagBag &diags);

  // Reverse postorder from entry (useful for forward dataflow)
  std::vector<std::size_t> rpo() const;
};
