# MECVR — Runtime VR Conversion Layer for Mirror's Edge Catalyst
Date: 2026-09-25 | Status: draft awaiting user review | Approach: C (motion-first, equal gamepad fallback)

Source: user-authored 27-section implementation plan, validated against prior research
(survey of Luke Ross R.E.A.L, Praydog RE Framework/UEVR, Team Beef ports, generic
injectors; Frostbite VR survey; titanfall2vr architecture; local game-dir inventory).

## Target result
One `MECVR` package delivering: full HMD 6DoF (rotation + position), binocular OpenXR
stereo, multiple stereo modes, Quest Touch motion controls, Xbox/XInput gamepad,
hybrid Touch + gamepad, HMD 6DoF + Xbox-only mode (no motion controllers required),
SteamVR / Meta / VDXR runtimes via the standard OpenXR loader, auto + manual
resolution scaling, auto/current refresh handling + manual selection where exposed,
runtime reprojection compatibility, parkour comfort controls, VR HUD + menus,
optional motion-controlled Faith arms/hands, GUI launcher/configurator, Frosty mods
left intact, no permanent game-asset modification, clean uninstall, version
detection, logs, crash recovery, safe hook validation.

## Architecture
```text
MECVR Launcher
     |-- game discovery / Frosty-aware launch / process attach
     |-- configuration, OpenXR diagnostics, mod conflict detection
     `-- injection --> mecvr_runtime.dll
                              |-- OpenXR (tracking, input, timing, swapchain)
                              |-- Game Bridge (camera, player, animation, input)
                              |-- DX11 Bridge (stereo, depth, HUD, mirror)
                              `-- VR Gameplay (comfort / hands / UI)
```

## 1. Operating rules and project foundation
- Evidence-gated hooks: every hook recorded in `HOOK_EVIDENCE.md` with: exe
  hash/version, module, signature, resolved address, function/data hypothesis,
  discovery method, observed evidence, calling convention, thread, lifetime,
  validation test, failure behavior, confidence. Unknown builds fail closed for
  invasive hooks. Never promote a hypothesis to a permanent offset silently.
- Parallel workstreams (`render/OpenXR`, `game RE`, `input/gameplay`, `launcher`,
  `test/compatibility`) with explicit file ownership before parallel editing.
- Foundation docs: ARCHITECTURE, HOOK_EVIDENCE, GAME_BUILDS, OPENXR, INPUT,
  STEREO, CAMERA, COMFORT, COMPATIBILITY, TEST_MATRIX, RELEASE.
- Layout: `/docs /src (bootstrap, compatibility, config, diagnostics, game,
  input, openxr, render, camera, gameplay, ui, motion, launcher) /tests /tools
  /third_party`.

## 2. Compatibility-first loader (before VR rendering)
- Primary loader is `MECVR.exe` + `mecvr_runtime.dll`. Do NOT use `d3d11.dll`,
  `dxgi.dll`, or a Frosty `.fbmod` as the loader (collision points for
  ReShade/3Dmigoto/shader mods).
- Modes: launch normally, launch through Frosty, attach to running MEC, wait for
  MEC, diagnostics only. Frosty flow: user configures Frosty mod set, MECVR
  starts/waits for the Frosty-launched game, validates `MirrorsEdgeCatalyst.exe`
  (also recognize `MirrorsEdgeCatalystTrial.exe` seen in local inventory),
  injects runtime DLL, leaves Frosty-generated data untouched. Attach-after-launch
  is the escape hatch.
- Executable-name recognition is NOT build compatibility. Retail and Trial are
  independently hashed, independently fingerprinted, independently
  evidence-hooked, and independently select `IGameAdapter`. Never treat a Trial
  signature as valid against retail or vice versa.
- Compatibility scanner enumerates loaded modules and warns (not blocks) on:
  ReShade, 3Dmigoto, Special K, RTSS, other d3d11/dxgi proxies, graphics
  debuggers, unknown Present hooks, other MEC camera tools.
- Compatibility rule: Frosty content/asset mods are first-class targets; other
  DLL injectors, shader replacers, or mods touching the same camera/render
  functions need collision detection + testing. "Compatible with all mods" is
  NOT literally guaranteed.
- Runtime supports bounded shutdown and clean game-process exit. Hot
  detach/unload (FreeLibrary while render threads may hold trampolines) is
  OPTIONAL until all hooks prove safe quiescence — do not spend early effort
  perfecting arbitrary hot unload.
- Phase exit gate: vanilla start, Frosty start, several ordinary Frosty mods,
  MECVR load + bounded shutdown, diagnostics, identical-to-vanilla behavior
  with VR disabled.
## 3. OpenXR core (no MEC dependencies initially)
- Standard OpenXR (not Quest-specific APIs); runtimes (VDXR, Meta, SteamVR)
  provide the implementation underneath.
- State machine: UNKNOWN, IDLE, READY, SYNCHRONIZED, VISIBLE, FOCUSED,
  STOPPING, EXITING, LOSS_PENDING.
- Implement: instance creation, runtime identification, system discovery, D3D11
  graphics requirements, session, LOCAL + STAGE (where available) + VIEW spaces,
  recenter, view config, swapchains, events, frame timing, session loss, restart.
- `xrWaitFrame` is authoritative for scheduling (predicted display time +
  interval). Mock backend (`MockXRBackend` vs `RealOpenXRBackend`) supplies
  configurable pose, synthetic controller poses, fake frequency/resolution,
  button input, head-motion trajectories for headset-less development.
- Exit gate: independent test app shows two views and survives disconnect,
  sleep, runtime absent/restart, focus loss, recenter, resolution changes.

## 4. DX11 rendering bridge (observation first)
- Hook minimum: `IDXGISwapChain::Present`, `ResizeBuffers`, render-target/depth
  binding where necessary, viewport changes, draw/dispatch boundaries only when
  justified. Full D3D11 state preservation around MECVR work.
- Identify: primary swapchain, final color buffer, depth buffer, pre-HUD frame,
  post-HUD frame, motion vectors if accessible. GPU inspectors + frame dumps
  before stereo. No random shader manipulation.
- First visible milestone: copy MEC's rendered frame into an OpenXR projection
  layer (mono is fine) to prove MEC -> DX11 capture -> OpenXR -> headset.

## 5. Camera discovery and full 6DoF
- Find authoritative FP camera: position, orientation, view matrix, projection/
  FOV, animation offsets, player/root transform, body yaw, roll, shake; and when
  it is generated relative to rendering. Don't settle for FOV tweaks.
- Transform hierarchy: World -> Player Root -> VR Body Origin -> Game Camera
  Base -> Comfort Filtering -> HMD Local Pose -> Eye Pose. HMD orientation +
  translation independent of Faith's animation camera.
- 6DoF: lean, physical crouch, lateral head move, slight forward/back, look
  behind, natural inspection. Physical head movement must NOT drive world
  locomotion. Configurable bounds for unreasonable translations/wall penetration.
- Yaw model separates locomotion heading / body heading / head heading / hand
  aim (e.g. left stick moves, right stick turns body, HMD turns head, Touch aims).
- Xbox-only VR is a complete route (HMD 6DoF + stick movement + buttons), title
  screen through gameplay, not a debug fallback.
- Exit gate: free-roam positional + rotational head movement without altering
  simulation or breaking locomotion.
## 6. True stereo renderer (major milestone)
- Preferred: native dual-pass. Per logical frame: simulation update once ->
  immutable frame state -> left-eye render -> right-eye render -> XR submit.
  The second eye must NOT re-update the world. Discover MEC's
  simulation / scene-prep / submission / Present split to splice this in.
- Hard gate: do not advance past dual-eye rendering if the eyes show different
  simulation states.

## 7. Stereo modes (one `IStereoRenderer` interface)
- A, Native Dual Pass (target): two independent viewpoints, correct near
  geometry/translation/depth.
- B, Depth-Reconstructed fallback: one eye + depth, reconstruct the other;
  expect disocclusion/particle/transparency/reflection/screen-space artifacts.
- C, Mono VR: debug/compatibility only. D, Cinema: virtual screen for
  unsupported cutscenes, startup, broken cameras, failure recovery.
- No single-pass instanced stereo until dual-pass proves the hooks.

## 8. Correct projection and world scale
- Never fake stereo with +/-IPD/2 on a normal projection. Build each eye's
  projection from its OpenXR `XrView` pose + asymmetric FOV. Calibrate world
  units->meters, near/far planes, IPD, camera/player height; configurable scale
  with mathematically derived default.

## 9. Resolution
- OpenXR-recommended/maximum view dimensions drive `Auto`. Manual: 50-150% +
  Custom. Keep OpenXR swapchain size, MEC internal size, and desktop mirror size
  independent; never resize the desktop window for the headset. Later: dynamic
  resolution (Off/Conservative/Aggressive) from rolling GPU frame time.

## 10. Refresh handling
- Keep HMD physical refresh, OpenXR predicted interval, and MEC sim/render cap
  separate. Where `XR_FB_display_refresh_rate` exists, enumerate and offer only
  runtime-exposed rates (Runtime Default, Auto, 72/80/90/120...). Otherwise
  derive cadence from `xrWaitFrame`; MECVR never pretends to change the panel.

## 11. Reprojection
- Target runtime-native reprojection first (accurate predicted display time,
  correct poses, stable pacing, depth where available). Modes: Auto, Full Rate,
  Half Rate/Reprojection-friendly, Runtime Controlled. Depth composition and
  custom motion reprojection are late milestones, not first-playable work.
## 12. Unified input layer (Approach C: equal from day one)
- Layered action taxonomy (inputs vs gameplay outcomes are distinct):
  Pose inputs: HeadPose, LeftHandGripPose, LeftHandAimPose, RightHandGripPose,
  RightHandAimPose. Continuous: Move2D, Turn2D, Look2D (mouse/gamepad fallback,
  not HMD pose), TriggerLeft/Right, GripLeft/Right. Discrete intents: Jump,
  Crouch, Slide, Interact, Melee, HeavyMelee, Ability, Sprint, Menu, Pause,
  Recenter. Derived/contextual MEC actions: Vault, WallRun, Climb, Swing, Rope
  and contextual traversal states — MEC already decides when a jump becomes a
  vault/wall-run/climb; MECVR feeds intent into the game's parkour logic and
  does NOT recreate Frostbite's traversal decisions.
- `VRActionState` carries more than values:
```cpp
struct ActionState {
    ActionValue value;      // analog scalar/2D or digital
    ActionSource source;    // which device route owns it
    uint64_t timestamp;     // source clock tick
    bool pressed;
    bool released;
    bool held;              // a held action stays owned by its original
                            // source until released
};
```
  Pose actions additionally carry pose validity / tracking quality separately.
- Gameplay code never knows whether Jump came from Touch A, Xbox A, Space, or
  a gesture.
- Device adapters: OpenXRInput, XInputInput, MECNativeInput,
  KeyboardMouseInput, resolved through `VRActionState`.
- Config enums (defined in sub-project 1, functional later as marked):
  InputMode (Motion, HybridAuto, XboxVR, Custom), DominantHand (Right, Left),
  TurnMode (Smooth, Snap), MovementReference (Head, Body, LeftHand).

## 13. Control profiles (all fully supported)
- Motion: HMD head; left Touch movement/left hand; right Touch turning/right
  hand/aim; triggers combat/context; grip grab/context; buttons parkour/actions.
- Hybrid: HMD + Touch tracking + Xbox simultaneously, no switching screen.
  Arbitration is per action family, NOT global most-recent-wins: HEAD is always
  HMD when VR is active; POSE is Touch when tracked and never stolen by Xbox
  stick activity; LOCOMOTION/TURN arbitrate between Xbox and Touch sticks with
  activity threshold + deadzone + short hysteresis (inactive -> exceeds
  threshold -> owned -> stays owned while active -> neutral + hysteresis ->
  unowned), so stick noise never steals ownership and swinging a controller
  never fights Xbox locomotion; DIGITAL game actions merge presses from both
  devices with held actions owned by the original source until release;
  MOTION/GESTURE actions are Touch-only; SYSTEM (recenter/menu) accepts either
  route.
- Xbox VR: HMD 6DoF head + Xbox for the entire game, title screen onward.
- Use OpenXR pose/action-space machinery (grip/aim spaces, suggested bindings).

## 14. Motion-controlled hands (layered, non-blocking)
- Stage 1: MECVR's own minimal tracked hand/controller models (prove
  transforms, rays, calibration, offsets).
- Stage 2: locate Faith's FP arm skeleton (location, bones, timing, hand and
  camera-relative transforms, IK opportunities).
- Stage 3: runtime upper-body IK (grip pose -> hand -> wrist -> elbow solver ->
  shoulder constraint), preserving MEC animations.
- Stage 4: contextual motion actions (punch/push/grab/interact). Parkour stays
  gameplay/context-driven initially; physical imitation of wall-runs/vaults and
  gesture shortcuts come later as options.

## 15. Parkour VR comfort system
- Independently adjustable: camera roll, animation pitch, camera shake, landing
  kick, vault/roll influence, wall-run roll, slide camera drop, swing camera
  movement, FOV effects, motion blur, vignette. Presets: Original, Balanced,
  Comfort, Strong Comfort, Custom.
- Invariants: comfort filters game-camera animation, NEVER live HMD
  tracking (never filter HMD yaw/pitch/roll or physical translation — a laggy
  or stabilized HMD pose would be disastrous). Comfort may attenuate
  camera-local animation offsets but must NOT silently cancel Faith's
  authoritative world/root translation (e.g. a slide's real downward movement
  stays; only the layered artificial camera animation is attenuated).
- May filter: Frostbite camera animation roll, animation pitch, shake,
  animation camera offsets, FOV impulses.
- Default preset is Balanced (first launch), NOT Original. Initial design
  targets (tunable by later playtesting, not validated thresholds):
  ORIGINAL: game animation 100%, wall-run roll 100%, shake 100%, landing kick
  100%, FOV effects 100%, dynamic vignette off, motion blur = game setting.
  BALANCED: HMD 100% untouched; camera roll ~25%; animation pitch ~40%; shake
  ~25%; landing kick ~35%; roll/somersault rotation 0%; wall-run roll ~20-25%;
  FOV kick 0%; motion blur off where controllable; mild dynamic vignette in
  high-angular-motion traversal. COMFORT: roll ~5-10%, pitch ~15%, shake ~10%,
  landing ~15%, somersault 0%, wall-run roll 0-10%, FOV 0%, blur off, moderate
  vignette. STRONG COMFORT: all artificial roll/pitch/shake/somersault/wall-run
  roll/FOV 0%, blur off, strong vignette. Plus Custom.

## 16. Collision and head clipping
- Track body-camera origin, physical HMD offset, desired vs collision-safe
  camera. Options: allow clipping, fade to black, push camera, clamp offset.
  Never push Faith's gameplay capsule because the user leaned through a wall.
## 17. HUD and menus
- Preferred path: scene render -> capture pre-HUD -> stereo world -> capture
  HUD -> OpenXR quad/cylinder layer. Community stereo fixes suggest much of the
  HUD can be handled together at the shader level (starting point for finding
  the UI stage).
- HUD modes: head-locked, soft head-locked, world-locked, depth HUD, off; with
  distance/scale/curvature/opacity/vertical-position controls.
- Menus work with Touch pointer/buttons/stick, Xbox, and mouse as emergency
  fallback.

## 18. Scripted-camera and cutscene manager
- Classify: normal gameplay, scripted FP, third-person/cinematic, pre-rendered,
  loading, menu, unknown. Per category: native VR, reduced-motion VR, cinema
  screen, or force mono. Unknown cameras must never attach the HMD to a wild
  cinematic camera.

## 19. Mirror output
- Modes: off, left eye, right eye, center crop, undistorted spectator,
  side-by-side debug. Mirror never determines VR resolution.

## 20. GUI launcher (zero command-line flags for normal use)
- Tabs PLAY / VR / GRAPHICS / CONTROLS / COMFORT / MODS / DIAGNOSTICS / ADVANCED.
- PLAY: install, version, Frosty state, runtime, headset/controller state,
  Start/Attach. VR: stereo mode, world scale, recenter, standing-seated,
  refresh, resolution, render scale, reprojection. CONTROLS: Motion/Hybrid/
  Xbox/Custom with interactive binding display. MODS (read-only): Frosty
  profile, injected DLLs, known collisions, unknown injectors; never reorder
  user Frosty mods unprompted. DIAGNOSTICS: runtime, extensions, resolutions,
  refresh, CPU/GPU/XR timing, build hash, hook status, devices, modules,
  errors; copy diagnostics, open logs, support bundle.

## 21. Version-safe game bridge
- `IGameAdapter` + per-build `MECAdapter_<build>` + `UnknownBuildAdapter`
  (safe facilities only: OpenXR, diagnostics, possibly cinema; refuses
  unvalidated invasive hooks). Signatures use semantic validation (references,
  code section, neighbors, call targets), not bare byte matches.
## 22. Performance instrumentation
- Measure separately: sim CPU, game-render CPU, per-eye GPU, MECVR GPU, OpenXR
  wait/submit, HUD, mirror. In-headset compact overlay. Optimize from
  measurements; temporal and screen-space effects get isolated investigation
  (Catalyst is heavy on post-processing/temporal techniques).

## 23. Automated testing (no MEC/HMD required where possible)
- Cover: matrix/quaternion conversion, OpenXR->MEC coordinates, IPD offsets,
  projection creation, world scale, neck model, camera composition, input
  merging, binding conflicts, calibration, dynamic-resolution algorithm, frame
  pacing, config migration, signature scanner/validation, module detection,
  launcher state machine. Fake game-camera harness: synthetic camera/HMD/hands/
  gamepad -> transform system -> expected eye matrices.

## 24. Hardware validation ladder
- M0 DLL loads, game unchanged. M1 OpenXR session. M2 mono frame in headset.
- M3 HMD rotation. M4 full 6DoF. M5 per-eye projection. M6 native stereo.
- M7 Xbox VR playable. M8 Touch inputs. M9 tracked hands. M10 HUD/menus.
- M11 parkour comfort. M12 arm IK. M13 auto resolution/refresh. M14 compat/perf.
- M15 packaged release. Gate: never pass M6 with different sim states per eye.

## 25. Compatibility test matrix (release-blocking Frosty coexistence)
- Vanilla; Frosty texture mod; Frosty gameplay mod; multiple mods; heavy
  modpack; ReShade; 3Dmigoto; EA App; Steam + EA App; VDXR/Meta/SteamVR; Touch/
  Xbox/hybrid; headset sleep/wake/restart; alt-tab; resolution change;
  fullscreen/windowed.

## 26. Packaging
- Ship: `MECVR/MECVR.exe, mecvr_runtime.dll, openxr/, assets/, licenses/,
  README.html`. No DLLs beside the game exe; launcher discovers + attaches.
- User data in `Documents/MECVR/`: `config.toml, profiles/, logs/, crashes/,
  diagnostics/, cache/`. Uninstall = delete MECVR (+ optional config).

## 27. Definition of done (ALL required for "VR port complete")
Binocular stereo; same sim state both eyes; correct OpenXR projections; full
positional + rotational tracking; stable world scale; Xbox-only, Touch, and
hybrid playability; VR HUD + usable menus; free roam, parkour, combat, scripted
events, death/restart, checkpoints, major missions, cutscene handling; auto
resolution, refresh handling, reprojection-compatible timing, stable recenter;
comfort options; clean Frosty coexistence; clean launcher; no permanent
game-file changes; crash diagnostics.

## Guiding mindset
Don't build a "VR injector." Build a compatibility layer that makes Catalyst
behave like a native OpenXR game. Unified input from the first playable
milestone; Xbox+HMD 6DoF and Touch as equal routes; true dual-pass stereo with
one simulation epoch as the central milestone. OpenXR supplies per-eye
resolution, poses, input spaces, and predicted timing; the hard work is safely
splicing those into Frostbite's camera and rendering pipeline.

## Process notes (spec self-review, 2026-09-25)
- Placeholders: none (no TBD/TODO). Consistency: Approach C unified actions
  vs layered hand visuals (§14) is intentional — actions unified day one,
  visuals phased. Cinema mode (§7) + cutscene manager (§18) agree.
- Scope: too large for one implementation plan. Decompose into sub-projects,
  in order: (1) loader + OpenXR core + DX11 observe + input interfaces
  (M0-M2; defines IInputDevice, VRActionState, ActionValue, ActionSource,
  arbitration, binding/config schema, OpenXR + XInput skeletons — wiring only
  Recenter, diagnostics, and test inputs; MEC gameplay integration stays in
  sub-project 3); (2) camera 6DoF +
  dual-pass stereo (M3-M6, hard gate); (3) MEC gameplay input + Xbox VR (M7-M8);
  (4) HUD/menus + comfort + hands (M9-M12); (5) auto resolution/refresh +
  compat/perf + packaging (M13-M15). Each gets its own plan cycle.
- Ambiguity resolved: hybrid conflicts -> most-recent device wins; Trial exe ->
  recognized for version detection; no git repo in MECVR, so no commit.
- Status: awaiting user review of this file before implementation planning.

## Sub-project 1 milestone detail (M0-M2; approved revision)
- P1.0 Foundation: git repository, CMake/toolchain, docs structure, logging,
  config, tests, CI/local build script.
- M0A Baseline characterization: detect retail/trial exe; hash + version;
  enumerate modules; identify DXGI/D3D11 device and primary swapchain; record
  resolution/window mode; measure Present cadence; record overlays/injectors;
  produce support report.
- M0B Safe runtime injection: mecvr_runtime.dll loads, bounded worker
  lifecycle, zero rendering modifications, identical game behavior, working
  diagnostics.
- M0C DX11 observer: Present/Resize observation, swapchain tracking,
  device/context acquisition, state-preservation tests, frame capture/dump, no
  XR dependency.
- M1A Standalone MockXR: mock runtime abstraction, poses, views, controller
  state, synthetic timing. M1B Real OpenXR: instance/system/session, event
  state machine, view config, reference spaces, swapchains, xrWaitFrame timing,
  graceful headset absence. M1C OpenXR test scene: standalone stereo test on
  real + mock backends.
- M2A MEC frame capture: identify final game color buffer, confirm pre/post-HUD
  boundaries where possible. M2B MEC -> OpenXR mono: game frame copied into XR,
  headset pose does NOT touch the game camera yet, stable submit, desktop
  remains usable.
- M2 PASS: MEC image reliably visible in headset; no camera manipulation, no
  stereo hack, no gameplay hooks. Deliberately mono — proves
  Catalyst -> DX11 -> MECVR -> OpenXR frame loop -> headset.





