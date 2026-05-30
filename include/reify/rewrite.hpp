#pragma once

// [v0.2.2] Peephole rewrite engine for rylink.
//
// Given a caller FunDecl and a callee FuncDescriptor (with its solved
// param/ret values for one or more realizations), the engine finds
// semantically-safe rewrite sites in the caller and substitutes them with
// `call @callee(args)`. v1 ships LiteralToCallRule, which rewrites
// scalar-literal `let` initializers. The interface is shaped so future
// rules (unchanged-var-to-call, binop-fold-to-call, etc.) plug in without
// touching the engine.

#include <memory>
#include <optional>
#include <random>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "reify/func_desc.hpp"

namespace symir::reify {

  // A candidate location inside a FunDecl where a rewrite could fire.
  // Opaque to the engine — only the rule that produced it knows how to
  // apply it. Kind discriminates so a future engine can sort/filter by
  // category, but at v1 there's only one kind.
  struct RewriteSite {
    enum class Kind { LetInitIntLit, LetInitFloatLit };
    Kind kind;
    // Index into FunDecl::lets. The rule that emitted this site is
    // responsible for re-validating the index before applying (cheap
    // because v1 only mutates the init value, not the lets vector).
    int letIdx = 0;
    // The literal's value and its SIR-surface type-string. Used by the
    // engine to filter callees whose retType / ret value match without
    // re-walking the AST.
    std::int64_t intVal = 0;
    double floatVal = 0.0;
    std::string sirType;
  };

  class RewriteRule {
  public:
    virtual ~RewriteRule() = default;
    virtual const char *name() const = 0;
    virtual std::vector<RewriteSite> findSites(const FunDecl &caller) = 0;
    // Decide whether the site can be rewritten by calling into
    // `callee` using `fixedRealizationIdx` as the bundled realization
    // (the engine has already locked which realization runs at the
    // call site; rules cannot pick a different one). Returns true when
    // the rule can produce an `apply()` for this combination.
    virtual bool matchCallee(
        const RewriteSite &site, const FuncDescriptor &callee, std::size_t fixedRealizationIdx
    ) = 0;
    // Splice the call in. Returns true on success; false if some
    // late-stage check fails (e.g. a param type the rule can't handle).
    virtual bool apply(
        FunDecl &caller, const RewriteSite &site, const FuncDescriptor &callee,
        std::size_t realizationIdx
    ) = 0;
  };

  // v1 rule: literal `let` initializers (scalar Int/Float).
  std::unique_ptr<RewriteRule> makeLiteralToCallRule();

  struct RewriteResult {
    int sitesFound = 0;
    int sitesRewritten = 0;
  };

  class RewriteEngine {
  public:
    void addRule(std::unique_ptr<RewriteRule> r) { rules_.push_back(std::move(r)); }

    // Try to rewrite at least one site in `caller` that calls into
    // `callee`. The engine picks rules + sites randomly under `rng` and
    // stops after the first successful splice OR after exhausting the
    // configured per-edge attempts cap. Returns counters for telemetry.
    //
    // `fixedRealizationIdx` pins which realization of the callee the
    // rule must use — it has to be the one whose .sir was actually
    // merged into the bundle, otherwise the call would return a
    // different solved value than the rewrite expression assumes and
    // --validate would fail.
    //
    // Composition safety: each rule is individually UB-free (the call
    // expression evaluates to the original literal under BV arithmetic),
    // but *stacking* rewrites on the same site is not — consecutive
    // sub-expressions evaluate left-to-right in SymIR, so e.g. composing
    // `c → f1() + (c - r1)` with a later rewrite of the literal `(c - r1)`
    // into `f2() + ((c - r1) - r2)` produces `f1() + f2() + …` and that
    // left-prefix sum can wrap in unintended ways. The engine therefore
    // marks each (caller, site) it successfully rewrites as consumed and
    // skips it on subsequent edges; one splice per site for the lifetime
    // of the engine.
    RewriteResult rewriteEdge(
        FunDecl &caller, const FuncDescriptor &callee, std::size_t fixedRealizationIdx,
        std::mt19937 &rng
    );

  private:
    std::vector<std::unique_ptr<RewriteRule>> rules_;
    // Identity = (caller FunDecl pointer, site letIdx). Caller pointers
    // are stable across one rylink program (the bundle's funs vector is
    // reserved upfront — see rylink.cpp generateOne).
    std::unordered_set<std::uint64_t> consumed_;

    static std::uint64_t consumedKey(const FunDecl *caller, int letIdx) {
      return (reinterpret_cast<std::uint64_t>(caller) << 16) ^
             static_cast<std::uint64_t>(static_cast<std::uint32_t>(letIdx));
    }
  };

} // namespace symir::reify
