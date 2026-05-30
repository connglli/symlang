"""Differential test: rysmith-generated programs interpreted by symiri vs.
compiled by symirc + gcc and run under UBSan. Catches bugs in any of
reify / interp / C-backend / solver by cross-checking their outputs.

Usage:
  python -m test.lib.run_reify_diff_tests \\
      --rysmith ./rysmith --symiri ./symiri --symirc ./symirc \\
      [--n 100] [--seed 1234] [--out build/test_tmp/reify_diff]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

from test.lib.style import bold, green, red, yellow

_FUN_RE = re.compile(r"fun\s+@([a-zA-Z0-9_]+)\(\)\s*:\s*(i[0-9]+|f32|f64)")
_RESULT_RE = re.compile(r"Result:\s*(-?[0-9]+)")

# rylink-bundled program.sir header lines + entry signature.
_ENTRY_RE = re.compile(r"^//\s*ENTRY:\s*@([a-zA-Z0-9_]+)")
_PARAMS_RE = re.compile(r"^//\s*PARAMS:(.*)$")
_RETURN_RE = re.compile(r"^//\s*RETURN:\s*(.+?)\s*$")
_FUN_SIG_RE = re.compile(
  r"fun\s+@([a-zA-Z0-9_]+)\s*\(([^)]*)\)\s*:\s*(i[0-9]+|f32|f64)"
)


_CRET = {
  "i8": "int8_t",
  "i16": "int16_t",
  "i32": "int32_t",
  "i64": "int64_t",
}

# Batch size for generation + cross-validation. Large -n runs proceed
# one batch at a time so the user sees periodic progress (and so a
# 10000-program run does not need to materialise 10000 .sir/.c files
# on disk before testing starts). Each batch uses seed = base + batch_idx
# so generated programs do not repeat across batches.
BATCH_SIZE = 100


def _parse_fun(sir_path):
  """Returns (func_name, ret_type) for the first function declared, or None."""
  with open(sir_path, "r") as f:
    for line in f:
      m = _FUN_RE.search(line)
      if m:
        return m.group(1), m.group(2)
  return None


def _write_main_c(path, fname, ret_type):
  cret = _CRET.get(ret_type)
  if cret is None:
    return False  # f32/f64 returns: skip (need printf %f and tolerance)
  with open(path, "w") as f:
    f.write(
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      f"extern {cret} symir_{fname}(void);\n"
      "int main(void) {\n"
      f"  {cret} r = symir_{fname}();\n"
      '  printf("Result: %lld\\n", (long long)r);\n'
      "  return 0;\n"
      "}\n"
    )
  return True


def _classify_one(sir_name, out_dir, symiri, clang, main_c, exe, verbose):
  """Run one (sir, c) pair and return (bucket, msg).

  ``bucket`` is one of: "passed", "skipped" (counter buckets, msg=None);
  or "mismatch", "ubsan", "cfail", "sirfail" (failure-list buckets, msg=str).
  """
  sir_path = os.path.join(out_dir, sir_name)
  c_path = sir_path[:-4] + ".c"
  if not os.path.exists(c_path):
    return ("cfail", f"{sir_name}: no .c emitted")

  fun = _parse_fun(sir_path)
  if fun is None:
    return ("sirfail", f"{sir_name}: cannot parse fun decl")
  fname, rtype = fun

  si = subprocess.run(
    [symiri, "--main", f"@{fname}", sir_path],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=10,
  )
  if si.returncode != 0:
    return ("sirfail", f"{sir_name}: symiri exit {si.returncode}")
  sm = _RESULT_RE.search(si.stdout) or _RESULT_RE.search(si.stderr)
  if sm is None:
    return ("sirfail", f"{sir_name}: symiri no Result line")
  sir_val = sm.group(1)

  if not _write_main_c(main_c, fname, rtype):
    return ("skipped", None)
  cc = subprocess.run(
    [
      clang,
      "-O0",
      c_path,
      main_c,
      "-o",
      exe,
      "-fsanitize=undefined",
      "-fno-sanitize-recover=all",
      "-w",
      "-lm",
    ],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=15,
  )
  if cc.returncode != 0:
    if verbose:
      print(cc.stderr)
    return ("cfail", f"{sir_name}: clang failed")
  cr = subprocess.run(
    [exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5
  )
  if cr.returncode != 0:
    head = cr.stderr.strip().splitlines()[0] if cr.stderr else "trap"
    return ("ubsan", f"{sir_name}: {head}")
  cm = _RESULT_RE.search(cr.stdout)
  if cm is None:
    return ("cfail", f"{sir_name}: no Result from C binary")
  c_val = cm.group(1)

  if sir_val == c_val:
    return ("passed", None)
  return ("mismatch", f"{sir_name}: symiri={sir_val} c={c_val}")


def _parse_rylink_prog(sir_path):
  """Parse a rylink-bundled program.sir. Returns
  (entry_name, ret_type, param_types[], param_values[], expected_ret) or None
  if any of those pieces are missing / malformed."""
  entry = None
  param_vals = None
  expected = None
  param_types = None
  ret_type = None
  with open(sir_path, "r") as f:
    for line in f:
      m = _ENTRY_RE.match(line)
      if m:
        entry = m.group(1)
        continue
      m = _PARAMS_RE.match(line)
      if m:
        body = m.group(1).strip()
        if body == "(none)":
          param_vals = []
        else:
          # Whitespace-separated `name=value` tokens.
          param_vals = [tok.split("=", 1)[1] for tok in body.split() if "=" in tok]
        continue
      m = _RETURN_RE.match(line)
      if m:
        v = m.group(1).strip()
        expected = None if v == "(none)" else v
        continue
      if entry is not None and ret_type is None:
        m = _FUN_SIG_RE.search(line)
        if m and m.group(1) == entry:
          ret_type = m.group(3)
          params_str = m.group(2).strip()
          if params_str:
            # `%pa0: i32, %pa1: i8, %pa2: f64` — keep types in order.
            param_types = [
              p.split(":", 1)[1].strip() for p in params_str.split(",") if ":" in p
            ]
          else:
            param_types = []
  if entry is None or param_vals is None or param_types is None or ret_type is None:
    return None
  if len(param_types) != len(param_vals):
    return None
  return (entry, ret_type, param_types, param_vals, expected)


# C type for a rylink param. Mirrors symirc's mangling for scalars; we
# can't synthesize aggregates or pointers as positional CLI args (rysmith
# only ever emits scalar params, so this is sufficient in practice).
_CARG = {
  "i8": "int8_t",
  "i16": "int16_t",
  "i32": "int32_t",
  "i64": "int64_t",
  "f32": "float",
  "f64": "double",
}


def _write_rylink_main_c(path, fname, ret_type, param_types, param_values):
  cret = _CRET.get(ret_type)
  if cret is None:
    return False  # FP return: skip (see _write_main_c).
  cparams = []
  for pt in param_types:
    cp = _CARG.get(pt)
    if cp is None:
      return False  # non-scalar param: skip.
    cparams.append(cp)
  # Render literals: integers as-is; floats wrapped in a cast so a
  # value like "-0" parses as a double rather than int.
  lits = []
  for pt, pv in zip(param_types, param_values):
    if pt in ("f32", "f64"):
      suffix = "f" if pt == "f32" else ""
      # Accept solver outputs like "-0" by forcing FP literal form.
      if "." not in pv and "e" not in pv and "E" not in pv:
        pv = pv + ".0"
      lits.append(pv + suffix)
    else:
      lits.append(pv)
  decl_params = ", ".join(cparams) if cparams else "void"
  call_args = ", ".join(lits)
  with open(path, "w") as f:
    f.write(
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      f"extern {cret} symir_{fname}({decl_params});\n"
      "int main(void) {\n"
      f"  {cret} r = symir_{fname}({call_args});\n"
      '  printf("Result: %lld\\n", (long long)r);\n'
      "  return 0;\n"
      "}\n"
    )
  return True


def _classify_rylink_one(prog_dir, symiri, clang, main_c, exe, verbose):
  """Cross-validate one rylink-bundled program directory. Mirrors
  ``_classify_one`` but reads the entry / params / expected ret from
  program.sir's header comments instead of parsing a parameter-less
  rysmith function."""
  sir_path = os.path.join(prog_dir, "program.sir")
  c_path = os.path.join(prog_dir, "program.c")
  prog_name = os.path.basename(prog_dir.rstrip("/"))
  if not os.path.exists(c_path):
    return ("cfail", f"{prog_name}: no program.c emitted")

  parsed = _parse_rylink_prog(sir_path)
  if parsed is None:
    return ("sirfail", f"{prog_name}: cannot parse bundled program.sir header")
  entry, ret_type, ptypes, pvals, expected = parsed

  # symiri side — bundled .sir + positional param values. The
  # bundled program may execute an unbounded loop; a TimeoutExpired
  # is bucketed as a sirfail rather than aborting the whole run.
  argv = [symiri, "--main", f"@{entry}", sir_path]
  if pvals:
    argv += ["--"] + pvals
  try:
    si = subprocess.run(
      argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=15
    )
  except subprocess.TimeoutExpired:
    return ("sirfail", f"{prog_name}: symiri timeout")
  if si.returncode != 0:
    return ("sirfail", f"{prog_name}: symiri exit {si.returncode}")
  sm = _RESULT_RE.search(si.stdout) or _RESULT_RE.search(si.stderr)
  if sm is None:
    return ("sirfail", f"{prog_name}: symiri no Result line")
  sir_val = sm.group(1)

  # The descriptor's claimed return value is the third source of truth;
  # surface a mismatch against symiri as a sirfail rather than silently
  # trust either side.
  if expected is not None and expected != sir_val:
    return ("sirfail", f"{prog_name}: symiri={sir_val} expected={expected}")

  if not _write_rylink_main_c(main_c, entry, ret_type, ptypes, pvals):
    return ("skipped", None)
  cc = subprocess.run(
    [
      clang,
      "-O0",
      c_path,
      main_c,
      "-o",
      exe,
      "-fsanitize=undefined",
      "-fno-sanitize-recover=all",
      "-w",
      "-lm",
    ],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=20,
  )
  if cc.returncode != 0:
    if verbose:
      print(cc.stderr)
    return ("cfail", f"{prog_name}: clang failed")
  try:
    cr = subprocess.run(
      [exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10
    )
  except subprocess.TimeoutExpired:
    return ("cfail", f"{prog_name}: c binary timeout")
  if cr.returncode != 0:
    head = cr.stderr.strip().splitlines()[0] if cr.stderr else "trap"
    return ("ubsan", f"{prog_name}: {head}")
  cm = _RESULT_RE.search(cr.stdout)
  if cm is None:
    return ("cfail", f"{prog_name}: no Result from C binary")
  c_val = cm.group(1)

  if sir_val == c_val:
    return ("passed", None)
  return ("mismatch", f"{prog_name}: symiri={sir_val} c={c_val}")


def _clear_batch_files(out_dir):
  """Remove .sir/.c artefacts from a prior batch, preserving the harness."""
  for f in os.listdir(out_dir):
    if f in ("_main.c", "_test"):
      continue
    if f.endswith(".sir") or f.endswith(".c"):
      os.remove(os.path.join(out_dir, f))


def _save_bug(bugs_dir, bucket, sir_name, src_dir, batch_num):
  """Copy the failing .sir (and its companion .c, if present) into
  ``<bugs_dir>/<bucket>/`` so the case can be replayed/reduced after the
  next batch clears ``src_dir``. Filenames are prefixed with ``b<N>_`` to
  avoid collisions when rysmith reuses names across batches."""
  dst_dir = os.path.join(bugs_dir, bucket)
  os.makedirs(dst_dir, exist_ok=True)
  stem = sir_name[:-4] if sir_name.endswith(".sir") else sir_name
  prefix = f"b{batch_num}_"
  for suffix in (".sir", ".c"):
    src = os.path.join(src_dir, stem + suffix)
    if os.path.exists(src):
      shutil.copy2(src, os.path.join(dst_dir, prefix + stem + suffix))


def _print_test_summary(passed, mismatch, ubsan, cfail, sirfail, skipped):
  """The legacy per-test-block report. Used per-funder --verbose, and
  always at the end of a run."""
  print(f"  {green('passed')}:        {passed}")
  print(f"  {red('mismatched')}:    {len(mismatch)}")
  print(f"  {red('ubsan traps')}:   {len(ubsan)}")
  print(f"  {red('compile fail')}:  {len(cfail)}")
  print(f"  {red('symiri fail')}:   {len(sirfail)}")
  print(f"  {yellow('skipped')}:       {skipped}")


def run(
  rysmith,
  symiri,
  symirc,
  n,
  seed,
  out_dir,
  clang,
  verbose,
  fail_early=False,
  rylink=None,
  rylink_n_per_batch=0,
  rylink_n_nodes=4,
):
  os.makedirs(out_dir, exist_ok=True)
  # Wipe stale artefacts from a previous run, including a prior bugs/ tree.
  for f in os.listdir(out_dir):
    p = os.path.join(out_dir, f)
    if os.path.isdir(p):
      shutil.rmtree(p)
    else:
      os.remove(p)
  bugs_dir = os.path.join(out_dir, "bugs")
  os.makedirs(bugs_dir, exist_ok=True)

  passed = 0
  mismatch = []
  ubsan = []
  cfail = []
  sirfail = []
  skipped = 0
  _LISTS = {"mismatch": mismatch, "ubsan": ubsan, "cfail": cfail, "sirfail": sirfail}

  main_c = os.path.join(out_dir, "_main.c")
  exe = os.path.join(out_dir, "_test")

  generated_total = 0  # programs rysmith actually emitted (skips its own FAILs)
  gen_failed_total = 0  # rysmith [FAIL] lines across batches
  processed_total = 0  # programs we cross-validated this run

  stopped_early = False
  done = 0
  batch_idx = 0
  while done < n and not stopped_early:
    batch_n = min(BATCH_SIZE, n - done)
    batch_seed = seed + batch_idx
    _clear_batch_files(out_dir)

    gen_cmd = [
      rysmith,
      "-n",
      str(batch_n),
      "--target",
      "c",
      "--seed",
      str(batch_seed),
      "-o",
      out_dir,
      # The leaf-phase main.c stub assumes a parameter-less entry. Pin
      # --n-params=0 here so rysmith's default (3) doesn't render every
      # leaf undiffable. The rylink phase below reads its own header
      # comments and handles params on its own.
      "--n-params",
      "0",
    ]
    # rylink consumes the per-fn descriptor JSON sidecar, so when the
    # rylink phase is active we also have to ask rysmith to emit it.
    # No cost when the phase is off.
    if rylink and rylink_n_per_batch > 0:
      gen_cmd.append("--emit-desc")
    if verbose:
      print(bold(f"batch #{batch_idx + 1} generation ({done}/{n} programs):"))
      print(f"  running: {' '.join(gen_cmd)}")
    gen = subprocess.run(
      gen_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    batch_gen_failed = (gen.stderr or "").strip().count("[FAIL]")
    gen_failed_total += batch_gen_failed

    sirs = sorted(p for p in os.listdir(out_dir) if p.endswith(".sir"))
    generated_total += len(sirs)

    if verbose:
      print(f"  {green('succeeded')}:     {batch_n - batch_gen_failed}")
      print(f"  {yellow('failed')}:        {batch_gen_failed}")
      print(f"  {'generated'}:     {len(sirs)}")

    done += batch_n

    if verbose:
      print(bold(f"batch #{batch_idx} testing ({done}/{n} programs):"))
      print("  running: symiri vs clang+exe")

    for sir_name in sirs:
      bucket, msg = _classify_one(
        sir_name, out_dir, symiri, clang, main_c, exe, verbose
      )
      processed_total += 1
      if bucket == "passed":
        passed += 1
      elif bucket == "skipped":
        skipped += 1
      else:
        _LISTS[bucket].append(msg)
        _save_bug(bugs_dir, bucket, sir_name, out_dir, batch_idx + 1)
        if fail_early:
          print(
            f"  [batch {batch_idx + 1}, prog {sir_name}] "
            f"{red('first failure')} ({bucket}): {msg}",
            flush=True,
          )
          stopped_early = True
          break

    # ── rylink phase ──────────────────────────────────────────────
    # After the per-fn diff test, build whole programs from the same
    # rysmith batch and cross-validate them too. Inter-procedural
    # call+offset rewrites stress different paths in symiri / symirc
    # than leaf functions alone.
    if rylink and rylink_n_per_batch > 0 and not stopped_early:
      ry_dir = os.path.join(out_dir, "rylink")
      if os.path.exists(ry_dir):
        shutil.rmtree(ry_dir)
      ry_cmd = [
        rylink,
        "-n",
        str(rylink_n_per_batch),
        "--n-nodes",
        str(rylink_n_nodes),
        "--target",
        "c",
        "--seed",
        str(batch_seed),
        "-i",
        out_dir,
        "-o",
        ry_dir,
      ]
      if verbose:
        print(f"  rylink: {' '.join(ry_cmd)}")
      ry = subprocess.run(
        ry_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=300
      )
      if ry.returncode != 0 and verbose:
        print(f"  {yellow('rylink')} exit {ry.returncode}")
      ry_progs = []
      if os.path.isdir(ry_dir):
        ry_progs = sorted(
          os.path.join(ry_dir, d)
          for d in os.listdir(ry_dir)
          if d.startswith("prog_") and os.path.isdir(os.path.join(ry_dir, d))
        )
      for prog_dir in ry_progs:
        bucket, msg = _classify_rylink_one(
          prog_dir, symiri, clang, main_c, exe, verbose
        )
        processed_total += 1
        if bucket == "passed":
          passed += 1
        elif bucket == "skipped":
          skipped += 1
        else:
          _LISTS[bucket].append(msg)
          # Save the bundled program.sir + program.c for replay. We use
          # the program-dir name as the "sir_name" so prefixing logic
          # downstream stays consistent.
          prog_name = os.path.basename(prog_dir)
          stem_src = os.path.join(prog_dir, "program")
          dst_dir = os.path.join(bugs_dir, bucket)
          os.makedirs(dst_dir, exist_ok=True)
          prefix = f"b{batch_idx + 1}_{prog_name}_"
          for suffix in (".sir", ".c"):
            src = stem_src + suffix
            if os.path.exists(src):
              shutil.copy2(src, os.path.join(dst_dir, prefix + "program" + suffix))
          if fail_early:
            print(
              f"  [batch {batch_idx + 1}, rylink {prog_name}] "
              f"{red('first failure')} ({bucket}): {msg}",
              flush=True,
            )
            stopped_early = True
            break

    batch_idx += 1

    if not stopped_early:
      if verbose:
        _print_test_summary(passed, mismatch, ubsan, cfail, sirfail, skipped)
      else:
        print(
          f"  [batch #{batch_idx}: {done}/{n}] passed={passed} "
          f"mismatch={len(mismatch)} ubsan={len(ubsan)} "
          f"cfail={len(cfail)} sirfail={len(sirfail)} "
          f"skipped={skipped}",
          flush=True,
        )
      print()

  # Report.
  print()
  hdr = f"reify differential test (n={processed_total}, seed={seed}"
  if stopped_early:
    hdr += ", stopped early"
  hdr += "):"
  print(bold(hdr))
  if verbose:
    print(f"  {'generated'}:     {generated_total}")
    print(f"  {red('gen failed')}:    {gen_failed_total}")
    print("  ---")
  _print_test_summary(passed, mismatch, ubsan, cfail, sirfail, skipped)

  fail_lines = mismatch + ubsan + cfail + sirfail
  if fail_lines:
    print(bold("\nFailures:"))
    for line in fail_lines[:20]:
      print(f"  {line}")
    if len(fail_lines) > 20:
      print(f"  ... and {len(fail_lines) - 20} more")
    print(
      f"\n  {yellow('saved')} {len(fail_lines)} failing case(s) to "
      f"{bugs_dir}/<bucket>/  (.sir + .c)"
    )

  return len(fail_lines) == 0


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--rysmith", default="./rysmith")
  ap.add_argument("--symiri", default="./symiri")
  ap.add_argument(
    "--symirc",
    default="./symirc",
    help="(implicitly invoked by rysmith --target c)",
  )
  ap.add_argument("--clang", default="clang")
  ap.add_argument("--n", type=int, default=100)
  ap.add_argument(
    "--seed",
    type=int,
    default=1234,
    help="Fixed seed so make test is deterministic",
  )
  ap.add_argument("--out", default="build/test_tmp/reify_diff")
  ap.add_argument("--verbose", "-v", action="store_true")
  ap.add_argument(
    "--fail-early",
    action="store_true",
    help="Stop at the first mismatch/ubsan/cfail/sirfail instead of finishing the batch",
  )
  ap.add_argument(
    "--rylink",
    default=None,
    help="Path to rylink binary; enables a whole-program diff-test phase after each rysmith batch",
  )
  ap.add_argument(
    "--rylink-n-per-batch",
    type=int,
    default=10,
    help="Number of whole programs to link+test per rysmith batch (0 disables the phase)",
  )
  ap.add_argument(
    "--rylink-n-nodes",
    type=int,
    default=4,
    help="Target call-graph nodes per linked program (passed to rylink --n-nodes)",
  )
  args = ap.parse_args()

  for tool, path in (
    ("rysmith", args.rysmith),
    ("symiri", args.symiri),
    ("symirc", args.symirc),
  ):
    if not os.path.exists(path):
      print(red(f"error: {tool} not found at {path}"), file=sys.stderr)
      sys.exit(2)
  if args.rylink and not os.path.exists(args.rylink):
    print(red(f"error: rylink not found at {args.rylink}"), file=sys.stderr)
    sys.exit(2)
  if not shutil.which(args.clang):
    print(red(f"error: clang '{args.clang}' not on PATH"), file=sys.stderr)
    sys.exit(2)

  ok = run(
    args.rysmith,
    args.symiri,
    args.symirc,
    args.n,
    args.seed,
    args.out,
    args.clang,
    args.verbose,
    fail_early=args.fail_early,
    rylink=args.rylink,
    rylink_n_per_batch=args.rylink_n_per_batch,
    rylink_n_nodes=args.rylink_n_nodes,
  )
  sys.exit(0 if ok else 1)


if __name__ == "__main__":
  main()
