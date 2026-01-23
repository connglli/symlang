#include "analysis/unused_name.hpp"
#include <unordered_set>

namespace symir {

  symir::PassResult UnusedNameAnalysis::run(FunDecl &f, DiagBag &diags) {
    std::unordered_set<std::string> used;

    auto collectLValue = [&](const LValue &lv) {
      used.insert(lv.base.name);
      for (const auto &acc: lv.accesses) {
        if (auto ai = std::get_if<AccessIndex>(&acc)) {
          if (auto lid = std::get_if<LocalOrSymId>(&ai->index)) {
            std::visit([&](auto &&id) { used.insert(id.name); }, *lid);
          }
        }
      }
    };

    auto collectExpr = [&](const Expr &e, auto &self) -> void {
      auto collectAtom = [&](const Atom &a) {
        std::visit(
            [&](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, OpAtom>) {
                if (auto lsid = std::get_if<LocalOrSymId>(&arg.coef)) {
                  std::visit([&](auto &&id) { used.insert(id.name); }, *lsid);
                }
                collectLValue(arg.rval);
              } else if constexpr (std::is_same_v<T, SelectAtom>) {
                self(arg.cond->lhs, self);
                self(arg.cond->rhs, self);
                if (auto rv = std::get_if<RValue>(&arg.vtrue)) {
                  collectLValue(*rv);
                } else {
                  const auto &coef = std::get<Coef>(arg.vtrue);
                  if (auto lsid = std::get_if<LocalOrSymId>(&coef)) {
                    std::visit([&](auto &&id) { used.insert(id.name); }, *lsid);
                  }
                }
                if (auto rv = std::get_if<RValue>(&arg.vfalse)) {
                  collectLValue(*rv);
                } else {
                  const auto &coef = std::get<Coef>(arg.vfalse);
                  if (auto lsid = std::get_if<LocalOrSymId>(&coef)) {
                    std::visit([&](auto &&id) { used.insert(id.name); }, *lsid);
                  }
                }
              } else if constexpr (std::is_same_v<T, CoefAtom>) {
                if (auto lsid = std::get_if<LocalOrSymId>(&arg.coef)) {
                  std::visit([&](auto &&id) { used.insert(id.name); }, *lsid);
                }
              } else if constexpr (std::is_same_v<T, RValueAtom>) {
                collectLValue(arg.rval);
              } else if constexpr (std::is_same_v<T, CastAtom>) {
                std::visit(
                    [&](auto &&src) {
                      using S = std::decay_t<decltype(src)>;
                      if constexpr (std::is_same_v<S, LValue>) {
                        collectLValue(src);
                      } else if constexpr (std::is_same_v<S, SymId>) {
                        used.insert(src.name);
                      }
                    },
                    arg.src
                );
              } else if constexpr (std::is_same_v<T, UnaryAtom>) {
                collectLValue(arg.rval);
              }
            },
            a.v
        );
      };
      collectAtom(e.first);
      for (const auto &t: e.rest)
        collectAtom(t.atom);
    };

    for (const auto &b: f.blocks) {
      for (const auto &ins: b.instrs) {
        std::visit(
            [&](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, AssignInstr>) {
                collectExpr(arg.rhs, collectExpr);
                used.insert(arg.lhs.base.name);
              } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                collectExpr(arg.cond.lhs, collectExpr);
                collectExpr(arg.cond.rhs, collectExpr);
              } else if constexpr (std::is_same_v<T, RequireInstr>) {
                collectExpr(arg.cond.lhs, collectExpr);
                collectExpr(arg.cond.rhs, collectExpr);
              }
            },
            ins
        );
      }
      std::visit(
          [&](auto &&t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, BrTerm>) {
              if (t.isConditional && t.cond) {
                collectExpr(t.cond->lhs, collectExpr);
                collectExpr(t.cond->rhs, collectExpr);
              }
            } else if constexpr (std::is_same_v<T, RetTerm>) {
              if (t.value)
                collectExpr(*t.value, collectExpr);
            }
          },
          b.term
      );
    }

    for (const auto &p: f.params) {
      if (used.find(p.name.name) == used.end())
        diags.warn("Unused parameter: " + p.name.name, p.span);
    }
    for (const auto &s: f.syms) {
      if (used.find(s.name.name) == used.end())
        diags.warn("Unused symbol: " + s.name.name, s.span);
    }
    for (const auto &l: f.lets) {
      if (used.find(l.name.name) == used.end())
        diags.warn("Unused local: " + l.name.name, l.span);
    }

    return symir::PassResult::Success;
  }

} // namespace symir
