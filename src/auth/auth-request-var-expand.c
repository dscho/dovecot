/* Copyright (c) 2002-2015 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "str.h"
#include "strescape.h"
#include "auth-request.h"

struct auth_request_var_expand_ctx {
	struct auth_request *auth_request;
	auth_request_escape_func_t *escape_func;
};

const struct var_expand_table
auth_request_var_expand_static_tab[AUTH_REQUEST_VAR_TAB_COUNT+1] = {
	{ 'u', NULL, "user" },
	{ 'n', NULL, "username" },
	{ 'd', NULL, "domain" },
	{ 's', NULL, "service" },
	{ 'h', NULL, "home" },
	{ 'l', NULL, "lip" },
	{ 'r', NULL, "rip" },
	{ 'p', NULL, "pid" },
	{ 'w', NULL, "password" },
	{ '!', NULL, NULL },
	{ 'm', NULL, "mech" },
	{ 'c', NULL, "secured" },
	{ 'a', NULL, "lport" },
	{ 'b', NULL, "rport" },
	{ 'k', NULL, "cert" },
	{ '\0', NULL, "login_user" },
	{ '\0', NULL, "login_username" },
	{ '\0', NULL, "login_domain" },
	{ '\0', NULL, "session" },
	{ '\0', NULL, "real_lip" },
	{ '\0', NULL, "real_rip" },
	{ '\0', NULL, "real_lport" },
	{ '\0', NULL, "real_rport" },
	{ '\0', NULL, "domain_first" },
	{ '\0', NULL, "domain_last" },
	{ '\0', NULL, "master_user" },
	{ '\0', NULL, "session_pid" },
	{ '\0', NULL, "orig_user" },
	{ '\0', NULL, "orig_username" },
	{ '\0', NULL, "orig_domain" },
	/* be sure to update AUTH_REQUEST_VAR_TAB_COUNT */
	{ '\0', NULL, NULL }
};

static const char *
escape_none(const char *string,
	    const struct auth_request *request ATTR_UNUSED)
{
	return string;
}

const char *
auth_request_str_escape(const char *string,
			const struct auth_request *request ATTR_UNUSED)
{
	return str_escape(string);
}

struct var_expand_table *
auth_request_get_var_expand_table_full(const struct auth_request *auth_request,
				       auth_request_escape_func_t *escape_func,
				       unsigned int *count)
{
	const unsigned int auth_count =
		N_ELEMENTS(auth_request_var_expand_static_tab);
	struct var_expand_table *tab, *ret_tab;
	const char *orig_user;

	if (escape_func == NULL)
		escape_func = escape_none;

	/* keep the extra fields at the beginning. the last static_tab field
	   contains the ending NULL-fields. */
	tab = ret_tab = t_malloc((*count + auth_count) * sizeof(*tab));
	memset(tab, 0, *count * sizeof(*tab));
	tab += *count;
	*count += auth_count;

	memcpy(tab, auth_request_var_expand_static_tab,
	       auth_count * sizeof(*tab));

	tab[0].value = escape_func(auth_request->user, auth_request);
	tab[1].value = escape_func(t_strcut(auth_request->user, '@'),
				   auth_request);
	tab[2].value = strchr(auth_request->user, '@');
	if (tab[2].value != NULL)
		tab[2].value = escape_func(tab[2].value+1, auth_request);
	tab[3].value = escape_func(auth_request->service, auth_request);
	/* tab[4] = we have no home dir */
	if (auth_request->local_ip.family != 0)
		tab[5].value = net_ip2addr(&auth_request->local_ip);
	if (auth_request->remote_ip.family != 0)
		tab[6].value = net_ip2addr(&auth_request->remote_ip);
	tab[7].value = dec2str(auth_request->client_pid);
	if (auth_request->mech_password != NULL) {
		tab[8].value = escape_func(auth_request->mech_password,
					   auth_request);
	}
	if (auth_request->userdb_lookup) {
		tab[9].value = auth_request->userdb == NULL ? "" :
			dec2str(auth_request->userdb->userdb->id);
	} else {
		tab[9].value = auth_request->passdb == NULL ? "" :
			dec2str(auth_request->passdb->passdb->id);
	}
	tab[10].value = auth_request->mech_name == NULL ? "" :
		escape_func(auth_request->mech_name, auth_request);
	tab[11].value = auth_request->secured ? "secured" : "";
	tab[12].value = dec2str(auth_request->local_port);
	tab[13].value = dec2str(auth_request->remote_port);
	tab[14].value = auth_request->valid_client_cert ? "valid" : "";

	if (auth_request->requested_login_user != NULL) {
		const char *login_user = auth_request->requested_login_user;

		tab[15].value = escape_func(login_user, auth_request);
		tab[16].value = escape_func(t_strcut(login_user, '@'),
					    auth_request);
		tab[17].value = strchr(login_user, '@');
		if (tab[17].value != NULL) {
			tab[17].value = escape_func(tab[17].value+1,
						    auth_request);
		}
	}
	tab[18].value = auth_request->session_id == NULL ? NULL :
		escape_func(auth_request->session_id, auth_request);
	if (auth_request->real_local_ip.family != 0)
		tab[19].value = net_ip2addr(&auth_request->real_local_ip);
	if (auth_request->real_remote_ip.family != 0)
		tab[20].value = net_ip2addr(&auth_request->real_remote_ip);
	tab[21].value = dec2str(auth_request->real_local_port);
	tab[22].value = dec2str(auth_request->real_remote_port);
	tab[23].value = strchr(auth_request->user, '@');
	if (tab[23].value != NULL) {
		tab[23].value = escape_func(t_strcut(tab[23].value+1, '@'),
					    auth_request);
	}
	tab[24].value = strrchr(auth_request->user, '@');
	if (tab[24].value != NULL)
		tab[24].value = escape_func(tab[24].value+1, auth_request);
	tab[25].value = auth_request->master_user == NULL ? NULL :
		escape_func(auth_request->master_user, auth_request);
	tab[26].value = auth_request->session_pid == (pid_t)-1 ? NULL :
		dec2str(auth_request->session_pid);

	orig_user = auth_request->original_username != NULL ?
		auth_request->original_username : auth_request->user;
	tab[27].value = escape_func(orig_user, auth_request);
	tab[28].value = escape_func(t_strcut(orig_user, '@'), auth_request);
	tab[29].value = strchr(orig_user, '@');
	if (tab[29].value != NULL)
		tab[29].value = escape_func(tab[29].value+1, auth_request);
	return ret_tab;
}

const struct var_expand_table *
auth_request_get_var_expand_table(const struct auth_request *auth_request,
				  auth_request_escape_func_t *escape_func)
{
	unsigned int count = 0;

	return auth_request_get_var_expand_table_full(auth_request, escape_func,
						      &count);
}

static const char *field_get_default(const char *data)
{
	const char *p;

	p = strchr(data, ':');
	if (p == NULL)
		return "";
	else {
		/* default value given */
		return p+1;
	}
}

static const char *
auth_request_var_expand_func_passdb(const char *data, void *context)
{
	struct auth_request_var_expand_ctx *ctx = context;
	const char *field_name = t_strcut(data, ':');
	const char *value;

	value = auth_fields_find(ctx->auth_request->extra_fields, field_name);
	return ctx->escape_func(value != NULL ? value : field_get_default(data),
				ctx->auth_request);
}

static const char *
auth_request_var_expand_func_userdb(const char *data, void *context)
{
	struct auth_request_var_expand_ctx *ctx = context;
	const char *field_name = t_strcut(data, ':');
	const char *value;

	value = ctx->auth_request->userdb_reply == NULL ? NULL :
		auth_fields_find(ctx->auth_request->userdb_reply, field_name);
	return ctx->escape_func(value != NULL ? value : field_get_default(data),
				ctx->auth_request);
}

const struct var_expand_func_table auth_request_var_funcs_table[] = {
	{ "passdb", auth_request_var_expand_func_passdb },
	{ "userdb", auth_request_var_expand_func_userdb },
	{ NULL, NULL }
};

void auth_request_var_expand(string_t *dest, const char *str,
			     struct auth_request *auth_request,
			     auth_request_escape_func_t *escape_func)
{
	auth_request_var_expand_with_table(dest, str, auth_request,
		auth_request_get_var_expand_table(auth_request, escape_func),
		escape_func);
}

void auth_request_var_expand_with_table(string_t *dest, const char *str,
					struct auth_request *auth_request,
					const struct var_expand_table *table,
					auth_request_escape_func_t *escape_func)
{
	struct auth_request_var_expand_ctx ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.auth_request = auth_request;
	ctx.escape_func = escape_func == NULL ? escape_none : escape_func;
	var_expand_with_funcs(dest, str, table,
			      auth_request_var_funcs_table, &ctx);
}

const char *
t_auth_request_var_expand(const char *str,
			  struct auth_request *auth_request,
			  auth_request_escape_func_t *escape_func)
{
	string_t *dest = t_str_new(128);
	auth_request_var_expand(dest, str, auth_request, escape_func);
	return str_c(dest);
}
