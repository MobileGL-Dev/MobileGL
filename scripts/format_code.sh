#!/usr/bin/env bash
# MobileGL - scripts/format_code.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# Reformat in place every file the CI format check covers (is_formatted in check_text_hygiene.py:
# tracked C/C++ sources, minus generated files, .inc fragments and FORMAT_SKIP). The tree is
# already formatted, so this only rewrites lines that drifted. CLANG_FORMAT overrides the binary.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 - <<'EOF'
import importlib.util
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

spec = importlib.util.spec_from_file_location("hygiene", os.path.join("scripts", "check_text_hygiene.py"))
hygiene = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hygiene)
binary = os.environ.get("CLANG_FORMAT") or shutil.which("clang-format-23") or shutil.which("clang-format")
if not binary:
    sys.exit("clang-format not found; install it with: pip install 'clang-format==23.1.3'")
print(subprocess.run([binary, "--version"], stdout=subprocess.PIPE).stdout.decode().strip())
files = [f for f in hygiene.git("ls-files", *hygiene.SOURCE_DIRS).splitlines() if hygiene.is_formatted(f)]


def format_one(path):
    before = open(path, "rb").read()
    subprocess.run([binary, "-i", "--style=file", path], check=True)
    return open(path, "rb").read() != before


with ThreadPoolExecutor(os.cpu_count() or 4) as pool:
    changed = sum(pool.map(format_one, files))
print("%d of %d files reformatted" % (changed, len(files)))
EOF
