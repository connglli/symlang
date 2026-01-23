# SymIR Testing Infrastructure

This directory contains the regression test suite for the SymIR frontend, analysis pipeline, and reference interpreter.

## Test Driver (`symiri --check`)

The `symiri` executable with the `--check` flag is the primary tool for validating SymIR files' static semantics. It performs the following stages:
1. **Lexing & Parsing**: Converts source to AST.
2. **Semantic Analysis**: Enforces sigil scoping, duplicate checks, and structural well-formedness.
3. **Type Checking**: Validates Bit-Vector widths and type compatibility.
4. **Dataflow Analysis**: Runs Reachability and Definite Initialization passes.

Without the `--check` flag, `symiri` also executes the program using the reference interpreter.

## Unified Testing Framework

The SymIR test suite is managed by a unified Python-based testing framework located in `test/lib/`. It features:
- **Automatic Discovery**: Recursively finds all `.sir` files in a given directory.
- **Colored Output**: Uses ANSI colors for clear status reporting (**Green for OK**, **Red for FAIL**, **Yellow for TIMEOUT/SKIP**).
- **Timeouts**: Enforces execution limits (default 5s for tools, 1s for compiled binaries) to detect infinite loops.
- **Sanitization**: Compiler tests are linked with AddressSanitizer and UB Saniziter to catch memory and semantic errors.

### Metadata Tags

Each `.sir` file should contain metadata tags in the first few lines to guide the runner:

| Tag | Description |
|---|---|
| `// EXPECT: PASS` | The tool is expected to succeed (exit code 0). |
| `// EXPECT: FAIL` | The tool is expected to report an error (non-zero exit code). |
| `// ARGS: <args>` | Additional CLI arguments passed to the tool (e.g., `--sym %?x=10`). |

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

## Running Tests

The recommended way to run tests is via the `Makefile` targets:

```bash
# Run all tests (Frontend, Analysis, Interpreter, and Compiler)
make test
```

Alternatively, you can run specific suites using the Python module syntax:

```bash
# Run only the interpreter tests
python3 -m test.lib.run_tests test/interp ./symiri

# Run only the compiler tests
python3 -m test.lib.run_compiler_tests test/ ./symirc

# Run only a specific analysis directory
python3 -m test.lib.run_tests test/typechecker ./symiri --check
```
