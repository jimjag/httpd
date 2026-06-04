r"""Test for mod_proxy_nng -- nng PUB/SUB backend announcement -> balancer member.

The reverse proxy (main server) runs an nng SUBscriber listening on a loopback
TCP port; a backend vhost runs an nng PUBlisher that dials the proxy and
announces a routable URL once per second (see t/conf/proxy_nng.conf.in). Both
halves run in the same instance, driven by the mod_watchdog singleton thread.

Phase 1 proved the channel: the SUB logs each "ANNOUNCE" it receives.
Phase 2: on receiving an announcement carrying url=, the proxy adds that backend
as a member of balancer://nng (reusing mod_proxy_balancer's balancer_manage) and
enables it -- so a request to /nng routes to the dynamically-added backend.

Verified the same way t/modules/heartbeat.t verifies mod_heartbeat: snapshot the
error_log, wait while announcements flow, then scan the log. We additionally
drive a real request through balancer://nng to prove the added member serves.

mod_watchdog runs its singleton in a child process, so (like heartbeat.t) this
is skipped under the prefork MPM.
"""

import time
from pathlib import Path

import pytest

from apache_pytest import need_module, t_cmp

NB_SECONDS = 5
NB_EXPECTED = NB_SECONDS - 3


@need_module("proxy_nng", "watchdog", "proxy_balancer", "proxy_http")
def test_proxy_nng(http):
    if http.have_module("mpm_prefork"):
        pytest.skip("proxy_nng not run under the prefork MPM")

    error_log = Path(http.vars("t_logs")) / "error_log"
    start = error_log.stat().st_size if error_log.exists() else 0

    # Let several announcements flow and the member get added + enabled.
    time.sleep(NB_SECONDS)

    with error_log.open("r", errors="replace") as fh:
        fh.seek(start)
        loglines = fh.read().splitlines()

    # Phase 1: announcements are received, and now carry a routable url=.
    received = [ln for ln in loglines if "SUB received: ANNOUNCE" in ln]
    assert len(received) >= NB_EXPECTED, (
        f"Expecting >= {NB_EXPECTED} announcements received; "
        f"seen: {len(received)}")
    assert any("url=" in ln for ln in received), (
        "announcements should carry a url= token for Phase 2")

    # Phase 2a: the backend was added to the balancer -- exactly once, despite
    # re-announcing every second (verifies the ctx->seen dedup).
    added = [ln for ln in loglines if "added backend" in ln]
    assert len(added) == 1, (
        f"backend should be added exactly once; saw {len(added)}: {added}")

    # No repeated add-failure spam (would indicate the dedup/idempotence broke).
    failures = [ln for ln in loglines if "failed to add worker" in ln]
    assert not failures, f"unexpected add-failure log lines: {failures}"

    # Phase 2b (strongest): a request routed through balancer://nng reaches the
    # dynamically-added backend (the main server), proving it actually serves.
    vars_ = http.vars()
    expected_body = f"welcome to {vars_['servername']}:{vars_['port']}"
    r = http.GET("/nng/index.html")
    assert t_cmp(r.status_code, 200), "request through balancer://nng failed"
    assert expected_body in r.text, (
        f"expected backend body {expected_body!r}, got {r.text!r}")
