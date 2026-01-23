# SymIR / SymLang

SymIR (also referred to as SymLang) is a **CFG-based symbolic intermediate representation** designed for
program synthesis, symbolic execution, and constraint generation for SMT solvers using **bit-vector (BV) logic**.

The project provides:
- a well-defined symbolic IR (`.sir`)
- a reference interpreter
- translators to C and WebAssembly
- a clean foundation for future concretization and solver integration

The language specification is defined in detail in:

👉 **[./docs/SPEC_v0.md](./docs/SPEC_v0.md)**


## Core Concepts

- Programs may be **symbolic** (contain solver-chosen unknowns) or **concrete**
- Symbols are explicit (`@?x`, `%?y`) and immutable
- Control flow is explicit via basic blocks and `br` instructions
- Expressions are intentionally restricted for SMT friendliness
- Semantics include **strict undefined behavior (UB)**


## Tools Overview

This repository currently provides three primary tools:

| Tool      | Purpose |
|-----------|--------|
| `symirc` | Translate `.sir` programs into C or WebAssembly |
| `symiri` | Interpret `.sir` programs directly |
| `symirsolve` | (Planned) Concretize symbolic programs using SMT |

Documentation of each tool: [./docs/](./docs).


## Typical Workflows

### Interpret a concrete program
```bash
symiri input.sir --main @f0
````

### Interpret a symbolic program with bindings

```bash
symiri input.sir --main @f0 --sym @?c4=3 --sym %?k=10
```

### Translate to C/WASM
```bash
symirc input.sir --target c -o out.c
symirc input.sir --target wasm -o out.wat
```

Note: for symbolic programs, `symirc` emits extern C function declarations or WASM imports.

### Concretize a symbolic template
```bash
symirsolve template.sir --main @f0 --path '^entry,^b1,^b2,^b1,^b3,^exit' -o concrete.sir
```


## Repository Structure

```
.
├── README.md
├── AGENT.md
├── SPEC.md
├── docs/
│   ├── symirc.md
│   ├── symiri.md
│   └── symirsolve.md
├── include/
│   ├── ast/
│   ├── frontend/
│   ├── analysis/
│   ├── backend/
│   └── interp/
├── src/
│   ├── symircc.cpp
│   ├── symiri.cpp
│   ├── frontend/
│   ├── analysis/
│   ├── backend/
│   └── interp/
└── test/
```


## Status

The project is under active development.
APIs and IR details are still evolving, but semantic decisions in `SPEC.md` are authoritative for v0.
