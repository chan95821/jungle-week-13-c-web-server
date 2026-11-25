#!/usr/bin/env python3
"""Approved B benchmark: immutable C services + bounded, open-loop asyncio client.

prepare compiles an isolated snapshot; smoke checks the client/contracts; run
executes only the approved 30 trials. No OS tuning or server changes are made.
"""
import argparse
import asyncio
from collections import Counter
import csv
from datetime import datetime, timezone
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import resource
import shutil
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
QUERY_RATES = (50, 100, 200, 400, 800)
SUBMIT_RATE = 10
WORKERS = 4
WARMUP_SECONDS = 3
MEASURE_SECONDS = 15
REPEATS = 3
REQUEST_TIMEOUT = 2.0
MAX_INFLIGHT = 256
LATE_THRESHOLD_MS = 1.0
SUBMIT_BODY = bytes(range(256)) * 4
QUERY_BODY = b"status\0ready\xff"
FLAGS = "-std=c11 -Wall -Wextra -Werror -O2"


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".pending")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def environment():
    cpu_lines = Path("/proc/cpuinfo").read_text().splitlines()
    cpu_model = next((line.split(":", 1)[1].strip() for line in cpu_lines
                      if line.startswith("model name")), "unknown")
    mem = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        key, value = line.split(":", 1)
        if key in ("MemTotal", "MemAvailable", "SwapTotal"):
            mem[key + "_KiB"] = int(value.split()[0])
    result = {
        "utc": datetime.now(timezone.utc).isoformat(),
        "kernel": platform.release(), "machine": platform.machine(),
        "os_release": Path("/etc/os-release").read_text(),
        "cpu_model": cpu_model, "visible_logical_cpus": os.cpu_count(),
        "affinity_cpus": sorted(os.sched_getaffinity(0)), "memory": mem,
        "fd_soft_hard": resource.getrlimit(resource.RLIMIT_NOFILE),
        "python": platform.python_version(),
        "gcc": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
        "ephemeral_ports": Path("/proc/sys/net/ipv4/ip_local_port_range").read_text().strip(),
        "tcp_tw_reuse": Path("/proc/sys/net/ipv4/tcp_tw_reuse").read_text().strip(),
        "load_average": os.getloadavg(),
    }
    for relative in ("memory.max", "cpu.max", "cpuset.cpus.effective"):
        path = Path("/sys/fs/cgroup") / relative
        if path.exists():
            result["cgroup_" + relative] = path.read_text().strip()
    return result


def source_manifest(directory):
    paths = [directory / "Makefile"]
    for subdirectory in ("src", "cgi-src", "public", "tests/backends"):
        paths.extend(path for path in (directory / subdirectory).rglob("*") if path.is_file())
    return {str(path.relative_to(directory)): sha256(path) for path in sorted(paths)
            if path.name != "Makefile" and path.suffix != ".o"} | {"Makefile": sha256(directory / "Makefile")}


def prepare(output, revision):
    output.mkdir(parents=True, exist_ok=False)
    snapshot = output / "snapshot"
    snapshot.mkdir()
    shutil.copy2(ROOT / "Makefile", snapshot / "Makefile")
    for relative in ("src", "cgi-src", "public", "tests/backends"):
        shutil.copytree(ROOT / relative, snapshot / relative,
                        ignore=shutil.ignore_patterns("*.o", "__pycache__"))
    shutil.copy2(Path(__file__), output / "benchmark_iter5.py")
    with (output / "build.log").open("w") as log:
        subprocess.run(["make", "all", "backend", "-j4", "CFLAGS=" + FLAGS],
                       cwd=snapshot, stdout=log, stderr=log, check=True)
    original = source_manifest(ROOT)
    copied = source_manifest(snapshot)
    assert original == copied, "source changed while copying snapshot"
    manifest = {"revision_at_start": revision, "flags": FLAGS, "thread_flags": "-pthread",
                "original_sources": original, "snapshot_sources": copied,
                "server_sha256": sha256(snapshot / "build/server"),
                "backend_sha256": sha256(snapshot / "build/test_backend"),
                "generator_sha256": sha256(Path(__file__))}
    write_json(output / "manifest.json", manifest)
    write_json(output / "environment.json", environment())
    print("Prepared immutable -O2 B snapshot and identical C backends.", flush=True)


def free_port(used):
    for _ in range(100):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        if port not in used:
            used.add(port)
            return port
    raise RuntimeError("could not allocate distinct local ports")


class Fixture:
    def __init__(self, output, directory, mode):
        self.output, self.directory, self.mode = output, directory, mode
        self.children, self.logs, self.ports = {}, [], {}
        used = set()
        try:
            for role in ("submission", "query-a", "query-b"):
                self.ports[role] = free_port(used)
                self.spawn(role, [str(output / "snapshot/build/test_backend"),
                                 str(self.ports[role]), role], self.ports[role])
            if mode == "b":
                config = directory / "proxy.conf"
                config.write_text("".join(f"{role} 127.0.0.1:{port}\n"
                                          for role, port in self.ports.items()), encoding="ascii")
                self.proxy_port = free_port(used)
                self.spawn("proxy", [str(output / "snapshot/build/server"),
                                     str(self.proxy_port), str(config), str(WORKERS)], self.proxy_port)
        except BaseException:
            self.close()
            raise

    def spawn(self, role, argv, port):
        log = (self.directory / (role + ".stderr.log")).open("w")
        self.logs.append(log)
        child = subprocess.Popen(argv, cwd=self.output / "snapshot",
                                 stdout=subprocess.DEVNULL, stderr=log)
        self.children[role] = child
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if child.poll() is not None:
                raise RuntimeError(f"{role} exited at startup: {child.returncode}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    return
            except OSError:
                time.sleep(.02)
        raise TimeoutError(f"{role} startup")

    def destination(self, kind, index):
        if self.mode == "b":
            return self.proxy_port
        return self.ports["submission" if kind == "submit" else ("query-a" if index % 2 == 0 else "query-b")]

    def ensure_alive(self):
        for role, child in self.children.items():
            if child.poll() is not None:
                raise RuntimeError(f"service {role} exited: {child.returncode}")

    def close(self):
        for child in reversed(list(self.children.values())):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
        for log in self.logs:
            log.close()


def process_stats(pid):
    status = Path(f"/proc/{pid}/status").read_text().splitlines()
    rss = next(int(line.split()[1]) for line in status if line.startswith("VmRSS:"))
    return {"rss_mib": rss / 1024, "fds": len(list(Path(f"/proc/{pid}/fd").iterdir()))}


def percentile(values, fraction=.95):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def parse_header(header):
    lines = header[:-4].split(b"\r\n")
    version, code, _reason = lines[0].split(b" ", 2)
    if version != b"HTTP/1.1":
        raise ValueError("unexpected response HTTP version")
    fields = {}
    for line in lines[1:]:
        name, value = line.split(b":", 1)
        fields.setdefault(name.lower(), []).append(value.strip())
    if len(fields.get(b"content-length", [])) != 1 or b"transfer-encoding" in fields:
        raise ValueError("unexpected response framing")
    length = int(fields[b"content-length"][0])
    if not 0 <= length <= 65536:
        raise ValueError("response length outside fixture bounds")
    return int(code), fields, length


def validate_response(kind, status, fields, body):
    expected_status = 201 if kind == "submit" else 200
    expected_body = SUBMIT_BODY if kind == "submit" else QUERY_BODY
    allowed = (b"submission",) if kind == "submit" else (b"query-a", b"query-b")
    identities = fields.get(b"x-backend-id", [])
    if status != expected_status or body != expected_body or len(identities) != 1 or identities[0] not in allowed:
        raise ValueError("status/body/backend identity mismatch")
    return identities[0].decode("ascii")


def wire_request(kind):
    target = b"/api/submissions" if kind == "submit" else b"/api/submission-status"
    first = (b"POST " if kind == "submit" else b"GET ") + target + b" HTTP/1.1\r\n"
    fields = b"Host: benchmark.local\r\nConnection: close\r\n"
    if kind == "submit":
        fields += b"Content-Length: 1024\r\nContent-Type: application/octet-stream\r\n"
    return first + fields + b"\r\n" + (SUBMIT_BODY if kind == "submit" else b"")


def summarize_records(records, kind, duration, end):
    selected = [row for row in records if row["kind"] == kind]
    successful = [row for row in selected if row["outcome"] == "success"]
    started = [row for row in selected if row["started_offset_s"] is not None]
    failures = Counter(row["outcome"] for row in selected if row["outcome"] != "success")
    count = len(selected)
    return {
        "offered": count, "started": len(started), "success": len(successful),
        "failures": count - len(successful),
        "failure_pct": 100 * (count - len(successful)) / count if count else 0,
        "success_rps": len(successful) / max(duration, end),
        "success_in_window_rps": sum(row["finished_offset_s"] <= duration for row in successful) / duration,
        "scheduled_p95_ms": percentile([row["latency_ms"] for row in successful]),
        "network_p95_ms": percentile([row["service_ms"] for row in successful]),
        "dispatch_p95_ms": percentile([row["dispatch_lag_ms"] for row in selected]),
        "delayed_over_1ms": sum(row["dispatch_lag_ms"] > LATE_THRESHOLD_MS for row in selected),
        "timeout": failures["timeout"] + failures["schedule_timeout"],
        "generator_cap_rejections": failures["generator_cap"],
        "failure_categories": dict(failures),
        "status_5xx": sum(row["status"] is not None and row["status"] >= 500 for row in selected),
        "backend_hits": dict(Counter(row["backend"] for row in successful)),
    }


async def run_phase(fixture, query_rate, duration, directory, label):
    loop = asyncio.get_running_loop()
    start = loop.time() + .05
    inflight = set()
    slots = 0
    peak_slots = 0
    records = []
    resources = {role: process_stats(child.pid) for role, child in fixture.children.items()}
    resources["generator"] = process_stats(os.getpid())
    peaks = {role: dict(value) for role, value in resources.items()}
    sampling = True

    async def sample_resources():
        while sampling:
            for role, pid in [(role, child.pid) for role, child in fixture.children.items()] + [("generator", os.getpid())]:
                current = process_stats(pid)
                for metric in ("rss_mib", "fds"):
                    peaks[role][metric] = max(peaks[role][metric], current[metric])
            await asyncio.sleep(1)

    sampler = asyncio.create_task(sample_resources())

    def record(kind, index, due, dispatched):
        return {"kind": kind, "index": index, "scheduled_offset_s": due - start,
                "dispatch_lag_ms": max(0, (dispatched - due) * 1000),
                "started_offset_s": None, "finished_offset_s": dispatched - start,
                "latency_ms": max(0, (dispatched - due) * 1000), "service_ms": None,
                "outcome": "", "status": None, "backend": "", "error": ""}

    async def exchange(row, due):
        nonlocal slots
        writer = None
        began = loop.time()
        row["started_offset_s"] = began - start
        row["dispatch_lag_ms"] = max(0, (began - due) * 1000)
        try:
            async with asyncio.timeout_at(due + REQUEST_TIMEOUT):
                reader, writer = await asyncio.open_connection("127.0.0.1",
                    fixture.destination(row["kind"], row["index"]), limit=32768)
                writer.write(wire_request(row["kind"]))
                await writer.drain()
                writer.write_eof()  # Request complete: let bounded server drain observe EOF.
                header = await reader.readuntil(b"\r\n\r\n")
                status, fields, length = parse_header(header)
                row["status"] = status
                body = await reader.readexactly(length)
                if await reader.read(1):
                    raise ValueError("extra response bytes")
                row["backend"] = validate_response(row["kind"], status, fields, body)
                row["outcome"] = "success"
        except TimeoutError:
            row["outcome"] = "timeout"
        except (ValueError, asyncio.IncompleteReadError, asyncio.LimitOverrunError) as error:
            row["outcome"], row["error"] = "invalid_response", str(error)
        except OSError as error:
            row["outcome"], row["error"] = "io_error", str(error)
        finally:
            completed = loop.time()
            row["finished_offset_s"] = completed - start
            row["latency_ms"] = max(0, (completed - due) * 1000)
            row["service_ms"] = (completed - began) * 1000
            if writer:
                writer.close()
                try:
                    await asyncio.wait_for(writer.wait_closed(), timeout=.2)
                except (OSError, TimeoutError):
                    pass
            records.append(row)
            slots -= 1

    async def offer(kind, rate):
        nonlocal slots, peak_slots
        for index in range(round(rate * duration)):
            due = start + index / rate
            remaining = due - loop.time()
            if remaining > 0:
                await asyncio.sleep(remaining)
            dispatched = loop.time()
            row = record(kind, index, due, dispatched)
            if dispatched >= due + REQUEST_TIMEOUT:
                row["outcome"] = "schedule_timeout"
                records.append(row)
            elif slots >= MAX_INFLIGHT:
                row["outcome"] = "generator_cap"
                records.append(row)
            else:
                slots += 1
                peak_slots = max(peak_slots, slots)
                task = asyncio.create_task(exchange(row, due))
                inflight.add(task)
                task.add_done_callback(inflight.discard)
            if index % 64 == 63:
                await asyncio.sleep(0)

    try:
        await asyncio.gather(offer("submit", SUBMIT_RATE), offer("query", query_rate))
        if inflight:
            await asyncio.gather(*list(inflight))
        await asyncio.sleep(max(0, start + duration - loop.time()))
    finally:
        sampling = False
        sampler.cancel()
        await asyncio.gather(sampler, return_exceptions=True)
    end = max((row["finished_offset_s"] for row in records), default=duration)
    await asyncio.sleep(.3)  # Outside measurement: allow final connection cleanup.
    fixture.ensure_alive()
    after = {role: process_stats(child.pid) for role, child in fixture.children.items()}
    after["generator"] = process_stats(os.getpid())
    with gzip.open(directory / (label + "-requests.csv.gz"), "wt", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(sorted(records, key=lambda row: (row["kind"], row["index"])))
    for kind, rate in (("submit", SUBMIT_RATE), ("query", query_rate)):
        assert sum(row["kind"] == kind for row in records) == round(rate * duration)
    return {"duration_s": duration, "elapsed_with_drain_s": max(duration, end),
            "peak_inflight": peak_slots,
            "submit": summarize_records(records, "submit", duration, end),
            "query": summarize_records(records, "query", duration, end),
            "resources": {role: {"before": initial, "sampled_peak": peaks[role], "after": after[role]}
                          for role, initial in resources.items()}}


def verify_snapshot(output):
    manifest = json.loads((output / "manifest.json").read_text())
    assert manifest["snapshot_sources"] == source_manifest(output / "snapshot"), "snapshot sources changed"
    assert manifest["server_sha256"] == sha256(output / "snapshot/build/server")
    assert manifest["backend_sha256"] == sha256(output / "snapshot/build/test_backend")
    assert manifest["generator_sha256"] == sha256(Path(__file__)), "generator changed since prepare"


def smoke(output):
    verify_snapshot(output)
    for mode in ("direct", "b"):
        directory = output / ("smoke-" + mode)
        directory.mkdir(exist_ok=False)
        fixture = Fixture(output, directory, mode)
        try:
            result = asyncio.run(run_phase(fixture, 10, .3, directory, "smoke"))
            for kind in ("submit", "query"):
                assert result[kind]["offered"] == 3 and result[kind]["success"] == 3, result
            write_json(directory / "result.json", result)
        finally:
            fixture.close()
    # Reject corrupt responses without sending any extra benchmark traffic.
    try:
        validate_response("query", 200, {b"x-backend-id": [b"query-a"]}, b"wrong")
    except ValueError:
        pass
    else:
        raise AssertionError("load client accepted corrupt response")
    print("Smoke passed: both paths, binary POST/GET, response validation and offered counts.", flush=True)


def run(output):
    verify_snapshot(output)
    destination = output / "results.json"
    if destination.exists():
        raise RuntimeError("results already exist; refusing to overwrite or repeat measurements")
    result = {"completed": False, "conditions": {
        "workers": WORKERS, "local_threads": 2, "submission_backends": 1, "query_backends": 2,
        "submit_rps": SUBMIT_RATE, "query_rps": QUERY_RATES,
        "submit_body_bytes": len(SUBMIT_BODY), "query_body_bytes": len(QUERY_BODY),
        "warmup_s": WARMUP_SECONDS, "measure_s": MEASURE_SECONDS, "repeats": REPEATS,
        "request_timeout_s_from_schedule": REQUEST_TIMEOUT, "max_inflight": MAX_INFLIGHT,
        "late_threshold_ms": LATE_THRESHOLD_MS, "resource_sample_interval_s": 1,
        "latency": "scheduled request time through EOF and byte validation; successful requests",
        "success_rps": "success count / max(duration, last completion offset), including drain",
        "provisional_submit_p95_ms_limit": 100, "provisional_failure_pct_limit": 1,
        "new_connection_per_request": True, "backend_mode": "normal"}, "trials": []}
    write_json(destination, result)
    write_json(output / "environment-at-run.json", environment())
    started = time.monotonic()
    try:
        for rate in QUERY_RATES:
            for repeat in range(1, REPEATS + 1):
                # Counterbalance order without changing rates, workers or server implementation.
                order = ("b", "direct") if repeat % 2 else ("direct", "b")
                for mode in order:
                    directory = output / f"{mode}-q{rate}-r{repeat}"
                    directory.mkdir(exist_ok=False)
                    fixture = Fixture(output, directory, mode)
                    try:
                        warm = asyncio.run(run_phase(fixture, rate, WARMUP_SECONDS, directory, "warmup"))
                        measured = asyncio.run(run_phase(fixture, rate, MEASURE_SECONDS, directory, "measured"))
                        trial = {"mode": mode, "query_rps": rate, "repeat": repeat,
                                 "warmup": warm, "measured": measured}
                        result["trials"].append(trial)
                        write_json(directory / "result.json", trial)
                        write_json(destination, result)
                        submission, query = measured["submit"], measured["query"]
                        print(f"[{len(result['trials']):02}/30] {mode:6} q={rate:3} r={repeat} "
                              f"query={query['success_rps']:.2f}/s submit-p95={submission['scheduled_p95_ms']!s}ms "
                              f"fail={submission['failures']}/{submission['offered']}+{query['failures']}/{query['offered']} "
                              f"cap={measured['peak_inflight']}", flush=True)
                    finally:
                        fixture.close()
        result["completed"] = True
        result["wall_elapsed_s"] = time.monotonic() - started
        write_json(destination, result)
        print("Completed all 30 approved trials; no additional rates or tuning applied.", flush=True)
    except BaseException as error:
        result["interrupted_error"] = repr(error)
        write_json(destination, result)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "smoke", "run"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", default="unavailable; see source manifest")
    options = parser.parse_args()
    output = options.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("output must stay inside this workspace's build directory")
    if options.action == "prepare":
        prepare(output, options.revision)
    elif options.action == "smoke":
        smoke(output)
    else:
        run(output)


if __name__ == "__main__":
    main()
