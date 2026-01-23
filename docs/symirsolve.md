# symirsolve — SymIR Concretizer (SMT-based)

`symirsolve` concretizes a **symbolic** SymIR (`.sir`) program into a **concrete** SymIR program by solving
constraints with an SMT solver over **bit-vectors (BV)**.

Concretization means:
- assign concrete values to all declared symbols (`@?x`, `%?y`),
- optionally specialize to a particular execution path,
- and emit a `.sir` program with symbols replaced (or with an explicit symbol-value section).

`symirsolve` is the bridge between synthesis/verification constraints and runnable code.


## Goals

- Concretize symbolic templates into concrete SymIR programs
- Support path-based constraint extraction (symbolic execution along a user-specified path)
- Provide deterministic, reproducible models (optional: seed / model selection policy)
- Produce outputs that can be:
  - interpreted by `symiri`, or
  - translated by `symirsolvec`


## Usage

```bash
symirsolve <input.sir> [--main <func>] --path <labels> [options]
```

### Common Examples

Concretize using constraints embedded in the program (`assume`, `require`) and a specified path (defaults to `@main`):

```bash
symirsolve template.sir --path '^entry,^b1,^b3,^b1,^b2,^exit' -o concrete.sir
```

Concretize and also emit a model file:

```bash
symirsolve template.sir --path '^entry,^b1,^b3,^b1,^b2,^exit' --emit-model model.json -o concrete.sir
```

Concretize with additional symbol constraints provided on the command line:

```bash
symirsolve template.sir --path '^entry,^b1,^b3,^b1,^b2,^exit' --sym %?c4=3 -o concrete.sir
```


## Inputs and Constraint Sources

`symirsolve` derives constraints from:

1. **Symbol declarations** (`sym`) and their domains (`in [lo,hi]` or `in {…}`)
2. **Path constraints** induced by the chosen path:

   * `br cond, ^t, ^f;` contributes `cond` if the path takes `^t`, else `not(cond)`
3. **`assume` constraints** (feasibility constraints)
4. **`require` constraints** (property constraints that must hold)

Strict UB is enforced:

* Any UB encountered on the chosen path (e.g., division by zero or OOB) makes that path infeasible.


## Path Specification

A path is specified as a sequence of block labels, including repeats:

```text
^entry,^b1,^b3,^b1,^b2,^exit
```

Rules:

* The sequence must be compatible with `br` edges in the CFG.
* `symirsolve` uses the path to select which side of conditional branches is taken.


## Outputs

* **SAT**: If a solution exists, `symirsolve` reports `SAT`.
* **Concrete SIR**: If `-o <file>` is specified, it produces a concrete `.sir` where all symbols are replaced with concrete constants.
* **Model File**: If `--emit-model <file>` is specified, it produces a JSON file mapping the entry function to its solved symbol values.
* **AST Dump**: If `--dump-ast` is specified, it prints the internal AST representation of the concretized program to stdout.


## Options

| Option                | Description                                              |
| --------------------- | -------------------------------------------------------- |
| `--main <func>`       | Function to concretize (default: `@main`)                |
| `--path <labels>`     | Comma-separated block label sequence                     |
| `-o <file>`           | Output concrete `.sir` file                              |
| `--dump-ast`          | Dump concretized AST to stdout                           |
| `--timeout-ms <n>`    | Solver timeout in milliseconds                           |
| `--seed <n>`          | Seed for deterministic model selection                   |
| `--emit-model <file>` | Emit symbol assignments in nested JSON format            |
| `--sym sym=val`       | Fix a symbol to a concrete value before solving          |
| `-h, --help`          | Print usage                                              |


## Notes on BV Semantics

`symirsolve` encodes integer types as BV sorts:

* `i32` → `(_ BitVec 32)`
* `i64` → `(_ BitVec 64)`
* `iN`  → `(_ BitVec N)` (backend support may vary)

Operators:

* `/` uses signed truncating division: `bvsdiv`
* `%` uses signed remainder: `bvsrem`

These match the language semantics in `SPEC.md`.


## Failure Modes

| Result            | Meaning                                                |
| ----------------- | ------------------------------------------------------ |
| SAT               | Concretization succeeded; output program is emitted    |
| UNSAT             | No assignments satisfy constraints for the chosen path |
| UNKNOWN / TIMEOUT | Solver could not determine satisfiability              |

`symirsolve` should always report the solver status with a clear diagnostic.


## Relationship to Other Tools

* `symirsolve` produces concrete `.sir` that `symiri` can execute
* `symirc` can translate the concretized `.sir` into C/WASM
* `symirc` can also translate symbolic `.sir` directly (using extern/import symbol hooks), but `symirsolve` is the tool that actually solves and fixes symbol values
