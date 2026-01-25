#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include "ast/ast.hpp"

namespace symir {

  /**
   * Generates WebAssembly Text Format (.wat) from a SymIR program.
   */
  class WasmBackend {
  public:
    explicit WasmBackend(std::ostream &out) : out_(out) {}

    /**
     * Translates the entire program to WAT and writes it to the output stream.
     */
    void emit(const Program &prog);

  private:
    std::ostream &out_;
    int indent_level_ = 0;
    std::string curFuncName_;

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

    void emitExpr(const Expr &expr, std::uint32_t targetWidth);
    void emitAtom(const Atom &atom, std::uint32_t targetWidth);
    void emitCond(const Cond &cond);
    void emitLValue(const LValue &lv, bool isStore);
    void emitCoef(const Coef &coef, std::uint32_t targetWidth);
    void emitSelectVal(const SelectVal &sv, std::uint32_t targetWidth);
    void emitIndex(const Index &idx);
    void emitInitVal(const InitVal &iv, const TypePtr &type, std::uint32_t offset);
    void emitAddress(const LValue &lv);

    // --- Naming and structure ---
    std::string mangleName(const std::string &name);
    std::string stripSigil(const std::string &name);
    std::string getMangledSymbolName(const std::string &funcName, const std::string &symName);

    void emitMask(std::uint32_t bitwidth);
    void emitSignExtend(std::uint32_t fromWidth, std::uint32_t toWidth);
  };

} // namespace symir
