#pragma once

#include <vector>
#include "ast/ast.hpp"
#include "frontend/lexer.hpp"

class Parser {
public:
  explicit Parser(std::vector<Token> toks);
  Program parseProgram();

private:
  std::vector<Token> toks_;
  std::size_t idx_ = 0;

  const Token &peek(std::size_t k = 0) const;
  bool is(TokenKind k) const;
  const Token &consume(TokenKind k, const char *what);
  bool tryConsume(TokenKind k);
  [[noreturn]] void errorHere(const std::string &msg) const;

  GlobalId parseGlobalId();
  LocalId parseLocalId();
  SymId parseSymId();
  BlockLabel parseBlockLabel();

  TypePtr parseType();
  SourcePos prevEnd() const;

  StructDecl parseStructDecl();
  FunDecl parseFunDecl();
  std::vector<ParamDecl> parseParamList();
  SymKind parseSymKind();
  std::optional<Domain> parseOptionalDomain();
  SymDecl parseSymDecl();
  LetDecl parseLetDecl();
  InitVal parseInitVal();

  Block parseBlock();
  bool isStartOfInstr() const;
  Instr parseInstr();
  AssignInstr parseAssignInstr();
  AssumeInstr parseAssumeInstr();
  RequireInstr parseRequireInstr();
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
