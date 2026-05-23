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
    using SymBindings = std::unordered_map<std::string, std::variant<std::int64_t, double>>;

    void
    run(const std::string &entryFuncName, const SymBindings &symBindings, bool dumpExec = false);

  private:
    const Program &prog_;
    bool dumpExec_ = false;
    std::unordered_map<std::string, const StructDecl *> structs_;

    /**
     * Represents a value during runtime.
     */
    struct RuntimeValue {
      enum class Kind { Int, Float, Array, Struct, Undef, Ptr, Vec } kind;
      std::int64_t intVal = 0;
      double floatVal = 0.0;
      std::uint32_t bits = 64;   // bitwidth for Int or Float (32/64)
      std::uint64_t ptrVal = 0;  // for Ptr kind: raw address
      std::uint64_t ptrBase = 0; // for Ptr kind: base address of provenance object
      std::vector<RuntimeValue> arrayVal;
      std::unordered_map<std::string, RuntimeValue> structVal;
      // [v0.2.1] Vec: same shape as Array (per-lane RuntimeValue tuple),
      // but represents a vector value (no address; not in heap_; lane-wise
      // arithmetic). Element kind matches the lane scalar type.
    };

    using Store = std::unordered_map<std::string, RuntimeValue>;

    // ---- Memory model for pointer operations ----

    /// Per-object provenance: tracks base address, size, element size.
    struct ObjectInfo {
      std::string varName;    // originating local variable name
      std::string fieldName;  // non-empty for struct-field objects (addr lv.f)
      std::uint64_t base;     // base address (never 0)
      std::uint64_t end;      // base + totalSize (exclusive)
      std::uint64_t elemSize; // sizeof(element type) in bytes
      std::uint64_t count;    // number of elements
    };

    // heap_: flat address → RuntimeValue (one slot per element)
    std::unordered_map<std::uint64_t, RuntimeValue> heap_;
    // objects_: per-function allocation tracking
    std::vector<ObjectInfo> objects_;
    // addrMap_: varName → base address (assigned lazily on first addr)
    std::unordered_map<std::string, std::uint64_t> addrMap_;
    // typeMap_: varName → TypePtr, rebuilt at start of each execFunction call
    std::unordered_map<std::string, TypePtr> typeMap_;
    // nextAddr_: allocator counter (starts at 4096 to leave null = 0 at bottom)
    std::uint64_t nextAddr_ = 4096;

    std::uint64_t sizeofType(const TypePtr &t) const;
    std::uint64_t allocObject(const std::string &varName, const TypePtr &t, const Store &store);
    std::uint64_t fieldOffset(const StructDecl &s, const std::string &fieldName) const;
    std::uint64_t
    materializeStruct(const std::string &varName, const StructDecl &s, const Store &store);
    const ObjectInfo *findObject(std::uint64_t addr) const;
    const ObjectInfo *findObjectByBase(std::uint64_t base) const;
    const ObjectInfo *findObjectForArith(std::uint64_t addr) const;

    // --- Runtime evaluation helpers ---
    RuntimeValue makeUndef(const TypePtr &t);
    RuntimeValue broadcast(const TypePtr &t, const RuntimeValue &v);
    RuntimeValue evalInit(const InitVal &iv, const TypePtr &t, const Store &store);
    std::string rvToString(const RuntimeValue &rv) const;

    void execFunction(
        const FunDecl &f, const std::vector<RuntimeValue> &args, const SymBindings &symBindings
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
