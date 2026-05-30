#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>

// Central place to manage reify's *code-level* tunable hyperparameters.
//
// Scope:
//   - Probabilities / slot frequencies for atom-kind, type-kind, and var-phase
//     choices.
//   - Pools of "interest" literal values that the random generator draws from.
//   - Per-width concrete-literal coefficient ranges.
//
// Not in scope (these live elsewhere as struct defaults or CLI-tunable flags):
//   - n-funcs, n-vars, n-stmts, n-bbls, n-inits, max-retries, max-loop-iter
//   - p-branch, p-backedge, timeouts, RNG seed
//   - coef-domain / value-domain / index-domain (symbolic-coef domains)
//   - max-ptr-depth, max-agg-nest, max-agg-elems
//
// Editing a value here changes behaviour for all rysmith runs without
// touching any CLI default.
namespace symir::reify::rysmith::hp {

  // ===========================================================================
  // Atom-kind slot tables for genIntAtomOnPath / OffPath and genFloatAtomOnPath.
  //
  // Each generator draws s in [0, 99] and walks a series of "if (s < THRESH)"
  // checks. The slot reserved for a kind is [previous_thresh, this_thresh),
  // i.e. its share is (this_thresh - prev_thresh) percent. Order matters: the
  // first matching slot wins, with the final fallthrough used when no slot
  // matches (e.g. when a required precondition like "has an rvalue" fails).
  // ===========================================================================

  // --- genIntAtomOnPath: uses sym for coefs ---
  inline constexpr int kIntOnPath_CoefBareEnd = 40; // standalone sym Coef
  inline constexpr int kIntOnPath_MulEnd = 60;      // sym * rval
  inline constexpr int kIntOnPath_BitwiseEnd = 70;  // sym & | ^ rval
  inline constexpr int kIntOnPath_ShiftEnd = 75;    // sym <</>>/>>> rval
  inline constexpr int kIntOnPath_UnaryNotEnd = 80; // ~ rval
  inline constexpr int kIntOnPath_CastEnd = 85;     // var as T
  inline constexpr int kIntOnPath_DivModEnd = 90;   // sym / % rval (+ guard)
  inline constexpr int kIntOnPath_LoadEnd = 95;     // load %p
  inline constexpr int kIntOnPath_SelectEnd = 99;   // select cond, a, b
  // fallthrough → standalone sym Coef

  // --- genIntAtomOffPath: concrete coefs only, no syms ---
  inline constexpr int kIntOffPath_ConcreteEnd = 30;  // bare int literal
  inline constexpr int kIntOffPath_MulEnd = 50;       // lit * var
  inline constexpr int kIntOffPath_BitwiseEnd = 60;   // lit & | ^ var
  inline constexpr int kIntOffPath_CastEnd = 65;      // var as T
  inline constexpr int kIntOffPath_DivModEnd = 70;    // lit / % var
  inline constexpr int kIntOffPath_PlainRvalEnd = 75; // bare lvalue
  inline constexpr int kIntOffPath_LoadEnd = 85;      // load %p
  inline constexpr int kIntOffPath_SelectEnd = 95;    // select cond, a, b
  // fallthrough → concrete int literal

  // --- genFloatAtomOnPath ---
  inline constexpr int kFloatOnPath_CastFromI32SymEnd = 50; // sym as fT
  inline constexpr int kFloatOnPath_MulLitEnd = 70;         // float lit * var
  inline constexpr int kFloatOnPath_CastFromVarEnd = 85;    // i32 var as fT
  inline constexpr int kFloatOnPath_SelectEnd = 95;         // select cond, a, b
  // fallthrough → concrete float literal

  // ===========================================================================
  // Expression structure
  // ===========================================================================
  inline constexpr int kMinAtomsPerExpr = 1;
  inline constexpr int kMaxAtomsPerExpr = 3;

  // ===========================================================================
  // Type-kind probabilities in genRandomType (sum normalised at use-site).
  // depth-0 distribution; pArray/pStruct are zeroed past maxAggNesting and
  // pPtr is zeroed past maxPtrDepth.
  // ===========================================================================
  inline constexpr double kPTypeScalar = 0.50;
  inline constexpr double kPTypeArray = 0.20;
  inline constexpr double kPTypeStruct = 0.15;
  inline constexpr double kPTypePtr = 0.15;

  // [v0.2.1] Vector type probability at depth 0. Zeroed if enableVec=false.
  inline constexpr double kPTypeVec = 0.15;

  // [v0.2.1] When generating a pointer, probability the pointee is an
  // aggregate (array or struct) rather than a scalar. Zeroed if
  // enableAggPtr=false.
  inline constexpr double kPPtrAgg = 0.30;

  // ===========================================================================
  // Var catalogue split: fraction of cfg.nVars allocated to each phase.
  // The remainder after non-ptr + ptr1 goes to ptr-of-ptr vars.
  // ===========================================================================
  inline constexpr double kFracNonPtrVars = 0.65;
  inline constexpr double kFracPtr1Vars = 0.20;
  // remainder → ptr-of-ptr vars (~15%)

  // [v0.2.1] Fraction of nVars allocated to vec-typed locals.
  inline constexpr double kFracVecVars = 0.10;
  // [v0.2.1] Fraction of nVars allocated to aggregate-pointer locals.
  inline constexpr double kFracAggPtrVars = 0.10;

  // ===========================================================================
  // [v0.2.1] Vec atom slot thresholds for genExpr vec-target dispatch.
  // Slots: [0, kVecCopyEnd) = whole-vec copy
  //        [kVecCopyEnd, kVecSymMulEnd) = sym * vec (on-path only)
  //        [kVecSymMulEnd, kVecConcMulEnd) = concrete coef * vec
  //        [kVecConcMulEnd, 100) = fallback copy
  // ===========================================================================
  inline constexpr int kVecCopyEnd = 50;
  inline constexpr int kVecSymMulEnd = 70;
  inline constexpr int kVecConcMulEnd = 85;

  // [v0.2.1] Vec lane-write probability in genBlockStmts when LHS is vec.
  // When canWholeVec is true: probability of lane write vs whole-vec assign.
  inline constexpr int kVecLaneWriteProb = 40; // 40% lane write, 60% whole-vec

  // Probability that a struct field is an array (struct-of-arrays pattern).
  inline constexpr double kPStructFieldIsArray = 0.30;

  // ===========================================================================
  // Concrete int-literal coefficient ranges per bitwidth in genConcreteIntAtom.
  // Each entry is the inclusive [lo, hi] range used for that exact width and
  // covers the type's full representable signed range, so every width gets
  // equal access to boundary values (sign bit, INT_MIN-style wrap, etc.).
  //
  // These bound only the *literal* pool; symbolic coefs are bounded separately
  // by the --coef-domain / --value-domain / --index-domain CLI flags.
  // ===========================================================================
  inline constexpr std::int64_t kConcreteInt_I8_Lo = -128;
  inline constexpr std::int64_t kConcreteInt_I8_Hi = 127;
  inline constexpr std::int64_t kConcreteInt_I16_Lo = -32768;
  inline constexpr std::int64_t kConcreteInt_I16_Hi = 32767;
  inline constexpr std::int64_t kConcreteInt_I32_Lo = -2147483648;
  inline constexpr std::int64_t kConcreteInt_I32_Hi = 2147483647;
  inline constexpr std::int64_t kConcreteInt_I64_Lo = INT64_MIN;
  inline constexpr std::int64_t kConcreteInt_I64_Hi = INT64_MAX;
  // Used when targetType has no recognised width (shouldn't normally happen).
  inline constexpr std::int64_t kConcreteInt_Default_Lo = -4;
  inline constexpr std::int64_t kConcreteInt_Default_Hi = 8;

  // Concrete int range used as the multiplicative coef in off-path OpAtoms.
  // (Off-path code is constant-folded by the typechecker / runtime, so we
  // keep the coef small and human-friendly here rather than full-width.)
  inline constexpr std::int64_t kOffPathCoef_Lo = -8;
  inline constexpr std::int64_t kOffPathCoef_Hi = 8;
  inline constexpr std::int64_t kOffPathCoefI8_Lo = -4;
  inline constexpr std::int64_t kOffPathCoefI8_Hi = 4;
  // Off-path divisor must be nonzero — kept positive so the off-path branch
  // can never trigger div-by-zero UB without adding a runtime guard.
  inline constexpr std::int64_t kOffPathDivisor_Lo = 1;
  inline constexpr std::int64_t kOffPathDivisor_Hi = 8;

  // ===========================================================================
  // Float literal pools.
  //
  // SAFETY-CRITICAL — DO NOT REPLACE WITH uniform_real_distribution.
  //
  // reify's output is differentially tested against compiled C/WASM. If a
  // generated FP expression isn't bitwise-deterministic across the SymIR
  // interpreter, GCC (with/without FMA contraction), and WASM, we'd flag
  // legitimate rounding non-determinism as a compiler bug.
  //
  // Every entry below is a *dyadic rational* (p / 2^q) or a small odd int.
  // Sums, differences, products, and divisions of such values are themselves
  // dyadic and stay exactly representable in IEEE-754 binary64 until the
  // mantissa overflows — which doesn't happen for the 1–3 atom expressions
  // reify produces. Adding any non-dyadic value (e.g. 0.1, 0.3) here would
  // break differential testing.
  // ===========================================================================
  // Bare-literal float Coef pool (genConcreteFloatAtom).
  inline constexpr double kFloatLitPool[] = {0.0, 1.0,  -1.0, 2.0,   -2.0,  4.0,    -4.0, 8.0, -8.0,
                                             0.5, -0.5, 0.25, -0.25, 0.125, -0.125, 3.0,  -3.0};
  inline constexpr std::size_t kFloatLitPoolSize = sizeof(kFloatLitPool) / sizeof(kFloatLitPool[0]);

  // Multiplicative float coefs used in float * var atoms (genFloatAtomOnPath
  // Mul slot). 0.0 is intentionally omitted to avoid trivial zero terms.
  // Small odd ints (3, 5) are dyadic-exact themselves; products like 3*5=15
  // stay exact until precision overflow.
  inline constexpr double kFloatMulCoefPool[] = {2.0,  -2.0, 4.0,   -4.0, 8.0,  -8.0, 0.5,
                                                 -0.5, 0.25, -0.25, 3.0,  -3.0, 5.0,  -5.0};
  inline constexpr std::size_t kFloatMulCoefPoolSize =
      sizeof(kFloatMulCoefPool) / sizeof(kFloatMulCoefPool[0]);

} // namespace symir::reify::rysmith::hp

// Central place to manage rylink's *code-level* tunable hyperparameters.
//
// Scope:
//   - Call-graph shape probabilities and out-degree caps.
//   - Rewrite engine acceptance probabilities and per-edge attempt caps.
//   - Fixed naming conventions for output artifacts (prog dir prefix,
//     entry .sir filename).
//
// Not in scope (these live elsewhere as struct defaults or CLI-tunable flags):
//   - n-progs, n-nodes, max-outdeg, RNG seed, generation id
//   - --target / --validate / --keep-require / I-O paths
//
// Editing a value here changes behaviour for all rylink runs without
// touching any CLI default.
namespace symir::reify::rylink::hp {

  // ===========================================================================
  // Call-graph shape
  //
  // rylink builds a DAG over N nodes drawn from the pool. N comes from the
  // CLI (--n-nodes) and is shrunk to fit when the pool is smaller. The DAG
  // uses a fixed topological order: for every ordered pair (i, j) with
  // i < j we add edge i → j with probability kPEdge, capped so no node ever
  // exceeds kMaxOutDegree out-edges. The ordering guarantees acyclicity
  // (no recursion); the cap keeps individual callers from sprouting
  // unrealistically wide fan-outs.
  // ===========================================================================
  inline constexpr double kPEdge = 0.45;  // per-ordered-pair edge probability
  inline constexpr int kMaxOutDegree = 3; // hard out-degree cap per node

  // ===========================================================================
  // Rewrite engine
  //
  // Per caller→callee edge, the engine enumerates rewrite *sites* in the
  // caller AST (literal initializers in v1; unchanged-var uses next) and
  // tries up to kMaxAttemptsPerEdge (site × callee-realization) pairs before
  // giving up. kPRewrite gates each individual attempt so most literals
  // stay literal — without this throttle a caller saturated with matching
  // literals would degenerate into a long chain of calls and lose all
  // resemblance to the original synthesised program.
  // ===========================================================================
  inline constexpr int kMaxAttemptsPerEdge = 32; // hard work budget per edge
  inline constexpr double kPRewrite = 0.50;      // per-site accept probability

  // Per call-argument choice between the two argument modes
  // LiteralToCallRule supports:
  //   - "literal":  emit the solver's paramValue as a CoefAtom
  //   - "var+bias": pick an unchanged scalar caller-var of matching
  //                 type, compute bias = expected - var.let_init, emit
  //                 `%var + bias` (which evaluates to the same value
  //                 at runtime because the splice runs before any
  //                 user code that could mutate %var).
  // kPVarBiasArg is the probability we attempt the var+bias path for a
  // given call argument. The rule falls back to the literal path when
  // no suitable var exists for that param's type or the bias would
  // overflow the var's signed range.
  inline constexpr double kPVarBiasArg = 0.50;

  // ===========================================================================
  // Per-program retry budget
  //
  // generateOne is deterministic given its rng, but each fork can still
  // hit a non-rewrite-related dead end downstream (e.g. an `--validate`
  // run that trips runtime UB inside off-path code spliced into a callee
  // via a now-on-path branch). 3 attempts is a small budget that
  // tolerates the occasional rng-induced miss without masking systemic
  // bugs (which would fail every fork and still surface).
  // ===========================================================================
  inline constexpr int kMaxAttemptsPerProg = 3;

  // ===========================================================================
  // Output naming
  //
  // The per-program output directory is `<prog-prefix>_<id>_<i>` (e.g.
  // `prog_ab12cd_0`). The bundled entry-program .sir inside that directory
  // is always `kEntrySirName`. These are exposed as named constants so the
  // rylink driver and any external test/inspection tool agree on the
  // layout without re-deriving it from string literals.
  // ===========================================================================
  inline constexpr const char *kProgPrefix = "prog";
  inline constexpr const char *kEntrySirName = "program.sir";

} // namespace symir::reify::rylink::hp
