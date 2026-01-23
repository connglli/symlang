import os
import sys
import shutil
from test.lib.framework import run_test_suite, run_command, TestResult, strip_sigil


def parse_bindings(args):
  bindings = {}
  i = 0
  while i < len(args):
    if args[i] == "--sym":
      if i + 1 < len(args):
        val = args[i + 1]
        if "=" in val:
          k, v = val.split("=", 1)
          bindings[k] = v
        i += 1
    i += 1
  return bindings


def run_symirc_test(symirc_path):
  temp_dir = "test_build_tmp"
  os.makedirs(temp_dir, exist_ok=True)

  def test_func(file_path, expectation, args):
    base_name = os.path.basename(file_path)
    c_out = os.path.join(temp_dir, base_name + ".c")
    exe_out = os.path.join(temp_dir, base_name + ".exe")
    bindings_c = os.path.join(temp_dir, base_name + "_bindings.c")

    # 1. Run symirc
    cmd = [symirc_path, file_path, "-o", c_out]
    result, err = run_command(cmd, timeout=10)
    if err == "TIMEOUT":
      return TestResult.TIMEOUT, "Compiler timeout"

    is_interp_test = "test/interp" in file_path

    if result.returncode != 0:
      if expectation == "FAIL":
        return TestResult.PASS, ""
      return TestResult.FAIL, f"symirc failed:\n{result.stderr}"

    if not is_interp_test:
      if expectation == "FAIL":
        return TestResult.FAIL, "Expected compile error but succeeded"
      return TestResult.PASS, ""

    # 2. Generate bindings harness
    bindings = parse_bindings(args)
    with open(bindings_c, "w") as f:
      f.write("#include <stdint.h>\n#include <stdio.h>\n\n")
      for k, v in bindings.items():
        func_name = "main" + "__" + strip_sigil(k)
        f.write(f"int32_t {func_name}(void) {{ return {v}; }}\n")
      f.write("\nextern int32_t symir_main(void);\n")
      f.write("int main(void) {\n  symir_main();\n  return 0;\n}\n")

    # 3. Compile with gcc
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
    res_gcc, err_gcc = run_command(gcc_cmd, timeout=10)
    if err_gcc == "TIMEOUT":
      return TestResult.TIMEOUT, "GCC timeout"
    if res_gcc.returncode != 0:
      return TestResult.FAIL, f"GCC error:\n{res_gcc.stderr}"

    # 4. Run executable
    res_exe, err_exe = run_command([exe_out], timeout=1)
    if err_exe == "TIMEOUT":
      return TestResult.TIMEOUT, "Runtime timeout"

    if expectation == "PASS":
      if res_exe.returncode == 0:
        return TestResult.PASS, ""
      else:
        return (
          TestResult.FAIL,
          f"Runtime error (code {res_exe.returncode}):\n{res_exe.stderr}",
        )
    elif expectation == "FAIL":
      if res_exe.returncode != 0:
        return TestResult.PASS, ""
      else:
        return TestResult.FAIL, "Expected runtime error but succeeded"

    return TestResult.FAIL, "Logic error"

  return test_func, temp_dir


if __name__ == "__main__":
  if len(sys.argv) < 3:
    print("Usage: python3 run_compiler_tests.py <test_dir> <symirc_path>")
    sys.exit(1)

  test_dir = sys.argv[1]
  symirc_path = sys.argv[2]

  test_func, temp_dir = run_symirc_test(symirc_path)
  try:
    run_test_suite(test_dir, test_func)
  finally:
    if os.path.exists(temp_dir):
      shutil.rmtree(temp_dir)
