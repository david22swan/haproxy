/*
 * HTTP actions
 *
 * Copyright 2000-2018 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <sys/types.h>

#include <ctype.h>
#include <string.h>
#include <time.h>

#include <haproxy/action.h>
#include <haproxy/base.h>
#include <common/cfgparse.h>
#include <haproxy/chunk.h>
#include <haproxy/http.h>
#include <haproxy/http_htx.h>
#include <haproxy/http_rules.h>
#include <haproxy/pattern.h>
#include <haproxy/pool.h>
#include <haproxy/regex.h>
#include <haproxy/sample.h>
#include <haproxy/tools.h>
#include <common/uri_auth.h>
#include <haproxy/version.h>

#include <haproxy/capture-t.h>
#include <types/global.h>

#include <proto/acl.h>
#include <haproxy/arg.h>
#include <proto/log.h>
#include <proto/http_ana.h>
#include <proto/stream_interface.h>

/* Release memory allocated by most of HTTP actions. Concretly, it releases
 * <arg.http>.
 */
static void release_http_action(struct act_rule *rule)
{
	struct logformat_node *lf, *lfb;

	istfree(&rule->arg.http.str);
	if (rule->arg.http.re)
		regex_free(rule->arg.http.re);
	list_for_each_entry_safe(lf, lfb, &rule->arg.http.fmt, list) {
		LIST_DEL(&lf->list);
		release_sample_expr(lf->expr);
		free(lf->arg);
		free(lf);
	}
}

/* Release memory allocated by HTTP actions relying on an http reply. Concretly,
 * it releases <.arg.http_reply>
 */
static void release_act_http_reply(struct act_rule *rule)
{
	release_http_reply(rule->arg.http_reply);
	rule->arg.http_reply = NULL;
}


/* Check function for HTTP actions relying on an http reply. The function
 * returns 1 in success case, otherwise, it returns 0 and err is filled.
 */
static int check_act_http_reply(struct act_rule *rule, struct proxy *px, char **err)
{
	struct http_reply *reply = rule->arg.http_reply;

	if (!http_check_http_reply(reply, px, err)) {
		release_act_http_reply(rule);
		return 0;
	}
	return 1;
}


/* This function executes one of the set-{method,path,query,uri} actions. It
 * builds a string in the trash from the specified format string. It finds
 * the action to be performed in <.action>, previously filled by function
 * parse_set_req_line(). The replacement action is executed by the function
 * http_action_set_req_line(). On success, it returns ACT_RET_CONT. If an error
 * occurs while soft rewrites are enabled, the action is canceled, but the rule
 * processing continue. Otherwsize ACT_RET_ERR is returned.
 */
static enum act_return http_action_set_req_line(struct act_rule *rule, struct proxy *px,
                                                struct session *sess, struct stream *s, int flags)
{
	struct buffer *replace;
	enum act_return ret = ACT_RET_CONT;

	replace = alloc_trash_chunk();
	if (!replace)
		goto fail_alloc;

	/* If we have to create a query string, prepare a '?'. */
	if (rule->action == 2) // set-query
		replace->area[replace->data++] = '?';
	replace->data += build_logline(s, replace->area + replace->data,
				       replace->size - replace->data,
				       &rule->arg.http.fmt);

	if (http_req_replace_stline(rule->action, replace->area, replace->data, px, s) == -1)
		goto fail_rewrite;

  leave:
	free_trash_chunk(replace);
	return ret;

  fail_alloc:
	if (!(s->flags & SF_ERR_MASK))
		s->flags |= SF_ERR_RESOURCE;
	ret = ACT_RET_ERR;
	goto leave;

  fail_rewrite:
	_HA_ATOMIC_ADD(&sess->fe->fe_counters.failed_rewrites, 1);
	if (s->flags & SF_BE_ASSIGNED)
		_HA_ATOMIC_ADD(&s->be->be_counters.failed_rewrites, 1);
	if (sess->listener->counters)
		_HA_ATOMIC_ADD(&sess->listener->counters->failed_rewrites, 1);
	if (objt_server(s->target))
		_HA_ATOMIC_ADD(&__objt_server(s->target)->counters.failed_rewrites, 1);

	if (!(s->txn->req.flags & HTTP_MSGF_SOFT_RW)) {
		ret = ACT_RET_ERR;
		if (!(s->flags & SF_ERR_MASK))
			s->flags |= SF_ERR_PRXCOND;
	}
	goto leave;
}

/* parse an http-request action among :
 *   set-method
 *   set-path
 *   set-query
 *   set-uri
 *
 * All of them accept a single argument of type string representing a log-format.
 * The resulting rule makes use of <http.fmt> to store the log-format list head,
 * and <.action> to store the action type as an int (0=method, 1=path, 2=query,
 * 3=uri). It returns ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_set_req_line(const char **args, int *orig_arg, struct proxy *px,
                                             struct act_rule *rule, char **err)
{
	int cur_arg = *orig_arg;

	switch (args[0][4]) {
	case 'm' :
		rule->action = 0; // set-method
		break;
	case 'p' :
		rule->action = 1; // set-path
		break;
	case 'q' :
		rule->action = 2; // set-query
		break;
	case 'u' :
		rule->action = 3; // set-uri
		break;
	default:
		memprintf(err, "internal error: unhandled action '%s'", args[0]);
		return ACT_RET_PRS_ERR;
	}
	rule->action_ptr = http_action_set_req_line;
	rule->release_ptr = release_http_action;

	if (!*args[cur_arg] ||
	    (*args[cur_arg + 1] && strcmp(args[cur_arg + 1], "if") != 0 && strcmp(args[cur_arg + 1], "unless") != 0)) {
		memprintf(err, "expects exactly 1 argument <format>");
		return ACT_RET_PRS_ERR;
	}

	LIST_INIT(&rule->arg.http.fmt);
	px->conf.args.ctx = ARGC_HRQ;
	if (!parse_logformat_string(args[cur_arg], px, &rule->arg.http.fmt, LOG_OPT_HTTP,
	                            (px->cap & PR_CAP_FE) ? SMP_VAL_FE_HRQ_HDR : SMP_VAL_BE_HRQ_HDR, err)) {
		return ACT_RET_PRS_ERR;
	}

	(*orig_arg)++;
	return ACT_RET_PRS_OK;
}

/* This function executes a replace-uri action. It finds its arguments in
 * <rule>.arg.http. It builds a string in the trash from the format string
 * previously filled by function parse_replace_uri() and will execute the regex
 * in <http.re> to replace the URI. It uses the format string present in
 * <http.fmt>. The component to act on (path/uri) is taken from <.action> which
 * contains 1 for the path or 3 for the URI (values used by
 * http_req_replace_stline()). On success, it returns ACT_RET_CONT. If an error
 * occurs while soft rewrites are enabled, the action is canceled, but the rule
 * processing continue. Otherwsize ACT_RET_ERR is returned.
 */
static enum act_return http_action_replace_uri(struct act_rule *rule, struct proxy *px,
                                               struct session *sess, struct stream *s, int flags)
{
	enum act_return ret = ACT_RET_CONT;
	struct buffer *replace, *output;
	struct ist uri;
	int len;

	replace = alloc_trash_chunk();
	output  = alloc_trash_chunk();
	if (!replace || !output)
		goto fail_alloc;
	uri = htx_sl_req_uri(http_get_stline(htxbuf(&s->req.buf)));

	if (rule->action == 1) // replace-path
		uri = iststop(http_get_path(uri), '?');

	if (!regex_exec_match2(rule->arg.http.re, uri.ptr, uri.len, MAX_MATCH, pmatch, 0))
		goto leave;

	replace->data = build_logline(s, replace->area, replace->size, &rule->arg.http.fmt);

	/* note: uri.ptr doesn't need to be zero-terminated because it will
	 * only be used to pick pmatch references.
	 */
	len = exp_replace(output->area, output->size, uri.ptr, replace->area, pmatch);
	if (len == -1)
		goto fail_rewrite;

	if (http_req_replace_stline(rule->action, output->area, len, px, s) == -1)
		goto fail_rewrite;

  leave:
	free_trash_chunk(output);
	free_trash_chunk(replace);
	return ret;

  fail_alloc:
	if (!(s->flags & SF_ERR_MASK))
		s->flags |= SF_ERR_RESOURCE;
	ret = ACT_RET_ERR;
	goto leave;

  fail_rewrite:
	_HA_ATOMIC_ADD(&sess->fe->fe_counters.failed_rewrites, 1);
	if (s->flags & SF_BE_ASSIGNED)
		_HA_ATOMIC_ADD(&s->be->be_counters.failed_rewrites, 1);
	if (sess->listener->counters)
		_HA_ATOMIC_ADD(&sess->listener->counters->failed_rewrites, 1);
	if (objt_server(s->target))
		_HA_ATOMIC_ADD(&__objt_server(s->target)->counters.failed_rewrites, 1);

	if (!(s->txn->req.flags & HTTP_MSGF_SOFT_RW)) {
		ret = ACT_RET_ERR;
		if (!(s->flags & SF_ERR_MASK))
			s->flags |= SF_ERR_PRXCOND;
	}
	goto leave;
}

/* parse a "replace-uri" or "replace-path" http-request action.
 * This action takes 2 arguments (a regex and a replacement format string).
 * The resulting rule makes use of <.action> to store the action (1/3 for now),
 * <http.re> to store the compiled regex, and <http.fmt> to store the log-format
 * list head. It returns ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_replace_uri(const char **args, int *orig_arg, struct proxy *px,
                                            struct act_rule *rule, char **err)
{
	int cur_arg = *orig_arg;
	char *error = NULL;

	if (strcmp(args[cur_arg-1], "replace-path") == 0)
		rule->action = 1; // replace-path, same as set-path
	else
		rule->action = 3; // replace-uri, same as set-uri

	rule->action_ptr = http_action_replace_uri;
	rule->release_ptr = release_http_action;

	if (!*args[cur_arg] || !*args[cur_arg+1] ||
	    (*args[cur_arg+2] && strcmp(args[cur_arg+2], "if") != 0 && strcmp(args[cur_arg+2], "unless") != 0)) {
		memprintf(err, "expects exactly 2 arguments <match-regex> and <replace-format>");
		return ACT_RET_PRS_ERR;
	}

	if (!(rule->arg.http.re = regex_comp(args[cur_arg], 1, 1, &error))) {
		memprintf(err, "failed to parse the regex : %s", error);
		free(error);
		return ACT_RET_PRS_ERR;
	}

	LIST_INIT(&rule->arg.http.fmt);
	px->conf.args.ctx = ARGC_HRQ;
	if (!parse_logformat_string(args[cur_arg + 1], px, &rule->arg.http.fmt, LOG_OPT_HTTP,
	                            (px->cap & PR_CAP_FE) ? SMP_VAL_FE_HRQ_HDR : SMP_VAL_BE_HRQ_HDR, err)) {
		regex_free(rule->arg.http.re);
		return ACT_RET_PRS_ERR;
	}

	(*orig_arg) += 2;
	return ACT_RET_PRS_OK;
}

/* This function is just a compliant action wrapper for "set-status". */
static enum act_return action_http_set_status(struct act_rule *rule, struct proxy *px,
                                              struct session *sess, struct stream *s, int flags)
{
	if (http_res_set_status(rule->arg.http.i, rule->arg.http.str, s) == -1) {
		_HA_ATOMIC_ADD(&sess->fe->fe_counters.failed_rewrites, 1);
		if (s->flags & SF_BE_ASSIGNED)
			_HA_ATOMIC_ADD(&s->be->be_counters.failed_rewrites, 1);
		if (sess->listener->counters)
			_HA_ATOMIC_ADD(&sess->listener->counters->failed_rewrites, 1);
		if (objt_server(s->target))
			_HA_ATOMIC_ADD(&__objt_server(s->target)->counters.failed_rewrites, 1);

		if (!(s->txn->req.flags & HTTP_MSGF_SOFT_RW)) {
			if (!(s->flags & SF_ERR_MASK))
				s->flags |= SF_ERR_PRXCOND;
			return ACT_RET_ERR;
		}
	}

	return ACT_RET_CONT;
}

/* parse set-status action:
 * This action accepts a single argument of type int representing
 * an http status code. It returns ACT_RET_PRS_OK on success,
 * ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_set_status(const char **args, int *orig_arg, struct proxy *px,
                                                struct act_rule *rule, char **err)
{
	char *error;

	rule->action = ACT_CUSTOM;
	rule->action_ptr = action_http_set_status;
	rule->release_ptr = release_http_action;

	/* Check if an argument is available */
	if (!*args[*orig_arg]) {
		memprintf(err, "expects 1 argument: <status>; or 3 arguments: <status> reason <fmt>");
		return ACT_RET_PRS_ERR;
	}

	/* convert status code as integer */
	rule->arg.http.i = strtol(args[*orig_arg], &error, 10);
	if (*error != '\0' || rule->arg.http.i < 100 || rule->arg.http.i > 999) {
		memprintf(err, "expects an integer status code between 100 and 999");
		return ACT_RET_PRS_ERR;
	}

	(*orig_arg)++;

	/* set custom reason string */
	rule->arg.http.str = ist(NULL); // If null, we use the default reason for the status code.
	if (*args[*orig_arg] && strcmp(args[*orig_arg], "reason") == 0 &&
	    (*args[*orig_arg + 1] && strcmp(args[*orig_arg + 1], "if") != 0 && strcmp(args[*orig_arg + 1], "unless") != 0)) {
		(*orig_arg)++;
		rule->arg.http.str.ptr = strdup(args[*orig_arg]);
		rule->arg.http.str.len = strlen(rule->arg.http.str.ptr);
		(*orig_arg)++;
	}

	LIST_INIT(&rule->arg.http.fmt);
	return ACT_RET_PRS_OK;
}

/* This function executes the "reject" HTTP action. It clears the request and
 * response buffer without sending any response. It can be useful as an HTTP
 * alternative to the silent-drop action to defend against DoS attacks, and may
 * also be used with HTTP/2 to close a connection instead of just a stream.
 * The txn status is unchanged, indicating no response was sent. The termination
 * flags will indicate "PR". It always returns ACT_RET_ABRT.
 */
static enum act_return http_action_reject(struct act_rule *rule, struct proxy *px,
                                          struct session *sess, struct stream *s, int flags)
{
	si_must_kill_conn(chn_prod(&s->req));
	channel_abort(&s->req);
	channel_abort(&s->res);
	s->req.analysers &= AN_REQ_FLT_END;
	s->res.analysers &= AN_RES_FLT_END;

	_HA_ATOMIC_ADD(&s->be->be_counters.denied_req, 1);
	_HA_ATOMIC_ADD(&sess->fe->fe_counters.denied_req, 1);
	if (sess->listener && sess->listener->counters)
		_HA_ATOMIC_ADD(&sess->listener->counters->denied_req, 1);

	if (!(s->flags & SF_ERR_MASK))
		s->flags |= SF_ERR_PRXCOND;
	if (!(s->flags & SF_FINST_MASK))
		s->flags |= SF_FINST_R;

	return ACT_RET_ABRT;
}

/* parse the "reject" action:
 * This action takes no argument and returns ACT_RET_PRS_OK on success,
 * ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_action_reject(const char **args, int *orig_arg, struct proxy *px,
                                                   struct act_rule *rule, char **err)
{
	rule->action = ACT_CUSTOM;
	rule->action_ptr = http_action_reject;
	return ACT_RET_PRS_OK;
}

/* This function executes the "disable-l7-retry" HTTP action.
 * It disables L7 retries (all retry except for a connection failure). This
 * can be useful for example to avoid retrying on POST requests.
 * It just removes the L7 retry flag on the stream_interface, and always
 * return ACT_RET_CONT;
 */
static enum act_return http_req_disable_l7_retry(struct act_rule *rule, struct proxy *px,
                                          struct session *sess, struct stream *s, int flags)
{
	struct stream_interface *si = &s->si[1];

	/* In theory, the SI_FL_L7_RETRY flags isn't set at this point, but
	 * let's be future-proof and remove it anyway.
	 */
	si->flags &= ~SI_FL_L7_RETRY;
	si->flags |= SI_FL_D_L7_RETRY;
	return ACT_RET_CONT;
}

/* parse the "disable-l7-retry" action:
 * This action takes no argument and returns ACT_RET_PRS_OK on success,
 * ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_req_disable_l7_retry(const char **args,
							  int *orig_args, struct proxy *px,
							  struct act_rule *rule, char **err)
{
	rule->action = ACT_CUSTOM;
	rule->action_ptr = http_req_disable_l7_retry;
	return ACT_RET_PRS_OK;
}

/* This function executes the "capture" action. It executes a fetch expression,
 * turns the result into a string and puts it in a capture slot. It always
 * returns 1. If an error occurs the action is cancelled, but the rule
 * processing continues.
 */
static enum act_return http_action_req_capture(struct act_rule *rule, struct proxy *px,
                                               struct session *sess, struct stream *s, int flags)
{
	struct sample *key;
	struct cap_hdr *h = rule->arg.cap.hdr;
	char **cap = s->req_cap;
	int len;

	key = sample_fetch_as_type(s->be, sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL, rule->arg.cap.expr, SMP_T_STR);
	if (!key)
		return ACT_RET_CONT;

	if (cap[h->index] == NULL)
		cap[h->index] = pool_alloc(h->pool);

	if (cap[h->index] == NULL) /* no more capture memory */
		return ACT_RET_CONT;

	len = key->data.u.str.data;
	if (len > h->len)
		len = h->len;

	memcpy(cap[h->index], key->data.u.str.area, len);
	cap[h->index][len] = 0;
	return ACT_RET_CONT;
}

/* This function executes the "capture" action and store the result in a
 * capture slot if exists. It executes a fetch expression, turns the result
 * into a string and puts it in a capture slot. It always returns 1. If an
 * error occurs the action is cancelled, but the rule processing continues.
 */
static enum act_return http_action_req_capture_by_id(struct act_rule *rule, struct proxy *px,
                                                     struct session *sess, struct stream *s, int flags)
{
	struct sample *key;
	struct cap_hdr *h;
	char **cap = s->req_cap;
	struct proxy *fe = strm_fe(s);
	int len;
	int i;

	/* Look for the original configuration. */
	for (h = fe->req_cap, i = fe->nb_req_cap - 1;
	     h != NULL && i != rule->arg.capid.idx ;
	     i--, h = h->next);
	if (!h)
		return ACT_RET_CONT;

	key = sample_fetch_as_type(s->be, sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL, rule->arg.capid.expr, SMP_T_STR);
	if (!key)
		return ACT_RET_CONT;

	if (cap[h->index] == NULL)
		cap[h->index] = pool_alloc(h->pool);

	if (cap[h->index] == NULL) /* no more capture memory */
		return ACT_RET_CONT;

	len = key->data.u.str.data;
	if (len > h->len)
		len = h->len;

	memcpy(cap[h->index], key->data.u.str.area, len);
	cap[h->index][len] = 0;
	return ACT_RET_CONT;
}

/* Check an "http-request capture" action.
 *
 * The function returns 1 in success case, otherwise, it returns 0 and err is
 * filled.
 */
static int check_http_req_capture(struct act_rule *rule, struct proxy *px, char **err)
{
	if (rule->action_ptr != http_action_req_capture_by_id)
		return 1;

	/* capture slots can only be declared in frontends, so we can't check their
	 * existence in backends at configuration parsing step
	 */
	if (px->cap & PR_CAP_FE && rule->arg.capid.idx >= px->nb_req_cap) {
		memprintf(err, "unable to find capture id '%d' referenced by http-request capture rule",
			  rule->arg.capid.idx);
		return 0;
	}

	return 1;
}

/* Release memory allocate by an http capture action */
static void release_http_capture(struct act_rule *rule)
{
	if (rule->action_ptr == http_action_req_capture)
		release_sample_expr(rule->arg.cap.expr);
	else
		release_sample_expr(rule->arg.capid.expr);
}

/* parse an "http-request capture" action. It takes a single argument which is
 * a sample fetch expression. It stores the expression into arg->act.p[0] and
 * the allocated hdr_cap struct or the preallocated "id" into arg->act.p[1].
 * It returns ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_req_capture(const char **args, int *orig_arg, struct proxy *px,
                                                 struct act_rule *rule, char **err)
{
	struct sample_expr *expr;
	struct cap_hdr *hdr;
	int cur_arg;
	int len = 0;

	for (cur_arg = *orig_arg; cur_arg < *orig_arg + 3 && *args[cur_arg]; cur_arg++)
		if (strcmp(args[cur_arg], "if") == 0 ||
		    strcmp(args[cur_arg], "unless") == 0)
			break;

	if (cur_arg < *orig_arg + 3) {
		memprintf(err, "expects <expression> [ 'len' <length> | id <idx> ]");
		return ACT_RET_PRS_ERR;
	}

	cur_arg = *orig_arg;
	expr = sample_parse_expr((char **)args, &cur_arg, px->conf.args.file, px->conf.args.line, err, &px->conf.args, NULL);
	if (!expr)
		return ACT_RET_PRS_ERR;

	if (!(expr->fetch->val & SMP_VAL_FE_HRQ_HDR)) {
		memprintf(err,
			  "fetch method '%s' extracts information from '%s', none of which is available here",
			  args[cur_arg-1], sample_src_names(expr->fetch->use));
		release_sample_expr(expr);
		return ACT_RET_PRS_ERR;
	}

	if (!args[cur_arg] || !*args[cur_arg]) {
		memprintf(err, "expects 'len or 'id'");
		release_sample_expr(expr);
		return ACT_RET_PRS_ERR;
	}

	if (strcmp(args[cur_arg], "len") == 0) {
		cur_arg++;

		if (!(px->cap & PR_CAP_FE)) {
			memprintf(err, "proxy '%s' has no frontend capability", px->id);
			release_sample_expr(expr);
			return ACT_RET_PRS_ERR;
		}

		px->conf.args.ctx = ARGC_CAP;

		if (!args[cur_arg]) {
			memprintf(err, "missing length value");
			release_sample_expr(expr);
			return ACT_RET_PRS_ERR;
		}
		/* we copy the table name for now, it will be resolved later */
		len = atoi(args[cur_arg]);
		if (len <= 0) {
			memprintf(err, "length must be > 0");
			release_sample_expr(expr);
			return ACT_RET_PRS_ERR;
		}
		cur_arg++;

		hdr = calloc(1, sizeof(*hdr));
		hdr->next = px->req_cap;
		hdr->name = NULL; /* not a header capture */
		hdr->namelen = 0;
		hdr->len = len;
		hdr->pool = create_pool("caphdr", hdr->len + 1, MEM_F_SHARED);
		hdr->index = px->nb_req_cap++;

		px->req_cap = hdr;
		px->to_log |= LW_REQHDR;

		rule->action       = ACT_CUSTOM;
		rule->action_ptr   = http_action_req_capture;
		rule->release_ptr  = release_http_capture;
		rule->arg.cap.expr = expr;
		rule->arg.cap.hdr  = hdr;
	}

	else if (strcmp(args[cur_arg], "id") == 0) {
		int id;
		char *error;

		cur_arg++;

		if (!args[cur_arg]) {
			memprintf(err, "missing id value");
			release_sample_expr(expr);
			return ACT_RET_PRS_ERR;
		}

		id = strtol(args[cur_arg], &error, 10);
		if (*error != '\0') {
			memprintf(err, "cannot parse id '%s'", args[cur_arg]);
			release_sample_expr(expr);
			return ACT_RET_PRS_ERR;
		}
		cur_arg++;

		px->conf.args.ctx = ARGC_CAP;

		rule->action       = ACT_CUSTOM;
		rule->action_ptr   = http_action_req_capture_by_id;
		rule->check_ptr    = check_http_req_capture;
		rule->release_ptr  = release_http_capture;
		rule->arg.capid.expr = expr;
		rule->arg.capid.idx  = id;
	}

	else {
		memprintf(err, "expects 'len' or 'id', found '%s'", args[cur_arg]);
		release_sample_expr(expr);
		return ACT_RET_PRS_ERR;
	}

	*orig_arg = cur_arg;
	return ACT_RET_PRS_OK;
}

/* This function executes the "capture" action and store the result in a
 * capture slot if exists. It executes a fetch expression, turns the result
 * into a string and puts it in a capture slot. It always returns 1. If an
 * error occurs the action is cancelled, but the rule processing continues.
 */
static enum act_return http_action_res_capture_by_id(struct act_rule *rule, struct proxy *px,
                                                     struct session *sess, struct stream *s, int flags)
{
	struct sample *key;
	struct cap_hdr *h;
	char **cap = s->res_cap;
	struct proxy *fe = strm_fe(s);
	int len;
	int i;

	/* Look for the original configuration. */
	for (h = fe->rsp_cap, i = fe->nb_rsp_cap - 1;
	     h != NULL && i != rule->arg.capid.idx ;
	     i--, h = h->next);
	if (!h)
		return ACT_RET_CONT;

	key = sample_fetch_as_type(s->be, sess, s, SMP_OPT_DIR_RES|SMP_OPT_FINAL, rule->arg.capid.expr, SMP_T_STR);
	if (!key)
		return ACT_RET_CONT;

	if (cap[h->index] == NULL)
		cap[h->index] = pool_alloc(h->pool);

	if (cap[h->index] == NULL) /* no more capture memory */
		return ACT_RET_CONT;

	len = key->data.u.str.data;
	if (len > h->len)
		len = h->len;

	memcpy(cap[h->index], key->data.u.str.area, len);
	cap[h->index][len] = 0;
	return ACT_RET_CONT;
}

/* Check an "http-response capture" action.
 *
 * The function returns 1 in success case, otherwise, it returns 0 and err is
 * filled.
 */
static int check_http_res_capture(struct act_rule *rule, struct proxy *px, char **err)
{
	if (rule->action_ptr != http_action_res_capture_by_id)
		return 1;

	if (rule->arg.capid.idx >= px->nb_rsp_cap) {
		memprintf(err, "unable to find capture id '%d' referenced by http-response capture rule",
			  rule->arg.capid.idx);
		return 0;
	}

	return 1;
}

/* parse an "http-response capture" action. It takes a single argument which is
 * a sample fetch expression. It stores the expression into arg->act.p[0] and
 * the allocated hdr_cap struct od the preallocated id into arg->act.p[1].
 * It returns ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_res_capture(const char **args, int *orig_arg, struct proxy *px,
                                                 struct act_rule *rule, char **err)
{
	struct sample_expr *expr;
	int cur_arg;
	int id;
	char *error;

	for (cur_arg = *orig_arg; cur_arg < *orig_arg + 3 && *args[cur_arg]; cur_arg++)
		if (strcmp(args[cur_arg], "if") == 0 ||
		    strcmp(args[cur_arg], "unless") == 0)
			break;

	if (cur_arg < *orig_arg + 3) {
		memprintf(err, "expects <expression> id <idx>");
		return ACT_RET_PRS_ERR;
	}

	cur_arg = *orig_arg;
	expr = sample_parse_expr((char **)args, &cur_arg, px->conf.args.file, px->conf.args.line, err, &px->conf.args, NULL);
	if (!expr)
		return ACT_RET_PRS_ERR;

	if (!(expr->fetch->val & SMP_VAL_FE_HRS_HDR)) {
		memprintf(err,
			  "fetch method '%s' extracts information from '%s', none of which is available here",
			  args[cur_arg-1], sample_src_names(expr->fetch->use));
		release_sample_expr(expr);
		return ACT_RET_PRS_ERR;
	}

	if (!args[cur_arg] || !*args[cur_arg]) {
		memprintf(err, "expects 'id'");
		release_sample_expr(expr);
		return ACT_RET_PRS_ERR;
	}

	if (strcmp(args[cur_arg], "id") != 0) {
		memprintf(err, "expects 'id', found '%s'", args[cur_arg]);
		release_sample_expr(expr);
		return ACT_RET_PRS_ERR;
	}

	cur_arg++;

	if (!args[cur_arg]) {
		memprintf(err, "missing id value");
		release_sample_expr(expr);
		return ACT_RET_PRS_ERR;
	}

	id = strtol(args[cur_arg], &error, 10);
	if (*error != '\0') {
		memprintf(err, "cannot parse id '%s'", args[cur_arg]);
		release_sample_expr(expr);
		return ACT_RET_PRS_ERR;
	}
	cur_arg++;

	px->conf.args.ctx = ARGC_CAP;

	rule->action       = ACT_CUSTOM;
	rule->action_ptr   = http_action_res_capture_by_id;
	rule->check_ptr    = check_http_res_capture;
	rule->release_ptr  = release_http_capture;
	rule->arg.capid.expr = expr;
	rule->arg.capid.idx  = id;

	*orig_arg = cur_arg;
	return ACT_RET_PRS_OK;
}

/* Parse a "allow" action for a request or a response rule. It takes no argument. It
 * returns ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_allow(const char **args, int *orig_arg, struct proxy *px,
					   struct act_rule *rule, char **err)
{
	rule->action = ACT_ACTION_ALLOW;
	rule->flags |= ACT_FLAG_FINAL;
	return ACT_RET_PRS_OK;
}

/* Parse "deny" or "tarpit" actions for a request rule or "deny" action for a
 * response rule. It returns ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on
 * error. It relies on http_parse_http_reply() to set
 * <.arg.http_reply>.
 */
static enum act_parse_ret parse_http_deny(const char **args, int *orig_arg, struct proxy *px,
					  struct act_rule *rule, char **err)
{
	int default_status;
	int cur_arg, arg = 0;

	cur_arg = *orig_arg;
	if (rule->from == ACT_F_HTTP_REQ) {
		if (!strcmp(args[cur_arg-1], "tarpit")) {
			rule->action = ACT_HTTP_REQ_TARPIT;
			default_status = 500;
		}
		else {
			rule->action = ACT_ACTION_DENY;
			default_status = 403;
		}
	}
	else {
		rule->action = ACT_ACTION_DENY;
		default_status = 502;
	}

	/* If no args or only a deny_status specified, fallback on the legacy
	 * mode and use default error files despite the fact that
	 * default-errorfiles is not used. Otherwise, parse an http reply.
	 */

	/* Prepare parsing of log-format strings */
	px->conf.args.ctx = ((rule->from == ACT_F_HTTP_REQ) ? ARGC_HRQ : ARGC_HRS);

	if (!*(args[cur_arg])) {
		rule->arg.http_reply = http_parse_http_reply((const char *[]){"default-errorfiles", ""}, &arg, px, default_status, err);
		goto end;
	}

	if (strcmp(args[cur_arg], "deny_status") == 0) {
		if (!*(args[cur_arg+2]) ||
		    (strcmp(args[cur_arg+2], "errorfile") != 0 && strcmp(args[cur_arg+2], "errorfiles") != 0)) {
			rule->arg.http_reply = http_parse_http_reply((const char *[]){"status", args[cur_arg+1], "default-errorfiles", ""},
								     &arg, px, default_status, err);
			*orig_arg += 2;
			goto end;
		}
		args[cur_arg] += 5; /* skip "deny_" for the parsing */
	}

	rule->arg.http_reply = http_parse_http_reply(args, orig_arg, px, default_status, err);

  end:
	if (!rule->arg.http_reply)
		return ACT_RET_PRS_ERR;

	rule->flags |= ACT_FLAG_FINAL;
	rule->check_ptr = check_act_http_reply;
	rule->release_ptr = release_act_http_reply;
	return ACT_RET_PRS_OK;
}


/* This function executes a auth action. It builds an 401/407 HTX message using
 * the corresponding proxy's error message. On success, it returns
 * ACT_RET_ABRT. If an error occurs ACT_RET_ERR is returned.
 */
static enum act_return http_action_auth(struct act_rule *rule, struct proxy *px,
					struct session *sess, struct stream *s, int flags)
{
	struct channel *req = &s->req;
	struct channel *res = &s->res;
	struct htx *htx = htx_from_buf(&res->buf);
	struct http_reply *reply;
	const char *auth_realm;
	struct http_hdr_ctx ctx;
	struct ist hdr;

	/* Auth might be performed on regular http-req rules as well as on stats */
	auth_realm = rule->arg.http.str.ptr;
	if (!auth_realm) {
		if (px->uri_auth && s->current_rule_list == &px->uri_auth->http_req_rules)
			auth_realm = STATS_DEFAULT_REALM;
		else
			auth_realm = px->id;
	}

	if (!(s->txn->flags & TX_USE_PX_CONN)) {
		s->txn->status = 401;
		hdr = ist("WWW-Authenticate");
	}
	else {
		s->txn->status = 407;
		hdr = ist("Proxy-Authenticate");
	}
	reply = http_error_message(s);
	channel_htx_truncate(res, htx);

	if (chunk_printf(&trash, "Basic realm=\"%s\"", auth_realm) == -1)
		goto fail;

	/* Write the generic 40x message */
	if (http_reply_to_htx(s, htx, reply) == -1)
		goto fail;

	/* Remove all existing occurrences of the XXX-Authenticate header */
	ctx.blk = NULL;
	while (http_find_header(htx, hdr, &ctx, 1))
		http_remove_header(htx, &ctx);

	/* Now a the right XXX-Authenticate header */
	if (!http_add_header(htx, hdr, ist2(b_orig(&trash), b_data(&trash))))
		goto fail;

	/* Finally forward the reply */
	htx_to_buf(htx, &res->buf);
	if (!http_forward_proxy_resp(s, 1))
		goto fail;

	/* Note: Only eval on the request */
	s->logs.tv_request = now;
	req->analysers &= AN_REQ_FLT_END;

	if (s->sess->fe == s->be) /* report it if the request was intercepted by the frontend */
		_HA_ATOMIC_ADD(&s->sess->fe->fe_counters.intercepted_req, 1);

	if (!(s->flags & SF_ERR_MASK))
		s->flags |= SF_ERR_LOCAL;
	if (!(s->flags & SF_FINST_MASK))
		s->flags |= SF_FINST_R;

	stream_inc_http_err_ctr(s);
	return ACT_RET_ABRT;

  fail:
	/* If an error occurred, remove the incomplete HTTP response from the
	 * buffer */
	channel_htx_truncate(res, htx);
	return ACT_RET_ERR;
}

/* Parse a "auth" action. It may take 2 optional arguments to define a "realm"
 * parameter. It returns ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_auth(const char **args, int *orig_arg, struct proxy *px,
					  struct act_rule *rule, char **err)
{
	int cur_arg;

	rule->action = ACT_CUSTOM;
	rule->flags |= ACT_FLAG_FINAL;
	rule->action_ptr = http_action_auth;
	rule->release_ptr = release_http_action;

	cur_arg = *orig_arg;
	if (!strcmp(args[cur_arg], "realm")) {
		cur_arg++;
		if (!*args[cur_arg]) {
			memprintf(err, "missing realm value.\n");
			return ACT_RET_PRS_ERR;
		}
		rule->arg.http.str.ptr = strdup(args[cur_arg]);
		rule->arg.http.str.len = strlen(rule->arg.http.str.ptr);
		cur_arg++;
	}

	LIST_INIT(&rule->arg.http.fmt);
	*orig_arg = cur_arg;
	return ACT_RET_PRS_OK;
}

/* Parse a "set-nice" action. It takes the nice value as argument. It returns
 * ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_set_nice(const char **args, int *orig_arg, struct proxy *px,
					      struct act_rule *rule, char **err)
{
	int cur_arg;

	rule->action = ACT_HTTP_SET_NICE;

	cur_arg = *orig_arg;
	if (!*args[cur_arg]) {
		memprintf(err, "expects exactly 1 argument (integer value)");
		return ACT_RET_PRS_ERR;
	}
	rule->arg.http.i = atoi(args[cur_arg]);
	if (rule->arg.http.i < -1024)
		rule->arg.http.i = -1024;
	else if (rule->arg.http.i > 1024)
		rule->arg.http.i = 1024;

	LIST_INIT(&rule->arg.http.fmt);
	*orig_arg = cur_arg + 1;
	return ACT_RET_PRS_OK;
}

/* Parse a "set-tos" action. It takes the TOS value as argument. It returns
 * ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_set_tos(const char **args, int *orig_arg, struct proxy *px,
					      struct act_rule *rule, char **err)
{
#ifdef IP_TOS
	char *endp;
	int cur_arg;

	rule->action = ACT_HTTP_SET_TOS;

	cur_arg = *orig_arg;
	if (!*args[cur_arg]) {
		memprintf(err, "expects exactly 1 argument (integer/hex value)");
		return ACT_RET_PRS_ERR;
	}
	rule->arg.http.i = strtol(args[cur_arg], &endp, 0);
	if (endp && *endp != '\0') {
		memprintf(err, "invalid character starting at '%s' (integer/hex value expected)", endp);
		return ACT_RET_PRS_ERR;
	}

	LIST_INIT(&rule->arg.http.fmt);
	*orig_arg = cur_arg + 1;
	return ACT_RET_PRS_OK;
#else
	memprintf(err, "not supported on this platform (IP_TOS undefined)");
	return ACT_RET_PRS_ERR;
#endif
}

/* Parse a "set-mark" action. It takes the MARK value as argument. It returns
 * ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_set_mark(const char **args, int *orig_arg, struct proxy *px,
					      struct act_rule *rule, char **err)
{
#ifdef SO_MARK
	char *endp;
	int cur_arg;

	rule->action = ACT_HTTP_SET_MARK;

	cur_arg = *orig_arg;
	if (!*args[cur_arg]) {
		memprintf(err, "expects exactly 1 argument (integer/hex value)");
		return ACT_RET_PRS_ERR;
	}
	rule->arg.http.i = strtoul(args[cur_arg], &endp, 0);
	if (endp && *endp != '\0') {
		memprintf(err, "invalid character starting at '%s' (integer/hex value expected)", endp);
		return ACT_RET_PRS_ERR;
	}

	LIST_INIT(&rule->arg.http.fmt);
	*orig_arg = cur_arg + 1;
	global.last_checks |= LSTCHK_NETADM;
	return ACT_RET_PRS_OK;
#else
	memprintf(err, "not supported on this platform (SO_MARK undefined)");
	return ACT_RET_PRS_ERR;
#endif
}

/* Parse a "set-log-level" action. It takes the level value as argument. It
 * returns ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_set_log_level(const char **args, int *orig_arg, struct proxy *px,
						   struct act_rule *rule, char **err)
{
	int cur_arg;

	rule->action = ACT_HTTP_SET_LOGL;

	cur_arg = *orig_arg;
	if (!*args[cur_arg]) {
	  bad_log_level:
		memprintf(err, "expects exactly 1 argument (log level name or 'silent')");
		return ACT_RET_PRS_ERR;
	}
	if (strcmp(args[cur_arg], "silent") == 0)
		rule->arg.http.i = -1;
	else if ((rule->arg.http.i = get_log_level(args[cur_arg]) + 1) == 0)
		goto bad_log_level;

	LIST_INIT(&rule->arg.http.fmt);
	*orig_arg = cur_arg + 1;
	return ACT_RET_PRS_OK;
}

/* This function executes a early-hint action. It adds an HTTP Early Hint HTTP
 * 103 response header with <.arg.http.str> name and with a value built
 * according to <.arg.http.fmt> log line format. If it is the first early-hint
 * rule of series, the 103 response start-line is added first. At the end, if
 * the next rule is not an early-hint rule or if it is the last rule, the EOH
 * block is added to terminate the response. On success, it returns
 * ACT_RET_CONT. If an error occurs while soft rewrites are enabled, the action
 * is canceled, but the rule processing continue. Otherwsize ACT_RET_ERR is
 * returned.
 */
static enum act_return http_action_early_hint(struct act_rule *rule, struct proxy *px,
					      struct session *sess, struct stream *s, int flags)
{
	struct act_rule *prev_rule, *next_rule;
	struct channel *res = &s->res;
	struct htx *htx = htx_from_buf(&res->buf);
	struct buffer *value = alloc_trash_chunk();
	enum act_return ret = ACT_RET_CONT;

	if (!(s->txn->req.flags & HTTP_MSGF_VER_11))
		goto leave;

	if (!value) {
		if (!(s->flags & SF_ERR_MASK))
			s->flags |= SF_ERR_RESOURCE;
		goto error;
	}

	/* get previous and next rules */
	prev_rule = LIST_PREV(&rule->list, typeof(rule), list);
	next_rule = LIST_NEXT(&rule->list, typeof(rule), list);

	/* if no previous rule or previous rule is not early-hint, start a new response. Otherwise,
	 * continue to add link to a previously started response */
	if (&prev_rule->list == s->current_rule_list || prev_rule->action_ptr != http_action_early_hint) {
		struct htx_sl *sl;
		unsigned int flags = (HTX_SL_F_IS_RESP|HTX_SL_F_VER_11|
				      HTX_SL_F_XFER_LEN|HTX_SL_F_BODYLESS);

		sl = htx_add_stline(htx, HTX_BLK_RES_SL, flags,
				    ist("HTTP/1.1"), ist("103"), ist("Early Hints"));
		if (!sl)
			goto error;
		sl->info.res.status = 103;
	}

	/* Add the HTTP Early Hint HTTP 103 response heade */
	value->data = build_logline(s, b_tail(value), b_room(value), &rule->arg.http.fmt);
	if (!htx_add_header(htx, rule->arg.http.str, ist2(b_head(value), b_data(value))))
		goto error;

	/* if it is the last rule or the next one is not an early-hint, terminate the current
	 * response. */
	if (&next_rule->list == s->current_rule_list || next_rule->action_ptr != http_action_early_hint) {
		if (!htx_add_endof(htx, HTX_BLK_EOH)) {
			/* If an error occurred during an Early-hint rule,
			 * remove the incomplete HTTP 103 response from the
			 * buffer */
			goto error;
		}

		if (!http_forward_proxy_resp(s, 0))
			goto error;
	}

  leave:
	free_trash_chunk(value);
	return ret;

  error:
	/* If an error occurred during an Early-hint rule, remove the incomplete
	 * HTTP 103 response from the buffer */
	channel_htx_truncate(res, htx);
	ret = ACT_RET_ERR;
	goto leave;
}

/* This function executes a set-header or add-header actions. It builds a string
 * in the trash from the specified format string. It finds the action to be
 * performed in <.action>, previously filled by function parse_set_header(). The
 * replacement action is executed by the function http_action_set_header(). On
 * success, it returns ACT_RET_CONT. If an error occurs while soft rewrites are
 * enabled, the action is canceled, but the rule processing continue. Otherwsize
 * ACT_RET_ERR is returned.
 */
static enum act_return http_action_set_header(struct act_rule *rule, struct proxy *px,
					      struct session *sess, struct stream *s, int flags)
{
	struct http_msg *msg = ((rule->from == ACT_F_HTTP_REQ) ? &s->txn->req : &s->txn->rsp);
	struct htx *htx = htxbuf(&msg->chn->buf);
	enum act_return ret = ACT_RET_CONT;
	struct buffer *replace;
	struct http_hdr_ctx ctx;
	struct ist n, v;

	replace = alloc_trash_chunk();
	if (!replace)
		goto fail_alloc;

	replace->data = build_logline(s, replace->area, replace->size, &rule->arg.http.fmt);
	n = rule->arg.http.str;
	v = ist2(replace->area, replace->data);

	if (rule->action == 0) { // set-header
		/* remove all occurrences of the header */
		ctx.blk = NULL;
		while (http_find_header(htx, n, &ctx, 1))
			http_remove_header(htx, &ctx);
	}

	/* Now add header */
	if (!http_add_header(htx, n, v))
		goto fail_rewrite;

  leave:
	free_trash_chunk(replace);
	return ret;

  fail_alloc:
	if (!(s->flags & SF_ERR_MASK))
		s->flags |= SF_ERR_RESOURCE;
	ret = ACT_RET_ERR;
	goto leave;

  fail_rewrite:
	_HA_ATOMIC_ADD(&sess->fe->fe_counters.failed_rewrites, 1);
	if (s->flags & SF_BE_ASSIGNED)
		_HA_ATOMIC_ADD(&s->be->be_counters.failed_rewrites, 1);
	if (sess->listener->counters)
		_HA_ATOMIC_ADD(&sess->listener->counters->failed_rewrites, 1);
	if (objt_server(s->target))
		_HA_ATOMIC_ADD(&__objt_server(s->target)->counters.failed_rewrites, 1);

	if (!(msg->flags & HTTP_MSGF_SOFT_RW)) {
		ret = ACT_RET_ERR;
		if (!(s->flags & SF_ERR_MASK))
			s->flags |= SF_ERR_PRXCOND;
	}
	goto leave;
}

/* Parse a "set-header", "add-header" or "early-hint" actions. It takes an
 * header name and a log-format string as arguments. It returns ACT_RET_PRS_OK
 * on success, ACT_RET_PRS_ERR on error.
 *
 * Note: same function is used for the request and the response. However
 * "early-hint" rules are only supported for request rules.
 */
static enum act_parse_ret parse_http_set_header(const char **args, int *orig_arg, struct proxy *px,
						   struct act_rule *rule, char **err)
{
	int cap, cur_arg;

	if (args[*orig_arg-1][0] == 'e') {
		rule->action = ACT_CUSTOM;
		rule->action_ptr = http_action_early_hint;
	}
	else {
		if (args[*orig_arg-1][0] == 's')
			rule->action = 0; // set-header
		else
			rule->action = 1; // add-header
		rule->action_ptr = http_action_set_header;
	}
	rule->release_ptr = release_http_action;

	cur_arg = *orig_arg;
	if (!*args[cur_arg] || !*args[cur_arg+1]) {
		memprintf(err, "expects exactly 2 arguments");
		return ACT_RET_PRS_ERR;
	}


	rule->arg.http.str.ptr = strdup(args[cur_arg]);
	rule->arg.http.str.len = strlen(rule->arg.http.str.ptr);
	LIST_INIT(&rule->arg.http.fmt);

	if (rule->from == ACT_F_HTTP_REQ) {
		px->conf.args.ctx = ARGC_HRQ;
		cap = (px->cap & PR_CAP_FE) ? SMP_VAL_FE_HRQ_HDR : SMP_VAL_BE_HRQ_HDR;
	}
	else{
		px->conf.args.ctx =  ARGC_HRS;
		cap = (px->cap & PR_CAP_BE) ? SMP_VAL_BE_HRS_HDR : SMP_VAL_FE_HRS_HDR;
	}

	cur_arg++;
	if (!parse_logformat_string(args[cur_arg], px, &rule->arg.http.fmt, LOG_OPT_HTTP, cap, err)) {
		istfree(&rule->arg.http.str);
		return ACT_RET_PRS_ERR;
	}

	free(px->conf.lfs_file);
	px->conf.lfs_file = strdup(px->conf.args.file);
	px->conf.lfs_line = px->conf.args.line;

	*orig_arg = cur_arg + 1;
	return ACT_RET_PRS_OK;
}

/* This function executes a replace-header or replace-value actions. It
 * builds a string in the trash from the specified format string. It finds
 * the action to be performed in <.action>, previously filled by function
 * parse_replace_header(). The replacement action is executed by the function
 * http_action_replace_header(). On success, it returns ACT_RET_CONT. If an error
 * occurs while soft rewrites are enabled, the action is canceled, but the rule
 * processing continue. Otherwsize ACT_RET_ERR is returned.
 */
static enum act_return http_action_replace_header(struct act_rule *rule, struct proxy *px,
						  struct session *sess, struct stream *s, int flags)
{
	struct http_msg *msg = ((rule->from == ACT_F_HTTP_REQ) ? &s->txn->req : &s->txn->rsp);
	struct htx *htx = htxbuf(&msg->chn->buf);
	enum act_return ret = ACT_RET_CONT;
	struct buffer *replace;
	int r;

	replace = alloc_trash_chunk();
	if (!replace)
		goto fail_alloc;

	replace->data = build_logline(s, replace->area, replace->size, &rule->arg.http.fmt);

	r = http_replace_hdrs(s, htx, rule->arg.http.str, replace->area, rule->arg.http.re, (rule->action == 0));
	if (r == -1)
		goto fail_rewrite;

  leave:
	free_trash_chunk(replace);
	return ret;

  fail_alloc:
	if (!(s->flags & SF_ERR_MASK))
		s->flags |= SF_ERR_RESOURCE;
	ret = ACT_RET_ERR;
	goto leave;

  fail_rewrite:
	_HA_ATOMIC_ADD(&sess->fe->fe_counters.failed_rewrites, 1);
	if (s->flags & SF_BE_ASSIGNED)
		_HA_ATOMIC_ADD(&s->be->be_counters.failed_rewrites, 1);
	if (sess->listener->counters)
		_HA_ATOMIC_ADD(&sess->listener->counters->failed_rewrites, 1);
	if (objt_server(s->target))
		_HA_ATOMIC_ADD(&__objt_server(s->target)->counters.failed_rewrites, 1);

	if (!(msg->flags & HTTP_MSGF_SOFT_RW)) {
		ret = ACT_RET_ERR;
		if (!(s->flags & SF_ERR_MASK))
			s->flags |= SF_ERR_PRXCOND;
	}
	goto leave;
}

/* Parse a "replace-header" or "replace-value" actions. It takes an header name,
 * a regex and replacement string as arguments. It returns ACT_RET_PRS_OK on
 * success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_replace_header(const char **args, int *orig_arg, struct proxy *px,
						    struct act_rule *rule, char **err)
{
	int cap, cur_arg;

	if (args[*orig_arg-1][8] == 'h')
		rule->action = 0; // replace-header
	else
		rule->action = 1; // replace-value
	rule->action_ptr = http_action_replace_header;
	rule->release_ptr = release_http_action;

	cur_arg = *orig_arg;
	if (!*args[cur_arg] || !*args[cur_arg+1] || !*args[cur_arg+2]) {
		memprintf(err, "expects exactly 3 arguments");
		return ACT_RET_PRS_ERR;
	}

	rule->arg.http.str.ptr = strdup(args[cur_arg]);
	rule->arg.http.str.len = strlen(rule->arg.http.str.ptr);
	LIST_INIT(&rule->arg.http.fmt);

	cur_arg++;
	if (!(rule->arg.http.re = regex_comp(args[cur_arg], 1, 1, err))) {
		istfree(&rule->arg.http.str);
		return ACT_RET_PRS_ERR;
	}

	if (rule->from == ACT_F_HTTP_REQ) {
		px->conf.args.ctx = ARGC_HRQ;
		cap = (px->cap & PR_CAP_FE) ? SMP_VAL_FE_HRQ_HDR : SMP_VAL_BE_HRQ_HDR;
	}
	else{
		px->conf.args.ctx =  ARGC_HRS;
		cap = (px->cap & PR_CAP_BE) ? SMP_VAL_BE_HRS_HDR : SMP_VAL_FE_HRS_HDR;
	}

	cur_arg++;
	if (!parse_logformat_string(args[cur_arg], px, &rule->arg.http.fmt, LOG_OPT_HTTP, cap, err)) {
		istfree(&rule->arg.http.str);
		regex_free(rule->arg.http.re);
		return ACT_RET_PRS_ERR;
	}

	free(px->conf.lfs_file);
	px->conf.lfs_file = strdup(px->conf.args.file);
	px->conf.lfs_line = px->conf.args.line;

	*orig_arg = cur_arg + 1;
	return ACT_RET_PRS_OK;
}

/* Parse a "del-header" action. It takes an header name as argument. It returns
 * ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_del_header(const char **args, int *orig_arg, struct proxy *px,
						struct act_rule *rule, char **err)
{
	int cur_arg;

	rule->action = ACT_HTTP_DEL_HDR;
	rule->release_ptr = release_http_action;

	cur_arg = *orig_arg;
	if (!*args[cur_arg]) {
		memprintf(err, "expects exactly 1 arguments");
		return ACT_RET_PRS_ERR;
	}

	rule->arg.http.str.ptr = strdup(args[cur_arg]);
	rule->arg.http.str.len = strlen(rule->arg.http.str.ptr);
	px->conf.args.ctx = (rule->from == ACT_F_HTTP_REQ ? ARGC_HRQ : ARGC_HRS);

	LIST_INIT(&rule->arg.http.fmt);
	*orig_arg = cur_arg + 1;
	return ACT_RET_PRS_OK;
}

/* Release memory allocated by an http redirect action. */
static void release_http_redir(struct act_rule *rule)
{
	struct logformat_node *lf, *lfb;
	struct redirect_rule *redir;

	redir = rule->arg.redir;
	LIST_DEL(&redir->list);
	if (redir->cond) {
		prune_acl_cond(redir->cond);
		free(redir->cond);
	}
	free(redir->rdr_str);
	free(redir->cookie_str);
	list_for_each_entry_safe(lf, lfb, &redir->rdr_fmt, list) {
		LIST_DEL(&lf->list);
		free(lf);
	}
	free(redir);
}

/* Parse a "redirect" action. It returns ACT_RET_PRS_OK on success,
 * ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_redirect(const char **args, int *orig_arg, struct proxy *px,
					      struct act_rule *rule, char **err)
{
	struct redirect_rule *redir;
	int dir, cur_arg;

	rule->action = ACT_HTTP_REDIR;
	rule->flags |= ACT_FLAG_FINAL;
	rule->release_ptr = release_http_redir;

	cur_arg = *orig_arg;

	dir = (rule->from == ACT_F_HTTP_REQ ? 0 : 1);
	if ((redir = http_parse_redirect_rule(px->conf.args.file, px->conf.args.line, px, &args[cur_arg], err, 1, dir)) == NULL)
		return ACT_RET_PRS_ERR;

	rule->arg.redir = redir;
	rule->cond = redir->cond;
	redir->cond = NULL;

	/* skip all arguments */
	while (*args[cur_arg])
		cur_arg++;

	*orig_arg = cur_arg;
	return ACT_RET_PRS_OK;
}

/* This function executes a add-acl, del-acl, set-map or del-map actions. On
 * success, it returns ACT_RET_CONT. Otherwsize ACT_RET_ERR is returned.
 */
static enum act_return http_action_set_map(struct act_rule *rule, struct proxy *px,
					   struct session *sess, struct stream *s, int flags)
{
	struct pat_ref *ref;
	struct buffer *key = NULL, *value = NULL;
	enum act_return ret = ACT_RET_CONT;

	/* collect reference */
	ref = pat_ref_lookup(rule->arg.map.ref);
	if (!ref)
		goto leave;

	/* allocate key */
	key = alloc_trash_chunk();
	if (!key)
		goto fail_alloc;

	/* collect key */
	key->data = build_logline(s, key->area, key->size, &rule->arg.map.key);
	key->area[key->data] = '\0';

	switch (rule->action) {
	case 0: // add-acl
		/* add entry only if it does not already exist */
		HA_SPIN_LOCK(PATREF_LOCK, &ref->lock);
		if (pat_ref_find_elt(ref, key->area) == NULL)
			pat_ref_add(ref, key->area, NULL, NULL);
		HA_SPIN_UNLOCK(PATREF_LOCK, &ref->lock);
		break;

	case 1: // set-map
		/* allocate value */
		value = alloc_trash_chunk();
		if (!value)
			goto fail_alloc;

		/* collect value */
		value->data = build_logline(s, value->area, value->size, &rule->arg.map.value);
		value->area[value->data] = '\0';

		HA_SPIN_LOCK(PATREF_LOCK, &ref->lock);
		if (pat_ref_find_elt(ref, key->area) != NULL) {
			/* update entry if it exists */
			pat_ref_set(ref, key->area, value->area, NULL);
		}
		else {
			/* insert a new entry */
			pat_ref_add(ref, key->area, value->area, NULL);
		}
		HA_SPIN_UNLOCK(PATREF_LOCK, &ref->lock);
		break;

	case 2: // del-acl
	case 3: // del-map
		/* returned code: 1=ok, 0=ko */
		HA_SPIN_LOCK(PATREF_LOCK, &ref->lock);
		pat_ref_delete(ref, key->area);
		HA_SPIN_UNLOCK(PATREF_LOCK, &ref->lock);
		break;

	default:
		ret = ACT_RET_ERR;
	}


  leave:
	free_trash_chunk(key);
	free_trash_chunk(value);
	return ret;

  fail_alloc:
	if (!(s->flags & SF_ERR_MASK))
		s->flags |= SF_ERR_RESOURCE;
	ret = ACT_RET_ERR;
	goto leave;
}

/* Release memory allocated by an http map/acl action. */
static void release_http_map(struct act_rule *rule)
{
	struct logformat_node *lf, *lfb;

	free(rule->arg.map.ref);
	list_for_each_entry_safe(lf, lfb, &rule->arg.map.key, list) {
		LIST_DEL(&lf->list);
		release_sample_expr(lf->expr);
		free(lf->arg);
		free(lf);
	}
	if (rule->action == 1) {
		list_for_each_entry_safe(lf, lfb, &rule->arg.map.value, list) {
			LIST_DEL(&lf->list);
			release_sample_expr(lf->expr);
			free(lf->arg);
			free(lf);
		}
	}
}

/* Parse a "add-acl", "del-acl", "set-map" or "del-map" actions. It takes one or
 * two log-format string as argument depending on the action. The action is
 * stored in <.action> as an int (0=add-acl, 1=set-map, 2=del-acl,
 * 3=del-map). It returns ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_set_map(const char **args, int *orig_arg, struct proxy *px,
					     struct act_rule *rule, char **err)
{
	int cap, cur_arg;

	if (args[*orig_arg-1][0] == 'a') // add-acl
		rule->action = 0;
	else if (args[*orig_arg-1][0] == 's') // set-map
		rule->action = 1;
	else if (args[*orig_arg-1][4] == 'a') // del-acl
		rule->action = 2;
	else if (args[*orig_arg-1][4] == 'm') // del-map
		rule->action = 3;
	else {
		memprintf(err, "internal error: unhandled action '%s'", args[0]);
		return ACT_RET_PRS_ERR;
	}
	rule->action_ptr = http_action_set_map;
	rule->release_ptr = release_http_map;

	cur_arg = *orig_arg;
	if (rule->action == 1 && (!*args[cur_arg] || !*args[cur_arg+1])) {
		/* 2 args for set-map */
		memprintf(err, "expects exactly 2 arguments");
		return ACT_RET_PRS_ERR;
	}
	else if (!*args[cur_arg]) {
		/* only one arg for other actions */
		memprintf(err, "expects exactly 1 arguments");
		return ACT_RET_PRS_ERR;
	}

	/*
	 * '+ 8' for 'set-map(' (same for del-map)
	 * '- 9' for 'set-map(' + trailing ')'  (same for del-map)
	 */
	rule->arg.map.ref = my_strndup(args[cur_arg-1] + 8, strlen(args[cur_arg-1]) - 9);

	if (rule->from == ACT_F_HTTP_REQ) {
		px->conf.args.ctx = ARGC_HRQ;
		cap = (px->cap & PR_CAP_FE) ? SMP_VAL_FE_HRQ_HDR : SMP_VAL_BE_HRQ_HDR;
	}
	else{
		px->conf.args.ctx =  ARGC_HRS;
		cap = (px->cap & PR_CAP_BE) ? SMP_VAL_BE_HRS_HDR : SMP_VAL_FE_HRS_HDR;
	}

	/* key pattern */
	LIST_INIT(&rule->arg.map.key);
	if (!parse_logformat_string(args[cur_arg], px, &rule->arg.map.key, LOG_OPT_HTTP, cap, err)) {
		free(rule->arg.map.ref);
		return ACT_RET_PRS_ERR;
	}

	if (rule->action == 1) {
		/* value pattern for set-map only */
		cur_arg++;
		LIST_INIT(&rule->arg.map.value);
		if (!parse_logformat_string(args[cur_arg], px, &rule->arg.map.value, LOG_OPT_HTTP, cap, err)) {
			free(rule->arg.map.ref);
			return ACT_RET_PRS_ERR;
		}
	}

	free(px->conf.lfs_file);
	px->conf.lfs_file = strdup(px->conf.args.file);
	px->conf.lfs_line = px->conf.args.line;

	*orig_arg = cur_arg + 1;
	return ACT_RET_PRS_OK;
}

/* This function executes a track-sc* actions. On success, it returns
 * ACT_RET_CONT. Otherwsize ACT_RET_ERR is returned.
 */
static enum act_return http_action_track_sc(struct act_rule *rule, struct proxy *px,
					    struct session *sess, struct stream *s, int flags)
{
	struct stktable *t;
	struct stksess *ts;
	struct stktable_key *key;
	void *ptr1, *ptr2, *ptr3, *ptr4;
	int opt;

	ptr1 = ptr2 = ptr3 = ptr4 = NULL;
	opt = ((rule->from == ACT_F_HTTP_REQ) ? SMP_OPT_DIR_REQ : SMP_OPT_DIR_RES) | SMP_OPT_FINAL;

	t = rule->arg.trk_ctr.table.t;
	key = stktable_fetch_key(t, s->be, sess, s, opt, rule->arg.trk_ctr.expr, NULL);

	if (!key)
		goto end;
	ts = stktable_get_entry(t, key);
	if (!ts)
		goto end;

	stream_track_stkctr(&s->stkctr[rule->action], t, ts);

	/* let's count a new HTTP request as it's the first time we do it */
	ptr1 = stktable_data_ptr(t, ts, STKTABLE_DT_HTTP_REQ_CNT);
	ptr2 = stktable_data_ptr(t, ts, STKTABLE_DT_HTTP_REQ_RATE);

	/* When the client triggers a 4xx from the server, it's most often due
	 * to a missing object or permission. These events should be tracked
	 * because if they happen often, it may indicate a brute force or a
	 * vulnerability scan. Normally this is done when receiving the response
	 * but here we're tracking after this ought to have been done so we have
	 * to do it on purpose.
	 */
	if (rule->from == ACT_F_HTTP_RES && (unsigned)(s->txn->status - 400) < 100) {
		ptr3 = stktable_data_ptr(t, ts, STKTABLE_DT_HTTP_ERR_CNT);
		ptr4 = stktable_data_ptr(t, ts, STKTABLE_DT_HTTP_ERR_RATE);
	}

	if (ptr1 || ptr2 || ptr3 || ptr4) {
		HA_RWLOCK_WRLOCK(STK_SESS_LOCK, &ts->lock);

		if (ptr1)
			stktable_data_cast(ptr1, http_req_cnt)++;
		if (ptr2)
			update_freq_ctr_period(&stktable_data_cast(ptr2, http_req_rate),
					       t->data_arg[STKTABLE_DT_HTTP_REQ_RATE].u, 1);
		if (ptr3)
			stktable_data_cast(ptr3, http_err_cnt)++;
		if (ptr4)
			update_freq_ctr_period(&stktable_data_cast(ptr4, http_err_rate),
					       t->data_arg[STKTABLE_DT_HTTP_ERR_RATE].u, 1);

		HA_RWLOCK_WRUNLOCK(STK_SESS_LOCK, &ts->lock);

		/* If data was modified, we need to touch to re-schedule sync */
		stktable_touch_local(t, ts, 0);
	}

	stkctr_set_flags(&s->stkctr[rule->action], STKCTR_TRACK_CONTENT);
	if (sess->fe != s->be)
		stkctr_set_flags(&s->stkctr[rule->action], STKCTR_TRACK_BACKEND);

  end:
	return ACT_RET_CONT;
}

static void release_http_track_sc(struct act_rule *rule)
{
	release_sample_expr(rule->arg.trk_ctr.expr);
}

/* Parse a "track-sc*" actions. It returns ACT_RET_PRS_OK on success,
 * ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_track_sc(const char **args, int *orig_arg, struct proxy *px,
						 struct act_rule *rule, char **err)
{
	struct sample_expr *expr;
	unsigned int where;
	unsigned int tsc_num;
	const char *tsc_num_str;
	int cur_arg;

	tsc_num_str = &args[*orig_arg-1][8];
	if (cfg_parse_track_sc_num(&tsc_num, tsc_num_str, tsc_num_str + strlen(tsc_num_str), err) == -1)
		return ACT_RET_PRS_ERR;

	cur_arg = *orig_arg;
	expr = sample_parse_expr((char **)args, &cur_arg, px->conf.args.file, px->conf.args.line,
				 err, &px->conf.args, NULL);
	if (!expr)
		return ACT_RET_PRS_ERR;

	where = 0;
	if (px->cap & PR_CAP_FE)
		where |= (rule->from == ACT_F_HTTP_REQ ? SMP_VAL_FE_HRQ_HDR : SMP_VAL_FE_HRS_HDR);
	if (px->cap & PR_CAP_BE)
		where |= (rule->from == ACT_F_HTTP_REQ ? SMP_VAL_BE_HRQ_HDR : SMP_VAL_BE_HRS_HDR);

	if (!(expr->fetch->val & where)) {
		memprintf(err, "fetch method '%s' extracts information from '%s', none of which is available here",
			  args[cur_arg-1], sample_src_names(expr->fetch->use));
		release_sample_expr(expr);
		return ACT_RET_PRS_ERR;
	}

	if (strcmp(args[cur_arg], "table") == 0) {
		cur_arg++;
		if (!*args[cur_arg]) {
			memprintf(err, "missing table name");
			release_sample_expr(expr);
			return ACT_RET_PRS_ERR;
		}

		/* we copy the table name for now, it will be resolved later */
		rule->arg.trk_ctr.table.n = strdup(args[cur_arg]);
		cur_arg++;
	}

	rule->action = tsc_num;
	rule->arg.trk_ctr.expr = expr;
	rule->action_ptr = http_action_track_sc;
	rule->release_ptr = release_http_track_sc;
	rule->check_ptr = check_trk_action;

	*orig_arg = cur_arg;
	return ACT_RET_PRS_OK;
}

/* This function executes a strict-mode actions. On success, it always returns
 * ACT_RET_CONT
 */
static enum act_return http_action_strict_mode(struct act_rule *rule, struct proxy *px,
					       struct session *sess, struct stream *s, int flags)
{
	struct http_msg *msg = ((rule->from == ACT_F_HTTP_REQ) ? &s->txn->req : &s->txn->rsp);

	if (rule->action == 0) // strict-mode on
		msg->flags &= ~HTTP_MSGF_SOFT_RW;
	else // strict-mode off
		msg->flags |= HTTP_MSGF_SOFT_RW;
	return ACT_RET_CONT;
}

/* Parse a "strict-mode" action. It returns ACT_RET_PRS_OK on success,
 * ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret parse_http_strict_mode(const char **args, int *orig_arg, struct proxy *px,
						 struct act_rule *rule, char **err)
{
	int cur_arg;

	cur_arg = *orig_arg;
	if (!*args[cur_arg]) {
		memprintf(err, "expects exactly 1 arguments");
		return ACT_RET_PRS_ERR;
	}

	if (strcasecmp(args[cur_arg], "on") == 0)
		rule->action = 0; // strict-mode on
	else if (strcasecmp(args[cur_arg], "off") == 0)
		rule->action = 1; // strict-mode off
	else {
		memprintf(err, "Unexpected value '%s'. Only 'on' and 'off' are supported", args[cur_arg]);
		return ACT_RET_PRS_ERR;
	}
	rule->action_ptr = http_action_strict_mode;

	*orig_arg = cur_arg + 1;
	return ACT_RET_PRS_OK;
}

/* This function executes a return action. It builds an HTX message from an
 * errorfile, an raw file or a log-format string, depending on <.action>
 * value. On success, it returns ACT_RET_ABRT. If an error occurs ACT_RET_ERR is
 * returned.
 */
static enum act_return http_action_return(struct act_rule *rule, struct proxy *px,
					  struct session *sess, struct stream *s, int flags)
{
	struct channel *req = &s->req;

	if (http_reply_message(s, rule->arg.http_reply) == -1)
		return ACT_RET_ERR;

	if (rule->from == ACT_F_HTTP_REQ) {
		/* let's log the request time */
		s->logs.tv_request = now;
		req->analysers &= AN_REQ_FLT_END;

		if (s->sess->fe == s->be) /* report it if the request was intercepted by the frontend */
			_HA_ATOMIC_ADD(&s->sess->fe->fe_counters.intercepted_req, 1);
	}

	if (!(s->flags & SF_ERR_MASK))
		s->flags |= SF_ERR_LOCAL;
	if (!(s->flags & SF_FINST_MASK))
		s->flags |= ((rule->from == ACT_F_HTTP_REQ) ? SF_FINST_R : SF_FINST_H);

	return ACT_RET_ABRT;
}

/* Parse a "return" action. It returns ACT_RET_PRS_OK on success,
 * ACT_RET_PRS_ERR on error. It relies on http_parse_http_reply() to set
 * <.arg.http_reply>.
 */
static enum act_parse_ret parse_http_return(const char **args, int *orig_arg, struct proxy *px,
					    struct act_rule *rule, char **err)
{
	/* Prepare parsing of log-format strings */
	px->conf.args.ctx = ((rule->from == ACT_F_HTTP_REQ) ? ARGC_HRQ : ARGC_HRS);
	rule->arg.http_reply = http_parse_http_reply(args, orig_arg, px, 200, err);
	if (!rule->arg.http_reply)
		return ACT_RET_PRS_ERR;

	rule->flags |= ACT_FLAG_FINAL;
	rule->action = ACT_CUSTOM;
	rule->check_ptr = check_act_http_reply;
	rule->action_ptr = http_action_return;
	rule->release_ptr = release_act_http_reply;
	return ACT_RET_PRS_OK;
}

/************************************************************************/
/*   All supported http-request action keywords must be declared here.  */
/************************************************************************/

static struct action_kw_list http_req_actions = {
	.kw = {
		{ "add-acl",          parse_http_set_map,              1 },
		{ "add-header",       parse_http_set_header,           0 },
		{ "allow",            parse_http_allow,                0 },
		{ "auth",             parse_http_auth,                 0 },
		{ "capture",          parse_http_req_capture,          0 },
		{ "del-acl",          parse_http_set_map,              1 },
		{ "del-header",       parse_http_del_header,           0 },
		{ "del-map",          parse_http_set_map,              1 },
		{ "deny",             parse_http_deny,                 0 },
		{ "disable-l7-retry", parse_http_req_disable_l7_retry, 0 },
		{ "early-hint",       parse_http_set_header,           0 },
		{ "redirect",         parse_http_redirect,             0 },
		{ "reject",           parse_http_action_reject,        0 },
		{ "replace-header",   parse_http_replace_header,       0 },
		{ "replace-path",     parse_replace_uri,               0 },
		{ "replace-uri",      parse_replace_uri,               0 },
		{ "replace-value",    parse_http_replace_header,       0 },
		{ "return",           parse_http_return,               0 },
		{ "set-header",       parse_http_set_header,           0 },
		{ "set-log-level",    parse_http_set_log_level,        0 },
		{ "set-map",          parse_http_set_map,              1 },
		{ "set-method",       parse_set_req_line,              0 },
		{ "set-mark",         parse_http_set_mark,             0 },
		{ "set-nice",         parse_http_set_nice,             0 },
		{ "set-path",         parse_set_req_line,              0 },
		{ "set-query",        parse_set_req_line,              0 },
		{ "set-tos",          parse_http_set_tos,              0 },
		{ "set-uri",          parse_set_req_line,              0 },
		{ "strict-mode",      parse_http_strict_mode,          0 },
		{ "tarpit",           parse_http_deny,                 0 },
		{ "track-sc",         parse_http_track_sc,             1 },
		{ NULL, NULL }
	}
};

INITCALL1(STG_REGISTER, http_req_keywords_register, &http_req_actions);

static struct action_kw_list http_res_actions = {
	.kw = {
		{ "add-acl",         parse_http_set_map,        1 },
		{ "add-header",      parse_http_set_header,     0 },
		{ "allow",           parse_http_allow,          0 },
		{ "capture",         parse_http_res_capture,    0 },
		{ "del-acl",         parse_http_set_map,        1 },
		{ "del-header",      parse_http_del_header,     0 },
		{ "del-map",         parse_http_set_map,        1 },
		{ "deny",            parse_http_deny,           0 },
		{ "redirect",        parse_http_redirect,       0 },
		{ "replace-header",  parse_http_replace_header, 0 },
		{ "replace-value",   parse_http_replace_header, 0 },
		{ "return",          parse_http_return,         0 },
		{ "set-header",      parse_http_set_header,     0 },
		{ "set-log-level",   parse_http_set_log_level,  0 },
		{ "set-map",         parse_http_set_map,        1 },
		{ "set-mark",        parse_http_set_mark,       0 },
		{ "set-nice",        parse_http_set_nice,       0 },
		{ "set-status",      parse_http_set_status,     0 },
		{ "set-tos",         parse_http_set_tos,        0 },
		{ "strict-mode",     parse_http_strict_mode,    0 },
		{ "track-sc",        parse_http_track_sc,       1 },
		{ NULL, NULL }
	}
};

INITCALL1(STG_REGISTER, http_res_keywords_register, &http_res_actions);

static struct action_kw_list http_after_res_actions = {
	.kw = {
		{ "add-header",      parse_http_set_header,     0 },
		{ "allow",           parse_http_allow,          0 },
		{ "del-header",      parse_http_del_header,     0 },
		{ "replace-header",  parse_http_replace_header, 0 },
		{ "replace-value",   parse_http_replace_header, 0 },
		{ "set-header",      parse_http_set_header,     0 },
		{ "set-status",      parse_http_set_status,     0 },
		{ "strict-mode",     parse_http_strict_mode,    0 },
		{ NULL, NULL }
	}
};

INITCALL1(STG_REGISTER, http_after_res_keywords_register, &http_after_res_actions);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
