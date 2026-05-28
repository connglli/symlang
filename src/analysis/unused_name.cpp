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

    // Shared atom visitor — walks all names referenced by any atom variant.
    // `recurseExpr` handles embedded expressions (SelectAtom cond / maskExpr).
    auto collectAtom = [&](const Atom &a, auto &recurseExpr) {
      std::visit(
          [&](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, OpAtom>) {
              if (auto lsid = std::get_if<LocalOrSymId>(&arg.coef))
                std::visit([&](auto &&id) { used.insert(id.name); }, *lsid);
              collectLValue(arg.rval);
            } else if constexpr (std::is_same_v<T, SelectAtom>) {
              if (arg.cond) {
                recurseExpr(arg.cond->lhs, recurseExpr);
                recurseExpr(arg.cond->rhs, recurseExpr);
              } else if (arg.maskExpr) {
                recurseExpr(*arg.maskExpr, recurseExpr);
              }
              auto handleSv = [&](const SelectVal &sv) {
                if (auto rv = std::get_if<RValue>(&sv))
                  collectLValue(*rv);
                else if (auto cf = std::get_if<Coef>(&sv))
                  if (auto lsid = std::get_if<LocalOrSymId>(cf))
                    std::visit([&](auto &&id) { used.insert(id.name); }, *lsid);
              };
              handleSv(arg.vtrue);
              handleSv(arg.vfalse);
            } else if constexpr (std::is_same_v<T, CmpAtom>) {
              auto handleSv = [&](const SelectVal &sv) {
                if (auto rv = std::get_if<RValue>(&sv))
                  collectLValue(*rv);
                else if (auto cf = std::get_if<Coef>(&sv))
                  if (auto lsid = std::get_if<LocalOrSymId>(cf))
                    std::visit([&](auto &&id) { used.insert(id.name); }, *lsid);
              };
              handleSv(arg.lhs);
              handleSv(arg.rhs);
            } else if constexpr (std::is_same_v<T, CoefAtom>) {
              if (auto lsid = std::get_if<LocalOrSymId>(&arg.coef))
                std::visit([&](auto &&id) { used.insert(id.name); }, *lsid);
            } else if constexpr (std::is_same_v<T, RValueAtom>) {
              collectLValue(arg.rval);
            } else if constexpr (std::is_same_v<T, CastAtom>) {
              std::visit(
                  [&](auto &&src) {
                    using S = std::decay_t<decltype(src)>;
                    if constexpr (std::is_same_v<S, LValue>)
                      collectLValue(src);
                    else if constexpr (std::is_same_v<S, SymId>)
                      used.insert(src.name);
                  },
                  arg.src
              );
            } else if constexpr (std::is_same_v<T, UnaryAtom>) {
              collectLValue(arg.rval);
            } else if constexpr (std::is_same_v<T, AddrAtom>) {
              collectLValue(arg.lv);
            } else if constexpr (std::is_same_v<T, LoadAtom>) {
              collectLValue(arg.rval);
            } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
              collectLValue(arg.rval);
              std::visit(
                  [&](auto &&iv) {
                    using IV = std::decay_t<decltype(iv)>;
                    if constexpr (std::is_same_v<IV, LocalOrSymId>)
                      std::visit([&](auto &&id) { used.insert(id.name); }, iv);
                  },
                  arg.index
              );
            } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
              collectLValue(arg.rval);
            } else if constexpr (std::is_same_v<T, CallAtom>) {
              // [v0.2.2] Walk each argument expression so locals/params
              // referenced inside `call @f(args...)` are marked used.
              for (const auto &ap: arg.args)
                recurseExpr(*ap, recurseExpr);
            }
          },
          a.v
      );
    };

    auto collectExpr = [&](const Expr &e, auto &self) -> void {
      collectAtom(e.first, self);
      for (const auto &t: e.rest)
        collectAtom(t.atom, self);
    };

    std::function<void(const InitVal &)> collectInitVal = [&](const InitVal &iv) {
      switch (iv.kind) {
        case InitVal::Kind::Local:
          used.insert(std::get<LocalId>(iv.value).name);
          break;
        case InitVal::Kind::Sym:
          used.insert(std::get<SymId>(iv.value).name);
          break;
        case InitVal::Kind::Aggregate:
          for (const auto &child: std::get<std::vector<InitValPtr>>(iv.value))
            if (child)
              collectInitVal(*child);
          break;
        case InitVal::Kind::Atom:
          if (auto &atomPtr = std::get<AtomPtr>(iv.value))
            collectAtom(*atomPtr, collectExpr);
          break;
        default:
          break;
      }
    };
    for (const auto &l: f.lets) {
      if (l.init)
        collectInitVal(*l.init);
    }

    for (const auto &b: f.blocks) {
      for (const auto &ins: b.instrs) {
        std::visit(
            [&](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, AssignInstr>) {
                collectExpr(arg.rhs, collectExpr);
                collectLValue(arg.lhs);
              } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                collectExpr(arg.cond.lhs, collectExpr);
                collectExpr(arg.cond.rhs, collectExpr);
              } else if constexpr (std::is_same_v<T, RequireInstr>) {
                collectExpr(arg.cond.lhs, collectExpr);
                collectExpr(arg.cond.rhs, collectExpr);
              } else if constexpr (std::is_same_v<T, StoreInstr>) {
                collectExpr(arg.ptr, collectExpr);
                collectExpr(arg.val, collectExpr);
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
