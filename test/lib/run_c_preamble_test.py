"""Verify the C backend preamble enforces IEEE 754 and disables FP contraction.

SPEC §2.9 requires strict RNE rounding with no contraction (FMA). C doesn't
mandate IEEE 754 — the implementation declares conformance by predefining
__STDC_IEC_559__. We can't *define* that macro ourselves (the standard
reserves it for the implementation), but we MUST refuse to compile on a
non-conforming platform and we MUST disable contraction so that operations
like `a*b + c` match the SymIR interpreter and WASM lowering.

This is a single-shot test, not a per-file test suite — it invokes symirc
on a minimal fixture and greps the emitted preamble. Output format mirrors
the framework's per-file test runners so make-test output stays uniform.
"""

import os
import subprocess
import sys
import tempfile
import time

from test.lib.style import bold, green, red

CWD = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Minimal self-contained SymIR input. The preamble is independent of the
# program body, so any well-typed function works.
SIR_FIXTURE = """\
fun @main() : i32 {
^entry:
  ret 0;
}
"""


def run(symirc):
  with tempfile.NamedTemporaryFile("w", suffix=".sir", delete=False) as f:
    f.write(SIR_FIXTURE)
    sir = f.name
  out = sir.replace(".sir", ".c")
  try:
    start = time.time()
    print(f"Testing C preamble via {symirc}...", end=" ", flush=True)
    r = subprocess.run(
      [symirc, sir, "--target", "c", "-o", out],
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
      text=True,
    )
    duration_ms = int((time.time() - start) * 1000)
    if r.returncode != 0:
      print(f"{red('FAIL')} ({duration_ms}ms)")
      print(f"--- {red('symirc failed')} ---")
      print(r.stderr)
      return 1

    with open(out) as f:
      src = f.read()

    failures = []
    if "__STDC_IEC_559__" not in src:
      failures.append("missing __STDC_IEC_559__ conformance check")
    if "FLT_EVAL_METHOD" not in src:
      failures.append("missing FLT_EVAL_METHOD check for extended precision")
    if "FP_CONTRACT" not in src or "OFF" not in src:
      failures.append("missing #pragma STDC FP_CONTRACT OFF")
    if "#error" not in src and "# error" not in src:
      failures.append("missing #error directive on non-IEC-559 platforms")

    if failures:
      print(f"{red('FAIL')} ({duration_ms}ms)")
      print(bold("\nFailures Details:"))
      print(f"--- {red('C preamble checks')} ---")
      for msg in failures:
        print(f"  - {msg}")
      print("\nEmitted preamble (first 25 lines):")
      for line in src.splitlines()[:25]:
        print(f"  {line}")
      return 1
    print(f"{green('OK')} ({duration_ms}ms)")
    return 0
  finally:
    for p in (sir, out):
      try:
        os.remove(p)
      except OSError:
        pass


if __name__ == "__main__":
  if len(sys.argv) > 1:
    symirc = sys.argv[1]
  else:
    symirc = os.path.join(CWD, "symirc")
  sys.exit(run(symirc))
