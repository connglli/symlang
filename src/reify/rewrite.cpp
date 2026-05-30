#include "reify/rewrite.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

#include "ast/sir_printer.hpp"
#include "reify/hyperparameters.hpp"

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Type-string helper
  // ---------------------------------------------------------------------------
  //
  // The descriptor stores ret/param types as the canonical SIR surface
  // syntax (e.g. "i32", "f64", "ptr i32"). We need the same encoding for
  // a TypePtr coming out of the parser so we can equality-compare. The
  // SIRPrinter is the canonical printer so we just route through it.
  static std::string typeToSir(const TypePtr &t) {
    std::ostringstream os;
    SIRPrinter sp(os);
    sp.printType(t);
    return os.str();
  }

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
    try {
      size_t pos = 0;
      double v = std::stod(s, &pos);
      if (pos != s.size())
        return std::nullopt;
      return v;
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
          // Only scalar literal initializers — Atom-form inits are
          // skipped (they're already calls/loads/etc.) and aggregate
          // inits are out of scope for v1.
          if (ld.init->kind == InitVal::Kind::Int) {
            RewriteSite s;
            s.kind = RewriteSite::Kind::LetInitIntLit;
            s.letIdx = static_cast<int>(i);
            s.intVal = std::get<IntLit>(ld.init->value).value;
            s.sirType = typeToSir(ld.type);
            sites.push_back(s);
          } else if (ld.init->kind == InitVal::Kind::Float) {
            RewriteSite s;
            s.kind = RewriteSite::Kind::LetInitFloatLit;
            s.letIdx = static_cast<int>(i);
            s.floatVal = std::get<FloatLit>(ld.init->value).value;
            s.sirType = typeToSir(ld.type);
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
          std::size_t realizationIdx
      ) override {
        if (site.letIdx < 0 || (size_t) site.letIdx >= caller.lets.size())
          return false;
        const auto &rz = callee.realizations[realizationIdx];
        if (rz.paramValues.size() != callee.params.size())
          return false;
        auto retVal = parseI64(rz.retValue);
        if (!retVal)
          return false;

        // Build the call atom.
        CallAtom ca;
        GlobalId gid;
        gid.name = callee.name;
        ca.callee = gid;
        for (size_t i = 0; i < callee.params.size(); ++i) {
          auto maybeArg = makeScalarLitExpr(callee.params[i].type, rz.paramValues[i].second);
          if (!maybeArg)
            return false; // unsupported arg type — bail without mutating
          ca.args.push_back(std::make_shared<Expr>(std::move(*maybeArg)));
        }
        Atom callAtom;
        callAtom.v = std::move(ca);

        // Offset literal: `c - ret` under bitvector arithmetic. SymIR
        // `T + lit` truncates with wraparound (§6.5), so dropping the
        // offset into the expression reproduces `c` even when the
        // intermediate subtraction overflows. We don't mask here —
        // the typechecker accepts int literals up to i64 in any int
        // slot, and the runtime narrows on use.
        IntLit offsetLit;
        offsetLit.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(site.intVal) - static_cast<std::uint64_t>(*retVal)
        );

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
      for (auto &s: sites)
        cands.push_back({rule.get(), s});
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
      if (c.rule->apply(caller, c.site, callee, fixedRealizationIdx)) {
        ++res.sitesRewritten;
        return res; // one splice per edge is enough for v1
      }
    }
    return res;
  }

} // namespace symir::reify
