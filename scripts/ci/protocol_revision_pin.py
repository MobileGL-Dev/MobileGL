#!/usr/bin/env python3
"""The control-schema revision pin (P7 wave 2-F, F fix round; PH-7 (4) review).

MOBILEGL_PROTOCOL_CONTROL_REVISION (MobileGL/MG_Remote/Protocol/mg_protocol_base.h) is mixed into
wireFingerprint so that a control-schema change moves the fingerprint and two peers built on
either side of it refuse each other by name (Refuse{WireFingerprint}). The revision is a
hand-bumped integer, and until this gate nothing tied it to protocol.fbs's CONTENT: a later
protocol.fbs change without a bump would leave old and new peers agreeing on wireFingerprint and
surface as a silently absent field - the exact failure the revision was added to prevent.

This script pins the two together. protocol_revision_pins.json beside it is a table
{revision: sha256(protocol.fbs)} of the schema each revision was minted for. The check reads the
header's current revision, hashes protocol.fbs (line endings normalised, so a CRLF checkout hashes
the same bytes as an LF one) and refuses when the current revision has no row or its row is not
the file's digest. Both directions are covered: a schema edit that forgot the bump is red (stale
row), and a bump that forgot the table is red (no row), so the table cannot fall behind the header.

Registered as `ProtocolSchema.RevisionPinsDigest` (label unit, MG_Test/Wire/CMakeLists.txt) and as
a step of CI's flatc-check job - the job that regenerates the header on any protocol.fbs change,
which is where a forgotten bump would otherwise sail through.

    python3 scripts/ci/protocol_revision_pin.py               the check
    python3 scripts/ci/protocol_revision_pin.py --self-test   the check, plus proof it goes red
    python3 scripts/ci/protocol_revision_pin.py --write       after a bump: pin the new revision

`--write` refuses to overwrite an existing row with a different digest: that is what a schema edit
without a bump looks like, and the answer to it is a bump, not a re-pin (`--force` for the rare
fix-up of the bump commit itself).
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "MobileGL" / "MG_Remote" / "Protocol" / "mg_protocol_base.h"
SCHEMA = ROOT / "MobileGL" / "MG_Remote" / "Protocol" / "protocol.fbs"
PINS = Path(__file__).with_name("protocol_revision_pins.json")

REVISION = re.compile(r"^\s*#define\s+MOBILEGL_PROTOCOL_CONTROL_REVISION\s+(\d+)\s*$", re.MULTILINE)


def read_revision(header_text):
    """The header's current revision, or None when the define is missing."""
    found = REVISION.search(header_text)
    return int(found.group(1)) if found else None


def schema_digest(schema_bytes):
    """sha256 of the schema with CRLF folded to LF, hex."""
    return hashlib.sha256(schema_bytes.replace(b"\r\n", b"\n")).hexdigest()


def check(revision, digest, pins):
    """The failures, as messages; empty when the pin holds."""
    if revision is None:
        return [f"{HEADER.relative_to(ROOT).as_posix()} has no `#define "
                f"MOBILEGL_PROTOCOL_CONTROL_REVISION <n>` line to pin"]
    row = pins.get(str(revision))
    if row is None:
        return [f"MOBILEGL_PROTOCOL_CONTROL_REVISION is {revision} and "
                f"{PINS.relative_to(ROOT).as_posix()} has no row for it. A bump records the "
                f"schema it was minted for in the same commit: "
                f"`python3 scripts/ci/protocol_revision_pin.py --write`."]
    if row != digest:
        return [f"protocol.fbs has sha256 {digest} but MOBILEGL_PROTOCOL_CONTROL_REVISION is still "
                f"{revision}, which is pinned to {row}. A control-schema change a peer must agree "
                f"on needs a revision bump (mg_protocol_base.h) so wireFingerprint moves and an "
                f"old peer is refused by name instead of misreading the new frame; then "
                f"`python3 scripts/ci/protocol_revision_pin.py --write` pins the new revision."]
    return []


def load_pins():
    if not PINS.is_file():
        return {}
    return json.loads(PINS.read_text(encoding="utf-8"))


def self_test(revision, digest, pins):
    """The check must go red on the two perturbations it exists for, and green on the truth."""
    failures = []
    touched = schema_digest(SCHEMA.read_bytes() + b"\n// touched without a bump\n")
    if not check(revision, touched, pins):
        failures.append("self-test: a protocol.fbs edit with no revision bump was NOT refused")
    if not check(revision + 1, digest, pins):
        failures.append("self-test: a revision bump with no table row was NOT refused")
    if check(revision, digest, pins):
        failures.append("self-test: the recorded pin itself does not hold (see the check's own line)")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--write", action="store_true",
                        help="record the current revision's digest in protocol_revision_pins.json")
    parser.add_argument("--force", action="store_true",
                        help="with --write: overwrite an existing row that disagrees")
    parser.add_argument("--self-test", action="store_true",
                        help="also prove the check goes red on a touched schema and on a rowless bump")
    args = parser.parse_args()

    revision = read_revision(HEADER.read_text(encoding="utf-8"))
    digest = schema_digest(SCHEMA.read_bytes())
    pins = load_pins()

    if args.write:
        if revision is None:
            print("::error::no MOBILEGL_PROTOCOL_CONTROL_REVISION define to pin", file=sys.stderr)
            return 1
        existing = pins.get(str(revision))
        if existing is not None and existing != digest and not args.force:
            print(f"::error::revision {revision} is already pinned to {existing} and protocol.fbs "
                  f"now hashes to {digest}: that is a schema change without a bump. Bump "
                  f"MOBILEGL_PROTOCOL_CONTROL_REVISION first, then --write (or --force to re-pin "
                  f"this revision on purpose).", file=sys.stderr)
            return 1
        pins[str(revision)] = digest
        ordered = {key: pins[key] for key in sorted(pins, key=int)}
        PINS.write_text(json.dumps(ordered, indent=2) + "\n", encoding="utf-8")
        print(f"protocol revision pin: revision {revision} -> sha256 {digest}")
        return 0

    failures = check(revision, digest, pins)
    if args.self_test and not failures:
        failures = self_test(revision, digest, pins)
    if failures:
        for message in failures:
            print(f"::error::{message}", file=sys.stderr)
        return 1
    print(f"protocol revision pin: revision {revision} holds (sha256 {digest[:16]}..., "
          f"{len(pins)} revision(s) pinned{', self-test red on both perturbations' if args.self_test else ''})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
