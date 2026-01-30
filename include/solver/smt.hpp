#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace symir::smt {

  // Opaque handles for Sort and Term
  // Implementations should cast these to their concrete types.
  // We use shared_ptr to allow implementations to manage lifetime (e.g. ref counting)
  // if necessary.
  struct Sort {
    std::shared_ptr<void> internal;

    bool operator==(const Sort &other) const { return internal == other.internal; }

    bool operator!=(const Sort &other) const { return internal != other.internal; }
  };

  struct Term {
    std::shared_ptr<void> internal;

    bool operator==(const Term &other) const { return internal == other.internal; }

    bool operator!=(const Term &other) const { return internal != other.internal; }
  };

  enum class Kind {
    // Bit-vector arithmetic
    BV_ADD,
    BV_SUB,
    BV_MUL,
    BV_SDIV,
    BV_UDIV,
    BV_SREM,
    BV_UREM,
    BV_AND,
    BV_OR,
    BV_XOR,
    BV_NOT,
    BV_SHL,
    BV_ASHR,
    BV_SHR,
    BV_NEG,

    // Bit-vector comparison
    BV_SLT,
    BV_SLE,
    BV_SGT,
    BV_SGE,
    BV_ULT,
    BV_ULE,
    BV_UGT,
    BV_UGE,

    // Equality
    EQUAL,
    DISTINCT,

    // Control flow / Logic
    ITE,
    AND,
    OR,
    NOT,
    IMPLIES,

    // Floating point
    FP_ADD,
    FP_SUB,
    FP_MUL,
    FP_DIV,
    FP_REM,
    FP_SQRT,
    FP_RTI, // Round to integral
    FP_MIN,
    FP_MAX,

    FP_EQUAL,
    FP_LT,
    FP_LEQ,
    FP_GT,
    FP_GEQ,

    // Conversions
    FP_TO_SBV,
    FP_TO_UBV,
    FP_TO_FP_FROM_FP,
    FP_TO_FP_FROM_SBV,
    FP_TO_FP_FROM_UBV,
    BV_SIGN_EXTEND,
    BV_ZERO_EXTEND,
    BV_EXTRACT,
    BV_CONCAT,

    // Overflow checks
    BV_SADD_OVERFLOW,
    BV_SSUB_OVERFLOW,
    BV_SMUL_OVERFLOW
  };

  enum class RoundingMode { RNE, RNA, RTP, RTN, RTZ };

  enum class Result { SAT, UNSAT, UNKNOWN };

  class ISolver {
  public:
    virtual ~ISolver() = default;

    // Sort creation
    virtual Sort make_bv_sort(uint32_t size) = 0;
    virtual Sort make_fp_sort(uint32_t exp, uint32_t sig) = 0;
    virtual Sort make_bool_sort() = 0;

    // Sort inspection
    virtual bool is_bv_sort(Sort s) = 0;
    virtual bool is_fp_sort(Sort s) = 0;
    virtual bool is_bool_sort(Sort s) = 0;
    virtual uint32_t get_bv_width(Sort s) = 0;
    virtual std::pair<uint32_t, uint32_t> get_fp_dims(Sort s) = 0;

    // Term creation - Constants
    virtual Term make_true() = 0;
    virtual Term make_false() = 0;
    virtual Term make_bv_value(Sort s, const std::string &val, uint8_t base) = 0;
    virtual Term make_bv_value_uint64(Sort s, uint64_t val) = 0;
    virtual Term make_bv_value_int64(Sort s, int64_t val) = 0;
    virtual Term make_bv_zero(Sort s) = 0;
    virtual Term make_bv_one(Sort s) = 0;
    virtual Term make_bv_min_signed(Sort s) = 0;
    virtual Term make_bv_max_signed(Sort s) = 0;

    virtual Term make_fp_value(Sort s, const std::string &val, RoundingMode rm) = 0;
    virtual Term make_fp_value_from_real(Sort s, double val, RoundingMode rm) = 0;

    // Term creation - Variables
    virtual Term make_const(Sort s, const std::string &name) = 0;

    // Term creation - Operations
    virtual Term
    make_term(Kind k, const std::vector<Term> &args, const std::vector<uint32_t> &indices = {}) = 0;

    // Helper for simple binary ops
    Term make_term(Kind k, Term a, Term b) { return make_term(k, std::vector<Term>{a, b}); }

    // Helper for simple unary ops
    Term make_term(Kind k, Term a) { return make_term(k, std::vector<Term>{a}); }

    // Term inspection
    virtual Sort get_sort(Term t) = 0;
    virtual bool is_true(Term t) = 0;
    virtual bool is_false(Term t) = 0;

    // Solving
    virtual void assert_formula(Term t) = 0;
    virtual Result check_sat() = 0;

    // Model generation
    virtual Term get_value(Term t) = 0; // Returns a constant term representing the value
    virtual std::string get_bv_value_string(Term t, uint8_t base) = 0;
    virtual std::string get_fp_value_string(Term t) = 0; // IEEE 754 binary representation
  };

} // namespace symir::smt
