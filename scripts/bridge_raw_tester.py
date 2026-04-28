#!/usr/bin/env python3
"""Bridge-first raw command harness for Balboa frame troubleshooting.

Sends one or more hex frames to the firmware TCP bridge (default port 4257)
with controlled retries/cooldowns and emits structured JSON results.
"""

from __future__ import annotations

import argparse
import json
import socket
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class TestCase:
    label: str
    frame_hex: str
    retries: int
    cooldown_ms: int
    timeout_ms: int
    expect_hex: str | None = None


def normalize_hex(hex_text: str) -> str:
    cleaned = "".join(ch for ch in hex_text if ch not in " \t\r\n")
    if cleaned.lower().startswith("0x"):
        cleaned = cleaned[2:]
    if len(cleaned) % 2 != 0:
        raise ValueError(f"hex string must contain an even number of chars: {hex_text}")
    # Validate by round-tripping to bytes
    bytes.fromhex(cleaned)
    return cleaned.lower()


def to_spaced_hex(payload: bytes) -> str:
    return " ".join(f"{b:02x}" for b in payload)


def parse_case(raw: dict[str, Any], defaults: dict[str, int]) -> TestCase:
    return TestCase(
        label=str(raw.get("label", "unnamed")),
        frame_hex=normalize_hex(str(raw["frame_hex"])),
        retries=max(1, int(raw.get("retries", defaults["retries"]))),
        cooldown_ms=max(0, int(raw.get("cooldown_ms", defaults["cooldown_ms"]))),
        timeout_ms=max(1, int(raw.get("timeout_ms", defaults["timeout_ms"]))),
        expect_hex=normalize_hex(str(raw["expect_hex"])) if raw.get("expect_hex") else None,
    )


def load_matrix(path: Path, defaults: dict[str, int]) -> list[TestCase]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or "cases" not in data:
        raise ValueError("matrix JSON must be an object with 'cases' list")
    raw_cases = data["cases"]
    if not isinstance(raw_cases, list) or not raw_cases:
        raise ValueError("matrix JSON 'cases' must be a non-empty list")
    return [parse_case(raw, defaults) for raw in raw_cases]


def run_attempt(host: str, port: int, case: TestCase) -> dict[str, Any]:
    frame_bytes = bytes.fromhex(case.frame_hex)
    started_ms = int(time.time() * 1000)
    result: dict[str, Any] = {
        "startedMs": started_ms,
        "sentHex": to_spaced_hex(frame_bytes),
        "rxHex": "",
        "ok": False,
        "error": None,
        "expectMatched": None,
        "durationMs": 0,
    }

    started_perf = time.perf_counter()
    try:
        with socket.create_connection((host, port), timeout=case.timeout_ms / 1000.0) as sock:
            sock.settimeout(case.timeout_ms / 1000.0)
            sock.sendall(frame_bytes)
            try:
                rx = sock.recv(512)
                result["rxHex"] = to_spaced_hex(rx)
            except TimeoutError:
                result["rxHex"] = ""
        result["ok"] = True
        if case.expect_hex is not None:
            result["expectMatched"] = case.expect_hex in result["rxHex"].replace(" ", "")
    except OSError as exc:
        result["error"] = str(exc)
        result["ok"] = False
    finally:
        result["durationMs"] = int((time.perf_counter() - started_perf) * 1000)
    return result


def run_case(host: str, port: int, case: TestCase, dry_run: bool) -> dict[str, Any]:
    case_result: dict[str, Any] = {
        "label": case.label,
        "frameHex": to_spaced_hex(bytes.fromhex(case.frame_hex)),
        "retries": case.retries,
        "cooldownMs": case.cooldown_ms,
        "timeoutMs": case.timeout_ms,
        "expectHex": case.expect_hex,
        "attempts": [],
    }
    for attempt_index in range(case.retries):
        if dry_run:
            attempt = {
                "startedMs": int(time.time() * 1000),
                "sentHex": to_spaced_hex(bytes.fromhex(case.frame_hex)),
                "rxHex": "",
                "ok": True,
                "error": None,
                "expectMatched": None,
                "durationMs": 0,
                "dryRun": True,
            }
        else:
            attempt = run_attempt(host, port, case)
        attempt["attempt"] = attempt_index + 1
        case_result["attempts"].append(attempt)
        if attempt_index < case.retries - 1 and case.cooldown_ms > 0:
            time.sleep(case.cooldown_ms / 1000.0)
    return case_result


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Send raw Balboa frames through bridge TCP port.")
    parser.add_argument("--host", required=True, help="Bridge host/IP")
    parser.add_argument("--port", type=int, default=4257, help="Bridge TCP port (default: 4257)")
    parser.add_argument("--label", default="single", help="Label for single-frame run")
    parser.add_argument("--frame-hex", help="Single frame bytes in hex format")
    parser.add_argument("--expect-hex", help="Optional expected hex fragment in response")
    parser.add_argument("--retries", type=int, default=1, help="Attempts per case")
    parser.add_argument("--cooldown-ms", type=int, default=1200, help="Delay between attempts")
    parser.add_argument("--timeout-ms", type=int, default=1200, help="Socket timeout per attempt")
    parser.add_argument("--matrix", type=Path, help="JSON file with a list of cases")
    parser.add_argument("--out", type=Path, help="Optional output JSON file")
    parser.add_argument("--dry-run", action="store_true", help="Validate and simulate without network I/O")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    defaults = {
        "retries": args.retries,
        "cooldown_ms": args.cooldown_ms,
        "timeout_ms": args.timeout_ms,
    }

    if args.matrix:
        cases = load_matrix(args.matrix, defaults)
    elif args.frame_hex:
        cases = [
            TestCase(
                label=args.label,
                frame_hex=normalize_hex(args.frame_hex),
                retries=max(1, args.retries),
                cooldown_ms=max(0, args.cooldown_ms),
                timeout_ms=max(1, args.timeout_ms),
                expect_hex=normalize_hex(args.expect_hex) if args.expect_hex else None,
            )
        ]
    else:
        raise SystemExit("Provide --frame-hex for single case or --matrix for multiple cases.")

    run_started_ms = int(time.time() * 1000)
    results = [run_case(args.host, args.port, case, args.dry_run) for case in cases]
    payload = {
        "runStartedMs": run_started_ms,
        "host": args.host,
        "port": args.port,
        "dryRun": args.dry_run,
        "caseCount": len(results),
        "results": results,
    }

    formatted = json.dumps(payload, indent=2)
    print(formatted)
    if args.out:
        args.out.write_text(formatted + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
