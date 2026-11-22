# SPDX-License-Identifier: Apache-2.0
"""Build fixtures and manage the two isolated UET test endpoints."""

from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import queue
import re
import shutil
import signal
import subprocess
import threading
import time


def source_manifest(root):
    selected = list(root.glob("*.c")) + list(root.glob("*.h")) + [root / "Makefile"]
    for directory in ("util", "nic_shim", "imp_shim", "crypto", "libfabric_headers"):
        selected += [p for p in (root / directory).rglob("*") if p.is_file() and p.suffix in (".c", ".h")]
    manifest = {}
    for path in sorted(set(selected)):
        manifest[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return manifest


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(args, timeout=60):
    result = subprocess.run([str(x) for x in args], text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"command failed: {Path(str(args[0])).name}")
    return result.stdout


def logged(args, path, timeout=300):
    with path.open("w") as log:
        result = subprocess.run([str(x) for x in args], stdout=log, stderr=subprocess.STDOUT,
                                timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"build failed; see {path.name}")


class Process:
    def __init__(self, args, env, path):
        self.log = path.open("w")
        self.path = path
        self.lines = queue.Queue()
        self.collect = True
        self.process = subprocess.Popen([str(x) for x in args], env=env, text=True,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=1)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            self.log.write(line)
            self.log.flush()
            if self.collect:
                self.lines.put(line.strip())
        self.lines.put(None)

    def wait(self, prefix, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if line is None:
                return None
            if line.startswith(prefix):
                return line[len(prefix):].strip()
        raise TimeoutError(prefix)

    def send(self, line):
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.reader.join(timeout=3)
        if self.reader.is_alive():
            raise RuntimeError("log reader failed to stop")
        self.log.close()


def build(root, reference, patch, compiler_options=()):
    root.mkdir()
    manifest = source_manifest(reference)
    for name in manifest:
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(reference / name, target)
    for index, source_patch in enumerate(patch or ()):
        logged(["git", "-C", root, "apply", source_patch], root / f"patch-{index}.log")
    flags = " ".join(("-Wall", "-Wno-address-of-packed-member",
                      "-Wno-unused-variable", "-O2", "-g", *compiler_options))
    ldflags = " ".join(("-pthread", *(f for f in compiler_options if f.startswith("-fsanitize="))))
    logged(["make", "-j4", "-C", root, f"LIBFABRIC={root}",
            f"CFLAGS={flags}", f"LDFLAGS={ldflags}", "libuet_verbs.so"], root / "build.log")


@contextmanager
def physical_ports(names):
    if not names:
        yield None
        return
    ports = []
    for name in names:
        net = Path("/sys/class/net") / name
        state = json.loads(command(["ip", "-j", "address", "show", "dev", name]))[0]
        if "UP" in state["flags"] or state["addr_info"] or (net / "master").exists():
            raise RuntimeError(f"{name} must be an unused, administratively down test port")
        (device,) = (net / "device/infiniband").iterdir()
        ports.append({"name": name, "device": device.name, "mac": state["address"], "sysfs": net})
    def interrupt(signum, _frame):
        raise SystemExit(f"interrupted by signal {signum}")

    handlers = {sig: signal.signal(sig, interrupt) for sig in (signal.SIGHUP, signal.SIGTERM)}
    up = []
    try:
        for port in ports:
            command(["ip", "link", "set", "dev", port["name"], "up"])
            up.append(port["name"])
        for _ in range(20):
            if all((p["sysfs"] / "carrier").read_text().strip() == "1" for p in ports):
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("the two test ports did not establish a physical link")
        for port in ports:
            port["speed_mbps"] = int((port["sysfs"] / "speed").read_text())
        yield ports
    finally:
        try:
            for name in reversed(up):
                command(["ip", "link", "set", "dev", name, "down"])
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)


def wire_counters(port):
    output = command(["ethtool", "-S", port["name"]])
    counters = dict(re.findall(r"^\s+(rx_packets_phy|tx_packets_phy):\s+(\d+)$", output, re.M))
    return {field: int(counters[field]) for field in ("rx_packets_phy", "tx_packets_phy")}
