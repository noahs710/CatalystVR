# Faith palette binding contract

The native writer now requires two independent contracts:

1. The title-specific executable/resource/layout contract loaded through
   `MECVR_NATIVE_BONE_MAP`.
2. A live-byte geometry match against the retail
   `Characters/Skeletons/Skeleton_Female` arm chain.

The geometry matcher requires matrices through retail index 118 and reads the
eight verified indices for both arms. It accepts either model-space matrices
(parent-child segment lengths) or local-space matrices (bone translation
lengths), but both arms must have mirrored segment lengths and both sides must
fit the three retail segment lengths under one common scale.

An unmatched upload clears the native adapter and palette target. Therefore a
resource cannot remain writable after it changes from a proven Faith palette to
an unrelated animated buffer. The matcher does not authorize writes on its own;
the existing executable fingerprint, resource identity, layout, freshness,
bounds, and explicit map gates remain mandatory.
