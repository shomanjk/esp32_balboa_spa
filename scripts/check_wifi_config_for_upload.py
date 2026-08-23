"""Refuse firmware upload when src/config.h still has placeholder Wi‑Fi credentials.

OTA/USB upload embeds WIFI_SSID / WIFI_PASSWORD from the local (gitignored) config.h.
Flashing a device with the config-example placeholders (e.g. "xxxxxx") overwrites a
working image and leaves the unit unable to join the LAN — recovery then needs USB.

Compile-only (`pio run`) is unaffected so CI can keep using config-example.h.
Override for intentional bench tests: SPA_ALLOW_PLACEHOLDER_WIFI=1
"""

try:
    Import("env")  # noqa: F821 — PlatformIO injects env
except NameError:
    env = None  # allow direct python unit tests outside PlatformIO

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

_DEFINE_RE = re.compile(r'^\s*#\s*define\s+(WIFI_SSID|WIFI_PASSWORD)\s+"([^"]*)"')
_UNDEF_RE = re.compile(r"^\s*#\s*undef\s+(WIFI_SSID|WIFI_PASSWORD)\b")
_IF_RE = re.compile(r"^\s*#\s*if(n?def)?\b(.*)$")
_ELSE_RE = re.compile(r"^\s*#\s*else\b")
_ELIF_RE = re.compile(r"^\s*#\s*elif\b(.*)$")
_ENDIF_RE = re.compile(r"^\s*#\s*endif\b")


def _config_paths(project_dir):
    return [
        os.path.join(project_dir, "src", "config.h"),
        os.path.join(project_dir, "config.h"),
    ]


def _strip_line_comment(line):
    in_str = False
    i = 0
    while i < len(line) - 1:
        c = line[i]
        if c == '"' and (i == 0 or line[i - 1] != "\\"):
            in_str = not in_str
        elif not in_str and c == "/" and line[i + 1] == "/":
            return line[:i]
        i += 1
    return line


def _if_condition_state(kind, rest):
    """Return ('active'|'inactive'|'unknown') for a #if / #ifdef / #ifndef / #elif."""
    expr = (rest or "").strip()
    if kind in ("def", "ndef"):
        # Depends on -D / other headers; treat as ambiguous for credential defines.
        return "unknown"
    # Plain #if / #elif: only resolve trivial constants.
    if re.fullmatch(r"0+|false", expr, flags=re.IGNORECASE):
        return "inactive"
    if re.fullmatch(r"1+|true", expr, flags=re.IGNORECASE):
        return "active"
    return "unknown"


def _frame_active(parent_active, branch_state, taken_known_active):
    """Whether the current #if/#else/#elif arm is active for credential scanning."""
    if not parent_active:
        return False
    if branch_state == "inactive":
        return False
    if branch_state == "active":
        return not taken_known_active
    # unknown: still "active" for scanning so we can detect ambiguous defines
    return True


class _IfFrame(object):
    __slots__ = ("parent_active", "branch_state", "taken_known_active", "arm_active")

    def __init__(self, parent_active, branch_state):
        self.parent_active = parent_active
        self.branch_state = branch_state
        self.taken_known_active = branch_state == "active"
        self.arm_active = _frame_active(parent_active, branch_state, False)


def _read_wifi_defines(path):
    """Return (ssid, password) from active regions, or raise SystemExit on ambiguity.

    Skips inactive arms such as #if 0. Rejects WIFI_SSID / WIFI_PASSWORD that appear
    under #if/#ifdef/#ifndef/#elif conditions we cannot resolve without full cpp.
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as exc:
        print("ERROR: cannot read %s: %s" % (path, exc))
        sys.exit(1)

    values = {"WIFI_SSID": None, "WIFI_PASSWORD": None}
    stack = []
    in_block_comment = False
    line_no = 0

    for raw in text.splitlines():
        line_no += 1
        line = raw

        if in_block_comment:
            end = line.find("*/")
            if end < 0:
                continue
            line = line[end + 2 :]
            in_block_comment = False

        while True:
            start = line.find("/*")
            if start < 0:
                break
            end = line.find("*/", start + 2)
            if end < 0:
                line = line[:start]
                in_block_comment = True
                break
            line = line[:start] + " " + line[end + 2 :]

        line = _strip_line_comment(line).rstrip()
        if not line.strip():
            continue

        parent_active = stack[-1].arm_active if stack else True

        m_if = _IF_RE.match(line)
        if m_if:
            kind = m_if.group(1) if m_if.group(1) else "if"
            state = _if_condition_state(kind, m_if.group(2))
            stack.append(_IfFrame(parent_active, state))
            continue

        if _ELSE_RE.match(line):
            if not stack:
                print("ERROR: %s:%d: #else without #if" % (path, line_no))
                sys.exit(1)
            fr = stack[-1]
            # A prior known-true arm (#if 1 / taken #elif) makes #else unreachable.
            if fr.taken_known_active:
                next_state = "inactive"
            elif fr.branch_state == "active":
                next_state = "inactive"
            elif fr.branch_state == "inactive":
                next_state = "active"
            else:
                next_state = "unknown"
            fr.branch_state = next_state
            fr.arm_active = _frame_active(fr.parent_active, next_state, fr.taken_known_active)
            if next_state == "active":
                fr.taken_known_active = True
            continue

        m_elif = _ELIF_RE.match(line)
        if m_elif:
            if not stack:
                print("ERROR: %s:%d: #elif without #if" % (path, line_no))
                sys.exit(1)
            fr = stack[-1]
            state = _if_condition_state("if", m_elif.group(1))
            # Once a known-true arm has been taken, later #elif/#else are inactive even
            # if their condition is unresolved (cpp never evaluates them).
            if fr.taken_known_active:
                state = "inactive"
            elif fr.branch_state == "unknown" or state == "unknown":
                state = "unknown"
            fr.branch_state = state
            fr.arm_active = _frame_active(fr.parent_active, state, False)
            if state == "active":
                fr.taken_known_active = True
            continue

        if _ENDIF_RE.match(line):
            if not stack:
                print("ERROR: %s:%d: #endif without #if" % (path, line_no))
                sys.exit(1)
            stack.pop()
            continue

        region_active = stack[-1].arm_active if stack else True
        region_unknown = any(fr.branch_state == "unknown" and fr.arm_active for fr in stack)

        m_undef = _UNDEF_RE.match(line)
        if m_undef:
            name = m_undef.group(1)
            if region_unknown:
                print(
                    "ERROR: refusing upload — ambiguous %s under unresolved #if/#ifdef in %s:%d"
                    % (name, path, line_no)
                )
                print("       Put WIFI_SSID / WIFI_PASSWORD in unconditional #define lines only.")
                sys.exit(1)
            if region_active:
                values[name] = None
            continue

        m_def = _DEFINE_RE.match(line)
        if m_def:
            name, value = m_def.group(1), m_def.group(2)
            if region_unknown:
                print(
                    "ERROR: refusing upload — ambiguous %s under unresolved #if/#ifdef in %s:%d"
                    % (name, path, line_no)
                )
                print("       Put WIFI_SSID / WIFI_PASSWORD in unconditional #define lines only.")
                print("       (Or use SPA_ALLOW_PLACEHOLDER_WIFI=1 for intentional bench tests.)")
                sys.exit(1)
            if region_active:
                values[name] = value
            continue

    if stack:
        print("ERROR: %s: unclosed #if before EOF" % path)
        sys.exit(1)

    return values["WIFI_SSID"], values["WIFI_PASSWORD"]


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
            ssid, password = _read_wifi_defines(path)
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
if env is not None:
    env.AddPreAction("upload", check_wifi_before_upload)
