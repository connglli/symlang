#include <algorithm>
#include <functional>
#include "analysis/cfg.hpp"

CFG CFG::build(const FunDecl &f, DiagBag &diags) {
  CFG g;

  if (f.blocks.empty()) {
    diags.error("Function has no blocks", f.span);
    return g;
  }

  // Index blocks
  g.blocks.reserve(f.blocks.size());
  for (std::size_t i = 0; i < f.blocks.size(); ++i) {
    const auto &b = f.blocks[i];
    auto key = labelKey(b.label);
    if (g.indexOf.count(key)) {
      diags.error("Duplicate block label: " + key, b.span);
      continue;
    }
    g.indexOf[key] = i;
    g.blocks.push_back(key);
  }

  g.succ.assign(f.blocks.size(), {});
  g.pred.assign(f.blocks.size(), {});

  // Determine entry: prefer "^entry" else block 0
  if (g.indexOf.count("^entry"))
    g.entry = g.indexOf["^entry"];
  else
    g.entry = 0;

  auto add_edge = [&](std::size_t from, const BlockLabel &to, SourceSpan sp) {
    auto k = labelKey(to);
    auto it = g.indexOf.find(k);
    if (it == g.indexOf.end()) {
      diags.error("Unknown block label: " + k, sp);
      return;
    }
    std::size_t dst = it->second;
    g.succ[from].push_back(dst);
    g.pred[dst].push_back(from);
  };

  // Build edges from terminators
  for (std::size_t i = 0; i < f.blocks.size(); ++i) {
    const auto &b = f.blocks[i];
    std::visit(
        [&](auto &&term) {
          using T = std::decay_t<decltype(term)>;
          if constexpr (std::is_same_v<T, BrTerm>) {
            if (term.isConditional) {
              if (term.cond) {
                add_edge(i, term.thenLabel, term.span);
                add_edge(i, term.elseLabel, term.span);
              }
            } else {
              add_edge(i, term.dest, term.span);
            }
          } else {
            // ret/unreachable: no outgoing edges
          }
        },
        b.term
    );
  }

  return g;
}

std::vector<std::size_t> CFG::rpo() const {
  std::vector<std::size_t> order;
  std::vector<char> vis(blocks.size(), 0);

  std::function<void(std::size_t)> dfs = [&](std::size_t u) {
    vis[u] = 1;
    for (auto v: succ[u])
      if (!vis[v])
        dfs(v);
    order.push_back(u);
  };
  dfs(entry);
  std::reverse(order.begin(), order.end());
  return order;
}
