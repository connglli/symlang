# SymIR Fuzzer (`symirfuzz`)

`symirfuzz` is a Python-based black-box generation fuzzer for the SymIR toolchain. It generates SymIR programs to test the correctness, robustness, and consistency of the Interpreter (`symiri`), Compiler (`symirc`), and Solver (`symirsolve`).

## Usage

```bash
# Differential fuzzing (Interpreter vs Compiler)
python3 test/fuzz/symirfuzz.py --mode differential --iterations 100

# Solver soundness (Verification of SAT templates)
python3 test/fuzz/symirfuzz.py --mode solver --gen-mode markov

# Frontend robustness (Testing rejection of malformed programs)
python3 test/fuzz/symirfuzz.py --mode crash --validity invalid
```

## Design Philosophy

The fuzzer is implemented in **Python** to decouple the test generator from the C++ implementation under test. This prevents shared implementation bugs from masking issues.

### 1. Validity Modes
The fuzzer supports three primary validity modes:

*   **`--validity valid`**: Generates well-typed, UB-free programs. Used for differential fuzzing and solver soundness.
*   **`--validity ub`**: Generates well-typed programs that may contain Undefined Behavior (e.g., division by zero, signed overflow).
*   **`--validity invalid`**: Generates intentionally malformed programs (syntactic, semantic, or CFG errors) to test frontend diagnostics.

### 2. UB-Free Generation (`--validity valid`)
To enable **Differential Fuzzing**, generated programs must be strictly UB-free. `symirfuzz` employs:

*   **Shadow Execution**: For concrete programs, the generator maintains a mirror state in Python. It checks shadow values before emitting instructions to ensure they won't trigger UB.
*   **CFG Guarding (Defensive Branching)**: For programs with unknown values (loops, parameters, symbols), dangerous operations are wrapped in guards:
    ```sir
    %is_safe = ... // check for overflow/div-zero
    br %is_safe, ^do_op, ^skip_op;
    ^do_op:
      %x = %a + %b;
      br ^continue;
    ^skip_op:
      %x = %a; // safe fallback
      br ^continue;
    ^continue:
    ```
*   **Symbol/Parameter Constraints**: Using `assume` to restrict inputs to safe ranges.

### 3. Verification Architecture
*   **Interpreter (`symiri`)**: Invoked via CLI with `--sym` flags for input values.
*   **Compiler (`symirc`)**:
    1.  Translate `.sir` to `.c`.
    2.  Compile to a shared library (`.so`) using `gcc -shared -fPIC`.
    3.  **Python-C Interop**: Use `ctypes` to load the library and register callbacks for external symbols (mangled as `func__sym`). The runner invokes the target function directly from Python.

## Statistics and Feedback

The fuzzer provides periodic updates on its progress and the health of the toolchain.

### 1. Coverage Feedback
*   Uses `gcov`/`llvm-cov` to track edge and branch coverage of the C++ codebase.
*   Displays total coverage, coverage delta (new edges discovered), and module-wise breakdown.

### 2. Internal IR Metrics
*   **Structure**: Average atoms per expression, max LValue path depth, type diversity.
*   **Logic**: UB pruning rate (efficiency of shadow executor), guard density.
*   **CFG**: Loop nesting depth, edge connectivity, and reachability.

### 3. Tool Performance
*   Average and max time spent in the solver, interpreter, and compiler backend.

## Implementation Structure

*   `symirfuzz.py`: Main entry point, CLI, and orchestration loop.
*   `generator.py`: Core logic for AST construction, Shadow Execution, and UB masking. Supports both Weighted Random and Markov-chain generation.
*   `runner.py`: Driver for tool invocation, `ctypes` interop, and result comparison.
