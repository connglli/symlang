# SymLang (SymIR): A CFG-Based Symbolic Language / Intermediate Representation

> [!WARNING]
> The project is under active development. APIs and IR details are still evolving, but semantic decisions in `SPEC.md` are authoritative for v0.

SymIR (also referred to as SymLang) is a **CFG-based symbolic intermediate representation** designed for program synthesis, symbolic execution, and constraint generation for SMT solvers using **bit-vector (BV) logic**.

It provides a robust foundation for building tools that need to reason about program semantics, explore execution paths, or synthesize code snippets that satisfy specific properties.

## Table of Contents

- [Core Concepts](#core-concepts)
- [Tools Overview](#tools-overview)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Building](#building)
  - [Running Tests](#running-tests)
  - [Switching SMT Backends](#switching-smt-backends)
- [Typical Workflows](#typical-workflows)
- [Project Structure](#project-structure)
- [Documentation](#documentation)


## Core Concepts

- **Path-Oriented:** Designed specifically for symbolic execution and path-based analysis.
- **Symbolic by Design:** Programs may be **symbolic** (contain solver-chosen unknowns marked with `?`) or fully concrete.
- **Explicit Control Flow:** CFG-based representation using basic blocks and `br` instructions (no structured nesting requirements).
- **SMT-Friendly:** Expressions are intentionally restricted to flat, left-to-right forms to ensure predictable BV constraints.
- **Strict Semantics:** Includes **strict undefined behavior (UB)** for operations like division-by-zero or out-of-bounds access, facilitating bug-finding.

## Tools Overview

| Tool | Purpose |
| :--- | :--- |
| `symiri` | **Interpreter**: Execute `.sir` programs directly with concrete values or symbol bindings. |
| `symirc` | **Compiler**: Translate `.sir` programs into optimized C or WebAssembly (WASM). |
| `symirsolve` | **Solver**: Concretize symbolic programs by solving path constraints via SMT. |

## Getting Started

### Prerequisites

- **C++20** compatible compiler (GCC 10+ or Clang 10+)
- **Bitwuzla** (Required by default)
  - Install: https://github.com/bitwuzla/bitwuzla
- **Z3** (Optional)
  - Install: https://github.com/Z3Prover/z3
- **Python 3** (for running the test suite)
- **WASM runtime** (Optional, for running WASM backend tests such as Wasmtime, Wasmer, or Node.js)

### Building

To build all tools (interpreter, compiler, and solver):

```bash
make -j$(nproc)
```

To clean the build artifacts:

```bash
make clean
```

To test the build:

```bash
make test
```

### Running Tests

The project includes an extensive test suite covering lexing, parsing, analysis, interpretation, compilation, and solving.

```bash
make test
```

### Switching SMT Backends

SymLang supports multiple SMT solvers via an abstract interface. The following backends are available:

- **Bitwuzla (Default):** Highly optimized for Bit-Vector (BV) and Floating-Point (FP) logic. It is the recommended solver for performance and reliability in symbolic execution tasks.
- **AliveSMT (Z3-based):** A Z3-based backend derived from the Alive2 project. It provides an alternative for verification tasks where Z3's specific heuristics or theories are preferred.

The solver backend is selected at compile-time using the `SOLVER` variable in the `Makefile`.

```bash
# Build with Bitwuzla (default)
make SOLVER=bitwuzla

# Build with AliveSMT (Z3)
make SOLVER=alivesmt
```

## Typical Workflows

### 1. Interpret a Concrete Program
```bash
./symiri examples/hello.sir
```

### 2. Interpret a Symbolic Program with Bindings
Provide concrete values for symbols at runtime:
```bash
./symiri examples/template.sir --sym %?a=10 --dump-trace
```

### 3. Compile to C or WebAssembly
```bash
# Compile to C
./symirc input.sir --target c -o out.c

# Compile to WebAssembly
./symirc input.sir --target wasm -o out.wat
```

### 4. Solve for Symbolic Values
Automatically find values for symbols that satisfy a specific execution path:
```bash
./symirsolve template.sir --path '^entry,^b1,^exit' -o concrete.sir
```
## SymLang Example

More in [./examples](./examples/) and [./test/](./test/).

```
// EXPECT: PASS
// ARGS: --main @main --path '^entry,^loop,^loop,^loop,^loop,^exit'

// Polynomial loop synthesis
// Find 'x' such that iterating `acc = acc + x + i` for 4 times results in specific value.
// acc_0 = 0
// i=0: acc_1 = 0 + x + 0 = x
// i=1: acc_2 = x + x + 1 = 2x + 1
// i=2: acc_3 = (2x+1) + x + 2 = 3x + 3
// i=3: acc_4 = (3x+3) + x + 3 = 4x + 6
// Target: 46.
// 4x + 6 = 46 => 4x = 40 => x = 10.

fun @main() : i32 {
  sym %?x : value i32 in [0, 100];
  let mut %acc: i32 = 0;
  let mut %i: i32 = 0;
  let %one: i32 = 1;
  let %target: i32 = 46;
  let mut %tmp: i32 = 0;

^entry:
  br ^loop;

^loop:
  // acc = acc + x + i
  %tmp = %?x + %i;
  %acc = %acc + %tmp;

  %i = %i + %one;
  br %i < 4, ^loop, ^exit;

^exit:
  require %acc == %target, "accumulator must match target value after loop";
  ret 0;
}
```

## Project Structure

```text
.
├── include/          # Header files
│   ├── ast/          # AST definitions
│   ├── frontend/     # Lexer, Parser, TypeChecker
│   ├── analysis/     # CFG, Dataflow, Pass Manager
│   ├── backend/      # C and WASM backends
│   └── solver/       # SMT integration
├── src/              # Implementation files
├── docs/             # Tool and language documentation
├── test/             # Test suite and regression tests
└── Makefile          # Build system
```

## Documentation

- **[Language Specification (v0)](./docs/SPEC_v0.md)**
- **[symiri User Guide](./docs/symiri.md)**
- **[symirc User Guide](./docs/symirc.md)**
- **[symirsolve User Guide](./docs/symirsolve.md)**

## License

MIT.
