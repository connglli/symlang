#include "reify/type_gen.hpp"

#include <cassert>
#include <stdexcept>
#include "reify/hyperparameters.hpp"

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Factory helpers
  // ---------------------------------------------------------------------------

  static TypePtr makeIntTypeOfBits(uint32_t bits) {
    IntType t;
    switch (bits) {
      case 8:
        t.kind = IntType::Kind::ICustom;
        t.bits = 8;
        break;
      case 16:
        t.kind = IntType::Kind::ICustom;
        t.bits = 16;
        break;
      case 32:
        t.kind = IntType::Kind::I32;
        break;
      case 64:
        t.kind = IntType::Kind::I64;
        break;
      default:
        t.kind = IntType::Kind::ICustom;
        t.bits = (int) bits;
        break;
    }
    return std::make_shared<Type>(Type{t, {}});
  }

  // ---------------------------------------------------------------------------
  // Public helpers
  // ---------------------------------------------------------------------------

  bool isIntType(const TypePtr &t) { return t && std::holds_alternative<IntType>(t->v); }

  bool isFpType(const TypePtr &t) { return t && std::holds_alternative<FloatType>(t->v); }

  bool isPtrType(const TypePtr &t) { return t && std::holds_alternative<PtrType>(t->v); }

  bool isAggType(const TypePtr &t) {
    if (!t)
      return false;
    return std::holds_alternative<ArrayType>(t->v) || std::holds_alternative<StructType>(t->v);
  }

  bool isScalarType(const TypePtr &t) { return isIntType(t) || isFpType(t); }

  uint32_t intBitWidth(const TypePtr &t) {
    assert(isIntType(t));
    const auto &it = std::get<IntType>(t->v);
    switch (it.kind) {
      case IntType::Kind::I32:
        return 32;
      case IntType::Kind::I64:
        return 64;
      case IntType::Kind::ICustom:
        return (uint32_t) *it.bits;
    }
    return 32;
  }

  TypePtr pointeeType(const TypePtr &t) {
    assert(isPtrType(t));
    return std::get<PtrType>(t->v).pointee;
  }

  bool typeEquals(const TypePtr &a, const TypePtr &b) {
    if (!a || !b)
      return a == b;
    // Compare by index first
    if (a->v.index() != b->v.index())
      return false;

    if (isIntType(a)) {
      return intBitWidth(a) == intBitWidth(b);
    }
    if (isFpType(a)) {
      auto fa = std::get<FloatType>(a->v).kind;
      auto fb = std::get<FloatType>(b->v).kind;
      return fa == fb;
    }
    if (isPtrType(a)) {
      return typeEquals(std::get<PtrType>(a->v).pointee, std::get<PtrType>(b->v).pointee);
    }
    if (std::holds_alternative<ArrayType>(a->v)) {
      const auto &aa = std::get<ArrayType>(a->v);
      const auto &ab = std::get<ArrayType>(b->v);
      return aa.size == ab.size && typeEquals(aa.elem, ab.elem);
    }
    if (std::holds_alternative<StructType>(a->v)) {
      return std::get<StructType>(a->v).name.name == std::get<StructType>(b->v).name.name;
    }
    if (std::holds_alternative<VecType>(a->v)) {
      const auto &va = std::get<VecType>(a->v);
      const auto &vb = std::get<VecType>(b->v);
      return va.size == vb.size && typeEquals(va.elem, vb.elem);
    }
    return false;
  }

  // ---------------------------------------------------------------------------
  // Generators
  // ---------------------------------------------------------------------------

  TypePtr genIntType(std::mt19937 &rng) {
    static const uint32_t widths[] = {8, 16, 32, 64};
    std::uniform_int_distribution<int> d(0, 3);
    return makeIntTypeOfBits(widths[d(rng)]);
  }

  TypePtr genScalarType(std::mt19937 &rng, bool enableFp) {
    // Integer types: i8, i16, i32, i64 — equal probability (each 1 slot)
    // Float types (f32, f64 combined) get the same probability as ONE integer type
    // Total slots: 5 if fp enabled (4 int + 1 fp bucket), 4 if not
    std::uniform_int_distribution<int> d(0, enableFp ? 4 : 3);
    int pick = d(rng);
    if (pick < 4) {
      static const uint32_t widths[] = {8, 16, 32, 64};
      return makeIntTypeOfBits(widths[pick]);
    }
    // Float bucket: pick f32 or f64
    std::uniform_int_distribution<int> fpick(0, 1);
    FloatType ft;
    ft.kind = fpick(rng) ? FloatType::Kind::F64 : FloatType::Kind::F32;
    return std::make_shared<Type>(Type{ft, {}});
  }

  TypePtr genVecType(std::mt19937 &rng, bool enableFp) {
    static const uint64_t lanes[] = {2, 4, 8};
    std::uniform_int_distribution<int> ld(0, 2);
    uint64_t N = lanes[ld(rng)];
    TypePtr elem = genScalarType(rng, enableFp);
    VecType vt;
    vt.size = N;
    vt.elem = elem;
    return std::make_shared<Type>(Type{vt, {}});
  }

  bool isVecType(const TypePtr &t) { return t && std::holds_alternative<VecType>(t->v); }

  TypePtr genRandomType(std::mt19937 &rng, const TypeGenConfig &cfg, int depth) {
    // Type-kind probability buckets — see rysmith::hp::kPType*. Aggregates are zeroed
    // past maxAggNesting and pointers past maxPtrDepth, then renormalized.
    double pScalar = rysmith::hp::kPTypeScalar;
    double pArray = (depth >= cfg.maxAggNesting) ? 0.0 : rysmith::hp::kPTypeArray;
    double pStruct = (depth >= cfg.maxAggNesting) ? 0.0 : rysmith::hp::kPTypeStruct;
    double pPtr = (depth >= cfg.maxPtrDepth) ? 0.0 : rysmith::hp::kPTypePtr;
    // [v0.2.1] Vectors only at depth 0 (no nested vec, no vec in arrays/structs).
    double pVec = (depth == 0 && cfg.enableVec) ? rysmith::hp::kPTypeVec : 0.0;

    // Renormalize
    double total = pScalar + pArray + pStruct + pPtr + pVec;
    if (total <= 0.0) {
      return genScalarType(rng, cfg.enableFp);
    }

    std::uniform_real_distribution<double> prob(0.0, total);
    double r = prob(rng);

    if (r < pScalar) {
      return genScalarType(rng, cfg.enableFp);
    }
    r -= pScalar;
    if (r < pArray) {
      // Array: [N] elem — elem is recursively generated with depth+1
      std::uniform_int_distribution<int> szd(1, std::max(1, cfg.maxAggElems));
      uint64_t sz = (uint64_t) szd(rng);
      TypePtr elem = genRandomType(rng, cfg, depth + 1);
      ArrayType at;
      at.size = sz;
      at.elem = elem;
      return std::make_shared<Type>(Type{at, {}});
    }
    r -= pArray;
    if (r < pStruct) {
      StructType st;
      st.name = GlobalId{"@_pending_struct", {}};
      return std::make_shared<Type>(Type{st, {}});
    }
    r -= pStruct;
    if (r < pVec) {
      return genVecType(rng, cfg.enableFp);
    }
    r -= pVec;
    // Pointer
    TypePtr pointee = genRandomType(rng, cfg, depth + 1);
    // [v0.2.1] If enableAggPtr, allow aggregate pointees (ptr [N] T, ptr @S).
    // Otherwise fall back to scalar (v0.2.0 behavior).
    // Pointer to vector (ptr <N> T) is always forbidden (§6.8.1).
    if (isVecType(pointee)) {
      pointee = genScalarType(rng, cfg.enableFp);
    }
    if (!cfg.enableAggPtr && isAggType(pointee)) {
      pointee = genScalarType(rng, cfg.enableFp);
    }
    PtrType pt;
    pt.pointee = pointee;
    return std::make_shared<Type>(Type{pt, {}});
  }

} // namespace symir::reify
