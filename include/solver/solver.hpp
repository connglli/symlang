#pragma once

#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "ast/ast.hpp"
#include "solver/smt.hpp"

namespace symir {

  /**
   * Performs path-based symbolic execution on the SymIR program.
   * Generates SMT constraints for a selected path and uses an SMT solver
   * to find concrete values for symbolic variables that satisfy the path.
   */
  class SymbolicExecutor {
  public:
    struct Config {
      uint32_t timeout_ms = 0;
      uint32_t seed = 0;
      uint32_t num_threads = 1;
      uint32_t num_smt_threads = 1; // Number of threads for the SMT solver backend
    };

    using SolverFactory = std::function<std::unique_ptr<smt::ISolver>(const Config &)>;

    explicit SymbolicExecutor(
        const Program &prog, const Config &config, SolverFactory solverFactory
    );

    /**
     * Represents the result of symbolic execution/solving.
     */
    struct Result {
      bool sat = false;
      bool unsat = false;
      bool unknown = false;
      std::string message;
      using ModelVal = std::variant<int64_t, double>;
      std::unordered_map<std::string, ModelVal> model;
      // [v0.2.1] Per-lane model for vector syms. Populated only for syms
      // of vector type. Each entry holds N lane values matching the sym's
      // declared `<N> T` shape. Floats stored as bit-exact int64 too (so
      // a single map type fits both `<N> iM` and `<N> fM` cases —
      // consumers reinterpret as needed).
      std::unordered_map<std::string, std::vector<ModelVal>> vecModel;
      // [v0.2.2] Solved values for the entry function's parameters
      // (treated as symbols by the solver but printed verbatim in the
      // output). Empty when the entry has no parameters.
      std::unordered_map<std::string, ModelVal> paramModel;
      // [v0.2.2] Solved value for the entry function's `ret`
      // expression on the chosen path. `has_value() == false` when the
      // path's terminator is not a `ret <expr>;`.
      std::optional<ModelVal> retModel;
    };

    /**
     * Solves for a specific path in a function.
     * @param funcName The function to analyze.
     * @param path A sequence of block labels representing the path to execute.
     * @param fixedSyms Optional mapping to fix certain symbols to concrete values.
     */
    Result solve(
        const std::string &funcName, const std::vector<std::string> &path,
        const std::unordered_map<std::string, int64_t> &fixedSyms = {}
    );

    /**
     * Samples N paths randomly and tries to solve each of them.
     * Stops and returns the first SAT result found.
     */
    Result sample(
        const std::string &funcName, uint32_t n, uint32_t maxPathLen, bool requireTerminal,
        const std::vector<std::string> &prefixPath = {},
        const std::unordered_map<std::string, int64_t> &fixedSyms = {}
    );

  private:
    const Program &prog_;
    Config config_;
    SolverFactory solverFactory_;

    /**
     * Represents a symbolic value during execution.
     * Maps to SMT terms or nested aggregate structures.
     */
    struct SymbolicValue {
      // [v0.2.1] Vec: N-lane tuple (held in arrayVal, same shape as Array).
      // Distinguished from Array so the solver can apply lane-wise UB
      // semantics and the C-backend-compatible 0/1 mask representation.
      enum class Kind { Int, Array, Struct, Undef, Vec } kind = Kind::Undef;
      smt::Term term;       // For scalar Int (the BV value)
      smt::Term is_defined; // Boolean term: true if value is defined
      std::vector<SymbolicValue> arrayVal;
      std::unordered_map<std::string, SymbolicValue> structVal;

      smt::Term prov_base; // [v0.2.1] Pointer provenance base tag (BV64)
      smt::Term prov_size; // [v0.2.1] Pointer provenance size in tag-units (BV64)

      SymbolicValue() = default;

      SymbolicValue(Kind k) : kind(k) {}

      SymbolicValue(Kind k, smt::Term t, smt::Term d) : kind(k), term(t), is_defined(d) {}

      SymbolicValue(Kind k, smt::Term t, smt::Term d, smt::Term pb, smt::Term ps) :
          kind(k), term(t), is_defined(d), prov_base(pb), prov_size(ps) {}

      SymbolicValue(const SymbolicValue &other) = default;
      SymbolicValue &operator=(const SymbolicValue &other) = default;
      SymbolicValue(SymbolicValue &&) = default;
      SymbolicValue &operator=(SymbolicValue &&) = default;
    };

    using SymbolicStore = std::unordered_map<std::string, SymbolicValue>;

    // --- Symbolic evaluation helpers ---
    SymbolicValue
    mergeAggregate(const std::vector<SymbolicValue> &elements, smt::Term idx, smt::ISolver &solver);

    // [v0.2.1] Evaluate an Expr whose value is a vector, returning a
    // SymbolicValue of Kind::Vec. Used by AssignInstr when the LHS is
    // vector-typed. Handles CoefAtom/RValueAtom (whole-vector reference),
    // OpAtom (per-lane scalar arith with coef-broadcast), UnaryAtom,
    // CmpAtom (vec→<N>i1), and mask-form SelectAtom.
    SymbolicValue evalVecExpr(
        const Expr &e, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc
    );

    // [v0.2.1] Per-atom evaluator for vector RHS. Pulled out so the
    // chain-application loop in evalVecExpr can lower each atom without
    // round-tripping through Expr (which is not copy-assignable).
    SymbolicValue evalVecExprAtom(
        const Atom &a, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc
    );

    SymbolicValue evalExpr(
        const Expr &e, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::optional<smt::Sort> expectedSort = std::nullopt
    );
    SymbolicValue evalAtom(
        const Atom &a, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::optional<smt::Sort> expectedSort = std::nullopt
    );
    smt::Term evalCoef(
        const Coef &c, smt::ISolver &solver, SymbolicStore &store,
        std::optional<smt::Sort> expectedSort = std::nullopt
    );
    SymbolicValue evalSelectVal(
        const SelectVal &sv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::optional<smt::Sort> expectedSort = std::nullopt
    );

    SymbolicValue evalLValue(
        const LValue &lv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        bool forWrite = false
    );
    void setLValue(
        const LValue &lv, const SymbolicValue &val, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc
    );

    SymbolicValue muxSymbolicValue(
        smt::Term cond, const SymbolicValue &t, const SymbolicValue &f, smt::ISolver &solver
    );

    SymbolicValue updateLValueRec(
        const SymbolicValue &cur, std::span<const Access> accesses, const SymbolicValue &val,
        smt::Term pathCond, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        int depth = 0
    );

    smt::Term
    evalCond(const Cond &c, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc);

    smt::Sort getSort(const TypePtr &t, smt::ISolver &solver);
    SymbolicValue createSymbolicValue(
        const TypePtr &t, const std::string &name, smt::ISolver &solver, bool isSymbol = false
    );

    SymbolicValue makeUndef(const TypePtr &t, smt::ISolver &solver);
    SymbolicValue broadcast(const TypePtr &t, smt::Term val, smt::ISolver &solver);
    SymbolicValue evalInit(
        const InitVal &iv, const TypePtr &t, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc
    );

    TypePtr resolveLValueType(const LValue &lv) const;
    std::string buildLValueKey(const LValue &lv) const;
    TypePtr resolveExprType(const Expr &e) const;
    TypePtr resolveAtomType(const Atom &a) const;
    TypePtr resolveSelectValType(const SelectVal &sv) const;

    std::unordered_map<std::string, const StructDecl *> structs_;

    // Pointer dispatch helpers (v0.2.0):
    // Pointers are encoded as BV64 tags identifying their target local. The
    // current FunDecl is held per-solve via thread_local storage to support
    // load/store dispatch over candidate targets, while keeping sample() safe
    // for concurrent workers.
    static thread_local const FunDecl *currentFun_;

    // [v0.2.1] Provenance tracking for pointer locals (rule 14, 19).
    // Maps a `ptr T` local name to its provenance base tag and the
    // size (in BV64 tag units) of the addressable region:
    //   - scalar pointee:  size = 1
    //   - array pointee:   size = N
    //   - struct pointee:  size = fields.size()
    // Updated on `%p = addr %x` / `ptrindex` / `ptrfield`. Pointer
    // arithmetic preserves the provenance. Load-derived pointers
    // have no entry (their provenance is unknown).
    //
    // Per-solve state — reset at the start of each `solve()`.
    struct PtrProvenance {
      std::uint64_t baseTag;
      std::uint64_t size;
    };

    std::unordered_map<std::string, PtrProvenance> ptrProv_;

    // [v0.2.2] §9.6.1 shared sym cache: a `sym` declared in a `fun`
    // body becomes one solver constant reused across every call to
    // that function on the path. Keyed by FunDecl*.
    std::unordered_map<const FunDecl *, std::unordered_map<std::string, SymbolicValue>> calleeSyms_;

    // [v0.2.2] Symbolic interprocedural call. Evaluates the callee
    // body on its straight-line CFG path and returns the symbolic
    // value of the ret expression. Caller-side store stays unchanged
    // for non-pointer locals; pointer-argument havoc (full §9.6.1
    // step 5) is reserved for Phase 8's contract-form path.
    SymbolicValue callFunction(
        const FunDecl &callee, std::vector<SymbolicValue> args, smt::ISolver &solver,
        std::vector<smt::Term> &pc
    );

    // [v0.2.2 Phase 8] §9.6.2 contract-form decl expansion. `pre`
    // clauses join PC; a fresh ret_sym stands for the return value;
    // `post` clauses are assumed by joining PC. Memory havoc
    // (§9.6.2.4) is the next refinement and is currently a TODO --
    // pointer arguments are not yet havoc'd, so contracts that talk
    // about post-state pointee memory are sound only when the caller
    // has already constrained it.
    SymbolicValue callContract(
        const ExtDecl &decl, const std::vector<std::shared_ptr<Expr>> &argExprs,
        std::vector<SymbolicValue> args, smt::ISolver &solver, SymbolicStore &callerStore,
        std::vector<smt::Term> &pc
    );

    // [v0.2.2] Currently active requirements vector (so nested callees
    // can push REQ terms). Set by solve() and consumed by callFunction.
    std::vector<smt::Term> *currentReq_ = nullptr;


    // [v0.2.2] Per-solve RNG used by callFunction to pick a branch when
    // a callee has a non-straight CFG. Seeded from config_.seed at the
    // start of solve(). A future sub-path syntax (see SPEC §13) will
    // replace this random sampling with a user-supplied callee path.
    std::mt19937 calleeRng_;
  };

} // namespace symir
