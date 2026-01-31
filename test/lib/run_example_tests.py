import os
import sys
import re
import tempfile
from test.lib.framework import run_test_suite, run_command, TestResult
from test.lib.style import green, yellow, red


def run_example_test(symirsolve_path, symiri_path):
  def test_func(file_path, expectation, args, skips):
    with open(file_path, "r") as fd:
      content = fd.read()

    # Regex to find the symirsolve command in comments
    match = re.search(r"\.\/symirsolve\s+(.*)", content)
    if not match:
      return TestResult.SKIP, "No symirsolve command found in file"

    line = match.group(1).split("\n")[0].strip()

    # Handle $(cat path_file) anywhere in the line
    while True:
      cat_match = re.search(r"\$\(\s*cat\s+(.*?)\s*\)", line)
      if not cat_match:
        break
      path_file = cat_match.group(1).strip()
      if path_file.startswith("./"):
        path_file = path_file[2:]
      try:
        with open(path_file, "r") as pf:
          val = pf.read().strip()
        line = line.replace(cat_match.group(0), val)
      except Exception as e:
        return TestResult.FAIL, f"Could not read path file {path_file}: {e}"

    parts = line.split()
    processed_args = []
    skip_next = False
    for i, p in enumerate(parts):
      if skip_next:
        skip_next = False
        continue
      if p in ["-o", "--output"]:
        skip_next = True
        continue
      if p.endswith(".sir") and i == len(parts) - 1:
        continue
      processed_args.append(p)

    with tempfile.NamedTemporaryFile(suffix=".sir", delete=False) as tmp:
      tmp_path = tmp.name

    try:
      # 1. Run symirsolve
      cmd = [symirsolve_path, file_path, "-o", tmp_path] + processed_args
      result, err = run_command(cmd, timeout=30)

      if err == "TIMEOUT":
        return TestResult.TIMEOUT, "symirsolve timeout"

      if result.returncode != 0:
        if "UNKNOWN" in result.stdout:
          return TestResult.SKIP, "UNKNOWN"
        return (
          TestResult.FAIL,
          f"symirsolve failed.\nSTDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}",
        )

      if "SAT" not in result.stdout:
        if "UNKNOWN" in result.stdout:
          return TestResult.SKIP, "UNKNOWN"
        return (
          TestResult.FAIL,
          f"symirsolve did not return SAT.\nSTDOUT:\n{result.stdout}",
        )

      # 2. Run symiri verification
      interp_main_args = []
      if "--main" in processed_args:
        idx = processed_args.index("--main")
        interp_main_args = ["--main", processed_args[idx + 1]]

      interp_cmd = [symiri_path] + interp_main_args + [tmp_path]
      interp_res, interp_err = run_command(interp_cmd, timeout=5)

      if interp_err == "TIMEOUT":
        return TestResult.TIMEOUT, "symiri timeout"

      if interp_res.returncode != 0:
        return (
          TestResult.FAIL,
          f"symiri verification failed.\nSTDOUT:\n{interp_res.stdout}\nSTDERR:\n{interp_res.stderr}",
        )

      return TestResult.PASS, ""

    finally:
      if os.path.exists(tmp_path):
        os.remove(tmp_path)

  return test_func


if __name__ == "__main__":
  if len(sys.argv) < 4:
    print(
      "Usage: python3 -m test.lib.run_example_tests <examples_dir> <symirsolve_path> <symiri_path>"
    )
    sys.exit(1)

  run_test_suite(sys.argv[1], run_example_test(sys.argv[2], sys.argv[3]))
