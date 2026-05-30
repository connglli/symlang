#include "reify/rewrite.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>

#include "ast/sir_printer.hpp"
#include "reify/hyperparameters.hpp"

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Descriptor-value parsing
  //
  // Descriptors stringify scalars via std::to_string (ints) or a printf
  // float format (floats). std::stoll / std::stod accept both, so
  // round-tripping is straight. Both helpers return nullopt on any
  // partial parse so a malformed descriptor field just disables one
  // rewrite opportunity rather than crashing the engine.
  // ---------------------------------------------------------------------------
  static std::optional<std::int64_t> parseI64(const std::string &s) {
    try {
      size_t pos = 0;
      long long v = std::stoll(s, &pos);
      if (pos != s.size())
        return std::nullopt;
      return static_cast<std::int64_t>(v);
    } catch (...) {
      return std::nullopt;
    }
  }

  static std::optional<double> parseF64(const std::string &s) {
    // Delegate to the canonical SymIR float parser so descriptor values
    // round-trip identically to lexed float literals — same subnormal
    // handling, same signed-zero behaviour, same overflow rule.
    try {
      return parseFloatLiteral(s);
    } catch (...) {
      return std::nullopt;
    }
  }

  // ---------------------------------------------------------------------------
  // Build an Expr that evaluates to a scalar literal of the given SIR
  // type. Used to construct call arguments from descriptor.paramValues.
  // Returns nullopt for types we can't synthesize (pointers, vectors,
  // structs) — the rule then declines to apply.
  // ---------------------------------------------------------------------------
  static std::optional<Expr> makeScalarLitExpr(const std::string &sirType, const std::string &val) {
    Atom a;
    if (sirType.size() >= 1 && sirType[0] == 'i') {
      auto iv = parseI64(val);
      if (!iv)
        return std::nullopt;
      IntLit lit;
      lit.value = *iv;
      Coef coef = lit;
      CoefAtom ca;
      ca.coef = coef;
      a.v = ca;
    } else if (sirType == "f32" || sirType == "f64") {
      auto fv = parseF64(val);
      if (!fv)
        return std::nullopt;
      FloatLit lit;
      lit.value = *fv;
      Coef coef = lit;
      CoefAtom ca;
      ca.coef = coef;
      a.v = ca;
    } else {
      return std::nullopt;
    }
    Expr e;
    e.first = std::move(a);
    return e;
  }

  // ---------------------------------------------------------------------------
  // Var+bias call arg
  //
  // Build `%var + bias` for an integer call argument. `paramType` is the
  // callee's i<N> param type as a SIR string; `expectedValStr` is the
  // solved value the call should evaluate to. We scan the caller for
  // int-typed mutable lets whose declared type matches and whose init
  // is a literal we can read back, then pick uniformly. The chosen var's
  // value is `var.let_init` at the splice site because the rewrite
  // prepends to the head of the entry block — no user code has run yet.
  //
  // Returns nullopt when:
  //   - parseI64(expectedValStr) fails (descriptor value malformed);
  //   - no caller let matches the param type / has a literal init;
  //   - the computed bias overflows the var's signed range (we use the
  //     same conservative range check the call-site offset uses, so a
  //     UBSan-clean C lowering is guaranteed).
  // ---------------------------------------------------------------------------
  static std::optional<Expr> tryMakeVarBiasArg(
      const FunDecl &caller, const std::string &paramType, const std::string &expectedValStr,
      std::mt19937 &rng
  ) {
    auto expectedOpt = parseI64(expectedValStr);
    if (!expectedOpt)
      return std::nullopt;
    int paramBits = std::atoi(paramType.c_str() + 1);
    if (paramBits <= 0)
      return std::nullopt;

    struct Cand {
      const LetDecl *ld;
      std::int64_t initVal;
    };

    std::vector<Cand> cands;
    cands.reserve(caller.lets.size());
    for (const auto &ld: caller.lets) {
      if (!ld.isMutable)
        continue;
      if (!ld.init || ld.init->kind != InitVal::Kind::Int)
        continue;
      if (SIRPrinter::typeToString(ld.type) != paramType)
        continue;
      cands.push_back({&ld, std::get<IntLit>(ld.init->value).value});
    }
    if (cands.empty())
      return std::nullopt;

    std::uniform_int_distribution<size_t> pick(0, cands.size() - 1);
    const Cand &c = cands[pick(rng)];

    // bias = expected - var.let_init. Same overflow + range check the
    // call-site offset uses — keeps the C lowering UBSan-clean and
    // makes the wrap math observably identical to the literal path.
    std::int64_t bias = 0;
    if (__builtin_sub_overflow((std::int64_t) *expectedOpt, c.initVal, &bias))
      return std::nullopt;
    if (paramBits < 64) {
      std::int64_t maxv = (std::int64_t{1} << (paramBits - 1)) - 1;
      std::int64_t minv = -(std::int64_t{1} << (paramBits - 1));
      if (bias < minv || bias > maxv)
        return std::nullopt;
    }

    LValue lv;
    lv.base = c.ld->name;
    Atom varAtom;
    varAtom.v = RValueAtom{lv, {}};
    Expr e;
    e.first = std::move(varAtom);
    if (bias != 0) {
      IntLit lit;
      lit.value = bias;
      Coef coef = lit;
      CoefAtom ca;
      ca.coef = coef;
      Atom biasAtom;
      biasAtom.v = std::move(ca);
      Expr::Tail tail;
      tail.op = (bias >= 0) ? AddOp::Plus : AddOp::Minus;
      // For Minus we negate the literal so the tail reads `... - |bias|`.
      // Negating INT64_MIN would overflow, so guard against it; the
      // bias range check above already rejects values where -bias
      // can't be represented in the param's signed range, but bias
      // can still equal INT64_MIN for paramBits=64.
      if (tail.op == AddOp::Minus) {
        if (bias == std::numeric_limits<std::int64_t>::min())
          return std::nullopt;
        std::get<IntLit>(std::get<CoefAtom>(biasAtom.v).coef).value = -bias;
      }
      tail.atom = std::move(biasAtom);
      e.rest.push_back(std::move(tail));
    }
    return e;
  }

  // ---------------------------------------------------------------------------
  // LiteralToCallRule
  // ---------------------------------------------------------------------------

  namespace {

    class LiteralToCallRule : public RewriteRule {
    public:
      const char *name() const override { return "LiteralToCall"; }

      std::vector<RewriteSite> findSites(const FunDecl &caller) override {
        std::vector<RewriteSite> sites;
        for (size_t i = 0; i < caller.lets.size(); ++i) {
          const auto &ld = caller.lets[i];
          if (!ld.init)
            continue;
          // Skip the checksum accumulator — it is unconditionally
          // overwritten to `0` at the top of the exit block (see
          // func_gen::buildChecksum), so any splice into its let-init
          // is dead computation. Filtering it out at the source rule
          // saves a wasted attempt budget per edge.
          if (ld.name.name == "%_chk")
            continue;
          // Only scalar literal initializers — Atom-form inits are
          // skipped (they're already calls/loads/etc.) and aggregate
          // inits are out of scope for v1.
          if (ld.init->kind == InitVal::Kind::Int) {
            RewriteSite s;
            s.kind = RewriteSite::Kind::LetInitIntLit;
            s.letIdx = static_cast<int>(i);
            s.intVal = std::get<IntLit>(ld.init->value).value;
            s.sirType = SIRPrinter::typeToString(ld.type);
            sites.push_back(s);
          } else if (ld.init->kind == InitVal::Kind::Float) {
            RewriteSite s;
            s.kind = RewriteSite::Kind::LetInitFloatLit;
            s.letIdx = static_cast<int>(i);
            s.floatVal = std::get<FloatLit>(ld.init->value).value;
            s.sirType = SIRPrinter::typeToString(ld.type);
            sites.push_back(s);
          }
        }
        return sites;
      }

      bool matchCallee(
          const RewriteSite &site, const FuncDescriptor &callee, std::size_t fixedRealizationIdx
      ) override {
        // Only the ret-type has to match: we splice in `call + (c - ret)`
        // so semantics are preserved regardless of whether the callee
        // happens to solve to the same value as the literal. Float
        // literals are excluded because IEEE 754 subtraction is not
        // generally lossless (c - ret + ret ≠ c when rounding fires),
        // which would break --validate equivalence.
        if (callee.retType != site.sirType)
          return false;
        if (site.kind == RewriteSite::Kind::LetInitFloatLit)
          return false;
        if (fixedRealizationIdx >= callee.realizations.size())
          return false;
        const auto &rz = callee.realizations[fixedRealizationIdx];
        return !rz.retValue.empty() && parseI64(rz.retValue).has_value();
      }

      bool apply(
          FunDecl &caller, const RewriteSite &site, const FuncDescriptor &callee,
          std::size_t realizationIdx, std::mt19937 &rng
      ) override {
        if (site.letIdx < 0 || (size_t) site.letIdx >= caller.lets.size())
          return false;
        const auto &rz = callee.realizations[realizationIdx];
        if (rz.paramValues.size() != callee.params.size())
          return false;
        auto retVal = parseI64(rz.retValue);
        if (!retVal)
          return false;

        // Build the call atom. Each arg is either a bare literal
        // (matching the solver's paramValue) or `%var + bias` where
        // %var is an unchanged-since-init caller scalar of the same
        // type. The var+bias path is what makes the rewrite look
        // lifelike: without it every call arg is a plain literal.
        CallAtom ca;
        GlobalId gid;
        gid.name = callee.name;
        ca.callee = gid;
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        for (size_t i = 0; i < callee.params.size(); ++i) {
          const auto &paramType = callee.params[i].type;
          const auto &paramValStr = rz.paramValues[i].second;
          // The var+bias path needs an int expected-value and an int
          // var.let_init — restrict to integer scalar param types and
          // gate on the per-arg probability.
          std::optional<Expr> argExpr;
          if (paramType.size() >= 2 && paramType[0] == 'i' && uni(rng) < rylink::hp::kPVarBiasArg) {
            argExpr = tryMakeVarBiasArg(caller, paramType, paramValStr, rng);
          }
          if (!argExpr)
            argExpr = makeScalarLitExpr(paramType, paramValStr);
          if (!argExpr)
            return false; // unsupported arg type — bail without mutating
          ca.args.push_back(std::make_shared<Expr>(std::move(*argExpr)));
        }
        Atom callAtom;
        callAtom.v = std::move(ca);

        // Offset literal: `c - ret` under bitvector arithmetic. SymIR
        // `T + lit` truncates with wraparound (§6.5), so the BV-side
        // math always reproduces `c`. The C backend, however, emits
        // the call site as a signed-int addition, and UBSan trips
        // when the intermediate `ret + offset` overflows the let's
        // signed range — even though the wrapped result equals `c`.
        //
        // Decline the rewrite when the offset doesn't fit in the
        // let's signed range. Concretely: compute `c - ret` in 64-bit
        // signed (rejecting the i64-vs-i64 overflow case via the
        // builtin), then range-check against the destination width.
        //
        // The check is conservative — some perfectly fine programs
        // are skipped — but the alternative (BV-correct C output) is
        // a deep change to the backend; declining costs at most a
        // missed splice on a single edge and the rewrite engine just
        // tries another candidate site.
        int parsedBits = 0;
        if (site.sirType.size() < 2 || site.sirType[0] != 'i' ||
            (parsedBits = std::atoi(site.sirType.c_str() + 1)) <= 0)
          return false;
        std::int64_t offsetVal = 0;
        if (__builtin_sub_overflow((std::int64_t) site.intVal, (std::int64_t) *retVal, &offsetVal))
          return false;
        if (parsedBits < 64) {
          std::int64_t maxv = (std::int64_t{1} << (parsedBits - 1)) - 1;
          std::int64_t minv = -(std::int64_t{1} << (parsedBits - 1));
          if (offsetVal < minv || offsetVal > maxv)
            return false;
        }
        IntLit offsetLit;
        offsetLit.value = offsetVal;

        // Build `call (+ offset)?` as the RHS of an AssignInstr that
        // we prepend to the entry block: `%x = call @callee(args) (+
        // offset)?;`. The original literal init stays in place — it
        // runs before any block, then the prepended assign overwrites
        // %x with the semantically-equivalent call expression. We
        // can't fold the call straight into the let-init because
        // InitVal::Atom holds a single Atom and we need an Expr (call
        // + offset).
        Expr rhs;
        rhs.first = std::move(callAtom);
        if (offsetLit.value != 0) {
          Coef offsetCoef = offsetLit;
          CoefAtom offsetCa;
          offsetCa.coef = offsetCoef;
          Atom offsetAtom;
          offsetAtom.v = std::move(offsetCa);
          Expr::Tail tail;
          tail.op = AddOp::Plus;
          tail.atom = std::move(offsetAtom);
          rhs.rest.push_back(std::move(tail));
        }

        AssignInstr ai;
        ai.lhs.base = caller.lets[site.letIdx].name;
        ai.rhs = std::move(rhs);

        // Caller must have at least one block (rysmith always emits
        // an `^entry`). Prepend the assignment so it runs before any
        // user code that reads the let.
        if (caller.blocks.empty())
          return false;

        // Loop guard: if any block branches back to the entry,
        // prepending into the entry block re-fires the call on every
        // back-edge traversal — resetting the let to its original
        // literal value every iteration, which can flip back-edge
        // conditions into infinite loops. The original let-init runs
        // once before any block, so the leaf doesn't have this
        // problem; only the spliced AssignInstr does. Decline if the
        // entry has any predecessor — including a self-loop from the
        // entry's own terminator (start at bi=0 so we don't miss it).
        const std::string &entryLabel = caller.blocks.front().label.name;
        for (size_t bi = 0; bi < caller.blocks.size(); ++bi) {
          const auto &term = caller.blocks[bi].term;
          if (auto br = std::get_if<BrTerm>(&term)) {
            if (br->isConditional) {
              if (br->thenLabel.name == entryLabel || br->elseLabel.name == entryLabel)
                return false;
            } else if (br->dest.name == entryLabel) {
              return false;
            }
          }
        }

        caller.blocks.front().instrs.insert(
            caller.blocks.front().instrs.begin(), Instr{std::move(ai)}
        );
        return true;
      }
    };

  } // namespace

  std::unique_ptr<RewriteRule> makeLiteralToCallRule() {
    return std::make_unique<LiteralToCallRule>();
  }

  // ---------------------------------------------------------------------------
  // Engine
  // ---------------------------------------------------------------------------

  RewriteResult RewriteEngine::rewriteEdge(
      FunDecl &caller, const FuncDescriptor &callee, std::size_t fixedRealizationIdx,
      std::mt19937 &rng
  ) {
    RewriteResult res;

    // Collect (rule, site) pairs.
    struct Candidate {
      RewriteRule *rule;
      RewriteSite site;
    };

    std::vector<Candidate> cands;
    for (auto &rule: rules_) {
      auto sites = rule->findSites(caller);
      for (auto &s: sites) {
        // Skip sites already consumed by an earlier rewriteEdge call —
        // stacking calls on the same let-init produces left-to-right
        // chains like `f1() + f2() + ...` whose prefix sums can wrap
        // in unintended ways even though each individual rewrite is
        // BV-sound. See RewriteEngine class header for the full note.
        if (consumed_.count(consumedKey(&caller, s.letIdx)))
          continue;
        cands.push_back({rule.get(), s});
      }
    }
    res.sitesFound = static_cast<int>(cands.size());
    if (cands.empty())
      return res;

    std::shuffle(cands.begin(), cands.end(), rng);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    int attempts = 0;
    for (auto &c: cands) {
      if (attempts++ >= rylink::hp::kMaxAttemptsPerEdge)
        break;
      if (uni(rng) >= rylink::hp::kPRewrite)
        continue;
      if (!c.rule->matchCallee(c.site, callee, fixedRealizationIdx))
        continue;
      if (c.rule->apply(caller, c.site, callee, fixedRealizationIdx, rng)) {
        consumed_.insert(consumedKey(&caller, c.site.letIdx));
        ++res.sitesRewritten;
        return res; // one splice per edge is enough for v1
      }
    }
    return res;
  }

} // namespace symir::reify
