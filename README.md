# SymLang (SymIR): A CFG-Based Symbolic Language / Intermediate Representation

> [!WARNING]
> The project is under active development. APIs and IR details are still evolving, but semantic decisions in `docs/SPEC_v0.2.1.md` are authoritative for the current release (v0.2.1).

SymIR (also referred to as SymLang) is a **CFG-based symbolic intermediate representation** designed for program synthesis, symbolic execution, and constraint generation for SMT solvers using **bit-vector (BV) logic**.

It provides a robust foundation for building tools that need to reason about program semantics, explore execution paths, or synthesize code snippets that satisfy specific properties.

## Table of Contents

- [Core Concepts](#💡-core-concepts)
- [Tools Overview](#🛠️-tools-overview)
- [Getting Started](#🚀-getting-started)
  - [Prerequisites](#prerequisites)
  - [Building](#building)
  - [Usage](#usage)
  - [Switching SMT Backends](#switching-smt-backends)
- [SymLang Example](#📝-symlang-example)
- [Project Structure](#📁-project-structure)
- [Documentation](#📚-documentation)

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
| `rysmith` | **Reifier**: Generate random SymLang leaf functions for compiler testing. |
| `rylink` | **Reifier**: Generate random SymLang whole programs for compiler testing. |

## 🚀 Getting Started

### Prerequisites

- **C++20** compatible compiler (GCC 10+ or Clang 10+)
- **Bitwuzla** (Required by default)
  - Install: https://github.com/bitwuzla/bitwuzla
- **Z3** (Optional)
  - Install: https://github.com/Z3Prover/z3
- **Python 3** (for Usage the test suite)
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

#### Generate Random Programs
```bash
./rysmith -n 100
./rylink -n 100
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

A brainfuck interpreter showcasing v0.2.1 features:
`ptr @S` + `ptrfield` for register-file navigation, `<N> T` vector opcode table with `cmp`-based decoding, and reified `i1` comparison results.

```rust
struct @BFRegs {
  dp:      i32;  // Data pointer (index into 'tape')
  ip:      i32;  // Instruction pointer (index into 'code')
  out_idx: i32;  // Next write position in 'output'
}

fun @main() : i32 {
  // Missing instruction byte — solver must find 60 ('<').
  sym %?missing : value i8 in {60, 62, 43, 45};

  // BF opcode table as a <8> i8 SIMD vector (v0.2.1).
  // Each lane holds one command byte; the decoder reads lanes via subscript.
  let %opcodes: <8> i8 = {62, 60, 43, 45, 46, 91, 93, 0};
  let %LANE_INC_PTR:    i32 = 0; // '>'
  let %LANE_DEC_PTR:    i32 = 1; // '<'
  let %LANE_INC_VAL:    i32 = 2; // '+'
  let %LANE_DEC_VAL:    i32 = 3; // '-'
  let %LANE_OUTPUT:     i32 = 4; // '.'
  let %LANE_LOOP_START: i32 = 5; // '['
  let %LANE_LOOP_END:   i32 = 6; // ']'
  let %LANE_NUL:        i32 = 7; // NUL

  let mut %tape:   [100] i8 = 0;
  let mut %code:   [100] i8 = 0;
  let mut %output: [100] i8 = 0;

  // Register file in a struct; accessed exclusively through ptrfield (v0.2.1).
  let mut %regs:  @BFRegs = 0;
  let mut %pregs: ptr @BFRegs = null;
  let mut %p_dp:  ptr i32     = null;
  let mut %p_ip:  ptr i32     = null;
  let mut %p_out: ptr i32     = null;

  let mut %instr:   i8  = 0;
  let mut %byte_tmp: i8 = 0;
  let mut %opcode:  i8  = 0;
  let mut %ret_val: i32 = 0;
  let mut %depth:   i32 = 0;
  let mut %idx_tmp: i32 = 0;
  let mut %d_plus:  i32 = 0;
  let mut %d_minus: i32 = 0;
  // v0.2.1: reified comparison — i1 value produced by cmp.
  let mut %eq_flag: i1  = 0;

  let %CODE_LIMIT: i32 = 100;
  let %OUT_LIMIT:  i32 = 100;
  let %ONE:        i32 = 1;

^entry:
  // v0.2.1 ptrfield: project typed pointers to each register field.
  %pregs = addr %regs;
  %p_dp  = ptrfield %pregs, dp;
  %p_ip  = ptrfield %pregs, ip;
  %p_out = ptrfield %pregs, out_idx;

  // Load BF program: ++++++++[>++++++++<-]>+.  (Prints 'A')
  %code[0] = 43; %code[1] = 43; %code[2] = 43; %code[3] = 43;
  %code[4] = 43; %code[5] = 43; %code[6] = 43; %code[7] = 43;
  %code[8] = 91; %code[9] = 62;
  %code[10] = 43; %code[11] = 43; %code[12] = 43; %code[13] = 43;
  %code[14] = 43; %code[15] = 43; %code[16] = 43; %code[17] = 43;
  %code[18] = %?missing; // [SYMBOLIC HOLE] — expected: 60 ('<')
  %code[19] = 45; %code[20] = 93; %code[21] = 62;
  %code[22] = 43; %code[23] = 46; %code[24] = 0;
  br ^dispatch;

^dispatch:
  %idx_tmp = load %p_ip;
  br %idx_tmp >= %CODE_LIMIT, ^exit, ^fetch;

^fetch:
  %idx_tmp = load %p_ip;
  %instr   = %code[%idx_tmp];
  // v0.2.1 cmp: compare against NUL lane — produces a reified i1.
  %opcode  = %opcodes[%LANE_NUL];
  %eq_flag = cmp == %instr, %opcode;
  br %eq_flag == 1, ^exit, ^decode_inc_ptr;

^decode_inc_ptr:
  %opcode  = %opcodes[%LANE_INC_PTR];
  %eq_flag = cmp == %instr, %opcode;
  br %eq_flag == 1, ^do_inc_ptr, ^decode_dec_ptr;

^decode_dec_ptr:
  %opcode  = %opcodes[%LANE_DEC_PTR];
  %eq_flag = cmp == %instr, %opcode;
  br %eq_flag == 1, ^do_dec_ptr, ^decode_inc_val;

^decode_inc_val:
  %opcode  = %opcodes[%LANE_INC_VAL];
  %eq_flag = cmp == %instr, %opcode;
  br %eq_flag == 1, ^do_inc_val, ^decode_dec_val;

^decode_dec_val:
  %opcode  = %opcodes[%LANE_DEC_VAL];
  %eq_flag = cmp == %instr, %opcode;
  br %eq_flag == 1, ^do_dec_val, ^decode_output;

^decode_output:
  %opcode  = %opcodes[%LANE_OUTPUT];
  %eq_flag = cmp == %instr, %opcode;
  br %eq_flag == 1, ^do_output, ^decode_loop_start;

^decode_loop_start:
  %opcode  = %opcodes[%LANE_LOOP_START];
  %eq_flag = cmp == %instr, %opcode;
  br %eq_flag == 1, ^do_loop_start, ^decode_loop_end;

^decode_loop_end:
  %opcode  = %opcodes[%LANE_LOOP_END];
  %eq_flag = cmp == %instr, %opcode;
  br %eq_flag == 1, ^do_loop_end, ^next_instr;

// Register mutations go through ptrfield-derived pointers.
^do_inc_ptr:
  %idx_tmp = load %p_dp;
  %idx_tmp = %idx_tmp + %ONE;
  store %p_dp, %idx_tmp;
  br ^next_instr;

^do_dec_ptr:
  %idx_tmp = load %p_dp;
  %idx_tmp = %idx_tmp - %ONE;
  store %p_dp, %idx_tmp;
  br ^next_instr;

^do_inc_val:
  %idx_tmp  = load %p_dp;
  %byte_tmp = %tape[%idx_tmp];
  %byte_tmp = %byte_tmp + 1;
  %tape[%idx_tmp] = %byte_tmp;
  br ^next_instr;

^do_dec_val:
  %idx_tmp  = load %p_dp;
  %byte_tmp = %tape[%idx_tmp];
  %byte_tmp = %byte_tmp - 1;
  %tape[%idx_tmp] = %byte_tmp;
  br ^next_instr;

^do_output:
  %idx_tmp = load %p_out;
  br %idx_tmp < %OUT_LIMIT, ^write_out, ^next_instr;

^write_out:
  %idx_tmp  = load %p_dp;
  %byte_tmp = %tape[%idx_tmp];
  %idx_tmp  = load %p_out;
  %output[%idx_tmp] = %byte_tmp;
  %idx_tmp = %idx_tmp + %ONE;
  store %p_out, %idx_tmp;
  br ^next_instr;

^do_loop_start:
  %idx_tmp  = load %p_dp;
  %byte_tmp = %tape[%idx_tmp];
  br %byte_tmp == 0, ^scan_fwd_init, ^next_instr;

^do_loop_end:
  %idx_tmp  = load %p_dp;
  %byte_tmp = %tape[%idx_tmp];
  br %byte_tmp != 0, ^scan_bwd_init, ^next_instr;

^scan_fwd_init:
  %depth = 1; br ^scan_fwd_step;

^scan_fwd_step:
  %idx_tmp = load %p_ip;
  %idx_tmp = %idx_tmp + %ONE;
  store %p_ip, %idx_tmp;
  br %idx_tmp >= %CODE_LIMIT, ^exit, ^scan_fwd_check;

^scan_fwd_check:
  %idx_tmp  = load %p_ip;
  %byte_tmp = %code[%idx_tmp];
  // cmp + select to update bracket depth without branching.
  %opcode  = %opcodes[%LANE_LOOP_START];
  %eq_flag = cmp == %byte_tmp, %opcode;
  %d_plus  = %depth + 1;
  %depth   = select %eq_flag == 1, %d_plus, %depth;
  %opcode  = %opcodes[%LANE_LOOP_END];
  %eq_flag = cmp == %byte_tmp, %opcode;
  %d_minus = %depth - 1;
  %depth   = select %eq_flag == 1, %d_minus, %depth;
  br %depth == 0, ^next_instr, ^scan_fwd_step;

^scan_bwd_init:
  %depth = 1; br ^scan_bwd_step;

^scan_bwd_step:
  %idx_tmp = load %p_ip;
  %idx_tmp = %idx_tmp - %ONE;
  store %p_ip, %idx_tmp;
  br %idx_tmp < 0, ^exit, ^scan_bwd_check;

^scan_bwd_check:
  %idx_tmp  = load %p_ip;
  %byte_tmp = %code[%idx_tmp];
  %opcode  = %opcodes[%LANE_LOOP_END];
  %eq_flag = cmp == %byte_tmp, %opcode;
  %d_plus  = %depth + 1;
  %depth   = select %eq_flag == 1, %d_plus, %depth;
  %opcode  = %opcodes[%LANE_LOOP_START];
  %eq_flag = cmp == %byte_tmp, %opcode;
  %d_minus = %depth - 1;
  %depth   = select %eq_flag == 1, %d_minus, %depth;
  br %depth == 0, ^scan_bwd_done, ^scan_bwd_step;

^scan_bwd_done:
  br ^dispatch;

^next_instr:
  %idx_tmp = load %p_ip;
  %idx_tmp = %idx_tmp + %ONE;
  store %p_ip, %idx_tmp;
  br ^dispatch;

^exit:
  %byte_tmp = %output[0];
  %ret_val  = %byte_tmp as i32;
  ret %ret_val;
}
```

The full annotated source is at [./examples/brainfuck_v021.sir](./examples/brainfuck_v021.sir).
Find more examples in [./examples](./examples/) and [./test/](./test/).

## 📁 Project Structure

```text
.
├── include/          # Header files
│   ├── ast/          # AST definitions
│   ├── frontend/     # Lexer, Parser, TypeChecker
│   ├── analysis/     # CFG, Dataflow, Pass Manager
│   ├── backend/      # C and WASM backends
│   ├── solver/       # SMT integration
│   └── reify/        # Reify generator
├── src/              # Implementation files
├── docs/             # Tool and language documentation
├── test/             # Test suite and regression tests
└── Makefile          # Build system
```

## 📚 Documentation

- **[Language Specification (v0.2.1, current)](./docs/SPEC_v0.2.1.md)** — the current normative spec, including v0.2.1 additions: aggregate pointers (`ptr [N] T`, `ptr @S`, `ptrindex`, `ptrfield`), SIMD vector types (`<N> T`), and `cmp` expressions.
- **[Language Specification (v0.2.0, archived)](./docs/SPEC_v0.2.0.md)** — the pointer baseline (`ptr T`, `addr`, `load`, `store`, `null`).
- **[Language Specification (v0.1.0, archived)](./docs/SPEC_v0.1.0.md)** — the pre-pointer baseline, kept for reference.
- **[Undefined Behaviour reference](./docs/UB.md)**
- **[symiri User Guide](./docs/symiri.md)**
- **[symirc User Guide](./docs/symirc.md)**
- **[symirsolve User Guide](./docs/symirsolve.md)**
- **[rysmith/rylink User Guide](./docs/reify.md)**

## 📋 License

MIT.
