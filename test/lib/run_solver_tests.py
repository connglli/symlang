import os
import sys
import subprocess
from test.lib.framework import run_test_suite, run_command, TestResult


def run_symirsolve_test(symirsolve_path):
  def test_func(file_path, expectation, args, skips):
    if "SOLVER" in skips:
      return TestResult.SKIP, "Skipped by SOLVER tag"

    # Solver tests need --main and --path in ARGS metadata
    # // ARGS: --main @f --path '^entry,^exit'
    cmd = [symirsolve_path, file_path] + args
    result, err = run_command(cmd, timeout=10)

    if err == "TIMEOUT":
      return TestResult.TIMEOUT, "Solver timeout"

    passed = False
    if expectation == "PASS":
      if result.returncode == 0 and "SAT" in result.stdout:
        passed = True
    elif expectation == "FAIL":
      if result.returncode != 0 or "UNSAT" in result.stdout:
        passed = True

    if passed:
      return TestResult.PASS, ""
    else:
      return TestResult.FAIL, f"STDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}"

  return test_func


if __name__ == "__main__":
  if len(sys.argv) < 3:
    print("Usage: python3 -m test.lib.run_solver_tests <test_dir> <symirsolve_path>")
    sys.exit(1)

  test_dir = sys.argv[1]
  symirsolve_path = sys.argv[2]

  run_test_suite(test_dir, run_symirsolve_test(symirsolve_path))
