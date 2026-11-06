#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Lichii-Android contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the GitHub Actions build matrix from .ci/android/devices.yml.

Builds are deduplicated per build_target (devices sharing a target share the
same APK), so the CI matrix runs once per build_target — not once per device.

Modes:
  default          — emit `targets=<json>` for $GITHUB_OUTPUT (or stdout)
  --markdown       — emit a Markdown device -> APK table (CI step summary)
"""

import json
import os
import sys

try:
    import yaml
except ImportError:
    sys.exit("-- ! PyYAML is required: python3 -m pip install pyyaml")

HERE = os.path.dirname(os.path.abspath(__file__))
DEVICES_FILE = os.path.join(HERE, "devices.yml")


def load():
    with open(DEVICES_FILE, encoding="utf-8") as f:
        return yaml.safe_load(f)


def matrix_include(data):
    """One matrix entry per build_target, with its device list attached."""
    devices_by_target = {}
    for device in data.get("devices", []):
        devices_by_target.setdefault(device["build_target"], []).append(device["id"])

    include = []
    for target in data["build_targets"]:
        devices = devices_by_target.get(target["id"], [])
        include.append(
            {
                "id": target["id"],
                "preset": target["preset"],
                "march": target["march"],
                "mtune": target["mtune"],
                "compat": target["compat"],
                "devices": ", ".join(devices) if devices
                else "(all unlisted devices)",
            }
        )
    return {"include": include}


def markdown_summary(data):
    devices_by_target = {}
    for device in data.get("devices", []):
        devices_by_target.setdefault(device["build_target"], []).append(
            f"`{device['id']}`"
        )

    lines = [
        "## APK per device (matrix generated from `.ci/android/devices.yml`)",
        "",
        "| APK | CMake preset | Flags | Compatibility | Mapped devices |",
        "|---|---|---|---|---|",
    ]
    for target in data["build_targets"]:
        devices = devices_by_target.get(target["id"], [])
        lines.append(
            "| `Lichii-android-{id}.apk` | `{preset}` | `{march} -mtune={mtune}` "
            "| {compat} | {devices} |".format(
                id=target["id"],
                preset=target["preset"],
                march=target["march"],
                mtune=target["mtune"],
                compat=target["compat"],
                devices=", ".join(devices) if devices
                else "(default — all unlisted devices)",
            )
        )
    lines.append("")
    return "\n".join(lines)


def main():
    data = load()
    if "--markdown" in sys.argv:
        sys.stdout.write(markdown_summary(data))
        return

    payload = json.dumps(matrix_include(data), ensure_ascii=False)
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as f:
            f.write(f"targets={payload}\n")
    else:
        sys.stdout.write(f"targets={payload}\n")


if __name__ == "__main__":
    main()
