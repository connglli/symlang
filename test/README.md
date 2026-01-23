# SymIR Testing Infrastructure

This directory contains the regression test suite for the SymIR frontend, analysis pipeline, and reference interpreter.

## Test Driver (`symiri --check`)

The `symiri` executable with the `--check` flag is the primary tool for validating SymIR files' static semantics. It performs the following stages:
1. **Lexing & Parsing**: Converts source to AST.
2. **Semantic Analysis**: Enforces sigil scoping, duplicate checks, and structural well-formedness.
3. **Type Checking**: Validates Bit-Vector widths and type compatibility.
4. **Dataflow Analysis**: Runs Reachability and Definite Initialization passes.

Without the `--check` flag, `symiri` also executes the program using the reference interpreter.

## Test Runner (`run_tests.py`)

The Python script `run_tests.py` automates the execution of the test suite. It discovers all `.sir` files in a directory, parses their metadata tags, and compares the tool's exit code against the expected outcome.

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

**Example: `test/interp/math.sir`**
```sir
// EXPECT: PASS
// ARGS: --sym %?input=5
fun @main() : i32 {
  sym %?input : value i32;
^entry:
  let %res: i32 = %?input * 2;
  require %res == 10, "5 * 2 must be 10";
  ret %res;
}
```

## Running Tests

Use the provided `Makefile` targets:

```bash
# Run all tests (Frontend, Analysis, Interpreter, and Compiler)
make test

# Run only the interpreter tests
python3 run_tests.py test/interp ./symiri

# Run only a specific analysis directory
python3 run_tests.py test/typechecker ./symiri --check
```
