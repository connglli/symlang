# symiri — SymIR Interpreter

`symiri` is the **reference interpreter** for SymIR programs.

It executes programs directly from `.sir`, enforcing:
- strict evaluation order
- strict undefined behavior
- exact language semantics as defined in `SPEC.md`


## Goals

- Provide a semantic oracle for SymIR
- Enable fast debugging and validation
- Serve as a correctness baseline for code generation and synthesis


## Usage

```bash
symiri <input.sir> [--main <function>] [--sym name=value ...] [options]
```

### Examples

Interpret a concrete program (defaults to `@main`):

```bash
symiri prog.sir
```

Interpret a specific entry function:

```bash
symiri prog.sir --main @f0
```

Interpret a symbolic program:

```bash
symiri prog.sir --main @f0 --sym @?c4=3 --sym %?k=10
```


## Symbol Handling

If the program declares symbols:

* **All symbols must be bound** using `--sym`
* Missing or extra bindings result in an error
* Symbol values are treated as immutable BV constants

Value parsing:

* Decimal by default
* Parsed according to the symbol’s declared bit-width


## Runtime Semantics

* Expressions evaluate **left-to-right**
* `select` is lazy (only selected arm evaluated)
* Division and modulo round toward zero
* Undefined behavior immediately aborts execution with a diagnostic


## Options

| Option             | Description                                              |
| ------------------ | -------------------------------------------------------- |
| `--main <func>`    | Entry function to execute (default: `@main`)             |
| `--sym name=value` | Bind a symbol                                            |
| `--check`          | Check semantics and type correctness only (don't execute)|
| `--dump-trace`     | Dump executed blocks and variable updates during execution|
| `-w`               | Inhibit all warning messages                             |
| `--Werror`         | Make all warnings into errors                            |
| `-h, --help`       | Print usage                                              |


## Intended Use

`symiri` is intended for:

* validating symbolic solutions
* regression testing
* checking translation correctness
* exploring execution paths manually
