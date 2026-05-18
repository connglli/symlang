# SymIR Strict Undefined Behavior (UB)

SymIR enforces **Strict UB semantics**. If any operation on the executed path triggers Undefined Behavior, the entire path is considered **infeasible**.

In the Interpreter (`symiri`) and Compiler (`symirc`), UB causes immediate termination (crash/abort).
In the Solver (`symirsolve`), UB generates constraints that negate the path condition (pruning the path).

## 1. Use Before Initialization
**Definition:** Reading a local variable, array element, or struct field that has not been definitely initialized.

- **Status:** Enforced.
- **Mechanism:**
  - **Static:** `DefiniteInitAnalysis` warns at compile time (conservative).
  - **Dynamic:** Interpreter tracks `undef` state per-scalar-leaf. Reading `undef` throws a runtime error.
  - **Solver:** Tracks a boolean `is_defined` flag for every symbolic value. Reading a value (e.g., in an expression or branch condition) adds its `is_defined` term to the path condition, ensuring the path is only feasible if all read values are definitely initialized.

## 2. Out-of-Bounds Access
**Definition:** Accessing an array with an index `i` such that `i < 0` or `i >= Size`.

- **Status:** Enforced.
- **Mechanism:**
  - **Dynamic:** Interpreter checks bounds on every array access.
  - **Solver:** Generates `(bvult index size)` constraints (unsigned less-than handles negative values as large positives).

## 3. Division by Zero
**Definition:** Executing `a / b` or `a % b` where `b == 0`.

- **Status:** Enforced.
- **Mechanism:**
  - **Dynamic:** Interpreter checks divisor before operation.
  - **Solver:** Generates `(distinct divisor 0)` constraints.

## 4. Integer Overflow
**Definition:** Signed integer arithmetic resulting in a value not representable in the target bit-width.

- **Status:** **Enforced.**
- **Semantics:**
  - Arithmetic operations (`+`, `-`, `*`) on signed integers must not overflow.
  - **Dynamic:** Interpreter checks for signed overflow on every operation.
  - **Solver:** Generates `(bv ssubo/saddo/smulo)` checks to ensure no overflow occurs.
  - *Note:* This aligns SymIR with strict C/C++ signed integer semantics.

## 5. Overshift
**Definition:** Shifting a value by an amount `n` such that `n < 0` or `n >= bit_width`.

- **Status:** **Enforced.**
- **Mechanism:**
  - **Dynamic:** Interpreter checks the shift amount before the operation.
  - **Solver:** Generates `(bvult amount width)` constraints.
  - *Note:* This prevents architecture-defined behavior (like masking) from affecting program semantics.

## 6. Floating-Point UB
**Definition:** Operations that produce IEEE 754 NaN or infinite results are UB in SymIR.

- **Status:** **Enforced.**
- **Scope:** Applies to `f32` and `f64` arithmetic (`+`, `-`, `*`, `/`, `%`) and float-to-integer casts.
- **Cases:**
  - `inf`: Addition, subtraction, multiplication, or division that produces ±∞.
  - `nan`: Division of ±∞ by ±∞, or 0.0/0.0, or `%` with 0 divisor.
  - `cast`: Casting a float to an integer type when the float is ±∞, NaN, or the truncated value is out of range for the target integer type.
- **Mechanism:**
  - **Dynamic:** Interpreter checks `std::isinf` / `std::isnan` after each float operation, and checks range before float→int cast.
  - **Solver:** Adds path constraints that exclude ∞ and NaN results.

## 7. Pointer UB
**Definition:** Pointer operations that violate the SymIR pointer model.

- **Status:** **Enforced.**
- **Cases and mechanisms:**

### 7a. Null Pointer Dereference
**Definition:** Performing `load` or `store` through a pointer with value `null`.

- **Dynamic:** Interpreter checks that `ptr != null` before any `load` or `store`.
- **Solver:** Adds a constraint `(distinct ptr null_ptr_id)` on every `load`/`store` path.

### 7b. Out-of-Bounds Pointer Arithmetic
**Definition:** `ptr ± n` that moves the pointer outside `[base, base + size)` of the originating allocation.

- **Dynamic:** Interpreter tracks the backing allocation for every pointer. Arithmetic that would place the result outside the allocation's bounds is UB.
- **Solver:** Generates `(bvule new_offset size)` constraints to keep the pointer in range.

### 7c. Relational Comparison Across Objects
**Definition:** Applying `<`, `<=`, `>`, or `>=` to two pointers that do not point into the same object.

- **Rationale:** Cross-object pointer ordering is undefined in C and SymIR; only equality (`==`, `!=`) is always well-defined.
- **Dynamic:** Interpreter checks that both operands reference the same allocation object before a relational compare.
- **Solver:** Adds a constraint that both pointers share the same base object on any path that contains a relational pointer comparison.

### 7d. Store Through Undef Pointer
**Definition:** Executing `store %p, val` when `%p` is `undef`.

- **Dynamic:** Interpreter checks that the pointer value is defined (not `undef`) before a store.
- **Solver:** Treats the `is_defined` flag of the pointer like any other variable; an undefined pointer store prunes the path.

### 7e. Struct-Field Pointer Arithmetic
**Definition:** `ptr ± n` where `ptr` was derived from `addr lv.f` (a struct field address) and `n ≠ 0` steps past the single-element field boundary.

- **Rationale:** A struct-field pointer has provenance over exactly one element — the storage of field `f`. Adjacent fields may have different types; stepping into them through an `i32` pointer would alias an `i64` field (or vice-versa), breaking type safety. Even adjacent same-typed fields are separate provenance regions.
- **Dynamic:** Interpreter tracks a `ptrBase` provenance tag per pointer value identifying the backing `ObjectInfo` (one per field). Arithmetic uses `ptrBase` to locate the correct object; results outside `[obj.base, obj.end]` (strictly: `obj.base = field_addr`, `obj.end = field_addr + sizeof(T)`) throw `UndefinedBehaviorError`.
- **Solver:** Each `addr lv.f` yields an abstract address constant with a one-element non-overlap axiom. Arithmetic that places the result outside this range generates an unsatisfiable constraint.
