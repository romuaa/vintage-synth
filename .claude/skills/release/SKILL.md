---
name: release
description: Version bump, the release run that packs the per-platform zips and creates a draft release, publishing, and (later) code signing and installers. Use for any packaging or distribution task.
---

# Release

Signing secrets live in CI secrets or the user's keychain. Never ask for them in chat and never
write them to files. Ask the user to configure them, under "What I need from you".
Publishing a release is outward-facing: **confirm with the user before tagging or uploading.**

## How a release is made today (no signing, no installers; owner decision 2026-11-19)

Contract: `docs/architecture.md` section 10. A release is one zip per platform (plain `.vst3`
bundle, `LICENSE.txt`, `THIRD-PARTY.txt`, `SOURCE.txt`, `INSTALL.txt`), Vintage only.

1. **Version and notes:** `project(VERSION x.y.z)` in `CMakeLists.txt`, the version string in
   `scripts/dev-host.sh`, and a section `## [x.y.z]` in `CHANGELOG.md` (its text becomes the
   release notes). Merge through a pull request; the local gate must pass.
2. **Release run** (the orchestrator, with the owner's go-ahead):
   `gh workflow run build.yml --ref main -f targets=all -f release=true`. It builds and tests
   all five targets, packs the zips (`scripts/package-release.sh`), unpacks and loads each on a
   clean runner, and creates a **draft** release `vx.y.z` with `SHA256SUMS.txt`. It refuses to
   run if the tag or a published release exists, or if the repository is not public.
3. **Check the draft:** download the zips, `shasum -a 256 -c SHA256SUMS.txt`,
   `scripts/package-release.sh verify --license LICENSE <zip>`.
4. **Owner:** installs from a zip following only its `INSTALL.txt`, then publishes the draft
   (publishing creates the tag). A defect that needs a code change: delete the draft, fix, run
   step 2 again.
5. **After publishing:** update `README.md` (status box, Platforms, Roadmap), `SECURITY.md`
   (supported versions), the changelog header and `docs/status.md`.

## Later: signing and installers

Steps 3 and 4 below apply only once the owner has signing identities.

1. **Version:** bump `project(VERSION x.y.z)` in `CMakeLists.txt` and update `CHANGELOG.md`.
2. **Full validation:** run the `validate-plugin` skill on all targets; a dispatched CI run with
   `targets=all` must be green.
3. **macOS:**
   ```bash
   codesign --force --options runtime --timestamp \
     --sign "Developer ID Application: <TEAM>" "<bundle>.vst3"
   productbuild ... --sign "Developer ID Installer: <TEAM>" VintageSynth.pkg
   xcrun notarytool submit VintageSynth.pkg --keychain-profile "<profile>" --wait
   xcrun stapler staple VintageSynth.pkg
   spctl -a -vvv -t install VintageSynth.pkg
   ```
   Install path: `/Library/Audio/Plug-Ins/VST3/`.
4. **Windows:** sign each `.vst3` DLL (x64 and ARM64) and the installer with Authenticode
   (Azure Trusted Signing or an EV certificate), using a timestamp server. Install to
   `C:\Program Files\Common Files\VST3\`.
5. **Smoke test:** do a clean install on a fresh machine or VM for each platform. Load the plugin in
   at least one host per architecture, and reload a session saved with the previous release.
6. **AGPLv3 compliance:** the release notes link to the exact source tag. Include `LICENSE` in
   both installers.
   **Font licence (SIL OFL 1.1):** the editor embeds the Inter typeface; ship its licence text
   `resources/fonts/inter/OFL.txt` with every binary (in the plugin bundle's resources and/or the
   installer, next to `LICENSE`). The OFL requires the licence to accompany the font.
7. **Tag and publish:** only after the user confirms.
