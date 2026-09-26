"""Web bridge for the Reflow Oven board.

The board has no network of its own - no Ethernet, and USB device mode is out
because Y1 is unpopulated - so the only live path to it is SWD. This serves a
small web app and relays between it and the firmware's RAM blocks:

    browser  <-- HTTP -->  this script  <-- TCL -->  OpenOCD  <-- SWD -->  board

OpenOCD can read and write target memory through the DAP while the core is
running, so nothing here ever halts the control loop.

Run it through run_webapp.ps1, which starts OpenOCD first.
"""

import argparse
import http.server
import json
import re
import socket
import socketserver
import subprocess
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ELF = HERE.parent / "build" / "thermo_monitor.elf"
NM = Path(
    r"C:\ST\STM32CubeIDE_1.15.0\STM32CubeIDE\plugins"
    r"\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.12.3.rel1.win32_1.0.100.202403111256"
    r"\tools\bin\arm-none-eabi-nm.exe"
)

TCL_HOST, TCL_PORT = "127.0.0.1", 6666
HTTP_PORT = 8770

CMD_MAGIC = 0x434D4431
SRV_MAGIC = 0x53525631

# Clamps applied here as well as in firmware. The browser is the least
# trustworthy part of this chain, so nothing it sends is taken at face value.
SETPOINT_MIN_C, SETPOINT_MAX_C = 0.0, 100.0
KP_MAX = 200.0          # %/C     - above this a 1 C error alone saturates
KI_MAX = 50.0           # %/C/s


class OpenOcd:
    """Minimal client for OpenOCD's TCL RPC port. Commands are terminated
    with 0x1a in both directions."""

    SEP = b"\x1a"

    def __init__(self, host=TCL_HOST, port=TCL_PORT):
        self.addr = (host, port)
        self.sock = None
        self.lock = threading.Lock()

    def connect(self, timeout=30.0):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                s = socket.create_connection(self.addr, timeout=5)
                s.settimeout(5)
                self.sock = s
                return
            except OSError as exc:            # OpenOCD not listening yet
                last = exc
                time.sleep(0.3)
        raise RuntimeError("could not reach OpenOCD's TCL port: %s" % last)

    def command(self, cmd):
        with self.lock:
            if self.sock is None:
                self.connect()
            self.sock.sendall(cmd.encode() + self.SEP)
            buf = b""
            while not buf.endswith(self.SEP):
                chunk = self.sock.recv(4096)
                if not chunk:
                    self.sock = None
                    raise RuntimeError("OpenOCD closed the connection")
                buf += chunk
            return buf[:-1].decode(errors="replace")

    def read_words(self, addr, count):
        out = self.command("read_memory 0x%08x 32 %d" % (addr, count))
        return [int(tok, 0) for tok in out.split()]

    def write_word(self, addr, value):
        self.command("write_memory 0x%08x 32 {%d}" % (addr, value & 0xFFFFFFFF))


def symbol_addresses():
    """Ask nm where the command and telemetry blocks landed, so the layout is
    never duplicated between firmware and host."""
    if not ELF.exists():
        raise SystemExit("%s not found - build with MODE=server first." % ELF)
    out = subprocess.run([str(NM), str(ELF)], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        m = re.match(r"([0-9a-fA-F]{8})\s+\S\s+(\S+)$", line.strip())
        if m:
            syms[m.group(2)] = int(m.group(1), 16)
    for name in ("g_cmd", "g_srv"):
        if name not in syms:
            raise SystemExit(
                "%s not in the ELF - is this a MODE=server build?" % name)
    return syms["g_cmd"], syms["g_srv"]


G_CMD, G_SRV = symbol_addresses()
ocd = OpenOcd()

STATE_NAMES = {
    0: "idle",
    1: "running",
    2: "sensor fault",
    3: "over-temperature (latched)",
    4: "host timeout",
}

# Mirror of the command block. The firmware latches on a seq change, so fields
# are written first and seq last.
cmd_state = {"setpoint_c": 30.0, "enable": 0, "kp": 12.0, "ki": 0.6, "seq": 0}
cmd_lock = threading.Lock()

history = []          # (uptime_s, temp_c, setpoint_c, duty_pct, p, i)
history_lock = threading.Lock()
HISTORY_MAX = 4000

last_error = {"msg": None}

# The dead-man switch has to follow the *browser*, not this process. If the
# bridge kept the heartbeat going on its own, closing the tab would leave the
# heater running under nobody's supervision. So the heartbeat is only relayed
# while a client has talked to the API recently.
CLIENT_TIMEOUT_S = 2.5
last_client = {"t": 0.0}
beat = {"n": 0}


def kick():
    """Bump the firmware's dead-man counter."""
    beat["n"] = (beat["n"] + 1) & 0xFFFFFFFF
    ocd.write_word(G_CMD + 24, beat["n"])


def push_command():
    """Write the whole block, then bump seq so the firmware acts on it.

    The heartbeat goes first. After a dead-man trip the firmware clears its
    own enable flag and will only re-arm on a *new* seq - so if the heartbeat
    were still stale when that seq landed, the command would be latched and
    then immediately cleared again, and nothing would re-arm it. Refreshing
    the heartbeat before the seq makes an explicit Start take effect at once.
    """
    with cmd_lock:
        kick()
        cmd_state["seq"] = (cmd_state["seq"] + 1) & 0xFFFFFFFF
        ocd.write_word(G_CMD + 8, int(round(cmd_state["setpoint_c"] * 1000)))
        ocd.write_word(G_CMD + 12, cmd_state["enable"])
        ocd.write_word(G_CMD + 16, int(round(cmd_state["kp"] * 1000000)))
        ocd.write_word(G_CMD + 20, int(round(cmd_state["ki"] * 1000000)))
        ocd.write_word(G_CMD + 4, cmd_state["seq"])


def poller():
    """Read telemetry and kick the firmware's dead-man timer. If this thread
    dies the heater shuts itself off, which is the intended behaviour."""
    while True:
        try:
            w = ocd.read_words(G_SRV, 12)
            if w[0] == SRV_MAGIC:
                temp = _s32(w[2]) / 1000.0
                sp = _s32(w[3]) / 1000.0
                duty = w[4] / 10.0
                p_term = _s32(w[10]) / 10.0
                i_term = _s32(w[11]) / 10.0
                sample = {
                    "seq": w[1], "temp_c": temp, "setpoint_c": sp,
                    "duty_pct": duty, "fault": w[5], "state": w[6],
                    "state_name": STATE_NAMES.get(w[6], "?"),
                    "uptime_s": w[7] / 1000.0,
                    "kp": _s32(w[8]) / 1e6, "ki": _s32(w[9]) / 1e6,
                    "p_term": p_term, "i_term": i_term,
                }
                with history_lock:
                    if not history or history[-1][0] != sample["uptime_s"]:
                        history.append((sample["uptime_s"], temp, sp, duty,
                                        p_term, i_term))
                        del history[:-HISTORY_MAX]
                    latest.update(sample)
            # Relay the heartbeat only while a client is actually watching.
            if time.time() - last_client["t"] < CLIENT_TIMEOUT_S:
                kick()
            last_error["msg"] = None
        except Exception as exc:                      # keep the bridge alive
            last_error["msg"] = str(exc)
        time.sleep(0.25)


def _s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


latest = {"state_name": "starting", "state": 0, "temp_c": None,
          "setpoint_c": None, "duty_pct": 0.0, "fault": 0, "uptime_s": 0.0,
          "seq": 0, "kp": None, "ki": None, "p_term": 0.0, "i_term": 0.0}


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(HERE), **kw)

    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/api/state"):
            last_client["t"] = time.time()
            with history_lock:
                hist = list(history[-1200:])
            with cmd_lock:
                cmd = dict(cmd_state)
            return self._json({"latest": latest, "history": hist,
                               "command": cmd, "error": last_error["msg"]})
        return super().do_GET()

    def do_POST(self):
        if not self.path.startswith("/api/command"):
            return self._json({"error": "unknown endpoint"}, 404)
        last_client["t"] = time.time()
        try:
            n = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(n) or b"{}")
        except Exception as exc:
            return self._json({"error": "bad request: %s" % exc}, 400)

        with cmd_lock:
            if "setpoint_c" in body:
                sp = float(body["setpoint_c"])
                if not SETPOINT_MIN_C <= sp <= SETPOINT_MAX_C:
                    return self._json(
                        {"error": "setpoint must be %g-%g C"
                                  % (SETPOINT_MIN_C, SETPOINT_MAX_C)}, 400)
                cmd_state["setpoint_c"] = sp
            if "kp" in body:
                kp = float(body["kp"])
                if not 0.0 <= kp <= KP_MAX:
                    return self._json(
                        {"error": "Kp must be 0-%g %%/C" % KP_MAX}, 400)
                cmd_state["kp"] = kp
            if "ki" in body:
                ki = float(body["ki"])
                if not 0.0 <= ki <= KI_MAX:
                    return self._json(
                        {"error": "Ki must be 0-%g %%/C/s" % KI_MAX}, 400)
                cmd_state["ki"] = ki
            if "enable" in body:
                cmd_state["enable"] = 1 if body["enable"] else 0
        try:
            push_command()
        except Exception as exc:
            return self._json({"error": str(exc)}, 502)
        with cmd_lock:
            return self._json({"ok": True, "command": dict(cmd_state)})


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kp", type=float, default=12.0)
    ap.add_argument("--ki", type=float, default=0.6)
    ap.add_argument("--setpoint", type=float, default=30.0)
    args = ap.parse_args()
    cmd_state["kp"] = max(0.0, min(KP_MAX, args.kp))
    cmd_state["ki"] = max(0.0, min(KI_MAX, args.ki))
    cmd_state["setpoint_c"] = max(SETPOINT_MIN_C, min(SETPOINT_MAX_C, args.setpoint))

    print("g_cmd @ 0x%08x   g_srv @ 0x%08x" % (G_CMD, G_SRV))
    ocd.connect()
    print("connected to OpenOCD")
    push_command()                       # start disabled, known setpoint
    threading.Thread(target=poller, daemon=True).start()
    print("\n  Reflow Oven web app:  http://127.0.0.1:%d/\n" % HTTP_PORT)
    with Server(("127.0.0.1", HTTP_PORT), Handler) as httpd:
        httpd.serve_forever()


if __name__ == "__main__":
    main()
