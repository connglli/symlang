#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "analysis/cfg.hpp"
#include "analysis/definite_init.hpp"
#include "analysis/pass_manager.hpp"
#include "analysis/reachability.hpp"
#include "analysis/unused_name.hpp"
#include "cxxopts.hpp"
#include "error.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"
#include "interp/interpreter.hpp"

namespace {
  // [v0.2.2] Load every .sir file under each -I directory (recursively).
  // Returns a vector of (path, parsed Program) pairs. Lifetime of the
  // Programs must extend over the interpreter run.
  std::vector<symir::Program> loadIncludeDirs(const std::vector<std::string> &dirs) {
    std::vector<symir::Program> libs;
    for (const auto &dir: dirs) {
      if (!std::filesystem::exists(dir))
        throw std::runtime_error("-I path does not exist: " + dir);
      for (const auto &entry: std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file())
          continue;
        if (entry.path().extension() != ".sir")
          continue;
        std::ifstream ifs(entry.path());
        std::stringstream ss;
        ss << ifs.rdbuf();
        std::string libSrc = ss.str();
        try {
          symir::Lexer lx(libSrc);
          auto toks = lx.lexAll();
          symir::Parser ps(std::move(toks));
          libs.push_back(ps.parseProgram());
        } catch (const symir::LexError &e) {
          std::cerr << "Error in -I file " << entry.path() << ": " << e.what() << "\n";
          throw;
        } catch (const symir::ParseError &e) {
          std::cerr << "Error in -I file " << entry.path() << ": " << e.what() << "\n";
          throw;
        }
      }
    }
    return libs;
  }

  // [v0.2.2] Resolve every link-form `decl @name` in main against the
  // loaded -I library programs. For each match, copy the lib's `fun` into
  // main.funs and drop the matching link-form decl. Contract-form decls
  // are left alone (their bodies are not expected anywhere).
  void resolveLinkDecls(symir::Program &main, std::vector<symir::Program> &libs) {
    std::unordered_set<std::string> mainFunNames;
    for (const auto &f: main.funs)
      mainFunNames.insert(f.name.name);

    std::vector<symir::ExtDecl> keep;
    for (auto &d: main.extDecls) {
      if (d.contract) {
        keep.push_back(std::move(d));
        continue;
      }
      if (mainFunNames.count(d.name.name)) {
        // Already defined in main as a fun -- the link-form decl is a
        // duplicate global name. Keep both so SemChecker reports it.
        keep.push_back(std::move(d));
        continue;
      }
      bool found = false;
      for (auto &lib: libs) {
        for (auto it = lib.funs.begin(); it != lib.funs.end(); ++it) {
          if (it->name.name == d.name.name) {
            main.funs.push_back(std::move(*it));
            lib.funs.erase(it);
            mainFunNames.insert(d.name.name);
            found = true;
            break;
          }
        }
        if (found)
          break;
      }
      if (!found)
        keep.push_back(std::move(d));
    }
    main.extDecls = std::move(keep);

    // Pull in struct decls from libs that aren't already in main.
    std::unordered_set<std::string> mainStructNames;
    for (const auto &s: main.structs)
      mainStructNames.insert(s.name.name);
    for (auto &lib: libs) {
      for (auto it = lib.structs.begin(); it != lib.structs.end();) {
        if (!mainStructNames.count(it->name.name)) {
          main.structs.push_back(std::move(*it));
          mainStructNames.insert(main.structs.back().name.name);
          it = lib.structs.erase(it);
        } else {
          ++it;
        }
      }
    }

    // Pull in intrinsic decls from libs (lib funs may call them).
    std::unordered_set<std::string> mainIntrNames;
    for (const auto &i: main.intrinsics)
      mainIntrNames.insert(i.name.name);
    for (auto &lib: libs) {
      for (auto it = lib.intrinsics.begin(); it != lib.intrinsics.end();) {
        if (!mainIntrNames.count(it->name.name)) {
          main.intrinsics.push_back(*it);
          mainIntrNames.insert(it->name.name);
          it = lib.intrinsics.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
} // namespace

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
    ("I", "Include path for resolving link-form `decl`s (may repeat)", cxxopts::value<std::vector<std::string>>())
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

    // 1b. [v0.2.2] Load -I libraries and resolve link-form `decl`s.
    std::vector<Program> libs;
    if (result.count("I")) {
      libs = loadIncludeDirs(result["I"].as<std::vector<std::string>>());
    }
    resolveLinkDecls(prog, libs);

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
      return ExitCode::StaticError;
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

  } catch (const UndefinedBehaviorError &e) {
    std::cerr << e.what() << "\n";
    return ExitCode::UndefinedBehavior;
  } catch (const RequireViolationError &e) {
    std::cerr << "Requirement failed: " << e.what() << "\n";
    return ExitCode::RequireViolation;
  } catch (const LexError &e) {
    printMessage(std::cerr, src, e.span, e.what(), DiagLevel::Error);
    return ExitCode::LexError;
  } catch (const ParseError &e) {
    printMessage(std::cerr, src, e.span, e.what(), DiagLevel::Error);
    return ExitCode::ParseError;
  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return ExitCode::Error;
  }

  return 0;
}
