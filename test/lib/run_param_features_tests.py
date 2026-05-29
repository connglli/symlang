"""End-to-end tests for the v0.2.2 entry-point param-args features:

  (A) `symiri` accepts positional arguments after `--` and binds them
      to the entry function's parameters.
  (B) `symirsolve -o <out.sir>` prepends a `// SOLVED: %p=v, ..., ret=v`
      header to the emitted concrete program. The param identifiers in
      the body are NOT substituted (they appear verbatim).
  (C) `symirc --split-by-source -o <dir>` writes one `<stem>.c` per
      source file plus a shared `common.h`; every per-source `.c`
      starts with `#include "common.h"`.

Run as:

  python3 -m test.lib.run_param_features_tests ./symiri ./symirc ./symirsolve

Each test prints PASS or FAIL; exit code reflects the worst result.
"""

import os
import re
import subprocess
import sys
import tempfile

# (A) symiri positional args is also covered by the `.sir` tests under
# test/interp/v022_param_args_*. We keep one round-trip check here to
# verify that the round trip "solver synthesises params -> SOLVED header
# names them -> symiri replays them as positional args" works.

GREEN = "\033[32m"
RED = "\033[31m"
NC = "\033[0m"

results = []  # (name, passed, detail)


def run(cmd, **kw):
  return subprocess.run(cmd, capture_output=True, text=True, timeout=30, **kw)


def check(name, ok, detail=""):
  results.append((name, ok, detail))
  color = GREEN if ok else RED
  tag = "PASS" if ok else "FAIL"
  print(f"  [{color}{tag}{NC}] {name}" + (f" — {detail}" if detail and not ok else ""))


def test_symiri_positional(symiri):
  """symiri --main @f input.sir -- a b c"""
  src = """fun @add(%a: i32, %b: i32) : i32 {
  let mut %r: i32 = 0;
^entry:
  %r = %a + %b;
  ret %r;
}
"""
  with tempfile.NamedTemporaryFile("w", suffix=".sir", delete=False) as f:
    f.write(src)
    path = f.name
  try:
    r = run([symiri, "--main", "@add", path, "--", "3", "4"])
    ok = r.returncode == 0 and "Result: 7" in r.stdout
    check(
      "symiri positional args: @add(3, 4) -> 7",
      ok,
      f"rc={r.returncode}, stdout={r.stdout!r}",
    )
  finally:
    os.unlink(path)


def test_symirsolve_solved_header(symirsolve, symiri):
  """symirsolve writes // SOLVED: header; symiri round-trips it."""
  src = """fun @f(%a: i32, %b: i32) : i32 {
  let mut %r: i32 = 0;
^entry:
  %r = %a + %b;
  require %r == 7, "sum is 7";
  br ^exit;
^exit:
  ret %r;
}
"""
  with tempfile.TemporaryDirectory() as d:
    inp = os.path.join(d, "in.sir")
    out = os.path.join(d, "out.sir")
    open(inp, "w").write(src)
    r = run([symirsolve, "--main", "@f", "--path", "^entry,^exit", "-o", out, inp])
    check(
      "symirsolve runs SAT on a parameterised entry fun",
      r.returncode == 0 and "SAT" in r.stdout,
      f"rc={r.returncode}, stdout={r.stdout!r}, stderr={r.stderr!r}",
    )
    body = open(out).read()
    # Header present and well-formed
    m = re.match(r"^// SOLVED:(.+)$", body.split("\n")[0])
    check("output starts with // SOLVED: header", m is not None, body[:120])
    if not m:
      return
    header = m.group(1)
    # Both %a and %b appear in header
    check("SOLVED header mentions %a", "%a=" in header, header)
    check("SOLVED header mentions %b", "%b=" in header, header)
    check("SOLVED header mentions ret", "ret=" in header, header)
    # Body still has the parameter names verbatim (NOT substituted)
    check("body still references %a verbatim", "%a" in body, body[:200])
    check("body still references %b verbatim", "%b" in body, body[:200])
    # Parse the values and round-trip through symiri
    kv = dict(re.findall(r"(%\w+|ret)=(-?\d+)", header))
    a = kv.get("%a")
    b = kv.get("%b")
    ret = kv.get("ret")
    check(
      "all three values parsed",
      a is not None and b is not None and ret is not None,
      str(kv),
    )
    if a is not None and b is not None:
      r2 = run([symiri, "--main", "@f", out, "--", a, b])
      ok = r2.returncode == 0 and f"Result: {ret}" in r2.stdout
      check(
        f"round-trip: symiri @f({a},{b}) -> {ret}",
        ok,
        f"rc={r2.returncode}, stdout={r2.stdout!r}",
      )


def test_symirc_split_by_source(symirc):
  """symirc --split-by-source writes <stem>.c per source + common.h."""
  src = """decl @clamp(%x: i32, %lo: i32, %hi: i32) : i32;
fun @main(%a: i32) : i32 {
  let mut %r: i32 = 0;
^entry:
  %r = call @clamp(%a, 1, 100);
  ret %r;
}
"""
  with tempfile.TemporaryDirectory() as d:
    inp = os.path.join(d, "primary.sir")
    outd = os.path.join(d, "out")
    open(inp, "w").write(src)
    r = run([symirc, "--split-by-source", "-o", outd, inp, "-I", "test/lib/std/scalar"])
    check(
      "symirc --split-by-source exits 0",
      r.returncode == 0,
      f"rc={r.returncode}, stderr={r.stderr!r}",
    )
    if r.returncode != 0:
      return
    common = os.path.join(outd, "common.h")
    check("common.h exists", os.path.isfile(common))
    check(
      "common.h has #pragma once",
      os.path.isfile(common) and "#pragma once" in open(common).read(),
    )
    primary_c = os.path.join(outd, "primary.c")
    check("primary stem .c exists", os.path.isfile(primary_c))
    check(
      'primary.c starts with #include "common.h"',
      os.path.isfile(primary_c)
      and open(primary_c).read().lstrip().startswith('#include "common.h"'),
    )
    # Stdlib helpers from -I produce their own .c files
    clamp_c = os.path.join(outd, "abs_clamp.c")
    check("stdlib helper abs_clamp.c emitted", os.path.isfile(clamp_c))
    check(
      'abs_clamp.c starts with #include "common.h"',
      os.path.isfile(clamp_c)
      and open(clamp_c).read().lstrip().startswith('#include "common.h"'),
    )
    # All emitted .c files compile together (with gcc, just syntax-check).
    cs = [os.path.join(outd, x) for x in os.listdir(outd) if x.endswith(".c")]
    if cs:
      r2 = run(["gcc", "-c", "-Wno-everything"] + cs, cwd=outd)
      check(
        f"all {len(cs)} emitted .c files compile",
        r2.returncode == 0,
        f"gcc rc={r2.returncode}, stderr={r2.stderr[:300]!r}",
      )


def main():
  if len(sys.argv) != 4:
    print(
      "Usage: python3 -m test.lib.run_param_features_tests "
      "<symiri> <symirc> <symirsolve>"
    )
    sys.exit(2)
  symiri, symirc, symirsolve = sys.argv[1:4]

  print("=== symiri positional args ===")
  test_symiri_positional(symiri)
  print("=== symirsolve SOLVED header ===")
  test_symirsolve_solved_header(symirsolve, symiri)
  print("=== symirc --split-by-source ===")
  test_symirc_split_by_source(symirc)

  passed = sum(1 for _, ok, _ in results if ok)
  total = len(results)
  print(f"\nSummary (param_features_tests): {passed}/{total} passed.\n")
  sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
  main()
