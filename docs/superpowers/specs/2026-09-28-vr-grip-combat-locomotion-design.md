# VR Grip, Combat, MAG Rope, and STRIDE Locomotion Design

## Status

Approved direction from the project owner on 2026-09-28. Climbing remains
game-owned and continues to use the existing jump/ledge/autograb behavior
rather than being reassigned to grip.

## Goals

- Holding either Quest 3 Touch Plus grip closes that hand into a fist.
- A gripped hand can drive Faith's future native arm/hand pose and emit combat
  intent when swung.
- A gripped hand pulled toward the player's body emits MAG-rope intent.
- STRIDE-like locomotion uses tracked-hand swing when the left stick is neutral.
- Existing gamepad controls remain available for combat, jumping, wall-running,
  sliding, and any title action not yet represented in VR.
- Climbing stays compatible with the game's jump/ledge/autograb behavior.

## Non-goals

- Grip does not replace climbing or jump/ledge autograb.
- This milestone does not attempt to replace Faith's legs or torso animation.
- Native skeleton writes remain fail-closed until the title-specific palette
  contract is verified at runtime.

## Recommended architecture

Keep the OpenXR layer responsible only for producing canonical controller
state: grip held/value, trigger value, pose, and validity. Add a pure gameplay
intent layer above the existing motion scheme. It consumes per-hand controller
poses and timestamps, applies filtering and hysteresis, and emits:

- fist_left / fist_right: held grip state for hand pose and animation.
- combat_left / combat_right: one-shot strike events plus normalized swing
  strength and direction.
- mag_rope_left / mag_rope_right: held pull state plus pull velocity and
  direction.
- locomotion speed/sprint values from alternating arm swing.
- existing jump, crouch, wall-run, slide, and climbing-compatible gamepad
  actions without changing their ownership.

The intent layer must be deterministic and headless-testable. Runtime code
publishes the latest intent frame through the existing input/body mailboxes;
the renderer and future native Faith palette writer consume that frame without
sampling OpenXR directly.

## Input contract

Quest Touch Plus grip maps to the existing OpenXR squeeze action:

- /user/hand/left/input/squeeze/click
- /user/hand/right/input/squeeze/click

The binding remains per hand. Grip is treated as a held action, with a
short press/release hysteresis threshold so controller noise cannot flicker a
fist or repeatedly trigger combat.

The existing trigger remains available for title-native interaction and
weapons. Thumbsticks remain available for movement/turn fallback. Jump remains
the gamepad/jump action and is not consumed by grip.

## Combat detection

Combat is emitted per hand when all are true:

1. That hand's grip is held.
2. Its filtered linear speed exceeds the combat threshold.
3. The velocity projects forward into the current facing direction enough to
   distinguish a punch/swing from a small hand adjustment.
4. A cooldown has elapsed since that hand's last strike.

The event is one-shot on threshold crossing. Holding grip through a continuous
motion cannot produce an unbounded event stream. The intent contains strength
and direction so later title-specific routing can select light/heavy attacks
without changing controller sampling.

## MAG-rope detection

MAG-rope is a held per-hand state, not a one-shot event:

1. Grip is held.
2. The hand starts inside a configurable forward interaction cone.
3. The hand's velocity points toward the player's body with a minimum pull
   speed.
4. The pull remains active while grip is held and the hand stays within the
   release hysteresis band.

The output includes pull strength, direction, and active hand. Actual target
selection and rope physics remain title/gameplay integration work; the intent
layer must never invent a target or interfere with climbing.

## Hand pose and Faith mesh integration

The arm IK input uses grip state to select open/palm and closed/fist hand
poses. The arm target remains the tracked grip pose. The solved arm frame is
published through the existing body pose mailbox. Once the verified
title-specific palette writer is active, hand/arm intent is passed to that
writer along the same fail-closed validation path as the existing native
contract.

No debug skeleton overlay is enabled by default. It remains opt-in for
diagnostics.

## Locomotion priority

The locomotion arbitration order is:

1. Explicit left-stick movement.
2. Alternating tracked-hand swing movement.
3. Neutral movement.

Right-stick turn remains available. Grip state does not prevent arm swing from
driving locomotion, but a combat event is generated only for the gripped hand
that crosses the combat threshold. Physical crouch, jump, wall-run, and slide
signals remain separate intents and preserve their current gamepad fallbacks.

## Safety and performance

- No allocations or OpenXR calls in the render hook.
- Pose filtering and intent evaluation run on the existing XR/input worker.
- All thresholds are configurable through the existing runtime configuration
  path, with conservative defaults.
- Stale or invalid controller poses produce neutral intent, never extrapolated
  combat or rope input.
- Native Faith writes continue to require verified executable, palette, and
  pose contracts.

## Verification

Add deterministic tests for:

- grip press/release hysteresis and independent left/right fists;
- trigger-held locomotion remaining available;
- combat threshold crossing, direction filtering, and per-hand cooldown;
- MAG-rope pull activation/release hysteresis;
- neutral output for stale/invalid poses;
- stick-over-swing locomotion priority;
- climbing remaining unaffected by grip intent;
- arm IK hand closure being reflected in the published frame;
- existing gamepad jump, wall-run, slide, and combat fallback preservation.

The release gate remains the full CTest matrix, launcher self-test, package
manifest validation, and a bounded retail injection smoke.
