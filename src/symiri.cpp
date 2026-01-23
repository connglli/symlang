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
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"
#include "interp/interpreter.hpp"

void print_usage(const char *prog_name) {
  std::cerr << "Usage: " << prog_name << " <input.sir> [--main @func]\n";
}

int main(int argc, char **argv) {
  using namespace symir;

  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::string inputPath = argv[1];
  std::string mainFunc = "@main"; // Default entry point
  std::unordered_map<std::string, std::int64_t> symBindings;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--main" && i + 1 < argc) {
      mainFunc = argv[++i];
    } else if (arg == "--sym" && i + 1 < argc) {
      std::string bind = argv[++i];
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
    interp.run(mainFunc, symBindings);

  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
