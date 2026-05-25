#include "reify/expr_gen.hpp"

#include <algorithm>
#include <cassert>
#include "reify/hyperparameters.hpp"

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Internal helpers — AST factories
  // ---------------------------------------------------------------------------

  static TypePtr makeI32() {
    return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
  }

  static LValue localLV(const std::string &name) { return LValue{LocalId{name, {}}, {}, {}}; }

  static LValue arrayLV(const std::string &name, int64_t idx) {
    LValue lv;
    lv.base = LocalId{name, {}};
    lv.accesses.push_back(AccessIndex{Index{IntLit{idx, {}}}, {}});
    return lv;
  }

  static LValue structLV(const std::string &name, const std::string &field) {
    LValue lv;
    lv.base = LocalId{name, {}};
    lv.accesses.push_back(AccessField{field, {}});
    return lv;
  }

  static Coef symCoef(const std::string &sname) { return LocalOrSymId{SymId{sname, {}}}; }

  static Coef intCoef(int64_t v) { return IntLit{v, {}}; }

  static Coef floatCoef(double v) { return FloatLit{v, {}}; }

  static Atom coefAtom(Coef c) { return Atom{CoefAtom{std::move(c), {}}, {}}; }

  static Atom opAtom(AtomOpKind op, Coef c, RValue rv) {
    return Atom{OpAtom{op, std::move(c), std::move(rv), {}}, {}};
  }

  static Atom rvalAtom(RValue rv) { return Atom{RValueAtom{std::move(rv), {}}, {}}; }

  static Atom unaryAtom(RValue rv) {
    return Atom{UnaryAtom{UnaryOpKind::Not, std::move(rv), {}}, {}};
  }

  static Expr simpleExpr(Atom a) { return Expr{std::move(a), {}, {}}; }

  // Pick a random element from a non-empty vector
  template<typename T>
  static const T &pickOne(std::mt19937 &rng, const std::vector<T> &v) {
    assert(!v.empty());
    std::uniform_int_distribution<int> d(0, (int) v.size() - 1);
    return v[d(rng)];
  }

  // ---------------------------------------------------------------------------
  // SymCounter implementation
  // ---------------------------------------------------------------------------

  std::string SymCounter::next(SymKind kind, TypePtr type) {
    auto name = "%?s" + std::to_string(n++);
    int64_t lo, hi;
    if (kind == SymKind::Coef) {
      lo = coefLo;
      hi = coefHi;
    } else if (kind == SymKind::Value) {
      lo = valueLo;
      hi = valueHi;
    } else {
      lo = indexLo;
      hi = indexHi;
    }
    entries.push_back({name, kind, std::move(type), lo, hi});
    return name;
  }

  std::string SymCounter::nextCoef(const TypePtr &type) { return next(SymKind::Coef, type); }

  std::string SymCounter::nextValue() { return next(SymKind::Value, makeI32()); }

  std::string SymCounter::nextIndex() { return next(SymKind::Index, makeI32()); }

  int SymCounter::countOfKind(SymKind kind) const {
    int c = 0;
    for (const auto &e: entries)
      if (e.kind == kind)
        c++;
    return c;
  }

  std::vector<std::string> SymCounter::namesOfKindSince(SymKind kind, int since) const {
    std::vector<std::string> result;
    int c = 0;
    for (const auto &e: entries) {
      if (e.kind == kind) {
        if (c >= since)
          result.push_back(e.name);
        c++;
      }
    }
    return result;
  }

  std::vector<SymDecl> SymCounter::makeDecls() const {
    std::vector<SymDecl> decls;
    for (const auto &e: entries) {
      SymDecl d;
      d.name = SymId{e.name, {}};
      d.kind = e.kind;
      d.type = e.type;
      d.domain = Domain{DomainInterval{e.lo, e.hi, {}}};
      decls.push_back(std::move(d));
    }
    return decls;
  }

  // ---------------------------------------------------------------------------
  // Interest coef requires
  // ---------------------------------------------------------------------------

  std::vector<Instr> interestCoefRequires(const SymCounter &sym, int coefCountBefore) {
    auto newCoefs = sym.namesOfKindSince(SymKind::Coef, coefCountBefore);
    std::vector<Instr> instrs;
    for (const auto &c: newCoefs) {
      // require c != 0
      {
        RequireInstr req;
        req.cond.lhs = simpleExpr(coefAtom(symCoef(c)));
        req.cond.op = RelOp::NE;
        req.cond.rhs = simpleExpr(coefAtom(intCoef(0)));
        req.message = "coef nonzero";
        instrs.push_back(Instr{std::move(req)});
      }
      // require (c - 1) != 0  i.e. c != 1
      {
        RequireInstr req;
        Expr lhs = simpleExpr(coefAtom(symCoef(c)));
        lhs.rest.push_back({AddOp::Minus, coefAtom(intCoef(1)), {}});
        req.cond.lhs = std::move(lhs);
        req.cond.op = RelOp::NE;
        req.cond.rhs = simpleExpr(coefAtom(intCoef(0)));
        req.message = "coef not 1";
        instrs.push_back(Instr{std::move(req)});
      }
      // require (c + 1) != 0  i.e. c != -1
      {
        RequireInstr req;
        Expr lhs = simpleExpr(coefAtom(symCoef(c)));
        lhs.rest.push_back({AddOp::Plus, coefAtom(intCoef(1)), {}});
        req.cond.lhs = std::move(lhs);
        req.cond.op = RelOp::NE;
        req.cond.rhs = simpleExpr(coefAtom(intCoef(0)));
        req.message = "coef not -1";
        instrs.push_back(Instr{std::move(req)});
      }
    }
    return instrs;
  }

  // ---------------------------------------------------------------------------
  // Core expression builders
  // ---------------------------------------------------------------------------

  // Generate a random concrete integer literal in [lo, hi] of the given bitwidth.
  // The type checker validates literal range, so we keep values small.
  static Atom genConcreteIntAtom(std::mt19937 &rng, const TypePtr &targetType) {
    uint32_t bits = intBitWidth(targetType);
    int64_t lo = hp::kConcreteInt_Default_Lo, hi = hp::kConcreteInt_Default_Hi;
    if (bits == 8) {
      lo = hp::kConcreteInt_I8_Lo;
      hi = hp::kConcreteInt_I8_Hi;
    } else if (bits == 16) {
      lo = hp::kConcreteInt_I16_Lo;
      hi = hp::kConcreteInt_I16_Hi;
    } else if (bits == 32) {
      lo = hp::kConcreteInt_I32_Lo;
      hi = hp::kConcreteInt_I32_Hi;
    } else if (bits == 64) {
      lo = hp::kConcreteInt_I64_Lo;
      hi = hp::kConcreteInt_I64_Hi;
    }
    std::uniform_int_distribution<int64_t> d(lo, hi);
    return coefAtom(intCoef(d(rng)));
  }

  // Generate a concrete float atom
  static Atom genConcreteFloatAtom(std::mt19937 &rng) {
    std::uniform_int_distribution<std::size_t> d(0, hp::kFloatLitPoolSize - 1);
    return coefAtom(floatCoef(hp::kFloatLitPool[d(rng)]));
  }

  // Build a SelectVal of targetType: either a same-typed scalar local or a
  // literal/sym Coef. For off-path (sym == nullptr), only locals and literals.
  static SelectVal pickSelectVal(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType
  ) {
    auto scalars = vars.scalarsOf(targetType);
    std::uniform_int_distribution<int> kind(0, 2);
    int k = kind(rng);
    if (k == 0 && !scalars.empty()) {
      auto *v = pickOne(rng, scalars);
      return SelectVal{LValue{LocalId{v->name, {}}, {}, {}}};
    }
    if (k == 1 && sym != nullptr) {
      return SelectVal{symCoef(sym->nextValue())};
    }
    // Literal of the right kind
    if (isFpType(targetType))
      return SelectVal{floatCoef(1.0)};
    return SelectVal{intCoef(1)};
  }

  // Generate a SelectAtom of the given target type. Cond compares a scalar
  // local to a literal (defensive: same-type) so the typechecker is happy.
  static Atom genSelectAtom(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType
  ) {
    Cond cond;
    auto allScalars = vars.allScalars();
    if (!allScalars.empty()) {
      auto *vL = pickOne(rng, allScalars);
      cond.lhs = simpleExpr(rvalAtom(localLV(vL->name)));
      // RHS: a 0-literal of the same int width, or 0.0 for fp — keeps types compatible.
      if (isFpType(vL->type))
        cond.rhs = simpleExpr(coefAtom(floatCoef(0.0)));
      else
        cond.rhs = simpleExpr(coefAtom(intCoef(0)));
    } else {
      cond.lhs = simpleExpr(coefAtom(intCoef(0)));
      cond.rhs = simpleExpr(coefAtom(intCoef(0)));
    }
    static const RelOp relops[] = {RelOp::EQ, RelOp::NE, RelOp::LT,
                                   RelOp::LE, RelOp::GT, RelOp::GE};
    std::uniform_int_distribution<int> ro(0, 5);
    cond.op = relops[ro(rng)];

    SelectAtom sa;
    sa.cond = std::make_unique<Cond>(std::move(cond));
    sa.vtrue = pickSelectVal(rng, sym, vars, targetType);
    sa.vfalse = pickSelectVal(rng, sym, vars, targetType);
    return Atom{std::move(sa), {}};
  }

  // Generate a single Atom of the given integer type (on-path, uses sym)
  static Atom genIntAtomOnPath(
      std::mt19937 &rng, SymCounter &sym, const VarCatalogue &vars, const TypePtr &targetType,
      const ExprGenConfig &cfg, std::vector<Instr> &extraRequires
  ) {
    // Collect scalars of the target type for use as RValues
    auto scalarsOfT = vars.scalarsOf(targetType);
    bool hasRval = !scalarsOfT.empty();

    // Also collect scalars of ANY int type for casts
    auto allScalars = vars.allScalars();
    std::vector<const VarEntry *> otherIntScalars;
    for (auto *v: allScalars)
      if (isIntType(v->type) && !typeEquals(v->type, targetType))
        otherIntScalars.push_back(v);

    // Probability distribution over atom kinds
    // We use a simple slot-based approach
    // Slots:
    //  0-39: CoefAtom{sym}  (standalone symbolic coef)
    //  40-59: OpAtom{Mul, sym, rval}  (requires rval)
    //  60-69: bitwise ops (if enableAllOps, requires rval)
    //  70-74: shift (if enableAllOps, requires rval)
    //  75-79: unary not (requires rval)
    //  80-84: cast from other int width (requires otherIntScalars)
    //  85-89: div/mod (if enableDiv, requires rval)
    //  90-99: CoefAtom{sym} fallback

    std::uniform_int_distribution<int> slot(0, 99);
    int s = slot(rng);

    auto pickRval = [&]() -> RValue {
      assert(hasRval);
      auto *v = pickOne(rng, scalarsOfT);
      return localLV(v->name);
    };

    if (s < hp::kIntOnPath_CoefBareEnd) {
      // Standalone coef sym
      return coefAtom(symCoef(sym.nextCoef(targetType)));
    }
    if (s < hp::kIntOnPath_MulEnd && hasRval) {
      // Linear: sym * rval
      return opAtom(AtomOpKind::Mul, symCoef(sym.nextCoef(targetType)), pickRval());
    }
    if (s < hp::kIntOnPath_BitwiseEnd && cfg.enableAllOps && hasRval) {
      // Bitwise
      static const AtomOpKind bops[] = {AtomOpKind::And, AtomOpKind::Or, AtomOpKind::Xor};
      std::uniform_int_distribution<int> opPick(0, 2);
      return opAtom(bops[opPick(rng)], symCoef(sym.nextCoef(targetType)), pickRval());
    }
    if (s < hp::kIntOnPath_ShiftEnd && cfg.enableAllOps && hasRval) {
      // Shift: index_sym << rval  (coef=index sym, rval=variable being shifted)
      // Per the existing pattern: index sym is the VALUE being shifted, rval is shift amount
      // But shift amount must be i32 — we need an i32 rval for the shift amount
      // Actually in SymIR: OpAtom{Shl, coef, rval} = coef SHL rval
      // coef is the value to shift, rval is the shift amount
      // For proper typing: coef must match targetType, rval (shift amount) must be i32
      // So we need an i32 var as rval for shift amount, and use an index sym as the coef
      // The index sym type must also be targetType for type consistency
      auto i32scalars = vars.scalarsOf(makeI32());
      if (!i32scalars.empty() || isIntType(targetType)) {
        // Use index sym of targetType as the value being shifted
        // use an i32 var as shift amount if available, else use an int literal
        static const AtomOpKind sops[] = {AtomOpKind::Shl, AtomOpKind::Shr, AtomOpKind::LShr};
        std::uniform_int_distribution<int> opPick(0, 2);
        auto idxSym = sym.nextIndex(); // always i32
        // The shift coef should be the same type as targetType for type correctness
        // but nextIndex always returns i32. For non-i32 targets, we use a coef sym instead
        if (intBitWidth(targetType) == 32) {
          if (!i32scalars.empty()) {
            auto *shiftAmt = pickOne(rng, i32scalars);
            return opAtom(sops[opPick(rng)], symCoef(idxSym), localLV(shiftAmt->name));
          }
        } else {
          // Non-i32 target: use coef sym of targetType << i32 literal for shift amount
          // But OpAtom{Shl, coef, rval}: coef must be targetType, rval must be...
          // In SymIR, shift amount is the rval. For i64 << i32, the rval should be i32.
          // Use a concrete integer rval instead:
          (void) idxSym; // index sym was consumed, drop it
          // Just fall through to coef standalone
          return coefAtom(symCoef(sym.nextCoef(targetType)));
        }
      }
      return coefAtom(symCoef(sym.nextCoef(targetType)));
    }
    if (s < hp::kIntOnPath_UnaryNotEnd && hasRval) {
      // Unary NOT
      return unaryAtom(pickRval());
    }
    if (s < hp::kIntOnPath_CastEnd && !otherIntScalars.empty()) {
      // CastAtom from another int width
      auto *srcVar = pickOne(rng, otherIntScalars);
      CastAtom ca;
      ca.src = LValue{LocalId{srcVar->name, {}}, {}, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    if (s < hp::kIntOnPath_DivModEnd && cfg.enableDiv && hasRval) {
      // Div/Mod: OpAtom{Div/Mod, sym_coef, rval}  = sym / rval
      // We need a require(rval != 0) guard on-path
      std::uniform_int_distribution<int> dm(0, 1);
      AtomOpKind op = dm(rng) ? AtomOpKind::Mod : AtomOpKind::Div;
      auto *rv = pickOne(rng, scalarsOfT);
      std::string symName = sym.nextCoef(targetType);
      // Add require: rval != 0
      RequireInstr req;
      req.cond.lhs = simpleExpr(rvalAtom(localLV(rv->name)));
      req.cond.op = RelOp::NE;
      req.cond.rhs = simpleExpr(coefAtom(intCoef(0)));
      req.message = "div nonzero";
      extraRequires.push_back(Instr{std::move(req)});
      return opAtom(op, symCoef(symName), localLV(rv->name));
    }
    if (s < hp::kIntOnPath_LoadEnd) {
      // Load from a ptr T var if any exist
      auto ptrs = vars.ptrsOf(targetType);
      if (!ptrs.empty()) {
        auto *pv = pickOne(rng, ptrs);
        return Atom{LoadAtom{localLV(pv->name), {}}, {}};
      }
    }
    if (s < hp::kIntOnPath_SelectEnd && cfg.enableSelect) {
      return genSelectAtom(rng, &sym, vars, targetType);
    }
    // Fallback: standalone sym
    return coefAtom(symCoef(sym.nextCoef(targetType)));
  }

  // Generate a single Atom of the given integer type (off-path, concrete only)
  static Atom genIntAtomOffPath(
      std::mt19937 &rng, const VarCatalogue &vars, const TypePtr &targetType,
      const ExprGenConfig &cfg
  ) {
    auto scalarsOfT = vars.scalarsOf(targetType);
    bool hasRval = !scalarsOfT.empty();

    auto allScalars = vars.allScalars();
    std::vector<const VarEntry *> otherIntScalars;
    for (auto *v: allScalars)
      if (isIntType(v->type) && !typeEquals(v->type, targetType))
        otherIntScalars.push_back(v);

    std::uniform_int_distribution<int> slot(0, 99);
    int s = slot(rng);

    if (s < hp::kIntOffPath_ConcreteEnd || !hasRval) {
      return genConcreteIntAtom(rng, targetType);
    }
    if (s < hp::kIntOffPath_MulEnd) {
      auto *v = pickOne(rng, scalarsOfT);
      uint32_t bits = intBitWidth(targetType);
      int64_t lo = hp::kOffPathCoef_Lo, hi = hp::kOffPathCoef_Hi;
      if (bits == 8) {
        lo = hp::kOffPathCoefI8_Lo;
        hi = hp::kOffPathCoefI8_Hi;
      }
      std::uniform_int_distribution<int64_t> cd(lo, hi);
      return opAtom(AtomOpKind::Mul, intCoef(cd(rng)), localLV(v->name));
    }
    if (s < hp::kIntOffPath_BitwiseEnd && cfg.enableAllOps) {
      auto *v = pickOne(rng, scalarsOfT);
      static const AtomOpKind bops[] = {AtomOpKind::And, AtomOpKind::Or, AtomOpKind::Xor};
      std::uniform_int_distribution<int> op(0, 2);
      std::uniform_int_distribution<int64_t> cv(hp::kOffPathCoef_Lo, hp::kOffPathCoef_Hi);
      return opAtom(bops[op(rng)], intCoef(cv(rng)), localLV(v->name));
    }
    if (s < hp::kIntOffPath_CastEnd && !otherIntScalars.empty()) {
      auto *v = pickOne(rng, otherIntScalars);
      CastAtom ca;
      ca.src = LValue{LocalId{v->name, {}}, {}, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    if (s < hp::kIntOffPath_DivModEnd && cfg.enableDiv && hasRval) {
      auto *v = pickOne(rng, scalarsOfT);
      std::uniform_int_distribution<int> dm(0, 1);
      AtomOpKind op = dm(rng) ? AtomOpKind::Mod : AtomOpKind::Div;
      std::uniform_int_distribution<int64_t> cd(hp::kOffPathDivisor_Lo, hp::kOffPathDivisor_Hi);
      return opAtom(op, intCoef(cd(rng)), localLV(v->name));
    }
    if (s < hp::kIntOffPath_PlainRvalEnd) {
      auto *v = pickOne(rng, scalarsOfT);
      return rvalAtom(localLV(v->name));
    }
    if (s < hp::kIntOffPath_LoadEnd) {
      // Load from a ptr T var if available
      auto ptrs = vars.ptrsOf(targetType);
      if (!ptrs.empty()) {
        auto *pv = pickOne(rng, ptrs);
        return Atom{LoadAtom{localLV(pv->name), {}}, {}};
      }
    }
    if (s < hp::kIntOffPath_SelectEnd && cfg.enableSelect) {
      return genSelectAtom(rng, /*sym=*/nullptr, vars, targetType);
    }
    return genConcreteIntAtom(rng, targetType);
  }

  // Generate a float atom (on-path: cast from i32 sym or concrete float)
  static Atom genFloatAtomOnPath(
      std::mt19937 &rng, SymCounter &sym, const VarCatalogue &vars, const TypePtr &targetType,
      const ExprGenConfig &cfg
  ) {
    auto fpVars = vars.scalarsOf(targetType);
    auto i32scalars = vars.scalarsOf(makeI32());

    std::uniform_int_distribution<int> slot(0, 99);
    int s = slot(rng);

    if (s < hp::kFloatOnPath_CastFromI32SymEnd) {
      // Cast from i32 sym to float (keeps SMT in BV theory)
      auto symName = sym.nextValue();
      CastAtom ca;
      ca.src = SymId{symName, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    if (s < hp::kFloatOnPath_MulLitEnd && !fpVars.empty()) {
      // Multiply by concrete float literal
      auto *v = pickOne(rng, fpVars);
      std::uniform_int_distribution<std::size_t> ld(0, hp::kFloatMulCoefPoolSize - 1);
      return opAtom(AtomOpKind::Mul, floatCoef(hp::kFloatMulCoefPool[ld(rng)]), localLV(v->name));
    }
    if (s < hp::kFloatOnPath_CastFromVarEnd && !i32scalars.empty()) {
      // CastAtom from i32 var
      auto *v = pickOne(rng, i32scalars);
      CastAtom ca;
      ca.src = LValue{LocalId{v->name, {}}, {}, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    if (s < hp::kFloatOnPath_SelectEnd && cfg.enableSelect) {
      return genSelectAtom(rng, &sym, vars, targetType);
    }
    // Concrete float literal
    return genConcreteFloatAtom(rng);
  }

  // Generate a float atom (off-path: concrete literals only)
  static Atom genFloatAtomOffPath(std::mt19937 &rng) { return genConcreteFloatAtom(rng); }

  // Generate a ptr-type atom. Collects all candidate atoms uniformly so each
  // option (addr, copy, load-from-ptr-ptr) appears with equal probability.
  static Atom genPtrAtom(std::mt19937 &rng, const VarCatalogue &vars, const TypePtr &targetType) {
    assert(isPtrType(targetType));
    TypePtr ptee = pointeeType(targetType);

    std::vector<Atom> options;

    // addr of any addressable var whose type equals the pointee
    for (auto *v: vars.allAddressable()) {
      if (typeEquals(v->type, ptee)) {
        options.push_back(Atom{AddrAtom{localLV(v->name), {}}, {}});
      }
    }

    // copy from an existing ptr T var (same type)
    for (auto *pv: vars.ptrsOf(ptee)) {
      options.push_back(rvalAtom(localLV(pv->name)));
    }

    // load from a ptr ptr T var to materialise a ptr T value
    for (auto *ppv: vars.ptrsOf(targetType)) {
      options.push_back(Atom{LoadAtom{localLV(ppv->name), {}}, {}});
    }

    if (!options.empty()) {
      std::uniform_int_distribution<int> d(0, (int) options.size() - 1);
      return std::move(options[d(rng)]);
    }
    return coefAtom(NullLit{{}});
  }

  // ---------------------------------------------------------------------------
  // Main genExpr
  // ---------------------------------------------------------------------------

  Expr genExpr(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      bool onPath, const ExprGenConfig &cfg
  ) {
    // Build a 1-3 atom expression (first + rest).
    // Note: div/mod safety requires are lost in this public API;
    // use genBlockStmts which calls genExprWithRequires internally.
    std::vector<Instr> dummyReqs;
    std::vector<Atom> atoms;

    // Determine number of atoms in this expression
    std::uniform_int_distribution<int> nAtomsDist(hp::kMinAtomsPerExpr, hp::kMaxAtomsPerExpr);
    int nAtoms = nAtomsDist(rng);

    // For ptr types, always single atom
    if (isPtrType(targetType)) {
      nAtoms = 1;
    }
    // For float types, limit to 1-2 atoms
    if (isFpType(targetType) && nAtoms > 2) {
      nAtoms = 2;
    }
    // [v0.2.1] Vec types: single atom for now (whole-vec copy or broadcast).
    if (isVecType(targetType)) {
      nAtoms = 1;
    }

    for (int i = 0; i < nAtoms; i++) {
      Atom a;
      if (isIntType(targetType)) {
        if (onPath && sym) {
          a = genIntAtomOnPath(rng, *sym, vars, targetType, cfg, dummyReqs);
        } else {
          a = genIntAtomOffPath(rng, vars, targetType, cfg);
        }
      } else if (isFpType(targetType)) {
        if (onPath && sym) {
          a = genFloatAtomOnPath(rng, *sym, vars, targetType, cfg);
        } else {
          a = genFloatAtomOffPath(rng);
        }
      } else if (isPtrType(targetType)) {
        a = genPtrAtom(rng, vars, targetType);
      } else if (isVecType(targetType)) {
        // [v0.2.1] Vec expression: pick a same-typed vec var as RValueAtom
        // (whole-vec copy). If none available, fall back to a broadcast
        // literal (the typechecker accepts scalar init for vec targets).
        auto vecs = vars.vecsOf(targetType);
        if (!vecs.empty()) {
          std::uniform_int_distribution<int> vd(0, (int) vecs.size() - 1);
          auto *v = vecs[vd(rng)];
          RValueAtom ra;
          ra.rval = LValue{LocalId{v->name, {}}, {}, {}};
          a = Atom{std::move(ra), {}};
        } else {
          // Broadcast: use 0 literal (valid for any vec type).
          CoefAtom ca;
          ca.coef = IntLit{0, {}};
          a = Atom{std::move(ca), {}};
        }
      } else {
        // Unknown type (e.g. struct) — fallback to i32 concrete atom
        a = genConcreteIntAtom(rng, makeI32());
      }
      atoms.push_back(std::move(a));
    }

    // Build Expr from atoms
    Expr expr;
    expr.first = std::move(atoms[0]);
    std::uniform_int_distribution<int> coin(0, 1);
    for (std::size_t i = 1; i < atoms.size(); i++) {
      // For float, always use Plus (subtraction is fine but let's keep it simple)
      AddOp op = isFpType(targetType) ? AddOp::Plus : (coin(rng) ? AddOp::Plus : AddOp::Minus);
      expr.rest.push_back({op, std::move(atoms[i]), {}});
    }

    (void) dummyReqs; // div/mod requires are handled by genExprWithRequires / genBlockStmts
    return expr;
  }

  // ---------------------------------------------------------------------------
  // genExprWithRequires — internal, returns both expr and any safety requires
  // ---------------------------------------------------------------------------

  // Generate the first atom of an off-path integer expression.
  // MUST produce an atom whose type is unambiguously targetType (not just
  // inferred from context). This is because the TypeChecker uses the first
  // atom's type to set the expected type for tail atoms. If the first atom
  // is a bare IntLit with no context, it defaults to i32 which may mismatch
  // the tail atoms that reference actual vars of targetType.
  static Atom genFirstIntAtomOffPath(
      std::mt19937 &rng, const VarCatalogue &vars, const TypePtr &targetType,
      [[maybe_unused]] const ExprGenConfig &cfg
  ) {
    auto scalarsOfT = vars.scalarsOf(targetType);
    auto allScalars = vars.allScalars();
    std::vector<const VarEntry *> otherIntScalars;
    for (auto *v: allScalars)
      if (isIntType(v->type) && !typeEquals(v->type, targetType))
        otherIntScalars.push_back(v);

    if (!scalarsOfT.empty()) {
      // RValueAtom: type is determined by the variable's declared type
      auto *v = pickOne(rng, scalarsOfT);
      return rvalAtom(localLV(v->name));
    }
    if (!otherIntScalars.empty()) {
      // CastAtom: explicitly typed by dstType
      auto *v = pickOne(rng, otherIntScalars);
      CastAtom ca;
      ca.src = LValue{LocalId{v->name, {}}, {}, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    // No variables at all — bare literal (OK for single-atom expressions
    // where expectedBits comes from the assignment context)
    return genConcreteIntAtom(rng, targetType);
  }

  // Generate the first atom of an on-path integer expression.
  // For non-i32 types, must produce an atom whose type is unambiguously
  // targetType so the printed concrete program (after symbol substitution)
  // still type-checks. A bare CoefAtom{SymId} prints as a bare integer after
  // substitution, which the TypeChecker defaults to i32 — causing a bitwidth
  // mismatch when targetType is wider/narrower. We use CastAtom{SymId, dst}
  // which prints as "<value> as <type>" and preserves the type annotation.
  static Atom genFirstIntAtomOnPath(
      std::mt19937 &rng, SymCounter &sym, const VarCatalogue &vars, const TypePtr &targetType,
      const ExprGenConfig &cfg, std::vector<Instr> &extraRequires
  ) {
    // For i32, a bare CoefAtom is fine (i32 is the default inferred type).
    if (intBitWidth(targetType) == 32) {
      return genIntAtomOnPath(rng, sym, vars, targetType, cfg, extraRequires);
    }
    // Non-i32: prefer RValueAtom (variable of targetType) since its type is
    // inferred from the declaration. Failing that, wrap a sym coef in a CastAtom.
    auto scalarsOfT = vars.scalarsOf(targetType);
    if (!scalarsOfT.empty()) {
      // 50% chance of RValueAtom vs CastAtom{sym}
      std::uniform_int_distribution<int> coin(0, 1);
      if (coin(rng)) {
        auto *v = pickOne(rng, scalarsOfT);
        return rvalAtom(localLV(v->name));
      }
    }
    // CastAtom{SymId, targetType}: prints as "<solved_value> as <type>"
    CastAtom ca;
    ca.src = SymId{sym.nextCoef(targetType), {}};
    ca.dstType = targetType;
    return Atom{std::move(ca), {}};
  }

  static std::pair<Expr, std::vector<Instr>> genExprWithRequires(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      bool onPath, const ExprGenConfig &cfg
  ) {
    std::vector<Instr> reqs;
    std::vector<Atom> atoms;

    std::uniform_int_distribution<int> nAtomsDist(hp::kMinAtomsPerExpr, hp::kMaxAtomsPerExpr);
    int nAtoms = nAtomsDist(rng);

    if (isPtrType(targetType))
      nAtoms = 1;
    if (isFpType(targetType) && nAtoms > 2)
      nAtoms = 2;

    for (int i = 0; i < nAtoms; i++) {
      Atom a;
      if (isIntType(targetType)) {
        if (onPath && sym) {
          // For i==0, use genFirstIntAtomOnPath to ensure non-i32 types get an
          // explicitly-typed first atom. A bare CoefAtom{SymId} would print as a
          // bare integer literal after model substitution, defaulting to i32.
          if (i == 0) {
            a = genFirstIntAtomOnPath(rng, *sym, vars, targetType, cfg, reqs);
          } else {
            a = genIntAtomOnPath(rng, *sym, vars, targetType, cfg, reqs);
          }
        } else {
          // The first atom of an off-path expression must be explicitly typed.
          // The TypeChecker evaluates the condition LHS with expectedBits=nullopt,
          // meaning a bare IntLit first atom defaults to i32 regardless of condType.
          // Using genFirstIntAtomOffPath for i==0 ensures the first atom is always
          // a RValueAtom or CastAtom whose type is unambiguous.
          if (i == 0) {
            a = genFirstIntAtomOffPath(rng, vars, targetType, cfg);
          } else {
            a = genIntAtomOffPath(rng, vars, targetType, cfg);
          }
        }
      } else if (isFpType(targetType)) {
        if (onPath && sym) {
          a = genFloatAtomOnPath(rng, *sym, vars, targetType, cfg);
        } else {
          a = genFloatAtomOffPath(rng);
        }
      } else if (isPtrType(targetType)) {
        a = genPtrAtom(rng, vars, targetType);
      } else {
        // Struct type — generate i32 concrete (struct assignments handled specially)
        a = genConcreteIntAtom(rng, makeI32());
      }
      atoms.push_back(std::move(a));
    }

    Expr expr;
    expr.first = std::move(atoms[0]);
    std::uniform_int_distribution<int> coin(0, 1);
    for (std::size_t i = 1; i < atoms.size(); i++) {
      AddOp op = isFpType(targetType) ? AddOp::Plus : (coin(rng) ? AddOp::Plus : AddOp::Minus);
      expr.rest.push_back({op, std::move(atoms[i]), {}});
    }

    return {std::move(expr), std::move(reqs)};
  }

  // ---------------------------------------------------------------------------
  // genCond
  // ---------------------------------------------------------------------------

  Cond genCond(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, bool onPath,
      const ExprGenConfig &cfg
  ) {
    // Pick a random integer scalar type from available vars for the condition
    auto scalars = vars.allScalars();
    TypePtr condType = makeI32(); // default
    if (!scalars.empty()) {
      // Filter to int types only (can't compare floats with relational ops in SymIR)
      std::vector<const VarEntry *> intScalars;
      for (auto *v: scalars)
        if (isIntType(v->type))
          intScalars.push_back(v);
      if (!intScalars.empty()) {
        auto *v = pickOne(rng, intScalars);
        condType = v->type;
      }
    }

    std::vector<Instr> lhsReqs, rhsReqs;
    auto [lhs, lreqs] = genExprWithRequires(rng, sym, vars, condType, onPath, cfg);
    auto [rhs, rreqs] = genExprWithRequires(rng, sym, vars, condType, onPath, cfg);
    // Note: condition requires are discarded here (condition can't have inline requires easily)
    // This is acceptable since the requires from div are an optional safety feature
    (void) lreqs;
    (void) rreqs;

    static const RelOp relOps[] = {RelOp::LT, RelOp::GT, RelOp::LE,
                                   RelOp::GE, RelOp::EQ, RelOp::NE};
    std::uniform_int_distribution<int> opPick(0, 5);

    return Cond{std::move(lhs), relOps[opPick(rng)], std::move(rhs), {}};
  }

  // ---------------------------------------------------------------------------
  // genBlockStmts
  // ---------------------------------------------------------------------------

  std::vector<Instr> genBlockStmts(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, int nStmts, bool onPath,
      bool safeOffPath, const ExprGenConfig &cfg
  ) {
    std::vector<Instr> result;
    std::uniform_real_distribution<double> prob(0.0, 1.0);

    // Collect mutable (non-ptr) vars for assignment targets
    // We assign to scalars and array/struct elements
    std::vector<const VarEntry *> allVars;
    for (const auto &v: vars.vars)
      allVars.push_back(&v);

    // Collect ptr vars for store operations
    std::vector<const VarEntry *> ptrVars;
    for (const auto &v: vars.vars)
      if (isPtrType(v.type))
        ptrVars.push_back(&v);

    for (int s = 0; s < nStmts; s++) {
      // 80% assignment, 20% store (if ptr vars exist)
      double r = prob(rng);
      bool doStore = (r > 0.80) && !ptrVars.empty();

      if (doStore) {
        // StoreInstr: pick a ptr T var, generate Expr of type T
        auto *pv = pickOne(rng, ptrVars);
        TypePtr ptee = pointeeType(pv->type);

        // ptr expr: RValueAtom of the ptr var
        Expr ptrExpr = simpleExpr(rvalAtom(localLV(pv->name)));

        if (isIntType(ptee) || isFpType(ptee)) {
          auto [valExpr, reqs] = genExprWithRequires(rng, sym, vars, ptee, onPath, cfg);
          // Insert safety requires before store if on-path
          if (onPath || safeOffPath) {
            for (auto &req: reqs)
              result.push_back(std::move(req));
          }
          StoreInstr st;
          st.ptr = std::move(ptrExpr);
          st.val = std::move(valExpr);
          result.push_back(Instr{std::move(st)});
        }
        // Skip stores if pointee is agg (not supported)
        continue;
      }

      // Assignment: pick a random mutable target
      if (allVars.empty())
        continue;
      auto *lhsVar = pickOne(rng, allVars);

      // Build LHS based on var type
      LValue lhs;
      TypePtr assignType;

      if (isScalarType(lhsVar->type)) {
        lhs = localLV(lhsVar->name);
        assignType = lhsVar->type;
      } else if (std::holds_alternative<ArrayType>(lhsVar->type->v)) {
        const auto &at = std::get<ArrayType>(lhsVar->type->v);
        std::uniform_int_distribution<int64_t> idxd(0, (int64_t) at.size - 1);
        int64_t idx = idxd(rng);
        if (isScalarType(at.elem)) {
          lhs = arrayLV(lhsVar->name, idx);
          assignType = at.elem;
        } else if (std::holds_alternative<StructType>(at.elem->v)) {
          // Array-of-struct: pick a scalar field, generate %a[i].f = expr
          const std::string &ename = std::get<StructType>(at.elem->v).name.name;
          const StructDecl *sd = nullptr;
          for (const auto &decl: vars.structDecls)
            if (decl.name.name == ename) {
              sd = &decl;
              break;
            }
          if (!sd || sd->fields.empty())
            continue;
          std::vector<const FieldDecl *> scalarFields;
          for (const auto &f: sd->fields)
            if (isScalarType(f.type))
              scalarFields.push_back(&f);
          if (scalarFields.empty())
            continue;
          const FieldDecl *f = pickOne(rng, scalarFields);
          lhs = arrayLV(lhsVar->name, idx);
          lhs.accesses.push_back(AccessField{f->name, {}});
          assignType = f->type;
        } else {
          continue; // nested array or other, skip
        }
      } else if (std::holds_alternative<StructType>(lhsVar->type->v)) {
        const std::string &sname = lhsVar->structTypeName;
        const StructDecl *sd = nullptr;
        for (const auto &decl: vars.structDecls)
          if (decl.name.name == sname) {
            sd = &decl;
            break;
          }
        if (!sd || sd->fields.empty()) {
          // Fallback: scalar assignment
          auto scalars = vars.allScalars();
          if (scalars.empty())
            continue;
          auto *sv = pickOne(rng, scalars);
          lhs = localLV(sv->name);
          assignType = sv->type;
        } else {
          std::uniform_int_distribution<int> fpick(0, (int) sd->fields.size() - 1);
          const auto &f = sd->fields[fpick(rng)];
          if (isScalarType(f.type)) {
            lhs = structLV(lhsVar->name, f.name);
            assignType = f.type;
          } else if (std::holds_alternative<ArrayType>(f.type->v)) {
            // Struct-of-array field: pick an element, generate %t.f[i] = expr
            const auto &fat = std::get<ArrayType>(f.type->v);
            if (!isScalarType(fat.elem))
              continue;
            std::uniform_int_distribution<int64_t> idxd2(0, (int64_t) fat.size - 1);
            lhs = structLV(lhsVar->name, f.name);
            lhs.accesses.push_back(AccessIndex{Index{IntLit{idxd2(rng), {}}}, {}});
            assignType = fat.elem;
          } else {
            continue; // ptr field or other, skip
          }
        }
      } else if (isPtrType(lhsVar->type)) {
        // Ptr reassignment: redirect to another target
        lhs = localLV(lhsVar->name);
        Expr rhs = simpleExpr(genPtrAtom(rng, vars, lhsVar->type));
        result.push_back(Instr{AssignInstr{std::move(lhs), std::move(rhs), {}}});
        continue;
      } else {
        continue; // unknown type, skip
      }

      if (!assignType)
        continue;

      auto [rhs, reqs] = genExprWithRequires(rng, sym, vars, assignType, onPath, cfg);

      // Insert safety requires before assignment if on-path (or safeOffPath)
      if (onPath || safeOffPath) {
        for (auto &req: reqs)
          result.push_back(std::move(req));
      }

      result.push_back(Instr{AssignInstr{std::move(lhs), std::move(rhs), {}}});
    }

    return result;
  }

} // namespace symir::reify
