#!/usr/bin/env python3
"""Summarize Balboa bridge diagnostic logs into actionable findings."""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Pattern


@dataclass(frozen=True)
class MatchRule:
    key: str
    regex: Pattern[str]


RULES: tuple[MatchRule, ...] = (
    MatchRule("cts", re.compile(r"\[BridgeDiag\]: cts ")),
    MatchRule("rs485_sent", re.compile(r"\[BridgeDiag\]: rs485_sent ")),
    MatchRule("invalid_length", re.compile(r"\[rs485\]: Invalid message, corrupted length:")),
    MatchRule("ha_discovery_minimal", re.compile(r"\[HA discovery\]: minimal MQTT discovery")),
    MatchRule("ha_discovery_publish_failed", re.compile(r"\[HA discovery\]: publish failed for ")),
    MatchRule("ha_handshake1_notfound", re.compile(r"POST /app/handshake1")),
)

FRAME_REGEX = re.compile(r"frame=([0-9a-fA-F ]+)")
INVALID_FRAME_REGEX = re.compile(r"corrupted length:\s*([0-9a-fA-F ]+)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze firmware log text for RS485 and MQTT/HA diagnostic signals."
    )
    parser.add_argument("--in", dest="infile", type=Path, required=True, help="Path to input log text")
    parser.add_argument("--out", dest="outfile", type=Path, help="Optional JSON output path")
    return parser.parse_args()


def normalized_line(line: str) -> str:
    return line.strip()


def summarize(lines: list[str]) -> dict:
    counts: Counter[str] = Counter()
    sent_frames: Counter[str] = Counter()
    bad_frames: Counter[str] = Counter()
    repeated_lines: Counter[str] = Counter()

    for raw_line in lines:
        line = normalized_line(raw_line)
        if not line:
            continue
        repeated_lines[line] += 1
        for rule in RULES:
            if rule.regex.search(line):
                counts[rule.key] += 1
        sent_match = FRAME_REGEX.search(line)
        if sent_match:
            sent_frames[" ".join(sent_match.group(1).split()).lower()] += 1
        invalid_match = INVALID_FRAME_REGEX.search(line)
        if invalid_match:
            bad_frames[" ".join(invalid_match.group(1).split()).lower()] += 1

    duplicate_line_count = sum(v - 1 for v in repeated_lines.values() if v > 1)
    top_duplicate_lines = [
        {"line": line, "count": count}
        for line, count in repeated_lines.items()
        if count > 1
    ]
    top_duplicate_lines.sort(key=lambda item: item["count"], reverse=True)

    findings: list[str] = []
    if counts["rs485_sent"] > 0:
        findings.append("Outbound RS485 command frames were sent by firmware.")
    if counts["invalid_length"] > 0:
        findings.append("Inbound RS485 parser reported corrupted-length frames.")
    if counts["ha_discovery_publish_failed"] > 0:
        findings.append("Home Assistant discovery publish failures were present.")
    if counts["ha_discovery_minimal"] > 1:
        findings.append("Repeated minimal discovery indicates reconnect/retry churn.")
    if counts["ha_handshake1_notfound"] > 0:
        findings.append("HA attempted legacy /app/handshake1 path (likely expected noise).")
    if duplicate_line_count > 0:
        findings.append("Input includes duplicated log lines/blocks; de-dup when possible.")

    return {
        "counts": dict(counts),
        "uniqueSentFrames": sent_frames.most_common(),
        "uniqueInvalidFrames": bad_frames.most_common(),
        "duplicateLineCount": duplicate_line_count,
        "topDuplicateLines": top_duplicate_lines[:10],
        "findings": findings,
        "nextSteps": [
            "Run a short capture with MQTT discovery temporarily disabled/reduced to isolate RS485.",
            "Check RS485 electrical/timing path (ground reference, termination/bias, adapter direction control).",
            "Capture one known-good status frame near an invalid frame to compare byte alignment drift.",
            "Stabilize MQTT broker connectivity before judging HA discovery behavior.",
        ],
    }


def main() -> int:
    args = parse_args()
    content = args.infile.read_text(encoding="utf-8")
    data = summarize(content.splitlines())
    rendered = json.dumps(data, indent=2)
    print(rendered)
    if args.outfile:
        args.outfile.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
