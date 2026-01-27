#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "analysis/definite_init.hpp"
#include "analysis/pass_manager.hpp"
#include "analysis/reachability.hpp"
#include "analysis/unused_name.hpp"
#include "ast/ast_dumper.hpp"
#include "backend/c_backend.hpp"
#include "backend/wasm_backend.hpp"
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
    ("target", "Backend target (c, wasm)", cxxopts::value<std::string>()->default_value("c"))
    ("dump-ast", "Dump AST to stdout and exit", cxxopts::value<bool>()->default_value("false"))
    ("w", "Inhibit all warning messages", cxxopts::value<bool>()->default_value("false"))
    ("Werror", "Make all warnings into errors", cxxopts::value<bool>()->default_value("false"))
    ("no-module-tags", "Omit (module ...) tags in WASM output", cxxopts::value<bool>()->default_value("false"))
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

    if (result["dump-ast"].as<bool>()) {
      ASTDumper dumper(std::cout);
      dumper.dump(prog);
      return 0;
    }

    // 2. Analysis
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

    // Print warnings
    if (!nowarn) {
      for (const auto &d: diags.diags) {
        if (d.level == DiagLevel::Warning) {
          printMessage(std::cerr, src, d.span, d.message, d.level);
        }
      }
    }

    // 3. Backend
    std::string target = result["target"].as<std::string>();
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

    if (target == "c") {
      CBackend cb(*outStream);
      cb.emit(prog);
    } else if (target == "wasm") {
      WasmBackend wb(*outStream);
      if (result.count("no-module-tags")) {
        wb.setNoModuleTags(result["no-module-tags"].as<bool>());
      }
      wb.emit(prog);
    } else {
      std::cerr << "Error: Unsupported target: " << target << "\n";
      return 1;
    }

  } catch (const ParseError &e) {
    printMessage(std::cerr, src, e.span, e.what(), DiagLevel::Error);
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
