#pragma once

#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "ast/ast.hpp"
#include "bitwuzla/cpp/bitwuzla.h"

namespace symir {

  /**
   * Performs path-based symbolic execution on the SymIR program.
   * Generates SMT constraints for a selected path and uses Bitwuzla
   * to find concrete values for symbolic variables that satisfy the path.
   */
  class SymbolicExecutor {
  public:
    struct Config {
      uint32_t timeout_ms = 0;
      uint32_t seed = 0;
    };

    explicit SymbolicExecutor(const Program &prog, const Config &config);

    /**
     * Represents the result of symbolic execution/solving.
     */
    struct Result {
      bool sat = false;
      bool unsat = false;
      bool unknown = false;
      std::string message;
      std::unordered_map<std::string, int64_t> model;
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

  private:
    const Program &prog_;
    Config config_;

    /**
     * Represents a symbolic value during execution.
     * Maps to SMT terms or nested aggregate structures.
     */
    struct SymbolicValue {
      enum class Kind { Int, Array, Struct, Undef } kind = Kind::Undef;
      bitwuzla::Term term;       // For scalar Int (the BV value)
      bitwuzla::Term is_defined; // Boolean term: true if value is defined
      std::vector<SymbolicValue> arrayVal;
      std::unordered_map<std::string, SymbolicValue> structVal;

      SymbolicValue() = default;

      SymbolicValue(Kind k) : kind(k) {}

      SymbolicValue(Kind k, bitwuzla::Term t, bitwuzla::Term d) : kind(k), term(t), is_defined(d) {}

      SymbolicValue(const SymbolicValue &other) = default;
      SymbolicValue &operator=(const SymbolicValue &other) = default;
      SymbolicValue(SymbolicValue &&) = default;
      SymbolicValue &operator=(SymbolicValue &&) = default;
    };

    using SymbolicStore = std::unordered_map<std::string, SymbolicValue>;

    // --- Symbolic evaluation helpers ---
    SymbolicValue mergeAggregate(
        const std::vector<SymbolicValue> &elements, bitwuzla::Term idx, bitwuzla::TermManager &tm
    );

    bitwuzla::Term evalExpr(
        const Expr &e, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store,
        std::vector<bitwuzla::Term> &pc, std::optional<bitwuzla::Sort> expectedSort = std::nullopt
    );
    bitwuzla::Term evalAtom(
        const Atom &a, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store,
        std::vector<bitwuzla::Term> &pc, std::optional<bitwuzla::Sort> expectedSort = std::nullopt
    );
    bitwuzla::Term evalCoef(
        const Coef &c, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store,
        std::optional<bitwuzla::Sort> expectedSort = std::nullopt
    );
    bitwuzla::Term evalSelectVal(
        const SelectVal &sv, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver,
        SymbolicStore &store, std::vector<bitwuzla::Term> &pc,
        std::optional<bitwuzla::Sort> expectedSort = std::nullopt
    );

    SymbolicValue evalLValue(
        const LValue &lv, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver,
        SymbolicStore &store, std::vector<bitwuzla::Term> &pc
    );
    void setLValue(
        const LValue &lv, const SymbolicValue &val, bitwuzla::TermManager &tm,
        bitwuzla::Bitwuzla &solver, SymbolicStore &store, std::vector<bitwuzla::Term> &pc
    );

    SymbolicValue muxSymbolicValue(
        bitwuzla::Term cond, const SymbolicValue &t, const SymbolicValue &f,
        bitwuzla::TermManager &tm
    );

    SymbolicValue updateLValueRec(
        const SymbolicValue &cur, std::span<const Access> accesses, const SymbolicValue &val,
        bitwuzla::Term pathCond, bitwuzla::TermManager &tm, SymbolicStore &store,
        std::vector<bitwuzla::Term> &pc, int depth = 0
    );

    bitwuzla::Term evalCond(
        const Cond &c, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store,
        std::vector<bitwuzla::Term> &pc
    );

    bitwuzla::Sort getSort(const TypePtr &t, bitwuzla::TermManager &tm);
    SymbolicValue createSymbolicValue(
        const TypePtr &t, const std::string &name, bitwuzla::TermManager &tm, bool isSymbol = false
    );

    SymbolicValue makeUndef(const TypePtr &t, bitwuzla::TermManager &tm);
    SymbolicValue broadcast(const TypePtr &t, bitwuzla::Term val, bitwuzla::TermManager &tm);
    SymbolicValue
    evalInit(const InitVal &iv, const TypePtr &t, bitwuzla::TermManager &tm, SymbolicStore &store);

    std::unordered_map<std::string, const StructDecl *> structs_;
  };

} // namespace symir
