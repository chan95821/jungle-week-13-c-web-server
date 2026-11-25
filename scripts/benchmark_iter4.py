#!/usr/bin/env python3
"""Approved local benchmark orchestration. HTTP workload is generated only by C."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import socket
import subprocess
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'performance'
CLIENT = OUT / 'load_client'
BACKEND = OUT / 'test_backend'
FLAGS = '-std=c11 -Wall -Wextra -Werror -O2'
STAGES = [('S1', 10, 60), ('S2', 25, 8), ('S3', 50, 8), ('S4', 100, 8),
          ('S5', 200, 8), ('S6', 400, 8), ('S_MAX', 0, 5)]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def command(argv, **kwargs):
    return subprocess.check_output(argv, text=True, **kwargs).strip()

def prepare():
    OUT.mkdir(parents=True, exist_ok=True)
    baseline = OUT / 'baseline'
    current = OUT / 'current'
    if baseline.exists() or current.exists():
        raise RuntimeError('Snapshot directories already exist; do not overwrite benchmark evidence')
    archive_path = OUT / 'baseline.tar'
    if not archive_path.exists():
        subprocess.run(['git', '-c', 'safe.directory=' + str(ROOT),
                        '--git-dir=' + str(ROOT / '.git'), '--work-tree=' + str(ROOT),
                        'archive', '--format=tar', '-o', str(archive_path), 'f32e17d'], check=True)
    baseline.mkdir()
    with tarfile.open(archive_path) as archive:
        for member in archive.getmembers():
            if member.name != 'Makefile' and member.name.split('/')[0] not in ('src', 'public', 'cgi-src'):
                continue
            target = (baseline / member.name).resolve()
            if baseline.resolve() not in target.parents or member.issym() or member.islnk():
                raise RuntimeError(f'Unexpected archive member: {member.name}')
            archive.extract(member, baseline)
    current.mkdir()
    shutil.copy2(ROOT / 'Makefile', current / 'Makefile')
    for directory in ('src', 'public', 'cgi-src'):
        shutil.copytree(ROOT / directory, current / directory, ignore=shutil.ignore_patterns('*.o', '__pycache__'))
    manifest = {}
    with (OUT / 'build.log').open('w') as log:
        for name in ('baseline', 'current'):
            snapshot = OUT / name
            subprocess.run(['make', 'CFLAGS=' + FLAGS, 'all'], cwd=snapshot, stdout=log, stderr=log, check=True)
            manifest[name] = {str(p.relative_to(snapshot)): digest(p) for p in snapshot.rglob('*')
                              if p.is_file() and (p.suffix in ('.c', '.h') or p.name == 'Makefile')}
            manifest[name]['binary_sha256'] = digest(snapshot / 'build' / 'server')
        subprocess.run(['gcc', *FLAGS.split(), '-o', str(BACKEND), 'tests/backends/server.c',
                        'src/socket_io.c', 'src/http.c'], cwd=ROOT, stdout=log, stderr=log, check=True)
        subprocess.run(['gcc', *FLAGS.split(), '-pthread', '-o', str(CLIENT),
                        'tests/performance/load_client.c', '-lm'], cwd=ROOT, stdout=log, stderr=log, check=True)
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                        '-fsanitize=address,undefined', '-pthread', '-o', str(OUT / 'load_client_sanitized'),
                        'tests/performance/load_client.c', '-lm'], cwd=ROOT, stdout=log, stderr=log, check=True)
    manifest['fixture'] = {'source_sha256': digest(ROOT / 'tests/backends/server.c'),
                           'binary_sha256': digest(BACKEND)}
    manifest['load_client'] = {'source_sha256': digest(ROOT / 'tests/performance/load_client.c'),
                               'binary_sha256': digest(CLIENT)}
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    print('Isolated baseline/current -O2 builds and C tools compiled.', flush=True)

def unused_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]

def wait_port(port, child):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise RuntimeError(f'Child {child.args} exited with {child.returncode}')
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.1): return
        except OSError: time.sleep(.02)
    raise TimeoutError('startup')

def stop(child):
    if child.poll() is None:
        child.terminate()
        try: child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill(); child.wait()

class Fixture:
    def __init__(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='web-perf-')
        self.directory = Path(self.temporary.name)
        shutil.copytree(OUT / 'current' / 'public', self.directory / 'public')
        self.children = []
        self.logs = []
        self.ports = {}
        self.backends = {}
        try:
            for role in ('submission', 'query-a', 'query-b'):
                self.ports[role] = unused_port()
                self.backends[role] = self.spawn([str(BACKEND), str(self.ports[role]), role, 'benchmark'],
                                                 self.ports[role], role)
            self.config = self.directory / 'proxy.conf'
            self.config.write_text(''.join(f'{role} 127.0.0.1:{port}\n' for role, port in self.ports.items()))
        except BaseException:
            self.close(); raise
    def spawn(self, argv, port, label):
        log = (OUT / f'{label}.stderr.log').open('a')
        self.logs.append(log)
        child = subprocess.Popen(argv, cwd=self.directory, stdout=subprocess.DEVNULL, stderr=log)
        self.children.append(child)
        wait_port(port, child)
        return child
    def close(self):
        for child in reversed(self.children): stop(child)
        for log in self.logs: log.close()
        self.temporary.cleanup()

def client_args(port, alternate, method, rate, seconds, binary=CLIENT):
    return [str(binary), str(port), str(alternate), method, str(rate),
            '2' if method == 'POST' else '16', str(seconds)]

def smoke():
    fixture = Fixture()
    try:
        for method, port, other in (
            ('POST', fixture.ports['submission'], fixture.ports['submission']),
            ('GET', fixture.ports['query-a'], fixture.ports['query-b'])):
            result = json.loads(command(client_args(port, other, method, 10, .3,
                                                    OUT / 'load_client_sanitized')))
            assert result['success'] == 3 and not result['failures'], result
        for build in ('baseline', 'current'):
            port = unused_port()
            proxy = fixture.spawn([str(OUT / build / 'build/server'), str(port), str(fixture.config)], port, 'smoke-' + build)
            for method in ('GET', 'POST'):
                result = json.loads(command(client_args(port, port, method, 10, .3,
                                                        OUT / 'load_client_sanitized')))
                assert result['success'] == 3 and not result['failures'], result
            stop(proxy)
        port = unused_port()
        normal = fixture.spawn([str(BACKEND), str(port), 'query-a'], port, 'smoke-normal')
        result = subprocess.run(client_args(port, port, 'GET', 10, .3, OUT / 'load_client_sanitized'),
                                capture_output=True, text=True)
        data = json.loads(result.stdout)
        assert result.returncode == 1 and data['invalid_responses'] == 3 and not data['success'], data
        stop(normal)
        print('Tool checks PASS: exact POST/GET binary bodies, both snapshots, invalid-body rejection, ASan/UBSan.', flush=True)
    finally: fixture.close()

def process_stats(process):
    raw = Path(f'/proc/{process.pid}/stat').read_text().rsplit(')', 1)[1].split()
    ticks = os.sysconf('SC_CLK_TCK')
    status = Path(f'/proc/{process.pid}/status').read_text().splitlines()
    rss = next(int(line.split()[1]) for line in status if line.startswith('VmRSS:'))
    return {'user_s': int(raw[11]) / ticks, 'system_s': int(raw[12]) / ticks, 'rss_kib': rss,
            'fds': len(list(Path(f'/proc/{process.pid}/fd').iterdir()))}

def run_phase(fixture, proxy, rate, duration):
    resources = dict(fixture.backends)
    if proxy: resources['proxy'] = proxy
    before = {role: process_stats(child) for role, child in resources.items()}
    peak = {role: values['rss_kib'] for role, values in before.items()}
    if proxy:
        submit_port = query_a = query_b = fixture.proxy_port
    else:
        submit_port = fixture.ports['submission']
        query_a, query_b = fixture.ports['query-a'], fixture.ports['query-b']
    usage_before = resource.getrusage(resource.RUSAGE_SELF)
    started = time.monotonic()
    submit = subprocess.Popen(client_args(submit_port, submit_port, 'POST', 2, duration), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    query = subprocess.Popen(client_args(query_a, query_b, 'GET', rate, duration), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    loads = [submit, query]
    try:
        while any(child.poll() is None for child in loads):
            for role, child in resources.items(): peak[role] = max(peak[role], process_stats(child)['rss_kib'])
            if time.monotonic() - started > duration + 8: raise TimeoutError('load generator exceeded duration + request deadline')
            time.sleep(.1)
        metrics = {}
        for role, child in zip(('submit', 'query'), loads):
            output, error = child.communicate()
            if child.returncode not in (0, 1): raise RuntimeError(f'{role} client error: {error}')
            metrics[role] = json.loads(output)
        after = {role: process_stats(child) for role, child in resources.items()}
    finally:
        for child in loads: stop(child)
    elapsed = time.monotonic() - started
    usage_after = resource.getrusage(resource.RUSAGE_SELF)
    metrics['resource_interval_s'] = elapsed
    metrics['processes'] = {role: {
        'cpu_user_s': after[role]['user_s'] - initial['user_s'],
        'cpu_system_s': after[role]['system_s'] - initial['system_s'],
        'rss_start_kib': initial['rss_kib'], 'rss_sampled_peak_kib': peak[role],
        'fds_before': initial['fds'], 'fds_after': after[role]['fds']}
        for role, initial in before.items()}
    metrics['orchestrator_cpu_s'] = (usage_after.ru_utime + usage_after.ru_stime -
                                     usage_before.ru_utime - usage_before.ru_stime)
    return metrics

def environment():
    return {'uname': command(['uname', '-sr']), 'cpu': command(['lscpu']),
            'memory': command(['free', '-m']), 'nproc': command(['nproc']),
            'fd_soft_hard': resource.getrlimit(resource.RLIMIT_NOFILE),
            'ephemeral_ports': Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().strip(),
            'tcp_tw_reuse': Path('/proc/sys/net/ipv4/tcp_tw_reuse').read_text().strip(),
            'loadavg': Path('/proc/loadavg').read_text().strip(),
            'gcc': command(['gcc', '--version']), 'flags': FLAGS}

def run():
    path = OUT / 'results.json'
    if path.exists(): raise RuntimeError('Results already exist; do not overwrite measurements')
    output = {'conditions': {'stages': STAGES, 'warmup_s': 5, 'repeats': 3, 'request_timeout_s': 5,
                              'query_concurrency': 16, 'submit_concurrency': 2, 'response_bytes': 1024,
                              'keep_alive': False, 'rss_sampling_s': .1},
              'environment_start': environment(), 'trials': []}
    fixture = Fixture()
    try:
        jobs = []
        for stage, rate, duration in STAGES:
            if stage in ('S1', 'S5'):
                jobs += [('direct', stage, rate, duration, repeat) for repeat in range(1, 4)]
            for repeat in range(1, 4):
                order = ('baseline', 'current') if repeat % 2 else ('current', 'baseline')
                jobs += [(build, stage, rate, duration, repeat) for build in order]
        for index, (build, stage, rate, duration, repeat) in enumerate(jobs, 1):
            proxy = None
            if build != 'direct':
                fixture.proxy_port = unused_port()
                proxy = fixture.spawn([str(OUT / build / 'build/server'), str(fixture.proxy_port), str(fixture.config)],
                                       fixture.proxy_port, build)
            try:
                print(f'[{index}/{len(jobs)}] {build} {stage} repeat {repeat}: warmup 5s, measure {duration}s', flush=True)
                warmup = run_phase(fixture, proxy, rate, 5)
                measured = run_phase(fixture, proxy, rate, duration)
                row = {'build': build, 'stage': stage, 'repeat': repeat,
                       'warmup': warmup, 'measured': measured, 'loadavg': Path('/proc/loadavg').read_text().strip()}
                output['trials'].append(row)
                path.write_text(json.dumps(output, indent=2))
                s, q = measured['submit'], measured['query']
                print(f'  DONE submit p95={s["p95_ms"]:.3f}ms, query={q["success_rps"]:.1f}rps/{q["p95_ms"]:.3f}ms, failures={s["failures"]}/{q["failures"]}', flush=True)
            finally:
                if proxy: stop(proxy)
        output['environment_end'] = environment()
        output['completed'] = True
        path.write_text(json.dumps(output, indent=2))
        print('All 48 approved measured trials completed.', flush=True)
    finally: fixture.close()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=('prepare', 'smoke', 'run'))
    action = parser.parse_args().action
    {'prepare': prepare, 'smoke': smoke, 'run': run}[action]()