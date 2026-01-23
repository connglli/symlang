#include "analysis/definite_init.hpp"

void DefiniteInitAnalysis::run(const FunDecl &f, const CFG &cfg, DiagBag &diags) {
  Context ctx{f, cfg, diags, {}, {}};
  for (const auto &p: f.params)
    ctx.isParam[p.name.name] = true;
  for (const auto &l: f.lets) {
    if (l.init && l.init->kind != InitVal::Kind::Undef) {
      ctx.hasInit[l.name.name] = true;
    }
  }

  std::unordered_map<std::string, InitSet> in, out;
  InitSet top = makeAllFalse(f);

  for (const auto &bname: cfg.blocks) {
    in[bname] = top;
    out[bname] = top;
  }

  // Seed entry
  InitSet start = makeAllFalse(f);
  for (const auto &p: f.params)
    start[p.name.name] = true;
  for (const auto &l: f.lets) {
    if (l.init && l.init->kind != InitVal::Kind::Undef) {
      start[l.name.name] = true;
    }
  }

  in[cfg.blocks[cfg.entry]] = start;

  std::vector<std::size_t> rpo = cfg.rpo();
  bool changed = true;
  int iter = 0;
  while (changed && iter++ < 100) {
    changed = false;
    for (std::size_t idx: rpo) {
      const std::string &name = cfg.blocks[idx];
      const Block &b = f.blocks[idx];

      if (idx != cfg.entry && !cfg.pred[idx].empty()) {
        InitSet meet = makeAllTrue(f);
        for (std::size_t pidx: cfg.pred[idx]) {
          meet = meetAND(meet, out[cfg.blocks[pidx]]);
        }
        if (!sameInitSet(in[name], meet)) {
          in[name] = meet;
          changed = true;
        }
      }

      InitSet newOut = transferBlock(b, in[name], ctx);
      if (!sameInitSet(out[name], newOut)) {
        out[name] = newOut;
        changed = true;
      }
    }
  }
}

DefiniteInitAnalysis::InitSet DefiniteInitAnalysis::makeAllFalse(const FunDecl &f) {
  InitSet s;
  for (const auto &p: f.params)
    s[p.name.name] = false;
  for (const auto &l: f.lets)
    s[l.name.name] = false;
  return s;
}

DefiniteInitAnalysis::InitSet DefiniteInitAnalysis::makeAllTrue(const FunDecl &f) {
  InitSet s;
  for (const auto &p: f.params)
    s[p.name.name] = true;
  for (const auto &l: f.lets)
    s[l.name.name] = true;
  return s;
}

bool DefiniteInitAnalysis::sameInitSet(const InitSet &a, const InitSet &b) {
  if (a.size() != b.size())
    return false;
  for (auto const &[key, val]: a) {
    auto it = b.find(key);
    if (it == b.end() || it->second != val)
      return false;
  }
  return true;
}

DefiniteInitAnalysis::InitSet DefiniteInitAnalysis::meetAND(const InitSet &a, const InitSet &b) {
  InitSet r;
  for (auto const &[key, val]: a) {
    r[key] = val && b.at(key);
  }
  return r;
}

DefiniteInitAnalysis::InitSet
DefiniteInitAnalysis::transferBlock(const Block &b, InitSet state, Context &ctx) {
  auto checkLValue = [&](const LValue &lv) {
    if (state.count(lv.base.name) && !state.at(lv.base.name)) {
      ctx.diags.error("Read of possibly uninitialized local: " + lv.base.name, lv.base.span);
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
                    ctx.diags.error("Read of uninitialized local in coef: " + lid->name, lid->span);
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
                    ctx.diags.error("Read of uninitialized local: " + lid->name, lid->span);
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
