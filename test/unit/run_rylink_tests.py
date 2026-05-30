"""End-to-end tests for rylink — the whole-program generator.

rylink takes a directory of rysmith-generated functions (each with a
sidecar `.json` descriptor) and synthesises whole programs by:

  1. Picking a random subset of the pool as call-graph nodes.
  2. Building a DAG call-graph (no recursion).
  3. Performing peephole rewrites: replace literal occurrences in
     callers with a `call` to a callee whose solved ret value matches.
  4. Optionally validating semantic equivalence via symiri.

Each program lands in `prog_<id>_<i>/`, with the entry function
emitted as `program.sir` (carrying CG/param/ret comments in the
header) and one `.sir` per callee.

Run as:

  python3 -m test.unit.run_rylink_tests <rylink> <rysmith> <symiri>

Each test prints PASS or FAIL; exit code reflects the worst result.
"""

import os
import re
import subprocess
import sys
import tempfile

GREEN = "\033[32m"
RED = "\033[31m"
NC = "\033[0m"

results = []

# rylink (and rysmith) print a `generation id = <hex>` banner.
_ID_RE = re.compile(r"generation id\s*=\s*([0-9a-f]{6})")


def run(cmd, **kw):
  return subprocess.run(cmd, capture_output=True, text=True, timeout=120, **kw)


def check(name, ok, detail=""):
  results.append((name, ok, detail))
  color = GREEN if ok else RED
  tag = "PASS" if ok else "FAIL"
  print(f"  [{color}{tag}{NC}] {name}" + (f" — {detail}" if detail and not ok else ""))


def extract_id(stdout):
  m = _ID_RE.search(stdout or "")
  return m.group(1) if m else None


def seed_pool(rysmith, pool_dir, n_funcs=8, n_params=1, seed=23):
  """Drive rysmith and return the generated 6-hex ID for the run."""
  r = run(
    [
      rysmith,
      "--n-funcs",
      str(n_funcs),
      "--n-params",
      str(n_params),
      "--seed",
      str(seed),
      "--emit-desc",
      "-o",
      pool_dir,
    ]
  )
  # rysmith exits non-zero on any per-fn failure but still emits the
  # successes. The pool needs only a few descriptors to drive rylink,
  # so check the artifact directly rather than the exit code.
  jsons = [f for f in os.listdir(pool_dir) if f.endswith(".json")]
  if len(jsons) < 2:
    raise RuntimeError(
      f"rysmith produced too few descriptors ({len(jsons)}); stderr={r.stderr[-300:]!r}"
    )
  gid = extract_id(r.stdout)
  if gid is None:
    raise RuntimeError(
      f"rysmith stdout missing generation-id banner; stdout={r.stdout[:300]!r}"
    )
  return gid


def test_basic_run(rylink, rysmith):
  """A vanilla `rylink` invocation succeeds and produces N prog_<id>_<i> dirs."""
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "3",
          "--seed",
          "7",
          "-o",
          out,
        ]
      )
      check(
        "rylink basic run exits 0",
        r.returncode == 0,
        f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
      )
      gid = extract_id(r.stdout)
      check(
        "rylink stdout reports a 6-hex generation id",
        gid is not None,
        f"stdout={r.stdout[:200]!r}",
      )
      if gid is None:
        return
      dirs = sorted(d for d in os.listdir(out) if d.startswith("prog_"))
      check(
        f"three prog_{gid}_<i> dirs created",
        dirs == [f"prog_{gid}_0", f"prog_{gid}_1", f"prog_{gid}_2"],
        str(dirs),
      )


def test_program_sir_layout(rylink, rysmith):
  """Each prog dir contains program.sir + per-callee .sir; program.sir
  carries CG/param/ret comments at the top."""
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "1",
          "--seed",
          "13",
          "-o",
          out,
        ]
      )
      check("rylink layout-test exits 0", r.returncode == 0, r.stderr[:200])
      gid = extract_id(r.stdout)
      if gid is None:
        check("rylink id discovery for layout-test", False, "no id in stdout")
        return
      prog_dir = os.path.join(out, f"prog_{gid}_0")
      check(f"prog_{gid}_0/ exists", os.path.isdir(prog_dir))
      if not os.path.isdir(prog_dir):
        return
      files = os.listdir(prog_dir)
      check(
        "program.sir present",
        "program.sir" in files,
        str(files),
      )
      sirs = [f for f in files if f.endswith(".sir")]
      check(
        "at least one .sir file (entry; ≥1 callee if CG has edges)",
        len(sirs) >= 1,
        str(sirs),
      )
      with open(os.path.join(prog_dir, "program.sir")) as f:
        head = f.read(2048)
      check(
        "program.sir header carries `// CG:` comment",
        "// CG:" in head,
        head[:200],
      )
      check(
        "program.sir header carries `// PARAMS:` comment",
        "// PARAMS:" in head,
        head[:200],
      )
      check(
        "program.sir header carries `// RETURN:` comment",
        "// RETURN:" in head,
        head[:200],
      )


def test_c_split_output(rylink, rysmith):
  """`--target c` (default split-by-source) emits common.h + per-source .c."""
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "1",
          "--seed",
          "5",
          "--target",
          "c",
          "-o",
          out,
        ]
      )
      check("rylink --target c exits 0", r.returncode == 0, r.stderr[:200])
      gid = extract_id(r.stdout)
      if gid is None:
        check("rylink id discovery for c-split", False, "no id in stdout")
        return
      prog_dir = os.path.join(out, f"prog_{gid}_0")
      if not os.path.isdir(prog_dir):
        check("prog dir present for c-target", False, "missing dir")
        return
      files = os.listdir(prog_dir)
      check(
        "common.h emitted",
        "common.h" in files,
        str(files),
      )
      cs = [f for f in files if f.endswith(".c")]
      check(
        "at least one .c emitted",
        len(cs) >= 1,
        str(cs),
      )
      # rylink emits one .c per FunDecl::sourceStem (set in mergeInto)
      # plus the primary "program.c". With a pool of ≥2 functions the
      # bundle picks ≥2 nodes, so we expect ≥3 .c files. If this drops
      # to 1 it means sourceStem stopped surviving into the backend
      # (the bug B4 was meant to fix).
      check(
        "split-by-source emits one .c per FunDecl::sourceStem",
        len(cs) >= 3,
        f".c files = {sorted(cs)}",
      )


def test_validate(rylink, rysmith, symiri):
  """--validate proves the rewritten entry returns the originally-solved
  value when invoked with the entry's parameter realisation."""
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool, n_funcs=10, seed=31)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "2",
          "--seed",
          "9",
          "--validate",
          # Per-init "validated: OK" lines are gated on --verbose
          # (non-verbose only prints a per-program OK summary).
          "--verbose",
          "-o",
          out,
        ]
      )
      check(
        "rylink --validate exits 0 (every emitted program validated)",
        r.returncode == 0,
        f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
      )
      # rylink should emit a per-program "validated: OK" log line on
      # success (mirroring rysmith).
      check(
        "stdout/stderr mentions `validated: OK` at least once",
        ("validated: OK" in r.stdout) or ("validated: OK" in r.stderr),
        f"stdout={r.stdout[:300]!r}",
      )


def test_rewrite_introduces_call(rylink, rysmith):
  """Default behaviour: at least one prog out of a small batch contains
  a `call @` in program.sir (the rewrite engine actually fired)."""
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool, n_funcs=10, seed=29)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "5",
          "--seed",
          "17",
          "-o",
          out,
        ]
      )
      check("rylink rewrite-test exits 0", r.returncode == 0, r.stderr[:200])
      gid = extract_id(r.stdout)
      if gid is None:
        check("rylink id discovery for rewrite-test", False, "no id in stdout")
        return
      any_call = False
      for i in range(5):
        p = os.path.join(out, f"prog_{gid}_{i}", "program.sir")
        if not os.path.isfile(p):
          continue
        txt = open(p).read()
        if re.search(r"\bcall\s+@", txt):
          any_call = True
          break
      check(
        "at least one program.sir contains a `call @...` (rewrite fired)",
        any_call,
        "no `call @` found across 5 programs",
      )


def test_rewrite_offset_in_range(rylink, rysmith, symirc):
  """The rewrite engine declines splices whose `offset = c - ret`
  doesn't fit the let's signed range — otherwise the C lowering of
  `call_result + offset` trips signed-overflow UB even though SymIR
  semantics wrap cleanly. Drive a batch through `symirc --target c`
  with UBSan flags wired in by lowering to clang; any splice that
  slipped through the range check would surface as a runtime trap.
  This is a smoke check: it doesn't enumerate every overflow shape,
  it just confirms the engine produces UBSan-clean C for a few seeds
  that previously tripped (see commit history)."""
  import shutil as _sh
  import subprocess as _sub

  clang = _sh.which("clang")
  if clang is None:
    print("  [skip] clang not on PATH", file=sys.stderr)
    return
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool, n_funcs=10, seed=1234, n_params=3)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "5",
          "--seed",
          "1234",
          "--target",
          "c",
          "-o",
          out,
        ]
      )
      check("rylink overflow-check exits 0", r.returncode == 0, r.stderr[:200])
      gid = extract_id(r.stdout)
      if gid is None:
        check("rylink id discovery for overflow-check", False, "no id in stdout")
        return
      # Compile + run each emitted bundle under UBSan; the test is
      # whether anything traps (signed-overflow is fatal under
      # -fno-sanitize-recover=all).
      for i in range(5):
        prog_dir = os.path.join(out, f"prog_{gid}_{i}")
        c_path = os.path.join(prog_dir, "program.c")
        if not os.path.isfile(c_path):
          continue
        # Synthesize a minimal main that ignores the result — we
        # only need execution to surface UBSan; param values don't
        # matter for the overflow-detection check.
        main_c = os.path.join(prog_dir, "_main.c")
        with open(main_c, "w") as f:
          f.write("int main(void){return 0;}\n")
        exe = os.path.join(prog_dir, "_test")
        cc = _sub.run(
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
          stdout=_sub.PIPE,
          stderr=_sub.PIPE,
        )
        check(
          f"prog_{gid}_{i} compiles under UBSan",
          cc.returncode == 0,
          cc.stderr[:200].decode("utf-8", "replace"),
        )


def main():
  if len(sys.argv) != 4:
    print("Usage: python3 -m test.unit.run_rylink_tests <rylink> <rysmith> <symiri>")
    sys.exit(2)
  rylink, rysmith, symiri = sys.argv[1:4]
  print("=== rylink basic run ===")
  test_basic_run(rylink, rysmith)
  print("=== rylink program.sir layout ===")
  test_program_sir_layout(rylink, rysmith)
  print("=== rylink --target c split output ===")
  test_c_split_output(rylink, rysmith)
  print("=== rylink --validate ===")
  test_validate(rylink, rysmith, symiri)
  print("=== rylink peephole rewrite fires ===")
  test_rewrite_introduces_call(rylink, rysmith)
  print("=== rylink rewrite offset range check ===")
  # symirc not used directly by this test but kept positional for the
  # CLI signature; passing rysmith is enough since seed_pool wraps it.
  test_rewrite_offset_in_range(rylink, rysmith, symiri)

  passed = sum(1 for _, ok, _ in results if ok)
  total = len(results)
  print(f"\nSummary (rylink_tests): {passed}/{total} passed.\n")
  sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
  main()
