#pragma once

#include <list>
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
      enum class Kind { Int, Float, Array, Struct, Undef, Ptr, Vec } kind = Kind::Undef;
      std::int64_t intVal = 0;
      double floatVal = 0.0;
      std::uint32_t bits = 64;    // bitwidth for Int or Float (32/64)
      std::uint64_t ptrVal = 0;   // for Ptr kind: raw address
      std::uint64_t ptrBase = 0;  // for Ptr kind: base address of provenance object
      std::uint64_t elemSize = 1; // for Ptr kind: static element size of the pointee type
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
      // [v0.2.1] For array-of-struct field cells: the element index of
      // the containing struct (i.e. `%arr[k].fld`'s k). -1 / SIZE_MAX
      // when not array-nested. Used by StoreInstr to mirror the heap
      // write back into the right `store["%arr"].arrayVal[k]` cell.
      std::uint64_t arrayIdx = static_cast<std::uint64_t>(-1);
      std::uint64_t provId = 0; // unique provenance object ID
      TypePtr type = nullptr;   // [v0.2.1] The static type of the object/field

      ObjectInfo() = default;

      ObjectInfo(
          std::string vn, std::string fn, std::uint64_t b, std::uint64_t e, std::uint64_t es,
          std::uint64_t c, std::uint64_t ai = -1, std::uint64_t pi = 0, TypePtr t = nullptr
      ) :
          varName(vn), fieldName(fn), base(b), end(e), elemSize(es), count(c), arrayIdx(ai),
          provId(pi), type(t) {}
    };

    // heap_: flat address → RuntimeValue (one slot per element)
    std::unordered_map<std::uint64_t, RuntimeValue> heap_;
    // objects_: per-function allocation tracking
    std::list<ObjectInfo> objects_;
    // addrMap_: varName → base address (assigned lazily on first addr)
    std::unordered_map<std::string, std::uint64_t> addrMap_;
    // typeMap_: varName → TypePtr, rebuilt at start of each execFunction call
    std::unordered_map<std::string, TypePtr> typeMap_;
    // nextAddr_: allocator counter (starts at 4096 to leave null = 0 at bottom)
    std::uint64_t nextAddr_ = 4096;
    // nextProvId_: unique provenance ID counter
    std::uint64_t nextProvId_ = 1;

    std::uint64_t sizeofType(const TypePtr &t) const;
    TypePtr getCellTypeAtOffset(TypePtr t, std::uint64_t offset) const;
    TypePtr getLValueType(const LValue &lv) const;
    std::uint64_t allocObject(const std::string &varName, const TypePtr &t, const Store &store);
    std::uint64_t fieldOffset(const StructDecl &s, const std::string &fieldName) const;
    std::uint64_t
    materializeStruct(const std::string &varName, const StructDecl &s, const Store &store);
    ObjectInfo &addObject(ObjectInfo obj);
    const ObjectInfo *findObject(std::uint64_t addr) const;
    const ObjectInfo *findObjectByProvId(std::uint64_t provId) const;
    const ObjectInfo *findObjectByBaseAddress(std::uint64_t base) const;
    const ObjectInfo *findFieldOrStructObject(std::uint64_t addr, const TypePtr &type) const;
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
    // [v0.2.2] Execute a built-in intrinsic. Argument values are already
    // evaluated. Result has the intrinsic's declared return bitwidth.
    RuntimeValue callIntrinsic(
        const IntrinsicDecl &intr, const std::vector<RuntimeValue> &args, SourceSpan callSpan
    );
    RuntimeValue evalCoef(const Coef &c, const Store &store);
    RuntimeValue evalSelectVal(const SelectVal &sv, const Store &store);
    RuntimeValue evalLValue(const LValue &lv, const Store &store);
    void setLValue(const LValue &lv, RuntimeValue val, Store &store);

    bool evalCond(const Cond &c, const Store &store);
  };

} // namespace symir
