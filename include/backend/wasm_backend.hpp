#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ast/ast.hpp"

namespace symir {

  /**
   * Generates WebAssembly Text Format (.wat) from a SymIR program.
   *
   * Note on Undefined Behavior: Unlike the C target (which leverages C's native
   * UB similarities and GCC sanitizers to catch and trap SymIR UBs at runtime),
   * the WASM backend does not instrument generated WAT with check logic to detect
   * or trap undefined behavior. In compliance with the compiler's semantic refinement
   * model, behavior is only guaranteed for UB-free inputs.
   */
  class WasmBackend {
  public:
    explicit WasmBackend(std::ostream &out) : out_(out) {}

    /**
     * Translates the entire program to WAT and writes it to the output stream.
     */
    void emit(const Program &prog);

    void setNoModuleTags(bool val) { noModuleTags_ = val; }

    void setNoRequire(bool val) { noRequire_ = val; }

  private:
    std::ostream &out_;
    int indent_level_ = 0;
    std::string curFuncName_;
    bool noModuleTags_ = false;
    bool noRequire_ = false;

    // Maps local/param names to their WASM local index or info
    struct LocalInfo {
      std::string wasmType;
      bool isParam = false;
      std::uint32_t bitwidth = 32;
      bool isAggregate = false;
      std::uint32_t offset = 0; // offset from stack pointer if aggregate
      TypePtr symirType;
    };

    std::unordered_map<std::string, LocalInfo> locals_;
    std::unordered_map<std::string, TypePtr> syms_;
    std::uint32_t stackSize_ = 0;

    struct FieldInfo {
      std::uint32_t offset;
      std::uint32_t size;
      TypePtr type;
    };

    struct StructInfo {
      std::uint32_t totalSize;
      std::unordered_map<std::string, FieldInfo> fields;
      std::vector<std::string> fieldNames;
    };

    std::unordered_map<std::string, StructInfo> structLayouts_;

    // --- Emission helpers ---
    void indent();
    void emitType(const TypePtr &type);
    std::string getWasmType(const TypePtr &type);
    std::uint32_t getTypeSize(const TypePtr &type);
    std::uint32_t getIntWidth(const TypePtr &type);
    void computeLayouts(const Program &prog);

    void emitExpr(const Expr &expr, std::uint32_t targetWidth, bool isFloat = false);
    // Emit a pointer-valued expression, scaling int offsets by pointee element size
    void emitPtrExpr(const Expr &expr, const TypePtr &ptrType);
    // Recognise `ptr - ptr` (i64 element distance per spec §6.8.6).
    bool isPtrDiff(const Expr &expr) const;
    void emitPtrDiff(const Expr &expr);
    void emitAtom(const Atom &atom, std::uint32_t targetWidth, bool isFloat = false);
    void emitCond(const Cond &cond);
    void emitLValue(const LValue &lv, bool isStore);
    void emitCoef(const Coef &coef, std::uint32_t targetWidth, bool isFloat = false);
    void emitSelectVal(const SelectVal &sv, std::uint32_t targetWidth, bool isFloat = false);
    void emitIndex(const Index &idx);
    void emitInitVal(const InitVal &iv, const TypePtr &type, std::uint32_t offset);
    void emitCopy(
        const TypePtr &type, std::uint32_t dstOffset, const std::string &srcName,
        std::uint32_t srcOffset
    );
    void emitAddress(const LValue &lv);

    TypePtr getLValueType(const LValue &lv);
    TypePtr getSelectValType(const SelectVal &sv);
    // TODO: Support native WebAssembly SIMD-128 (v128) lowering for vectors as planned
    // in SPEC v0.2.1 §10.16. Currently vectors are lowered by unrolling operations lane-by-lane.
    void emitVecExprLane(
        const Expr &expr, const VecType &vt, std::uint64_t lane, std::uint32_t targetWidth,
        bool isFloat
    );
    void emitVecAtomLane(
        const Atom &atom, const VecType &vt, std::uint64_t lane, std::uint32_t targetWidth,
        bool isFloat
    );
    void emitVecCoefLane(
        const Coef &coef, const VecType &vt, std::uint64_t lane, std::uint32_t targetWidth,
        bool isFloat
    );
    void emitVecLValueLane(
        const LValue &lv, const VecType &vt, std::uint64_t lane, std::uint32_t targetWidth,
        bool isFloat
    );
    void emitVecSelectValLane(
        const SelectVal &sv, const VecType &vt, std::uint64_t lane, std::uint32_t targetWidth,
        bool isFloat
    );

    // --- Naming and structure ---
    std::string mangleName(const std::string &name);
    std::string stripSigil(const std::string &name);
    std::string getMangledSymbolName(const std::string &funcName, const std::string &symName);

    void emitMask(std::uint32_t bitwidth, std::uint32_t wasmWidth);
    void emitSignExtend(std::uint32_t bitwidth, std::uint32_t wasmWidth);
  };

} // namespace symir
