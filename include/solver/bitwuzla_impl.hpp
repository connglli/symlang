#pragma once

#include "bitwuzla/cpp/bitwuzla.h"
#include "solver/smt.hpp"

namespace symir::solver {

  class BitwuzlaSolver : public smt::ISolver {
  public:
    BitwuzlaSolver(uint32_t timeout_ms = 0, uint32_t seed = 0);
    ~BitwuzlaSolver() override = default;

    smt::Sort make_bv_sort(uint32_t size) override;
    smt::Sort make_fp_sort(uint32_t exp, uint32_t sig) override;
    smt::Sort make_bool_sort() override;

    bool is_bv_sort(smt::Sort s) override;
    bool is_fp_sort(smt::Sort s) override;
    bool is_bool_sort(smt::Sort s) override;
    uint32_t get_bv_width(smt::Sort s) override;
    std::pair<uint32_t, uint32_t> get_fp_dims(smt::Sort s) override;

    smt::Term make_true() override;
    smt::Term make_false() override;
    smt::Term make_bv_value(smt::Sort s, const std::string &val, uint8_t base) override;
    smt::Term make_bv_value_uint64(smt::Sort s, uint64_t val) override;
    smt::Term make_bv_value_int64(smt::Sort s, int64_t val) override;
    smt::Term make_bv_zero(smt::Sort s) override;
    smt::Term make_bv_one(smt::Sort s) override;
    smt::Term make_bv_min_signed(smt::Sort s) override;
    smt::Term make_bv_max_signed(smt::Sort s) override;

    smt::Term make_fp_value(smt::Sort s, const std::string &val, smt::RoundingMode rm) override;
    smt::Term make_fp_value_from_real(smt::Sort s, double val, smt::RoundingMode rm) override;

    smt::Term make_const(smt::Sort s, const std::string &name) override;

    smt::Term make_term(
        smt::Kind k, const std::vector<smt::Term> &args, const std::vector<uint32_t> &indices
    ) override;

    smt::Sort get_sort(smt::Term t) override;
    bool is_true(smt::Term t) override;
    bool is_false(smt::Term t) override;

    void assert_formula(smt::Term t) override;
    smt::Result check_sat() override;

    smt::Term get_value(smt::Term t) override;
    std::string get_bv_value_string(smt::Term t, uint8_t base) override;
    std::string get_fp_value_string(smt::Term t) override;

  private:
    bitwuzla::TermManager tm;
    bitwuzla::Bitwuzla solver;

    // Using a map to cache or manage lifetime if necessary,
    // but for shared_ptr<void> we just allocate new wrappers.

    bitwuzla::Sort unwrap(smt::Sort s) const;
    bitwuzla::Term unwrap(smt::Term t) const;
    smt::Sort wrap(bitwuzla::Sort s) const;
    smt::Term wrap(bitwuzla::Term t) const;

    bitwuzla::Kind map_kind(smt::Kind k) const;
    bitwuzla::RoundingMode map_rm(smt::RoundingMode rm) const;
  };

} // namespace symir::solver
