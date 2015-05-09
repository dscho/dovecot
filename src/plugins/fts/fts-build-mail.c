/* Copyright (c) 2006-2015 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "istream.h"
#include "buffer.h"
#include "str.h"
#include "rfc822-parser.h"
#include "message-address.h"
#include "message-parser.h"
#include "message-decoder.h"
#include "mail-storage.h"
#include "fts-parser.h"
#include "fts-user.h"
#include "fts-language.h"
#include "fts-tokenizer.h"
#include "fts-filter.h"
#include "fts-api-private.h"
#include "fts-build-mail.h"

/* there are other characters as well, but this doesn't have to be exact */
#define IS_WORD_WHITESPACE(c) \
	((c) == ' ' || (c) == '\t' || (c) == '\n')
/* if we see a word larger than this, just go ahead and split it from
   wherever */
#define MAX_WORD_SIZE 1024

struct fts_mail_build_context {
	struct mail *mail;
	struct fts_backend_update_context *update_ctx;

	char *content_type, *content_disposition;
	struct fts_parser *body_parser;

	buffer_t *word_buf, *pending_input;
	struct fts_user_language *cur_user_lang;
};

static struct fts_user_language fts_user_language_data = {
	.lang = &fts_language_data,
	.filter = NULL
};

static int fts_build_data(struct fts_mail_build_context *ctx,
			  const unsigned char *data, size_t size, bool last);

static void fts_build_parse_content_type(struct fts_mail_build_context *ctx,
					 const struct message_header_line *hdr)
{
	struct rfc822_parser_context parser;
	string_t *content_type;

	rfc822_parser_init(&parser, hdr->full_value, hdr->full_value_len, NULL);
	rfc822_skip_lwsp(&parser);

	T_BEGIN {
		content_type = t_str_new(64);
		if (rfc822_parse_content_type(&parser, content_type) >= 0) {
			i_free(ctx->content_type);
			ctx->content_type =
				str_lcase(i_strdup(str_c(content_type)));
		}
	} T_END;
}

static void
fts_build_parse_content_disposition(struct fts_mail_build_context *ctx,
				    const struct message_header_line *hdr)
{
	/* just pass it as-is to backend. */
	i_free(ctx->content_disposition);
	ctx->content_disposition =
		i_strndup(hdr->full_value, hdr->full_value_len);
}

static bool header_has_language(const char *name)
{
	/* FIXME: should email address headers be detected as different
	   languages? That mainly contains people's names.. */
	/*if (message_header_is_address(name))
		return TRUE;*/

	/* Subject definitely contains language-specific data that can be
	   detected. Comment and Keywords headers also could contain, although
	   just about nobody uses those headers.

	   For now we assume that other headers contain non-language specific
	   data that we don't want to filter in special ways. For example
	   it is good to be able to search for Message-IDs. */
	return strcasecmp(name, "Subject") == 0 ||
		strcasecmp(name, "Comments") == 0 ||
		strcasecmp(name, "Keywords") == 0;
}

static void fts_parse_mail_header(struct fts_mail_build_context *ctx,
				  const struct message_block *raw_block)
{
	const struct message_header_line *hdr = raw_block->hdr;

	if (strcasecmp(hdr->name, "Content-Type") == 0)
		fts_build_parse_content_type(ctx, hdr);
	else if (strcasecmp(hdr->name, "Content-Disposition") == 0)
		fts_build_parse_content_disposition(ctx, hdr);
}

static void
fts_build_unstructured_header(struct fts_mail_build_context *ctx,
			      const struct message_header_line *hdr)
{
	const unsigned char *data = hdr->full_value;
	unsigned char *buf = NULL;
	unsigned int i;

	/* @UNSAFE: if there are any NULs, replace them with spaces */
	for (i = 0; i < hdr->full_value_len; i++) {
		if (data[i] == '\0') {
			if (buf == NULL) {
				buf = i_malloc(hdr->full_value_len);
				memcpy(buf, data, i);
				data = buf;
			}
			buf[i] = ' ';
		} else if (buf != NULL) {
			buf[i] = data[i];
		}
	}
	(void)fts_build_data(ctx, data, hdr->full_value_len, TRUE);
	i_free(buf);
}

static void fts_build_mail_header(struct fts_mail_build_context *ctx,
				  const struct message_block *block)
{
	const struct message_header_line *hdr = block->hdr;
	struct fts_backend_build_key key;

	if (hdr->eoh)
		return;

	/* hdr->full_value is always set because we get the block from
	   message_decoder */
	memset(&key, 0, sizeof(key));
	key.uid = ctx->mail->uid;
	key.type = block->part->physical_pos == 0 ?
		FTS_BACKEND_BUILD_KEY_HDR : FTS_BACKEND_BUILD_KEY_MIME_HDR;
	key.part = block->part;
	key.hdr_name = hdr->name;

	if (!header_has_language(key.hdr_name))
		ctx->cur_user_lang = &fts_user_language_data;
	else
		ctx->cur_user_lang = NULL;

	if (!fts_backend_update_set_build_key(ctx->update_ctx, &key))
		return;

	if (!message_header_is_address(hdr->name)) {
		/* regular unstructured header */
		fts_build_unstructured_header(ctx, hdr);
	} else T_BEGIN {
		/* message address. normalize it to give better
		   search results. */
		struct message_address *addr;
		string_t *str;

		addr = message_address_parse(pool_datastack_create(),
					     hdr->full_value,
					     hdr->full_value_len,
					     UINT_MAX, FALSE);
		str = t_str_new(hdr->full_value_len);
		message_address_write(str, addr);

		(void)fts_build_data(ctx, str_data(str), str_len(str), TRUE);
	} T_END;

	if ((ctx->update_ctx->backend->flags &
	     FTS_BACKEND_FLAG_TOKENIZED_INPUT) != 0) {
		/* index the header name itself */
		key.hdr_name = "";
		if (fts_backend_update_set_build_key(ctx->update_ctx, &key)) {
			(void)fts_build_data(ctx, (const void *)hdr->name,
					     strlen(hdr->name), TRUE);
		}
	}
}

static bool
fts_build_body_begin(struct fts_mail_build_context *ctx,
		     struct message_part *part, bool *binary_body_r)
{
	struct mail_storage *storage;
	const char *content_type;
	struct fts_backend_build_key key;

	i_assert(ctx->body_parser == NULL);

	*binary_body_r = FALSE;
	memset(&key, 0, sizeof(key));
	key.uid = ctx->mail->uid;
	key.part = part;

	content_type = ctx->content_type != NULL ?
		ctx->content_type : "text/plain";
	if (strncmp(content_type, "multipart/", 10) == 0) {
		/* multiparts are never indexed, only their contents */
		return FALSE;
	}

	
	storage = mailbox_get_storage(ctx->mail->box);
	if (fts_parser_init(mail_storage_get_user(storage),
			    content_type, ctx->content_disposition,
			    &ctx->body_parser)) {
		/* extract text using the the returned parser */
		*binary_body_r = TRUE;
		key.type = FTS_BACKEND_BUILD_KEY_BODY_PART;
	} else if (strncmp(content_type, "text/", 5) == 0 ||
		   strncmp(content_type, "message/", 8) == 0) {
		/* text body parts */
		key.type = FTS_BACKEND_BUILD_KEY_BODY_PART;
		ctx->body_parser = fts_parser_text_init();
	} else {
		/* possibly binary */
		if ((ctx->update_ctx->backend->flags &
		     FTS_BACKEND_FLAG_BINARY_MIME_PARTS) == 0)
			return FALSE;
		*binary_body_r = TRUE;
		key.type = FTS_BACKEND_BUILD_KEY_BODY_PART_BINARY;
	}
	key.body_content_type = content_type;
	key.body_content_disposition = ctx->content_disposition;
	ctx->cur_user_lang = NULL;
	if (!fts_backend_update_set_build_key(ctx->update_ctx, &key)) {
		if (ctx->body_parser != NULL)
			(void)fts_parser_deinit(&ctx->body_parser);
		return FALSE;
	}
	return TRUE;
}

static int
fts_build_add_tokens_with_filter(struct fts_mail_build_context *ctx,
				 const unsigned char *data, size_t size)
{
	struct fts_tokenizer *tokenizer;
	struct fts_filter *filter = ctx->cur_user_lang->filter;
	const char *token;
	const char *error;
	int ret;

	tokenizer = fts_user_get_index_tokenizer(ctx->update_ctx->backend->ns->user);
	while ((ret = fts_tokenizer_next(tokenizer, data, size, &token)) > 0) {
		if (filter != NULL) {
			ret = fts_filter_filter(filter, &token, &error);
			if (ret == 0)
				continue;
			if (ret < 0)
				break;
		}
		if (fts_backend_update_build_more(ctx->update_ctx,
						  (const void *)token,
						  strlen(token)) < 0)
			return -1;
	}
	if (ret < 0)
		i_error("fts: Couldn't create indexable tokens: %s", error);
	return ret;
}

static int
fts_detect_language(struct fts_mail_build_context *ctx,
		    const unsigned char *data, size_t size, bool last,
		    const struct fts_language **lang_r)
{
	struct mail_user *user = ctx->update_ctx->backend->ns->user;
	struct fts_language_list *lang_list = fts_user_get_language_list(user);
	const struct fts_language *lang;

	switch (fts_language_detect(lang_list, data, size, &lang)) {
	case FTS_LANGUAGE_RESULT_SHORT:
		/* save the input so far and try again later */
		buffer_append(ctx->pending_input, data, size);
		if (last) {
			/* we've run out of data. use the default language. */
			*lang_r = fts_language_list_get_first(lang_list);
			return 1;
		}
		return 0;
	case FTS_LANGUAGE_RESULT_UNKNOWN:
		/* use the default language */
		*lang_r = fts_language_list_get_first(lang_list);
		return 1;
	case FTS_LANGUAGE_RESULT_OK:
		*lang_r = lang;
		return 1;
	case FTS_LANGUAGE_RESULT_ERROR:
		/* internal language detection library failure
		   (e.g. invalid config). don't index anything. */
		return -1;
	default:
		i_unreached();
	}
}

static int
fts_build_tokenized(struct fts_mail_build_context *ctx,
		    const unsigned char *data, size_t size, bool last)
{
	struct mail_user *user = ctx->update_ctx->backend->ns->user;
	const struct fts_language *lang;
	int ret;

	if (ctx->cur_user_lang != NULL) {
		/* we already have a language */
	} else if ((ret = fts_detect_language(ctx, data, size, last, &lang)) < 0) {
		return -1;
	} else if (ret == 0) {
		/* wait for more data */
		return 0;
	} else {
		ctx->cur_user_lang = fts_user_language_find(user, lang);
		i_assert(ctx->cur_user_lang != NULL);

		if (ctx->pending_input->used > 0) {
			if (fts_build_add_tokens_with_filter(ctx,
					ctx->pending_input->data,
					ctx->pending_input->used) < 0)
				return -1;
			buffer_set_used_size(ctx->pending_input, 0);
		}
	}
	if (fts_build_add_tokens_with_filter(ctx, data, size) < 0)
		return -1;
	if (last) {
		if (fts_build_add_tokens_with_filter(ctx, NULL, 0) < 0)
			return -1;
	}
	return 0;
}

static int
fts_build_full_words(struct fts_mail_build_context *ctx,
		     const unsigned char *data, size_t size, bool last)
{
	size_t i;

	/* we'll need to send only full words to the backend */

	if (ctx->word_buf != NULL && ctx->word_buf->used > 0) {
		/* continuing previous word */
		for (i = 0; i < size; i++) {
			if (IS_WORD_WHITESPACE(data[i]))
				break;
		}
		buffer_append(ctx->word_buf, data, i);
		data += i;
		size -= i;
		if (size == 0 && ctx->word_buf->used < MAX_WORD_SIZE && !last) {
			/* word is still not finished */
			return 0;
		}
		/* we have a full word, index it */
		if (fts_backend_update_build_more(ctx->update_ctx,
						  ctx->word_buf->data,
						  ctx->word_buf->used) < 0)
			return -1;
		buffer_set_used_size(ctx->word_buf, 0);
	}

	/* find the boundary for last word */
	if (last)
		i = size;
	else {
		for (i = size; i > 0; i--) {
			if (IS_WORD_WHITESPACE(data[i-1]))
				break;
		}
	}

	if (fts_backend_update_build_more(ctx->update_ctx, data, i) < 0)
		return -1;

	if (i < size) {
		if (ctx->word_buf == NULL) {
			ctx->word_buf =
				buffer_create_dynamic(default_pool, 128);
		}
		buffer_append(ctx->word_buf, data + i, size - i);
	}
	return 0;
}

static int fts_build_data(struct fts_mail_build_context *ctx,
			  const unsigned char *data, size_t size, bool last)
{
	if ((ctx->update_ctx->backend->flags &
	     FTS_BACKEND_FLAG_TOKENIZED_INPUT) != 0) {
		return fts_build_tokenized(ctx, data, size, last);
	} else if ((ctx->update_ctx->backend->flags &
		    FTS_BACKEND_FLAG_BUILD_FULL_WORDS) != 0) {
		return fts_build_full_words(ctx, data, size, last);
	} else {
		return fts_backend_update_build_more(ctx->update_ctx, data, size);
	}
}

static int fts_build_body_block(struct fts_mail_build_context *ctx,
				const struct message_block *block, bool last)
{
	i_assert(block->hdr == NULL);

	return fts_build_data(ctx, block->data, block->size, last);
}

static int fts_body_parser_finish(struct fts_mail_build_context *ctx)
{
	struct message_block block;
	int ret = 0;

	do {
		memset(&block, 0, sizeof(block));
		fts_parser_more(ctx->body_parser, &block);
		if (fts_build_body_block(ctx, &block, FALSE) < 0) {
			ret = -1;
			break;
		}
	} while (block.size > 0);

	if (fts_parser_deinit(&ctx->body_parser) < 0)
		ret = -1;
	return ret;
}

static int
fts_build_mail_real(struct fts_backend_update_context *update_ctx,
		    struct mail *mail)
{
	struct fts_mail_build_context ctx;
	struct istream *input;
	struct message_parser_ctx *parser;
	struct message_decoder_context *decoder;
	struct message_block raw_block, block;
	struct message_part *prev_part, *parts;
	bool skip_body = FALSE, body_part = FALSE, body_added = FALSE;
	bool binary_body;
	int ret;

	if (mail_get_stream(mail, NULL, NULL, &input) < 0) {
		if (mail->expunged)
			return 0;
		i_error("Failed to read mailbox %s mail UID=%u stream: %s",
			mailbox_get_vname(mail->box), mail->uid,
			mailbox_get_last_error(mail->box, NULL));
		return -1;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.update_ctx = update_ctx;
	ctx.mail = mail;
	if ((update_ctx->backend->flags & FTS_BACKEND_FLAG_TOKENIZED_INPUT) != 0) {
		ctx.pending_input = buffer_create_dynamic(default_pool, 128);
		/* reset tokenizer between mails - just to be sure no state
		   leaks between mails (especially if previous indexing had
		   failed) */
		struct fts_tokenizer *tokenizer;

		tokenizer = fts_user_get_index_tokenizer(update_ctx->backend->ns->user);
		fts_tokenizer_reset(tokenizer);
	}

	prev_part = NULL;
	parser = message_parser_init(pool_datastack_create(), input,
				     MESSAGE_HEADER_PARSER_FLAG_CLEAN_ONELINE,
				     0);

	decoder = message_decoder_init(update_ctx->normalizer, 0);
	for (;;) {
		ret = message_parser_parse_next_block(parser, &raw_block);
		i_assert(ret != 0);
		if (ret < 0) {
			if (input->stream_errno == 0)
				ret = 0;
			break;
		}

		if (raw_block.part != prev_part) {
			/* body part changed. we're now parsing the end of
			   boundary, possibly followed by message epilogue */
			if (ctx.body_parser != NULL) {
				if (fts_body_parser_finish(&ctx) < 0) {
					ret = -1;
					break;
				}
			}
			message_decoder_set_return_binary(decoder, FALSE);
			fts_backend_update_unset_build_key(update_ctx);
			prev_part = raw_block.part;
			i_free_and_null(ctx.content_type);
			i_free_and_null(ctx.content_disposition);

			if (raw_block.size != 0) {
				/* multipart. skip until beginning of next
				   part's headers */
				skip_body = TRUE;
			}
		}

		if (raw_block.hdr != NULL) {
			/* always handle headers */
		} else if (raw_block.size == 0) {
			/* end of headers */
			skip_body = !fts_build_body_begin(&ctx, raw_block.part,
							  &binary_body);
			if (binary_body)
				message_decoder_set_return_binary(decoder, TRUE);
			body_part = TRUE;
		} else {
			if (skip_body)
				continue;
		}

		if (!message_decoder_decode_next_block(decoder, &raw_block,
						       &block))
			continue;

		if (block.hdr != NULL) {
			fts_parse_mail_header(&ctx, &raw_block);
			fts_build_mail_header(&ctx, &block);
		} else if (block.size == 0) {
			/* end of headers */
		} else {
			i_assert(body_part);
			if (ctx.body_parser != NULL)
				fts_parser_more(ctx.body_parser, &block);
			if (fts_build_body_block(&ctx, &block, FALSE) < 0) {
				ret = -1;
				break;
			}
			body_added = TRUE;
		}
	}
	if (ctx.body_parser != NULL) {
		if (ret == 0)
			ret = fts_body_parser_finish(&ctx);
		else
			(void)fts_parser_deinit(&ctx.body_parser);
	}
	if (ret == 0 && body_part && !skip_body && !body_added) {
		/* make sure body is added even when it doesn't exist */
		block.data = NULL; block.size = 0;
		ret = fts_build_body_block(&ctx, &block, TRUE);
	}
	if (message_parser_deinit(&parser, &parts) < 0)
		mail_set_cache_corrupted(mail, MAIL_FETCH_MESSAGE_PARTS);
	message_decoder_deinit(&decoder);
	i_free(ctx.content_type);
	i_free(ctx.content_disposition);
	if (ctx.word_buf != NULL)
		buffer_free(&ctx.word_buf);
	if (ctx.pending_input != NULL)
		buffer_free(&ctx.pending_input);
	return ret < 0 ? -1 : 1;
}

int fts_build_mail(struct fts_backend_update_context *update_ctx,
		   struct mail *mail)
{
	int ret;

	T_BEGIN {
		ret = fts_build_mail_real(update_ctx, mail);
	} T_END;
	return ret;
}
