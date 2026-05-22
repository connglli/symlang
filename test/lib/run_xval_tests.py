"""Cross-validation tests: run a SymIR program through the interpreter and
through the compiled C lowering, then compare bit-exact outputs.

For integer returns we compare decimal string equality. For floating-point
returns we compare the IEEE 754 hex-float string (`printf %a`) — round-trip-
exact for binary64, distinguishes +0/-0, preserves subnormals.

Each test is a .sir file with:
  - // EXPECT: PASS                              (framework metadata tag)
  - // EXPECT_RC: <decimal-int-or-hex-float>     (this runner's tag)

Usage: python3 -m test.lib.run_xval_tests <test_dir> <symiri> <symirc>
"""

import os
import re
import subprocess
import sys
import tempfile

from test.lib.framework import TestResult, run_command, run_test_suite

_FUN_RE = re.compile(r"fun\s+@([a-zA-Z0-9_]+)\(\)\s*:\s*(i[0-9]+|f32|f64)")
_RESULT_RE = re.compile(r"Result:\s*(\S+)")
_EXPECT_RC_RE = re.compile(r"//\s*EXPECT_RC:\s*(\S+)")

# Per-SymIR-type C harness format: (C return type, printf format, value cast).
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


def _parse_expect_rc(sir_path):
  with open(sir_path, "r") as f:
    for line in f:
      m = _EXPECT_RC_RE.search(line)
      if m:
        return m.group(1)
  return None


def run_xval_test(symiri_path, symirc_path, gcc_path):
  def test_func(file_path, expectation, args, skips):
    if "XVAL" in skips:
      return TestResult.SKIP, "Skipped by XVAL tag"
    if expectation != "PASS":
      return TestResult.SKIP, "xval only validates EXPECT: PASS programs"

    fun = _parse_fun(file_path)
    if fun is None:
      return TestResult.FAIL, "could not parse `fun @<name>() : <type>` declaration"
    fname, rtype = fun
    if rtype not in _CRET:
      return TestResult.FAIL, f"unsupported return type {rtype}"

    # 1. Interpreter side: run symiri and capture the `Result:` line.
    interp_cmd = [symiri_path, "--main", f"@{fname}", file_path]
    result, err = run_command(interp_cmd, timeout=10)
    if err == "TIMEOUT":
      return TestResult.TIMEOUT, "symiri timeout"
    if result.returncode != 0:
      return TestResult.FAIL, (
        f"symiri exit {result.returncode}\nSTDERR:\n{result.stderr}"
      )
    m = _RESULT_RE.search(result.stdout) or _RESULT_RE.search(result.stderr)
    if m is None:
      return TestResult.FAIL, "symiri produced no `Result:` line"
    interp_val = m.group(1)

    # 2. Compiled side: lower with symirc, link with a tiny harness, run.
    cret, fmt, cast = _CRET[rtype]
    with tempfile.TemporaryDirectory() as td:
      c_out = os.path.join(td, "prog.c")
      main_c = os.path.join(td, "main.c")
      exe = os.path.join(td, "exe")

      r = subprocess.run(
        [symirc_path, file_path, "--target", "c", "-o", c_out],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
      )
      if r.returncode != 0:
        return TestResult.FAIL, f"symirc failed:\n{r.stderr}"

      with open(main_c, "w") as f:
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

      gcc_cmd = [
        gcc_path,
        c_out,
        main_c,
        "-o",
        exe,
        "-fsanitize=undefined",
        "-fno-sanitize-recover=all",
        "-w",
        "-lm",
      ]
      r, err = run_command(gcc_cmd, timeout=15)
      if err == "TIMEOUT":
        return TestResult.TIMEOUT, "gcc timeout"
      if r.returncode != 0:
        return TestResult.FAIL, f"gcc failed:\n{r.stderr}"

      r, err = run_command([exe], timeout=5)
      if err == "TIMEOUT":
        return TestResult.TIMEOUT, "binary timeout"
      if r.returncode != 0:
        return TestResult.FAIL, f"binary trapped:\n{r.stderr}"
      m = _RESULT_RE.search(r.stdout)
      if m is None:
        return TestResult.FAIL, "compiled binary produced no `Result:` line"
      compiled_val = m.group(1)

    # 3. Compare bit-exactly and against EXPECT_RC if present.
    if interp_val != compiled_val:
      return TestResult.FAIL, (
        f"divergence: interp={interp_val} compiled={compiled_val}"
      )
    expect_rc = _parse_expect_rc(file_path)
    if expect_rc is not None and interp_val != expect_rc:
      return TestResult.FAIL, (f"EXPECT_RC={expect_rc} but got={interp_val}")
    return TestResult.PASS, ""

  return test_func


if __name__ == "__main__":
  if len(sys.argv) < 4:
    print(
      "Usage: python3 -m test.lib.run_xval_tests <test_dir> <symiri> <symirc> [gcc]"
    )
    sys.exit(1)
  test_dir = sys.argv[1]
  symiri = sys.argv[2]
  symirc = sys.argv[3]
  gcc = sys.argv[4] if len(sys.argv) > 4 else "gcc"
  run_test_suite(test_dir, run_xval_test(symiri, symirc, gcc))
