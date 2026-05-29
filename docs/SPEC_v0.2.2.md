# SymIR v0.2.2 Specification

**Status:** Draft v0.2.2

This document is the complete, standalone specification for SymIR v0.2.2. It supersedes v0.2.1. Sections and rules that are unchanged from v0.2.1 are included verbatim for self-containedness. All additions and changes are marked **[New in v0.2.2]**.


## What's new in v0.2.2

### Function calls

- **`call` atom**: invokes a function as an expression. `call @func(args...)` produces the callee's return value and may appear wherever an `Atom` is expected.
- **Call arguments**: zero or more comma-separated `Expr` values. Argument types must match parameter types exactly. Arguments are evaluated left-to-right before the call.
- **Interprocedural CFG**: calling a `fun` (with a body) creates a sub-execution context. Path conditions, store, and memory are threaded through the callee. UB in the callee makes the calling path infeasible.

### External declarations and contracts

- **`decl`**: declares a function defined in another compilation unit. Two forms:
  - **Contract form** `decl @name(params) : Type { pre... post... };` — provides a behavioral contract for solver-based reasoning. The body is **not** expected elsewhere; the contract *is* the specification.
  - **Link form** `decl @name(params) : Type;` — a plain signature. The body must be found in another `.sir` file via `-I` search paths.
- **`-I <path>` flag**: search path for `.sir` source files. When a tool encounters a link-form `decl`, it scans `-I` paths for a file containing a matching `fun @name`. Multiple `-I` flags may be specified; they are searched in order.
- **Mutual exclusion**: a `decl` may have a contract **or** resolve to an external body — never both. A `fun` may **never** have a contract.
- **`ret`**: a reserved identifier available only inside `post` clauses. It refers to the return value of the function.
- **No `old()`**: contracts can only express postconditions in terms of the post-call state. Comparing pre- and post-state requires staging through caller-side temporaries (see §10.4).

### Intrinsics

- **`intrinsic`**: a built-in function whose semantics are defined entirely by the SymIR toolchain — not delegated to the target language. Each intrinsic has a hard-coded implementation in the interpreter, a fixed SMT encoding in the solver, and a lowering rule in the compiler.
- **Standard intrinsics**: `@abs`, `@min`, `@max`, `@clz`, `@ctz`, `@popcount`. Memory intrinsics (`@memcpy`, `@memset`) are **not** included — their SMT encoding requires byte-level memory reasoning that is deferred.

### Restrictions

- **No indirect calls**: the target of `call` is always a statically-named `GlobalId`. Function pointers are not supported.
- **No recursion**: a `fun` body may not `call` itself (direct recursion) or participate in a cycle of `fun`-to-`fun` calls (mutual recursion). A contract-form `decl` may not reference `call` to itself. These are detected statically and reported as errors.
- **`decl` contract vs body is exclusive**: a `decl` may not both provide a contract and have a corresponding `fun` in the same program or in `-I` paths. The tool reports an error if both a contract and a body are visible for the same name.
- **No variadic functions**: all functions have a fixed parameter list.
- **All functions return a value**: there is no `void` return type.
- **No tail-call semantics**: calls are ordinary calls; tail-call elimination is a lowering concern.


## 1. Notation and identifier classes

SymIR uses sigils to make identifier categories immediately recognizable:

- `@name` — global identifiers (functions; global type names if desired).
- `%name` — local identifiers (parameters, locals).
- `@?name` — **global symbols** (solver-chosen unknowns).
- `%?name` — **local symbols** (solver-chosen unknowns).
- `^name` — basic block labels.

**Hard rule:** `?` is permitted **only** immediately after `@` or `%` to form `@?` / `%?`. It is forbidden in all other identifiers.


## 2. Key semantic commitments (v0.2.2)

### 2.1 Non-SSA and mutable store
SymIR is not SSA. Locals declared with `let mut` denote **mutable storage cells**. Assignments update the store at the given lvalue location.

### 2.2 Path-based execution
Given a user-chosen path `π` (e.g., `^entry -> ^b1 -> ^b3 -> ^b1 -> ^exit`), the tool executes blocks along `π` in order. Only statements and terminators encountered on `π` contribute constraints.

### 2.3 `assume` vs `require`
- `assume <cond>;` adds **feasibility/admissibility** constraints (part of template semantics).
- `require <cond>;` adds **property/synthesis** constraints (must hold on the chosen path).

### 2.4 Expressions: flat, left-to-right, no parentheses
- Expressions contain **no parentheses**.
- Expressions are a left-to-right chain of atoms combined by `+` and `-` only.
- Evaluation order is **left-to-right** (no reassociation or reordering).

### 2.5 `div` / `mod` round toward 0
Both integer and floating-point division and modulo round toward 0 (C-like truncation semantics). For floats, `%` is `fmod`, not IEEE `remainder`. See §8.

### 2.6 Strict undefined behavior (UB)
SymIR uses **strict UB** on the chosen path: if UB occurs during evaluation of any statement or condition on `π`, the path becomes **infeasible** and is pruned.

### 2.7 `select` expression (lazy)
`select` is supported as an atom:
- `select <cond>, <vtrue>, <vfalse>` — cond form, scalar predicate.
- `select <mask>, <vtrue>, <vfalse>` — mask form, `<N> i1` or `i1` per-lane blend.
- `select` is **lazy**: only the selected arm is evaluated.
- `vtrue` and `vfalse` are restricted to **scalar, pointer, or vector values** (`RValue`, constant `Coef`, or `null`). Both arms must have the same type.

### 2.8 Memory model
SymIR uses a typed, stack-only memory model:

- **Stack-only**: all addressable storage is a `let mut` local within the current function. Heap allocation is not supported.
- **No cross-object aliasing**: a pointer derived from `addr %x` can never alias a pointer derived from `addr %y` for distinct locals `%x` and `%y`. Cross-object pointer arithmetic is **UB**.
- **Typed memory regions**: the memory is conceptually partitioned by pointee type. Each distinct pointee type `T` has its own independent memory region.
- **Pointer width**: all pointers are 64-bit values (BV64 in the SMT model), regardless of pointee type.
- **Vectors are not in memory**: vector-typed values (`<N> T`) are pure register-like value types. No `ptr <N> T`, no `addr` on vector locals.

### 2.9 Floating-point value model

SymIR uses **finite IEEE 754-2008 semantics** for floating-point:

- **Domain**: the only valid floating-point values are **finite** IEEE 754 values. ±∞ and NaN are **not** SymIR values. Any operation whose IEEE 754 result would be ±∞ or NaN is UB (see §7.4).
- **Signed zeros**: `+0.0` and `-0.0` are distinct bit patterns and both are valid values. They compare equal (`+0.0 == -0.0` is `true`).
- **Subnormals**: subnormal (denormal) values are regular finite values. No flush-to-zero behavior.
- **Rounding mode**: all operations use a single fixed mode — **RNE (Round to Nearest, Ties to Even)**.
- **`%` for floats**: the `%` operator is **C's `fmod`** (truncated-quotient remainder), **not** IEEE 754 `remainder` (`fp.rem`).

SMT encoding: `f32` maps to `(_ FloatingPoint 8 24)` and `f64` maps to `(_ FloatingPoint 11 53)`. All FP operations use `roundNearestTiesToEven` except `%`, which encodes as `fp.sub(x, fp.mul(fp.roundToIntegral[RTZ](fp.div[RNE](x, y)), y))`.

### 2.10 Pointer arithmetic

- `ptr T + n` advances the address by `n * sizeof(T)` bytes, where `n` is an integer operand.
- `ptr T - n` retreats by `n * sizeof(T)` bytes.
- `ptr T - ptr T` yields the element distance as `i64`.
- `integer + ptr T` is **not supported**.
- Arithmetic is valid only within the bounds of the originating object. Out-of-bounds is UB.

**`sizeof`**:

| Type | `sizeof` |
|------|---------|
| `iN` | `⌈N / 8⌉` bytes |
| `f32` | 4 bytes |
| `f64` | 8 bytes |
| `ptr T` | 8 bytes |
| `[N] T` | `N * sizeof(T)` bytes |
| `@S` | `Σ sizeof(field_i)` bytes (packed, no padding) |

### 2.11 Vector types

Vectors are fixed-width SIMD value types `<N> T` where `T` is a scalar type (`iN`, `f32`, `f64`) and `N ≥ 2`. Vectors are pure value types (not addressable). Lane-wise arithmetic lifts all scalar operators. `cmp` produces `<N> i1` masks.

### 2.12 Function calls and interprocedural execution **[New in v0.2.2]**

- **`call @f(args...)`** evaluates arguments left-to-right, then transfers control to `@f`. The result is the callee's return value.
- **`fun` target**: a fresh callee context is created; parameters are bound to argument values. The callee's blocks execute along a sub-path. `Store`, `Mem`, `PC`, and `REQ` flow into and back from the callee. UB in the callee makes the calling path infeasible.
- **Contract `decl` target**: `pre` clauses are checked at the call site (violation → UB). `post` clauses are assumed true after the call, constraining the return value and post-call memory.
- **Link `decl` target**: the tool locates the `fun` body via `-I` and executes it as above. If no body is found, execution fails — the tool reports an error before producing any result.
- **`intrinsic` target**: the call is replaced by the intrinsic's built-in semantics (interpreter) or SMT encoding (solver) — see §12.
- **Calls in conditions and assumptions**: `call` is an atom and may appear inside `br`, `assume`, and `require` conditions. **Side effects (memory mutation, `PC`/`REQ` updates) commit unconditionally as part of evaluating the condition** — even if the resulting branch is not taken, even if an `assume` later renders the path infeasible. This is intentional: strict left-to-right evaluation order means the call's effects materialize at the point of evaluation.
- **`select` arms exclude `call`**: `select` arms (`SelectVal`) are restricted to `RValue` and `Coef` precisely because `select` is lazy. Allowing `call` in a lazy arm would make side-effect commitment depend on the predicate, breaking the strict left-to-right model.


## 3. Concrete syntax

### 3.1 Lexical
- `Ident` : `[A-Za-z_][A-Za-z0-9_]*`
- `Nat` : `[0-9]+`
- `IntLit` :
  - Decimal: `"-"? [0-9]+`
  - Hexadecimal: `"-"? "0x" [0-9A-Fa-f]+`
  - Octal: `"-"? "0o" [0-7]+`
  - Binary: `"-"? "0b" [01]+`
- `FloatLit` : standard floating point literal (e.g. `1.5`, `-0.2`, `1e-5`, `3.14E+2`).
- `StringLit` : double-quoted string (implementation-defined escapes)

**[New in v0.2.2]** New keywords: `call`, `decl`, `intrinsic`, `pre`, `post`, `ret`.

### 3.2 Types

```ebnf
Type        := IntType | FloatType | StructName | ArrayType | PtrType | VecType ;
IntType     := "i" Nat ;
FloatType   := "f32" | "f64" ;
ArrayType   := "[" Nat "]" Type ;
StructName  := GlobalId ;
PtrType     := "ptr" PointeeType ;
PointeeType := IntType | FloatType | PtrType | ArrayType | StructName ;
VecType     := "<" Nat ">" Type ;
```

### 3.3 Program structure **[Revised in v0.2.2]**

```ebnf
Program     := (StructDecl | FunDecl | ExtDecl | IntrinsicDecl)* ;

FunDecl     := "fun" GlobalId "(" ParamList? ")" ":" Type
               "{" SymDecl* LetDecl* Block+ "}" ;

ExtDecl     := "decl" GlobalId "(" ParamList? ")" ":" Type
               ( Contract | ";" ) ;

Contract    := "{" PreClause* PostClause+ "}" ;

PreClause   := "pre" Cond ("," StringLit)? ";" ;
PostClause  := "post" Cond ("," StringLit)? ";" ;

IntrinsicDecl := "intrinsic" GlobalId "(" ParamList? ")" ":" Type ";" ;
```

**[New in v0.2.2]** A `decl` may be:
- **Link form**: `decl @name(...) : Type;` — signature only. The body must be found via `-I` paths.
- **Contract form**: `decl @name(...) : Type { pre ... post ... };` — a behavioral contract. The body is **not** expected elsewhere.

A `fun` **never** has a contract. The `fun` body is the ground truth for both interpretation and solving. If a `fun` body and a `decl` with contract share the same name across compilation units (found via `-I`), the tool reports an error — body and contract are mutually exclusive per function name.

**`fun` bodies vs `decl` contracts are mutually exclusive**: a given function name `@f` may be defined by exactly one `fun` (possibly in another `.sir` file found via `-I`) **or** by exactly one `decl` with a contract — never both.

Declarations must appear before any `call` that references them (no forward references to names not yet declared, except that link-form `decl` may reference a `fun` in a yet-to-be-scanned file — the name is available after the `decl` is parsed).

**Two-pass loading**: tools perform an initial scan of the primary file and all `-I` directories to collect every top-level declaration (`fun`, `decl`, `intrinsic`) before any body-checking, call-graph construction, or symbolic execution begins. This pre-scan is parse-only (signatures and contracts, no body checking) and is what makes link-form `decl` resolution and recursion detection possible. For large `-I` trees the cost is linear in the number of `.sir` files visible.

### 3.4 Contract semantics **[New in v0.2.2]**

- **`pre <cond>`**: a **precondition**. Evaluated in the caller's context at the call site with parameters bound to argument values. If any `pre` clause evaluates to `false`, the call is UB (path infeasible). `ret` must **not** appear in `pre` clauses.
- **`post <cond>`**: a **postcondition**. After the call returns, `<cond>` is assumed true. It may reference:
  - The function's parameters (which are immutable — their values equal the arguments passed).
  - The special identifier **`ret`**, representing the return value. `ret` has the return type of the `decl`.
  - Caller-side locals in scope at the call site.
- All pointer parameters are **modifiable** — the callee may write through any pointer parameter. A contract makes no implicit guarantee about what is or is not written. If the caller needs a specific post-state, it expresses it in a `post` clause or via caller-side temporaries (see below).
- A contract must contain at least one `post` clause. `pre` clauses are optional; a contract with no `pre` clauses imposes no preconditions.

**No `old()`**: to express a relationship between pre- and post-call state, the caller saves the pre-call value in a temporary and compares explicitly (§10.4).

**Contract-only `decl`**: a `decl` with a contract has no body anywhere. The interpreter rejects `call` on such a `decl` with an error — it has no concrete semantics to execute. The solver uses the contract as its behavioral model.

### 3.5 Declarations

#### 3.5.1 Symbols (`sym` implies immutable `let`)
A `sym` declaration introduces a solver-chosen unknown. Symbols are **immutable**.

```ebnf
SymDecl     := "sym" SymId ":" SymKind Type Domain? ";" ;
SymKind     := "value" | "coef" | "index" ;
Domain      := "in" Interval | "in" Set ;
Interval    := "[" IntLit "," IntLit "]" ;
Set         := "{" IntLit ("," IntLit)* "}" ;
```

**Restrictions:** `sym` of pointer type is not allowed. `sym` of vector type is allowed (per-lane independent symbols).

#### 3.5.2 Locals (`let` and `let mut`)
```ebnf
LetDecl     := "let" ("mut")? LocalId ":" Type ("=" InitVal)? ";" ;
InitVal     := ScalarInit | "null" | "undef" | BraceInit | AtomInit ;
ScalarInit  := IntLit | FloatLit | SymId | LocalId ;
BraceInit   := "{" InitVal ("," InitVal)* "}" ;
```

**[New in v0.2.2]** `AtomInit` extends the v0.2.1 atom-form initializer to include `call`:

```text
let %y: i32 = call @helper(%x);
```

`call` may appear as an initializer for any non-aggregate target. The referenced function must be declared earlier in the source.

Other initialization rules (broadcast, brace, `null`, `undef`) are unchanged from v0.2.1.

**`addr` restriction**: only a `let mut` local (or a sub-lvalue rooted at a `let mut` local) may appear as the operand of `addr`. Parameters and `let` (immutable) locals cannot have their address taken.


## 4. CFG blocks and instructions

### 4.1 Blocks and terminators
```ebnf
Block       := BlockLabel ":" Instr* Terminator ;
BlockLabel  := "^" Ident ;

Terminator  := BrTerm | RetTerm | UnreachTerm ;

BrTerm      := "br" (Cond "," BlockLabel "," BlockLabel | BlockLabel) ";" ;
RetTerm     := "ret" Expr? ";" ;
UnreachTerm := "unreachable" ";" ;
```

### 4.2 Instructions
```ebnf
Instr       := AssignInstr | AssumeInstr | RequireInstr | StoreInstr ;

AssignInstr := LValue "=" Expr ";" ;
AssumeInstr := "assume" Cond ";" ;
RequireInstr:= "require" Cond ("," StringLit)? ";" ;
StoreInstr  := "store" RValue "," Expr ";" ;
```

**`store`**: `store <ptr>, <val>` writes `val` to the memory location addressed by `ptr`. `store` is a **statement**; it does not produce a value.


## 5. LValues, conditions, expressions

### 5.1 LValues and access paths
```ebnf
LValue      := Base Access* ;
Base        := LocalId ;
Access      := "[" Index "]" | "." Ident ;
Index       := IntLit | LocalId | SymId ;
```

### 5.2 Conditions
```ebnf
Cond        := Expr RelOp Expr ;
RelOp       := "==" | "!=" | "<" | "<=" | ">" | ">=" ;
```

### 5.3 Expressions (no parentheses, left-to-right) **[Revised in v0.2.2]**
```ebnf
Expr        := Atom (("+" | "-") Atom)* ;

Atom        := Coef "*" RValue
            | Coef "/" RValue
            | Coef "%" RValue
            | Coef "&" RValue
            | Coef "|" RValue
            | Coef "^" RValue
            | Coef "<<" RValue
            | Coef ">>" RValue
            | Coef ">>>" RValue
            | "~" RValue
            | Select
            | Cast
            | AddrOf
            | Load
            | Cmp
            | PtrIndex
            | PtrField
            | Call                         (* [New in v0.2.2] *)
            | Coef
            | RValue ;

Select      := "select" ( Cond | Expr ) "," SelectVal "," SelectVal ;
SelectVal   := RValue | Coef ;

Cast        := RValue "as" Type ;

AddrOf      := "addr" LValue ;
Load        := "load" RValue ;

Cmp         := "cmp" RelOp SelectVal "," SelectVal ;
PtrIndex    := "ptrindex" RValue "," Index ;
PtrField    := "ptrfield" RValue "," Ident ;

Call        := "call" GlobalId "(" ArgList ")" ;      (* [New in v0.2.2] *)
ArgList     := Expr ("," Expr)* | ε ;

Coef        := IntLit | FloatLit | LocalId | SymId | "null" ;
RValue      := LValue ;
```

**`call` [New in v0.2.2]**: invokes the function named by `GlobalId`. Arguments are evaluated left-to-right. The result type is the return type of the called function. `call` is an `Atom` and may appear wherever an `Atom` is expected. Zero-argument calls use empty parentheses: `call @get_value()`.

### 5.4 `ret` in contract expressions **[New in v0.2.2]**

Inside `post` clauses of a contract-form `decl`, the identifier `ret` is a reserved name referring to the function's return value. It has the return type declared in the `decl` header. It behaves like an immutable local in scope only within `post` clauses of that specific `decl`.

`ret` is **not** a keyword outside `post` clauses. A local named `%ret` is a distinct identifier (different sigil) and is legal anywhere.

**Parser disambiguation**: the same token `ret` appears as (a) a terminator (`ret Expr?;` in §4.1) and (b) an expression identifier inside `post` clauses. These are unambiguous by context: terminators occur only at block-end positions; the `ret` identifier occurs only inside `post`-clause expressions. The parser distinguishes by the syntactic context in which the token is encountered.


## 6. Typing and well-formedness (v0.2.2)

### 6.1 Scalar arithmetic restriction
Arithmetic is defined over **scalar integer and floating-point leaves**. Pointers may only appear in arithmetic under §2.10.

### 6.2 LValue typing
- If `%x : T` then `%x` has type `T`.
- If `lv : [N] U` then `lv[i] : U`.
- If `lv : S` and struct `S` has field `f : U` then `lv.f : U`.
- If `lv : <N> T` then `lv[i] : T` (vector lane access).

### 6.3 `select` typing
`select c, a, b` is well-typed iff `a` and `b` have the same type and `c` is a boolean condition or `<N> i1` mask. Result has that type.

### 6.4 `as` typing
`rval as T` is well-typed iff both are scalar. Pointer/integer casts are not supported.

### 6.5 Bitwise and shift typing
Both operands must be scalar integers of the same bit-width. Pointers not valid.

### 6.6 Mutability rules
- LHS of `=` must be an lvalue rooted at a `let mut` local.
- `sym` identifiers, `let` (immutable) locals, and parameters are immutable.

### 6.7 Floating-point arithmetic typing
- All atoms in the same `+`/`-` chain must have the exact same floating-point type.
- Mixed arithmetic between different float widths or integers/floats is forbidden without explicit `as` casts.

### 6.8 Pointer typing rules
Pointer typing rules (v0.2.1) carry forward. See the v0.2.1 specification for full details on `addr`, `load`, `store`, `null`, pointer arithmetic,-pointer comparison, `ptrindex`, `ptrfield`, and their type rules.

### 6.9 Vector typing rules
Vector typing rules (v0.2.1) carry forward. Vectors are pure value types, not addressable. See v0.2.1 for `cmp`, lane access, whole-vector copy, and mask-based `select` typing.

### 6.10 Call typing **[New in v0.2.2]**

`call @f(e0, ..., eN-1)` is well-typed iff:

- `@f` is declared (via `fun`, `decl`, or `intrinsic`) earlier in the source.
- The number of arguments equals the number of parameters of `@f`.
- For each `i`, the type of `e_i` equals the type of parameter `i` of `@f` (exact match; no implicit conversions).
- If `@f` is a contract-form `decl`, the call target must already be declared. Recursion (direct or mutual) through `fun` or `decl` is forbidden and detected statically.

Result type: the return type of `@f`.

### 6.11 Contract well-formedness **[New in v0.2.2]**

A contract block `{ pre... post... }` on a `decl` is well-formed iff:

- Every `pre` clause is a well-typed `Cond` whose free variables are a subset of the function's parameters. `ret` must not appear.
- Every `post` clause is a well-typed `Cond` whose free variables are a subset of the function's parameters ∪ `{ret}`.
- `ret` has the return type declared in the `decl` header.
- At least one `post` clause is present.

### 6.12 Literal typing and inference
Literals are inferred from context. Integer default: `i32`. Float default: `f32`. `null` has no default; context must be available.


## 7. Strict UB rules (v0.2.2)

UB is checked during symbolic execution along the chosen path. Any UB makes the path infeasible.

**Static vs runtime.** Rules 1–22 are inherited from v0.2.1. Rules 23–24 are new in v0.2.2. Conditions caught at check time (undeclared call target, argument-parameter count/type mismatch, recursion cycles, contract-form `decl` call in the interpreter) are semantic errors, not runtime UB — they are rejected before execution begins.

### 7.1 Scalar UB

1. **Integer division/modulo by zero**: `a / b` or `a % b` where `b == 0` (both integer-typed) is UB.

2. **Out-of-bounds array access**: `a[i]` where `a : [N] T` and `i < 0` or `i >= N` is UB.

3. **Reading `undef`**: reading any leaf whose stored value is `undef` is UB. This covers uninitialised locals, uninitialised pointer values, and uninitialised vector lanes.

4. **Signed integer overflow**: `+`, `-`, `*`, `<<` that produce a value outside the representable range of the target bit-width. Also: `INT_MIN / -1`. For `<<` specifically: UB if the result `x * 2^n` is not representable in `width(x)` signed bits, OR if `x < 0`. SymIR treats `<<` as signed integer arithmetic (aligning with `+`/`-`/`*`), not as a bit-vector shift — this keeps the overflow story consistent across all integer arithmetic operators.

5. **Overshift**: in `x << n`, `x >> n`, `x >>> n`, UB if `n < 0` or `n >= width(x)`. Separate from rule 4 — overshift is about the shift *amount*, not the shifted result.

### 7.2 `select` and strict UB (lazy)
For `select c, a, b`:
- Evaluate `c` first; UB in `c` makes the path infeasible.
- If `c` is true, evaluate only `a`; UB in `a` makes the path infeasible. `b` is not evaluated.
- If `c` is false, evaluate only `b`; UB in `b` makes the path infeasible. `a` is not evaluated.

Same for mask-based `select`: only lanes selected by the mask are evaluated per-lane.

### 7.3 Floating-point arithmetic semantics
See §2.9 for the full floating-point value model.

### 7.4 Floating-point UB

6. **FP overflow (±∞ result)**: any arithmetic operation (`+`, `-`, `*`, `/`) whose IEEE 754 result (under RNE) would be ±∞ is UB. This covers finite-operand overflow and division of any non-zero value by ±0.0.

7. **FP invalid operation (NaN result)**: any operation whose IEEE 754 result would be NaN is UB. This covers `±0.0 / ±0.0` and `x % ±0.0` for any `x`.

8. **Float-to-integer out-of-range**: `fN as iM` is UB if the float value, after truncation toward zero, is outside the representable range of `iM`.

### 7.5 Pointer UB

9. **Null pointer dereference**: `load <ptr>` or `store <ptr>, <val>` where `ptr` evaluates to `null` (address 0) is UB.

10. **Out-of-bounds pointer arithmetic**: every pointer carries a *provenance object* (the aggregate or scalar storage it was derived from — see rule 15 for how provenance is assigned). For `%p ± n`:
    - Let `base = addr(provenance)` and `size = sizeof(provenance)`.
    - UB if the resulting address falls outside `[base, base + size]`.
    - The "one-past-the-end" address `base + size` is a valid non-dereferenceable address (valid for arithmetic and equality; UB to `load`/`store`).

11. **Out-of-bounds load/store**: `load <ptr>` or `store <ptr>, <val>` where `ptr` points outside the bounds of the originating object (including the one-past-the-end address) is UB.

12. **Cross-object pointer arithmetic**: forming a pointer by arithmetic that crosses from one local variable's storage into another is UB. The memory regions of distinct local variables do not overlap.

13. **Uninitialized pointer dereference**: `load <ptr>` or `store <ptr>, <val>` where `ptr` itself is `undef` is UB (follows from rule 3 applied to the pointer value, kept as a separate rule for clarity).

14. **Cross-object pointer comparison**: comparing two pointers derived from different originating objects with `<`, `<=`, `>`, `>=` is UB. Equality (`==`, `!=`) between pointers of different objects is well-defined and always produces `false` / `true` respectively (distinct objects occupy disjoint address ranges per the non-overlap axioms).

15. **Aggregate-derived pointer provenance [revised in v0.2.1]**: every pointer derivation carries a *provenance object*:
    - **Top-level `addr %lv`** where `%lv : T` is a `let mut` local: provenance = `%lv` (the whole local).
    - **Field access — `addr lv.f` or `ptrfield <ptr>, f`**: provenance = the *immediate containing struct* of `f`.
    - **Index access — `addr lv[i]` or `ptrindex <ptr>, i`**: provenance = the *immediate containing array*.
    - **Pointer arithmetic — `<ptr> ± n`**: provenance is unchanged from `<ptr>`.

    Pointer arithmetic that walks outside the provenance object's storage range is UB (rule 10). One-past-the-end is valid for arithmetic and equality only.

15b. **Typed-access mismatch [v0.2.1]**: `load %p` or `store %p, v` through `%p : ptr T` is UB if the runtime address does **not** coincide with the start of a `T`-typed cell within the provenance object:
    - For an array `[N] U`: every offset `k * sizeof(U)` for `0 ≤ k < N` is a valid `U` cell.
    - For a struct `@S`: only the offsets of fields with declared type `T` are valid `T` cells. A `ptr i32` landing on the offset of an `i64` field is UB, even if arithmetic stayed within `@S`'s bounds.
    - For a top-level scalar local `%x : U`: offset `0` if `U == T`.

    Mid-cell, on a cell of a different type, or straddling cells: all UB. This rule does the real work for structs with mixed field types — rule 15 allows arithmetic within the struct, but the eventual deref must respect the field types.

16. **`ptrindex` out-of-bounds [v0.2.1]**: `ptrindex <ptr>, <index>` where `<ptr> : ptr [N] T` is UB if `index < 0` or `index > N`. Index `N` (one-past-the-end) produces a valid non-dereferenceable address. The provenance of the result is the array `<ptr>` points to (rule 15).

17. **Navigation through `null` [v0.2.1]**: `ptrindex <ptr>, <i>` or `ptrfield <ptr>, <f>` is UB if `<ptr>` evaluates to `null`. Declaring UB at the navigation site prunes the path immediately instead of waiting for a later `load`/`store`.

18. **Navigation through `undef` [v0.2.1]**: `ptrindex` and `ptrfield` count as reads of their pointer operand. A `<ptr>` operand whose value is `undef` is UB at the navigation site (consequence of rule 3, made explicit).

19. **Navigation from a one-past-the-end pointer [v0.2.1]**: `ptrindex <ptr>, <i>` or `ptrfield <ptr>, <f>` is UB if `<ptr>` is exactly the one-past-the-end address of its provenance object. That address is valid for arithmetic and equality (rule 10) but does not point to any element to navigate into.

### 7.6 Vector UB [v0.2.1]

20. **Out-of-bounds vector lane access**: a lane read `lv[i]` or lane write `lv[i] = …` where `lv : <N> T` is UB if `i < 0` or `i >= N`.

21. **Lane-wise scalar UB**: all scalar UB rules (1–8) apply **per-lane** to vector operations. If any single lane would trigger UB in a vector operation, the entire path is infeasible. For example:
    - `%a / %b` where `%a, %b : <4> i32` is UB if any lane of `%b` is `0`.
    - `%a + %b` where `%a, %b : <4> i32` is UB if any lane overflows.

22. **Reading `undef` vector lane**: reading a lane whose value is `undef` (from a vector initialized with `undef`, or from a vector where the read lane has not yet been written by lane-write or whole-vector copy) is UB. This is the per-lane extension of rule 3.

### 7.7 Function call UB **[New in v0.2.2]**

23. **Contract precondition violation**: `call @f(...)` where `@f` is a contract-form `decl`, and any `pre` clause evaluates to `false` at the call site → path infeasible. Analogous to `assume` on the caller side: the caller must ensure preconditions hold on the chosen path, and if they don't, the path is pruned.

24. **Callee UB propagation**: UB encountered during execution of a `fun` callee makes the **caller's** path infeasible. UB is not a sandbox — it propagates across call boundaries. If any statement, condition, or nested `call` inside the callee triggers UB, the calling path is pruned.

25. **Intrinsic result-overflow**: any intrinsic whose result is not representable in its declared return type is UB. This applies uniformly to all intrinsics (e.g., `@abs(INT_MIN)`), consistent with rule 4. Per-intrinsic UB-preconditions (e.g., `@ctz`/`@clz` require non-zero input) are listed in §12.

**Not UB (static checks).** The following are caught before execution and are **not** runtime UB:
- Call to an undeclared function (semantic error at check time).
- Argument-parameter count or type mismatch (type error at check time).
- Recursion cycle in the call graph (static error at check time, §9.7).
- Contract-form `decl` call in the interpreter (rejected before execution begins; the solver processes such calls via contract expansion, §9.6.2).


## 8. Division and modulo (round toward 0)

SymIR uses **truncation toward zero** for both integer and floating-point `%`. The result sign matches the dividend sign.

### 8.1 Integer division and modulo
`Q = trunc(A / B)`, `R = A - Q*B`. `|R| < |B|`, sign of `R` matches `A`.

### 8.2 Floating-point modulo (`fmod` semantics)
C's `fmod`, **not** IEEE `remainder`. SMT encoding: `fp.sub(A, fp.mul(fp.roundToIntegral[RTZ](fp.div[RNE](A, B)), B))`.


## 9. Path-based symbolic execution and constraint extraction

### 9.1 Symbolic state
The executor maintains:
- `Store`: mapping from local lvalues to symbolic terms.
- `Mem[T]`: typed memory arrays, one per pointee type `T`.
- `PC`: feasibility constraints (conjunction).
- `REQ`: property constraints (conjunction).

### 9.2 Constraint sources (inherited)
- **Branches**: `br cond, ^t, ^f;` conjoins `cond` or `not(cond)` to `PC`.
- **Assumptions**: `assume c;` → `PC`.
- **Requirements**: `require c;` → `REQ`.
- **Strict UB**: any UB → `PC := false`.
- **`store`**: `Mem[T] := store(Mem[T], addr, val)`.

### 9.3 Solve goal
`DOM ∧ PC ∧ REQ` sent to the SMT solver.

### 9.4 SMT encoding of pointers (inherited)
Abstract address constants, non-overlap axioms, typed `Mem[T]` arrays. Full details in the v0.2.1 specification §9.4.

### 9.5 SMT encoding of vectors (inherited)
Per-lane symbolic terms, lane-wise SMT operations, `cmp` as per-lane boolean tuples. Full details in v0.2.1.

### 9.6 SMT encoding of function calls **[New in v0.2.2]**

#### 9.6.1 `call @f` where `@f` is a `fun` (defined body)

1. **Argument evaluation**: each argument is evaluated left-to-right in the caller's context. When multiple `call` atoms appear in the same expression (e.g., `call @f() + call @g()`), the leftmost call's side effects (on `Mem`, `PC`, `REQ`) commit before the next call's arguments are evaluated.
2. **Callee context**: a fresh context with parameters bound to argument values. **Symbol declarations are introduced once per program** — the first time a callee is symbolically entered, its `sym` declarations create symbolic constants that are then reused across all subsequent calls to the same `fun` on the path. (See "shared symbols" note below.) Locals get fresh storage on each call. `Mem[T]` and `PC`/`REQ` are carried forward.
3. **Callee execution**: blocks are symbolically executed along the sub-path through the callee. The caller's path `π` specifies which callee blocks are visited per call site.
4. **Return**: the return value is the symbolic term of the `ret` expression. `PC` and `REQ` reflect callee constraints. `Mem[T]` reflects callee `store`s. Callee-local `Store` is discarded. If callee `PC` becomes `false`, caller `PC` also becomes `false`.
5. **Caller `Store` coherence**: for any caller-side `let mut %x` whose address was passed (directly or transitively through `addr`, `ptrindex`, `ptrfield`) into the callee, the caller's cached `Store[%x]` is invalidated. Subsequent reads of `%x` re-fetch from `Mem[T]`. This keeps the local store consistent with possibly-mutated memory.
6. **Result**: the `call` atom evaluates to the return value.

**Shared symbols (v0.2.2).** A `sym` declared inside a `fun` body denotes a single solver-chosen value, even if the `fun` is called multiple times on the path. Rationale: fewer SMT variables → faster solving; semantically natural ("one hole, one value"). *Planned for a future version:* per-call-site fresh symbol instantiation, allowing the same `fun` to expose independent unknowns at each call site.

#### 9.6.2 `call @f` where `@f` is a contract-form `decl`

1. **Precondition check**: each `pre` clause is evaluated in the caller's context with parameters bound to arguments. Any `pre` evaluating to `false` → `PC := false` (UB per rule 23).
2. **Return value**: a **fresh symbolic constant** `ret_sym : RetType` is introduced for the return value.
3. **Postcondition assumption**: each `post` clause, with parameters bound to arguments and `ret` bound to `ret_sym`, is conjoined to `PC` as an assumption.
4. **Memory havoc**: all pointer parameters are potentially modifiable. For each pointer parameter `%p`, **every storage cell within `%p`'s provenance object is havoc'd** — replaced by fresh symbolic values in the appropriate `Mem[T_i]` arrays:
   - If `%p : ptr T` with provenance a scalar local `%x : T`: the single cell `Mem[T][addr(%x)]` is havoc'd.
   - If provenance is an array `[N] U`: all `N` cells `Mem[U][addr+k*sizeof(U)]` for `k ∈ [0, N)` are havoc'd.
   - If provenance is a struct `@S`: every field cell is havoc'd in its respective `Mem[FieldType]`.
   - If any cell in the provenance is itself a pointer (`ptr V`), it is havoc'd in `Mem[ptr V]`; transitively reachable storage **is not** further havoc'd — the contract is responsible for constraining nested-pointer behavior explicitly via `post` clauses.
   - The havoc'd values are constrained only by `post` clauses that reference them.
5. **Caller `Store` coherence**: as in §9.6.1 step 5 — any caller-side `let mut` local whose address was reachable from an argument has its cached `Store` invalidated.
6. **Result**: the `call` atom evaluates to `ret_sym`.

#### 9.6.3 `call @f` where `@f` is an `intrinsic`

The solver applies the intrinsic's hard-coded SMT encoding. See §12 for per-intrinsic encodings.

#### 9.6.4 Path specification for interprocedural execution

The user-chosen path `π` is extended to a **tree** of block visits:

```
@outer: ^entry -> ^b1 -> [call @inner: ^entry -> ^body -> ^exit] -> ^b2 -> ^exit
```

`[call @name: ...]` denotes the sub-path through the callee at that call site. Each call site on `π` may specify a different sub-path.

### 9.7 No recursion **[New in v0.2.2]**

Recursion is **not supported** in v0.2.2:

- A `fun` body may not contain a `call` to itself (direct recursion).
- A `fun` may not participate in a cycle of calls — if `@a` calls `@b` and `@b` calls `@a`, that is mutual recursion and is forbidden.
- A contract-form `decl` may not contain a `call` to itself.

These conditions are detected statically by constructing the call graph from all `fun` and `decl` declarations visible in the program (including those resolved via `-I`). Any cycle in the call graph is reported as an error before execution begins.

This is a deliberate simplification: recursion introduces fixed-depth unrolling heuristics, complicates the SMT encoding with nested contexts, and provides limited value for the synthesis use cases v0.2.2 targets. Loops within a single `fun` (via CFG back-edges) remain the primary iteration mechanism.


## 10. Examples

### 10.1 Simple function call

```text
fun @add_one(%x: i32) : i32 {
  let %one: i32 = 1;
^entry:
  ret %x + %one;
}

fun @use_add(%a: i32) : i32 {
  let mut %y: i32 = 0;
^entry:
  %y = call @add_one(%a);
  ret %y;
}
```

### 10.2 Link-form `decl` with `-I` resolution

File `lib.sir`:
```text
fun @sort3(%a: ptr i32, %b: ptr i32, %c: ptr i32) : i32 {
  ...
}
```

File `main.sir`:
```text
decl @sort3(%a: ptr i32, %b: ptr i32, %c: ptr i32) : i32;

fun @median(%x: ptr i32, %y: ptr i32, %z: ptr i32) : i32 {
  let mut %result: i32 = 0;
^entry:
  %result = call @sort3(%x, %y, %z);
  ret %result;
}
```

The interpreter resolves `decl @sort3` by scanning `-I` paths for a file containing `fun @sort3`. The solver inlines the body from the discovered file.

### 10.3 Contract-form `decl` (abstract model)

```text
decl @alloc(%size: i32) : ptr i32 {
  pre %size > 0, "size must be positive";
  post ret != null, "allocation never returns null";
};

fun @make_buffer(%n: i32) : ptr i32 {
  let mut %p: ptr i32 = null;
^entry:
  %p = call @alloc(%n);
  require %p != null, "buffer allocated";
  ret %p;
}
```

The solver checks `%size > 0` as a precondition and assumes `ret != null` after the call. The interpreter rejects this program — `@alloc` has no body.

### 10.4 Contract with caller-side pre/post comparison

```text
decl @increment(%p: ptr i32) : i32 {
  pre %p != null, "non-null pointer";
  post ret == load %p, "returns the new value";
};

fun @call_increment(%p: ptr i32) : i32 {
  let mut %before: i32 = 0;
  let mut %after: i32 = 0;
^entry:
  %before = load %p;
  %after = call @increment(%p);
  require %after == %before + 1, "incremented by 1";
  ret %after;
}
```

The caller saves `%before` to relate pre- and post-state. The contract's `post ret == load %p` constrains the return value in terms of the post-call memory. Since all pointer parameters are potentially modifiable, the solver knows `Mem[i32]` at `%p` may have changed.

### 10.5 Call chain mixing `fun`, `decl`, and `intrinsic`

```text
intrinsic @abs(%x: i32) : i32;

decl @validate(%data: ptr i32, %len: i32) : i1 {
  pre %len > 0, "non-empty buffer";
  pre %data != null, "non-null buffer";
  post ret == 0 || ret == 1, "boolean result";
};

fun @validated_abs(%data: ptr i32, %len: i32) : i32 {
  let mut %ok: i1 = 0;
  let mut %val: i32 = 0;
  let mut %result: i32 = 0;
  let %one: i1 = 1;
  let %zero: i32 = 0;
^entry:
  %ok = call @validate(%data, %len);
  br %ok == %one, ^do_abs, ^skip;

^do_abs:
  %val = load %data;
  %result = call @abs(%val);
  ret %result;

^skip:
  ret %zero;
}
```

On path `^entry -> ^do_abs -> (return)`: the solver expands `@validate`'s contract (`pre` checked, `ret ∈ {0,1}` assumed), the branch constrains `%ok == 1`, then `@abs` uses its built-in BV-theory encoding.

### 10.6 Synthesis with a contract

```text
decl @lookup(%key: i32) : i32 {
  pre %key >= 0, "non-negative key";
  post ret >= 0, "non-negative result";
};

fun @find_key(%target: i32) : i32 {
  sym %?k: value i32 in [0, 100];
  let mut %val: i32 = 0;
^entry:
  %val = call @lookup(%?k);
  require %val == %target, "found the target value";
  ret %?k;
}
```

The solver picks `%?k ∈ [0, 100]` and a symbolic return value `ret_sym` (constrained by `ret_sym >= 0`) such that `ret_sym == %target`. If `%target >= 0`, a solution exists.

### 10.7 Call graph across compilation units

With `-I lib/`, a link-form `decl` in `main.sir` resolves to `fun @helper` in `lib/util.sir`:

`lib/util.sir`:
```text
fun @helper(%x: i32) : i32 {
  let %two: i32 = 2;
^entry:
  ret %x * %two;
}
```

`main.sir`:
```text
decl @helper(%x: i32) : i32;

fun @main(%a: i32) : i32 {
  let mut %result: i32 = 0;
^entry:
  %result = call @helper(%a);
  ret %result;
}
```

The call graph is `@main → @helper` (acyclic). This is valid. If `lib/util.sir` contained `call @main`, the cycle `@main → @helper → @main` would be detected and reported as an error.


## 11. Toolchain updates **[New in v0.2.2]**

### 11.1 `-I` search path (all tools)

The `-I <path>` flag specifies a directory to search for `.sir` source files. Multiple `-I` flags may be specified; directories are searched in order. When a tool encounters a link-form `decl @name`, it scans each `-I` directory for `.sir` files. For each file found, it parses the top-level declarations only (function names + kinds). If exactly one `fun @name` is found, that body is used. If zero are found, the tool reports an error. If multiple are found, the tool reports an ambiguity error.

`-I` is orthogonal to the primary input file — the primary file is always fully processed, and `-I` provides additional resolution context.

### 11.2 `symirc` (compiler)

| Declaration | Lowering |
|---|---|
| `fun @f` | Compile body to target language |
| `decl @f` (link form) | Emit `extern` (C) / `import` (WASM), link at target level |
| `decl @f` (contract form) | Emit `extern` / `import` with contract as structured comment |
| `intrinsic @f` | Emit target-language built-in (§11.4) |
| `call @f` | Lower to direct function call in target language |

### 11.3 `symiri` (interpreter)

| Declaration | Behavior on `call @f` |
|---|---|
| `fun @f` | Interpret callee body in fresh context |
| `decl @f` (link form) | Resolve body via `-I`, interpret it. **Error** if no body found |
| `decl @f` (contract form) | **Error**: no body to execute. Interpreter rejects before starting |
| `intrinsic @f` | Execute built-in interpreter implementation |

### 11.4 `symirsolve` (solver)

| Declaration | Behavior on `call @f` |
|---|---|
| `fun @f` | Inline body; interprocedural symbolic execution (§9.6.1) |
| `decl @f` (link form) | Resolve body via `-I`, inline and symbolically execute |
| `decl @f` (contract form) | Expand contract (§9.6.2): preconditions → UB, postconditions → assumptions |
| `intrinsic @f` | Apply hard-coded SMT encoding (§12) |

### 11.5 Intrinsic lowering in `symirc`

Each intrinsic is declared once for `iN` (any `N ≥ 1`), not per concrete width. The backends use the same widening-and-mask strategy they already apply to all `iN` operations:

- Map `iN` to the smallest machine width `W` that fits: `W = 8` for `N ≤ 8`, `16` for `N ≤ 16`, `32` for `N ≤ 32`, `64` for `N ≤ 64`.
- Widen operands to `W` bits, perform the operation at width `W`, then truncate/mask the result to `N` bits.
- For `@clz`/`@ctz`: subtract `(W − N)` from the result so leading/trailing zeros in the widened operand are not counted.

| Intrinsic | C (generic over `iN`) | WASM (generic over `iN`) |
|---|---|---|
| `@abs` | Widen to `intW_t`. `x < 0 ? -x : x`. Mask to `N` bits. | `iW.abs`. Mask to `N` bits. |
| `@min` | Widen to `intW_t`. `x < y ? x : y`. Mask to `N` bits. | `iW.min_s`. Mask to `N` bits. |
| `@max` | Widen to `intW_t`. `x > y ? x : y`. Mask to `N` bits. | `iW.max_s`. Mask to `N` bits. |
| `@clz` | Widen to `uintW_t`. `__builtin_clz[ll](x) − (W−N)`. Mask to `N` bits. | `iW.clz − (W−N)`. Mask to `N` bits. |
| `@ctz` | Widen to `uintW_t`. `__builtin_ctz[ll](x) − (W−N)`. (Both `@clz` and `@ctz` require `%x != 0` per §12.2 — UB on zero, consistent with GCC builtins.) Mask to `N` bits. | `iW.ctz − (W−N)`. Mask to `N` bits. |
| `@popcount` | Widen to `uintW_t`. `__builtin_popcount[ll](x)`. Mask to `N` bits. | `iW.popcnt`. Mask to `N` bits. |


## 12. Standard intrinsics **[New in v0.2.2]**

Intrinsics are built into the SymIR toolchain. They require no body, no contract, and no `-I` resolution. In this section, `iN` denotes **any concrete integer type** (`i1`, `i8`, `i16`, `i32`, `i64`, or any other `i<Nat>`). The intrinsic is declared once per concrete width the program uses (e.g., `intrinsic @abs(%x: i32) : i32;`), and the toolchain applies the same generic encoding regardless of `N`. Lowering follows the widening-and-mask strategy described in §11.5.

### 12.1 Arithmetic intrinsics

#### `@abs`

```text
intrinsic @abs(%x: iN) : iN;
```

Returns the absolute value of `%x`. **UB if `%x == INT_MIN`** (the result `-INT_MIN` is not representable in `iN` — this is a signed integer overflow, consistent with rule 4 in §7.1). All intrinsics follow this general principle: any result not representable in the declared return type is UB.

**SMT encoding**: `ite(bvsge(x, (_ bv0 N)), x, bvneg(x))`, with a UB-precondition `x != INT_MIN` conjoined to `PC`.
**Interpreter**: assert `x != INT_MIN` (mark path infeasible if violated); otherwise `x < 0 ? -x : x`.

#### `@min`, `@max`

```text
intrinsic @min(%a: iN, %b: iN) : iN;
intrinsic @max(%a: iN, %b: iN) : iN;
```

Signed minimum / maximum.

**SMT encoding**: `ite(bvsle(a, b), a, b)` / `ite(bvsge(a, b), a, b)`
**Interpreter**: `a < b ? a : b` / `a > b ? a : b`

### 12.2 Bit-counting intrinsics

#### `@clz`, `@ctz`

```text
intrinsic @clz(%x: iN) : iN;
intrinsic @ctz(%x: iN) : iN;
```

Count leading / trailing zero bits. Following C/C++ semantics for `__builtin_clz`/`__builtin_ctz`, **`%x == 0` is UB** — the intrinsic implicitly requires `%x != 0`. Callers must ensure non-zero input on the chosen path.

**SMT encoding**: there is no native `bvclz` in all SMT solvers. For concrete `%x`, the tool computes the result directly and substitutes a constant. For symbolic `%x`, the tool introduces a fresh symbolic value constrained by:
- `result ∈ [0, N−1]` (since `x != 0`)
- `x != 0` is a UB-precondition that conjoins to `PC`
- For `clz`: `x[N-1 : N−result] == 0` and `x[N−result−1] == 1`
- For `ctz`: `x[result−1 : 0] == 0` and `x[result] == 1`

These constraints may be added as quantifier-free assertions where the solver supports bit-vector extraction, or the tool may bit-blast.

**Interpreter**: `__builtin_clz(x)` / `__builtin_ctz(x)` (or software fallback for widths not natively supported).

#### `@popcount`

```text
intrinsic @popcount(%x: iN) : iN;
```

Counts the number of 1 bits.

**SMT encoding**: for concrete `%x`, substitute the computed count. For symbolic `%x`, introduce a fresh symbolic value constrained by `result ∈ [0, N]` and `result == Σ bit_i(x)`. May use `bvadd` tree over bit extractions, or leave as an uninterpreted function with range bounds.

**Interpreter**: `__builtin_popcount(x)` (or software fallback for widths not natively supported).

### 12.3 Extensibility

Additional intrinsics may be added in future versions. Each new intrinsic must specify:
- Its declared signature (over `iN`)
- Its SMT encoding (how the solver reasons about it)
- Its interpreter behavior (how `symiri` executes it)
- Its lowering pattern for each target (C, WASM), following the widening-and-mask strategy in §11.5

An intrinsic is accepted only if all four are defined. "Delegate to the target" is not acceptable — SymIR owns the semantics.


## 13. Non-goals for v0.2.2 (planned for later)

- **WASM SIMD support — planned for v0.2.3**. The SIMD-128 proposal and Relaxed SIMD are targets.
- **Addressable vectors — planned for v0.2.3**. Vectors are pure value types: no `ptr <N> T`, no `addr` on vector locals. v0.2.3 will add whole-vector `load`/`store` (LLVM-style) while keeping element pointers out; this requires defining `sizeof(<N> T) = N * sizeof(T)` and adding `<N> T` to the loadable-type set.
- **Vectors in aggregates — planned for v0.2.3**. Vectors cannot appear as struct fields or array elements. Deferred together with addressable vectors above.
- **Vector shuffles and permutations — planned for v0.2.3**. Instructions like `shuffle`, `swizzle`, lane reordering are deferred. Lane rearrangement can be achieved through sequences of lane reads and writes (`%w[0] = %v[2]; %w[1] = %v[0]; …`).
- **Horizontal vector reductions — planned for v0.2.3**. Intrinsics like `@reduce_add`, `@reduce_max` across lanes are not supported. Use lane subscripts (`%v[0]`, `%v[1]`, …) to access individual lanes and accumulate manually.
- **Callee sub-path syntax** — planned. §9.6.4 promotes the user-chosen path `π` to a tree of block visits, but the surface syntax to specify each callee's sub-path (e.g., a nested `[call @inner: ^entry -> ^body -> ^exit]` form, a per-callee `--call-path @inner=^a,^b` CLI flag, or a JSON object) is deferred. The current solver picks one random path per callee per `solve()` invocation, seeded from `--seed`, with a per-block visit cap to bound loops. A future version will replace the random choice with an exact user-supplied sub-path so synthesis results are fully reproducible across branchy callees.
- **Char and string types** — planned. Add first-class support for `char` and string literals.
- **Recursion**. A `fun` body may not call itself (direct recursion) or participate in a mutual recursion cycle. The call graph must be a DAG. Loops within a single `fun` via CFG back-edges remain the primary iteration mechanism. Recursion introduces fixed-depth unrolling heuristics and complicates the SMT encoding with nested contexts — it provides limited value for the synthesis use cases v0.2.2 targets.
- **Indirect calls / function pointers**. `call` always targets a statically-named `GlobalId`. Function pointer types (`ptr fun(...)`) and indirect calls through computed addresses are not supported. All call targets are resolved by name at parse time.
- **`old()` in contracts**. Post-state only. Pre-state references require caller-side temporaries. Adding `old()` introduces a two-state logic into the SMT encoding, which is not yet justified.
- **Contracts on `fun` bodies**. A `fun` never has a contract — the body is the ground truth. Modular verification (prove the body satisfies a contract, then callers use the contract instead of inlining) is deferred to a future version.
- **Mutable pointee annotations (`mut ptr T`)**. All pointer parameters are modifiable — the callee may write through any pointer. Per-parameter mutability annotations with static enforcement (i.e., `store` through a non-`mut` parameter is a compile-time error) is deferred. The uniform "all modifiable" model is simpler and sufficient for v0.2.2 synthesis patterns.
- **Memory intrinsics** (`@memcpy`, `@memset`). Their SMT encoding requires byte-level array reasoning with potentially symbolic sizes. Standard integer intrinsics (§12) are included. Memory intrinsics may arrive in a future version once byte-level memory reasoning is better supported by the solver backend.
- **Contract memory havoc — extended provenance forms.** The solver now havocs the storage backing each pointer parameter at contract-form `decl` call sites (§9.6.2 step 4) so post-state pointee constraints are sound. The current implementation resolves two argument-expression forms — direct `addr %x` and a plain ptr local `%p` with a known provenance — by replacing the source local's symbolic value with a fresh constant. Aggregate provenance (havocing every cell of a `[N] T` or every field of an `@S`), pointer arguments derived from `ptrindex`/`ptrfield`/pointer arithmetic, and transitive nested-pointer cells are not yet havoc'd; callers passing those forms today should constrain the post-state via additional contract clauses or caller-side asserts until the next refinement lands.
- **Multiple return values**. Functions return exactly one typed value. Multi-result patterns can be expressed through pointer out-parameters.
- **`sym` of pointer type**. Pointer symbols require a richer address domain theory.
- **Heap allocation** (`malloc`/`free` or arena allocation). Pointers are stack-only.
- **Pointer/integer casts** (`ptr T as iN`, `iN as ptr T`).
- **Aliasing between distinct locals** (deliberately UB; explicit alias modeling deferred).
- **Parentheses and general expression trees**.
