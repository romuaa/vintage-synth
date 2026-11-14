---
name: release
description: Version bump, code signing (Apple Developer ID + notarization, Windows Authenticode), installers (.pkg, Inno Setup/WiX), and release checklist. Use for any packaging or distribution task.
---

# Release

Signing secrets live in CI secrets or the user's keychain. Never ask for them in chat and never
write them to files. Ask the user to configure them, under "What I need from you".
Publishing a release is outward-facing: **confirm with the user before tagging or uploading.**

## Steps

1. **Version:** bump `project(VERSION x.y.z)` in `CMakeLists.txt` and update `CHANGELOG.md`.
2. **Full validation:** run the `validate-plugin` skill on all targets. CI must be green.
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
