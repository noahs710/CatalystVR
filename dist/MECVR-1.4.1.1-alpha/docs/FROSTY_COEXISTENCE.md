# Optional Frosty Mod Manager coexistence

Investigation date: 2026-09-26. This note covers Frosty Toolsuite 1.0.6.3 and
the current MECVR launcher/runtime. The optional backend described below is
implemented and remains disabled by default.

## Conclusion

Optional coexistence is implemented as an opt-in launcher mode and kept
separate from direct injection. No runtime or game-directory changes are made
by MECVR.

The safe boundary is:

1. Frosty owns mod-data generation and game launch.
2. MECVR observes the newly launched retail Catalyst process by its exact
   executable path.
3. MECVR injects the existing OpenXR loader and selected MECVR module into
   that process using the same ordered injection path as direct launch.
4. MECVR never edits Frosty profiles, `ModData`, `Data`, `Patch`, CAS files,
   or the game executable.

Direct launch remains the default and keeps its current behavior.

## Verified Frosty launch semantics

The 1.0.6.3 source parses:

```text
FrostyModManager.exe -launch <pack-name> [additional-game-arguments...]
```

`-launch` requires a pack name. Frosty selects the matching `FrostyPack` and
invokes its normal launch path after loading the configured game profile. A
missing pack cancels the launch. The command does not provide a game path;
the Frosty installation must already have Catalyst configured and, for
command-line startup, a usable default game profile.

For non-Steam installs, Frosty starts the executable under its configured
game directory with:

```text
MirrorsEdgeCatalyst.exe -dataPath "<game-path>\\ModData\\<pack-name>"
```

For installs containing `steam_appid.txt`, Frosty instead uses a
`steam://run/<appid>//<encoded arguments>/` URI and includes the equivalent
`-dataPath` argument. Therefore an integration must not append its own
`-dataPath`, and it must allow extra startup time for Steam/EA handoff.

Frosty first checks for an already-running Catalyst process and refuses to
launch another one. It can regenerate or update the selected pack's
`ModData`; that remains Frosty's responsibility and is why MECVR must not
attempt cleanup or rollback.

Sources:

- [Frosty command-line parser, 1.0.6.3](https://github.com/CadeEvs/FrostyToolsuite/blob/1.0.6.3/FrostyModManager/App.xaml.cs)
- [Frosty launch orchestration and existing-process guard, 1.0.6.3](https://raw.githubusercontent.com/CadeEvs/FrostyToolsuite/1.0.6.3/FrostyModSupport/FrostyModExecutor.cs)
- [Frosty 1.0.6.3 release](https://github.com/CadeEvs/FrostyToolsuite/releases/tag/v1.0.6.3)
- [Catalyst Frosty usage reference](https://www.nexusmods.com/mirrorsedgecatalyst/mods/89?tab=docs)

## Current MECVR interaction

The direct path in `tools/launch_preview.ps1` starts the exact executable,
waits three seconds, then injects `openxr_loader.dll` followed by the selected
MECVR module into the PID returned by `Start-Process`. The launcher validates
the game path and passes the runtime settings to that script.

That path should not be changed to start Frosty implicitly. Frosty is a
separate parent/launcher process and its Catalyst child may be delayed or
started through a platform URI.

## Implemented configuration

The launcher persists this explicit, direct-by-default section:

```ini
[launcher]
LaunchBackend=direct
FrostyExecutable=
FrostyPack=
```

The launcher provides these GUI controls:

- launch backend: `Direct MECVR` or `Frosty Mod Manager`
- Frosty executable picker
- Frosty pack-name field
- dry-run checks for executable existence, pack-name presence, and the
  already-configured Catalyst executable path
- a visible warning that Frosty must already be configured for Catalyst and
  that MECVR does not install, alter, or repair Frosty mods

The Frosty executable path should be persisted, but the game path should
remain the authoritative Catalyst path already used by MECVR for exact-process
matching and compatibility checks. Do not silently search arbitrary drives or
download Frosty.

## Implemented launch algorithm

The Frosty backend:

1. Validate the Frosty executable and nonempty pack name.
2. Record the PIDs whose normalized executable path equals the configured
   Catalyst executable; refuse to continue if one is already running.
3. Start Frosty with `-launch` and the pack name, using Frosty's directory as
   its working directory.
4. Poll for a new process whose normalized executable path is exactly the
   configured Catalyst path. Do not select by process name alone and do not
   inject into Frosty or an unrelated Catalyst process.
5. Inject `openxr_loader.dll` first, then the selected MECVR module, using the
   existing injector and fail closed if either step fails.
6. Wait on the matched Catalyst PID and send the existing MECVR shutdown
   request only to that PID when the game exits.

The process watcher has a bounded timeout, reports whether Frosty timed
out, refused the pack, or launched a different executable, and leave all
Frosty-generated files untouched. The implementation uses exact path matching
plus a pre-launch PID baseline; a name-only `Get-Process` search is not safe.

## Testing and limits

HMD-independent tests can prove argument construction, path normalization,
PID-baseline filtering, timeout behavior, and injection ordering with mock
process providers. A real no-HMD smoke test should additionally cover a
configured Catalyst install with an empty Frosty pack and confirm that the
MECVR loader attaches after Frosty has supplied `-dataPath`.

The PowerShell launch script is parser-tested and the launcher dry run
validates the Frosty executable and pack fields without starting either
Frosty or Catalyst. A real Frosty-pack smoke remains an integration check for
users with a configured Frosty installation; MECVR does not claim to install,
repair, or validate Frosty packs themselves.
