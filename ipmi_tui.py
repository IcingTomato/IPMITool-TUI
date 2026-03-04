#!/usr/bin/env python3
import curses
import json
import os
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple


CONFIG_PATH = Path(".") / ".config" / "ipmi_tui" / "config.json"


@dataclass
class AppState:
    mode: str = "inband"  # inband | oob
    host: str = ""
    username: str = ""
    password: str = ""
    remember_cred: bool = False
    refresh_interval: int = 3


def ensure_config_dir() -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)


def save_config(state: AppState) -> str:
    ensure_config_dir()
    payload = {
        "mode": state.mode,
        "host": state.host,
        "refresh_interval": state.refresh_interval,
        "remember_cred": state.remember_cred,
    }
    if state.remember_cred:
        payload["username"] = state.username
        payload["password"] = state.password

    with CONFIG_PATH.open("w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=True, indent=2)
    os.chmod(CONFIG_PATH, 0o600)
    return f"Saved config to {CONFIG_PATH}"


def load_config() -> AppState:
    state = AppState()
    if not CONFIG_PATH.exists():
        return state

    try:
        with CONFIG_PATH.open("r", encoding="utf-8") as f:
            payload = json.load(f)
        state.mode = payload.get("mode", state.mode)
        state.host = payload.get("host", state.host)
        state.refresh_interval = int(payload.get("refresh_interval", state.refresh_interval))
        state.remember_cred = bool(payload.get("remember_cred", False))
        if state.remember_cred:
            state.username = payload.get("username", "")
            state.password = payload.get("password", "")
    except Exception:
        pass
    return state


def run_cmd(cmd: List[str], timeout: int = 6) -> Tuple[bool, str]:
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)
        if proc.returncode == 0:
            return True, (proc.stdout or "").strip()
        stderr = (proc.stderr or "").strip()
        stdout = (proc.stdout or "").strip()
        return False, stderr or stdout or f"Command failed: {' '.join(cmd)}"
    except Exception as exc:
        return False, str(exc)


def build_base_cmd(state: AppState) -> List[str]:
    if state.mode == "inband":
        return ["ipmitool"]
    return [
        "ipmitool",
        "-I",
        "lanplus",
        "-H",
        state.host.strip(),
        "-U",
        state.username.strip(),
        "-P",
        state.password,
    ]


def fetch_ipmi(state: AppState) -> Dict[str, str]:
    out: Dict[str, str] = {
        "power": "N/A",
        "bmc": "N/A",
        "temp": "N/A",
        "fan": "N/A",
        "sel_error": "",
        "sel_lines": [],
        "last_error": "",
    }
    if shutil.which("ipmitool") is None:
        out["last_error"] = "ipmitool not found in PATH"
        return out

    if state.mode == "oob":
        if not state.host.strip():
            out["last_error"] = "Out-of-band mode requires IP/FQDN"
            return out
        if not state.username.strip() or not state.password:
            out["last_error"] = "Out-of-band mode requires username/password"
            return out

    base = build_base_cmd(state)
    ok, power = run_cmd(base + ["chassis", "power", "status"])
    if ok:
        out["power"] = power
    else:
        out["last_error"] = power

    ok, mc_info = run_cmd(base + ["mc", "info"])
    if ok:
        for line in mc_info.splitlines():
            if line.lower().startswith("firmware revision"):
                out["bmc"] = line.split(":", 1)[-1].strip()
                break
        if out["bmc"] == "N/A":
            out["bmc"] = "reachable"
    elif not out["last_error"]:
        out["last_error"] = mc_info

    ok, sensor = run_cmd(base + ["sensor"])
    if ok:
        temps = []
        fans = []
        for line in sensor.splitlines():
            low = line.lower()
            if "|" not in line:
                continue
            if "temp" in low and ("degrees c" in low or "celsius" in low):
                parts = [x.strip() for x in line.split("|")]
                if len(parts) >= 2:
                    temps.append(f"{parts[0]}={parts[1]}")
            if "fan" in low and "rpm" in low:
                parts = [x.strip() for x in line.split("|")]
                if len(parts) >= 2:
                    fans.append(f"{parts[0]}={parts[1]}")
        if temps:
            out["temp"] = ", ".join(temps[:3])
        if fans:
            out["fan"] = ", ".join(fans[:3])
    elif not out["last_error"]:
        out["last_error"] = sensor

    ok, sel = run_cmd(base + ["sel", "list"])
    if ok:
        lines = [line.strip() for line in sel.splitlines() if line.strip()]
        out["sel_lines"] = lines[-8:] if lines else ["(No SEL entries)"]
    else:
        out["sel_error"] = sel

    return out


def draw_box(stdscr, y: int, x: int, h: int, w: int, title: str = "") -> None:
    safe_addstr(stdscr, y, x, "+" + "-" * (w - 2) + "+")
    for row in range(y + 1, y + h - 1):
        safe_addstr(stdscr, row, x, "|")
        safe_addstr(stdscr, row, x + w - 1, "|")
    safe_addstr(stdscr, y + h - 1, x, "+" + "-" * (w - 2) + "+")
    if title:
        label = f" {title} "
        pos = x + 2
        if pos + len(label) < x + w - 1:
            safe_addstr(stdscr, y, pos, label)


def safe_addstr(stdscr, y: int, x: int, text: str, attr: int = 0) -> None:
    max_y, max_x = stdscr.getmaxyx()
    if y < 0 or x < 0 or y >= max_y or x >= max_x:
        return
    # Avoid writing to bottom-right cell; curses may raise ERR on that cell.
    max_len = max_x - x
    if y == max_y - 1:
        max_len -= 1
    if max_len <= 0:
        return
    out = text[:max_len]
    try:
        if attr:
            stdscr.addstr(y, x, out, attr)
        else:
            stdscr.addstr(y, x, out)
    except curses.error:
        return


def prompt_input(stdscr, title: str, current: str, secret: bool = False) -> str:
    h, w = stdscr.getmaxyx()
    box_w = min(80, w - 4)
    box_h = 5
    y = (h - box_h) // 2
    x = (w - box_w) // 2
    draw_box(stdscr, y, x, box_h, box_w, title)
    safe_addstr(stdscr, y + 2, x + 2, "> " + ("*" * len(current) if secret else current))
    curses.echo(False)
    stdscr.refresh()

    value = current
    while True:
        ch = stdscr.getch()
        if ch in (10, 13):
            return value
        if ch in (27,):
            return current
        if ch in (curses.KEY_BACKSPACE, 127, 8):
            value = value[:-1]
        elif 32 <= ch <= 126:
            value += chr(ch)
        safe_addstr(stdscr, y + 2, x + 4, " " * (box_w - 6))
        safe_addstr(stdscr, y + 2, x + 4, "*" * len(value) if secret else value)
        stdscr.refresh()


def clip(text: str, width: int) -> str:
    if width <= 0:
        return ""
    if len(text) <= width:
        return text
    return text[: max(0, width - 3)] + "..."


def render(stdscr, state: AppState, ipmi_info: Dict[str, str], status: str) -> None:
    stdscr.erase()
    h, w = stdscr.getmaxyx()
    if h < 22 or w < 90:
        safe_addstr(stdscr, 0, 0, f"Terminal too small: need >= 90x22, got {w}x{h}")
        stdscr.refresh()
        return

    draw_box(stdscr, 0, 0, 3, w, "IPMI TUI Dashboard")
    head = (
        f"Mode={state.mode} | Refresh={state.refresh_interval}s | "
        f"RememberCred={'ON' if state.remember_cred else 'OFF'}"
    )
    safe_addstr(stdscr, 1, 2, clip(head, w - 4))

    left_w = 44
    draw_box(stdscr, 3, 0, h - 8, left_w, "Connection")
    fields = [
        f"[M] Mode: {state.mode}",
        f"[H] Host/IP/FQDN: {state.host or '(empty)'}",
        f"[U] Username: {state.username or '(empty)'}",
        f"[P] Password: {'*' * len(state.password) if state.password else '(empty)'}",
        f"[R] Remember creds: {'Yes' if state.remember_cred else 'No'}",
        f"[I] Refresh interval: {state.refresh_interval}s",
        "[S] Save config now",
        "[F] Force refresh now",
    ]
    for idx, line in enumerate(fields):
        safe_addstr(stdscr, 5 + idx * 2, 2, clip(line, left_w - 4))

    right_x = left_w
    right_w = w - right_x
    top_h = (h - 8) // 2
    draw_box(stdscr, 3, right_x, top_h, right_w, "SEL List")
    sel_lines = ipmi_info.get("sel_lines", [])
    if not sel_lines:
        sel_lines = ["(No SEL data)"]
    max_rows = max(1, top_h - 3)
    for i, line in enumerate(sel_lines[-max_rows:]):
        safe_addstr(stdscr, 5 + i, right_x + 2, clip(str(line), right_w - 4))
    sel_err = ipmi_info.get("sel_error", "").strip()
    if sel_err and top_h >= 4:
        safe_addstr(stdscr, 3 + top_h - 2, right_x + 2, clip(f"SEL error: {sel_err}", right_w - 4), curses.A_BOLD)

    y2 = 3 + top_h
    h2 = h - 8 - top_h
    draw_box(stdscr, y2, right_x, h2, right_w, "IPMI")
    ipmi_lines = [
        f"Power   : {ipmi_info.get('power', 'N/A')}",
        f"BMC FW  : {ipmi_info.get('bmc', 'N/A')}",
        f"Temp    : {ipmi_info.get('temp', 'N/A')}",
        f"Fan     : {ipmi_info.get('fan', 'N/A')}",
    ]
    for i, line in enumerate(ipmi_lines):
        safe_addstr(stdscr, y2 + 2 + i * 2, right_x + 2, clip(line, right_w - 4))

    err = ipmi_info.get("last_error", "").strip()
    if err:
        safe_addstr(stdscr, y2 + h2 - 2, right_x + 2, clip(f"Error: {err}", right_w - 4), curses.A_BOLD)

    draw_box(stdscr, h - 5, 0, 5, w, "Keys")
    keyline = "[M/H/U/P/R/I] Edit  [F] Refresh  [S] Save  [Q] Quit"
    safe_addstr(stdscr, h - 3, 2, clip(keyline, w - 4))
    safe_addstr(stdscr, h - 2, 2, clip(f"Status: {status}", w - 4))
    stdscr.refresh()


def clamp_refresh(value: int) -> int:
    return max(1, min(300, value))


def run(stdscr) -> None:
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.timeout(200)
    state = load_config()
    last_refresh = 0.0
    ipmi_info = {
        "power": "N/A",
        "bmc": "N/A",
        "temp": "N/A",
        "fan": "N/A",
        "sel_error": "",
        "sel_lines": [],
        "last_error": "",
    }
    status = "Ready"

    while True:
        now = time.time()
        if now - last_refresh >= state.refresh_interval:
            ipmi_info = fetch_ipmi(state)
            last_refresh = now
            status = f"Refreshed at {time.strftime('%H:%M:%S')}"

        render(stdscr, state, ipmi_info, status)
        ch = stdscr.getch()
        if ch == -1:
            continue

        c = chr(ch).lower() if 0 <= ch < 256 else ""
        if c == "q":
            break
        if c == "m":
            state.mode = "oob" if state.mode == "inband" else "inband"
            status = f"Mode changed to {state.mode}"
        elif c == "h":
            state.host = prompt_input(stdscr, "Host/IP/FQDN (Enter=OK Esc=Cancel)", state.host, secret=False).strip()
            status = "Host updated"
        elif c == "u":
            state.username = prompt_input(stdscr, "Username (Enter=OK Esc=Cancel)", state.username, secret=False).strip()
            status = "Username updated"
        elif c == "p":
            state.password = prompt_input(stdscr, "Password (Enter=OK Esc=Cancel)", state.password, secret=True)
            status = "Password updated"
        elif c == "r":
            state.remember_cred = not state.remember_cred
            status = f"Remember creds {'enabled' if state.remember_cred else 'disabled'}"
        elif c == "i":
            raw = prompt_input(
                stdscr,
                "Refresh interval seconds 1-300 (Enter=OK Esc=Cancel)",
                str(state.refresh_interval),
                secret=False,
            ).strip()
            try:
                state.refresh_interval = clamp_refresh(int(raw))
                status = f"Refresh interval set to {state.refresh_interval}s"
            except ValueError:
                status = "Invalid refresh interval"
        elif c == "f":
            ipmi_info = fetch_ipmi(state)
            last_refresh = time.time()
            status = "Manual refresh completed"
        elif c == "s":
            status = save_config(state)

    if state.remember_cred:
        save_config(state)


def main() -> None:
    curses.wrapper(run)


if __name__ == "__main__":
    main()
