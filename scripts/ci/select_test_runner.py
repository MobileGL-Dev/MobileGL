#!/usr/bin/env python3
"""Choose one execution platform for the entire Test artifact dependency graph."""
import json
import os
import sys
import urllib.error
import urllib.request

NAME = "minipc-8845-arch-wsl-gpu"
LABELS = ["self-hosted", "Linux", "X64", "arch", "wsl", "gpu", "gles", "vulkan"]


def choose(runners):
    for runner in runners:
        labels = {label["name"].lower() for label in runner.get("labels", [])}
        if runner.get("name") == NAME and set(map(str.lower, LABELS)) <= labels:
            if runner.get("status") == "online":
                # Busy is still available: queue behind its current job.
                return LABELS, "arch-wsl", "preferred runner online (busy=%s)" % runner.get("busy", False)
    return ["ubuntu-latest"], "github", "preferred runner offline, absent, or missing required labels"


def runner_inventory(repository, token):
    if not token:
        raise ValueError("Set RUNNER_STATUS_TOKEN with repository Administration: read; runner availability is unknown")
    runners = []
    page = 1
    while True:
        url = f"https://api.github.com/repos/{repository}/actions/runners?per_page=100&page={page}"
        request = urllib.request.Request(url, headers={
            "Authorization": "Bearer " + token,
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "MobileGL-test-runner-selector",
        })
        with urllib.request.urlopen(request, timeout=20) as response:
            data = json.load(response)
        batch = data["runners"]
        runners.extend(batch)
        if len(batch) < 100:
            return runners
        page += 1


def main():
    try:
        runners = runner_inventory(os.environ["GITHUB_REPOSITORY"], os.environ.get("RUNNER_STATUS_TOKEN", ""))
        labels, pool, reason = choose(runners)
    except (ValueError, KeyError, OSError, urllib.error.URLError) as error:
        # An API/auth error is not evidence that the machine is offline. Do not
        # silently send the whole matrix to GitHub when the status cannot be read.
        print(f"::error::Cannot determine runner availability: {error}", file=sys.stderr)
        return 1
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write("runner=" + json.dumps(labels) + "\npool=" + pool + "\n")
    print(f"Test runner: {pool}: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
