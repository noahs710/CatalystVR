# Existing Mirror's Edge Catalyst mod research

Research date: 2026-09-27. Scope: primary repositories, direct project pages, official tool pages, and source-backed technical notes. No code or game files were modified.

## Executive findings

- No public, source-available Catalyst VR conversion was found. The closest VR implementation is for the original 2008 game, not Catalyst; it is still the best reference for OpenXR lifecycle, stereo presentation, controller bindings, calibration, packaging, and hook boundaries.
- Catalyst's most useful public reverse-engineering evidence is the Nexus **Camera on HEAD** project. It identifies `Skeleton_Faith`, `Head` bone id 52, `CameraBase` 178, `CameraJoint` 179, and the `FaithCharacter` camera `CameraForwardAxis` field. It also documents why simply moving the camera to the head loses vanilla camera animation/shake and vertical freedom.
- Frosty tooling can inspect and edit Catalyst EBX assets, including skeleton and animation data, but it is an authoring/asset route rather than a runtime 6DoF pose-injection solution. Runtime native-palette replacement still needs MECVR's own validated render/game hook.
- Redistribution must remain source-only for third-party research and must not include EA/DICE assets, extracted skeletons, Frosty project caches, or Nexus mod files without explicit permission.

## Projects and reuse assessment

### Mirror's Edge VR (original 2008 game)

Primary repository: <https://github.com/letsgosportsteam/mirrors-edge-vr-mod>

The repository explicitly implements native stereo, 6-DoF head tracking, motion controllers, optional tracked hands, pistols, and parkour. Its README documents a Direct3D 9 shim, OpenXR loader placement, VDXR/Virtual Desktop operation, input fallback, in-headset settings, recentering, per-hand calibration, motion locomotion, and performance/comfort controls. The repository also contains `src`, `reference`, packaging, engine notes, and feasibility documentation. This is highly reusable as an architectural reference for MECVR's transport and launcher contracts, but it targets a different executable, renderer, bitness, and game API.

**Actually reusable:** OpenXR session/view/input state-machine ideas; controller-to-action mapping; recenter/calibration UX; frame-pacing and resolution policy separation; release packaging and third-party notice discipline; MinHook integration patterns where ABI-compatible.

**Not reusable directly:** D3D9 hooks, game offsets, original-game camera/character addresses, or any binary/assets. Port concepts only after validating Catalyst's renderer and calling conventions.

**Confidence:** High for documented architecture; low for direct Catalyst compatibility.

**License/redistribution:** The repository's own `src/` and `tools/` are MIT, but it explicitly separates `third_party/` and `reference/` under their own terms. Its notices require carrying dependency notices with binary redistribution. Treat the repo as a reference and reimplement/adapt only clearly licensed source with attribution and notices.

Sources: [README](https://github.com/letsgosportsteam/mirrors-edge-vr-mod), [MIT license](https://raw.githubusercontent.com/letsgosportsteam/mirrors-edge-vr-mod/main/LICENSE), [third-party notices](https://raw.githubusercontent.com/letsgosportsteam/mirrors-edge-vr-mod/main/THIRD-PARTY-NOTICES.txt).

### Camera on HEAD (Catalyst)

Direct project page: <https://www.nexusmods.com/mirrorsedgecatalyst/mods/332>

This mod changes `Skeleton_Faith` `GameplayBonesToSkeleton[4]` from the default camera joint (179) to the desired body joint, and changes `FaithCharacter` camera `CameraForwardAxis` from `ZAxis` to `YAxis`. The author reports `Head` id 52, `CameraBase` id 178, and `CameraJoint` id 179. The page says the actual head is fully animated, but also records the important limitation: the vanilla `CameraJoint` carries view rotation, roll, slide, and other camera animation; attaching directly to the head loses some camera shake and leaves a vertical-angle limitation. The page also reports incompatibility with other mods touching `FaithCharacter`.

**Actually reusable:** authoritative leads for asset names, bone ids, hierarchy, camera-axis convention, and test hypotheses. This should guide MECVR's observation/validation tooling and any optional Frosty companion asset, not be hard-coded as a universal native runtime map without executable/resource validation.

**Not reusable directly:** the downloaded Frosty mod, modified game assets, or its author’s work product without explicit permission. Do not package it in CatalystVR.

**Confidence:** High for the page's observed asset relationships; medium for generality across patches/configurations; low for solving runtime vertical 6DoF by itself.

**License/redistribution:** Nexus permissions state asset use is allowed in ordinary mods but not in mods sold for money, Steam Workshop, or other platforms. Follow the page’s current permissions and obtain written permission before copying files or distributing a derivative asset.

Sources: [project page and technical notes](https://www.nexusmods.com/mirrorsedgecatalyst/mods/332?tab=files), [permissions](https://www.nexusmods.com/mirrorsedgecatalyst/mods/332?tab=description).

### Frosty Tool Suite

Primary repositories/pages: [legacy FrostyToolsuite](https://github.com/CadeEvs/FrostyToolsuite), [current rewrite](https://github.com/FrostyToolsuite/FrostyToolsuite), [official downloads](https://frostytoolsuitedev.gitlab.io/downloads).

Frosty is the established authoring/launch path for Frostbite mods. The current rewrite says its SDK accesses game data and its editor/CLI create and apply modified data; it also warns that the rewrite has no functional UI and points to the stable 1.0.6.3 line. The official downloads page identifies the legacy tool's CC BY-NC-ND 4.0 terms. The current rewrite is useful for understanding the project structure, but is not a drop-in dependency for MECVR.

**Actually reusable:** workflow knowledge for inspecting EBX assets, building a small optional Catalyst asset mod, and testing load order/conflicts. Use Frosty externally during research; keep MECVR's runtime DLL independent.

**Not reusable directly:** Frosty binaries/assets in our release, Frosty SDK code under incompatible terms, or undocumented internal offsets. Do not make the launcher silently redistribute Frosty.

**Confidence:** High for supported workflow; medium for any specific asset-editing capability until verified against the installed Catalyst build.

**License/redistribution:** The legacy project's stated CC BY-NC-ND license is not compatible with copying/modifying its code or bundling it into CatalystVR. The current repository has its own repository-level terms; audit the exact commit/license before any code reuse.

Sources: [legacy README/license statement](https://github.com/CadeEvs/FrostyToolsuite), [current README](https://github.com/FrostyToolsuite/FrostyToolsuite/blob/master/README.md), [official downloads and license](https://frostytoolsuitedev.gitlab.io/downloads).

### Frosty NM Edition

Repository: <https://github.com/BreakfastBrainz2/FrostyNMEdition>

The project lists Mirror's Edge Catalyst as having full editing support and describes developer tooling for profiles and large-asset inspection. It is an editor fork, not a runtime hook. Its README states the project is MIT licensed.

**Actually reusable:** a practical Catalyst-capable asset inspection/editing workflow; possibly useful for locating animation/skeleton references and prototyping a non-runtime asset companion.

**Not reusable directly:** editor installation files, game assets, or assumptions that an edited EBX asset can provide per-frame HMD/controller pose injection.

**Confidence:** Medium-high for the listed editor support; medium for current Catalyst compatibility because the claim is project-level rather than a MECVR-specific test.

**License/redistribution:** MIT permits reuse with notice, but dependencies and bundled Frosty components still require separate audit. Prefer using the tool separately rather than embedding it.

Source: [README, supported games, and license](https://github.com/BreakfastBrainz2/FrostyNMEdition#license).

### Frost4

Repository: <https://github.com/SockNastre/Frost4>

Frost4 is a Catalyst-focused toolkit for browsing/analyzing Frostbite filesystem data, with an explicit Mirror's Edge Catalyst focus and a GPL-3.0 license. Its README warns that it is beta/unfinished and that saving modified files does not guarantee game acceptance; it also explains that Frosty-style integrity bypasses may be required.

**Actually reusable:** research lead for Catalyst file formats, EBX/RES filesystem exploration, and asset discovery. Use it to locate candidate skeleton/animation assets and compare names/IDs against the Nexus findings.

**Not reusable directly:** no runtime pose hook, no VR renderer, no guarantee that writes are accepted, and no extracted game data in MECVR.

**Confidence:** Medium for analysis leads; low for production asset authoring.

**License/redistribution:** GPL-3.0 is a strong copyleft constraint. Do not copy Frost4 code into the MIT-compatible MECVR runtime unless the whole combined distribution is intentionally made GPL-compliant. Prefer independent use or a clean-room reimplementation of file-format behavior.

Source: [README](https://github.com/SockNastre/Frost4), [GPL-3.0 license](https://github.com/SockNastre/Frost4/blob/master/LICENSE).

### Catalyst modpack / Exceed

Repository: <https://github.com/RoonMoonlight/ModernMod/tree/main/mec>

The project is a curated Catalyst modpack and documents Frosty load order, conflicts, and the need for DatapathFixPlugin on some installations. It is useful operational evidence for launcher compatibility and conflict reporting, not for VR or skeleton runtime work.

**Actually reusable:** compatibility guidance; model a launcher “asset mod conflict” warning and preserve load-order/DatapathFix documentation in support materials.

**Not reusable directly:** modpack assets or Nexus downloads without each author’s permission; it provides no native stereo, input, or IK implementation.

**Confidence:** Medium for the documented mod-manager workflow; low for technical VR value.

Source: [MEC README](https://github.com/RoonMoonlight/ModernMod/blob/main/mec/README.md).

## Recommendations for MECVR

1. **Keep the runtime architecture original-game-independent.** Borrow the open-source VR project’s separation between OpenXR state, renderer transport, game input, calibration, and packaging, but retain MECVR-specific D3D11/D3D12 and Catalyst validation.
2. **Use the Nexus findings as a discovery oracle, not a contract.** Add a research-only asset checklist for `Skeleton_Faith`, ids 52/178/179, `GameplayBonesToSkeleton`, and `CameraForwardAxis`; require executable fingerprint, resource size, stride, and live draw correlation before enabling native writes.
3. **Pursue a two-layer camera design.** Preserve/disable vanilla `CameraJoint` animation deliberately while applying HMD orientation/position in the camera or view transform. The camera-on-head experiment proves that blindly replacing the camera joint creates comfort and animation regressions.
4. **Use Frosty/Frost4 only as external authoring/research tools.** If a companion asset mod is ever needed, ship only original MECVR metadata/configuration and require users to obtain game assets and third-party tools themselves.
5. **Add launcher conflict diagnostics.** Detect/describe Frosty-managed `FaithCharacter`/skeleton edits, DatapathFix/Frosty launch state, and incompatible camera mods instead of silently overwriting or assuming a clean asset set.
6. **Adopt strict release provenance.** Keep source/license notices for any copied MIT/BSD/Apache code, avoid CC BY-NC-ND and GPL code in the runtime unless separately approved, and never distribute EA/DICE content or Nexus mod archives.

## Confidence and scope limits

This report establishes reusable leads and legal/technical constraints, not proof that a specific Catalyst bone palette or renderer hook has been identified. Public pages often describe author observations rather than reproducible source-level contracts. MECVR should continue to fail closed until live Catalyst evidence validates the exact resource and application point.
