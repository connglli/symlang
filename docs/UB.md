# SymIR Strict Undefined Behavior (UB)

This document is a per-rule companion to the formal spec (§7 of `SPEC_v0.2.1.md`). Each section names the rule, the spec reference, and how each of `symiri` (interpreter), `symirc` (compiler), and `symirsolve` (solver) enforces it.

SymIR uses **strict UB**: if any operation on the executed path triggers UB, the entire path is **infeasible**.

- `symiri` aborts on UB (immediate termination, non-zero exit).
- `symirc`-emitted C runs under UBSan with `-fno-sanitize-recover=all`, so UB traps the executable.
- `symirsolve` adds the UB-precluding constraint to `PC`; satisfiable models avoid the UB.

The rule numbers below match the formal spec's UB rule numbers; rules unique to v0.2.1 are marked **[v0.2.1]**.

---

## Scalar arithmetic (§7.1, §7.4)

### Rule 1 — Integer division/modulo by zero
`a / b` or `a % b` with `b == 0` is UB.

- **symiri:** checks divisor before the op.
- **symirc:** UBSan `-fsanitize=integer-divide-by-zero`.
- **symirsolve:** `(distinct b 0)` per division site.

### Rule 2 — Out-of-bounds array access
`a[i]` where `a : [N] T` and `i < 0 ∨ i >= N` is UB.

- **symiri:** bounds-checks every lvalue index.
- **symirc:** UBSan `-fsanitize=array-bounds` (for fixed-size arrays).
- **symirsolve:** `(bvult i N)` (unsigned-less-than handles negative as large positive).

### Rule 3 — Reading `undef`
Reading any leaf whose stored value is `undef` is UB. This subsumes uses of uninitialised locals, uninitialised pointer values, and uninitialised vector lanes.

- **symiri:** per-leaf `undef` tracking; throws on read.
- **symirc:** emitted code initialises every scalar leaf at declaration; `undef` is lowered to a literal `0` plus a path-pruning `assume(false)` token, so the compiled program either zero-reads or traps under UBSan.
- **symirsolve:** every symbolic value carries an `is_defined` flag; reads conjoin it to `PC`.

Also enforced statically by `DefiniteInitAnalysis` (warning-level, conservative).

### Rule 4 — Signed integer overflow
`+`, `-`, `*`, `<<` whose result is outside the signed range of the target width is UB. Also `INT_MIN / -1`. SymIR treats `<<` as signed arithmetic (not BV wrap), so `x << n` is UB if `x * 2^n` doesn't fit OR if `x < 0`.

- **symiri:** checks via per-op signed-overflow predicate.
- **symirc:** UBSan `-fsanitize=signed-integer-overflow,shift`.
- **symirsolve:** `bvsaddo/bvssubo/bvsmulo` overflow predicates; for `<<`, an explicit reconstruct-and-compare.

### Rule 5 — Overshift
`x << n`, `x >> n`, `x >>> n` with `n < 0` or `n >= width(x)` is UB. This is about the *amount*; the shifted result is covered by rule 4.

- **symiri:** range-checks the shift amount.
- **symirc:** UBSan `-fsanitize=shift`.
- **symirsolve:** `(bvult n width)`.

### Rule 6 — FP overflow (±∞ result)
Any `+`, `-`, `*`, `/` whose RNE-rounded result would be ±∞ is UB. Covers finite-operand overflow and `x / ±0.0` for non-zero `x`.

- **symiri:** `std::isinf` after every FP op.
- **symirc:** UBSan `-fsanitize=float-divide-by-zero` plus an explicit `isinf` check on every FP arithmetic result (the C backend emits the check; UBSan does not catch overflow-to-inf by itself).
- **symirsolve:** `(not (fp.isInfinite result))` on each FP op.

### Rule 7 — FP invalid (NaN result)
Any FP op producing NaN is UB. Covers `±0.0 / ±0.0` and `x % ±0.0`.

- **symiri:** `std::isnan` after every FP op.
- **symirc:** emitted `isnan` check after each FP op.
- **symirsolve:** `(not (fp.isNaN result))`.

### Rule 8 — Float-to-integer out-of-range
`fN as iM` is UB if the truncated value (toward 0) is outside the representable range of `iM`.

- **symiri:** pre-cast bounds check.
- **symirc:** UBSan `-fsanitize=float-cast-overflow`.
- **symirsolve:** `(bvsle min_iM rounded)` and `(bvsle rounded max_iM)`.

---

## Pointer UB (§7.5)

### Rule 9 — Null pointer dereference
`load %p` or `store %p, v` with `%p == null` is UB.

- **symiri:** checks pointer ≠ `nullptr` before any deref.
- **symirc:** the emitted load/store traps under UBSan's null sanitiser.
- **symirsolve:** `(distinct %p (_ bv0 64))` on every deref.

### Rule 10 — Out-of-bounds pointer arithmetic [revised in v0.2.1]
Every pointer carries a **provenance object** (rule 15). Pointer arithmetic `%p ± n` is UB if the resulting address falls outside `[base_provenance, base_provenance + size_provenance]`. The one-past-the-end address is valid for arithmetic and equality, UB to `load`/`store` (rule 11).

- **symiri:** every `ptr` runtime value tags its `ObjectInfo` (base address + size in bytes). Arithmetic updates the offset; if `new_off ∉ [0, size]` the runtime aborts.
- **symirc:** in v0.2.1, `addr` of an aggregate / `ptrindex` / `ptrfield` lowers to GCC `__builtin_object_size`-instrumented arithmetic; UBSan `-fsanitize=pointer-overflow` catches the wrap; an explicit emitted range check covers the in-bounds path.
- **symirsolve:** path condition gets `(bvule new_off size)` (one-past-end allowed). Each `addr` operand records its provenance's base and size for downstream propagation.

### Rule 11 — Out-of-bounds load/store
`load %p` / `store %p, v` is UB if `%p` lies outside `[base, base + size)` (one-past-the-end is included as "outside" for the purpose of deref).

- **symiri:** combines rule 10's `ObjectInfo` with a strict `< size` check at deref.
- **symirc:** UBSan + the emitted bound check.
- **symirsolve:** `(bvult offset size)` on every `load`/`store`.

### Rule 12 — Cross-object pointer arithmetic
Forming a pointer by arithmetic that crosses from one local's storage into another's is UB.

- **symiri:** rule 10's `ObjectInfo` won't cross objects — the offset would exit the known allocation, triggering UB before any cross-object value is produced.
- **symirc:** UBSan catches the boundary cross via `-fsanitize=pointer-overflow`.
- **symirsolve:** non-overlap axioms (§9.4.2) make cross-object arithmetic infeasible by construction.

### Rule 13 — Uninitialised pointer dereference
`load %p` or `store %p, v` where `%p == undef` is UB (a consequence of rule 3 specialised to pointer values, kept as a separate rule for clarity).

- **symiri:** per-leaf `undef` flag, checked before deref.
- **symirc:** emitted code initialises pointer leaves to `null`, so the deref then triggers rule 9.
- **symirsolve:** `is_defined(%p)` conjoined to `PC` at deref.

### Rule 14 — Cross-object pointer comparison
`<`, `<=`, `>`, `>=` between pointers of different originating objects is UB. `==`/`!=` are always defined (distinct objects → distinct addresses → `false`/`true`).

- **symiri:** relational compare on pointers checks both operands share the same `ObjectInfo`.
- **symirc:** UBSan-free territory — the emitted code adds an explicit object-id check before the compare; cross-object compare aborts.
- **symirsolve:** the path condition for a relational compare requires both operands to share a base.

### Rule 15 — Aggregate-derived pointer provenance [revised in v0.2.1]
Every pointer derivation carries a **provenance object**. The rule is uniform between arrays and structs and depends on the *final access*:

- `addr %lv` (top-level local): provenance = `%lv` (the whole local).
- `addr lv.f` / `ptrfield <ptr>, f`: provenance = the **immediate containing struct** of `f`.
- `addr lv[i]` / `ptrindex <ptr>, i`: provenance = the **immediate containing array**.
- `<ptr> ± n`: provenance unchanged from `<ptr>`.

Pointer arithmetic that walks outside the provenance object is UB (rule 10). Type discipline at deref is preserved by rule 15b.

This **relaxes the v0.2.0 rule** ("one-element provenance per scalar struct field"). In v0.2.1, arithmetic within a struct or array is allowed; UB is moved to the deref site where the typed-access check kicks in.

- **symiri:** the `ObjectInfo` for a derived pointer is the *immediate containing aggregate*, not the field/element alone. Arithmetic checks against that aggregate's bounds.
- **symirc:** the emitted code uses the aggregate's `sizeof` for the range check; UBSan `-fsanitize=pointer-overflow` does the boundary trap.
- **symirsolve:** each `addr`/`ptrindex`/`ptrfield` records the provenance (a base address constant + a static byte size); rule 10's path condition uses that pair.

### Rule 15b — Typed-access mismatch [new in v0.2.1]
`load %p` / `store %p, v` through `%p : ptr T` is UB if the runtime address does **not** coincide with the start of a `T`-typed cell within the provenance object. Concretely:

- For a `[N] U` originating object: every address `base + k*sizeof(U)` for `0 ≤ k < N` is a valid `U` cell. (Because the pointer's type necessarily matches the array element type, in-bounds + element-aligned ⇒ valid.)
- For an `@S` originating object: only the offsets of fields with **declared type `T`** are valid cells. A `ptr i32` landing on the offset of an `i64` field — even though arithmetic stayed in `@S`'s bounds — is UB.
- Mid-cell, on a cell of a different type, or straddling cells: all UB.

This is what makes the revised rule 15 type-safe: arithmetic is permissive, but the deref must respect the field types.

- **symiri:** every `load`/`store` walks the provenance object's layout, computes the cell start nearest to the runtime offset, and aborts if the cell's declared type doesn't match the pointer's static type (or the offset isn't aligned to a cell start).
- **symirc:** the emitted load/store is preceded by a layout check generated from the struct's field table (a constant-folded disjunction over valid offsets). UBSan catches the trap path.
- **symirsolve:** at each `load`/`store`, the path condition becomes a disjunction over the valid `T`-typed cell offsets of the provenance object. Symbolic offsets are constrained to one of those values; off-set offsets force `PC := false`.

### Rule 16 — `ptrindex` out-of-bounds [v0.2.1]
`ptrindex <ptr>, <i>` with `<ptr> : ptr [N] T` is UB if `i < 0` or `i > N`. `i == N` produces a valid non-dereferenceable address (good for arithmetic and equality, UB to deref).

- **symiri:** index range check.
- **symirc:** UBSan + emitted range check.
- **symirsolve:** `(bvsle 0 i)` ∧ `(bvsle i N)`.

### Rule 17 — Navigation through `null` [v0.2.1]
`ptrindex <ptr>, <i>` or `ptrfield <ptr>, <f>` is UB if `<ptr>` evaluates to `null`. Caught at navigation, not at later deref, so the path prunes immediately.

- **symiri:** pointer null check at the navigation site.
- **symirc:** emitted pre-navigation null guard; UBSan-trapped on failure.
- **symirsolve:** `(distinct <ptr> (_ bv0 64))` on every navigation.

### Rule 18 — Navigation through `undef` [v0.2.1]
`ptrindex`/`ptrfield` count as reads of their pointer operand; an `undef` operand is UB at the navigation site (consequence of rule 3).

- **symiri:** `is_defined` check on the pointer operand before computing the offset.
- **symirc:** emitted code rejects `undef` pointer values upstream (initialised to `null`, then null-guarded by rule 17).
- **symirsolve:** `is_defined(<ptr>)` conjoined to `PC` at navigation.

### Rule 19 — Navigation from a one-past-the-end pointer [v0.2.1]
`ptrindex <ptr>, <i>` or `ptrfield <ptr>, <f>` is UB if `<ptr>` is exactly the one-past-the-end address of its provenance object — that address is valid for arithmetic and equality, but doesn't point to an element to navigate into.

- **symiri:** runtime check `offset != size_provenance` before each navigation.
- **symirc:** emitted guard alongside the rule-17 null check.
- **symirsolve:** `(distinct offset size_provenance)` conjoined to `PC` at every navigation.

---

## Vector UB (§7.6) [v0.2.1]

### Rule 20 — Out-of-bounds vector lane access
`lv[i]` (read or write) where `lv : <N> T` is UB if `i < 0 ∨ i >= N`.

- **symiri:** lane-index bounds check.
- **symirc:** the emitted code lowers `v[i]` to a subscript on the GCC vector-extension type with an explicit pre-check; UBSan traps on failure.
- **symirsolve:** `(bvult i N)` at each lane access; for symbolic `i` the `ite` chain encoding (§9.5.4) only defines lanes in range, so out-of-range indices force `PC := false`.

### Rule 21 — Lane-wise scalar UB
All scalar UB rules (1–8) apply **per-lane** to vector operations. UB in any single lane prunes the whole path. Examples:

- `%a / %b` where `%a, %b : <4> i32` — UB if any lane of `%b` is 0.
- `%a + %b` where `%a, %b : <4> i32` — UB if any lane overflows.
- `%v as <4> f32` where `%v : <4> f64` — UB if any lane would overflow to ±∞.

- **symiri:** lane-iterates each vector op and checks each scalar rule per lane.
- **symirc:** the emitted SIMD operation is preceded by per-lane checks (extracted via `__builtin_shufflevector` or equivalent), each gated by UBSan.
- **symirsolve:** each lane has its own scalar UB conjunct in `PC`; the entire path is feasible only if every lane's check passes.

### Rule 22 — Reading an `undef` vector lane
Reading a lane whose value is `undef` is UB — either from a vector initialised with `undef`, or from a vector where the read lane has not yet been written by a lane-write or whole-vector copy.

- **symiri:** per-lane `undef` flag tracked alongside lane values.
- **symirc:** emitted vector locals are initialised lane-by-lane to defined values (`0`) where the source uses `undef`; subsequent reads hit defined storage. UBSan handles edge cases.
- **symirsolve:** every lane carries an `is_defined` flag; lane reads conjoin it to `PC`.

---

## What's *not* UB in SymIR

For completeness, a few choices SymIR deliberately makes well-defined where other languages don't:

- **Equality across objects.** `==`/`!=` between pointers of different originating objects is always well-defined (and always `false`/`true`). Only relational compare is UB (rule 14).
- **One-past-the-end address.** Valid for arithmetic and equality. UB only when dereferenced (rule 11) or navigated through (rule 19).
- **Whole-vector copy.** `%v = %w` for `%v, %w : <N> T` is always well-defined (lane-by-lane copy; no overflow or aliasing concerns).
- **`fmod` semantics for FP `%`.** Aligned with integer `%` (truncate toward zero), not IEEE `fp.rem` (round to nearest even). No UB cases beyond rule 7 (divisor zero → NaN result).
- **Signed `<<` of `x >= 0` whose result fits.** Well-defined arithmetic shift; only `x < 0` or overflow is UB (rule 4).

---

## Cross-reference summary

| # | Name | Section in spec |
|---|------|----------------|
| 1 | Integer div/mod by zero | §7.1 |
| 2 | OOB array access | §7.1 |
| 3 | Reading `undef` | §7.1 |
| 4 | Signed overflow | §7.1 |
| 5 | Overshift | §7.1 |
| 6 | FP overflow (±∞) | §7.4 |
| 7 | FP invalid (NaN) | §7.4 |
| 8 | Float→int out-of-range | §7.4 |
| 9 | Null pointer deref | §7.5 |
| 10 | OOB pointer arithmetic | §7.5 |
| 11 | OOB load/store | §7.5 |
| 12 | Cross-object pointer arith | §7.5 |
| 13 | Uninitialised pointer deref | §7.5 |
| 14 | Cross-object pointer compare | §7.5 |
| 15 | Aggregate-derived pointer provenance **[revised v0.2.1]** | §7.5 |
| 15b | Typed-access mismatch **[v0.2.1]** | §7.5 |
| 16 | `ptrindex` OOB **[v0.2.1]** | §7.5 |
| 17 | Navigation through `null` **[v0.2.1]** | §7.5 |
| 18 | Navigation through `undef` **[v0.2.1]** | §7.5 |
| 19 | Navigation from one-past-end **[v0.2.1]** | §7.5 |
| 20 | OOB vector lane access **[v0.2.1]** | §7.6 |
| 21 | Lane-wise scalar UB **[v0.2.1]** | §7.6 |
| 22 | Reading `undef` vector lane **[v0.2.1]** | §7.6 |
