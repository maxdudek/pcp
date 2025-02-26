/*
 * Copyright (c) 2019 Red Hat.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */
#include <assert.h>
#include <ctype.h>
#include "openmetrics.h"
#include "server.h"
#include "util.h"

typedef enum pmWebRestKey {
    RESTKEY_NONE	= 0,
    RESTKEY_CONTEXT,
    RESTKEY_METRIC,
    RESTKEY_FETCH,
    RESTKEY_INDOM,
    RESTKEY_PROFILE,
    RESTKEY_STORE,
    RESTKEY_DERIVE,
    RESTKEY_SCRAPE,
} pmWebRestKey;

typedef struct pmWebRestCommand {
    const char		*name;
    unsigned int	size;
    pmWebRestKey	key;
} pmWebRestCommand;

typedef struct pmWebGroupBaton {
    struct client	*client;
    pmWebRestKey	restkey;
    uv_work_t		worker;
    sds			context;
    dict		*params;
    dict		*labels;
    sds			suffix;		/* response trailer (stack) */
    sds			username;	/* from basic auth header */
    sds			password;	/* from basic auth header */
    unsigned int	ctxnum;
    unsigned int	times : 1;
    unsigned int	compat : 1;
    unsigned int	working : 1;
    unsigned int	timeout;
    unsigned int	numpmids;
    unsigned int	numvsets;
    unsigned int	numinsts;
    unsigned int	numindoms;
    pmID		pmid;		/* metric currently being processed */
    pmInDom		indom;		/* indom currently being processed */
} pmWebGroupBaton;

static pmWebRestCommand commands[] = {
    { .key = RESTKEY_CONTEXT, .name = "context", .size = sizeof("context")-1 },
    { .key = RESTKEY_PROFILE, .name = "profile", .size = sizeof("profile")-1 },
    { .key = RESTKEY_SCRAPE, .name = "metrics", .size = sizeof("metrics")-1 },
    { .key = RESTKEY_METRIC, .name = "metric", .size = sizeof("metric")-1 },
    { .key = RESTKEY_DERIVE, .name = "derive", .size = sizeof("derive")-1 },
    { .key = RESTKEY_FETCH, .name = "fetch", .size = sizeof("fetch")-1 },
    { .key = RESTKEY_INDOM, .name = "indom", .size = sizeof("indom")-1 },
    { .key = RESTKEY_STORE, .name = "store", .size = sizeof("store")-1 },
    { .key = RESTKEY_NONE }
};

static pmWebRestCommand openmetrics[] = {
    { .key = RESTKEY_SCRAPE, .name = "/metrics", .size = sizeof("/metrics")-1 },
    { .key = RESTKEY_NONE }
};

static sds PARAM_NAMES, PARAM_NAME, PARAM_PMIDS, PARAM_PMID,
	   PARAM_INDOM, PARAM_EXPR, PARAM_VALUE, PARAM_TIMES,
	   PARAM_CONTEXT;


static pmWebRestKey
pmwebapi_lookup_restkey(sds url, unsigned int *compat, sds *context)
{
    pmWebRestCommand	*cp;
    const char		*name, *ctxid = NULL;

    if (sdslen(url) >= (sizeof("/pmapi/") - 1) &&
	strncmp(url, "/pmapi/", sizeof("/pmapi/") - 1) == 0) {
	name = (const char *)url + sizeof("/pmapi/") - 1;
	/* extract (optional) context identifier */
	if (isdigit(*name)) {
	    ctxid = name;
	    do {
		name++;
	    } while (isdigit(*name));
	    if (*name++ != '/')
		return RESTKEY_NONE;
	    *context = sdsnewlen(ctxid, name - ctxid - 1);
	}
	if (*name == '_') {
	    name++;		/* skip underscore designating */
	    *compat = 1;	/* backward-compatibility mode */
	}
	for (cp = &commands[0]; cp->name; cp++)
	    if (strncmp(cp->name, name, cp->size) == 0)
		return cp->key;
    }
    for (cp = &openmetrics[0]; cp->name; cp++)
	if (strncmp(cp->name, url, cp->size) == 0)
	    return cp->key;
    return RESTKEY_NONE;
}

static void
pmwebapi_free_baton(pmWebGroupBaton *baton)
{
    sdsfree(baton->suffix);
    sdsfree(baton->context);
    /* baton->params freed in http.c */
    if (baton->labels)
	dictRelease(baton->labels);
    memset(baton, 0, sizeof(*baton));
    free(baton);
}

static void
pmwebapi_set_context(pmWebGroupBaton *baton, sds context)
{
    if (baton->context == NULL) {
	baton->context = sdsdup(context);
    } else if (sdscmp(baton->context, context) != 0) {
	sdsfree(baton->context);
	baton->context = sdsdup(context);
    }
}

static void
on_pmwebapi_context(sds context, pmWebSource *source, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    struct client	*client = baton->client;
    sds			result;

    pmwebapi_set_context(baton, context);

    result = http_get_buffer(client);
    result = sdscatfmt(result, "{\"context\":%S", context);
    baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_OBJECT);
    if (baton->compat == 0) {
	result = sdscatfmt(result,
			",\"source\":\"%S\",\"hostspec\":\"%S\",\"labels\":",
			source->source, source->hostspec);
	if (source->labels)
	    result = sdscatsds(result, source->labels);
	else
	    result = sdscatlen(result, "{}", 2);
    }
    http_set_buffer(client, result, HTTP_FLAG_JSON);
    http_transfer(client);
}

static void
on_pmwebapi_metric(sds context, pmWebMetric *metric, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    struct client	*client = baton->client;
    char		pmidstr[20], indomstr[20];
    sds			quoted, result = http_get_buffer(client);
    int			first = (baton->numpmids == 0);

    pmwebapi_set_context(baton, context);

    if (first) {
	baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_OBJECT);
	baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_ARRAY);
	result = sdscatfmt(result, "{\"context\":%S,\"metrics\":[", context);
    } else { /* next metric */
	result = sdscatlen(result, ",", 1);
    }

    baton->numpmids++;

    result = sdscatfmt(result, "{\"name\":\"%S\",\"series\":\"%S\"",
			metric->name, metric->series);
    if (baton->compat == 0) {
	pmIDStr_r(metric->pmid, pmidstr, sizeof(pmidstr));
	pmInDomStr_r(metric->indom, indomstr, sizeof(indomstr));
	result = sdscatfmt(result, ",\"pmID\":\"%s\",\"indom\":\"%s\"",
				pmidstr, indomstr);
    } else {
	result = sdscatfmt(result, ",\"pmID\":%u,\"indom\":%u",
				metric->pmid, metric->indom);
    }
    result = sdscatfmt(result,
		",\"type\":\"%s\",\"sem\":\"%s\",\"units\":\"%s\",\"labels\":",
			metric->type, metric->sem, metric->units);
    if (metric->labels)
	result = sdscatsds(result, metric->labels);
    else
	result = sdscatlen(result, "{}", 2);
    if (metric->oneline &&
		(quoted = sdscatrepr(sdsempty(), metric->oneline,
			sdslen(metric->oneline))) != NULL) {
	result = sdscatfmt(result, ",\"text-oneline\":%S", quoted);
	sdsfree(quoted);
    }
    if (metric->helptext &&
		(quoted = sdscatrepr(sdsempty(), metric->helptext,
			sdslen(metric->helptext))) != NULL) {
	result = sdscatfmt(result, ",\"text-helptext\":%S", quoted);
	sdsfree(quoted);
    }
    result = sdscatlen(result, "}", 1);

    http_set_buffer(client, result, HTTP_FLAG_JSON);
    http_transfer(client);
}

static int
on_pmwebapi_fetch(sds context, pmWebResult *fetch, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    sds			result = http_get_buffer(baton->client);

    pmwebapi_set_context(baton, context);

    baton->numvsets = baton->numinsts = 0;
    if (baton->compat == 0)
	result = sdscatfmt(result,
		"{\"context\":%S,\"timestamp\":{\"sec\":%I,\"nsec\":%I},",
			context, fetch->seconds, fetch->nanoseconds);
    else
	result = sdscatfmt(result, "{\"timestamp\":{\"s\":%I,\"us\":%I},",
			fetch->seconds, fetch->nanoseconds / 1000);
    baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_OBJECT);
    result = sdscatfmt(result, "\"values\":[");
    baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_ARRAY);

    http_set_buffer(baton->client, result, HTTP_FLAG_JSON);
    http_transfer(baton->client);
    return 0;
}

static int
on_pmwebapi_fetch_values(sds context, pmWebValueSet *valueset, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    sds			result = http_get_buffer(baton->client);
    char		pmidstr[20];

    pmwebapi_set_context(baton, context);

    if (valueset->pmid != baton->pmid) {	/* new metric */
	if (baton->numvsets > 0) {
	    result = sdscatlen(result, "]},", 3);
	    baton->suffix = json_pop_suffix(baton->suffix);	/* '[' */
	    baton->suffix = json_pop_suffix(baton->suffix); /* '{' */
	}
	baton->pmid = valueset->pmid;
	baton->numvsets = 0;
	baton->numinsts = 0;
    } else if (baton->numvsets != 0) {
	result = sdscatlen(result, ",", 1);
    }
    baton->numvsets++;

    if (baton->compat == 0) {
	pmIDStr_r(valueset->pmid, pmidstr, sizeof(pmidstr));
	result = sdscatfmt(result, "{\"pmid\":\"%s\"", pmidstr);
    } else {
	result = sdscatfmt(result, "{\"pmid\":%u", valueset->pmid);
    }
    baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_OBJECT);
    result = sdscatfmt(result, ",\"name\":\"%S\",\"instances\":[",
				valueset->name);
    baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_ARRAY);

    http_set_buffer(baton->client, result, HTTP_FLAG_JSON);
    http_transfer(baton->client);
    return 0;
}

static int
on_pmwebapi_fetch_value(sds context, pmWebValue *value, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    sds			result = http_get_buffer(baton->client);

    assert(value->pmid == baton->pmid);
    pmwebapi_set_context(baton, context);

    if (baton->numinsts != 0)
	result = sdscatlen(result, ",", 1);
    baton->numinsts++;

    if (baton->compat == 0 && value->inst == PM_IN_NULL) {
	result = sdscatfmt(result, "{\"instance\":null,\"value\":%S}",
			    value->value);
    } else {
	result = sdscatfmt(result, "{\"instance\":%u,\"value\":%S}",
			    value->inst, value->value);
    }

    http_set_buffer(baton->client, result, HTTP_FLAG_JSON);
    http_transfer(baton->client);
    return 0;
}

static int
on_pmwebapi_indom(sds context, pmWebInDom *indom, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    char		indomstr[20];
    sds			quoted, result = http_get_buffer(baton->client);

    pmwebapi_set_context(baton, context);

    if (indom->indom != baton->indom) {	/* new indom */
	baton->indom = indom->indom;
	baton->numindoms = 0;
	baton->numinsts = 0;
    }

    if (baton->numindoms != 0)
	result = sdscatlen(result, ",", 1);
    baton->numindoms++;

    baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_OBJECT);
    if (baton->compat == 0) {
	pmInDomStr_r(indom->indom, indomstr, sizeof(indomstr));
	result = sdscatfmt(result, "{\"context\":%S,\"indom\":\"%s\"",
			context, indomstr);
    } else {
	result = sdscatfmt(result, "{\"indom\":%u", indom->indom);
    }
    result = sdscatfmt(result, ",\"labels\":");
    if (indom->labels)
	result = sdscatsds(result, indom->labels);
    else
	result = sdscatlen(result, "{}", 2);
    if (indom->oneline &&
		(quoted = sdscatrepr(sdsempty(),
			indom->oneline, sdslen(indom->oneline)))) {
	result = sdscatfmt(result, ",\"text-oneline\":%S", quoted);
	sdsfree(quoted);
    }
    if (indom->helptext &&
		(quoted = sdscatrepr(sdsempty(),
			indom->helptext, sdslen(indom->helptext)))) {
	result = sdscatfmt(result, ",\"text-helptext\":%S", quoted);
	sdsfree(quoted);
    }
    result = sdscatfmt(result, ",\"instances\":[");
    baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_ARRAY);

    http_set_buffer(baton->client, result, HTTP_FLAG_JSON);
    http_transfer(baton->client);
    return 0;
}

static int
on_pmwebapi_instance(sds context, pmWebInstance *instance, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    sds			quoted, result = http_get_buffer(baton->client);

    assert(instance->indom == baton->indom);
    pmwebapi_set_context(baton, context);

    if (baton->numinsts != 0)
	result = sdscatlen(result, ",", 1);
    baton->numinsts++;

    quoted = sdscatrepr(sdsempty(), instance->name, sdslen(instance->name));
    result = sdscatfmt(result, "{\"instance\":%u,\"name\":%S,\"labels\":",
			instance->inst, quoted);
    sdsfree(quoted);
    if (instance->labels)
	result = sdscatsds(result, instance->labels);
    else
	result = sdscatlen(result, "{}", 2);
    result = sdscatlen(result, "}", 1);

    http_set_buffer(baton->client, result, HTTP_FLAG_JSON);
    http_transfer(baton->client);
    return 0;
}

/*
 * https://openmetrics.io/
 * https://github.com/prometheus/docs/blob/master/content/docs/instrumenting/exposition_formats.md
 *
 * metric_name [
 *    "{" label_name "=" `"` label_value `"` { "," label_name "=" `"` label_value `"` } [ "," ] "}"
 * ] value [ timestamp ]
 */
static int
on_pmwebapi_scrape(sds context, pmWebScrape *scrape, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    pmWebInstance	*instance = &scrape->instance;
    pmWebMetric		*metric = &scrape->metric;
    pmWebValue		*value = &scrape->value;
    long long		milliseconds;
    char		pmidstr[20], indomstr[20];
    sds			name, semantics, result, quoted = NULL, labels = NULL;

    pmwebapi_set_context(baton, context);
    if (open_metrics_type_check(metric->type) < 0)
	return 0;
    semantics = open_metrics_semantics(metric->sem);
    result = http_get_buffer(baton->client);
    name = open_metrics_name(metric->name);

    if (baton->compat == 0) {	/* include pmid, indom and type */
	pmIDStr_r(metric->pmid, pmidstr, sizeof(pmidstr));
	pmInDomStr_r(metric->indom, indomstr, sizeof(indomstr));
	result = sdscatfmt(result, "# PCP %S %s %S %s %S %S\n",
			metric->name, pmidstr, metric->type,
			indomstr, metric->sem, metric->units);
    } else {
	result = sdscatfmt(result, "# PCP %S %S %S\n",
			metric->name, metric->sem, metric->units);
    }

    if (metric->oneline)
	result = sdscatfmt(result, "# HELP %S %S\n", name, metric->oneline);
    result = sdscatfmt(result, "# TYPE %S %S\n%S", name, semantics, name);

    if (metric->indom != PM_INDOM_NULL)
	labels = instance->labels;
    if (labels == NULL)
	labels = metric->labels;

    if (metric->indom != PM_INDOM_NULL || labels) {
	if (metric->indom != PM_INDOM_NULL) {
	    quoted = sdsempty();
	    quoted = sdscatrepr(quoted, instance->name, sdslen(instance->name));
	    if (baton->compat == 0)
		result = sdscatfmt(result,
				"{instance.name=%S,instance.id=\"%u\"",
				quoted, instance->inst);
	    else
		result = sdscatfmt(result, "{instance=%S", quoted);
	    sdsfree(quoted);
	    if (labels)
		result = sdscatfmt(result, ",%S} %S", labels, value->value);
	    else
		result = sdscatfmt(result, "} %S", value->value);
	} else {
	    result = sdscatfmt(result, "{%S} %S", labels, value->value);
	}
    } else {
	result = sdscatfmt(result, " %S", value->value);
    }

    if (baton->times) {
	milliseconds = (scrape->seconds * 1000) + (scrape->nanoseconds / 1000);
	result = sdscatfmt(result, " %I\n", milliseconds);
    } else {
	result = sdscatfmt(result, "\n");
    }

    sdsfree(semantics);
    sdsfree(name);

    http_set_buffer(baton->client, result, HTTP_FLAG_TEXT);
    http_transfer(baton->client);
    return 0;
}

/*
 * Given an array of labelset pointers produce Open Metrics format labels.
 * The labelset structure provided contains a pre-allocated result buffer.
 *
 * This function is comparable to pmMergeLabelSets(3), however it produces
 * labels in Open Metrics form instead of the native PCP JSONB style.
 */
static void
on_pmwebapi_scrape_labels(sds context, pmWebLabelSet *labelset, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    struct client	*client = (struct client *)baton->client;

    if (pmDebugOptions.labels || pmDebugOptions.series) {
	fprintf(stderr, "%s: client=%p (ctx=%s)\n",
		"on_pmwebapi_scrape_labels", client, context);
    }
    if (baton->labels == NULL)
	baton->labels = dictCreate(&sdsDictCallBacks, NULL);
    open_metrics_labels(labelset, baton->labels);
    dictEmpty(baton->labels, NULL);	/* reset for next caller */
}

static int
on_pmwebapi_check(sds context, pmWebAccess *access,
		int *status, sds *message, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    struct client	*client = (struct client *)baton->client;

    if (pmDebugOptions.auth || pmDebugOptions.series)
	fprintf(stderr, "%s: client=%p (ctx=%s) user=%s pass=%s realm=%s\n",
		"on_pmwebapi_check", client, context,
		access->username, access->password, access->realm);

    /* Does this context require username/password authentication? */
    if (access->username != NULL ||
		__pmServerHasFeature(PM_SERVER_FEATURE_CREDS_REQD)) {
	if (access->username == NULL || access->password == NULL) {
	    *message = sdsnew("authentication required");
	    *status = -EAGAIN;
	    return 1;
	}
	if ((access->username != NULL &&
		sdscmp(access->username, client->u.http.username) != 0) ||
	    (access->password != NULL &&
		(sdscmp(access->password, client->u.http.password) != 0))) {
	    *message = sdsnew("authentication failed");
	    *status = -EPERM;
	    return 1;
	}
    }

    return 0;
}

static void
on_pmwebapi_done(sds context, int status, sds message, void *arg)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)arg;
    struct client	*client = (struct client *)baton->client;
    sds			quoted, msg;
    http_flags		flags = client->u.http.flags;
    http_code		code;

    if (pmDebugOptions.series)
	fprintf(stderr, "%s: client=%p (sts=%d,msg=%s)\n", "on_pmwebapi_done",
			client, status, message ? message : "");

    if (status == 0) {
	code = HTTP_STATUS_OK;
	/* complete current response with JSON suffix if needed */
	if ((msg = baton->suffix) == NULL) {	/* empty OK response */
	    if (flags & HTTP_FLAG_JSON)
		msg = sdsnew("{\"success\":true}\r\n");
	    else
		msg = sdsempty();
	}
	baton->suffix = NULL;
    } else {
	flags |= HTTP_FLAG_JSON;
	if (((code = client->u.http.parser.status_code)) == 0) {
	    if (status == -EPERM)
		code = HTTP_STATUS_FORBIDDEN;
	    else if (status == -EAGAIN)
		code = HTTP_STATUS_UNAUTHORIZED;
	    else
		code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
	}
	if (message)
	    quoted = sdscatrepr(sdsempty(), message, sdslen(message));
	else
	    quoted = sdsnew("\"(none)\"");
	msg = sdsempty();
	msg = sdscatfmt(msg, "{\"success\":false,\"message\":%S}\r\n", quoted);
	sdsfree(quoted);
    }
    http_reply(client, msg, code, flags);
    /* baton freed on uv_work_t completion callback, not here */
}

static pmWebGroupSettings pmwebapi_settings = {
    .callbacks.on_context	= on_pmwebapi_context,
    .callbacks.on_metric	= on_pmwebapi_metric,
    .callbacks.on_fetch		= on_pmwebapi_fetch,
    .callbacks.on_fetch_values	= on_pmwebapi_fetch_values,
    .callbacks.on_fetch_value	= on_pmwebapi_fetch_value,
    .callbacks.on_indom		= on_pmwebapi_indom,
    .callbacks.on_instance	= on_pmwebapi_instance,
    .callbacks.on_scrape	= on_pmwebapi_scrape,
    .callbacks.on_scrape_labels	= on_pmwebapi_scrape_labels,
    .callbacks.on_check		= on_pmwebapi_check,
    .callbacks.on_done		= on_pmwebapi_done,
    .module.on_info		= proxylog,
};

/*
 * Finish processing of individual request parameters, preparing
 * for (later) submission of the requested PMWEBAPI(3) command.
 */
static void
pmwebapi_setup_request_parameters(struct client *client,
		pmWebGroupBaton *baton, dict *parameters)
{
    dictEntry	*entry;

    if ((parameters != NULL) && (baton->context == NULL) &&
	(entry = dictFind(parameters, PARAM_CONTEXT)) != NULL) {
	pmwebapi_set_context(baton, dictGetVal(entry));
    }

    switch (baton->restkey) {
    case RESTKEY_CONTEXT:
    case RESTKEY_METRIC:
	/* no mandatory parameters to check */
	break;

    case RESTKEY_SCRAPE:
	if (parameters && (entry = dictFind(parameters, PARAM_TIMES)))
	     baton->times = (strcmp(dictGetVal(entry), "true") == 0);
	break;

    case RESTKEY_FETCH:
	if (parameters == NULL ||
	    (dictFind(parameters, PARAM_NAME) == NULL &&
	     dictFind(parameters, PARAM_NAMES) == NULL &&
	     dictFind(parameters, PARAM_PMID) == NULL &&
	     dictFind(parameters, PARAM_PMIDS) == NULL))
	    client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	break;

    case RESTKEY_INDOM:
	if (parameters == NULL ||
	    (dictFind(parameters, PARAM_INDOM) == NULL &&
	     dictFind(parameters, PARAM_NAME) == NULL))
	    client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	break;

    case RESTKEY_PROFILE:
	if (parameters == NULL ||
	    dictFind(parameters, PARAM_INDOM) == NULL)
	    client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	break;

    case RESTKEY_STORE:
	if (parameters == NULL)
	    client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	else if (dictFind(parameters, PARAM_NAME) == NULL &&
	     dictFind(parameters, PARAM_PMID) == NULL)
	    client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	else if (dictFind(parameters, PARAM_VALUE) == NULL)
	    client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	break;

    case RESTKEY_DERIVE:
	/*
	 * Expect metric name and an expression string OR configuration
	 * via a POST (see pmwebapi_request_body() for further detail).
	 */
	if (parameters == NULL && 
	    (client->u.http.parser.method != HTTP_POST))
	    client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	else if (client->u.http.parser.method != HTTP_POST &&
	    dictFind(parameters, PARAM_NAME) == NULL &&
	    dictFind(parameters, PARAM_EXPR) == NULL)
	    client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	break;

    case RESTKEY_NONE:
    default:
	client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	break;
    }
}

/*
 * Test if this is a pmwebapi REST request, and if so, which one.
 * If this servlet is handling this URL, ensure space for state exists
 * and indicate acceptance for processing this URL via the return code.
 */
static int
pmwebapi_request_url(struct client *client, sds url, dict *parameters)
{
    pmWebGroupBaton	*baton;
    pmWebRestKey	key;
    unsigned int	compat = 0;
    sds			context = NULL;

    if ((key = pmwebapi_lookup_restkey(url, &compat, &context)) == RESTKEY_NONE)
	return 0;

    if ((baton = client->u.http.data) == NULL) {
	if ((baton = calloc(1, sizeof(*baton))) == NULL)
	    client->u.http.parser.status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
	client->u.http.data = baton;
    }
    if (baton && !baton->working) {
	baton->restkey = key;
	baton->compat = compat;
	baton->client = client;
	baton->params = client->u.http.parameters;
	baton->context = context;
	pmwebapi_setup_request_parameters(client, baton, parameters);
    } else if (baton && baton->working) {
	client->u.http.parser.status_code = HTTP_STATUS_CONFLICT;
    } else {
	client->u.http.parser.status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    }
    return 1;
}

static int
pmwebapi_request_body(struct client *client, const char *content, size_t length)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)client->u.http.data;

    if (baton->restkey == RESTKEY_DERIVE &&
	client->u.http.parser.method == HTTP_POST) {
	if (baton->params == NULL) {
	    baton->params = dictCreate(&sdsDictCallBacks, NULL);
	    client->u.http.parameters = baton->params;
	}
	dictAdd(baton->params, sdsnew(PARAM_EXPR), sdsnewlen(content, length));
    }
    return 0;
}

static void
pmwebapi_fetch(uv_work_t *work)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)work->data;

    pmWebGroupFetch(&pmwebapi_settings, baton->context, baton->params, baton);
}

static void
pmwebapi_indom(uv_work_t *work)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)work->data;

    pmWebGroupInDom(&pmwebapi_settings, baton->context, baton->params, baton);
}

static void
pmwebapi_metric(uv_work_t *work)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)work->data;

    pmWebGroupMetric(&pmwebapi_settings, baton->context, baton->params, baton);
}

static void
pmwebapi_store(uv_work_t *work)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)work->data;

    pmWebGroupStore(&pmwebapi_settings, baton->context, baton->params, baton);
}

static void
pmwebapi_derive(uv_work_t *work)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)work->data;

    pmWebGroupDerive(&pmwebapi_settings, baton->context, baton->params, baton);
}

static void
pmwebapi_profile(uv_work_t *work)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)work->data;

    pmWebGroupProfile(&pmwebapi_settings, baton->context, baton->params, baton);
}

static void
pmwebapi_scrape(uv_work_t *work)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)work->data;
    
    pmWebGroupScrape(&pmwebapi_settings, baton->context, baton->params, baton);
}

static void
pmwebapi_context(uv_work_t *work)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)work->data;

    pmWebGroupContext(&pmwebapi_settings, baton->context, baton->params, baton);
}

static void
pmwebapi_done(uv_work_t *work, int status)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)work->data;

    if (pmDebugOptions.series)
	fprintf(stderr, "%s: client=%p (sts=%d)\n", "pmwebapi_done",
			baton->client, status);

    baton->working = 0;
    baton->worker.data = NULL;
    pmwebapi_free_baton(baton);
}

static int
pmwebapi_request_done(struct client *client)
{
    pmWebGroupBaton	*baton = (pmWebGroupBaton *)client->u.http.data;
    uv_loop_t		*loop = client->proxy->events;

    /* submit command request to worker thread */
    baton->working = 1;
    baton->worker.data = baton;
    switch (baton->restkey) {
    case RESTKEY_CONTEXT:
	uv_queue_work(loop, &baton->worker, pmwebapi_context, pmwebapi_done);
	break;
    case RESTKEY_PROFILE:
	uv_queue_work(loop, &baton->worker, pmwebapi_profile, pmwebapi_done);
	break;
    case RESTKEY_METRIC:
	uv_queue_work(loop, &baton->worker, pmwebapi_metric, pmwebapi_done);
	break;
    case RESTKEY_FETCH:
	uv_queue_work(loop, &baton->worker, pmwebapi_fetch, pmwebapi_done);
	break;
    case RESTKEY_INDOM:
	uv_queue_work(loop, &baton->worker, pmwebapi_indom, pmwebapi_done);
	break;
    case RESTKEY_STORE:
	uv_queue_work(loop, &baton->worker, pmwebapi_store, pmwebapi_done);
	break;
    case RESTKEY_DERIVE:
	uv_queue_work(loop, &baton->worker, pmwebapi_derive, pmwebapi_done);
	break;
    case RESTKEY_SCRAPE:
	uv_queue_work(loop, &baton->worker, pmwebapi_scrape, pmwebapi_done);
	break;
    case RESTKEY_NONE:
    default:
	baton->working = 0;
	return 1;
    }
    return 0;
}

static void
pmwebapi_servlet_setup(struct proxy *proxy)
{
    PARAM_NAMES = sdsnew("names");
    PARAM_NAME = sdsnew("name");
    PARAM_PMIDS = sdsnew("pmids");
    PARAM_PMID = sdsnew("pmid");
    PARAM_INDOM = sdsnew("indom");
    PARAM_EXPR = sdsnew("expr");
    PARAM_VALUE = sdsnew("value");
    PARAM_TIMES = sdsnew("times");
    PARAM_CONTEXT = sdsnew("context");

    pmWebGroupSetup(&pmwebapi_settings.module);
    pmWebGroupSetEventLoop(&pmwebapi_settings.module, proxy->events);
    pmWebGroupSetConfiguration(&pmwebapi_settings.module, proxy->config);
    pmWebGroupSetMetricRegistry(&pmwebapi_settings.module, proxy->metrics);
}

static void
pmwebapi_servlet_close(void)
{
    sdsfree(PARAM_NAMES);
    sdsfree(PARAM_NAME);
    sdsfree(PARAM_PMIDS);
    sdsfree(PARAM_PMID);
    sdsfree(PARAM_INDOM);
    sdsfree(PARAM_EXPR);
    sdsfree(PARAM_VALUE);
    sdsfree(PARAM_TIMES);
    sdsfree(PARAM_CONTEXT);
}

struct servlet pmwebapi_servlet = {
    .name		= "webapi",
    .setup		= pmwebapi_servlet_setup,
    .close		= pmwebapi_servlet_close,
    .on_url		= pmwebapi_request_url,
    .on_body		= pmwebapi_request_body,
    .on_done		= pmwebapi_request_done,
};
