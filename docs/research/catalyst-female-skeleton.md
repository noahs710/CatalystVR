# Catalyst female skeleton contract

Read-only extraction from the installed retail asset
`Characters/Skeletons/Skeleton_Female` produced a self-describing Frostbite
v2 EBX with 169 bones. The raw asset SHA-256 is
`058277d86f1913bba58db8c948d489e8e80638e7b77d1aa026112a2ed5ac1b1a`.

The verified arm chains are:

| Canonical role | Retail bone | Index | Parent |
|---|---|---:|---:|
| Left shoulder/clavicle | `LeftShoulder` | 8 | 7 (`Spine2`) |
| Left upper arm | `LeftArm` | 9 | 8 |
| Left forearm | `LeftForeArm` | 12 | 9 |
| Left hand | `LeftHand` | 15 | 12 |
| Right shoulder/clavicle | `RightShoulder` | 111 | 7 (`Spine2`) |
| Right upper arm | `RightArm` | 112 | 111 |
| Right forearm | `RightForeArm` | 115 | 112 |
| Right hand | `RightHand` | 118 | 115 |

`tools/inspect_catalyst_skeleton.ps1` obtains the raw EBX through Frosty's
read-only asset index. `tools/parse_catalyst_skeleton.py` validates and decodes
the name, hash, hierarchy, local-pose, and model-pose arrays without relying on
Frosty's generated managed object ABI. The compiled identity and model-pose
signature live in `src/ik/faith_skeleton_contract.h`.

These indices identify the skeleton only. They do not by themselves authorize
a GPU palette write. Runtime promotion still requires matching executable,
resource, matrix layout, bone count, rest-length signature, and pose freshness.
