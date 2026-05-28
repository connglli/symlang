#include "frontend/semchecker.hpp"
#include <algorithm>

namespace symir {

  symir::PassResult SemChecker::run(Program &prog, DiagBag &diags) {
    std::unordered_set<std::string> globalNames;

    for (const auto &s: prog.structs) {
      if (globalNames.count(s.name.name)) {
        diags.error("Duplicate global name (struct): " + s.name.name, s.span);
      }
      globalNames.insert(s.name.name);
      checkStruct(s, diags);
    }

    for (const auto &f: prog.funs) {
      if (globalNames.count(f.name.name)) {
        diags.error("Duplicate global name (function): " + f.name.name, f.span);
      }
      globalNames.insert(f.name.name);
      checkFunction(f, diags);
    }

    // [v0.2.2] External declarations: each `decl @name` is a global name and
    // must not collide with any struct/fun/intrinsic. A contract-form `decl`
    // and a `fun` with the same name within the same file is rejected (the
    // cross-file body+contract conflict is enforced by the link resolver).
    for (const auto &d: prog.extDecls) {
      if (globalNames.count(d.name.name)) {
        diags.error("Duplicate global name (decl): " + d.name.name, d.span);
      }
      globalNames.insert(d.name.name);
      checkExtDecl(d, diags);
    }

    // [v0.2.2] Intrinsic declarations.
    for (const auto &d: prog.intrinsics) {
      if (globalNames.count(d.name.name)) {
        diags.error("Duplicate global name (intrinsic): " + d.name.name, d.span);
      }
      globalNames.insert(d.name.name);
      checkIntrinsicDecl(d, diags);
    }
    return diags.hasErrors() ? symir::PassResult::Error : symir::PassResult::Success;
  }

  void SemChecker::checkStruct(const StructDecl &s, DiagBag &diags) {
    std::unordered_set<std::string> fields;
    for (const auto &f: s.fields) {
      if (fields.count(f.name)) {
        diags.error("Duplicate field name: " + f.name, f.span);
      }
      fields.insert(f.name);
    }
  }

  void SemChecker::checkFunction(const FunDecl &f, DiagBag &diags) {
    if (f.blocks.empty()) {
      diags.error("Function must have at least one basic block", f.span);
    }

    checkSigils(f, diags);
    checkDuplicates(f, diags);

    // Check domains
    for (const auto &s: f.syms) {
      if (s.domain) {
        if (auto interval = std::get_if<DomainInterval>(&(*s.domain))) {
          if (interval->lo > interval->hi) {
            diags.error("Invalid symbol domain: lower bound > upper bound", interval->span);
          }
        }
      }
    }
  }

  void SemChecker::checkSigils(const FunDecl &f, DiagBag &diags) {
    // Inside a function, symbols must be local (%?) not global (@?)
    for (const auto &s: f.syms) {
      if (s.name.name.rfind("@?", 0) == 0) {
        diags.error(
            "Global symbol '" + s.name.name +
                "' declared in local scope. Use '%?' for local symbols.",
            s.name.span
        );
      }
    }
  }

  void SemChecker::checkDuplicates(const FunDecl &f, DiagBag &diags) {
    std::unordered_set<std::string> locals;
    std::unordered_set<std::string> labels;

    for (const auto &p: f.params) {
      if (locals.count(p.name.name)) {
        diags.error("Duplicate parameter name: " + p.name.name, p.span);
      }
      locals.insert(p.name.name);
    }

    for (const auto &s: f.syms) {
      if (locals.count(s.name.name)) {
        diags.error("Duplicate name (symbol): " + s.name.name, s.span);
      }
      locals.insert(s.name.name);
    }

    for (const auto &l: f.lets) {
      if (locals.count(l.name.name)) {
        diags.error("Duplicate name (local): " + l.name.name, l.span);
      }
      locals.insert(l.name.name);
    }

    for (const auto &b: f.blocks) {
      if (labels.count(b.label.name)) {
        diags.error("Duplicate block label: " + b.label.name, b.label.span);
      }
      labels.insert(b.label.name);
    }
  }

  // [v0.2.2] §3.4: a contract must have at least one `post` clause. `pre`
  // clauses are optional. Parameter names must be unique.
  void SemChecker::checkExtDecl(const ExtDecl &d, DiagBag &diags) {
    std::unordered_set<std::string> params;
    for (const auto &p: d.params) {
      if (params.count(p.name.name)) {
        diags.error("Duplicate parameter name: " + p.name.name, p.span);
      }
      params.insert(p.name.name);
    }
    if (d.contract) {
      if (d.contract->posts.empty()) {
        diags.error(
            "Contract on '" + d.name.name + "' must contain at least one `post` clause", d.span
        );
      }
    }
  }

  void SemChecker::checkIntrinsicDecl(const IntrinsicDecl &d, DiagBag &diags) {
    std::unordered_set<std::string> params;
    for (const auto &p: d.params) {
      if (params.count(p.name.name)) {
        diags.error("Duplicate parameter name: " + p.name.name, p.span);
      }
      params.insert(p.name.name);
    }
  }

} // namespace symir
