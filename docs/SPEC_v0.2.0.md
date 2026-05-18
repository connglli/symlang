# SymIR v0.2.0 Specification

**Status:** Draft v0.2.0

This document is the complete, standalone specification for SymIR v0.2.0. It supersedes v0.1.0. Sections and rules that are unchanged from v0.1.0 are included verbatim for self-containedness. All additions and changes are marked **[New in v0.2.0]**.


## What's new in v0.2.0

- **Pointer type** `ptr T`: a new first-class type for typed pointers.
- **`addr` expression**: takes the address of a mutable lvalue; yields `ptr T`.
- **`load` expression**: dereferences a pointer; yields the pointee value.
- **`store` instruction**: stores a value through a pointer.
- **`null` literal**: the null pointer constant, typed `ptr T` (inferred from context).
- **Pointer arithmetic**: `ptr T ± iN → ptr T`; `ptr T − ptr T → i64`.
- **Pointer comparison**: `==`, `!=`, `<`, `<=`, `>`, `>=` on pointer operands.
- **New UB rules**: null dereference, out-of-bounds pointer arithmetic, out-of-bounds load/store.
- **SMT encoding extension**: typed memory arrays and abstract address constants.
- **Restrictions** (v0.2.0): the pointee type `T` in `ptr T` is restricted to scalar or pointer types. No pointer to array or struct. No `sym` of pointer type.

Heap allocation remains a non-goal. All addressable storage is stack-resident (local variables).


## 1. Notation and identifier classes

SymIR uses sigils to make identifier categories immediately recognizable:

- `@name` — global identifiers (functions; global type names if desired).
- `%name` — local identifiers (parameters, locals).
- `@?name` — **global symbols** (solver-chosen unknowns).
- `%?name` — **local symbols** (solver-chosen unknowns).
- `^name` — basic block labels.

**Hard rule:** `?` is permitted **only** immediately after `@` or `%` to form `@?` / `%?`. It is forbidden in all other identifiers.


## 2. Key semantic commitments (v0.2.0)

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
- In v0.2.0, `vtrue` and `vfalse` are restricted to **scalar values** (`RValue` or constant `Coef`) to avoid nested expressions.

### 2.8 Memory model **[New in v0.2.0]**

v0.2.0 introduces a typed, stack-only memory:

- **Stack-only**: all addressable storage is a `let mut` local within the current function. Heap allocation is not supported.
- **No cross-object aliasing**: a pointer derived from `addr %x` can never alias a pointer derived from `addr %y` for distinct locals `%x` and `%y`. Cross-object pointer arithmetic is **UB**.
- **Typed memory regions**: the memory is conceptually partitioned by pointee type. Each distinct pointee type `T` has its own independent memory region. This is sound because pointer/integer casts are forbidden.
- **Pointer width**: all pointers are 64-bit values (BV64 in the SMT model), regardless of pointee type.

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

### 2.10 Pointer arithmetic **[New in v0.2.0]**

Pointer arithmetic is typed and stride-aware:

- `ptr T + n` advances the address by `n * sizeof(T)` bytes, where `n` is an integer operand.
- `ptr T - n` retreats the address by `n * sizeof(T)` bytes.
- `ptr T - ptr T` yields the element distance between two pointers of the same type, as `i64`.
- Arithmetic is valid only within the bounds of the originating object. Out-of-bounds arithmetic is UB.
- `integer + ptr T` (with the integer on the left) is **not supported**. The pointer must be the left operand.

**`sizeof`** is defined as follows (used implicitly by pointer arithmetic and explicitly in UB bounds checks):

| Type | `sizeof` |
|------|---------|
| `iN` | `⌈N / 8⌉` bytes (e.g., `i32` → 4, `i64` → 8, `i1` → 1) |
| `f32` | 4 bytes |
| `f64` | 8 bytes |
| `ptr T` | 8 bytes (64-bit address) |

Note: no struct or array pointees in v0.2.0, so `sizeof` of aggregates is not needed for pointer arithmetic.


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

**[New in v0.2.0]** New keywords: `ptr`, `addr`, `load`, `store`, `null`.

*Note: For the typing of literals, see [Section 6.9](#69-literal-typing-and-inference).*

### 3.2 Types

```ebnf
Type        := IntType | FloatType | StructName | ArrayType | PtrType ;
IntType     := "i" Nat ;
FloatType   := "f32" | "f64" ;
ArrayType   := "[" Nat "]" Type ;
StructName  := GlobalId ;
PtrType     := "ptr" PointeeType ;          (* [New in v0.2.0] *)
PointeeType := IntType | FloatType | PtrType ;   (* scalars and pointers only *)
```

Notes:
- Arrays are fixed-size.
- `Nat` is a literal (no symbolic sizes in v0.2.0).
- **Multidimensional arrays** are supported by nesting `ArrayType`.
- **Integer Types:** `i1`, `i8`, `i16`, `i32`, `i64`, and arbitrary `iN` are all valid.
- **Floating-point Types:** `f32` (IEEE 754 single-precision) and `f64` (IEEE 754 double-precision).
- **`ptr T`** **[New in v0.2.0]**: pointer to a scalar or pointer type. Pointer to array (`ptr [N] T`) and pointer to struct (`ptr @S`) are **not supported** in v0.2.0.
  - Valid: `ptr i32`, `ptr f64`, `ptr ptr i32`, `ptr ptr ptr i8`.
  - Invalid: `ptr [4] i32`, `ptr @Point`.

### 3.3 Program structure
```ebnf
Program     := (StructDecl | FunDecl)+ ;

StructDecl  := "struct" GlobalId "{" FieldDecl+ "}" ;
FieldDecl   := Ident ":" Type ";" ;

FunDecl     := "fun" GlobalId "(" ParamList? ")" ":" Type
               "{" SymDecl* LetDecl* Block+ "}" ;

ParamList   := Param ("," Param)* ;
Param       := LocalId ":" Type ;
```

Parameters may be of type `ptr T`. A pointer parameter represents an externally-provided address; the caller is responsible for ensuring it points to valid, appropriately-typed storage.

### 3.4 Declarations

#### 3.4.1 Symbols (`sym` implies immutable `let`)
A `sym` declaration introduces a solver-chosen unknown. Symbols are **immutable** (there is no `sym mut` in v0.2.0).

```ebnf
SymDecl     := "sym" SymId ":" SymKind Type Domain? ";" ;
SymKind     := "value" | "coef" | "index" ;
Domain      := "in" Interval | "in" Set ;
Interval    := "[" IntLit "," IntLit "]" ;
Set         := "{" IntLit ("," IntLit)* "}" ;
```

**[New in v0.2.0] Restriction:** `sym` declarations of pointer type (`ptr T`) are **not allowed** in v0.2.0. Pointer values must be derived from `addr` expressions or received as parameters. Integer symbols (`coef`, `index`) may still be used as operands in pointer arithmetic.

#### 3.4.2 Locals (`let` and `let mut`)
```ebnf
LetDecl     := "let" ("mut")? LocalId ":" Type ("=" InitVal)? ";" ;
InitVal     := ScalarInit | "null" | "undef" | BraceInit ;    (* null: [New in v0.2.0] *)
ScalarInit  := IntLit | FloatLit | SymId | LocalId ;
BraceInit   := "{" InitVal ("," InitVal)* "}" ;
```

**[New in v0.2.0]** `null` is a valid `InitVal` for any `ptr T` local. It represents the null pointer (address 0). Using `null` as the initial value for a non-pointer type is a type error.

Local initialization:
- **Scalar init** for `ptr T`: only `null`, a `LocalId` of type `ptr T`, or (from parameters) applies. Integer literals are not valid initializers for pointer locals.
- **Broadcast**: broadcast init is not valid for pointer types.
- **Brace init**: not applicable to pointer types.
- `undef` indicates an uninitialized value; **reading** `undef` is UB in v0.2.0.

Mutability:
- `let <id>` creates an **immutable binding**: the pointer value cannot be reassigned. However, the pointee can still be mutated via `store`.
- `let mut <id>` creates a **mutable binding**: the pointer value can be reassigned.

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
StoreInstr  := "store" RValue "," Expr ";" ;    (* [New in v0.2.0] *)
```

**`store` [New in v0.2.0]**: `store <ptr>, <val>` writes `val` to the memory location addressed by `ptr`.
- `ptr` must have type `ptr T` for some scalar or pointer type `T`.
- `val` (the `Expr`) must have type `T`.
- `store` is a **statement**; it does not produce a value.
- `store` through `null` or an out-of-bounds address is UB.


## 5. LValues, conditions, expressions

### 5.1 LValues and access paths
```ebnf
LValue      := Base Access* ;
Base        := LocalId ;                   (* v0.2.0: locals/params only; no global storage *)
Access      := "[" Index "]" | "." Ident ;
Index       := IntLit | LocalId | SymId ;
```

### 5.2 Conditions
```ebnf
Cond        := Expr RelOp Expr ;
RelOp       := "==" | "!=" | "<" | "<=" | ">" | ">=" ;
```

**[New in v0.2.0]** Pointer comparison: both operands of a `Cond` may be of the same `ptr T` type. Comparing pointers of different pointer types is a type error. `==` and `!=` are always defined. `<`, `<=`, `>`, `>=` are defined only within the same originating object; cross-object relational comparison is UB (rule 14, §7.5).

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
            | AddrOf                        (* [New in v0.2.0] *)
            | Load                          (* [New in v0.2.0] *)
            | Coef
            | RValue ;

Select      := "select" Cond "," SelectVal "," SelectVal ;
SelectVal   := RValue | Coef ;     (* v0.2.0 restriction: no nested expressions *)

Cast        := RValue "as" Type ;

AddrOf      := "addr" LValue ;              (* [New in v0.2.0] *)
Load        := "load" RValue ;              (* [New in v0.2.0] *)

Coef        := IntLit | FloatLit | LocalId | SymId | "null" ;   (* null: [New in v0.2.0] *)
RValue      := LValue ;
```

**`addr` [New in v0.2.0]**: takes the address of an lvalue. The lvalue's root must be a `let mut` local. The result type is `ptr T` where `T` is the type of the lvalue. `addr` may appear wherever an `Atom` is expected; it cannot appear in `Coef` position (left of a binary `*`, `/`, etc.).

**`load` [New in v0.2.0]**: dereferences a pointer. The `RValue` must have type `ptr T` for some scalar or pointer type `T`. The result type is `T`. `load` may appear wherever an `Atom` is expected.

**`null` [New in v0.2.0]**: the null pointer constant. Its type is `ptr T` for any `T`, inferred from context. There is no default type for `null`; it must always appear in a typed context (e.g., assigned to a typed `ptr T` local or used in a pointer comparison). Using `null` as an operand of `+`, `-`, `*`, etc. is a type error.

**Pointer arithmetic [New in v0.2.0]**: within the `Expr := Atom (("+"|"-") Atom)*` grammar, the following additional type combinations are valid (see [Section 6](#6-typing-and-well-formedness-v020) for full rules):
- `ptr T + iN` → `ptr T` (advances by `sizeof(T)` per unit)
- `ptr T - iN` → `ptr T` (retreats by `sizeof(T)` per unit)
- `ptr T - ptr T` → `i64` (signed element distance)

**Restrictions on pointer Atoms**: pointers may not appear as the left operand in `Coef "*" RValue`, `Coef "/" RValue`, `Coef "%" RValue`, or any bitwise/shift atom. Pointer multiplication, division, bitwise ops, and shifts are **not supported**.


## 6. Typing and well-formedness (v0.2.0)

### 6.1 Scalar arithmetic restriction
Arithmetic (`Expr`) is defined over **scalar integer and floating-point leaves**. Arrays and structs exist to provide addressable structure, but only their scalar elements/fields may be used in arithmetic or comparisons. Pointers may appear in arithmetic only under the rules of Section 2.9.

### 6.2 LValue typing
- If `%x : T` then `%x` has type `T`.
- If `lv : [N] U` then `lv[i] : U` (index must be integer-typed; bounds checked at runtime as UB).
- If `lv : S` and struct `S` has field `f : U` then `lv.f : U`.

### 6.3 `select` typing
`select c, a, b` is well-typed iff:
- `c` is a boolean condition, and
- `a` and `b` have the same scalar type (integer or floating-point).
The result has that type.

### 6.4 `as` typing
`rval as T` is well-typed iff:
- `rval` is a scalar (integer or floating-point).
- `T` is a scalar (integer or floating-point).
Pointer/integer casts (`ptr T as iN`, `iN as ptr T`) are **not supported** in v0.2.0.

Runtime cast behavior:
- **Integer resize**: truncation (narrowing) or sign/zero extension (widening). Never UB.
- **Integer-to-float** (`iN as fM`): rounded with RNE. Always produces a finite result (integers are bounded).
- **Float-to-float resize** (`f32 as f64`): exact (widening). (`f64 as f32`): rounded with RNE; UB if the result would overflow to ±∞.
- **Float-to-integer** (`fN as iM`): truncation toward zero. UB if the truncated value is outside the representable range of `iM` (see §7.4 rule 8).

### 6.5 Bitwise and shift typing
- `Coef op RValue` (`&`, `|`, `^`, `<<`, `>>`, `>>>`): well-typed iff both operands are scalar integers of the same bit-width.
- `~ RValue`: well-typed iff the operand is a scalar integer.
- Pointer operands are not valid for bitwise or shift operations.

### 6.6 Mutability rules
- The LHS of `=` must be an lvalue rooted at a `let mut` local.
- `sym` identifiers and `let` (immutable) locals cannot appear on the LHS.
- Parameters are immutable and cannot appear on the LHS.
- `store <ptr>, <val>`: the `ptr` operand may point to any mutable storage (local or received via parameter). The mutability constraint is enforced by the `addr` restriction (only `let mut` lvalues can have their address taken within the function).

### 6.7 Floating-point arithmetic typing
- `Expr` involving floating-point atoms must be homogeneous: all atoms in the same `+`/`-` chain must have the exact same floating-point type.
- Mixed arithmetic between different floating-point widths or between integers and floats is forbidden without explicit `as` casts.

### 6.8 Pointer typing rules **[New in v0.2.0]**

#### 6.8.1 `ptr T` as a type
`ptr T` is well-formed iff `T` is a scalar (`iN`, `f32`, `f64`) or a `ptr T'` for some well-formed `ptr T'`. Recursive pointer types (`ptr ptr ... T`) are well-formed.

#### 6.8.2 `addr` typing
`addr <lv>` is well-typed iff:
- The root of `lv` is declared with `let mut`.
- The type of `lv` is a valid `PointeeType` (scalar or pointer).
- Result type: `ptr T` where `T = type(lv)`.

Examples:
```
let mut %x: i32 = 0;          addr %x       : ptr i32
let mut %arr: [4] i32 = 0;    addr %arr[%i] : ptr i32
let mut %p: ptr i32 = null;   addr %p       : ptr ptr i32
```

#### 6.8.3 `load` typing
`load <rval>` is well-typed iff:
- `rval` has type `ptr T` for some valid `PointeeType` `T`.
- Result type: `T`.

#### 6.8.4 `store` typing
`store <ptr>, <val>` is well-typed iff:
- `ptr` has type `ptr T`.
- `val` has type `T`.
- `T` is a valid `PointeeType`.

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

#### 6.8.7 Pointer comparison typing
All six relational operators are syntactically valid on operands of the same `ptr T` type. Result is boolean.

Runtime behavior:
- `==` and `!=` are always defined: two pointers are equal iff they hold the same address. Comparing pointers of different objects with `==`/`!=` is defined (yields `false`/`true` respectively).
- `<`, `<=`, `>`, `>=` are defined only for pointers within the **same originating object** (same `addr` root). Cross-object relational comparison is **UB** (rule 14 in §7.5).

#### 6.8.8 Pointer locals and parameters
- A local `let %p: ptr T` is an immutable binding to a pointer value (the pointer itself cannot be reassigned; the pointee may still be `store`d through).
- A local `let mut %p: ptr T` is a mutable binding (the pointer can be reassigned via `%p = <expr>`).
- A parameter `%p: ptr T` is immutable (the binding cannot appear on the LHS of `=`).

### 6.9 Literal typing and inference
Literals do not have fixed types but are inferred from their context:
- **Integer Literals:** Inferred to match the bit-width of the operation target or neighboring operands. Default: `i32`.
- **Floating-point Literals:** Inferred to match the target or neighbors. Default: `f32`.
- **`null`:** Inferred to match the `ptr T` context. No default; context must be available.
- **Homogeneity Requirement:** Since mixed arithmetic is forbidden, a literal in `x + <lit>` is inferred to match the type of `x`.


## 7. Strict UB rules (v0.2.0)

UB is checked during symbolic execution along the chosen path. Any UB makes the path infeasible.

### 7.1 UB conditions (inherited from v0.1.0)
1. **Integer division/modulo by zero**: in `k / x` or `k % x` where `k` and `x` are integers, UB if `x == 0`. (For floating-point, see §7.4 rules 6–7.)
2. **Out-of-bounds array access**: for `a[i]` where `a : [N] T`, UB if `i < 0` or `i >= N`.
3. **Reading `undef`**: reading a location whose stored value is `undef` is UB.
4. **Signed integer overflow**: `+`, `-`, `*` that produce a value outside the representable range of the target bit-width. Also: `INT_MIN / -1`.
5. **Overshift**: in `x << n`, `x >> n`, `x >>> n`, UB if `n < 0` or `n >= width(x)`.

### 7.2 `select` and strict UB (lazy)
For `select c, a, b`:
- Evaluate `c` first; UB in `c` makes the path infeasible.
- If `c` is true, evaluate only `a`; UB in `a` makes the path infeasible. `b` is not evaluated.
- If `c` is false, evaluate only `b`; UB in `b` makes the path infeasible. `a` is not evaluated.

### 7.3 Floating-point arithmetic semantics
See §2.9 for the full floating-point value model. Key points:
- All operations use **RNE** rounding except `%` (see below).
- `%` is C's `fmod` (truncated-quotient), **not** IEEE 754 `remainder` (`fp.rem`). Result sign matches the dividend sign, consistent with integer `%`.
- Comparisons are total over the finite domain (NaN never occurs). `+0.0 == -0.0` is `true`.

### 7.4 Floating-point UB rules

6. **FP overflow (±∞ result)**: any arithmetic operation (`+`, `-`, `*`, `/`) whose IEEE 754 result (under RNE) would be ±∞ is UB. This covers finite-operand overflow and division of any non-zero value by ±0.0.

7. **FP invalid operation (NaN result)**: any operation whose IEEE 754 result would be NaN is UB. This covers `±0.0 / ±0.0` and `x % ±0.0` for any `x`.

8. **Float-to-integer out-of-range**: `fN as iM` is UB if the float value, after truncation toward zero, is outside the representable range of `iM`. (Non-finite values are already UB per rules 6–7, but would also trigger this rule.)

### 7.5 Pointer UB rules **[New in v0.2.0]**

9. **Null pointer dereference**: `load <ptr>` or `store <ptr>, <val>` where `ptr` evaluates to `null` (address 0) is UB.

10. **Out-of-bounds pointer arithmetic**: let `%p = addr <lv>` where `lv` has type `[N] T` (an array element pointer) or a scalar type (a single-element object of size 1). For pointer arithmetic `%p + n`:
    - Let `base` be the start address of the originating object and `count` be the number of elements in that object.
    - UB if the resulting address falls outside `[base, base + count * sizeof(T))`.
    - The "one-past-the-end" address `base + count * sizeof(T)` is a valid non-dereferenceable address (valid for arithmetic; UB to load/store).

11. **Out-of-bounds load/store**: `load <ptr>` or `store <ptr>, <val>` where `ptr` points outside the bounds of the originating object (including the one-past-the-end address) is UB.

12. **Cross-object pointer arithmetic**: forming a pointer by arithmetic that crosses from one local variable's storage into another is UB. The memory regions of distinct local variables do not overlap.

13. **Uninitialized pointer dereference**: `load <ptr>` or `store <ptr>, <val>` where `ptr` itself is `undef` is UB (follows from rule 3 applied to the pointer value).

14. **Cross-object pointer comparison**: comparing two pointers derived from different originating objects with `<`, `<=`, `>`, `>=` is UB. Equality (`==`, `!=`) between pointers of different objects is well-defined and always produces `false` / `true` respectively (distinct objects occupy disjoint address ranges per the non-overlap axioms). Only `<`/`<=`/`>`/`>=` comparisons between pointers of the same originating object are defined.

    **Rationale**: the SMT model assigns abstract address constants that satisfy non-overlap constraints but imposes no ordering between distinct objects. Permitting `<`/`>`/etc. across objects would allow `assume px < py` to spuriously constrain the abstract address assignment, producing results that are model-dependent and meaningless. Declaring it UB prevents this. This matches the C standard (§6.5.8p5).


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
- `Store`: mapping from local lvalues to symbolic terms (unchanged from v0.1.0).
- `Mem[T]`: a family of symbolic memory arrays, one per pointee type `T` **[New in v0.2.0]**.
  - Each `Mem[T]` is a function from 64-bit BV addresses to `T`-typed BV values.
  - In SMT: `Mem[T] : (Array (_ BitVec 64) (_ BitVec (8*sizeof(T))))`.
- `PC`: feasibility constraints (conjunction).
- `REQ`: property constraints (conjunction).

### 9.2 Constraint sources on the chosen path
While executing along `π`:
- **Branches**: for `br cond, ^t, ^f;`, conjoin `cond` (or `not(cond)`) to `PC` based on which successor is on `π`.
- **Assumptions**: `assume c;` conjoins `c` to `PC`.
- **Requirements**: `require c;` conjoins `c` to `REQ`.
- **Strict UB**: any UB encountered sets `PC := false`.
- **`store` [New in v0.2.0]**: `store %p, v;` updates `Mem[T]` functionally: `Mem[T] := store(Mem[T], addr(%p), v)`.

### 9.3 Solve goal
Let `DOM` be the conjunction of all symbol-domain constraints. The solver is invoked on:

`DOM ∧ PC ∧ REQ`

A satisfying model yields concrete values for all symbols.

### 9.4 SMT encoding of pointers **[New in v0.2.0]**

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

#### 9.4.3 `addr` evaluation
- `addr %x` evaluates to `addr_%x` (the base address constant for `%x`).
- `addr %arr[i]` evaluates to `bvadd(addr_%arr, bvmul(i_bv, sizeof(element)))` where `i_bv` is the BV encoding of index `i`.
- `addr %p` where `%p : ptr T` evaluates to `addr_%p` (a pointer-to-pointer; the address of the pointer variable itself).

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


## 10. Examples

This section demonstrates the pointer features added in v0.2.0. All examples follow v0.2.0 constraints.

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

^entry:
  store load %pp, %val;      // *(*pp) = val  i.e.  *p = val  i.e.  x = val
  ret load load %pp;         // return **pp
}
```
`load %pp` dereferences `%pp` to get `%p` (a `ptr i32`). `load load %pp` dereferences twice to get the `i32` value. `store load %pp, %val` stores through the inner pointer.

Note: `load %pp` is an `Atom` of type `ptr i32`; `load load %pp` is legal because the outer `load` operand is an `RValue` (an `LValue`), but `load %pp` is not an `LValue` — it is a value-producing `Atom`. Thus `load load %pp` must be written by staging through a local:

```text
fun @deref2_staged(%val: i32) : i32 {
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

### 10.6 Pointer arithmetic bounds (loop-style path)
Demonstrating that on a path that advances a pointer `N` times through a `[N] i32` array, all loads are in-bounds.
```text
fun @scan(%arr: [3] i32) : i32 {
  let mut %p: ptr i32 = addr %arr[0];
  let mut %sum: i32 = 0;
  let %one: i32 = 1;
  let mut %tmp: i32 = 0;

^entry:
  br ^loop;

^loop:
  br %p == addr %arr[3], ^exit, ^body;    // one-past-the-end check

^body:
  %tmp = load %p;
  %sum = %sum + %tmp;
  %p = %p + %one;
  br ^loop;

^exit:
  ret %sum;
}
```
On a path that visits `^body` exactly 3 times, the feasibility conditions ensure `%p` stays within `[addr %arr[0], addr %arr[3])`.

### 10.7 Pointer difference
```text
fun @ptr_diff(%arr: [8] i32) : i64 {
  let mut %p: ptr i32 = addr %arr[0];
  let mut %q: ptr i32 = addr %arr[5];

^entry:
  ret %q - %p;    // element distance: 5
}
```
`%q - %p` yields `i64` value `5` (not `20`, because the result is in elements, not bytes).

### 10.8 Synthesis with pointer offset symbol
```text
fun @find_slot(%arr: [8] i32, %target: i32) : i32 {
  sym %?off: index i32 in [0, 7];
  let mut %p: ptr i32 = addr %arr[0];

^entry:
  %p = %p + %?off;
  require load %p == %target, "slot contains target";
  ret %?off;
}
```
The solver finds the index `%?off` such that `%arr[%?off] == %target` on the chosen path.


## 11. Non-goals for v0.2.0 (planned for later)

- **Heap allocation** (`malloc`/`free` or arena allocation). Pointers are stack-only in v0.2.0.
- **Pointer to aggregate** (`ptr [N] T`, `ptr @S`). Planned for v0.2.1.
- **`sym` of pointer type**. Pointer symbols require a richer address domain theory.
- **Pointer/integer casts** (`ptr T as iN`, `iN as ptr T`).
- **Aliasing between distinct locals** (deliberately UB in v0.2.0; explicit alias modeling deferred).
- **SIMD / vector types**. Fixed-width vector types (e.g., `<4 x i32>`, `<8 x f32>`) and lane-wise operations are not supported. SymIR is scalar-only; vectorization is left to the lowering target (C compiler, WASM engine).
- **Parentheses and general expression trees**.
- **SSA / phi nodes**.
- **Interprocedural calls and summaries**.
