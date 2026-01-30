#include "solver/bitwuzla_impl.hpp"
#include <stdexcept>

namespace symir::solver {

  static bitwuzla::Options create_options(uint32_t timeout, uint32_t seed) {
    bitwuzla::Options options;
    options.set(bitwuzla::Option::PRODUCE_MODELS, true);
    if (timeout > 0)
      options.set(bitwuzla::Option::TIME_LIMIT_PER, (uint64_t) timeout);
    if (seed > 0)
      options.set(bitwuzla::Option::SEED, (uint64_t) seed);
    return options;
  }

  BitwuzlaSolver::BitwuzlaSolver(uint32_t timeout_ms, uint32_t seed) :
      tm(), solver(tm, create_options(timeout_ms, seed)) {}

  bitwuzla::Sort BitwuzlaSolver::unwrap(smt::Sort s) const {
    if (!s.internal)
      throw std::runtime_error("Empty sort handle");
    return *static_cast<bitwuzla::Sort *>(s.internal.get());
  }

  bitwuzla::Term BitwuzlaSolver::unwrap(smt::Term t) const {
    if (!t.internal)
      throw std::runtime_error("Empty term handle");
    return *static_cast<bitwuzla::Term *>(t.internal.get());
  }

  smt::Sort BitwuzlaSolver::wrap(bitwuzla::Sort s) const {
    return {std::make_shared<bitwuzla::Sort>(s)};
  }

  smt::Term BitwuzlaSolver::wrap(bitwuzla::Term t) const {
    return {std::make_shared<bitwuzla::Term>(t)};
  }

  bitwuzla::RoundingMode BitwuzlaSolver::map_rm(smt::RoundingMode rm) const {
    switch (rm) {
      case smt::RoundingMode::RNE:
        return bitwuzla::RoundingMode::RNE;
      case smt::RoundingMode::RNA:
        return bitwuzla::RoundingMode::RNA;
      case smt::RoundingMode::RTP:
        return bitwuzla::RoundingMode::RTP;
      case smt::RoundingMode::RTN:
        return bitwuzla::RoundingMode::RTN;
      case smt::RoundingMode::RTZ:
        return bitwuzla::RoundingMode::RTZ;
    }
    return bitwuzla::RoundingMode::RNE;
  }

  bitwuzla::Kind BitwuzlaSolver::map_kind(smt::Kind k) const {
    switch (k) {
      case smt::Kind::BV_ADD:
        return bitwuzla::Kind::BV_ADD;
      case smt::Kind::BV_SUB:
        return bitwuzla::Kind::BV_SUB;
      case smt::Kind::BV_MUL:
        return bitwuzla::Kind::BV_MUL;
      case smt::Kind::BV_SDIV:
        return bitwuzla::Kind::BV_SDIV;
      case smt::Kind::BV_UDIV:
        return bitwuzla::Kind::BV_UDIV;
      case smt::Kind::BV_SREM:
        return bitwuzla::Kind::BV_SREM;
      case smt::Kind::BV_UREM:
        return bitwuzla::Kind::BV_UREM;
      case smt::Kind::BV_AND:
        return bitwuzla::Kind::BV_AND;
      case smt::Kind::BV_OR:
        return bitwuzla::Kind::BV_OR;
      case smt::Kind::BV_XOR:
        return bitwuzla::Kind::BV_XOR;
      case smt::Kind::BV_NOT:
        return bitwuzla::Kind::BV_NOT;
      case smt::Kind::BV_SHL:
        return bitwuzla::Kind::BV_SHL;
      case smt::Kind::BV_ASHR:
        return bitwuzla::Kind::BV_ASHR;
      case smt::Kind::BV_SHR:
        return bitwuzla::Kind::BV_SHR;
      case smt::Kind::BV_NEG:
        return bitwuzla::Kind::BV_NEG;
      case smt::Kind::BV_SLT:
        return bitwuzla::Kind::BV_SLT;
      case smt::Kind::BV_SLE:
        return bitwuzla::Kind::BV_SLE;
      case smt::Kind::BV_SGT:
        return bitwuzla::Kind::BV_SGT;
      case smt::Kind::BV_SGE:
        return bitwuzla::Kind::BV_SGE;
      case smt::Kind::BV_ULT:
        return bitwuzla::Kind::BV_ULT;
      case smt::Kind::BV_ULE:
        return bitwuzla::Kind::BV_ULE;
      case smt::Kind::BV_UGT:
        return bitwuzla::Kind::BV_UGT;
      case smt::Kind::BV_UGE:
        return bitwuzla::Kind::BV_UGE;
      case smt::Kind::EQUAL:
        return bitwuzla::Kind::EQUAL;
      case smt::Kind::DISTINCT:
        return bitwuzla::Kind::DISTINCT;
      case smt::Kind::ITE:
        return bitwuzla::Kind::ITE;
      case smt::Kind::AND:
        return bitwuzla::Kind::AND;
      case smt::Kind::OR:
        return bitwuzla::Kind::OR;
      case smt::Kind::NOT:
        return bitwuzla::Kind::NOT;
      case smt::Kind::IMPLIES:
        return bitwuzla::Kind::IMPLIES;
      case smt::Kind::FP_ADD:
        return bitwuzla::Kind::FP_ADD;
      case smt::Kind::FP_SUB:
        return bitwuzla::Kind::FP_SUB;
      case smt::Kind::FP_MUL:
        return bitwuzla::Kind::FP_MUL;
      case smt::Kind::FP_DIV:
        return bitwuzla::Kind::FP_DIV;
      case smt::Kind::FP_REM:
        return bitwuzla::Kind::FP_REM;
      case smt::Kind::FP_SQRT:
        return bitwuzla::Kind::FP_SQRT;
      case smt::Kind::FP_RTI:
        return bitwuzla::Kind::FP_RTI;
      case smt::Kind::FP_MIN:
        return bitwuzla::Kind::FP_MIN;
      case smt::Kind::FP_MAX:
        return bitwuzla::Kind::FP_MAX;
      case smt::Kind::FP_EQUAL:
        return bitwuzla::Kind::FP_EQUAL;
      case smt::Kind::FP_LT:
        return bitwuzla::Kind::FP_LT;
      case smt::Kind::FP_LEQ:
        return bitwuzla::Kind::FP_LEQ;
      case smt::Kind::FP_GT:
        return bitwuzla::Kind::FP_GT;
      case smt::Kind::FP_GEQ:
        return bitwuzla::Kind::FP_GEQ;
      case smt::Kind::FP_TO_SBV:
        return bitwuzla::Kind::FP_TO_SBV;
      case smt::Kind::FP_TO_UBV:
        return bitwuzla::Kind::FP_TO_UBV;
      case smt::Kind::FP_TO_FP_FROM_FP:
        return bitwuzla::Kind::FP_TO_FP_FROM_FP;
      case smt::Kind::FP_TO_FP_FROM_SBV:
        return bitwuzla::Kind::FP_TO_FP_FROM_SBV;
      case smt::Kind::FP_TO_FP_FROM_UBV:
        return bitwuzla::Kind::FP_TO_FP_FROM_UBV;
      case smt::Kind::BV_SIGN_EXTEND:
        return bitwuzla::Kind::BV_SIGN_EXTEND;
      case smt::Kind::BV_ZERO_EXTEND:
        return bitwuzla::Kind::BV_ZERO_EXTEND;
      case smt::Kind::BV_EXTRACT:
        return bitwuzla::Kind::BV_EXTRACT;
      case smt::Kind::BV_CONCAT:
        return bitwuzla::Kind::BV_CONCAT;
      case smt::Kind::BV_SADD_OVERFLOW:
        return bitwuzla::Kind::BV_SADD_OVERFLOW;
      case smt::Kind::BV_SSUB_OVERFLOW:
        return bitwuzla::Kind::BV_SSUB_OVERFLOW;
      case smt::Kind::BV_SMUL_OVERFLOW:
        return bitwuzla::Kind::BV_SMUL_OVERFLOW;
    }
    throw std::runtime_error("Unknown kind");
  }

  smt::Sort BitwuzlaSolver::make_bv_sort(uint32_t size) { return wrap(tm.mk_bv_sort(size)); }

  smt::Sort BitwuzlaSolver::make_fp_sort(uint32_t exp, uint32_t sig) {
    return wrap(tm.mk_fp_sort(exp, sig));
  }

  smt::Sort BitwuzlaSolver::make_bool_sort() { return wrap(tm.mk_bool_sort()); }

  bool BitwuzlaSolver::is_bv_sort(smt::Sort s) { return unwrap(s).is_bv(); }

  bool BitwuzlaSolver::is_fp_sort(smt::Sort s) { return unwrap(s).is_fp(); }

  bool BitwuzlaSolver::is_bool_sort(smt::Sort s) { return unwrap(s).is_bool(); }

  uint32_t BitwuzlaSolver::get_bv_width(smt::Sort s) { return unwrap(s).bv_size(); }

  std::pair<uint32_t, uint32_t> BitwuzlaSolver::get_fp_dims(smt::Sort s) {
    auto sort = unwrap(s);
    return {sort.fp_exp_size(), sort.fp_sig_size()};
  }

  smt::Term BitwuzlaSolver::make_true() { return wrap(tm.mk_true()); }

  smt::Term BitwuzlaSolver::make_false() { return wrap(tm.mk_false()); }

  smt::Term BitwuzlaSolver::make_bv_value(smt::Sort s, const std::string &val, uint8_t base) {
    return wrap(tm.mk_bv_value(unwrap(s), val, base));
  }

  smt::Term BitwuzlaSolver::make_bv_value_uint64(smt::Sort s, uint64_t val) {
    return wrap(tm.mk_bv_value(unwrap(s), std::to_string(val), 10)); // Bitwuzla < 0.4.0 style
    // Or: return wrap(tm.mk_bv_value_uint64(unwrap(s), val)); if available
  }

  smt::Term BitwuzlaSolver::make_bv_value_int64(smt::Sort s, int64_t val) {
    return wrap(tm.mk_bv_value_int64(unwrap(s), val));
  }

  smt::Term BitwuzlaSolver::make_bv_zero(smt::Sort s) { return wrap(tm.mk_bv_zero(unwrap(s))); }

  smt::Term BitwuzlaSolver::make_bv_one(smt::Sort s) { return wrap(tm.mk_bv_one(unwrap(s))); }

  smt::Term BitwuzlaSolver::make_bv_min_signed(smt::Sort s) {
    return wrap(tm.mk_bv_min_signed(unwrap(s)));
  }

  smt::Term BitwuzlaSolver::make_bv_max_signed(smt::Sort s) {
    return wrap(tm.mk_bv_max_signed(unwrap(s)));
  }

  smt::Term
  BitwuzlaSolver::make_fp_value(smt::Sort s, const std::string &val, smt::RoundingMode rm) {
    auto rm_term = tm.mk_rm_value(map_rm(rm));
    return wrap(tm.mk_fp_value(unwrap(s), rm_term, val));
  }

  smt::Term BitwuzlaSolver::make_fp_value_from_real(smt::Sort s, double val, smt::RoundingMode rm) {
    // Bitwuzla might not support double directly, usually strings.
    return wrap(tm.mk_fp_value(unwrap(s), tm.mk_rm_value(map_rm(rm)), std::to_string(val)));
  }

  smt::Term BitwuzlaSolver::make_const(smt::Sort s, const std::string &name) {
    return wrap(tm.mk_const(unwrap(s), name));
  }

  smt::Term BitwuzlaSolver::make_term(
      smt::Kind k, const std::vector<smt::Term> &args, const std::vector<uint32_t> &indices
  ) {
    std::vector<bitwuzla::Term> bargs;
    bargs.reserve(args.size());
    for (const auto &a: args)
      bargs.push_back(unwrap(a));

    // Handle operations that require RoundingMode as first argument
    if (k == smt::Kind::FP_ADD || k == smt::Kind::FP_SUB || k == smt::Kind::FP_MUL ||
        k == smt::Kind::FP_DIV || k == smt::Kind::FP_SQRT || k == smt::Kind::FP_RTI ||
        k == smt::Kind::FP_TO_SBV || k == smt::Kind::FP_TO_UBV ||
        k == smt::Kind::FP_TO_FP_FROM_FP || k == smt::Kind::FP_TO_FP_FROM_SBV ||
        k == smt::Kind::FP_TO_FP_FROM_UBV) {
      // For simplicity, we assume RNE if not provided, OR we might need to change the interface to
      // accept RM. But SymLang uses RNE. The current implementation in solver.cpp injects RNE. We
      // should inject RNE here if the backend expects it.

      // bitwuzla expects RM as first child for FP ops.
      auto rm = tm.mk_rm_value(bitwuzla::RoundingMode::RNE);
      bargs.insert(bargs.begin(), rm);
    }

    if (indices.empty())
      return wrap(tm.mk_term(map_kind(k), bargs));
    else
      return wrap(
          tm.mk_term(map_kind(k), bargs, std::vector<uint64_t>(indices.begin(), indices.end()))
      );
  }

  smt::Sort BitwuzlaSolver::get_sort(smt::Term t) { return wrap(unwrap(t).sort()); }

  bool BitwuzlaSolver::is_true(smt::Term t) { return unwrap(t).is_true(); }

  bool BitwuzlaSolver::is_false(smt::Term t) { return unwrap(t).is_false(); }

  void BitwuzlaSolver::assert_formula(smt::Term t) { solver.assert_formula(unwrap(t)); }

  smt::Result BitwuzlaSolver::check_sat() {
    auto res = solver.check_sat();
    if (res == bitwuzla::Result::SAT)
      return smt::Result::SAT;
    if (res == bitwuzla::Result::UNSAT)
      return smt::Result::UNSAT;
    return smt::Result::UNKNOWN;
  }

  smt::Term BitwuzlaSolver::get_value(smt::Term t) { return wrap(solver.get_value(unwrap(t))); }

  std::string BitwuzlaSolver::get_bv_value_string(smt::Term t, uint8_t base) {
    return unwrap(t).value<std::string>(base);
  }

  std::string BitwuzlaSolver::get_fp_value_string(smt::Term t) {
    return unwrap(t).value<std::string>(2); // Binary
  }

} // namespace symir::solver
