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


def extract_sir_info(file_path):
  info = {"main_ret": "i32", "syms": {}}
  with open(file_path, "r") as f:
    content = f.read()
    # Remove comments
    import re

    content = re.sub(r"//.*", "", content)

    # Match main return type: fun @main(...) : i64
    main_match = re.search(r"fun\s+@main\s*\([^)]*\)\s*:\s*([a-z0-9]+)", content)
    if main_match:
      ret_type = main_match.group(1).strip()
      if ret_type == "i64":
        info["main_ret"] = "i64"
      elif ret_type == "f32":
        info["main_ret"] = "f32"
      elif ret_type == "f64":
        info["main_ret"] = "f64"
    else:
      # Try without colon if any
      if re.search(r"fun\s+@main\s*\([^)]*\)\s+i64", content):
        info["main_ret"] = "i64"

    # FOR ll_ tests, if we still didn't find it, look for the ret instruction
    if info["main_ret"] == "i32" and "test/interp/ll_" in file_path:
      if "ret %" in content or "ret @" in content:
        # This is a bit weak but let's see
        pass

    # Match symbols: sym %?name : value i64;
    sym_matches = re.finditer(
      r"sym\s+(%?[\?a-z0-9_]+)\s*:\s*value\s+([a-z0-9]+)", content
    )
    for m in sym_matches:
      name = m.group(1)
      if name.startswith("%?"):
        name = name[2:]
      elif name.startswith("@?"):
        name = name[2:]
      elif name.startswith("%") or name.startswith("@"):
        name = name[1:]

      t = m.group(2)
      info["syms"][name] = "i64" if t == "i64" else "i32"

  return info


def run_symirc_test(symirc_path, target="c"):
  temp_dir = "test_build_tmp"
  os.makedirs(temp_dir, exist_ok=True)

  # Check for WASM runtime if needed
  wasm_runtime = None
  if target == "wasm":
    if shutil.which("wasmtime"):
      wasm_runtime = "wasmtime"
    elif shutil.which("wasmer"):
      wasm_runtime = "wasmer"

  def test_func(file_path, expectation, args, skips):
    if "COMPILER" in skips:
      return TestResult.SKIP, "Skipped by COMPILER tag"

    if target == "wasm" and "WASM" in skips:
      return TestResult.SKIP, "Skipped by WASM tag"

    if target == "wasm" and not wasm_runtime:
      return TestResult.SKIP, "No WASM runtime found (wasmtime or wasmer)"

    base_name = os.path.basename(file_path)
    output_ext = ".c" if target == "c" else ".wat"
    gen_out = os.path.join(temp_dir, base_name + output_ext)
    exe_out = os.path.join(temp_dir, base_name + ".exe")

    # 1. Run symirc
    cmd = [symirc_path, file_path, "--target", target, "-o", gen_out, "-w"]
    if target == "wasm":
      cmd.append("--no-module-tags")

    result, err = run_command(cmd, timeout=10)
    if err == "TIMEOUT":
      return TestResult.TIMEOUT, "Compiler timeout"

    is_runnable_test = "test/interp" in file_path or "test/compile" in file_path

    if result.returncode != 0:
      if expectation == "FAIL":
        return TestResult.PASS, ""
      return TestResult.FAIL, f"symirc failed:\n{result.stderr}"

    if not is_runnable_test:
      if expectation == "FAIL":
        return TestResult.FAIL, "Expected compile error but succeeded"
      return TestResult.PASS, ""

    bindings = parse_bindings(args)

    if target == "c":
      bindings_c = os.path.join(temp_dir, base_name + "_bindings.c")
      # Generate bindings harness for C
      with open(bindings_c, "w") as f:
        f.write("#include <stdint.h>\n#include <stdio.h>\n\n")
        for k, v in bindings.items():
          func_name = "main" + "__" + strip_sigil(k)
          f.write(f"int32_t {func_name}(void) {{ return {v}; }}\n")
        f.write("\nextern int32_t symir_main(void);\n")
        f.write("int main(void) {\n  symir_main();\n  return 0;\n}\n")

      # Compile with gcc
      gcc_cmd = [
        "gcc",
        gen_out,
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

      # Run executable
      run_cmd = [exe_out]
    else:
      # WASM execution
      sir_info = extract_sir_info(file_path)
      combined_wat = os.path.join(temp_dir, base_name + "_combined.wat")

      with open(gen_out, "r") as f:
        gen_content = f.read()

      with open(combined_wat, "w") as f:
        f.write("(module\n")
        import re

        processed_content = gen_content
        if bindings:
          for k, v in bindings.items():
            sk = strip_sigil(k)
            t = sir_info["syms"].get(sk, "i32")
            # wasm_backend mangles symbol as $main__<name> or similar
            # module name can be anything (stripped function name)
            # Match: (import "..." "sk" (func $mangled_name (result ...)))
            pattern = rf'\(import\s+"[^"]+"\s+"{sk}"\s+\(func\s+(\$[^\s\)]+)\s+\(result\s+[^\s\)]+\)\)\)'
            replacement = f"(func \\1 (result {t}) ({t}.const {v}))"
            processed_content = re.sub(pattern, replacement, processed_content)

        f.write(processed_content)
        f.write(
          f'\n  (func (export "main") (result {sir_info["main_ret"]}) (call $main))\n'
        )
        f.write(")\n")

      if wasm_runtime == "wasmtime":
        run_cmd = ["wasmtime", "run", "--invoke", "main", combined_wat]
      else:
        run_cmd = ["wasmer", "run", combined_wat, "--", "main"]

    res_exe, err_exe = run_command(run_cmd, timeout=2)
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
  import argparse

  parser = argparse.ArgumentParser()
  parser.add_argument("test_dir")
  parser.add_argument("symirc_path")
  parser.add_argument("--target", choices=["c", "wasm"], default="c")
  args = parser.parse_args()

  test_func, temp_dir = run_symirc_test(args.symirc_path, args.target)
  try:
    run_test_suite(args.test_dir, test_func)
  finally:
    if os.path.exists(temp_dir):
      shutil.rmtree(temp_dir)
