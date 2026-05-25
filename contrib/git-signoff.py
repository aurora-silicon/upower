#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Verify that all commits added on the current branch (compared to
# origin/master) contain a properly formatted Signed-off-by tag.

from __future__ import annotations

import re
import subprocess
import sys

SIGNOFF_RE = re.compile(r"^Signed-off-by: .+ <.+@.+\..+>$")


def get_added_commits() -> list[tuple[str, str]]:
    result = subprocess.run(
        ["git", "log", "--format=%H %s", "origin/master..HEAD"],
        capture_output=True,
        text=True,
        check=True,
    )
    commits = []
    for line in result.stdout.strip().splitlines():
        sha, subject = line.split(" ", 1)
        commits.append((sha, subject))
    return commits


def get_commit_message(sha: str) -> str:
    result = subprocess.run(
        ["git", "log", "-1", "--format=%B", sha],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def main() -> int:
    commits = get_added_commits()
    if not commits:
        print("No commits found between origin/master and HEAD.")
        return 0

    retval = 0
    for sha, subject in commits:
        message = get_commit_message(sha)
        has_signoff = any(SIGNOFF_RE.match(line) for line in message.splitlines())
        if not has_signoff:
            print(
                f"ERROR: Signed-off-by tag cannot be found in commit "
                f'{sha[:12]} ("{subject}")'
            )
            retval = 1

    return retval


if __name__ == "__main__":
    raise SystemExit(main())
