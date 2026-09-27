# Catalyst retail asset index evidence

This note records a read-only index of the installed retail archives. It is
evidence for selecting the correct Catalyst character resources; it is not a
bone map and must not be used to enable native pose writes by itself.

## Sampled installation

- Root: `C:\\Users\\Gabrielle Monlea\\Downloads\\Mirrors Edge Catalyst`
- Executable: `MirrorsEdgeCatalyst.exe`
- Executable fingerprint: `0x436D1A0883E80E86`
- Executable size: `87,780,864` bytes
- Indexed archives: `48`
- Index records: `3,619`

The same package names were found under both `Data/Win32` and `Patch/Win32`.
The patch layer is therefore part of compatibility discovery and must not be
ignored when validating a user's installation.

## Faith and skeleton resources observed

Faith-specific package paths:

- `characters/ainpc/ainpc_spec_faith`
- `characters/customizations/faith/c_faith_prologue_nogridlink_npc`
- `characters/faith/hair/faith_hair_3pcloth_ingame_noskiplods`
- `characters/faith/hair/faith_hair_3pcloth_ingame_noskiplods_mesh`
- `characters/faith/hair/t_alphahair_a`
- `characters/faith/hair/t_alphahair_aniso`
- `characters/faith/hair/t_alphahair_d`
- `characters/faith/hair/t_faithhair_n`

Shared skeleton package paths:

- `characters/skeletons/masterske_female`
- `characters/skeletons/masterske_female_balc`
- `characters/skeletons/masterske_femaleheels`
- `characters/skeletons/masterske_male`
- `characters/skeletons/masterske_pigeon`
- `characters/skeletons/skeleton_female`
- `characters/skeletons/skeleton_female_balc`
- `characters/skeletons/skeleton_female_heels`
- `characters/skeletons/skeleton_male`
- `characters/skeletons/skeleton_pigeon`

Shared animation/skeleton package paths:

- `animations/skeletons/masterpropscommon_ske`
- `animations/skeletons/propscommon_ske`

## Engineering consequence

These records establish a strong offline filter for future resource capture:
prefer Faith's character package and the female master/runtime skeleton
families, while preserving the active `Patch` override layer. They do **not**
establish the runtime palette layout, joint ordering, or Faith bone indices.
Those values still require a live, resource-qualified capture from the running
game and must match the executable fingerprint, resource size, palette
offset/stride/layout, and complete unique joint map before the guarded writer
can be enabled.

The reproducible command is:

```powershell
powershell -ExecutionPolicy Bypass -File tools/index_catalyst_assets.ps1 `
  -GameRoot 'C:\Users\Gabrielle Monlea\Downloads\Mirrors Edge Catalyst' `
  -OutputPath "$env:TEMP\\mecvr_catalyst_asset_index.json"
```
