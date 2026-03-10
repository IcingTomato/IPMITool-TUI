#!/usr/bin/env python3
import json
import os
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Tuple

from textual import work
from textual.app import App, ComposeResult
from textual.containers import Container, Horizontal, Vertical
from textual.widgets import Header, Footer, Static, Label, LoadingIndicator, Input, Button, Switch
from textual.reactive import reactive

CONFIG_PATH = Path(".") / ".config" / "ipmi_tui" / "config.json"

@dataclass
class AppState:
    mode: str = "inband"
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
    return str(CONFIG_PATH)

def load_config() -> AppState:
    state = AppState()
    if not CONFIG_PATH.exists(): return state
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
    except Exception: pass
    return state

def run_cmd(cmd: List[str], timeout: int = 6) -> Tuple[bool, str]:
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)
        if proc.returncode == 0: return True, (proc.stdout or "").strip()
        return False, (proc.stderr or proc.stdout or "").strip()
    except Exception as exc: return False, str(exc)

def build_base_cmd(state: AppState) -> List[str]:
    if state.mode == "inband": return ["ipmitool"]
    return ["ipmitool", "-I", "lanplus", "-H", state.host.strip(), "-U", state.username.strip(), "-P", state.password]

def fetch_ipmi(state: AppState) -> Dict:
    out = {"power": "N/A", "bmc": "N/A", "temp": "N/A", "fan": "N/A", "sel_lines": [], "last_error": "", "raw_temp": 0.0, "raw_fan": 0.0}
    if shutil.which("ipmitool") is None:
        out["last_error"] = "ipmitool not found in PATH"
        return out
    if state.mode == "oob" and (not state.host.strip() or not state.username.strip()):
        out["last_error"] = "OOB requires IP/User/Pass"
        return out

    base = build_base_cmd(state)
    ok, power = run_cmd(base + ["chassis", "power", "status"])
    if ok: out["power"] = power
    else: out["last_error"] = power

    _, mc = run_cmd(base + ["mc", "info"])
    for line in mc.splitlines():
        if line.lower().startswith("firmware revision"):
            out["bmc"] = line.split(":", 1)[-1].strip()
            break
            
    ok, sensor = run_cmd(base + ["sensor"])
    if ok:
        for line in sensor.splitlines():
            low = line.lower()
            if "|" not in line: continue
            parts = [x.strip() for x in line.split("|")]
            if len(parts) >= 2:
                if "temp" in low and ("degrees c" in low or "celsius" in low) and out["temp"] == "N/A":
                    out["temp"] = f"{parts[0]}={parts[1]}"
                    try: out["raw_temp"] = float(parts[1])
                    except: pass
                if "fan" in low and "rpm" in low and out["fan"] == "N/A":
                    out["fan"] = f"{parts[0]}={parts[1]}"
                    try: out["raw_fan"] = float(parts[1])
                    except: pass
    elif not out["last_error"]: out["last_error"] = sensor

    ok, sel = run_cmd(base + ["sel", "list"])
    if ok: out["sel_lines"] = [line.strip() for line in sel.splitlines() if line.strip()][-5:]

    return out

class BrailleChart(Static):
    history: reactive[list[float]] = reactive(list)
    chart_color: reactive[str] = reactive("green")

    def __init__(self, color="green", **kwargs):
        super().__init__(**kwargs)
        self.chart_color = color
        self.history = [0.0] * 40
        
    def add_point(self, val: float):
        lst = list(self.history)
        lst.append(val)
        self.history = lst[-40:]
        
    def render(self) -> str:
        if not self.history or max(self.history) == 0:
            return "[dim]No Data[/dim]"
        mx, mn = max(self.history), min(self.history)
        if mx == mn: mx += 1
        
        # Simplified block-based chart for fallback since rigorous braille is verbose
        blocks = [" ", "▂", "▃", "▄", "▅", "▆", "▇", "█"]
        res = ""
        for val in self.history:
            idx = int((val - mn) / (mx - mn) * 7)
            idx = max(0, min(7, idx))
            res += blocks[idx]
        return f"[{self.chart_color}]{res}[/]\nMin:{mn:.1f} Max:{mx:.1f}"

class IpmiTuiApp(App):
    CSS = """
    Screen { align: center middle; }
    #startup { color: auto; text-align: center; }
    #main_layout { display: none; height: 100%; border: panel $accent; padding: 1; }
    .box { border: round $primary; padding: 1; }
    BrailleChart { height: 4; margin: 1; }
    """
    
    app_state = load_config()

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        yield Label("🚀 \nB U I L D I N G   C O N N E C T I O N\nIPMI TUI", id="startup")
        with Horizontal(id="main_layout"):
            with Vertical(classes="box"):
                yield Label("[b]Device Info[/b]")
                yield Label(id="l_power")
                yield Label(id="l_bmc")
                yield Label(id="l_temp")
                yield Label(id="l_fan")
                yield Label("\n[b]Temp History[/b]")
                yield BrailleChart(color="red", id="temp_chart")
                yield Label("\n[b]Fan History[/b]")
                yield BrailleChart(color="cyan", id="fan_chart")
                
            with Vertical(classes="box"):
                yield Label("[b]SEL Logs[/b]")
                yield Label(id="l_sel", classes="box")
                yield Label("[b]Status[/b]")
                yield Label(id="l_status", classes="box")
        yield Footer()

    def on_mount(self) -> None:
        self.set_timer(1.5, self.hide_startup)

    def hide_startup(self):
        self.query_one("#startup").display = False
        self.query_one("#main_layout").display = "block"
        self.update_data()
        self.set_interval(self.app_state.refresh_interval, self.update_data)

    @work(exclusive=True, thread=True)
    def update_data(self):
        res = fetch_ipmi(self.app_state)
        self.call_from_thread(self._update_ui, res)

    def _update_ui(self, res: dict):
        self.query_one("#l_power", Label).update(f"Power: {res['power']}")
        self.query_one("#l_bmc", Label).update(f"BMC FW: {res['bmc']}")
        self.query_one("#l_temp", Label).update(f"Temp: {res['temp']}")
        self.query_one("#l_fan", Label).update(f"Fan: {res['fan']}")
        self.query_one("#temp_chart", BrailleChart).add_point(res['raw_temp'])
        self.query_one("#fan_chart", BrailleChart).add_point(res['raw_fan'])
        self.query_one("#l_sel", Label).update("\n".join(res['sel_lines']))
        
        status = res["last_error"] or f"Last Refresh: {time.strftime('%H:%M:%S')}"
        self.query_one("#l_status", Label).update(status)
        
if __name__ == "__main__":
    app = IpmiTuiApp()
    app.run()
