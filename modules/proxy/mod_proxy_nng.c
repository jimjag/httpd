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
 * mod_proxy_nng -- Phase 1 proof-of-concept for an nng (nanomsg-next-gen)
 * PUB/SUB channel between a front-end reverse proxy and its backend balancers.
 *
 * Direction: backends announce themselves to the proxy.  Because an nng PUB
 * socket is send-only, the data direction (backend -> proxy) makes the backend
 * the PUBlisher and the proxy the SUBscriber:
 *
 *   - Reverse proxy:  ProxyNngSubscribe tcp://0.0.0.0:5555   (SUB, listens)
 *   - Backend server: ProxyNngPublish   tcp://proxy-host:5555 (PUB, dials in)
 *
 * The proxy is the stable rendezvous listener; each backend dials in on
 * startup and periodically publishes an "ANNOUNCE" heartbeat.  The proxy logs
 * every announcement it receives.  This PoC only proves the channel; it does
 * not yet feed the balancer member list.
 *
 * The background work runs on a mod_watchdog SINGLETON instance (exactly one
 * child process owns the socket), mirroring mod_proxy_hcheck.c.
 */

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "ap_provider.h"
#include "mod_watchdog.h"

#include "apr_strings.h"
#include "apr_time.h"

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

typedef struct {
    apr_pool_t          *p;            /* subpool for this context */
    server_rec          *s;            /* canonical server identity */
    char                *pub_url;      /* ProxyNngPublish   (backend dial url) */
    char                *sub_url;      /* ProxyNngSubscribe (proxy listen url) */
    apr_interval_time_t  interval;     /* ProxyNngInterval (announce period) */
    nng_role_e           role;
    nng_socket           sock;         /* live socket; valid only when opened */
    int                  opened;       /* guard double-open / stop-without-start */
    apr_time_t           last_publish; /* PUB throttle */
    apr_uint64_t         seq;          /* announcement counter */
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

static const command_rec nng_cmds[] = {
    AP_INIT_TAKE1("ProxyNngSubscribe", nng_set_sub_url, NULL, RSRC_CONF,
                  "nng SUB listen URL on the reverse proxy, "
                  "e.g. tcp://0.0.0.0:5555"),
    AP_INIT_TAKE1("ProxyNngPublish", nng_set_pub_url, NULL, RSRC_CONF,
                  "nng PUB dial URL to the proxy from a backend, "
                  "e.g. tcp://proxy-host:5555"),
    AP_INIT_TAKE1("ProxyNngInterval", nng_set_interval, NULL, RSRC_CONF,
                  "backend announcement interval in seconds (default 5)"),
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
            char *msg = apr_psprintf(pool,
                            "ANNOUNCE host=%s pid=%" APR_PID_T_FMT
                            " seq=%" APR_UINT64_T_FMT,
                            ctx->s->server_hostname
                                ? ctx->s->server_hostname : "(unknown)",
                            getpid(), ctx->seq++);
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
            nng_free(buf, sz);
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
    static const char *const aszSucc[] = { "mod_watchdog.c", NULL };
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
