#include "frontend/parser.hpp"
#include <algorithm>
#include <iostream>

namespace symir {

  Parser::Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

  Program Parser::parseProgram() {
    Program prog;
    while (!is(TokenKind::End)) {
      if (is(TokenKind::KwStruct)) {
        prog.structs.push_back(parseStructDecl());
      } else if (is(TokenKind::KwFun)) {
        prog.funs.push_back(parseFunDecl());
      } else if (is(TokenKind::KwDecl)) {
        prog.extDecls.push_back(parseExtDecl());
      } else if (is(TokenKind::KwIntrinsic)) {
        prog.intrinsics.push_back(parseIntrinsicDecl());
      } else {
        errorHere("Expected struct, fun, decl, or intrinsic declaration");
      }
    }
    prog.span = SourceSpan{toks_.front().span.begin, prevEnd()};
    return prog;
  }

  const Token &Parser::peek(std::size_t k) const {
    if (idx_ + k >= toks_.size())
      return toks_.back();
    return toks_[idx_ + k];
  }

  bool Parser::is(TokenKind k) const { return peek().kind == k; }

  const Token &Parser::consume(TokenKind k, const char *what) {
    if (is(k)) {
      return toks_[idx_++];
    }
    std::string msg = "Expected ";
    msg += what;
    msg += ", got '";
    msg += peek().lexeme;
    msg += "'";
    throw ParseError(msg, peek().span);
  }

  bool Parser::tryConsume(TokenKind k) {
    if (is(k)) {
      idx_++;
      return true;
    }
    return false;
  }

  [[noreturn]] void Parser::errorHere(const std::string &msg) const {
    throw ParseError(msg, peek().span);
  }

  GlobalId Parser::parseGlobalId() {
    const Token &t = consume(TokenKind::GlobalId, "global identifier (@name)");
    return GlobalId{t.lexeme, t.span};
  }

  LocalId Parser::parseLocalId() {
    const Token &t = consume(TokenKind::LocalId, "local identifier (%name)");
    return LocalId{t.lexeme, t.span};
  }

  SymId Parser::parseSymId() {
    const Token &t = consume(TokenKind::SymId, "symbol identifier (%?name or @?name)");
    return SymId{t.lexeme, t.span};
  }

  BlockLabel Parser::parseBlockLabel() {
    const Token &t = consume(TokenKind::BlockLabel, "block label (^name)");
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
    if (is(TokenKind::FloatType)) {
      std::string lex = consume(TokenKind::FloatType, "float type").lexeme;
      FloatType ft;
      ft.kind = (lex == "f32") ? FloatType::Kind::F32 : FloatType::Kind::F64;
      ft.span = SourceSpan{b, prevEnd()};
      return std::make_shared<Type>(ft, SourceSpan{b, prevEnd()});
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
      consume(TokenKind::RBracket, "']' after array size");
      TypePtr elem = parseType();
      return std::make_shared<Type>(
          ArrayType{size, std::move(elem), SourceSpan{b, prevEnd()}}, SourceSpan{b, prevEnd()}
      );
    }
    if (is(TokenKind::KwPtr)) {
      consume(TokenKind::KwPtr, "'ptr'");
      TypePtr pointee = parseType();
      return std::make_shared<Type>(
          PtrType{std::move(pointee), SourceSpan{b, prevEnd()}}, SourceSpan{b, prevEnd()}
      );
    }
    // [v0.2.1] vector type: <N> ScalarType. N is parsed as IntLit; the
    // typechecker enforces N >= 2 and the elem-is-scalar restriction.
    if (tryConsume(TokenKind::Lt)) {
      Token t = consume(TokenKind::IntLit, "vector lane count N");
      std::size_t size = std::stoull(t.lexeme);
      consume(TokenKind::Gt, "'>' after vector lane count");
      TypePtr elem = parseType();
      return std::make_shared<Type>(
          VecType{size, std::move(elem), SourceSpan{b, prevEnd()}}, SourceSpan{b, prevEnd()}
      );
    }
    errorHere("Expected a type (iN, f32/f64, array type, struct type @Name, ptr T, or <N> T)");
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
    return StructDecl{std::move(name), std::move(fields), SourceSpan{b, prevEnd()}};
  }

  FunDecl Parser::parseFunDecl() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwFun, "'fun'");
    GlobalId name = parseGlobalId();
    consume(TokenKind::LParen, "'('");
    std::vector<ParamDecl> params = parseParamList();
    consume(TokenKind::RParen, "')'");
    consume(TokenKind::Colon, "':'");
    TypePtr ret = parseType();
    consume(TokenKind::LBrace, "'{'");

    std::vector<SymDecl> syms;
    while (is(TokenKind::KwSym)) {
      syms.push_back(parseSymDecl());
    }

    std::vector<LetDecl> lets;
    while (is(TokenKind::KwLet)) {
      lets.push_back(parseLetDecl());
    }

    std::vector<Block> blocks;
    while (!is(TokenKind::RBrace)) {
      blocks.push_back(parseBlock());
    }
    consume(TokenKind::RBrace, "'}'");

    return FunDecl{std::move(name), std::move(params), std::move(ret),          std::move(syms),
                   std::move(lets), std::move(blocks), SourceSpan{b, prevEnd()}};
  }

  ExtDecl Parser::parseExtDecl() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwDecl, "'decl'");
    GlobalId name = parseGlobalId();
    consume(TokenKind::LParen, "'('");
    std::vector<ParamDecl> params = parseParamList();
    consume(TokenKind::RParen, "')'");
    consume(TokenKind::Colon, "':'");
    TypePtr ret = parseType();

    ExtDecl d;
    d.name = std::move(name);
    d.params = std::move(params);
    d.retType = std::move(ret);

    if (tryConsume(TokenKind::Semicolon)) {
      // Link form: signature only; body resolved via -I.
      d.span = SourceSpan{b, prevEnd()};
      return d;
    }

    // Contract form: `{ pre... post... };`
    SourcePos cb = peek().span.begin;
    consume(TokenKind::LBrace, "'{' or ';'");
    Contract contract;
    while (is(TokenKind::KwPre)) {
      SourcePos pb = peek().span.begin;
      consume(TokenKind::KwPre, "'pre'");
      Cond c = parseCond();
      std::optional<std::string> msg;
      if (tryConsume(TokenKind::Comma)) {
        const Token &s = consume(TokenKind::StringLit, "string literal message");
        msg = s.lexeme;
      }
      consume(TokenKind::Semicolon, "';'");
      contract.pres.push_back(PreClause{std::move(c), msg, SourceSpan{pb, prevEnd()}});
    }
    while (is(TokenKind::KwPost)) {
      SourcePos pb = peek().span.begin;
      consume(TokenKind::KwPost, "'post'");
      inPostClause_ = true;
      Cond c = parseCond();
      inPostClause_ = false;
      std::optional<std::string> msg;
      if (tryConsume(TokenKind::Comma)) {
        const Token &s = consume(TokenKind::StringLit, "string literal message");
        msg = s.lexeme;
      }
      consume(TokenKind::Semicolon, "';'");
      contract.posts.push_back(PostClause{std::move(c), msg, SourceSpan{pb, prevEnd()}});
    }
    contract.span = SourceSpan{cb, prevEnd()};
    consume(TokenKind::RBrace, "'}'");
    consume(TokenKind::Semicolon, "';'");
    d.contract = std::move(contract);
    d.span = SourceSpan{b, prevEnd()};
    return d;
  }

  IntrinsicDecl Parser::parseIntrinsicDecl() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwIntrinsic, "'intrinsic'");
    GlobalId name = parseGlobalId();
    consume(TokenKind::LParen, "'('");
    std::vector<ParamDecl> params = parseParamList();
    consume(TokenKind::RParen, "')'");
    consume(TokenKind::Colon, "':'");
    TypePtr ret = parseType();
    consume(TokenKind::Semicolon, "';'");
    return IntrinsicDecl{
        std::move(name), std::move(params), std::move(ret), SourceSpan{b, prevEnd()}
    };
  }

  Atom Parser::parseCallAtom() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwCall, "'call'");
    GlobalId callee = parseGlobalId();
    consume(TokenKind::LParen, "'('");
    std::vector<std::shared_ptr<Expr>> args;
    if (!is(TokenKind::RParen)) {
      while (true) {
        args.push_back(std::make_shared<Expr>(parseExpr()));
        if (!tryConsume(TokenKind::Comma))
          break;
      }
    }
    consume(TokenKind::RParen, "')'");
    CallAtom ca{std::move(callee), std::move(args), SourceSpan{b, prevEnd()}};
    return Atom{std::move(ca), ca.span};
  }

  std::vector<ParamDecl> Parser::parseParamList() {
    std::vector<ParamDecl> params;
    if (is(TokenKind::RParen))
      return params;

    while (true) {
      SourcePos b = peek().span.begin;
      LocalId id = parseLocalId();
      consume(TokenKind::Colon, "':'");
      TypePtr ty = parseType();
      params.push_back(ParamDecl{std::move(id), std::move(ty), SourceSpan{b, prevEnd()}});
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
      di.lo = parseIntegerLiteral(loT.lexeme);
      di.hi = parseIntegerLiteral(hiT.lexeme);
      di.span = SourceSpan{b, prevEnd()};
      return Domain{di};
    }

    if (tryConsume(TokenKind::LBrace)) {
      DomainSet ds;
      ds.span.begin = b;
      if (!is(TokenKind::RBrace)) {
        while (true) {
          const Token &v = consume(TokenKind::IntLit, "domain set element");
          ds.values.push_back(parseIntegerLiteral(v.lexeme));
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

  InitVal Parser::parseInitVal(bool allowAtom) {
    SourcePos b = peek().span.begin;

    if (tryConsume(TokenKind::LBrace)) {
      std::vector<InitValPtr> elements;
      if (is(TokenKind::RBrace)) {
        throw ParseError("Empty brace initializers '{}' are disallowed", peek().span);
      }
      while (true) {
        // Inside braces: forbid atom-form (spec §3.4.2).
        elements.push_back(std::make_shared<InitVal>(parseInitVal(/*allowAtom=*/false)));
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

    if (is(TokenKind::KwNull)) {
      consume(TokenKind::KwNull, "'null'");
      InitVal iv;
      iv.kind = InitVal::Kind::Null;
      iv.span = SourceSpan{b, prevEnd()};
      return iv;
    }

    if (is(TokenKind::IntLit)) {
      const Token &t = consume(TokenKind::IntLit, "integer literal");
      IntLit lit{parseIntegerLiteral(t.lexeme), t.span};
      InitVal iv;
      iv.kind = InitVal::Kind::Int;
      iv.value = lit;
      iv.span = SourceSpan{b, prevEnd()};
      return iv;
    }

    if (is(TokenKind::FloatLit)) {
      const Token &t = consume(TokenKind::FloatLit, "float literal");
      FloatLit lit{parseFloatLiteral(t.lexeme), t.span};
      InitVal iv;
      iv.kind = InitVal::Kind::Float;
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
      // Parse as LValue first; if there are no accesses, treat as a
      // simple Local init. Otherwise, fall through to the atom-form
      // path so `%v[1]` / `%s.f` work as initializers (§3.4.2 v0.2.1).
      LValue lv = parseLValue();
      if (lv.accesses.empty()) {
        InitVal iv;
        iv.kind = InitVal::Kind::Local;
        iv.value = lv.base;
        iv.span = SourceSpan{b, prevEnd()};
        return iv;
      }
      if (!allowAtom)
        errorHere(
            "Atom-form initializer (LValue with accesses) is not permitted inside "
            "aggregate braces (§3.4.2)"
        );
      RValueAtom ra{std::move(lv), SourceSpan{b, prevEnd()}};
      Atom atom{std::move(ra), ra.span};
      InitVal iv;
      iv.kind = InitVal::Kind::Atom;
      iv.value = std::make_shared<Atom>(std::move(atom));
      iv.span = SourceSpan{b, prevEnd()};
      return iv;
    }

    // [v0.2.1] §3.4.2: an atom (addr / load / cmp / ptrindex / ptrfield /
    // select / cast / unary / op) is a valid initializer for a non-
    // aggregate target. Forbidden inside aggregate braces.
    if (is(TokenKind::KwAddr) || is(TokenKind::KwLoad) || is(TokenKind::KwCmp) ||
        is(TokenKind::KwPtrIndex) || is(TokenKind::KwPtrField) || is(TokenKind::KwSelect) ||
        is(TokenKind::KwCall) || is(TokenKind::Tilde)) {
      if (!allowAtom)
        errorHere("Atom-form initializer is not permitted inside aggregate braces (§3.4.2)");
      Atom atom = parseAtom();
      InitVal iv;
      iv.kind = InitVal::Kind::Atom;
      iv.value = std::make_shared<Atom>(std::move(atom));
      iv.span = SourceSpan{b, prevEnd()};
      return iv;
    }

    errorHere(
        "Expected initializer: IntLit, SymId, LocalId, atom (addr/load/cmp/ptrindex/"
        "ptrfield/select), 'undef', 'null', or '{...}'"
    );
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
    return is(TokenKind::LocalId) || is(TokenKind::KwAssume) || is(TokenKind::KwRequire) ||
           is(TokenKind::KwStore);
  }

  Instr Parser::parseInstr() {
    if (is(TokenKind::KwAssume))
      return Instr{parseAssumeInstr()};
    if (is(TokenKind::KwRequire))
      return Instr{parseRequireInstr()};
    if (is(TokenKind::KwStore))
      return Instr{parseStoreInstr()};
    return Instr{parseAssignInstr()};
  }

  StoreInstr Parser::parseStoreInstr() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwStore, "'store'");
    // SPEC §4.2: StoreInstr := "store" RValue "," Expr ";"
    // RValue := LValue  — the pointer must be an lvalue (local or field/index
    // access), not an arbitrary expression. Complex pointer values must be
    // staged through a 'let mut' local first.
    LValue ptrLV = parseLValue();
    SourceSpan ptrSpan{b, prevEnd()};
    // Wrap the LValue into a minimal Expr (single RValueAtom) so the AST
    // field type (Expr) is satisfied while enforcing the RValue restriction.
    RValueAtom ra{ptrLV, ptrSpan};
    Atom ptrAtom{ra, ptrSpan};
    Expr ptr;
    ptr.first = std::move(ptrAtom);
    ptr.span = ptrSpan;
    consume(TokenKind::Comma, "','");
    Expr val = parseExpr();
    consume(TokenKind::Semicolon, "';'");
    return StoreInstr{std::move(ptr), std::move(val), SourceSpan{b, prevEnd()}};
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
    if (is(TokenKind::BlockLabel)) {
      BlockLabel dest = parseBlockLabel();
      consume(TokenKind::Semicolon, "';'");
      BrTerm bt;
      bt.dest = std::move(dest);
      bt.isConditional = false;
      bt.span = SourceSpan{b, prevEnd()};
      return bt;
    } else {
      Cond c = parseCond();
      consume(TokenKind::Comma, "','");
      BlockLabel t = parseBlockLabel();
      consume(TokenKind::Comma, "','");
      BlockLabel f = parseBlockLabel();
      consume(TokenKind::Semicolon, "';'");
      BrTerm bt;
      bt.cond = std::move(c);
      bt.thenLabel = std::move(t);
      bt.elseLabel = std::move(f);
      bt.isConditional = true;
      bt.span = SourceSpan{b, prevEnd()};
      return bt;
    }
  }

  RetTerm Parser::parseRetTerm() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwRet, "'ret'");
    std::optional<Expr> val;
    if (!is(TokenKind::Semicolon)) {
      val = parseExpr();
    }
    consume(TokenKind::Semicolon, "';'");
    return RetTerm{std::move(val), SourceSpan{b, prevEnd()}};
  }

  UnreachableTerm Parser::parseUnreachableTerm() {
    SourcePos b = peek().span.begin;
    consume(TokenKind::KwUnreachable, "'unreachable'");
    consume(TokenKind::Semicolon, "';'");
    return UnreachableTerm{SourceSpan{b, prevEnd()}};
  }

  LValue Parser::parseLValue() {
    SourcePos b = peek().span.begin;
    LocalId base;
    if (inPostClause_ && is(TokenKind::KwRet)) {
      // [v0.2.2] Inside `post` clauses, the bareword `ret` is a reserved
      // identifier referring to the callee's return value. Synthesize a
      // LocalId so the rest of the AST/typechecker sees it uniformly.
      const Token &t = consume(TokenKind::KwRet, "'ret'");
      base = LocalId{"ret", t.span};
    } else {
      base = parseLocalId();
    }
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
      const Token &t = consume(TokenKind::IntLit, "index");
      return Index{IntLit{parseIntegerLiteral(t.lexeme), t.span}};
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
      const Token &t = consume(TokenKind::IntLit, "coefficient");
      return Coef{IntLit{parseIntegerLiteral(t.lexeme), t.span}};
    }
    if (is(TokenKind::FloatLit)) {
      const Token &t = consume(TokenKind::FloatLit, "float coefficient");
      return Coef{FloatLit{parseFloatLiteral(t.lexeme), t.span}};
    }
    if (is(TokenKind::LocalId)) {
      return Coef{LocalOrSymId{parseLocalId()}};
    }
    if (inPostClause_ && is(TokenKind::KwRet)) {
      const Token &t = consume(TokenKind::KwRet, "'ret'");
      return Coef{LocalOrSymId{LocalId{"ret", t.span}}};
    }
    if (is(TokenKind::SymId)) {
      return Coef{LocalOrSymId{parseSymId()}};
    }
    if (is(TokenKind::KwNull)) {
      const Token &t = consume(TokenKind::KwNull, "'null'");
      return Coef{NullLit{t.span}};
    }
    errorHere("Expected coefficient: IntLit, LocalId, SymId, or 'null'");
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

    if (is(TokenKind::KwCall)) {
      return parseCallAtom();
    }

    if (is(TokenKind::KwAddr)) {
      consume(TokenKind::KwAddr, "'addr'");
      LValue lv = parseLValue();
      AddrAtom aa{std::move(lv), SourceSpan{b, prevEnd()}};
      return Atom{std::move(aa), aa.span};
    }

    if (is(TokenKind::KwLoad)) {
      consume(TokenKind::KwLoad, "'load'");
      RValue rv = parseLValue();
      LoadAtom la{std::move(rv), SourceSpan{b, prevEnd()}};
      return Atom{std::move(la), la.span};
    }

    if (is(TokenKind::KwSelect)) {
      consume(TokenKind::KwSelect, "'select'");
      // [v0.2.1] Two select forms. Parse the first Expr, then dispatch on
      // the next token: RelOp → Cond form; Comma → mask form.
      SourcePos sb = peek().span.begin;
      Expr first = parseExpr();
      SelectAtom sa;
      if (is(TokenKind::EqEq) || is(TokenKind::NotEq) || is(TokenKind::Lt) || is(TokenKind::Le) ||
          is(TokenKind::Gt) || is(TokenKind::Ge)) {
        RelOp op = parseRelOp();
        Expr rhs = parseExpr();
        Cond c{std::move(first), op, std::move(rhs), SourceSpan{sb, prevEnd()}};
        sa.cond = std::make_unique<Cond>(std::move(c));
      } else {
        sa.maskExpr = std::make_unique<Expr>(std::move(first));
      }
      consume(TokenKind::Comma, "','");
      SelectVal t = parseSelectVal();
      consume(TokenKind::Comma, "','");
      SelectVal f = parseSelectVal();
      sa.vtrue = std::move(t);
      sa.vfalse = std::move(f);
      sa.span = SourceSpan{b, prevEnd()};
      return Atom{std::move(sa), sa.span};
    }

    // [v0.2.1] cmp atom: cmp <RelOp> <lhs>, <rhs>
    // Produces i1 for scalar operands, <N> i1 for vector operands.
    if (is(TokenKind::KwCmp)) {
      consume(TokenKind::KwCmp, "'cmp'");
      RelOp op = parseRelOp();
      SelectVal lhs = parseSelectVal();
      consume(TokenKind::Comma, "','");
      SelectVal rhs = parseSelectVal();
      CmpAtom ca{op, std::move(lhs), std::move(rhs), SourceSpan{b, prevEnd()}};
      return Atom{std::move(ca), ca.span};
    }

    // [v0.2.1] ptrindex atom: ptrindex <RValue>, <Index>
    if (is(TokenKind::KwPtrIndex)) {
      consume(TokenKind::KwPtrIndex, "'ptrindex'");
      RValue rv = parseLValue();
      consume(TokenKind::Comma, "','");
      Index idx = parseIndex();
      PtrIndexAtom pa{std::move(rv), std::move(idx), SourceSpan{b, prevEnd()}};
      return Atom{std::move(pa), pa.span};
    }

    // [v0.2.1] ptrfield atom: ptrfield <RValue>, <Ident>
    if (is(TokenKind::KwPtrField)) {
      consume(TokenKind::KwPtrField, "'ptrfield'");
      RValue rv = parseLValue();
      consume(TokenKind::Comma, "','");
      const Token &fld = consume(TokenKind::Ident, "field name");
      PtrFieldAtom pa{std::move(rv), fld.lexeme, SourceSpan{b, prevEnd()}};
      return Atom{std::move(pa), pa.span};
    }

    if (tryConsume(TokenKind::Tilde)) {
      RValue rv = parseLValue();
      UnaryAtom ua{UnaryOpKind::Not, std::move(rv), SourceSpan{b, prevEnd()}};
      return Atom{std::move(ua), ua.span};
    }

    // Try binary op or cast
    std::size_t save = idx_;
    try {
      Coef c = parseCoef();
      if (is(TokenKind::Star) || is(TokenKind::Slash) || is(TokenKind::Percent) ||
          is(TokenKind::Amp) || is(TokenKind::Pipe) || is(TokenKind::Caret) || is(TokenKind::Shl) ||
          is(TokenKind::Shr) || is(TokenKind::LShr)) {

        // Disallow accessed lvalues as coefficients
        if (auto lsid = std::get_if<LocalOrSymId>(&c)) {
          if (std::holds_alternative<LocalId>(*lsid)) {
            idx_ = save;
            LValue lv = parseLValue();
            if (!lv.accesses.empty()) {
              throw ParseError(
                  "An accessed lvalue cannot be used as a coefficient for binary operators",
                  peek().span
              );
            }
            idx_ = save;
            c = parseCoef(); // re-consume
          }
        }

        AtomOpKind op = parseAtomOp();
        RValue rv = parseLValue();
        OpAtom oa{op, std::move(c), std::move(rv), SourceSpan{b, prevEnd()}};
        return Atom{std::move(oa), oa.span};
      }

      // Try 'as'
      // For a LocalId coef, the coef parser only consumed the bare identifier —
      // any array/field accesses remain in the token stream. Re-parse as a full
      // LValue first so that `%a0[0] as i32` is handled correctly.
      if (auto lsid = std::get_if<LocalOrSymId>(&c)) {
        if (std::holds_alternative<LocalId>(*lsid)) {
          // Re-parse as full LValue (consumes accesses), then look for 'as'
          idx_ = save;
          LValue lv = parseLValue();
          if (tryConsume(TokenKind::KwAs)) {
            TypePtr dst = parseType();
            CastAtom ca;
            ca.src = std::move(lv);
            ca.dstType = std::move(dst);
            ca.span = SourceSpan{b, prevEnd()};
            return Atom{std::move(ca), ca.span};
          }
          // No 'as' — not a cast; reset and fall through to leaf atom
          idx_ = save;
        }
      }
      if (tryConsume(TokenKind::KwAs)) {
        TypePtr dst = parseType();
        CastAtom ca;
        if (auto lit = std::get_if<IntLit>(&c)) {
          ca.src = *lit;
        } else if (auto flit = std::get_if<FloatLit>(&c)) {
          ca.src = *flit;
        } else if (std::get_if<NullLit>(&c)) {
          // `null as T` is not supported; use null in a typed context instead
          throw ParseError("'null as T' is not supported; null is inferred from context", ca.span);
        } else {
          auto &lsid = std::get<LocalOrSymId>(c);
          if (auto sid = std::get_if<SymId>(&lsid)) {
            ca.src = *sid;
          } else {
            // Simple LocalId (no accesses) — use as-is
            auto &lid = std::get<LocalId>(lsid);
            ca.src = LValue{lid, {}, {}};
          }
        }
        ca.dstType = std::move(dst);
        ca.span = SourceSpan{b, prevEnd()};
        return Atom{std::move(ca), ca.span};
      }
    } catch (const ParseError &e) {
      // If it's a specific "accessed lvalue" error, propagate it
      if (std::string(e.what()).find("accessed lvalue") != std::string::npos)
        throw;
    }
    idx_ = save;

    // Leaf Atom
    if (is(TokenKind::IntLit) || is(TokenKind::FloatLit) || is(TokenKind::SymId) ||
        is(TokenKind::LocalId) || is(TokenKind::KwNull) ||
        (inPostClause_ && is(TokenKind::KwRet))) {
      std::size_t save_leaf = idx_;
      Coef c = parseCoef();
      if (auto lsid = std::get_if<LocalOrSymId>(&c)) {
        if (std::holds_alternative<LocalId>(*lsid)) {
          idx_ = save_leaf;
          LValue lv = parseLValue();
          return Atom{
              RValueAtom{std::move(lv), SourceSpan{b, prevEnd()}}, SourceSpan{b, prevEnd()}
          };
        }
      }
      return Atom{CoefAtom{std::move(c), SourceSpan{b, prevEnd()}}, SourceSpan{b, prevEnd()}};
    }

    errorHere("Expected atom (select, cast, bitwise not, coefficient, or lvalue)");
  }

  AtomOpKind Parser::parseAtomOp() {
    if (tryConsume(TokenKind::Star))
      return AtomOpKind::Mul;
    if (tryConsume(TokenKind::Slash))
      return AtomOpKind::Div;
    if (tryConsume(TokenKind::Percent))
      return AtomOpKind::Mod;
    if (tryConsume(TokenKind::Amp))
      return AtomOpKind::And;
    if (tryConsume(TokenKind::Pipe))
      return AtomOpKind::Or;
    if (tryConsume(TokenKind::Caret))
      return AtomOpKind::Xor;
    if (tryConsume(TokenKind::Shl))
      return AtomOpKind::Shl;
    if (tryConsume(TokenKind::Shr))
      return AtomOpKind::Shr;
    if (tryConsume(TokenKind::LShr))
      return AtomOpKind::LShr;
    errorHere("Expected atom operator (*, /, %, &, |, ^, <<, >>, >>>)");
  }

  SelectVal Parser::parseSelectVal() {
    if (is(TokenKind::LocalId)) {
      RValue rv = parseLValue();
      return SelectVal{rv};
    }
    if (is(TokenKind::IntLit) || is(TokenKind::FloatLit) || is(TokenKind::SymId) ||
        is(TokenKind::KwNull)) {
      Coef c = parseCoef();
      return SelectVal{c};
    }
    errorHere("Expected select arm value: lvalue, coefficient, or 'null'");
  }

} // namespace symir
