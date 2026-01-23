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
