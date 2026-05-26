import sys

from test.lib.framework import TestResult, run_command, run_test_suite

# Maps EXPECT: FAIL:<subtype> to the exit code the tool must return.
FAIL_EXIT_CODES = {
  "FAIL:LexError": 2,
  "FAIL:ParseError": 3,
  "FAIL:StaticError": 4,
  "FAIL:UndefinedBehavior": 5,
  "FAIL:RequireViolation": 6,
}


def run_symiri_test(binary_cmd_parts):
  def test_func(file_path, expectation, args, skips):
    if "INTERPRETER" in skips:
      return TestResult.SKIP, "Skipped by INTERPRETER tag"

    cmd = binary_cmd_parts + [file_path] + args["INTERP_ARGS"]
    result, err = run_command(cmd, timeout=5)

    if err == "TIMEOUT":
      return TestResult.TIMEOUT, "Timeout"

    passed = False
    if expectation == "PASS":
      passed = result.returncode == 0
    elif expectation == "FAIL":
      passed = result.returncode != 0
    elif expectation in FAIL_EXIT_CODES:
      passed = result.returncode == FAIL_EXIT_CODES[expectation]
    else:
      passed = result.returncode != 0  # unknown subtype: any failure

    if passed:
      return TestResult.PASS, ""
    else:
      return (
        TestResult.FAIL,
        f"exit code {result.returncode} (expected {expectation})\n"
        f"STDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}",
      )

  return test_func


if __name__ == "__main__":
  if len(sys.argv) < 3:
    print("Usage: python3 run_tests.py <test_dir> <binary_path> [args...]")
    sys.exit(1)

  test_dir = sys.argv[1]
  binary_cmd_parts = sys.argv[2:]

  run_test_suite("interp_tests", test_dir, run_symiri_test(binary_cmd_parts))
