#!/usr/bin/env python3
"""Iteration 5 benchmark runner: baseline main (577a554) vs C thread pool."""
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import socket
import statistics
import subprocess
import tarfile
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "performance-iter5"
CLIENT = OUT / "load_client"
BACKEND = OUT / "test_backend"
SLOW_BACKEND = OUT / "slow_backend"
FLAGS = "-std=c11 -Wall -Wextra -Werror -O2 -pthread"
BASELINE_REV = "577a554"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cmd_out(argv, **kwargs):
    return subprocess.check_output(argv, text=True, **kwargs).strip()


def unused_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_port(port, child):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise RuntimeError(f"Child {child.args} exited with {child.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise TimeoutError(f"startup timeout on port {port}")


def stop(child):
    if child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def process_stats(process):
    raw = Path(f"/proc/{process.pid}/stat").read_text().rsplit(")", 1)[1].split()
    ticks = os.sysconf("SC_CLK_TCK")
    status = Path(f"/proc/{process.pid}/status").read_text().splitlines()
    rss = next(int(line.split()[1]) for line in status if line.startswith("VmRSS:"))
    return {
        "user_s": int(raw[11]) / ticks,
        "system_s": int(raw[12]) / ticks,
        "rss_kib": rss,
        "fds": len(list(Path(f"/proc/{process.pid}/fd").iterdir())),
    }


def prepare():
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    baseline = OUT / "baseline"
    current = OUT / "current"
    archive_path = OUT / "baseline.tar"
    subprocess.run(
        [
            "git",
            "-c",
            "safe.directory=" + str(ROOT),
            "--git-dir=" + str(ROOT / ".git"),
            "--work-tree=" + str(ROOT),
            "archive",
            "--format=tar",
            "-o",
            str(archive_path),
            BASELINE_REV,
        ],
        check=True,
    )
    baseline.mkdir()
    with tarfile.open(archive_path) as archive:
        for member in archive.getmembers():
            if member.name != "Makefile" and member.name.split("/")[0] not in ("src", "public", "cgi-src"):
                continue
            target = (baseline / member.name).resolve()
            if baseline.resolve() not in target.parents or member.issym() or member.islnk():
                raise RuntimeError(f"Unexpected archive member: {member.name}")
            archive.extract(member, baseline)

    current.mkdir()
    shutil.copy2(ROOT / "Makefile", current / "Makefile")
    for directory in ("src", "public", "cgi-src"):
        shutil.copytree(ROOT / directory, current / directory, ignore=shutil.ignore_patterns("*.o", "__pycache__"))

    manifest = {}
    with (OUT / "build.log").open("w") as log:
        for name, cflags in (
            ("baseline", "-std=c11 -Wall -Wextra -Werror -O2"),
            ("current", FLAGS),
        ):
            snap = OUT / name
            subprocess.run(["make", "CFLAGS=" + cflags, "all"], cwd=snap, stdout=log, stderr=log, check=True)
            manifest[name] = {
                str(p.relative_to(snap)): digest(p)
                for p in snap.rglob("*")
                if p.is_file() and (p.suffix in (".c", ".h") or p.name == "Makefile")
            }
            manifest[name]["binary_sha256"] = digest(snap / "build" / "server")

        subprocess.run(
            ["gcc", *FLAGS.split(), "-o", str(BACKEND), "tests/backends/server.c", "src/socket_io.c", "src/http.c"],
            cwd=ROOT,
            stdout=log,
            stderr=log,
            check=True,
        )
        subprocess.run(
            ["gcc", *FLAGS.split(), "-o", str(SLOW_BACKEND), "tests/performance/slow_backend.c", "src/socket_io.c", "src/http.c"],
            cwd=ROOT,
            stdout=log,
            stderr=log,
            check=True,
        )
        subprocess.run(
            ["gcc", *FLAGS.split(), "-o", str(CLIENT), "tests/performance/load_client.c", "-lm"],
            cwd=ROOT,
            stdout=log,
            stderr=log,
            check=True,
        )
    manifest["baseline_rev"] = BASELINE_REV
    manifest["load_client"] = digest(ROOT / "tests/performance/load_client.c")
    manifest["slow_backend"] = digest(ROOT / "tests/performance/slow_backend.c")
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print("[PREPARE] Isolated baseline & current -O2 builds ready.", flush=True)


class BenchFixture:
    def __init__(self, slow_query_ms=0):
        self.temp = tempfile.TemporaryDirectory(prefix="iter5-bench-")
        self.dir = Path(self.temp.name)
        shutil.copytree(OUT / "current" / "public", self.dir / "public")
        shutil.copytree(OUT / "current" / "cgi-bin", self.dir / "cgi-bin")
        self.children = []
        self.ports = {}
        for role in ("submission", "query-a", "query-b"):
            self.ports[role] = unused_port()
            if slow_query_ms > 0 and role != "submission":
                argv = [str(SLOW_BACKEND), str(self.ports[role]), role, str(slow_query_ms)]
            else:
                argv = [str(BACKEND), str(self.ports[role]), role, "benchmark"]
            p = subprocess.Popen(argv, cwd=self.dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.children.append(p)
            wait_port(self.ports[role], p)
        self.config = self.dir / "proxy.conf"
        self.config.write_text("".join(f"{r} 127.0.0.1:{p}\n" for r, p in self.ports.items()))

    def spawn_proxy(self, build, workers=None):
        port = unused_port()
        argv = [str(OUT / build / "build" / "server"), str(port), str(self.config)]
        if workers is not None:
            argv.append(str(workers))
        p = subprocess.Popen(argv, cwd=self.dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_port(port, p)
        return p, port

    def close(self):
        for c in reversed(self.children):
            stop(c)
        self.temp.cleanup()


def run_load_client(port, method, rate, concurrency, duration):
    argv = [str(CLIENT), str(port), str(port), method, str(rate), str(concurrency), str(duration)]
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return proc


def run_exp1_trial(fixture, build, workers, stage, rate, duration):
    proxy, port = fixture.spawn_proxy(build, workers)
    try:
        # Warmup
        w_sub = run_load_client(port, "POST", 2, 2, 2.0)
        w_qry = run_load_client(port, "GET", rate, 16, 2.0)
        w_sub.wait(timeout=10)
        w_qry.wait(timeout=10)

        before = process_stats(proxy)
        peak_rss = before["rss_kib"]
        sub = run_load_client(port, "POST", 2, 2, duration)
        qry = run_load_client(port, "GET", rate, 16, duration)
        while sub.poll() is None or qry.poll() is None:
            peak_rss = max(peak_rss, process_stats(proxy)["rss_kib"])
            time.sleep(0.05)
        after = process_stats(proxy)
        s_json = json.loads(sub.communicate()[0])
        q_json = json.loads(qry.communicate()[0])
        return {
            "build": build,
            "workers": workers,
            "stage": stage,
            "submit": s_json,
            "query": q_json,
            "proxy_cpu_s": (after["user_s"] - before["user_s"]) + (after["system_s"] - before["system_s"]),
            "proxy_rss_peak_kib": peak_rss,
            "fds_before": before["fds"],
            "fds_after": after["fds"],
        }
    finally:
        stop(proxy)


def run_exp2_hol():
    """Hold K concurrent 800ms slow queries while measuring fast POST /api/submissions."""
    fixture = BenchFixture(slow_query_ms=800)
    results = []
    configs = [("baseline", None), ("current", 4), ("current", 8)]
    try:
        for build, workers in configs:
            for k in (0, 2, 4, 8):
                proxy, port = fixture.spawn_proxy(build, workers)
                try:
                    # Warmup 1 fast request
                    w = run_load_client(port, "POST", 5, 1, 0.5)
                    w.wait(timeout=5)

                    stop_slow = threading.Event()

                    def slow_loop():
                        while not stop_slow.is_set():
                            try:
                                with socket.create_connection(("127.0.0.1", port), timeout=4) as s:
                                    s.settimeout(4)
                                    s.sendall(b"GET /api/submission-status HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                                    while s.recv(4096):
                                        pass
                            except OSError:
                                time.sleep(0.02)

                    slow_threads = [threading.Thread(target=slow_loop) for _ in range(k)]
                    for t in slow_threads:
                        t.start()
                    if k > 0:
                        time.sleep(0.15)

                    fast = run_load_client(port, "POST", 10, 4, 3.2)
                    fast_out, _ = fast.communicate(timeout=15)
                    stop_slow.set()
                    for t in slow_threads:
                        t.join(timeout=5)
                    fj = json.loads(fast_out)
                    results.append({
                        "build": build,
                        "workers": workers,
                        "slow_k": k,
                        "fast_submit": fj,
                    })
                    print(
                        f"[EXP2] {build} w={workers} K={k}: submit rps={fj['success_rps']:.1f} "
                        f"p50={fj['p50_ms']:.2f}ms p95={fj['p95_ms']:.2f}ms fail={fj['failures']}",
                        flush=True,
                    )
                finally:
                    stop(proxy)
    finally:
        fixture.close()
    return results


def run_exp3_saturation():
    """Compare workers=1,2,4,8,16,32 under c=32 and queue-overflow c=150 burst."""
    fixture = BenchFixture(slow_query_ms=0)
    results = []
    try:
        for workers in (1, 2, 4, 8, 16, 32):
            for label, n_procs, conc, dur in (("c32", 2, 16, 4.0), ("c150_burst", 10, 15, 4.0)):
                proxy, port = fixture.spawn_proxy("current", workers)
                try:
                    w = run_load_client(port, "GET", 0, 16, 1.0)
                    w.wait(timeout=10)
                    before = process_stats(proxy)
                    peak_rss = before["rss_kib"]
                    procs = [run_load_client(port, "GET", 0, conc, dur) for _ in range(n_procs)]
                    while any(p.poll() is None for p in procs):
                        peak_rss = max(peak_rss, process_stats(proxy)["rss_kib"])
                        time.sleep(0.05)
                    after = process_stats(proxy)
                    items = [json.loads(p.communicate()[0]) for p in procs]
                    total_succ = sum(x["success"] for x in items)
                    total_fail = sum(x["failures"] for x in items)
                    avg_dur = statistics.mean(x["duration_s"] for x in items)
                    max_p95 = max(x["p95_ms"] for x in items)
                    med_p50 = statistics.median(x["p50_ms"] for x in items)
                    qa = sum(x["backend_hits"]["query-a"] for x in items)
                    qb = sum(x["backend_hits"]["query-b"] for x in items)
                    row = {
                        "workers": workers,
                        "mode": label,
                        "total_rps": total_succ / avg_dur,
                        "success": total_succ,
                        "failures": total_fail,
                        "p50_ms": med_p50,
                        "p95_max_ms": max_p95,
                        "qa": qa,
                        "qb": qb,
                        "proxy_cpu_s": (after["user_s"] - before["user_s"]) + (after["system_s"] - before["system_s"]),
                        "proxy_rss_peak_kib": peak_rss,
                        "fds_before": before["fds"],
                        "fds_after": after["fds"],
                    }
                    results.append(row)
                    print(
                        f"[EXP3] w={workers:2d} {label:10s}: rps={row['total_rps']:7.1f} "
                        f"p50={med_p50:6.2f}ms p95_max={max_p95:6.2f}ms fail={total_fail} "
                        f"rss={peak_rss}KiB fd={before['fds']}->{after['fds']}",
                        flush=True,
                    )
                finally:
                    stop(proxy)
    finally:
        fixture.close()
    return results


def main():
    prepare()
    exp1 = []
    fixture0 = BenchFixture(slow_query_ms=0)
    try:
        stages = [("S1", 10, 10.0), ("S5", 200, 6.0), ("S_MAX", 0, 5.0)]
        targets = [("baseline", None), ("current", 1), ("current", 2), ("current", 4), ("current", 8)]
        for stage, rate, dur in stages:
            for rep in range(1, 4):
                order = targets if rep % 2 else list(reversed(targets))
                for build, w in order:
                    row = run_exp1_trial(fixture0, build, w, stage, rate, dur)
                    row["repeat"] = rep
                    exp1.append(row)
                    q = row["query"]
                    s = row["submit"]
                    print(
                        f"[EXP1] {stage:5s} rep={rep} {build:8s} w={str(w):4s}: "
                        f"q_rps={q['success_rps']:7.1f} q_p95={q['p95_ms']:6.3f}ms "
                        f"s_p95={s['p95_ms']:6.3f}ms fail={s['failures']}/{q['failures']} "
                        f"rss={row['proxy_rss_peak_kib']}KiB fd={row['fds_before']}->{row['fds_after']}",
                        flush=True,
                    )
    finally:
        fixture0.close()

    exp2 = run_exp2_hol()
    exp3 = run_exp3_saturation()

    summary = {
        "env": {
            "uname": cmd_out(["uname", "-sr"]),
            "nproc": cmd_out(["nproc"]),
            "loadavg": Path("/proc/loadavg").read_text().strip(),
            "ip_local_port_range": Path("/proc/sys/net/ipv4/ip_local_port_range").read_text().strip(),
        },
        "exp1": exp1,
        "exp2": exp2,
        "exp3": exp3,
    }
    (OUT / "results.json").write_text(json.dumps(summary, indent=2))
    print(f"[DONE] Saved benchmark data to {OUT / 'results.json'}", flush=True)


if __name__ == "__main__":
    main()
