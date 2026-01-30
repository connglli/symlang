#include "solver/alive_impl.hpp"
#include <iostream>
#include <stdexcept>

namespace symir::solver {

  // Helper to manage global initialization state for Alive2 SMT
  // Assuming alivesmt::solver_init() and alivesmt::solver_destroy() are available or handled by
  // smt_initializer alivesmt::smt_initializer is in smt.h

  AliveSolver::AliveSolver(uint32_t timeout_ms, uint32_t seed) {
    // initializer member handles smt::smt_initializer::init() which calls ctx.init() and
    // solver_init()
    if (timeout_ms > 0) {
      ::alivesmt::set_query_timeout(std::to_string(timeout_ms));
    }
    if (seed > 0) {
      ::alivesmt::set_random_seed(std::to_string(seed));
    }
    solver = std::make_unique<::alivesmt::Solver>();
  }

  AliveSolver::~AliveSolver() {
    // optional: ::alivesmt::solver_destroy();
    // Usually better to leave it if other instances exist, but here we likely run one.
  }

  ::alivesmt::expr AliveSolver::unwrap(smt::Term t) const {
    if (!t.internal)
      throw std::runtime_error("Empty term handle");
    return *static_cast<::alivesmt::expr *>(t.internal.get());
  }

  ::alivesmt::expr AliveSolver::unwrap(smt::Sort s) const {
    if (!s.internal)
      throw std::runtime_error("Empty sort handle");
    return *static_cast<::alivesmt::expr *>(s.internal.get());
  }

  smt::Term AliveSolver::wrap(const ::alivesmt::expr &t) const {
    return {std::make_shared<::alivesmt::expr>(t)};
  }

  smt::Sort AliveSolver::wrap_sort(const ::alivesmt::expr &s) const {
    return {std::make_shared<::alivesmt::expr>(s)};
  }

  smt::Sort AliveSolver::make_bv_sort(uint32_t size) {
    return wrap_sort(::alivesmt::expr::mkUInt(0, size));
  }

  smt::Sort AliveSolver::make_fp_sort(uint32_t exp, uint32_t sig) {
    if (exp == 8 && sig == 24)
      return wrap_sort(::alivesmt::expr::mkFloat(0.0f));
    if (exp == 11 && sig == 53)
      return wrap_sort(::alivesmt::expr::mkDouble(0.0));

    throw std::runtime_error("Unsupported FP sort dims in AliveSolver");
  }

  smt::Sort AliveSolver::make_bool_sort() {
    return wrap_sort(::alivesmt::expr(false)); // boolean false as representative
  }

  bool AliveSolver::is_bv_sort(smt::Sort s) { return unwrap(s).isBV(); }

  bool AliveSolver::is_fp_sort(smt::Sort s) { return unwrap(s).isFloat(); }

  bool AliveSolver::is_bool_sort(smt::Sort s) { return unwrap(s).isBool(); }

  uint32_t AliveSolver::get_bv_width(smt::Sort s) { return unwrap(s).bits(); }

  std::pair<uint32_t, uint32_t> AliveSolver::get_fp_dims(smt::Sort s) {
    auto e = unwrap(s);
    if (e.isSameTypeOf(::alivesmt::expr::mkFloat(0.0f)))
      return {8, 24};
    if (e.isSameTypeOf(::alivesmt::expr::mkDouble(0.0)))
      return {11, 53};

    return {0, 0}; // Error/Unknown
  }

  smt::Term AliveSolver::make_true() { return wrap(::alivesmt::expr(true)); }

  smt::Term AliveSolver::make_false() { return wrap(::alivesmt::expr(false)); }

  smt::Term AliveSolver::make_bv_value(smt::Sort s, const std::string &val, uint8_t base) {
    auto type = unwrap(s);
    if (base == 10) {
      return wrap(::alivesmt::expr::mkNumber(val.c_str(), type));
    } else if (base == 16) {
      std::string hex = "#x" + val;
      return wrap(::alivesmt::expr::mkNumber(hex.c_str(), type));
    } else if (base == 2) {
      std::string bin = "#b" + val;
      return wrap(::alivesmt::expr::mkNumber(bin.c_str(), type));
    }
    // Fallback or error
    return wrap(::alivesmt::expr::mkNumber(val.c_str(), type));
  }

  smt::Term AliveSolver::make_bv_value_uint64(smt::Sort s, uint64_t val) {
    return wrap(::alivesmt::expr::mkUInt(val, unwrap(s)));
  }

  smt::Term AliveSolver::make_bv_value_int64(smt::Sort s, int64_t val) {
    return wrap(::alivesmt::expr::mkInt(val, unwrap(s)));
  }

  smt::Term AliveSolver::make_bv_zero(smt::Sort s) {
    return wrap(::alivesmt::expr::mkUInt(0, unwrap(s)));
  }

  smt::Term AliveSolver::make_bv_one(smt::Sort s) {
    return wrap(::alivesmt::expr::mkUInt(1, unwrap(s)));
  }

  smt::Term AliveSolver::make_bv_min_signed(smt::Sort s) {
    return wrap(::alivesmt::expr::IntSMin(unwrap(s).bits()));
  }

  smt::Term AliveSolver::make_bv_max_signed(smt::Sort s) {
    return wrap(::alivesmt::expr::IntSMax(unwrap(s).bits()));
  }

  ::alivesmt::expr AliveSolver::map_rm(smt::RoundingMode rm) const {
    switch (rm) {
      case smt::RoundingMode::RNE:
        return ::alivesmt::expr::rne();
      case smt::RoundingMode::RNA:
        return ::alivesmt::expr::rna();
      case smt::RoundingMode::RTP:
        return ::alivesmt::expr::rtp();
      case smt::RoundingMode::RTN:
        return ::alivesmt::expr::rtn();
      case smt::RoundingMode::RTZ:
        return ::alivesmt::expr::rtz();
    }
    return ::alivesmt::expr::rne();
  }

  smt::Term
  AliveSolver::make_fp_value(smt::Sort s, const std::string &val, smt::RoundingMode /*rm*/) {
    return wrap(::alivesmt::expr::mkNumber(val.c_str(), unwrap(s)));
  }

  smt::Term
  AliveSolver::make_fp_value_from_real(smt::Sort s, double val, smt::RoundingMode /*rm*/) {
    auto type = unwrap(s);
    if (type.isSameTypeOf(::alivesmt::expr::mkFloat(0.0f)))
      return wrap(::alivesmt::expr::mkFloat((float) val));
    if (type.isSameTypeOf(::alivesmt::expr::mkDouble(0.0)))
      return wrap(::alivesmt::expr::mkDouble(val));
    return wrap(::alivesmt::expr::mkDouble(val)); // Fallback
  }

  smt::Term AliveSolver::make_const(smt::Sort s, const std::string &name) {
    return wrap(::alivesmt::expr::mkFreshVar(name.c_str(), unwrap(s)));
  }

  smt::Term AliveSolver::make_term(
      smt::Kind k, const std::vector<smt::Term> &args, const std::vector<uint32_t> &indices
  ) {
    std::vector<::alivesmt::expr> eargs;
    for (auto &a: args)
      eargs.push_back(unwrap(a));

    // Map kinds to Alive2 expr operations
    switch (k) {
      case smt::Kind::BV_ADD:
        return wrap(eargs[0] + eargs[1]);
      case smt::Kind::BV_SUB:
        return wrap(eargs[0] - eargs[1]);
      case smt::Kind::BV_MUL:
        return wrap(eargs[0] * eargs[1]);
      case smt::Kind::BV_SDIV:
        return wrap(eargs[0].sdiv(eargs[1]));
      case smt::Kind::BV_UDIV:
        return wrap(eargs[0].udiv(eargs[1]));
      case smt::Kind::BV_SREM:
        return wrap(eargs[0].srem(eargs[1]));
      case smt::Kind::BV_UREM:
        return wrap(eargs[0].urem(eargs[1]));
      case smt::Kind::BV_AND:
        return wrap(eargs[0] & eargs[1]);
      case smt::Kind::BV_OR:
        return wrap(eargs[0] | eargs[1]);
      case smt::Kind::BV_XOR:
        return wrap(eargs[0] ^ eargs[1]);
      case smt::Kind::BV_NOT:
        return wrap(~eargs[0]); // bitwise not
      case smt::Kind::BV_SHL:
        return wrap(eargs[0] << eargs[1]);
      case smt::Kind::BV_ASHR:
        return wrap(eargs[0].ashr(eargs[1]));
      case smt::Kind::BV_SHR:
        return wrap(eargs[0].lshr(eargs[1]));
      case smt::Kind::BV_NEG:
        return wrap(::alivesmt::expr::mkUInt(0, eargs[0]) - eargs[0]); // negation 0 - x

      case smt::Kind::BV_SLT:
        return wrap(eargs[0].slt(eargs[1]));
      case smt::Kind::BV_SLE:
        return wrap(eargs[0].sle(eargs[1]));
      case smt::Kind::BV_SGT:
        return wrap(eargs[0].sgt(eargs[1]));
      case smt::Kind::BV_SGE:
        return wrap(eargs[0].sge(eargs[1]));
      case smt::Kind::BV_ULT:
        return wrap(eargs[0].ult(eargs[1]));
      case smt::Kind::BV_ULE:
        return wrap(eargs[0].ule(eargs[1]));
      case smt::Kind::BV_UGT:
        return wrap(eargs[0].ugt(eargs[1]));
      case smt::Kind::BV_UGE:
        return wrap(eargs[0].uge(eargs[1]));

      case smt::Kind::EQUAL:
        return wrap(eargs[0] == eargs[1]);
      case smt::Kind::DISTINCT:
        return wrap(eargs[0] != eargs[1]);

      case smt::Kind::ITE:
        return wrap(::alivesmt::expr::mkIf(eargs[0], eargs[1], eargs[2]));
      case smt::Kind::AND:
        return wrap(eargs[0] && eargs[1]);
      case smt::Kind::OR:
        return wrap(eargs[0] || eargs[1]);
      case smt::Kind::NOT:
        return wrap(!eargs[0]); // boolean not
      case smt::Kind::IMPLIES:
        return wrap(eargs[0].implies(eargs[1]));

      case smt::Kind::FP_ADD:
        return wrap(eargs[1].fadd(eargs[2], eargs[0])); // args: RM, a, b -> a.fadd(b, rm)
      case smt::Kind::FP_SUB:
        return wrap(eargs[1].fsub(eargs[2], eargs[0]));
      case smt::Kind::FP_MUL:
        return wrap(eargs[1].fmul(eargs[2], eargs[0]));
      case smt::Kind::FP_DIV:
        return wrap(eargs[1].fdiv(eargs[2], eargs[0]));
      case smt::Kind::FP_REM:
        return wrap(eargs[0].frem(eargs[1])); // no RM for frem
      case smt::Kind::FP_SQRT:
        return wrap(eargs[1].sqrt(eargs[0]));
      case smt::Kind::FP_RTI:
        return wrap(eargs[1].round(eargs[0])); // Round to integral
      case smt::Kind::FP_MIN:
        return wrap(eargs[0].fmin(eargs[1]));
      case smt::Kind::FP_MAX:
        return wrap(eargs[0].fmax(eargs[1]));

      case smt::Kind::FP_EQUAL:
        return wrap(eargs[0].foeq(eargs[1]));
      case smt::Kind::FP_LT:
        return wrap(eargs[0].folt(eargs[1]));
      case smt::Kind::FP_LEQ:
        return wrap(eargs[0].fole(eargs[1]));
      case smt::Kind::FP_GT:
        return wrap(eargs[0].fogt(eargs[1]));
      case smt::Kind::FP_GEQ:
        return wrap(eargs[0].foge(eargs[1]));

      case smt::Kind::FP_TO_SBV:
        // args: RM, val. indices: {width}.
        // expr::fp2sint(unsigned bits, const expr &rm) const;
        return wrap(eargs[1].fp2sint(indices[0], eargs[0]));

      case smt::Kind::FP_TO_UBV:
        return wrap(eargs[1].fp2uint(indices[0], eargs[0]));

      case smt::Kind::FP_TO_FP_FROM_FP:
        // args: RM, val. indices: {exp, sig} -> we need target type.
        // expr::float2Float(const expr &type, const expr &rm) const;
        // We need to construct a type from indices.
        return wrap(eargs[1].float2Float(
            make_fp_sort(indices[0], indices[1]).internal
                ? unwrap(make_fp_sort(indices[0], indices[1]))
                : ::alivesmt::expr(),
            eargs[0]
        ));

      case smt::Kind::FP_TO_FP_FROM_SBV:
        // args: RM, val. indices: {exp, sig}.
        // expr::sint2fp(const expr &type, const expr &rm) const;
        return wrap(eargs[1].sint2fp(unwrap(make_fp_sort(indices[0], indices[1])), eargs[0]));

      case smt::Kind::FP_TO_FP_FROM_UBV:
        return wrap(eargs[1].uint2fp(unwrap(make_fp_sort(indices[0], indices[1])), eargs[0]));

      case smt::Kind::BV_SIGN_EXTEND:
        // args: val. indices: {amount}
        return wrap(eargs[0].sext(indices[0]));

      case smt::Kind::BV_ZERO_EXTEND:
        return wrap(eargs[0].zext(indices[0]));

      case smt::Kind::BV_EXTRACT:
        // args: val. indices: {high, low}
        return wrap(eargs[0].extract(indices[0], indices[1]));

      case smt::Kind::BV_CONCAT:
        return wrap(eargs[0].concat(eargs[1]));

      case smt::Kind::BV_SADD_OVERFLOW:
        return wrap(!eargs[0].add_no_soverflow(eargs[1]));

      case smt::Kind::BV_SSUB_OVERFLOW:
        return wrap(!eargs[0].sub_no_soverflow(eargs[1]));

      case smt::Kind::BV_SMUL_OVERFLOW:
        return wrap(!eargs[0].mul_no_soverflow(eargs[1]));

      default:
        throw std::runtime_error("Unknown/Unimplemented kind in AliveSolver");
    }
  }

  smt::Sort AliveSolver::get_sort(smt::Term t) {
    auto e = unwrap(t);
    return wrap_sort(e);
  }

  bool AliveSolver::is_true(smt::Term t) { return unwrap(t).isTrue(); }

  bool AliveSolver::is_false(smt::Term t) { return unwrap(t).isFalse(); }

  void AliveSolver::assert_formula(smt::Term t) { solver->add(unwrap(t)); }

  smt::Result AliveSolver::check_sat() {
    auto res = solver->check("check_sat");
    if (res.isSat()) {
      last_result = std::make_unique<::alivesmt::Result>(std::move(res));
      return smt::Result::SAT;
    }
    if (res.isUnsat())
      return smt::Result::UNSAT;
    return smt::Result::UNKNOWN;
  }

  smt::Term AliveSolver::get_value(smt::Term t) {
    if (!last_result || !last_result->isSat())
      throw std::runtime_error("get_value called without SAT result");

    return wrap(last_result->getModel().eval(unwrap(t), true));
  }

  std::string AliveSolver::get_bv_value_string(smt::Term t, uint8_t base) {
    auto e = unwrap(t);

    if (base == 10) {
      return std::string(e.numeral_string());
    }
    if (base == 2) {
      // Use Z3_get_numeral_binary_string via expr wrapper
      return std::string(e.numeral_binary_string());
    }
    return "";
  }

  std::string AliveSolver::get_fp_value_string(smt::Term t) {
    auto e = unwrap(t);
    auto bv = e.float2BV();

    // Convert BV constant to binary string.
    uint64_t n;
    if (bv.isUInt(n)) {
      std::string s;
      uint32_t width = bv.bits();
      for (uint32_t i = 0; i < width; ++i) {
        s = ((n & 1) ? "1" : "0") + s;
        n >>= 1;
      }
      return s;
    }
    throw std::runtime_error("get_fp_value_string failed to convert to bits");
  }

} // namespace symir::solver
