#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "ast/ast.hpp"

namespace symir {

  /**
   * Enumeration of all token types recognized by the SymIR lexer.
   */
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
    Amp,   // '&'
    Pipe,  // '|'
    Caret, // '^'
    Tilde, // '~'
    Shl,   // '<<'
    Shr,   // '>>'
    LShr,  // '>>>'
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
    IntType, // i1, i8, i16, i32, i64, iN
    KwUndef,
    KwAs, // "as"
  };

  /**
   * Represents a single lexical token.
   */
  struct Token {
    TokenKind kind;
    std::string lexeme;
    SourceSpan span;
  };

  /**
   * Lexical analyzer for the SymIR language.
   * Converts a source string into a sequence of tokens.
   */
  class Lexer {
  public:
    explicit Lexer(std::string_view src);
    /**
     * Lexes the entire source and returns a vector of tokens.
     */
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

} // namespace symir
