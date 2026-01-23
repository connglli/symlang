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
| `--target c`       | Emit C source                              |
| `--target wasm`    | Emit WebAssembly (WAT)                     |
| `-o <file>`        | Output file (default: stdout)              |


## Limitations (v0)

* Only `i32` and `i64` are guaranteed to translate cleanly
* No heap or pointer support
* No optimization passes
