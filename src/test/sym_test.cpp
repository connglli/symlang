#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "analysis/cfg.hpp"
#include "analysis/definite_init.hpp"
#include "ast/ast_dumper.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/typechecker.hpp"

// Simple check for expected outcome in the first few lines of the file
// Format: // EXPECT: PASS  or // EXPECT: FAIL
enum class Expected { Pass, Fail, Unknown };

Expected getExpected(const std::string &src) {
  std::istringstream iss(src);
  std::string line;
  for (int i = 0; i < 5 && std::getline(iss, line); ++i) {
    if (line.find("// EXPECT: PASS") != std::string::npos)
      return Expected::Pass;
    if (line.find("// EXPECT: FAIL") != std::string::npos)
      return Expected::Fail;
  }
  return Expected::Unknown;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <input.sir>\n";
    return 1;
  }

  std::string path = argv[1];
  std::ifstream ifs(path);
  if (!ifs) {
    std::cerr << "Could not open file: " << path << "\n";
    return 1;
  }
  std::stringstream ss;
  ss << ifs.rdbuf();
  std::string src = ss.str();

  Expected exp = getExpected(src);
  bool failed = false;
  std::string failureStage;

  try {
    Lexer lx(src);
    auto toks = lx.lexAll();

    Parser ps(std::move(toks));
    Program prog = ps.parseProgram();

    // If just parsing tests, we could stop here, but let's run full pipeline
    // unless flags say otherwise. For now, full pipeline.

    DiagBag diags;
    TypeChecker tc(prog);
    tc.runAll(diags); // This also builds CFGs internally

    if (!diags.hasErrors()) {
      // Run Definite Init
      for (const auto &f: prog.funs) {
        // We need to rebuild CFG here because TypeChecker doesn't expose it
        // (Optimization: TypeChecker could expose it, but this is fine for tests)
        CFG cfg = CFG::build(f, diags);
        DefiniteInitAnalysis::run(f, cfg, diags);
      }
    }

    if (diags.hasErrors()) {
      failed = true;
      failureStage = "Semantic/Analysis";
      for (const auto &d: diags.diags) {
        if (d.level == DiagLevel::Error) {
          std::cerr << "Error: " << d.message << " at " << d.span.begin.line << ":"
                    << d.span.begin.col << "\n";
        }
      }
    }

  } catch (const ParseError &e) {
    failed = true;
    failureStage = "Parse/Lex";
    std::cerr << "ParseError: " << e.what() << " at " << e.span.begin.line << ":"
              << e.span.begin.col << "\n";
  } catch (const std::exception &e) {
    failed = true;
    failureStage = "Exception";
    std::cerr << "Exception: " << e.what() << "\n";
  }

  if (exp == Expected::Pass) {
    if (failed) {
      std::cout << "TEST FAILED: " << path << " (Expected PASS, got FAIL in " << failureStage
                << ")\n";
      return 1;
    } else {
      std::cout << "TEST PASSED: " << path << "\n";
      return 0;
    }
  } else if (exp == Expected::Fail) {
    if (failed) {
      std::cout << "TEST PASSED: " << path << " (Expected FAIL, got FAIL)\n";
      return 0;
    } else {
      std::cout << "TEST FAILED: " << path << " (Expected FAIL, got PASS)\n";
      return 1;
    }
  } else {
    // Unknown expectation, just report status
    if (failed) {
      std::cout << "FAIL: " << path << "\n";
      return 1;
    } else {
      std::cout << "PASS: " << path << "\n";
      return 0;
    }
  }
}
