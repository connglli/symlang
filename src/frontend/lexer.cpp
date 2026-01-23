#include "frontend/lexer.hpp"
#include <cctype>
#include <stdexcept>

namespace symir {

  Lexer::Lexer(std::string_view src) : src_(src) {}

  std::vector<Token> Lexer::lexAll() {
    std::vector<Token> out;
    while (true) {
      Token t = next();
      out.push_back(t);
      if (t.kind == TokenKind::End)
        break;
    }
    return out;
  }

  char Lexer::peek(std::size_t k) const {
    if (i_ + k >= src_.size())
      return '\0';
    return src_[i_ + k];
  }

  char Lexer::get() {
    char c = peek();
    if (c == '\0')
      return c;
    i_++;
    if (c == '\n') {
      line_++;
      col_ = 1;
    } else {
      col_++;
    }
    return c;
  }

  SourcePos Lexer::pos() const { return SourcePos{i_, line_, col_}; }

  void Lexer::skipWhitespaceAndComments() {
    while (true) {
      // whitespace
      while (std::isspace(static_cast<unsigned char>(peek())))
        get();

      // // comment
      if (peek() == '/' && peek(1) == '/') {
        while (peek() != '\0' && peek() != '\n')
          get();
        continue;
      }
      // /* comment */
      if (peek() == '/' && peek(1) == '*') {
        get();
        get();
        while (peek() != '\0') {
          if (peek() == '*' && peek(1) == '/') {
            get();
            get();
            break;
          }
          get();
        }
        continue;
      }
      break;
    }
  }

  bool Lexer::isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
  }

  bool Lexer::isIdentCont(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  }

  Token Lexer::make(TokenKind k, std::string lex, SourcePos b, SourcePos e) {
    return Token{k, std::move(lex), SourceSpan{b, e}};
  }

  Token Lexer::next() {
    skipWhitespaceAndComments();
    SourcePos b = pos();
    char c = peek();
    if (c == '\0') {
      return make(TokenKind::End, "", b, pos());
    }

    // String literal
    if (c == '"') {
      get(); // "
      std::string val;
      while (true) {
        char ch = get();
        if (ch == '\0' || ch == '\n') {
          throw ParseError("Unterminated string literal", SourceSpan{b, pos()});
        }
        if (ch == '"')
          break;
        if (ch == '\\') {
          char esc = get();
          if (esc == 'n')
            val.push_back('\n');
          else if (esc == 't')
            val.push_back('\t');
          else if (esc == 'r')
            val.push_back('\r');
          else if (esc == '"')
            val.push_back('"');
          else if (esc == '\\')
            val.push_back('\\');
          else {
            val.push_back(esc);
          }
        } else {
          val.push_back(ch);
        }
      }
      return make(TokenKind::StringLit, val, b, pos());
    }

    // Sigiled identifiers
    if ((c == '@' || c == '%') && (isIdentStart(peek(1)) || peek(1) == '?')) {
      get();
      char c2 = peek();
      bool isSym = false;
      if (c2 == '?') {
        isSym = true;
        get();
      }
      if (!isIdentStart(peek())) {
        throw ParseError("Expected identifier after sigil", SourceSpan{b, pos()});
      }
      std::string name;
      while (isIdentCont(peek()))
        name.push_back(get());
      std::string lex = std::string(1, c) + (isSym ? "?" : "") + name;
      return make(
          isSym ? TokenKind::SymId : (c == '@' ? TokenKind::GlobalId : TokenKind::LocalId), lex, b,
          pos()
      );
    }

    if (c == '^') {
      if (isIdentStart(peek(1))) {
        get();
        std::string name;
        while (isIdentCont(peek()))
          name.push_back(get());
        return make(TokenKind::BlockLabel, "^" + name, b, pos());
      } else {
        get();
        return make(TokenKind::Caret, "^", b, pos());
      }
    }

    // Integer literal
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '-' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
      std::string num;
      if (c == '-')
        num.push_back(get());

      if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        num.push_back(get()); // '0'
        num.push_back(get()); // 'x'
        while (std::isxdigit(static_cast<unsigned char>(peek())))
          num.push_back(get());
      } else {
        while (std::isdigit(static_cast<unsigned char>(peek())))
          num.push_back(get());
      }
      return make(TokenKind::IntLit, num, b, pos());
    }

    // Punctuators / two-char ops
    auto two = std::string_view(src_.data() + i_, std::min<std::size_t>(2, src_.size() - i_));
    if (two == "==") {
      get();
      get();
      return make(TokenKind::EqEq, "==", b, pos());
    }
    if (two == "!=") {
      get();
      get();
      return make(TokenKind::NotEq, "!=", b, pos());
    }
    if (two == "<=") {
      get();
      get();
      return make(TokenKind::Le, "<=", b, pos());
    }
    if (two == ">=") {
      get();
      get();
      return make(TokenKind::Ge, ">=", b, pos());
    }

    // Single-char tokens
    switch (c) {
      case '{':
        get();
        return make(TokenKind::LBrace, "{", b, pos());
      case '}':
        get();
        return make(TokenKind::RBrace, "}", b, pos());
      case '(':
        get();
        return make(TokenKind::LParen, "(", b, pos());
      case ')':
        get();
        return make(TokenKind::RParen, ")", b, pos());
      case '[':
        get();
        return make(TokenKind::LBracket, "[", b, pos());
      case ']':
        get();
        return make(TokenKind::RBracket, "]", b, pos());
      case ':':
        get();
        return make(TokenKind::Colon, ":", b, pos());
      case ';':
        get();
        return make(TokenKind::Semicolon, ";", b, pos());
      case ',':
        get();
        return make(TokenKind::Comma, ",", b, pos());
      case '.':
        get();
        return make(TokenKind::Dot, ".", b, pos());
      case '+':
        get();
        return make(TokenKind::Plus, "+", b, pos());
      case '-':
        get();
        return make(TokenKind::Minus, "-", b, pos());
      case '*':
        get();
        return make(TokenKind::Star, "*", b, pos());
      case '/':
        get();
        return make(TokenKind::Slash, "/", b, pos());
      case '%':
        get();
        return make(TokenKind::Percent, "%", b, pos());
      case '&':
        get();
        return make(TokenKind::Amp, "&", b, pos());
      case '|':
        get();
        return make(TokenKind::Pipe, "|", b, pos());
      case '~':
        get();
        return make(TokenKind::Tilde, "~", b, pos());
      case '=':
        get();
        return make(TokenKind::Equal, "=", b, pos());
      case '<':
        get();
        return make(TokenKind::Lt, "<", b, pos());
      case '>':
        get();
        return make(TokenKind::Gt, ">", b, pos());
      default:
        break;
    }

    // Identifier / keyword
    if (isIdentStart(c)) {
      std::string name;
      while (isIdentCont(peek()))
        name.push_back(get());

      if (name == "struct")
        return make(TokenKind::KwStruct, name, b, pos());
      if (name == "fun")
        return make(TokenKind::KwFun, name, b, pos());
      if (name == "sym")
        return make(TokenKind::KwSym, name, b, pos());
      if (name == "let")
        return make(TokenKind::KwLet, name, b, pos());
      if (name == "mut")
        return make(TokenKind::KwMut, name, b, pos());
      if (name == "assume")
        return make(TokenKind::KwAssume, name, b, pos());
      if (name == "require")
        return make(TokenKind::KwRequire, name, b, pos());
      if (name == "br")
        return make(TokenKind::KwBr, name, b, pos());
      if (name == "ret")
        return make(TokenKind::KwRet, name, b, pos());
      if (name == "unreachable")
        return make(TokenKind::KwUnreachable, name, b, pos());
      if (name == "in")
        return make(TokenKind::KwIn, name, b, pos());
      if (name == "select")
        return make(TokenKind::KwSelect, name, b, pos());
      if (name == "undef")
        return make(TokenKind::KwUndef, name, b, pos());
      if (name == "as")
        return make(TokenKind::KwAs, name, b, pos());

      if (name.size() >= 2 && name[0] == 'i' && std::isdigit(static_cast<unsigned char>(name[1]))) {
        bool allDigits = true;
        for (size_t k = 1; k < name.size(); ++k) {
          if (!std::isdigit(static_cast<unsigned char>(name[k]))) {
            allDigits = false;
            break;
          }
        }
        if (allDigits)
          return make(TokenKind::IntType, name, b, pos());
      }

      return make(TokenKind::Ident, name, b, pos());
    }

    // Unknown character
    {
      std::string msg = "Unexpected character: '";
      msg.push_back(c);
      msg.push_back('"');
      throw ParseError(msg, SourceSpan{b, pos()});
    }
  }

} // namespace symir
