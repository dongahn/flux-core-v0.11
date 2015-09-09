/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <syslog.h>
#include <sys/time.h>
#include <zmq.h>

#include "flog.h"
#include "info.h"
#include "message.h"
#include "rpc.h"

#include "src/common/libutil/shortjson.h"
#include "src/common/libutil/xzmalloc.h"

typedef struct {
    char *facility;
    flux_log_f cb;
    void *cb_arg;
} logctx_t;

static void freectx (void *arg)
{
    logctx_t *ctx = arg;
    if (ctx->facility)
        free (ctx->facility);
    free (ctx);
}

static logctx_t *getctx (flux_t h)
{
    logctx_t *ctx = (logctx_t *)flux_aux_get (h, "flux::log");

    if (!ctx) {
        ctx = xzmalloc (sizeof (*ctx));
        ctx->facility = xstrdup ("unknown");
        flux_aux_set (h, "flux::log", ctx, freectx);
    }
    return ctx;
}

void flux_log_set_facility (flux_t h, const char *facility)
{
    logctx_t *ctx = getctx (h);

    if (ctx->facility)
        free (ctx->facility);
    ctx->facility = xstrdup (facility);
}

void flux_log_set_redirect (flux_t h, flux_log_f fun, void *arg)
{
    logctx_t *ctx = getctx (h);
    ctx->cb = fun;
    ctx->cb_arg = arg;
}

static void log_external (logctx_t *ctx, const char *json_str)
{
    JSON o = NULL;
    const char *facility, *s;
    int rank, level, tv_sec, tv_usec;
    struct timeval tv;

    if ((o = Jfromstr (json_str))
            && Jget_str (o, "facility", &facility)
            && Jget_int (o, "level", &level)
            && Jget_int (o, "rank", &rank)
            && Jget_int (o, "timestamp_usec", &tv_usec)
            && Jget_int (o, "timestamp_sec", &tv_sec)
            && Jget_str (o, "message", &s)) {
        tv.tv_sec = tv_sec;
        tv.tv_usec = tv_usec;
        ctx->cb (facility, level, (uint32_t)rank, tv, s, ctx->cb_arg);
        Jput (o);
    }
}

int flux_log_json (flux_t h, const char *json_str)
{
    logctx_t *ctx = getctx (h);
    flux_rpc_t *r = NULL;
    int rc = -1;

    if (ctx->cb)
        log_external (ctx, json_str);
    else if (!(r = flux_rpc (h, "cmb.log", json_str, 0, FLUX_RPC_NORESPONSE)))
        goto done;
    rc = 0;
done:
    flux_rpc_destroy (r);
    return rc;
}

int flux_vlog (flux_t h, int lev, const char *fmt, va_list ap)
{
    logctx_t *ctx = getctx (h);
    char *s = xvasprintf (fmt, ap);
    struct timeval tv;
    JSON o = Jnew ();
    uint32_t rank;
    int rc = -1;

    if (gettimeofday (&tv, NULL) < 0)
        err_exit ("gettimeofday");
    if (flux_get_rank (h, &rank) < 0)
        goto done;
    Jadd_str (o, "facility", ctx->facility);
    Jadd_int (o, "level", lev);
    Jadd_int (o, "rank", rank);
    Jadd_int (o, "timestamp_usec", (int)tv.tv_usec);
    Jadd_int (o, "timestamp_sec", (int)tv.tv_sec);
    Jadd_str (o, "message", s);
    rc = flux_log_json (h, Jtostr (o));
done:
    if (s)
        free (s);
    Jput (o);
    return rc;
}

int flux_log (flux_t h, int lev, const char *fmt, ...)
{
    va_list ap;
    int rc;

    va_start (ap, fmt);
    rc = flux_vlog (h, lev, fmt, ap);
    va_end (ap);
    return rc;
}

int flux_log_verror (flux_t h, const char *fmt, va_list ap)
{
    char *s = xvasprintf (fmt, ap);
    int rc;

    rc = flux_log (h, LOG_ERR, "%s: %s", s, zmq_strerror (errno));
    free (s);

    return rc;
}

int flux_log_error (flux_t h, const char *fmt, ...)
{
    va_list ap;
    int rc;

    va_start (ap, fmt);
    rc = flux_log_verror (h, fmt, ap);
    va_end (ap);

    return rc;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
