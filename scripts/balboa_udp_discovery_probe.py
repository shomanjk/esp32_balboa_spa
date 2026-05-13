#!/usr/bin/env python3
"""
Send Balboa-style UDP discovery to port 30303 and print replies.

This matches the behavior documented in lib/findSpa/findSpa.cpp: broadcast
"Discovery: Who is out there?" on UDP 30303. The tub gateway (LOCAL_CONNECT)
should respond with ASCII lines starting with BWGSPA and a 00-15-27-… MAC.

Examples:
  python3 scripts/balboa_udp_discovery_probe.py
  python3 scripts/balboa_udp_discovery_probe.py --target 192.168.12.54
  python3 scripts/balboa_udp_discovery_probe.py --target spa-142B2FA1127C.local
"""

from __future__ import annotations

import argparse
import socket
import sys


def main() -> int:
    p = argparse.ArgumentParser(description="UDP discovery probe for Balboa-style spa gateways")
    p.add_argument(
        "--target",
        default="255.255.255.255",
        help="IPv4, broadcast, or hostname (default: 255.255.255.255)",
    )
    p.add_argument("--port", type=int, default=30303, help="UDP port (default: 30303)")
    p.add_argument(
        "--message",
        default="Discovery: Who is out there?",
        help="UDP payload (default: Balboa-style discovery string)",
    )
    p.add_argument("--timeout", type=float, default=4.0, help="Seconds to wait for replies")
    args = p.parse_args()

    msg = args.message.encode("utf-8")
    host = args.target

    if host != "255.255.255.255" and not host.replace(".", "").isdigit():
        try:
            host = socket.gethostbyname(host)
            print(f"Resolved to {host}", file=sys.stderr)
        except socket.gaierror as e:
            print(f"Could not resolve {args.target!r}: {e}", file=sys.stderr)
            return 2

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if host == "255.255.255.255":
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        except OSError as e:
            print(f"SO_BROADCAST failed: {e}", file=sys.stderr)
            return 1
    s.bind(("0.0.0.0", 0))
    s.settimeout(args.timeout)

    try:
        s.sendto(msg, (host, args.port))
        print(f"Sent {len(msg)} bytes to {host}:{args.port} (local UDP port {s.getsockname()[1]})")
    except OSError as e:
        print(f"sendto failed: {e}", file=sys.stderr)
        print(
            "Hint: broadcast needs a routable IPv4 Wi‑Fi/Ethernet interface on this machine.",
            file=sys.stderr,
        )
        return 1

    got = 0
    while True:
        try:
            data, addr = s.recvfrom(4096)
        except socket.timeout:
            break
        got += 1
        print(f"\nReply #{got} from {addr[0]}:{addr[1]} ({len(data)} bytes):")
        try:
            text = data.decode("utf-8", errors="replace")
            print(text)
        except Exception:
            print(repr(data))
        if b"BWGSPA" in data:
            print("--- OK: payload contains BWGSPA (expected gateway reply)")

    s.close()
    if got == 0:
        print("\nNo reply before timeout. Check: same LAN, AP isolation off, gateway has LOCAL_CONNECT, Wi‑Fi up.")
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
