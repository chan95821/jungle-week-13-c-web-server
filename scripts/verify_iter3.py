#!/usr/bin/env python3
"""Iteration 3 static HTTP checks; no load or race-safety claims.

Example after integration:
  python3 scripts/verify_iter3.py --root public --cafe pages/cafe.html \
      --menu pages/menu.html

The default server command is build/server PORT PROXY_CONFIG (temporary config by default). For a different CLI, supply
--server-arg once per argument, using {port} and, if needed, {root}.
All fixtures live in an isolated temporary working directory.
"""
import argparse
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time

from verify_iter1 import SERVER_BIN, pick_free_port
from proxy_test_config import regression_config
from verify_iter2 import ResponseReader


def relative_path(value):
    path = Path(value)
    if path.is_absolute() or not path.parts or any(p in (".", "..") for p in path.parts):
        raise argparse.ArgumentTypeError("use a nonempty relative path without '..'")
    return path


def command(args, port):
    return [str(args.server)] + [
        arg.replace("{port}", str(port)).replace("{root}", str(args.root))
        for arg in (args.server_arg if args.server_arg is not None else ["{port}", regression_config()])
    ]


def start(args, cwd):
    port = pick_free_port()
    proc = subprocess.Popen(command(args, port), cwd=cwd,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 3
    try:
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise AssertionError(f"server exited during startup: {proc.returncode}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return proc, port
            except OSError:
                time.sleep(0.05)
        raise AssertionError("server did not listen within 3 seconds")
    except BaseException:
        stop(proc, require_clean=False)
        raise


def stop(proc, require_clean=True):
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
    try:
        code = proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        if require_clean:
            raise AssertionError("server did not stop after SIGINT")
        return
    if require_clean:
        assert code == 0, f"server exit status: {code}"


def request(port, target, status, body=None, mime=None, method=b"GET"):
    payload = body if method == b"POST" and body is not None else b""
    wire = (method + b" " + target + b" HTTP/1.1\r\nHost: localhost\r\n"
            + b"Content-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload)
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.settimeout(3)
        sock.sendall(wire)
        sock.shutdown(socket.SHUT_WR)
        reader = ResponseReader(sock)
        actual, headers, received = reader.read()
        assert actual == status, (target, actual, status, received)
        assert headers[b"connection"].lower() == b"close", target
        if body is not None and status == 200:
            assert received == body, (target, "body mismatch")
        if mime is not None:
            assert headers[b"content-type"].split(b";", 1)[0].strip().lower() == mime, target
        reader.assert_closed()
        return headers, received


def write(root, name, data):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def invalid_root(args, cwd):
    # A valid relative root's parents must exist for the ordinary-file case.
    root = cwd / args.root
    root.parent.mkdir(parents=True, exist_ok=True)
    for kind in ("missing", "file"):
        if kind == "file":
            root.write_bytes(b"not a directory")
        port = pick_free_port()
        try:
            result = subprocess.run(command(args, port), cwd=cwd, capture_output=True, timeout=3)
        except subprocess.TimeoutExpired as exc:
            raise AssertionError(f"{kind} root did not cause startup failure") from exc
        assert result.returncode != 0, f"{kind} root accepted"
        assert result.stderr or result.stdout, f"{kind} root failure was not reported"
        if kind == "file":
            root.unlink()
    print("[PASS] missing and non-directory root rejected")


def run(args, cwd):
    root = cwd / args.root
    invalid_root(args, cwd)
    root.mkdir(parents=True)

    # A valid root alone is enough to start; pages are checked per request.
    proc, port = start(args, cwd)
    try:
        request(port, b"/cafe", 404)
        request(port, b"/menu", 404)
        request(port, b"/assets/missing.css", 404)
        request(port, b"/hello", 200, b"Hello", b"text/plain")
    finally:
        stop(proc)

    cafe, menu = b"<h1>Cafe fixture</h1>", b"<h1>Menu fixture</h1>"
    write(root, args.cafe, cafe)
    write(root, args.menu, menu)
    assets = root / "assets"
    assets.mkdir(exist_ok=True)
    samples = {
        "nested/page.html": (b"html", b"text/html"),
        "style.CSS": (b"css", b"text/css"),
        "script.Js": (b"js", b"text/javascript"),
        "image.png": (bytes(range(256)) + b"\x00\xff", b"image/png"),
        "one.jpg": (b"jpg", b"image/jpeg"),
        "two.JpEg": (b"jpeg", b"image/jpeg"),
        "empty.css": (b"", b"text/css"),
        "large.js": (bytes(range(256)) * 4097, b"text/javascript"),
    }
    for name, (data, _) in samples.items():
        write(assets, name, data)
    write(assets, "unknown.xyz", b"hidden")
    write(assets, "extensionless", b"hidden")
    write(assets, "style.css.extra", b"hidden")
    write(assets, "with.css/extensionless", b"hidden")
    write(assets, "sub/file.css", b"os-resolution")
    (assets / "sub/deep").mkdir()
    (assets / "internal.css").symlink_to("style.CSS")
    (assets / "link").symlink_to("sub/deep", target_is_directory=True)
    (assets / "broken.css").symlink_to("missing.css")
    outside = root / "outside.css"
    outside.write_bytes(b"outside secret")
    sibling = root / "assets-other"
    sibling.mkdir()
    (sibling / "secret.css").write_bytes(b"sibling secret")
    (assets / "outside.css").symlink_to(outside)
    (assets / "sibling.css").symlink_to(sibling / "secret.css")
    os.mkfifo(assets / "pipe.css")
    denied = assets / "denied.css"
    denied.write_bytes(b"permission secret")
    denied.chmod(0)

    proc, port = start(args, cwd)
    try:
        request(port, b"/cafe", 200, cafe, b"text/html")
        request(port, b"/menu", 200, menu, b"text/html")
        for name, (data, mime) in samples.items():
            request(port, b"/assets/" + name.encode(), 200, data, mime)
        request(port, b"/assets/internal.css", 200, b"css", b"text/css")
        request(port, b"/assets/style.CSS?x=%61", 200, b"css", b"text/css")
        request(port, b"/assets/nested/../style.CSS", 200, b"css", b"text/css")
        request(port, b"/assets/link/../file.css", 200, b"os-resolution", b"text/css")
        print("[PASS] page mapping, MIME, binary, empty and large files")

        missing_headers, missing_body = request(port, b"/assets/not-found.css", 404)
        for target in (b"/assets", b"/assets/", b"/assets/nested", b"/assets-old/style.CSS",
                       b"/pages/not-registered.html", b"/cafeteria", b"/assets/unknown.xyz",
                       b"/assets/extensionless", b"/assets/style.css.extra",
                       b"/assets/with.css/extensionless", b"/assets/broken.css", b"/assets/pipe.css",
                       b"/assets/outside.css", b"/assets/sibling.css", b"/assets/../outside.css",
                       b"/assets/../assets-other/secret.css", b"/%61ssets/style.CSS"):
            request(port, target, 404)
        for target in (b"/assets/outside.css", b"/assets/sibling.css", b"/assets/../outside.css"):
            headers, body = request(port, target, 404)
            assert body == missing_body, target
            for name in (b"content-type", b"content-length", b"connection"):
                assert headers.get(name) == missing_headers.get(name), (target, name)
        # chmod is ineffective for privileged processes; do not claim this case passed.
        if os.access(denied, os.R_OK, effective_ids=True):
            print("[SKIP] access-denied policy: server credentials can read chmod 000")
        else:
            headers, body = request(port, b"/assets/denied.css", 404)
            assert body == missing_body
            for name in (b"content-type", b"content-length", b"connection"):
                assert headers.get(name) == missing_headers.get(name), name
            print("[PASS] personal access-denied 404 policy (team 403 remains unresolved)")
        print("[PASS] fixed filesystem boundaries; no realpath/open race guarantee")

        for target in (b"/assets/%73tyle.CSS", b"/assets/a%2fb.css",
                       b"/assets/%2e%2e/outside.css", b"/assets/a%25.css",
                       b"/assets/bad%xy.css"):
            request(port, target, 400)
        request(port, b"/assets%2Fstyle.CSS", 404)
        for target in (b"/", b"/hello", b"/hello?x=1"):
            request(port, target, 200, b"Hello", b"text/plain")
        for target in (b"/%68ello", b"/a/../hello"):
            request(port, target, 404)
        for method, target, allowed in ((b"GET", b"/echo", b"POST"),
                                        (b"POST", b"/hello", b"GET")):
            headers, _ = request(port, target, 405, method=method)
            assert {x.strip() for x in headers[b"allow"].split(b",")} == {allowed}
        request(port, b"/hello", 501, method=b"DELETE")
        for data in (b"", b"\x00binary\xff\r\n"):
            request(port, b"/echo", 200, data, b"application/octet-stream", b"POST")
        print("[PASS] encoding and existing HTTP routing/echo regression")
    finally:
        denied.chmod(0o600)
        stop(proc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=relative_path)
    parser.add_argument("--cafe", required=True, type=relative_path)
    parser.add_argument("--menu", required=True, type=relative_path)
    parser.add_argument("--server", type=Path, default=Path(SERVER_BIN))
    parser.add_argument("--server-arg", action="append")
    args = parser.parse_args()
    args.server = args.server.resolve()
    pages = (args.cafe, args.menu)
    reserved = {"assets", "assets-other", "outside.css"}
    if (any(p.parts[0] in reserved for p in pages)
            or args.cafe == args.menu
            or args.cafe in args.menu.parents or args.menu in args.cafe.parents):
        parser.error("page paths must be distinct files outside reserved fixture paths")
    if args.server_arg is not None and not any("{port}" in a for a in args.server_arg):
        parser.error("--server-arg must include a {port} placeholder")
    with tempfile.TemporaryDirectory(prefix="verify-iter3-") as tmp:
        run(args, Path(tmp))
    print("Static checks complete; dynamic, load, race and internal fault cases not covered.")


if __name__ == "__main__":
    main()
