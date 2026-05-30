#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include "ast/ast.hpp"
#include "solver/solver.hpp"

namespace symir {

  class SIRPrinter {
  public:
    explicit SIRPrinter(std::ostream &out) : out_(out) {}

    explicit SIRPrinter(
        std::ostream &out,
        const std::unordered_map<std::string, SymbolicExecutor::Result::ModelVal> &model
    ) : out_(out), model_(model) {}

    explicit SIRPrinter(
        std::ostream &out,
        const std::unordered_map<std::string, SymbolicExecutor::Result::ModelVal> &model,
        const std::unordered_map<std::string, std::vector<SymbolicExecutor::Result::ModelVal>>
            &vecModel
    ) : out_(out), model_(model), vecModel_(vecModel) {}

    void print(const Program &p);

    // [v0.2.2] Publicly exposed so tools (e.g. rysmith's func_desc
    // emitter, future rylink) can serialize a TypePtr back to its
    // canonical SIR surface syntax without rolling a private printer.
    void printType(const TypePtr &t);

    // Convenience: render a TypePtr to its canonical SIR surface
    // string without spinning up an SIRPrinter at the call site.
    static std::string typeToString(const TypePtr &t);

  private:
    std::ostream &out_;
    std::unordered_map<std::string, SymbolicExecutor::Result::ModelVal> model_;
    // [v0.2.1] Per-lane concrete values for vector syms produced by the
    // solver. References to a vec sym `%?v` are rewritten to a synthetic
    // local `%v__solved` whose init list carries these lane values.
    std::unordered_map<std::string, std::vector<SymbolicExecutor::Result::ModelVal>> vecModel_;
    int indent_level_ = 0;

    // Translate `%?v` / `@?v` to a corresponding local identifier suitable
    // for substituting concrete vector-sym references. Result starts with
    // `%` (locals are function-scoped, which is the only scope vec syms
    // live in for now).
    std::string vecSymLocalName(const std::string &symName) const;

    void indent();
    void printExpr(const Expr &e);
    void printAtom(const Atom &a);
    void printCond(const Cond &c);
    void printLValue(const LValue &lv);
    void printCoef(const Coef &c);
    void printSelectVal(const SelectVal &sv);
    void printIndex(const Index &idx);
    void printInitVal(const InitVal &iv);
    void printDomain(const Domain &d);

    std::string relOpToString(RelOp op);
    std::string atomOpToString(AtomOpKind op);
  };

} // namespace symir
