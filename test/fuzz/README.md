# SymIR Fuzzer (`symirfuzz`)

`symirfuzz` is a Python-based black-box generation fuzzer for the SymIR toolchain. It generates syntactically valid SymIR programs to test the correctness, robustness, and consistency of the Interpreter (`symiri`), Compiler (`symirc`), and Solver (`symirsolve`).

## Design Philosophy

The fuzzer is implemented in **Python** to decouple the test generator from the C++ implementation under test. This prevents shared implementation bugs (e.g., in a shared AST library) from masking issues.

### 1. Correct-by-Construction (Structure)
The fuzzer maintains a valid compilation state (symbol table, type environment, CFG context) to ensure it always emits syntactically valid and well-typed programs.

### 2. Safe-by-Construction (Semantics)
To enable **Differential Fuzzing** (comparing Interpreter vs Compiler), the generated programs must be free of Undefined Behavior (UB). `symirfuzz` achieves this via **Shadow Execution**:

*   **Shadow State**: The generator tracks the concrete runtime value of every variable it creates.
*   **Oracle Validation**: Before emitting an instruction (e.g., `a / b`), the generator checks its shadow values:
    *   If `b == 0`, it emits a `select` to mask `b` to a non-zero value.
    *   If `a + b` would overflow (32-bit signed), it emits a mask or chooses different operands.
*   **Guaranteed Validity**: The resulting program is guaranteed to execute without UB on the specific path generated.

## Fuzzing Modes

### Mode 1: Differential Fuzzing (`--mode differential`)
Verifies that the Interpreter and Compiled C code behave identically.
1.  Generate a **Concrete, UB-Free** program.
2.  Run `symiri <prog.sir>` -> Capture Output A.
3.  Run `symirc <prog.sir>`, Compile with `gcc`, Run `./a.out` -> Capture Output B.
4.  **Fail if**: `Output A != Output B`.

### Mode 2: Solver Soundness (`--mode solver`)
Verifies that the Solver can find a known-valid solution.
1.  Generate a program with `sym` unknowns.
2.  Internally pick "Secret Values" for these symbols.
3.  Compute the expected result using Shadow Execution.
4.  Emit `require %result == EXPECTED`.
5.  Run `symirsolve <prog.sir>`.
6.  **Fail if**: Solver returns `UNSAT` (it should be SAT because we constructed a valid solution).

### Mode 3: Crash Testing (`--mode crash`)
Stress-tests the tools for stability.
1.  Generate syntactically valid programs **without** UB protection.
2.  Allowed to generate division by zero, overflow, etc.
3.  Run all tools.
4.  **Fail if**: Tool Segfaults, hangs, or triggers an Internal Compiler Error (ASAN/UBSAN failure).
5.  **Pass if**: Tool exits with 0 or 1 (graceful error handling).

## Implementation Plan

*   `symirfuzz.py`: Main entry point and CLI.
*   `generator.py`: Core logic for AST construction, Shadow Execution, and UB masking.
*   `runner.py`: Driver to invoke `symiri`, `symirc`, `gcc`, and `symirsolve` and compare results.
