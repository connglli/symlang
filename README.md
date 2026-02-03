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

## 💡 Core Concepts

- **Path-Oriented:** Designed specifically for symbolic execution and path-based analysis.
- **Symbolic by Design:** Programs may be **symbolic** (contain solver-chosen unknowns marked with `?`) or fully concrete.
- **Explicit Control Flow:** CFG-based representation using basic blocks and `br` instructions (no structured nesting requirements).
- **SMT-Friendly:** Expressions are intentionally restricted to flat, left-to-right forms to ensure predictable BV constraints.
- **Strict Semantics:** Includes **strict undefined behavior (UB)** for operations like division-by-zero or out-of-bounds access, facilitating bug-finding.

## 🛠️ Tools Overview

| Tool | Purpose |
| :--- | :--- |
| `symiri` | **Interpreter**: Execute `.sir` programs directly with concrete values or symbol bindings. |
| `symirc` | **Compiler**: Translate `.sir` programs into optimized C or WebAssembly (WASM). |
| `symirsolve` | **Solver**: Concretize symbolic programs by solving path constraints via SMT. |

## 🚀 Getting Started

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

To test all tools (interpreter, compiler, and solver):

```bash
make test
```

### Usages

#### Interpret a Concrete Program
```bash
./symiri examples/hello.sir
```

#### Interpret a Symbolic Program with Bindings
Provide concrete values for symbols at runtime:
```bash
./symiri examples/template.sir --sym %?a=10 --dump-trace
```

#### Compile to C or WebAssembly
```bash
# Compile to C
./symirc input.sir --target c -o out.c

# Compile to WebAssembly
./symirc input.sir --target wasm -o out.wat

```

#### Solve for Symbolic Values
Automatically find values for symbols that satisfy any execution path within 100 samples:
```bash
./symirsolve template.sir --sample 100 --require-terminal -o concrete.sir
```

Automatically find values for symbols that satisfy a specific execution path:
```bash
./symirsolve template.sir --path '^entry,^b1,^exit' -o concrete.sir
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

## 📝 SymLang Example

A brainfuck interpreter example:

```rust
struct @BFContext {
  tape: [100] i8;    // Memory tape for Brainfuck cells
  code: [100] i8;    // Memory for the Brainfuck bytecode
  output: [100] i8;  // Buffer to store program output characters
  dp: i32;           // Data Pointer (points into 'tape')
  ip: i32;           // Instruction Pointer (points into 'code')
  out_idx: i32;      // Index for the next output character
}

fun @main() : i32 {
  // =========================================================================================
  // SymIR Brainfuck Interpreter
  //
  // This program implements a simple Brainfuck interpreter.
  // It executes a hardcoded Brainfuck program stored in the %code array.
  // The code array contains a symbol %?missing whose expected value is 60.
  // To solve the program, the path in ./brainfuck_path.txt is required.
  //
  // Interpret:
  //   ./symiri --sym %?missing=60 ./examples/brainfuck.sir
  //
  // Compile:
  //   ./symirc -o brainfuck.c ./examples/brainfuck.sir
  //
  // Solve:
  //   ./symirsolve --path $(cat ./examples/brainfuck_path.txt) -o brainfuck.sir ./examples/brainfuck.sir
  //
  // Configuration:
  // - Tape Size: Defined by the size of %tape (default 100) and %TAPE_LIMIT.
  // - Code Size: Defined by the size of %code (default 100) and %CODE_LIMIT.
  // - Output Size: Defined by the size of %output (default 100).
  //
  // To change the program:
  // 1. Modify the array sizes below if your program is larger than 100 bytes.
  // 2. Update %CODE_LIMIT and %TAPE_LIMIT constants to match.
  // 3. Edit the "^entry" block to load your Brainfuck bytecode into %code.
  // =========================================================================================

  // --- 0. Symbol Definition ---
  // [SYMBOL] Missing instruction at %code[18].
  // Domain restricted to common BF operators to keep solving efficient.
  sym %?missing : value i8 in {60, 62, 43, 45}; // 60 is expected

  // --- 1. Memory Configuration ---
  // [USER-CONFIG] Change array sizes here if needed.
  // Broadcast 0 to all members of the context struct
  let mut %ctx: @BFContext = 0;

  // --- 2. State Variables ---
  // Pointers and Indices (Now fields in %ctx)

  // Temporary Variables for Logic
  let mut %instr:    i8 = 0;
  let mut %byte_tmp: i8 = 0;
  let mut %ret_val:  i32 = 0;

  // Depth calculation temps for loop scanning
  let mut %d_plus:  i32 = 0;
  let mut %d_minus: i32 = 0;

  // Intermediate indices (v0 doesn't allow nested indexing like %a.b[%c.d])
  let mut %idx_tmp: i32 = 0;

  // --- 3. Constants ---
  // [USER-CONFIG] Update limits to match array sizes above.
  let %CODE_LIMIT: i32 = 100;
  // let %TAPE_LIMIT: i32 = 100; // Unused in this simple version (we rely on UB for out-of-bounds)

  let %ONE: i32 = 1;
  // let %ZERO: i32 = 0; // Unused

  // --- 4. Brainfuck Command Set ---
  let %CMD_INC_PTR:    i8 = 62; // >
  let %CMD_DEC_PTR:    i8 = 60; // <
  let %CMD_INC_VAL:    i8 = 43; // +
  let %CMD_DEC_VAL:    i8 = 45; // -
  let %CMD_OUTPUT:     i8 = 46; // .
  // let %CMD_INPUT:      i8 = 44; // , (Implemented as no-op, commented out to avoid unused warning)
  let %CMD_LOOP_START: i8 = 91; // [
  let %CMD_LOOP_END:   i8 = 93; // ]

  // Bracket scanning depth counter
  let mut %depth: i32 = 0;

^entry:
  // =========================================================================================
  // [USER-CODE] Load Brainfuck Program Here
  // Current Program: ++++++++[>++++++++<-]>+. (Prints 'A')
  // =========================================================================================

  // 0-7: ++++++++ (Set cell #0 to 8)
  %ctx.code[0] = 43; %ctx.code[1] = 43; %ctx.code[2] = 43; %ctx.code[3] = 43;
  %ctx.code[4] = 43; %ctx.code[5] = 43; %ctx.code[6] = 43; %ctx.code[7] = 43;

  // 8: [ (Start loop)
  %ctx.code[8] = 91;

  // 9: > (Move to cell #1)
  %ctx.code[9] = 62;

  // 10-17: ++++++++ (Add 8 to cell #1)
  %ctx.code[10] = 43; %ctx.code[11] = 43; %ctx.code[12] = 43; %ctx.code[13] = 43;
  %ctx.code[14] = 43; %ctx.code[15] = 43; %ctx.code[16] = 43; %ctx.code[17] = 43;

  // 18: < (Move back to cell #0)
  // [SYMBOLIC HOLE]
  // Originally this was '<' (60). The solver must find this to satisfy the output requirement.
  %ctx.code[18] = %?missing;

  // 19: - (Decrement cell #0)
  %ctx.code[19] = 45;

  // 20: ] (End loop - repeat until cell #0 is 0)
  // Loop logic: cell #1 += 8 for each iteration. Total cell #1 = 8 * 8 = 64.
  %ctx.code[20] = 93;

  // 21: > (Move to cell #1, which is now 64)
  %ctx.code[21] = 62;

  // 22: + (Increment cell #1 to 65)
  %ctx.code[22] = 43;

  // 23: . (Output cell #1 - ASCII 'A')
  %ctx.code[23] = 46;

  // 24: Terminate (0)
  %ctx.code[24] = 0;

  // End of Program Loading
  br ^dispatch;

  // --- Main Dispatch Loop ---
^dispatch:
  // Check code bounds
  %idx_tmp = %ctx.ip;
  br %idx_tmp >= %CODE_LIMIT, ^exit, ^fetch;

^fetch:
  %idx_tmp = %ctx.ip;
  %instr = %ctx.code[%idx_tmp];
  // 0 terminates execution
  br %instr == 0, ^exit, ^decode_inc_ptr;

// --- Instruction Decoding ---
^decode_inc_ptr:
  br %instr == %CMD_INC_PTR, ^do_inc_ptr, ^decode_dec_ptr;

^decode_dec_ptr:
  br %instr == %CMD_DEC_PTR, ^do_dec_ptr, ^decode_inc_val;

^decode_inc_val:
  br %instr == %CMD_INC_VAL, ^do_inc_val, ^decode_dec_val;

^decode_dec_val:
  br %instr == %CMD_DEC_VAL, ^do_dec_val, ^decode_output;

^decode_output:
  br %instr == %CMD_OUTPUT, ^do_output, ^decode_loop_start;

^decode_loop_start:
  br %instr == %CMD_LOOP_START, ^do_loop_start, ^decode_loop_end;

^decode_loop_end:
  br %instr == %CMD_LOOP_END, ^do_loop_end, ^next_instr; // Skip unknown/comments

// --- Command Execution ---

^do_inc_ptr: // >
  %idx_tmp = %ctx.dp;
  %ctx.dp = %idx_tmp + %ONE;
  br ^next_instr;

^do_dec_ptr: // <
  %idx_tmp = %ctx.dp;
  %ctx.dp = %idx_tmp - %ONE;
  br ^next_instr;

^do_inc_val: // +
  %idx_tmp = %ctx.dp;
  %byte_tmp = %ctx.tape[%idx_tmp];
  %ctx.tape[%idx_tmp] = %byte_tmp + 1;
  br ^next_instr;

^do_dec_val: // -
  %idx_tmp = %ctx.dp;
  %byte_tmp = %ctx.tape[%idx_tmp];
  %ctx.tape[%idx_tmp] = %byte_tmp - 1;
  br ^next_instr;

^do_output: // .
  // Hardcoded output limit check (100)
  %idx_tmp = %ctx.out_idx;
  br %idx_tmp < 100, ^write_out, ^next_instr;
^write_out:
  %idx_tmp = %ctx.dp;
  %byte_tmp = %ctx.tape[%idx_tmp];
  %idx_tmp = %ctx.out_idx;
  %ctx.output[%idx_tmp] = %byte_tmp;
  %ctx.out_idx = %idx_tmp + %ONE;
  br ^next_instr;

^do_loop_start: // [
  %idx_tmp = %ctx.dp;
  %byte_tmp = %ctx.tape[%idx_tmp];
  // If current tape value is 0, jump to matching ]
  br %byte_tmp == 0, ^scan_fwd_init, ^next_instr;

^do_loop_end: // ]
  %idx_tmp = %ctx.dp;
  %byte_tmp = %ctx.tape[%idx_tmp];
  // If current tape value is nonzero, jump back to matching [
  br %byte_tmp != 0, ^scan_bwd_init, ^next_instr;

// --- Loop Skipping Logic (Forward Scan [ -> ]) ---
^scan_fwd_init:
  %depth = 1;
  br ^scan_fwd_step;

^scan_fwd_step:
  %idx_tmp = %ctx.ip;
  %ctx.ip = %idx_tmp + %ONE;
  %idx_tmp = %ctx.ip;
  br %idx_tmp >= %CODE_LIMIT, ^exit, ^scan_fwd_check;

^scan_fwd_check:
  %idx_tmp = %ctx.ip;
  %byte_tmp = %ctx.code[%idx_tmp];

  // depth++ if we see another [
  %d_plus = %depth + 1;
  %depth = select %byte_tmp == %CMD_LOOP_START, %d_plus, %depth;

  // depth-- if we see a ]
  %d_minus = %depth - 1;
  %depth = select %byte_tmp == %CMD_LOOP_END, %d_minus, %depth;

  // If depth is 0, we found the matching ]
  br %depth == 0, ^next_instr, ^scan_fwd_step;

// --- Loop Rewinding Logic (Backward Scan ] -> [) ---
^scan_bwd_init:
  %depth = 1;
  br ^scan_bwd_step;

^scan_bwd_step:
  %idx_tmp = %ctx.ip;
  %ctx.ip = %idx_tmp - %ONE;
  %idx_tmp = %ctx.ip;
  br %idx_tmp < 0, ^exit, ^scan_bwd_check;

^scan_bwd_check:
  %idx_tmp = %ctx.ip;
  %byte_tmp = %ctx.code[%idx_tmp];

  // depth++ if we see a ] (since we are going backwards)
  %d_plus = %depth + 1;
  %depth = select %byte_tmp == %CMD_LOOP_END, %d_plus, %depth;

  // depth-- if we see a [
  %d_minus = %depth - 1;
  %depth = select %byte_tmp == %CMD_LOOP_START, %d_minus, %depth;

  // If depth is 0, we found the matching [.
  br %depth == 0, ^scan_bwd_done, ^scan_bwd_step;

^scan_bwd_done:
  // We are at [. We want to execute it again (re-evaluate condition).
  // Jump directly to dispatch to avoid incrementing %ip in ^next_instr
  br ^dispatch;


^next_instr:
  %idx_tmp = %ctx.ip;
  %ctx.ip = %idx_tmp + %ONE;
  br ^dispatch;

^exit:
  // Return the first byte of output (helpful for testing)
  %byte_tmp = %ctx.output[0];
  %ret_val = %byte_tmp as i32;
  ret %ret_val;
}
```

Find more examples in [./examples](./examples/) and [./test/](./test/).

## 📁 Project Structure

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

## 📚 Documentation

- **[Language Specification (v0)](./docs/SPEC_v0.md)**
- **[symiri User Guide](./docs/symiri.md)**
- **[symirc User Guide](./docs/symirc.md)**
- **[symirsolve User Guide](./docs/symirsolve.md)**

## 📋 License

MIT.
