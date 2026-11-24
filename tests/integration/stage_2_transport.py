"""Run the protocol v3 HTTP and WebSocket checks against a local server executable."""

import argparse
import base64
from contextlib import contextmanager
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
from urllib.parse import urlencode


HOST = "127.0.0.1"
ORIGIN = "http://127.0.0.1:1420"
ADMIN_SECRET = "stage-2-fixture-admin-credential"
DEVICE_SECRET = "stage-2-fixture-device-credential"
CONTROL = "/ws/control"
AUDIO = "/ws/audio"
HEALTH = "/api/health"
STATUS = "/api/status"
SESSION = "/api/admin/session"
WEBSOCKET_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

FIXTURE = json.loads(
    (Path(__file__).resolve().parents[1] / "contracts/fixtures/protocol-v3.json")
    .read_text(encoding="utf-8")
)


def request(port, path, method="GET", token=None, body=None, headers=None):
    request_headers = dict(headers or {})
    if token:
        request_headers["Authorization"] = f"Bearer {token}"
    if body is not None:
        request_headers.setdefault("Content-Type", "application/json")
    connection = http.client.HTTPConnection(HOST, port, timeout=3)
    try:
        connection.request(method, path, body, request_headers)
        response = connection.getresponse()
        data = response.read()
        if data and response.getheader("Content-Type", "").startswith("application/json"):
            data = json.loads(data)
        return response.status, data, response.headers
    finally:
        connection.close()


def wait_health(port, paired, process=None):
    expected = FIXTURE["http"][
        f"GET /api/health {'after' if paired else 'before'} pairing"
    ]
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        if process and process.poll() is not None:
            raise AssertionError(f"server quit with code {process.returncode}")
        try:
            actual = request(port, HEALTH)[:2]
            if actual == (expected["status"], expected["body"]):
                return
        except OSError:
            pass
        time.sleep(0.02)
    raise AssertionError(f"health did not reach bridge_connected={paired}")


@contextmanager
def server(executable, rust=False, authenticated=False, locked=False):
    with tempfile.TemporaryDirectory(prefix="meowlive-transport-") as directory:
        work = Path(directory)
        with socket.socket() as probe:
            probe.bind((HOST, 0))
            port = probe.getsockname()[1]
        config = work / "server.toml"
        rust_sections = "\n[viewers]\nenabled = false\n[training]\nenabled = false\n" if rust else ""
        config.write_text(
            f'[server]\nlisten_address = "{HOST}:{port}"\nallowed_origins = ["{ORIGIN}"]\n'
            f'[auth]\nenabled = {str(authenticated).lower()}\nsession_lifetime_seconds = 2\n'
            + rust_sections,
            encoding="utf-8",
        )
        arguments = [executable, "--config", config]
        if not rust:
            arguments += ["--data-dir", work / "data"]
        env = os.environ.copy()
        env.pop("MEOWLIVE_ADMIN_TOKEN", None)
        env.pop("MEOWLIVE_DEVICE_TOKEN", None)
        if authenticated and not locked:
            env["MEOWLIVE_ADMIN_TOKEN"] = ADMIN_SECRET
            env["MEOWLIVE_DEVICE_TOKEN"] = DEVICE_SECRET
        with (work / "server.log").open("wb") as log:
            process = subprocess.Popen(
                arguments, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT
            )
            try:
                wait_health(port, False, process)
                yield port
            except Exception:
                if process.poll() is not None:
                    print(f"Server exit status: {process.returncode}")
                print((work / "server.log").read_text(encoding="utf-8", errors="replace"))
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


class WebSocket:
    def __init__(self, port, path, token=None, origin=None):
        self.socket = socket.create_connection((HOST, port), timeout=8)
        key = base64.b64encode(os.urandom(16))
        headers = [
            f"GET {path} HTTP/1.1",
            f"Host: {HOST}:{port}",
            "Connection: Upgrade",
            "Upgrade: websocket",
            f"Sec-WebSocket-Key: {key.decode('ascii')}",
            "Sec-WebSocket-Version: 13",
        ]
        if token:
            headers.append(f"Authorization: Bearer {token}")
        if origin:
            headers.append(f"Origin: {origin}")
        try:
            self.socket.sendall(("\r\n".join(headers) + "\r\n\r\n").encode("ascii"))
            self.response = http.client.HTTPResponse(self.socket)
            self.response.begin()
            self.status = self.response.status
            if self.status == 101:
                accept = base64.b64encode(hashlib.sha1(key + WEBSOCKET_GUID).digest())
                assert self.response.getheader("Sec-WebSocket-Accept") == accept.decode("ascii")
                self.body = None
            else:
                self.body = json.loads(self.response.read())
        except Exception:
            self.socket.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.response.close()
        self.socket.close()

    def read(self, length):
        data = self.response.fp.read(length)
        if len(data) != length:
            raise EOFError("WebSocket closed")
        return data

    def receive(self):
        first, second = self.read(2)
        assert first & 0x80 and not second & 0x80, (first, second)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self.read(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.read(8))[0]
        return first & 0x0F, self.read(length)

    def send(self, opcode, payload):
        mask = os.urandom(4)
        length = len(payload)
        frame = bytes([0x80 | opcode, 0x80 | (length if length < 126 else 126)])
        if length >= 126:
            frame += struct.pack("!H", length)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.socket.sendall(frame + mask + masked)

    def send_json(self, data):
        self.send(1, json.dumps(data).encode("utf-8"))

    def wait_frame(self, expected_opcode):
        while True:
            opcode, data = self.receive()
            if opcode == expected_opcode:
                return data
            assert opcode == 9, opcode
            self.send(10, data)

    def wait_closed(self):
        try:
            self.wait_frame(8)
        except EOFError:
            pass


def hello(control):
    assert control.status == 101, control.status
    control.send_json(FIXTURE["control"]["client_hello"])
    answer = json.loads(control.wait_frame(1))
    assert answer["type"] == "hello" and answer["protocol_version"] == 3, answer
    assert answer["session_id"] and answer["bridge_id"] and answer["generation"] > 0, answer
    return answer


def pair_path(answer):
    return f"{AUDIO}?" + urlencode({
        "session_id": answer["session_id"],
        "bridge_id": answer["bridge_id"],
    })


def reject_contract(port, path, sample):
    expected = FIXTURE["http"][sample]
    with WebSocket(port, path) as rejected:
        assert (rejected.status, rejected.body) == (expected["status"], expected["body"])


def check_bridge(port):
    with WebSocket(port, CONTROL, origin="https://invalid.example") as rejected:
        assert rejected.status == 403 and rejected.body["code"] == "invalid_origin"

    with WebSocket(port, CONTROL) as control:
        answer = hello(control)
        wait_health(port, False)
        status = request(port, STATUS)[1]
        assert status == {
            "protocol_version": 3,
            "bridge_connected": False,
            "session_id": answer["session_id"],
            "generation": answer["generation"],
            "speeches": [],
        }, status
        reject_contract(port, f"{AUDIO}?session_id=old&bridge_id=old", "GET /ws/audio invalid pair")
        reject_contract(port, CONTROL, "GET /ws/control occupied")
        with WebSocket(port, pair_path(answer)) as audio:
            assert audio.status == 101
            wait_health(port, True)
            reject_contract(port, pair_path(answer), "GET /ws/audio occupied")
            receipt = FIXTURE["control"]["client_completed"]
            control.send_json(receipt)
            control.send_json(receipt)
            control.send(9, b"duplicate-receipt")
            assert control.wait_frame(10) == b"duplicate-receipt"
            status = request(port, STATUS)[1]
            assert status["speeches"] == [] and status["generation"] == answer["generation"]
        control.wait_closed()
    wait_health(port, False)

    with WebSocket(port, CONTROL) as control:
        next_answer = hello(control)
        assert next_answer["bridge_id"] != answer["bridge_id"]
        assert next_answer["generation"] > answer["generation"]
        reject_contract(port, pair_path(answer), "GET /ws/audio invalid pair")
        with WebSocket(port, pair_path(next_answer)) as audio:
            assert audio.status == 101
            control.send(8, struct.pack("!H", 1000))
            audio.wait_closed()
    wait_health(port, False)

    with WebSocket(port, CONTROL) as control:
        assert control.status == 101
        control.send_json({**FIXTURE["control"]["client_hello"], "protocol_version": 999})
        control.wait_closed()
    with WebSocket(port, CONTROL) as control:
        timed_out = hello(control)
        control.wait_closed()
    reject_contract(port, pair_path(timed_out), "GET /ws/audio invalid pair")
    with WebSocket(port, CONTROL) as control:
        assert control.status == 101
        control.wait_closed()
    with WebSocket(port, CONTROL) as control:
        assert hello(control)["session_id"] == answer["session_id"]
    return answer


def check_restart(port, previous):
    reject_contract(port, pair_path(previous), "GET /ws/audio invalid pair")
    with WebSocket(port, CONTROL) as control:
        answer = hello(control)
        assert answer["session_id"] != previous["session_id"]
        with WebSocket(port, pair_path(answer)) as audio:
            assert audio.status == 101
            wait_health(port, True)


def check_auth(port, rust):
    status, view, headers = request(port, SESSION)
    assert (status, view) == (200, {"enabled": True, "authenticated": False})
    assert headers["Cache-Control"] == "no-store"
    for supplied in (None, ADMIN_SECRET, DEVICE_SECRET):
        status, error, _ = request(port, STATUS, token=supplied)
        assert status == 401 and error["code"] == "authentication_required"
    for supplied in (None, ADMIN_SECRET):
        with WebSocket(port, CONTROL, token=supplied) as denied:
            assert denied.status == 401 and denied.body["code"] == "authentication_required"
    if not rust:
        nested = "[" * 128 + "0" + "]" * 128
        for malformed in (
            json.dumps({"token": ADMIN_SECRET, "extra": True}),
            '{"token":"' + ADMIN_SECRET + '","extra":' + nested + "}",
        ):
            status, error, _ = request(port, SESSION, "POST", body=malformed)
            assert status == 400 and error["code"] == "invalid_request"
    status, view, headers = request(
        port, SESSION, "POST", body=json.dumps({"token": ADMIN_SECRET})
    )
    assert status == 200 and headers["Cache-Control"] == "no-store"
    assert view["expires_in_seconds"] == 2
    token = view["token"]
    assert request(port, STATUS, token=token)[0] == 200
    assert request(port, SESSION, token=token)[1]["authenticated"]
    with WebSocket(port, CONTROL, token=token) as denied:
        assert denied.status == 401
    with WebSocket(port, CONTROL, token=DEVICE_SECRET, origin=ORIGIN) as control:
        location = pair_path(hello(control))
        with WebSocket(port, location, token=token) as denied:
            assert denied.status == 401
        with WebSocket(port, location, token=DEVICE_SECRET, origin=ORIGIN) as audio:
            assert audio.status == 101
            wait_health(port, True)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if request(port, STATUS, token=token)[0] == 401:
            break
        time.sleep(0.05)
    else:
        raise AssertionError("expired administrator session remained valid")
    status, view, _ = request(
        port, SESSION, "POST", body=json.dumps({"token": ADMIN_SECRET})
    )
    assert status == 200
    token = view["token"]
    assert request(port, SESSION, "DELETE", token=token)[0] == 204
    assert request(port, STATUS, token=token)[0] == 401


def check_locked(port):
    assert request(port, STATUS)[0] == 503
    with WebSocket(port, CONTROL) as denied:
        assert denied.status == 503 and denied.body["code"] == "auth_locked"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, help="path to meowlive-server or meowlive-server.exe")
    parser.add_argument("--rust", action="store_true", help="use the Rust server command line")
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)

    with server(executable, args.rust) as port:
        previous = check_bridge(port)
    with server(executable, args.rust) as port:
        check_restart(port, previous)
    print("Protocol v3 handshake, timeout, conflict, receipts and reconnect passed.")
    with server(executable, args.rust, authenticated=True) as port:
        check_auth(port, args.rust)
    print("Administrator sessions, device credentials and origin passed.")
    if not args.rust:
        with server(executable, authenticated=True, locked=True) as port:
            check_locked(port)
        print("Services with unloaded credentials rejected protected requests.")


if __name__ == "__main__":
    main()
