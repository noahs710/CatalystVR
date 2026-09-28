#pragma once

#include <array>
#include <cstdint>

namespace mecvr::ik {

struct FaithArmBone {
  const char* name;
  std::uint32_t index;
  std::int32_t parent;
  std::array<double, 3> model_position;
};

inline constexpr char kFaithSkeletonAsset[] =
    "Characters/Skeletons/Skeleton_Female";
inline constexpr char kFaithSkeletonSourceSha256[] =
    "058277d86f1913bba58db8c948d489e8e80638e7b77d1aa026112a2ed5ac1b1a";
inline constexpr std::uint32_t kFaithSkeletonBoneCount = 169;

// Extracted retail skeleton indices and model-pose positions. This is a
// read-only identity contract; live writes still require independent resource
// and matrix-layout proof.
inline constexpr std::array<FaithArmBone, 8> kFaithArmBones{{
    {"LeftShoulder", 8, 7, {0.0297602937, 1.3956729174, 0.0405097380}},
    {"LeftArm", 9, 8, {0.1561537385, 1.3844256401, -0.0025653511}},
    {"LeftForeArm", 12, 9, {0.3516482115, 1.1834721565, 0.0207982510}},
    {"LeftHand", 15, 12, {0.4763822258, 1.0538215637, 0.1784794331}},
    {"RightShoulder", 111, 7, {-0.0297605172, 1.3956727982, 0.0405098200}},
    {"RightArm", 112, 111, {-0.1561535597, 1.3844244480, -0.0025658945}},
    {"RightForeArm", 115, 112, {-0.3516476154, 1.1834706068, 0.0207978021}},
    {"RightHand", 118, 115, {-0.4763813317, 1.0538196564, 0.1784790009}},
}};

}  // namespace mecvr::ik
