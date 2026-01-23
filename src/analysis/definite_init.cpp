#include "analysis/definite_init.hpp"

namespace symir {

  symir::PassResult DefiniteInitAnalysis::run(FunDecl &f, DiagBag &diags) {
    CFG cfg = CFG::build(f, diags);
    if (diags.hasErrors())
      return symir::PassResult::Error;

    Problem p(f, diags);
    symir::DataflowSolver<InitSet>::solve(f, cfg, p);

    return diags.hasErrors() ? symir::PassResult::Error : symir::PassResult::Success;
  }

  DefiniteInitAnalysis::Problem::Problem(const FunDecl &f, DiagBag &diags) : f_(f), diags_(diags) {}

  DefiniteInitAnalysis::InitSet DefiniteInitAnalysis::Problem::bottom() {
    InitSet s;
    for (const auto &p: f_.params)
      s[p.name.name] = true;
    for (const auto &l: f_.lets)
      s[l.name.name] = true;
    for (const auto &sy: f_.syms)
      s[sy.name.name] = true;
    return s;
  }

  DefiniteInitAnalysis::InitSet DefiniteInitAnalysis::Problem::entryState() {
    InitSet s;
    // Params and symbols are initialized
    for (const auto &p: f_.params)
      s[p.name.name] = true;
    for (const auto &sy: f_.syms)
      s[sy.name.name] = true;
    // Locals are initialized only if they have a non-undef initializer
    for (const auto &l: f_.lets) {
      s[l.name.name] = (l.init && l.init->kind != InitVal::Kind::Undef);
    }
    return s;
  }

  DefiniteInitAnalysis::InitSet
  DefiniteInitAnalysis::Problem::meet(const InitSet &lhs, const InitSet &rhs) {
    InitSet r;
    for (auto const &[key, val]: lhs) {
      r[key] = val && rhs.at(key);
    }
    return r;
  }

  bool DefiniteInitAnalysis::Problem::equal(const InitSet &lhs, const InitSet &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (auto const &[key, val]: lhs) {
      auto it = rhs.find(key);
      if (it == rhs.end() || it->second != val)
        return false;
    }
    return true;
  }

  DefiniteInitAnalysis::InitSet
  DefiniteInitAnalysis::Problem::transfer(const Block &b, const InitSet &in) {
    InitSet state = in;

    auto checkLValue = [&](const LValue &lv) {
      if (state.count(lv.base.name) && !state.at(lv.base.name)) {
        diags_.error("Read of possibly uninitialized local: " + lv.base.name, lv.base.span);
      }
    };

    auto checkExpr = [&](const Expr &e, auto &self) -> void {
      auto checkAtom = [&](const Atom &a) {
        std::visit(
            [&](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, OpAtom>) {
                if (auto lsid = std::get_if<LocalOrSymId>(&arg.coef)) {
                  if (auto lid = std::get_if<LocalId>(lsid)) {
                    if (state.count(lid->name) && !state.at(lid->name))
                      diags_.error("Read of uninitialized local in coef: " + lid->name, lid->span);
                  }
                }
                checkLValue(arg.rval);
              } else if constexpr (std::is_same_v<T, SelectAtom>) {
                self(arg.cond->lhs, self);
                self(arg.cond->rhs, self);
              } else if constexpr (std::is_same_v<T, RValueAtom>) {
                checkLValue(arg.rval);
              } else if constexpr (std::is_same_v<T, CoefAtom>) {
                if (auto lsid = std::get_if<LocalOrSymId>(&arg.coef)) {
                  if (auto lid = std::get_if<LocalId>(lsid)) {
                    if (state.count(lid->name) && !state.at(lid->name))
                      diags_.error("Read of uninitialized local: " + lid->name, lid->span);
                  }
                }
              }
            },
            a.v
        );
      };
      checkAtom(e.first);
      for (const auto &t: e.rest)
        checkAtom(t.atom);
    };

    for (const auto &ins: b.instrs) {
      std::visit(
          [&](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, AssignInstr>) {
              checkExpr(arg.rhs, checkExpr);
              state[arg.lhs.base.name] = true;
            } else if constexpr (std::is_same_v<T, AssumeInstr>) {
              checkExpr(arg.cond.lhs, checkExpr);
              checkExpr(arg.cond.rhs, checkExpr);
            } else if constexpr (std::is_same_v<T, RequireInstr>) {
              checkExpr(arg.cond.lhs, checkExpr);
              checkExpr(arg.cond.rhs, checkExpr);
            }
          },
          ins
      );
    }

    std::visit(
        [&](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, BrTerm>) {
            if (arg.isConditional && arg.cond) {
              checkExpr(arg.cond->lhs, checkExpr);
              checkExpr(arg.cond->rhs, checkExpr);
            }
          } else if constexpr (std::is_same_v<T, RetTerm>) {
            if (arg.value)
              checkExpr(*arg.value, checkExpr);
          }
        },
        b.term
    );

    return state;
  }

} // namespace symir
