#pragma once

#include <vector>
#include "ast/ast.hpp"
#include "frontend/lexer.hpp"

namespace symir {

  /**
   * Recursive-descent parser for the SymIR language.
   * Transforms a sequence of tokens into a structured AST (Program).
   */
  class Parser {
  public:
    explicit Parser(std::vector<Token> toks);
    /**
     * Entry point for parsing a complete SymIR program.
     */
    Program parseProgram();

  private:
    std::vector<Token> toks_;
    std::size_t idx_ = 0;

    const Token &peek(std::size_t k = 0) const;
    bool is(TokenKind k) const;
    const Token &consume(TokenKind k, const char *what);
    bool tryConsume(TokenKind k);
    [[noreturn]] void errorHere(const std::string &msg) const;

    // --- Sub-parsers for different AST components ---
    GlobalId parseGlobalId();
    LocalId parseLocalId();
    SymId parseSymId();
    BlockLabel parseBlockLabel();

    TypePtr parseType();
    SourcePos prevEnd() const;

    StructDecl parseStructDecl();
    FunDecl parseFunDecl();
    // [v0.2.2] decl @name(params): T;   OR  decl @name(params): T { pre... post... };
    ExtDecl parseExtDecl();
    // [v0.2.2] intrinsic @name(params): T;
    IntrinsicDecl parseIntrinsicDecl();
    // [v0.2.2] call @name(args) atom — parsed by parseAtom on KwCall.
    Atom parseCallAtom();
    // [v0.2.2] In `post` clauses the bareword `ret` is a reserved identifier
    // referring to the callee's return value. We carry this context flag so
    // parseCoef can synthesize a LocalId{"ret", ...} when it sees KwRet.
    bool inPostClause_ = false;
    std::vector<ParamDecl> parseParamList();
    SymKind parseSymKind();
    std::optional<Domain> parseOptionalDomain();
    SymDecl parseSymDecl();
    LetDecl parseLetDecl();
    // [v0.2.1] `allowAtom` is true at the top of `let mut x: T = …;`
    // — atom forms like `addr %x` are spec §3.4.2 valid. Inside
    // aggregate braces it's false: BraceInit restricts elements to
    // literals / names / null / undef / nested braces.
    InitVal parseInitVal(bool allowAtom = true);

    Block parseBlock();
    bool isStartOfInstr() const;
    Instr parseInstr();
    AssignInstr parseAssignInstr();
    AssumeInstr parseAssumeInstr();
    RequireInstr parseRequireInstr();
    StoreInstr parseStoreInstr();
    Terminator parseTerminator();
    BrTerm parseBrTerm();
    RetTerm parseRetTerm();
    UnreachableTerm parseUnreachableTerm();

    LValue parseLValue();
    Index parseIndex();
    Coef parseCoef();
    Cond parseCond();
    RelOp parseRelOp();
    Expr parseExpr();
    Atom parseAtom();
    AtomOpKind parseAtomOp();
    SelectVal parseSelectVal();
  };

} // namespace symir
