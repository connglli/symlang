# SymIR Testing Infrastructure

This directory contains the regression test suite for the SymIR frontend, analysis pipeline, reference interpreter, and C compiler.

## Unified Testing Framework

The SymIR test suite is managed by a unified Python-based testing framework located in `test/lib/`. It orchestrates the following tools:

- **`symiri --check`**: Validates static semantics (lexing, parsing, duplicate checks, type checking, and dataflow analysis).
- **`symiri`**: Executes SymIR programs using the reference interpreter.
- **`symirc`**: Compiles SymIR programs to C and verifies them by linking with a test harness.
- **`symirsolve`**: Solves symbolic SymIR programs into concrete SymIR programs given a path.

### Framework Features
- **Automatic Discovery**: Recursively finds all `.sir` files in a given directory.
- **Colored Output**: Uses ANSI colors for clear status reporting (**Green for OK**, **Red for FAIL**, **Yellow for TIMEOUT/SKIP**).
- **Timeouts**: Enforces execution limits (default 5s for tools, 1s for compiled binaries) to detect infinite loops.
- **Sanitization**: Compiler tests are linked with AddressSanitizer and UB Sanitizer to catch memory and semantic errors.

### Metadata Tags

Each `.sir` file should contain metadata tags in the first few lines to guide the runner:

| Tag | Description |
|---|---|
| `// EXPECT: PASS` | The tool is expected to succeed (exit code 0). |
| `// EXPECT: FAIL` | The tool is expected to report an error (non-zero exit code). |
| `// ARGS: <args>` | Additional CLI arguments passed to the tool (e.g., `--sym %?x=10`). |
| `// SKIP: <TOOL>` | Skip this test for a specific tool (`INTERPRETER` or `COMPILER`). |

## Writing Tests

### 1. Frontend & Analysis Tests
These tests validate that the compiler correctly identifies valid or invalid code. They are typically run using the `symiri --check` command.

**Example: `test/typechecker/fail_mismatch.sir`**
```sir
// EXPECT: FAIL
fun @main() : i32 {
  let %a: i32 = 1;
  let %b: i64 = 2;
^entry:
  ret %a + %b; // Should fail due to bitwidth mismatch
}
```

### 2. Interpreter Tests
These tests validate the runtime semantics of the language. They are run using the `symiri` binary without `--check`. To perform assertions in the interpreter, use the `require` instruction.

### 3. Compiler Tests
These tests validate the C backend by compiling `.sir` files to C, linking them with a generated test harness, and executing them. They are managed by `test/lib/run_compiler_tests.py`.

### 4. Solver Tests
These tests validate the symbolic execution capabilities of the `symirsolve` tool. They ensure that symbolic programs can be correctly solved into concrete programs.

## Running Tests

The recommended way to run tests is via the `Makefile` targets:

```bash
# Run all tests (Frontend, Analysis, Interpreter, and Compiler)
make test
```

Alternatively, you can run specific suites using the Python module syntax:

```bash
# Run only the interpreter tests
python3 -m test.lib.run_interp_tests test/interp ./symiri

# Run only the compiler tests
python3 -m test.lib.run_compiler_tests test/ ./symirc

# Run only the solver tests
python3 -m test.lib.run_solver_tests test/solver ./symirsolve

# Run only a specific analysis directory
python3 -m test.lib.run_interp_tests test/typechecker ./symiri --check
```
