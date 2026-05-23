# SymIR v0.2.1 Specification

**Status:** Draft v0.2.1

This document is the complete, standalone specification for SymIR v0.2.1. It supersedes v0.2.0. Sections and rules that are unchanged from v0.2.0 are included verbatim for self-containedness. All additions and changes are marked **[New in v0.2.1]**.


## What's new in v0.2.1

### Aggregate pointers

- **`ptr [N] T`**: pointer to an array type. `ptrindex` navigates to an element pointer.
- **`ptr @S`**: pointer to a struct type. `ptrfield` navigates to a field pointer.
- **`ptrindex` expression**: navigates from `ptr [N] T` to `ptr T` at a given index.
- **`ptrfield` expression**: navigates from `ptr @S` to `ptr FieldType` at a named field.
- **`addr` on aggregates**: `addr %arr` yields `ptr [N] T`; `addr %s` yields `ptr @S`.
- **Packed struct layout**: `sizeof(@S) = Σ sizeof(field_i)`. No padding.
- **No aggregate `load`/`store`**: `load`/`store` through `ptr [N] T` or `ptr @S` is a type error. Navigate to scalar/pointer elements first.

### Vector types (SIMD)

- **Vector type `<N> T`**: fixed-width SIMD vector of `N` scalar elements.
- **Lane-wise arithmetic**: all scalar operators (`+`, `-`, `*`, `/`, `%`, bitwise, `~`) lift to lane-wise vector operations.
- **`cmp` expression**: reifies a comparison as a value. Produces `<N> i1` (vector mask) or `i1` (scalar boolean). `cmp` exists alongside `Cond` because `Cond` is a path-level predicate (consumed by `br`/`assume`/`require`) and cannot produce a vector mask. Letting `<`/`==`/etc. also be value-producing atoms would force precedence rules, which are an explicit non-goal (§11). `cmp` covers both scalar and vector cases with one keyword.
- **Mask-based `select`**: `select <mask>, <vtrue>, <vfalse>` performs per-lane blend.
- **Lane access via subscript**: `%v[%i]` reads lane `i` of a vector; `%v[%i] = %x;` writes lane `i`. Uniform with array indexing; no new keywords needed.
- **Whole-vector copy**: `%v = %w;` copies all lanes (vectors have a single defined bitwidth — see §6.6). Whole-aggregate copy for `[N] T` / `@S` remains unsupported.
- **Not addressable**: vectors are pure value types. No `ptr <N> T`, no `addr` on vector locals (and no `addr` on a vector lane). (Addressable vectors with whole-vector `load`/`store` are deferred to v0.2.2 — see §11.)
- **`sym` of vector type**: allowed. Each lane is an independent symbolic variable.
- **Deferred to v0.2.2**: whole-vector `load`/`store` and `addr` on vector locals; vectors as struct fields or array elements.

### Other changes

- **`cmp` for scalars**: `cmp` also works on scalar operands, producing `i1` — reified booleans. The same keyword used for vector lane-mask production keeps scalar and vector comparison-as-value syntax uniform.
- **Updated `sizeof`**: includes struct types (packed layout).
- **New UB rules**: `ptrindex` bounds check, per-lane vector UB, vector lane-access bounds.


## 1. Notation and identifier classes

SymIR uses sigils to make identifier categories immediately recognizable:

- `@name` — global identifiers (functions; global type names if desired).
- `%name` — local identifiers (parameters, locals).
- `@?name` — **global symbols** (solver-chosen unknowns).
- `%?name` — **local symbols** (solver-chosen unknowns).
- `^name` — basic block labels.

**Hard rule:** `?` is permitted **only** immediately after `@` or `%` to form `@?` / `%?`. It is forbidden in all other identifiers.


## 2. Key semantic commitments (v0.2.1)

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
- `select <cond>, <vtrue>, <vfalse>`
- `select` is **lazy**: only the selected arm is evaluated.
- `vtrue` and `vfalse` are restricted to **scalar, pointer, or vector values** (`RValue`, constant `Coef`, or `null`) to avoid nested expressions. Both arms must have the same type (see §6.3 for pointer and `null` arm typing).

**[New in v0.2.1] Mask-based select**: `select <mask>, <vtrue>, <vfalse>` where `<mask>` is an expression of type `<N> i1` (vector mask) or `i1` (scalar boolean). The mask determines per-lane selection: for each lane `k`, if `mask[k]` is `1` then `vtrue[k]` is selected, otherwise `vfalse[k]`. See §5.3 for the grammar and §6.10 for typing rules.

### 2.8 Memory model
SymIR uses a typed, stack-only memory model:

- **Stack-only**: all addressable storage is a `let mut` local within the current function. Heap allocation is not supported.
- **No cross-object aliasing**: a pointer derived from `addr %x` can never alias a pointer derived from `addr %y` for distinct locals `%x` and `%y`. Cross-object pointer arithmetic is **UB**.
- **Typed memory regions**: the memory is conceptually partitioned by pointee type. Each distinct pointee type `T` has its own independent memory region. This is sound because pointer/integer casts are forbidden.
- **Pointer width**: all pointers are 64-bit values (BV64 in the SMT model), regardless of pointee type.

**[New in v0.2.1] Vectors are not in memory**: vector-typed values (`<N> T`) are pure register-like value types. They do not participate in the memory model. There is no `ptr <N> T`, and vector locals cannot have their address taken.

### 2.9 Floating-point value model

SymIR uses **finite IEEE 754-2008 semantics** for floating-point:

- **Domain**: the only valid floating-point values are **finite** IEEE 754 values. ±∞ and NaN are **not** SymIR values. Any operation whose IEEE 754 result would be ±∞ or NaN is UB (see §7.4).
- **Signed zeros**: `+0.0` and `-0.0` are distinct bit patterns and both are valid values. They compare equal (`+0.0 == -0.0` is `true`). Negating `+0.0` yields `-0.0` and vice versa. Writing `-0.0` as a literal is valid.
- **Subnormals**: subnormal (denormal) values are regular finite values. There is no flush-to-zero behavior.
- **Rounding mode**: all operations use a single fixed mode — **RNE (Round to Nearest, Ties to Even)**. There are no dynamic rounding modes and no per-operation rounding specifiers.
- **`%` for floats**: the `%` operator is **C's `fmod`** (truncated-quotient remainder), **not** the IEEE 754 `remainder` (`fp.rem`). Defined as `x - trunc(x/y) * y` — the quotient is truncated toward zero, matching integer `%`. They differ when `x/y` is near a half-integer: `fmod(1.5, 1.0) = 0.5`, `remainder(1.5, 1.0) = -0.5`.

  **Design rationale**: `fmod` was chosen over `fp.rem` for semantic consistency — both integer and float `%` truncate toward zero, so the result sign always matches the dividend sign. The trade-off is a more complex SMT encoding (see below).
- **No fast-math**: expressions are evaluated strictly left-to-right (no reassociation). No implicit NaN/infinity assumptions beyond what is stated here. No contraction (`fma`), no approximate functions, no flags of any kind.

SMT encoding: `f32` maps to `(_ FloatingPoint 8 24)` and `f64` maps to `(_ FloatingPoint 11 53)`. All FP operations use `roundNearestTiesToEven` except `%`, which encodes as `fp.sub(x, fp.mul(fp.roundToIntegral[RTZ](fp.div[RNE](x, y)), y))` — the `fp.roundToIntegral[RTZ]` truncates the quotient toward zero before multiplying back.

### 2.10 Pointer arithmetic
Pointer arithmetic is typed and stride-aware:

- `ptr T + n` advances the address by `n * sizeof(T)` bytes, where `n` is an integer operand.
- `ptr T - n` retreats the address by `n * sizeof(T)` bytes.
- `ptr T - ptr T` yields the element distance between two pointers of the same type, as `i64`.
- Arithmetic is valid only within the bounds of the originating object. Out-of-bounds arithmetic is UB.
- `integer + ptr T` (with the integer on the left) is **not supported**. The pointer must be the left operand.

**[New in v0.2.1]** The pointee `T` may now be any type (including `[N] U`, `@S`). The stride is always `sizeof(T)`:

| Pointer type | `+ 1` stride | Example use |
|-------------|-------------|-------------|
| `ptr i32` | 4 bytes | Element in `[N] i32` |
| `ptr @S` | `sizeof(@S)` (packed) | Element in `[N] @S` |
| `ptr [4] i32` | 16 bytes | Element in `[N][4] i32` |

To access an element **within** a `[N] T` through a `ptr [N] T`, use `ptrindex` (§5.3) instead of `+`.

**`sizeof`** is defined as follows (used implicitly by pointer arithmetic and explicitly in UB bounds checks and non-overlap axioms):

| Type | `sizeof` |
|------|---------|
| `iN` | `⌈N / 8⌉` bytes (e.g., `i32` → 4, `i64` → 8, `i1` → 1) |
| `f32` | 4 bytes |
| `f64` | 8 bytes |
| `ptr T` | 8 bytes (64-bit address) |
| `[N] T` | `N * sizeof(T)` bytes |
| `@S { f₁: T₁; …; fₖ: Tₖ; }` | `Σ sizeof(Tᵢ)` bytes (packed, no padding; computed recursively when any `Tᵢ` is itself a struct or array) **[New in v0.2.1]** |

Vectors (`<N> T`) do not have a `sizeof` because they are not addressable and do not participate in the memory model. v0.2.2 plans to make vectors addressable, at which point `sizeof(<N> T)` will be defined as `N * sizeof(T)` (packed); see §11.

### 2.11 Vector value model **[New in v0.2.1]**

SymIR v0.2.1 introduces fixed-width SIMD vector types:

- **`<N> T`** where `T` is a scalar type (`iN`, `f32`, `f64`) and `N ≥ 2`.
- Vectors are **pure value types**: they model SIMD registers, not memory locations.
- A vector holds `N` independent **lanes**, each of type `T`.
- All scalar arithmetic and bitwise operators apply **lane-wise** to vectors of matching type and width.
- Comparisons between vectors produce **mask vectors** (`<N> i1`), enabling conditional per-lane operations.
- Vectors are **not addressable**: no `ptr <N> T`, no `addr` on vector-typed lvalues, and no `addr` of a vector lane (`addr %v[%i]` is also forbidden — a lane has no independent address).
- Vectors **cannot be nested** inside other types: no `[M] <N> T` (array of vectors), no `<N> T` as struct fields, no `ptr <N> T`.

**Design rationale**: vectors are kept separate from the memory model for simplicity. In SIMD programming, vector operations are register-to-register; memory interaction happens at the scalar/array level. If vector data must be stored, use arrays and copy lane-by-lane via subscript access (`%arr[k] = %v[k]`).


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

**[New in v0.2.1]** New keywords: `ptrindex`, `ptrfield`, `cmp`. (Lane access uses subscript syntax `%v[%i]`, not a keyword.)

**Reserved keywords (full v0.2.1 list).** The lexer rejects any of these as an `Ident`:

```
addr      and       as        assume    br
cmp       coef      fun       i1...iN   f32
f64       in        index     let       load
mod       mut       null      or        ptr
ptrfield  ptrindex  require   ret       select
store     struct    sym       undef     unreachable
value
```

(`iN` denotes any integer-width keyword `i1`, `i8`, `i16`, `i32`, `i64`, …; the lexer recognises these by the `i` + digits pattern in §3.1.) Programs from v0.2.0 that used any of the new v0.2.1 keywords as identifiers must be renamed.

*Note: For the typing of literals, see [Section 6.11](#611-literal-typing-and-inference).*

### 3.2 Types

```ebnf
Type        := IntType | FloatType | StructName | ArrayType | PtrType
             | VecType ;                                        (* [New in v0.2.1] *)
IntType     := "i" Nat ;
FloatType   := "f32" | "f64" ;
ArrayType   := "[" Nat "]" Type ;
StructName  := GlobalId ;
PtrType     := "ptr" PointeeType ;
PointeeType := IntType | FloatType | PtrType
             | ArrayType | StructName ;                         (* [New in v0.2.1] *)
VecType     := "<" Nat ">" ScalarType ;                         (* [New in v0.2.1] *)
ScalarType  := IntType | FloatType ;
```

Notes:
- Arrays are fixed-size.
- `Nat` is a literal (no symbolic sizes).
- **Multidimensional arrays** are supported by nesting `ArrayType`.
- **Integer Types:** `i1`, `i8`, `i16`, `i32`, `i64`, and arbitrary `iN` are all valid.
- **Floating-point Types:** `f32` (IEEE 754 single-precision) and `f64` (IEEE 754 double-precision).
- **`ptr T`**: pointer to a type `T`.
  - **[Updated in v0.2.1]**: `T` may now be any scalar, pointer, array, or struct type.
  - Valid: `ptr i32`, `ptr f64`, `ptr ptr i32`, `ptr [4] i32`, `ptr @Point`, `ptr [3] ptr i32`.
  - Invalid: `ptr <4> i32` (no pointer to vector).
- **`<N> T`** **[New in v0.2.1]**: fixed-width SIMD vector.
  - `N` must be ≥ 2.
  - `T` must be a scalar type (`iN`, `f32`, `f64`).
  - `N` is not limited to powers of 2. `<3> f32`, `<7> i16`, `<4> i32` are all valid. Implementations should reject `N > 64` to keep SMT formulas tractable (each lane is encoded as an independent constant; see §9.5).
  - Vectors cannot be nested: `<4> <2> i32`, `<4> ptr i32`, `[3] <4> i32` are all invalid.
  - Vectors cannot appear as struct fields.

### 3.3 Program structure
```ebnf
Program     := (StructDecl | FunDecl)+ ;

StructDecl  := "struct" GlobalId "{" FieldDecl+ "}" ;
FieldDecl   := Ident ":" FieldType ";" ;
FieldType   := IntType | FloatType | StructName | ArrayType | PtrType ;
              (* [New in v0.2.1]: VecType is NOT a valid FieldType *)

FunDecl     := "fun" GlobalId "(" ParamList? ")" ":" Type
               "{" SymDecl* LetDecl* Block+ "}" ;

ParamList   := Param ("," Param)* ;
Param       := LocalId ":" Type ;
```

Parameters may be of type `ptr T` (including aggregate pointers) or `<N> T` (vector). A pointer parameter represents an externally-provided address; the caller is responsible for ensuring it points to valid, appropriately-typed storage. A vector parameter is passed by value.

### 3.4 Declarations

#### 3.4.1 Symbols (`sym` implies immutable `let`)
A `sym` declaration introduces a solver-chosen unknown. Symbols are **immutable** (there is no `sym mut`).

```ebnf
SymDecl     := "sym" SymId ":" SymKind Type Domain? ";" ;
SymKind     := "value" | "coef" | "index" ;
Domain      := "in" Interval | "in" Set ;
Interval    := "[" IntLit "," IntLit "]" ;
Set         := "{" IntLit ("," IntLit)* "}" ;
```

**Restriction:** `sym` declarations of pointer type (`ptr T`) are **not allowed**. Pointer values must be derived from `addr` expressions or received as parameters. Integer symbols (`coef`, `index`) may still be used as operands in pointer arithmetic.

**[New in v0.2.1]** `sym` declarations of vector type (`<N> T`) **are allowed**. Each lane is an independent symbolic variable. Domain constraints apply **per-lane**: all `N` lanes share the same domain specification (e.g., `sym %?v: value <4> i32 in [0, 10];` means each of the 4 lanes is independently in `[0, 10]`).

#### 3.4.2 Locals (`let` and `let mut`)
```ebnf
LetDecl     := "let" ("mut")? LocalId ":" Type ("=" InitVal)? ";" ;
InitVal     := Atom | "undef" | BraceInit ;          (* [Updated in v0.2.1] *)
BraceInit   := "{" BraceElem ("," BraceElem)* "}" ;
BraceElem   := ScalarInit | "null" | "undef" | BraceInit ;
ScalarInit  := IntLit | FloatLit | SymId | LocalId ;  (* still used inside braces *)
```

`null` is a valid `Atom` (via `Coef`) and is the canonical initializer for any `ptr T` local. It represents the null pointer (address 0). Using `null` as the initial value for a non-pointer type is a type error.

**[Updated in v0.2.1] Atom-form initializers.** `InitVal` now permits any `Atom` for non-aggregate types — this covers `addr %x` for pointer initializers, `load %p` for scalar-from-pointer initializers, `cmp …` for `i1`/`<N> i1` initializers, and so on. (The v0.2.0 grammar listed only literals/names; the v0.2.0 implementation already accepted atoms here, so this is the grammar catching up to existing behavior.) Inside braces (`BraceInit`), elements stay restricted to literals/names/`null`/`undef`/nested braces — no atom-form initializers inside aggregate braces, since the brace shape is for static layouts.

Local initialization:
- **Scalar init** for `ptr T`: `null`, a `LocalId` of type `ptr T`, an `addr` atom, a `load` of a `ptr ptr T` source, or `ptrindex`/`ptrfield` results (matching the local's `ptr T` type). Integer literals are not valid initializers for pointer locals.
- **Broadcast**: a scalar `InitVal` for an array `[N] T` fills all elements with the same value. Broadcast init is not valid for pointer types.
- `undef` indicates an uninitialized value; **reading** `undef` is UB.

**[New in v0.2.1] Vector initialization**: for a vector local `<N> T`:
- **Brace init**: `{v₁, v₂, …, vₙ}` with exactly `N` scalar values. Each `vᵢ` must be a valid `BraceElem` of type `T` — i.e., a literal, a `LocalId` of type `T`, or a `SymId` of type `T`. **Symbolic lanes are explicitly supported** (e.g., `let %v: <4> i32 = {%?a, %?b, %?c, %?d};` synthesises four independently-solved lane values). `null` and `undef` are not valid `BraceElem`s for vector lanes (lanes are scalars, not pointers; per-lane `undef` is only reachable via the top-level `undef` form below).
- **Broadcast init**: a single scalar value `v` fills all `N` lanes with `v`.
- **`undef`**: all lanes are undefined; reading any lane is UB.
- `null` is **not** valid for vector types.

Examples:
```text
let %v: <4> i32 = {1, 2, 3, 4};       // brace init
let %w: <4> i32 = 0;                   // broadcast: {0, 0, 0, 0}
let %u: <4> i32 = undef;               // all lanes undefined
let mut %m: <3> f64 = {1.0, 2.0, 3.0}; // mutable vector
```

Mutability:
- `let <id>` creates an **immutable binding**: the value cannot be reassigned. For pointer bindings, the pointer itself is immutable, but the pointee can still be mutated via `store`.
- `let mut <id>` creates a **mutable binding**: the value can be reassigned.

**`addr` restriction**: only a `let mut` local (or a sub-lvalue rooted at a `let mut` local) may appear as the operand of `addr`. Parameters and `let` (immutable) locals cannot have their address taken. **[New in v0.2.1]** Additionally, `addr` is forbidden on vector-typed lvalues (the result would require `ptr <N> T`, which is not a valid type).


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

**`store`**: `store <ptr>, <val>` writes `val` to the memory location addressed by `ptr`.
- `ptr` must be an `RValue` of pointer type — i.e., a local of type `ptr T` (a base local, possibly with field/index accesses that themselves have pointer type). The navigation atoms `ptrindex` and `ptrfield` produce pointer values but are *not* lvalues, so they cannot appear directly here; stage them through a `let mut` local first (same rule as `load load %pp` requiring a `%inner` temp in v0.2.0).
- `ptr` must have type `ptr T` for some **loadable type** `T` (scalar or pointer; see §6.8.3).
- `val` (the `Expr`) must have type `T`.
- `store` is a **statement**; it does not produce a value.
- `store` through `null` or an out-of-bounds address is UB.


## 5. LValues, conditions, expressions

### 5.1 LValues and access paths
```ebnf
LValue      := Base Access* ;
Base        := LocalId ;                   (* locals/params only; no global storage *)
Access      := "[" Index "]" | "." Ident ;
Index       := IntLit | LocalId | SymId ;
```

### 5.2 Conditions
```ebnf
Cond        := Expr RelOp Expr ;
RelOp       := "==" | "!=" | "<" | "<=" | ">" | ">=" ;
```

`Cond` always produces a **scalar boolean** value. Both operands must have the same scalar or pointer type.

Pointer comparison: both operands of a `Cond` may be of the same `ptr T` type. Comparing pointers of different pointer types is a type error. `==` and `!=` are always defined. `<`, `<=`, `>`, `>=` are defined only within the same originating object; cross-object relational comparison is UB (rule 14, §7.5).

**[New in v0.2.1]** Vector comparisons are **not** written as `Cond`. To compare vectors lane-wise, use the `cmp` atom (§5.3) which produces `<N> i1`. `Cond` is reserved for scalar/pointer comparisons used in `br`, `assume`, and `require`.

### 5.3 Expressions (no parentheses, left-to-right)
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
            | PtrIndex                          (* [New in v0.2.1] *)
            | PtrField                          (* [New in v0.2.1] *)
            | CmpAtom                           (* [New in v0.2.1] *)
            | Coef
            | RValue ;

Select      := "select" Cond "," SelectVal "," SelectVal
            | "select" Expr "," SelectVal "," SelectVal ;   (* mask-based [New in v0.2.1] *)
SelectVal   := RValue | Coef ;     (* no nested expressions *)

Cast        := RValue "as" Type ;

AddrOf      := "addr" LValue ;
Load        := "load" RValue ;

PtrIndex    := "ptrindex" RValue "," Index ;                (* [New in v0.2.1] *)
PtrField    := "ptrfield" RValue "," Ident ;                (* [New in v0.2.1] *)

CmpAtom     := "cmp" RelOp RValue "," RValue ;              (* [New in v0.2.1] *)

Coef        := IntLit | FloatLit | LocalId | SymId | "null" ;
RValue      := LValue ;
```

**`addr`**: takes the address of an lvalue. The lvalue's root must be a `let mut` local. The result type is `ptr T` where `T` is the type of the lvalue. `addr` may appear wherever an `Atom` is expected; it cannot appear in `Coef` position (left of a binary `*`, `/`, etc.). **[New in v0.2.1]**: `addr` may now produce aggregate pointer types (`ptr [N] T`, `ptr @S`). `addr` is forbidden on vector-typed lvalues.

**`load`**: dereferences a pointer. The operand must be an `RValue` (lvalue of pointer type), not a value-producing Atom — the same staging rule v0.2.0 used for `load load %pp`. To `load` through a `ptrindex`/`ptrfield` result, stage the pointer through a `let mut` local first:

```text
%t = ptrfield %p, x;   // %t : ptr i32
%v = load %t;          // %v : i32
```

The operand must have type `ptr T` where `T` is a **loadable type** (scalar or pointer). `load` through `ptr [N] T` or `ptr @S` is a type error — use `ptrindex`/`ptrfield` to navigate to a scalar/pointer element first, then `load` the result. The result type is `T`. `load` may appear wherever an `Atom` is expected.

**`null`**: the null pointer constant. Its type is `ptr T` for any `T`, inferred from context. There is no default type for `null`; it must always appear in a typed context. Using `null` as an operand of `+`, `-`, `*`, etc. is a type error.

**Pointer arithmetic**: within the `Expr := Atom (("+"|"-") Atom)*` grammar, the following additional type combinations are valid (see [Section 6](#6-typing-and-well-formedness-v021) for full rules):
- `ptr T + iN` → `ptr T` (advances by `sizeof(T)` per unit)
- `ptr T - iN` → `ptr T` (retreats by `sizeof(T)` per unit)
- `ptr T - ptr T` → `i64` (signed element distance)

**Restrictions on pointer Atoms**: pointers may not appear as the left operand in `Coef "*" RValue`, `Coef "/" RValue`, `Coef "%" RValue`, or any bitwise/shift atom. Pointer multiplication, division, bitwise ops, and shifts are **not supported**.

**`ptrindex` [New in v0.2.1]**: navigates from an array pointer to an element pointer.
- `ptrindex <ptr>, <index>` where `<ptr>` has type `ptr [N] T` and `<index>` is an integer-typed index.
- Result type: `ptr T`.
- The resulting address is `base + index * sizeof(T)`.
- UB if `index < 0` or `index > N`. The one-past-the-end address (`index == N`) is valid for arithmetic but UB to `load`/`store` through (see §7.5 rule 16).

Note the distinction from pointer arithmetic (`+`): `ptr [N] T + 1` advances by `sizeof([N] T)` (treating the pointer as pointing to an array-of-arrays), while `ptrindex (ptr [N] T), 1` advances by `sizeof(T)` (navigating into the array). See §6.8.9 for the full typing rule.

**`ptrfield` [New in v0.2.1]**: navigates from a struct pointer to a field pointer.
- `ptrfield <ptr>, <fieldname>` where `<ptr>` has type `ptr @S` and `<fieldname>` is a field of struct `@S`.
- If field `fieldname` has type `F` in `@S`, the result type is `ptr F`.
- The resulting address is `base + offset(fieldname)`, where `offset` is the sum of `sizeof` of all preceding fields (packed layout).
- `ptrfield` is never UB by itself (the field offset is statically known).

**Vector lane access via subscript [New in v0.2.1]**: lane read and write reuse the LValue subscript syntax `lv[i]`.
- **Read**: `%v[%i]` where `%v` (possibly with prior accesses) has type `<N> T` is an `RValue` (via the `RValue := LValue` rule) of type `T`. UB if `i < 0` or `i >= N` (§7.6 rule 20).
- **Write**: `%v[%i] = %x;` where `%v[%i] : T` and `%x : T` updates lane `i`. Semantically equivalent to "produce a new vector whose lane `i` is `%x` and whose other lanes are unchanged, then rebind `%v`". Same UB rule.
- For a non-destructive update (build a new vector without mutating the original), use a copy first (§6.6 vector assignment):
  ```text
  %w = %v;      // value copy of the vector
  %w[0] = 42;   // mutate the copy; %v unchanged
  ```
- `addr %v[%i]` is **not** valid — vector lanes are not addressable (§2.11, §6.8.2).

**`cmp` [New in v0.2.1]**: reifies a comparison as a first-class value.
- `cmp <relop> <lhs>, <rhs>` where `<relop>` is a relational operator and both operands have the same type.
- For **scalar** operands (integer, float, or pointer): result type is `i1`.
- For **vector** operands (`<N> T`): result type is `<N> i1` (per-lane comparison mask).
- Semantics are identical to `Cond` but the result is a value, not a branch/assume/require condition.
- Pointer comparisons via `cmp` follow the same rules as `Cond` pointer comparison (§5.2).

**Mask-based `select` [New in v0.2.1]**: the second form of `select` takes an expression (not a `Cond`) as the condition:
- `select <mask>, <vtrue>, <vfalse>` where `<mask>` has type `<N> i1` or `i1`.
- If `<mask>` has type `<N> i1`: both arms must have type `<N> T` for the same `N` and `T`. Selection is **per-lane**: lane `k` of the result is `vtrue[k]` if `mask[k] == 1`, else `vfalse[k]`.
- If `<mask>` has type `i1`: both arms must have the same type (scalar, pointer, or vector). Selection is all-or-nothing.
- Mask-based `select` is **lazy per-lane**: only the selected lane's value is evaluated (relevant for UB in lane-access chains such as `%v[%i]` with a dynamic `%i`).

**Parsing note**: the parser distinguishes the two `select` forms by lookahead. After parsing `select` and the first `Expr`, if the next token is a `RelOp` (`==`, `!=`, `<`, `<=`, `>`, `>=`), it continues parsing as `select Cond, ...`. If the next token is `,`, the `Expr` is treated as a mask value.

**Fixed-arity rule for multi-argument atoms [New in v0.2.1]**. The atoms introduced in v0.2.1 use comma-separated arguments inside otherwise comma-separated host contexts (e.g., `require`, `select` arms). To remove the apparent ambiguity each keyword has a **fixed arity** that the parser dispatches on:

| Keyword     | Arity | Form |
|-------------|-------|------|
| `ptrindex`  | 2     | `ptrindex <RValue>, <Index>` |
| `ptrfield`  | 2     | `ptrfield <RValue>, <Ident>` |
| `cmp`       | 2     | `cmp <RelOp> <RValue>, <RValue>` (RelOp is a leading separator, not an arg) |

The parser consumes exactly *arity* comma-separated arguments after the keyword (after the `RelOp` for `cmp`), then returns control to the caller. The comma that follows the atom — if any — belongs to the host context. This disambiguates constructs such as `require cmp == %a, %b, "msg";` (where the second `,` is the `require` separator) to a one-token-lookahead parser.

(Note: `load`/`store` themselves do *not* accept these atoms directly. Navigation atoms must be staged through an `RValue` local first — same rule v0.2.0 uses for `load load %pp`. See `load` description above.)

**[New in v0.2.1] Vector arithmetic**: all scalar binary operators (`+`, `-`, `*`, `/`, `%`, `&`, `|`, `^`, `<<`, `>>`, `>>>`) and unary `~` extend to vectors **lane-wise** when both operands have the same vector type `<N> T`. No implicit scalar-to-vector broadcasting occurs for variables — only literals are inferred to vector type in vector contexts (see §6.11).


## 6. Typing and well-formedness (v0.2.1)

### 6.1 Scalar arithmetic restriction
Arithmetic (`Expr`) is defined over **scalar integer and floating-point leaves**, and over **vector types lane-wise** **[New in v0.2.1]**. Arrays and structs exist to provide addressable structure, but only their scalar elements/fields may be used in arithmetic or comparisons. Pointers may appear in arithmetic only under the rules of Section 2.10.

### 6.2 LValue typing
- If `%x : T` then `%x` has type `T`.
- If `lv : [N] U` then `lv[i] : U` (index must be integer-typed; bounds checked at runtime as UB).
- If `lv : S` and struct `S` has field `f : U` then `lv.f : U`.
- **[New in v0.2.1]**: if `lv : <N> T` then `lv[i] : T` (vector lane access; index must be integer-typed; bounds checked at runtime as UB per §7.6 rule 20). Lane access is the **only** form of index access permitted on a vector — it is what replaces the previously-proposed `extract`/`insert` keywords. Lane access in *write* position (LHS of `=`) is permitted; it semantically rebinds the whole vector to one with the named lane updated. Lane access does **not** make the vector addressable: `addr %v[%i]` is still forbidden (see §6.8.2).

### 6.3 `select` typing
`select c, a, b` (with `Cond` form) is well-typed iff:
- `c` is a boolean condition (`Cond`), and
- `a` and `b` have the same type — scalar (integer or floating-point), pointer (`ptr T`), **or vector (`<N> T`)** **[New in v0.2.1]**.

The result has that type.

**Pointer and `null` arms**: either or both arms may be of pointer type. When one arm is `null` and the other has a known `ptr T` type, `null` is inferred to be `ptr T` regardless of which arm it occupies (sibling type propagation). If both arms are `null` with no other type context, this is a type error.

**[New in v0.2.1] Mask-based `select`**: `select m, a, b` (with `Expr` mask form) is well-typed iff:
- `m` has type `<N> i1` and `a`, `b` have type `<N> T` for the same `N` — per-lane blend.
- OR `m` has type `i1` and `a`, `b` have the same type (scalar, pointer, or vector) — all-or-nothing.

### 6.4 `as` typing
`rval as T` is well-typed iff:
- `rval` is a scalar (integer or floating-point), and `T` is a scalar (integer or floating-point).
- **[New in v0.2.1]** OR: `rval` is `<N> U` (vector) and `T` is `<N> V` (vector with same `N`), where `U as V` is a valid scalar cast. The cast applies **lane-wise**.

Pointer/integer casts (`ptr T as iN`, `iN as ptr T`) are **not supported**.

Runtime cast behavior:
- **Integer resize**: truncation (narrowing) or sign/zero extension (widening). Never UB.
- **Integer-to-float** (`iN as fM`): rounded with RNE. Always produces a finite result (integers are bounded).
- **Float-to-float resize** (`f32 as f64`): exact (widening). (`f64 as f32`): rounded with RNE; UB if the result would overflow to ±∞.
- **Float-to-integer** (`fN as iM`): truncation toward zero. UB if the truncated value is outside the representable range of `iM` (see §7.4 rule 8).
- **[New in v0.2.1] Vector cast** (`<N> U as <N> V`): applies the corresponding scalar cast to each lane independently. Lane-wise UB applies (e.g., if any lane overflows in `<N> f64 as <N> f32`, the path is infeasible).

### 6.5 Bitwise and shift typing
- `Coef op RValue` (`&`, `|`, `^`, `<<`, `>>`, `>>>`): well-typed iff both operands are scalar integers of the same bit-width, **or both are `<N> iM` vectors of the same `N` and `M`** **[New in v0.2.1]**.
- `~ RValue`: well-typed iff the operand is a scalar integer or **a vector of integers (`<N> iM`)** **[New in v0.2.1]**.
- Pointer operands are not valid for bitwise or shift operations.

### 6.6 Mutability rules
- The LHS of `=` must be an lvalue rooted at a `let mut` local.
- `sym` identifiers and `let` (immutable) locals cannot appear on the LHS.
- Parameters are immutable and cannot appear on the LHS.
- `store <ptr>, <val>`: the `ptr` operand may point to any mutable storage (local or received via parameter). The mutability constraint is enforced by the `addr` restriction (only `let mut` lvalues can have their address taken within the function).

**Assignment type rule.** An `LValue = Expr` instruction is well-typed iff:
1. LHS and RHS have the **same** type (no implicit coercion; use `as` for casts), and
2. that type has a **single defined bitwidth as a value** — i.e., a scalar (`iN`, `f32`, `f64`), a pointer (`ptr T`, always 64 bits), or **[New in v0.2.1]** a vector (`<N> T`, with bitwidth `N * bitwidth(T)`).

Aggregate-typed assignment is **not supported**: `%s = %t;` for `%s, %t : @S` and `%a = %b;` for `%a, %b : [N] T` are both type errors. Aggregates have a well-defined `sizeof` (§2.10) for layout purposes, but they don't have a single value-level bitwidth — they're collections of separately-addressable cells, modeled in SMT as per-cell `Store` slots and (when addr-taken) `Mem[T]` arrays, not as a single BV term. Whole-aggregate copy must be written explicitly as field-by-field or element-by-element assignments. Rationale: scalars, pointers, and vectors all map to a single value (single SMT term or, for vectors, an N-tuple that is one logical SIMD register); copying them is one move. Structs and arrays are multi-cell layouts; copying them is a sequence of moves left explicit so the user sees the cost.

**[New in v0.2.1] Vector assignment.** `%v = %w;` for `%v, %w : <N> T` is a whole-vector copy: all `N` lanes are copied in parallel. Semantically equivalent to `N` independent lane copies, encoded in the SMT model as `N` term bindings (§9.5.7).

### 6.7 Floating-point arithmetic typing
- `Expr` involving floating-point atoms must be homogeneous: all atoms in the same `+`/`-` chain must have the exact same floating-point type.
- Mixed arithmetic between different floating-point widths or between integers and floats is forbidden without explicit `as` casts.
- **[New in v0.2.1]** The same rule applies to vector types: `<N> f32` and `<N> f64` cannot be mixed.

### 6.8 Pointer typing rules

#### 6.8.1 `ptr T` as a type
**[Updated in v0.2.1]**: `ptr T` is well-formed iff `T` is a scalar (`iN`, `f32`, `f64`), a `ptr T'` for some well-formed `ptr T'`, an array type `[N] U` for valid `U`, or a struct name `@S`. Recursive pointer types (`ptr ptr ... T`) are well-formed. `ptr <N> T` (pointer to vector) is **not** well-formed.

#### 6.8.2 `addr` typing
`addr <lv>` is well-typed iff:
- The root of `lv` is declared with `let mut`.
- The type of `lv` is a valid `PointeeType` (scalar, pointer, array, or struct — but **not** vector).
- **[New in v0.2.1]** No access in `lv` traverses a vector type. In particular, `addr %v[%i]` and `addr %s.f[%i]` (where `s.f : <N> T`) are both forbidden — a vector lane has no address because the vector itself is not addressable.
- Result type: `ptr T` where `T = type(lv)`.

Examples:
```
let mut %x: i32 = 0;             addr %x       : ptr i32
let mut %arr: [4] i32 = 0;       addr %arr[%i] : ptr i32
let mut %arr: [4] i32 = 0;       addr %arr     : ptr [4] i32     (* [New in v0.2.1] *)
let mut %p: ptr i32 = null;      addr %p       : ptr ptr i32
let mut %s: @Point = {...};      addr %s       : ptr @Point      (* [New in v0.2.1] *)
let mut %s: @Point = {...};      addr %s.x     : ptr i32         (* field addr *)
let mut %v: <4> i32 = 0;         addr %v       : ERROR           (* [New in v0.2.1]: vectors not addressable *)
let mut %v: <4> i32 = 0;         addr %v[%i]   : ERROR           (* [New in v0.2.1]: vector lanes not addressable *)
```

#### 6.8.3 `load` typing
`load <rval>` is well-typed iff:
- `rval` has type `ptr T` for some **loadable type** `T`.
- A type is **loadable** iff it is a scalar (`iN`, `f32`, `f64`) or a pointer (`ptr U`).
- **[New in v0.2.1]**: aggregate types (`[N] T`, `@S`) and vector types (`<N> T`) are **not loadable**. To read through an aggregate pointer, use `ptrindex`/`ptrfield` to navigate to a loadable sub-element.
- Result type: `T`.

#### 6.8.4 `store` typing
`store <ptr>, <val>` is well-typed iff:
- `ptr` has type `ptr T`.
- `val` has type `T`.
- `T` is a loadable type (scalar or pointer).

#### 6.8.5 `null` typing
`null` has type `ptr T` for any well-formed `ptr T`, inferred from context. It is a type error if the context does not provide a `ptr T` type for `null`.

#### 6.8.6 Pointer arithmetic typing
Within `Expr := Atom (("+"|"-") Atom)*`, the following rules apply when the left-hand running type is a pointer:

| Left type | Operator | Right type | Result type |
|-----------|----------|------------|-------------|
| `ptr T`   | `+`      | `iN` (any width) | `ptr T` |
| `ptr T`   | `-`      | `iN` (any width) | `ptr T` |
| `ptr T`   | `-`      | `ptr T` (same T) | `i64`   |

Notes:
- `iN + ptr T` (integer on the left) is a type error. Pointer must be the left operand.
- `ptr T - ptr U` where `T ≠ U` is a type error.
- After a pointer subtraction `ptr T - ptr T`, the result is `i64` and further arithmetic follows integer rules.
- `ptr T + ptr T` is a type error (pointer addition is meaningless).
- **[New in v0.2.1]**: `T` may be an aggregate type. The stride is `sizeof(T)` regardless of what `T` is.
- **Element-distance divisibility**: the result of `ptr T - ptr T` is defined only when both operands share an originating object (rule 12, §7.5). Within a single object, addresses produced by `addr`, `ptrindex`, and `+ iN` always lie on element-size boundaries (multiples of `sizeof(T)` from the object base), so the byte difference is exactly divisible by `sizeof(T)` and the element distance is well-defined as a signed `i64`. Cross-object subtraction is UB.

#### 6.8.7 Pointer comparison typing
All six relational operators are syntactically valid on operands of the same `ptr T` type. Result is boolean.

Runtime behavior:
- `==` and `!=` are always defined: two pointers are equal iff they hold the same address.
- `<`, `<=`, `>`, `>=` are defined only for pointers within the **same originating object** (same `addr` root). Cross-object relational comparison is **UB** (rule 14 in §7.5).

#### 6.8.8 Pointer locals and parameters
- A local `let %p: ptr T` is an immutable binding to a pointer value (the pointer itself cannot be reassigned; the pointee may still be `store`d through).
- A local `let mut %p: ptr T` is a mutable binding (the pointer can be reassigned via `%p = <expr>`).
- A parameter `%p: ptr T` is immutable (the binding cannot appear on the LHS of `=`).

#### 6.8.9 `ptrindex` typing **[New in v0.2.1]**

`ptrindex <rval>, <index>` is well-typed iff:
- `rval` has type `ptr [N] T` for some array type `[N] T`.
- `index` has an integer type (`iN`).
- Result type: `ptr T`.
- **Provenance** (§7.5 rule 15): the result's provenance is the **array** `<rval>` points to. Subsequent arithmetic on the result can roam over all `N` elements of that array. Stepping outside is UB (rule 10). UB to `ptrindex` from `null`, `undef`, or a one-past-the-end pointer (rules 17–19).

**Semantic distinction from `+`**:

| Expression | Input | Stride | Result |
|-----------|-------|--------|--------|
| `%p + %i` where `%p : ptr [N] T` | `ptr [N] T` | `sizeof([N] T) = N * sizeof(T)` | `ptr [N] T` |
| `ptrindex %p, %i` where `%p : ptr [N] T` | `ptr [N] T` | `sizeof(T)` | `ptr T` |

Pointer arithmetic (`+`) treats `ptr [N] T` as a pointer to an array-of-arrays and steps by whole arrays. `ptrindex` navigates **into** the array and steps by elements. This mirrors LLVM's GEP semantics: `+` corresponds to the first GEP index (into the allocation), `ptrindex` to subsequent indices (into the pointee structure).

#### 6.8.10 `ptrfield` typing **[New in v0.2.1]**

`ptrfield <rval>, <fieldname>` is well-typed iff:
- `rval` has type `ptr @S` for some struct type `@S`.
- `fieldname` is a declared field of `@S` with type `F`.
- Result type: `ptr F`.
- **Provenance** (§7.5 rule 15): the result's provenance is the **struct** `<rval>` points to. Subsequent arithmetic on the result can roam over all `sizeof(@S)` bytes. `load`/`store` through the result is UB unless the address lands on a `F`-typed field cell of `@S` (rule 15b — the typed-access mismatch check). UB to `ptrfield` from `null`, `undef`, or a one-past-the-end pointer (rules 17–19).

Since struct fields cannot have vector type (§3.2 `FieldType`), `F` is automatically a valid pointee type; no extra check is needed at `ptrfield` sites.

### 6.9 Vector typing rules **[New in v0.2.1]**

#### 6.9.1 `<N> T` as a type
`<N> T` is well-formed iff:
- `N ≥ 2`.
- `T` is a scalar type (`iN`, `f32`, `f64`).
- Vectors of pointers (`<N> ptr T`), vectors of vectors (`<N> <M> T`), and vectors of aggregates are not well-formed.

#### 6.9.2 Vector arithmetic typing
All binary arithmetic operators (`+`, `-`, `*`, `/`, `%`) are well-typed on vector operands iff both operands have the same vector type `<N> T`. Result type: `<N> T`. The operation applies lane-wise.

All binary bitwise/shift operators (`&`, `|`, `^`, `<<`, `>>`, `>>>`) are well-typed on vector operands iff both operands have the same vector type `<N> iM`. Result type: `<N> iM`.

Unary `~` is well-typed on `<N> iM`. Result type: `<N> iM`.

Mixed scalar-vector arithmetic is a type error. To operate between a scalar and a vector, broadcast the scalar to a vector first (via a local with broadcast initialization), or use a literal (see §6.11).

#### 6.9.3 Vector lane access typing
Lane access reuses the LValue subscript syntax `lv[i]` (§6.2). The relevant typing rules:
- **Read**: `lv[i]` where `lv : <N> T` and `i` has an integer type has type `T`.
- **Write**: `lv[i] = expr;` is well-typed iff `lv : <N> T`, `i` is integer-typed, and `expr : T`. Semantically rebinds `lv`'s base local to a vector with lane `i` updated; all other lanes unchanged. The base of `lv` must therefore be a `let mut` local (§6.6).
- **Address**: `addr lv[i]` is a type error (§6.8.2).
- **UB**: `i < 0` or `i >= N` is UB (§7.6 rule 20).

#### 6.9.4 `cmp` typing
`cmp <relop> <lhs>, <rhs>` is well-typed iff:
- Both `lhs` and `rhs` have the same type.
- For **scalar** operands (integer, float, or pointer): result type is `i1`.
- For **vector** operands (`<N> T`): result type is `<N> i1`.
- The same relational operator restrictions apply as for `Cond`: pointer `<`/`<=`/`>`/`>=` is only defined within the same object (UB otherwise).
- Vector `cmp` is **not defined** on pointer vectors (no `<N> ptr T`).

#### 6.9.5 Vector locals and parameters
- A local `let %v: <N> T` is an immutable vector binding. Lane-write `%v[%i] = …;` is rejected on an immutable binding (the assignment would mutate the binding).
- A local `let mut %v: <N> T` is a mutable vector binding (can be reassigned in whole via `%v = %w;` or per-lane via `%v[%i] = %x;`).
- A parameter `%v: <N> T` is immutable (same restriction as immutable `let`).
- Vectors are passed by value.

### 6.10 Mask-based `select` typing **[New in v0.2.1]**

`select m, a, b` (mask form) is well-typed iff one of:
1. `m : <N> i1`, `a : <N> T`, `b : <N> T` — result type `<N> T` (per-lane blend).
2. `m : i1`, `a : U`, `b : U` for any valid scalar/pointer/vector type `U` — result type `U` (all-or-nothing).

For case (1), `null` arm inference works as in the `Cond` form when `T = ptr V`.

### 6.11 Literal typing and inference
Literals do not have fixed types but are inferred from their context:
- **Integer Literals:** Inferred to match the bit-width of the operation target or neighboring operands. Default: `i32`.
- **Floating-point Literals:** Inferred to match the target or neighbors. Default: `f32`.
- **`null`:** Inferred to match the `ptr T` context. No default; context must be available. In `select c, a, b`, if one arm is `null` and the other has type `ptr T`, `null` is inferred to `ptr T` from the sibling arm (bidirectional — order does not matter).
- **Homogeneity Requirement:** Since mixed arithmetic is forbidden, a literal in `x + <lit>` is inferred to match the type of `x`.

**[New in v0.2.1] Vector literal inference**: when a literal appears in a context requiring `<N> T`:
- An integer or floating-point literal is inferred as a **broadcast** vector `<N> T` with all lanes set to the literal value.
- Example: `2 * %v` where `%v : <4> i32` — the literal `2` is inferred as `<4> i32` with value `{2, 2, 2, 2}`.
- This applies only to **literals** (integer, float). Variables and symbols are never implicitly broadcast. `%x * %v` where `%x : i32` and `%v : <4> i32` is a type error.


## 7. Strict UB rules (v0.2.1)

UB is checked during symbolic execution along the chosen path. Any UB makes the path infeasible.

### 7.1 UB conditions (inherited)
1. **Integer division/modulo by zero**: in `k / x` or `k % x` where `k` and `x` are integers, UB if `x == 0`. (For floating-point, see §7.4 rules 6–7.)
2. **Out-of-bounds array access**: for `a[i]` where `a : [N] T`, UB if `i < 0` or `i >= N`.
3. **Reading `undef`**: reading a location whose stored value is `undef` is UB.
4. **Signed integer overflow**: `+`, `-`, `*`, `<<` that produce a value outside the representable range of the target bit-width. Also: `INT_MIN / -1`. For `<<` specifically: UB if the result `x * 2^n` is not representable in `width(x)` signed bits, OR if `x < 0`. SymIR treats `<<` as signed integer arithmetic (aligning with `+`/`-`/`*`), not as a bit-vector shift — this keeps the overflow story consistent across all integer arithmetic operators.
5. **Overshift**: in `x << n`, `x >> n`, `x >>> n`, UB if `n < 0` or `n >= width(x)`. (Separate from rule 4 — overshift is about the shift *amount*, not the shifted result.)

### 7.2 `select` and strict UB (lazy)
For `select c, a, b` (with `Cond`):
- Evaluate `c` first; UB in `c` makes the path infeasible.
- If `c` is true, evaluate only `a`; UB in `a` makes the path infeasible. `b` is not evaluated.
- If `c` is false, evaluate only `b`; UB in `b` makes the path infeasible. `a` is not evaluated.

**[New in v0.2.1]** For mask-based `select m, a, b` (with `<N> i1` mask):
- Evaluate `m` first; UB in `m` makes the path infeasible.
- For each lane `k`: if `m[k] == 1`, only `a[k]` is evaluated; if `m[k] == 0`, only `b[k]` is evaluated. UB in any evaluated lane makes the path infeasible.

### 7.3 Floating-point arithmetic semantics
See §2.9 for the full floating-point value model. Key points:
- All operations use **RNE** rounding except `%` (see below).
- `%` is C's `fmod` (truncated-quotient), **not** IEEE 754 `remainder` (`fp.rem`). Result sign matches the dividend sign, consistent with integer `%`.
- Comparisons are total over the finite domain (NaN never occurs). `+0.0 == -0.0` is `true`.

### 7.4 Floating-point UB rules

6. **FP overflow (±∞ result)**: any arithmetic operation (`+`, `-`, `*`, `/`) whose IEEE 754 result (under RNE) would be ±∞ is UB. This covers finite-operand overflow and division of any non-zero value by ±0.0.

7. **FP invalid operation (NaN result)**: any operation whose IEEE 754 result would be NaN is UB. This covers `±0.0 / ±0.0` and `x % ±0.0` for any `x`.

8. **Float-to-integer out-of-range**: `fN as iM` is UB if the float value, after truncation toward zero, is outside the representable range of `iM`.

### 7.5 Pointer UB rules

9. **Null pointer dereference**: `load <ptr>` or `store <ptr>, <val>` where `ptr` evaluates to `null` (address 0) is UB.

10. **Out-of-bounds pointer arithmetic**: every pointer carries a *provenance object* (the aggregate or scalar storage it was derived from — see rule 15 for how provenance is assigned). For `%p ± n`:
    - Let `base = addr(provenance)` and `size = sizeof(provenance)` (the full byte size of the provenance object: for `[N] T` it is `N * sizeof(T)`; for `@S` it is the packed sum of field sizes per §2.10; for a scalar it is `sizeof(scalar)`).
    - UB if the resulting address falls outside `[base, base + size]`.
    - The "one-past-the-end" address `base + size` is a valid non-dereferenceable address (valid for arithmetic and equality; UB to `load`/`store`).

11. **Out-of-bounds load/store**: `load <ptr>` or `store <ptr>, <val>` where `ptr` points outside the bounds of the originating object (including the one-past-the-end address) is UB.

12. **Cross-object pointer arithmetic**: forming a pointer by arithmetic that crosses from one local variable's storage into another is UB. The memory regions of distinct local variables do not overlap.

13. **Uninitialized pointer dereference**: `load <ptr>` or `store <ptr>, <val>` where `ptr` itself is `undef` is UB (follows from rule 3 applied to the pointer value).

14. **Cross-object pointer comparison**: comparing two pointers derived from different originating objects with `<`, `<=`, `>`, `>=` is UB. Equality (`==`, `!=`) between pointers of different objects is well-defined and always produces `false` / `true` respectively (distinct objects occupy disjoint address ranges per the non-overlap axioms). Only `<`/`<=`/`>`/`>=` comparisons between pointers of the same originating object are defined.

    **Rationale**: the SMT model assigns abstract address constants that satisfy non-overlap constraints but imposes no ordering between distinct objects. Permitting `<`/`>`/etc. across objects would allow `assume px < py` to spuriously constrain the abstract address assignment, producing results that are model-dependent and meaningless. Declaring it UB prevents this. This matches the C standard (§6.5.8p5).

15. **Aggregate-derived pointer provenance [revised in v0.2.1]**: every pointer derivation carries a *provenance object*. The rule is uniform between arrays and structs and depends on the **final access** in the derivation:
    - **Top-level `addr %lv`** where `%lv : T` is a `let mut` local: provenance = `%lv` (the whole local). For aggregates this is the whole array / whole struct; for scalars it is the one-element object of size `sizeof(T)`.
    - **Field access — `addr lv.f` or `ptrfield <ptr>, f`**: provenance = the *immediate containing struct* of `f` (the struct `lv` for the direct form; the struct `<ptr>` points to for the `ptrfield` form). The result pointer's static type is `ptr T_f`, but its provenance covers the entire struct, so arithmetic can roam over any byte offset in `[0, sizeof(@S))`.
    - **Index access — `addr lv[i]` or `ptrindex <ptr>, i`**: provenance = the *immediate containing array* (the array `lv` for the direct form; the array `<ptr>` points to for the `ptrindex` form). The result pointer's static type is `ptr T`, and arithmetic can roam over the array's byte range `[0, N * sizeof(T))`.
    - **Pointer arithmetic — `<ptr> ± n`**: provenance is **unchanged** from `<ptr>`. The address shifts; the provenance object stays the same.

    Chained derivations (`ptrfield (ptrindex (addr %arr), i), x`) mirror the direct lvalue form (`addr %arr[i].x`): provenance is the immediate parent of the *final* access — here `%arr[i]` (one struct), not the whole array. Each `ptrindex` or `ptrfield` step replaces the provenance with the immediate parent of its access.

    Pointer arithmetic that walks outside the provenance object's storage range is UB (rule 10). One-past-the-end is valid for arithmetic and equality only.

    **Rationale**: this generalises the v0.2.0 "one-element" rule to allow pointer arithmetic within an aggregate. Type discipline is preserved by rule 15b below: even if arithmetic stays in bounds, the eventual `load`/`store` must hit a cell whose declared type matches the pointer's static type.

15b. **Typed-access mismatch [new in v0.2.1]**: `load %p` or `store %p, v` through `%p : ptr T` is UB if the runtime address `%p` evaluates to does not coincide with the start of a `T`-typed cell within the provenance object. Formally, define *valid `T` cells* of an object:
    - For an array `[N] U`: every offset `k * sizeof(U)` for `0 ≤ k < N`. Each cell has type `U`. (For `U ≠ T`, no cell is `T`-typed — but in that case the pointer type itself, `ptr U` derived from this array, is necessarily `T = U`, so the mismatch reduces to a bounds question.)
    - For a struct `@S { f₁ : T₁; …; fₖ : Tₖ; }`: every offset `off_fᵢ` for each field `fᵢ` whose declared type is `T`. Offsets of fields with a *different* declared type are **not** valid `T` cells, even if the field size matches `sizeof(T)`.
    - For a top-level scalar local `%x : U`: offset `0` if `U == T`; no valid cells otherwise.

    If the address falls anywhere else — mid-cell, on a cell of a different type, straddling cells, or outside the object — the access is UB. For homogeneous arrays of `T`, in-bounds element-aligned arithmetic automatically satisfies this; the rule does real work for structs with mixed field types.

    **Example.**
    ```text
    struct @T { a: i32; b: i64; }
    let mut %t: @T = ...;
    let %pa: ptr i32 = addr %t.a;     // valid: cell at offset 0, type i32
    %pa = %pa + 1;                      // address = base + 4, in bounds (sizeof(@T) = 12)
    load %pa;                           // UB: offset 4 is %t.b (i64), not i32
    ```

**[New in v0.2.1]**

16. **`ptrindex` out-of-bounds**: `ptrindex <ptr>, <index>` where `<ptr> : ptr [N] T` is UB if `index < 0` or `index > N`. Index `N` (one-past-the-end) produces a valid non-dereferenceable address (valid for arithmetic and equality; UB to `load`/`store` — caught by rule 11 or 15b). Index values in `[0, N-1]` produce valid dereferenceable addresses. The provenance of the result is the array `<ptr>` points to (rule 15).

17. **Navigation through `null` [new in v0.2.1]**: `ptrindex <ptr>, <i>` or `ptrfield <ptr>, <f>` is UB if `<ptr>` evaluates to `null` (address 0). Rationale: the result would be a non-null pointer with no meaningful provenance; declaring UB at the navigation site prunes the path immediately instead of waiting for the eventual `load`/`store`.

18. **Navigation through `undef` [new in v0.2.1]**: `ptrindex` and `ptrfield` count as reads of their pointer operand, so a `<ptr>` operand whose value is `undef` is UB at the navigation site (consequence of rule 3 applied to the pointer value, made explicit here).

19. **Navigation from a one-past-the-end pointer [new in v0.2.1]**: `ptrindex <ptr>, <i>` or `ptrfield <ptr>, <f>` is UB if `<ptr>` is exactly the one-past-the-end address of its provenance object. Such an address is valid for arithmetic and equality (rule 10) but does not point at any element to navigate into.

### 7.6 Vector UB rules **[New in v0.2.1]**

20. **Out-of-bounds vector lane access**: a lane read `lv[i]` or lane write `lv[i] = …` where `lv : <N> T` is UB if `i < 0` or `i >= N`. Same rule for both read and write positions.

21. **Lane-wise scalar UB**: all scalar UB rules (rules 1–8) apply **per-lane** to vector operations. If any single lane would trigger UB in a vector operation, the entire path is infeasible. For example:
    - `%a / %b` where `%a, %b : <4> i32` is UB if any lane of `%b` is `0`.
    - `%a + %b` where `%a, %b : <4> i32` is UB if any lane overflows.
    - `%v as <4> f32` where `%v : <4> f64` is UB if any lane would overflow to ±∞.

22. **Reading `undef` vector lane**: reading a lane whose value is `undef` (from a vector initialized with `undef`, or from a vector where the read lane has not yet been written by lane-write or whole-vector copy) is UB. This is the per-lane extension of rule 3.


## 8. Division and modulo (round toward 0)

SymIR uses **truncation toward zero** for both integer and floating-point `%`, so the result sign always matches the sign of the dividend.

### 8.1 Integer division and modulo

For integers `A` and `B` with `B != 0`:
- `Q = trunc(A / B)` (round toward 0)
- `R = A - Q*B`; `A % B = R`

Properties: `|R| < |B|`; `R` has the same sign as `A` (or is 0).

### 8.2 Floating-point modulo (`fmod` semantics)

For finite floats `A` and `B` with `B != ±0.0`:
- `Q = trunc(A / B)` (the real quotient truncated toward zero)
- `R = A - Q*B`; `A % B = R`

This matches C's `fmod`, **not** IEEE 754 `remainder` (`fp.rem`).

| Expression | `fmod` (SymIR `%`) | `fp.rem` (IEEE remainder) |
|---|---|---|
| `1.5 % 1.0` | `0.5` | `-0.5` |
| `-1.5 % 1.0` | `-0.5` | `0.5` |
| `3.5 % 2.0` | `1.5` | `-0.5` |

**SMT encoding**: `fp.sub(A, fp.mul(fp.roundToIntegral[RTZ](fp.div[RNE](A, B)), B))`.

**Trade-off vs `fp.rem`**: `fp.rem` maps to a single `fp.rem` SMT primitive with direct solver support. The `fmod` encoding requires four composed FP operations and loses that direct-primitive efficiency. This cost is accepted for semantic uniformity with integer `%`.


## 9. Path-based symbolic execution and constraint extraction

### 9.1 Symbolic state
The executor maintains:
- `Store`: mapping from local lvalues to symbolic terms.
- `Mem[T]`: a family of symbolic memory arrays, one per loadable pointee type `T`.
  - Each `Mem[T]` is a function from 64-bit BV addresses to `T`-typed BV values.
  - In SMT: `Mem[T] : (Array (_ BitVec 64) (_ BitVec (8*sizeof(T))))`.
- `PC`: feasibility constraints (conjunction).
- `REQ`: property constraints (conjunction).

**[New in v0.2.1]** Vector values in the store are represented as `N`-tuples of individual lane terms. See §9.5 for the SMT encoding.

### 9.2 Constraint sources on the chosen path
While executing along `π`:
- **Branches**: for `br cond, ^t, ^f;`, conjoin `cond` (or `not(cond)`) to `PC` based on which successor is on `π`.
- **Assumptions**: `assume c;` conjoins `c` to `PC`.
- **Requirements**: `require c;` conjoins `c` to `REQ`.
- **Strict UB**: any UB encountered sets `PC := false`.
- **`store`**: `store %p, v;` updates `Mem[T]` functionally: `Mem[T] := store(Mem[T], addr(%p), v)`.

### 9.3 Solve goal
Let `DOM` be the conjunction of all symbol-domain constraints. The solver is invoked on:

`DOM ∧ PC ∧ REQ`

A satisfying model yields concrete values for all symbols.

### 9.4 SMT encoding of pointers

#### 9.4.1 Abstract address assignment
For each `let mut` local `%x` that appears as the operand of `addr` on the chosen path, the SMT model introduces a **fresh abstract address constant**:

```
(declare-const addr_%x (_ BitVec 64))
```

This constant represents the base address of `%x`'s storage.

#### 9.4.2 Non-overlap axioms
For every pair of distinct `addr`-taken locals `%x : T_x` and `%y : T_y` on the path, add:

```
(assert (bvuge (bvsub addr_%y addr_%x) (_ bv<sizeof(T_x)> 64)))
(assert (bvuge (bvsub addr_%x addr_%y) (_ bv<sizeof(T_y)> 64)))
```

These two assertions together ensure the address ranges `[addr_%x, addr_%x + sizeof(T_x))` and `[addr_%y, addr_%y + sizeof(T_y))` do not overlap. They are added unconditionally (not path-conditional).

**[New in v0.2.1]**: `sizeof(T)` now includes structs: `sizeof(@S) = Σ sizeof(field_i)` (packed layout, computed recursively per §2.10). The non-overlap axiom for a struct local uses this packed size.

Example: with `struct @Vec2 { x: f64; y: f64; }` and `let mut %p: @Vec2 = ...`, `sizeof(@Vec2) = 16`, so for any other addr-taken local `%y : T_y` the axiom uses `(_ bv16 64)` on the `%p` side:

```smt2
(assert (bvuge (bvsub addr_%y addr_%p) (_ bv16 64)))
(assert (bvuge (bvsub addr_%p addr_%y) (_ bv<sizeof(T_y)> 64)))
```

#### 9.4.3 `addr` evaluation
- `addr %x` evaluates to `addr_%x` (the base address constant for `%x`).
- `addr %arr[i]` evaluates to `bvadd(addr_%arr, bvmul(i_bv, sizeof(element)))` where `i_bv` is the BV encoding of index `i`.
- `addr %p` where `%p : ptr T` evaluates to `addr_%p` (a pointer-to-pointer; the address of the pointer variable itself).

**[New in v0.2.1]**:
- `addr %arr` where `%arr : [N] T` evaluates to `addr_%arr` (same base address as `addr %arr[0]`, but typed as `ptr [N] T`).
- `addr %s` where `%s : @S` evaluates to `addr_%s`.
- `addr %s.f` where `%s : @S` and field `f` has offset `off` evaluates to `bvadd(addr_%s, off)`.

#### 9.4.4 `load` evaluation
`load %p` where `%p` evaluates to address `a` and has type `ptr T`:

```
(select Mem[T] a)
```

The path condition receives the UB constraints: `a ≠ 0` (not null), and `a` is within bounds of the originating object.

#### 9.4.5 `store` evaluation
`store %p, v` where `%p` evaluates to address `a`, type `ptr T`, value `v`:

```
Mem[T] := (store Mem[T] a v)    ; functional update
```

The path condition receives: `a ≠ 0`, `a` within bounds.

#### 9.4.6 Pointer parameter encoding
A pointer parameter `%p: ptr T` is encoded as a fresh 64-bit BV:

```
(declare-const param_%p (_ BitVec 64))
```

No non-overlap axioms are generated for parameter pointers (their origin is external). The path condition may accumulate `assume`/`require` constraints on the parameter pointer value.

#### 9.4.7 Consistency of `Store` and `Mem`
When a `let mut` local `%x : T` is `addr`-taken:
- Direct assignments `%x = v` update both `Store[%x]` and `Mem[T][addr_%x]` consistently.
- `load (addr %x)` equals `Store[%x]` by construction.
- `store (addr %x), v` and `%x = v` are semantically equivalent and both update the same memory slot.

**Array-element consistency**: when a `let mut` local `%arr : [N] T` is addr-taken via `addr %arr[i]`:
- The base address constant `addr_%arr` is introduced once for the whole array (§9.4.1).
- `addr %arr[i]` evaluates to `bvadd(addr_%arr, bvmul(i_bv, sizeof(T)))` (§9.4.3).
- A direct assignment `%arr[i] = v` updates `Store[%arr][i]` and simultaneously updates `Mem[T][addr_%arr + i*sizeof(T)]` consistently.
- `load (addr %arr[i])` equals `Store[%arr][i]` by construction.
- `store (addr %arr[i]), v` and `%arr[i] = v` are semantically equivalent.
- The non-overlap axiom for `%arr` uses `sizeof([N] T) = N * sizeof(T)` (§9.4.2), reserving the full array storage range.

**Struct-field consistency [New in v0.2.1]**: when a `let mut` local `%s : @S` is addr-taken (whole-struct via `addr %s` or per-field via `addr %s.f`):
- The base address constant `addr_%s` is introduced once for the whole struct (§9.4.1).
- For each field `f : T_f` with declaration-order offset `off_f = Σ sizeof(field_j) for j < f`:
  - `addr %s.f` evaluates to `bvadd(addr_%s, (_ bv<off_f> 64))` (§9.4.3).
  - A direct assignment `%s.f = v` updates `Store[%s].f` and simultaneously updates `Mem[T_f][addr_%s + off_f]`.
  - `load (addr %s.f)` equals `Store[%s].f` by construction.
  - `store (addr %s.f), v` and `%s.f = v` are semantically equivalent.
  - `store (ptrfield %p, f), v` where `%p` evaluates to `addr_%s` updates the same slot — `Mem[T_f][addr_%s + off_f]` and `Store[%s].f`.
- Per §7.5 rule 15 (revised), the *provenance* of `addr_%s + off_f` is the whole struct `%s`: arithmetic from this pointer may range over `[addr_%s, addr_%s + sizeof(@S))`. Stepping outside the struct is UB (rule 10). Stepping *within* the struct to a field of a different type is **arithmetically valid** but **UB to `load`/`store`** through the original pointer's type (rule 15b — typed-access mismatch).
- Concretely, a `load %p` or `store %p, v` where `%p : ptr T_f` and `%p` evaluates to `addr_%s + δ` adds the path condition: `δ ∈ {off_g : @S field g has declared type T_f}` (a disjunction over the field offsets of the matching type). Off this disjunction → UB; on it → the load resolves to `Mem[T_f][addr_%s + δ]` and is consistent with `Store[%s].g` for the field `g` at offset `δ`.
- The non-overlap axiom for `%s` uses `sizeof(@S) = Σ sizeof(field_j)`; this is what reserves the full struct storage range against other locals. Cross-field non-overlap *within* a single struct is enforced by the disjoint per-field offsets (statically known), so no SMT axiom is needed for it.

#### 9.4.8 `ptrindex` evaluation **[New in v0.2.1]**

`ptrindex %p, %i` where `%p : ptr [N] T` evaluates to:

```
(bvadd p_addr (bvmul i_bv (_ bv<sizeof(T)> 64)))
```

The result is a `ptr T` pointing to element `i` within the array. The path condition receives UB constraints: `i >= 0` and `i <= N` (rule 16); `p_addr ≠ 0` (rule 17, navigation through null); and `p_addr ≠ base_provenance(p) + size_provenance(p)` (rule 19, navigation from one-past-end). Without those guards the term `bvadd p_addr (bvmul i_bv sizeof(T))` would silently wrap modulo 2^64 or produce wild addresses. The resulting pointer's provenance is the same array `%p` points to (so further pointer arithmetic ranges over `[base, base + N*sizeof(T)]`). It can be used for `load`/`store` (if `T` is loadable and `i < N`) or further `ptrindex`/`ptrfield` navigation.

#### 9.4.9 `ptrfield` evaluation **[New in v0.2.1]**

`ptrfield %p, f` where `%p : ptr @S` and field `f` has type `F` at offset `off`:

```
(bvadd p_addr (_ bv<off> 64))
```

The offset is computed from the packed layout: `off = Σ sizeof(field_j)` for all fields `j` preceding `f` in the struct declaration order. The result is a `ptr F` whose provenance is the same `@S` `%p` points to (so further pointer arithmetic ranges over `[base, base + sizeof(@S)]`). UB constraints conjoined to PC: `p_addr ≠ 0` (rule 17, null navigation); `p_addr ≠ base_provenance(p) + sizeof(@S)` (rule 19, navigation from one-past-end). No bounds constraint on the resulting address itself — the offset is static, so it always lands inside the struct.

### 9.5 SMT encoding of vectors **[New in v0.2.1]**

#### 9.5.1 Vector variables

Each vector value `%v : <N> T` is encoded as `N` independent BV constants:

```smt2
;; %v : <4> i32
(declare-const v_0 (_ BitVec 32))
(declare-const v_1 (_ BitVec 32))
(declare-const v_2 (_ BitVec 32))
(declare-const v_3 (_ BitVec 32))
```

For floating-point vectors, each lane uses the corresponding FP sort:

```smt2
;; %w : <2> f64
(declare-const w_0 (_ FloatingPoint 11 53))
(declare-const w_1 (_ FloatingPoint 11 53))
```

#### 9.5.2 Vector symbols

A `sym %?v: value <N> T in [lo, hi];` introduces `N` independent symbolic constants with per-lane domain constraints:

```smt2
(declare-const sym_v_0 (_ BitVec 32))
(declare-const sym_v_1 (_ BitVec 32))
(declare-const sym_v_2 (_ BitVec 32))
(declare-const sym_v_3 (_ BitVec 32))
(assert (and (bvsge sym_v_0 lo) (bvsle sym_v_0 hi)))
(assert (and (bvsge sym_v_1 lo) (bvsle sym_v_1 hi)))
;; ... etc.
```

#### 9.5.3 Lane-wise operations

Binary operations encode as `N` independent scalar operations:

```smt2
;; %c = %a + %b  where %a, %b, %c : <4> i32
(define-fun c_0 () (_ BitVec 32) (bvadd a_0 b_0))
(define-fun c_1 () (_ BitVec 32) (bvadd a_1 b_1))
(define-fun c_2 () (_ BitVec 32) (bvadd a_2 b_2))
(define-fun c_3 () (_ BitVec 32) (bvadd a_3 b_3))
```

Per-lane UB constraints (e.g., overflow) are conjoined to `PC` for each lane independently. If any lane triggers UB, `PC := false`.

#### 9.5.4 Vector lane access encoding

**Read** `%v[%i]` — lane selection.

*Constant index*: direct.
```smt2
;; %v[2]  where %v : <4> i32
v_2    ;; just the lane constant
```

*Dynamic index*: `ite` chain.
```smt2
;; %v[%i]  where %v : <4> i32
(ite (= i 0) v_0 (ite (= i 1) v_1 (ite (= i 2) v_2 v_3)))
```
The UB guard `i >= 0 ∧ i < N` is conjoined to `PC` (§7.6 rule 20).

**Write** `%v[%i] = %val;` — rebinds `%v` to `N` new lane terms, one per lane.

*Constant index*:
```smt2
;; %v[2] = %val  where %v : <4> i32
;; new lanes: v'_0 = v_0, v'_1 = v_1, v'_2 = val, v'_3 = v_3
```

*Dynamic index*: per lane `k`, the new lane term is `ite(i == k, val, v_k)`.
```smt2
;; %v[%i] = %val  →  for each k:
(define-fun v'_0 () (_ BitVec 32) (ite (= i 0) val v_0))
(define-fun v'_1 () (_ BitVec 32) (ite (= i 1) val v_1))
(define-fun v'_2 () (_ BitVec 32) (ite (= i 2) val v_2))
(define-fun v'_3 () (_ BitVec 32) (ite (= i 3) val v_3))
```
After the write, `Store[%v]` is updated to point at the new lane terms `v'_0..v'_{N-1}`. The same UB guard applies.

#### 9.5.5 `cmp` encoding

```smt2
;; %mask = cmp < %a, %b  where %a, %b : <4> i32, %mask : <4> i1
(define-fun mask_0 () (_ BitVec 1) (ite (bvslt a_0 b_0) #b1 #b0))
(define-fun mask_1 () (_ BitVec 1) (ite (bvslt a_1 b_1) #b1 #b0))
(define-fun mask_2 () (_ BitVec 1) (ite (bvslt a_2 b_2) #b1 #b0))
(define-fun mask_3 () (_ BitVec 1) (ite (bvslt a_3 b_3) #b1 #b0))
```

#### 9.5.6 Mask-based `select` encoding

```smt2
;; %r = select %mask, %a, %b  where %mask : <4> i1, %a, %b : <4> i32
(define-fun r_0 () (_ BitVec 32) (ite (= mask_0 #b1) a_0 b_0))
(define-fun r_1 () (_ BitVec 32) (ite (= mask_1 #b1) a_1 b_1))
(define-fun r_2 () (_ BitVec 32) (ite (= mask_2 #b1) a_2 b_2))
(define-fun r_3 () (_ BitVec 32) (ite (= mask_3 #b1) a_3 b_3))
```

#### 9.5.7 Vector assignment encoding

A whole-vector assignment `%v = %w;` updates each of `%v`'s lane terms to the corresponding lane term of `%w`:

```smt2
;; %v = %w  where %v, %w : <4> i32
(define-fun v_0 () (_ BitVec 32) w_0)
(define-fun v_1 () (_ BitVec 32) w_1)
(define-fun v_2 () (_ BitVec 32) w_2)
(define-fun v_3 () (_ BitVec 32) w_3)
```

This mirrors how `%x = %y` is encoded for a scalar, just lifted to `N` parallel scalar bindings. No additional path conditions are generated — vector copy is total and side-effect-free.


## 10. Examples

### 10.1 Simple pointer read and write
```text
fun @ptr_rw(%val: i32) : i32 {
  let mut %x: i32 = 0;
  let mut %p: ptr i32 = addr %x;

^entry:
  store %p, %val;
  ret load %p;
}
```
This function stores `%val` into `%x` through the pointer and returns it. The result equals `%val`.

### 10.2 Array traversal with a pointer
```text
fun @sum4(%arr: [4] i32) : i32 {
  let mut %p: ptr i32 = addr %arr[0];
  let mut %acc: i32 = 0;
  let %one: i32 = 1;

^entry:
  %acc = %acc + load %p;
  %p = %p + %one;
  %acc = %acc + load %p;
  %p = %p + %one;
  %acc = %acc + load %p;
  %p = %p + %one;
  %acc = %acc + load %p;
  ret %acc;
}
```
`%p` is advanced by 1 element (4 bytes for `i32`) on each step. Each `load %p` reads the next array element.

### 10.3 Pointer parameter — out-parameter synthesis
Synthesize a coefficient `@?c` such that a function writing through an output pointer satisfies a given postcondition.
```text
fun @out_write(%out: ptr i32, %x: i32) : i32 {
  sym @?c: coef i32 in [-8, 8];

^entry:
  store %out, @?c * %x;
  require load %out == 6, "output is 6";
  ret load %out;
}
```
The solver finds `@?c` such that `@?c * %x == 6` for a concrete `%x` (e.g., `%x = 3` → `@?c = 2`).

### 10.4 Pointer-to-pointer (two-level indirection)
```text
fun @deref2(%val: i32) : i32 {
  let mut %x: i32 = 0;
  let mut %p: ptr i32 = addr %x;
  let mut %pp: ptr ptr i32 = addr %p;
  let mut %inner: ptr i32 = null;

^entry:
  %inner = load %pp;
  store %inner, %val;
  ret load %inner;
}
```
`load %pp` dereferences `%pp` to get `%p` (a `ptr i32`). The staged form is required because `load %pp` is an Atom, not an LValue, so chained `load load %pp` requires an intermediate local.

### 10.5 Null guard (pointer validity check)
```text
fun @safe_load(%p: ptr i32) : i32 {
  let %zero: i32 = 0;
  let %null_ptr: ptr i32 = null;

^entry:
  br %p == %null_ptr, ^is_null, ^valid;

^is_null:
  ret %zero;

^valid:
  ret load %p;
}
```
On the path `^entry -> ^valid`, the path condition includes `%p != null`, so `load %p` is UB-free.

### 10.6 Pointer difference
```text
fun @ptr_diff(%arr: [8] i32) : i64 {
  let mut %p: ptr i32 = addr %arr[0];
  let mut %q: ptr i32 = addr %arr[5];

^entry:
  ret %q - %p;    // element distance: 5
}
```
`%q - %p` yields `i64` value `5` (not `20`, because the result is in elements, not bytes).

### 10.7 Aggregate pointer — `ptrindex` **[New in v0.2.1]**
Access an element of a `[4] i32` through a `ptr [4] i32` using `ptrindex`. The `ptrindex` result is an Atom (not an LValue), so it is staged through a `let mut` local before being used as the operand of `load`.
```text
fun @agg_ptr_read() : i32 {
  let mut %arr: [4] i32 = {10, 20, 30, 40};
  let %p: ptr [4] i32 = addr %arr;
  let mut %elem_p: ptr i32 = null;

^entry:
  // ptrindex navigates into the array: ptr [4] i32 → ptr i32
  %elem_p = ptrindex %p, 2;
  ret load %elem_p;          // returns 30
}
```
Compare: `addr %arr[2]` directly yields `ptr i32` and needs no staging. `ptrindex` is needed when you have a `ptr [N] T` value (e.g., from a parameter or computation) rather than direct lvalue access.

### 10.8 Aggregate pointer — `ptrfield` **[New in v0.2.1]**
Navigate a struct pointer to read and write individual fields. Each `ptrfield` result must be staged through a local before `load` or `store`.
```text
struct @Vec2 { x: f64; y: f64; }

fun @swap_fields(%p: ptr @Vec2) : f64 {
  let mut %tmp: f64 = 0.0;
  let mut %px: ptr f64 = null;
  let mut %py: ptr f64 = null;
  let mut %vy: f64 = 0.0;

^entry:
  %px = ptrfield %p, x;
  %py = ptrfield %p, y;
  %tmp = load %px;            // save x
  %vy = load %py;              // read y
  store %px, %vy;              // x <- old y
  store %py, %tmp;             // y <- old x
  ret load %px;
}
```

### 10.9 Nested aggregate navigation **[New in v0.2.1]**
Navigate through a struct containing an array field. Each navigation step is its own statement.
```text
struct @Particle {
  pos: [3] f64;
  mass: f64;
}

fun @get_pos_z(%p: ptr @Particle) : f64 {
  let mut %arr_ptr: ptr [3] f64 = null;
  let mut %elem_p: ptr f64 = null;

^entry:
  %arr_ptr = ptrfield %p, pos;       // struct → array-field pointer
  %elem_p = ptrindex %arr_ptr, 2;    // array → element pointer
  ret load %elem_p;
}
```
Navigation must be staged through locals because `ptrfield` and `ptrindex` results are Atoms, not LValues.

### 10.10 Array of structs via pointer arithmetic **[New in v0.2.1]**
Traverse an array of structs using pointer arithmetic on `ptr @S`. Field accesses through the running pointer are staged through per-field pointers.
```text
struct @Pair { a: i32; b: i32; }

fun @sum_pairs(%data: [3] @Pair) : i32 {
  let mut %p: ptr @Pair = addr %data[0];
  let mut %acc: i32 = 0;
  let mut %pa: ptr i32 = null;
  let mut %pb: ptr i32 = null;
  let %one: i32 = 1;

^entry:
  // ptr @Pair + 1 advances by sizeof(@Pair) = 8 bytes (packed: 4+4)
  %pa = ptrfield %p, a;  %pb = ptrfield %p, b;
  %acc = %acc + load %pa;
  %acc = %acc + load %pb;
  %p = %p + %one;
  %pa = ptrfield %p, a;  %pb = ptrfield %p, b;
  %acc = %acc + load %pa;
  %acc = %acc + load %pb;
  %p = %p + %one;
  %pa = ptrfield %p, a;  %pb = ptrfield %p, b;
  %acc = %acc + load %pa;
  %acc = %acc + load %pb;
  ret %acc;
}
```

### 10.11 Vector arithmetic **[New in v0.2.1]**
Basic lane-wise vector operations. Lane access uses the LValue subscript `%c[i]`.
```text
fun @vec_add() : i32 {
  let %a: <4> i32 = {1, 2, 3, 4};
  let %b: <4> i32 = {10, 20, 30, 40};
  let mut %c: <4> i32 = {0, 0, 0, 0};

^entry:
  %c = %a + %b;                      // {11, 22, 33, 44}  (lane-wise add)
  %c = 2 * %c;                       // {22, 44, 66, 88}  — literal broadcast
  ret %c[2];                         // returns 66
}
```

### 10.12 Vector mask and blend **[New in v0.2.1]**
Use `cmp` to produce a mask and `select` to blend.
```text
fun @clamp_neg(%v: <4> i32) : <4> i32 {
  let %zero: <4> i32 = 0;

^entry:
  // Compare each lane against 0: produces <4> i1 mask
  // cmp < returns 1 for lanes where v[k] < 0
  let %mask: <4> i1 = cmp < %v, %zero;

  // Blend: where mask is 1 (negative), select 0; else keep v
  ret select %mask, %zero, %v;
}
```
For input `{3, -5, 7, -1}`:
- `cmp < %v, %zero` → `{0, 1, 0, 1}`
- `select %mask, %zero, %v` → `{3, 0, 7, 0}`

### 10.13 Vector symbol synthesis **[New in v0.2.1]**
Find a vector of coefficients that transforms one vector into another. Lane access uses subscript syntax directly inside `require`.
```text
fun @find_coefs(%input: <4> i32, %target: <4> i32) : <4> i32 {
  sym %?c: coef <4> i32 in [-10, 10];

  let mut %result: <4> i32 = 0;
^entry:
  %result = %?c * %input;
  require %result[0] == %target[0], "lane 0";
  require %result[1] == %target[1], "lane 1";
  require %result[2] == %target[2], "lane 2";
  require %result[3] == %target[3], "lane 3";
  ret %result;
}
```
Each lane of `%?c` is solved independently: `%?c[k] = %target[k] / %input[k]`.

### 10.14 Mask logic — combining conditions **[New in v0.2.1]**
Bitwise operations on `<N> i1` masks enable complex per-lane conditions.
```text
fun @range_check(%v: <4> i32) : <4> i32 {
  let %lo: <4> i32 = 0;
  let %hi: <4> i32 = 100;
  let %clamp_val: <4> i32 = 50;

^entry:
  // Lanes where v < 0 or v > 100
  let %too_low: <4> i1 = cmp < %v, %lo;
  let %too_high: <4> i1 = cmp > %v, %hi;
  let %out_of_range: <4> i1 = %too_low | %too_high;

  // Replace out-of-range lanes with 50
  ret select %out_of_range, %clamp_val, %v;
}
```

### 10.15 Scalar `cmp` — reified boolean **[New in v0.2.1]**
`cmp` also works on scalar operands, producing `i1`. Note that `select` arms are restricted to `SelectVal` (`RValue | Coef`), not full `Expr`, so the two differences are staged through `let mut` locals before the `select`.
```text
fun @abs_diff(%a: i32, %b: i32) : i32 {
  let mut %diff_ab: i32 = 0;
  let mut %diff_ba: i32 = 0;
  let mut %flag: i1 = 0;
^entry:
  %diff_ab = %a - %b;
  %diff_ba = %b - %a;
  %flag = cmp >= %a, %b;
  ret select %flag, %diff_ab, %diff_ba;
}
```


### 10.16 Implementation notes for backends **[New in v0.2.1]**

These are not part of the language semantics but pin down what backends must do to stay bit-exact with the interpreter and SMT model.

- **Packed struct layout.** `sizeof(@S) = Σ sizeof(field_i)` with no padding (§2.10). The C backend must emit struct declarations with `__attribute__((packed))` (or the platform-equivalent pragma) — otherwise the C compiler will insert alignment padding and the byte offsets used by `ptrfield`, `addr %s.f`, and the SMT memory model will disagree with the compiled binary. This is the same class of risk as the FP precision mismatch fixed in v0.2.0 (`FLT_EVAL_METHOD == 0`): a layout mismatch can survive type-checking and surface only as a differential-test failure.
- **Vectors in C lowering.** Vector parameters, returns, and locals lower to platform SIMD types (e.g., GCC/Clang vector extensions `T __attribute__((vector_size(N*sizeof(T))))`) so the by-value semantics survive ABI. Lane-wise arithmetic maps to the corresponding vector operator. Lane access `%v[i]` lowers directly to the C subscript `v[i]` on the vector-extension type (read and write). `cmp` on vectors lowers to a SIMD compare that produces an all-ones / all-zeros lane (truncated to the `<N> i1` representation).
- **Vectors in WASM lowering.** Use the SIMD-128 proposal where `N * sizeof(T) ≤ 16`. For larger widths the backend may split into multiple SIMD registers; this is permitted but the semantics seen by SymIR remains a single `<N> T`.


## 11. Non-goals for v0.2.1 (planned for later)

- **Heap allocation** (`malloc`/`free` or arena allocation). Pointers are stack-only.
- **`sym` of pointer type**. Pointer symbols require a richer address domain theory.
- **Pointer/integer casts** (`ptr T as iN`, `iN as ptr T`).
- **Aliasing between distinct locals** (deliberately UB; explicit alias modeling deferred).
- **Vector shuffles and permutations**. Operations like `shuffle`, `swizzle`, lane reordering are deferred to a future version. Lane rearrangement can be achieved through sequences of lane reads and writes (`%w[0] = %v[2]; %w[1] = %v[0]; …`).
- **Addressable vectors — planned for v0.2.2**. Vectors are pure value types in v0.2.1: no `ptr <N> T`, no `addr` on vector locals (§2.11). v0.2.2 will add whole-vector `load`/`store` (LLVM-style) while keeping element pointers out; this requires defining `sizeof(<N> T) = N * sizeof(T)` and adding `<N> T` to the loadable-type set.
- **Vectors in aggregates — planned for v0.2.2**. Vectors cannot appear as struct fields or array elements in v0.2.1. Deferred together with addressable vectors above.
- **Horizontal vector reductions**. Operations like `reduce_add`, `reduce_max` across lanes are not supported. Use lane subscripts (`%v[0]`, `%v[1]`, …) to access individual lanes and accumulate manually.
- **Parentheses and general expression trees**.
- **SSA / phi nodes**.
- **Interprocedural calls and summaries**.
