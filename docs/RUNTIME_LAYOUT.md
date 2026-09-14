# Rebuild runtime identity and PNG skin directory

## Runtime layout

All normal runtime resolution remains relative to the current EXE, not the
working directory or an original player's installation elsewhere.

```text
TTPlayerRebuild.exe
TTPlayerRebuild.xml
ttpcomm.dll / ttpres.dll / AddIn/ ...
Skin/
  Default.xml                 # embedded skin's existing profile
  Example.skn
  Example.skn.xml
  new/
    Example.skn               # PNG skin; distinct from Skin/Example.skn
    Example.skn.xml
```

`Skin/` and `Skin/new/` are both enumerated for `.skn`/`.zip` packages. The
scan remains asynchronous and metadata-sorted; no unrelated subdirectories
are recursively searched. PNG parsing is unchanged and remains supported
in either directory. This does not move existing packages or copy profiles
between packages.

The `Skin/@PackageName` selector retains `new\Example.skn`. The shared path
policy is used for startup, context menus, the options skin list/preview,
options-wide reapplication, and profile saves. Dropping a package already
in `Skin/new` onto the player reuses that path instead of copying it over a
same-named root package. Drops from outside the installation retain the
existing root `Skin/` installation behavior.

Returning from a `new/` package to the embedded skin explicitly uses
`Skin/Default.xml`, never `Skin/new/Default.xml`. Package extraction caches
now include a content fingerprint: same-named packages with the same file
timestamp cannot reuse each other's extracted BMP/PNG resources. Preview
caches already used content fingerprints and continue doing so.

## Main configuration migration

Normal application startup reads/writes only `TTPlayerRebuild.xml`. When
that file is absent, a same-directory `TTPlayer.xml` is imported once,
preserving its bytes/unknown XML fields and leaving the old file intact.
Existing new configuration always takes precedence; even a malformed new
file does not silently fall back to old settings. A read-only destination
may import old preferences in memory, but its save path is still the new name.

Local runtime staging seeds a missing new configuration from the output
directory's old user config first, then the source new/old config. It never
overwrites an existing `TTPlayerRebuild.xml` on an incremental build. No
configuration is bundled in the GitHub Actions artifact.

The CMake target remains `ttplayer_rebuild` for build-command compatibility.
Its output is `TTPlayerRebuild.exe`, with `TTPlayerRebuild.pdb` when generated.
The manual workflow's artifact collection and SHA256SUMS use these names.
Self-hosted decoder/output/tag workers keep using the running executable path;
they do not acquire a dependency on a second EXE with the previous filename.

## Verification — 2026-09-13

Release build and seven host CTest cases passed: `runtime_paths_tests`,
`stage_settings_tests`, `skin_profile_tests`, `skin_rebind_tests`,
`skin_png_tests`, `window_topmost_tests`, `taskbar_playback_tests`.

The new native regression uses an isolated temporary EXE-local runtime,
with LX-iPlay and DEFAULT_SKIN__6120 copied as same-named root/new packages
with identical timestamps. It checks async enumeration, real menu checks,
options list selection/PNG preview, options-wide reload, independent sidecars,
embedded-profile restoration, and stable main HWNDs. It also launches the
actual `TTPlayerRebuild.exe --smoke-test` twice from an unrelated working
directory, checking old-config migration, new-config precedence, the persisted
`new\common.skn` selector and preservation of the original config. No user
playlists/media are used and Discord is disabled in the fixtures.

The CMake staging test checks missing files, preservation of an existing
output config, old output import, source-new precedence and legacy-source
fallback. Workflow packaging names were updated and the output filename was
verified locally; no GitHub Actions run was triggered.

The verified `TTPlayerRebuild.exe` and migrated `TTPlayerRebuild.xml` were
deployed to `build/Release/`; `Skin/new/` was created without moving any skins
or their profiles. The old main XML was preserved byte-for-byte. The previous
EXE was moved to
`out/png-6120/ttplayer_rebuild.before-runtime-rename-20260913-154601-766.exe`.
Release EXE SHA-256:
`29C05CAF8384788D5AB33DE9E2AEF56A469BB98C1AF9DEBDDE5CE71A57AAC698`.
