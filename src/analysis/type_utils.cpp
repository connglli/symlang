#include "analysis/type_utils.hpp"

namespace symir {

  std::optional<std::uint32_t> TypeUtils::getBitWidth(const TypePtr &t) {
    if (!t)
      return std::nullopt;
    if (auto it = std::get_if<IntType>(&t->v)) {
      switch (it->kind) {
        case IntType::Kind::I32:
          return 32;
        case IntType::Kind::I64:
          return 64;
        case IntType::Kind::ICustom:
          return it->bits.value_or(0);
      }
    }
    return std::nullopt;
  }

  bool TypeUtils::areTypesEqual(const TypePtr &a, const TypePtr &b) {
    if (a.get() == b.get())
      return true;
    if (!a || !b)
      return false;
    if (a->v.index() != b->v.index())
      return false;

    if (auto ia = std::get_if<IntType>(&a->v)) {
      auto ib = std::get_if<IntType>(&b->v);
      if (ia->kind != ib->kind)
        return false;
      if (ia->kind == IntType::Kind::ICustom)
        return ia->bits == ib->bits;
      return true;
    }
    if (auto sa = std::get_if<StructType>(&a->v)) {
      return sa->name.name == std::get<StructType>(b->v).name.name;
    }
    if (auto aa = std::get_if<ArrayType>(&a->v)) {
      auto ab = std::get_if<ArrayType>(&b->v);
      return aa->size == ab->size && areTypesEqual(aa->elem, ab->elem);
    }
    return false;
  }

  const ArrayType *TypeUtils::asArray(const TypePtr &t) {
    return t ? std::get_if<ArrayType>(&t->v) : nullptr;
  }

  const StructType *TypeUtils::asStruct(const TypePtr &t) {
    return t ? std::get_if<StructType>(&t->v) : nullptr;
  }

  bool TypeUtils::isArray(const TypePtr &t) { return t && std::holds_alternative<ArrayType>(t->v); }

  bool TypeUtils::isStruct(const TypePtr &t) {
    return t && std::holds_alternative<StructType>(t->v);
  }

} // namespace symir
