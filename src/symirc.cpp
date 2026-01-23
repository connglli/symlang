#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "analysis/definite_init.hpp"
#include "analysis/pass_manager.hpp"
#include "analysis/reachability.hpp"
#include "analysis/unused_name.hpp"
#include "backend/c_backend.hpp"
#include "cxxopts.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"

int main(int argc, char **argv) {
  using namespace symir;

  cxxopts::Options options("symirc", "SymIR Compiler");

  // clang-format off
  options.add_options()
    ("input", "Input .sir file", cxxopts::value<std::string>())
    ("o,output", "Output file (default: stdout)", cxxopts::value<std::string>())
    ("backend", "Backend target (c, wasm)", cxxopts::value<std::string>()->default_value("c"))
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
  std::ifstream ifs(inputPath);
  if (!ifs) {
    std::cerr << "Error: Could not open file " << inputPath << "\n";
    return 1;
  }
  std::stringstream ss;
  ss << ifs.rdbuf();
  std::string src = ss.str();

  try {
    // 1. Frontend
    Lexer lx(src);
    auto toks = lx.lexAll();
    Parser ps(std::move(toks));
    Program prog = ps.parseProgram();

    // 2. Analysis
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

    // Print warnings
    for (const auto &d: diags.diags) {
      if (d.level == DiagLevel::Warning) {
        std::cerr << "Warning: " << d.message << " at " << d.span.begin.line << ":"
                  << d.span.begin.col << "\n";
      }
    }

    // 3. Backend
    std::string backend = result["backend"].as<std::string>();
    std::ostream *outStream = &std::cout;
    std::ofstream ofs;

    if (result.count("output")) {
      std::string outPath = result["output"].as<std::string>();
      ofs.open(outPath);
      if (!ofs) {
        std::cerr << "Error: Could not open output file " << outPath << "\n";
        return 1;
      }
      outStream = &ofs;
    }

    if (backend == "c") {
      CBackend cb(*outStream);
      cb.emit(prog);
    } else {
      std::cerr << "Error: Unsupported backend: " << backend << "\n";
      return 1;
    }

  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
