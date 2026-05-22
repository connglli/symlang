"""Verify the C backend preamble enforces IEEE 754 and disables FP contraction.

SPEC §2.9 requires strict RNE rounding with no contraction (FMA). C doesn't
mandate IEEE 754 — the implementation declares conformance by predefining
__STDC_IEC_559__. We can't *define* that macro ourselves (the standard
reserves it for the implementation), but we MUST refuse to compile on a
non-conforming platform and we MUST disable contraction so that operations
like (a*b + c) match the SymIR interpreter and WASM lowering.
"""

import os
import subprocess
import sys
import tempfile

CWD = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Minimal self-contained SymIR input. The preamble emission is independent of
# the program body, so any well-typed function works — keep it tiny and own
# the fixture inline so the test never depends on another test's file.
SIR_FIXTURE = """\
fun @main() : i32 {
^entry:
  ret 0;
}
"""


def run():
  symirc = os.path.join(CWD, "symirc")
  with tempfile.NamedTemporaryFile("w", suffix=".sir", delete=False) as f:
    f.write(SIR_FIXTURE)
    sir = f.name
  out = sir.replace(".sir", ".c")
  try:
    r = subprocess.run(
      [symirc, sir, "--target", "c", "-o", out], capture_output=True, text=True
    )
    if r.returncode != 0:
      print("symirc failed:", r.stderr)
      return 1
    with open(out) as f:
      src = f.read()
  finally:
    for p in (sir, out):
      try:
        os.remove(p)
      except OSError:
        pass

  failures = []
  # 1. Hard refusal on non-IEC-559 implementations.
  if "__STDC_IEC_559__" not in src:
    failures.append("missing __STDC_IEC_559__ conformance check")
  # 2. No FMA contraction — SPEC §2.9 forbids it.
  if "FP_CONTRACT" not in src or "OFF" not in src:
    failures.append("missing #pragma STDC FP_CONTRACT OFF")
  # 3. The #error line must be present (not just a comment). The C standard
  #    permits whitespace between `#` and the directive name.
  has_error = "#error" in src or "# error" in src
  if not has_error:
    failures.append("missing #error on non-IEC-559 platforms")

  if failures:
    print("FAIL: C preamble checks")
    for f in failures:
      print(f"  - {f}")
    print()
    print("Emitted preamble (first 25 lines):")
    for line in src.splitlines()[:25]:
      print(f"  {line}")
    return 1

  print("OK: C preamble enforces IEC 559 conformance and FP_CONTRACT OFF")
  return 0


if __name__ == "__main__":
  sys.exit(run())
