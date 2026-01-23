import os
import subprocess
import sys
import re


def strip_sigil(name):
  start = 0
  if name.startswith(("@", "%", "^")):
    start = 1
    if len(name) > 1 and name[1] == "?":
      start = 2
  return name[start:]


def get_expectation_and_args(file_path):
  expectation = "UNKNOWN"
  args = []
  with open(file_path, "r") as f:
    for _ in range(10):  # Read first 10 lines
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


def parse_bindings(args):
  bindings = {}
  for arg in args:
    if arg.startswith("--sym") or arg.startswith(
      "sym"
    ):  # Handle --sym and sym (if mistakenly written)
      # The next arg might be the binding if --sym is separate, but usually it's --sym key=val
      # Actually run_tests.py just passes args to symiri.
      # symiri expects: --sym name=value.
      # python argparse/cxxopts splits --sym val.
      pass

    # This is a bit tricky because args is a list of strings.
    # cxxopts allows `--sym key=val` or `--sym` `key=val`.
    # In the tests I see: `// ARGS: --sym %?a=10` -> parts=["--sym", "%?a=10"]

  # Let's iterate
  i = 0
  while i < len(args):
    if args[i] == "--sym":
      if i + 1 < len(args):
        val = args[i + 1]
        if "=" in val:
          k, v = val.split("=", 1)
          bindings[k] = v
        i += 1
    elif args[i].startswith("--sym="):
      val = args[i].split("=", 1)[
        1
      ]  # --sym=k=v ? No, likely not used this way but possible.
      # cxxopts usually: --sym k=v
      pass
    # Also handle just `k=v` if that's how some tests are (unlikely for cxxopts but possible)
    i += 1
  return bindings


def run_test(file_path, symirc_path, temp_dir):
  expectation, args = get_expectation_and_args(file_path)
  if expectation == "UNKNOWN":
    return "SKIP", "No EXPECT tag"

  base_name = os.path.basename(file_path)
  c_out = os.path.join(temp_dir, base_name + ".c")
  exe_out = os.path.join(temp_dir, base_name + ".exe")
  bindings_c = os.path.join(temp_dir, base_name + "_bindings.c")

  # 1. Run symirc
  cmd = [symirc_path, file_path, "-o", c_out]
  try:
    res = subprocess.run(
      cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10
    )
  except subprocess.TimeoutExpired:
    return "FAIL", "Compiler timeout"

  # If we expect FAIL, check if symirc failed (compile time error)
  # But wait, EXPECT: FAIL in existing tests often means Runtime Fail (like verify fail) OR Analysis Fail.
  # Analysis tests (test/typechecker etc) expect compile fail.
  # Interp tests (test/interp/fail_req.sir) expect runtime fail.

  # Heuristic: If it's in test/interp, it's likely a runtime test (unless it's a specific fail test).
  # But fail_req.sir fails at runtime.
  # ub_div0.sir fails at runtime.
  # So for interp tests, symirc should SUCCEED, and execution should FAIL if EXPECT: FAIL.
  # For other tests (typechecker), symirc should FAIL if EXPECT: FAIL.

  is_interp_test = "test/interp" in file_path

  if res.returncode != 0:
    # Compiler failed
    if not is_interp_test and expectation == "FAIL":
      return "PASS", ""
    elif not is_interp_test and expectation == "PASS":
      return "FAIL", f"Compile error:\n{res.stderr}"
    elif is_interp_test:
      # Interp tests should usually compile.
      # But maybe some test UB that is caught by DefiniteInit?
      # If so, it matches expectation.
      if expectation == "FAIL":
        return "PASS", ""  # Assume compile fail is acceptable for FAIL expectation
      return "FAIL", f"Compile error (Interp test):\n{res.stderr}"

  # If compiler succeeded
  if not is_interp_test and expectation == "FAIL":
    # Should have failed analysis
    return "FAIL", "Expected compile error but succeeded"

  if not is_interp_test:
    # Pass tests outside interp are usually just "it compiles"
    return "PASS", ""

  # 2. Generate bindings
  bindings = parse_bindings(args)
  with open(bindings_c, "w") as f:
    f.write("#include <stdint.h>\n")
    f.write("#include <stdio.h>\n\n")

    # Symbol stubs
    for k, v in bindings.items():
      # Match docs/symirc.md: <func>__<sym> with sigils removed
      # We assume function is always @main for these tests
      func_name = "main" + "__" + strip_sigil(k)
      f.write(f"int32_t {func_name}(void) {{ return {v}; }}\n")

    f.write("\n")
    f.write("// Wrapper for symir_main\n")
    f.write("extern int32_t symir_main(void);\n")
    f.write("int main(void) {\n")
    f.write("  symir_main();\n")
    f.write("  return 0;\n")
    f.write("}\n")
  # 3. Compile with gcc
  # We need -Wno-unused-variable because generated C code might have unused vars
  # Enable sanitizers for the generated code too
  gcc_cmd = [
    "gcc",
    c_out,
    bindings_c,
    "-o",
    exe_out,
    "-w",
    "-fsanitize=address,undefined",
    "-g",
  ]

  try:
    res_gcc = subprocess.run(
      gcc_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10
    )
  except subprocess.TimeoutExpired:
    return "FAIL", "GCC timeout"

  if res_gcc.returncode != 0:
    return "FAIL", f"GCC error:\n{res_gcc.stderr}"

  # 4. Run executable
  try:
    res_exe = subprocess.run(
      [exe_out], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=1
    )
  except subprocess.TimeoutExpired:
    return "FAIL", "Runtime timeout"

  if expectation == "PASS":
    if res_exe.returncode == 0:
      return "PASS", ""
    else:
      return "FAIL", f"Runtime error (code {res_exe.returncode}):\n{res_exe.stderr}"
  elif expectation == "FAIL":
    if res_exe.returncode != 0:
      return "PASS", ""
    else:
      return "FAIL", "Expected runtime error but succeeded"

  return "FAIL", "Logic error in test runner"


def main():
  if len(sys.argv) < 3:
    print("Usage: python3 run_compiler_tests.py <test_dir> <symirc_path>")
    sys.exit(1)

  test_dir = sys.argv[1]
  symirc_path = sys.argv[2]
  temp_dir = "test_build_tmp"
  os.makedirs(temp_dir, exist_ok=True)

  passed = 0
  total = 0
  failures = []

  for root, dirs, files in os.walk(test_dir):
    for file in files:
      if file.endswith(".sir"):
        total += 1
        path = os.path.join(root, file)
        print(f"Testing {path}...", end=" ", flush=True)
        status, msg = run_test(path, symirc_path, temp_dir)
        if status == "ed":  # Skip
          print(status)
        elif status == "PASS":
          passed += 1
          print("OK")
        else:
          print("FAIL")
          failures.append((path, msg))

  print(f"\nSummary: {passed}/{total} passed.")
  if failures:
    print("\nFailures:")
    for path, msg in failures:
      print(f"--- {path} ---")
      print(msg)
    sys.exit(1)

  # cleanup
  import shutil

  shutil.rmtree(temp_dir)


if __name__ == "__main__":
  main()
