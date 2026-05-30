"""End-to-end tests for rysmith's v0.2.2 generation surface:

  (1) Every run prints a 6-hex-char generation ID derived from --seed
      and prefixes every generated function and struct with it.
  (2) `--n-params N` adds N scalar parameters to each generated
      function. Parameters appear in the function signature and
      are usable as RValues throughout the body.
  (3) Every successful generation writes a `func_<id>_<i>.json`
      descriptor with the canonical schema.
  (4) The concrete .sir output carries a `// SOLVED:` header naming
      the solved parameter values, which `symiri ... -- <vals>`
      replays bit-exact.

Run as:

  python3 -m test.lib.run_rysmith_tests <rysmith> <symiri>

Each test prints PASS or FAIL; exit code reflects the worst result.
"""

import json
import os
import re
import subprocess
import sys
import tempfile

GREEN = "\033[32m"
RED = "\033[31m"
NC = "\033[0m"

results = []

# rysmith emits a `generation id = <hex>` banner on stdout — every test
# below relies on this line because --id was removed. Same regex is
# duplicated in run_rylink_tests.py to keep each module standalone.
_ID_RE = re.compile(r"generation id\s*=\s*([0-9a-f]{6})")


def run(cmd, **kw):
  return subprocess.run(cmd, capture_output=True, text=True, timeout=60, **kw)


def check(name, ok, detail=""):
  results.append((name, ok, detail))
  color = GREEN if ok else RED
  tag = "PASS" if ok else "FAIL"
  print(f"  [{color}{tag}{NC}] {name}" + (f" — {detail}" if detail and not ok else ""))


def extract_id(stdout):
  m = _ID_RE.search(stdout or "")
  return m.group(1) if m else None


def test_id_banner(rysmith):
  """rysmith prints a 6-hex-char generation ID and uses it for every
  generated function and struct."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "2",
        "--seed",
        "42",
        "--emit-desc",
        "-o",
        d,
      ]
    )
    check(
      "rysmith default-id run exits 0",
      r.returncode == 0,
      f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
    )
    gid = extract_id(r.stdout)
    check(
      "stdout reports a 6-hex generation id",
      gid is not None,
      f"stdout={r.stdout[:200]!r}",
    )
    if gid is None:
      return
    files = sorted(os.listdir(d))
    expected_sirs = {f"func_{gid}_0", f"func_{gid}_1"}
    sir_stems = {f.rsplit(".", 1)[0] for f in files if f.endswith(".sir")}
    # Single-init produces `func_<id>_<i>.sir`; multi-init produces
    # `func_<id>_<i><a..z>.sir`. Trim a trailing init-letter to
    # recover the base name in both cases.
    base_stems = set()
    for s in sir_stems:
      m = re.match(rf"(func_{gid}_\d+)[a-z]?$", s)
      if m:
        base_stems.add(m.group(1))
    check(
      f"both func_{gid}_<i> base names present",
      expected_sirs.issubset(base_stems),
      f"base={base_stems}",
    )
    json_names = sorted(f for f in files if f.endswith(".json"))
    check(
      f"descriptors func_{gid}_0.json and func_{gid}_1.json emitted",
      json_names == [f"func_{gid}_0.json", f"func_{gid}_1.json"],
      str(json_names),
    )


def test_seed_id_determinism(rysmith):
  """Two runs with the same --seed produce the same generation ID."""
  with tempfile.TemporaryDirectory() as d1, tempfile.TemporaryDirectory() as d2:
    r1 = run([rysmith, "--n-funcs", "1", "--seed", "777", "-o", d1])
    r2 = run([rysmith, "--n-funcs", "1", "--seed", "777", "-o", d2])
    g1 = extract_id(r1.stdout)
    g2 = extract_id(r2.stdout)
    check(
      "same --seed yields the same generation id across runs",
      g1 is not None and g1 == g2,
      f"g1={g1!r} g2={g2!r}",
    )


def test_n_params(rysmith):
  """--n-params 3 puts three scalar parameters into every generated function."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--emit-desc",
        "--seed",
        "7",
        "--n-params",
        "3",
        "-o",
        d,
      ]
    )
    check(
      "rysmith --n-params=3 exits 0", r.returncode == 0, f"stderr={r.stderr[:200]!r}"
    )
    gid = extract_id(r.stdout)
    if gid is None:
      check("rysmith id discovery for --n-params", False, "no id in stdout")
      return
    desc_path = os.path.join(d, f"func_{gid}_0.json")
    check("descriptor exists", os.path.isfile(desc_path))
    if not os.path.isfile(desc_path):
      return
    desc = json.load(open(desc_path))
    check(
      "descriptor lists 3 params",
      len(desc.get("params", [])) == 3,
      f"params={desc.get('params')}",
    )
    names = [p["name"] for p in desc.get("params", [])]
    check("param names are %pa0..%pa2", names == ["%pa0", "%pa1", "%pa2"], str(names))
    types = [p["type"] for p in desc.get("params", [])]
    scalar_pat = re.compile(r"^(i\d+|f32|f64)$")
    check(
      "all params are scalar types", all(scalar_pat.match(t) for t in types), str(types)
    )
    # Body actually uses each param somewhere (or at least references them
    # — rysmith may not always pick them when many other vars are around)
    sir_path = next(
      (
        os.path.join(d, f)
        for f in os.listdir(d)
        if f.startswith(f"func_{gid}_0") and f.endswith(".sir")
      ),
      None,
    )
    check("at least one concrete .sir emitted", sir_path is not None)
    if sir_path:
      body = open(sir_path).read()
      check(
        "signature uses %pa0..%pa2",
        all(p in body for p in ["%pa0", "%pa1", "%pa2"]),
        body[:200],
      )


def test_descriptor_schema(rysmith):
  """Descriptor JSON parses and carries every documented field."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--emit-desc",
        "--seed",
        "11",
        "--n-params",
        "2",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith descriptor-schema setup", False, r.stderr[:200])
      return
    gid = extract_id(r.stdout)
    if gid is None:
      check("rysmith id discovery for descriptor-schema", False, "no id in stdout")
      return
    desc = json.load(open(os.path.join(d, f"func_{gid}_0.json")))
    for k in (
      "id",
      "name",
      "ret_type",
      "params",
      "path",
      "structs",
      "realizations",
    ):
      check(f"descriptor field `{k}` present", k in desc, str(list(desc.keys())))
    # Top-level `syms` was dropped (each realization has its own
    # potentially-different sym set; the old top-level field reflected
    # only the last init and was misleading).
    check("descriptor.syms removed", "syms" not in desc, str(list(desc.keys())))
    check("descriptor.id matches stdout id", desc.get("id") == gid, desc.get("id"))
    check(
      f"name is @func_{gid}_0",
      desc.get("name") == f"@func_{gid}_0",
      desc.get("name"),
    )
    rzs = desc.get("realizations")
    check(
      "at least one realization listed",
      isinstance(rzs, list) and rzs,
      str(rzs),
    )
    if rzs:
      rz = rzs[0]
      check("realization has file", "file" in rz, str(rz))
      check("realization has params dict", isinstance(rz.get("params"), dict), str(rz))
      check("realization has syms dict", isinstance(rz.get("syms"), dict), str(rz))
      check("realization has ret", "ret" in rz, str(rz))
      # All declared params have a value
      param_names = [p["name"] for p in desc.get("params", [])]
      check(
        "every declared param has a solved value",
        all(n in rz.get("params", {}) for n in param_names),
        f"params={rz.get('params')}",
      )


def test_solved_replay(rysmith, symiri):
  """SOLVED header round-trips through symiri positional args."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--emit-desc",
        "--seed",
        "13",
        "--n-params",
        "2",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith solved-replay setup", False, r.stderr[:200])
      return
    gid = extract_id(r.stdout)
    if gid is None:
      check("rysmith id discovery for solved-replay", False, "no id in stdout")
      return
    sirs = sorted(
      f for f in os.listdir(d) if f.startswith(f"func_{gid}_0") and f.endswith(".sir")
    )
    if not sirs:
      check("at least one concrete .sir for replay", False, "no sirs")
      return
    sir_path = os.path.join(d, sirs[0])
    header = open(sir_path).readline().strip()
    check("first line is // SOLVED:", header.startswith("// SOLVED:"), header)
    if not header.startswith("// SOLVED:"):
      return
    kv = dict(re.findall(r"(%\w+|ret)=(-?\d+(?:\.\d+(?:[eE][-+]?\d+)?)?)", header))
    pa0 = kv.get("%pa0")
    pa1 = kv.get("%pa1")
    ret = kv.get("ret")
    check(
      "SOLVED has %pa0, %pa1, ret",
      pa0 is not None and pa1 is not None and ret is not None,
      str(kv),
    )
    if pa0 is None or pa1 is None:
      return
    # Replay through symiri — must produce the same return value.
    r2 = run([symiri, "--main", f"@func_{gid}_0", sir_path, "--", pa0, pa1])
    expected = f"Result: {ret.rstrip('.0')}" if ret.endswith(".0") else f"Result: {ret}"
    # symiri prints int returns as bare decimal; tolerate equality on the int.
    ok = r2.returncode == 0 and (f"Result: {ret}" in r2.stdout or expected in r2.stdout)
    check(
      f"symiri replay @func_{gid}_0({pa0},{pa1}) -> {ret}",
      ok,
      f"rc={r2.returncode}, stdout={r2.stdout[:120]!r}",
    )


def main():
  if len(sys.argv) != 3:
    print("Usage: python3 -m test.lib.run_rysmith_tests <rysmith> <symiri>")
    sys.exit(2)
  rysmith, symiri = sys.argv[1:3]
  print("=== rysmith generation-id banner ===")
  test_id_banner(rysmith)
  print("=== rysmith --seed determinism ===")
  test_seed_id_determinism(rysmith)
  print("=== rysmith --n-params ===")
  test_n_params(rysmith)
  print("=== rysmith descriptor schema ===")
  test_descriptor_schema(rysmith)
  print("=== rysmith SOLVED header replay via symiri ===")
  test_solved_replay(rysmith, symiri)

  passed = sum(1 for _, ok, _ in results if ok)
  total = len(results)
  print(f"\nSummary (rysmith_tests): {passed}/{total} passed.\n")
  sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
  main()
