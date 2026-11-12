# Security policy

## Reporting a vulnerability

Please report security problems **privately**, not in a public issue or pull request.

Use GitHub's private vulnerability reporting: open the **Security** tab of this repository,
choose **Report a vulnerability**, and describe what you found. Helpful details: the version or
commit, your OS and host, how to reproduce it, and what you think the impact is. A file or MIDI
sequence that triggers the problem is the best possible report.

This is a small project maintained by one person with AI agents. Reports are read and answered on
a best-effort basis: expect an acknowledgement within a few days, not hours, and no guaranteed fix
time. If a report is valid we will work on a fix, tell you what we plan, and credit you in the
release notes if you want that.

## What is in scope

- The plugin loading **untrusted input**: saved plugin state, session data from a host, and
  (once they exist) preset and bank files. Memory-safety bugs, crashes, hangs or unbounded
  memory use caused by malformed state are in scope.
- **MIDI input** that crashes, hangs or corrupts memory in the plugin.
- The **build and release pipeline**: GitHub Actions workflows, how dependencies and tools are
  fetched, and anything that could let a pull request or a dependency tamper with a build.

## What is out of scope

- Bugs in third-party code (JUCE, Catch2, the Steinberg VST3 SDK, pluginval, host applications).
  Please report those upstream; tell us too if they affect this project.
- Audio glitches, wrong sound or other non-security bugs: use a normal issue.
- Problems that need an attacker who already controls your machine or your DAW session.

## Supported versions

The project is pre-release. Only the latest commit on `main` is supported. There are no released
versions with security backports yet.
