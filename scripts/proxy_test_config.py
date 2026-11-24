"""Temporary configuration for regression scripts; never changes user configuration."""
import atexit
from pathlib import Path
import tempfile

_fixture = None


def regression_config():
    global _fixture
    if _fixture is None:
        _fixture = tempfile.TemporaryDirectory(prefix="web-proxy-regression-")
        atexit.register(_fixture.cleanup)
        path = Path(_fixture.name) / "proxy.conf"
        path.write_text(
            "submission 127.0.0.1:8001\n"
            "query-a 127.0.0.1:8002\n"
            "query-b 127.0.0.1:8003\n",
            encoding="ascii",
        )
    return str(Path(_fixture.name) / "proxy.conf")