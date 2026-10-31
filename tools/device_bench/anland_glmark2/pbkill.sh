# MobileGL - tools/device_bench/anland_glmark2/pbkill.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# pbkill.sh - stop every benchmark helper and glmark2 (phone, root)
for p in $(ps -A -o pid,args | grep -E "pbbatch|pbcool|pbsamp|stfix|profrun|pbprof" | grep -v grep | grep -v pbkill | awk '{print $1}'); do kill $p 2>/dev/null; done
pkill -x glmark2-es2-wayland; pkill -x glmark2-wayland; pkill -x glmark2-es2-way; pkill -x glmark2-wayland
sleep 1; ps -A -o pid,args | grep -E "pbbatch|glmark2" | grep -v grep
