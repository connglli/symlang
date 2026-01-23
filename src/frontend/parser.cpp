#include "frontend/parser.hpp"
#include <sstream>

namespace symir {

  Parser::Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

  Program Parser::parseProgram() {
    Program p;
    p.span.begin = peek().span.begin;

    while (!is(TokenKind::End)) {
      if (is(TokenKind::KwStruct)) {
        p.structs.push_back(parseStructDecl());
      } else if (is(TokenKind::KwFun)) {
        p.funs.push_back(parseFunDecl());
      } else {
        errorHere("Expected 'struct' or 'fun' at top level");
      }
    }

    p.span.end = peek().span.end;
    return p;
  }

  const Token &Parser::peek(std::size_t k) const {
    std::size_t j = idx_ + k;
    if (j >= toks_.size())
      return toks_.back();
    return toks_[j];
  }

  bool Parser::is(TokenKind k) const { return peek().kind == k; }

  const Token &Parser::consume(TokenKind k, const char *what) {
    if (!is(k)) {
      std::ostringstream oss;
      oss << "Expected " << what << ", got '" << peek().lexeme << "'";
      throw ParseError(oss.str(), peek().span);
    }
    return toks_[idx_++];
  }

  bool Parser::tryConsume(TokenKind k) {
    if (is(k)) {
      idx_++;
      return true;
    }
    return false;
  }

  void Parser::errorHere(const std::string &msg) const { throw ParseError(msg, peek().span); }

  GlobalId Parser::parseGlobalId() {
    const Token &t = consume(TokenKind::GlobalId, "global identifier like @name");
    return GlobalId{t.lexeme, t.span};
  }

  LocalId Parser::parseLocalId() {
    const Token &t = consume(TokenKind::LocalId, "local identifier like %name");
    return LocalId{t.lexeme, t.span};
  }

  SymId Parser::parseSymId() {
    const Token &t = consume(TokenKind::SymId, "symbol identifier like @?name or %?name");
    return SymId{t.lexeme, t.span};
  }

  BlockLabel Parser::parseBlockLabel() {
    const Token &t = consume(TokenKind::BlockLabel, "block label like ^name");
    return BlockLabel{t.lexeme, t.span};
  }

  TypePtr Parser::parseType() {
    SourcePos b = peek().span.begin;
    if (is(TokenKind::IntType)) {
      std::string lex = consume(TokenKind::IntType, "integer type").lexeme;
      int bits = std::stoi(lex.substr(1));
      IntType it;
      if (bits == 32)
        it.kind = IntType::Kind::I32;
      else if (bits == 64)
        it.kind = IntType::Kind::I64;
      else {
        it.kind = IntType::Kind::ICustom;
        it.bits = bits;
      }
      it.span = SourceSpan{b, prevEnd()};
      return std::make_shared<Type>(it, SourceSpan{b, prevEnd()});
    }
    if (is(TokenKind::GlobalId)) {
      GlobalId name = parseGlobalId();
      return std::make_shared<Type>(
          StructType{std::move(name), SourceSpan{b, prevEnd()}}, SourceSpan{b, prevEnd()}
      );
    }
    if (tryConsume(TokenKind::LBracket)) {
      Token t = consume(TokenKind::IntLit, "array size");
      std::size_t size = std::stoull(t.lexeme);
      consume(TokenKind::RBracket, "]' after array size");
      TypePtr elem = parseType();
      return std::make_shared<Type>(
          ArrayType{size, std::move(elem), SourceSpan{b, prevEnd()}}, SourceSpan{b, prevEnd()}
      );
    }
    errorHere("Expected a type (iN, array type, or struct type @Name)");
  }

  SourcePos Parser::prevEnd() const {
    if (idx_ == 0)
      return toks_[0].span.begin;
    return toks_[idx_ - 1].span.end;
  }

  StructDecl Parser::parseStructDecl() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwStruct, "'struct'");
    GlobalId name = parseGlobalId();
    consume(TokenKind::LBrace, "'{'");

    std::vector<FieldDecl> fields;
    while (!is(TokenKind::RBrace)) {
      const Token &fname = consume(TokenKind::Ident, "field name");
      consume(TokenKind::Colon, "':'");
      TypePtr ty = parseType();
      consume(TokenKind::Semicolon, "';'");
      FieldDecl f{fname.lexeme, ty, SourceSpan{fname.span.begin, prevEnd()}};
      fields.push_back(std::move(f));
    }

    consume(TokenKind::RBrace, "'}'");
    return StructDecl{name, std::move(fields), SourceSpan{b, prevEnd()}};
  }

  FunDecl Parser::parseFunDecl() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwFun, "'fun'");
    GlobalId name = parseGlobalId();
    consume(TokenKind::LParen, "'('");

    std::vector<ParamDecl> params;
    if (!is(TokenKind::RParen)) {
      params = parseParamList();
    }
    consume(TokenKind::RParen, "')'");
    consume(TokenKind::Colon, "':'");
    TypePtr retTy = parseType();
    consume(TokenKind::LBrace, "'{'");

    std::vector<SymDecl> syms;
    std::vector<LetDecl> lets;

    while (is(TokenKind::KwSym) || is(TokenKind::KwLet)) {
      if (is(TokenKind::KwSym))
        syms.push_back(parseSymDecl());
      else
        lets.push_back(parseLetDecl());
    }

    std::vector<Block> blocks;
    while (is(TokenKind::BlockLabel)) {
      blocks.push_back(parseBlock());
    }

    consume(TokenKind::RBrace, "'}'");

    FunDecl f;
    f.name = name;
    f.params = std::move(params);
    f.retType = retTy;
    f.syms = std::move(syms);
    f.lets = std::move(lets);
    f.blocks = std::move(blocks);
    f.span = SourceSpan{b, prevEnd()};
    return f;
  }

  std::vector<ParamDecl> Parser::parseParamList() {
    std::vector<ParamDecl> params;
    while (true) {
      SourcePos b = peek().span.begin;
      LocalId id = parseLocalId();
      consume(TokenKind::Colon, "':'");
      TypePtr ty = parseType();
      params.push_back(ParamDecl{id, ty, SourceSpan{b, prevEnd()}});
      if (!tryConsume(TokenKind::Comma))
        break;
    }
    return params;
  }

  SymKind Parser::parseSymKind() {
    const Token &t = consume(TokenKind::Ident, "symbol kind (value/coef/index)");
    if (t.lexeme == "value")
      return SymKind::Value;
    if (t.lexeme == "coef")
      return SymKind::Coef;
    if (t.lexeme == "index")
      return SymKind::Index;
    throw ParseError("Unknown symbol kind: " + t.lexeme, t.span);
  }

  std::optional<Domain> Parser::parseOptionalDomain() {
    if (!is(TokenKind::KwIn))
      return std::nullopt;
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwIn, "'in'");

    if (tryConsume(TokenKind::LBracket)) {
      const Token &loT = consume(TokenKind::IntLit, "domain interval lower bound");
      consume(TokenKind::Comma, "','");
      const Token &hiT = consume(TokenKind::IntLit, "domain interval upper bound");
      consume(TokenKind::RBracket, "']'");
      DomainInterval di;
      di.lo = std::stoll(loT.lexeme);
      di.hi = std::stoll(hiT.lexeme);
      di.span = SourceSpan{b, prevEnd()};
      return Domain{di};
    }

    if (tryConsume(TokenKind::LBrace)) {
      DomainSet ds;
      ds.span.begin = b;
      if (!is(TokenKind::RBrace)) {
        while (true) {
          const Token &v = consume(TokenKind::IntLit, "domain set element");
          ds.values.push_back(std::stoll(v.lexeme));
          if (!tryConsume(TokenKind::Comma))
            break;
        }
      }
      consume(TokenKind::RBrace, "'}'");
      ds.span.end = prevEnd();
      return Domain{ds};
    }

    errorHere("Expected domain interval [lo,hi] or set {a,b,...} after 'in'");
  }

  SymDecl Parser::parseSymDecl() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwSym, "'sym'");

    SymId sid = parseSymId();
    consume(TokenKind::Colon, "':'");

    SymKind kind = parseSymKind();
    TypePtr ty = parseType();

    std::optional<Domain> dom = parseOptionalDomain();
    consume(TokenKind::Semicolon, "';'");
    return SymDecl{sid, kind, ty, dom, SourceSpan{b, prevEnd()}};
  }

  LetDecl Parser::parseLetDecl() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwLet, "'let'");
    bool isMut = tryConsume(TokenKind::KwMut);

    LocalId id = parseLocalId();
    consume(TokenKind::Colon, "':'");
    TypePtr ty = parseType();

    std::optional<InitVal> init;
    if (tryConsume(TokenKind::Equal)) {
      init = parseInitVal();
    }
    consume(TokenKind::Semicolon, "';'");
    return LetDecl{isMut, id, ty, init, SourceSpan{b, prevEnd()}};
  }

  InitVal Parser::parseInitVal() {
    SourcePos b = peek().span.begin;

    if (tryConsume(TokenKind::LBrace)) {
      std::vector<InitValPtr> elements;
      if (is(TokenKind::RBrace)) {
        throw ParseError("Empty brace initializers '{}' are disallowed", peek().span);
      }
      while (true) {
        elements.push_back(std::make_shared<InitVal>(parseInitVal()));
        if (!tryConsume(TokenKind::Comma))
          break;
      }
      consume(TokenKind::RBrace, "'}'");
      InitVal iv;
      iv.kind = InitVal::Kind::Aggregate;
      iv.value = std::move(elements);
      iv.span = SourceSpan{b, prevEnd()};
      return iv;
    }

    if (is(TokenKind::KwUndef)) {
      consume(TokenKind::KwUndef, "'undef'");
      InitVal iv;
      iv.kind = InitVal::Kind::Undef;
      iv.span = SourceSpan{b, prevEnd()};
      return iv;
    }

    if (is(TokenKind::IntLit)) {
      const Token &t = consume(TokenKind::IntLit, "integer literal");
      IntLit lit{std::stoll(t.lexeme), t.span};
      InitVal iv;
      iv.kind = InitVal::Kind::Int;
      iv.value = lit;
      iv.span = SourceSpan{b, prevEnd()};
      return iv;
    }

    if (is(TokenKind::SymId)) {
      SymId sid = parseSymId();
      InitVal iv;
      iv.kind = InitVal::Kind::Sym;
      iv.value = sid;
      iv.span = SourceSpan{b, prevEnd()};
      return iv;
    }

    if (is(TokenKind::LocalId)) {
      LocalId lid = parseLocalId();
      InitVal iv;
      iv.kind = InitVal::Kind::Local;
      iv.value = lid;
      iv.span = SourceSpan{b, prevEnd()};
      return iv;
    }

    errorHere("Expected initializer: IntLit, SymId, LocalId, 'undef', or '{...}'");
  }

  Block Parser::parseBlock() {
    SourcePos b = peek().span.begin;
    BlockLabel lab = parseBlockLabel();
    consume(TokenKind::Colon, "':'");

    std::vector<Instr> instrs;
    while (isStartOfInstr()) {
      instrs.push_back(parseInstr());
    }

    Terminator term = parseTerminator();
    return Block{lab, std::move(instrs), std::move(term), SourceSpan{b, prevEnd()}};
  }

  bool Parser::isStartOfInstr() const {
    return is(TokenKind::LocalId) || is(TokenKind::KwAssume) || is(TokenKind::KwRequire);
  }

  Instr Parser::parseInstr() {
    if (is(TokenKind::KwAssume))
      return Instr{parseAssumeInstr()};
    if (is(TokenKind::KwRequire))
      return Instr{parseRequireInstr()};
    return Instr{parseAssignInstr()};
  }

  AssignInstr Parser::parseAssignInstr() {
    SourcePos b = peek().span.begin;
    LValue lhs = parseLValue();
    consume(TokenKind::Equal, "'='");
    Expr rhs = parseExpr();
    consume(TokenKind::Semicolon, "';'");
    return AssignInstr{std::move(lhs), std::move(rhs), SourceSpan{b, prevEnd()}};
  }

  AssumeInstr Parser::parseAssumeInstr() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwAssume, "'assume'");
    Cond c = parseCond();
    consume(TokenKind::Semicolon, "';'");
    return AssumeInstr{std::move(c), SourceSpan{b, prevEnd()}};
  }

  RequireInstr Parser::parseRequireInstr() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwRequire, "'require'");
    Cond c = parseCond();

    std::optional<std::string> msg;
    if (tryConsume(TokenKind::Comma)) {
      const Token &s = consume(TokenKind::StringLit, "string literal message");
      msg = s.lexeme;
    }
    consume(TokenKind::Semicolon, "';'");
    return RequireInstr{std::move(c), msg, SourceSpan{b, prevEnd()}};
  }

  Terminator Parser::parseTerminator() {
    if (is(TokenKind::KwBr))
      return Terminator{parseBrTerm()};
    if (is(TokenKind::KwRet))
      return Terminator{parseRetTerm()};
    if (is(TokenKind::KwUnreachable))
      return Terminator{parseUnreachableTerm()};
    errorHere("Expected terminator: br/ret/unreachable");
  }

  BrTerm Parser::parseBrTerm() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwBr, "'br'");

    BrTerm bt;
    bt.span.begin = b;

    if (is(TokenKind::BlockLabel)) {
      bt.isConditional = false;
      bt.dest = parseBlockLabel();
      consume(TokenKind::Semicolon, "';'");
      bt.span.end = prevEnd();
      return bt;
    }

    bt.isConditional = true;
    bt.cond = parseCond();
    consume(TokenKind::Comma, "','");
    bt.thenLabel = parseBlockLabel();
    consume(TokenKind::Comma, "','");
    bt.elseLabel = parseBlockLabel();
    consume(TokenKind::Semicolon, "';'");
    bt.span.end = prevEnd();
    return bt;
  }

  RetTerm Parser::parseRetTerm() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwRet, "'ret'");
    RetTerm rt;
    rt.span.begin = b;

    if (!is(TokenKind::Semicolon)) {
      rt.value = parseExpr();
    }
    consume(TokenKind::Semicolon, "';'");
    rt.span.end = prevEnd();
    return rt;
  }

  UnreachableTerm Parser::parseUnreachableTerm() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwUnreachable, "'unreachable'");
    consume(TokenKind::Semicolon, "';'");
    return UnreachableTerm{SourceSpan{b, prevEnd()}};
  }

  LValue Parser::parseLValue() {
    SourcePos b = peek().span.begin;
    LocalId base = parseLocalId();
    std::vector<Access> acc;

    while (true) {
      if (tryConsume(TokenKind::LBracket)) {
        SourcePos ib = prevEnd();
        Index idx = parseIndex();
        consume(TokenKind::RBracket, "']'");
        SourceSpan sp{SourcePos{ib.offset, ib.line, ib.col}, prevEnd()};
        acc.push_back(AccessIndex{idx, sp});
        continue;
      }
      if (tryConsume(TokenKind::Dot)) {
        const Token &fld = consume(TokenKind::Ident, "field name after '.'");
        acc.push_back(AccessField{fld.lexeme, fld.span});
        continue;
      }
      break;
    }

    return LValue{base, std::move(acc), SourceSpan{b, prevEnd()}};
  }

  Index Parser::parseIndex() {
    if (is(TokenKind::IntLit)) {
      const Token &t = consume(TokenKind::IntLit, "integer index");
      return Index{IntLit{std::stoll(t.lexeme), t.span}};
    }
    if (is(TokenKind::LocalId)) {
      return Index{LocalOrSymId{parseLocalId()}};
    }
    if (is(TokenKind::SymId)) {
      return Index{LocalOrSymId{parseSymId()}};
    }
    errorHere("Expected index: IntLit, LocalId, or SymId");
  }

  Coef Parser::parseCoef() {
    if (is(TokenKind::IntLit)) {
      const Token &t = consume(TokenKind::IntLit, "integer literal");
      return Coef{IntLit{std::stoll(t.lexeme), t.span}};
    }
    if (is(TokenKind::LocalId)) {
      return Coef{LocalOrSymId{parseLocalId()}};
    }
    if (is(TokenKind::SymId)) {
      return Coef{LocalOrSymId{parseSymId()}};
    }
    errorHere("Expected coefficient: IntLit, LocalId, or SymId");
  }

  Cond Parser::parseCond() {
    SourcePos b = peek().span.begin;
    Expr lhs = parseExpr();
    RelOp op = parseRelOp();
    Expr rhs = parseExpr();
    return Cond{std::move(lhs), op, std::move(rhs), SourceSpan{b, prevEnd()}};
  }

  RelOp Parser::parseRelOp() {
    if (tryConsume(TokenKind::EqEq))
      return RelOp::EQ;
    if (tryConsume(TokenKind::NotEq))
      return RelOp::NE;
    if (tryConsume(TokenKind::Le))
      return RelOp::LE;
    if (tryConsume(TokenKind::Ge))
      return RelOp::GE;
    if (tryConsume(TokenKind::Lt))
      return RelOp::LT;
    if (tryConsume(TokenKind::Gt))
      return RelOp::GT;
    errorHere("Expected relational operator (==, !=, <, <=, >, >=)");
  }

  Expr Parser::parseExpr() {
    SourcePos b = peek().span.begin;
    Atom first = parseAtom();
    Expr e;
    e.first = std::move(first);

    while (is(TokenKind::Plus) || is(TokenKind::Minus)) {
      SourcePos tb = peek().span.begin;
      AddOp op = is(TokenKind::Plus) ? AddOp::Plus : AddOp::Minus;
      idx_++; // consume +/-
      Atom a = parseAtom();
      e.rest.push_back(Expr::Tail{op, std::move(a), SourceSpan{tb, prevEnd()}});
    }

    e.span = SourceSpan{b, prevEnd()};
    return e;
  }

  Atom Parser::parseAtom() {
    SourcePos b = peek().span.begin;

    if (is(TokenKind::KwSelect)) {
      consume(TokenKind::KwSelect, "'select'");
      Cond c = parseCond();
      consume(TokenKind::Comma, "','");
      SelectVal t = parseSelectVal();
      consume(TokenKind::Comma, "','");
      SelectVal f = parseSelectVal();
      SelectAtom sa;
      sa.cond = std::make_unique<Cond>(std::move(c));
      sa.vtrue = std::move(t);
      sa.vfalse = std::move(f);
      sa.span = SourceSpan{b, prevEnd()};
      Atom atom;
      atom.v = std::move(sa);
      atom.span = SourceSpan{b, prevEnd()};
      return atom;
    }

    // Handle 'as' for IntLit, SymId, or LocalId (LValue)
    if (is(TokenKind::IntLit)) {
      std::size_t save = idx_;
      Coef c = parseCoef();
      if (tryConsume(TokenKind::KwAs)) {
        TypePtr dst = parseType();
        CastAtom ca{std::get<IntLit>(c), std::move(dst), SourceSpan{b, prevEnd()}};
        Atom a;
        a.v = std::move(ca);
        a.span = SourceSpan{b, prevEnd()};
        return a;
      }
      idx_ = save;
    } else if (is(TokenKind::SymId)) {
      std::size_t save = idx_;
      Coef c = parseCoef();
      if (tryConsume(TokenKind::KwAs)) {
        TypePtr dst = parseType();
        auto &lsid = std::get<LocalOrSymId>(c);
        if (auto sid = std::get_if<SymId>(&lsid)) {
          CastAtom ca{std::move(*sid), std::move(dst), SourceSpan{b, prevEnd()}};
          Atom a;
          a.v = std::move(ca);
          a.span = SourceSpan{b, prevEnd()};
          return a;
        }
      }
      idx_ = save;
    } else if (is(TokenKind::LocalId)) {
      std::size_t save = idx_;
      LValue lv = parseLValue();
      if (tryConsume(TokenKind::KwAs)) {
        TypePtr dst = parseType();
        CastAtom ca{std::move(lv), std::move(dst), SourceSpan{b, prevEnd()}};
        Atom a;
        a.v = std::move(ca);
        a.span = SourceSpan{b, prevEnd()};
        return a;
      }
      idx_ = save;
    }

    if (is(TokenKind::LocalId)) {
      std::size_t save = idx_;
      LValue lv = parseLValue();
      if (!lv.accesses.empty()) {
        if (is(TokenKind::Star) || is(TokenKind::Slash) || is(TokenKind::Percent)) {
          throw ParseError(
              "An accessed lvalue cannot be used as a coefficient for '*', '/', '%'", peek().span
          );
        }
        RValueAtom ra{std::move(lv), SourceSpan{b, prevEnd()}};
        Atom a;
        a.v = std::move(ra);
        a.span = SourceSpan{b, prevEnd()};
        return a;
      }
      idx_ = save;
    }

    if (is(TokenKind::IntLit) || is(TokenKind::LocalId) || is(TokenKind::SymId)) {
      std::size_t save = idx_;
      Coef c = parseCoef();
      if (is(TokenKind::Star) || is(TokenKind::Slash) || is(TokenKind::Percent)) {
        AtomOpKind op = parseAtomOp();
        RValue rv = parseLValue();
        OpAtom oa{op, std::move(c), std::move(rv), SourceSpan{b, prevEnd()}};
        Atom a;
        a.v = std::move(oa);
        a.span = SourceSpan{b, prevEnd()};
        return a;
      }

      if (std::holds_alternative<LocalOrSymId>(c) &&
          std::holds_alternative<LocalId>(std::get<LocalOrSymId>(c))) {
        idx_ = save;
        LValue lv = parseLValue();
        RValueAtom ra{std::move(lv), SourceSpan{b, prevEnd()}};
        Atom a;
        a.v = std::move(ra);
        a.span = SourceSpan{b, prevEnd()};
        return a;
      } else {
        CoefAtom ca{std::move(c), SourceSpan{b, prevEnd()}};
        Atom a;
        a.v = std::move(ca);
        a.span = SourceSpan{b, prevEnd()};
        return a;
      }
    }

    errorHere("Expected atom (select, cast, coefficient, or lvalue)");
  }

  AtomOpKind Parser::parseAtomOp() {
    if (tryConsume(TokenKind::Star))
      return AtomOpKind::Mul;
    if (tryConsume(TokenKind::Slash))
      return AtomOpKind::Div;
    if (tryConsume(TokenKind::Percent))
      return AtomOpKind::Mod;
    errorHere("Expected atom operator (*, /, %)");
  }

  SelectVal Parser::parseSelectVal() {
    if (is(TokenKind::LocalId)) {
      RValue rv = parseLValue();
      return SelectVal{rv};
    }
    if (is(TokenKind::IntLit) || is(TokenKind::SymId)) {
      Coef c = parseCoef();
      return SelectVal{c};
    }
    errorHere("Expected select arm value: lvalue or coefficient");
  }

} // namespace symir
