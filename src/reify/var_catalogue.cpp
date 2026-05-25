#include "reify/var_catalogue.hpp"

#include <algorithm>
#include <cassert>
#include "reify/hyperparameters.hpp"

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // VarCatalogue accessors
  // ---------------------------------------------------------------------------

  std::vector<const VarEntry *> VarCatalogue::scalarsOf(const TypePtr &t) const {
    std::vector<const VarEntry *> result;
    for (const auto &v: vars)
      if (isScalarType(v.type) && typeEquals(v.type, t))
        result.push_back(&v);
    return result;
  }

  std::vector<const VarEntry *> VarCatalogue::allScalars() const {
    std::vector<const VarEntry *> result;
    for (const auto &v: vars)
      if (isScalarType(v.type))
        result.push_back(&v);
    return result;
  }

  std::vector<const VarEntry *> VarCatalogue::ptrsOf(const TypePtr &pointeeT) const {
    std::vector<const VarEntry *> result;
    for (const auto &v: vars)
      if (isPtrType(v.type) && typeEquals(pointeeType(v.type), pointeeT))
        result.push_back(&v);
    return result;
  }

  std::vector<const VarEntry *> VarCatalogue::ptrsToPtr() const {
    std::vector<const VarEntry *> result;
    for (const auto &v: vars)
      if (isPtrType(v.type) && isPtrType(pointeeType(v.type)))
        result.push_back(&v);
    return result;
  }

  std::vector<const VarEntry *> VarCatalogue::addressable() const {
    std::vector<const VarEntry *> result;
    for (const auto &v: vars)
      if (!isPtrType(v.type) && !isAggType(v.type))
        result.push_back(&v);
    return result;
  }

  std::vector<const VarEntry *> VarCatalogue::allAddressable() const {
    std::vector<const VarEntry *> result;
    for (const auto &v: vars)
      if (!isAggType(v.type)) // scalars and ptr vars are addressable
        result.push_back(&v);
    return result;
  }

  const VarEntry *VarCatalogue::findAny(const TypePtr &t, std::mt19937 &rng) const {
    std::vector<const VarEntry *> candidates;
    for (const auto &v: vars)
      if (typeEquals(v.type, t))
        candidates.push_back(&v);
    if (candidates.empty())
      return nullptr;
    std::uniform_int_distribution<int> d(0, (int) candidates.size() - 1);
    return candidates[d(rng)];
  }

  const VarEntry *VarCatalogue::findAddressableOfType(const TypePtr &t, std::mt19937 &rng) const {
    std::vector<const VarEntry *> candidates;
    for (const auto &v: vars)
      if (!isAggType(v.type) && typeEquals(v.type, t))
        candidates.push_back(&v);
    if (candidates.empty())
      return nullptr;
    std::uniform_int_distribution<int> d(0, (int) candidates.size() - 1);
    return candidates[d(rng)];
  }

  // ---------------------------------------------------------------------------
  // genVarCatalogue
  // ---------------------------------------------------------------------------

  VarCatalogue genVarCatalogue(std::mt19937 &rng, const VarGenConfig &cfg) {
    VarCatalogue cat;
    const TypeGenConfig &tcfg = cfg.typeConfig;

    int structIdx = 0;
    int scalarIdx = 0;
    int arrayIdx = 0;
    int ptrIdx = 0;
    int ptrPtrIdx = 0;

    // Track vars by their type for pointer target lookup
    // We generate in three phases:
    // Phase 1: non-pointer vars (scalars, arrays, structs)
    // Phase 2: ptr T vars (pointing to phase-1 vars)
    // Phase 3: ptr ptr T vars (pointing to phase-2 ptr vars)

    std::uniform_real_distribution<double> prob(0.0, 1.0);

    // Helper: create a struct declaration. Fields are independently drawn —
    // ~30% chance of an array field ([N] scalar) to produce struct-of-arrays.
    auto makeStructDecl = [&](const TypePtr &) -> std::string {
      std::string sname = "@RY_S" + std::to_string(structIdx++);
      std::uniform_int_distribution<int> nfd(1, std::max(1, tcfg.maxAggElems));
      int nf = nfd(rng);
      StructDecl sd;
      sd.name = GlobalId{sname, {}};
      for (int fi = 0; fi < nf; fi++) {
        FieldDecl fd;
        fd.name = "f" + std::to_string(fi);
        // Struct field is an array with probability hp::kPStructFieldIsArray
        if (prob(rng) < hp::kPStructFieldIsArray && tcfg.maxAggElems >= 1) {
          std::uniform_int_distribution<int> szd(1, std::max(1, tcfg.maxAggElems));
          uint64_t sz = (uint64_t) szd(rng);
          ArrayType at;
          at.size = sz;
          at.elem = genScalarType(rng, tcfg.enableFp);
          fd.type = std::make_shared<Type>(Type{at, {}});
        } else {
          fd.type = genScalarType(rng, tcfg.enableFp);
        }
        sd.fields.push_back(std::move(fd));
      }
      cat.structDecls.push_back(std::move(sd));
      return sname;
    };

    // Split cfg.nVars between phases — see hp::kFracNonPtrVars / kFracPtr1Vars.
    int nNonPtr = std::max(1, (int) (cfg.nVars * hp::kFracNonPtrVars));
    int nPtr1 = std::max(0, (int) (cfg.nVars * hp::kFracPtr1Vars));
    int nPtr2 = cfg.nVars - nNonPtr - nPtr1;
    if (nPtr2 < 0)
      nPtr2 = 0;

    // Phase 1: non-pointer vars
    for (int i = 0; i < nNonPtr; i++) {
      // Draw a type that is not ptr
      TypePtr t = genRandomType(rng, tcfg, 0);

      // If we drew a ptr type at depth 0, convert to scalar fallback
      // (ptr handling is reserved for phases 2/3)
      if (isPtrType(t)) {
        t = genScalarType(rng, tcfg.enableFp);
      }

      VarEntry v;
      v.type = t;

      if (isScalarType(t)) {
        v.name = "%v" + std::to_string(scalarIdx++);
      } else if (std::holds_alternative<ArrayType>(t->v)) {
        auto at = std::get<ArrayType>(t->v);
        // If element is a struct placeholder, resolve it to a real struct decl
        // (array-of-struct) rather than replacing with a scalar.
        if (std::holds_alternative<StructType>(at.elem->v)) {
          std::string elemSname = makeStructDecl(at.elem);
          StructType st;
          st.name = GlobalId{elemSname, {}};
          at.elem = std::make_shared<Type>(Type{st, {}});
          v.type = std::make_shared<Type>(Type{at, {}});
          // Store elem struct name for expr/checksum lookups
          v.structTypeName = elemSname;
        }
        v.name = "%a" + std::to_string(arrayIdx++);
      } else if (std::holds_alternative<StructType>(t->v)) {
        // Assign a real struct name
        std::string sname = makeStructDecl(t);
        v.structTypeName = sname;
        // Update the type to reference the correct struct name
        StructType st;
        st.name = GlobalId{sname, {}};
        v.type = std::make_shared<Type>(Type{st, {}});
        v.name = "%t" + std::to_string(structIdx - 1);
      }
      cat.vars.push_back(std::move(v));
    }

    // Guarantee at least one i32 scalar (needed as fallback RValue)
    auto i32scalars =
        cat.scalarsOf(std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}}));
    if (i32scalars.empty()) {
      VarEntry v;
      v.name = "%v" + std::to_string(scalarIdx++);
      v.type = std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
      cat.vars.push_back(std::move(v));
    }

    // Phase 2: ptr T vars — each must point to an existing non-ptr, non-agg var.
    // We snapshot names+types BEFORE pushing to cat.vars to avoid invalidating
    // pointers when the vector reallocates.
    if (tcfg.maxPtrDepth >= 1) {
      struct TargetInfo {
        std::string name;
        TypePtr type;
      };

      std::vector<TargetInfo> addressableTargets;
      for (const auto &v: cat.vars)
        if (!isPtrType(v.type) && !isAggType(v.type) && !isVecType(v.type))
          addressableTargets.push_back({v.name, v.type});

      for (int i = 0; i < nPtr1 && !addressableTargets.empty(); i++) {
        std::uniform_int_distribution<int> pd(0, (int) addressableTargets.size() - 1);
        const TargetInfo &target = addressableTargets[pd(rng)];

        VarEntry pv;
        pv.name = "%p" + std::to_string(ptrIdx++);
        pv.ptrTarget = target.name;

        PtrType pt;
        pt.pointee = target.type;
        pv.type = std::make_shared<Type>(Type{pt, {}});

        cat.vars.push_back(std::move(pv));
      }
    }

    // Phase 3: ptr ptr T vars — each must point to an existing ptr var.
    // Snapshot before the loop to avoid dangling pointers on vector reallocation.
    if (tcfg.maxPtrDepth >= 2 && nPtr2 > 0) {
      struct TargetInfo {
        std::string name;
        TypePtr type;
      };

      std::vector<TargetInfo> ptrTargets;
      for (const auto &v: cat.vars)
        if (isPtrType(v.type))
          ptrTargets.push_back({v.name, v.type});

      for (int i = 0; i < nPtr2 && !ptrTargets.empty(); i++) {
        std::uniform_int_distribution<int> pd(0, (int) ptrTargets.size() - 1);
        const TargetInfo &target = ptrTargets[pd(rng)];

        VarEntry ppv;
        ppv.name = "%pp" + std::to_string(ptrPtrIdx++);
        ppv.ptrTarget = target.name;

        PtrType pt;
        pt.pointee = target.type;
        ppv.type = std::make_shared<Type>(Type{pt, {}});

        cat.vars.push_back(std::move(ppv));
      }
    }

    return cat;
  }

} // namespace symir::reify
