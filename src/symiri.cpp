#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/cfg.hpp"
#include "analysis/definite_init.hpp"
#include "analysis/pass_manager.hpp"
#include "analysis/reachability.hpp"
#include "analysis/unused_name.hpp"
#include "cxxopts.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"
#include "interp/interpreter.hpp"

int main(int argc, char **argv) {
  using namespace symir;

  cxxopts::Options options("symiri", "SymIR Reference Interpreter");

  // clang-format off
  options.add_options()
    ("input", "Input .sir file", cxxopts::value<std::string>())
    ("main", "Entry function to execute", cxxopts::value<std::string>()->default_value("@main"))
    ("sym", "Bind a symbol (name=value)", cxxopts::value<std::vector<std::string>>())
    ("check", "Check semantics only (do not execute)", cxxopts::value<bool>()->default_value("false"))
    ("dump-trace", "Dump executed blocks and variable updates", cxxopts::value<bool>()->default_value("false"))
    ("w", "Inhibit all warning messages", cxxopts::value<bool>()->default_value("false"))
    ("Werror", "Make all warnings into errors", cxxopts::value<bool>()->default_value("false"))
    ("h,help", "Print usage");
  options.parse_positional({"input"});
  // clang-format on

  auto result = options.parse(argc, argv);

  if (result.count("help")) {
    std::cout << options.help() << std::endl;
    return 0;
  }

  if (!result.count("input")) {
    std::cerr << "Error: No input file specified." << std::endl;
    std::cerr << options.help() << std::endl;
    return 1;
  }

  std::string inputPath = result["input"].as<std::string>();
  std::string mainFunc = result["main"].as<std::string>();
  Interpreter::SymBindings symBindings;

  if (result.count("sym")) {
    for (const auto &bind: result["sym"].as<std::vector<std::string>>()) {
      size_t eq = bind.find('=');
      if (eq == std::string::npos) {
        std::cerr << "Error: Invalid symbol binding format (expected name=value): " << bind << "\n";
        return 1;
      }
      std::string name = bind.substr(0, eq);
      std::string valStr = bind.substr(eq + 1);
      try {
        symBindings[name] = parseNumberLiteral(valStr);
      } catch (...) {
        std::cerr << "Error: Invalid number value for symbol " << name << ": " << valStr << "\n";
        return 1;
      }
    }
  }

  std::ifstream ifs(inputPath);
  if (!ifs) {
    std::cerr << "Error: Could not open file " << inputPath << "\n";
    return 1;
  }
  std::stringstream ss;
  ss << ifs.rdbuf();
  std::string src = ss.str();

  try {
    // 1. Frontend: Lex & Parse
    Lexer lx(src);
    auto toks = lx.lexAll();
    Parser ps(std::move(toks));
    Program prog = ps.parseProgram();

    // 2. Analysis: Pass Manager orchestration
    DiagBag diags;
    symir::PassManager pm(diags);
    pm.addModulePass(std::make_unique<SemChecker>());
    pm.addModulePass(std::make_unique<TypeChecker>());
    pm.addFunctionPass(std::make_unique<ReachabilityAnalysis>());
    pm.addFunctionPass(std::make_unique<DefiniteInitAnalysis>());
    pm.addFunctionPass(std::make_unique<UnusedNameAnalysis>());

    bool werror = result["Werror"].as<bool>();
    bool nowarn = result["w"].as<bool>();

    if (pm.run(prog) == symir::PassResult::Error || (werror && diags.hasWarnings())) {
      std::cerr << "Errors:\n";
      for (const auto &d: diags.diags) {
        if (d.level == DiagLevel::Error || (werror && d.level == DiagLevel::Warning)) {
          printMessage(std::cerr, src, d.span, d.message, d.level);
        }
      }
      return 1;
    }

    if (!nowarn) {
      for (const auto &d: diags.diags) {
        if (d.level == DiagLevel::Warning) {
          printMessage(std::cout, src, d.span, d.message, d.level);
        }
      }
    }

    if (result["check"].as<bool>()) {
      return 0;
    }

    // 4. Interpret
    Interpreter interp(prog);
    interp.run(mainFunc, symBindings, result["dump-trace"].as<bool>());

  } catch (const ParseError &e) {
    printMessage(std::cerr, src, e.span, e.what(), DiagLevel::Error);
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
