# NNG PUB/SUB for httpd Reverse-Proxy Balancers — Verified Findings

Status: code-verified against this checkout (`trunk`, June 2026). This document
builds on the earlier desk review at `~/Desktop/nng-pubsub-proxy-analysis.md`,
**corrects two material errors in it**, and records the architecture facts that
any nng integration must respect.

---

## 0. Corrections to the preliminary review

The earlier draft is broadly accurate on data structures, slotmem, and the nng
API. Two claims are wrong and would derail an implementation:

### ❌ Error 1 — "The parent process is the publisher"

The draft's central diagram (§1.8, §1.16) puts `nng_pub0_open()` /
`nng_listen()` in the **parent (master) process** and has the BalancerManager
publish from there. **This cannot work as drawn.**

In httpd, the master process never serves requests. The BalancerManager
(`balancer_handler`) is an ordinary HTTP handler registered with
`ap_hook_handler` (`mod_proxy_balancer.c:2057`) and therefore runs **inside
whichever child process happens to accept the admin request**. The worker-add
code path (`balancer_process_balancer_worker`, `mod_proxy_balancer.c:1098`)
executes in that child, writes to slotmem, and bumps the timestamp
(`mod_proxy_balancer.c:1337`):

```c
bsel->wupdated = bsel->s->wupdated = nworker->s->updated = apr_time_now();
```

The other children then notice the bumped `s->wupdated` on their next sync.
So the producer of change events is **a child**, and the consumers are **its
sibling children**. The publisher must live in the child that mutates the
balancer (or events must be relayed to the master and fanned back out). A naive
"publish from parent" design has no code path that ever fires.

> Implication: either (a) the publishing child opens a PUB socket, or (b) use a
> single well-known PUB endpoint owned by the master and have the mutating child
> hand the event to the master (pipe/inproc) for rebroadcast. Option (b) is
> cleaner but needs an inter-process hop the draft didn't account for.

### ❌ Error 2 — "Removing a worker: `storage->release(...)`"

The draft (§1.5) shows a runtime worker-removal flow calling
`storage->release(bsel->wslot, index)`. **No such call exists.** Grep of
`mod_proxy_balancer.c` finds `storage->grab` and `storage->num_free_slots` but
**zero `storage->release`** in the manager path. Runtime workers can only be
**added** and **status-changed** (DISABLED / STOPPED / DRAIN / ENABLED) — never
removed from the slot array while running. The `(Use '-' to delete)` hint at
`mod_proxy_balancer.c:1849` concerns LBSet/route field editing, not slot
release. A worker, once added at runtime, occupies its slot until restart.

> Implication: the message protocol's `REMOVE` verb (§1.15) has no current
> backend operation behind it. An honest first cut publishes `ADD`,
> `ENABLE`/`DISABLE`, `STATUS-CHANGE`, and the health-check `FAIL`/`RECOVER`
> events — not `REMOVE`.

Everything else in the draft (structures, slotmem provider ops, nng API usage,
subscriber-side filtering having no bandwidth benefit) checks out.

---

## 1. The mechanism nng would replace (verified call sites)

Discovery of dynamically-added workers is **pull/poll**, keyed on a timestamp.
`ap_proxy_sync_balancer` (`proxy_util.c:4386`) short-circuits unless the shared
timestamp is newer than the local one:

```c
if (b->s->wupdated <= b->wupdated)
    return APR_SUCCESS;
```

When it does fire, it walks every slot in the worker slotmem and grafts any
worker missing from this child's local array (`proxy_util.c:4441-4461`).

It is invoked from exactly four places (verified):

| Caller | File:line | Frequency |
|---|---|---|
| `proxy_balancer_pre_request` | `mod_proxy_balancer.c:498` | **every proxied request** |
| `balancer_handler` | `mod_proxy_balancer.c:1926` | every balancer-manager hit |
| `balancer_post_config` (persist path) | `mod_proxy_balancer.c:1035` | startup |
| hcheck watchdog loop | `mod_proxy_hcheck.c:1047` | every watchdog tick |

The hot path is the per-request call. Note the existing in-tree TODO at
`mod_proxy_balancer.c:497` — *"Step 3.5: Update member list … TODO: Implement as
provider!"* — which is precisely the seam an nng/event-driven mechanism slots
into. The `wupdated` guard already makes the steady-state cost cheap (one
compare), so the *real* win from nng is **cross-machine** propagation and
**reduced discovery latency**, not local CPU.

Timestamp write points (the natural publish triggers), all verified:

| File:line | Event |
|---|---|
| `mod_proxy_balancer.c:999` | startup share of balancer |
| `mod_proxy_balancer.c:1231` | manager status edit of a worker |
| `mod_proxy_balancer.c:1337` | **runtime worker ADD** |

These three are where a `proxy_nng_publish(...)` call would be inserted.

---

## 2. nng PUB/SUB — confirmed semantics relevant here

From the nng docs (verified via the live pages):

- **Publisher does no filtering.** `nng_pub0` delivers every message to every
  connected subscriber; the *subscriber* filters by topic prefix
  (`NNG_OPT_SUB_SUBSCRIBE`). So topics save no bandwidth — they only save the
  subscriber a parse. For our handful of event types that's fine.
- **Topic = leading bytes of the message body.** Put the topic first.
- **PUB sockets are send-only**; recv returns `NNG_ENOTSUP`.
- **No delivery/order guarantee; late joiners miss prior messages.** A
  subscriber that connects after an `ADD` never sees it. → the `wupdated` poll
  **must remain** as the source of truth / catch-up path. nng is an *accelerator
  and a fan-out-across-hosts* layer, not a replacement for slotmem.
- API (from the getting-started example): `nng_pub0_open` → `nng_listen`;
  `nng_sub0_open` → `nng_setopt(NNG_OPT_SUB_SUBSCRIBE, "", 0)` → `nng_dial` →
  `nng_recv(..., NNG_FLAG_ALLOC)` → `nng_free`. (Note: `nng_setopt` is the
  legacy spelling; nng ≥1.5 prefers `nng_socket_set` / `nng_sub0_socket_set`.)

---

## 3. Corrected target architecture

```
        ┌──────────────────────── master (no request handling) ────────────────┐
        │  optional relay: owns the canonical PUB endpoint, rebroadcasts        │
        │  events received from the mutating child over inproc/pipe             │
        └───────────────▲───────────────────────────────┬──────────────────────┘
                        │ event (mutating child → master)│ PUB fan-out
   ┌────────────────────┴────────┐        ┌──────────────▼───────────────┐
   │ child A  (served the admin   │        │ child B … N                  │
   │ request → ran ADD)           │        │ SUB: nng_dial(endpoint)      │
   │  • mutates slotmem           │        │ recv → set local wupdated--  │
   │  • bumps s->wupdated (1337)  │        │       → ap_proxy_sync_balancer│
   │  • emits ADD event           │        │ (reactive, off the hot path) │
   └──────────────────────────────┘        └──────────────────────────────┘
```

Two viable topologies:

1. **Master-owned endpoint (recommended).** Master opens the PUB socket in
   `post_config`/child-spawn coordination and `nng_listen`s on a stable
   `ipc://` path (local) or `tcp://` (cross-host). The mutating child sends its
   event to the master (existing httpd pattern: a pipe, or an `inproc://` nng
   leg) which rebroadcasts. Stable endpoint, single listener, survives child
   recycling. Costs one extra hop + the relay code.

2. **Per-child PUB (simplest prototype).** Every child both PUBs and SUBs on a
   shared `ipc://` rendezvous. Avoids the master relay but means N listeners and
   messier bind/connect choreography under MPM child churn. Fine for a spike,
   not for production.

Either way the subscriber side is identical and minimal: on message receipt,
force a sync by clearing the local high-water mark and calling the existing
function — i.e. reuse, not rewrite:

```c
/* in the SUB recv loop, per affected balancer */
balancer->wupdated--;                 /* defeat the >= guard */
ap_proxy_sync_balancer(balancer, s, conf);
```

This keeps slotmem as the single source of truth and uses nng purely as the
"poke." It also means the fallback is automatic: if nng dies, the per-request
poll still works exactly as today.

---

## 4. Honest event protocol (only verbs with real backends today)

```
topic    verb            payload
-------  --------------  ---------------------------------
bal      ADD             balancer://cluster|http://h:port|lf=100   (mod_proxy_balancer.c:1337)
bal      STATUS          balancer://cluster|http://h:port|D|S|R|N  (status edit, :1231)
hc       FAIL            balancer://cluster|http://h:port          (mod_proxy_hcheck.c)
hc       RECOVER         balancer://cluster|http://h:port
```

`REMOVE` is intentionally omitted (see Correction 2). Add it only if/when a
slot-release path is implemented in the manager.

Subscribers `NNG_OPT_SUB_SUBSCRIBE` to `""` (all) for the prototype; topic
prefixes `bal`/`hc` let a child opt out of HC noise later.

---

## 5. Integration points (revised from draft §1.9)

| File | Function | Change |
|---|---|---|
| `mod_proxy_balancer.c` | `balancer_child_init` (`:1998`) | after `storage->attach`, start SUB thread, `nng_dial` the endpoint |
| `mod_proxy_balancer.c` | `balancer_process_balancer_worker` (`:1337`) | after the `wupdated` bump, publish `ADD` |
| `mod_proxy_balancer.c` | status-edit path (`:1231`) | publish `STATUS` |
| `mod_proxy_hcheck.c` | watchdog transition (`~:1047`) | publish `FAIL`/`RECOVER` |
| `proxy_util.c` | `ap_proxy_sync_balancer` (`:4386`) | unchanged; just gets *called* reactively. Honor the `:497` "implement as provider" TODO |
| new `mod_proxy_nng.{c,h}` | — | PUB/SUB lifecycle, thread, parse → sync dispatch |
| `mod_proxy.c` directives | — | `ProxyNngEndpoint ipc:///var/run/httpd-bal.pub` (one directive; derive listen vs dial from role) |

Lifecycle, corrected: SUB thread starts in **child_init** (after slotmem
attach), tears down in **child_exit**. PUB endpoint, if master-owned, opens once
the master knows it will spawn children; if per-child, opens in child_init too.
**Not** `post_config` for the listener if you want it owned by a long-lived
process under graceful restart — post_config runs twice and the master may
re-exec.

---

## 6. Risks specific to this codebase (beyond the draft's generic list)

1. **Master-recycle / graceful restart.** httpd re-execs the master on
   `SIGHUP`; an `ipc://` socket path must be re-bindable (unlink stale node) or
   the relisten fails. Handle `EADDRINUSE`.
2. **MPM child churn.** `MaxConnectionsPerChild` recycles children constantly.
   SUB sockets must be created per child and cleaned in `child_exit`; a leaked
   `nng_dial` per recycle is a real fd/thread leak over hours.
3. **Thread model.** `event`/`worker` MPMs are threaded; `prefork` is not. The
   nng recv loop runs on its own `nng_thread`, but its callback mutates the
   balancer worker array — the existing code already guards that with
   `proxy_mutex` (global) inside `ap_proxy_sync_balancer` (`:4448`), so reusing
   that function keeps the locking correct. Do **not** mutate the array from the
   nng thread directly.
4. **Build/deploy.** nng is a new external dependency (`-lnng`). Needs
   `config.m4` plumbing and an `--with-nng` opt-in; httpd ships no third-party
   transport today, so this is a packaging decision, not just code.
5. **Security.** A `tcp://` PUB endpoint is an unauthenticated control-plane
   broadcast of backend topology. Default to `ipc://` with filesystem perms;
   require `tls://` + verified peers for any cross-host use.

---

## 7. Recommended minimal prototype (scoped honestly)

1. New `mod_proxy_nng.c`: per-child SUB on a configurable `ipc://` endpoint;
   recv loop parses `bal|ADD|...` and calls `wupdated--; ap_proxy_sync_balancer`.
2. PUB: per-child for the spike (topology 2). Insert one `publish()` at
   `mod_proxy_balancer.c:1337`.
3. Demonstrate: add a BalancerMember via the manager on one child; confirm a
   sibling child picks it up *without* waiting for its next proxied request
   (i.e., before the poll would have fired). Keep the poll on — show fallback by
   killing the nng endpoint.
4. Defer: master-owned relay, `REMOVE`, TLS, sequence numbers, HC events.

This is faithful to what the code can actually do today and avoids the two
dead-ends in the original draft.

---

## 7b. How httpd already makes outbound connections (reuse, don't reinvent)

A key design question for nng: do we use nng's own transports (`tcp://`,
`ipc://`, `tls://`), or reuse httpd's connection machinery? **Use nng's
transports** — nng owns its socket lifecycle and threading. But it's worth
recording the in-tree pattern, both as the conceptual template and because the
`ipc://` choice mirrors httpd's existing Unix-domain-socket support.

The canonical outbound-TCP path lives in `proxy_util.c` (all verified present):

| Function | Line | Role |
|---|---|---|
| `ap_proxy_acquire_connection` | `2773` | get/create a `proxy_conn_rec` from the worker's pool (`apr_reslist` when threaded, single conn under prefork) |
| `ap_proxy_determine_address` | `2958` | `apr_sockaddr_info_get` → `conn->addr`, with TTL address caching |
| `ap_proxy_connect_backend` | `3782` | the real socket work: `apr_socket_create` → `apr_socket_opt_set` (`APR_TCP_NODELAY`, `APR_SO_KEEPALIVE`, `APR_SO_RCVBUF`) → `apr_socket_timeout_set` → `apr_socket_connect`, looping over the resolved address list |
| `ap_proxy_connection_create[_ex]` | `4188/4195` | wrap the raw `apr_socket_t` in a `conn_rec` + filter chain |
| `ap_proxy_release_connection` | `2826` | return to pool or tear down |

`proxy_conn_rec` (`mod_proxy.h:302`) carries `apr_socket_t *sock`,
`apr_sockaddr_t *addr`, `conn_rec *connection`, the owning `proxy_worker`, and
pools; `proxy_conn_pool` (`mod_proxy.h:310`) holds the `apr_reslist_t *res`
(threaded) or single `conn` (prefork). `ap_sock_disable_nagle`
(`server/mpm_common.c`) is the in-tree helper that sets `APR_TCP_NODELAY`.

**Relevance to nng:** none of this is needed for the nng leg — `nng_dial` /
`nng_listen` replace the whole chain. What carries over is the *threading and
pooling discipline*: httpd already isolates backend sockets in subpools with
explicit cleanup and reslist bounds. The nng SUB thread and socket must follow
the same hygiene (own pool, `child_exit` teardown, no shared mutable state with
request threads except via `proxy_mutex`). The fact that proxy already speaks
Unix domain sockets to backends (`ap_proxy_connect_uds`, UDS worker syntax) also
means an `ipc://` nng endpoint is consistent with existing operational
expectations, not a novel transport for operators to reason about.

## 9. Phase 1 PoC — implementation log

Goal: prove an nng PUB/SUB channel works end-to-end across *separate, remote*
httpd servers over TCP, before any balancer integration.

### Role inversion (important)

The user's clarified intent is **"backends announce themselves to the proxy."**
An nng PUB socket is **send-only**, so the data direction (backend → proxy)
forces the roles to invert from the original "proxy is publisher" framing:

| Server | nng role | Socket op | Behavior |
|---|---|---|---|
| **Backend balancer** | **PUB** (`nng_pub0_open`) | **dials** the proxy (`nng_dial`) on startup | sends `ANNOUNCE host=… pid=… seq=N` every interval |
| **Reverse proxy** | **SUB** (`nng_sub0_open`) | **listens** (`nng_listen`), stable rendezvous | receives + logs each backend's announcement |

One proxy SUB listener; many backend PUB dialers connect in. (nng supports
SUB-listen / PUB-dial.) This is the minimal correct PoC for the chosen
direction; bidirectional membership is deferred.

### Backbone

Reuses the **mod_watchdog singleton** pattern from `mod_proxy_hcheck.c`
(verified precedent): `post_config` retrieves the watchdog optional fns, gets a
singleton instance (`get_instance(&wd,"_proxy_nng_",0,1,p)` — one child owns the
socket, mutex-protected), and registers a callback per canonical server that has
an nng directive. Callback states STARTING (open+listen/dial) / RUNNING
(throttled send | nonblock drain) / STOPPING (close).

### Directives (per-server, RSRC_CONF, URLs mutually exclusive per server)

- `ProxyNngSubscribe <url>` — proxy SUB listen addr, e.g. `tcp://0.0.0.0:5555`
- `ProxyNngPublish <url>` — backend PUB dial addr, e.g. `tcp://proxy-host:5555`
- `ProxyNngInterval <seconds>` — backend announce period (default 5)

### nng 1.11 API notes (verified against installed headers)

- `nng_pub0_open` / `nng_sub0_open`; subscribe via the dedicated
  `nng_sub0_socket_subscribe(sock, "", 0)` (empty prefix = all) — cleaner than
  generic `nng_socket_set(NNG_OPT_SUB_SUBSCRIBE,...)`.
- `nng_listen(sock,url,NULL,0)` / `nng_dial(sock,url,NULL,0)` (dial NONBLOCK +
  auto-retry by default).
- `nng_send(sock,buf,len,0)`; `nng_recv(sock,&buf,&sz,NNG_FLAG_ALLOC|NNG_FLAG_NONBLOCK)`
  returns `NNG_EAGAIN`(8) when empty; free with `nng_free(buf,sz)`.
- Header: `/opt/homebrew/Cellar/nng/1.11/include`; lib: `…/lib/libnng.dylib`.

### Files (this PoC)
- new `modules/proxy/mod_proxy_nng.c`
- `acinclude.m4` — `APACHE_CHECK_NNG` macro + `--with-nng`
- `modules/proxy/config.m4` — `APACHE_MODULE(proxy_nng, …, no, …, [proxy,watchdog])`
- build: `./buildconf` then `./configure --enable-proxy --enable-watchdog --enable-proxy-nng=shared --with-nng=/opt/homebrew/Cellar/nng/1.11`

### Build wiring (implemented & verified)

- `acinclude.m4`: added `APACHE_CHECK_NNG` + `--with-nng=PATH`. **Modeled on
  `APACHE_CHECK_OPENSSL` (mod_ssl), NOT serf** — serf's `APR_ADDTO(MOD_INCLUDES,…)`
  is unreliable because `MOD_INCLUDES` is referenced in `build/rules.mk` but
  never emitted to `config_vars.mk` (absent from the `APACHE_GEN_CONFIG_VARS`
  subst list), so make expands it to empty. The openssl pattern puts the include
  on global `CPPFLAGS` (which *is* `APACHE_SUBST`'d) and the lib on `NNG_LIBS`.
- `modules/proxy/config.m4`: `APACHE_MODULE(proxy_nng, …, no, [APACHE_CHECK_NNG;
  APR_ADDTO(MOD_PROXY_NNG_LDADD,[$(NNG_LIBS)]) …], [proxy,watchdog])`. Default
  `no` ⇒ only builds with `--enable-proxy-nng`.
- `configure.in` needs **no** edit — like openssl/serf, the check macro lives in
  `acinclude.m4` and is invoked from `config.m4`; `./buildconf` regenerates
  `configure` from both (verified `--with-nng`/`proxy_nng` appear in `configure`).
- Module compiles clean and `otool -L mod_proxy_nng.so` shows it links
  `libnng.1.dylib`. Two source fixes needed: include `http_core.h` (for
  `ap_state_query`/`AP_SQ_*`) and `<unistd.h>` (for `getpid`).

### End-to-end test (pytest_suite) — PASSING

Leveraged the existing `test/pytest_suite` framework, modeled on
`t/modules/test_heartbeat.py` (the closest precedent: a watchdog-driven
background-messaging module verified by grepping the error_log):

- `t/conf/proxy_nng.conf.in`: main server = SUB (`ProxyNngSubscribe
  tcp://127.0.0.1:${NNG_PORT}`), a vhost = PUB (`ProxyNngPublish` same port,
  `ProxyNngInterval 1`), `LogLevel proxy_nng:info`. Both halves in one instance,
  talking over loopback TCP via the watchdog singleton thread.
- `tests/t/modules/test_proxy_nng.py`: snapshots error_log size, sleeps 5s,
  counts `SUB received: ANNOUNCE` lines; asserts ≥ (seconds−3). Skips under
  prefork MPM (like heartbeat.t).

Result: **1 passed**. Error log confirms the full lifecycle —
`PUB dialing …:8556`, `SUB listening on …:8556`, then 5 announcements
`SUB received: ANNOUNCE host=localhost pid=59423 seq=0..4` ~1s apart.

Run: `cd test/pytest_suite && ./runtests.sh --apxs /tmp/httpd-nng-poc/bin/apxs
tests/t/modules/test_proxy_nng.py -v`

Test-harness gotchas: the pytest probe only inherits **active** (uncommented)
`LoadModule` lines from the install `httpd.conf`; `--enable-load-all-modules`
does not activate default-`no` modules like `proxy_nng`. For the run, the
needed modules were uncommented in `/tmp/httpd-nng-poc/conf/httpd.conf`
(slotmem_shm, watchdog, proxy*, proxy_nng, session*).

Status: **Phase 1 complete** — PUB/SUB channel proven over TCP. Configured for
loopback here; cross-host is the same config with the backend's `ProxyNngPublish`
pointing at the proxy host's IP (see §7 verification recipe).

## 10. Phase 2 — announced backend becomes a balancer member (DONE)

Phase 2 closes the loop: when the proxy SUB receives an announcement carrying a
routable URL, it adds that backend as a member of a configured balancer and
enables it — driven by the backend announcing itself.

### Reuse strategy (user-chosen)
Reuse mod_proxy_balancer's exported optional fn **`balancer_manage(request_rec*,
apr_table_t*)`** — the same core the balancer-manager UI calls
(`balancer_process_balancer_worker`). It skips the CSRF nonce/Referer (those
guard the web UI only; this is a trusted internal path). We do **not** issue a
real HTTP request and do **not** re-implement the add primitives. Two calls:
- add: `{ b=<name>, b_nwrkr=<url>, b_wyes=1 }` (worker created DISABLED)
- enable: `{ b=<name>, w=<url>, w_status_D=0 }` (clears DISABLED → usable)

### The one real hazard: synthetic request_rec
The SUB runs in the watchdog thread with no `request_rec`, but `balancer_manage`
logs via `ap_log_rerror`. Verified in `server/log.c`: `log_error_core` asserts
`r->connection != NULL`, `do_errorlog_default` reads `r->connection->outgoing`
unconditionally, and `add_log_id`→`core_generate_log_id` reads
`c->current_thread`. So `nng_make_fake_request` (modeled on
`mod_proxy_hcheck.c:344 create_request_rec`, plus a fake `conn_rec` that hcheck
doesn't need because it attaches a real backend conn) sets a non-NULL conn_rec
with `->outgoing` and pre-sets `r->log_id`/`c->log_id` to skip log-id generation.
Confirmed crash-free at runtime.

### New directives
- `ProxyNngAdvertise <url>` (backend/PUB) — routable origin to advertise,
  uri-validated at config time. Payload becomes
  `ANNOUNCE url=http://host:port host=.. pid=.. seq=..` (Phase-1 payload kept
  when unset → backward compatible).
- `ProxyNngBalancer <name>` (proxy/SUB) — target balancer (bare name; a leading
  `balancer://` is stripped). Without it, SUB stays Phase-1 log-only.

### Idempotence
`ctx->seen` (apr_hash) tracks added URLs so a backend re-announcing every
interval is added exactly once — and avoids misleading per-interval
"failed to add" ERR spam once slots fill. The add bumps `bsel->wupdated` in
slotmem, so all children pick up the new worker via `ap_proxy_sync_balancer`.

### Build/deps
config.m4 dep list → `[proxy,watchdog,proxy_balancer]`; hook `aszSucc` gains
`mod_proxy_balancer.c`. `balancer_manage` retrieved lazily via
`APR_RETRIEVE_OPTIONAL_FN` on first announcement.

### End-to-end test — PASSING
Extended `t/conf/proxy_nng.conf.in` (empty `balancer://nng` with `growth=10`,
`ProxyPass /nng balancer://nng`, `ProxyNngBalancer nng`; backend
`ProxyNngAdvertise http://@SERVERNAME@:@PORT@` → the main server's index.html)
and `tests/t/modules/test_proxy_nng.py`. Asserts: announcements carry `url=`;
backend added **exactly once** (dedup); no add-failure spam; and
`GET /nng/index.html` returns 200 with the backend body — proving the
dynamically added+enabled member actually serves. Error log confirms:
```
SUB received: ANNOUNCE url=http://localhost:8529 ... seq=0..4
added backend http://localhost:8529 to balancer://nng     (once; 0 failures)
```

Status: **Phase 2 complete.**

## 11. Phase 3 — evict a backend that stops announcing (DONE)

The inverse of Phase 2: when a backend stops announcing for longer than a
timeout, take it out of rotation; re-enable it if it starts announcing again.
Gives the cluster automatic liveness with no operator action.

### Mechanism (reuses Phase-2 machinery)
Runtime worker *removal* doesn't exist in httpd, so eviction is a **status-flag
flip** — exactly what mod_proxy_hcheck does on health-check failure
(`mod_proxy_hcheck.c:967`). Via the same `balancer_manage`:
- evict: `{ b, w=<url>, w_status_D="1" }` → DISABLED (out of rotation)
- re-enable: `{ b, w=<url>, w_status_D="0" }`

`w_status_D` set=1/0 go through identical code (`mod_proxy_balancer.c:1147`); the
disable→enable transition sets `need_reset` so the lb method re-syncs. Disabling
only flips the shm bit — in-flight requests unaffected. The Phase-2
`nng_make_fake_request` + `manage_fn` cover it; the add/enable/evict/re-enable
calls were factored into one `nng_set_worker_disabled(ctx,pool,url,disabled)`.

### State (per-URL, in the watchdog singleton's ctx — no shared memory)
`ctx->seen` now maps url → `nng_member_t { apr_time_t last_seen; int added;
int evicted; }`. `evicted` gates redundant flips (don't re-disable every sweep
tick / re-enable every announce). `last_seen` is set to *now* at add time so a
just-added member isn't immediately swept.
- on ANNOUNCE: new → add+enable+record; existing → refresh `last_seen`, and if
  `evicted` re-enable + clear.
- sweep (in the SUB branch, throttled ~1s via `last_sweep`): skip if
  `evict_timeout==0`; disable members with `added && !evicted &&
  now-last_seen > evict_timeout`.

### Directive (scope: directive-configured, live-tuning deferred)
`ProxyNngTimeout <secs>` (proxy/SUB). **Default 0 ⇒ eviction disabled ⇒ pure
Phase-2 behavior** (backward compatible). The user explicitly deferred a
*live-tunable* timeout; when revisited it moves to a 1-slot named slotmem (so the
arbitrary handler child and the watchdog singleton share it) + a small
`proxy-nng-manager` handler. No shared memory / handler / child_init needed now.

### End-to-end test — PASSING
Single-instance harness can't make a backend go silent, so the test creates a
*gap*: `ProxyNngTimeout 1` on the proxy + `ProxyNngInterval 4` on the backend ⇒
each cycle is add → (stale ~1s) evict → next announce re-enables.
`test_proxy_nng.py` polls `/nng/index.html` for a 200 with the backend body
(serves while enabled), then scans the error_log for the full lifecycle. Log
confirms:
```
added backend http://localhost:8529 to balancer://nng
evicted backend http://localhost:8529 ... (no announcement for 1s)
re-enabled backend http://localhost:8529 ... (announcing again)
evicted ... / re-enabled ...   (cycle repeats each 4s interval)
```

Status: **Phase 3 complete.** Eviction is the symmetric inverse of the Phase-2
add, via the same balancer_manage path; member-removal-from-slotmem remains a
non-feature of httpd, so "evict" = disable (reversible).

## 12. Phase 4 — authenticate the announcement channel (DONE)

Before Phase 4 the channel was unauthenticated: anyone who could reach the SUB's
listen port could announce an arbitrary `url=` and the proxy would route real
client traffic to it (SSRF / hijack / exfiltration). Phase 4 closes this with a
pre-shared cluster secret.

### Approach: authenticate, don't encrypt (user-confirmed)
A keyed MAC + timestamp on each message. The payload (backend URLs) isn't
secret; the threat is *unauthorized injection*, which integrity+authenticity
closes. nng TLS (`tls+tcp://`, needs nng built with mbedTLS + certs) is an
orthogonal transport layer documented for a future phase — it secures the pipe
but wouldn't stop an authorized TLS peer from announcing a bad URL.

### Primitive: SipHash-2-4 (in-tree, no new dependency)
Direct precedent: `mod_session_crypto.c:176` derives a 16-byte siphash key from
a passphrase via MD5 then calls `apr_siphash24_auth` — Phase 4 does the same.
- `apr_md5(secret)` → 16-byte key (this is KDF, not message hashing, so MD5 is
  fine).
- `apr_siphash24_auth` → 8-byte MAC, `ap_bin2hex` → 16 hex chars on the wire.
- Constant-time compare: `apr_crypto_equals` turned out to be gated behind
  `APU_HAVE_CRYPTO` (not enabled in the local apr-util), so a tiny self-contained
  branchless `nng_const_time_eq` is used instead (same XOR-accumulate idiom).

### Wire format
```
ANNOUNCE url=… host=… pid=… seq=… ts=<unix-sec> mac=<16-hex>
```
`mac` is the SipHash of the message prefix up to (not including) `" mac="`,
computed over `strlen` (no trailing NUL) on both sides. SUB verifies **before**
`nng_parse_url`, so a bad-MAC message never gets its URL acted on. Anti-replay:
reject if `|now - ts| > max_skew` (`ProxyNngMaxSkew`, default max(30s, 2×interval);
requires NTP-synced clocks across hosts). `seq` is NOT a replay counter (resets
on backend restart) — the signed `ts` is.

### Directives
- `ProxyNngSecret <passphrase>` — on both PUB and SUB. PUB signs only if set; SUB
  requires a valid+fresh MAC only if set. Mismatched secrets ⇒ all dropped
  (logged) — a documented footgun that looks like an outage.
- `ProxyNngMaxSkew <secs>` — optional replay window.

### Compatibility (fail-open + warn, user-confirmed)
No secret on the proxy ⇒ Phase 1–3 behavior, plus a one-time startup WARNING
that the channel is UNAUTHENTICATED. Security is opt-in. Rejections are
rate-limited (~1/s) with a count + reason to avoid log flooding under a forged
flood (each check is ~tens of ns).

### End-to-end test — PASSING
Two backends dial the **same** proxy port: PUB #1 with the matching secret +
real URL, PUB #2 with a **wrong** secret + decoy `http://127.0.0.1:1`. Asserts:
the legit add/evict/re-enable lifecycle still works and `/nng` serves; the decoy
is **never** added; and the proxy logs dropping the wrong-secret announcements.
Log confirms:
```
SUB received: ANNOUNCE url=… ts=1780587143 mac=ab902d4fad6589f7
added backend http://localhost:8529 to balancer://nng        (legit)
dropped 1 unauthenticated/invalid announcement(s) (last: mac mismatch)   (rogue)
# http://127.0.0.1:1 never appears in an "added backend" line
```

Status: **Phase 4 complete.** Channel authenticated against forgery, tampering,
and replay with an in-tree MAC; transport encryption (nng TLS) remains an
optional future layer.

## 13. Security / correctness audit (post Phase 1-4)

A full review of mod_proxy_nng.c against the reused balancer/watchdog code, plus
an independent adversarial pass. Conclusions:

### Confirmed safe (no change needed)
- **No per-tick memory leak.** The watchdog passes its `temp_pool` to the
  RUNNING/STARTING callbacks and `apr_pool_clear`s it every tick
  (`mod_watchdog.c:194`). The PUB `apr_psprintf`, the sweep's `apr_hash_first`,
  and the per-call subpools in the add/disable helpers all allocate into that
  pool (or short-lived subpools they destroy), so nothing accumulates per tick.
- **Slot exhaustion is safe at the balancer layer.**
  `balancer_process_balancer_worker` checks `!nworker && num_free_slots()`
  *before* `ap_proxy_define_worker`, so a full balancer just returns
  HTTP_BAD_REQUEST with no allocation/partial-add.
- **No buffer issues in the parser.** `buf[sz-1]='\0'` is guarded by `sz>0`, and
  is set before any `strstr`/`strlen`/`strncmp`, so all scans are bounded even on
  a non-NUL-terminated foreign payload; `url[512]` truncates safely; the MAC
  compare is constant-time over a fixed length; verify runs before the URL is
  parsed/acted on; the reject path `continue`s past the trailing `nng_free`
  (no double free).

### Fixed (the issue you flagged + a related one)
- **Slot-exhaustion retry/log storm (was: real).** Previously, when the balancer
  was full the failed add recorded *nothing* in `ctx->seen`, so every subsequent
  announcement (every interval, per un-addable backend, forever) re-ran the full
  `balancer_manage` add path — global-mutex cycle + an ERROR log each time.
  Attacker-amplifiable when unauthenticated. **Fix:** track every url in
  `ctx->seen` up front (with `last_attempt`); a failed add now backs off
  (`NNG_RETRY_BACKOFF`, 60s) instead of retrying every announcement, and the
  failure log is rate-limited (~1/s) and shared with the Phase-4 reject log via
  `nng_log_throttled`.
- **Unbounded `ctx->seen` growth.** Distinct advertised urls accumulated forever
  in the process-lifetime pool (worst case: an unauthenticated flood of distinct
  urls). **Fix:** cap the table at `NNG_MAX_MEMBERS` (256); beyond that, unknown
  urls are dropped with a throttled log rather than tracked.

### Regression coverage
The pytest now includes a capacity scenario: a second SUB with a `growth=1`
`balancer://cap` and two backends advertising distinct urls. Asserts exactly one
is added and that the un-addable one produces no per-interval failure storm
(<= 3 failure logs over ~16s of 1s-interval announcements, vs >10 without the
backoff). The existing add/evict/re-enable and Phase-4 reject assertions still
pass (now balancer-qualified so the two balancers don't cross-perturb counts).

Status: **audited; the flagged slot-exhaustion case is handled and regression
tested.** No memory-safety bugs, double-frees, or exploitable parsing defects
found. Module remains default-off in the build and interacts with other modules
only through the existing `balancer_manage` optional fn.

## 8. References
- nng PUB/SUB getting started: https://nanomsg.org/gettingstarted/nng/pubsub.html
- nng_pub(7): https://nng.nanomsg.org/man/v1.10.0/nng_pub.7.html
- httpd reverse proxy howto: https://httpd.apache.org/docs/trunk/howto/reverse_proxy.html
- Verified source: `modules/proxy/{mod_proxy_balancer.c,proxy_util.c,mod_proxy_hcheck.c,mod_proxy.h}`
