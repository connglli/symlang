#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace symir {

  // ---------------------------
  // Source location primitives
  // ---------------------------

  struct SourcePos {
    std::size_t offset = 0; // byte offset in source
    int line = 1;           // 1-based
    int col = 1;            // 1-based
  };

  struct SourceSpan {
    SourcePos begin;
    SourcePos end;
  };

  // A structured parse error with location.
  struct ParseError : std::runtime_error {
    SourceSpan span;

    explicit ParseError(const std::string &msg, SourceSpan sp) :
        std::runtime_error(msg), span(sp) {}
  };

  // ---------------------------
  // Node Base
  // ---------------------------

  using NodeId = std::uint32_t;

  struct NodeBase {
    NodeId id{};
    SourceSpan span{};
  };

  // ---------------------------
  // Identifier kinds (type-safe)
  // ---------------------------

  struct GlobalId {
    std::string name;
    SourceSpan span;
  }; // "@foo"

  struct LocalId {
    std::string name;
    SourceSpan span;
  }; // "%x"

  struct SymId {
    std::string name;
    SourceSpan span;
  }; // "@?c0" or "%?k"

  struct BlockLabel {
    std::string name;
    SourceSpan span;
  }; // "^entry"

  using AnyId = std::variant<GlobalId, LocalId, SymId, BlockLabel>;

  // For places where LocalId or SymId are permitted.
  using LocalOrSymId = std::variant<LocalId, SymId>;

  // ---------------------------
  // Type system (AST-level)
  // ---------------------------

  struct IntType {
    // "i32", "i64", or "i<N>"
    enum class Kind { I32, I64, ICustom } kind = Kind::I32;
    std::optional<int> bits; // for ICustom
    SourceSpan span;
  };

  struct StructType {
    GlobalId name; // e.g. @Rect
    SourceSpan span;
  };

  struct ArrayType;

  struct Type;
  using TypePtr = std::shared_ptr<Type>;

  struct ArrayType {
    std::uint64_t size = 0; // fixed-size in v0
    TypePtr elem;
    SourceSpan span;
  };

  struct Type {
    using Variant = std::variant<IntType, StructType, ArrayType>;
    Variant v;
    SourceSpan span;
  };

  // ---------------------------
  // AST: expressions
  // ---------------------------

  struct IntLit {
    std::int64_t value = 0;
    SourceSpan span;
  };

  using Coef = std::variant<IntLit, LocalOrSymId>; // v0: IntLit | LocalId | SymId

  // Index: IntLit | LocalId | SymId
  using Index = std::variant<IntLit, LocalOrSymId>;

  // LValue access segments: [index] or .field
  struct AccessIndex {
    Index index;
    SourceSpan span;
  };

  struct AccessField {
    std::string field;
    SourceSpan span;
  };

  using Access = std::variant<AccessIndex, AccessField>;

  struct LValue {
    LocalId base;                 // v0: base is LocalId (params/locals)
    std::vector<Access> accesses; // zero or more
    SourceSpan span;
  };

  // RValue is just an LValue read in v0.
  using RValue = LValue;

  // Relational operators
  enum class RelOp { EQ, NE, LT, LE, GT, GE };

  // Forward decl
  struct Expr;
  struct Cond;

  // SelectVal := RValue | Coef (v0 restriction: no nested expressions)
  using SelectVal = std::variant<RValue, Coef>;

  struct SelectAtom {
    std::unique_ptr<Cond> cond;
    SelectVal vtrue;
    SelectVal vfalse;
    SourceSpan span;
  };

  // Atom kinds:
  //   Coef * RValue
  //   Coef / RValue
  //   Coef % RValue
  //   select ...
  //   Coef
  //   RValue
  enum class AtomOpKind { Mul, Div, Mod };

  struct OpAtom {
    AtomOpKind op;
    Coef coef;
    RValue rval;
    SourceSpan span;
  };

  struct CoefAtom {
    Coef coef;
    SourceSpan span;
  };

  struct RValueAtom {
    RValue rval;
    SourceSpan span;
  };

  struct CastAtom {
    using Variant = std::variant<IntLit, SymId, LValue>;
    Variant src;
    TypePtr dstType;
    SourceSpan span;
  };

  struct Atom {
    using Variant = std::variant<OpAtom, SelectAtom, CoefAtom, RValueAtom, CastAtom>;
    Variant v;
    SourceSpan span;
  };

  // Expr := Atom (('+'|'-') Atom)* evaluated left-to-right
  enum class AddOp { Plus, Minus };

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

  // Condition: Expr RelOp Expr
  struct Cond {
    Expr lhs;
    RelOp op;
    Expr rhs;
    SourceSpan span;
  };

  // ---------------------------
  // AST: instructions / terminators
  // ---------------------------

  struct AssignInstr {
    LValue lhs;
    Expr rhs;
    SourceSpan span;
  };

  struct AssumeInstr {
    Cond cond;
    SourceSpan span;
  };

  struct RequireInstr {
    Cond cond;
    std::optional<std::string> message;
    SourceSpan span;
  };

  using Instr = std::variant<AssignInstr, AssumeInstr, RequireInstr>;

  struct BrTerm {
    std::optional<Cond> cond;
    BlockLabel dest;      // used if unconditional
    BlockLabel thenLabel; // used if conditional
    BlockLabel elseLabel; // used if conditional
    bool isConditional = false;
    SourceSpan span;
  };

  struct RetTerm {
    std::optional<Expr> value;
    SourceSpan span;
  };

  struct UnreachableTerm {
    SourceSpan span;
  };

  using Terminator = std::variant<BrTerm, RetTerm, UnreachableTerm>;

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

  struct StructDecl {
    GlobalId name; // @TypeName
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

  struct SymDecl {
    SymId name; // @? or %?
    SymKind kind;
    TypePtr type;
    std::optional<Domain> domain;
    SourceSpan span;
  };

  // let / let mut
  struct InitVal;
  using InitValPtr = std::shared_ptr<InitVal>;

  struct InitVal {
    enum class Kind { Int, Sym, Local, Undef, Aggregate } kind;
    std::variant<IntLit, SymId, LocalId, std::vector<InitValPtr>> value;
    SourceSpan span;
  };

  struct LetDecl {
    bool isMutable = false; // let mut
    LocalId name;           // %x
    TypePtr type;
    std::optional<InitVal> init;
    SourceSpan span;
  };

  struct ParamDecl {
    LocalId name; // %p
    TypePtr type;
    SourceSpan span;
  };

  struct FunDecl {
    GlobalId name; // @f
    std::vector<ParamDecl> params;
    TypePtr retType;
    std::vector<SymDecl> syms;
    std::vector<LetDecl> lets;
    std::vector<Block> blocks;
    SourceSpan span;
  };

  struct Program {
    std::vector<StructDecl> structs;
    std::vector<FunDecl> funs;
    SourceSpan span;
  };

} // namespace symir
