/**
 * rylink — whole-program generator over a rysmith function pool (v0.2.2).
 *
 * Pipeline per generated program:
 *   1. Pick K functions from the pool (K from --n-nodes, capped by pool size).
 *   2. Build a DAG call-graph over those nodes (cg_gen).
 *   3. Parse each chosen .sir and merge into one bundled Program
 *      (deduplicating struct decls by name; rysmith already namespaces
 *      structs by genID so collisions only happen across same-id picks).
 *   4. For each (caller→callee) edge in the CG, drive the RewriteEngine
 *      to splice a `call @callee(args)` into the caller body.
 *   5. Emit `prog_<id>_<i>/program.sir` (bundled, with CG/PARAMS/RET
 *      header comments). The bundled file is the source of truth for
 *      every downstream consumer.
 *   6. (--target c) Invoke symirc --split-by-source on program.sir to
 *      emit common.h + one .c per FunDecl::sourceStem.
 *   7. (--validate) Run symiri on program.sir with the entry's solved
 *      parameter values and assert the returned value equals the entry
 *      descriptor's ret.
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "analysis/definite_init.hpp"
#include "analysis/pass_manager.hpp"
#include "analysis/reachability.hpp"
#include "analysis/unused_name.hpp"
#include "ast/sir_printer.hpp"
#include "backend/c_backend.hpp"
#include "backend/vec_lowering.hpp"
#include "backend/wasm_backend.hpp"
#include "cxxopts.hpp"
#include "frontend/diagnostics.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"
#include "reify/cg_gen.hpp"
#include "reify/func_desc.hpp"
#include "reify/func_pool.hpp"
#include "reify/gen_id.hpp"
#include "reify/hyperparameters.hpp"
#include "reify/rewrite.hpp"

namespace fs = std::filesystem;
using namespace symir;
using namespace symir::reify;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::string readFile(const fs::path &p) {
  std::ifstream ifs(p);
  std::stringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

// Parse a rysmith-emitted .sir file into a Program. We deliberately skip
// the heavy analysis passes (sem/type/reachability) — rysmith produced
// these files itself and they're trusted well-formed.
static std::optional<Program> parseSir(const fs::path &p) {
  std::string src = readFile(p);
  try {
    Lexer lx(src);
    auto toks = lx.lexAll();
    Parser ps(std::move(toks));
    return ps.parseProgram();
  } catch (const std::exception &e) {
    std::cerr << "rylink: parse failed for " << p << ": " << e.what() << "\n";
    return std::nullopt;
  }
}

// ---------------------------------------------------------------------------
// Bundle merging
// ---------------------------------------------------------------------------

struct Node {
  int idx;               // index in the CG
  size_t poolIdx;        // index into FuncPool::entries
  size_t realizationIdx; // which realization we picked
  std::string funcName;  // e.g. "@func_<id>_<i>"
  std::string stem;      // basename without ".sir" — used for sourceStem
  FunDecl *fn = nullptr; // points into the merged bundle's funs
};

// Pick K entries from the pool without replacement (capped by pool size).
// Duplicates would require renaming support we don't have yet, so when K
// exceeds the pool we shrink K rather than reuse entries.
static std::vector<size_t> pickPoolIndices(std::mt19937 &rng, size_t k, size_t poolSize) {
  std::vector<size_t> all(poolSize);
  for (size_t i = 0; i < poolSize; ++i)
    all[i] = i;
  std::shuffle(all.begin(), all.end(), rng);
  if (k > poolSize)
    k = poolSize;
  all.resize(k);
  return all;
}

// Merge a per-fn Program into the bundle. Returns a pointer to the
// just-moved FunDecl, or nullptr on failure. Struct decls are
// deduplicated by name (rysmith already namespaces structs by genID so
// same-name dups are guaranteed identical; the first wins).
//
// Pointer-stability precondition: `bundle.funs` MUST have been reserved
// to its final size before the first call. The returned FunDecl* is
// held by the per-program Node table across subsequent merges; without
// the reserve a later push_back would reallocate and invalidate every
// earlier pointer. Assert rather than silently corrupt.
static FunDecl *mergeInto(
    Program &bundle, Program src, const std::string &sourceStem,
    std::unordered_set<std::string> &haveStructs
) {
  assert(
      bundle.funs.size() < bundle.funs.capacity() &&
      "rylink: bundle.funs must be reserved upfront — see generateOne"
  );
  for (auto &s: src.structs)
    if (haveStructs.insert(s.name.name).second)
      bundle.structs.push_back(std::move(s));
  if (src.funs.size() != 1)
    return nullptr; // rysmith always emits exactly one fun per file
  bundle.funs.push_back(std::move(src.funs[0]));
  FunDecl *fn = &bundle.funs.back();
  fn->sourceStem = sourceStem;
  return fn;
}

// ---------------------------------------------------------------------------
// Emit
// ---------------------------------------------------------------------------

// Write the bundled program.sir with header comments listing the call
// graph, the entry's solved parameter values, and the entry's solved
// return value. These comments are informational — symiri and symirc
// ignore them — but make the file self-describing for inspection.
static void writeBundledSir(
    const fs::path &outPath, const Program &bundle, const CallGraph &cg,
    const std::vector<Node> &nodes, const FuncDescriptor::Realization &entryRz
) {
  std::ofstream ofs(outPath);
  ofs << "// ENTRY: " << nodes[cg.entry()].funcName << "\n";
  ofs << "// CG:\n";
  for (int i = 0; i < cg.nNodes; ++i) {
    ofs << "//   n" << i << " " << nodes[i].funcName;
    if (!cg.outEdges[i].empty()) {
      ofs << " ->";
      for (int j: cg.outEdges[i])
        ofs << " n" << j;
    }
    ofs << "\n";
  }
  ofs << "// PARAMS:";
  if (entryRz.paramValues.empty())
    ofs << " (none)";
  for (const auto &pv: entryRz.paramValues)
    ofs << " " << pv.first << "=" << pv.second;
  ofs << "\n";
  ofs << "// RETURN: " << (entryRz.retValue.empty() ? "(none)" : entryRz.retValue) << "\n\n";
  SIRPrinter sp(ofs);
  sp.print(bundle);
}

// ---------------------------------------------------------------------------
// In-process backend emission
//
// rylink keeps the bundled Program in memory and drives CBackend /
// WasmBackend directly. Going through symirc-as-subprocess would
// require writing the bundle to text and re-parsing it, which loses
// FunDecl::sourceStem (the AST field isn't serialised by SIRPrinter)
// — and that single fact is what makes --split-by-source useful for
// rylink in the first place. Calling the backends here also drops
// every cross-process boundary so symirc-side log lines (e.g. the
// old `wrote …` per-file message) stop leaking into rylink's output.
// ---------------------------------------------------------------------------

// Run the same analysis passes symirc runs before backend emission.
// Returns true iff the program is well-formed; on failure the errors
// are dropped (the caller treats a failed generateOne as a retry).
static bool runAnalysisPasses(Program &prog, bool verbose) {
  DiagBag diags;
  PassManager pm(diags);
  pm.addModulePass(std::make_unique<SemChecker>());
  pm.addModulePass(std::make_unique<TypeChecker>());
  pm.addFunctionPass(std::make_unique<ReachabilityAnalysis>());
  pm.addFunctionPass(std::make_unique<DefiniteInitAnalysis>());
  pm.addFunctionPass(std::make_unique<UnusedNameAnalysis>());
  if (pm.run(prog) == PassResult::Error) {
    if (verbose) {
      std::cerr << "rylink: analysis passes failed:\n";
      for (const auto &d: diags.diags)
        if (d.level == DiagLevel::Error)
          std::cerr << "  error: " << d.message << "\n";
    }
    return false;
  }
  return true;
}

// Emit the bundled program as one .c per distinct FunDecl::sourceStem
// plus common.h, in `outDir`. `primaryStem` keys the .c file that
// holds funs with empty sourceStem (none in rylink's case — every
// merged fn carries the .sir stem it came from — but the parameter
// is still required by CBackend::emitSplit).
static bool emitCInProcess(
    Program &prog, const fs::path &outDir, const std::string &primaryStem, bool keepRequire,
    bool verbose
) {
  if (!runAnalysisPasses(prog, verbose))
    return false;
  // Suppress stdout chatter inside the backend; CBackend writes to its
  // own streams and any extra logging we add via `verbose` belongs to
  // rylink, not the backend. The emit itself prints nothing.
  std::ofstream sink;
  CBackend cb(sink);
  cb.setNoRequire(!keepRequire);
  cb.setVecLowering(makeVecLowering("vecext"));
  try {
    cb.emitSplit(prog, outDir.string(), primaryStem);
  } catch (const std::exception &e) {
    if (verbose)
      std::cerr << "rylink: CBackend failed: " << e.what() << "\n";
    return false;
  }
  return true;
}

static bool
emitWasmInProcess(Program &prog, const fs::path &outFile, bool keepRequire, bool verbose) {
  if (!runAnalysisPasses(prog, verbose))
    return false;
  std::ofstream ofs(outFile);
  if (!ofs) {
    if (verbose)
      std::cerr << "rylink: cannot open " << outFile << "\n";
    return false;
  }
  WasmBackend wb(ofs);
  wb.setNoRequire(!keepRequire);
  try {
    wb.emit(prog);
  } catch (const std::exception &e) {
    if (verbose)
      std::cerr << "rylink: WasmBackend failed: " << e.what() << "\n";
    return false;
  }
  return true;
}

// Run symiri on the bundled program with the given entry function and
// positional param args. Captures stdout and extracts the "Result: <v>"
// line. Returns nullopt on any tool failure or missing Result line.
static std::optional<std::string> runSymiri(
    const fs::path &symiri, const fs::path &programSir, const std::string &entryFn,
    const std::vector<std::string> &paramArgs
) {
  std::string outPath = "/tmp/rylink_symiri_" + std::to_string(::getpid()) + ".out";
  // Wrap in `timeout` because symiri has no internal wall-clock limit;
  // a generated program with an unbounded loop can hang the validator
  // and stall the batch. 10s is plenty for a properly-terminating run.
  std::string cmd = "timeout 10s \"" + symiri.string() + "\" --main " + entryFn + " \"" +
                    programSir.string() + "\"";
  if (!paramArgs.empty()) {
    cmd += " --";
    for (const auto &a: paramArgs)
      cmd += " " + a;
  }
  cmd += " > " + outPath + " 2>&1";
  int rc = std::system(cmd.c_str());
  std::string out = readFile(outPath);
  fs::remove(outPath);
  if (rc != 0)
    return std::nullopt;
  auto pos = out.find("Result:");
  if (pos == std::string::npos)
    return std::nullopt;
  std::string tail = out.substr(pos + 7);
  size_t i = 0;
  while (i < tail.size() && std::isspace((unsigned char) tail[i]))
    ++i;
  size_t j = tail.size();
  while (j > i && (tail[j - 1] == '\n' || tail[j - 1] == '\r'))
    --j;
  return tail.substr(i, j - i);
}

// ---------------------------------------------------------------------------
// Per-program pipeline
// ---------------------------------------------------------------------------

struct PerProgConfig {
  int nNodes = 4;
  double pEdge = rylink::hp::kPEdge;
  int maxOutDeg = rylink::hp::kMaxOutDegree;
  std::string genId;
  int progIdx = 0;
  fs::path outRoot;
  std::string target; // "sir" | "c" | "wasm"
  bool keepRequire = false;
  bool validate = false;
  fs::path symiriPath;
  bool verbose = false;
};

static bool generateOne(const FuncPool &pool, std::mt19937 &rng, const PerProgConfig &cfg) {
  if (pool.entries.empty())
    return false;
  int k = std::min((int) pool.entries.size(), std::max(1, cfg.nNodes));
  auto pickIdxs = pickPoolIndices(rng, (size_t) k, pool.entries.size());

  CGGenConfig cgCfg{k, cfg.pEdge, cfg.maxOutDeg};
  CallGraph cg = genCallGraph(rng, cgCfg);

  // Build the bundle by parsing each chosen .sir.
  // Reserve funs upfront so Node::fn pointers stay valid across the
  // subsequent push_backs — without this, vector growth invalidates
  // every earlier pointer and the rewrite phase reads freed memory.
  Program bundle;
  bundle.funs.reserve(k);
  std::unordered_set<std::string> haveStructs;
  std::vector<Node> nodes(k);
  for (int i = 0; i < k; ++i) {
    nodes[i].idx = i;
    nodes[i].poolIdx = pickIdxs[i];
    const auto &entry = pool.entries[pickIdxs[i]];
    std::uniform_int_distribution<size_t> rd(0, entry.desc.realizations.size() - 1);
    nodes[i].realizationIdx = rd(rng);
    fs::path sirPath = entry.sirPaths[nodes[i].realizationIdx];
    nodes[i].funcName = entry.desc.name;
    nodes[i].stem = sirPath.stem().string();
    auto prog = parseSir(sirPath);
    if (!prog)
      return false;
    nodes[i].fn = mergeInto(bundle, std::move(*prog), nodes[i].stem, haveStructs);
    if (!nodes[i].fn)
      return false;
  }

  // Run the rewrite engine for each edge.
  RewriteEngine engine;
  engine.addRule(makeLiteralToCallRule());
  for (int i = 0; i < cg.nNodes; ++i) {
    for (int j: cg.outEdges[i]) {
      engine.rewriteEdge(
          *nodes[i].fn, pool.entries[nodes[j].poolIdx].desc, nodes[j].realizationIdx, rng
      );
    }
  }

  // Create the output dir and emit program.sir.
  fs::path progDir = cfg.outRoot / (std::string(rylink::hp::kProgPrefix) + "_" + cfg.genId + "_" +
                                    std::to_string(cfg.progIdx));
  fs::create_directories(progDir);
  fs::path programSir = progDir / rylink::hp::kEntrySirName;

  // Pull entry's solved param/ret values from its descriptor for header.
  const auto &entryEntry = pool.entries[nodes[cg.entry()].poolIdx];
  const auto &entryRz = entryEntry.desc.realizations[nodes[cg.entry()].realizationIdx];
  writeBundledSir(programSir, bundle, cg, nodes, entryRz);
  // Always echo the bundled .sir path so the user can find it in the
  // common case (target=sir). Matches rysmith's per-artifact
  // `concrete: <path>` line.
  std::cout << "  bundled: " << programSir << "\n";

  // Target backends. Both code paths run the analysis pipeline on the
  // in-memory bundle and call CBackend / WasmBackend directly so
  // FunDecl::sourceStem (set during mergeInto) survives into emitSplit
  // — going through symirc-as-subprocess would round-trip the bundle
  // through text and collapse every sourceStem to "".
  if (cfg.target == "c") {
    // primaryStem = "program": the bundled entry program's "virtual"
    // .sir file. No fn has empty sourceStem (every merge sets one),
    // so the resulting "program.c" stays empty of bodies and the
    // per-source .c files carry the funs.
    if (!emitCInProcess(bundle, progDir, "program", cfg.keepRequire, cfg.verbose)) {
      if (cfg.verbose)
        std::cerr << "  backend FAIL (" << progDir.filename().string() << ")\n";
      return false;
    }
    // Mirror rysmith's `compiled: <path>` per-emission line. We report
    // the per-program directory because --split-by-source produces
    // multiple .c files there.
    std::cout << "  compiled: " << progDir << "\n";
  } else if (cfg.target == "wasm") {
    fs::path wasmOut = progDir / "program.wasm";
    if (!emitWasmInProcess(bundle, wasmOut, cfg.keepRequire, cfg.verbose)) {
      if (cfg.verbose)
        std::cerr << "  backend FAIL (" << progDir.filename().string() << ")\n";
      return false;
    }
    std::cout << "  compiled: " << wasmOut << "\n";
  }

  // Validate: run symiri with the entry's param realization values and
  // assert the returned value matches the descriptor's solved ret.
  if (cfg.validate) {
    std::vector<std::string> args;
    for (const auto &pv: entryRz.paramValues)
      args.push_back(pv.second);
    auto got = runSymiri(cfg.symiriPath, programSir, nodes[cg.entry()].funcName, args);
    bool ok = got && (*got == entryRz.retValue);
    if (ok) {
      if (cfg.verbose)
        std::cout << "  validated: OK (" << progDir.filename().string() << ")\n";
    } else {
      if (cfg.verbose)
        std::cerr << "  validated: FAIL (" << progDir.filename().string()
                  << ") expected=" << entryRz.retValue
                  << " got=" << (got ? *got : std::string("<no Result>")) << "\n";
      return false;
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  cxxopts::Options opts("rylink", "rylink — whole-program generator for SymIR");
  // clang-format off
  opts.add_options()
    ("n,n-progs", "Number of whole programs to generate",
        cxxopts::value<int>()->default_value("1"))
    // CG
    ("n-nodes", "Target number of call-graph nodes per program",
        cxxopts::value<int>()->default_value("8"))
    ("max-outdeg", "Maximum out-degree per CG node",
        cxxopts::value<int>()->default_value(std::to_string(rylink::hp::kMaxOutDegree)))
    // Input and output
    ("i,input-dir", "Directory of rysmith-emitted (.sir + .json) pairs",
        cxxopts::value<std::string>()->default_value("rysmith_out"))
    ("o,output-dir", "Root output directory; each program lands in <root>/prog_<id>_<i>/",
        cxxopts::value<std::string>()->default_value("rylink_out"))
    ("target", "sir | c | wasm (default: sir)",
        cxxopts::value<std::string>()->default_value("sir"))
    ("keep-require", "Keep `require` checks in C/WASM output",
        cxxopts::value<bool>()->default_value("false"))
    // Validate
    ("validate", "Run symiri on each emitted program and check semantics",
        cxxopts::value<bool>()->default_value("false"))
    // Misc
    ("seed", "RNG seed (random if omitted)",
        cxxopts::value<uint32_t>())
    ("v,verbose", "Verbose output", cxxopts::value<bool>()->default_value("false"))
    ("h,help", "Print help");
  // clang-format on

  cxxopts::ParseResult res;
  try {
    res = opts.parse(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "rylink: " << e.what() << "\n" << opts.help() << "\n";
    return 2;
  }
  if (res.count("help")) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  uint32_t seed = res.count("seed") ? res["seed"].as<uint32_t>() : std::random_device{}();
  std::mt19937 rng(seed);
  // [v0.2.2] Generation ID is always derived from --seed via the
  // shared genHexId helper — no CLI override. Two runs with the same
  // seed produce identical prog_<id>_<i> directories.
  std::string genId = genHexId(rng);

  PerProgConfig pc;
  pc.outRoot = res["output-dir"].as<std::string>();
  pc.nNodes = res["n-nodes"].as<int>();
  pc.maxOutDeg = res["max-outdeg"].as<int>();
  pc.pEdge = rylink::hp::kPEdge;
  pc.genId = genId;
  pc.target = res["target"].as<std::string>();
  pc.keepRequire = res["keep-require"].as<bool>();
  pc.validate = res["validate"].as<bool>();
  pc.verbose = res["v"].as<bool>();

  if (pc.target != "sir" && pc.target != "c" && pc.target != "wasm") {
    std::cerr << "rylink: --target must be sir | c | wasm\n";
    return 2;
  }

  // Locate symiri/symirc as siblings of this binary (same pattern as
  // rysmith). Disable validate / non-sir targets gracefully if missing
  // rather than refusing to start.
  if (pc.validate) {
    pc.symiriPath = fs::path(argv[0]).parent_path() / "symiri";
    if (!fs::exists(pc.symiriPath)) {
      std::cerr << "rylink: symiri not found at " << pc.symiriPath << " — disabling --validate\n";
      pc.validate = false;
    }
  }
  // [v0.2.2] No `--target` sibling-binary discovery any more: the C
  // and WASM backends are linked directly into rylink, so all
  // target=c / target=wasm runs work without symirc on disk.

  std::cout << "rylink: seed = " << seed << "\n";
  std::cout << "rylink: generation id = " << genId << "\n";
  FuncPool pool = loadFuncPool(res["input-dir"].as<std::string>());
  if (pool.entries.empty()) {
    std::cerr << "rylink: empty pool — aborting\n";
    return 1;
  }
  std::cout << "rylink: pool size = " << pool.entries.size() << "\n";

  fs::create_directories(pc.outRoot);
  int nProgs = std::max(1, res["n-progs"].as<int>());
  int nOk = 0, nFail = 0;
  // Per-program retry budget — see rylink::hp::kMaxAttemptsPerProg for
  // the rationale. The rewrite engine itself is sound; this just
  // tolerates the occasional rng-induced downstream miss.
  constexpr int kMaxAttempts = rylink::hp::kMaxAttemptsPerProg;
  auto wallStart = std::chrono::steady_clock::now();
  for (int i = 0; i < nProgs; ++i) {
    pc.progIdx = i;
    std::string progName = "prog_" + genId + "_" + std::to_string(i);
    std::cout << "[" << (i + 1) << "/" << nProgs << "] generating " << progName << "\n";
    bool succeeded = false;
    int usedAttempts = 0;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
      ++usedAttempts;
      std::mt19937 progRng(rng());
      if (generateOne(pool, progRng, pc)) {
        succeeded = true;
        break;
      }
      if (pc.verbose)
        std::cerr << "  retry " << (attempt + 1) << "/" << kMaxAttempts << "\n";
    }
    if (succeeded) {
      ++nOk;
      // Per-program seal — matches rysmith's per-fn `compiled:` /
      // `validated:` discipline; the actual artefact paths were
      // already echoed by generateOne.
      std::cout << "  "
                << (usedAttempts > 1
                        ? "completed (after " + std::to_string(usedAttempts) + " attempts): "
                        : "completed: ")
                << progName << "\n";
    } else {
      ++nFail;
      std::cerr << "  [FAIL] " << progName << " after " << kMaxAttempts << " attempts\n";
    }
  }
  auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
  double throughput = elapsed > 0 ? nOk / elapsed : 0.0;
  std::cout << "\nDone: " << nOk << " succeeded, " << nFail << " failed (total " << nProgs << ")"
            << "  [" << elapsed << "s, " << throughput << " progs/s]\n";
  return nFail == 0 ? 0 : 1;
}
