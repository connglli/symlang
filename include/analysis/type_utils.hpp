#pragma once

#include <cstdint>
#include <optional>
#include "ast/ast.hpp"

namespace symir {

  struct TypeUtils {
    /**
     * Returns the bitwidth of the given type if it is an integer type.
     * Returns std::nullopt otherwise.
     */
    static std::optional<std::uint32_t> getBitWidth(const TypePtr &t);

    /**
     * Checks if two types are structurally equal.
     */
    static bool areTypesEqual(const TypePtr &a, const TypePtr &b);

    /**
     * Casts to ArrayType if possible, otherwise returns nullptr.
     */
    static const ArrayType *asArray(const TypePtr &t);

    /**
     * Casts to StructType if possible, otherwise returns nullptr.
     */
    static const StructType *asStruct(const TypePtr &t);

    /**
     * Returns true if the type is an array type.
     */
    static bool isArray(const TypePtr &t);

    /**
     * Returns true if the type is a struct type.
     */
    static bool isStruct(const TypePtr &t);
  };

} // namespace symir
