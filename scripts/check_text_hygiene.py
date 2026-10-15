#!/usr/bin/env python3
# MobileGL - scripts/check_text_hygiene.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""Check commit messages and source text against CONTRIBUTING.md.

    python3 scripts/check_text_hygiene.py scope                  # scope line for the staged changes
    python3 scripts/check_text_hygiene.py message MSG_FILE       # a message file, against the staged changes
    python3 scripts/check_text_hygiene.py commits RANGE          # the commits CI would check in a range
    python3 scripts/check_text_hygiene.py tree [--update-baseline]
    python3 scripts/check_text_hygiene.py format RANGE           # clang-format on each commit's changed lines
    python3 scripts/check_text_hygiene.py ci                     # what CI runs
    python3 scripts/check_text_hygiene.py self-test

Commits: only the branch's own line of history is checked (first parents), and only commits made
after this script was added. A merge commit's subject is checked, not the commits it brings in.

Tree: strict checks fail on any hit. Ratchets compare a total with
scripts/data/text_hygiene_baseline.json and fail when the total grows or when the baseline itself
was raised. Totals are global rather than per file, so moving code between files does not trip
them.
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE_PATH = "scripts/data/text_hygiene_baseline.json"
SELF_PATH = "scripts/check_text_hygiene.py"

# The header rules live in the script that writes headers; load it rather than restate them.
_spec = importlib.util.spec_from_file_location("source_headers", os.path.join(ROOT, "scripts", "update-source-file-header.py"))
HEADERS = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(HEADERS)

TYPES = {"Feat", "Fix", "Perf", "Refactor", "Test", "Docs", "Build", "CI", "Chore", "Tools"}
MERGE_TYPE = "Merge"
CONTENT_MAX = 72
CONTINUATION_MAX = 2
COMMENT_BLOCK_MAX = 15
BASE_BRANCH = os.environ.get("TEXT_HYGIENE_BASE_BRANCH", "feat/disaggregated")

SUBJECT_RE = re.compile(r"^\[(?P<types>[^\]]+)\] \((?P<scopes>[^)]+)\): (?P<content>\S.*)$")
# Ids from the phased planning process: decision ids, contract documents, lettered phases, and a
# phase number followed by a wave, round or review label. Plain "P3" or "P99" stays ordinary text.
PLANNING_RE = re.compile(
    r"\bID-(?:P\d+-)?\d+\b"
    r"|\bCONTRACT-[A-Z0-9]+"
    r"|\bP\d{1,2}[a-f]\b"
    r"|\bP\d{1,2}(?:\.\d)?(?=\s+(?:W\d|S\d|wave|round|review|package|phase|close|contract|integrat)|:\s|\))"
    r"|(?<=[Pp]hase )P\d{1,2}[a-f]?\b")
LOCAL_PATH_RE = re.compile(r"/mnt/[a-z]/|/home/\w|/Users/\w|\b[A-Za-z]:[\\/]Users[\\/]")
# File, package and extension names that contain these words (50_mesa.json, libegl1-mesa-dev,
# GL_MESA_*) are allowed; the project names on their own are not.
EXTERNAL_NAME_RE = re.compile(r"(?<![\w./-])(Mesa|mesa|MESA|Zink|zink|ZINK|Gallium|gallium)(?!\w)")
CITATION_RE = re.compile(r"(?<![\w/.-])((?:[\w.-]+/)*[\w.-]+\.(?:h|hpp|cpp|cc|c|inc|def|py|md|json|yml|yaml|txt|fbs))"
                         r":(\d+)(?:-(\d+))?")
CJK_RE = re.compile("[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af\uf900-\ufaff\uff01-\uff60]")
# Three or more capitalised words in a row count as shouting only when two of them are function
# words; a run of acronyms ("RGB TO YUV", "GLSL, HLSL, MSL") is ordinary text.
CAPS_RUN_RE = re.compile(r"\b[A-Z][A-Z']+(?:[ ,;:-]+[A-Z][A-Z']+){2,}\b")
CAPS_FUNCTION_WORDS = {"THE", "IS", "ARE", "NOT", "AND", "OR", "MUST", "NEVER", "ONLY", "THIS", "THAT", "EVERY",
                       "ALL", "NO", "BE", "IT", "OF", "TO", "IN", "ON", "AT", "AS", "BY", "FOR", "WITH", "WHY",
                       "WHAT", "WHEN", "DO", "DOES", "CAN", "CANNOT", "WILL", "AN", "IF", "BUT", "ONE", "TWO"}
TRAILER_ITEM_RE = re.compile(r"^- [\w-]+-[Bb]y:|Co-Authored-By", re.IGNORECASE)
HEADER_MARK = "End of Source File Header"

SLASH_COMMENT_EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".def", ".java", ".mjs",
                     ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese", ".glsl")
HASH_COMMENT_EXT = (".py", ".sh", ".cmake", ".yml", ".yaml")
FORMAT_EXT = (".c", ".cc", ".cpp", ".h", ".hpp")  # .inc fragments cannot be formatted on their own
CITATION_TEXT_EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".def", ".fbs", ".py", ".sh", ".cmake", ".txt",
                     ".yml", ".yaml")
SOURCE_DIRS = ("MobileGL/", "scripts/", "tools/")
# Files whose exact bytes are pinned elsewhere: changing them, even a comment, means a protocol
# revision bump (scripts/ci/protocol_revision_pins.json), so only the UTF-8 rule applies to them.
FROZEN = {"MobileGL/MG_Remote/Protocol/protocol.fbs"}
NOTES_DIR = "docs/Disaggregated/notes/"
RATCHETS = ("line_citations", "planning_code_comment_lines", "long_comment_excess_lines",
            "allcaps_comment_lines", "non_english_comment_lines")


# ----------------------------------------------------------------------------------------- git

def git(*args, check=True, repo=ROOT, data=None):
    proc = subprocess.run(["git", "-C", repo, "-c", "core.quotePath=false"] + list(args),
                          input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and proc.returncode != 0:
        raise SystemExit("git %s failed: %s" % (" ".join(args), proc.stderr.decode("utf-8", "replace").strip()))
    return proc.stdout.decode("utf-8", "replace")


def is_generated(path):
    return "/generated/" in path or "/gen/" in path or os.path.basename(path).startswith("generated_")


def submodule_paths(repo=ROOT):
    out = git("config", "--file", ".gitmodules", "--get-regexp", r"submodule\..*\.path", check=False, repo=repo)
    return {line.split(" ", 1)[1].strip() for line in out.splitlines() if " " in line}


# --------------------------------------------------------------------------------------- scope

def scope_of(path, submodules=frozenset()):
    """The scope a changed path belongs to: the path without a leading MobileGL/, cut to its first
    two parts. A shorter path is used whole; a file directly in MobileGL/ uses "MobileGL"; a
    submodule uses its path."""
    if path in submodules:
        return path
    parts = path.split("/")
    if parts[0] == "MobileGL":
        parts = parts[1:]
        if len(parts) == 1:
            return "MobileGL"
    if len(parts) <= 2:
        return parts[0]
    return parts[0] + "/" + parts[1]


def scopes_for(paths, submodules):
    return sorted({scope_of(p, submodules) for p in paths}, key=str.lower)


# ------------------------------------------------------------------------------------- message

def external_names(text):
    """Project names in text, skipping lowercase package and path names (mesa-libEGL-devel, mesa/)."""
    return [m.group(1) for m in EXTERNAL_NAME_RE.finditer(text)
            if not (m.group(1).islower() and text[m.end():m.end() + 1] in ("-", "/"))]


def check_message(message, changed_paths, added_paths, submodules, is_merge=False):
    """Return the list of problems with one commit message."""
    problems = []
    if message.startswith("\ufeff"):
        problems.append("message starts with a byte-order mark; save it as UTF-8 without one")
        message = message[1:]
    lines = message.replace("\r\n", "\n").rstrip("\n").split("\n")
    subject = lines[0]
    m = SUBJECT_RE.match(subject)
    if not m:
        return problems + ["subject is not '[Type] (Scope): content': %r" % subject]

    types = [t.strip() for t in m.group("types").split(",")]
    if is_merge:
        if types != [MERGE_TYPE]:
            problems.append("a merge commit uses [Merge]")
    else:
        bad = [t for t in types if t not in TYPES]
        if bad:
            problems.append("unknown type %s; use %s" % (", ".join(bad), ", ".join(sorted(TYPES))))
        if changed_paths:
            listed = [s.strip() for s in m.group("scopes").split(",")]
            expected = scopes_for(changed_paths, submodules)
            if sorted(listed, key=str.lower) != expected:
                problems.append("scope should be (%s), not (%s)" % (", ".join(expected), m.group("scopes")))

    content = m.group("content")
    if len(content) > CONTENT_MAX:
        problems.append("content after '): ' is %d characters; the limit is %d" % (len(content), CONTENT_MAX))

    whole = "\n".join(lines)
    if PLANNING_RE.search(whole):
        problems.append("message carries a planning id (%s)" % PLANNING_RE.search(whole).group(0))
    if LOCAL_PATH_RE.search(whole):
        problems.append("message contains a local filesystem path")
    if external_names(whole):
        problems.append("message names an external project (%s)" % external_names(whole)[0])
    if CJK_RE.search(whole):
        problems.append("message is not in English")

    body = lines[1:]
    if body:
        if body[0].strip():
            problems.append("line 2 must be blank")
        continuation = None
        for line in body[1:]:
            if not line.strip():
                continuation = None
                continue
            if line.startswith("- "):
                continuation = 0
                if TRAILER_ITEM_RE.search(line):
                    problems.append("no trailers, not even as a detail line (%r)" % line[:60])
            elif continuation is not None and line.startswith("  ") and continuation < CONTINUATION_MAX:
                continuation += 1
            else:
                problems.append("detail lines start with '- ' and wrap onto at most %d indented lines (got %r)"
                                % (CONTINUATION_MAX, line[:60]))
                break

    for path in added_paths:
        if path.startswith(NOTES_DIR):
            problems.append("adds %s: working papers stay outside the repository" % path)
        elif path.endswith(".log"):
            problems.append("adds %s: logs are not committed" % path)
    return problems


def commit_paths(sha, repo=ROOT):
    def names(*extra):
        return [p for p in git("diff-tree", "--no-commit-id", "-r", "--no-renames", "--name-only", "--root",
                               *extra, sha, repo=repo).splitlines() if p]
    return names(), names("--diff-filter=A")


def grandfather_cutoff(repo=ROOT):
    """The commit that added this script; commits before it predate the rules."""
    added = git("log", "--diff-filter=A", "--format=%H", "--", SELF_PATH, check=False, repo=repo).split()
    return added[-1] if added else None


def commits_to_check(rev_range, repo=ROOT):
    args = ["rev-list", "--first-parent", "--reverse", rev_range]
    cutoff = grandfather_cutoff(repo)
    if cutoff:
        parent = git("rev-parse", "--verify", "--quiet", cutoff + "^", check=False, repo=repo).strip()
        if parent:
            args.append("^" + parent)
    return [s for s in git(*args, repo=repo).splitlines() if s]


def is_merge(sha, repo=ROOT):
    return len(git("rev-list", "--parents", "-n", "1", sha, repo=repo).split()) > 2


def check_commits(rev_range, repo=ROOT):
    shas = commits_to_check(rev_range, repo)
    submodules = submodule_paths(repo)
    failures = 0
    for sha in shas:
        merge = is_merge(sha, repo)
        message = git("log", "-1", "--format=%B", sha, repo=repo)
        changed, added = ([], []) if merge else commit_paths(sha, repo)
        problems = check_message(message, changed, added, submodules, is_merge=merge)
        for p in problems:
            report(None, None, "%s %s: %s" % (sha[:9], message.split("\n", 1)[0][:60], p))
        failures += bool(problems)
    print("commits: %d checked, %d with problems" % (len(shas), failures))
    return failures == 0


# ------------------------------------------------------------------------------------ comments

def comments(text):
    """Yield (line, comment_text, own_line) for every comment in C-family source. own_line is True
    when nothing but whitespace precedes the comment on its line."""
    i, n, line, line_start = 0, len(text), 1, 0
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            line_start = i + 1
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            yield line, text[i + 2:j], not text[line_start:i].strip()
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j
            own = not text[line_start:i].strip()
            for k, part in enumerate(text[i + 2:j].split("\n")):
                yield line + k, part, own or k > 0
            line += text.count("\n", i, j)
            i = j + 2
        elif c == '"':
            raw = re.search(r'(?:^|[^\w])(?:u8|u|U|L)?R$', text[max(0, i - 3):i])
            if raw:
                k = text.find("(", i + 1)
                delim = text[i + 1:k]
                j = text.find(")" + delim + '"', k)
                j = n if j < 0 else j + len(delim) + 2
                line += text.count("\n", i, j)
                i = j
            else:
                i = skip_quoted(text, i, '"')
        elif c == "'":
            k = i - 1
            while k >= 0 and (text[k].isalnum() or text[k] in "_.'"):
                k -= 1
            if text[k + 1:i][:1].isdigit():  # a digit separator: 1'000'000
                i += 1
            else:
                i = skip_quoted(text, i, "'")
        else:
            i += 1


def skip_quoted(text, i, quote):
    """Index just past a quoted literal starting at i; an unterminated literal ends before its
    newline so the newline is still counted."""
    j = i + 1
    while j < len(text) and text[j] != quote and text[j] != "\n":
        j += 2 if text[j] == "\\" and text[j + 1:j + 2] != "\n" else 1
    return j + 1 if j < len(text) and text[j] == quote else j


def hash_comments(text):
    """Yield (line, comment_text, own_line) for # comments in Python, shell, CMake and YAML."""
    for lineno, raw in enumerate(text.split("\n"), 1):
        if lineno == 1 and raw.startswith("#!"):
            continue
        stripped = re.sub(r"\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'", lambda m: " " * len(m.group(0)), raw)
        m = re.search(r"(?:^|\s)#", stripped)
        if m:
            k = m.end()
            yield lineno, raw[k:].rstrip("\r"), not stripped[:k - 1].strip()


def comment_style(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in HASH_COMMENT_EXT or os.path.basename(path) == "CMakeLists.txt":
        return hash_comments
    if ext in SLASH_COMMENT_EXT:
        return comments
    return None


def comment_blocks(found):
    """Group own-line comments on consecutive lines into blocks of (first_line, [texts])."""
    blocks, current, last = [], None, None
    for line, body, own in found:
        if not own:
            continue
        if last is not None and line == last + 1:
            current[1].append(body)
        elif last is not None and line == last:
            current[1][-1] += " " + body
        else:
            current = (line, [body])
            blocks.append(current)
        last = line
    return blocks


def is_shouting(text):
    return any(len(CAPS_FUNCTION_WORDS & set(re.split(r"[ ,;:-]+", m.group(0)))) >= 2
               for m in CAPS_RUN_RE.finditer(text))


# ---------------------------------------------------------------------------------------- tree

def read_file(path):
    try:
        with open(os.path.join(ROOT, path), "rb") as fh:
            return fh.read()
    except OSError:
        return b""


def checks_comments(path):
    if is_generated(path) or path.startswith(("3rdparty/", "include/", "docs/")):
        return None
    style = comment_style(path)
    if style is comments and path.startswith(SOURCE_DIRS):
        return style
    if style is hash_comments and (path.startswith(SOURCE_DIRS + (".github/",)) or path == "CMakeLists.txt"):
        return style
    return None


def scan_tree(files, read=read_file):
    """Return (strict_problems, totals, per_file_totals) for the given paths."""
    by_base = {}
    for p in files:
        by_base.setdefault(os.path.basename(p), []).append(p)
    lengths = {}

    def length(path):
        if path not in lengths:
            lengths[path] = read(path).count(b"\n") + 1
        return lengths[path]

    strict = []
    totals = {k: 0 for k in RATCHETS}
    per_file = {}

    def bump(key, path, k=1):
        totals[key] += k
        per_file.setdefault(key, {}).setdefault(path, 0)
        per_file[key][path] += k

    for path in files:
        if path.startswith(("3rdparty/", "include/")):
            continue
        raw = read(path)
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            strict.append((path, 1, "is not valid UTF-8"))
            text = raw.decode("utf-8", "replace")
        if path in FROZEN:
            continue

        if HEADERS.covered(path):
            status, _ = HEADERS.plan(path, text)
            if status not in HEADERS.QUIET:
                strict.append((path, 1, "license header: %s (python3 scripts/update-source-file-header.py %s)"
                               % (status, path)))

        # Count on LF text, so a CRLF checkout gives the same totals as CI.
        text = text.replace("\r\n", "\n")
        ext = os.path.splitext(path)[1].lower()
        if (not path.startswith("docs/") and not is_generated(path)
                and (ext in CITATION_TEXT_EXT or os.path.basename(path) == "CMakeLists.txt")):
            for lineno, line in enumerate(text.split("\n"), 1):
                for m in CITATION_RE.finditer(line):
                    bump("line_citations", path)
                    name, end = m.group(1), int(m.group(3) or m.group(2))
                    cands = ([p for p in files if p.endswith("/" + name) or p == name] if "/" in name
                             else by_base.get(name, []))
                    if cands and all(length(c) < end for c in cands):
                        strict.append((path, lineno, "%s points past the end of %s" % (m.group(0), cands[0])))

        style = checks_comments(path)
        if style is None:
            continue
        found = list(style(text))
        for lineno, body, _own in found:
            names = external_names(body)
            if names:
                strict.append((path, lineno, "names an external project (%s); describe the technique instead" % names[0]))
            if PLANNING_RE.search(body):
                bump("planning_code_comment_lines", path)
            if is_shouting(body):
                bump("allcaps_comment_lines", path)
            if CJK_RE.search(body):
                bump("non_english_comment_lines", path)
        for _first, block in comment_blocks(found):
            marks = [k for k, b in enumerate(block) if HEADER_MARK in b]
            if marks:
                block = block[marks[-1] + 1:]
            if len(block) > COMMENT_BLOCK_MAX:
                bump("long_comment_excess_lines", path, len(block) - COMMENT_BLOCK_MAX)
    return strict, totals, per_file


def text_files(repo=ROOT):
    """Tracked files git treats as text."""
    out = git("ls-files", "--eol", repo=repo)
    files = []
    for line in out.splitlines():
        info, _, path = line.partition("\t")
        if path and not info.startswith("i/-text"):
            files.append(path)
    return files


def load_baseline(text):
    try:
        return json.loads(text)
    except ValueError:
        return None


def check_tree(update_baseline=False, base=None):
    strict, totals, per_file = scan_tree(text_files())
    for path, line, msg in strict:
        report(path, line, msg)
    ok = not strict
    path = os.path.join(ROOT, BASELINE_PATH)
    try:
        with open(path, encoding="utf-8") as fh:
            baseline = json.load(fh)
    except OSError:
        baseline = {}
    if update_baseline:
        lowered = {k: min(baseline.get(k, v), v) for k, v in totals.items()}
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(lowered, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("baseline written (it can only go down): %s" % lowered)
        return ok
    previous = load_baseline(git("show", "%s:%s" % (base, BASELINE_PATH), check=False)) if base else None
    for key in RATCHETS:
        now, allowed = totals[key], baseline.get(key, 0)
        if previous and key in previous and allowed > previous[key]:
            ok = False
            report(BASELINE_PATH, 1, "the %s baseline was raised from %d to %d; it can only go down"
                   % (key, previous[key], allowed))
        if now > allowed:
            ok = False
            top = sorted(per_file.get(key, {}).items(), key=lambda kv: -kv[1])[:5]
            report(None, None, "%s grew from %d to %d; largest: %s"
                   % (key, allowed, now, ", ".join("%s (%d)" % kv for kv in top)))
        elif now < allowed:
            notice("%s fell from %d to %d: lower the baseline with --update-baseline" % (key, allowed, now))
    print("tree: %d strict problems; totals %s" % (len(strict), totals))
    return ok


# -------------------------------------------------------------------------------------- format

HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
CLANG_ERROR_RE = re.compile(r"^(.*?):(\d+):(\d+): (?:error|warning):")


def changed_ranges(diff_text):
    """Map each new path in a unified diff to the line ranges its hunks add."""
    ranges, path = {}, None
    for line in diff_text.splitlines():
        if line.startswith("+++ "):
            path = line[6:] if line.startswith("+++ b/") else None
        elif path:
            m = HUNK_RE.match(line)
            if m:
                start, count = int(m.group(1)), int(m.group(2) or "1")
                if count:
                    ranges.setdefault(path, []).append((start, start + count - 1))
    return ranges


def check_format(rev_range):
    binary = os.environ.get("CLANG_FORMAT") or shutil.which("clang-format")
    if not binary:
        report(None, None, "clang-format not found; install it with: pip install 'clang-format==23.1.3'")
        return False
    print(subprocess.run([binary, "--version"], stdout=subprocess.PIPE).stdout.decode().strip())
    ok, checked = True, 0
    for sha in commits_to_check(rev_range):
        if is_merge(sha) or not git("rev-parse", "--verify", "--quiet", sha + "^", check=False).strip():
            continue
        for path, ranges in changed_ranges(git("diff", "-U0", "-M", "--no-color", sha + "^", sha)).items():
            if not (path.endswith(FORMAT_EXT) and path.startswith(SOURCE_DIRS)) or is_generated(path):
                continue
            checked += 1
            content = subprocess.run(["git", "-C", ROOT, "show", "%s:%s" % (sha, path)], stdout=subprocess.PIPE).stdout
            args = ([binary, "--dry-run", "--Werror", "--style=file", "--assume-filename=" + os.path.join(ROOT, path)]
                    + ["--lines=%d:%d" % r for r in ranges])
            proc = subprocess.run(args, input=content, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if proc.returncode != 0:
                ok = False
                lines = sorted({int(m.group(2)) for m in map(CLANG_ERROR_RE.match, proc.stderr.decode("utf-8", "replace").splitlines()) if m})
                for line in lines or [ranges[0][0]]:
                    report(path, line, "line %d is not clang-formatted (commit %s)" % (line, sha[:9]))
    print("format: %d changed files checked" % checked)
    return ok


# ------------------------------------------------------------------------------------------ CI

def ci_range():
    """The commits a CI run is about: the pushed ones, or what this branch adds to the base branch."""
    head = git("rev-parse", "HEAD").strip()
    before = ""
    event = os.environ.get("GITHUB_EVENT_PATH")
    if os.environ.get("GITHUB_EVENT_NAME") == "push" and event and os.path.exists(event):
        with open(event, encoding="utf-8") as fh:
            before = json.load(fh).get("before") or ""
    if (before and set(before) != {"0"} and git("cat-file", "-t", before, check=False).strip() == "commit"
            and subprocess.run(["git", "-C", ROOT, "merge-base", "--is-ancestor", before, head]).returncode == 0):
        return before, "%s..HEAD" % before
    for ref in ("origin/" + BASE_BRANCH, "origin/HEAD"):
        base = git("merge-base", "HEAD", ref, check=False).strip()
        if base and base != head:
            return base, "%s..HEAD" % base
        if base == head:
            break
    parent = git("rev-parse", "HEAD^").strip()
    notice("no base branch to compare with; checking only the last commit")
    return parent, "%s..HEAD" % parent


def run_ci():
    base, rev_range = ci_range()
    print("range: %s" % rev_range)
    results = [check_commits(rev_range), check_tree(base=base), check_format(rev_range)]
    return all(results)


# -------------------------------------------------------------------------------------- report

def escape(msg):
    return msg.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def report(path, line, msg):
    if os.environ.get("GITHUB_ACTIONS") == "true":
        loc = " file=%s,line=%d" % (path, line or 1) if path else ""
        print("::error%s::%s" % (loc, escape(msg)))
    elif path:
        print("%s:%s: %s" % (path, line, msg))
    else:
        print(msg)


def notice(msg):
    print("::notice::%s" % escape(msg) if os.environ.get("GITHUB_ACTIONS") == "true" else msg)


# ----------------------------------------------------------------------------------- self-test

def self_test():
    failures = []

    def expect(name, cond):
        if not cond:
            failures.append(name)

    subs = {"3rdparty/glslang"}
    expect("scope: component", scope_of("MobileGL/MG_Backend/DirectGLES/Managers.cpp") == "MG_Backend/DirectGLES")
    expect("scope: file in subsystem", scope_of("MobileGL/MG_Backend/BackendObject.h") == "MG_Backend")
    expect("scope: file in MobileGL", scope_of("MobileGL/Config.h") == "MobileGL")
    expect("scope: root file", scope_of("CMakeLists.txt") == "CMakeLists.txt")
    expect("scope: top dir file", scope_of("scripts/gen_pipe.py") == "scripts")
    expect("scope: outside MobileGL", scope_of("docs/Disaggregated/notes/p5/x.md") == "docs/Disaggregated")
    expect("scope: submodule", scope_of("3rdparty/glslang", subs) == "3rdparty/glslang")

    paths = ["MobileGL/MG_Backend/DirectGLES/a.cpp", "MobileGL/MG_Test/Wire/b.cpp"]
    good = "[Fix] (MG_Backend/DirectGLES, MG_Test/Wire): flush once per frame\n"
    expect("message: valid", check_message(good, paths, [], subs) == [])
    expect("message: valid with details",
           check_message(good + "\n- the driver drops the second flush\n  on Adreno\n", paths, [], subs) == [])
    expect("message: ordinary P3 and P99", check_message(
        "[Feat] (MG_Backend/DirectGLES, MG_Test/Wire): add Display P3 and P99 frame times\n", paths, [], subs) == [])
    expect("message: package name", check_message(
        good + "\n- needs mesa-libEGL-devel for the symlink\n", paths, [], subs) == [])
    expect("message: CRLF file", check_message(good.replace("\n", "\r\n"), paths, [], subs) == [])
    cases = {
        "format": "Fix (MG_Backend/DirectGLES): flush once\n",
        "type": "[Feature] (MG_Backend/DirectGLES, MG_Test/Wire): flush once\n",
        "scope missing": "[Fix] (MG_Backend/DirectGLES): flush once\n",
        "scope extra": "[Fix] (MG_Backend/DirectGLES, MG_Test/Wire, MG_Util/Async): flush once\n",
        "length": "[Fix] (MG_Backend/DirectGLES, MG_Test/Wire): " + "x" * 73 + "\n",
        "planning id": "[Fix] (MG_Backend/DirectGLES, MG_Test/Wire): finish P13 W6c\n",
        "planning id in body": good + "\n- lands ID-81\n",
        "blank line": good + "- detail\n",
        "prose body": good + "\nThis paragraph explains at length.\n",
        "trailer": good + "\nCo-Authored-By: Someone <a@b.c>\n",
        "bulleted trailer": good + "\n- Co-Authored-By: Someone <a@b.c>\n",
        "long continuation": good + "\n- one\n  two\n  three\n  four\n",
        "external name": "[Fix] (MG_Backend/DirectGLES, MG_Test/Wire): match Zink's layout\n",
        "external name before a slash": "[Fix] (MG_Backend/DirectGLES, MG_Test/Wire): skip Mesa/llvmpipe\n",
        "not English": "[Fix] (MG_Backend/DirectGLES, MG_Test/Wire): \u4fee\u590d\u4e00\u6b21\u5237\u65b0\n",
        "BOM": "\ufeff" + good,
    }
    for name, msg in cases.items():
        expect("message rejects " + name, check_message(msg, paths, [], subs) != [])
    expect("message rejects new notes",
           check_message("[Docs] (docs/Disaggregated): add a review\n", ["docs/Disaggregated/notes/p9/r.md"],
                         ["docs/Disaggregated/notes/p9/r.md"], subs) != [])
    expect("message rejects new log",
           check_message("[Test] (tools/trace_replay): keep the run log\n", ["tools/trace_replay/run.log"],
                         ["tools/trace_replay/run.log"], subs) != [])
    expect("merge: valid", check_message("[Merge] (MG_Remote): take the reply-window fix\n", [], [], subs, True) == [])
    expect("merge: local path", check_message(
        "[Merge] (MG_Remote): take fix\n\n- from /mnt/c/Users/x/repo\n", [], [], subs, True) != [])

    src = ('// line one\n// line two\nint a = 1; // trailing\nconst char* s = "// not a comment";\n'
           'auto r = R"x(/* not a comment */)x";\n/* block\n   continues */\nint b;\n'
           "long n = 2'000'000'000; // after separators\n"
           'auto u = u8R"(// not a comment either)";\n'
           "#error don't stop here\n// after an apostrophe\n")
    found = [(ln, body.strip()) for ln, body, _ in comments(src)]
    expect("comments: line comments", (1, "line one") in found and (3, "trailing") in found)
    expect("comments: strings ignored", not any("not a comment" in b for _, b in found))
    expect("comments: block", (6, "block") in found and (7, "continues") in found)
    expect("comments: digit separators", (9, "after separators") in found)
    expect("comments: line count survives an apostrophe", (12, "after an apostrophe") in found)
    expect("comments: grouping", [(f, len(b)) for f, b in comment_blocks(comments(src))][:2] == [(1, 2), (6, 2)])
    hashed = [(ln, body.strip()) for ln, body, _ in hash_comments("#!/bin/sh\necho '#no' # yes\n# own\n")]
    expect("hash comments", hashed == [(2, "yes"), (3, "own")])

    stale = "ok.cpp" + ":40"  # spelled in two parts so this file itself carries no citation
    files = {
        "MobileGL/MG_A/B/ok.cpp": "// cache keyed by a lifetime id\nint x;\n",
        "MobileGL/MG_A/B/noheader.cpp": None,
        "MobileGL/MG_A/B/wrongpath.cpp": "int w;\n",
        "MobileGL/MG_A/B/name.cpp": "// works like Mesa's cache\n",
        "MobileGL/MG_A/B/cite.cpp": "// see %s\n" % stale,
        "MobileGL/MG_A/B/long.cpp": "#pragma once\n" + "// x\n" * 18 + "int z;\n// P13 W6c of the plan\n"
                                    "// THIS IS NOT THE RULE\n// RGB TO YUV\n// \u4e2d\u6587\n",
        "MobileGL/MG_A/B/gbk.cpp": "// section 7.3\n",
        "scripts/tool.py": "# works like Zink does\nx = '50_mesa.json'\n",
    }
    for path, body in files.items():
        stamped = "wrong/path.cpp" if "wrongpath" in path else path
        style = "#" if path.endswith(".py") else "//"
        text = "int y;\n" if body is None else HEADERS.HEADER.format(c=style, path=stamped) + "\n" + body
        files[path] = text.encode("utf-8").replace(b"section ", b"\xa1\xec") if "gbk" in path else text.encode("utf-8")
    strict, totals, _ = scan_tree(list(files), files.__getitem__)
    reasons = " | ".join("%s: %s" % (p, m) for p, _, m in strict)
    expect("tree: header", "noheader.cpp: license header: add" in reasons)
    expect("tree: header path", "wrongpath.cpp: license header: fix-path" in reasons)
    expect("tree: utf-8", "gbk.cpp: is not valid UTF-8" in reasons)
    expect("tree: external name", "name.cpp: names an external project" in reasons)
    expect("tree: external name in # comment", "tool.py: names an external project" in reasons)
    expect("tree: file names allowed", reasons.count("tool.py") == 1)
    expect("tree: stale citation", "cite.cpp: %s points past the end" % stale in reasons)
    expect("tree: clean file", "ok.cpp:" not in reasons.replace(stale, ""))
    expect("tree: long block", totals["long_comment_excess_lines"] == 3)
    expect("tree: planning code", totals["planning_code_comment_lines"] == 1)
    expect("tree: caps", totals["allcaps_comment_lines"] == 1)
    expect("tree: non-English", totals["non_english_comment_lines"] == 1)
    lf = files["MobileGL/MG_A/B/long.cpp"].replace(b"P13 W6c of the plan", b"what it is after P2:")
    crlf = lf.replace(b"\n", b"\r\n")
    expect("tree: line endings do not change totals",
           scan_tree(["MobileGL/MG_A/B/long.cpp"], lambda _: lf)[1]
           == scan_tree(["MobileGL/MG_A/B/long.cpp"], lambda _: crlf)[1])

    expect("format: hunks", changed_ranges("+++ b/a.cpp\n@@ -3,0 +4,2 @@\n@@ -9 +11 @@\n@@ -20,3 +22,0 @@\n")
           == {"a.cpp": [(4, 5), (11, 11)]})
    expect("range: first parent and cutoff", range_self_test())

    for name in failures:
        print("self-test FAILED: %s" % name)
    print("self-test: %d checks failed" % len(failures) if failures else "self-test: all checks pass")
    return not failures


def range_self_test():
    """In a scratch repository: commits before the script landed and commits a merge brings in are
    not checked; the merge itself and later first-parent commits are."""
    tmp = tempfile.mkdtemp()
    try:
        def g(*args):
            return git(*args, repo=tmp)
        g("init", "-q", "-b", "main")
        g("config", "user.email", "t@t")
        g("config", "user.name", "t")

        def commit(path, msg):
            full = os.path.join(tmp, path)
            os.makedirs(os.path.dirname(full) or tmp, exist_ok=True)
            with open(full, "a", encoding="utf-8") as fh:
                fh.write("x\n")
            g("add", path)
            g("commit", "-q", "-m", msg)
            return g("rev-parse", "HEAD").strip()

        old = commit("a.txt", "old style subject")
        lint = commit(SELF_PATH, "[CI] (scripts): add the lint")
        g("checkout", "-q", "-b", "side", old)
        side = commit("b.txt", "side branch, old style")
        g("checkout", "-q", "main")
        g("merge", "-q", "--no-ff", "-m", "[Merge] (side): take the side branch", side)
        merge = g("rev-parse", "HEAD").strip()
        after = commit("MobileGL/MG_A/B/c.cpp", "[Fix] (MG_A/B): change c")
        checked = commits_to_check("%s..HEAD" % old, tmp)
        return checked == [lint, merge, after] and side not in checked
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scope")
    p = sub.add_parser("message")
    p.add_argument("file")
    p = sub.add_parser("commits")
    p.add_argument("range")
    p = sub.add_parser("tree")
    p.add_argument("--update-baseline", action="store_true")
    p = sub.add_parser("format")
    p.add_argument("range")
    sub.add_parser("ci")
    sub.add_parser("self-test")
    args = ap.parse_args()

    if args.cmd in ("scope", "message"):
        staged = [p for p in git("diff", "--cached", "--name-only", "--no-renames").splitlines() if p]
        if args.cmd == "scope":
            print("(%s)" % ", ".join(scopes_for(staged, submodule_paths())))
            return 0
        added = [p for p in git("diff", "--cached", "--name-only", "--no-renames", "--diff-filter=A").splitlines() if p]
        with open(args.file, encoding="utf-8") as fh:
            problems = check_message(fh.read(), staged, added, submodule_paths())
        for prob in problems:
            print(prob)
        return 1 if problems else 0
    ok = {"commits": lambda: check_commits(args.range),
          "tree": lambda: check_tree(args.update_baseline),
          "format": lambda: check_format(args.range),
          "ci": run_ci,
          "self-test": self_test}[args.cmd]()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
