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
    ("trace", "Print execution trace", cxxopts::value<bool>()->default_value("false"))
    ("h,help", "Print usage");
  // clang-format on

  options.parse_positional({"input"});

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
  std::unordered_map<std::string, std::int64_t> symBindings;

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
        symBindings[name] = std::stoll(valStr);
      } catch (...) {
        std::cerr << "Error: Invalid integer value for symbol " << name << ": " << valStr << "\n";
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

    if (pm.run(prog) == symir::PassResult::Error) {
      std::cerr << "Errors:\n";
      for (const auto &d: diags.diags) {
        if (d.level == DiagLevel::Error) {
          std::cerr << "  " << d.message << " at " << d.span.begin.line << ":" << d.span.begin.col
                    << "\n";
        }
      }
      return 1;
    }

    for (const auto &d: diags.diags) {
      if (d.level == DiagLevel::Warning) {
        std::cout << "Warning: " << d.message << " at " << d.span.begin.line << ":"
                  << d.span.begin.col << "\n";
      }
    }

    // 4. Interpret
    Interpreter interp(prog);
    // TODO: Pass trace flag to interpreter when implemented
    interp.run(mainFunc, symBindings);

  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
