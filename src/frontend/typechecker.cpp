#include "frontend/typechecker.hpp"
#include "analysis/cfg.hpp"
#include "analysis/type_utils.hpp"

namespace symir {

  symir::PassResult TypeChecker::run(Program &prog, DiagBag &diags) {
    collectStructs(prog, diags);
    for (const auto &f: prog.funs) {
      TypeAnnotations ann;
      checkFunction(f, ann, diags);
    }
    return diags.hasErrors() ? symir::PassResult::Error : symir::PassResult::Success;
  }

  // [v0.2.1] Recursive well-formedness check on type usages. Catches the
  // structural restrictions §3.2 and §6.9.1: no vector of non-scalar, no
  // ptr to vector, no vector with N<2, no vector as struct field, no array
  // of vector.
  static void validateTypeWF(
      const TypePtr &t, DiagBag &diags, bool insideVec = false, bool insideField = false,
      bool insideArray = false
  ) {
    if (!t)
      return;
    std::visit(
        [&](auto &&x) {
          using X = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<X, VecType>) {
            if (insideVec)
              diags.error("Vectors cannot be nested (<N> <M> T)", t->span);
            if (insideField)
              diags.error("Vectors cannot appear as struct fields", t->span);
            if (insideArray)
              diags.error("Arrays of vectors are not supported", t->span);
            if (x.size < 2)
              diags.error("Vector lane count must be >= 2; got " + std::to_string(x.size), t->span);
            // The lane element must itself be scalar (Int or Float). Recurse
            // with insideVec=true so the recursion catches nested vectors;
            // PtrType / StructType / ArrayType / VecType inside a vector
            // is rejected by the recursive call (or by the explicit branches
            // below).
            if (x.elem && !std::holds_alternative<IntType>(x.elem->v) &&
                !std::holds_alternative<FloatType>(x.elem->v)) {
              diags.error("Vector lane type must be a scalar (iN, f32, f64)", t->span);
            }
            validateTypeWF(x.elem, diags, true, false, false);
          } else if constexpr (std::is_same_v<X, PtrType>) {
            // ptr <N> T is forbidden (§6.8.1).
            if (x.pointee && std::holds_alternative<VecType>(x.pointee->v))
              diags.error("Pointer to vector (ptr <N> T) is not supported", t->span);
            validateTypeWF(x.pointee, diags, false, false, false);
          } else if constexpr (std::is_same_v<X, ArrayType>) {
            validateTypeWF(x.elem, diags, false, false, true);
          }
          // StructType: validated separately in collectStructs.
        },
        t->v
    );
  }

  void TypeChecker::collectStructs(const Program &prog, DiagBag &diags) {
    for (const auto &sd: prog.structs) {
      StructInfo si;
      si.declSpan = sd.span;
      for (const auto &fd: sd.fields) {
        // [v0.2.1] Validate per-field type: in particular reject vector
        // fields explicitly with insideField=true.
        validateTypeWF(fd.type, diags, false, true, false);
        si.fields[fd.name] = fd.type;
        si.fieldList.push_back({fd.name, fd.type});
      }
      structs_[sd.name.name] = std::move(si);
    }
  }

  void TypeChecker::checkInitVal(
      const InitVal &iv, const TypePtr &targetType,
      const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, DiagBag &diags
  ) {
    if (iv.kind == InitVal::Kind::Undef)
      return;

    if (iv.kind == InitVal::Kind::Null) {
      if (!targetType || !std::holds_alternative<PtrType>(targetType->v))
        diags.error("'null' can only initialize a pointer (ptr T) variable", iv.span);
      return;
    }

    auto at = TypeUtils::asArray(targetType);
    auto st = TypeUtils::asStruct(targetType);
    auto vt = TypeUtils::asVec(targetType);

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
      } else if (vt) {
        // [v0.2.1] Vector brace init: exactly N lane initializers, each of
        // the lane scalar type. BraceElem restrictions (no atom-form inside
        // a brace) are enforced syntactically by the parser.
        if (elements.size() != vt->size) {
          diags.error(
              "Vector initializer lane count mismatch: expected " + std::to_string(vt->size) +
                  ", got " + std::to_string(elements.size()),
              iv.span
          );
          return;
        }
        for (const auto &elem: elements) {
          checkInitVal(*elem, vt->elem, vars, syms, diags);
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
      if (auto inner_at = TypeUtils::asArray(t)) {
        collect(inner_at->elem);
      } else if (auto inner_st = TypeUtils::asStruct(t)) {
        auto sit = structs_.find(inner_st->name.name);
        if (sit != structs_.end()) {
          for (const auto &fld: sit->second.fieldList)
            collect(fld.second);
        }
      } else if (auto inner_vt = TypeUtils::asVec(t)) {
        // [v0.2.1] Vector broadcast: scalar init type must match the lane
        // scalar type. Recursing on `elem` lets the existing leaf-check
        // reject `let %v: <4> i32 = 1.5;` etc.
        collect(inner_vt->elem);
      } else {
        targetLeaves.push_back(t);
      }
    };
    collect(targetType);

    TypePtr initType = nullptr;
    if (iv.kind == InitVal::Kind::Int) {
      // Literals must fit in the target type's range
      auto bits = TypeUtils::getBitWidth(targetType);
      if (bits) {
        checkLiteralRange(std::get<IntLit>(iv.value).value, *bits, iv.span, diags);
      }
      return;
    } else if (iv.kind == InitVal::Kind::Float) {
      for (const auto &leaf: targetLeaves) {
        if (!std::holds_alternative<FloatType>(leaf->v)) {
          diags.error("Cannot initialize non-float with float literal", iv.span);
          return;
        }
      }
      return;
    } else if (iv.kind == InitVal::Kind::Sym) {
      auto it = syms.find(std::get<SymId>(iv.value).name);
      if (it != syms.end())
        initType = it->second.type;
    } else if (iv.kind == InitVal::Kind::Local) {
      auto it = vars.find(std::get<LocalId>(iv.value).name);
      if (it != vars.end())
        initType = it->second.type;
      else {
        diags.error(
            "Undeclared local in initializer: " + std::get<LocalId>(iv.value).name, iv.span
        );
        return;
      }
    }

    if (initType) {
      // [v0.2.1] Whole-vector / whole-pointer init: if initType matches
      // the target type exactly, accept without descending. This covers
      // `let %w: <4> i32 = %v;` and `let %p: ptr T = %q;` where the
      // RHS is itself a value of the target's aggregate-shaped type.
      if (TypeUtils::areTypesEqual(targetType, initType))
        return;
      for (const auto &leaf: targetLeaves) {
        if (!TypeUtils::areTypesEqual(leaf, initType)) {
          diags.error("Type mismatch in initializer", iv.span);
          return;
        }
      }
    }
  }

  void TypeChecker::checkFunction(const FunDecl &f, TypeAnnotations &ann, DiagBag &diags) {
    std::unordered_map<std::string, VarInfo> vars;
    std::unordered_map<std::string, SymInfo> syms;

    // [v0.2.1] Up-front type validation so structural errors fire before
    // any expression check that would also touch the type.
    validateTypeWF(f.retType, diags);
    for (const auto &p: f.params) {
      validateTypeWF(p.type, diags);
      vars[p.name.name] = VarInfo{p.type, false, true, p.span};
    }
    for (const auto &s: f.syms) {
      // v0.2.0 §3.4.1: `sym` declarations of pointer type are not allowed.
      // Pointer values must come from `addr` or parameters.
      if (s.type && std::holds_alternative<PtrType>(s.type->v)) {
        diags.error("sym of pointer type is not allowed in v0.2.0: " + s.name.name, s.span);
      }
      validateTypeWF(s.type, diags);
      syms[s.name.name] = SymInfo{s.type, s.kind, s.span};
    }
    for (const auto &l: f.lets) {
      if (vars.count(l.name.name)) {
        diags.error("Duplicate name: " + l.name.name, l.span);
      }
      validateTypeWF(l.type, diags);
      vars[l.name.name] = VarInfo{l.type, l.isMutable, false, l.span};
      if (l.init) {
        checkInitVal(*l.init, l.type, vars, syms, diags);
      }
    }

    CFG::build(f, diags);

    auto retBits = TypeUtils::getBitWidth(f.retType);
    bool isRetFloat = f.retType && std::holds_alternative<FloatType>(f.retType->v);
    bool isRetPtr = f.retType && std::holds_alternative<PtrType>(f.retType->v);
    bool isRetVec = f.retType && std::holds_alternative<VecType>(f.retType->v); // [v0.2.1]

    if (!retBits && !isRetFloat && !isRetPtr && !isRetVec)
      diags.error("Return type must be a scalar, pointer, or vector type", f.span);

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
                  if (std::holds_alternative<PtrType>(lt->v)) {
                    // Pointer assignment: RHS must be ptr T or null
                    Ty rhsTy = typeOfExpr(arg.rhs, vars, syms, ann, diags, std::nullopt, lt);
                    if (!rhsTy.isPtr())
                      diags.error(
                          "Expected pointer expression on RHS of pointer assignment", arg.rhs.span
                      );
                    else if (!TypeUtils::areTypesEqual(rhsTy.ptrType(), lt))
                      diags.error("Pointer type mismatch in assignment", arg.rhs.span);
                  } else if (auto lvt = TypeUtils::asVec(lt)) {
                    // [v0.2.1] Vector assignment: whole-vector copy or lane
                    // write. For lane write (LHS has a trailing [i] access
                    // and i resolves to a vector lane), the LHS lt is the
                    // lane element type — which is handled by the integer/
                    // float branch below. So this branch only fires when
                    // the LHS is a whole vector lvalue. RHS must be the
                    // same vector type (literal broadcast is handled by
                    // typeOfExpr's `expected` plumbing).
                    Ty rhsTy = typeOfExpr(arg.rhs, vars, syms, ann, diags, std::nullopt, lt);
                    if (!rhsTy.isVec())
                      diags.error("Expected vector expression on RHS", arg.rhs.span);
                    else if (!TypeUtils::areTypesEqual(rhsTy.vecType(), lt))
                      diags.error("Vector type mismatch in assignment", arg.rhs.span);
                    (void) lvt;
                  } else {
                    std::optional<uint32_t> expected;
                    bool isFloat = false;
                    if (auto lb = TypeUtils::getBitWidth(lt)) {
                      expected = *lb;
                    } else if (std::holds_alternative<FloatType>(lt->v)) {
                      auto &ft = std::get<FloatType>(lt->v);
                      expected = (ft.kind == FloatType::Kind::F32) ? 32 : 64;
                      isFloat = true;
                    } else {
                      diags.error(
                          "LHS of assignment must be a scalar or pointer type", arg.lhs.span
                      );
                    }
                    if (expected) {
                      Ty rhsTy = typeOfExpr(arg.rhs, vars, syms, ann, diags, expected);
                      if (isFloat) {
                        if (!rhsTy.isFloat())
                          diags.error("Expected float expression on RHS", arg.rhs.span);
                        else if (rhsTy.floatBits() != *expected)
                          diags.error("Float width mismatch in assignment", arg.rhs.span);
                      } else {
                        if (!rhsTy.isBV())
                          diags.error("Expected integer expression on RHS", arg.rhs.span);
                        else if (rhsTy.bvBits() != *expected)
                          diags.error("Bitwidth mismatch in assignment", arg.rhs.span);
                      }
                    }
                  }
                }
              } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                checkCond(arg.cond, vars, syms, ann, diags);
              } else if constexpr (std::is_same_v<T, RequireInstr>) {
                checkCond(arg.cond, vars, syms, ann, diags);
              } else if constexpr (std::is_same_v<T, StoreInstr>) {
                // store <ptr>, <val>: ptr must be ptr T, val must be T
                Ty ptrTy = typeOfExpr(arg.ptr, vars, syms, ann, diags, std::nullopt);
                if (!ptrTy.isPtr()) {
                  diags.error("First operand of 'store' must be a pointer (ptr T)", arg.ptr.span);
                } else {
                  auto &pt = std::get<PtrType>(ptrTy.ptrType()->v);
                  TypePtr valueTy = pt.pointee;
                  if (auto vb = TypeUtils::getBitWidth(valueTy)) {
                    Ty valTy = typeOfExpr(arg.val, vars, syms, ann, diags, *vb);
                    if (!valTy.isBV() || valTy.bvBits() != *vb)
                      diags.error("'store' value type mismatch", arg.val.span);
                  } else if (valueTy && std::holds_alternative<FloatType>(valueTy->v)) {
                    auto &ft = std::get<FloatType>(valueTy->v);
                    uint32_t bits = (ft.kind == FloatType::Kind::F32) ? 32u : 64u;
                    Ty valTy = typeOfExpr(arg.val, vars, syms, ann, diags, bits);
                    if (!valTy.isFloat() || valTy.floatBits() != bits)
                      diags.error("'store' value float type mismatch", arg.val.span);
                  } else if (valueTy && std::holds_alternative<PtrType>(valueTy->v)) {
                    Ty valTy = typeOfExpr(arg.val, vars, syms, ann, diags, std::nullopt, valueTy);
                    if (!valTy.isPtr() || !TypeUtils::areTypesEqual(valTy.ptrType(), valueTy))
                      diags.error("'store' pointer value type mismatch", arg.val.span);
                  } else {
                    diags.error("'store' pointee type not supported", arg.span);
                  }
                }
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
              if (arg.value) {
                Ty rhsTy;
                if (retBits) {
                  rhsTy = typeOfExpr(*arg.value, vars, syms, ann, diags, *retBits);
                  if (!rhsTy.isBV())
                    diags.error("Expected integer return value", arg.value->span);
                  else if (rhsTy.bvBits() != *retBits)
                    diags.error("Return bitwidth mismatch", arg.value->span);
                } else if (isRetFloat) {
                  auto const &ft = std::get<FloatType>(f.retType->v);
                  uint32_t bits = (ft.kind == FloatType::Kind::F32) ? 32 : 64;
                  rhsTy = typeOfExpr(*arg.value, vars, syms, ann, diags, bits);
                  if (!rhsTy.isFloat())
                    diags.error("Expected float return value", arg.value->span);
                  else if (rhsTy.floatBits() != bits)
                    diags.error("Return float width mismatch", arg.value->span);
                } else if (isRetPtr) {
                  rhsTy = typeOfExpr(*arg.value, vars, syms, ann, diags, std::nullopt, f.retType);
                  if (!rhsTy.isPtr())
                    diags.error("Expected pointer return value", arg.value->span);
                  else if (!TypeUtils::areTypesEqual(rhsTy.ptrType(), f.retType))
                    diags.error("Return pointer type mismatch", arg.value->span);
                } else if (isRetVec) {
                  // [v0.2.1] Vector return: RHS must be the same vector type.
                  rhsTy = typeOfExpr(*arg.value, vars, syms, ann, diags, std::nullopt, f.retType);
                  if (!rhsTy.isVec())
                    diags.error("Expected vector return value", arg.value->span);
                  else if (!TypeUtils::areTypesEqual(rhsTy.vecType(), f.retType))
                    diags.error("Return vector type mismatch", arg.value->span);
                }
              } else if (!arg.value) {
                diags.error("Missing return value", arg.span);
              }
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
        if (auto vt = TypeUtils::asVec(cur)) {
          // [v0.2.1] Vector lane access: lv[i] : T_elem. Bounds-check
          // constant index now (§7.6 rule 20 statically); dynamic index
          // bounds are runtime UB.
          if (auto il = std::get_if<IntLit>(&ai->index)) {
            if (il->value < 0 || static_cast<std::uint64_t>(il->value) >= vt->size) {
              diags.error(
                  "Vector lane index " + std::to_string(il->value) + " out of bounds for <" +
                      std::to_string(vt->size) + "> vector",
                  ai->span
              );
              return nullptr;
            }
          }
          checkIndex(ai->index, vars, syms, diags);
          cur = vt->elem;
          continue;
        }
        auto at = TypeUtils::asArray(cur);
        if (!at) {
          diags.error("Indexing non-array", ai->span);
          return nullptr;
        }
        checkIndex(ai->index, vars, syms, diags);
        cur = at->elem;
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        auto st = TypeUtils::asStruct(cur);
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
      if (!TypeUtils::getBitWidth(it->second.type))
        diags.error("Non-integer index", lid->span);
    } else {
      auto sid = std::get_if<SymId>(&id);
      auto it = syms.find(sid->name);
      if (it == syms.end()) {
        diags.error("Undeclared symbol index: " + sid->name, sid->span);
        return;
      }
      if (!TypeUtils::getBitWidth(it->second.type))
        diags.error("Non-integer symbol index", sid->span);
    }
  }

  Ty TypeChecker::typeOfExpr(
      const Expr &e, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags,
      std::optional<std::uint32_t> expectedBits, TypePtr ptrCtx
  ) {
    auto t = typeOfAtom(e.first, vars, syms, ann, diags, expectedBits, ptrCtx);
    for (const auto &tail: e.rest) {
      if (t.isPtr()) {
        // Pointer arithmetic: ptr T +/- iN → ptr T,  ptr T - ptr T → i64
        auto ti = typeOfAtom(tail.atom, vars, syms, ann, diags, 64);
        if (tail.op == AddOp::Minus && ti.isPtr()) {
          // ptr - ptr → i64
          if (!TypeUtils::areTypesEqual(t.ptrType(), ti.ptrType()))
            diags.error("Pointer subtraction requires same pointee type", tail.span);
          t = Ty{Ty::BVTy{64}};
        } else if (ti.isBV()) {
          // ptr +/- int → ptr (type unchanged)
        } else {
          diags.error("Pointer arithmetic requires integer offset or same-type pointer", tail.span);
        }
      } else {
        auto ti = typeOfAtom(
            tail.atom, vars, syms, ann, diags,
            t.isBV()      ? std::optional(t.bvBits())
            : t.isFloat() ? std::optional(t.floatBits())
                          : expectedBits
        );
        // [v0.2.1] Vector chain: both atoms must agree on vector type, but
        // a scalar literal atom (`CoefAtom` of IntLit/FloatLit) broadcasts
        // to the running vector type, mirroring §6.11.
        if (t.isVec() || ti.isVec()) {
          bool tIsLitScalar =
              (t.isBV() || t.isFloat()) && std::holds_alternative<CoefAtom>(e.first.v) &&
              (std::holds_alternative<IntLit>(std::get<CoefAtom>(e.first.v).coef) ||
               std::holds_alternative<FloatLit>(std::get<CoefAtom>(e.first.v).coef));
          bool tiIsLitScalar =
              (ti.isBV() || ti.isFloat()) && std::holds_alternative<CoefAtom>(tail.atom.v) &&
              (std::holds_alternative<IntLit>(std::get<CoefAtom>(tail.atom.v).coef) ||
               std::holds_alternative<FloatLit>(std::get<CoefAtom>(tail.atom.v).coef));
          if (t.isVec() && ti.isVec()) {
            if (!TypeUtils::areTypesEqual(t.vecType(), ti.vecType()))
              diags.error("Vector arithmetic type mismatch", tail.span);
          } else if (t.isVec() && tiIsLitScalar) {
            // Literal broadcasts; chain stays vector. Carry t forward.
          } else if (ti.isVec() && tIsLitScalar) {
            // Symmetric — but we'd need to convert t to the vector type;
            // simpler to require Coef on the left for literal broadcast.
            diags.error("Vector arithmetic: literal must appear on the left", tail.span);
          } else {
            diags.error("Vector arithmetic type mismatch", tail.span);
          }
          continue;
        }
        if (t.isBV() && ti.isBV() && t.bvBits() != ti.bvBits())
          diags.error("Bitwidth mismatch", tail.span);
        if (t.isFloat() && ti.isFloat() && t.floatBits() != ti.floatBits())
          diags.error("Float width mismatch", tail.span);
        if (t.isBV() != ti.isBV() || t.isFloat() != ti.isFloat())
          diags.error("Mixed integer/float arithmetic not allowed", tail.span);
      }
    }
    return t;
  }

  Ty TypeChecker::typeOfAtom(
      const Atom &a, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags,
      std::optional<std::uint32_t> expectedBits, TypePtr ptrCtx
  ) {
    return std::visit(
        [&](auto &&arg) -> Ty {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            auto rt = typeOfLValue(arg.rval, vars, syms, diags);
            auto rb = TypeUtils::getBitWidth(rt);

            // [v0.2.1] Vector OpAtom: Coef can be a vector LValue of the
            // same type, or a literal that broadcasts to the vector.
            if (auto vt = TypeUtils::asVec(rt)) {
              if (auto lsid = std::get_if<LocalOrSymId>(&arg.coef)) {
                // Vector coef must match rval's vector type exactly.
                std::string nm = std::visit([](auto &&v) { return v.name; }, *lsid);
                TypePtr ct;
                if (std::holds_alternative<LocalId>(*lsid)) {
                  auto vit = vars.find(nm);
                  if (vit != vars.end())
                    ct = vit->second.type;
                } else {
                  auto sit = syms.find(nm);
                  if (sit != syms.end())
                    ct = sit->second.type;
                }
                if (!TypeUtils::areTypesEqual(ct, rt))
                  diags.error("Vector op: coefficient type must match", arg.span);
              }
              // Literal coef (IntLit/FloatLit): broadcasts to vector. No
              // type check needed beyond elem-kind being plausible.
              return Ty{Ty::VecTy{rt}};
            }

            if (rb) {
              // Integer case
              auto targetBits = rb; // Use rval bits as truth
              auto ct = typeOfCoef(arg.coef, vars, syms, diags, targetBits);
              auto cb = TypeUtils::getBitWidth(ct);
              if (cb && rb && *cb != *rb)
                diags.error("Bitwidth mismatch in operation", arg.span);
              return Ty{Ty::BVTy{rb.value()}};
            } else if (rt && std::holds_alternative<FloatType>(rt->v)) {
              // Float case
              auto &ft = std::get<FloatType>(rt->v);
              uint32_t bits = (ft.kind == FloatType::Kind::F32) ? 32 : 64;

              if (arg.op != AtomOpKind::Mul && arg.op != AtomOpKind::Div &&
                  arg.op != AtomOpKind::Mod) {
                diags.error("Invalid operator for float type", arg.span);
              }

              auto ct = typeOfCoef(arg.coef, vars, syms, diags, bits);
              if (!ct || !std::holds_alternative<FloatType>(ct->v)) {
                diags.error("Coefficient must be float", arg.span);
              } else {
                auto &cft = std::get<FloatType>(ct->v);
                uint32_t cbits = (cft.kind == FloatType::Kind::F32) ? 32 : 64;
                if (cbits != bits)
                  diags.error("Float width mismatch in operation", arg.span);
              }
              return Ty{Ty::FloatTy{bits}};
            }
            return Ty{std::monostate{}};
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            auto rt = typeOfLValue(arg.rval, vars, syms, diags);
            if (auto rb = TypeUtils::getBitWidth(rt)) {
              return Ty{Ty::BVTy{*rb}};
            }
            // [v0.2.1] Unary ~ on a vector of integers yields the same type.
            if (auto vt = TypeUtils::asVec(rt)) {
              if (!vt->elem || !std::holds_alternative<IntType>(vt->elem->v))
                diags.error("Unary ~ requires integer lane type", arg.span);
              return Ty{Ty::VecTy{rt}};
            }
            diags.error("Unary op not supported for float", arg.span);
            return Ty{std::monostate{}};
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            // [v0.2.1] Two forms. cond=Cond form; maskExpr=mask form.
            bool maskIsVec = false;
            if (arg.cond) {
              checkCond(*arg.cond, vars, syms, ann, diags);
            } else if (arg.maskExpr) {
              auto mt = typeOfExpr(*arg.maskExpr, vars, syms, ann, diags, std::nullopt);
              if (mt.isVec()) {
                // Mask must be <N> i1.
                auto vt = std::get_if<VecType>(&mt.vecType()->v);
                bool ok = vt && vt->elem && std::holds_alternative<IntType>(vt->elem->v) &&
                          (std::get<IntType>(vt->elem->v).kind == IntType::Kind::ICustom) &&
                          (std::get<IntType>(vt->elem->v).bits.value_or(0) == 1);
                if (!ok)
                  diags.error("Mask-based select requires <N> i1 mask", arg.span);
                maskIsVec = true;
              } else if (mt.isBV()) {
                if (mt.bvBits() != 1)
                  diags.error("Mask-based select scalar mask must be i1", arg.span);
              } else {
                diags.error("Mask-based select: mask must be i1 or <N> i1", arg.span);
              }
            }
            auto t1 = typeOfSelectVal(arg.vtrue, vars, syms, ann, diags, expectedBits, ptrCtx);
            auto t2 = typeOfSelectVal(
                arg.vfalse, vars, syms, ann, diags, expectedBits, t1.isPtr() ? t1.ptrType() : ptrCtx
            );
            // Mask-form with <N> i1: arms must be matching <N> T.
            if (maskIsVec) {
              if (!t1.isVec() || !t2.isVec() ||
                  !TypeUtils::areTypesEqual(t1.vecType(), t2.vecType()))
                diags.error(
                    "Mask-based select with <N> i1 mask requires matching <N> T arms", arg.span
                );
              return t1;
            }
            // Resolve null arm using sibling's type
            if (t1.isPtr() && !t2.isPtr())
              diags.error("Select arm type mismatch (pointer vs non-pointer)", arg.span);
            if (!t1.isPtr() && t2.isPtr())
              diags.error("Select arm type mismatch (non-pointer vs pointer)", arg.span);
            if (t1.isBV() && t2.isBV() && t1.bvBits() != t2.bvBits())
              diags.error("Select width mismatch", arg.span);
            if (t1.isFloat() && t2.isFloat() && t1.floatBits() != t2.floatBits())
              diags.error("Select float width mismatch", arg.span);
            if (t1.isVec() && t2.isVec() && !TypeUtils::areTypesEqual(t1.vecType(), t2.vecType()))
              diags.error("Select vector type mismatch", arg.span);
            if ((t1.isBV() || t1.isFloat()) &&
                (t2.isBV() != t1.isBV() || t2.isFloat() != t1.isFloat()))
              diags.error("Select type mismatch", arg.span);
            return t1;
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            auto ct = typeOfCoef(arg.coef, vars, syms, diags, expectedBits, ptrCtx);
            if (!ct)
              return Ty{std::monostate{}};
            if (auto cb = TypeUtils::getBitWidth(ct))
              return Ty{Ty::BVTy{*cb}};
            if (std::holds_alternative<FloatType>(ct->v))
              return Ty{
                  Ty::FloatTy{(std::get<FloatType>(ct->v).kind == FloatType::Kind::F32) ? 32u : 64u}
              };
            if (std::holds_alternative<PtrType>(ct->v))
              return Ty{Ty::PtrTy{ct}};
            // [v0.2.1] Sym of vector type: a CoefAtom referencing a
            // vector sym yields a vector value (used to stage sym
            // vectors into a local before lane subscripting).
            if (std::holds_alternative<VecType>(ct->v))
              return Ty{Ty::VecTy{ct}};
            return Ty{std::monostate{}};
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            auto rt = typeOfLValue(arg.rval, vars, syms, diags);
            if (auto rb = TypeUtils::getBitWidth(rt))
              return Ty{Ty::BVTy{*rb}};
            if (rt && std::holds_alternative<FloatType>(rt->v))
              return Ty{
                  Ty::FloatTy{(std::get<FloatType>(rt->v).kind == FloatType::Kind::F32) ? 32u : 64u}
              };
            if (rt && std::holds_alternative<PtrType>(rt->v))
              return Ty{Ty::PtrTy{rt}};
            if (rt && std::holds_alternative<VecType>(rt->v))
              return Ty{Ty::VecTy{rt}};
            return Ty{std::monostate{}};
          } else if constexpr (std::is_same_v<T, CmpAtom>) {
            // [v0.2.1] cmp: result is i1 (scalar) or <N> i1 (vector).
            // Both operands must have the same type; we infer from lhs.
            auto t1 = typeOfSelectVal(arg.lhs, vars, syms, ann, diags, std::nullopt);
            // [v0.2.1] Sibling pointer context: if LHS is a pointer, pass it
            // as ptrCtx to RHS so that `cmp == %p, null` can infer null's type.
            // This mirrors the pattern in checkCond() (line ~1047).
            TypePtr cmpPtrCtx = t1.isPtr() ? t1.ptrType() : nullptr;
            auto t2 = typeOfSelectVal(arg.rhs, vars, syms, ann, diags, std::nullopt, cmpPtrCtx);
            if (t1.isVec()) {
              if (!t2.isVec() || !TypeUtils::areTypesEqual(t1.vecType(), t2.vecType())) {
                diags.error("cmp: vector operand type mismatch", arg.span);
                return Ty{std::monostate{}};
              }
              // Result type: <N> i1
              auto vt = std::get_if<VecType>(&t1.vecType()->v);
              auto i1 = std::make_shared<Type>();
              i1->v = IntType{IntType::Kind::ICustom, 1, {}};
              auto resVec = std::make_shared<Type>();
              resVec->v = VecType{vt->size, i1, {}};
              return Ty{Ty::VecTy{resVec}};
            }
            // Scalar cmp: both operands must agree (BV/Float/Ptr); result i1
            if (t1.isBV() && t2.isBV() && t1.bvBits() != t2.bvBits())
              diags.error("cmp: integer width mismatch", arg.span);
            else if (t1.isFloat() && t2.isFloat() && t1.floatBits() != t2.floatBits())
              diags.error("cmp: float width mismatch", arg.span);
            return Ty{Ty::BVTy{1}};
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            // 'as' can cast between Int and Float or Int resizing
            TypePtr srcType = nullptr;
            if (auto lit = std::get_if<IntLit>(&arg.src)) {
              (void) lit; // IntLit -> infer from dstType
            } else if (auto flit = std::get_if<FloatLit>(&arg.src)) {
              (void) flit; // FloatLit
            } else if (auto sid = std::get_if<SymId>(&arg.src)) {
              auto it = syms.find(sid->name);
              if (it != syms.end())
                srcType = it->second.type;
            } else if (auto lv = std::get_if<LValue>(&arg.src)) {
              srcType = typeOfLValue(*lv, vars, syms, diags);
            }

            // Check destination
            if (auto it = std::get_if<IntType>(&arg.dstType->v)) {
              return Ty{Ty::BVTy{
                  static_cast<uint32_t>(it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64))
              }};
            } else if (auto ft = std::get_if<FloatType>(&arg.dstType->v)) {
              return Ty{Ty::FloatTy{ft->kind == FloatType::Kind::F32 ? 32u : 64u}};
            } else if (auto vt = std::get_if<VecType>(&arg.dstType->v)) {
              // [v0.2.1] Vector cast: src must be a vector of matching N.
              // The cast applies lane-wise; the elem-cast must be valid by
              // the scalar rules above (we don't re-check here).
              if (srcType) {
                auto svt = std::get_if<VecType>(&srcType->v);
                if (!svt || svt->size != vt->size)
                  diags.error("Vector cast: lane count must match", arg.span);
              }
              return Ty{Ty::VecTy{arg.dstType}};
            }
            diags.error("Destination of 'as' must be scalar or vector", arg.dstType->span);
            return Ty{std::monostate{}};
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            // addr <lv> : result is ptr T where T = type(lv)
            auto lvTy = typeOfLValue(arg.lv, vars, syms, diags);
            if (!lvTy)
              return Ty{std::monostate{}};
            // v0.2.0 §3.4.2: only a `let mut` local may have its address taken.
            // Parameters and immutable `let` locals are forbidden.
            auto it = vars.find(arg.lv.base.name);
            if (it != vars.end() && (it->second.isParam || !it->second.isMutable)) {
              diags.error(
                  "'addr' requires a 'let mut' local; '" + arg.lv.base.name + "' is " +
                      (it->second.isParam ? "a parameter" : "immutable"),
                  arg.span
              );
            }
            // [v0.2.1] §6.8.2: addr is forbidden on vector-typed lvalues
            // (whole vector or any lane). Walk the access chain looking for
            // any traversal through a vector.
            if (it != vars.end()) {
              TypePtr curT = it->second.type;
              for (const auto &acc: arg.lv.accesses) {
                if (curT && std::holds_alternative<VecType>(curT->v)) {
                  diags.error(
                      "addr is forbidden on a vector lane (vectors are not addressable)", arg.span
                  );
                  break;
                }
                if (auto ai_ = std::get_if<AccessIndex>(&acc)) {
                  (void) ai_;
                  if (auto at = TypeUtils::asArray(curT))
                    curT = at->elem;
                  else if (auto vt = TypeUtils::asVec(curT))
                    curT = vt->elem;
                  else
                    curT = nullptr;
                } else if (auto af_ = std::get_if<AccessField>(&acc)) {
                  if (auto st = TypeUtils::asStruct(curT)) {
                    auto sit = structs_.find(st->name.name);
                    if (sit != structs_.end()) {
                      auto fit = sit->second.fields.find(af_->field);
                      curT = (fit != sit->second.fields.end()) ? fit->second : nullptr;
                    }
                  } else
                    curT = nullptr;
                }
              }
              if (curT && std::holds_alternative<VecType>(curT->v))
                diags.error("addr is forbidden on a vector-typed lvalue (§2.11)", arg.span);
            }
            auto ptrNode = std::make_shared<Type>(PtrType{lvTy, arg.span}, arg.span);
            return Ty{Ty::PtrTy{ptrNode}};
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            // load <rval> : rval must be ptr T, result type is T
            auto rvTy = typeOfLValue(arg.rval, vars, syms, diags);
            if (!rvTy)
              return Ty{std::monostate{}};
            auto pt = std::get_if<PtrType>(&rvTy->v);
            if (!pt) {
              diags.error("'load' requires a pointer (ptr T) operand", arg.span);
              return Ty{std::monostate{}};
            }
            TypePtr pointee = pt->pointee;
            // [v0.2.1] §6.8.5: aggregate / vector pointees aren't loadable.
            if (std::holds_alternative<ArrayType>(pointee->v) ||
                std::holds_alternative<StructType>(pointee->v) ||
                std::holds_alternative<VecType>(pointee->v)) {
              diags.error(
                  "'load' pointee must be scalar or pointer (§6.8.5): aggregate / vector "
                  "pointees aren't loadable — use 'ptrindex' or 'ptrfield' to navigate first",
                  arg.span
              );
              return Ty{std::monostate{}};
            }
            if (auto pb = TypeUtils::getBitWidth(pointee))
              return Ty{Ty::BVTy{*pb}};
            if (std::holds_alternative<FloatType>(pointee->v)) {
              auto &ft = std::get<FloatType>(pointee->v);
              return Ty{Ty::FloatTy{ft.kind == FloatType::Kind::F32 ? 32u : 64u}};
            }
            if (std::holds_alternative<PtrType>(pointee->v))
              return Ty{Ty::PtrTy{pointee}};
            diags.error("'load' pointee type not supported", arg.span);
            return Ty{std::monostate{}};
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            // [v0.2.1] §6.8.9: ptrindex <ptr>, <index> where ptr : ptr [N] T,
            // index : iN, result : ptr T.
            auto rvTy = typeOfLValue(arg.rval, vars, syms, diags);
            if (!rvTy)
              return Ty{std::monostate{}};
            auto pt = std::get_if<PtrType>(&rvTy->v);
            if (!pt) {
              diags.error("'ptrindex' requires a pointer (ptr [N] T) operand", arg.span);
              return Ty{std::monostate{}};
            }
            auto at = std::get_if<ArrayType>(&pt->pointee->v);
            if (!at) {
              diags.error(
                  "'ptrindex' requires a ptr [N] T operand; got pointer to non-array", arg.span
              );
              return Ty{std::monostate{}};
            }
            // index must be integer-typed (IntLit or LocalOrSymId of int).
            std::visit(
                [&](auto &&iv) {
                  using IV = std::decay_t<decltype(iv)>;
                  if constexpr (std::is_same_v<IV, IntLit>) {
                    // OK
                  } else {
                    std::visit(
                        [&](auto &&id) {
                          auto vit = vars.find(id.name);
                          if (vit != vars.end()) {
                            if (!vit->second.type ||
                                !std::holds_alternative<IntType>(vit->second.type->v))
                              diags.error("'ptrindex' index must be integer-typed", arg.span);
                            return;
                          }
                          auto sit = syms.find(id.name);
                          if (sit != syms.end() && sit->second.type &&
                              !std::holds_alternative<IntType>(sit->second.type->v))
                            diags.error("'ptrindex' index must be integer-typed", arg.span);
                        },
                        iv
                    );
                  }
                },
                arg.index
            );
            auto resultPtr = std::make_shared<Type>(PtrType{at->elem, arg.span}, arg.span);
            return Ty{Ty::PtrTy{resultPtr}};
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            // [v0.2.1] §6.8.10: ptrfield <ptr>, <fld> where ptr : ptr @S,
            // result : ptr F where F is the declared type of @S.fld.
            auto rvTy = typeOfLValue(arg.rval, vars, syms, diags);
            if (!rvTy)
              return Ty{std::monostate{}};
            auto pt = std::get_if<PtrType>(&rvTy->v);
            if (!pt) {
              diags.error("'ptrfield' requires a pointer (ptr @S) operand", arg.span);
              return Ty{std::monostate{}};
            }
            auto st = std::get_if<StructType>(&pt->pointee->v);
            if (!st) {
              diags.error(
                  "'ptrfield' requires a ptr @S operand; got pointer to non-struct", arg.span
              );
              return Ty{std::monostate{}};
            }
            auto sit = structs_.find(st->name.name);
            if (sit == structs_.end()) {
              diags.error("'ptrfield': unknown struct " + st->name.name, arg.span);
              return Ty{std::monostate{}};
            }
            auto fit = sit->second.fields.find(arg.field);
            if (fit == sit->second.fields.end()) {
              diags.error(
                  "'ptrfield': struct " + st->name.name + " has no field '" + arg.field + "'",
                  arg.span
              );
              return Ty{std::monostate{}};
            }
            auto resultPtr = std::make_shared<Type>(PtrType{fit->second, arg.span}, arg.span);
            return Ty{Ty::PtrTy{resultPtr}};
          }
          return Ty{std::monostate{}};
        },
        a.v
    );
  }

  TypePtr TypeChecker::typeOfCoef(
      const Coef &c, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, DiagBag &diags,
      std::optional<std::uint32_t> expectedBits, TypePtr ptrCtx
  ) {
    if (auto lit = std::get_if<IntLit>(&c)) {
      uint32_t bits = expectedBits.value_or(32);
      checkLiteralRange(lit->value, bits, lit->span, diags);
      auto t = std::make_shared<Type>();
      IntType it;
      if (bits == 32)
        it.kind = IntType::Kind::I32;
      else if (bits == 64)
        it.kind = IntType::Kind::I64;
      else {
        it.kind = IntType::Kind::ICustom;
        it.bits = bits;
      }
      it.span = lit->span;
      t->v = it;
      t->span = lit->span;
      return t;
    }
    if (auto flit = std::get_if<FloatLit>(&c)) {
      uint32_t bits = expectedBits.value_or(32);
      auto t = std::make_shared<Type>();
      FloatType ft;
      ft.kind = (bits == 64) ? FloatType::Kind::F64 : FloatType::Kind::F32;
      ft.span = flit->span;
      t->v = ft;
      t->span = flit->span;
      return t;
    }
    if (auto nl = std::get_if<NullLit>(&c)) {
      if (ptrCtx && std::holds_alternative<PtrType>(ptrCtx->v))
        return ptrCtx;
      diags.error("'null' requires a pointer type context (ptr T)", nl->span);
      return nullptr;
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

  void TypeChecker::checkLiteralRange(int64_t val, uint32_t bits, SourceSpan sp, DiagBag &diags) {
    if (bits >= 64)
      return;
    int64_t min = -(1LL << (bits - 1));
    int64_t max = (1LL << bits) - 1; // Allow up to unsigned max for convenience
    if (val < min || val > max) {
      diags.error(
          "Literal " + std::to_string(val) + " out of range for i" + std::to_string(bits) + " ([" +
              std::to_string(min) + ", " + std::to_string(max) + "])",
          sp
      );
    }
  }

  Ty TypeChecker::typeOfSelectVal(
      const SelectVal &sv, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, [[maybe_unused]] TypeAnnotations &ann,
      DiagBag &diags, std::optional<std::uint32_t> expectedBits, TypePtr ptrCtx
  ) {
    TypePtr t;
    if (auto rv = std::get_if<RValue>(&sv)) {
      t = typeOfLValue(*rv, vars, syms, diags);
    } else {
      t = typeOfCoef(std::get<Coef>(sv), vars, syms, diags, expectedBits, ptrCtx);
    }

    if (!t)
      return Ty{std::monostate{}};
    if (auto bits = TypeUtils::getBitWidth(t)) {
      return Ty{Ty::BVTy{*bits}};
    }
    if (std::holds_alternative<FloatType>(t->v)) {
      auto &ft = std::get<FloatType>(t->v);
      return Ty{Ty::FloatTy{ft.kind == FloatType::Kind::F32 ? 32u : 64u}};
    }
    if (std::holds_alternative<PtrType>(t->v))
      return Ty{Ty::PtrTy{t}};
    // [v0.2.1] Vector arms of `select` (Cond form or mask form).
    if (std::holds_alternative<VecType>(t->v))
      return Ty{Ty::VecTy{t}};
    return Ty{std::monostate{}};
  }

  void TypeChecker::checkCond(
      const Cond &c, const std::unordered_map<std::string, VarInfo> &vars,
      const std::unordered_map<std::string, SymInfo> &syms, TypeAnnotations &ann, DiagBag &diags
  ) {
    auto t1 = typeOfExpr(c.lhs, vars, syms, ann, diags, std::nullopt);
    // If LHS is a pointer, pass it as ptrCtx for RHS (null inference)
    TypePtr rhsPtrCtx = t1.isPtr() ? t1.ptrType() : nullptr;
    auto t2 = typeOfExpr(
        c.rhs, vars, syms, ann, diags, t1.isBV() ? std::optional(t1.bvBits()) : std::nullopt,
        rhsPtrCtx
    );
    if (t1.isBV() && t2.isBV() && t1.bvBits() != t2.bvBits())
      diags.error("Bitwidth mismatch in condition", c.span);
    if (t1.isPtr() && !t2.isPtr())
      diags.error("Pointer condition requires both operands to be pointers", c.span);
    if (!t1.isPtr() && t2.isPtr())
      diags.error("Pointer condition requires both operands to be pointers", c.span);
    if (t1.isPtr() && t2.isPtr() && !TypeUtils::areTypesEqual(t1.ptrType(), t2.ptrType()))
      diags.error("Pointer comparison requires same pointee type", c.span);
  }

} // namespace symir
