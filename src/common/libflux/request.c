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
#include "request.h"
#include "message.h"

#include "src/common/libutil/shortjson.h"
#include "src/common/libutil/jsonutil.h"
#include "src/common/libutil/xzmalloc.h"
#include "src/common/libutil/shortjson.h"


int flux_response_recv (flux_t h, JSON *respp, char **tagp, bool nb)
{
    zmsg_t *zmsg;
    int rc = -1;

    if (!(zmsg = flux_response_recvmsg (h, nb)))
        goto done;
    if (flux_msg_get_errnum (zmsg, &errno) < 0 || errno != 0)
        goto done;
    if (flux_msg_decode (zmsg, tagp, respp) < 0)
        goto done;
    rc = 0;
done:
    if (zmsg)
        zmsg_destroy (&zmsg);
    return rc;
}

static zmsg_t *response_matched_recvmsg (flux_t h, const char *match, bool nb)
{
    zmsg_t *zmsg, *response = NULL;
    zlist_t *nomatch = NULL;

    do {
        if (!(response = flux_response_recvmsg (h, nb)))
            goto done;
        if (!flux_msg_streq_topic (response, match)) {
            if (!nomatch && !(nomatch = zlist_new ()))
                oom ();
            if (zlist_append (nomatch, response) < 0)
                oom ();
            response = NULL;
        }
    } while (!response);
done:
    if (nomatch) {
        while ((zmsg = zlist_pop (nomatch))) {
            if (flux_response_putmsg (h, &zmsg) < 0)
                zmsg_destroy (&zmsg);
        }
        zlist_destroy (&nomatch);
    }
    return response;
}

/* If 'o' is non-NULL, encode to string and set as payload in 'zmsg'.
 * Otherwise, clear payload in 'zmsg', if any.
 * Return 0 on success, -1 on failure with errno set.
 */
static int msg_set_payload_json (zmsg_t *zmsg, JSON o)
{
    int rc;
    if (o) {
        const char *s = json_object_to_json_string (o);
        int len = strlen (s);
        rc = flux_msg_set_payload (zmsg, FLUX_MSGFLAG_JSON, (char *)s, len);
    } else
        rc = flux_msg_set_payload (zmsg, 0, NULL, 0);
    return rc;
}

/* If 'zmsg' contains payload, return it in 'o', else set 'o' to NULL.
 * Return 0 on success, -1 on failure with errno set.
 */
static int msg_get_payload_json (zmsg_t *zmsg, JSON *o)
{
    struct json_tokener *tok = NULL;
    int flags;
    char *buf;
    int size;
    int rc = -1;

    assert (o != NULL);

    if (flux_msg_get_payload (zmsg, &flags, (void **)&buf, &size) < 0) {
        errno = 0;
        *o = NULL;
    } else {
        if (!buf || size == 0 || !(flags & FLUX_MSGFLAG_JSON)) {
            errno = EPROTO;
            goto done;
        }
        if (!(tok = json_tokener_new ())) {
            errno = ENOMEM;
            goto done;
        }
        if (!(*o = json_tokener_parse_ex (tok, buf, size))) {
            errno = EPROTO;
            goto done;
        }
    }
    rc = 0;
done:
    if (tok)
        json_tokener_free (tok);
    return rc;
}

int flux_json_request (flux_t h, uint32_t nodeid, const char *topic, JSON in)
{
    zmsg_t *zmsg;
    int rc = -1;

    if (!topic) {
        errno = EINVAL;
        goto done;
    }
    if (!(zmsg = flux_msg_create (FLUX_MSGTYPE_REQUEST)))
        goto done;
    if (flux_msg_set_nodeid (zmsg, nodeid) < 0)
        goto done;
    if (flux_msg_set_topic (zmsg, topic) < 0)
        goto done;
    if (msg_set_payload_json (zmsg, in) < 0)
        goto done;
    if (flux_msg_enable_route (zmsg) < 0)
        goto done;
    rc = flux_request_sendmsg (h, &zmsg);
done:
    zmsg_destroy (&zmsg);
    return rc;
}


int flux_json_rpc (flux_t h, uint32_t nodeid, const char *topic,
                   JSON in, JSON *out)
{
    zmsg_t *zmsg = NULL;
    int rc = -1;
    int errnum;
    JSON o;

    if (flux_json_request (h, nodeid, topic, in) < 0)
        goto done;
    if (!(zmsg = response_matched_recvmsg (h, topic, false)))
        goto done;
    if (flux_msg_get_errnum (zmsg, &errnum) < 0)
        goto done;
    if (errnum != 0) {
        errno = errnum;
        goto done;
    }
    if (msg_get_payload_json (zmsg, &o) < 0)
        goto done;
    /* In order to support flux_rpc(), which in turn must support no-payload
     * responses, this cannot be an error yet.
     */
    if ((!o && out)) {
        *out = NULL;
        //errno = EPROTO;
        //goto done;
    }
    if ((o && !out)) {
        Jput (o);
        errno = EPROTO;
        goto done;
    }
    if (out)
        *out = o;
    rc = 0;
done:
    return rc;
}

int flux_json_respond (flux_t h, int errnum, JSON out, zmsg_t **zmsg)
{
    int rc = -1;

    if (flux_msg_set_type (*zmsg, FLUX_MSGTYPE_RESPONSE) < 0)
        goto done;
    if (flux_msg_set_errnum (*zmsg, errnum) < 0)
        goto done;
    if (errnum)
        out = NULL; /* clear payload */
    if (msg_set_payload_json (*zmsg, out) < 0)
        goto done;
    rc = flux_response_sendmsg (h, zmsg);
done:
    return rc;
}

/**
 ** Deprecated functions.
 */

int flux_respond (flux_t h, zmsg_t **zmsg, JSON o)
{
    return flux_json_respond (h, 0, o, zmsg);
}

int flux_respond_errnum (flux_t h, zmsg_t **zmsg, int errnum)
{
    return flux_json_respond (h, errnum, NULL, zmsg);
}

JSON flux_rank_rpc (flux_t h, int rank, JSON o, const char *fmt, ...)
{
    uint32_t nodeid = rank == -1 ? FLUX_NODEID_ANY : rank;
    va_list ap;
    char *topic;
    JSON out;
    int rc;

    va_start (ap, fmt);
    topic = xvasprintf (fmt, ap);
    va_end (ap);

    rc = flux_json_rpc (h, nodeid, topic, o, &out);
    free (topic);
    return rc < 0 ? NULL : out;
}

int flux_rank_request_send (flux_t h, int rank, JSON o, const char *fmt, ...)
{
    uint32_t nodeid = (rank == -1 ? FLUX_NODEID_ANY : rank);
    va_list ap;
    char *topic;
    int rc;

    va_start (ap, fmt);
    topic = xvasprintf (fmt, ap);
    va_end (ap);

    rc = flux_json_request (h, nodeid, topic, o);
    free (topic);
    return rc;
}

JSON flux_rpc (flux_t h, JSON o, const char *fmt, ...)
{
    va_list ap;
    JSON out;
    char *topic;
    int rc;

    va_start (ap, fmt);
    topic = xvasprintf (fmt, ap);
    va_end (ap);

    rc = flux_json_rpc (h, FLUX_NODEID_ANY, topic, o, &out);
    free (topic);
    return rc < 0 ? NULL : out;
}

int flux_request_send (flux_t h, JSON o, const char *fmt, ...)
{
    va_list ap;
    char *topic;
    int rc;

    va_start (ap, fmt);
    topic = xvasprintf (fmt, ap);
    va_end (ap);

    rc = flux_json_request (h, FLUX_NODEID_ANY, topic, o);
    free (topic);
    return rc;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
