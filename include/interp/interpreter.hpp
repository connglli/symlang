#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include "ast/ast.hpp"

namespace symir {

  /**
   * A concrete interpreter for the SymIR language.
   * Executes SymIR programs by evaluating expressions and instructions
   * against concrete values for symbolic variables.
   */
  class Interpreter {
  public:
    explicit Interpreter(const Program &prog);
    /**
     * Executes the specified entry function with given symbolic bindings.
     * @param entryFuncName The name of the function to start execution from.
     * @param symBindings Mapping of symbolic identifiers to concrete values.
     * @param dumpExec Whether to print execution trace to stderr.
     */
    void
    run(const std::string &entryFuncName,
        const std::unordered_map<std::string, std::int64_t> &symBindings, bool dumpExec = false);

  private:
    const Program &prog_;
    bool dumpExec_ = false;
    std::unordered_map<std::string, const StructDecl *> structs_;

    /**
     * Represents a value during runtime.
     */
    struct RuntimeValue {
      enum class Kind { Int, Array, Struct, Undef } kind;
      std::int64_t intVal = 0;
      std::uint32_t bits = 64; // bitwidth for Int
      std::vector<RuntimeValue> arrayVal;
      std::unordered_map<std::string, RuntimeValue> structVal;
    };

    using Store = std::unordered_map<std::string, RuntimeValue>;

    // --- Runtime evaluation helpers ---
    RuntimeValue makeUndef(const TypePtr &t);
    RuntimeValue broadcast(const TypePtr &t, const RuntimeValue &v);
    RuntimeValue evalInit(const InitVal &iv, const TypePtr &t, const Store &store);
    std::string rvToString(const RuntimeValue &rv) const;

    void execFunction(
        const FunDecl &f, const std::vector<RuntimeValue> &args,
        const std::unordered_map<std::string, std::int64_t> &symBindings
    );

    RuntimeValue evalExpr(const Expr &e, const Store &store);
    RuntimeValue evalAtom(const Atom &a, const Store &store);
    RuntimeValue evalCoef(const Coef &c, const Store &store);
    RuntimeValue evalSelectVal(const SelectVal &sv, const Store &store);
    RuntimeValue evalLValue(const LValue &lv, const Store &store);
    void setLValue(const LValue &lv, RuntimeValue val, Store &store);

    bool evalCond(const Cond &c, const Store &store);
  };

} // namespace symir
