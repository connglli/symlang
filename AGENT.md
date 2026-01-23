# SymLang (SymIR): Agent Guideline

+ Knowledge Background: Optimizing Compilers, Program Analysis, Symbolic Execution, SMT (Bit-Vector Theory)
+ Implementation Language: C++ 20
+ Primary Source Directory: ./src


## Project Overview

SymLang (internally called **SymIR**) is a **CFG-based symbolic intermediate representation** designed for:

- **Program synthesis**
- **Symbolic execution**
- **Constraint generation for SMT solvers (Bit-Vector logic)**

A SymIR program is a **template**, not a fully concrete program. It may contain **symbols** (unknowns, marked with `?`) whose values are solved later by an SMT solver under constraints derived from:

1. A **specific execution path** through the CFG
2. Explicit **properties** (e.g., `require` and/or other user-provided properties) that must hold on that path

The key design goals are:

- SMT-friendliness (predictable BV constraints, minimal nonlinearity)
- Clear semantics with **strict undefined behavior (UB)**
- Explicit control-flow (CFG, no SSA, user-friendly)
- Simplicity and analyzability over expressiveness

SymIR deliberately restricts expressions (flat, left-to-right, no parentheses) and uses **LLVM-style syntax** (`@`, `%`, `br`, basic blocks) while remaining language-agnostic.

The complete formal specification of the language is presented in: **[./docs/SPEC_v0.md](./docs/SPEC_v0.md)**

## Language at a Glance

Key characteristics:

- **Non-SSA**, mutable locals via `let mut`
- **Symbols**: solver-chosen unknowns (`@?x`, `%?y`)
- **CFG-based**: explicit basic blocks (`^label`)
- **Expressions**:
  - Flat, left-to-right
  - `+`, `-` at expression level
  - `*`, `/`, `%` at atom level
- **Division/modulo**: round toward 0 (C / SMT `bvsdiv`, `bvsrem`)
- **select** expression:
  - Lazy (only selected arm evaluated)
  - Expression-level conditional
- **Strict UB**:
  - Division/modulo by zero
  - Out-of-bounds array access
  - Reading `undef`

## Toolchain Overview

| Tool | Role |
|------|------|
| `symirc` | Translate `.sir` to C / WebAssembly |
| `symiri` | Interpret `.sir` programs |
| `symirsolve` | Concretize symbolic programs using SMT |

Documentation of each tool: [./docs/](./docs).

## Compilation / Analysis Pipeline

### Shared Frontend Pipeline

```
Source (.sir)
  ↓
Lexer
  ↓
Parser → AST
  ↓
CFG Builder
  ↓
TypeChecker (BV-aware)
  ↓
Semantic Checker
```

### Tool-Specific Pipelines

`symirc`:
```
Checked AST + CFG
  ↓
Lowering
  ↓
C / WASM Code Generation
```

`symiri`:
```
Checked AST + CFG
  ↓
Symbol Binding (--sym)
  ↓
Interpreter Execution
```

`symirsolve`:
```
Checked AST + CFG
  ↓
Path-based Symbolic Execution
  ↓
SMT Solving
  ↓
Concrete .sir
```

### 1. Lexer
- Converts source text into tokens
- Handles identifiers with sigils (`@`, `%`, `@?`, `%?`, `^`)
- Handles comments and string literals
- No semantic knowledge

### 2. Parser
- Recursive-descent parser
- Builds a **typed, structured AST**
- Preserves source spans for diagnostics
- AST is analysis-oriented (not syntax-oriented)

### 3. CFG Builder
- Indexes basic blocks by label
- Builds successor and predecessor lists
- Validates `br` targets
- Computes traversal orders (e.g., reverse postorder)
- Forms the backbone for all dataflow analyses

### 4. TypeChecker (BV-aware)
- Maps SymIR integer types to **SMT bit-vectors**
  - `i32` → `(_ BitVec 32)`
  - `i64` → `(_ BitVec 64)`
  - `iN`  → `(_ BitVec N)`
- Ensures:
  - Type correctness of expressions
  - Bitwidth compatibility
  - Correct typing of `select`
  - Assignment compatibility
  - Function return correctness
- Produces **typed annotations** for AST nodes
- Boolean conditions are treated separately from BV integers

### 5. Semantic Checker
Ensures program well-formedness beyond typing:

- Variables and symbols are declared before use
- No duplicate declarations
- Assignment only to `let mut` locals
- Parameters and symbols are immutable
- Definite initialization:
  - Parameters are initialized
  - `undef` is uninitialized
  - Reads before initialization are errors
- CFG consistency checks

### 6. Symbolic Execution / Constraint Generation
(Not fully implemented yet)

- Executes along a **user-selected path**
- Collects:
  - Path conditions from branches
  - Assumptions (`assume`)
  - Required properties (`require`)
- Applies **strict UB pruning**
- Produces BV constraints suitable for SMT solvers

### 7. Language Lower / Translator
- Translate a symbolic or concrete program into an existing language
- First-class support are C and WebAssembly.
- For symbolic program translation, use external function declarations to indicate symbols.
  - C: `extern int func_name_symbol_name(...);`
  - WASM: `import func_name symbol_name (func func_name_symbol_name (....))`


## Project Structure

Goto [./README.md]


## Testing – TDD Approach (MANDATORY)

ALWAYS follow a strict Test-Driven Development discipline.

### Required workflow

1. Write **five failing tests** that expose the bug or demonstrate the desired behavior
2. Run the tests **one by one** to confirm they fail
3. Implement the fix or feature
4. Run the tests **one by one** to confirm they pass
5. Add the **smallest additional test** that covers edge cases, in the correct test directory

### Never

1. Disable failing tests
2. Modify tests to avoid triggering bugs
3. Add workarounds that bypass the real issue
4. Implement features without a test demonstrating them first

## Dependency Management

### C++
- Dependencies are managed **manually**
- Prefer header-only or standard-library-only solutions
- When introducing a new dependency:
  - Update `README.md`
  - Clearly document installation steps and versions

### Python (if used for tooling)
- Virtual environment: `./venv`
- Activate with:
  ```bash
  source venv/bin/activate
````

* Dependencies:
  * `requirements.txt` – runtime
  * `requirements.dev.txt` – development
* Always pin exact versions

## Best Practices

1. Use git frequently and meaningfully
2. Follow **Conventional Commits**
3. Keep `README.md`, `SPEC.md`, `AGENT.md`, and `TODO.md` up to date
4. Fix **all compiler warnings**
5. Keep a clean, layered project structure
6. Write high-quality comments that explain *why*, not *what*

## Before Starting Work

1. Review recent history:

   ```bash
   git log [--oneline] [--stat] [--name-only] # Show brief/extended history
   git show [--summary] [--stat] [--name-only] <commit> # Show brief/extended history of a commit
   git diff <commit> <commit> # Compare two different commits
   git checkout <commit> # Checkout and inspect all the details of a commit
   ```
2. Understand existing design decisions before changing behavior
3. For large tasks, commit incrementally with clear messages

## Before Saving Changes

ALWAYS:

1. Clear all compiler warnings
2. Format code with `clang-format`
3. Ensure all tests pass (timeouts excepted)
4. Check changes with `git status`
5. Split work into small, reviewable commits
6. Use Conventional Commit messages:

```text
<type>[optional scope]: <title>

<body>

[optional footer]
```

* Title ≤ 50 characters
* Body explains intent and design impact

**Remember:**
SymIR prioritizes *clarity, analyzability, and solver-friendliness* over surface-level convenience.
Preserve these properties in every change.
