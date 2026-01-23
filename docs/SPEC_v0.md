# SymIR v0 Specification

**Status:** Draft v0

SymIR is a CFG-based symbolic IR for **program templates**. A SymIR function contains **symbols** (unknowns) that are solved by an SMT solver under constraints derived from:

1) a **specific execution path** (a sequence of basic blocks / edges, potentially with repeats), and
2) user-specified **properties** that must hold on that path.

The tool symbolically executes the template **only along the chosen path**, collects constraints, and solves them to obtain a concrete instantiation of the symbols (and, if desired, other unconstrained inputs). Basic blocks not on the chosen path contribute no constraints (except global symbol-domain constraints).

This v0 surface syntax adopts LLVM-style annotations and branching, while remaining **non-SSA** and using a **mutable store** semantics.


## 1. Notation and identifier classes

SymIR uses sigils to make identifier categories immediately recognizable:

- `@name` — global identifiers (functions; global type names if desired).
- `%name` — local identifiers (parameters, locals).
- `@?name` — **global symbols** (solver-chosen unknowns).
- `%?name` — **local symbols** (solver-chosen unknowns).
- `^name` — basic block labels.

**Hard rule:** `?` is permitted **only** immediately after `@` or `%` to form `@?` / `%?`. It is forbidden in all other identifiers.


## 2. Key semantic commitments (v0)

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
Integer division and modulo round toward 0 (C-like truncation semantics).

### 2.6 Strict undefined behavior (UB)
SymIR uses **strict UB** on the chosen path:
- If UB occurs during evaluation of any statement/condition on `π`, the path becomes **infeasible** and is pruned.

### 2.7 `select` expression (lazy)
`select` is supported as an atom:

- `select <cond>, <vtrue>, <vfalse>`
- `select` is **lazy**: only the selected arm is evaluated.
- In v0, `vtrue` and `vfalse` are restricted to **scalar values** (`RValue` or constant `Coef`) to avoid nested expressions.


## 3. Concrete syntax

### 3.1 Lexical
- `Ident` : `[A-Za-z_][A-Za-z0-9_]*`
- `Nat` : `[0-9]+`
- `IntLit` : `"-"? Nat`
- `StringLit` : double-quoted string (implementation-defined escapes)

### 3.2 Types
```ebnf
Type        := IntType | StructName | ArrayType ;
IntType     := "i32" | "i64" | "i" Nat ;
ArrayType   := "[" Nat "]" Type ;
StructName  := GlobalId ;     (* recommended: use @TypeName for struct names *)
```

Notes:
- Arrays are fixed-size.
- `Nat` is a literal (no symbolic sizes in v0).
- **Multidimensional arrays** are supported by nesting `ArrayType`. For example, `[3][4] i32` represents a 3x4 grid of integers. Whitespace between brackets is optional.

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

### 3.4 Declarations

#### 3.4.1 Symbols (`sym` implies immutable `let`)
A `sym` declaration introduces a solver-chosen unknown. Symbols are **immutable** (there is no `sym mut` in v0).

```ebnf
SymDecl     := "sym" SymId ":" SymKind Type Domain? ";" ;
SymKind     := "value" | "coef" | "index" ;
Domain      := "in" Interval | "in" Set ;
Interval    := "[" IntLit "," IntLit "]" ;
Set         := "{" IntLit ("," IntLit)* "}" ;
```

Domains are strongly recommended (especially for `coef` and `index`) to keep solving practical.

#### 3.4.2 Locals (`let` and `let mut`)
```ebnf
LetDecl     := "let" ("mut")? LocalId ":" Type ("=" InitVal)? ";" ;
InitVal     := ScalarInit | "undef" | BraceInit ;
ScalarInit  := IntLit | SymId | LocalId ;
BraceInit   := "{" InitVal ("," InitVal)* "}" ;
```

Local initialization:
- **Broadcast:** If a `ScalarInit` is provided for an array or struct type, the value is broadcast to all leaf scalar elements. For example, `let mut %v: [4] i32 = -1;` initializes every element to `-1`.
- **Brace Initialization:** `BraceInit` allows positional initialization of arrays and structs.
  - **Arrays:** `{v0, v1, ..., vN-1}` initializes an array of size `N`. The number of elements in the braces must **exactly match** the array size.
  - **Structs:** `{f0, f1, ..., fK-1}` initializes a struct with `K` fields in the order they were declared. The number of elements must **exactly match** the field count.
  - **Recursive:** Braces can be nested to initialize multidimensional arrays or nested structs (e.g., `let %m: [2][2] i32 = { {1, 2}, {3, 4} };`).
- **Empty Braces:** `{}` is **disallowed**.
- `undef` indicates an uninitialized value; **reading** `undef` is UB in v0.

Mutability:
- `let <id>` creates an immutable binding.
- `let mut <id>` creates a mutable storage cell.

**Parameters are immutable by default** and may not appear on the LHS of assignment.


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

Branching is LLVM-style:
- Conditional: `br <cond>, ^then, ^else;`
- Unconditional: `br ^dest;`

### 4.2 Instructions
```ebnf
Instr       := AssignInstr | AssumeInstr | RequireInstr ;

AssignInstr := LValue "=" Expr ";" ;
AssumeInstr := "assume" Cond ";" ;
RequireInstr:= "require" Cond ("," StringLit)? ";" ;
```


## 5. LValues, conditions, expressions

### 5.1 LValues and access paths
```ebnf
LValue      := Base Access* ;
Base        := LocalId ;                   (* v0: locals/params only; no global storage *)
Access      := "[" Index "]" | "." Ident ;
Index       := IntLit | LocalId | SymId ;
```

### 5.2 Conditions
```ebnf
Cond        := Expr RelOp Expr ;
RelOp       := "==" | "!=" | "<" | "<=" | ">" | ">=" ;
```

### 5.3 Expressions (no parentheses, left-to-right)
```ebnf
Expr        := Atom (("+" | "-") Atom)* ;

Atom        := Coef "*" RValue
            | Coef "/" RValue
            | Coef "%" RValue
            | Select
            | Coef
            | RValue ;

Select      := "select" Cond "," SelectVal "," SelectVal ;
SelectVal   := RValue | Coef ;     (* v0 restriction: no nested expressions *)

Coef        := IntLit | LocalId | SymId ;
RValue      := LValue ;
```

Evaluation order:
- `Expr` is evaluated strictly left-to-right: `(((a0 op a1) op a2) ...)`.
- `select` evaluates its `Cond` first, then evaluates only the chosen arm.


## 6. Typing and well-formedness (v0)

### 6.1 Scalar arithmetic restriction
Arithmetic (`Expr`) is defined only over **scalar integer leaves**. Arrays and structs exist to provide addressable structure, but only their scalar elements/fields may be used in arithmetic or comparisons.

### 6.2 LValue typing
- If `%x : T` then `%x` has type `T`.
- If `lv : [N] U` then `lv[i] : U` (index must be integer-typed; bounds checked at runtime as UB).
- If `lv : S` and struct `S` has field `f : U` then `lv.f : U`.

### 6.3 `select` typing
`select c, a, b` is well-typed iff:
- `c` is a boolean condition, and
- `a` and `b` have the same integer type.
The result has that type.

### 6.4 Mutability rules
- The LHS of `=` must be an lvalue rooted at a `let mut` local.
- `sym` identifiers and `let` (immutable) locals cannot appear on the LHS.
- Parameters are immutable and cannot appear on the LHS.


## 7. Strict UB rules (v0)

UB is checked during symbolic execution along the chosen path. Any UB makes the path infeasible.

### 7.1 UB conditions
1. **Division/modulo by zero**
   In `k / x` or `k % x`, UB if `x == 0`.

2. **Out-of-bounds array access**
   For `a[i]` where `a : [N] T`, UB if `i < 0` or `i >= N`.

3. **Reading `undef`**
   Reading a location whose current stored value is `undef` is UB.

### 7.2 `select` and strict UB (lazy)
For `select c, a, b`:
- Evaluate `c` first. UB in `c` makes the path infeasible.
- If `c` is true, evaluate only `a`. UB in `a` makes the path infeasible. `b` is not evaluated.
- If `c` is false, evaluate only `b`. UB in `b` makes the path infeasible. `a` is not evaluated.


## 8. Integer division and modulo (round toward 0)

For integers `A` and `B` with `B != 0`:

- `Q = trunc(A / B)` (round toward 0)
- `R = A - Q*B`
- `A % B = R`

Properties:
- `|R| < |B|`
- `R` has the same sign as `A` (or is 0)

This semantics is used for all `/` and `%` atoms.


## 9. Path-based symbolic execution and constraint extraction

### 9.1 Symbolic state
The executor maintains:
- `Store`: mapping from locations (lvalues) to symbolic terms
- `PC`: feasibility constraints (conjunction)
- `REQ`: property constraints (conjunction)

### 9.2 Constraint sources on the chosen path
While executing along the chosen path `π`, collect constraints from:
- **Branches**:
  For `br cond, ^t, ^f;`, if the next block on `π` is `^t`, conjoin `cond` to `PC`; if it is `^f`, conjoin `not(cond)` to `PC`.
- **Assumptions**: `assume c;` conjoins `c` to `PC`.
- **Requirements**: `require c;` conjoins `c` to `REQ`.
- **Strict UB**: any UB encountered makes `PC := false` (path infeasible).

Unconditional `br ^dest;` contributes no constraint but must match the provided path.

Blocks not on `π` do not contribute constraints.

### 9.3 Solve goal
Let `DOM` be the conjunction of all symbol-domain constraints from `sym` declarations. The solver is invoked on:

`DOM ∧ PC ∧ REQ`

A satisfying model yields concrete values for `@?` / `%?` symbols (and any other unconstrained values the implementation chooses to solve for).


## 10. Examples

This section provides additional examples of common patterns and “classical” problems expressed in SymIR v0. All examples follow v0 constraints:
- no parentheses in expressions (left-to-right evaluation),
- strict UB,
- `div`/`mod` round toward 0,
- `select` is lazy and its arms are scalar values (lvalues or constants).

### 10.1 Structs, symbols, and simple arithmetic
```text
struct @Point { x: i32; y: i32; }
struct @Rect  { tl: @Point; br: @Point; }

fun @f0(%arr: [10] i32, %r: @Rect) : i32 {
  sym @?c4: coef i32 in [-16, 16];
  let mut %tmp: i32 = 100;

^entry:
  %arr[0] = 12 * %r.tl.x + @?c4 * %tmp;
  require %arr[0] >= 0, "nonnegative output";
  ret %arr[0];
}
```

### 10.2 `select` as a lazy value choice (absolute value)
Compute `abs(x)` without introducing CFG branches. This uses a temporary and a conditional negation.
```text
fun @abs(%x: i32) : i32 {
  let %zero: i32 = 0;
  let mut %neg: i32 = 0;

^entry:
  %neg = %zero - %x;
  ret select %x >= 0, %x, %neg;
}
```

### 10.3 Maximum of two values (classic)
```text
fun @max2(%a: i32, %b: i32) : i32 {
^entry:
  ret select %a >= %b, %a, %b;
}
```

### 10.4 Saturating clamp (classic)
Clamp `%x` into `[0, 255]` using nested selects with v0 restrictions (arms are scalar values).
```text
fun @clamp_u8(%x: i32) : i32 {
  let %lo: i32 = 0;
  let %hi: i32 = 255;
  let mut %t: i32 = 0;

^entry:
  %t = select %x < %lo, %lo, %x;
  ret select %t > %hi, %hi, %t;
}
```

### 10.5 Conditional division with strict UB (classic guard pattern)
Because UB is strict, division must be guarded on the chosen path. A common pattern is to branch around the division.
```text
fun @safe_div(%num: i32, %den: i32) : i32 {
  let %zero: i32 = 0;
  let mut %q: i32 = 0;

^entry:
  br %den == 0, ^zero, ^nonzero;

^zero:
  ret %zero;

^nonzero:
  %q = %num / %den;
  ret %q;
}
```
On a chosen path that reaches `^nonzero`, feasibility constraints will include `%den != 0`.

### 10.6 One-step Euclidean update (gcd-style) using `mod`
This example performs a single Euclidean step `(a, b) := (b, a % b)` with strict UB. The `%` operator rounds toward 0 and is UB when the divisor is 0.
```text
fun @gcd_step(%a: i32, %b: i32) : i32 {
  let mut %r: i32 = 0;

^entry:
  // Strict UB if %b == 0
  %r = %a % %b;
  // Return the remainder as a representative “step output”
  ret %r;
}
```
For synthesis along a path that includes this statement, you typically add `assume %b != 0;` (or ensure the branch path implies it).

### 10.7 Array indexing with strict UB (bounds discipline)
```text
fun @get0(%arr: [4] i32, %i: i32) : i32 {
  let %zero: i32 = 0;

^entry:
  // Strict UB if %i is out of bounds
  ret %arr[%i];
}
```
For any path that reads `%arr[%i]`, feasibility must include `0 <= %i` and `%i < 4`.

### 10.8 Local initialization from a symbol and a parameter
```text
fun @init(%p: i32) : i32 {
  sym %?k: value i32 in [0, 10];
  let mut %x: i32 = %p;
  let %y: i32 = %?k;

^entry:
  %x = %x + %y;
  ret %x;
}
```

### 10.9 Template synthesis “toy” (linear regression-style coefficient fitting)
Fit unknown coefficients so that, along the chosen path, a linear model matches a required output.
```text
fun @linfit(%x: i32) : i32 {
  sym @?a: coef i32 in [-8, 8];
  sym @?b: coef i32 in [-8, 8];
  let %k: i32 = 1;

^entry:
  // y = a*x + b
  require @?a * %x + @?b == 10, "fit y( x ) = 10 for this path";
  ret @?a * %x + @?b;
}
```
A solver model concretizes `@?a` and `@?b` to satisfy the `require` constraint (subject to domains).

### 10.10 Explicit path with loop back-edge (illustrative)
This demonstrates how a path may revisit blocks. The concrete feasibility of a path depends on how mutable state changes across visits.
```text
fun @loop_demo(%n: i32) : i32 {
  let %one: i32 = 1;
  let mut %i: i32 = 0;

^entry:
  br ^b1;

^b1:
  br %i < %n, ^body, ^exit;

^body:
  %i = %i + %one;
  br ^b1;

^exit:
  require %i == %n, "loop counted to n on this path";
  ret %i;
}
```
A path like `^entry -> ^b1 -> ^body -> ^b1 -> ^body -> ... -> ^exit` is feasible only if the number of `^body` visits makes `%i < %n` false at the final `^b1` visit.



## 11. Non-goals for v0 (planned extensions)
- Heap, pointers, aliasing, pointer arithmetic.
- Parentheses, operator precedence, general expression trees.
- Floating-point.
- SSA/phi.
- Interprocedural calls and summaries.
