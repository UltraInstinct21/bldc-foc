"""Parse the board's console lines.

Lines that start with '@' are the drive lab's machine protocol (see
firmware/tests/test_08_drive_lab/main.c and docs/ui_plan.md); everything else is
human console output and is passed through unchanged.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any

# '@T {...}', '@RAW {...}', '@w 1 2 3 ...'. The tag may be preceded by stray
# characters (an echoed keystroke before "echo off" took effect).
_TAGGED = re.compile(r"@(H|T|P|R|RAW|RL|C|W|w)(?:\s+(.*))?$")

KINDS = {
    "H": "hello",
    "T": "tel",
    "P": "params",
    "R": "result",
    "RAW": "raw",
    "RL": "rl",
    "C": "csa",
    "W": "wave_hdr",
    "w": "wave_row",
}


@dataclass
class Msg:
    kind: str           # one of KINDS.values() or "console"
    data: Any           # dict for JSON tags, list[int] for wave rows, str for console


def parse_line(line: str) -> Msg:
    line = line.rstrip("\r\n")
    at = line.find("@")
    if at >= 0:
        m = _TAGGED.match(line[at:])
        if m:
            tag, body = m.group(1), (m.group(2) or "").strip()
            kind = KINDS[tag]
            if kind == "wave_row":
                try:
                    return Msg(kind, [int(x) for x in body.split()])
                except ValueError:
                    return Msg("console", line)
            try:
                return Msg(kind, json.loads(body) if body else {})
            except json.JSONDecodeError:
                return Msg("console", line)
    return Msg("console", line)


def wave_from_rows(hdr: dict, rows: list[list[int]]) -> dict:
    """@w rows -> column arrays in engineering units."""
    cols: dict[str, list[float]] = {k: [] for k in
                                    ("ia", "ib", "ic", "da", "db", "dc", "th", "tho", "valid", "nf")}
    for r in rows:
        if len(r) < 10:
            continue
        cols["ia"].append(r[0] / 1000.0)
        cols["ib"].append(r[1] / 1000.0)
        cols["ic"].append(r[2] / 1000.0)
        cols["da"].append(r[3] / 10.0)          # percent
        cols["db"].append(r[4] / 10.0)
        cols["dc"].append(r[5] / 10.0)
        cols["th"].append(r[6] / 10.0)          # degrees
        cols["tho"].append(r[7] / 10.0)
        cols["valid"].append(r[8])
        cols["nf"].append(r[9])
    return {"dt_us": hdr.get("dt_us", 50), "st": hdr.get("st", "?"),
            "frozen": hdr.get("frozen", 0), "n": len(cols["ia"]), **cols}
