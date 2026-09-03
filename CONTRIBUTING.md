# Contributing to LuxTrace

Bug reports, scenes, and patches are welcome. One thing has to happen before any
code is merged, and it is unusual enough to state first.

## The CLA comes first

LuxTrace is dual-licensed — [AGPL-3.0-only](LICENSE) for everyone, plus a paid
[commercial licence](COMMERCIAL-LICENSE.md) for closed-source and network use.
Only the copyright holder can offer that second licence, so **every contributor
must sign the [Contributor Licence Agreement](CLA.md) before their code is
merged.** You keep the copyright in what you write; you grant the right to
include it under both licences.

Without this, one merged patch would permanently end the commercial arm. There
is no way to make an exception for a small change.

**To sign:** state in your first pull request

> I have read CLA.md and I agree to it.

along with your full name and the email on your commits. `[SET UP AN AUTOMATED
CLA CHECK — E.G. THE CLA ASSISTANT GITHUB APP — BEFORE THE REPOSITORY GOES
PUBLIC, AND REPLACE THIS PARAGRAPH WITH ITS INSTRUCTIONS.]`

If you cannot sign — your employer forbids it, or you would rather not — open an
issue describing the fix instead of a pull request. A described bug is genuinely
useful and carries no licensing question.

## Before you open a pull request

- **Build and test.** See the build and test sections of the [README](README.md).
  `ctest` must pass on your machine, and the CI must pass on the PR.
- **Match the surrounding code.** Naming, comment density, and idiom in the file
  you are editing are the style guide.
- **Keep the licence header.** Every source file carries a six-line
  `AGPL-3.0-only` header. New files need it too; copy one from a neighbouring
  file.
- **Say what you changed and why.** The physics matters here: if a change alters
  what a simulation produces, say what the previous behaviour was, what it is
  now, and how you know the new one is right.
- **Third-party code.** If any part of your contribution is not your own work,
  identify it and its licence in the PR. This includes code generated with
  substantial AI assistance. Do not add a new runtime dependency without opening
  an issue first — the dependency footprint is deliberately two libraries, and
  both are LGPL for reasons set out in
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## What is likely to be accepted

Fixes to physics or numerics with a test that fails before and passes after; new
scenes in the geometry registry; performance work with a measurement; platform
and build fixes; documentation that corrects something wrong.

Large architectural changes and new dependencies should start as an issue, not a
pull request — it is a poor trade for you to write a thousand lines that turn
out to cut against a direction you could not have known about.

## Reporting bugs

Include the LuxTrace version, your OS, your Qt and OCCT versions, the scene or a
minimal reproduction, and what you expected versus what you got. For a numerical
disagreement, the expected value and its source are the useful part.
