/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * mod_proxy_nng -- an nng (nanomsg-next-gen) PUB/SUB channel between a
 * front-end reverse proxy and its backend balancers.
 *
 * Direction: backends announce themselves to the proxy.  Because an nng PUB
 * socket is send-only, the data direction (backend -> proxy) makes the backend
 * the PUBlisher and the proxy the SUBscriber:
 *
 *   - Reverse proxy:  ProxyNngSubscribe tcp://0.0.0.0:5555   (SUB, listens)
 *   - Backend server: ProxyNngPublish   tcp://proxy-host:5555 (PUB, dials in)
 *
 * The proxy is the stable rendezvous listener; each backend dials in on
 * startup and periodically publishes an "ANNOUNCE" heartbeat.
 *
 * Phase 1 (the channel): the proxy simply logged each announcement, proving the
 * PUB/SUB transport works between separate, remote servers over TCP.
 *
 * Phase 2: when the proxy SUB receives an announcement that carries a backend
 * URL, it ADDS that backend as a member (worker) of a configured balancer and
 * ENABLES it -- the dynamic equivalent of "Add Worker" in the balancer-manager
 * web UI, but driven by the backend announcing itself.  We reuse
 * mod_proxy_balancer's exported balancer_manage() optional function (the same
 * core balancer_handler calls), so the add path is identical to the proven
 * manager flow.  The backend advertises its routable URL via ProxyNngAdvertise;
 * the proxy names the target balancer via ProxyNngBalancer.
 *
 * Phase 3 (this file): the inverse -- when a backend STOPS announcing for longer
 * than ProxyNngTimeout, the proxy takes it out of rotation by setting the
 * worker's DISABLED flag (again via balancer_manage), and re-enables it if the
 * backend starts announcing again.  This gives the cluster automatic liveness
 * without operator action.  The timeout is a config directive (0 = disabled =
 * pure Phase-2 behavior); a live-tunable timeout is a future extension.  All of
 * this runs in the watchdog singleton that already owns the SUB socket, so the
 * per-URL last-seen table lives in this process's ctx -- no shared memory.
 *
 * The background work runs on a mod_watchdog SINGLETON instance (exactly one
 * child process owns the socket), mirroring mod_proxy_hcheck.c.  That child has
 * no request_rec, so the SUB synthesizes a minimal one for balancer_manage()
 * (see nng_make_fake_request -- adapted from mod_proxy_hcheck's
 * create_request_rec, plus a fake conn_rec that ap_log_rerror requires).
 */

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "ap_provider.h"
#include "mod_watchdog.h"
#include "mod_proxy.h"

#include "apr_strings.h"
#include "apr_time.h"
#include "apr_hash.h"
#include "apr_uri.h"

#if APR_HAVE_UNISTD_H
#include <unistd.h>             /* for getpid() */
#endif

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

module AP_MODULE_DECLARE_DATA proxy_nng_module;

#define NNG_WATCHDOG_NAME       "_proxy_nng_"
#define NNG_DEFAULT_INTERVAL    apr_time_from_sec(5)

typedef enum {
    NNG_ROLE_NONE = 0,
    NNG_ROLE_PUB,   /* backend: dials the proxy and announces itself */
    NNG_ROLE_SUB    /* reverse proxy: listens and receives announcements */
} nng_role_e;

/* SUB-side per-backend state, keyed by url in ctx->seen. */
typedef struct {
    apr_time_t last_seen;  /* updated on every ANNOUNCE for this url */
    int        added;      /* worker exists in the balancer (add dedup) */
    int        evicted;    /* WE set DISABLED on timeout (guards flip churn) */
} nng_member_t;

typedef struct {
    apr_pool_t          *p;            /* subpool for this context */
    server_rec          *s;            /* canonical server identity */
    char                *pub_url;      /* ProxyNngPublish   (backend dial url) */
    char                *sub_url;      /* ProxyNngSubscribe (proxy listen url) */
    char                *advertise_url;/* ProxyNngAdvertise (PUB: routable url) */
    char                *balancer_name;/* ProxyNngBalancer  (SUB: bare bal name) */
    apr_interval_time_t  interval;     /* ProxyNngInterval (announce period) */
    apr_interval_time_t  evict_timeout;/* ProxyNngTimeout (0 = no eviction) */
    nng_role_e           role;
    nng_socket           sock;         /* live socket; valid only when opened */
    int                  opened;       /* guard double-open / stop-without-start */
    apr_time_t           last_publish; /* PUB throttle */
    apr_time_t           last_sweep;   /* SUB eviction-scan throttle */
    apr_uint64_t         seq;          /* announcement counter */
    apr_hash_t          *seen;         /* SUB: url -> nng_member_t* */
    APR_OPTIONAL_FN_TYPE(balancer_manage) *manage_fn; /* SUB: cached, lazy */
} nng_ctx_t;

/* Process-wide watchdog handle, like mod_proxy_hcheck's static watchdog. */
static ap_watchdog_t *nng_watchdog;

/*
 * Per-server config.  No nng calls here -- the role may not even be known yet,
 * and this runs in the parent at config-parse time (possibly more than once).
 */
static void *nng_create_server_config(apr_pool_t *p, server_rec *s)
{
    nng_ctx_t *ctx = apr_pcalloc(p, sizeof(nng_ctx_t));

    ctx->s = s;
    apr_pool_create(&ctx->p, p);
    apr_pool_tag(ctx->p, "proxy_nng");
    ctx->interval = NNG_DEFAULT_INTERVAL;
    ctx->role = NNG_ROLE_NONE;
    ctx->seen = apr_hash_make(ctx->p);

    return ctx;
}

static const char *nng_set_sub_url(cmd_parms *cmd, void *dummy, const char *arg)
{
    nng_ctx_t *ctx = ap_get_module_config(cmd->server->module_config,
                                          &proxy_nng_module);
    const char *err = ap_check_cmd_context(cmd, NOT_IN_HTACCESS);
    if (err) {
        return err;
    }
    if (ctx->pub_url) {
        return "ProxyNngSubscribe and ProxyNngPublish are mutually exclusive "
               "on the same server";
    }
    ctx->sub_url = apr_pstrdup(cmd->pool, arg);
    ctx->role = NNG_ROLE_SUB;
    return NULL;
}

static const char *nng_set_pub_url(cmd_parms *cmd, void *dummy, const char *arg)
{
    nng_ctx_t *ctx = ap_get_module_config(cmd->server->module_config,
                                          &proxy_nng_module);
    const char *err = ap_check_cmd_context(cmd, NOT_IN_HTACCESS);
    if (err) {
        return err;
    }
    if (ctx->sub_url) {
        return "ProxyNngSubscribe and ProxyNngPublish are mutually exclusive "
               "on the same server";
    }
    ctx->pub_url = apr_pstrdup(cmd->pool, arg);
    ctx->role = NNG_ROLE_PUB;
    return NULL;
}

static const char *nng_set_advertise(cmd_parms *cmd, void *dummy,
                                     const char *arg)
{
    nng_ctx_t *ctx = ap_get_module_config(cmd->server->module_config,
                                          &proxy_nng_module);
    apr_uri_t uri;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_HTACCESS);
    if (err) {
        return err;
    }
    /* Must be a routable backend URL (scheme://host[:port]); it becomes a
     * BalancerMember on the proxy. */
    if (apr_uri_parse(cmd->pool, arg, &uri) != APR_SUCCESS
        || !uri.scheme || !uri.hostname) {
        return apr_psprintf(cmd->pool,
                            "ProxyNngAdvertise: '%s' is not a valid "
                            "scheme://host[:port] URL", arg);
    }
    ctx->advertise_url = apr_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char *nng_set_balancer(cmd_parms *cmd, void *dummy,
                                    const char *arg)
{
    nng_ctx_t *ctx = ap_get_module_config(cmd->server->module_config,
                                          &proxy_nng_module);
    const char *err = ap_check_cmd_context(cmd, NOT_IN_HTACCESS);
    if (err) {
        return err;
    }
    /* Store the bare name; balancer_manage() prepends BALANCER_PREFIX itself.
     * Be forgiving if the admin wrote the full "balancer://name". */
    if (!strncasecmp(arg, BALANCER_PREFIX, sizeof(BALANCER_PREFIX) - 1)) {
        arg += sizeof(BALANCER_PREFIX) - 1;
    }
    if (!*arg) {
        return "ProxyNngBalancer: empty balancer name";
    }
    ctx->balancer_name = apr_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char *nng_set_interval(cmd_parms *cmd, void *dummy,
                                    const char *arg)
{
    nng_ctx_t *ctx = ap_get_module_config(cmd->server->module_config,
                                          &proxy_nng_module);
    apr_interval_time_t iv;
    apr_status_t rv;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_HTACCESS);
    if (err) {
        return err;
    }
    rv = ap_timeout_parameter_parse(arg, &iv, "s");
    if (rv != APR_SUCCESS) {
        return "Unparse-able ProxyNngInterval setting";
    }
    if (iv < AP_WD_TM_SLICE) {
        return apr_psprintf(cmd->pool,
                            "ProxyNngInterval must be greater than %"
                            APR_TIME_T_FMT "ms",
                            apr_time_as_msec(AP_WD_TM_SLICE));
    }
    ctx->interval = iv;
    return NULL;
}

static const char *nng_set_timeout(cmd_parms *cmd, void *dummy,
                                   const char *arg)
{
    nng_ctx_t *ctx = ap_get_module_config(cmd->server->module_config,
                                          &proxy_nng_module);
    apr_interval_time_t iv;
    apr_status_t rv;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_HTACCESS);
    if (err) {
        return err;
    }
    rv = ap_timeout_parameter_parse(arg, &iv, "s");
    if (rv != APR_SUCCESS || iv < 0) {
        return "ProxyNngTimeout must be a non-negative time (0 disables "
               "eviction)";
    }
    /* 0 = eviction disabled (pure announce-and-add behavior). */
    ctx->evict_timeout = iv;
    return NULL;
}

static const command_rec nng_cmds[] = {
    AP_INIT_TAKE1("ProxyNngSubscribe", nng_set_sub_url, NULL, RSRC_CONF,
                  "nng SUB listen URL on the reverse proxy, "
                  "e.g. tcp://0.0.0.0:5555"),
    AP_INIT_TAKE1("ProxyNngPublish", nng_set_pub_url, NULL, RSRC_CONF,
                  "nng PUB dial URL to the proxy from a backend, "
                  "e.g. tcp://proxy-host:5555"),
    AP_INIT_TAKE1("ProxyNngInterval", nng_set_interval, NULL, RSRC_CONF,
                  "backend announcement interval in seconds (default 5)"),
    AP_INIT_TAKE1("ProxyNngAdvertise", nng_set_advertise, NULL, RSRC_CONF,
                  "backend: routable URL to advertise to the proxy, "
                  "e.g. http://10.0.0.5:8080 (added as a BalancerMember)"),
    AP_INIT_TAKE1("ProxyNngBalancer", nng_set_balancer, NULL, RSRC_CONF,
                  "proxy: name of the balancer that announced backends are "
                  "added to, e.g. nng (for balancer://nng)"),
    AP_INIT_TAKE1("ProxyNngTimeout", nng_set_timeout, NULL, RSRC_CONF,
                  "proxy: seconds without an announcement after which a backend "
                  "is disabled (taken out of rotation); 0 disables eviction"),
    { NULL }
};

/* STARTING: open the socket and listen (SUB) or dial (PUB). */
static void nng_cb_starting(nng_ctx_t *ctx)
{
    server_rec *s = ctx->s;
    int rv;

    if (ctx->opened) {
        return;
    }

    if (ctx->role == NNG_ROLE_SUB) {
        if ((rv = nng_sub0_open(&ctx->sock)) != 0) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                         "mod_proxy_nng: nng_sub0_open failed: %s",
                         nng_strerror(rv));
            return;
        }
        /* Empty prefix subscribes to everything. */
        if ((rv = nng_sub0_socket_subscribe(ctx->sock, "", 0)) != 0) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                         "mod_proxy_nng: subscribe failed: %s",
                         nng_strerror(rv));
            nng_close(ctx->sock);
            return;
        }
        if ((rv = nng_listen(ctx->sock, ctx->sub_url, NULL, 0)) != 0) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                         "mod_proxy_nng: nng_listen(%s) failed: %s",
                         ctx->sub_url, nng_strerror(rv));
            nng_close(ctx->sock);
            return;
        }
        ctx->opened = 1;
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s,
                     "mod_proxy_nng: SUB listening on %s", ctx->sub_url);
    }
    else if (ctx->role == NNG_ROLE_PUB) {
        if ((rv = nng_pub0_open(&ctx->sock)) != 0) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                         "mod_proxy_nng: nng_pub0_open failed: %s",
                         nng_strerror(rv));
            return;
        }
        /*
         * NNG_FLAG_NONBLOCK: do not block this watchdog thread waiting for the
         * proxy to be up.  nng retries the connection in the background, so a
         * backend that starts before the proxy will connect once the proxy
         * appears.
         */
        if ((rv = nng_dial(ctx->sock, ctx->pub_url, NULL,
                           NNG_FLAG_NONBLOCK)) != 0) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                         "mod_proxy_nng: nng_dial(%s) failed: %s",
                         ctx->pub_url, nng_strerror(rv));
            nng_close(ctx->sock);
            return;
        }
        ctx->opened = 1;
        ctx->last_publish = 0; /* announce on the first running tick */
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s,
                     "mod_proxy_nng: PUB dialing %s", ctx->pub_url);
    }
}

/*
 * Synthesize a minimal request_rec for balancer_manage(), which runs here in
 * the watchdog thread with no real client request.  Adapted from
 * mod_proxy_hcheck.c's create_request_rec (same watchdog context), but that one
 * attaches a real backend conn_rec before logging; we have none, so we fabricate
 * a minimal conn_rec too.
 *
 * balancer_manage()/balancer_process_balancer_worker() touch only r->pool,
 * r->server, and ap_log_rerror().  ap_log_rerror is the trap: log_error_core()
 * asserts r->connection != NULL and do_errorlog_default() unconditionally reads
 * r->connection->outgoing, while add_log_id()->core_generate_log_id() reads
 * c->current_thread.  So we (a) give r a non-NULL conn_rec with ->outgoing set,
 * and (b) pre-set r->log_id / c->log_id so the log-id generation path is skipped
 * entirely.
 */
static request_rec *nng_make_fake_request(apr_pool_t *p, server_rec *s)
{
    conn_rec *c = apr_pcalloc(p, sizeof(*c));
    request_rec *r = apr_pcalloc(p, sizeof(*r));

    c->pool         = p;
    c->base_server  = s;
    c->log          = &s->log;
    c->log_id       = "nng";    /* non-NULL -> skip add_log_id/log-id gen */
    c->outgoing     = 1;        /* read by do_errorlog_default() */
    c->client_ip    = "-";
    c->notes        = apr_table_make(p, 1);

    r->pool           = p;
    r->server         = s;
    r->connection     = c;
    r->log            = &s->log;
    r->log_id         = "nng";
    r->per_dir_config = s->lookup_defaults;
    r->request_config = ap_create_request_config(p);
    r->notes          = apr_table_make(p, 4);
    r->subprocess_env = apr_table_make(p, 4);
    r->headers_in     = apr_table_make(p, 1);
    r->headers_out    = apr_table_make(p, 1);
    r->err_headers_out = apr_table_make(p, 1);
    r->useragent_ip   = "-";
    r->useragent_addr = NULL;
    r->proxyreq       = PROXYREQ_RESPONSE;
    r->status         = HTTP_OK;

    return r;
}

/*
 * Resolve mod_proxy_balancer's balancer_manage() optional fn (lazily, cached).
 * Registered at hook-registration time, so available by the time we run.
 */
static int nng_have_manage_fn(nng_ctx_t *ctx)
{
    if (!ctx->manage_fn) {
        ctx->manage_fn = APR_RETRIEVE_OPTIONAL_FN(balancer_manage);
        if (!ctx->manage_fn) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, ctx->s,
                         "mod_proxy_nng: balancer_manage unavailable "
                         "(is mod_proxy_balancer loaded?)");
            return 0;
        }
    }
    return 1;
}

/*
 * Set (or clear) a worker's DISABLED flag via balancer_manage -- the exact path
 * the balancer-manager UI uses (params { b, w, w_status_D }).  This is how both
 * "enable" (disabled=0) and "evict" (disabled=1) are performed.  Returns
 * APR_SUCCESS on success.
 */
static apr_status_t nng_set_worker_disabled(nng_ctx_t *ctx, apr_pool_t *pool,
                                            const char *url, int disabled)
{
    apr_pool_t *subp;
    request_rec *r;
    apr_table_t *params;
    apr_status_t rv;

    apr_pool_create(&subp, pool);
    r = nng_make_fake_request(subp, ctx->s);

    params = apr_table_make(subp, 4);
    apr_table_setn(params, "b", ctx->balancer_name);
    apr_table_setn(params, "w", url);
    apr_table_setn(params, "w_status_D", disabled ? "1" : "0");
    rv = ctx->manage_fn(r, params);

    apr_pool_destroy(subp);
    return rv;
}

/*
 * Add (and enable) a freshly-announced backend as a member of the configured
 * balancer, reusing balancer_manage().  Two calls: add (the worker is created
 * DISABLED by default), then clear the DISABLED flag so it serves traffic.
 * Records an nng_member_t in ctx->seen so repeated announcements are idempotent
 * (and so the eviction sweep can track last-seen time).
 */
static void nng_add_member(nng_ctx_t *ctx, apr_pool_t *pool, const char *url,
                           apr_time_t now)
{
    server_rec *s = ctx->s;
    apr_pool_t *subp;
    request_rec *r;
    apr_table_t *params;
    nng_member_t *m;
    apr_status_t rv;

    if (!nng_have_manage_fn(ctx)) {
        return;
    }

    apr_pool_create(&subp, pool);
    r = nng_make_fake_request(subp, s);

    /* Step 1: add the worker (created DISABLED). */
    params = apr_table_make(subp, 4);
    apr_table_setn(params, "b", ctx->balancer_name);
    apr_table_setn(params, "b_nwrkr", url);
    apr_table_setn(params, "b_wyes", "1");
    rv = ctx->manage_fn(r, params);
    apr_pool_destroy(subp);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "mod_proxy_nng: failed to add worker %s to balancer://%s",
                     url, ctx->balancer_name);
        return;
    }

    /* Step 2: enable it (clear the DISABLED flag) so it serves traffic. */
    rv = nng_set_worker_disabled(ctx, pool, url, 0);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s,
                     "mod_proxy_nng: added but failed to enable worker %s "
                     "in balancer://%s", url, ctx->balancer_name);
        /* fall through: still a member, just disabled */
    }

    /* Record membership + last-seen (now, never 0, so the sweep won't
     * immediately evict a just-added member). */
    m = apr_pcalloc(ctx->p, sizeof(*m));
    m->last_seen = now;
    m->added = 1;
    m->evicted = 0;
    apr_hash_set(ctx->seen, apr_pstrdup(ctx->p, url), APR_HASH_KEY_STRING, m);

    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s,
                 "mod_proxy_nng: added backend %s to balancer://%s",
                 url, ctx->balancer_name);
}

/*
 * Handle one announced url: add it if new; otherwise refresh last_seen and, if
 * we had evicted it on timeout, re-enable it (clear DISABLED).
 */
static void nng_handle_announce(nng_ctx_t *ctx, apr_pool_t *pool,
                                const char *url, apr_time_t now)
{
    nng_member_t *m = apr_hash_get(ctx->seen, url, APR_HASH_KEY_STRING);

    if (!m) {
        nng_add_member(ctx, pool, url, now);
        return;
    }

    m->last_seen = now;

    if (m->evicted) {
        if (nng_set_worker_disabled(ctx, pool, url, 0) == APR_SUCCESS) {
            m->evicted = 0;
            ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ctx->s,
                         "mod_proxy_nng: re-enabled backend %s in "
                         "balancer://%s (announcing again)",
                         url, ctx->balancer_name);
        }
    }
}

/*
 * Eviction sweep: disable any member that has not announced within
 * evict_timeout.  No-op when eviction is disabled (timeout 0).  Runs in the
 * watchdog singleton, throttled by the caller.
 */
static void nng_sweep(nng_ctx_t *ctx, apr_pool_t *pool, apr_time_t now)
{
    apr_hash_index_t *hi;

    if (ctx->evict_timeout <= 0 || !nng_have_manage_fn(ctx)) {
        return;
    }

    for (hi = apr_hash_first(pool, ctx->seen); hi; hi = apr_hash_next(hi)) {
        const void *key;
        void *val;
        nng_member_t *m;

        apr_hash_this(hi, &key, NULL, &val);
        m = val;
        if (!m->added || m->evicted) {
            continue;
        }
        if (now - m->last_seen > ctx->evict_timeout) {
            const char *url = key;
            if (nng_set_worker_disabled(ctx, pool, url, 1) == APR_SUCCESS) {
                m->evicted = 1;
                ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ctx->s,
                             "mod_proxy_nng: evicted backend %s from "
                             "balancer://%s (no announcement for %"
                             APR_TIME_T_FMT "s)",
                             url, ctx->balancer_name,
                             apr_time_sec(now - m->last_seen));
            }
        }
    }
}

/*
 * Extract the value of the "url=" token from an ANNOUNCE message into buf.
 * Returns 1 on success.  The payload is untrusted network input, so parse
 * defensively: the token is "url=" followed by non-space characters.
 */
static int nng_parse_url(const char *msg, char *buf, apr_size_t buflen)
{
    const char *p = msg;

    while (p && *p) {
        if (!strncmp(p, "url=", 4)) {
            apr_size_t i = 0;
            p += 4;
            while (*p && *p != ' ' && i + 1 < buflen) {
                buf[i++] = *p++;
            }
            buf[i] = '\0';
            return (i > 0);
        }
        /* advance to the next space-separated token */
        p = strchr(p, ' ');
        if (p) {
            p++;
        }
    }
    return 0;
}

/* RUNNING: PUB sends a throttled heartbeat; SUB drains its receive queue. */
static void nng_cb_running(nng_ctx_t *ctx, apr_pool_t *pool)
{
    server_rec *s = ctx->s;
    int rv;

    if (!ctx->opened) {
        return;
    }

    if (ctx->role == NNG_ROLE_PUB) {
        apr_time_t now = apr_time_now();
        if (now >= ctx->last_publish + ctx->interval) {
            const char *host = ctx->s->server_hostname
                                   ? ctx->s->server_hostname : "(unknown)";
            char *msg;
            /* Carry the routable URL so the proxy can add us as a member.
             * Without ProxyNngAdvertise, fall back to the Phase-1 payload
             * (channel proof only; the SUB just logs it). */
            if (ctx->advertise_url) {
                msg = apr_psprintf(pool,
                            "ANNOUNCE url=%s host=%s pid=%" APR_PID_T_FMT
                            " seq=%" APR_UINT64_T_FMT,
                            ctx->advertise_url, host, getpid(), ctx->seq++);
            }
            else {
                msg = apr_psprintf(pool,
                            "ANNOUNCE host=%s pid=%" APR_PID_T_FMT
                            " seq=%" APR_UINT64_T_FMT,
                            host, getpid(), ctx->seq++);
            }
            /* nng copies the data on send; msg lives in the temp pool. */
            if ((rv = nng_send(ctx->sock, msg, strlen(msg) + 1, 0)) != 0) {
                ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s,
                             "mod_proxy_nng: nng_send failed: %s",
                             nng_strerror(rv));
            }
            ctx->last_publish = now;
        }
    }
    else if (ctx->role == NNG_ROLE_SUB) {
        apr_time_t now;

        /* Drain the receive queue every tick (re-enable latency matters). */
        for (;;) {
            char *buf = NULL;
            size_t sz = 0;
            rv = nng_recv(ctx->sock, &buf, &sz,
                          NNG_FLAG_ALLOC | NNG_FLAG_NONBLOCK);
            if (rv == NNG_EAGAIN) {
                break; /* queue drained */
            }
            if (rv != 0) {
                ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s,
                             "mod_proxy_nng: nng_recv failed: %s",
                             nng_strerror(rv));
                break;
            }
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                         "mod_proxy_nng: SUB received: %.*s",
                         (int)sz, buf ? buf : "");

            /* Phase 2/3: if a target balancer is configured and the message
             * carries a routable url=, add the backend (or refresh its
             * last-seen / re-enable it if previously evicted). */
            if (ctx->balancer_name && buf && sz > 0) {
                char url[512];
                /* ensure NUL-terminated for the parser */
                buf[sz - 1] = '\0';
                if (nng_parse_url(buf, url, sizeof(url))) {
                    nng_handle_announce(ctx, pool, url, apr_time_now());
                }
            }

            nng_free(buf, sz);
        }

        /* Phase 3: evict backends that stopped announcing.  Throttle the scan
         * to ~1s (the recv drain above already runs every tick). */
        now = apr_time_now();
        if (ctx->balancer_name && ctx->evict_timeout > 0
            && now - ctx->last_sweep >= apr_time_from_sec(1)) {
            nng_sweep(ctx, pool, now);
            ctx->last_sweep = now;
        }
    }
}

/* STOPPING: close the socket if we opened it. */
static void nng_cb_stopping(nng_ctx_t *ctx)
{
    if (ctx->opened) {
        nng_close(ctx->sock);
        ctx->opened = 0;
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ctx->s,
                     "mod_proxy_nng: socket closed");
    }
}

/*
 * Watchdog callback.  Always returns APR_SUCCESS: a non-success return would
 * unregister the callback and permanently stop the channel, which we never
 * want for a transient nng error.
 */
static apr_status_t nng_watchdog_callback(int state, void *data,
                                          apr_pool_t *pool)
{
    nng_ctx_t *ctx = (nng_ctx_t *)data;

    switch (state) {
    case AP_WATCHDOG_STATE_STARTING:
        nng_cb_starting(ctx);
        break;
    case AP_WATCHDOG_STATE_RUNNING:
        nng_cb_running(ctx, pool);
        break;
    case AP_WATCHDOG_STATE_STOPPING:
        nng_cb_stopping(ctx);
        break;
    }

    return APR_SUCCESS;
}

static int nng_post_config(apr_pool_t *pconf, apr_pool_t *plog,
                           apr_pool_t *ptemp, server_rec *main_s)
{
    apr_status_t rv;
    server_rec *s;
    APR_OPTIONAL_FN_TYPE(ap_watchdog_get_instance) *nng_get_instance;
    APR_OPTIONAL_FN_TYPE(ap_watchdog_register_callback) *nng_register_callback;

    /* post_config runs twice; skip the initial config-test pass. */
    if (ap_state_query(AP_SQ_MAIN_STATE) == AP_SQ_MS_CREATE_PRE_CONFIG) {
        return OK;
    }

    nng_get_instance = APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_get_instance);
    nng_register_callback =
        APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_register_callback);
    if (!nng_get_instance || !nng_register_callback) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, main_s,
                     "mod_proxy_nng: mod_watchdog is required");
        return !OK;
    }

    rv = nng_get_instance(&nng_watchdog, NNG_WATCHDOG_NAME, 0, 1, pconf);
    if (rv) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, main_s,
                     "mod_proxy_nng: failed to create watchdog instance (%s)",
                     NNG_WATCHDOG_NAME);
        return !OK;
    }

    for (s = main_s; s; s = s->next) {
        nng_ctx_t *ctx = ap_get_module_config(s->module_config,
                                              &proxy_nng_module);
        /* Skip non-canonical server contexts (shared across vhosts). */
        if (s != ctx->s) {
            continue;
        }
        /* Only servers with a directive participate. */
        if (ctx->role == NNG_ROLE_NONE) {
            continue;
        }
        rv = nng_register_callback(nng_watchdog, AP_WD_TM_SLICE, ctx,
                                   nng_watchdog_callback);
        if (rv) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s,
                         "mod_proxy_nng: failed to register watchdog callback");
            return !OK;
        }
    }

    return OK;
}

static void nng_register_hooks(apr_pool_t *p)
{
    static const char *const aszSucc[] = { "mod_watchdog.c",
                                           "mod_proxy_balancer.c", NULL };
    ap_hook_post_config(nng_post_config, NULL, aszSucc, APR_HOOK_LAST);
}

AP_DECLARE_MODULE(proxy_nng) =
{
    STANDARD20_MODULE_STUFF,
    NULL,                       /* create per-dir config */
    NULL,                       /* merge per-dir config */
    nng_create_server_config,   /* create per-server config */
    NULL,                       /* merge per-server config */
    nng_cmds,                   /* command table */
    nng_register_hooks          /* register hooks */
};
