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
  - **Solver:** Initialization constraints ensure variables are determined or explicitly `undef`.

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

- **Status:** **NOT UB** (Defined as wrapping).
- **Semantics:** SymIR integers (`i32`, `i64`) follow standard **Two's Complement Modular Arithmetic**.
  - `MAX_INT + 1` wraps to `MIN_INT`.
  - This matches the behavior of SMT Bit-Vector theory (`(_ BitVec N)`).
  - *Note:* This differs from C/C++ strict aliasing rules but aligns with hardware behavior and SMT solvers.

## 5. Shift Operators
**Definition:** Shifting by a value greater than or equal to the bit-width.

- **Status:** **NOT UB** (Logic is currently not part of core v0 arithmetic atoms).
- *Future Note:* If shifts (`shl`, `lshr`, `ashr`) are added, over-shifting should likely be defined as UB or masking, consistent with the chosen design philosophy.
