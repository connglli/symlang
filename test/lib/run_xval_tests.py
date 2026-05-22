"""Cross-validation tests: run a SymIR program through the interpreter and
through the compiled C lowering, then compare bit-exact outputs.

For integer returns we compare decimal string equality.

For floating-point returns we compare the IEEE 754 hex-float string
(`printf("%a", x)` in C; the same format in symiri). This is round-trip-
exact: it distinguishes `+0` from `-0` (`0x0p+0` vs `-0x0p+0`), preserves
subnormals (`0x1p-150` etc.), and shows the full mantissa. Decimal output
(`%f` / default `<<`) cannot do this.

Each test is a .sir file in test/xval/ that contains:
  - // EXPECT_RC: <decimal-int-or-hex-float>   — required exact output match
  - any normal SymIR body returning the value to compare
"""

import os
import re
import subprocess
import sys
import tempfile

from test.lib.style import bold, green, red

CWD = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

_FUN_RE = re.compile(r"fun\s+@([a-zA-Z0-9_]+)\(\)\s*:\s*(i[0-9]+|f32|f64)")
_RESULT_RE = re.compile(r"Result:\s*(\S+)")
_EXPECT_RE = re.compile(r"//\s*EXPECT_RC:\s*(\S+)")


_CRET = {
  "i8": ("int8_t", "%lld", "(long long)"),
  "i16": ("int16_t", "%lld", "(long long)"),
  "i32": ("int32_t", "%lld", "(long long)"),
  "i64": ("int64_t", "%lld", "(long long)"),
  "f32": ("float", "%a", "(double)"),
  "f64": ("double", "%a", "(double)"),
}


def _parse_fun(sir_path):
  with open(sir_path, "r") as f:
    for line in f:
      m = _FUN_RE.search(line)
      if m:
        return m.group(1), m.group(2)
  return None


def _parse_expect(sir_path):
  with open(sir_path, "r") as f:
    for line in f:
      m = _EXPECT_RE.search(line)
      if m:
        return m.group(1)
  return None


def _run_symiri(symiri, sir_path, fname):
  r = subprocess.run(
    [symiri, "--main", f"@{fname}", sir_path],
    capture_output=True,
    text=True,
    timeout=10,
  )
  if r.returncode != 0:
    return None, f"symiri exit {r.returncode}: {r.stderr.strip()}"
  m = _RESULT_RE.search(r.stdout) or _RESULT_RE.search(r.stderr)
  if m is None:
    return None, "no Result line"
  return m.group(1), None


def _run_compiled(symirc, gcc, sir_path, fname, rtype):
  cret, fmt, cast = _CRET[rtype]
  with tempfile.TemporaryDirectory() as td:
    c_path = os.path.join(td, "prog.c")
    main_path = os.path.join(td, "main.c")
    exe = os.path.join(td, "exe")
    r = subprocess.run(
      [symirc, sir_path, "--target", "c", "-o", c_path],
      capture_output=True,
      text=True,
    )
    if r.returncode != 0:
      return None, f"symirc failed: {r.stderr.strip()}"
    with open(main_path, "w") as f:
      f.write(
        "#include <stdint.h>\n"
        "#include <stdio.h>\n"
        f"extern {cret} symir_{fname}(void);\n"
        "int main(void) {\n"
        f"  {cret} r = symir_{fname}();\n"
        f'  printf("Result: {fmt}\\n", {cast}r);\n'
        "  return 0;\n"
        "}\n"
      )
    r = subprocess.run(
      [
        gcc,
        c_path,
        main_path,
        "-o",
        exe,
        "-fsanitize=undefined",
        "-fno-sanitize-recover=all",
        "-w",
        "-lm",
      ],
      capture_output=True,
      text=True,
      timeout=15,
    )
    if r.returncode != 0:
      return None, f"gcc failed: {r.stderr.strip()[:200]}"
    r = subprocess.run([exe], capture_output=True, text=True, timeout=5)
    if r.returncode != 0:
      return None, f"binary trapped: {r.stderr.strip()[:200]}"
    m = _RESULT_RE.search(r.stdout)
    if m is None:
      return None, "no Result from binary"
    return m.group(1), None


def main():
  xval_dir = os.path.join(CWD, "test/xval")
  symiri = os.path.join(CWD, "symiri")
  symirc = os.path.join(CWD, "symirc")
  if not os.path.isdir(xval_dir):
    print(f"no xval tests at {xval_dir} — nothing to do")
    return 0

  files = sorted(
    os.path.join(xval_dir, f) for f in os.listdir(xval_dir) if f.endswith(".sir")
  )
  passed = 0
  failed = []
  for sir in files:
    name = os.path.basename(sir)
    expect = _parse_expect(sir)
    fun = _parse_fun(sir)
    if fun is None:
      failed.append((name, "could not parse fun decl"))
      continue
    fname, rtype = fun
    if rtype not in _CRET:
      failed.append((name, f"unsupported return type {rtype}"))
      continue
    sym_val, sym_err = _run_symiri(symiri, sir, fname)
    if sym_err:
      failed.append((name, f"interp: {sym_err}"))
      continue
    c_val, c_err = _run_compiled(symirc, "gcc", sir, fname, rtype)
    if c_err:
      failed.append((name, f"compiled: {c_err}"))
      continue
    if sym_val != c_val:
      failed.append((name, f"divergence: interp={sym_val} compiled={c_val}"))
      continue
    if expect is not None and sym_val != expect:
      failed.append((name, f"expected={expect} but got={sym_val}"))
      continue
    print(f"  {green('OK')}   {name}: {sym_val}")
    passed += 1

  print()
  print(bold(f"xval (interp ⇄ compiled C): {passed}/{len(files)} passed"))
  if failed:
    print(bold("Failures:"))
    for n, msg in failed:
      print(f"  {red('FAIL')} {n}: {msg}")
    return 1
  return 0


if __name__ == "__main__":
  sys.exit(main())
