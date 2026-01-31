#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ast/ast_dumper.hpp"
#include "ast/sir_printer.hpp"
#include "cxxopts.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"
#include "solver/solver.hpp"
#if defined(USE_ALIVESMT)
#include "solver/alive_impl.hpp"
#elif defined(USE_BITWUZLA)
#include "solver/bitwuzla_impl.hpp"
#endif

using namespace symir;

std::vector<std::string> split(const std::string &s, char delimiter) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(s);
  while (std::getline(tokenStream, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

int main(int argc, char **argv) {
  cxxopts::Options options("symirsolve", "SymIR SMT-based Concretizer");

  // clang-format off
  options.add_options()
    ("input", "Input .sir file", cxxopts::value<std::string>())
    ("main", "Function to concretize", cxxopts::value<std::string>()->default_value("@main"))
    ("path", "Comma-separated block labels for execution path (acts as prefix if --sample is used)", cxxopts::value<std::string>())
    ("sample", "Number of paths to sample randomly", cxxopts::value<uint32_t>())
    ("max-path-len", "Maximum random path length", cxxopts::value<uint32_t>()->default_value("100"))
    ("require-terminal", "Force paths to reach 'ret' by appending shortest path if needed", cxxopts::value<bool>()->default_value("false"))
    ("o,output", "Output .sir file", cxxopts::value<std::string>())
    ("dump-ast", "Dump concretized AST to stdout", cxxopts::value<bool>()->default_value("false"))
    ("timeout-ms", "Solver timeout in milliseconds", cxxopts::value<uint32_t>()->default_value("0"))
    ("seed", "Solver seed", cxxopts::value<uint32_t>()->default_value("0"))
    ("emit-model", "Emit symbol assignments to a JSON-like file", cxxopts::value<std::string>())
    ("sym", "Fix a symbol to a value (name=val)", cxxopts::value<std::vector<std::string>>())
    ("h,help", "Print usage");
  options.parse_positional({"input"});
  // clang-format on

  auto result = options.parse(argc, argv);

  if (result.count("help")) {
    std::cout << options.help() << std::endl;
    return 0;
  }

  if (!result.count("input") || (!result.count("path") && !result.count("sample"))) {
    std::cerr << "Error: input and either --path or --sample are required." << std::endl;
    std::cerr << options.help() << std::endl;
    return 1;
  }

  std::string inputPath = result["input"].as<std::string>();
  std::string funcName = result["main"].as<std::string>();

  std::vector<std::string> path;
  if (result.count("path")) {
    std::string path_str = result["path"].as<std::string>();
    if (path_str.size() >= 2 && ((path_str.front() == '\'' && path_str.back() == '\'') ||
                                 (path_str.front() == '\"' && path_str.back() == '\"'))) {
      path_str = path_str.substr(1, path_str.size() - 2);
    }
    path = split(path_str, ',');
  }

  std::unordered_map<std::string, int64_t> fixedSyms;
  if (result.count("sym")) {
    for (const auto &s: result["sym"].as<std::vector<std::string>>()) {
      auto eq = s.find('=');
      if (eq != std::string::npos) {
        fixedSyms[s.substr(0, eq)] = parseIntegerLiteral(s.substr(eq + 1));
      }
    }
  }

  std::ifstream ifs(inputPath);
  if (!ifs) {
    std::cerr << "Error: Could not open file " << inputPath << std::endl;
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
    PassManager pm(diags);
    pm.addModulePass(std::make_unique<SemChecker>());
    pm.addModulePass(std::make_unique<TypeChecker>());
    if (pm.run(prog) == PassResult::Error) {
      std::cerr << "Errors in input program:" << std::endl;
      for (const auto &d: diags.diags) {
        if (d.level == DiagLevel::Error)
          printMessage(std::cerr, src, d.span, d.message, d.level);
      }
      return 1;
    }

    SymbolicExecutor::Config config;
    config.timeout_ms = result["timeout-ms"].as<uint32_t>();
    config.seed = result["seed"].as<uint32_t>();

    auto solverFactory = [](const SymbolicExecutor::Config &cfg
                         ) -> std::unique_ptr<symir::smt::ISolver> {
#if defined(USE_ALIVESMT)
      return std::make_unique<symir::solver::AliveSolver>(cfg.timeout_ms, cfg.seed);
#elif defined(USE_BITWUZLA)
      return std::make_unique<symir::solver::BitwuzlaSolver>(cfg.timeout_ms, cfg.seed);
#else
      (void) cfg;
      throw std::runtime_error("No solver backend available");
#endif
    };

    SymbolicExecutor executor(prog, config, solverFactory);
    SymbolicExecutor::Result res;
    if (result.count("sample")) {
      res = executor.sample(
          funcName, result["sample"].as<uint32_t>(), result["max-path-len"].as<uint32_t>(),
          result["require-terminal"].as<bool>(), path, fixedSyms
      );
    } else {
      res = executor.solve(funcName, path, fixedSyms);
    }

    if (res.sat) {
      std::cout << "SAT" << std::endl;

      if (result.count("emit-model")) {
        std::ofstream mfs(result["emit-model"].as<std::string>());
        mfs << "{\n";
        mfs << "  \"" << funcName << "\": {\n";
        bool first = true;
        for (const auto &[name, val]: res.model) {
          if (!first)
            mfs << ",\n";
          mfs << "    \"" << name << "\": ";
          if (std::holds_alternative<int64_t>(val))
            mfs << std::get<int64_t>(val);
          else
            mfs << std::get<double>(val);
          first = false;
        }
        mfs << "\n  }\n";
        mfs << "}\n";
      }

      if (result["dump-ast"].as<bool>()) {
        std::unordered_map<std::string, int64_t> intModel;
        for (const auto &[name, val]: res.model) {
          if (std::holds_alternative<int64_t>(val))
            intModel[name] = std::get<int64_t>(val);
        }
        ASTDumper dumper(std::cout, intModel);
        dumper.dump(prog);
      }

      if (result.count("output")) {
        std::ofstream ofs(result["output"].as<std::string>());
        if (!ofs) {
          std::cerr << "Error: Could not open output file " << result["output"].as<std::string>()
                    << std::endl;
          return 1;
        }
        SIRPrinter printer(ofs, res.model);
        printer.print(prog);
      }

    } else if (res.unsat) {
      std::cout << "UNSAT" << std::endl;
      return 1;
    } else {
      std::cout << "UNKNOWN" << std::endl;
      return 1;
    }

  } catch (const ParseError &e) {
    printMessage(std::cerr, src, e.span, e.what(), DiagLevel::Error);
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
