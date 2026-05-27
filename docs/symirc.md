# symirc — SymIR Translator

`symirc` translates SymIR (`.sir`) programs into **C** or **WebAssembly**.

It supports both:
- **concrete programs**, and
- **symbolic programs**, where symbols are emitted as external declarations/imports.

`symirc` does not solve constraints; it preserves symbolic structure for external integration.


## Goals

- Provide first-class translation to C and WebAssembly
- Preserve precise SymIR semantics
- Allow symbolic programs to be linked with external providers of symbol values
- Act as a backend-independent lowering stage for future targets


## Usage

```bash
symirc <input.sir> --target <c|wasm> [-o output]
````

### Examples

Translate to C:

```bash
symirc prog.sir --target c -o prog.c
```

Translate to WebAssembly (text format):

```bash
symirc prog.sir --target wasm -o prog.wat
```


## Symbolic Programs

If the input program declares symbols (`sym`), `symirc` emits **external symbol hooks**.

### C target

Each symbol is translated into a zero-argument external function:

```c
extern int32_t f0__c4(void);
```

Symbol references become calls:

```c
f0__c4()
```

### WebAssembly target

Each symbol becomes an imported function:

```wat
(import "f0" "c4" (func $f0__c4 (result i32)))
```

Symbol references become:

```wat
(call $f0__c4)
```

### Name Mangling

SymIR symbols may appear as global symbols (`@?name`) or local symbols (`%?name`). For translation targets,
**both are treated as external providers**. The naming scheme is stable and deterministic:

- Format: `<func>__<sym>`
- `<func>` is the function basename with sigils removed (e.g., `@f0` → `f0`)
- `<sym>` is the symbol basename with sigils removed (e.g., `@?c4` → `c4`, `%?k` → `k`)

Examples:
- `@f0` + `@?c4` → `f0__c4`
- `@f0` + `%?k`  → `f0__k`


## Options

| Option             | Description                                |
| ------------------ | ------------------------------------------ |
| `--target c`       | Emit C source (default)                    |
| `--target wasm`    | Emit WebAssembly (WAT)                     |
| `-o <file>`        | Output file (default: stdout)              |
| `--vec-lowering <s>` | Vector lowering strategy for the C backend |
| `--dump-ast`       | Dump the AST to stdout and exit            |
| `-w`               | Inhibit all warning messages               |
| `--Werror`         | Make all warnings into errors              |
| `--no-require`     | Omit `require` checks from emitted code (useful for compiler testing) |
| `-h, --help`       | Print usage                                |


## Limitations (v0.2.1)

* `i1`, `i8`, `i16`, `i32`, `i64`, `f32`, `f64`, arrays, structs, and pointers (`ptr T`) lower to both C and WASM.
* Heap allocation is still out of scope; pointers always refer to stack-resident `let mut` locals (see spec §2.8).
* No optimization passes — the lowered C/WASM follows the source closely.
* In WASM, pointers are 32-bit addresses into the linear memory; in C they are native C pointers. Pointer arithmetic and `ptr - ptr` (element distance) are both supported, but cross-object arithmetic remains UB per spec §7.5.
* WASM vector support currently unrolls operations lane-by-lane on the shadow stack (as aggregates). Native WebAssembly SIMD-128 lowering (using the `v128` type and instructions) is planned per SPEC §10.16.

## Refinement and Undefined Behavior Semantics

The compilers perform **semantic refinement** over the input SymIR program. Under refinement:
- The compiled target program (C or WebAssembly) must not exhibit any observable behavior that was not allowed by the original source program.
- This semantic equivalence is guaranteed only when the input program is **UB-free** (free of Undefined Behavior).
- If the input program executes a path containing Undefined Behavior (such as signed integer overflow, division/modulo by zero, or invalid pointer navigation/comparison), the behavior of the target program is **not guaranteed** and may deviate from strict SymIR interpreter/solver checks (which model UB as a fatal execution constraint).
- **C Target vs. WASM Target**:
  - For the **C target**, we try our best to preserve the trapping semantics of SymIR undefined behaviors. Because many of SymIR's undefined behaviors map cleanly to native C undefined behaviors, compiling the output C code with GCC and enabling sanitizers (e.g., `-fsanitize=address,undefined,float-cast-overflow,pointer-compare,pointer-subtract`) allows the runtime to catch and trap these events.
  - However, **this effort is not put on the WebAssembly (WASM) target**. The WASM backend lowers SymIR constructs to clean, native WASM instructions without inserting safety checks or runtime sanitizer assertions. Any executed undefined behavior on WASM will follow standard WASM instruction behavior (e.g. wrapping on signed overflow, returning 0 on modulo overflow, or ignoring relational pointer provenance).

### Minimal WebAssembly Example (Signed Modulo Overflow)

Consider the following SymIR program:

```sir
fun @main() : i32 {
  let mut %min: i32 = -2147483648; // INT_MIN
  let mut %neg1: i32 = -1;
  let mut %res: i32 = 0;
^entry:
  // INT_MIN % -1 triggers signed overflow (Undefined Behavior under Spec §7.1 Rule 4)
  %res = %min % %neg1;
  ret %res;
}
```

- **SymIR Semantics**: Since this operation triggers signed overflow, a strict SymIR symbolic execution path or interpreter execution will trap/fail immediately, treating the execution as invalid.
- **WebAssembly compilation**: When translated to WebAssembly, the modulo instruction is compiled directly to WASM's native signed remainder instruction:
  ```wat
  local.get $min
  local.get $neg1
  i32.rem_s
  ```
  In the WebAssembly specification, `i32.rem_s` with `INT_MIN` and `-1` does not trap (unlike `i32.div_s`); instead, it returns `0` silently. Consequently, the compiled target program exits successfully with `0`, demonstrating that the behavior of target code is not guaranteed once undefined behavior is introduced in the source.
