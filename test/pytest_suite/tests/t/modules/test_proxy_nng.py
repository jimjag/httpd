r"""Phase 1 PoC test for mod_proxy_nng -- nng PUB/SUB between proxy and backend.

The reverse proxy (main server) runs an nng SUBscriber that listens on a
loopback TCP port; a backend vhost runs an nng PUBlisher that dials the proxy
and announces itself once per second (see t/conf/proxy_nng.conf.in).  Both halves
run in the same test instance, driven by the mod_watchdog singleton thread, and
talk over TCP -- proving the PUB/SUB channel end to end.

We assert the same way t/modules/heartbeat.t does for mod_heartbeat/heartmonitor:
snapshot the error_log size, wait while announcements are emitted, then count the
"SUB received: ANNOUNCE" lines the SUBscriber logs at LogLevel proxy_nng:info.

mod_watchdog runs its singleton in a child process, so (like heartbeat.t) this is
skipped under the prefork MPM, where the model differs.
"""

import time
from pathlib import Path

import pytest

from apache_pytest import need_module

NB_SECONDS = 5
# The PUB announces every 1s; allow startup/dial slack and require most to land.
NB_EXPECTED = NB_SECONDS - 3


@need_module("proxy_nng", "watchdog")
def test_proxy_nng(http):
    if http.have_module("mpm_prefork"):
        pytest.skip("proxy_nng PoC not run under the prefork MPM")

    error_log = Path(http.vars("t_logs")) / "error_log"
    start = error_log.stat().st_size if error_log.exists() else 0

    time.sleep(NB_SECONDS)

    with error_log.open("r", errors="replace") as fh:
        fh.seek(start)
        loglines = fh.read().splitlines()

    received = [ln for ln in loglines if "SUB received: ANNOUNCE" in ln]
    assert len(received) >= NB_EXPECTED, (
        f"Expecting at least {NB_EXPECTED} announcements received by the nng "
        f"SUBscriber; seen: {len(received)}")
