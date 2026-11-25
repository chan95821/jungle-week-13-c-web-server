"""Minimal product-entry integration; temporary fixtures, no load/latency metrics."""
import errno
import http.client
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

def free_port():
    with socket.socket() as connection:
        connection.bind(('127.0.0.1', 0))
        return connection.getsockname()[1]

def wait_until(check, message):
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.01)
    raise AssertionError(message)

def listening(port):
    try:
        with socket.create_connection(('127.0.0.1', port), .1):
            return True
    except OSError:
        return False

def fetch(port, path, body=None):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=4)
    try:
        connection.request('POST' if body is not None else 'GET', path, body=body)
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()

def children(pid):
    try:
        return [int(value) for value in Path(f'/proc/{pid}/task/{pid}/children').read_text().split()]
    except FileNotFoundError:
        return []

def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False

def begin_cgi(port, query):
    connection = socket.create_connection(('127.0.0.1', port), 2)
    connection.sendall(f'GET /cgi-bin/fixture?{query} HTTP/1.1\r\nHost: x\r\n\r\n'.encode())
    return connection

def marker_pid(path):
    try:
        return int(path.read_text().strip())
    except (FileNotFoundError, ValueError):
        return None

def main():
    processes = []
    clients = []
    server = None
    with tempfile.TemporaryDirectory(prefix='jungle-process-smoke-') as directory:
        fixture = Path(directory)
        (fixture/'public/assets').mkdir(parents=True)
        (fixture/'public/pages').mkdir()
        (fixture/'cgi-bin').mkdir()
        binary = bytes(range(256)) * 512
        (fixture/'public/assets/fixture.png').write_bytes(binary)
        (fixture/'public/pages/cafe.html').write_text('<h1>fixture</h1>')
        shutil.copy2(ROOT/'build/test_cgi', fixture/'cgi-bin/fixture')
        (fixture/'cgi-bin/broken').write_text('not an executable format\n')
        (fixture/'cgi-bin/broken').chmod(0o700)
        ports = [free_port() for _ in range(4)]
        config = fixture/'proxy.conf'
        config.write_text('worker-policy rr\nquery-policy rr\n'+ ''.join(
            f'{name} 127.0.0.1:{port}\n' for name,port in zip(('submission','query-a','query-b'), ports[1:])))
        with (fixture/'server.log').open('w+') as server_log:
            try:
                for name,port in zip(('submission','query-a','query-b'),ports[1:]):
                    process = subprocess.Popen([str(ROOT/'build/test_backend'),str(port),name,'normal'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                    processes.append(process)
                    wait_until(lambda: listening(port), 'C backend startup')
                server = subprocess.Popen([str(ROOT/'build/server'),str(ports[0]),str(config)],cwd=fixture,stdout=server_log,stderr=server_log)
                wait_until(lambda: listening(ports[0]), 'product server startup')
                workers = wait_until(lambda: children(server.pid) if len(children(server.pid))==4 else None,'four workers')
                assert fetch(ports[0],'/hello')[2] == b'Hello'
                assert fetch(ports[0],'/echo',b'a\0b')[2] == b'a\0b'
                assert fetch(ports[0],'/assets/fixture.png')[2] == binary
                assert b'Alice' in fetch(ports[0],'/greet?name=Alice')[2]
                assert fetch(ports[0],'/api/submissions',b'payload')[0] == 201
                assert fetch(ports[0],'/api/hello')[0] == 200
                status,headers,body = fetch(ports[0],'/cgi-bin/fixture?binary')
                assert status == 201 and body == b'a\0b' and headers.get('Transfer-Encoding') == 'chunked'
                assert fetch(ports[0],'/cgi-bin/fixture?bad')[0] == 502
                assert fetch(ports[0],'/cgi-bin/broken')[0] == 502
                try:
                    fetch(ports[0],'/cgi-bin/fixture?fail')
                    raise AssertionError('nonzero CGI exit must not produce successful terminating chunk')
                except http.client.IncompleteRead:
                    pass
                baseline = {pid:len(list(Path(f'/proc/{pid}/fd').iterdir())) for pid in workers}
                slow = begin_cgi(ports[0],'slow');clients.append(slow)
                slow_pid = wait_until(lambda:marker_pid(fixture/'cgi-slow.pid'),'CGI marker')
                # Four consecutive RR requests visit every worker, including CGI's owner.
                for _ in range(4):
                    assert fetch(ports[0],'/hello')[2] == b'Hello'
                slow.settimeout(.02)
                try:
                    assert not slow.recv(1)
                    raise AssertionError('CGI unexpectedly completed before gate release')
                except TimeoutError:
                    pass
                (fixture/'cgi-release').write_bytes(b'')
                slow.settimeout(4)
                response = http.client.HTTPResponse(slow);response.begin()
                assert response.status == 200 and response.read() == b'slow'
                slow.close();clients.remove(slow)
                (fixture/'cgi-release').unlink()
                wait_until(lambda:not alive(slow_pid),'successful CGI reaped')
                cancel = begin_cgi(ports[0],'cancel');clients.append(cancel)
                cancelled_pid = wait_until(lambda:marker_pid(fixture/'cgi-cancel.pid'),'cancel CGI marker')
                import struct
                cancel.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
                cancel.close();clients.remove(cancel)
                wait_until(lambda:not alive(cancelled_pid),'cancelled CGI reaped')
                wait_until(lambda:all(len(list(Path(f'/proc/{pid}/fd').iterdir())) == baseline[pid] for pid in workers),'worker FD baseline')
                assert all(not children(pid) for pid in workers)
                print('[PASS] product main: four workers, static/echo/proxy, CGI binary/status/query/chunk/error; gated CGI allows all workers to progress; cancellation and FD baseline')
                # Shutdown with a live CGI, and verify known child cleanup.
                (fixture/'cgi-cancel.pid').unlink()
                pending = begin_cgi(ports[0],'cancel');clients.append(pending)
                pending_pid = wait_until(lambda:marker_pid(fixture/'cgi-cancel.pid'),'shutdown CGI marker')
                server.send_signal(signal.SIGTERM)
                assert server.wait(timeout=5) == 0
                wait_until(lambda:not alive(pending_pid) and all(not alive(pid) for pid in workers),'workers/CGI after shutdown')
                print('[PASS] product SIGTERM: pending CGI and four workers reaped; no response splicing after CGI failure')
            except BaseException:
                server_log.flush();server_log.seek(0)
                print('server log:',server_log.read(4096))
                raise
            finally:
                for client in clients:
                    client.close()
                for process in ([server] if server else [])+processes:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill();process.wait()

if __name__ == '__main__':
    main()
