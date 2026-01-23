#pragma once

#include <bitwuzla/cpp/bitwuzla.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "ast/ast.hpp"

namespace symir {

  class SymbolicExecutor {
  public:
    struct Config {
      uint32_t timeout_ms = 0;
      uint32_t seed = 0;
    };

    explicit SymbolicExecutor(const Program &prog, const Config &config);

    struct Result {
      bool sat = false;
      bool unsat = false;
      bool unknown = false;
      std::string message;
      std::unordered_map<std::string, int64_t> model;
    };

    Result solve(
        const std::string &funcName, const std::vector<std::string> &path,
        const std::unordered_map<std::string, int64_t> &fixedSyms = {}
    );

  private:
    const Program &prog_;
    Config config_;

    struct SymbolicValue {
      enum class Kind { Int, Array, Struct, Undef } kind = Kind::Undef;
      bitwuzla::Term term; // For scalar Int
      std::vector<SymbolicValue> arrayVal;
      std::unordered_map<std::string, SymbolicValue> structVal;

      SymbolicValue() = default;

      SymbolicValue(Kind k) : kind(k) {}

      SymbolicValue(Kind k, bitwuzla::Term t) : kind(k), term(t) {}
    };

    using SymbolicStore = std::unordered_map<std::string, SymbolicValue>;

    SymbolicValue mergeAggregate(
        const std::vector<SymbolicValue> &elements, bitwuzla::Term idx, bitwuzla::TermManager &tm
    );

    bitwuzla::Term evalExpr(
        const Expr &e, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store,
        std::vector<bitwuzla::Term> &pc
    );
    bitwuzla::Term evalAtom(
        const Atom &a, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store,
        std::vector<bitwuzla::Term> &pc
    );
    bitwuzla::Term evalCoef(
        const Coef &c, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store
    );
    bitwuzla::Term evalSelectVal(
        const SelectVal &sv, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver,
        SymbolicStore &store, std::vector<bitwuzla::Term> &pc
    );

    SymbolicValue evalLValue(
        const LValue &lv, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver,
        SymbolicStore &store, std::vector<bitwuzla::Term> &pc
    );
    void setLValue(
        const LValue &lv, const SymbolicValue &val, bitwuzla::TermManager &tm,
        bitwuzla::Bitwuzla &solver, SymbolicStore &store, std::vector<bitwuzla::Term> &pc
    );

    bitwuzla::Term evalCond(
        const Cond &c, bitwuzla::TermManager &tm, bitwuzla::Bitwuzla &solver, SymbolicStore &store,
        std::vector<bitwuzla::Term> &pc
    );

    bitwuzla::Sort getSort(const TypePtr &t, bitwuzla::TermManager &tm);
    SymbolicValue createSymbolicValue(
        const TypePtr &t, const std::string &name, bitwuzla::TermManager &tm, bool isSymbol = false
    );

    // Helpers for recursive aggregate handling
    SymbolicValue makeUndef(const TypePtr &t, bitwuzla::TermManager &tm);
    SymbolicValue broadcast(const TypePtr &t, bitwuzla::Term val, bitwuzla::TermManager &tm);
    SymbolicValue
    evalInit(const InitVal &iv, const TypePtr &t, bitwuzla::TermManager &tm, SymbolicStore &store);

    std::unordered_map<std::string, const StructDecl *> structs_;
  };

} // namespace symir
