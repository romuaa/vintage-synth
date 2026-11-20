**First release, no installers.** These builds are not signed with a developer certificate and not notarised (the macOS bundle carries only a local ad-hoc signature). Unpack the zip for your platform and follow its `INSTALL.txt`; it explains the one-time step that this needs on macOS and Windows.

**Experimental:** the Windows ARM64 and Linux ARM64 parts of the Windows and Linux bundles are not performance-tuned. Windows ARM64 is built, unit-tested and load-tested, but not run through pluginval or the VST3 validator. Linux ARM64 is built, unit-tested and run through the VST3 validator, but not through pluginval. macOS (universal), Windows x64 and Linux x86_64 are the fully checked builds.

The complete source for each zip is named in its `SOURCE.txt`.

