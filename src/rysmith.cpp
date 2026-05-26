/**
 * rysmith — C++ random SymIR leaf-function generator (v2).
 *
 * Builds SymIR Programs directly in memory (no text generation/parsing),
 * then calls SymbolicExecutor in-process (no subprocess) to concretize them.
 *
 * Pipeline per leaf function:
 *   S1. Random CFG with n interior blocks
 *   S2. Sample a random execution path (EP)
 *   S3. Build symbolic Program AST directly (func_gen)
 *   S4. Validate (SemChecker + TypeChecker) and solve in-process
 *   S5. Emit concrete .sir via SIRPrinter
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "analysis/pass_manager.hpp"
#include "ast/sir_printer.hpp"
#include "cxxopts.hpp"
#include "frontend/diagnostics.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"
#include "reify/cfg_gen.hpp"
#include "reify/func_gen.hpp"
#include "reify/path_sampler.hpp"
#include "reify/var_catalogue.hpp"
#include "solver/solver.hpp"
#if defined(USE_BITWUZLA)
#include "solver/bitwuzla_impl.hpp"
#elif defined(USE_ALIVESMT)
#include "solver/alive_impl.hpp"
#endif

namespace fs = std::filesystem;
using namespace symir;
using namespace symir::reify;

// Parse "[lo, hi]" domain string
static std::pair<int64_t, int64_t> parseDomain(const std::string &s) {
  if (s.size() < 5 || s.front() != '[' || s.back() != ']')
    throw std::invalid_argument("invalid domain (expected [lo, hi]): " + s);
  auto inner = s.substr(1, s.size() - 2);
  auto comma = inner.find(',');
  if (comma == std::string::npos)
    throw std::invalid_argument("invalid domain (missing comma): " + s);
  auto loStr = inner.substr(0, comma);
  auto hiStr = inner.substr(comma + 1);
  while (!loStr.empty() && std::isspace((unsigned char) loStr.back()))
    loStr.pop_back();
  while (!hiStr.empty() && std::isspace((unsigned char) hiStr.front()))
    hiStr = hiStr.substr(1);
  return {std::stoll(loStr), std::stoll(hiStr)};
}

static auto makeSolverFactory() {
  return [](const SymbolicExecutor::Config &cfg) -> std::unique_ptr<smt::ISolver> {
#if defined(USE_BITWUZLA)
    return std::make_unique<solver::BitwuzlaSolver>(cfg.timeout_ms, cfg.seed, cfg.num_smt_threads);
#elif defined(USE_ALIVESMT)
    return std::make_unique<solver::AliveSolver>(cfg.timeout_ms, cfg.seed, cfg.num_smt_threads);
#else
    (void) cfg;
    throw std::runtime_error("No solver backend compiled in");
#endif
  };
}

static bool validateWithSymiri(
    const fs::path &symiriPath, const fs::path &sirPath, const std::string &funcName, bool verbose
) {
  std::string cmd =
      "\"" + symiriPath.string() + "\" --main @" + funcName + " \"" + sirPath.string() + "\"";
  if (!verbose)
    cmd += " > /dev/null 2>&1";
  int status = std::system(cmd.c_str());
  return status == 0;
}

static bool compileWithSymirc(
    const fs::path &symircPath, const fs::path &sirPath, const std::string &target,
    const fs::path &outPath, bool noRequire, const std::string &vecLowering, bool verbose
) {
  std::string cmd = "\"" + symircPath.string() + "\" \"" + sirPath.string() + "\" --target " +
                    target + " -o \"" + outPath.string() + "\"";
  if (noRequire)
    cmd += " --no-require";
  if (!vecLowering.empty())
    cmd += " --vec-lowering " + vecLowering;
  if (!verbose)
    cmd += " 2>/dev/null";
  int status = std::system(cmd.c_str());
  return status == 0;
}

static std::string pickVecLowering(std::mt19937 &rng, const std::string &requested) {
  if (requested != "random")
    return requested;
  static const char *strategies[] = {"vecext", "scalars", "array", "structscalars", "structarray"};
  std::uniform_int_distribution<int> d(0, 4);
  return strategies[d(rng)];
}

struct GenerateResult {
  std::vector<fs::path> produced;
};

static GenerateResult generateLeaf(
    // CFG params
    int nBbls, double pBranch, double pBackedge,
    // Path params
    int maxLoopIter, int minLoopIter,
    // Var params (varCfg.typeConfig contains the type generation configuration)
    const VarGenConfig &varCfg,
    // Func params
    const std::string &funcName, int nStmts, bool safeOffPath, bool enableInterestCoefs,
    int64_t coefLo, int64_t coefHi, int64_t valueLo, int64_t valueHi, int64_t indexLo,
    int64_t indexHi, const ExprGenConfig &exprCfg,
    // Solver params
    uint32_t timeoutMs,
    // Retry params
    int maxRetries, int nInits,
    // IO
    const fs::path &outDir, bool keepSymbolic, bool verbose,
    // RNG (by value — safe to run in a detached thread)
    std::mt19937 rng, uint32_t baseSeed
) {
  // S1: CFG
  GenCFGParams cfgParams;
  cfgParams.nBbls = nBbls;
  cfgParams.seed = rng();
  cfgParams.pBranch = pBranch;
  cfgParams.pBackedge = pBackedge;
  auto cfg = genCFG(cfgParams);

  if (verbose)
    std::cout << "[cfg] " << cfg.blocks.size() << " blocks\n";

  // Generate VarCatalogue (shared across all inits for the same CFG)
  VarCatalogue vars = genVarCatalogue(rng, varCfg);

  if (verbose)
    std::cout << "[vars] " << vars.vars.size() << " vars, " << vars.structDecls.size()
              << " structs\n";

  for (int attempt = 0; attempt <= maxRetries; attempt++) {
    // Path sampling (reduce loop iterations on retry)
    SamplePathParams pathParams;
    pathParams.seed = rng();
    // Keep max ≥ min so retry decay can't violate the requested minimum.
    pathParams.maxLoopIter = std::max(minLoopIter, maxLoopIter - attempt);
    pathParams.minLoopIter = minLoopIter;

    auto maybePath = samplePath(cfg, pathParams);
    if (!maybePath) {
      if (verbose)
        std::cerr << "[sampler] attempt=" << attempt
                  << " sample failed (minLoopIter=" << minLoopIter
                  << ", maxLoopIter=" << pathParams.maxLoopIter << ")\n";
      continue;
    }
    const auto &path = *maybePath;

    if (verbose)
      std::cout << "[sampler] attempt=" << attempt << " EP len=" << path.size() << "\n";

    // Generate nInits independently-seeded programs
    std::vector<fs::path> produced;
    for (int initIdx = 0; initIdx < nInits; initIdx++) {
      FuncGenConfig fcfg;
      fcfg.funcName = funcName;
      fcfg.seed = rng();
      fcfg.nStmts = nStmts;
      fcfg.safeOffPath = safeOffPath;
      fcfg.enableInterestCoefs = enableInterestCoefs;
      fcfg.enableInterestInits = true;
      fcfg.exprCfg = exprCfg;
      fcfg.coefLo = coefLo;
      fcfg.coefHi = coefHi;
      fcfg.valueLo = valueLo;
      fcfg.valueHi = valueHi;
      fcfg.indexLo = indexLo;
      fcfg.indexHi = indexHi;

      auto [prog, pathLabels] = genFunction(cfg, path, vars, fcfg);

      auto writePathHeader = [&](std::ostream &os) {
        os << "// path:";
        for (std::size_t k = 0; k < path.size(); k++)
          os << (k == 0 ? " " : " -> ") << path[k];
        os << "\n\n";
      };

      // Optionally dump symbolic program
      if (keepSymbolic) {
        auto symPath = outDir / (funcName + "_sym" + std::to_string(initIdx) + ".sir");
        std::ofstream ofs(symPath);
        writePathHeader(ofs);
        SIRPrinter printer(ofs);
        printer.print(prog);
        if (verbose)
          std::cout << "  symbolic: " << symPath << "\n";
      }

      // Validate AST
      DiagBag diags;
      PassManager pm(diags);
      pm.addModulePass(std::make_unique<SemChecker>());
      pm.addModulePass(std::make_unique<TypeChecker>());
      if (pm.run(prog) == PassResult::Error) {
        if (verbose) {
          std::cerr << "[validate] init " << initIdx << ": generated program failed validation\n";
          for (const auto &d: diags.diags)
            if (d.level == DiagLevel::Error)
              std::cerr << "  error: " << d.message << "\n";
        }
        continue;
      }

      // Solve
      SymbolicExecutor::Config solverCfg;
      solverCfg.timeout_ms = timeoutMs;
      solverCfg.seed = baseSeed + (uint32_t) (attempt * 100 + initIdx);
      solverCfg.num_threads = 1;
      solverCfg.num_smt_threads = 1;

      SymbolicExecutor executor(prog, solverCfg, makeSolverFactory());
      SymbolicExecutor::Result res;
      try {
        res = executor.solve("@" + funcName, pathLabels);
      } catch (const std::exception &e) {
        if (verbose)
          std::cerr << "[solver] init " << initIdx << ": exception: " << e.what() << "\n";
        res.unknown = true;
        continue;
      } catch (...) {
        if (verbose)
          std::cerr << "[solver] init " << initIdx << ": unknown exception\n";
        res.unknown = true;
        continue;
      }

      if (res.sat) {
        std::string outName =
            nInits > 1 ? funcName + "_" + std::to_string(initIdx) + ".sir" : funcName + ".sir";
        auto concretePath = outDir / outName;
        {
          std::ofstream ofs(concretePath);
          if (!ofs) {
            std::cerr << "error: cannot open " << concretePath << "\n";
            continue;
          }
          writePathHeader(ofs);
          SIRPrinter printer(ofs, res.model);
          printer.print(prog);
        }
        produced.push_back(concretePath);
        if (verbose)
          std::cout << "[emit] init " << initIdx << ": " << concretePath << "\n";
      } else if (verbose) {
        std::cerr << "[solver] init " << initIdx << ": " << (res.unsat ? "UNSAT" : "UNKNOWN")
                  << "\n";
      }
    }

    if (!produced.empty())
      return GenerateResult{std::move(produced)};

    if (verbose)
      std::cerr << "[solver] attempt=" << attempt << ": all inits failed, retrying\n";
  }

  return GenerateResult{};
}

int main(int argc, char **argv) {
  cxxopts::Options opts("rysmith", "rysmith — C++ random SymIR leaf-function generator");

  // clang-format off
  opts.add_options()
    ("n,n-funcs",         "Number of leaf functions to generate",
                          cxxopts::value<int>()->default_value("1"))
    // Type control
    ("no-fp",             "Disable f32/f64 types entirely")
    ("no-vec",            "Disable <N> T vector type generation")
    ("no-agg-ptr",        "Disable ptr [N] T / ptr @S aggregate pointer generation")
    ("vec-lowering",      "Vec-lowering strategy for C backend (random|vecext|scalars|array|structscalars|structarray)",
                          cxxopts::value<std::string>()->default_value("random"))
    ("max-ptr-depth",     "Maximum pointer nesting depth (0 disables pointers)",
                          cxxopts::value<int>()->default_value("2"))
    ("max-agg-nest",      "Maximum aggregate nesting depth",
                          cxxopts::value<int>()->default_value("2"))
    ("max-agg-elems",     "Maximum array size and struct field count",
                          cxxopts::value<int>()->default_value("3"))
    // Generation
    ("n-vars",            "Variables per function",
                          cxxopts::value<int>()->default_value("10"))
    ("n-stmts",           "Statements per block on path",
                          cxxopts::value<int>()->default_value("3"))
    ("safe-off-path",     "Add UB guards in off-path code")
    // Operators
    ("no-divmod",         "Disable integer division and modulo")
    ("no-select",         "Disable select ternary expressions")
    // CFG
    ("n-bbls",            "Basic blocks between entry and exit per CFG",
                          cxxopts::value<int>()->default_value("15"))
    ("p-branch",          "Probability of two-successor block",
                          cxxopts::value<double>()->default_value("0.5"))
    ("p-backedge",        "Probability of back-edge",
                          cxxopts::value<double>()->default_value("0.3"))
    // Solver
    ("timeout",           "SMT solver timeout per attempt in ms",
                          cxxopts::value<uint32_t>()->default_value("2000"))
    ("seed",              "Master RNG seed (default: random)",
                          cxxopts::value<uint32_t>())
    // Domains
    ("coef-domain",       "Domain for coef symbols",
                          cxxopts::value<std::string>()->default_value("[-2147483647, 2147483647]"))
    ("value-domain",      "Domain for value/constant symbols",
                          cxxopts::value<std::string>()->default_value("[-2147483647, 2147483647]"))
    ("index-domain",      "Domain for index symbols",
                          cxxopts::value<std::string>()->default_value("[1, 30]"))
    // Retry/inits
    ("n-inits",           "Concretizations per template (different seeds)",
                          cxxopts::value<int>()->default_value("3"))
    ("max-retries",       "Retry attempts on solver failure",
                          cxxopts::value<int>()->default_value("2"))
    ("max-loop-iter",     "Max loop iterations in the execution path (EP) sample",
                          cxxopts::value<int>()->default_value("1"))
    ("min-loop-iter",     "Require at least one loop in the EP to iterate this many times",
                          cxxopts::value<int>())
    // Output
    ("o,output-dir",      "Output directory",
                          cxxopts::value<std::string>()->default_value("reify_out"))
    ("target",            "Compile concrete .sir to target (sir, c, wasm); sir = no compilation",
                          cxxopts::value<std::string>()->default_value("sir"))
    ("keep-require",      "Include require checks in compiled output (default: omitted)")
    ("keep-symbolic",     "Write intermediate symbolic .sir files to disk")
    ("validate",          "Run symiri on each concrete .sir to validate")
    ("v,verbose",         "Verbose output")
    ("h,help",            "Print usage");
  // clang-format on

  cxxopts::ParseResult result;
  try {
    result = opts.parse(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n" << opts.help() << "\n";
    return 1;
  }

  if (result.count("help")) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  // ---- Parse domains -------------------------------------------------------
  int64_t coefLo, coefHi, valueLo, valueHi, indexLo, indexHi;
  try {
    auto [clo, chi] = parseDomain(result["coef-domain"].as<std::string>());
    auto [vlo, vhi] = parseDomain(result["value-domain"].as<std::string>());
    auto [ilo, ihi] = parseDomain(result["index-domain"].as<std::string>());
    coefLo = clo;
    coefHi = chi;
    valueLo = vlo;
    valueHi = vhi;
    indexLo = ilo;
    indexHi = ihi;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  // ---- Setup ---------------------------------------------------------------
  uint32_t masterSeed =
      result.count("seed") ? result["seed"].as<uint32_t>() : (uint32_t) std::random_device{}();
  std::cout << "rysmith: master seed = " << masterSeed << "\n";
  std::mt19937 rng(masterSeed);

  fs::path outDir = result["output-dir"].as<std::string>();
  fs::create_directories(outDir);

  // Type config
  TypeGenConfig typeCfg;
  typeCfg.enableFp = !result.count("no-fp");
  typeCfg.enableVec = !result.count("no-vec");
  typeCfg.enableAggPtr = !result.count("no-agg-ptr");
  typeCfg.maxPtrDepth = result["max-ptr-depth"].as<int>();
  typeCfg.maxAggNesting = result["max-agg-nest"].as<int>();
  typeCfg.maxAggElems = result["max-agg-elems"].as<int>();

  // Var config
  VarGenConfig varCfg;
  varCfg.nVars = result["n-vars"].as<int>();
  varCfg.typeConfig = typeCfg;

  // Expr config
  ExprGenConfig exprCfg;
  exprCfg.enableAllOps = true; // always enable all ops by default
  exprCfg.enableDiv = !result.count("no-divmod");
  exprCfg.enableSelect = !result.count("no-select");
  exprCfg.enableFp = typeCfg.enableFp;

  int nFuncs = result["n-funcs"].as<int>();
  int nBbls = result["n-bbls"].as<int>();
  int nStmts = result["n-stmts"].as<int>();
  int maxLoopIter = result["max-loop-iter"].as<int>();
  int minLoopIter = result.count("min-loop-iter") ? result["min-loop-iter"].as<int>() : 0;
  int nInits = result["n-inits"].as<int>();
  int maxRetries = result["max-retries"].as<int>();
  double pBranch = result["p-branch"].as<double>();
  double pBackedge = result["p-backedge"].as<double>();
  bool safeOffPath = result.count("safe-off-path") > 0;
  bool enableInterestCoefs = true; // kept in code; not user-exposed
  uint32_t timeoutMs = result["timeout"].as<uint32_t>();
  // Wall-clock budget per function: covers all retries × inits plus 50 ms for non-solver overhead
  // (CFG gen, path sampling, formula construction, SIRPrinter). Compilation runs outside the
  // thread.
  uint32_t funcTimeoutMs = (uint32_t) ((uint64_t) (maxRetries + 1) * nInits * timeoutMs + 50);
  bool keepSymbolic = result.count("keep-symbolic") > 0;
  bool doValidate = result.count("validate") > 0;
  bool verbose = result.count("verbose") > 0;
  std::string target = result["target"].as<std::string>();
  bool noRequire = !result.count("keep-require");
  std::string vecLoweringOpt = result["vec-lowering"].as<std::string>();

  if (target != "sir" && target != "c" && target != "wasm") {
    std::cerr << "error: unknown target '" << target << "' (expected sir, c, wasm)\n";
    return 1;
  }

  // Find symiri for validation (sibling of this binary)
  fs::path symiriPath;
  if (doValidate) {
    symiriPath = fs::path(argv[0]).parent_path() / "symiri";
    if (!fs::exists(symiriPath)) {
      std::cerr << "warning: symiri not found at " << symiriPath << " — disabling --validate\n";
      doValidate = false;
    }
  }

  // Find symirc for compilation
  fs::path symircPath;
  if (target != "sir") {
    symircPath = fs::path(argv[0]).parent_path() / "symirc";
    if (!fs::exists(symircPath)) {
      std::cerr << "warning: symirc not found at " << symircPath << " — disabling --target\n";
      target = "sir";
    }
  }

  // ---- Main loop -----------------------------------------------------------
  auto wallStart = std::chrono::steady_clock::now();
  int nOk = 0, nFail = 0;

  for (int i = 0; i < nFuncs; i++) {
    std::string funcName = "func" + std::to_string(i);
    uint32_t funcSeed = rng();
    std::cout << "[" << (i + 1) << "/" << nFuncs << "] generating " << funcName
              << " (seed=" << funcSeed << ")\n";

    // Heap-allocated state lets us safely detach the thread on timeout without
    // dangling references. Leaked on timeout — bounded by nFuncs, cleaned at exit.
    struct FuncState {
      std::mt19937 rng;
      GenerateResult result;
      std::atomic<bool> done{false};
    };

    auto *state = new FuncState{std::mt19937(funcSeed), {}, false};

    std::thread t([&, state]() {
      state->result = generateLeaf(
          nBbls, pBranch, pBackedge, maxLoopIter, minLoopIter, varCfg, funcName, nStmts,
          safeOffPath, enableInterestCoefs, coefLo, coefHi, valueLo, valueHi, indexLo, indexHi,
          exprCfg, timeoutMs, maxRetries, nInits, outDir, keepSymbolic, verbose, state->rng,
          funcSeed
      );
      state->done.store(true, std::memory_order_release);
    });

    bool timedOut = false;
    if (funcTimeoutMs > 0) {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(funcTimeoutMs);
      while (!state->done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
          timedOut = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }

    if (timedOut) {
      t.detach(); // state leaked; thread completes or dies with the process
      std::cerr << "[TIMEOUT] " << funcName << " exceeded " << funcTimeoutMs << "ms wall clock\n";
      nFail++;
      continue;
    }
    t.join();
    GenerateResult genRes = std::move(state->result);
    delete state;

    if (genRes.produced.empty()) {
      std::cerr << "[FAIL] all attempts failed for " << funcName << " (seed=" << funcSeed << ")\n";
      nFail++;
      continue;
    }

    for (const auto &p: genRes.produced) {
      std::cout << "  concrete: " << p << "\n";

      if (target != "sir") {
        std::string ext = (target == "c") ? ".c" : ".wat";
        fs::path outPath = p.parent_path() / (p.stem().string() + ext);
        std::string vecLowering = pickVecLowering(rng, vecLoweringOpt);
        if (verbose && !vecLowering.empty())
          std::cout << "  vec-lowering: " << vecLowering << "\n";
        bool ok =
            compileWithSymirc(symircPath, p, target, outPath, noRequire, vecLowering, verbose);
        if (ok)
          std::cout << "  compiled: " << outPath << "\n";
        else
          std::cerr << "  compile FAIL: " << p << "\n";
      }
    }

    if (doValidate) {
      bool allOk = true;
      for (const auto &p: genRes.produced) {
        // Strip "_N" suffix from stem to get base function name
        std::string stem = p.stem().string();
        std::string baseFuncName = stem;
        if (auto pos = stem.rfind('_'); pos != std::string::npos) {
          bool isNum = true;
          for (std::size_t j = pos + 1; j < stem.size(); j++)
            if (!std::isdigit((unsigned char) stem[j])) {
              isNum = false;
              break;
            }
          if (isNum)
            baseFuncName = stem.substr(0, pos);
        }
        bool ok = validateWithSymiri(symiriPath, p, baseFuncName, verbose);
        std::cout << "  validated: " << (ok ? "OK" : "FAIL") << " (" << p.filename() << ")\n";
        if (!ok) {
          allOk = false;
          nFail++;
          break;
        }
      }
      if (allOk)
        nOk++;
    } else {
      nOk++;
    }
  }

  auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
  double throughput = elapsed > 0 ? nOk / elapsed : 0.0;
  std::cout << "\nDone: " << nOk << " succeeded, " << nFail << " failed (total " << nFuncs << ")"
            << "  [" << elapsed << "s, " << throughput << " funcs/s]\n";

  return nFail == 0 ? 0 : 1;
}
