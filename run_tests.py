import os
import subprocess
import sys


def get_expectation(file_path):
  with open(file_path, "r") as f:
    for _ in range(5):
      line = f.readline()
      if "// EXPECT: PASS" in line:
        return "PASS"
      if "// EXPECT: FAIL" in line:
        return "FAIL"
  return "UNKNOWN"


def run_tests(test_dir, binary_path):
  failed_tests = []
  passed_count = 0
  total_count = 0

  for root, dirs, files in os.walk(test_dir):
    for file in files:
      if file.endswith(".sir"):
        total_count += 1
        file_path = os.path.join(root, file)
        expectation = get_expectation(file_path)

        if expectation == "UNKNOWN":
          print(f"[SKIP] {file_path}: No EXPECT tag")
          continue

        # Run binary
        # The binary returns 0 if the test matched expectation, 1 otherwise
        result = subprocess.run(
          [binary_path, file_path],
          stdout=subprocess.PIPE,
          stderr=subprocess.PIPE,
          text=True,
        )

        if result.returncode == 0:
          passed_count += 1
          print(f"[OK] {file_path}")
        else:
          failed_tests.append(file_path)
          print(f"[FAIL] {file_path}")
          print("--- OUTPUT ---")
          print(result.stdout)
          # print("--- STDERR ---")
          # print(result.stderr) # Stdout usually contains the error message printed by sym_test

  print(f"\nSummary: {passed_count}/{total_count} tests passed.")
  if failed_tests:
    sys.exit(1)
  else:
    sys.exit(0)


if __name__ == "__main__":
  if len(sys.argv) < 3:
    print("Usage: python3 run_tests.py <test_dir> <binary_path>")
    sys.exit(1)
  run_tests(sys.argv[1], sys.argv[2])
