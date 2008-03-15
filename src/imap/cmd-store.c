/* Copyright (c) 2002-2008 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "seq-range-array.h"
#include "str.h"
#include "commands.h"
#include "imap-search.h"
#include "imap-util.h"

#include <stdlib.h>

struct imap_store_context {
	struct client_command_context *cmd;
	const char *messageset;
	uint64_t max_modseq;

	enum mail_flags flags;
	struct mail_keywords *keywords;

	enum modify_type modify_type;
	bool silent;
};

static bool
get_modify_type(struct imap_store_context *ctx, const char *type)
{
	if (*type == '+') {
		ctx->modify_type = MODIFY_ADD;
		type++;
	} else if (*type == '-') {
		ctx->modify_type = MODIFY_REMOVE;
		type++;
	} else {
		ctx->modify_type = MODIFY_REPLACE;
	}

	if (strncasecmp(type, "FLAGS", 5) != 0)
		return FALSE;

	ctx->silent = strcasecmp(type+5, ".SILENT") == 0;
	if (!ctx->silent && type[5] != '\0')
		return FALSE;
	return TRUE;
}

static bool
store_parse_modifiers(struct imap_store_context *ctx,
		      const struct imap_arg *args)
{
	const char *name;

	for (; args->type != IMAP_ARG_EOL; args++) {
		if (args->type != IMAP_ARG_ATOM ||
		    args[1].type != IMAP_ARG_ATOM) {
			client_send_command_error(ctx->cmd,
				"STORE modifiers contain non-atoms.");
			return FALSE;
		}
		name = IMAP_ARG_STR(args);
		if (strcasecmp(name, "UNCHANGEDSINCE") == 0) {
			args++;
			ctx->max_modseq =
				strtoull(imap_arg_string(args), NULL, 10);
			client_enable(ctx->cmd->client,
				      MAILBOX_FEATURE_CONDSTORE);
		} else {
			client_send_command_error(ctx->cmd,
						  "Unknown STORE modifier");
			return FALSE;
		}
	}
	return TRUE;
}

static bool
store_parse_args(struct imap_store_context *ctx, const struct imap_arg *args)
{
	struct client_command_context *cmd = ctx->cmd;
	const char *type;
	const char *const *keywords_list = NULL;

	ctx->max_modseq = (uint64_t)-1;
	ctx->messageset = imap_arg_string(args++);

	if (args->type == IMAP_ARG_LIST) {
		if (!store_parse_modifiers(ctx, IMAP_ARG_LIST_ARGS(args)))
			return FALSE;
		args++;
	}

	type = imap_arg_string(args++);
	if (ctx->messageset == NULL || type == NULL ||
	    !get_modify_type(ctx, type)) {
		client_send_command_error(cmd, "Invalid arguments.");
		return FALSE;
	}

	if (args->type == IMAP_ARG_LIST) {
		if (!client_parse_mail_flags(cmd, IMAP_ARG_LIST_ARGS(args),
					     &ctx->flags, &keywords_list))
			return FALSE;
	} else {
		if (!client_parse_mail_flags(cmd, args,
					     &ctx->flags, &keywords_list))
			return FALSE;
	}

	if (keywords_list != NULL || ctx->modify_type == MODIFY_REPLACE) {
		if (mailbox_keywords_create(cmd->client->mailbox, keywords_list,
					    &ctx->keywords) < 0) {
			/* invalid keywords */
			client_send_storage_error(cmd,
				mailbox_get_storage(cmd->client->mailbox));
			return FALSE;
		}
	}
	return TRUE;
}

bool cmd_store(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	const struct imap_arg *args;
	struct mail_search_arg *search_arg;
	struct mail_search_context *search_ctx;
        struct mailbox_transaction_context *t;
	struct mail *mail;
	struct imap_store_context ctx;
	ARRAY_TYPE(seq_range) modified_set = ARRAY_INIT;
	const char *tagged_reply;
	string_t *str;
	bool failed;

	if (!client_read_args(cmd, 0, 0, &args))
		return FALSE;

	if (!client_verify_open_mailbox(cmd))
		return TRUE;

	memset(&ctx, 0, sizeof(ctx));
	ctx.cmd = cmd;
	if (!store_parse_args(&ctx, args))
		return TRUE;

	search_arg = imap_search_get_seqset(cmd, ctx.messageset, cmd->uid);
	if (search_arg == NULL)
		return TRUE;

	t = mailbox_transaction_begin(client->mailbox, !ctx.silent ? 0 :
				      MAILBOX_TRANSACTION_FLAG_HIDE);
	search_ctx = mailbox_search_init(t, NULL, search_arg, NULL);

	/* FIXME: UNCHANGEDSINCE should be atomic, but this requires support
	   from mail-storage API. So for now we fake it. */
	mail = mail_alloc(t, MAIL_FETCH_FLAGS, NULL);
	while (mailbox_search_next(search_ctx, mail) > 0) {
		if (ctx.max_modseq < (uint64_t)-1) {
			if (mail_get_modseq(mail) > ctx.max_modseq) {
				seq_range_array_add(&modified_set, 64,
					cmd->uid ? mail->uid : mail->seq);
				continue;
			}
		}
		if (ctx.modify_type == MODIFY_REPLACE || ctx.flags != 0)
			mail_update_flags(mail, ctx.modify_type, ctx.flags);
		if (ctx.modify_type == MODIFY_REPLACE || ctx.keywords != NULL) {
			mail_update_keywords(mail, ctx.modify_type,
					     ctx.keywords);
		}
	}
	mail_free(&mail);

	if (!array_is_created(&modified_set))
		tagged_reply = "OK Store completed.";
	else {
		str = str_new(cmd->pool, 256);
		str_append(str, "OK [MODIFIED ");
		imap_write_seq_range(str, &modified_set);
		array_free(&modified_set);
		str_append(str, "] Conditional store failed.");
		tagged_reply = str_c(str);
	}

	if (ctx.keywords != NULL)
		mailbox_keywords_free(client->mailbox, &ctx.keywords);

	if (mailbox_search_deinit(&search_ctx) < 0) {
		failed = TRUE;
		mailbox_transaction_rollback(&t);
	} else {
		failed = mailbox_transaction_commit(&t) < 0;
	}

	if (!failed) {
		/* With UID STORE we have to return UID for the flags as well.
		   Unfortunately we don't have the ability to separate those
		   flag changes that were caused by UID STORE and those that
		   came externally, so we'll just send the UID for all flag
		   changes that we see. */
		enum imap_sync_flags imap_sync_flags = 0;

		if (cmd->uid &&
		    (!ctx.silent || (client->enabled_features &
				     MAILBOX_FEATURE_CONDSTORE) != 0))
			imap_sync_flags |= IMAP_SYNC_FLAG_SEND_UID;

		return cmd_sync(cmd,
				(cmd->uid ? 0 : MAILBOX_SYNC_FLAG_NO_EXPUNGES),
				imap_sync_flags, tagged_reply);
	} else {
		client_send_storage_error(cmd,
			mailbox_get_storage(client->mailbox));
		return TRUE;
	}
}
