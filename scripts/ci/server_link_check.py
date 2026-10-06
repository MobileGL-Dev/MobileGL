#!/usr/bin/env python3
"""P13 W7 (D12): the exemption list for the server-only link check target.

THE TARGET. `MobileGL_server_linkcheck` (CMakeLists.txt) links the SERVER and SHARED module objects
(mg_util, mg_pipe, mg_backend, mg_remote_transport, mg_remote_server) WITHOUT mg_frontend and
mg_remote_client, with `--no-undefined`: a server image must link from its own half of the tree.
The references that still need the frontend are the link ratchet's baseline
(scripts/data/link_ratchet_baseline.txt) - each one a named debt, not an accident - and this script
turns exactly those into named exemptions: one `--defsym=<mangled>=mgl_server_link_check_exempt`
per symbol, written to a compiler-driver response file the target links with. The stub aborts; the target
is a link proof, never run.

WHAT IS REFUSED. A SERVER object's reference that only a FRONTEND object satisfies and that the
baseline does not list fails here, before the link, naming the symbol and the referrer - the same
verdict the ratchet gives, raised by the build. References from SHARED objects (Init, the global
objects, the config loader) into the frontend are exempted and only counted: those files ARE the
library's entry glue, and a real server image replaces them rather than sheds them.

Usage: server_link_check.py --build-dir B --baseline F --out RSP [--nm nm] [--cxxfilt c++filt]
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import link_ratchet as lr  # noqa: E402

STUB = "mgl_server_link_check_exempt"


def nm_mangled(nm_tool, mode, keys, cwd):
    """{key: {mangled: type}} via `nm -o --<mode>-only` (no demangling)."""
    table = {k: {} for k in keys}
    back = {lr.OBJECT_PATHS.get(k, k): k for k in keys}
    for i in range(0, len(keys), 150):
        batch = [lr.OBJECT_PATHS.get(k, k) for k in keys[i:i + 150]]
        done = subprocess.run([nm_tool, "-o", "--" + mode + "-only"] + batch, cwd=cwd,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if done.returncode != 0 and done.stderr.strip():
            raise SystemExit("server-link-check: nm failed: " + done.stderr.strip())
        for line in done.stdout.splitlines():
            head, _, rest = line.partition(":")
            key = back.get(head)
            parts = rest.split()
            if key is None or len(parts) < 2:
                continue
            table[key][parts[-1]] = parts[-2]
    return table


def demangle(names, tool):
    names = sorted(names)
    done = subprocess.run([tool], input="\n".join(names) + "\n", stdout=subprocess.PIPE, text=True,
                          check=True)
    return dict(zip(names, done.stdout.splitlines()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--cxxfilt", default="c++filt")
    args = ap.parse_args()

    root, objects = lr.find_objects(args.build_dir, "MobileGL")
    roles = {o: lr.classify(o) for o in objects}
    unclassified = [o for o, r in roles.items() if r is None]
    if unclassified:
        raise SystemExit("server-link-check: object(s) match no PARTITION row: " + ", ".join(unclassified))
    server = sorted(o for o, r in roles.items() if r == lr.SERVER)
    shared = sorted(o for o, r in roles.items() if r == lr.SHARED)
    frontend = sorted(o for o, r in roles.items() if r == lr.FRONTEND)

    undefined = nm_mangled(args.nm, "undefined", server + shared, root)
    defined = nm_mangled(args.nm, "defined", objects, root)
    image_defined = set()
    for obj in server + shared:
        image_defined.update(defined[obj])
    frontend_defined = set()
    for obj in frontend:
        frontend_defined.update(defined[obj])

    wanted = {}  # mangled -> set of referrer keys
    for obj in server + shared:
        for name in undefined[obj]:
            if name in image_defined or name not in frontend_defined:
                continue
            wanted.setdefault(name, set()).add(obj)

    readable = demangle(wanted, args.cxxfilt)
    baseline, _notes = lr.read_baseline(args.baseline)
    baseline = set(baseline)
    refused, from_shared = [], 0
    for name, referrers in sorted(wanted.items()):
        from_server = [r for r in referrers if roles[r] == lr.SERVER]
        if not from_server:
            from_shared += 1
            continue
        if readable[name] not in baseline:
            refused.append((readable[name], sorted(from_server)))
    if refused:
        for symbol, referrers in refused:
            print("server-link-check: NEW frontend reference from the server half: {}  <- {}"
                  .format(symbol, ", ".join(referrers)))
        print("server-link-check: {} reference(s) the baseline does not name - fix the reference or, "
              "if it is a debt, add it to {} with its reason".format(len(refused), args.baseline))
        return 1
    with open(args.out, "w", encoding="utf-8") as out:
        for name in sorted(wanted):
            out.write("-Wl,--defsym={}={}\n".format(name, STUB))
    print("server-link-check: {} named exemption(s) ({} from server objects, all in the baseline; {} "
          "from the shared entry glue)".format(len(wanted), len(wanted) - from_shared, from_shared))
    return 0


if __name__ == "__main__":
    sys.exit(main())
