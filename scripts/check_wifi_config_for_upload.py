"""Refuse firmware upload when src/config.h still has placeholder Wi‑Fi credentials.

OTA/USB upload embeds WIFI_SSID / WIFI_PASSWORD from the local (gitignored) config.h.
Flashing a device with the config-example placeholders (e.g. "xxxxxx") overwrites a
working image and leaves the unit unable to join the LAN — recovery then needs USB.

Compile-only (`pio run`) is unaffected so CI can keep using config-example.h.
Override for intentional bench tests: SPA_ALLOW_PLACEHOLDER_WIFI=1
"""

Import("env")  # noqa: F821 — PlatformIO injects env
import os
import re
import sys

# Match config-example.h and other obvious non-production values.
_PLACEHOLDER_VALUES = {
    "",
    "xxxxxx",
    "your-ssid",
    "your-password",
    "changeme",
    "change-me",
    "ssid",
    "password",
    "wifi",
    "hotspot",
}


def _config_paths(project_dir):
    return [
        os.path.join(project_dir, "src", "config.h"),
        os.path.join(project_dir, "config.h"),
    ]


def _read_define(path, name):
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as exc:
        print("ERROR: cannot read %s: %s" % (path, exc))
        sys.exit(1)
    # Last #define wins (allows #undef + redefine patterns).
    matches = re.findall(
        r'^\s*#\s*define\s+%s\s+"([^"]*)"' % re.escape(name),
        text,
        flags=re.MULTILINE,
    )
    if not matches:
        return None
    return matches[-1]


def _is_placeholder(value):
    if value is None:
        return True
    normalized = value.strip().lower()
    return normalized in _PLACEHOLDER_VALUES


def check_wifi_before_upload(source, target, env):
    if os.environ.get("SPA_ALLOW_PLACEHOLDER_WIFI", "").strip() in ("1", "true", "yes"):
        print("WARNING: SPA_ALLOW_PLACEHOLDER_WIFI set — skipping Wi‑Fi credential upload check")
        return

    project_dir = env["PROJECT_DIR"]
    config_path = None
    ssid = None
    password = None
    for path in _config_paths(project_dir):
        if os.path.isfile(path):
            config_path = path
            ssid = _read_define(path, "WIFI_SSID")
            password = _read_define(path, "WIFI_PASSWORD")
            break

    if config_path is None:
        print("ERROR: src/config.h not found — copy from src/config-example.h and set real Wi‑Fi")
        print("        before USB/OTA upload (compile-only builds may still use the example via CI).")
        sys.exit(1)

    bad = []
    if _is_placeholder(ssid):
        bad.append("WIFI_SSID")
    if _is_placeholder(password):
        bad.append("WIFI_PASSWORD")

    if not bad:
        # Do not print the password. SSID is useful for operator confirmation.
        print("Wi‑Fi upload check OK: WIFI_SSID=%r from %s" % (ssid, config_path))
        return

    print("ERROR: refusing upload — placeholder Wi‑Fi credentials in %s" % config_path)
    print("       Bad fields: %s" % ", ".join(bad))
    print("       Set real WIFI_SSID / WIFI_PASSWORD in src/config.h (gitignored), then retry.")
    print("       Flashing placeholders overwrites a working device and it will not join Wi‑Fi.")
    print("       Escape hatch (bench only): SPA_ALLOW_PLACEHOLDER_WIFI=1")
    sys.exit(1)


# Runs for firmware upload (USB esptool and espota). Does not run on compile-only.
env.AddPreAction("upload", check_wifi_before_upload)
