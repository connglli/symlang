import os
import sys
import subprocess
import tempfile
from test.lib.framework import run_test_suite, run_command, TestResult


def run_symirsolve_test(symirsolve_path, symiri_path=None):
  def test_func(file_path, expectation, args, skips):
    if "SOLVER" in skips:
      return TestResult.SKIP, "Skipped by SOLVER tag"

    # Solver tests need --main and --path in SOLVER_ARGS metadata
    solver_args = args["SOLVER_ARGS"]

    tmp_path = None
    if symiri_path and expectation == "PASS":
      with tempfile.NamedTemporaryFile(suffix=".sir", delete=False) as tmp:
        tmp_path = tmp.name
      cmd = [symirsolve_path, file_path, "-o", tmp_path] + solver_args
    else:
      cmd = [symirsolve_path, file_path] + solver_args

    try:
      result, err = run_command(cmd, timeout=30)

      if err == "TIMEOUT":
        return TestResult.TIMEOUT, "Solver timeout"

      passed = False
      if expectation == "PASS":
        if result.returncode == 0 and "SAT" in result.stdout:
          passed = True

          # Verify concretized program with interpreter if requested
          if (
            symiri_path
            and tmp_path
            and os.path.exists(tmp_path)
            and os.path.getsize(tmp_path) > 0
          ):
            # Extract --main if present to tell symiri which function to run
            interp_main_args = []
            try:
              main_idx = solver_args.index("--main")
              interp_main_args = ["--main", solver_args[main_idx + 1]]
            except (ValueError, IndexError):
              pass

            interp_cmd = [symiri_path] + interp_main_args + [tmp_path]
            interp_res, interp_err = run_command(interp_cmd, timeout=5)
            if interp_err == "TIMEOUT":
              return TestResult.TIMEOUT, "Interpreter timeout on concretized program"
            if interp_res.returncode != 0:
              return (
                TestResult.FAIL,
                f"Interpreter failed on concretized program.\nSTDOUT:\n{interp_res.stdout}\nSTDERR:\n{interp_res.stderr}",
              )

      elif expectation == "FAIL":
        if result.returncode != 0 or "UNSAT" in result.stdout:
          passed = True

      if passed:
        return TestResult.PASS, ""
      else:
        return TestResult.FAIL, f"STDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}"
    finally:
      if tmp_path and os.path.exists(tmp_path):
        os.remove(tmp_path)

  return test_func


if __name__ == "__main__":
  if len(sys.argv) < 3:
    print(
      "Usage: python3 -m test.lib.run_solver_tests <test_dir> <symirsolve_path> [symiri_path]"
    )
    sys.exit(1)

  test_dir = sys.argv[1]
  symirsolve_path = sys.argv[2]
  symiri_path = sys.argv[3] if len(sys.argv) > 3 else None

  run_test_suite(test_dir, run_symirsolve_test(symirsolve_path, symiri_path))
