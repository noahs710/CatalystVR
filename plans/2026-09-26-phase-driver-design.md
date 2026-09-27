# Autonomous Phase Driver — Design

- Status: APPROVED (2026-09-26, supervised-autonomy variant)
- Purpose: remove the human driver from M3a (and later) captures. The user
  launches + positions Faith, says "go"; the driver performs all phases
  with machine-precise scripted input, then quits the game.
- Gates respected: M2 frozen (untouched); res-override frozen (WI-1..5 stay
  queued); M3a observation-only intact (synthetic OS input is what a player
  does; zero game-memory writes; no probe changes in v1).

## Decisions

- D1: supervised autonomy (user sets scene; driver performs + quits).
- D2: synthetic keyboard/mouse via SendInput. No virtual-gamepad driver.
- D3: phase markers via driver wall-clock log joined to presents in
  analysis (no probe changes).

## Design

1. Tool: `src/tools/phase_driver` (C++, user32 only, new CMake target).
   Reads a phase script, focuses the game window by name, emits the input
   timeline with precise sleeps, appends `markers.jsonl`
   (phase, wall_ms_start/end, events_sent), then quits via Alt+F4.
2. Script schema (versioned, e.g. `tools/phase_scripts/m3a_round3.json`):
   array of phases; each phase = {name, duration_ms, mouse_dps{x,y} |
   mouse_sweep{axis, amplitude_deg, period_ms}, keys_held[]}. Example:
   yaw = mouse_sweep{x, 90 deg, 4000 ms} x 20 s; walk = keys_held:[W].
   The driver converts mouse degrees to raw deltas using the game's
   measured deg-per-delta, self-calibrated from the probe log (basis
   change per emitted delta during the first sweep), stored in the script.
3. Safety: any physical keypress/mouse-button aborts instantly; focus
   guard re-asserts per phase and aborts on focus loss; `--dry-run`
   prints the event list and sends nothing. Game launch + focus control
   remain behind the operator's shell approval each run.
4. Analysis join: markers.jsonl wall times map to presents via the probe
   log's present rate + off0 sim-clock (existing v1-v6 tooling pattern).
5. Tests: unit test for the script compiler (phases -> timestamped event
   list, sweep discretization, abort/dry-run paths) wired into
   `tests/CMakeLists.txt`; SendInput edge verified by dry-run + live smoke.

## Out of scope

Full-blind launch/menu-nav (offered, declined); virtual-gamepad driver
(phase 2 if analog curves ever needed); probe modifications; any
Map/Unmap writes.

## Sequencing

Spec review -> implementation plan -> implement -> dry-run review ->
live smoke (user sets scene, says go) -> round-2b/3 capture with driver.
