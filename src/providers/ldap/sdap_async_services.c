/*
    SSSD

    Authors:
        Stephen Gallagher <sgallagh@redhat.com>

    Copyright (C) 2012 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "util/util.h"
#include "db/sysdb.h"
#include "db/sysdb_services.h"
#include "providers/ldap/sdap_async_private.h"
#include "providers/ldap/ldap_common.h"

struct sdap_get_services_state {
    struct tevent_context *ev;
    struct sdap_options *opts;
    struct sdap_handle *sh;
    struct sss_domain_info *dom;
    struct sysdb_ctx *sysdb;
    const char **attrs;
    const char *base_filter;
    char *filter;
    int timeout;
    bool enumeration;

    char *higher_usn;
    struct sysdb_attrs **services;
    size_t count;

    size_t base_iter;
    struct sdap_search_base **search_bases;
};

static errno_t
sdap_get_services_next_base(struct tevent_req *req);
static void
sdap_get_services_process(struct tevent_req *subreq);
static errno_t
sdap_save_services(TALLOC_CTX *memctx,
                   struct sysdb_ctx *sysdb,
                   const char **attrs,
                   struct sss_domain_info *dom,
                   struct sdap_options *opts,
                   struct sysdb_attrs **services,
                   size_t num_services,
                   char **_usn_value);
static errno_t
sdap_save_service(TALLOC_CTX *mem_ctx,
                  struct sysdb_ctx *sysdb,
                  struct sdap_options *opts,
                  struct sss_domain_info *dom,
                  struct sysdb_attrs *attrs,
                  const char **ldap_attrs,
                  char **_usn_value,
                  time_t now);

struct tevent_req *
sdap_get_services_send(TALLOC_CTX *memctx,
                       struct tevent_context *ev,
                       struct sss_domain_info *dom,
                       struct sysdb_ctx *sysdb,
                       struct sdap_options *opts,
                       struct sdap_search_base **search_bases,
                       struct sdap_handle *sh,
                       const char **attrs,
                       const char *filter,
                       int timeout,
                       bool enumeration)
{
    errno_t ret;
    struct tevent_req *req;
    struct sdap_get_services_state *state;

    req = tevent_req_create(memctx, &state, struct sdap_get_services_state);
    if (!req) return NULL;

    state->ev = ev;
    state->opts = opts;
    state->dom = dom;
    state->sh = sh;
    state->sysdb = sysdb;
    state->attrs = attrs;
    state->higher_usn = NULL;
    state->services =  NULL;
    state->count = 0;
    state->timeout = timeout;
    state->base_filter = filter;
    state->base_iter = 0;
    state->search_bases = search_bases;
    state->enumeration = enumeration;

    ret = sdap_get_services_next_base(req);
    if (ret != EOK) {
        tevent_req_error(req, ret);
        tevent_req_post(req, state->ev);
    }

    return req;
}

static errno_t
sdap_get_services_next_base(struct tevent_req *req)
{
    struct tevent_req *subreq;
    struct sdap_get_services_state *state;

    state = tevent_req_data(req, struct sdap_get_services_state);

    talloc_zfree(state->filter);
    state->filter = sdap_get_id_specific_filter(state,
                        state->base_filter,
                        state->search_bases[state->base_iter]->filter);
    if (!state->filter) {
        return ENOMEM;
    }

    DEBUG(SSSDBG_TRACE_FUNC,
          ("Searching for services with base [%s]\n",
           state->search_bases[state->base_iter]->basedn));

    subreq = sdap_get_generic_send(
            state, state->ev, state->opts, state->sh,
            state->search_bases[state->base_iter]->basedn,
            state->search_bases[state->base_iter]->scope,
            state->filter, state->attrs,
            state->opts->service_map, SDAP_OPTS_SERVICES,
            state->timeout);
    if (!subreq) {
        return ENOMEM;
    }
    tevent_req_set_callback(subreq, sdap_get_services_process, req);

    return EOK;
}

static void
sdap_get_services_process(struct tevent_req *subreq)
{
    struct tevent_req *req =
            tevent_req_callback_data(subreq, struct tevent_req);
    struct sdap_get_services_state *state =
            tevent_req_data(req, struct sdap_get_services_state);
    int ret;
    size_t count, i;
    struct sysdb_attrs **services;
    bool next_base = false;

    ret = sdap_get_generic_recv(subreq, state,
                                &count, &services);
    talloc_zfree(subreq);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    DEBUG(SSSDBG_TRACE_FUNC,
          ("Search for services, returned %d results.\n",
           count));

    if (state->enumeration || count == 0) {
        /* No services found in this search or enumerating */
        next_base = true;
    }

    /* Add this batch of sevices to the list */
    if (count > 0) {
        state->services =
                talloc_realloc(state,
                               state->services,
                               struct sysdb_attrs *,
                               state->count + count + 1);
        if (!state->services) {
            tevent_req_error(req, ENOMEM);
            return;
        }

        /* Copy the new services into the list
         * They're already allocated on 'state'
         */
        for (i = 0; i < count; i++) {
            state->services[state->count + i] = services[i];
        }

        state->count += count;
        state->services[state->count] = NULL;
    }

    if (next_base) {
        state->base_iter++;
        if (state->search_bases[state->base_iter]) {
            /* There are more search bases to try */
            ret = sdap_get_services_next_base(req);
            if (ret != EOK) {
                tevent_req_error(req, ret);
            }
            return;
        }
    }

    /* No more search bases
     * Return ENOENT if no services were found
     */
    if (state->count == 0) {
        tevent_req_error(req, ENOENT);
        return;
    }

    ret = sdap_save_services(state, state->sysdb,
                             state->attrs,
                             state->dom, state->opts,
                             state->services, state->count,
                             &state->higher_usn);
    if (ret) {
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("Failed to store services.\n"));
        tevent_req_error(req, ret);
        return;
    }

    DEBUG(SSSDBG_TRACE_INTERNAL,
          ("Saving %d services - Done\n", state->count));

    tevent_req_done(req);
}

static errno_t
sdap_save_services(TALLOC_CTX *mem_ctx,
                   struct sysdb_ctx *sysdb,
                   const char **attrs,
                   struct sss_domain_info *dom,
                   struct sdap_options *opts,
                   struct sysdb_attrs **services,
                   size_t num_services,
                   char **_usn_value)
{
    errno_t ret, sret;
    time_t now;
    size_t i;
    bool in_transaction;
    char *higher_usn = NULL;
    char *usn_value;
    TALLOC_CTX *tmp_ctx;

    if (num_services == 0) {
        /* Nothing to do */
        return ENOENT;
    }

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        return ENOMEM;
    }

    ret = sysdb_transaction_start(sysdb);
    if (ret != EOK) goto done;

    in_transaction = true;

    now = time(NULL);
    for (i = 0; i < num_services; i++) {
        usn_value = NULL;

        ret = sdap_save_service(tmp_ctx, sysdb, opts, dom,
                                services[i], attrs,
                                &usn_value, now);

        /* Do not fail completely on errors.
         * Just report the failure to save and go on */
        if (ret) {
            DEBUG(SSSDBG_MINOR_FAILURE,
                  ("Failed to store service %d. Ignoring.\n", i));
        } else {
            DEBUG(SSSDBG_TRACE_INTERNAL,
                  ("Service [%lu/%lu] processed!\n", i, num_services));
        }

        if (usn_value) {
            if (higher_usn) {
                if ((strlen(usn_value) > strlen(higher_usn)) ||
                    (strcmp(usn_value, higher_usn) > 0)) {
                    talloc_zfree(higher_usn);
                    higher_usn = usn_value;
                } else {
                    talloc_zfree(usn_value);
                }
            } else {
                higher_usn = usn_value;
            }
        }
    }

    ret = sysdb_transaction_commit(sysdb);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Failed to commit transaction!\n"));
        goto done;
    }
    in_transaction = false;

    if (_usn_value) {
        *_usn_value = talloc_steal(mem_ctx, higher_usn);
    }

done:
    if (in_transaction) {
        sret = sysdb_transaction_cancel(sysdb);
        if (sret != EOK) {
            DEBUG(SSSDBG_CRIT_FAILURE,
                  ("Failed to cancel transaction!\n"));
        }
    }
    talloc_free(tmp_ctx);
    return ret;
}

static errno_t
sdap_save_service(TALLOC_CTX *mem_ctx,
                  struct sysdb_ctx *sysdb,
                  struct sdap_options *opts,
                  struct sss_domain_info *dom,
                  struct sysdb_attrs *attrs,
                  const char **ldap_attrs,
                  char **_usn_value,
                  time_t now)
{
    errno_t ret;
    TALLOC_CTX *tmp_ctx = NULL;
    struct sysdb_attrs *svc_attrs;
    struct ldb_message_element *el;
    char *usn_value = NULL;
    const char *name = NULL;
    const char **aliases;
    const char **protocols;
    char **missing;
    uint16_t port;
    uint64_t cache_timeout;

    DEBUG(SSSDBG_TRACE_ALL, ("Saving service\n"));

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        ret = ENOMEM;
        goto done;
    }

    svc_attrs = sysdb_new_attrs(tmp_ctx);
    if (!svc_attrs) {
        ret = ENOMEM;
        goto done;
    }

    /* Identify the primary name of this services */
    ret = sysdb_attrs_primary_name(
            sysdb, attrs,
            opts->service_map[SDAP_AT_SERVICE_NAME].name,
            &name);
    if (ret != EOK) {
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("Could not determine the primary name of the service\n"));
        goto done;
    }

    DEBUG(SSSDBG_TRACE_INTERNAL, ("Primary name: [%s]\n", name));


    /* Handle any available aliases */
    ret = sysdb_attrs_get_aliases(tmp_ctx, attrs, name,
                                  !dom->case_sensitive,
                                  &aliases);
    if (ret != EOK) {
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("Failed to identify service aliases\n"));
        goto done;
    }

    /* Get the port number */
    ret = sysdb_attrs_get_uint16_t(attrs, SYSDB_SVC_PORT, &port);
    if (ret != EOK) {
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("Failed to identify service port: [%s]\n",
               strerror(ret)));
        goto done;
    }

    /* Get the protocols this service offers on that port */
    ret = sysdb_attrs_get_string_array(attrs, SYSDB_SVC_PROTO,
                                       tmp_ctx, &protocols);
    if (ret != EOK) {
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("Failed to identify service protocols: [%s]\n",
               strerror(ret)));
        goto done;
    }

    /* Get the USN value, if available */
    ret = sysdb_attrs_get_el(attrs,
                      opts->service_map[SDAP_AT_SERVICE_USN].sys_name, &el);
    if (ret && ret != ENOENT) {
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("Failed to retrieve USN value: [%s]\n",
               strerror(ret)));
        goto done;
    }
    if (ret == ENOENT || el->num_values == 0) {
        DEBUG(SSSDBG_TRACE_LIBS,
              ("Original USN value is not available for [%s].\n",
               name));
    } else {
        ret = sysdb_attrs_add_string(svc_attrs,
                          opts->service_map[SDAP_AT_SERVICE_USN].sys_name,
                          (const char*)el->values[0].data);
        if (ret) {
            DEBUG(SSSDBG_MINOR_FAILURE,
                  ("Failed to add USN value: [%s]\n",
                   strerror(ret)));
            goto done;
        }
        usn_value = talloc_strdup(tmp_ctx, (const char*)el->values[0].data);
        if (!usn_value) {
            ret = ENOMEM;
            goto done;
        }
    }

    /* Make sure to remove any extra attributes from the sysdb
     * that have been removed from LDAP
     */
    ret = list_missing_attrs(svc_attrs, opts->service_map, SDAP_OPTS_SERVICES,
                             ldap_attrs, attrs, &missing);
    if (ret != EOK) {
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("Failed to identify removed attributes: [%s]\n",
               strerror(ret)));
        goto done;
    }

    cache_timeout = dp_opt_get_int(opts->basic, SDAP_ENTRY_CACHE_TIMEOUT);

    ret = sysdb_store_service(sysdb, name, port, aliases, protocols,
                              svc_attrs, missing, cache_timeout, now);
    if (ret != EOK) {
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("Failed to store service in the sysdb: [%s]\n",
               strerror(ret)));
        goto done;
    }

    *_usn_value = talloc_steal(mem_ctx, usn_value);

done:
    talloc_free(tmp_ctx);
    return ret;
}

errno_t
sdap_get_services_recv(TALLOC_CTX *mem_ctx,
                       struct tevent_req *req,
                       char **usn_value)
{
    struct sdap_get_services_state *state =
            tevent_req_data(req, struct sdap_get_services_state);

    TEVENT_REQ_RETURN_ON_ERROR(req);

    if (usn_value) {
        *usn_value = talloc_steal(mem_ctx, state->higher_usn);
    }

    return EOK;
}
