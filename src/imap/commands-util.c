/* Copyright (c) 2002-2008 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "array.h"
#include "buffer.h"
#include "str.h"
#include "str-sanitize.h"
#include "mail-storage.h"
#include "commands-util.h"
#include "imap-parser.h"
#include "imap-sync.h"
#include "imap-util.h"
#include "mail-namespace.h"

/* Maximum length for mailbox name, including it's path. This isn't fully
   exact since the user can create folder hierarchy with small names, then
   rename them to larger names. Mail storages should set more strict limits
   to them, mbox/maildir currently allow paths only up to PATH_MAX. */
#define MAILBOX_MAX_NAME_LEN 512

struct mail_namespace *
client_find_namespace(struct client_command_context *cmd, const char **mailbox)
{
	struct mail_namespace *ns;

	ns = mail_namespace_find(cmd->client->user->namespaces, mailbox);
	if (ns != NULL)
		return ns;

	client_send_tagline(cmd, "NO Unknown namespace.");
	return NULL;
}

struct mail_storage *
client_find_storage(struct client_command_context *cmd, const char **mailbox)
{
	struct mail_namespace *ns;

	ns = client_find_namespace(cmd, mailbox);
	return ns == NULL ? NULL : ns->storage;
}

bool client_verify_mailbox_name(struct client_command_context *cmd,
				const char *mailbox,
				bool should_exist, bool should_not_exist)
{
	struct mail_namespace *ns;
	struct mailbox_list *list;
	enum mailbox_name_status mailbox_status;
	const char *orig_mailbox, *p;

	orig_mailbox = mailbox;
	ns = client_find_namespace(cmd, &mailbox);
	if (ns == NULL)
		return FALSE;

	/* make sure it even looks valid */
	if (*mailbox == '\0') {
		client_send_tagline(cmd, "NO Empty mailbox name.");
		return FALSE;
	}

	if (ns->real_sep != ns->sep && ns->prefix_len < strlen(orig_mailbox)) {
		/* make sure there are no real separators used in the mailbox
		   name. */
		orig_mailbox += ns->prefix_len;
		for (p = orig_mailbox; *p != '\0'; p++) {
			if (*p == ns->real_sep) {
				client_send_tagline(cmd, t_strdup_printf(
					"NO Character not allowed "
					"in mailbox name: '%c'",
					ns->real_sep));
				return FALSE;
			}
		}
	}

	/* make sure two hierarchy separators aren't next to each others */
	for (p = mailbox+1; *p != '\0'; p++) {
		if (p[0] == ns->real_sep && p[-1] == ns->real_sep) {
			client_send_tagline(cmd, "NO Invalid mailbox name.");
			return FALSE;
		}
	}

	if (strlen(mailbox) > MAILBOX_MAX_NAME_LEN) {
		client_send_tagline(cmd, "NO Mailbox name too long.");
		return FALSE;
	}

	/* check what our storage thinks of it */
	list = mail_storage_get_list(ns->storage);
	if (mailbox_list_get_mailbox_name_status(list, mailbox,
						 &mailbox_status) < 0) {
		client_send_storage_error(cmd, ns->storage);
		return FALSE;
	}

	switch (mailbox_status) {
	case MAILBOX_NAME_EXISTS:
		if (should_exist || !should_not_exist)
			return TRUE;

		client_send_tagline(cmd, "NO Mailbox exists.");
		break;

	case MAILBOX_NAME_VALID:
		if (!should_exist)
			return TRUE;

		client_send_tagline(cmd, t_strconcat(
			"NO [TRYCREATE] Mailbox doesn't exist: ",
			str_sanitize(orig_mailbox, MAILBOX_MAX_NAME_LEN),
			NULL));
		break;

	case MAILBOX_NAME_INVALID:
		client_send_tagline(cmd, t_strconcat(
			"NO Invalid mailbox name: ",
			str_sanitize(orig_mailbox, MAILBOX_MAX_NAME_LEN),
			NULL));
		break;

	case MAILBOX_NAME_NOINFERIORS:
		client_send_tagline(cmd,
			"NO Mailbox parent doesn't allow inferior mailboxes.");
		break;

	default:
                i_unreached();
	}

	return FALSE;
}

bool client_verify_open_mailbox(struct client_command_context *cmd)
{
	if (cmd->client->mailbox != NULL)
		return TRUE;
	else {
		client_send_tagline(cmd, "BAD No mailbox selected.");
		return FALSE;
	}
}

void client_send_list_error(struct client_command_context *cmd,
			    struct mailbox_list *list)
{
	const char *error_string;
	enum mail_error error;

	error_string = mailbox_list_get_last_error(list, &error);
	client_send_tagline(cmd, t_strconcat("NO ", error_string, NULL));
}

void client_send_storage_error(struct client_command_context *cmd,
			       struct mail_storage *storage)
{
	const char *error_string;
	enum mail_error error;

	if (cmd->client->mailbox != NULL &&
	    mailbox_is_inconsistent(cmd->client->mailbox)) {
		/* we can't do forced CLOSE, so have to disconnect */
		client_disconnect_with_error(cmd->client,
			"IMAP session state is inconsistent, please relogin.");
		return;
	}

	error_string = mail_storage_get_last_error(storage, &error);
	client_send_tagline(cmd, t_strconcat("NO ", error_string, NULL));
}

void client_send_untagged_storage_error(struct client *client,
					struct mail_storage *storage)
{
	const char *error_string;
	enum mail_error error;

	if (client->mailbox != NULL &&
	    mailbox_is_inconsistent(client->mailbox)) {
		/* we can't do forced CLOSE, so have to disconnect */
		client_disconnect_with_error(client,
			"IMAP session state is inconsistent, please relogin.");
		return;
	}

	error_string = mail_storage_get_last_error(storage, &error);
	client_send_line(client, t_strconcat("* NO ", error_string, NULL));
}

bool client_parse_mail_flags(struct client_command_context *cmd,
			     const struct imap_arg *args,
			     enum mail_flags *flags_r,
			     const char *const **keywords_r)
{
	const char *atom;
	ARRAY_DEFINE(keywords, const char *);

	*flags_r = 0;
	*keywords_r = NULL;
	p_array_init(&keywords, cmd->pool, 16);

	while (args->type != IMAP_ARG_EOL) {
		if (args->type != IMAP_ARG_ATOM) {
			client_send_command_error(cmd,
				"Flags list contains non-atoms.");
			return FALSE;
		}

		atom = IMAP_ARG_STR(args);
		if (*atom == '\\') {
			/* system flag */
			atom = t_str_ucase(atom);
			if (strcmp(atom, "\\ANSWERED") == 0)
				*flags_r |= MAIL_ANSWERED;
			else if (strcmp(atom, "\\FLAGGED") == 0)
				*flags_r |= MAIL_FLAGGED;
			else if (strcmp(atom, "\\DELETED") == 0)
				*flags_r |= MAIL_DELETED;
			else if (strcmp(atom, "\\SEEN") == 0)
				*flags_r |= MAIL_SEEN;
			else if (strcmp(atom, "\\DRAFT") == 0)
				*flags_r |= MAIL_DRAFT;
			else {
				client_send_tagline(cmd, t_strconcat(
					"BAD Invalid system flag ",
					atom, NULL));
				return FALSE;
			}
		} else {
			/* keyword validity checks are done by lib-storage */
			array_append(&keywords, &atom, 1);
		}

		args++;
	}

	if (array_count(&keywords) == 0)
		*keywords_r = NULL;
	else {
		(void)array_append_space(&keywords); /* NULL-terminate */
		*keywords_r = array_idx(&keywords, 0);
	}
	return TRUE;
}

static const char *get_keywords_string(const ARRAY_TYPE(keywords) *keywords)
{
	string_t *str;
	const char *const *names;
	unsigned int i, count;

	str = t_str_new(256);
	names = array_get(keywords, &count);
	for (i = 0; i < count; i++) {
		str_append_c(str, ' ');
		str_append(str, names[i]);
	}
	return str_c(str);
}

#define SYSTEM_FLAGS "\\Answered \\Flagged \\Deleted \\Seen \\Draft"

void client_send_mailbox_flags(struct client *client, bool selecting)
{
	unsigned int count = array_count(client->keywords.names);
	const char *str;

	if (!selecting && count == client->keywords.announce_count) {
		/* no changes to keywords and we're not selecting a mailbox */
		return;
	}

	client->keywords.announce_count = count;
	str = count == 0 ? "" : get_keywords_string(client->keywords.names);
	client_send_line(client,
		t_strconcat("* FLAGS ("SYSTEM_FLAGS, str, ")", NULL));

	if (mailbox_is_readonly(client->mailbox)) {
		client_send_line(client, "* OK [PERMANENTFLAGS ()] "
				 "Read-only mailbox.");
	} else {
		bool star = mailbox_allow_new_keywords(client->mailbox);

		client_send_line(client,
			t_strconcat("* OK [PERMANENTFLAGS ("SYSTEM_FLAGS, str,
				    star ? " \\*" : "",
				    ")] Flags permitted.", NULL));
	}
}

void client_update_mailbox_flags(struct client *client,
				 const ARRAY_TYPE(keywords) *keywords)
{
	client->keywords.names = keywords;
	client->keywords.announce_count = 0;
}

const char *const *
client_get_keyword_names(struct client *client, ARRAY_TYPE(keywords) *dest,
			 const ARRAY_TYPE(keyword_indexes) *src)
{
	const unsigned int *kw_indexes;
	const char *const *all_names;
	unsigned int i, kw_count, all_count;

	client_send_mailbox_flags(client, FALSE);

	all_names = array_get(client->keywords.names, &all_count);
	kw_indexes = array_get(src, &kw_count);

	/* convert indexes to names */
	array_clear(dest);
	for (i = 0; i < kw_count; i++) {
		i_assert(kw_indexes[i] < all_count);
		array_append(dest, &all_names[kw_indexes[i]], 1);
	}

	(void)array_append_space(dest);
	return array_idx(dest, 0);
}

bool mailbox_equals(const struct mailbox *box1,
		    const struct mail_storage *storage2, const char *name2)
{
	struct mail_storage *storage1 = mailbox_get_storage(box1);
	const char *name1;

	if (storage1 != storage2)
		return FALSE;

        name1 = mailbox_get_name(box1);
	if (strcmp(name1, name2) == 0)
		return TRUE;

	return strcasecmp(name1, "INBOX") == 0 &&
		strcasecmp(name2, "INBOX") == 0;
}

void msgset_generator_init(struct msgset_generator_context *ctx, string_t *str)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->str = str;
	ctx->last_uid = (uint32_t)-1;
}

void msgset_generator_next(struct msgset_generator_context *ctx, uint32_t uid)
{
	if (uid != ctx->last_uid+1) {
		if (ctx->first_uid == 0)
			;
		else if (ctx->first_uid == ctx->last_uid)
			str_printfa(ctx->str, "%u,", ctx->first_uid);
		else {
			str_printfa(ctx->str, "%u:%u,",
				    ctx->first_uid, ctx->last_uid);
		}
		ctx->first_uid = uid;
	}
	ctx->last_uid = uid;
}

void msgset_generator_finish(struct msgset_generator_context *ctx)
{
	if (ctx->first_uid == ctx->last_uid)
		str_printfa(ctx->str, "%u", ctx->first_uid);
	else
		str_printfa(ctx->str, "%u:%u", ctx->first_uid, ctx->last_uid);
}
