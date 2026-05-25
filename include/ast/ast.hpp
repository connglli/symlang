#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace symir {

  /**
   * Represents a location in the source code.
   */
  struct SourcePos {
    std::size_t offset = 0; // byte offset in source
    int line = 1;           // 1-based
    int col = 1;            // 1-based
  };

  /**
   * Represents a span between two source positions.
   */
  struct SourceSpan {
    SourcePos begin;
    SourcePos end;
  };

  /**
   * A structured parse error with location information.
   */
  struct ParseError : std::runtime_error {
    SourceSpan span;

    explicit ParseError(const std::string &msg, SourceSpan sp) :
        std::runtime_error(msg), span(sp) {}
  };

  /**
   * A lexer error — a ParseError subclass so existing catch(ParseError)
   * sites still handle it, but the main driver can distinguish lex vs parse.
   */
  struct LexError : ParseError {
    explicit LexError(const std::string &msg, SourceSpan sp) : ParseError(msg, sp) {}
  };

  // ---------------------------
  // Node Base
  // ---------------------------

  using NodeId = std::uint32_t;

  /**
   * Base struct for all AST nodes containing common metadata.
   */
  struct NodeBase {
    NodeId id{};
    SourceSpan span{};
  };

  // ---------------------------
  // Identifier kinds (type-safe)
  // ---------------------------

  /**
   * Global identifier starting with '@', e.g., '@main'.
   */
  struct GlobalId {
    std::string name;
    SourceSpan span;
  };

  /**
   * Local identifier starting with '%', e.g., '%x'.
   */
  struct LocalId {
    std::string name;
    SourceSpan span;
  };

  /**
   * Symbolic identifier starting with '@?' or '%?', e.g., '%?v'.
   */
  struct SymId {
    std::string name;
    SourceSpan span;
  };

  /**
   * Block label identifier starting with '^', e.g., '^entry'.
   */
  struct BlockLabel {
    std::string name;
    SourceSpan span;
  };

  using AnyId = std::variant<GlobalId, LocalId, SymId, BlockLabel>;
  using LocalOrSymId = std::variant<LocalId, SymId>;

  // ---------------------------
  // Type system (AST-level)
  // ---------------------------

  /**
   * Represents integer types with specific bitwidths.
   */
  struct IntType {
    enum class Kind { I32, I64, ICustom } kind = Kind::I32;
    std::optional<int> bits; // bitwidth for ICustom
    SourceSpan span;
  };

  /**
   * Represents floating-point types.
   */
  struct FloatType {
    enum class Kind { F32, F64 } kind = Kind::F32;
    SourceSpan span;
  };

  /**
   * Represents user-defined struct types.
   */
  struct StructType {
    GlobalId name;
    SourceSpan span;
  };

  struct ArrayType;
  struct PtrType;
  struct VecType;
  struct Type;
  using TypePtr = std::shared_ptr<Type>;

  /**
   * Represents fixed-size array types.
   */
  struct ArrayType {
    std::uint64_t size = 0;
    TypePtr elem;
    SourceSpan span;
  };

  /**
   * Represents pointer types: ptr T.
   * In v0.2.0 the pointee must be a scalar (iN, f32, f64) or another ptr T.
   */
  struct PtrType {
    TypePtr pointee;
    SourceSpan span;
  };

  /**
   * [v0.2.1] Fixed-width SIMD vector type: <N> T.
   * T must be a scalar (iN, f32, f64). N >= 2. The bitwidth is
   * N * bitwidth(T); vectors are register-shaped value types and not
   * addressable (no `ptr <N> T`). Lane access uses LValue subscript.
   */
  struct VecType {
    std::uint64_t size = 0; // N (lane count)
    TypePtr elem;           // lane scalar type
    SourceSpan span;
  };

  /**
   * Wrapper for all possible types in SymIR.
   */
  struct Type {
    using Variant = std::variant<IntType, FloatType, StructType, ArrayType, PtrType, VecType>;
    Variant v;
    SourceSpan span;
  };

  // ---------------------------
  // AST: expressions
  // ---------------------------

  /**
   * Literal integer value.
   */
  struct IntLit {
    std::int64_t value = 0;
    SourceSpan span;
  };

  /**
   * Literal floating-point value.
   */
  struct FloatLit {
    double value = 0.0;
    SourceSpan span;
  };

  /**
   * The null pointer constant, typed by context.
   */
  struct NullLit {
    SourceSpan span;
  };

  /**
   * A coefficient in an expression (literal, variable, or null pointer).
   */
  using Coef = std::variant<IntLit, FloatLit, LocalOrSymId, NullLit>;

  /**
   * An index for array access.
   */
  using Index = std::variant<IntLit, LocalOrSymId>;

  /**
   * Represents an array index access segment.
   */
  struct AccessIndex {
    Index index;
    SourceSpan span;
  };

  /**
   * Represents a struct field access segment.
   */
  struct AccessField {
    std::string field;
    SourceSpan span;
  };

  using Access = std::variant<AccessIndex, AccessField>;

  /**
   * Represents an addressable location (e.g., %x.y[0]).
   */
  struct LValue {
    LocalId base;
    std::vector<Access> accesses;
    SourceSpan span;
  };

  using RValue = LValue;

  /**
   * Relational operators for comparisons.
   */
  enum class RelOp { EQ, NE, LT, LE, GT, GE };

  struct Expr;
  struct Cond;

  using SelectVal = std::variant<RValue, Coef>;

  /**
   * Ternary select expression (lazy evaluation).
   *
   * Two forms (spec §5.3):
   *   - Cond form:  `select Cond, vt, vf` — scalar boolean predicate.
   *   - Mask form:  `select Expr, vt, vf` — Expr of type i1 or <N> i1.
   *
   * Exactly one of `cond` and `maskExpr` is non-null. The parser
   * disambiguates by lookahead: after parsing the first Expr, if the
   * next token is a RelOp the form is Cond; if it is `,` the form is
   * mask.
   */
  struct SelectAtom {
    std::unique_ptr<Cond> cond;     // Cond form
    std::unique_ptr<Expr> maskExpr; // mask form [v0.2.1]
    SelectVal vtrue;
    SelectVal vfalse;
    SourceSpan span;
  };

  /**
   * Binary operator kinds for atoms.
   */
  enum class AtomOpKind { Mul, Div, Mod, And, Or, Xor, Shl, Shr, LShr };

  /**
   * Binary operation atom.
   */
  struct OpAtom {
    AtomOpKind op;
    Coef coef;
    RValue rval;
    SourceSpan span;
  };

  enum class UnaryOpKind { Not };

  /**
   * Unary operation atom.
   */
  struct UnaryAtom {
    UnaryOpKind op;
    RValue rval;
    SourceSpan span;
  };

  /**
   * Constant or variable atom.
   */
  struct CoefAtom {
    Coef coef;
    SourceSpan span;
  };

  /**
   * Read from an LValue atom.
   */
  struct RValueAtom {
    RValue rval;
    SourceSpan span;
  };

  /**
   * Type cast atom.
   */
  struct CastAtom {
    using Variant = std::variant<IntLit, FloatLit, SymId, LValue>;
    Variant src;
    TypePtr dstType;
    SourceSpan span;
  };

  /**
   * Address-of atom: addr <lv>.  Result type is ptr T where T = type(lv).
   * The root of lv must be a let mut local.
   */
  struct AddrAtom {
    LValue lv;
    SourceSpan span;
  };

  /**
   * Load-through-pointer atom: load <rval>.
   * rval must have type ptr T; result type is T.
   */
  struct LoadAtom {
    RValue rval;
    SourceSpan span;
  };

  /**
   * [v0.2.1] Reified comparison: cmp <relop> <lhs>, <rhs>.
   * Produces i1 for scalar operands or <N> i1 for vector operands.
   * lhs/rhs are SelectVal (RValue | Coef) so literals are admitted.
   */
  struct CmpAtom {
    RelOp op;
    SelectVal lhs;
    SelectVal rhs;
    SourceSpan span;
  };

  /**
   * [v0.2.1] Aggregate-pointer navigation atoms (§6.8.9, §6.8.10).
   *
   * `ptrindex <ptr>, <index>` navigates from `ptr [N] T` to `ptr T` at
   * a runtime index. Strict UB rules: index in [0, N], non-null, non-
   * undef, non-one-past-end source.
   *
   * `ptrfield <ptr>, <fieldname>` navigates from `ptr @S` to
   * `ptr FieldType` at a statically-known field offset. Same source
   * UB checks (null / undef / one-past-end) as ptrindex.
   */
  struct PtrIndexAtom {
    RValue rval;
    Index index;
    SourceSpan span;
  };

  struct PtrFieldAtom {
    RValue rval;
    std::string field;
    SourceSpan span;
  };

  /**
   * The fundamental building block of expressions.
   */
  struct Atom {
    using Variant = std::variant<
        OpAtom, SelectAtom, CoefAtom, RValueAtom, CastAtom, UnaryAtom, AddrAtom, LoadAtom, CmpAtom,
        PtrIndexAtom, PtrFieldAtom>;
    Variant v;
    SourceSpan span;
  };

  enum class AddOp { Plus, Minus };

  /**
   * Represents a linear expression of atoms.
   */
  struct Expr {
    Atom first;

    struct Tail {
      AddOp op;
      Atom atom;
      SourceSpan span;
    };

    std::vector<Tail> rest;
    SourceSpan span;
  };

  /**
   * A boolean condition (comparison of two expressions).
   */
  struct Cond {
    Expr lhs;
    RelOp op;
    Expr rhs;
    SourceSpan span;
  };

  // ---------------------------
  // AST: instructions / terminators
  // ---------------------------

  /**
   * Assignment instruction: lhs = rhs.
   */
  struct AssignInstr {
    LValue lhs;
    Expr rhs;
    SourceSpan span;
  };

  /**
   * Assume instruction: provides a constraint to the solver.
   */
  struct AssumeInstr {
    Cond cond;
    SourceSpan span;
  };

  /**
   * Require instruction: an assertion that must hold.
   */
  struct RequireInstr {
    Cond cond;
    std::optional<std::string> message;
    SourceSpan span;
  };

  /**
   * Store instruction: store <ptr>, <val>.
   * Writes val (type T) through ptr (type ptr T).
   */
  struct StoreInstr {
    Expr ptr; // must evaluate to ptr T
    Expr val; // must evaluate to T
    SourceSpan span;
  };

  using Instr = std::variant<AssignInstr, AssumeInstr, RequireInstr, StoreInstr>;

  /**
   * Branch terminator (conditional or unconditional).
   */
  struct BrTerm {
    std::optional<Cond> cond;
    BlockLabel dest;
    BlockLabel thenLabel;
    BlockLabel elseLabel;
    bool isConditional = false;
    SourceSpan span;
  };

  /**
   * Return terminator.
   */
  struct RetTerm {
    std::optional<Expr> value;
    SourceSpan span;
  };

  /**
   * Unreachable terminator.
   */
  struct UnreachableTerm {
    SourceSpan span;
  };

  using Terminator = std::variant<BrTerm, RetTerm, UnreachableTerm>;

  /**
   * A basic block containing instructions and ending with a terminator.
   */
  struct Block {
    BlockLabel label;
    std::vector<Instr> instrs;
    Terminator term;
    SourceSpan span;
  };

  // ---------------------------
  // AST: declarations
  // ---------------------------

  struct FieldDecl {
    std::string name;
    TypePtr type;
    SourceSpan span;
  };

  /**
   * User-defined struct declaration.
   */
  struct StructDecl {
    GlobalId name;
    std::vector<FieldDecl> fields;
    SourceSpan span;
  };

  enum class SymKind { Value, Coef, Index };

  struct DomainInterval {
    std::int64_t lo = 0;
    std::int64_t hi = 0;
    SourceSpan span;
  };

  struct DomainSet {
    std::vector<std::int64_t> values;
    SourceSpan span;
  };

  using Domain = std::variant<DomainInterval, DomainSet>;

  /**
   * Symbolic variable declaration.
   */
  struct SymDecl {
    SymId name;
    SymKind kind;
    TypePtr type;
    std::optional<Domain> domain;
    SourceSpan span;
  };

  struct InitVal;
  using InitValPtr = std::shared_ptr<InitVal>;

  /**
   * Initializer value for variables.
   */
  struct InitVal {
    enum class Kind { Int, Float, Sym, Local, Undef, Aggregate, Null } kind;
    std::variant<IntLit, FloatLit, SymId, LocalId, std::vector<InitValPtr>> value;
    SourceSpan span;
  };

  /**
   * Local variable declaration (mutable or immutable).
   */
  struct LetDecl {
    bool isMutable = false;
    LocalId name;
    TypePtr type;
    std::optional<InitVal> init;
    SourceSpan span;
  };

  /**
   * Function parameter declaration.
   */
  struct ParamDecl {
    LocalId name;
    TypePtr type;
    SourceSpan span;
  };

  /**
   * Function declaration.
   */
  struct FunDecl {
    GlobalId name;
    std::vector<ParamDecl> params;
    TypePtr retType;
    std::vector<SymDecl> syms;
    std::vector<LetDecl> lets;
    std::vector<Block> blocks;
    SourceSpan span;
  };

  /**
   * Represents a complete SymIR program.
   */
  struct Program {
    std::vector<StructDecl> structs;
    std::vector<FunDecl> funs;
    SourceSpan span;
  };

  // ---------------------------
  // Utilities
  // ---------------------------
  inline int64_t parseIntegerLiteral(const std::string &s) {
    // The literal might be:
    //   0x... (hexadecimal)
    //   0o... (octal)
    //   0b... (binary)
    //   ...or decimal by default.
    // Or with a leading '-'.
    if (s.size() >= 2) {
      size_t n = (s[0] == '-') ? 1 : 0;
      if (s.size() > n + 1 && s[n] == '0') {
        if (s[n + 1] == 'x' || s[n + 1] == 'X')
          return std::stoll(s, nullptr, 16);
        if (s[n + 1] == 'o' || s[n + 1] == 'O') {
          std::string octal = (n == 1 ? "-" : "") + s.substr(n + 2);
          return std::stoll(octal, nullptr, 8);
        }
        if (s[n + 1] == 'b' || s[n + 1] == 'B') {
          std::string binary = (n == 1 ? "-" : "") + s.substr(n + 2);
          return std::stoll(binary, nullptr, 2);
        }
      }
    }
    return static_cast<int64_t>(std::stoll(s, nullptr, 10));
  }

  inline double parseFloatLiteral(const std::string &s) { return std::stod(s); }

  inline std::variant<int64_t, double> parseNumberLiteral(const std::string &s) {
    if (s.find('.') != std::string::npos || s.find('e') != std::string::npos ||
        s.find('E') != std::string::npos) {
      return parseFloatLiteral(s);
    }
    return parseIntegerLiteral(s);
  }
} // namespace symir
