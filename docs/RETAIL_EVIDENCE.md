# Retail validation evidence

## Observe-only D3D11 run

- Executable: `MirrorsEdgeCatalyst.exe` supplied in the local retail install.
- Probe: `mecvr_m3a_probe.dll`, palette discovery enabled, diagnostic profile.
- HMD: not required; no game or process files were modified.
- Runtime result: the process remained responsive and was stopped after the
  observation window.
- Probe result: approximately 5,795 presents at approximately 193 FPS,
  `state_fail=0`, `drops=0`, and no crash was observed.
- Resource result: multiple shader-resource buffers in palette-like sizes were
  sampled and dumped, including 576, 1,056, 1,392, 1,856, 2,000, and 2,848
  byte resources.

## What this does and does not prove

The run proves that the read-only D3D11 observation path is stable in the
retail process and that candidate palette-shaped resources exist during the
observed scene. It did not produce a draw-correlation sample or a stable
semantic Faith skeleton contract. Native pose writes therefore remain disabled
unless an exact executable/resource/layout/joint-index contract is supplied.
Palette order is not treated as bone order.
