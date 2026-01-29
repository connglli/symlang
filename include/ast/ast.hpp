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
   * Wrapper for all possible types in SymIR.
   */
  struct Type {
    using Variant = std::variant<IntType, FloatType, StructType, ArrayType>;
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
   * A coefficient in an expression (literal or variable).
   */
  using Coef = std::variant<IntLit, FloatLit, LocalOrSymId>;

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
   */
  struct SelectAtom {
    std::unique_ptr<Cond> cond;
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
   * The fundamental building block of expressions.
   */
  struct Atom {
    using Variant = std::variant<OpAtom, SelectAtom, CoefAtom, RValueAtom, CastAtom, UnaryAtom>;
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

  using Instr = std::variant<AssignInstr, AssumeInstr, RequireInstr>;

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
    enum class Kind { Int, Float, Sym, Local, Undef, Aggregate } kind;
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
