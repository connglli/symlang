import sys
from test.lib.framework import run_test_suite, run_command, TestResult


def run_symiri_test(binary_cmd_parts):
  def test_func(file_path, expectation, extra_args, skips):
    if "INTERPRETER" in skips:
      return TestResult.SKIP, "Skipped by INTERPRETER tag"

    cmd = binary_cmd_parts + [file_path] + extra_args
    result, err = run_command(cmd, timeout=5)

    if err == "TIMEOUT":
      return TestResult.TIMEOUT, "Timeout"

    passed = False
    if expectation == "PASS":
      if result.returncode == 0:
        passed = True
    elif expectation == "FAIL":
      if result.returncode != 0:
        passed = True

    if passed:
      return TestResult.PASS, ""
    else:
      return TestResult.FAIL, f"STDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}"

  return test_func


if __name__ == "__main__":
  if len(sys.argv) < 3:
    print("Usage: python3 run_tests.py <test_dir> <binary_path> [args...]")
    sys.exit(1)

  test_dir = sys.argv[1]
  binary_cmd_parts = sys.argv[2:]

  run_test_suite(test_dir, run_symiri_test(binary_cmd_parts))
