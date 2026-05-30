#include "reify/rewrite.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

#include "analysis/type_utils.hpp"
#include "ast/sir_printer.hpp"
#include "reify/hyperparameters.hpp"

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Descriptor-value parsing
  //
  // Descriptors stringify scalars via std::to_string (ints) or a printf
  // float format (floats). std::stoll / std::stod accept both, so
  // round-tripping is straight. Both helpers return nullopt on any
  // partial parse so a malformed descriptor field just disables one
  // rewrite opportunity rather than crashing the engine.
  // ---------------------------------------------------------------------------
  static std::optional<std::int64_t> parseI64(const std::string &s) {
    try {
      size_t pos = 0;
      long long v = std::stoll(s, &pos);
      if (pos != s.size())
        return std::nullopt;
      return static_cast<std::int64_t>(v);
    } catch (...) {
      return std::nullopt;
    }
  }

  static std::optional<double> parseF64(const std::string &s) {
    // Delegate to the canonical SymIR float parser so descriptor values
    // round-trip identically to lexed float literals — same subnormal
    // handling, same signed-zero behaviour, same overflow rule.
    try {
      return parseFloatLiteral(s);
    } catch (...) {
      return std::nullopt;
    }
  }

  // ---------------------------------------------------------------------------
  // Build an Expr that evaluates to a scalar literal of the given SIR
  // type. Used to construct call arguments from descriptor.paramValues.
  // Returns nullopt for types we can't synthesize (pointers, vectors,
  // structs) — the rule then declines to apply.
  // ---------------------------------------------------------------------------
  static std::optional<Expr> makeScalarLitExpr(const std::string &sirType, const std::string &val) {
    Atom a;
    if (sirType.size() >= 1 && sirType[0] == 'i') {
      auto iv = parseI64(val);
      if (!iv)
        return std::nullopt;
      IntLit lit;
      lit.value = *iv;
      Coef coef = lit;
      CoefAtom ca;
      ca.coef = coef;
      a.v = ca;
    } else if (sirType == "f32" || sirType == "f64") {
      auto fv = parseF64(val);
      if (!fv)
        return std::nullopt;
      FloatLit lit;
      lit.value = *fv;
      Coef coef = lit;
      CoefAtom ca;
      ca.coef = coef;
      a.v = ca;
    } else {
      return std::nullopt;
    }
    Expr e;
    e.first = std::move(a);
    return e;
  }

  // ---------------------------------------------------------------------------
  // Var+bias call arg
  //
  // Build `%var + bias` for an integer call argument. `paramType` is the
  // callee's i<N> param type as a SIR string; `expectedValStr` is the
  // solved value the call should evaluate to. We scan the caller for
  // int-typed mutable lets whose declared type matches and whose init
  // is a literal we can read back, then pick uniformly. The chosen var's
  // value is `var.let_init` at the splice site because the rewrite
  // prepends to the head of the entry block — no user code has run yet.
  //
  // Returns nullopt when:
  //   - parseI64(expectedValStr) fails (descriptor value malformed);
  //   - no caller let matches the param type / has a literal init;
  //   - the computed bias overflows the var's signed range (we use the
  //     same conservative range check the call-site offset uses, so a
  //     UBSan-clean C lowering is guaranteed).
  // ---------------------------------------------------------------------------
  struct AddrChecker {
    const std::string &varName;
    bool addressTaken = false;

    AddrChecker(const std::string &vn) : varName(vn) {}

    void check(const Atom &atom) {
      if (addressTaken)
        return;
      if (auto addr = std::get_if<AddrAtom>(&atom.v)) {
        if (addr->lv.base.name == varName) {
          addressTaken = true;
          return;
        }
      }
      if (auto select = std::get_if<SelectAtom>(&atom.v)) {
        if (select->cond)
          check(*select->cond);
        if (select->maskExpr)
          check(*select->maskExpr);
      } else if (auto call = std::get_if<CallAtom>(&atom.v)) {
        for (auto &arg: call->args) {
          if (arg)
            check(*arg);
        }
      }
    }

    void check(const Expr &expr) {
      if (addressTaken)
        return;
      check(expr.first);
      for (auto &tail: expr.rest) {
        check(tail.atom);
      }
    }

    void check(const Cond &cond) {
      if (addressTaken)
        return;
      check(cond.lhs);
      check(cond.rhs);
    }

    void check(const Instr &instr) {
      if (addressTaken)
        return;
      std::visit(
          [this](auto &arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, AssignInstr>) {
              check(arg.rhs);
            } else if constexpr (std::is_same_v<T, AssumeInstr> ||
                                 std::is_same_v<T, RequireInstr>) {
              check(arg.cond);
            } else if constexpr (std::is_same_v<T, StoreInstr>) {
              check(arg.ptr);
              check(arg.val);
            }
          },
          instr
      );
    }

    void check(const Terminator &term) {
      if (addressTaken)
        return;
      std::visit(
          [this](auto &arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, BrTerm>) {
              if (arg.cond)
                check(*arg.cond);
            } else if constexpr (std::is_same_v<T, RetTerm>) {
              if (arg.value)
                check(*arg.value);
            }
          },
          term
      );
    }
  };

  static bool isVarConstantBeforeBlock(
      const FunDecl &caller, const FuncDescriptor &callerDesc, const std::string &varName,
      const std::string &targetBlockLabel
  ) {
    AddrChecker checker(varName);
    for (const auto &block: caller.blocks) {
      for (const auto &instr: block.instrs) {
        checker.check(instr);
      }
      checker.check(block.term);
    }
    if (checker.addressTaken) {
      return false;
    }

    auto it = std::find(callerDesc.path.begin(), callerDesc.path.end(), targetBlockLabel);
    if (it == callerDesc.path.end()) {
      return true;
    }

    std::unordered_set<std::string> checkedBlocks;
    for (auto pathIt = callerDesc.path.begin(); pathIt != it; ++pathIt) {
      const std::string &blockLabel = *pathIt;
      if (checkedBlocks.count(blockLabel)) {
        continue;
      }
      checkedBlocks.insert(blockLabel);

      const Block *blockPtr = nullptr;
      for (const auto &b: caller.blocks) {
        if (b.label.name == blockLabel) {
          blockPtr = &b;
          break;
        }
      }
      if (!blockPtr) {
        continue;
      }

      for (const auto &instr: blockPtr->instrs) {
        if (auto assign = std::get_if<AssignInstr>(&instr)) {
          if (assign->lhs.base.name == varName) {
            return false;
          }
        }
      }
    }

    return true;
  }

  // ---------------------------------------------------------------------------
  // Var+bias call arg
  //
  // Build `%var + bias` for an integer call argument. `paramType` is the
  // callee's i<N> param type as a SIR string; `expectedValStr` is the
  // solved value the call should evaluate to. We scan the caller for
  // int-typed mutable lets whose declared type matches and whose init
  // is a literal we can read back, then pick uniformly. The chosen var's
  // value must be unchanged along the execution path prior to targetBlockLabel.
  //
  // Returns nullopt when:
  //   - parseI64(expectedValStr) fails (descriptor value malformed);
  //   - no caller let matches the param type / has a literal init;
  //   - the computed bias overflows the var's signed range (we use the
  //     same conservative range check the call-site offset uses, so a
  //     UBSan-clean C lowering is guaranteed).
  // ---------------------------------------------------------------------------
  static std::optional<Expr> tryMakeVarBiasArg(
      const FunDecl &caller, const FuncDescriptor &callerDesc, const std::string &targetBlockLabel,
      const std::string &paramType, const std::string &expectedValStr, std::mt19937 &rng
  ) {
    auto expectedOpt = parseI64(expectedValStr);
    if (!expectedOpt)
      return std::nullopt;
    int paramBits = std::atoi(paramType.c_str() + 1);
    if (paramBits <= 0)
      return std::nullopt;

    struct Cand {
      const LetDecl *ld;
      std::int64_t initVal;
    };

    std::vector<Cand> cands;
    cands.reserve(caller.lets.size());
    for (const auto &ld: caller.lets) {
      if (!ld.isMutable)
        continue;
      if (!ld.init || ld.init->kind != InitVal::Kind::Int)
        continue;
      if (SIRPrinter::typeToString(ld.type) != paramType)
        continue;
      if (!isVarConstantBeforeBlock(caller, callerDesc, ld.name.name, targetBlockLabel))
        continue;
      cands.push_back({&ld, std::get<IntLit>(ld.init->value).value});
    }
    if (cands.empty())
      return std::nullopt;

    std::uniform_int_distribution<size_t> pick(0, cands.size() - 1);
    const Cand &c = cands[pick(rng)];

    // bias = expected - var.let_init. Same overflow + range check the
    // call-site offset uses — keeps the C lowering UBSan-clean and
    // makes the wrap math observably identical to the literal path.
    std::int64_t bias = 0;
    if (__builtin_sub_overflow((std::int64_t) *expectedOpt, c.initVal, &bias))
      return std::nullopt;
    if (paramBits < 64) {
      std::int64_t maxv = (std::int64_t{1} << (paramBits - 1)) - 1;
      std::int64_t minv = -(std::int64_t{1} << (paramBits - 1));
      if (bias < minv || bias > maxv)
        return std::nullopt;
    }

    LValue lv;
    lv.base = c.ld->name;
    Atom varAtom;
    varAtom.v = RValueAtom{lv, {}};
    Expr e;
    e.first = std::move(varAtom);
    if (bias != 0) {
      IntLit lit;
      lit.value = bias;
      Coef coef = lit;
      CoefAtom ca;
      ca.coef = coef;
      Atom biasAtom;
      biasAtom.v = std::move(ca);
      Expr::Tail tail;
      tail.op = (bias >= 0) ? AddOp::Plus : AddOp::Minus;
      // For Minus we negate the literal so the tail reads `... - |bias|`.
      // Negating INT64_MIN would overflow, so guard against it; the
      // bias range check above already rejects values where -bias
      // can't be represented in the param's signed range, but bias
      // can still equal INT64_MIN for paramBits=64.
      if (tail.op == AddOp::Minus) {
        if (bias == std::numeric_limits<std::int64_t>::min())
          return std::nullopt;
        std::get<IntLit>(std::get<CoefAtom>(biasAtom.v).coef).value = -bias;
      }
      tail.atom = std::move(biasAtom);
      e.rest.push_back(std::move(tail));
    }
    return e;
  }

  // ---------------------------------------------------------------------------
  // LiteralToCallRule
  // ---------------------------------------------------------------------------

  namespace {

    class LiteralToCallRule : public RewriteRule {
    public:
      const char *name() const override { return "LiteralToCall"; }

      std::vector<RewriteSite> findSites(const FunDecl &caller) override {
        std::vector<RewriteSite> sites;
        for (size_t i = 0; i < caller.lets.size(); ++i) {
          const auto &ld = caller.lets[i];
          if (!ld.init)
            continue;
          // Apply emits an AssignInstr to ld.name; only `let mut` is
          // assignable per spec §3.5.2. rysmith currently emits every
          // let as mutable so this filter is defensive, but it
          // protects the rule against future generators that mix in
          // immutable lets.
          if (!ld.isMutable)
            continue;
          // Skip the checksum accumulator — it is unconditionally
          // overwritten to `0` at the top of the exit block (see
          // func_gen::buildChecksum), so any splice into its let-init
          // is dead computation. Filtering it out at the source rule
          // saves a wasted attempt budget per edge.
          if (ld.name.name == "%_chk")
            continue;
          // Only scalar literal initializers — Atom-form inits are
          // skipped (they're already calls/loads/etc.) and aggregate
          // inits are out of scope for v1.
          if (ld.init->kind == InitVal::Kind::Int) {
            RewriteSite s;
            s.kind = RewriteSite::Kind::LetInitIntLit;
            s.letIdx = static_cast<int>(i);
            s.intVal = std::get<IntLit>(ld.init->value).value;
            s.sirType = SIRPrinter::typeToString(ld.type);
            sites.push_back(s);
          } else if (ld.init->kind == InitVal::Kind::Float) {
            RewriteSite s;
            s.kind = RewriteSite::Kind::LetInitFloatLit;
            s.letIdx = static_cast<int>(i);
            s.floatVal = std::get<FloatLit>(ld.init->value).value;
            s.sirType = SIRPrinter::typeToString(ld.type);
            sites.push_back(s);
          }
        }
        return sites;
      }

      bool matchCallee(
          const RewriteSite &site, const FuncDescriptor &callee, std::size_t fixedRealizationIdx
      ) override {
        // Only the ret-type has to match: we splice in `call + (c - ret)`
        // so semantics are preserved regardless of whether the callee
        // happens to solve to the same value as the literal. Float
        // literals are excluded because IEEE 754 subtraction is not
        // generally lossless (c - ret + ret ≠ c when rounding fires),
        // which would break --validate equivalence.
        if (callee.retType != site.sirType)
          return false;
        if (site.kind == RewriteSite::Kind::LetInitFloatLit)
          return false;
        if (fixedRealizationIdx >= callee.realizations.size())
          return false;
        const auto &rz = callee.realizations[fixedRealizationIdx];
        return !rz.retValue.empty() && parseI64(rz.retValue).has_value();
      }

      bool apply(
          FunDecl &caller, const FuncDescriptor &callerDesc, const RewriteSite &site,
          const FuncDescriptor &callee, std::size_t realizationIdx, std::mt19937 &rng
      ) override {
        if (site.letIdx < 0 || (size_t) site.letIdx >= caller.lets.size())
          return false;
        const auto &rz = callee.realizations[realizationIdx];
        if (rz.paramValues.size() != callee.params.size())
          return false;
        auto retVal = parseI64(rz.retValue);
        if (!retVal)
          return false;

        // Check if the variable is mutated (written to) or has its address taken.
        bool isMutatedOrAddressTaken = false;
        const std::string &varName = caller.lets[site.letIdx].name.name;
        for (const auto &block: caller.blocks) {
          for (const auto &instr: block.instrs) {
            if (auto assign = std::get_if<AssignInstr>(&instr)) {
              if (assign->lhs.base.name == varName) {
                isMutatedOrAddressTaken = true;
                break;
              }
            }
          }
          if (isMutatedOrAddressTaken)
            break;
        }


        if (!isMutatedOrAddressTaken) {
          AddrChecker checker(varName);
          for (const auto &block: caller.blocks) {
            for (const auto &instr: block.instrs) {
              checker.check(instr);
            }
            checker.check(block.term);
          }
          if (checker.addressTaken) {
            isMutatedOrAddressTaken = true;
          }
        }

        // Select a random executed block of the caller that is not part of a loop.
        // Selecting a random executed block allows us to distribute call realizations
        // across different blocks of the caller execution path.
        std::vector<std::size_t> executedIndices;
        std::unordered_set<std::string> pathSet(callerDesc.path.begin(), callerDesc.path.end());
        for (std::size_t i = 0; i < caller.blocks.size(); ++i) {
          if (pathSet.find(caller.blocks[i].label.name) != pathSet.end()) {
            executedIndices.push_back(i);
          }
        }

        if (isMutatedOrAddressTaken) {
          // If mutated or address-taken, we MUST insert at the entry block (index 0)
          // to ensure it runs before any reads/mutations.
          executedIndices = {0};
        } else {
          std::shuffle(executedIndices.begin(), executedIndices.end(), rng);
        }

        auto getSuccessors = [&](std::size_t bi) -> std::vector<std::size_t> {
          std::vector<std::size_t> succs;
          const auto &term = caller.blocks[bi].term;
          auto addLabel = [&](const std::string &labelName) {
            for (std::size_t idx = 0; idx < caller.blocks.size(); ++idx) {
              if (caller.blocks[idx].label.name == labelName) {
                succs.push_back(idx);
                break;
              }
            }
          };
          if (auto br = std::get_if<BrTerm>(&term)) {
            if (br->isConditional) {
              addLabel(br->thenLabel.name);
              addLabel(br->elseLabel.name);
            } else {
              addLabel(br->dest.name);
            }
          }
          return succs;
        };

        auto isPartOfLoop = [&](std::size_t startIdx) -> bool {
          std::vector<bool> visited(caller.blocks.size(), false);
          std::vector<std::size_t> stack;
          for (std::size_t s: getSuccessors(startIdx)) {
            stack.push_back(s);
          }
          while (!stack.empty()) {
            std::size_t curr = stack.back();
            stack.pop_back();
            if (curr == startIdx)
              return true;
            if (!visited[curr]) {
              visited[curr] = true;
              for (std::size_t s: getSuccessors(curr)) {
                stack.push_back(s);
              }
            }
          }
          return false;
        };

        std::optional<std::size_t> targetBlockIdx;
        for (std::size_t blockIdx: executedIndices) {
          // Loop guard: if the block is part of a loop, prepending into it
          // re-fires the call on every back-edge traversal — resetting the let
          // to its original literal value every iteration.
          if (isPartOfLoop(blockIdx)) {
            continue;
          }
          targetBlockIdx = blockIdx;
          break;
        }

        if (!targetBlockIdx) {
          return false;
        }

        // Build the call atom. Each arg is either a bare literal
        // (matching the solver's paramValue) or `%var + bias` where
        // %var is a caller scalar of the same type that is constant
        // along the execution path prior to targetBlockIdx.
        CallAtom ca;
        GlobalId gid;
        gid.name = callee.name;
        ca.callee = gid;
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        for (size_t i = 0; i < callee.params.size(); ++i) {
          const auto &paramType = callee.params[i].type;
          const auto &paramValStr = rz.paramValues[i].second;
          std::optional<Expr> argExpr;
          if (paramType.size() >= 2 && paramType[0] == 'i' && uni(rng) < rylink::hp::kPVarBiasArg) {
            argExpr = tryMakeVarBiasArg(
                caller, callerDesc, caller.blocks[*targetBlockIdx].label.name, paramType,
                paramValStr, rng
            );
          }
          if (!argExpr)
            argExpr = makeScalarLitExpr(paramType, paramValStr);
          if (!argExpr)
            return false; // unsupported arg type — bail without mutating
          ca.args.push_back(std::make_shared<Expr>(std::move(*argExpr)));
        }
        Atom callAtom;
        callAtom.v = std::move(ca);

        // Offset literal: `c - ret` under bitvector arithmetic. SymIR
        // `T + lit` truncates with wraparound (§6.5), so the BV-side
        // math always reproduces `c`. The C backend, however, emits
        // the call site as a signed-int addition, and UBSan trips
        // when the intermediate `ret + offset` overflows the let's
        // signed range — even though the wrapped result equals `c`.
        //
        // Decline the rewrite when the offset doesn't fit in the
        // let's signed range. Concretely: compute `c - ret` in 64-bit
        // signed (rejecting the i64-vs-i64 overflow case via the
        // builtin), then range-check against the destination width.
        //
        // The check is conservative — some perfectly fine programs
        // are skipped — but the alternative (BV-correct C output) is
        // a deep change to the backend; declining costs at most a
        // missed splice on a single edge and the rewrite engine just
        // tries another candidate site.
        int parsedBits = 0;
        if (site.sirType.size() < 2 || site.sirType[0] != 'i' ||
            (parsedBits = std::atoi(site.sirType.c_str() + 1)) <= 0)
          return false;
        std::int64_t offsetVal = 0;
        if (__builtin_sub_overflow((std::int64_t) site.intVal, (std::int64_t) *retVal, &offsetVal))
          return false;
        if (parsedBits < 64) {
          std::int64_t maxv = (std::int64_t{1} << (parsedBits - 1)) - 1;
          std::int64_t minv = -(std::int64_t{1} << (parsedBits - 1));
          if (offsetVal < minv || offsetVal > maxv)
            return false;
        }
        IntLit offsetLit;
        offsetLit.value = offsetVal;

        // Build `call (+ offset)?` as the RHS of an AssignInstr that
        // we prepend to the target block: `%x = call @callee(args) (+
        // offset)?;`. The original literal init stays in place — it
        // runs before any block, then the prepended assign overwrites
        // %x with the semantically-equivalent call expression. We
        // can't fold the call straight into the let-init because
        // InitVal::Atom holds a single Atom and we need an Expr (call
        // + offset).
        Expr rhs;
        rhs.first = std::move(callAtom);
        if (offsetLit.value != 0) {
          Coef offsetCoef = offsetLit;
          CoefAtom offsetCa;
          offsetCa.coef = offsetCoef;
          Atom offsetAtom;
          offsetAtom.v = std::move(offsetCa);
          Expr::Tail tail;
          tail.op = AddOp::Plus;
          tail.atom = std::move(offsetAtom);
          rhs.rest.push_back(std::move(tail));
        }

        AssignInstr ai;
        ai.lhs.base = caller.lets[site.letIdx].name;
        ai.rhs = std::move(rhs);

        caller.blocks[*targetBlockIdx].instrs.insert(
            caller.blocks[*targetBlockIdx].instrs.begin(), Instr{std::move(ai)}
        );
        return true;
      }
    };

  } // namespace

  std::unique_ptr<RewriteRule> makeLiteralToCallRule() {
    return std::make_unique<LiteralToCallRule>();
  }

  // ---------------------------------------------------------------------------
  // Engine
  // ---------------------------------------------------------------------------

  static std::optional<std::string>
  getVarTypeString(const std::string &name, const FunDecl &caller) {
    for (const auto &p: caller.params) {
      if (p.name.name == name) {
        return SIRPrinter::typeToString(p.type);
      }
    }
    for (const auto &l: caller.lets) {
      if (l.name.name == name) {
        return SIRPrinter::typeToString(l.type);
      }
    }
    for (const auto &s: caller.syms) {
      if (s.name.name == name) {
        return SIRPrinter::typeToString(s.type);
      }
    }
    return std::nullopt;
  }

  static std::optional<std::string> inferExprType(const Expr &expr, const FunDecl &caller) {
    auto getAtomType = [&](const Atom &atom) -> std::optional<std::string> {
      if (auto coef = std::get_if<CoefAtom>(&atom.v)) {
        if (auto varId = std::get_if<LocalOrSymId>(&coef->coef)) {
          std::string varName;
          if (auto loc = std::get_if<LocalId>(varId))
            varName = loc->name;
          else if (auto sym = std::get_if<SymId>(varId))
            varName = sym->name;
          return getVarTypeString(varName, caller);
        }
      } else if (auto rval = std::get_if<RValueAtom>(&atom.v)) {
        if (rval->rval.accesses.empty()) {
          return getVarTypeString(rval->rval.base.name, caller);
        }
      }
      return std::nullopt;
    };

    if (auto t = getAtomType(expr.first))
      return t;
    for (const auto &tail: expr.rest) {
      if (auto t = getAtomType(tail.atom))
        return t;
    }
    return std::nullopt;
  }

  static std::optional<std::string> getExpectedTypeForExpr(
      const Expr &expr, const FunDecl &caller, const std::optional<std::string> &contextType
  ) {
    if (auto inferred = inferExprType(expr, caller)) {
      return inferred;
    }
    return contextType;
  }

  // ---------------------------------------------------------------------------
  // CallReplacer
  //
  // An AST mutator that walks expression trees, conditions, instructions,
  // and terminators in search of variables or literals that match the callee's
  // return type. It collects pointers to all matching atoms as candidates.
  //
  // Expected types of literals are inferred from their context (e.g. assignment
  // LHS, condition partner, or store target) to guarantee type checker safety.
  // ---------------------------------------------------------------------------
  struct CallReplacer {
    const TypePtr &expectedType;
    const FunDecl &caller;
    std::string expectedTypeStr;
    std::vector<Atom *> candidates;

    CallReplacer(const TypePtr &et, const FunDecl &c) : expectedType(et), caller(c) {
      if (expectedType) {
        expectedTypeStr = SIRPrinter::typeToString(expectedType);
      }
    }

    void collect(Atom &atom, const std::optional<std::string> &contextType) {
      if (expectedTypeStr.empty())
        return;

      if (auto coefAtom = std::get_if<CoefAtom>(&atom.v)) {
        bool match = false;
        if (std::holds_alternative<IntLit>(coefAtom->coef)) {
          if (contextType && *contextType == expectedTypeStr && expectedTypeStr[0] == 'i') {
            match = true;
          }
        } else if (std::holds_alternative<FloatLit>(coefAtom->coef)) {
          if (contextType && *contextType == expectedTypeStr &&
              (expectedTypeStr == "f32" || expectedTypeStr == "f64")) {
            match = true;
          }
        } else if (std::holds_alternative<NullLit>(coefAtom->coef)) {
          if (contextType && *contextType == expectedTypeStr &&
              expectedTypeStr.rfind("ptr", 0) == 0) {
            match = true;
          }
        } else if (auto varId = std::get_if<LocalOrSymId>(&coefAtom->coef)) {
          std::string varName = std::holds_alternative<LocalId>(*varId)
                                    ? std::get<LocalId>(*varId).name
                                    : std::get<SymId>(*varId).name;
          if (auto tStr = getVarTypeString(varName, caller)) {
            if (*tStr == expectedTypeStr)
              match = true;
          }
        }
        if (match) {
          candidates.push_back(&atom);
        }
      } else if (auto rvalAtom = std::get_if<RValueAtom>(&atom.v)) {
        if (rvalAtom->rval.accesses.empty()) {
          if (auto tStr = getVarTypeString(rvalAtom->rval.base.name, caller)) {
            if (*tStr == expectedTypeStr) {
              candidates.push_back(&atom);
            }
          }
        }
      }

      // Traverse nested
      if (auto select = std::get_if<SelectAtom>(&atom.v)) {
        if (select->cond)
          collect(*select->cond);
        if (select->maskExpr)
          collect(*select->maskExpr, std::nullopt);
      } else if (auto call = std::get_if<CallAtom>(&atom.v)) {
        for (auto &arg: call->args) {
          if (arg)
            collect(*arg, std::nullopt);
        }
      }
    }

    void collect(Expr &expr, const std::optional<std::string> &contextType) {
      auto effType = getExpectedTypeForExpr(expr, caller, contextType);
      collect(expr.first, effType);
      for (auto &tail: expr.rest) {
        collect(tail.atom, effType);
      }
    }

    void collect(Cond &cond) {
      auto lhsType = inferExprType(cond.lhs, caller);
      auto rhsType = inferExprType(cond.rhs, caller);
      collect(cond.lhs, rhsType);
      collect(cond.rhs, lhsType);
    }

    void collect(Instr &instr) {
      std::visit(
          [this](auto &arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, AssignInstr>) {
              std::optional<std::string> lhsType;
              if (arg.lhs.accesses.empty()) {
                lhsType = getVarTypeString(arg.lhs.base.name, caller);
              }
              collect(arg.rhs, lhsType);
            } else if constexpr (std::is_same_v<T, AssumeInstr> ||
                                 std::is_same_v<T, RequireInstr>) {
              collect(arg.cond);
            } else if constexpr (std::is_same_v<T, StoreInstr>) {
              std::optional<std::string> valType;
              if (auto ptrType = inferExprType(arg.ptr, caller)) {
                if (ptrType->rfind("ptr ", 0) == 0) {
                  valType = ptrType->substr(4);
                }
              }
              collect(arg.ptr, std::nullopt);
              collect(arg.val, valType);
            }
          },
          instr
      );
    }

    void collect(Terminator &term) {
      std::visit(
          [this](auto &arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, BrTerm>) {
              if (arg.cond)
                collect(*arg.cond);
            } else if constexpr (std::is_same_v<T, RetTerm>) {
              if (arg.value) {
                std::optional<std::string> retTypeStr = SIRPrinter::typeToString(caller.retType);
                collect(*arg.value, retTypeStr);
              }
            }
          },
          term
      );
    }
  };

  // ---------------------------------------------------------------------------
  // Build arguments for call realization.
  //
  // - If `randomize` is true (for unexecuted blocks), we synthesize random
  //   arguments or pick matching variables since they are never executed and cannot trigger UB.
  // - If `randomize` is false (for the executed path fallback call), we MUST
  //   use only exact literal constants representing the callee's solved param
  //   values. We avoid tryMakeVarBiasArg here because if the caller's entry block
  //   is re-entered via a back-edge loop, local variables used in bias math
  //   will have been mutated, leading to incorrect arguments and runtime UB.
  // ---------------------------------------------------------------------------
  static std::optional<std::vector<std::shared_ptr<Expr>>> makeCallArgs(
      const FunDecl &caller, const FunDecl &calleeFn, const FuncDescriptor &callee,
      const FuncDescriptor::Realization &rz, std::mt19937 &rng, bool randomize
  ) {
    std::vector<std::shared_ptr<Expr>> args;
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::uniform_int_distribution<int> randInt(0, 100);
    std::uniform_real_distribution<double> randFloat(0.0, 100.0);

    for (size_t i = 0; i < callee.params.size(); ++i) {
      const auto &paramType = callee.params[i].type;
      const auto &paramValStr = rz.paramValues[i].second;
      std::optional<Expr> argExpr;

      if (randomize) {
        std::vector<std::string> matchingVars;
        for (const auto &p: caller.params) {
          if (SIRPrinter::typeToString(p.type) == paramType) {
            matchingVars.push_back(p.name.name);
          }
        }
        for (const auto &l: caller.lets) {
          if (SIRPrinter::typeToString(l.type) == paramType) {
            matchingVars.push_back(l.name.name);
          }
        }
        for (const auto &s: caller.syms) {
          if (SIRPrinter::typeToString(s.type) == paramType) {
            matchingVars.push_back(s.name.name);
          }
        }

        bool chooseVar = false;
        if (!matchingVars.empty()) {
          std::uniform_int_distribution<int> coin(0, 1);
          if (coin(rng) == 0) {
            chooseVar = true;
          }
        }

        if (chooseVar) {
          std::uniform_int_distribution<size_t> pick(0, matchingVars.size() - 1);
          std::string chosenVar = matchingVars[pick(rng)];
          if (chosenVar.size() >= 2 && chosenVar[1] == '?') {
            SymId sid;
            sid.name = chosenVar;
            sid.span = calleeFn.name.span;
            CoefAtom ca;
            ca.coef = LocalOrSymId{sid};
            ca.span = calleeFn.name.span;
            Atom a;
            a.v = ca;
            a.span = calleeFn.name.span;
            Expr e;
            e.first = std::move(a);
            e.span = calleeFn.name.span;
            argExpr = std::move(e);
          } else {
            LocalId lid;
            lid.name = chosenVar;
            lid.span = calleeFn.name.span;
            LValue lv;
            lv.base = lid;
            lv.span = calleeFn.name.span;
            RValueAtom rva;
            rva.rval = lv;
            rva.span = calleeFn.name.span;
            Atom a;
            a.v = rva;
            a.span = calleeFn.name.span;
            Expr e;
            e.first = std::move(a);
            e.span = calleeFn.name.span;
            argExpr = std::move(e);
          }
        } else {
          if (paramType.size() >= 2 && paramType[0] == 'i') {
            int bits = std::atoi(paramType.c_str() + 1);
            if (bits <= 0)
              bits = 32;

            IntLit lit;
            lit.value = randInt(rng) % (1LL << std::min(bits, 30));
            Coef coef = lit;
            CoefAtom ca;
            ca.coef = coef;
            Atom a;
            a.v = ca;
            Expr e;
            e.first = std::move(a);
            argExpr = std::move(e);
          } else if (paramType == "f32" || paramType == "f64") {
            FloatLit lit;
            lit.value = randFloat(rng);
            Coef coef = lit;
            CoefAtom ca;
            ca.coef = coef;
            Atom a;
            a.v = ca;
            Expr e;
            e.first = std::move(a);
            argExpr = std::move(e);
          } else if (paramType.rfind("ptr", 0) == 0) {
            NullLit lit;
            Coef coef = lit;
            CoefAtom ca;
            ca.coef = coef;
            Atom a;
            a.v = ca;
            Expr e;
            e.first = std::move(a);
            argExpr = std::move(e);
          }
        }
      }

      if (!argExpr) {
        argExpr = makeScalarLitExpr(paramType, paramValStr);
      }
      if (!argExpr) {
        return std::nullopt;
      }
      args.push_back(std::make_shared<Expr>(std::move(*argExpr)));
    }
    return args;
  }

  // ---------------------------------------------------------------------------
  // insertCallInUnexecBlock
  //
  // Attempts to realize a call edge in a block of the caller.
  // Rather than prepending dummy let-assignments, this method uses CallReplacer
  // to substitute an existing variable or type-safe literal inside the block
  // with the call. Returns true on successful replacement; false if no match.
  // ---------------------------------------------------------------------------
  static bool insertCallInUnexecBlock(
      FunDecl &caller, const FunDecl &calleeFn, const FuncDescriptor &callee,
      const FuncDescriptor::Realization &rz, std::mt19937 &rng, std::size_t blockIdx, bool randomize
  ) {
    if (blockIdx >= caller.blocks.size())
      return false;

    auto argsOpt = makeCallArgs(caller, calleeFn, callee, rz, rng, randomize);
    if (!argsOpt)
      return false;

    CallAtom ca;
    GlobalId gid;
    gid.name = callee.name;
    ca.callee = gid;
    ca.args = std::move(*argsOpt);
    ca.span = calleeFn.name.span;

    CallReplacer replacer(calleeFn.retType, caller);

    // Collect all matching candidates in the block
    for (auto &instr: caller.blocks[blockIdx].instrs) {
      replacer.collect(instr);
    }
    replacer.collect(caller.blocks[blockIdx].term);

    // If we have candidates, pick one at random and replace it
    if (!replacer.candidates.empty()) {
      std::uniform_int_distribution<size_t> pick(0, replacer.candidates.size() - 1);
      Atom *chosen = replacer.candidates[pick(rng)];
      chosen->v = std::move(ca);
      return true;
    }

    return false;
  }

  RewriteResult RewriteEngine::rewriteEdge(
      FunDecl &caller, const FuncDescriptor &callerDesc, const FunDecl &calleeFn,
      const FuncDescriptor &callee, std::size_t fixedRealizationIdx, std::mt19937 &rng
  ) {
    RewriteResult res;

    // Collect (rule, site) pairs.
    struct Candidate {
      RewriteRule *rule;
      RewriteSite site;
    };

    std::vector<Candidate> cands;
    for (auto &rule: rules_) {
      auto sites = rule->findSites(caller);
      for (auto &s: sites) {
        // Skip sites already consumed by an earlier rewriteEdge call —
        // stacking calls on the same let-init produces left-to-right
        // chains like `f1() + f2() + ...` whose prefix sums can wrap
        // in unintended ways even though each individual rewrite is
        // BV-sound. See RewriteEngine class header for the full note.
        if (consumed_.count({&caller, s.letIdx}))
          continue;
        cands.push_back({rule.get(), s});
      }
    }
    res.sitesFound = static_cast<int>(cands.size());

    if (!cands.empty()) {
      std::shuffle(cands.begin(), cands.end(), rng);
      std::uniform_real_distribution<double> uni(0.0, 1.0);

      int attempts = 0;
      for (auto &c: cands) {
        if (attempts++ >= rylink::hp::kMaxAttemptsPerEdge)
          break;
        if (uni(rng) >= rylink::hp::kPRewrite)
          continue;
        if (!c.rule->matchCallee(c.site, callee, fixedRealizationIdx))
          continue;
        if (c.rule->apply(caller, callerDesc, c.site, callee, fixedRealizationIdx, rng)) {
          consumed_.insert({&caller, c.site.letIdx});
          ++res.sitesRewritten;
          break;
        }
      }
    }

    const auto &rz = callee.realizations[fixedRealizationIdx];

    // [v0.2.2] Target unexecuted blocks safely. The execution path (block labels)
    // is recorded in callerDesc.path. Any block in the caller not found in this
    // path is unexecuted under the solved model, making it safe to populate with
    // additional calls using randomized arguments. We collect all unexecuted blocks,
    // shuffle them, and attempt to realize the call edge in one at random.
    std::vector<std::size_t> unexecutedIndices;
    std::unordered_set<std::string> pathSet(callerDesc.path.begin(), callerDesc.path.end());
    for (std::size_t i = 0; i < caller.blocks.size(); ++i) {
      if (pathSet.find(caller.blocks[i].label.name) == pathSet.end()) {
        unexecutedIndices.push_back(i);
      }
    }

    std::shuffle(unexecutedIndices.begin(), unexecutedIndices.end(), rng);
    for (std::size_t i: unexecutedIndices) {
      if (insertCallInUnexecBlock(caller, calleeFn, callee, rz, rng, i, true)) {
        ++res.sitesRewritten;
        break;
      }
    }

    return res;
  }

} // namespace symir::reify
