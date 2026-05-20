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

from test.lib.style import green, red, yellow, bold


_FUN_RE = re.compile(r"fun\s+@([a-zA-Z0-9_]+)\(\)\s*:\s*(i[0-9]+|f32|f64)")
_RESULT_RE = re.compile(r"Result:\s*(-?[0-9]+)")


_CRET = {
  "i8": "int8_t",
  "i16": "int16_t",
  "i32": "int32_t",
  "i64": "int64_t",
}


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


def run(rysmith, symiri, symirc, n, seed, out_dir, gcc, verbose):
  os.makedirs(out_dir, exist_ok=True)
  for f in os.listdir(out_dir):
    os.remove(os.path.join(out_dir, f))

  # 1. Generate N programs with rysmith --target c (also emits .c via symirc).
  gen_cmd = [
    rysmith,
    "-n",
    str(n),
    "--target",
    "c",
    "--seed",
    str(seed),
    "-o",
    out_dir,
  ]
  if verbose:
    print(bold(f"Generating {n} programs: {' '.join(gen_cmd)}"))
  gen = subprocess.run(
    gen_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
  )
  if gen.returncode != 0:
    print(red(f"rysmith failed (exit {gen.returncode}):"))
    print(gen.stderr)
    return False

  # 2. For each (sir, c) pair, compare symiri's Result to the compiled-C Result.
  passed = 0
  mismatch = []
  ubsan = []
  cfail = []
  sirfail = []
  skipped_fp = 0

  main_c = os.path.join(out_dir, "_main.c")
  exe = os.path.join(out_dir, "_test")

  sirs = sorted(p for p in os.listdir(out_dir) if p.endswith(".sir"))
  for sir_name in sirs:
    sir_path = os.path.join(out_dir, sir_name)
    c_path = sir_path[:-4] + ".c"
    if not os.path.exists(c_path):
      cfail.append(f"{sir_name}: no .c emitted")
      continue

    fun = _parse_fun(sir_path)
    if fun is None:
      sirfail.append(f"{sir_name}: cannot parse fun decl")
      continue
    fname, rtype = fun

    # symiri output
    si = subprocess.run(
      [symiri, "--main", f"@{fname}", sir_path],
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
      text=True,
      timeout=10,
    )
    if si.returncode != 0:
      sirfail.append(f"{sir_name}: symiri exit {si.returncode}")
      continue
    sm = _RESULT_RE.search(si.stdout) or _RESULT_RE.search(si.stderr)
    if sm is None:
      sirfail.append(f"{sir_name}: symiri no Result line")
      continue
    sir_val = sm.group(1)

    # C harness
    if not _write_main_c(main_c, fname, rtype):
      skipped_fp += 1
      continue
    gc = subprocess.run(
      [
        gcc,
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
    if gc.returncode != 0:
      cfail.append(f"{sir_name}: gcc failed")
      if verbose:
        print(gc.stderr)
      continue
    cr = subprocess.run(
      [exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5
    )
    if cr.returncode != 0:
      ubsan.append(
        f"{sir_name}: {cr.stderr.strip().splitlines()[0] if cr.stderr else 'trap'}"
      )
      continue
    cm = _RESULT_RE.search(cr.stdout)
    if cm is None:
      cfail.append(f"{sir_name}: no Result from C binary")
      continue
    c_val = cm.group(1)

    if sir_val == c_val:
      passed += 1
    else:
      mismatch.append(f"{sir_name}: symiri={sir_val} c={c_val}")

  # 3. Report.
  total = len(sirs)
  print()
  print(bold(f"reify differential test (n={total}, seed={seed}):"))
  print(f"  {green('passed')}:        {passed}")
  print(f"  {red('mismatched')}:    {len(mismatch)}")
  print(f"  {red('ubsan traps')}:   {len(ubsan)}")
  print(f"  {red('compile fail')}:  {len(cfail)}")
  print(f"  {red('symiri fail')}:   {len(sirfail)}")
  print(f"  {yellow('skipped (fp)')}: {skipped_fp}")

  fail_lines = mismatch + ubsan + cfail + sirfail
  if fail_lines:
    print(bold("\nFailures:"))
    for line in fail_lines[:20]:
      print(f"  {line}")
    if len(fail_lines) > 20:
      print(f"  ... and {len(fail_lines) - 20} more")

  return len(fail_lines) == 0


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--rysmith", default="./rysmith")
  ap.add_argument("--symiri", default="./symiri")
  ap.add_argument(
    "--symirc", default="./symirc", help="(implicitly invoked by rysmith --target c)"
  )
  ap.add_argument("--gcc", default="gcc")
  ap.add_argument("--n", type=int, default=100)
  ap.add_argument(
    "--seed", type=int, default=1234, help="Fixed seed so make test is deterministic"
  )
  ap.add_argument("--out", default="build/test_tmp/reify_diff")
  ap.add_argument("--verbose", "-v", action="store_true")
  args = ap.parse_args()

  for tool, path in (
    ("rysmith", args.rysmith),
    ("symiri", args.symiri),
    ("symirc", args.symirc),
  ):
    if not os.path.exists(path):
      print(red(f"error: {tool} not found at {path}"), file=sys.stderr)
      sys.exit(2)
  if not shutil.which(args.gcc):
    print(red(f"error: gcc '{args.gcc}' not on PATH"), file=sys.stderr)
    sys.exit(2)

  ok = run(
    args.rysmith,
    args.symiri,
    args.symirc,
    args.n,
    args.seed,
    args.out,
    args.gcc,
    args.verbose,
  )
  sys.exit(0 if ok else 1)


if __name__ == "__main__":
  main()
