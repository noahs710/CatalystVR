# MECVR Input Layer (T4, Sub-project 1)

Scope: input interfaces + skeletons ONLY. Architecture, tests, and config.
No MEC gameplay wiring, no game process interaction. Only Recenter,
diagnostics, and test paths are meaningful; everything else is a skeleton
for sub-project 3.

## Files owned by the input stream

- `src/input/actions.h` — layered action taxonomy, `ActionValue`,
  `ActionSource`, `ActionState`, `PoseState`, config enums.
- `src/input/arbitration.h` / `arbitration.cc` — per-family arbiters.
- `src/input/device.h` — `IInputDevice` interface + `RawFrame`.
- `src/input/openxr_device.h` / `.cc` — OpenXR (HMD + Touch) skeleton.
- `src/input/xinput_device.h` / `.cc` — XInput (Xbox) skeleton.
- `src/input/input_tests_main.cc` — assert-style unit-test exe main.
- `src/config/input_bindings.h` / `.cc` — isolated input-config module on
  top of the frozen Registry (`src/config/config.h`, never modified).
- `docs/INPUT.md` — this file.

## Layered taxonomy (spec section 12)

Pose inputs: `HeadPose`, `LeftHandGripPose`, `LeftHandAimPose`,
`RightHandGripPose`, `RightHandAimPose`.
Continuous: `Move2D`, `Turn2D`, `Look2D` (mouse/gamepad fallback, never the
HMD pose), `TriggerL`/`TriggerR`, `GripL`/`GripR`.
Discrete intents: `Jump`, `Crouch`, `Slide`, `Interact`, `Melee`,
`HeavyMelee`, `Ability`, `Sprint`, `Menu`, `Pause`, `Recenter`.
Derived MEC actions: `Vault`, `WallRun`, `Climb`, `Swing`, `Rope` — flagged
by `IsGameDerived()`. MEC already decides when a jump becomes a
vault/wall-run/climb; MECVR feeds intent into the game's parkour logic and
does NOT recreate Frostbite's traversal decisions. The input stream must
NEVER produce derived actions.

`ActionState` carries `{value, source, timestamp, pressed, released, held}`.
A held action stays owned by its original source until release
(`NextDigitalState()` in `actions.h`, `DigitalArbiter` for merged routes).
Pose validity/tracking quality travel in `PoseState` (`valid`, `quality`
0..1, position, orientation), separately from button state.

Config enums: `InputMode` (Motion, HybridAuto, XboxVR, Custom),
`DominantHand` (Right, Left), `TurnMode` (Smooth, Snap),
`MovementReference` (Head, Body, LeftHand). Defined now, functional later.

## Per-family arbitration (spec section 13)

Family ownership is independent — each `StickArbiter`/`DigitalArbiter`
instance owns only its own family, so Xbox can own locomotion while Touch
owns turning. There is NO global most-recent-wins.

- HEAD: always HMD when VR is active (`HeadSource()`). No arbitration.
- POSE: Touch when tracked, never stolen by Xbox stick activity
  (`PoseOwner(tracked)`; untracked yields `kNone` — Xbox never owns a pose).
- LOCOMOTION/TURN (`Move2D`, `Turn2D`, and the fallback `Look2D`):
  `StickArbiter` with activation threshold (default 0.35) + deadzone
  (default 0.15) + short release hysteresis (default 6 frames):
  inactive -> exceeds threshold -> owned -> stays owned while the owner's
  stick is above the deadzone -> neutral + hysteresis -> unowned. Stick
  noise below the deadzone never claims ownership; a challenger cannot
  steal during hysteresis — ownership returns to unowned first, then a
  claiming stick takes it fresh in the same frame.
- DIGITAL (discrete intents plus analog `TriggerL/R`, `GripL/R`, which
  merge rather than arbitrate): `DigitalArbiter` merges presses from both
  devices; while held, attribution stays with the originating source until
  fully released (both routes neutral). A release from a non-originator
  changes nothing. Press/release edges fire exactly once.
- MOTION/GESTURE: Touch-only. No member actions exist yet in the taxonomy;
  the family is reserved for sub-project 4 gesture work.
- SYSTEM (`Menu`, `Pause`, `Recenter`): either route, reusing the digital
  merge rule.

## Devices

`IInputDevice` translates raw hardware into `RawFrame`s (per-action values
+ per-pose `PoseState`s) keyed by `ActionId`. The device classes remain
testable seams, while the real XR backend now creates an optional native
OpenXR action set, grip spaces, boolean buttons, and thumbstick state. If a
runtime lacks an interaction profile, action setup fails closed and the view
session remains usable with neutral controller state.

## Binding schema

`src/config/input_bindings.*` owns the Registry section
`"input_bindings"` (registered via `RegisterInputBindings()`). Keys:
`mode` (`motion|hybrid_auto|xbox_vr|custom`), `dominant_hand`
(`right|left`), `turn_mode` (`smooth|snap`), `movement_reference`
(`head|body|left_hand`), plus one key per action name
(`ActionName()`, e.g. `Move2D`) with value
`"route:path;route:path"` (e.g. `"touch:left_stick;xbox:left_stick"`).
`DefaultInputBindings()` ships Approach-C hybrid defaults and binds
NOTHING to derived actions. Load: missing keys keep defaults; malformed
binding entries are skipped per entry; unknown enum values fail closed
(`LoadInputBindings` returns false). Unknown stored keys stay in the
Registry/file untouched. Save/load round-trips through the frozen
Registry file format (`# mecvr-config v1`, `[section]`/`key=value`).

## Build and test

The repository CMake flow owns the integrated tests. Run:

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64
cd /d %TEMP%\mecvr_t4
cl.exe /W4 /WX /EHsc /std:c++17 /I<repo>\src <repo>\src\input\arbitration.cc <repo>\src\input\openxr_device.cc <repo>\src\input\xinput_device.cc <repo>\src\config\config.cc <repo>\src\config\input_bindings.cc <repo>\src\input\input_tests_main.cc /Fe:input_tests.exe
input_tests.exe
```

The authoritative Release check is `ctest --test-dir build -C Release
--output-on-failure`; the current Release run is 25/25 green.

## Motion scheme (M3 input foundation)

`src/input/motion_scheme.*` adds a deterministic device-neutral route for
STRIDE-style movement. When the left stick is neutral, alternating forward /
back hand motion produces forward `Move2D`; a sufficiently fast alternating
swing produces `Sprint`. An active left stick always wins, the right stick
remains `Turn2D`, and trigger/grip/jump/crouch levels plus tracked hand poses
are passed through. A fast hand displacement produces a one-frame `Melee`
intent. The optional game bridge debounces that intent into a primary-attack
click when neither trigger is held. Positions are OpenXR meters in the body frame; the scheme does not
modify world/player transforms. Coverage is in CTest `motion_scheme_test`.
The live body worker keeps a calibrated standing floor instead of deriving the
floor from the current head height, so physical head lowering reaches the
solver's crouch/slide states. Raising both tracked hands above the head also
produces a debounced physical jump intent through the same game bridge.

## Open items (unresolved design questions, not silent assumptions)

1. Simultaneous stick claim (both routes past threshold while unowned):
   larger deflection wins, exact tie goes to Touch. Playtesting may prefer
   most-recent-wins or a handedness-aware rule.
2. Simultaneous press on both routes: attribution goes to Touch. Same
   playtesting caveat as (1).
3. Merged digital hold continues while EITHER route holds (attribution
   stays with the originator until full release). Alternative reading —
   originator release ends the hold even if the other route holds — was
   rejected as less useful for gameplay; revisit if sub-project 3 finds
   stuck-input cases.
4. `TriggerL/R`, `GripL/R` merge as DIGITAL rather than stick-arbitrate.
   If analog trigger fencing (e.g. half-pull vs full-pull per device) is
   needed, they may need their own family.
5. `Look2D` sits in LOCOMOTION/TURN (stick arbitration). If mouse-look and
   stick-turn ever compete for it, split mouse out as its own source.
6. MOTION/GESTURE has no member actions yet; gesture action IDs arrive
   with sub-project 4 hands work.
7. Threshold/deadzone/hysteresis defaults (0.35/0.15/6 frames) are
   unvalidated initial values, not playtested thresholds — tune with real
   sticks in sub-project 3.

## Runtime boundary

The OpenXR action set is sampled on the XR-owned worker cadence and exposed
through `ControllerState`; the game Present thread never calls OpenXR. The
motion scheme consumes device-neutral samples, and game-specific action
injection remains a separate integration boundary with its own evidence gate.

Comfort turning is now part of that bridge. Smooth mode preserves proportional
right-stick mouse motion. Snap mode emits a configured angular step with
threshold, neutral re-arm, and cooldown hysteresis; `MECVR_TURN_MODE=snap`
selects it at runtime and smooth mode remains the default.

The packaged launcher enables the viewport-qualified temporal stereo producer
by default (`MECVR_ENABLE_STEREO=1`). `-DisableStereo` turns it off.

Holding both controller menu buttons requests one recenter. The worker
recreates the OpenXR LOCAL space on the next frame and advances the immutable
pose-snapshot generation, so the camera anchor resets exactly once per chord.
