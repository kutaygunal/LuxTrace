#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kutay Gunal
#
# This file is part of LuxTrace, distributed under the GNU Affero General
# Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
# A commercial licence is available; see LICENSING.md.

"""Assert the counts the README quotes against the code they describe.

Two numbers in the README are facts about the source, not prose: how many scenes
the library holds, and how many closed forms `--validate` checks. Both had drifted
-- the README carried "27-scene library" three paragraphs from "29 scenes", and
described `--validate` as checking seven results when it checks fifteen -- because
nothing failed when they did.

This does. It reads the two authorities:

  * `GeometryProvider::Scene` in src/core/GeometryProvider.h -- the enumerators
    before `Count` are the scenes.
  * `studies::validate()` in src/core/Studies.cpp -- the cases it pushes, including
    the ones pushed by the helpers it hands `cases` to.

and checks every sentence in the README that quotes either. Exits non-zero, naming
the sentence and both numbers, if one disagrees. Run from anywhere:

    python tools/check_readme_counts.py

Deliberately static: it parses the header and the function rather than running the
binary, so it is a few milliseconds in CI and needs no build, no Qt and no OCCT.
The cost is that it is coupled to the shape of that enum and that function -- which
is why it fails loudly when it cannot find either, rather than quietly checking
nothing.
"""

import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

WORDS = {
    "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7,
    "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12, "thirteen": 13,
    "fourteen": 14, "fifteen": 15, "sixteen": 16, "seventeen": 17, "eighteen": 18,
    "nineteen": 19, "twenty": 20, "thirty": 30,
}
NUMBER = r"(\d+|" + "|".join(WORDS) + r")"


def read(*parts):
    with open(os.path.join(REPO_ROOT, *parts), encoding="utf-8") as f:
        return f.read()


def as_int(token):
    """A count written either as digits or as an English word."""
    return int(token) if token.isdigit() else WORDS[token.lower()]


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


# --------------------------------------------------------------- authorities --

def count_scenes():
    """The enumerators in `GeometryProvider::Scene` before its `Count` sentinel."""
    header = read("src", "core", "GeometryProvider.h")
    m = re.search(r"enum\s+class\s+Scene\s*\{(.*?)\}\s*;", header, re.S)
    if not m:
        raise SystemExit("check_readme_counts: no `enum class Scene` in "
                         "src/core/GeometryProvider.h -- the parser needs updating")
    body = strip_comments(m.group(1))
    names = [n.strip() for n in body.split(",")]
    names = [n.split("=")[0].strip() for n in names if n.strip()]
    if "Count" not in names:
        raise SystemExit("check_readme_counts: `enum class Scene` has no `Count` "
                         "sentinel -- the parser needs updating")
    return names.index("Count")


def _pushes(body):
    return len(re.findall(r"cases\.push_back\s*\(", body))


def _function_body(source, signature_pattern):
    """The braced body of the first function matching `signature_pattern`."""
    m = re.search(signature_pattern, source)
    if not m:
        return None
    start = source.index("{", m.end() - 1)
    depth, i = 0, start
    while i < len(source):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:i]
        i += 1
    return None


def count_validation_cases():
    """The cases `studies::validate()` returns.

    Most are pushed in `validate()` itself; the point-source helper is handed the
    vector and pushes its own, so a helper called with `cases` contributes what it
    pushes rather than one.
    """
    source = strip_comments(read("src", "core", "Studies.cpp"))
    body = _function_body(source, r"std::vector<ValidationCase>\s+validate\s*\(")
    if body is None:
        raise SystemExit("check_readme_counts: no `validate(...)` in "
                         "src/core/Studies.cpp -- the parser needs updating")

    total = _pushes(body)
    for helper in sorted(set(re.findall(r"\b(\w+)\s*\(\s*cases\s*,", body))):
        helper_body = _function_body(source, r"\bvoid\s+" + helper + r"\s*\(")
        if helper_body is None:
            raise SystemExit(f"check_readme_counts: `{helper}` is handed `cases` "
                             "but its definition was not found in Studies.cpp")
        total += _pushes(helper_body)

    if total == 0:
        raise SystemExit("check_readme_counts: `validate()` appears to push no "
                         "cases -- the parser needs updating")
    return total


# ------------------------------------------------------------------- claims --

# Each entry is (authority, regex, what the groups mean). The regexes run against
# the README with all runs of whitespace collapsed to one space, so a claim that
# happens to wrap across two lines still matches.
CLAIMS = [
    ("scenes", NUMBER + r" parametric OCCT scenes", "same"),
    ("scenes", NUMBER + r" scenes, all built from OCCT B-Rep", "same"),
    ("scenes", r"whole " + NUMBER + r"-scene library", "same"),
    ("scenes", r"the " + NUMBER + r" scenes it takes " + NUMBER, "one-fewer"),
    ("scenes", r"scene registry: " + NUMBER + r" parametric OCCT scenes", "same"),
    ("validation", r"against " + NUMBER + r" closed forms", "same"),
    ("validation", r"the same " + NUMBER + r" closed forms", "same"),
    ("validation", r"against " + NUMBER + r" results derived entirely outside it",
     "same"),
    ("validation", NUMBER + r"/" + NUMBER + r" closed-form checks", "same-twice"),
]


def main():
    scenes = count_scenes()
    validation = count_validation_cases()
    expected = {"scenes": scenes, "validation": validation}

    readme = re.sub(r"\s+", " ", read("README.md"))

    failures = []
    unmatched = []
    for authority, pattern, shape in CLAIMS:
        want = expected[authority]
        matches = list(re.finditer(pattern, readme, re.I))
        if not matches:
            unmatched.append(pattern)
            continue
        for m in matches:
            quoted = [as_int(g) for g in m.groups()]
            if shape == "same":
                ok = quoted[0] == want
                detail = f"{want}"
            elif shape == "one-fewer":
                ok = quoted[0] == want and quoted[1] == want - 1
                detail = f"{want} then {want - 1}"
            else:  # same-twice
                ok = quoted[0] == want and quoted[1] == want
                detail = f"{want} then {want}"
            if not ok:
                failures.append((m.group(0), detail, authority))

    print(f"GeometryProvider::Scene   {scenes} scenes")
    print(f"studies::validate()       {validation} closed-form cases")

    # A claim that stopped matching is as much a failure as one that disagrees:
    # it means the sentence was reworded and this check quietly stopped covering
    # it, which is the exact failure the whole script exists to prevent.
    for pattern in unmatched:
        print(f"\nERROR: no README sentence matches /{pattern}/", file=sys.stderr)
        print("       If the wording changed, update CLAIMS in this script.",
              file=sys.stderr)

    for quoted, detail, authority in failures:
        print(f"\nERROR: README says \"{quoted}\"", file=sys.stderr)
        print(f"       the {authority} authority says {detail}", file=sys.stderr)

    if failures or unmatched:
        print(f"\n{len(failures)} stale count(s), {len(unmatched)} unmatched "
              f"claim(s).", file=sys.stderr)
        return 1

    print(f"\nEvery README count agrees with the code ({len(CLAIMS)} claims).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
