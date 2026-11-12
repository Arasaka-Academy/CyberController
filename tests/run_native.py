#!/usr/bin/env python3
"""Build and run protocol and firmware regressions without a connected device."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="cybercontroller-tests-") as temporary:
        for name in ("protocol_test", "firmware_test", "pyramid_test", "wifi_queue_test"):
            executable = str(Path(temporary) / name)
            subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                 *(["-DCYBERCONTROLLER_ATOMS3_PYRAMID=1"] if name == "pyramid_test" else []),
                 "-I", str(ROOT / "tests/support"), "-I", str(ROOT / "include"),
                 str(ROOT / "tests" / f"{name}.cpp"), "-o", executable], check=True,
            )
            subprocess.run([executable], check=True)
            print(f"PASS {name} (ASAN/UBSAN)", flush=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
