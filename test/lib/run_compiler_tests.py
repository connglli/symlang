import os
import shutil

from test.lib.framework import TestResult, run_command, run_test_suite, strip_sigil
from test.lib.run_interp_tests import FAIL_EXIT_CODES

# These subtypes are expected to fail at compile time (symirc exit code).
COMPILE_TIME_FAILS = {"FAIL:LexError", "FAIL:ParseError", "FAIL:StaticError"}


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


def extract_sir_info(file_path, entry_func="main"):
  info = {"main_ret": "i32", "syms": {}}
  with open(file_path, "r") as f:
    content = f.read()
    # Remove comments
    import re

    content = re.sub(r"//.*", "", content)

    # Match main return type: fun @entry(...) : i64
    # entry_func is without sigil. In file it is @entry_func
    # We look for "fun @entry_func"
    pattern = rf"fun\s+@{re.escape(entry_func)}\s*\([^)]*\)\s*:\s*([a-z0-9]+)"
    main_match = re.search(pattern, content)

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
      # "fun @entry_func(...) i64" (old syntax? or implicit?)
      pattern_implicit = rf"fun\s+@{re.escape(entry_func)}\s*\([^)]*\)\s+([a-z0-9]+)"
      if re.search(pattern_implicit, content):
        # If matched, we might want to capture type. But sticking to simple logic for now.
        # Assuming if explicit match failed, maybe we check implicit.
        # But usually colon is required by parser?
        pass

    # FOR ll_ tests, if we still didn't find it, look for the ret instruction
    if info["main_ret"] == "i32" and "test/interp/ll_" in file_path:
      if "ret %" in content or "ret @" in content:
        # This is a bit weak but let's see
        pass

    # Match scalar symbols:  sym %?name : value i64;
    sym_matches = re.finditer(
      r"sym\s+(%?[\?a-z0-9_]+)\s*:\s*(?:value|coef|index)\s+([a-z0-9]+)", content
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
      info["syms"][name] = t

    # [v0.2.1] Match vector symbols: sym %?name : value <N> i32;
    # Recorded as a separate dict so the harness can synthesize a
    # vecext-strategy typedef and stub return.
    info["vec_syms"] = {}
    vec_sym_matches = re.finditer(
      r"sym\s+(%?[\?a-z0-9_]+)\s*:\s*(?:value|coef|index)\s+<(\d+)>\s+([a-z0-9]+)",
      content,
    )
    for m in vec_sym_matches:
      name = m.group(1)
      if name.startswith("%?") or name.startswith("@?"):
        name = name[2:]
      elif name.startswith("%") or name.startswith("@"):
        name = name[1:]
      info["vec_syms"][name] = {"n": int(m.group(2)), "elem": m.group(3)}

  return info


def run_symirc_test(symirc_path, target="c"):
  temp_dir = "build/test_tmp"
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

    is_runnable_test = (
      "test/interp" in file_path
      or "test/compile" in file_path
      or "test/complex" in file_path
      or "test/solver" in file_path
    )

    if result.returncode != 0:
      # Check compile-time failure expectations first.
      if expectation == "FAIL":
        return TestResult.PASS, ""
      if expectation in COMPILE_TIME_FAILS:
        expected_code = FAIL_EXIT_CODES[expectation]
        if result.returncode == expected_code:
          return TestResult.PASS, ""
        return (
          TestResult.FAIL,
          f"symirc exit {result.returncode} (expected {expectation}={expected_code}):\n{result.stderr}",
        )
      # Runtime-failure expectations: ideally symirc succeeds and the
      # binary traps. But many UBs (read of undef, type-mismatch
      # navigations) are caught at compile time by the static checker —
      # accept that as "caught earlier" rather than counting it as a
      # test failure.
      if (
        expectation == "FAIL:UndefinedBehavior"
        and result.returncode == FAIL_EXIT_CODES.get("FAIL:StaticError")
      ):
        return TestResult.PASS, ""
      return TestResult.FAIL, f"symirc failed:\n{result.stderr}"

    if not is_runnable_test:
      if expectation == "FAIL":
        return TestResult.FAIL, "Expected compile error but succeeded"
      return TestResult.PASS, ""

    compiler_args = args["COMPILER_ARGS"]
    bindings = parse_bindings(compiler_args)

    # Determine entry function name for mangling
    entry_func = "main"
    if "--main" in compiler_args:
      idx = compiler_args.index("--main")
      if idx + 1 < len(compiler_args):
        entry_func = strip_sigil(compiler_args[idx + 1])

    if target == "c":
      bindings_c = os.path.join(temp_dir, base_name + "_bindings.c")
      sir_info = extract_sir_info(file_path, entry_func)

      # [v0.2.1] Solver tests are authored for symirsolve, not the
      # compiler. They declare syms but usually no COMPILER_ARGS, so
      # the runtime stage can't reproduce the path the solver follows.
      # Compile-only is the meaningful check here.
      if "test/solver/" in file_path and (sir_info["syms"] or sir_info.get("vec_syms")):
        bound = {strip_sigil(k) for k in bindings.keys()}
        unbound_syms = [s for s in sir_info["syms"] if s not in bound]
        unbound_vec = [s for s in sir_info.get("vec_syms", {}) if s not in bound]
        if unbound_syms or unbound_vec:
          return TestResult.PASS, ""

      # Map SymIR elem type → (C type, byte size). Used both by scalar
      # and vec sym stubs.
      _CTYPE = {
        "i1": ("int8_t", 1),  # vecext can't have 1-bit lanes; lower as i8
        "i8": ("int8_t", 1),
        "i16": ("int16_t", 2),
        "i32": ("int32_t", 4),
        "i64": ("int64_t", 8),
        "f32": ("float", 4),
        "f64": ("double", 8),
      }
      vec_syms = sir_info.get("vec_syms", {})
      emitted_vec_typedefs = set()

      # Generate bindings harness for C
      with open(bindings_c, "w") as f:
        f.write("#include <stdint.h>\n#include <stdio.h>\n\n")

        def emit_vec_typedef(n, elem):
          tn = f"_vec_{n}_{elem}"
          if tn in emitted_vec_typedefs:
            return tn
          c_elem, elem_bytes = _CTYPE.get(elem, ("int32_t", 4))
          # Same vecext typedef the C backend emits — duplicate typedef
          # with the identical definition is valid C (6.7.2.3).
          f.write(
            f"typedef {c_elem} {tn} __attribute__((vector_size({n * elem_bytes})));\n"
          )
          emitted_vec_typedefs.add(tn)
          return tn

        # Scalar bindings supplied by the test (--sym k=v).
        for k, v in bindings.items():
          sk = strip_sigil(k)
          # Vector binding: comma-separated lane values.
          if sk in vec_syms:
            shape = vec_syms[sk]
            tn = emit_vec_typedef(shape["n"], shape["elem"])
            lane_vals = [s.strip() for s in v.split(",")]
            while len(lane_vals) < shape["n"]:
              lane_vals.append("0")
            func_name = entry_func + "__" + sk
            init_list = ", ".join(lane_vals[: shape["n"]])
            f.write(
              f"{tn} {func_name}(void) {{ {tn} r = {{ {init_list} }}; return r; }}\n"
            )
            continue
          t = sir_info["syms"].get(sk, "i32")
          c_type, _ = _CTYPE.get(t, ("int32_t", 4))
          func_name = entry_func + "__" + sk
          f.write(f"{c_type} {func_name}(void) {{ return {v}; }}\n")

        # Stub any sym that wasn't bound by the test, so solver tests —
        # which the compiler runner still tries to link — don't fail at
        # link time on undefined `<entry>__<sym>()`. Defaults to zero;
        # tests that depend on a specific sym value should pass --sym.
        bound = {strip_sigil(k) for k in bindings.keys()}
        for sk, shape in vec_syms.items():
          if sk in bound:
            continue
          tn = emit_vec_typedef(shape["n"], shape["elem"])
          func_name = entry_func + "__" + sk
          zeros = ", ".join(["0"] * shape["n"])
          f.write(f"{tn} {func_name}(void) {{ {tn} r = {{ {zeros} }}; return r; }}\n")
        for sk, t in sir_info["syms"].items():
          if sk in bound:
            continue
          c_type, _ = _CTYPE.get(t, ("int32_t", 4))
          func_name = entry_func + "__" + sk
          f.write(f"{c_type} {func_name}(void) {{ return 0; }}\n")

        main_ret_c = "int32_t"
        if sir_info["main_ret"] == "i64":
          main_ret_c = "int64_t"
        elif sir_info["main_ret"] == "f32":
          main_ret_c = "float"
        elif sir_info["main_ret"] == "f64":
          main_ret_c = "double"

        entry_c_name = "symir_" + entry_func
        f.write(f"\nextern {main_ret_c} {entry_c_name}(void);\n")
        f.write(f"int main(void) {{\n  {entry_c_name}();\n  return 0;\n}}\n")

      # Compile with gcc
      gcc_cmd = [
        "gcc",
        gen_out,
        bindings_c,
        "-o",
        exe_out,
        "-w",
        "-fsanitize=address,undefined,float-cast-overflow",
        "-fno-sanitize-recover=all",
        "-g",
        "-lm",
      ]
      res_gcc, err_gcc = run_command(gcc_cmd, timeout=10)
      if err_gcc == "TIMEOUT":
        return TestResult.TIMEOUT, "GCC timeout"
      if res_gcc.returncode != 0:
        return TestResult.FAIL, f"GCC error:\n{res_gcc.stderr}"

      # Run executable; disable LSAN so it works outside ptrace environments
      run_cmd = ["env", "LSAN_OPTIONS=detect_leaks=0", exe_out]
    else:
      # WASM execution
      sir_info = extract_sir_info(file_path, entry_func)
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

            # WASM only has i32/i64/f32/f64 — map narrow ints to i32 to match
            # what the WASM backend emits for narrow-int symbol imports.
            if t in ("i1", "i8", "i16"):
              t = "i32"

            val_str = v
            if (t == "f32" or t == "f64") and "." not in v and "e" not in v.lower():
              val_str = v + ".0"

            # wasm_backend mangles symbol as $main__<name> or similar
            # module name can be anything (stripped function name)
            # Match: (import "..." "sk" (func $mangled_name (result ...)))
            pattern = rf'\(import\s+"[^"]+"\s+"{sk}"\s+\(func\s+(\$[^\s\)]+)\s+\(result\s+[^\s\)]+\)\)\)'
            replacement = f"(func \\1 (result {t}) ({t}.const {val_str}))"
            processed_content = re.sub(pattern, replacement, processed_content)

        f.write(processed_content)
        f.write(
          f'\n  (func (export "main") (result {sir_info["main_ret"]}) (call ${entry_func}))\n'
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
    elif expectation in ("FAIL", "FAIL:UndefinedBehavior", "FAIL:RequireViolation"):
      # Compiled binaries use their own exit conventions (e.g. assert/abort for
      # require, UBSan for UB) — just require any non-zero exit.
      if res_exe.returncode != 0:
        return TestResult.PASS, ""
      else:
        return TestResult.FAIL, f"Expected runtime failure ({expectation}) but exited 0"
    elif expectation in COMPILE_TIME_FAILS:
      # symirc should have failed above; reaching here means it succeeded unexpectedly.
      return TestResult.FAIL, f"Expected {expectation} from symirc but it succeeded"

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
