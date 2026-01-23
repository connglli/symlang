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
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"

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

  try {
    Lexer lx(src);
    auto toks = lx.lexAll();

    Parser ps(std::move(toks));

    Program prog = ps.parseProgram();


    DiagBag diags;

    symir::PassManager pm(diags);

    pm.addModulePass(std::make_unique<SemChecker>());

    pm.addModulePass(std::make_unique<TypeChecker>());

    pm.addFunctionPass(std::make_unique<DefiniteInitAnalysis>());


    if (pm.run(prog) == symir::PassResult::Error) {

      for (const auto &d: diags.diags) {

        if (d.level == DiagLevel::Error) {

          std::cerr << "Error: " << d.message << " at " << d.span.begin.line << ":"
                    << d.span.begin.col << "\n";
        }
      }

      return 1;
    }


  } catch (const ParseError &e) {
    std::cerr << "ParseError: " << e.what() << " at " << e.span.begin.line << ":"
              << e.span.begin.col << "\n";
    return 1; // Parse Error
  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return 1;
  }

  return 0; // Success
}
