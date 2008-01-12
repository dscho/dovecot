/* Copyright (c) 2002-2008 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "str.h"
#include "commands.h"
#include "imap-parser.h"
#include "imap-date.h"
#include "mail-storage.h"

#include <sys/time.h>

struct cmd_append_context {
	struct client *client;
        struct client_command_context *cmd;
	struct mail_storage *storage;
	struct mailbox *box;
        struct mailbox_transaction_context *t;

	struct istream *input;
	uoff_t msg_size;

	struct imap_parser *save_parser;
	struct mail_save_context *save_ctx;
	unsigned int count;

	unsigned int message_input:1;
	unsigned int failed:1;
};

static void cmd_append_finish(struct cmd_append_context *ctx);
static bool cmd_append_continue_message(struct client_command_context *cmd);
static bool cmd_append_continue_parsing(struct client_command_context *cmd);

static void client_input_append(struct client_command_context *cmd)
{
	struct cmd_append_context *ctx = cmd->context;
	struct client *client = cmd->client;
	bool finished;

	i_assert(!client->destroyed);

	client->last_input = ioloop_time;
	timeout_reset(client->to_idle);

	switch (i_stream_read(client->input)) {
	case -1:
		/* disconnected */
		cmd_append_finish(cmd->context);
		/* Reset command so that client_destroy() doesn't try to call
		   cmd_append_continue_message() anymore. */
		client_command_free(cmd);
		client_destroy(client, "Disconnected in APPEND");
		return;
	case -2:
		if (ctx->message_input) {
			/* message data, this is handled internally by
			   mailbox_save_continue() */
			break;
		}
		cmd_append_finish(cmd->context);

		/* parameter word is longer than max. input buffer size.
		   this is most likely an error, so skip the new data
		   until newline is found. */
		client->input_skip_line = TRUE;

		client_send_command_error(cmd, "Too long argument.");
		cmd->param_error = TRUE;
		client_command_free(cmd);
		return;
	}

	o_stream_cork(client->output);
	finished = cmd->func(cmd);
	o_stream_uncork(client->output);
	if (!finished && cmd->state != CLIENT_COMMAND_STATE_DONE)
		(void)client_handle_unfinished_cmd(cmd);
	else {
		client_command_free(cmd);
		client_continue_pending_input(&client);
	}
}

/* Returns -1 = error, 0 = need more data, 1 = successful. flags and
   internal_date may be NULL as a result, but mailbox and msg_size are always
   set when successful. */
static int validate_args(const struct imap_arg *args,
			 const struct imap_arg **flags_r,
			 const char **internal_date_r, uoff_t *msg_size_r,
			 bool *nonsync_r)
{
	/* [<flags>] */
	if (args->type != IMAP_ARG_LIST)
		*flags_r = NULL;
	else {
		*flags_r = IMAP_ARG_LIST_ARGS(args);
		args++;
	}

	/* [<internal date>] */
	if (args->type != IMAP_ARG_STRING)
		*internal_date_r = NULL;
	else {
		*internal_date_r = IMAP_ARG_STR(args);
		args++;
	}

	if (args->type != IMAP_ARG_LITERAL_SIZE &&
	    args->type != IMAP_ARG_LITERAL_SIZE_NONSYNC) {
		*nonsync_r = FALSE;
		return FALSE;
	}

	*nonsync_r = args->type == IMAP_ARG_LITERAL_SIZE_NONSYNC;
	*msg_size_r = IMAP_ARG_LITERAL_SIZE(args);
	return TRUE;
}

static void cmd_append_finish(struct cmd_append_context *ctx)
{
	imap_parser_destroy(&ctx->save_parser);

	i_assert(ctx->client->input_lock == ctx->cmd);

	io_remove(&ctx->client->io);
	/* we must put back the original flush callback before beginning to
	   sync (the command is still unfinished at that point) */
	o_stream_set_flush_callback(ctx->client->output,
				    client_output, ctx->client);

	if (ctx->input != NULL)
		i_stream_unref(&ctx->input);
	if (ctx->save_ctx != NULL)
		mailbox_save_cancel(&ctx->save_ctx);
	if (ctx->t != NULL)
		mailbox_transaction_rollback(&ctx->t);
	if (ctx->box != ctx->cmd->client->mailbox && ctx->box != NULL)
		mailbox_close(&ctx->box);
}

static bool cmd_append_continue_cancel(struct client_command_context *cmd)
{
	struct cmd_append_context *ctx = cmd->context;
	size_t size;

	if (cmd->cancel) {
		cmd_append_finish(ctx);
		return TRUE;
	}

	(void)i_stream_read(ctx->input);
	(void)i_stream_get_data(ctx->input, &size);
	i_stream_skip(ctx->input, size);

	if (cmd->client->input->closed) {
		cmd_append_finish(ctx);
		return TRUE;
	}

	if (ctx->input->v_offset == ctx->msg_size) {
		/* finished, but with MULTIAPPEND and LITERAL+ we may get
		   more messages. */
		i_stream_unref(&ctx->input);
		ctx->input = NULL;

		ctx->message_input = FALSE;
		imap_parser_reset(ctx->save_parser);
		cmd->func = cmd_append_continue_parsing;
		return cmd_append_continue_parsing(cmd);
	}

	return FALSE;
}

static bool cmd_append_cancel(struct cmd_append_context *ctx, bool nonsync)
{
	ctx->failed = TRUE;

	if (!nonsync) {
		cmd_append_finish(ctx);
		return TRUE;
	}

	/* we have to read the nonsynced literal so we don't treat the message
	   data as commands. */
	ctx->input = i_stream_create_limit(ctx->client->input, ctx->msg_size);

	ctx->message_input = TRUE;
	ctx->cmd->func = cmd_append_continue_cancel;
	ctx->cmd->context = ctx;
	return cmd_append_continue_cancel(ctx->cmd);
}

static bool cmd_append_continue_parsing(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	struct cmd_append_context *ctx = cmd->context;
	const struct imap_arg *args;
	const struct imap_arg *flags_list;
	enum mail_flags flags;
	const char *const *keywords_list;
	struct mail_keywords *keywords;
	const char *internal_date_str;
	time_t internal_date;
	int ret, timezone_offset;
	bool nonsync;

	if (cmd->cancel) {
		cmd_append_finish(ctx);
		return TRUE;
	}

	/* if error occurs, the CRLF is already read. */
	client->input_skip_line = FALSE;

	/* [<flags>] [<internal date>] <message literal> */
	ret = imap_parser_read_args(ctx->save_parser, 0,
				    IMAP_PARSE_FLAG_LITERAL_SIZE, &args);
	if (ret == -1 || client->output->closed) {
		if (!ctx->failed)
			client_send_command_error(cmd, NULL);
		cmd_append_finish(ctx);
		return TRUE;
	}
	if (ret < 0) {
		/* need more data */
		return FALSE;
	}

	if (args->type == IMAP_ARG_EOL) {
		/* last message */
		enum mailbox_sync_flags sync_flags;
		enum imap_sync_flags imap_flags;
		uint32_t uid_validity, uid1, uid2;
		const char *msg;

		/* eat away the trailing CRLF */
		client->input_skip_line = TRUE;

		if (ctx->failed) {
			/* we failed earlier, error message is sent */
			cmd_append_finish(ctx);
			return TRUE;
		}

		ret = mailbox_transaction_commit_get_uids(&ctx->t,
							  &uid_validity,
							  &uid1, &uid2);
		if (ret < 0) {
			client_send_storage_error(cmd, ctx->storage);
			cmd_append_finish(ctx);
			return TRUE;
		}
		i_assert(ctx->count == uid2 - uid1 + 1);

		if (uid1 == uid2) {
			msg = t_strdup_printf("OK [APPENDUID %u %u] "
					      "Append completed.",
					      uid_validity, uid1);
		} else {
			msg = t_strdup_printf("OK [APPENDUID %u %u:%u] "
					      "Append completed.",
					      uid_validity, uid1, uid2);
		}

		if (ctx->box == cmd->client->mailbox) {
			sync_flags = 0;
			imap_flags = IMAP_SYNC_FLAG_SAFE;
		} else {
			sync_flags = MAILBOX_SYNC_FLAG_FAST;
			imap_flags = 0;
		}

		cmd_append_finish(ctx);
		return cmd_sync(cmd, sync_flags, imap_flags, msg);
	}

	if (!validate_args(args, &flags_list, &internal_date_str,
			   &ctx->msg_size, &nonsync)) {
		client_send_command_error(cmd, "Invalid arguments.");
		return cmd_append_cancel(ctx, nonsync);
	}

	if (ctx->failed) {
		/* we failed earlier, make sure we just eat nonsync-literal
		   if it's given. */
		return cmd_append_cancel(ctx, nonsync);
	}

	if (flags_list != NULL) {
		if (!client_parse_mail_flags(cmd, flags_list,
					     &flags, &keywords_list))
			return cmd_append_cancel(ctx, nonsync);
		if (keywords_list == NULL)
			keywords = NULL;
		else if (mailbox_keywords_create(ctx->box, keywords_list,
						 &keywords) < 0) {
			client_send_storage_error(cmd, ctx->storage);
			return cmd_append_cancel(ctx, nonsync);
		}
	} else {
		flags = 0;
		keywords = NULL;
	}

	if (internal_date_str == NULL) {
		/* no time given, default to now. */
		internal_date = (time_t)-1;
		timezone_offset = 0;
	} else if (!imap_parse_datetime(internal_date_str,
					&internal_date, &timezone_offset)) {
		client_send_tagline(cmd, "BAD Invalid internal date.");
		return cmd_append_cancel(ctx, nonsync);
	}

	if (ctx->msg_size == 0) {
		/* no message data, abort */
		client_send_tagline(cmd, "NO Can't save a zero byte message.");
		return cmd_append_cancel(ctx, nonsync);
	}

	/* save the mail */
	ctx->input = i_stream_create_limit(client->input, ctx->msg_size);
	ret = mailbox_save_init(ctx->t, flags, keywords,
				internal_date, timezone_offset, NULL,
				ctx->input, FALSE, &ctx->save_ctx);

	if (keywords != NULL)
		mailbox_keywords_free(ctx->box, &keywords);

	if (ret < 0) {
		/* save initialization failed */
		client_send_storage_error(cmd, ctx->storage);
		return cmd_append_cancel(ctx, nonsync);
	}

	/* after literal comes CRLF, if we fail make sure we eat it away */
	client->input_skip_line = TRUE;

	if (!nonsync) {
		o_stream_send(client->output, "+ OK\r\n", 6);
		o_stream_flush(client->output);
		o_stream_uncork(client->output);
		o_stream_cork(client->output);
	}

	ctx->count++;
	ctx->message_input = TRUE;
	cmd->func = cmd_append_continue_message;
	return cmd_append_continue_message(cmd);
}

static bool cmd_append_continue_message(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	struct cmd_append_context *ctx = cmd->context;
	size_t size;
	int ret;

	if (cmd->cancel) {
		cmd_append_finish(ctx);
		return TRUE;
	}

	if (ctx->save_ctx != NULL) {
		while (ctx->input->v_offset != ctx->msg_size) {
			ret = i_stream_read(ctx->input);
			if (mailbox_save_continue(ctx->save_ctx) < 0) {
				/* we still have to finish reading the message
				   from client */
				mailbox_save_cancel(&ctx->save_ctx);
				break;
			}
			if (ret == -1 || ret == 0)
				break;
		}
	}

	if (ctx->save_ctx == NULL) {
		(void)i_stream_read(ctx->input);
		(void)i_stream_get_data(ctx->input, &size);
		i_stream_skip(ctx->input, size);
	}

	if (ctx->input->eof || client->input->closed) {
		bool all_written = ctx->input->v_offset == ctx->msg_size;

		/* finished */
		i_stream_unref(&ctx->input);
		ctx->input = NULL;

		if (ctx->save_ctx == NULL) {
			/* failed above */
			client_send_storage_error(cmd, ctx->storage);
			ctx->failed = TRUE;
		} else if (!all_written) {
			/* client disconnected before it finished sending the
			   whole message. */
			ctx->failed = TRUE;
			mailbox_save_cancel(&ctx->save_ctx);
			client_disconnect(client, "EOF while appending");
		} else if (mailbox_save_finish(&ctx->save_ctx) < 0) {
			ctx->failed = TRUE;
			client_send_storage_error(cmd, ctx->storage);
		}
		ctx->save_ctx = NULL;

		if (client->input->closed) {
			cmd_append_finish(ctx);
			return TRUE;
		}

		/* prepare for next message */
		ctx->message_input = FALSE;
		imap_parser_reset(ctx->save_parser);
		cmd->func = cmd_append_continue_parsing;
		return cmd_append_continue_parsing(cmd);
	}

	return FALSE;
}

static struct mailbox *
get_mailbox(struct client_command_context *cmd, const char *name)
{
	struct mail_storage *storage;
	struct mailbox *box;

	if (!client_verify_mailbox_name(cmd, name, TRUE, FALSE))
		return NULL;

	storage = client_find_storage(cmd, &name);
	if (storage == NULL)
		return NULL;

	if (cmd->client->mailbox != NULL &&
	    mailbox_equals(cmd->client->mailbox, storage, name))
		return cmd->client->mailbox;

	box = mailbox_open(storage, name, NULL, MAILBOX_OPEN_SAVEONLY |
			   MAILBOX_OPEN_FAST | MAILBOX_OPEN_KEEP_RECENT);
	if (box == NULL) {
		client_send_storage_error(cmd, storage);
		return NULL;
	}
	return box;
}

bool cmd_append(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
        struct cmd_append_context *ctx;
	const char *mailbox;

	/* <mailbox> */
	if (!client_read_string_args(cmd, 1, &mailbox))
		return FALSE;

	/* we keep the input locked all the time */
	client->input_lock = cmd;

	ctx = p_new(cmd->pool, struct cmd_append_context, 1);
	ctx->cmd = cmd;
	ctx->client = client;
	ctx->box = get_mailbox(cmd, mailbox);
	if (ctx->box == NULL)
		ctx->failed = TRUE;
	else {
		ctx->storage = mailbox_get_storage(ctx->box);

		ctx->t = mailbox_transaction_begin(ctx->box,
					MAILBOX_TRANSACTION_FLAG_EXTERNAL |
					MAILBOX_TRANSACTION_FLAG_ASSIGN_UIDS);
	}

	io_remove(&client->io);
	client->io = io_add(i_stream_get_fd(client->input), IO_READ,
			    client_input_append, cmd);
	/* append is special because we're only waiting on client input, not
	   client output, so disable the standard output handler until we're
	   finished */
	o_stream_unset_flush_callback(client->output);

	ctx->save_parser = imap_parser_create(client->input, client->output,
					      imap_max_line_length);

	cmd->func = cmd_append_continue_parsing;
	cmd->context = ctx;
	return cmd_append_continue_parsing(cmd);
}
