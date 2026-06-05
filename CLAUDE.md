# CLAUDE.md — Apache HTTP Server (2.5.x trunk **and** 2.4.x)

This file is the working memory for hacking on Apache httpd. It covers **two release lines** —
**2.5.x (trunk)** and **2.4.x** — because the same checkout layout is used for both. They share one
architecture but differ in details (line numbers, bundled-vs-system APR, module set, ABI, a few
APIs). **First figure out which line you're in, then read only that line's part below.**

---

## ▶ STEP 0 — Detect the version (do this first)

Check `include/ap_release.h` → `AP_SERVER_MINORVERSION_NUMBER`:

| If you see… | You are in… | Read |
|-------------|-------------|------|
| `MINORVERSION = 5` (e.g. `2.5.1-dev`) | **trunk / 2.5.x** | **PART A** |
| `MINORVERSION = 4` (e.g. `2.4.68-dev`) | **stable / 2.4.x** | **PART B** |

Quick confirmation signals (any one is sufficient if `ap_release.h` is ambiguous):

- **`srclib/` populated** (`srclib/apr/`, `srclib/apr-util/` present, bundled APR 1.7.x-dev) → **2.5.x**.
  **`srclib/` empty** (APR comes from the system via `apr-config`/`apu-config`) → **2.4.x**.
- **`test/` has `pytest_suite/`** → 2.5.x has the full Python functional suite.
- **`include/ap_mmn.h`**: 2.4.x is `MODULE_MAGIC_COOKIE 0x41503234` ("AP24"), `MAJOR 20120211`. Trunk
  carries a higher/different magic number (it tracks 2.5 ABI).
- Provider line in `.svn/`’s URL (`…/httpd/httpd/trunk` vs `…/httpd/httpd/branches/2.4.x`), if SVN.

The two parts are independent and self-contained — don't mix line numbers or API claims across them.

---

## Shared note: source control & change tracking (applies to both lines)

httpd's **canonical VCS is Subversion**; there is an official **read-only Git mirror**
(`https://github.com/apache/httpd`). Detect which this tree is with `ls -d .svn .git 2>/dev/null`.

- **SVN working copy** (an `.svn/` dir; the harness then reports "not a git repository"): use
  `svn info`/`status`/`diff`/`log`/`blame`, `svn update`, `svn commit`. Revisions are monotonic
  integers (e.g. `r1934997`). Trunk lives at `…/repos/asf/httpd/httpd/trunk`; the stable branch at
  `…/branches/2.4.x`. Backports cite the trunk revision(s) in the `STATUS` file's vote section.
- **Git clone/mirror**: normal `git log`/`diff`/`blame`, branches, and **PRs** to `apache/httpd`
  (CI in `.github/workflows/`). The mirror isn't the source of truth — committers land changes back
  via SVN, so don't expect to push to it.
- **Day-to-day dev is identical regardless of VCS** — edit, `./buildconf && ./configure && make`,
  test. The VCS only differs at clone/diff/commit time; both `svn diff` and `git diff` produce
  patches the project accepts.
- **The project's own change tracking is in-tree, not in the VCS**: `CHANGES`, `STATUS` (pending
  backports/votes), and `changes-entries/`. New user-visible changes need a small `*.txt` file in
  `changes-entries/` (see `README.CHANGES`) rather than a direct `CHANGES` edit (merge-conflict
  magnet); it's folded into `CHANGES` at release. Format mirrors a CHANGES entry:
  ```
    *) mod_ssl: For "SSLVerifyClient optional_no_ca" mode, accept
       expired client certificates.  PR 60028
       [Naveen Albert <apache2 phreaknet.org>]
  ```

---
---

# PART A — Apache httpd 2.5.x (trunk)

**Version:** `2.5.1-dev` (`include/ap_release.h`). APR: **bundled** in `srclib/apr` (1.7.x-dev) +
`srclib/apr-util`. C, autoconf build, ~250 modules. Line numbers drift — treat them as starting
points; symbol names are stable.

## A1. Mental model in one paragraph

httpd is a **hook-driven, filter-based** server built on **APR** (the Apache Portable Runtime).
Everything is a *module* that registers callbacks into *hooks* (ordered extension points) at
startup. A *Multi-Processing Module* (MPM) owns the process/thread model and the accept loop. When
a request arrives it flows through an ordered pipeline of hook phases (translate → map → auth →
type → fixup → handler → log). Data — both request body and response — flows as **bucket
brigades** through a stack of **filters**. Memory is managed by **pools**, not malloc/free.

## A2. Directory map

| Path | What lives there |
|------|------------------|
| `include/` | Public API headers (`httpd.h`, `http_*.h`, `ap_*.h`, `util_*.h`). Start here. |
| `server/` | Core engine: request/connection/config/protocol/filters/vhost/scoreboard. 59 `.c`. |
| `server/mpm/` | MPMs: `event/`, `worker/`, `prefork/` (+ `winnt`, `motorz`, `simple`, os2/netware). |
| `modules/` | ~250 modules grouped by function (see A9). |
| `os/` | OS-specific shims: `unix/`, `win32/`, `os2/`, `netware/`, `bs2000/`. |
| `srclib/apr/` | **Bundled APR 1.7.x-dev** (pools, I/O, threads, tables, buckets). The foundation. |
| `srclib/apr-util/` | APR-util (needed because bundled APR is 1.x): DBD, crypto, xml, socache helpers. |
| `support/` | Tools: `apxs` (out-of-tree module build), `apachectl`, `ab`, `htpasswd`, `rotatelogs`. |
| `test/` | `pytest_suite/` (functional, Python), `unit/` (httpdunit/Check), ad-hoc C tests. |
| `docs/` | Manual + config samples. |
| `build/` | autoconf/automake machinery (`rules.mk`, `program.mk`, `*.m4` copied from APR). |

## A3. Core data structures (`include/httpd.h`)

The "big four" records, each tied to a pool of matching lifetime:

- **`process_rec`** (`httpd.h:850`) — whole-process. `pool` (global, dies at exit), `pconf`
  (config pool, **cleared on every restart** — config-lifetime allocations go here).
- **`server_rec`** (`httpd.h:1401`) — a (v)host. `module_config` vector, `process`, log config,
  `next` links the vhost list. Main server + one per `<VirtualHost>`.
- **`conn_rec`** (`httpd.h:1203`) — one TCP connection. `pool` (connection lifetime), `base_server`,
  `input_filters`/`output_filters` (connection-level), `bucket_alloc`, `cs` (conn_state for async
  MPMs), `master`/`slaves` (HTTP/2: master c1 + secondary c2 conns), `keepalive`.
- **`request_rec`** (`httpd.h:866`) — one HTTP request. `pool` (request lifetime), `connection`,
  `server`, `per_dir_config` (result of the dir/location/file walk), `headers_in`/`headers_out`/
  `err_headers_out`/`subprocess_env`/`notes` (all `apr_table_t`), `the_request`, `uri`, `filename`,
  `handler`, `content_type`, `status`, `input_filters`/`output_filters`/`proto_*_filters`.
  Sub-requests and internal redirects chain via `main`/`prev`/`next`.

Config storage is the **`ap_conf_vector_t`** (opaque): one slot per module, indexed by the module's
assigned `module_index`. Access with `ap_get_module_config(cv, &my_module)` /
`ap_set_module_config(...)` (`http_config.h:512`).

## A4. The hooks system (`include/ap_hooks.h`, APR's `apr_hooks.h`)

Modules don't subclass anything; they **register functions into named hooks**. The macro layer:

- Declare: `AP_DECLARE_HOOK(ret, name, args)` in a header.
- Implement (in a `.c`) one of three runners:
  - `AP_IMPLEMENT_HOOK_VOID` — run every registered fn, ignore returns (e.g. `child_init`).
  - `AP_IMPLEMENT_HOOK_RUN_ALL(ok, decline)` — run until one returns something other than
    `ok`/`decline`; that value short-circuits and is returned (e.g. `pre_config`, `fixups`).
  - `AP_IMPLEMENT_HOOK_RUN_FIRST(decline)` — run until one returns **non-`DECLINED`**; that wins
    (e.g. `handler`, `translate_name`, `map_to_storage`). "First module to claim it wins."
- Register (in a module's `register_hooks`): `ap_hook_<name>(fn, predecessors, successors, order)`.
  - `order` ∈ `APR_HOOK_{REALLY_FIRST=-10, FIRST=0, MIDDLE=10, LAST=20, REALLY_LAST=30}`.
  - `predecessors`/`successors` are NULL-terminated arrays of **module filenames** (e.g.
    `{"core.c", NULL}`) for fine ordering relative to specific modules — stronger than `order`.
- **Optional hooks** (`AP_OPTIONAL_HOOK`, `APR_IMPLEMENT_OPTIONAL_HOOK_*`) — hooks that may not
  exist if the providing module isn't loaded.
- **Optional functions** (`APR_DECLARE_OPTIONAL_FN` / `APR_REGISTER_OPTIONAL_FN` /
  `APR_RETRIEVE_OPTIONAL_FN`) — typed function pointers looked up by name at runtime; returns NULL
  if the provider is absent. This is how loosely-coupled modules call each other (e.g. ssl vars).

Return-code convention: `OK` (handled), `DECLINED` (not interested, pass on), `DONE` (fully handled,
stop), `SUSPENDED` (async), or an HTTP status (≥100). Hooks are linked into a global struct via
`APR_HOOK_STRUCT`/`APR_HOOK_LINK` (see `server/config.c:71`).

## A5. The module struct (`include/http_config.h:348`)

```c
struct module_struct {
    int version, minor_version, module_index;
    const char *name;            void *dynamic_load_handle;
    struct module_struct *next;  unsigned long magic;
    void (*rewrite_args)();                          /* MPMs only */
    void *(*create_dir_config)(apr_pool_t*, char*);  /* per-<Directory>/<Location> */
    void *(*merge_dir_config)(apr_pool_t*, void*, void*);
    void *(*create_server_config)(apr_pool_t*, server_rec*);
    void *(*merge_server_config)(apr_pool_t*, void*, void*);
    const command_rec *cmds;                         /* directive table */
    void (*register_hooks)(apr_pool_t *p);           /* <-- where a module wires itself in */
};
```

Boilerplate: `STANDARD20_MODULE_STUFF` fills the header; declare with `AP_DECLARE_MODULE(name)` (or
`module AP_MODULE_DECLARE_DATA name_module`). MPMs use `MPM20_MODULE_STUFF`.

**Directives**: `command_rec` (`http_config.h:204`) maps a name → callback. `args_how`
(`enum cmd_how`, `http_config.h:49`) picks the signature: `RAW_ARGS`, `TAKE1`/`TAKE2`/`TAKE3`/
`TAKE12`/`TAKE13`/`TAKE23`/`TAKE123`, `ITERATE`/`ITERATE2`, `FLAG`, `NO_ARGS`, `TAKE_ARGV`.
`req_override` (`OR_*`, `ACCESS_CONF`, `RSRC_CONF`, …) controls where the directive is legal
(.htaccess vs server). Helpers like `ap_set_flag_slot`, `ap_set_string_slot`, `AP_INIT_TAKE1(...)`.

**Config merging**: parent config + child section config are merged top-down (server → `<Directory>`
→ `<Location>` → `<Files>` → `<If>`) via the `merge_*_config` callbacks; the final per-request result
lands in `r->per_dir_config`.

**Providers** (`server/provider.c`, `include/ap_provider.h`): `ap_register_provider` /
`ap_lookup_provider` — a lighter extension mechanism than hooks, used by authn/authz, socache,
slotmem, dbd.

## A6. Request lifecycle (the pipeline)

**Connection**: MPM accepts → `ap_process_connection()` (`server/connection.c:244`) →
`ap_update_vhost_given_ip(c)` picks `base_server` by IP:port → `pre_connection` hook (mod_ssl, etc.
insert connection filters here) → `ap_run_process_connection()` (RUN_FIRST; the HTTP/1.1 default
loops reading requests, HTTP/2 takes over here).

**Read**: `ap_read_request()` (`server/protocol.c:1306`) parses the request line + headers (pulling
through `proto_input_filters`), then `ap_post_read_request` hook.

**Process** — `ap_process_request_internal()` (`server/request.c:192`) runs the ordered phases. Each
is a hook; most are RUN_FIRST (first non-DECLINED wins) or RUN_ALL:

1. `ap_normalize_path` / unescape, then **location_walk** + **if_walk**
2. `pre_translate_name` (operates on still-encoded URI)
3. **`translate_name`** — URI → filename (mod_alias, mod_rewrite, mod_proxy, mod_userdir)
4. **`map_to_storage`** — sets `r->filename`/`finfo`, then **`directory_walk`** (`request.c:680`)
   + **`file_walk`** stat the path and merge `<Directory>`/`<Files>` configs into `per_dir_config`
5. `post_perdir_config`, then `header_parser` (RUN_ALL)
6. **AuthN/AuthZ** decision tree (`request.c:336`+): `access_checker` → `access_checker_ex` →
   `check_user_id` (authn, RUN_FIRST) → `auth_checker` (authz, RUN_FIRST). Honors `Satisfy`.
   Newer: `token_checker`. Provider-based (see A9 / aaa).
7. **`type_checker`** — sets content-type / handler (mod_mime, mod_negotiation)
8. **`fixups`** (RUN_ALL) — last chance to tweak before content (mod_headers, mod_env, mod_rewrite)
9. **`handler`** (RUN_FIRST) — generates the response (`ap_run_handler`, `config.c:443`). Matched by
   `r->handler` string. The **default handler** in `core.c` serves static files.
10. **`log_transaction`** — fired as the request/EOR bucket is cleaned up (mod_log_config).

`quick_handler` (RUN_FIRST) runs *before* this whole chain — used by mod_cache to short-circuit.
Sub-requests (`ap_sub_req_lookup_uri`, ESI/SSI, mod_negotiation) re-run a slice with `r->main` set;
internal redirects (`ap_internal_redirect`) start a fresh `request_rec` with `r->prev`.

## A7. Filters & buckets — the I/O model (`include/util_filter.h`, APR `apr_buckets.h`)

Content moves as **`apr_bucket_brigade`** (a doubly-linked list of `apr_bucket`s) pushed/pulled
through a filter chain — not as `write()` calls.

- **Buckets** are typed chunks: `transient` (borrowed stack data), `heap`, `pool`, `immortal`,
  `file` (sendfile-able), `mmap`, `socket`, `pipe`, plus **metadata** buckets carrying no data:
  `FLUSH`, `EOS` (end of stream), `EOR` (end of request → triggers logging/cleanup,
  `server/eor_bucket.c`), `EOC` (end of connection, `eoc_bucket.c`), `error` (`error_bucket.c`).
  Reads are lazy via `apr_bucket_read`.
- **Output filters** call `ap_pass_brigade(f, bb)` to send downstream. **Input filters** call
  `ap_get_brigade(f, bb, mode, block, readbytes)` to pull upstream. Input modes: `AP_MODE_GETLINE`,
  `READBYTES`, `EATCRLF`, `SPECULATIVE`, `EXHAUSTIVE`, `INIT`.
- **Filter types** order the chain (low number = closer to the handler/content, runs first on output;
  `util_filter.h:158`): `RESOURCE(10)` → `CONTENT_SET(20)` → `PROTOCOL(30)` → `TRANSCODE(40)` →
  `CONNECTION(50)` → `NETWORK(60)`. Network filter (core_output) is last and writes the socket.
- Register: `ap_register_input_filter` / `ap_register_output_filter(name, fn, init, ftype)`
  (`server/util_filter.c:279`). Insert per request/conn: `ap_add_{input,output}_filter[_handle]`.
- Core filters live in `server/core_filters.c`: `ap_core_input_filter` (socket read, `:91`) and
  `ap_core_output_filter` (socket write, buffering, sendfile). EOR bucket and content-length
  filter are in `server/core.c`.

Filter idioms: bail on empty brigade; pass metadata (EOS/FLUSH) through untouched; do one-time setup
when `f->ctx == NULL`; use `ap_save_brigade` to hold buckets across calls; always end with
`ap_pass_brigade(f->next, bb)`. mod_deflate and mod_filter are the reference examples.

## A8. MPMs — process/thread model (`server/mpm/`, `include/ap_mpm.h`, `mpm_common.h`)

The MPM registers the `mpm` hook (`ap_mpm_run`, `APR_HOOK_MIDDLE`) which owns the main loop until
shutdown/restart. Query its traits with **`ap_mpm_query(AP_MPMQ_*, &result)`** (e.g.
`AP_MPMQ_IS_THREADED`, `_IS_FORKED`, `_IS_ASYNC`, `_MAX_THREADS`, `_MAX_DAEMONS`).

- **prefork** — one single-threaded child **process per connection**. Children serialize on an
  accept mutex. Robust/compatible (non-thread-safe libs), heavy. `HARD_THREAD_LIMIT=1`.
- **worker** — hybrid: several processes × N threads. Per child: one **listener thread** accepts and
  feeds a bounded **fd_queue** (`mpm_fdqueue.c`) drained by worker threads. Keepalive ties up a worker.
- **event** (the modern default) — like worker, but the listener thread also owns **keepalive,
  write-completion, and lingering-close** connections via a **pollset** + **timeout queues**
  (`event.c`: `event_conn_state_t`, `waitio_q`/`write_completion_q`/`keepalive_q`/`linger_q`). Worker
  threads aren't pinned to idle keepalive conns, so far more concurrent connections than threads.
  `AsyncRequestWorkerFactor` tunes async conns per idle worker. Required for HTTP/2.

**Connection states** (`conn_state_e` in `httpd.h`): KEEPALIVE, PROCESSING, HANDLER,
WRITE_COMPLETION, SUSPENDED, LINGER(+NORMAL/SHORT), ASYNC_WAITIO; event transitions via `c->cs`.

**Shared infra**: the **scoreboard** (`server/scoreboard.c`, `include/scoreboard.h`) is shared memory
tracking every worker's state (`SERVER_READY/BUSY_READ/BUSY_WRITE/BUSY_KEEPALIVE/...`), used by
mod_status and for spawn/reap decisions; async MPMs also track per-process connection/keepalive/
write-completion/suspended counts. **Pipe-of-death** (`ap_mpm_pod_*` / `podx`) signals children to
exit on graceful restart (`ap_generation_t` increments per restart). Hooks `child_init`,
`child_stopping`, `child_stopped`, `monitor`, `suspend_connection`/`resume_connection`,
`mpm_register_timed_callback` (event-driven modules).

**Startup/config lifecycle** (`server/main.c`): init APR + pools, parse args, then a **two-pass**
config process — `pre_config` → process config tree → `check_config` → `open_logs` → `post_config`
(run **twice**; tell passes apart via `ap_state_query(AP_SQ_MAIN_STATE)`), then `ap_run_mpm` in a
loop (re-reads config on restart). `pconf` is `apr_pool_clear`'d (not destroyed) between restarts.

## A9. APR & the module ecosystem

**Pools (`apr_pool_t`) are the #1 thing to internalize.** httpd almost never calls `free()`.
- Allocate from a pool: `apr_palloc(p,n)` (uninit), `apr_pcalloc(p,n)` (zeroed), `apr_pstrdup`,
  `apr_psprintf`. Memory lives until the pool is **cleared or destroyed** — never freed individually.
- Pools form a **tree**; destroying a pool destroys its subpools. Lifetimes match the records:
  process `pool`/`pconf` ⊃ connection `pool` ⊃ request `pool`. Pick the pool whose lifetime matches
  what you're allocating. Request data in a long-lived pool = leak.
- **Cleanups**: `apr_pool_cleanup_register(p, data, cleanup_fn, child_cleanup_fn)` runs `cleanup_fn`
  when the pool dies (LIFO, after subpools) — how files/sockets/locks get released.
  `apr_pool_cleanup_kill` unregisters.
- Not thread-safe for concurrent access to the *same* pool; subpool creation under one parent is.

**Other APR**: `apr_table_t` (case-insensitive multimap for headers/env; `apr_table_get/set/add/
setn/addn`, iterate via `apr_table_elts` → `apr_table_entry_t[]`; `setn`/`addn` skip strdup),
`apr_array_header_t` (`apr_array_make`, `apr_array_push`), `apr_hash_t`, `apr_uri_t`, `apr_socket_t`,
`apr_thread_*`, `apr_time_t` (µs since epoch).

**Modules** (`modules/`, grouped by purpose):

| Dir | Highlights |
|-----|-----------|
| `http/` | **Core HTTP/1.1**: `http_core.c` (registers protocol filters), `http_filters.c` (input/chunk/byterange/dechunk state machines), `http_protocol.c` (header parse/response gen), `http_request.c`. |
| `http2/` | **mod_http2** over nghttp2. Trio: `h2_session` (c1 connection state machine) / `h2_stream` (one request) / `h2_mplx` (multiplexer between c1 and worker c2 conns). `h2_bucket_beam` does zero-copy hand-off across threads. Naming: `h2_mplx_c1_*` primary, `_c2_*` secondary, `_worker_*`. Requires async (event) MPM. |
| `aaa/` | **Auth, provider-based** (`include/mod_auth.h`). `mod_authn_core`/`mod_authz_core` host the provider registries; `mod_auth_basic`/`_digest`/`_form`/`_bearer` are mechanisms; `mod_authn_file/dbm/dbd/ldap/socache/anon` authn backends; `mod_authz_user/groupfile/host/owner/dbd` answer `Require`. `mod_access_compat` = legacy Order/Allow/Deny. |
| `proxy/` | **mod_proxy** core + backends `proxy_http`/`_http2`/`_fcgi`/`_ajp`/`_scgi`/`_uwsgi`/`_wstunnel`/`_connect`/`_ftp`. `mod_proxy_balancer` + balancer-method providers (byrequests/bytraffic/bybusyness/heartbeat); `proxy_worker`/`proxy_balancer` structs in `mod_proxy.h`; shared state via slotmem. `mod_proxy_hcheck` health checks (uses mod_watchdog). |
| `ssl/` | **mod_ssl** = OpenSSL as a **connection filter**. `ssl_engine_io.c` (the filter), `_kernel.c` (handshake/SNI vhost pick), `_init.c`/`_config.c`, `_pphrase.c`, OCSP stapling, session cache via socache. |
| `cache/` | **mod_cache** filter framework (`CACHE_SAVE`/`CACHE_OUT`, optional quick-handler) + backends `mod_cache_disk`, `mod_cache_socache`; `mod_file_cache`. socache providers: `shmcb`/`dbm`/`memcache`/`redis`/`dc`. |
| `filters/` | Body transforms: `mod_deflate`/`mod_brotli`, `mod_filter`, `mod_include` (SSI), `mod_substitute`/`mod_sed`, `mod_ext_filter`, `mod_buffer`, `mod_ratelimit`, `mod_reqtimeout`, `mod_charset_lite`, `mod_xml2enc`, `mod_proxy_html`. |
| `generators/` | `mod_cgi`/`mod_cgid` (fork vs daemon), `mod_status`, `mod_info`, `mod_autoindex`, `mod_asis`, `mod_suexec`. |
| `mappers/` | `mod_rewrite`, `mod_alias`, `mod_dir`, `mod_userdir`, `mod_negotiation`, `mod_vhost_alias`, `mod_speling`, `mod_actions`, `mod_imagemap`. |
| `metadata/` | `mod_headers`, `mod_env`, `mod_setenvif`, `mod_expires`, `mod_remoteip` (X-Forwarded-For), `mod_unique_id`, `mod_mime_magic`, `mod_usertrack`, `mod_version`, `mod_ident`. |
| `loggers/` | `mod_log_config` (CustomLog/LogFormat), `mod_logio`, `mod_log_forensic`, `mod_log_debug`, `mod_log_json`, `mod_journald`, `mod_syslog`. |
| `dav/` | **WebDAV** (RFC 4918): `main/mod_dav.c` engine + provider model; `fs/` filesystem provider with lock DB. |
| `session/` | `mod_session` + `mod_session_cookie`/`_dbd`/`_crypto`. |
| `md/` | **mod_md** — ACME / Let's Encrypt automatic certs (~28 files). Driven by mod_watchdog. |
| `lua/` | **mod_lua** — embed Lua to script hooks/handlers. |
| `core/` | `mod_so` (DSO loader — **always static**), `mod_watchdog`, `mod_macro`. |
| `slotmem/`, `database/`, `ldap/` | `mod_slotmem_shm`/`_plain`; `mod_dbd` (SQL pooling); `util_ldap` (LDAP conn+result cache). |
| `debugging/`, `experimental/`, `test/`, `examples/` | `mod_dumpio`, `mod_bucketeer`, `mod_example_hooks`. |

The big add-on subsystems (mod_ssl, mod_proxy, mod_http2) have the same architecture as in PART B
§10 — refer there for the deeper internals; they apply to trunk too.

## A10. Build system (trunk specifics)

- **Bootstrap**: `./buildconf` regenerates `configure` from `configure.in` using APR's build m4
  (`build/apr_common.m4`, `find_apr.m4`, `find_apu.m4` copied from `srclib/apr`). Needs autoconf +
  libtoolize/glibtoolize. Required for any SVN/Git checkout — only release tarballs ship `configure`.
- **Configure → make → install**: `./configure --prefix=... [flags] && make -jN && make install`.
  `SUBDIRS = srclib os server modules support`; the binary is `httpd`.
- **Static vs shared (DSO) modules**: the `APACHE_MODULE(name, help, objs, struct, default, …)`
  macro in each `modules/*/config.m4` (defined in `acinclude.m4`). Static = linked into `httpd`
  (`BUILTIN_LIBS`); shared = `mod_<name>.so`, loaded at runtime by `mod_so` via `LoadModule`. Select
  with `--enable-<mod>[=shared|static]` or `--enable-mods-shared=most|reallyall`. **`mod_so` is
  always static** (it's the DSO loader).
- **APR**: trunk **bundles** APR 1.7.x-dev (`srclib/apr/include/apr_version.h`) + apr-util (required
  because bundled APR is 1.x). `--with-included-apr` forces the bundled copy.
- **`apxs`** (`support/apxs.in`) builds modules **out-of-tree** against an installed httpd, reading
  `<prefix>/build/config_vars.mk`: `apxs -c -i -a mod_foo.c`.

### A10.1 Local build recipe (macOS / Apple Silicon, Homebrew + MacPorts) — VERIFIED

A working build for the pytest suite lives at **`~/src/asf/httpd-build`** (httpd 2.5.1-dev); its apxs
is **`~/src/asf/httpd-build/bin/apxs`**. From this source tree root:

```sh
./buildconf            # generates configure from configure.in; uses bundled srclib/apr + apr-util
./configure --prefix=$HOME/src/asf/httpd-build \
  --enable-mods-shared=reallyall \
  --disable-ssl_ct --disable-crypto --disable-session-crypto --disable-autht_jwt \
  --enable-deflate --enable-ssl \
  --with-ssl=$(brew --prefix openssl@3) \
  --with-pcre=$(brew --prefix pcre2)/bin/pcre2-config \
  --with-z=$(brew --prefix zlib) \
  --with-included-apr
make -j12 && make install
```

Why the disables (macOS-specific):
- `--disable-ssl_ct` — `mod_ssl_ct` rejects OpenSSL 3.x.
- `--disable-crypto` / `--disable-session-crypto` / `--disable-autht_jwt` — need APR's crypto driver,
  which the bundled APR isn't built with.
- **brotli** isn't installed locally (dangling brew symlink), so `mod_brotli` is left out and
  `test_brotli` skips.

This matches the tree's `config.nice`.

### A10.2 Testing (trunk has the Python functional suite)

- `test/pytest_suite` — functional suite (Python). Run: `cd test/pytest_suite &&
  ./runtests.sh --apxs ~/src/asf/httpd-build/bin/apxs <paths>`.
- `test/unit` — C unit tests (httpdunit / Check). Other ad-hoc C tests sit in `test/`.

**Gotcha 1 — module activation.** The pytest probe (`test/pytest_suite/apache_pytest/probe.py`) only
inherits **active** `LoadModule` lines from `<prefix>/conf/httpd.conf`. The stock install conf
comments out most modules (deflate, include, reflector, case_filter, bucketeer, …), so
`need_module`-gated tests **SKIP**. The suite expects `--enable-load-all-modules`-style behavior —
uncomment them all, then re-comment the bogus example line, and verify:

```sh
sed -i '' -E 's/^([[:space:]]*)#[[:space:]]*(LoadModule[[:space:]])/\1\2/' <prefix>/conf/httpd.conf
# then re-comment the example:  LoadModule foo_module modules/mod_foo.so
<prefix>/bin/httpd -t
```

**Gotcha 2 — httpx auto-decompresses bodies.** The suite's HTTP client wraps **httpx**, which
**transparently decodes gzip/deflate response bodies** (`.content`/`.text` are decoded plaintext)
while leaving `Content-Encoding`/`Content-Length` headers intact, and sends a default
`Accept-Encoding: gzip, deflate` on every request. Any test asserting on the **body** of a
compressed response is therefore wrong by default. Breaks mod_deflate round-trip tests (pr49328,
pr43939, pr17629, deflate — they re-POST the gzip body through an inflate input filter) and
mod_reflector body-transform assertions.
- **Fix**: use `http.GET_RAW(path, ...)` (raw bytes) or `http.raw_response(method, path, ...)` (full
  response with `.raw_content` + headers) — both stream and read `iter_raw()` to bypass decoding.
  Added to `apache_pytest/client.py` (committed r1934949/r1934950).
- Length-based tests (rwrite, passbrigade, getfile, byterange7) are fine: `len(.content)` after
  decoding equals the original length.

## A11. Conventions & gotchas (trunk)

- **Pools, not malloc.** Match allocation lifetime to the right pool; register cleanups for resources.
- A handler returns an **HTTP status int**, `OK`, `DECLINED` (pass to next), or `DONE`. Most phase
  hooks return `DECLINED` to abstain.
- Per-request module data goes in the config vector / `r->notes` / `r->request_config`, never globals
  (threaded MPMs!). Prefork is the only single-threaded model.
- `err_headers_out` survives errors and internal redirects; `headers_out` does not.
- `ap_`/`AP_` = httpd public API; `apr_`/`APR_` = the portable runtime beneath it.
- Logging is `ap_log_error/rerror/cerror/perror(APLOG_MARK, level, status, …)`; this is the **error**
  log — the access log is mod_log_config, separate.
- Module Magic Number (`include/ap_mmn.h`, `AP_MODULE_MAGIC_*`) gates binary compat — bump when
  changing public structs/APIs.
- `.gdbinit` in the repo root has handy macros (dumping brigades/pools).

---
---

# PART B — Apache HTTP Server 2.4.x (stable)

**Version:** `2.4.68-dev` (`include/ap_release.h`). Module ABI: `MODULE_MAGIC_COOKIE 0x41503234`
("AP24"), `MAJOR 20120211`, `MINOR 142` (`include/ap_mmn.h`). **APR is system-installed** — `srclib/`
is empty here; headers are found via `apr-config`/`apu-config`.

## B1. The big picture

httpd is a **hook-driven, modular** web server built on top of **APR**. The core is small; almost
everything is a *module* that registers callbacks into *hooks* at well-defined phases. Concurrency is
abstracted behind a pluggable **MPM**. I/O is a **filter chain** operating on **bucket brigades**.
Memory is **pool**-based (no `free()`).

```
  process (main.c)
    └─ MPM (prefork/worker/event)  ── accepts connections, dispatches to threads/procs
         └─ ap_process_connection(c)             [connection hooks]
              └─ ap_read_request → ap_process_request   [request phase hooks]
                   └─ handler generates response
                        └─ output filter chain → core_output_filter → socket
```

Four nested lifetime structs (`include/httpd.h`): `request_rec` → `.connection` (`conn_rec`) →
`.server` (`server_rec`) → `.process` (`process_rec`). Each has its own APR pool.

## B2. Directory map

| Path | What's there |
|------|--------------|
| `server/` | Core: request/connection/protocol engine, config, filters, MPM common, util_* |
| `server/mpm/` | The MPMs: `prefork/`, `worker/`, `event/`, `winnt/`, `mpmt_os2/`, `netware/` |
| `include/` | Public API headers (`httpd.h`, `http_*.h`, `ap_*.h`, `util_*.h`, `scoreboard.h`) |
| `modules/` | All modules by category (see B9) |
| `os/` | Platform abstraction: `unix/` (unixd), `win32/`, `os2/`, `netware/`, `bs2000/` |
| `support/` | Tools: apachectl, apxs, ab, htpasswd, htdbm, rotatelogs, suexec, htcacheclean, fcgistarter |
| `docs/` | Bundled documentation, conf samples, error docs |
| `build/` | autoconf m4 macros (`find_apr.m4`, `find_apu.m4`), `rules.mk`, build glue |
| `srclib/` | (empty) Optional bundled APR/APR-util location (`--with-included-apr`) |

Build: autoconf. `./buildconf` → `./configure` → `make`. Modules opt in via `modules/*/config*.m4`
(`APACHE_MODULE(name, help, objs, struct, default)` where default ∈ {yes, most, no}). DSO modules
built/installed with `apxs`. **Requires APR 1.4+** (from the system; `--with-included-apr` would
unpack them into `srclib/`).

## B3. The hook system (`include/ap_hooks.h`)

Hooks are how modules plug into the server. Built on APR's external-hook macros.

- **Declare:** `AP_DECLARE_HOOK(ret, name, args)` in a header.
- **Implement** (generates `ap_run_<name>` + `ap_hook_<name>`):
  - `AP_IMPLEMENT_HOOK_RUN_FIRST(...)` — call modules in order until one returns something other than
    the `decline` value (e.g. `DECLINED`). First responder wins.
  - `AP_IMPLEMENT_HOOK_RUN_ALL(...)` — call all until one returns other than `ok`/`decline`.
  - `AP_IMPLEMENT_HOOK_VOID(...)` — call all, no return.
- **Register** (in a module's `register_hooks`): `ap_hook_<name>(fn, predecessors, successors,
  position)` where position ∈ `APR_HOOK_{REALLY_FIRST, FIRST, MIDDLE, LAST, REALLY_LAST}` and
  predecessors/successors are NULL-terminated module-name arrays for fine ordering.

Return-code convention (`include/httpd.h:460`): `OK` (0, handled), `DECLINED` (-1, not interested),
`DONE` (-2, fully handled, stop), `SUSPENDED` (-3, async), or an HTTP status (≥100). The core
registers default behavior at `REALLY_LAST` so other modules get first crack.

## B4. Request lifecycle & phase hooks

Entry: MPM accepts → `ap_process_connection` (`server/connection.c:210`) runs connection hooks →
`modules/http/http_core.c:193` loops `ap_read_request` / `ap_process_request`. Async path:
`ap_process_async_request` (`modules/http/http_request.c:407`).

**Connection hooks** (`include/http_connection.h`): `create_connection`, `pre_connection`,
`process_connection` (the HTTP module's implements the request loop), `pre_close_connection`.

**Request reading** (`server/protocol.c:1423` `ap_read_request`): create request → `pre_read_request`
→ parse request line → read MIME headers → check headers / select vhost → `post_read_request`.

**Request processing phases**, in order — all implemented in `server/request.c:78-104`, invoked from
`ap_process_request_internal`:

1. `pre_translate_name` (RUN_FIRST) — early URI rewrite before decode
2. URL normalize / unescape (`ap_normalize_path`, `ap_unescape_url_ex`)
3. `translate_name` (RUN_FIRST) — URI → filename or `proxy:` URL (core's `ap_core_translate` at
   REALLY_LAST; **mod_rewrite** & **mod_alias** hook here)
4. `map_to_storage` (RUN_FIRST) — filename → storage; `<Directory>`/`.htaccess` walk
5. `post_perdir_config` (RUN_ALL)
6. `header_parser` (RUN_FIRST) — **main request only**
7. Access control: `access_checker` (RUN_ALL) → `access_checker_ex` (RUN_FIRST) → `force_authn` →
   `check_user_id`/authn (RUN_FIRST) → `auth_checker`/authz (RUN_FIRST). `Satisfy any` vs `all`
   changes whether authn is skipped when access passes.
8. `type_checker` (RUN_FIRST) — sets `r->content_type` (mod_mime, mod_negotiation)
9. `fixups` (RUN_ALL) — last chance before handler (mod_headers, mod_env, mod_rewrite per-dir)
10. `handler` (RUN_FIRST) — dispatched on `r->handler` (`ap_invoke_handler` → `ap_run_handler`; core
    `default_handler` serves static files at REALLY_LAST)
11. `log_transaction` (RUN_ALL) — fired when the **EOR bucket** is destroyed during request-pool
    cleanup (`server/eor_bucket.c`), not immediately after the handler.

`quick_handler` (RUN_FIRST) runs *before* the whole internal-processing chain — used by **mod_cache**
to short-circuit. Subrequests (`ap_sub_req_lookup_*`) and internal redirects reuse this machinery;
auth is skipped if `r->prev->per_dir_config == r->per_dir_config`.

## B5. Filters & bucket brigades (the I/O pipeline)

`server/util_filter.c`, `include/util_filter.h`, `server/core_filters.c`.

**Buckets** (from APR) are typed data containers; a **brigade** is a doubly-linked list of buckets.
Bucket types: TRANSIENT, HEAP, POOL, IMMORTAL, FILE (enables sendfile/zero-copy), PIPE, plus metadata
buckets FLUSH and EOS. httpd-specific metadata buckets: **EOR** (end-of-request, carries
`request_rec`, triggers logging & pool cleanup — `server/eor_bucket.c`), **EOC** (end-of-connection,
`server/eoc_bucket.c`), **ERROR** (carries HTTP status, `server/error_bucket.c`).

**Filters** are typed and ordered by `ap_filter_type` (lower = closer to handler, runs earlier on
output): `AP_FTYPE_RESOURCE` (10) → `CONTENT_SET` (20, e.g. mod_deflate) → `PROTOCOL` (30) →
`TRANSCODE` (40, chunking) → `CONNECTION` (50) → `NETWORK` (60, core network filter). Within a type,
FIFO insertion order.

- Register once: `ap_register_{input,output}_filter[_protocol]`.
- Add per request/conn: `ap_add_{input,output}_filter(name, ctx, r, c)` — inserted in sorted
  position. Stored as `r->output_filters` / `r->proto_output_filters` / `c->output_filters` (and
  input equivalents).
- **Output:** handler calls `ap_rputs`/`ap_rwrite`/… → `ap_pass_brigade(f, bb)` walks down the chain
  → `ap_core_output_filter` (`core_filters.c:453`) writes via `writev`/`sendfile`, with non-blocking
  writes + deferred buffering via `ap_save_brigade`.
- **Input:** `ap_get_brigade(f, bb, mode, block, readbytes)` pulls up from `ap_core_input_filter`
  (`core_filters.c:90`). Modes: READBYTES, GETLINE, EATCRLF, SPECULATIVE, EXHAUSTIVE, INIT.

Filter idioms: bail on empty brigade; pass metadata (EOS/FLUSH) through untouched; one-time setup
when `f->ctx == NULL`; use `ap_save_brigade` to hold buckets across calls; always
`ap_pass_brigade(f->next, bb)` at the end. mod_deflate (`modules/filters/mod_deflate.c`) and
mod_filter are the reference examples.

## B6. MPMs (concurrency) — `server/mpm/`, `include/ap_mpm.h`

The MPM is itself a module registering the `mpm` hook, run by `ap_run_mpm` (from `server/main.c`).
Modules query MPM capabilities at runtime via `ap_mpm_query(AP_MPMQ_*)` (e.g. `IS_THREADED`,
`IS_ASYNC`, `MAX_THREADS`).

| MPM | Model | Notes |
|-----|-------|-------|
| **prefork** | 1 process / connection, no threads | Stable, memory-heavy. `HARD_THREAD_LIMIT=1`. Accept mutex serializes accept. |
| **worker** | N processes × M threads; listener thread + worker pool | Listener accepts, pushes sockets to **fdqueue**; workers pop and run `ap_process_connection`. |
| **event** | Like worker, but a listener thread does async polling | Keep-alive/lingering connections held in a pollset (epoll/kqueue), **not** a blocked worker thread. Workers only handle active requests. Enables 10k+ idle keep-alives. Required by HTTP/2. |

Shared infra: `server/mpm_common.c`, `server/mpm_fdqueue.{c,h}` (producer/consumer socket queue +
idler tracking), `server/mpm_unix.c` (signals, privilege drop, pid file), `os/unix/unixd.c`.

**Scoreboard** (`server/scoreboard.c`, `include/scoreboard.h`): shared-memory table the parent and
workers use to track state — `global_score`, `process_score[]`, `worker_score[][]` (status
SERVER_DEAD/READY/BUSY_*/GRACEFUL…, byte/access counts, current request). `mod_status` reads it. Async
MPMs also track per-process connection/keepalive/write-completion/suspended counts.

**Connection states** (`conn_state_e` in `httpd.h:1262`): KEEPALIVE, PROCESSING, HANDLER,
WRITE_COMPLETION, SUSPENDED, LINGER(+NORMAL/SHORT), ASYNC_WAITIO. The event MPM transitions
connections through these via `c->cs`.

**Restart/generations:** `ap_generation_t` increments per restart; pipe-of-death (`ap_mpm_pod_*`)
signals children. Graceful (SIGUSR1/`AP_SIG_GRACEFUL`) lets in-flight requests finish; hard restart
(SIGHUP) kills children; SIGTERM stops.

## B7. Startup & config lifecycle — `server/main.c`

`main()` (`server/main.c:488`): init APR + pools, parse args (`-C/-c/-D/-t/-S/-M/-k`), then a
**two-pass** config process, then `ap_run_mpm` in a loop (returns on restart, re-reads config; loops
until shutdown).

Init hook order **per pass** (declared `include/http_config.h`): `pre_config` →
`ap_process_config_tree` → `check_config` → `open_logs` → `post_config`. Then per child after fork:
`child_init`.

**Why `post_config` runs twice:** first pass with main-state `AP_SQ_MS_CREATE_PRE_CONFIG` (pre-flight
detection — lets modules skip one-time work on the throwaway first read), second pass
`AP_SQ_MS_CREATE_CONFIG` before the MPM runs. Use `ap_state_query(AP_SQ_MAIN_STATE)` to tell them
apart. The config pool `pconf` is `apr_pool_clear`'d (not destroyed) between restarts so it can be
reused.

## B8. Config & module system — `server/config.c`, `include/http_config.h`

A **module** is a `module` struct (`AP_DECLARE_MODULE(name) = {...}` / `STANDARD20_MODULE_STUFF`)
with callbacks: `create_dir_config`, `merge_dir_config`, `create_server_config`,
`merge_server_config`, a `command_rec cmds[]` directive table, and `register_hooks`.

- **Directives:** `AP_INIT_TAKE1/TAKE2/TAKE3/ITERATE/FLAG/RAW_ARGS/...(name, fn, cmd_data,
  req_override, help)`. `req_override` bitmask gates context (`RSRC_CONF` server, `ACCESS_CONF`
  `<Directory>`, `OR_*`/`.htaccess` overrides, `EXEC_ON_READ`, etc.). Handler gets a `cmd_parms*`
  (`include/http_config.h:288`).
- **Config vectors** (`ap_conf_vector_t`, opaque `void**` indexed by `module->module_index`):
  `ap_get_module_config(cv, mod)` / `ap_set_module_config`. Three kinds: per-dir
  (`r->per_dir_config`), per-server (`s->module_config`), per-conn (`c->conn_config`).
- **Merging:** as the directory tree is walked (`/` → `/a` → `/a/b`), per-dir configs merge
  parent+child via each module's `merge_dir_config`. NULL child inherits; otherwise the merger
  decides. Server configs merge for vhosts.
- **Config tree:** httpd.conf parsed into `ap_directive_t` n-ary tree (`util_cfgtree.{c,h}`), then
  `ap_process_config_tree` → `ap_walk_config` dispatches each directive by name via `ap_config_hash`
  to the owning module's handler.
- **Module registration:** `ap_add_module` assigns `module_index`, links into `ap_top_module`,
  registers commands + hooks. Dynamic modules via **mod_so** / `LoadModule`. ABI checked against
  `MODULE_MAGIC_NUMBER_MAJOR`.

**Core module** (`server/core.c`) is the canonical example and owns `<VirtualHost>`, `<Directory>`,
`<Location>`, `<If>/<ElseIf>/<Else>`, DocumentRoot, etc.

**Vhost matching** (`server/vhost.c`, `include/http_vhost.h`): IP-hash → name-based fallback by Host
header. `ap_update_vhost_given_ip` (by socket), `ap_update_vhost_from_headers` (by Host).

**Providers** (`server/provider.c`, `include/ap_provider.h`): `ap_register_provider(pool, group,
name, version, provider)` / `ap_lookup_provider`. Used for authn/authz providers, socache, slotmem,
dbd, etc. A lighter-weight extension mechanism than hooks.

## B9. Module catalog (`modules/`)

- **aaa/** — authn/authz. Core: `mod_authn_core`, `mod_authz_core` (provider vectors). Frontends:
  `mod_auth_basic`, `mod_auth_digest`, `mod_auth_form`. Providers: `mod_authn_{file,dbm,dbd,anon,
  socache}`, `mod_authz_{host,user,groupfile,owner,dbm,dbd}`, `mod_authnz_ldap`, `mod_authnz_fcgi`.
  `mod_access_compat` (old Order/Allow/Deny), `mod_allowmethods`.
- **http/** — `mod_http` (HTTP/1.1, always linked), `mod_mime`.
- **http2/** — `mod_http2`, `mod_proxy_http2` (see B10).
- **proxy/** — `mod_proxy` + backends `mod_proxy_{http,fcgi,ajp,scgi,uwsgi,connect,wstunnel,fdpass}`,
  `mod_proxy_balancer`, `balancers/mod_lbmethod_*`, `mod_proxy_hcheck`, `mod_proxy_express` (see B10).
- **ssl/** — `mod_ssl` (see B10).
- **filters/** — `mod_deflate`, `mod_brotli`, `mod_filter` (smart filter routing), `mod_include`
  (SSI), `mod_substitute`, `mod_sed`, `mod_ext_filter`, `mod_buffer`, `mod_ratelimit`,
  `mod_reqtimeout`, `mod_request`, `mod_charset_lite`, `mod_data`, `mod_proxy_html`, `mod_xml2enc`,
  `mod_reflector`.
- **generators/** — `mod_cgi`, `mod_cgid` (daemon for threaded MPMs), `mod_status`, `mod_info`,
  `mod_autoindex`, `mod_asis`, `mod_suexec`.
- **mappers/** — `mod_rewrite`, `mod_alias`, `mod_dir`, `mod_negotiation`, `mod_userdir`,
  `mod_vhost_alias`, `mod_actions`, `mod_speling`, `mod_imagemap`, `mod_so`.
- **metadata/** — `mod_headers`, `mod_env`, `mod_setenvif`, `mod_mime`, `mod_expires`,
  `mod_unique_id`, `mod_remoteip`, `mod_version`, `mod_usertrack`, `mod_ident`, `mod_cern_meta`,
  `mod_mime_magic`.
- **cache/** — `mod_cache`, `mod_cache_disk`, `mod_cache_socache`, `mod_file_cache`,
  `mod_socache_{shmcb,dbm,memcache,redis,dc}`.
- **loggers/** — `mod_log_config` (access logs — separate from error log), `mod_log_debug`,
  `mod_log_forensic`, `mod_logio`.
- **session/** — `mod_session`, `mod_session_{cookie,crypto,dbd}`.
- **dav/** — `mod_dav`, `mod_dav_fs`, `mod_dav_lock`.
- **md/** — `mod_md` (ACME / Let's Encrypt automatic cert management).
- **core/** — `mod_so` (DSO), `mod_watchdog` (background timers), `mod_macro`.
- **Others:** `mod_lua`, `mod_ldap`, `mod_dbd`, `mod_slotmem_{shm,plain}`, `mod_heartbeat`/
  `mod_heartmonitor` (cluster).

## B10. The big three add-on subsystems

### mod_ssl (`modules/ssl/`)
TLS as a **connection-level** filter pair over OpenSSL. Files: `mod_ssl.c` (hooks/registration),
`ssl_engine_init.c` (SSL_CTX setup, cert provider hooks), `ssl_engine_kernel.c` (handshake, SNI vhost
selection), `ssl_engine_io.c` (custom OpenSSL BIO bridging brigades ↔ `SSL_read`/`SSL_write`),
`ssl_engine_config.c` (directives), `ssl_engine_vars.c` (`SSL_*` env vars / `ssl_var_lookup`),
`ssl_engine_{rand,pphrase,mutex,ocsp,log}.c`, `ssl_scache.c` (session cache via **mod_socache**),
`ssl_util_stapling.c` (OCSP stapling), `ssl_private.h` (structs: `SSLModConfigRec`, `SSLSrvConfigRec`,
`SSLDirConfigRec`, `SSLConnRec`, `modssl_ctx_t`). `pre_connection` installs SSL + filters;
`process_connection`'s first `AP_MODE_INIT` read drives the handshake. **SNI** callback
(`ssl_find_vhost`) switches `SSL_CTX` mid-handshake to the matching vhost. Public API:
`include/http_ssl.h` (`ap_ssl_*`, cert-provider hooks for **mod_md**, ACME tls-alpn-01 challenge, OCSP
hooks) plus legacy `APR_DECLARE_OPTIONAL_FN` in `modules/ssl/mod_ssl.h` (`ssl_var_lookup`,
`ssl_is_https`, `ssl_proxy_enable`, `ssl_engine_{set,disable}`).

### mod_proxy (`modules/proxy/`)
`mod_proxy.c` hooks `translate_name` (sets `r->filename = "proxy:<url>"`), `map_to_storage`
(proxy_walk for `<Proxy>`), `fixups` (URL canon), and `handler` (`proxy_handler`). The handler picks a
worker (`ap_proxy_pre_request`) and dispatches via the **`scheme_handler` hook** (`mod_proxy.h:647`)
to a backend (`mod_proxy_{http,fcgi,ajp,connect,wstunnel,scgi,uwsgi}`). **Connection pooling:**
`proxy_conn_rec` from a per-worker `apr_reslist` (min/smax/hmax/ttl), `ap_proxy_{acquire,release}_
connection`. **Workers & balancers** keep cross-process state in **slotmem** shared memory
(`proxy_worker_shared`, `proxy_balancer_shared`), guarded by global+thread mutexes. **Load balancing**
via the `proxy_balancer_method` callback interface (`balancers/mod_lbmethod_{byrequests,bytraffic,
bybusyness,heartbeat}`); `balancer://name/...` URLs resolve to a worker via the LB `finder`. Health
checks: `mod_proxy_hcheck` (watchdog-driven). Reverse rewriting: ProxyPassReverse (`proxy_alias`).

### mod_http2 (`modules/http2/`)
HTTP/2 over **libnghttp2**, requires an **async MPM** (event). Negotiated by ALPN/Upgrade
(`h2_switch.c` `h2_protocol_propose`/`h2_protocol_switch`). Model: the **primary connection c1**
(`h2_c1.c`, runs in the MPM thread) owns the socket, parses frames via `h2_session.c`, and supervises
streams. Each stream becomes a **secondary connection c2** (`h2_c2.c`) — a pseudo `conn_rec` carrying
a normal `request_rec` — processed by the **`h2_workers`** thread pool, so ordinary handlers and
filters see it as plain HTTP/1. The **`h2_mplx`** multiplexer bridges c1 ↔ c2s. Cross-thread data
moves through **`h2_bucket_beam`** (`h2_bucket_beam.c`): a mutex/condvar-guarded bucket queue with
flow control (`beam_in` = request body, `beam_out` = response). c1 polls output beams and emits DATA
frames. Server push: `h2_push.c`. `processing_limit` in `h2_mplx` caps concurrent streams (DoS
guard). Key files also: `h2_stream`, `h2_request`, `h2_headers`, `h2_conn_ctx`, `h2_c1_io`,
`h2_c2_filter`.

## B11. Expressions & rewriting

**ap_expr** (`include/ap_expr.h`, `server/util_expr_*`): the server-wide expression language. Grammar
in `util_expr_parse.y` (bison) + `util_expr_scan.l` (flex), generated into
`util_expr_parse.{c,h}`/`util_expr_scan.c`; evaluator in `util_expr_eval.c`. `ap_expr_parse[_cmd]`
builds a parse tree of `ap_expr_node`; `ap_expr_exec[_re]` evaluates (boolean or, with
`AP_EXPR_FLAG_STRING_RESULT`, string). Variables (`%{VAR}`), string funcs (`env()`, `req()`,
`tolower()`, `base64()`, `md5()`, `file()`…), unary ops (`-z -n -d -e -f -s -F -U -R …`), binary ops
(`-ipmatch -fnmatch -strmatch ==, =~ …`) are pluggable via the `expr_lookup` hook. Used by `<If>`
(core.c), `RewriteCond expr`, `Require expr`, `SetEnvIfExpr`, `Header ... expr=...`, `mod_filter`,
`LogFormat` conditions, etc.

**mod_rewrite** (`modules/mappers/mod_rewrite.c`): hooks `translate_name` (`hook_uri2file`,
server-level rules) and `fixups` (`hook_fixup`, per-dir/`.htaccess` rules), both `APR_HOOK_FIRST`
(before mod_proxy). RewriteRule regex + RewriteCond chain (which can call ap_expr), flags (`[L] [END]
[R] [P] [QSA] [N] [E] [T] …`), RewriteMap (txt/dbm/dbd/prg/int), looping up to `REWRITE_MAX_ROUNDS`.
For simple mapping, contrast `mod_alias` (prefix/regex) and `mod_dir` (DirectoryIndex).

## B12. APR & coding conventions — read before writing code

**Pools are everything.** Hierarchy: `process->pool` (whole process) ⊃ `pconf` (config, cleared on
restart) / `plog` ⊃ `ptemp` (config parse, thrown away) … `conn->pool` ⊃ `request->pool` ⊃
subrequest/temp pools. Allocate with `apr_palloc`/`apr_pcalloc`/`apr_pstrdup`/`apr_psprintf` against
the right-lifetime pool. **Almost never call `free()`** — memory dies with its pool. For non-memory
resources (fds, locks, external state) register `apr_pool_cleanup_register`. Pick the pool whose
lifetime matches the data: request data in `r->pool`, config in `pconf`, connection data in `c->pool`.
Putting request data in a long-lived pool is a leak.

**Core data types:** `apr_table_t` (string k/v, used for `headers_in/out`, `err_headers_out` [survive
errors & internal redirects], `subprocess_env`, `notes`), `apr_array_header_t`, `apr_hash_t`,
`apr_bucket_brigade`, `apr_status_t` (compare to `APR_SUCCESS`, render with `apr_strerror`),
`apr_pool_t`, `apr_sockaddr_t`, `apr_time_t` (µs since epoch). Iterate tables via `apr_table_elts` →
`apr_table_entry_t[]`.

**Naming/visibility:** `ap_*` = httpd API, `apr_*` = APR. Public functions wrapped in
`AP_DECLARE(type)` (or `AP_DECLARE_NONSTD` for varargs/indirect-call; `AP_CORE_DECLARE` = internal,
don't use from modules). Style: 4-space indent, no tabs, K&R-ish braces (see `emacs-style`,
`.indent.pro`). Pool params named `p`, `pconf`, `plog`, `ptemp`, `r->pool`, `c->pool`.

**Logging** (`include/http_log.h`, `server/log.c`): `ap_log_error(APLOG_MARK, level, status, server,
fmt, ...)`, `ap_log_rerror(...r...)`, `ap_log_cerror(...c...)`, `ap_log_perror(...pool...)`.
`APLOG_MARK` = `__FILE__,__LINE__,APLOG_MODULE_INDEX`. Levels `APLOG_EMERG..APLOG_DEBUG`,
`APLOG_TRACE1..8`. Per-module/per-dir `LogLevel` respected; the macro short-circuits if the level is
disabled. `APLOG_USE_MODULE(foo)` in each module .c sets `APLOG_MODULE_INDEX`. `status` is an
`apr_status_t` (0 if none). This is the **error** log; the **access** log is mod_log_config, separate.

**OS abstraction:** `os/<plat>/os.h` (included as `"os.h"`); `unixd.c` does privilege drop,
daemonize, mutex perms. Guard platform quirks with `CASE_BLIND_FILESYSTEM`, `HAVE_DRIVE_LETTERS`,
`AP_IS_SLASH`, etc.

**Server util files** (`server/`): `util.c` (strings, escaping, regex via `ap_pregcomp`, path
handling), `util_script.c` (CGI env), `util_cookies.c`, `util_time.c`, `util_md5.c`, `util_mutex.c`
(named mutexes / `Mutex` directive), `util_xml.c`, `util_fcgi.c`, `util_charset.c`/`util_ebcdic.c`,
`util_pcre.c`/`util_regex.c`.

## B13. Key reference points (file:line)

- Hook macros: `include/ap_hooks.h`
- Request phase hook impls: `server/request.c:78`
- `ap_read_request`: `server/protocol.c:1423`; async dispatch `modules/http/http_request.c:407`
- Connection processing: `server/connection.c:210`
- Core filters: `server/core_filters.c:90` (input), `:453` (output)
- Filter registry/insert + `ap_pass_brigade`/`ap_get_brigade`: `server/util_filter.c`
- `request_rec`/`conn_rec`/`server_rec`: `include/httpd.h:849/1156/1335`; conn states `:1262`
- MPM contract: `include/ap_mmn.h`; common `server/mpm_common.c`; event `server/mpm/event/event.c`
- Scoreboard: `include/scoreboard.h`, `server/scoreboard.c`
- `main()` + two-pass config: `server/main.c:488`
- Config engine: `server/config.c`; directive/module structs `include/http_config.h`
- Core module: `server/core.c` (`AP_DECLARE_MODULE(core)` near `:5821`)
- Module ABI: `include/ap_mmn.h`
- ap_expr eval: `server/util_expr_eval.c`; grammar `server/util_expr_parse.y`

## B14. Working notes / gotchas (2.4.x)

- **No configured/compiled build tree by default** — there may be no `./configure` output or
  top-level `Makefile`. Before anything compiles you must `./buildconf && ./configure ... && make`.
- **`srclib/` is empty** — APR/APR-util come from the system. You need APR 1.4+ dev headers; configure
  finds them via `apr-config`. (`--with-included-apr` would unpack them into `srclib/`.) **This is the
  biggest build difference from trunk, which bundles APR.**
- **`post_config` runs twice** — guard one-time init with `ap_state_query`.
- **Logging happens at EOR**, after the handler returns, during request-pool cleanup.
- **prefork can't run HTTP/2** (needs async/threaded MPM).
- **Don't allocate request data in `pconf`/`process->pool`** — wrong lifetime = leak.
- New user-visible changes need a file in `changes-entries/` (see `README.CHANGES`).
- `.gdbinit` in the repo root has handy macros (e.g. dumping brigades/pools).
