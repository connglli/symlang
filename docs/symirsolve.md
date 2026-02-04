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
symirsolve <input.sir> [--path <labels> | --sample <n>] [options]
```

### Common Examples

Concretize using constraints embedded in the program (`assume`, `require`) and a specified path (defaults to `@main`):

```bash
symirsolve template.sir --path '^entry,^b1,^b3,^b1,^b2,^exit' -o concrete.sir
```

Concretize by randomly sampling up to 100 paths until a SAT one is found:

```bash
symirsolve template.sir --sample 100 --require-terminal -o concrete.sir
```

Use multi-threading to speed up sampling (2 threads):

```bash
symirsolve template.sir --sample 100 --require-terminal -j 2 -o concrete.sir
```

Use all available CPU cores for sampling:

```bash
symirsolve template.sir --sample 100 --require-terminal -j 0 -o concrete.sir
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
* If `--sample` is used, the path acts as a mandatory **prefix** for all sampled traces.


## Random Path Sampling

Instead of providing a complete path, `symirsolve` can explore the CFG automatically:

- **`--sample N`**: Performs up to $N$ random walks starting from the function entry (or the `--path` prefix).
- **Early Exit**: The process stops as soon as the first logically feasible (`SAT`) path is found.
- **`--require-terminal`**: If a random walk reaches `--max-path-len` without hitting a `ret`, `symirsolve` will attempt to complete the trace using the shortest path to any `ret` block. If disabled, non-terminating samples are discarded.


## Multi-Threading Support

`symirsolve` supports parallel sampling to improve performance when exploring large search spaces:

- **`-j N` or `--num-threads N`**: Use `N` threads for parallel path sampling
- **`-j 0`**: Automatically use all available CPU cores (determined by `std::thread::hardware_concurrency()`)
- **Default**: Single-threaded execution (`-j 1`)

Multi-threading is most effective with:
- **Large sample counts** (`--sample` with high values)
- **Complex search spaces** (multiple symbolic variables, many paths)
- **Non-deterministic solving** (where different seeds/paths may have different solve times)

**Notes:**
- Each thread uses an independent solver instance with a different random seed (based on the base `--seed` + thread ID)
- The first thread to find a SAT result causes all threads to terminate early
- Thread-safety is ensured through proper synchronization of shared state
- Both Bitwuzla and AliveSMT (Z3) backends support multi-threading


## Outputs

* **SAT**: If a solution exists, `symirsolve` reports `SAT`.
* **Concrete SIR**: If `-o <file>` is specified, it produces a concrete `.sir` where all symbols are replaced with concrete constants.
* **Model File**: If `--emit-model <file>` is specified, it produces a JSON file mapping the entry function to its solved symbol values.
* **AST Dump**: If `--dump-ast` is specified, it prints the internal AST representation of the concretized program to stdout.


## Options

| Option                | Description                                              |
| --------------------- | -------------------------------------------------------- |
| `--main <func>`       | Function to concretize (default: `@main`)                |
| `--path <labels>`     | Comma-separated block labels (acts as prefix if sampling)|
| `--sample <n>`        | Number of paths to sample randomly until SAT is found    |
| `--max-path-len <n>`  | Maximum random path length (default: 100)                |
| `--require-terminal`  | Force paths to reach 'ret' via shortest path if needed   |
| `-j, --num-threads <n>` | Number of threads for parallel solving (0 = use all available CPU cores, default: 1) |
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
