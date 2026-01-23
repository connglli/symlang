#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include "ast/ast.hpp"

namespace symir {

  class Interpreter {
  public:
    explicit Interpreter(const Program &prog);
    void
    run(const std::string &entryFuncName,
        const std::unordered_map<std::string, std::int64_t> &symBindings);

  private:
    const Program &prog_;
    std::unordered_map<std::string, const StructDecl *> structs_;

    struct RuntimeValue {
      enum class Kind { Int, Array, Struct, Undef } kind;
      std::int64_t intVal = 0;
      std::vector<RuntimeValue> arrayVal;
      std::unordered_map<std::string, RuntimeValue> structVal;
    };

    using Store = std::unordered_map<std::string, RuntimeValue>;

    RuntimeValue makeUndef(const TypePtr &t);
    RuntimeValue broadcast(const TypePtr &t, const RuntimeValue &v);
    RuntimeValue evalInit(const InitVal &iv, const TypePtr &t, const Store &store);

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
