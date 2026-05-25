#include "reify/func_gen.hpp"

#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Internal helpers
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

  static Atom coefAtom(Coef c) { return Atom{CoefAtom{std::move(c), {}}, {}}; }

  static Atom rvalAtom(RValue rv) { return Atom{RValueAtom{std::move(rv), {}}, {}}; }

  static Expr simpleExpr(Atom a) { return Expr{std::move(a), {}, {}}; }

  // ---------------------------------------------------------------------------
  // Unified recursive initializer
  // ---------------------------------------------------------------------------

  static InitVal makeInitVal(const TypePtr &t, const std::vector<StructDecl> &structDecls) {
    if (isIntType(t)) {
      InitVal iv;
      iv.kind = InitVal::Kind::Int;
      iv.value = IntLit{1, {}};
      return iv;
    }
    if (isFpType(t)) {
      InitVal iv;
      iv.kind = InitVal::Kind::Float;
      iv.value = FloatLit{1.0, {}};
      return iv;
    }
    if (isPtrType(t)) {
      InitVal iv;
      iv.kind = InitVal::Kind::Undef;
      return iv;
    }
    if (std::holds_alternative<ArrayType>(t->v)) {
      const auto &at = std::get<ArrayType>(t->v);
      std::vector<InitValPtr> children;
      for (uint64_t i = 0; i < at.size; i++) {
        children.push_back(std::make_shared<InitVal>(makeInitVal(at.elem, structDecls)));
      }
      InitVal iv;
      iv.kind = InitVal::Kind::Aggregate;
      iv.value = std::move(children);
      return iv;
    }
    if (std::holds_alternative<StructType>(t->v)) {
      const std::string &sname = std::get<StructType>(t->v).name.name;
      const StructDecl *sd = nullptr;
      for (const auto &decl: structDecls)
        if (decl.name.name == sname) {
          sd = &decl;
          break;
        }
      if (sd) {
        std::vector<InitValPtr> children;
        for (const auto &field: sd->fields) {
          children.push_back(std::make_shared<InitVal>(makeInitVal(field.type, structDecls)));
        }
        InitVal iv;
        iv.kind = InitVal::Kind::Aggregate;
        iv.value = std::move(children);
        return iv;
      }
    }
    // [v0.2.1] VecType: brace-init with N scalar elements.
    if (std::holds_alternative<VecType>(t->v)) {
      const auto &vt = std::get<VecType>(t->v);
      std::vector<InitValPtr> children;
      for (uint64_t i = 0; i < vt.size; i++) {
        children.push_back(std::make_shared<InitVal>(makeInitVal(vt.elem, structDecls)));
      }
      InitVal iv;
      iv.kind = InitVal::Kind::Aggregate;
      iv.value = std::move(children);
      return iv;
    }
    InitVal iv;
    iv.kind = InitVal::Kind::Undef;
    return iv;
  }

  // ---------------------------------------------------------------------------
  // Checksum builder
  // ---------------------------------------------------------------------------

  static std::vector<Instr> buildChecksum(const VarCatalogue &vars) {
    std::vector<Instr> instrs;
    auto i32 = makeI32();

    // %_chk = 0;
    {
      Expr zero = simpleExpr(coefAtom(IntLit{0, {}}));
      instrs.push_back(Instr{AssignInstr{localLV("%_chk"), std::move(zero), {}}});
    }

    // Emit `%_chk = cast_atom + %_chk` (cast first) or `%_chk = %_chk + rval`.
    // CastAtom must be the FIRST atom — it cannot appear in tail position.
    auto emitChkAccum = [&](Atom valueAtom, bool isCast) {
      Expr rhs;
      if (isCast) {
        rhs.first = std::move(valueAtom);
        rhs.rest.push_back({AddOp::Plus, rvalAtom(localLV("%_chk")), {}});
      } else {
        rhs.first = rvalAtom(localLV("%_chk"));
        rhs.rest.push_back({AddOp::Plus, std::move(valueAtom), {}});
      }
      instrs.push_back(Instr{AssignInstr{localLV("%_chk"), std::move(rhs), {}}});
    };

    // Emit a scalar LValue into the checksum, casting to i32 when needed.
    auto emitScalarLV = [&](LValue lv, const TypePtr &t) {
      if (isIntType(t) && intBitWidth(t) == 32) {
        emitChkAccum(rvalAtom(std::move(lv)), /*isCast=*/false);
      } else if (isScalarType(t)) {
        CastAtom ca;
        ca.src = std::move(lv);
        ca.dstType = i32;
        emitChkAccum(Atom{std::move(ca), {}}, /*isCast=*/true);
      }
    };

    for (const auto &v: vars.vars) {
      if (isPtrType(v.type))
        continue;

      if (isScalarType(v.type)) {
        emitScalarLV(localLV(v.name), v.type);

      } else if (std::holds_alternative<ArrayType>(v.type->v)) {
        const auto &at = std::get<ArrayType>(v.type->v);
        if (isScalarType(at.elem)) {
          // Plain array: %a[i]
          for (uint64_t i = 0; i < at.size; i++) {
            emitScalarLV(arrayLV(v.name, (int64_t) i), at.elem);
          }
        } else if (std::holds_alternative<StructType>(at.elem->v)) {
          // Array-of-struct: %a[i].f for each scalar field f
          const std::string &ename = std::get<StructType>(at.elem->v).name.name;
          const StructDecl *sd = nullptr;
          for (const auto &decl: vars.structDecls)
            if (decl.name.name == ename) {
              sd = &decl;
              break;
            }
          if (!sd)
            continue;
          for (uint64_t i = 0; i < at.size; i++) {
            for (const auto &f: sd->fields) {
              if (!isScalarType(f.type))
                continue;
              LValue flv;
              flv.base = LocalId{v.name, {}};
              flv.accesses.push_back(AccessIndex{Index{IntLit{(int64_t) i, {}}}, {}});
              flv.accesses.push_back(AccessField{f.name, {}});
              emitScalarLV(std::move(flv), f.type);
            }
          }
        }

        // [v0.2.1] Vec: unroll lanes into the checksum.
      } else if (std::holds_alternative<VecType>(v.type->v)) {
        const auto &vt = std::get<VecType>(v.type->v);
        if (isScalarType(vt.elem)) {
          for (uint64_t i = 0; i < vt.size; i++) {
            LValue laneLV;
            laneLV.base = LocalId{v.name, {}};
            laneLV.accesses.push_back(AccessIndex{Index{IntLit{(int64_t) i, {}}}, {}});
            emitScalarLV(std::move(laneLV), vt.elem);
          }
        }

      } else if (std::holds_alternative<StructType>(v.type->v)) {
        const std::string &sname = v.structTypeName;
        const StructDecl *sd = nullptr;
        for (const auto &decl: vars.structDecls)
          if (decl.name.name == sname) {
            sd = &decl;
            break;
          }
        if (!sd)
          continue;
        for (const auto &f: sd->fields) {
          if (isScalarType(f.type)) {
            // Struct scalar field: %t.f
            LValue flv;
            flv.base = LocalId{v.name, {}};
            flv.accesses.push_back(AccessField{f.name, {}});
            emitScalarLV(std::move(flv), f.type);
          } else if (std::holds_alternative<ArrayType>(f.type->v)) {
            // Struct array field: %t.f[i]
            const auto &fat = std::get<ArrayType>(f.type->v);
            if (!isScalarType(fat.elem))
              continue;
            for (uint64_t i = 0; i < fat.size; i++) {
              LValue flv;
              flv.base = LocalId{v.name, {}};
              flv.accesses.push_back(AccessField{f.name, {}});
              flv.accesses.push_back(AccessIndex{Index{IntLit{(int64_t) i, {}}}, {}});
              emitScalarLV(std::move(flv), fat.elem);
            }
          }
        }
      }
    }

    return instrs;
  }

  // ---------------------------------------------------------------------------
  // genFunction
  // ---------------------------------------------------------------------------

  FuncGenResult genFunction(
      const RyCFG &cfg, const std::vector<std::string> &path, const VarCatalogue &vars,
      const FuncGenConfig &fcfg
  ) {
    std::mt19937 rng(fcfg.seed);

    SymCounter sym;
    sym.coefLo = fcfg.coefLo;
    sym.coefHi = fcfg.coefHi;
    sym.valueLo = fcfg.valueLo;
    sym.valueHi = fcfg.valueHi;
    sym.indexLo = fcfg.indexLo;
    sym.indexHi = fcfg.indexHi;

    // Generate a single input sym (i32) used for interest-init requires
    std::string inputSym = sym.nextValue();

    std::unordered_set<std::string> pathSet(path.begin(), path.end());
    auto cfgLabels = cfg.labels();

    std::vector<Block> blocks;
    blocks.reserve(cfgLabels.size());

    for (const auto &blkLabel: cfgLabels) {
      const auto *blk = cfg.get(blkLabel);
      bool onPath = pathSet.count(blkLabel) > 0;
      bool isExit = (blkLabel == cfg.exitLabel);

      Block block;
      block.label = BlockLabel{"^" + blkLabel, {}};

      int coefsBefore = sym.countOfKind(SymKind::Coef);

      if (isExit) {
        // Exit block: compute checksum and return
        auto chkInstrs = buildChecksum(vars);
        for (auto &ci: chkInstrs)
          block.instrs.push_back(std::move(ci));

        Expr retVal = simpleExpr(rvalAtom(localLV("%_chk")));
        block.term = Terminator{RetTerm{std::move(retVal), {}}};

      } else {
        // Non-exit block: generate statements

        // Special entry block setup: assign ptr vars and interest-init require
        if (blkLabel == cfg.entry) {
          // Assign all ptr vars (addr or addr-of-ptr)
          for (const auto &v: vars.vars) {
            if (!isPtrType(v.type))
              continue;
            if (!v.ptrTarget)
              continue;

            // %p = addr %target
            LValue lhs = localLV(v.name);
            Expr rhs = simpleExpr(Atom{AddrAtom{localLV(*v.ptrTarget), {}}, {}});
            block.instrs.push_back(Instr{AssignInstr{std::move(lhs), std::move(rhs), {}}});
          }

          // Interest-init: require inputSym != 0
          if (fcfg.enableInterestInits) {
            RequireInstr req;
            req.cond.lhs = simpleExpr(coefAtom(LocalOrSymId{SymId{inputSym, {}}}));
            req.cond.op = RelOp::NE;
            req.cond.rhs = simpleExpr(coefAtom(IntLit{0, {}}));
            req.message = "nonzero input";
            block.instrs.push_back(Instr{std::move(req)});
          }
        }

        // Generate statements for on-path non-exit blocks
        if (onPath) {
          auto stmts = genBlockStmts(rng, &sym, vars, fcfg.nStmts, true, false, fcfg.exprCfg);
          for (auto &s: stmts)
            block.instrs.push_back(std::move(s));
        } else {
          // Off-path: concrete-only stmts
          auto stmts =
              genBlockStmts(rng, nullptr, vars, fcfg.nStmts, false, fcfg.safeOffPath, fcfg.exprCfg);
          for (auto &s: stmts)
            block.instrs.push_back(std::move(s));
        }

        // Interest coef requires (on-path only, before terminator)
        if (fcfg.enableInterestCoefs && onPath) {
          auto reqs = interestCoefRequires(sym, coefsBefore);
          for (auto &r: reqs)
            block.instrs.push_back(std::move(r));
        }

        // Terminator
        if (blk->isBranch()) {
          Cond cond = genCond(rng, onPath ? &sym : nullptr, vars, onPath, fcfg.exprCfg);
          BrTerm br;
          br.cond = std::move(cond);
          br.thenLabel = BlockLabel{"^" + blk->succs[0], {}};
          br.elseLabel = BlockLabel{"^" + blk->succs[1], {}};
          br.dest = br.thenLabel;
          br.isConditional = true;
          block.term = Terminator{std::move(br)};
        } else if (blk->isGoto()) {
          BrTerm br;
          br.dest = BlockLabel{"^" + blk->succs[0], {}};
          br.thenLabel = br.dest;
          br.elseLabel = br.dest;
          br.isConditional = false;
          block.term = Terminator{std::move(br)};
        } else {
          // Dead-end non-exit (shouldn't happen in well-formed CFG)
          Expr zero = simpleExpr(coefAtom(IntLit{0, {}}));
          block.term = Terminator{RetTerm{std::move(zero), {}}};
        }
      }

      blocks.push_back(std::move(block));
    }

    // ---------------------------------------------------------------------------
    // Assemble Program
    // ---------------------------------------------------------------------------

    Program prog;
    prog.structs = vars.structDecls;

    FunDecl fun;
    fun.name = GlobalId{"@" + fcfg.funcName, {}};
    fun.retType = makeI32();
    fun.syms = sym.makeDecls();

    // LetDecls: one per variable + %_chk
    for (const auto &v: vars.vars) {
      LetDecl let;
      let.isMutable = true;
      let.name = LocalId{v.name, {}};
      let.type = v.type;

      if (isPtrType(v.type)) {
        // Ptr vars assigned in entry block via addr; declare as undef.
        InitVal iv;
        iv.kind = InitVal::Kind::Undef;
        let.init = std::move(iv);
      } else {
        let.init = makeInitVal(v.type, vars.structDecls);
      }

      fun.lets.push_back(std::move(let));
    }

    // %_chk: let mut %_chk: i32 = 0;
    {
      LetDecl let;
      let.isMutable = true;
      let.name = LocalId{"%_chk", {}};
      let.type = makeI32();
      InitVal iv;
      iv.kind = InitVal::Kind::Int;
      iv.value = IntLit{0, {}};
      let.init = std::move(iv);
      fun.lets.push_back(std::move(let));
    }

    fun.blocks = std::move(blocks);
    prog.funs.push_back(std::move(fun));

    // Build path labels with ^ prefix
    std::vector<std::string> pathLabels;
    pathLabels.reserve(path.size());
    for (const auto &lbl: path)
      pathLabels.push_back("^" + lbl);

    return FuncGenResult{std::move(prog), std::move(pathLabels)};
  }

} // namespace symir::reify
