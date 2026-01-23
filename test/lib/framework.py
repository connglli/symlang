import os
import subprocess
import sys
import time
from test.lib.style import green, red, yellow, bold


class TestResult:
  PASS = "PASS"
  FAIL = "FAIL"
  TIMEOUT = "TIMEOUT"
  SKIP = "SKIP"


def get_metadata(file_path):
  expectation = "UNKNOWN"
  args = []
  with open(file_path, "r") as f:
    for _ in range(10):
      line = f.readline()
      if not line:
        break
      if "// EXPECT: PASS" in line:
        expectation = "PASS"
      elif "// EXPECT: FAIL" in line:
        expectation = "FAIL"
      if "// ARGS:" in line:
        parts = line.split("ARGS:")[1].strip().split()
        args.extend(parts)
  return expectation, args


def strip_sigil(name):
  start = 0
  if name.startswith(("@", "%", "^")):
    start = 1
    if len(name) > 1 and name[1] == "?":
      start = 2
  return name[start:]


def run_command(cmd, timeout=None):
  try:
    result = subprocess.run(
      cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout
    )
    return result, None
  except subprocess.TimeoutExpired:
    return None, "TIMEOUT"


def run_test_suite(test_dir, test_func):
  passed_count = 0
  failed_count = 0
  timeout_count = 0
  total_count = 0
  failures = []

  test_files = []
  for root, dirs, files in os.walk(test_dir):
    for file in files:
      if file.endswith(".sir"):
        test_files.append(os.path.join(root, file))

  test_files.sort()

  for file_path in test_files:
    total_count += 1
    expectation, args = get_metadata(file_path)

    if expectation == "UNKNOWN":
      print(f"[{yellow('SKIP')}] {file_path}: No EXPECT tag")
      continue

    print(f"Testing {file_path}...", end=" ", flush=True)

    start_time = time.time()
    status, message = test_func(file_path, expectation, args)
    duration = time.time() - start_time

    if status == TestResult.PASS:
      passed_count += 1
      print(green("OK"))
    elif status == TestResult.TIMEOUT:
      timeout_count += 1
      print(yellow("TIMEOUT"))
      failures.append((file_path, "Timeout occurred"))
    else:
      failed_count += 1
      print(red("FAIL"))
      failures.append((file_path, message))

  print(f"\nSummary: {passed_count}/{total_count} passed", end="")
  if timeout_count > 0:
    print(f", {yellow(str(timeout_count) + ' timeouts')}", end="")
  if failed_count > 0:
    print(f", {red(str(failed_count) + ' failed')}", end="")
  print(".")

  if failures:
    print("\n" + bold("Failures Details:"))
    for path, msg in failures:
      print(f"--- {red(path)} ---")
      print(msg)
    sys.exit(1)
  else:
    sys.exit(0)
