#pragma once

#include <functional>
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
      enum class Kind { Int, Array, Struct, Undef } kind = Kind::Undef;
      smt::Term term;       // For scalar Int (the BV value)
      smt::Term is_defined; // Boolean term: true if value is defined
      std::vector<SymbolicValue> arrayVal;
      std::unordered_map<std::string, SymbolicValue> structVal;

      SymbolicValue() = default;

      SymbolicValue(Kind k) : kind(k) {}

      SymbolicValue(Kind k, smt::Term t, smt::Term d) : kind(k), term(t), is_defined(d) {}

      SymbolicValue(const SymbolicValue &other) = default;
      SymbolicValue &operator=(const SymbolicValue &other) = default;
      SymbolicValue(SymbolicValue &&) = default;
      SymbolicValue &operator=(SymbolicValue &&) = default;
    };

    using SymbolicStore = std::unordered_map<std::string, SymbolicValue>;

    // --- Symbolic evaluation helpers ---
    SymbolicValue
    mergeAggregate(const std::vector<SymbolicValue> &elements, smt::Term idx, smt::ISolver &solver);

    smt::Term evalExpr(
        const Expr &e, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::optional<smt::Sort> expectedSort = std::nullopt
    );
    smt::Term evalAtom(
        const Atom &a, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::optional<smt::Sort> expectedSort = std::nullopt
    );
    smt::Term evalCoef(
        const Coef &c, smt::ISolver &solver, SymbolicStore &store,
        std::optional<smt::Sort> expectedSort = std::nullopt
    );
    smt::Term evalSelectVal(
        const SelectVal &sv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::optional<smt::Sort> expectedSort = std::nullopt
    );

    SymbolicValue evalLValue(
        const LValue &lv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc
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
    SymbolicValue
    evalInit(const InitVal &iv, const TypePtr &t, smt::ISolver &solver, SymbolicStore &store);

    std::unordered_map<std::string, const StructDecl *> structs_;
  };

} // namespace symir
