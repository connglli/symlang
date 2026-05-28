# SymIR Standard Library (test/lib/std)

Reference implementations of common operations expressible in v0.2.2 SymIR.
Each `.sir` file is a self-contained, runnable test (`EXPECT: PASS`) with
`require` clauses that act as the unit-test assertions for the library
function it demonstrates.

These programs double as regression tests: the per-tool runners in
`test/lib/run_*.py` discover and execute them.

## Layout

```
test/lib/std/
  scalar/        Integer and floating-point helpers (abs/clamp/sign/avg/…).
  simd/          Vector reductions, shuffles, broadcasts using <N> T types.
  type/          Bit-width queries and integer-limit constants.
  collections/   Fixed-size array helpers (sum / max / find / reverse).
```

## Notes on what's expressible in v0.2.2

- `iN` integer arithmetic with strict UB on signed overflow / div-by-zero.
- `f32`/`f64` floating-point in finite IEEE-754 domain (`±∞`/`NaN` are UB).
- `ptr T`, `ptr [N] T`, `ptr @S` with `addr`/`load`/`store`/`ptrindex`/`ptrfield`.
- `<N> T` SIMD vectors with lane access, whole-vector copy, `cmp`/`select`,
  lane-wise arithmetic.
- Standard intrinsics `@abs` `@min` `@max` `@clz` `@ctz` `@popcount` at any
  declared `iN` width.
- Function calls (`call @fun(...)`) and external `decl`s (link- and
  contract-form). Recursion is rejected statically; call graph is a DAG.

## Not in v0.2.2 (planned)

- Heap allocation; pointer/integer casts; pointer symbols; addressable
  vectors; aggregate `load`/`store`; chars/strings; multi-return; variadics.
