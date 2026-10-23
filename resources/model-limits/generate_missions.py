#!/usr/bin/env python3
"""Create real XWA mission-space tests for a large Calamari Cruiser OPT.

Only the two flight groups' start coordinates and the player's initial look-at
waypoint are varied. No position is changed at rendering time.
"""
import argparse
import base64
from pathlib import Path
import struct
import zlib

# Complete v17 test mission: header, two FGs, goals, teams and briefing.
# A full framing template avoids synthesizing the opaque briefing records.
_TEMPLATE = (
    "eNrt3D1Lw0AcwOFLBwc3v0G+grNLweok6qCLW6o3BGsKl9TSTT957YtiSxUEq6n4PEOGPyX8aMuVI0cPQifMZeHdaSpjdTeYrIzCeRw1qRisjsJJFR/WXjRzXd1Xw3G1NuulYpz3yropqtuYnxX9AAAAAAAAAAAA/9DykM63js9cDopJTPlFv47pMabFbDoNoTO/bLc1W+Yuzhftvc1C9+u32F+5fv52bEyfstY/ol382mjSpEmTJk2aNAHt6c72RM9Hf2tduLmKdZMfp1FZv+4df2z3eLixewy/sXv0K6FJkyZNmjRp0gTsonafdG1rZfvo3waseQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAwA54AZvlKqU="
)
FG_BASE = 2 + 0x23EE
FG_SIZE = 0xE3E
FG_START = 0xD8A
PLAYER_WAYPOINT = 0xDE
METRES_PER_MISSION_COORDINATE = 6.25

SCENARIOS = [
    (f"ZTest_{km:03d}km", (1600, 0, -32767 + 160 * km),
     (0, 0, -32767), f"ZTest {km} km Z separation")
    for km in (150, 180, 220, 260, 300, 350, 400)
]
SCENARIOS += [
    ("ZTest_Z_Limit", (0, 0, 32767), (0, 0, -32767),
     "ZTest Z extrema 409.6 km"),
    ("ZTest_X_Limit", (32767, 0, 0), (-32767, 0, 0),
     "ZTest X extrema 409.6 km"),
    ("ZTest_XZ_Limit", (32767, 0, 32767), (-32767, 0, -32767),
     "ZTest XZ extrema 579.2 km"),
    ("ZTest_XYZ_Limit", (32767, 32767, 32767), (-32767, -32767, -32767),
     "ZTest XYZ extrema 709.4 km"),
]


def make_mission(template, player, cruiser):
    result = bytearray(template)
    for fg_index, position in enumerate((player, cruiser)):
        if any(not -32768 <= value <= 32767 for value in position):
            raise ValueError(f"Mission int16 position exceeded: {position}")
        struct.pack_into("<hhhh", result, FG_BASE + fg_index * FG_SIZE + FG_START,
                         *position, 1)
    # The first order waypoint determines the player's starting orientation.
    struct.pack_into("<hhhh", result, FG_BASE + PLAYER_WAYPOINT, *cruiser, 1)
    assert struct.unpack_from("<hHH", result, 0) == (17, 2, 0)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    template = zlib.decompress(base64.b64decode(_TEMPLATE))
    if len(template) != 53817:
        raise ValueError("Unexpected test mission template")
    # A standalone mission list is read from the packaged resource overlay.
    # Never append to or write into the user's original Combat/mission.lst.
    lines = ["[ZTest Model Limits]"]
    for index, (name, player, cruiser, description) in enumerate(SCENARIOS, 240):
        file_name = name.lower() + ".tie"
        (args.output / file_name).write_bytes(make_mission(template, player, cruiser))
        lines.extend((str(index), file_name, description))
    (args.output / "mission.lst").write_text(
        "\n".join(lines) + "\n", encoding="ascii")
    print(f"Generated {len(SCENARIOS)} mission-space tests in {args.output}")


if __name__ == "__main__":
    main()
