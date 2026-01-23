import os
import subprocess
import sys


def get_expectation_and_args(file_path):
  expectation = "UNKNOWN"
  args = []
  with open(file_path, "r") as f:
    for _ in range(5):
      line = f.readline()
      if not line:
        break
      if "// EXPECT: PASS" in line:
        expectation = "PASS"
      elif "// EXPECT: FAIL" in line:
        expectation = "FAIL"

      if "// ARGS:" in line:
        # Parse args
        parts = line.split("ARGS:")[1].strip().split()
        args.extend(parts)

  return expectation, args


def run_tests(test_dir, binary_cmd_parts):
  failed_tests = []
  passed_count = 0
  total_count = 0

  for root, dirs, files in os.walk(test_dir):
    for file in files:
      if file.endswith(".sir"):
        total_count += 1
        file_path = os.path.join(root, file)
        expectation, extra_args = get_expectation_and_args(file_path)

        if expectation == "UNKNOWN":
          print(f"[SKIP] {file_path}: No EXPECT tag")
          continue

        # Run binary
        cmd = binary_cmd_parts + [file_path] + extra_args
        result = subprocess.run(
          cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )

        passed = False
        if expectation == "PASS":
          if result.returncode == 0:
            passed = True
        elif expectation == "FAIL":
          if result.returncode != 0:
            passed = True

        if passed:
          passed_count += 1
          print(f"[OK] {file_path}")
        else:
          failed_tests.append(file_path)
          print(f"[FAIL] {file_path}")
          print("--- OUTPUT ---")
          print(result.stdout)
          print("--- STDERR ---")
          print(result.stderr)

  print(f"\nSummary: {passed_count}/{total_count} tests passed.")
  if failed_tests:
    sys.exit(1)
  else:
    sys.exit(0)


if __name__ == "__main__":
  if len(sys.argv) < 3:
    print("Usage: python3 run_tests.py <test_dir> <binary_path> [args...]")
    sys.exit(1)
  run_tests(sys.argv[1], sys.argv[2:])
