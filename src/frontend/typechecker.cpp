#include "frontend/typechecker.hpp"
#include "analysis/cfg.hpp"

namespace symir {

  symir::PassResult TypeChecker::run(Program &prog, DiagBag &diags) {
    collectStructs(prog, diags);
    for (const auto &f: prog.funs) {
      TypeAnnotations ann;
      checkFunction(f, ann, diags);
    }
    return diags.hasErrors() ? symir::PassResult::Error : symir::PassResult::Success;
  }

  void TypeChecker::collectStructs(const Program &prog, DiagBag &diags) {
    for (const auto &sd: prog.structs) {
      if (structs_.count(sd.name.name)) {
        diags.error("Duplicate struct declaration: " + sd.name.name, sd.span);
        continue;
      }
      StructInfo si;
      si.declSpan = sd.span;
      for (const auto &fd: sd.fields) {
        if (si.fields.count(fd.name)) {
          diags.error("Duplicate field '" + fd.name + "' in struct " + sd.name.name, fd.span);
          continue;
        }
        si.fields[fd.name] = fd.type;
        si.fieldList.push_back({fd.name, fd.type});
      }
      structs_[sd.name.name] = std::move(si);
    }
  }

  [[maybe_unused]] static bool isArrayType(const TypePtr &t) {
    return t && std::holds_alternative<ArrayType>(t->v);
  }

  [[maybe_unused]] static bool isStructType(const TypePtr &t) {
    return t && std::holds_alternative<StructType>(t->v);
  }

  static const ArrayType *asArrayType(const TypePtr &t) {
    return t ? std::get_if<ArrayType>(&t->v) : nullptr;
  }

  static const StructType *asStructType(const TypePtr &t) {
    return t ? std::get_if<StructType>(&t->v) : nullptr;
  }

  void TypeChecker::checkInitVal(
      const InitVal &iv, const TypePtr &targetType,
      const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, DiagBag &diags
  ) {
    if (iv.kind == InitVal::Kind::Undef)
      return;

    auto at = asArrayType(targetType);
    auto st = asStructType(targetType);

    if (iv.kind == InitVal::Kind::Aggregate) {
      const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
      if (at) {
        if (elements.size() != at->size) {
          diags.error(
              "Array initializer length mismatch: expected " + std::to_string(at->size) + ", got " +
                  std::to_string(elements.size()),
              iv.span
          );
          return;
        }
        for (const auto &elem: elements) {
          checkInitVal(*elem, at->elem, vars, syms, diags);
        }
      } else if (st) {
        auto sit = structs_.find(st->name.name);
        if (sit == structs_.end())
          return;
        if (elements.size() != sit->second.fieldList.size()) {
          diags.error(
              "Struct initializer field count mismatch: expected " +
                  std::to_string(sit->second.fieldList.size()) + ", got " +
                  std::to_string(elements.size()),
              iv.span
          );
          return;
        }
        for (size_t i = 0; i < elements.size(); ++i) {
          checkInitVal(*elements[i], sit->second.fieldList[i].second, vars, syms, diags);
        }
      } else {
        diags.error("Aggregate initializer for non-aggregate type", iv.span);
      }
      return;
    }

    // Scalar Init: Int, Sym, Local
    // Broadcast check: scalar must be compatible with ALL leaf scalar elements.
    std::vector<TypePtr> targetLeaves;
    std::function<void(const TypePtr &)> collect = [&](const TypePtr &t) {
      if (auto inner_at = asArrayType(t)) {
        collect(inner_at->elem);
      } else if (auto inner_st = asStructType(t)) {
        auto sit = structs_.find(inner_st->name.name);
        if (sit != structs_.end()) {
          for (const auto &fld: sit->second.fieldList)
            collect(fld.second);
        }
      } else {
        targetLeaves.push_back(t);
      }
    };
    collect(targetType);

    TypePtr initType = nullptr;
    if (iv.kind == InitVal::Kind::Int) {
      // Literals are compatible with all integer BV types
      return;
    } else if (iv.kind == InitVal::Kind::Sym) {
      auto it = syms.find(std::get<SymId>(iv.value).name);
      if (it != syms.end())
        initType = it->second.type;
    } else if (iv.kind == InitVal::Kind::Local) {
      auto it = vars.find(std::get<LocalId>(iv.value).name);
      if (it != vars.end())
        initType = it->second.type;
    }

    if (initType) {
      for (const auto &leaf: targetLeaves) {
        if (!typeEquals(leaf, initType)) {
          diags.error("Type mismatch in initializer", iv.span);
          return;
        }
      }
    }
  }

  bool TypeChecker::typeEquals(const TypePtr &a, const TypePtr &b) {
    if (a.get() == b.get())
      return true;
    if (!a || !b)
      return false;
    if (a->v.index() != b->v.index())
      return false;
    if (auto ia = std::get_if<IntType>(&a->v)) {
      auto ib = std::get_if<IntType>(&b->v);
      if (ia->kind != ib->kind)
        return false;
      if (ia->kind == IntType::Kind::ICustom)
        return ia->bits == ib->bits;
      return true;
    }
    if (auto sa = std::get_if<StructType>(&a->v)) {
      return sa->name.name == std::get<StructType>(b->v).name.name;
    }
    if (auto aa = std::get_if<ArrayType>(&a->v)) {
      auto ab = std::get_if<ArrayType>(&b->v);
      return aa->size == ab->size && typeEquals(aa->elem, ab->elem);
    }
    return false;
  }

  std::optional<std::uint32_t> TypeChecker::getBVWidth(
      const TypePtr &t, [[maybe_unused]] DiagBag &diags, [[maybe_unused]] SourceSpan sp
  ) {
    if (!t)
      return std::nullopt;
    if (auto it = std::get_if<IntType>(&t->v)) {
      switch (it->kind) {
        case IntType::Kind::I32:
          return 32;
        case IntType::Kind::I64:
          return 64;
        case IntType::Kind::ICustom:
          return it->bits.value_or(0);
        case IntType::Kind::IntKeyword:
          return 32;
      }
    }
    return std::nullopt;
  }

  void TypeChecker::checkFunction(const FunDecl &f, TypeAnnotations &ann, DiagBag &diags) {
    std::unordered_map<std::string, VarInfo> vars;
    std::unordered_map<std::string, SymInfo> syms;

    for (const auto &p: f.params) {
      vars[p.name.name] = VarInfo{p.type, false, true, p.span};
    }
    for (const auto &s: f.syms) {
      syms[s.name.name] = SymInfo{s.type, s.kind, s.span};
    }
    for (const auto &l: f.lets) {
      if (vars.count(l.name.name)) {
        diags.error("Duplicate name: " + l.name.name, l.span);
      }
      vars[l.name.name] = VarInfo{l.type, l.isMutable, false, l.span};
      if (l.init) {
        checkInitVal(*l.init, l.type, vars, syms, diags);
      }
    }

    CFG::build(f, diags);

    auto retBits = getBVWidth(f.retType, diags, f.span);
    if (!retBits)
      diags.error("Return type must be an integer BV type in v0", f.span);

    for (const auto &b: f.blocks) {
      for (const auto &ins: b.instrs) {
        std::visit(
            [&](auto &&arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, AssignInstr>) {
                auto it = vars.find(arg.lhs.base.name);
                if (it != vars.end() && !it->second.isMutable) {
                  diags.error("Assignment to immutable local: " + arg.lhs.base.name, arg.lhs.span);
                }
                auto lt = typeOfLValue(arg.lhs, vars, syms, diags);
                if (lt) {
                  auto lb = getBVWidth(lt, diags, arg.lhs.span);
                  if (lb)
                    typeOfExpr(arg.rhs, vars, syms, ann, diags, *lb);
                }
              } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                checkCond(arg.cond, vars, syms, ann, diags);
              } else if constexpr (std::is_same_v<T, RequireInstr>) {
                checkCond(arg.cond, vars, syms, ann, diags);
              }
            },
            ins
        );
      }
      std::visit(
          [&](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, BrTerm>) {
              if (arg.isConditional && arg.cond)
                checkCond(*arg.cond, vars, syms, ann, diags);
            } else if constexpr (std::is_same_v<T, RetTerm>) {
              if (arg.value && retBits)
                typeOfExpr(*arg.value, vars, syms, ann, diags, *retBits);
              else if (!arg.value)
                diags.error("Missing return value", arg.span);
            }
          },
          b.term
      );
    }
  }

  TypePtr TypeChecker::typeOfLValue(
      const LValue &lv, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, DiagBag &diags
  ) {
    auto it = vars.find(lv.base.name);
    if (it == vars.end()) {
      diags.error("Undeclared local: " + lv.base.name, lv.base.span);
      return nullptr;
    }
    TypePtr cur = it->second.type;
    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        auto at = asArrayType(cur);
        if (!at) {
          diags.error("Indexing non-array", ai->span);
          return nullptr;
        }
        checkIndex(ai->index, vars, syms, diags);
        cur = at->elem;
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        auto st = asStructType(cur);
        if (!st) {
          diags.error("Field access on non-struct", af->span);
          return nullptr;
        }
        auto sit = structs_.find(st->name.name);
        if (sit == structs_.end()) {
          diags.error("Unknown struct type: " + st->name.name, af->span);
          return nullptr;
        }
        auto fit = sit->second.fields.find(af->field);
        if (fit == sit->second.fields.end()) {
          diags.error("Unknown field '" + af->field + "' in struct " + st->name.name, af->span);
          return nullptr;
        }
        cur = fit->second;
      }
    }
    return cur;
  }

  void TypeChecker::checkIndex(
      const Index &idx, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, DiagBag &diags
  ) {
    if (std::holds_alternative<IntLit>(idx))
      return;
    auto id = std::get<LocalOrSymId>(idx);
    if (auto lid = std::get_if<LocalId>(&id)) {
      auto it = vars.find(lid->name);
      if (it == vars.end()) {
        diags.error("Undeclared local index: " + lid->name, lid->span);
        return;
      }
      if (!getBVWidth(it->second.type, diags, lid->span))
        diags.error("Non-integer index", lid->span);
    } else {
      auto sid = std::get_if<SymId>(&id);
      auto it = syms.find(sid->name);
      if (it == syms.end()) {
        diags.error("Undeclared symbol index: " + sid->name, sid->span);
        return;
      }
      if (!getBVWidth(it->second.type, diags, sid->span))
        diags.error("Non-integer symbol index", sid->span);
    }
  }

  Ty TypeChecker::typeOfExpr(
      const Expr &e, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags,
      std::optional<std::uint32_t> expectedBits
  ) {
    auto t = typeOfAtom(e.first, vars, syms, ann, diags, expectedBits);
    for (const auto &tail: e.rest) {
      auto ti = typeOfAtom(
          tail.atom, vars, syms, ann, diags, t.isBV() ? std::optional(t.bvBits()) : expectedBits
      );
      if (t.isBV() && ti.isBV() && t.bvBits() != ti.bvBits())
        diags.error("Bitwidth mismatch", tail.span);
    }
    return t;
  }

  Ty TypeChecker::typeOfAtom(
      const Atom &a, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags,
      std::optional<std::uint32_t> expectedBits
  ) {
    return std::visit(
        [&](auto &&arg) -> Ty {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            auto rt = typeOfLValue(arg.rval, vars, syms, diags);
            auto rb = getBVWidth(rt, diags, arg.rval.span);

            // Use rb as expectation for coef if available, otherwise use incoming expectedBits
            auto targetBits = rb ? rb : expectedBits;
            auto ct = typeOfCoef(arg.coef, vars, syms, diags, targetBits);
            auto cb = getBVWidth(ct, diags, arg.span);

            if (cb && rb && *cb != *rb)
              diags.error("Bitwidth mismatch in operation", arg.span);
            return Ty{Ty::BVTy{cb.value_or(32)}};
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            checkCond(*arg.cond, vars, syms, ann, diags);
            auto t1 = typeOfSelectVal(arg.vtrue, vars, syms, ann, diags, expectedBits);
            auto t2 = typeOfSelectVal(arg.vfalse, vars, syms, ann, diags, expectedBits);
            if (t1.isBV() && t2.isBV() && t1.bvBits() != t2.bvBits())
              diags.error("Select width mismatch", arg.span);
            return t1;
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            auto ct = typeOfCoef(arg.coef, vars, syms, diags, expectedBits);
            return Ty{Ty::BVTy{getBVWidth(ct, diags, arg.span).value_or(32)}};
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            auto rt = typeOfLValue(arg.rval, vars, syms, diags);
            return Ty{Ty::BVTy{getBVWidth(rt, diags, arg.rval.span).value_or(32)}};
          }
          return Ty{std::monostate{}};
        },
        a.v
    );
  }

  TypePtr TypeChecker::typeOfCoef(
      const Coef &c, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, [[maybe_unused]] DiagBag &diags,
      std::optional<std::uint32_t> expectedBits
  ) {
    if (auto lit = std::get_if<IntLit>(&c)) {
      auto t = std::make_shared<Type>();
      t->v = IntType{
          expectedBits && *expectedBits == 64 ? IntType::Kind::I64 : IntType::Kind::I32,
          std::nullopt, lit->span
      };
      return t;
    }
    auto id = std::get<LocalOrSymId>(c);
    if (auto lid = std::get_if<LocalId>(&id)) {
      auto it = vars.find(lid->name);
      return it != vars.end() ? it->second.type : nullptr;
    } else {
      auto sid = std::get_if<SymId>(&id);
      auto it = syms.find(sid->name);
      if (it == syms.end())
        return nullptr;
      return it->second.type;
    }
  }

  Ty TypeChecker::typeOfSelectVal(
      const SelectVal &sv, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, [[maybe_unused]] TypeAnnotations &ann,
      DiagBag &diags, std::optional<std::uint32_t> expectedBits
  ) {
    if (auto rv = std::get_if<RValue>(&sv)) {
      return Ty{
          Ty::BVTy{getBVWidth(typeOfLValue(*rv, vars, syms, diags), diags, rv->span).value_or(32)}
      };
    } else {
      return Ty{Ty::BVTy{getBVWidth(
                             typeOfCoef(std::get<Coef>(sv), vars, syms, diags, expectedBits), diags,
                             SourceSpan{}
      )
                             .value_or(32)}};
    }
  }

  void TypeChecker::checkCond(
      const Cond &c, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags
  ) {
    auto t1 = typeOfExpr(c.lhs, vars, syms, ann, diags, std::nullopt);
    auto t2 = typeOfExpr(
        c.rhs, vars, syms, ann, diags, t1.isBV() ? std::optional(t1.bvBits()) : std::nullopt
    );
    if (t1.isBV() && t2.isBV() && t1.bvBits() != t2.bvBits())
      diags.error("Bitwidth mismatch in condition", c.span);
  }

} // namespace symir
