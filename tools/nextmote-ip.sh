#!/bin/sh
set -eu

usage() {
    echo "Usage: $0 [SERIAL_PORT]"
    echo "       $0 --search [NETWORK_INTERFACE]"
    echo "Resets nextMote, reads its serial boot log, and prints its current IP."
    echo "--search finds nextMote on the active local IPv4 subnet without resetting it."
}

if [ "$#" -eq 1 ] && { [ "$1" = "-h" ] || [ "$1" = "--help" ]; }; then
    usage
    exit 0
fi

if [ "${1:-}" = "--search" ] || [ "${1:-}" = "-s" ]; then
    [ "$#" -le 2 ] || { usage >&2; exit 2; }
    interface=${2:-}
    python=${PYTHON:-python3}
    command -v "$python" >/dev/null 2>&1 || {
        echo "Python not found: $python (set PYTHON to a Python 3 executable)." >&2
        exit 1
    }
    exec "$python" - "$interface" <<'PY'
import concurrent.futures
import http.client
import ipaddress
import json
import subprocess
import sys


def command_output(args):
    try:
        result = subprocess.run(args, check=True, capture_output=True, text=True)
    except FileNotFoundError:
        raise RuntimeError(f"Required command not found: {args[0]}")
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.strip() or "command failed"
        raise RuntimeError(f"{' '.join(args)}: {detail}")
    return result.stdout


def default_interface():
    for route in command_output(["ip", "-4", "route", "show", "default"]).splitlines():
        fields = route.split()
        if "dev" in fields:
            return fields[fields.index("dev") + 1]
    raise RuntimeError("No default IPv4 network interface found.")


def interface_network(interface):
    output = command_output(["ip", "-o", "-4", "addr", "show", "dev", interface, "scope", "global"])
    for line in output.splitlines():
        fields = line.split()
        if "inet" in fields:
            return ipaddress.ip_interface(fields[fields.index("inet") + 1])
    raise RuntimeError(f"No global IPv4 address found on {interface}.")


def identify_nextmote(address):
    for path, mode in (("/api/status", "normal"), ("/api/wifi/state", "setup")):
        connection = http.client.HTTPConnection(str(address), 80, timeout=0.5)
        try:
            connection.request("GET", path, headers={"Connection": "close"})
            response = connection.getresponse()
            if response.status != 200:
                continue
            payload = json.loads(response.read(2048))
            if mode == "normal" and {
                "pairing_profile", "pairing_phase", "sync_window"
            }.issubset(payload):
                return address, mode
            if mode == "setup" and payload.get("mode") == "setup" and payload.get("ap", "").startswith(
                "nextMote-Setup-"
            ):
                return address, mode
        except (OSError, http.client.HTTPException, json.JSONDecodeError, UnicodeDecodeError):
            pass
        finally:
            connection.close()
    return None


try:
    selected_interface = sys.argv[1] or default_interface()
    local = interface_network(selected_interface)
    network = local.network
    if network.num_addresses > 256:
        raise RuntimeError(
            f"{network} has more than 256 addresses; refusing a broad scan. "
            "Choose a smaller local interface."
        )

    addresses = [address for address in network.hosts() if address != local.ip]
    print(
        f"Searching {network} on {selected_interface} "
        "(HTTP status checks only; nextMote will not be reset)...",
        flush=True,
    )
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
        results = list(executor.map(identify_nextmote, addresses))

    matches = [result for result in results if result]
    if not matches:
        print("No nextMote found. Check that it is on this network and its web server is running.")
        raise SystemExit(1)
    for address, mode in matches:
        suffix = " (setup mode)" if mode == "setup" else ""
        print(f"nextMote found{suffix}: http://{address}/")
except RuntimeError as exc:
    print(exc, file=sys.stderr)
    raise SystemExit(1)
PY
fi

[ "$#" -le 1 ] || { usage >&2; exit 2; }

port=${1:-}
if [ -z "$port" ]; then
    count=0
    for device in /dev/serial/by-id/*; do
        [ -e "$device" ] || continue
        port=$device
        count=$((count + 1))
    done
    if [ "$count" -eq 0 ]; then
        for device in /dev/ttyUSB* /dev/ttyACM*; do
            [ -e "$device" ] || continue
            port=$device
            count=$((count + 1))
        done
    fi
    if [ "$count" -eq 0 ]; then
        echo "No serial device found. Connect nextMote by USB, or pass its port path." >&2
        usage
    fi
    if [ "$count" -gt 1 ]; then
        echo "More than one serial device found. Pass the nextMote port explicitly:" >&2
        for device in /dev/serial/by-id/* /dev/ttyUSB* /dev/ttyACM*; do
            [ -e "$device" ] && echo "  $device" >&2
        done
        usage
    fi
fi

[ -e "$port" ] || {
    echo "Serial device not found: $port" >&2
    exit 1
}

python=${PYTHON:-python3}
command -v "$python" >/dev/null 2>&1 || {
    echo "Python not found: $python (set PYTHON to a Python 3 executable)." >&2
    exit 1
}
"$python" -c 'import serial' >/dev/null 2>&1 || {
    echo "pyserial is required. Install it with: python3 -m pip install --user pyserial" >&2
    echo "Alternatively, set PYTHON to an interpreter that already has pyserial." >&2
    exit 1
}

echo "Resetting nextMote on $port and waiting up to 60 seconds for its IP..." >&2
exec "$python" - "$port" <<'PY'
import re
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is required; install it with: python3 -m pip install --user pyserial",
          file=sys.stderr)
    raise SystemExit(1)

port_name = sys.argv[1]
deadline = time.monotonic() + 60
line_buffer = ""

try:
    with serial.Serial(port_name, 115200, timeout=0.25) as device:
        device.dtr = False
        device.rts = True
        time.sleep(0.1)
        device.rts = False

        while time.monotonic() < deadline:
            data = device.read(512)
            if not data:
                continue
            line_buffer += data.decode("utf-8", errors="replace")
            lines = line_buffer.splitlines(keepends=True)
            line_buffer = ""
            if lines and not lines[-1].endswith(("\n", "\r")):
                line_buffer = lines.pop()

            for line in lines:
                match = re.search(r"Wi-Fi connected; IP=(\d{1,3}(?:\.\d{1,3}){3})", line)
                if match:
                    print(f"nextMote IP: http://{match.group(1)}/")
                    raise SystemExit(0)

                if "Join Wi-Fi " in line:
                    ap = re.search(r'Join Wi-Fi "([^"]+)"', line)
                    url = re.search(r"(https?://\S+)", line)
                    if ap and url:
                        print(f"nextMote is in setup mode. Join {ap.group(1)} and open {url.group(1)}")
                        raise SystemExit(0)

except serial.SerialException as exc:
    print(f"Could not read {port_name}: {exc}", file=sys.stderr)
    raise SystemExit(1)

print("Timed out waiting for an IP. Check that nextMote is powered and can reach Wi-Fi.",
      file=sys.stderr)
raise SystemExit(1)
PY
