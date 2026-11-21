#!/usr/bin/env python3
# MobileGL - scripts/ci/release.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Validate and publish an annotated version tag after both required workflows succeed."""

import argparse
from collections import Counter
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode
from urllib.request import Request, urlopen


WORKFLOWS = {"test.yml": "Test", "apk.yml": "MobileGL APK"}
WAIT_SECONDS = 330 * 60
POLL_SECONDS = 30
VERSION = re.compile(r"v[0-9]+\.[0-9]+(?:-(?:alpha|rc)\.[1-9][0-9]*)?")
SHA = re.compile(r"[0-9a-f]{40}")
LOGIN = re.compile(r"[a-zA-Z0-9][a-zA-Z0-9-]*(?:\[bot\])?")


class ReleaseError(Exception):
    pass


class GitHub:
    def __init__(self):
        self.repo = os.environ["GITHUB_REPOSITORY"]
        self.url = f"{os.environ['GITHUB_SERVER_URL']}/{self.repo}"
        self.api = f"{os.environ['GITHUB_API_URL']}/repos/{self.repo}"
        self.headers = {
            "Authorization": f"Bearer {os.environ['GH_TOKEN']}",
            "Accept": "application/vnd.github+json",
            "Content-Type": "application/json",
            "X-GitHub-Api-Version": "2026-03-10",
        }

    def request(self, path, method="GET", body=None, *, deadline=None, allow_missing=False):
        data = json.dumps(body).encode() if body is not None else None
        request = Request(self.api + path, data=data, headers=self.headers, method=method)
        for attempt in range(3):
            remaining = deadline - time.monotonic() if deadline is not None else 20
            if remaining <= 0:
                raise ReleaseError("The 330-minute validation deadline elapsed.")
            try:
                with urlopen(request, timeout=min(20, remaining)) as response:
                    raw = response.read()
                    return json.loads(raw) if raw else None
            except HTTPError as exc:
                if exc.code == 404 and allow_missing:
                    return None
                if method != "GET" or exc.code not in {429, 500, 502, 503, 504} or attempt == 2:
                    raise ReleaseError(f"{method} {path} returned HTTP {exc.code}: "
                                       f"{exc.read().decode(errors='replace')}") from exc
            except (URLError, TimeoutError):
                if method != "GET" or attempt == 2:
                    raise
            time.sleep(min(2 ** attempt, remaining))

    def pages(self, path, params=(), key=None, deadline=None):
        for page in range(1, 1001):
            query = urlencode([*params, ("per_page", 100), ("page", page)])
            response = self.request(f"{path}?{query}", deadline=deadline)
            entries = response[key] if key else response
            yield from entries
            if len(entries) < 100:
                return
        raise ReleaseError(f"Too many result pages for {path}.")

    def tag_object(self, tag):
        ref = self.request(f"/git/ref/tags/{quote(tag, safe='')}", allow_missing=True)
        return ref["object"] if ref else None

    def release_for_tag(self, tag):
        return self.request(f"/releases/tags/{quote(tag, safe='')}", allow_missing=True)

    def dispatch(self, workflow, ref, deadline):
        response = self.request(f"/actions/workflows/{workflow}/dispatches", "POST",
                                {"ref": ref}, deadline=deadline)
        if not isinstance(response, dict) or not response.get("workflow_run_id"):
            raise ReleaseError(f"The dispatch for {workflow} did not return a run ID.")
        return response["workflow_run_id"]

    def create_release(self, tag, commit, body, draft=False):
        return self.request("/releases", "POST", {
            "tag_name": tag,
            "target_commitish": commit,
            "name": tag,
            "body": body,
            "draft": draft,
            "prerelease": "-" in tag,
            "generate_release_notes": False,
        })


def git(*args):
    return subprocess.check_output(["git", *args]).decode("utf-8")


def ancestor_of(commit, ref):
    return subprocess.run(["git", "merge-base", "--is-ancestor", commit, ref],
                          check=False).returncode == 0


def event_candidate(github):
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
    tag = os.environ["GITHUB_REF_NAME"]
    if not VERSION.fullmatch(tag):
        raise ReleaseError(f"Unsupported version tag {tag!r}.")
    if event.get("deleted") or event.get("forced") or not event.get("created"):
        raise ReleaseError("Release requires a newly created version tag.")
    ref = f"refs/tags/{tag}"
    tag_sha = git("rev-parse", "--verify", ref).strip()
    if git("cat-file", "-t", tag_sha).strip() != "tag":
        raise ReleaseError("Release requires an annotated tag.")
    commit = git("rev-parse", "--verify", f"{ref}^{{commit}}").strip()
    if git("rev-parse", "--verify", "HEAD").strip() != commit:
        raise ReleaseError("The checkout differs from the tagged commit.")
    if event.get("after") not in {tag_sha, commit} or event.get("ref") != ref:
        raise ReleaseError("The tag differs from the original push event.")
    if not ancestor_of(commit, "refs/remotes/origin/main"):
        raise ReleaseError("The tagged commit must be reachable from upstream main.")
    remote_tag = github.tag_object(tag)
    if not remote_tag or remote_tag["type"] != "tag" or remote_tag["sha"] != tag_sha:
        raise ReleaseError("The remote annotated tag differs from the checkout.")
    # Read the tag object's message so paragraph breaks and signed tags are handled by Git.
    notes = git("for-each-ref", "--format=%(contents)", ref).rstrip("\n")
    signature = git("for-each-ref", "--format=%(contents:signature)", ref).strip()
    if signature and notes.endswith(signature):
        notes = notes.removesuffix(signature).rstrip("\n")
    if not notes.strip():
        raise ReleaseError("The annotated tag must contain release notes.")
    sender = event.get("sender", {}).get("login", "")
    if not LOGIN.fullmatch(sender):
        raise ReleaseError("The tag push event does not identify a GitHub account.")
    return tag, tag_sha, commit, notes, sender


def release_already_exists(github, tag):
    release = github.release_for_tag(tag)
    if release and release["draft"]:
        raise ReleaseError(f"A draft Release already exists for {tag}.")
    if release:
        print(f"Release already published: {release['html_url']}")
    return bool(release)


def eligible_run(run, repo, commit, tag):
    if run["head_sha"] != commit or (run.get("head_repository") or {}).get("full_name") != repo:
        return False
    if run["event"] == "push":
        return run["head_branch"] == "dev"
    return run["event"] == "workflow_dispatch" and run["display_title"] in {
        f"Release validation (refs/tags/{tag})",
        "Release validation (refs/heads/main)",
    }


def latest_run(github, workflow, commit, tag, deadline):
    runs = github.pages(f"/actions/workflows/{workflow}/runs",
                        [("head_sha", commit)], key="workflow_runs", deadline=deadline)
    return next((run for run in runs if eligible_run(run, github.repo, commit, tag)), None)


def summarize(github, runs):
    with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as summary:
        summary.write("### Release validation\n\n")
        for workflow, run in runs.items():
            name = WORKFLOWS[workflow]
            status = run.get("conclusion") or run.get("status") or "dispatched"
            url = f"{github.url}/actions/runs/{run['id']}"
            summary.write(f"- [{name}]({url}) — {status}\n")


def wait_for_runs(github, runs, commit, deadline):
    while True:
        all_success = True
        for workflow, run in runs.items():
            if run.get("conclusion") == "success":
                continue
            run = github.request(f"/actions/runs/{run['id']}", deadline=deadline)
            runs[workflow] = run
            if run["head_sha"] != commit or (run.get("head_repository") or {}).get("full_name") != github.repo:
                raise ReleaseError(f"{WORKFLOWS[workflow]} ran against an unexpected repository or commit.")
            if run["status"] == "completed" and run["conclusion"] != "success":
                raise ReleaseError(f"{WORKFLOWS[workflow]} concluded {run['conclusion']}.")
            all_success &= run["conclusion"] == "success"
        if all_success:
            return
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ReleaseError("The 330-minute validation deadline elapsed.")
        time.sleep(min(POLL_SECONDS, remaining))


def validate_workflows(github, commit, tag=None):
    deadline = time.monotonic() + WAIT_SECONDS
    runs = {}
    try:
        if tag:
            for workflow in WORKFLOWS:
                run = latest_run(github, workflow, commit, tag, deadline)
                if run:
                    runs[workflow] = run
            for workflow, run in runs.items():
                if run["status"] == "completed" and run["conclusion"] != "success":
                    raise ReleaseError(f"{WORKFLOWS[workflow]} concluded {run['conclusion']}.")
        for workflow, name in WORKFLOWS.items():
            if workflow not in runs:
                runs[workflow] = {"id": github.dispatch(workflow, tag or "main", deadline)}
            print(f"{name}: {github.url}/actions/runs/{runs[workflow]['id']}", flush=True)
        wait_for_runs(github, runs, commit, deadline)
    finally:
        summarize(github, runs)
    return runs


def previous_release(github, tag, commit):
    for release in github.pages("/releases"):
        previous = release["tag_name"]
        if release["draft"] or previous == tag or not VERSION.fullmatch(previous):
            continue
        ref = f"refs/tags/{previous}"
        if ancestor_of(f"{ref}^{{commit}}", commit):
            return previous
    return None


def contributors(github, revision):
    representatives = {}
    commit_counts = Counter()
    data = git("log", "-z", "--format=%H%x00%aN%x00%aE", revision)
    fields = data.split("\0")
    for commit, name, email in zip(fields[::3], fields[1::3], fields[2::3]):
        identity = (name, email.casefold())
        representatives.setdefault(identity, commit)
        commit_counts[identity] += 1

    linked = {}
    for identity, commit in representatives.items():
        author = github.request(f"/commits/{commit}").get("author") or {}
        login = author.get("login", "")
        if LOGIN.fullmatch(login):
            linked[identity] = login

    # An unlinked author name can match a login confirmed by another commit.
    known_logins = {login.casefold(): login for login in linked.values()}
    labels = {}
    totals = Counter()
    for identity, count in commit_counts.items():
        name = " ".join(identity[0].split())
        login = linked.get(identity) or known_logins.get(name.removeprefix("@").casefold())
        escaped_name = "".join(f"\\{char}" if char in "\\@<>&*_[`]" else char
                               for char in name)
        credit = f"@{login}" if login else escaped_name
        if credit:
            key = credit.casefold()
            labels.setdefault(key, credit)
            totals[key] += count

    rows = []
    for key in sorted(totals, key=lambda key: (-totals[key], key)):
        count = totals[key]
        unit = "commit" if count == 1 else "commits"
        rows.append(f"- {labels[key]} — {count:,} {unit}")
    return "\n".join(rows)


def download_plugin_apk(github, tag, commit, run_id, directory):
    if not re.fullmatch(r"[0-9]+", run_id):
        raise ReleaseError("The successful APK run ID is missing or invalid.")
    run_path = f"/actions/runs/{run_id}"
    run = github.request(run_path)
    if (not eligible_run(run, github.repo, commit, tag)
            or run["path"].split("@", 1)[0] != ".github/workflows/apk.yml"
            or run["status"] != "completed" or run["conclusion"] != "success"):
        raise ReleaseError("The APK run must be a successful validation of the tagged commit.")
    name = f"MobileGL-plugin-release-{commit}.apk"
    artifacts = [
        entry for entry in github.pages(f"{run_path}/artifacts", [("name", name)], key="artifacts")
        if entry["name"] == name and not entry["expired"]
    ]
    if len(artifacts) != 1:
        raise ReleaseError(f"Expected one unexpired {name} in APK run {run_id}.")
    artifact = artifacts[0]
    destination = directory / f"MobileGL-plugin-{tag}.apk"
    with destination.open("wb") as output:
        subprocess.run(["gh", "api", "--allow-escape-sequences",
                        f"repos/{github.repo}/actions/artifacts/{artifact['id']}/zip"],
                       stdout=output, check=True, timeout=120)
    with destination.open("rb") as apk:
        digest = "sha256:" + hashlib.file_digest(apk, "sha256").hexdigest()
    if digest != artifact.get("digest"):
        raise ReleaseError("The plugin APK does not match the digest from the APK run.")
    print(f"Verified {destination.name} from APK run {run_id} ({digest}).", flush=True)
    return destination


def publish(github, tag, tag_sha, commit, notes, sender, apk_run_id):
    if release_already_exists(github, tag):
        return
    previous = previous_release(github, tag, commit)
    revision = f"refs/tags/{previous}..{commit}" if previous else commit
    credits = contributors(github, revision)
    history = (f"[Compare {previous} to {tag}]({github.url}/compare/{previous}...{tag})"
               if previous else f"[Commit history for {tag}]({github.url}/commits/{tag})")
    parts = [notes, f"Released by @{sender}"]
    if credits:
        parts.append(f"**Contributors**\n\n{credits}")
    parts.extend([history, "Clone with submodules:\n\n"
                  f"```sh\ngit clone --branch {tag} --recurse-submodules {github.url}.git\n```"])
    with tempfile.TemporaryDirectory() as directory:
        apk = download_plugin_apk(github, tag, commit, apk_run_id, Path(directory))
        remote_tag = github.tag_object(tag)
        if not remote_tag or remote_tag["sha"] != tag_sha:
            raise ReleaseError("The remote tag changed while preparing the release.")
        command = ["gh", "release", "create", tag, str(apk), "--repo", github.repo,
                   "--verify-tag", "--title", tag, "--notes-file", "-"]
        if "-" in tag:
            command.append("--prerelease")
        # The release becomes public after its asset uploads successfully.
        subprocess.run(command, input="\n\n".join(parts), text=True, check=True)


def preflight(github):
    commit = os.environ["GITHUB_SHA"]
    if os.environ["GITHUB_REF"] != "refs/heads/main" or not SHA.fullmatch(commit):
        raise ReleaseError("Release preflight must run on main.")
    tag = f"release-preflight-{os.environ['GITHUB_RUN_ID']}-{os.environ['GITHUB_RUN_ATTEMPT']}"
    if github.tag_object(tag):
        raise ReleaseError("The preflight name is already a Git tag.")
    draft = github.create_release(tag, commit, "Verifies release permissions.", draft=True)
    if not draft["draft"]:
        raise ReleaseError("The preflight Release is not a draft.")
    github.request(f"/releases/{draft['id']}", "DELETE")
    if github.tag_object(tag):
        raise ReleaseError("Preflight unexpectedly created a Git tag.")
    print("Draft Release creation and cleanup succeeded.", flush=True)
    validate_workflows(github, commit)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("validate", "publish", "preflight"))
    command = parser.parse_args().command
    github = GitHub()
    if command == "preflight":
        preflight(github)
        return
    tag, tag_sha, commit, notes, sender = event_candidate(github)
    if command == "validate":
        if not release_already_exists(github, tag):
            runs = validate_workflows(github, commit, tag)
            with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
                print(f"apk_run_id={runs['apk.yml']['id']}", file=output)
    else:
        publish(github, tag, tag_sha, commit, notes, sender, os.environ.get("APK_RUN_ID", ""))


if __name__ == "__main__":
    try:
        main()
    except (ReleaseError, OSError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
