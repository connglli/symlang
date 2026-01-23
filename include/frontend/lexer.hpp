#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "ast/ast.hpp"

enum class TokenKind {
  End,

  // Ident-like with sigils
  GlobalId,   // @foo
  LocalId,    // %x
  SymId,      // @?c or %?k
  BlockLabel, // ^entry

  Ident, // bare identifier (field names, keywords disambiguated)
  IntLit,
  StringLit,

  // Punctuators / operators
  LBrace,
  RBrace,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Colon,
  Semicolon,
  Comma,
  Dot,
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Equal, // '=' assignment
  EqEq,
  NotEq,
  Lt,
  Le,
  Gt,
  Ge,

  // Keywords
  KwStruct,
  KwFun,
  KwSym,
  KwLet,
  KwMut,
  KwAssume,
  KwRequire,
  KwBr,
  KwRet,
  KwUnreachable,
  KwIn, // domain
  KwSelect,
  KwI32,
  KwI64,
  KwInt,
  KwI, // "i" prefix for i<N>
  KwUndef,
};

struct Token {
  TokenKind kind;
  std::string lexeme;
  SourceSpan span;
};

class Lexer {
public:
  explicit Lexer(std::string_view src);
  std::vector<Token> lexAll();

private:
  std::string_view src_;
  std::size_t i_ = 0;
  int line_ = 1;
  int col_ = 1;

  char peek(std::size_t k = 0) const;
  char get();
  SourcePos pos() const;
  void skipWhitespaceAndComments();

  static bool isIdentStart(char c);
  static bool isIdentCont(char c);

  Token make(TokenKind k, std::string lex, SourcePos b, SourcePos e);
  Token next();
};
