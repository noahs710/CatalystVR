#!/usr/bin/env python3
"""Parse Catalyst's self-describing v2 SkeletonAsset EBX without Frosty ABI.

The companion PowerShell extractor obtains the raw EBX through Frosty's
read-only asset index. This parser intentionally implements only the narrow,
validated SkeletonAsset layout used by Mirror's Edge Catalyst.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


MAGIC_V2 = 0x0FB2D1CE
EXPECTED_BONES = 169


def unpack(data: bytes, offset: int, fmt: str):
    return struct.unpack_from("<" + fmt, data, offset)


def cstring(data: bytes, offset: int) -> str:
    end = data.index(0, offset)
    return data[offset:end].decode("utf-8")


def transform(data: bytes, offset: int) -> dict[str, list[float]]:
    values = unpack(data, offset, "16f")
    return {
        "right": list(values[0:3]),
        "up": list(values[4:7]),
        "forward": list(values[8:11]),
        "translation": list(values[12:15]),
    }


def parse(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 64:
        raise ValueError("Skeleton EBX is truncated")
    header = unpack(data, 0, "4I6H3I")
    (magic, strings_offset, _strings_and_data_len, guid_count,
     instance_count, exported_count, _unique_class_count, class_count,
     field_count, type_names_len, strings_len, array_count, data_len) = header
    if magic != MAGIC_V2:
        raise ValueError(f"Unsupported EBX magic 0x{magic:08x}")
    if guid_count != 0 or instance_count != 1 or exported_count != 1:
        raise ValueError("Unexpected SkeletonAsset instance/import layout")

    # Walk the self-describing tables to the array descriptor table. Entries
    # are 16-byte fields, 16-byte classes, 4-byte instances and 12-byte arrays.
    cursor = struct.calcsize("<4I6H3I") + 16  # fixed header + file GUID
    cursor = (cursor + 15) & ~15
    cursor += guid_count * 32 + type_names_len
    cursor += field_count * 16 + class_count * 16 + instance_count * 4
    cursor = (cursor + 15) & ~15
    arrays = [unpack(data, cursor + i * 12, "IIi")
              for i in range(array_count)]
    arrays_offset = strings_offset + strings_len + data_len

    # The root contains Name followed by ten array indices. Resolve by root
    # references instead of assuming array-table order.
    root_offset = strings_offset + strings_len
    root_values = unpack(data, root_offset + 16, "11I")
    bone_names_index, hashes_index, hierarchy_index, local_index, model_index = (
        root_values[1:6]
    )

    def descriptor(index: int):
        if index >= len(arrays):
            raise ValueError("Skeleton array reference is out of range")
        relative, count, class_ref = arrays[index]
        return arrays_offset + relative, count, class_ref

    names_offset, bone_count, _ = descriptor(bone_names_index)
    hashes_offset, hash_count, _ = descriptor(hashes_index)
    hierarchy_offset, hierarchy_count, _ = descriptor(hierarchy_index)
    local_offset, local_count, _ = descriptor(local_index)
    model_offset, model_count, _ = descriptor(model_index)
    counts = {bone_count, hash_count, hierarchy_count, local_count, model_count}
    if counts != {EXPECTED_BONES}:
        raise ValueError(f"Unexpected skeleton array counts: {sorted(counts)}")

    names = []
    for i in range(bone_count):
        string_relative = unpack(data, names_offset + i * 4, "I")[0]
        names.append(cstring(data, strings_offset + string_relative))
    hashes = list(unpack(data, hashes_offset, f"{bone_count}I"))
    parents = list(unpack(data, hierarchy_offset, f"{bone_count}i"))
    local_pose = [transform(data, local_offset + i * 64)
                  for i in range(bone_count)]
    model_pose = [transform(data, model_offset + i * 64)
                  for i in range(bone_count)]

    required = {
        "LeftShoulder": 8, "LeftArm": 9, "LeftForeArm": 12,
        "LeftHand": 15, "RightShoulder": 111, "RightArm": 112,
        "RightForeArm": 115, "RightHand": 118,
    }
    by_name = {name: index for index, name in enumerate(names)}
    if any(by_name.get(name) != index for name, index in required.items()):
        raise ValueError("Female skeleton arm contract does not match retail data")
    for child, parent in ((9, 8), (12, 9), (15, 12),
                          (112, 111), (115, 112), (118, 115)):
        if parents[child] != parent:
            raise ValueError("Female skeleton arm hierarchy is inconsistent")

    bones = []
    for i, name in enumerate(names):
        bones.append({
            "index": i,
            "name": name,
            "name_hash": f"0x{hashes[i]:08x}",
            "parent": parents[i],
            "local_pose": local_pose[i],
            "model_pose": model_pose[i],
        })
    return {
        "schema": 1,
        "game": "MirrorsEdgeCatalyst",
        "asset": "Characters/Skeletons/Skeleton_Female",
        "source_sha256": hashlib.sha256(data).hexdigest(),
        "bone_count": bone_count,
        "canonical_arm_indices": {
            "left_shoulder": 8,
            "left_elbow": 9,
            "left_wrist": 12,
            "left_hand": 15,
            "right_shoulder": 111,
            "right_elbow": 112,
            "right_wrist": 115,
            "right_hand": 118,
        },
        "bones": bones,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    contract = parse(args.input)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(contract, indent=2) + "\n", encoding="utf-8")
    print(f"Parsed {contract['bone_count']} bones -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
