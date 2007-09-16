/* Copyright (c) 2002-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "istream.h"
#include "ostream.h"
#include "strescape.h"
#include "imap-parser.h"

#define is_linebreak(c) \
	((c) == '\r' || (c) == '\n')

#define LIST_INIT_COUNT 7

enum arg_parse_type {
	ARG_PARSE_NONE = 0,
	ARG_PARSE_ATOM,
	ARG_PARSE_STRING,
	ARG_PARSE_LITERAL,
	ARG_PARSE_LITERAL_DATA
};

struct imap_parser {
	/* permanent */
	pool_t pool;
	struct istream *input;
	struct ostream *output;
	size_t max_line_size;
        enum imap_parser_flags flags;

	/* reset by imap_parser_reset(): */
	size_t line_size;
	ARRAY_TYPE(imap_arg_list) root_list;
        ARRAY_TYPE(imap_arg_list) *cur_list;
	struct imap_arg *list_arg;

	enum arg_parse_type cur_type;
	size_t cur_pos; /* parser position in input buffer */

	int str_first_escape; /* ARG_PARSE_STRING: index to first '\' */
	uoff_t literal_size; /* ARG_PARSE_LITERAL: string size */

	const char *error;

	unsigned int literal_skip_crlf:1;
	unsigned int literal_nonsync:1;
	unsigned int eol:1;
	unsigned int fatal_error:1;
};

struct imap_parser *
imap_parser_create(struct istream *input, struct ostream *output,
		   size_t max_line_size)
{
	struct imap_parser *parser;

	parser = i_new(struct imap_parser, 1);
        parser->pool = pool_alloconly_create("IMAP parser", 1024*10);
	parser->input = input;
	parser->output = output;
	parser->max_line_size = max_line_size;

	p_array_init(&parser->root_list, parser->pool, LIST_INIT_COUNT);
	parser->cur_list = &parser->root_list;
	return parser;
}

void imap_parser_destroy(struct imap_parser **parser)
{
	pool_unref(&(*parser)->pool);
	i_free(*parser);
	*parser = NULL;
}

void imap_parser_reset(struct imap_parser *parser)
{
	p_clear(parser->pool);

	parser->line_size = 0;

	p_array_init(&parser->root_list, parser->pool, LIST_INIT_COUNT);
	parser->cur_list = &parser->root_list;
	parser->list_arg = NULL;

	parser->cur_type = ARG_PARSE_NONE;
	parser->cur_pos = 0;

	parser->str_first_escape = 0;
	parser->literal_size = 0;

	parser->error = NULL;

	parser->literal_skip_crlf = FALSE;
	parser->eol = FALSE;
}

const char *imap_parser_get_error(struct imap_parser *parser, bool *fatal)
{
        *fatal = parser->fatal_error;
	return parser->error;
}

/* skip over everything parsed so far, plus the following whitespace */
static int imap_parser_skip_to_next(struct imap_parser *parser,
				    const unsigned char **data,
				    size_t *data_size)
{
	size_t i;

	for (i = parser->cur_pos; i < *data_size; i++) {
		if ((*data)[i] != ' ')
			break;
	}

	parser->line_size += i;
        i_stream_skip(parser->input, i);
	parser->cur_pos = 0;

	*data += i;
	*data_size -= i;
	return *data_size > 0;
}

static struct imap_arg *imap_arg_create(struct imap_parser *parser)
{
	struct imap_arg *arg;

	arg = array_append_space(parser->cur_list);
	arg->parent = parser->list_arg;
	return arg;
}

static void imap_parser_open_list(struct imap_parser *parser)
{
	parser->list_arg = imap_arg_create(parser);
	parser->list_arg->type = IMAP_ARG_LIST;
	p_array_init(&parser->list_arg->_data.list, parser->pool,
		     LIST_INIT_COUNT);
	parser->cur_list = &parser->list_arg->_data.list;

	parser->cur_type = ARG_PARSE_NONE;
}

static int imap_parser_close_list(struct imap_parser *parser)
{
	struct imap_arg *arg;

	if (parser->list_arg == NULL) {
		/* we're not inside list */
		parser->error = "Unexpected ')'";
		return FALSE;
	}

	arg = imap_arg_create(parser);
	arg->type = IMAP_ARG_EOL;

	parser->list_arg = parser->list_arg->parent;
	if (parser->list_arg == NULL) {
		parser->cur_list = &parser->root_list;
	} else {
		parser->cur_list = &parser->list_arg->_data.list;
	}

	parser->cur_type = ARG_PARSE_NONE;
	return TRUE;
}

static void imap_parser_save_arg(struct imap_parser *parser,
				 const unsigned char *data, size_t size)
{
	struct imap_arg *arg;
	char *str;

	arg = imap_arg_create(parser);

	switch (parser->cur_type) {
	case ARG_PARSE_ATOM:
		if (size == 3 && memcmp(data, "NIL", 3) == 0) {
			/* NIL argument */
			arg->type = IMAP_ARG_NIL;
		} else {
			/* simply save the string */
			arg->type = IMAP_ARG_ATOM;
			arg->_data.str = p_strndup(parser->pool, data, size);
		}
		break;
	case ARG_PARSE_STRING:
		/* data is quoted and may contain escapes. */
		i_assert(size > 0);

		arg->type = IMAP_ARG_STRING;
		str = p_strndup(parser->pool, data+1, size-1);

		/* remove the escapes */
		if (parser->str_first_escape >= 0 &&
		    (parser->flags & IMAP_PARSE_FLAG_NO_UNESCAPE) == 0) {
			/* -1 because we skipped the '"' prefix */
			str_unescape(str + parser->str_first_escape-1);
		}
		arg->_data.str = str;
		break;
	case ARG_PARSE_LITERAL_DATA:
		if ((parser->flags & IMAP_PARSE_FLAG_LITERAL_SIZE) != 0) {
			/* save literal size */
			arg->type = parser->literal_nonsync ?
				IMAP_ARG_LITERAL_SIZE_NONSYNC :
				IMAP_ARG_LITERAL_SIZE;
			arg->_data.literal_size = parser->literal_size;
		} else if ((parser->flags &
			    IMAP_PARSE_FLAG_LITERAL_TYPE) != 0) {
			arg->type = IMAP_ARG_LITERAL;
			arg->_data.str = p_strndup(parser->pool, data, size);
		} else {
			arg->type = IMAP_ARG_STRING;
			arg->_data.str = p_strndup(parser->pool, data, size);
		}
		break;
	default:
                i_unreached();
	}

	parser->cur_type = ARG_PARSE_NONE;
}

static int is_valid_atom_char(struct imap_parser *parser, char chr)
{
	if (IS_ATOM_SPECIAL_INPUT((unsigned char)chr)) {
		parser->error = "Invalid characters in atom";
		return FALSE;
	} else if ((chr & 0x80) != 0) {
		parser->error = "8bit data in atom";
		return FALSE;
	}

	return TRUE;
}

static int imap_parser_read_atom(struct imap_parser *parser,
				 const unsigned char *data, size_t data_size)
{
	size_t i;

	/* read until we've found space, CR or LF. */
	for (i = parser->cur_pos; i < data_size; i++) {
		if (data[i] == ' ' || data[i] == ')' ||
			 is_linebreak(data[i])) {
			imap_parser_save_arg(parser, data, i);
			break;
		} else if (!is_valid_atom_char(parser, data[i]))
			return FALSE;
	}

	parser->cur_pos = i;
	return parser->cur_type == ARG_PARSE_NONE;
}

static int imap_parser_read_string(struct imap_parser *parser,
				   const unsigned char *data, size_t data_size)
{
	size_t i;

	/* read until we've found non-escaped ", CR or LF */
	for (i = parser->cur_pos; i < data_size; i++) {
		if (data[i] == '"') {
			imap_parser_save_arg(parser, data, i);

			i++; /* skip the trailing '"' too */
			break;
		}

		if (data[i] == '\\') {
			if (i+1 == data_size) {
				/* known data ends with '\' - leave it to
				   next time as well if it happens to be \" */
				break;
			}

			/* save the first escaped char */
			if (parser->str_first_escape < 0)
				parser->str_first_escape = i;

			/* skip the escaped char */
			i++;
		}

		/* check linebreaks here, so escaping CR/LF isn't possible.
		   string always ends with '"', so it's an error if we found
		   a linebreak.. */
		if (is_linebreak(data[i])) {
			parser->error = "Missing '\"'";
			return FALSE;
		}
	}

	parser->cur_pos = i;
	return parser->cur_type == ARG_PARSE_NONE;
}

static int imap_parser_literal_end(struct imap_parser *parser)
{
	if ((parser->flags & IMAP_PARSE_FLAG_LITERAL_SIZE) == 0) {
		if (parser->line_size >= parser->max_line_size ||
		    parser->literal_size >
		    parser->max_line_size - parser->line_size) {
			/* too long string, abort. */
			parser->error = "Literal size too large";
			parser->fatal_error = TRUE;
			return FALSE;
		}

		if (parser->output != NULL && !parser->literal_nonsync) {
			o_stream_send(parser->output, "+ OK\r\n", 6);
			o_stream_flush(parser->output);
		}
	}

	parser->cur_type = ARG_PARSE_LITERAL_DATA;
	parser->literal_skip_crlf = TRUE;

	parser->cur_pos = 0;
	return TRUE;
}

static int imap_parser_read_literal(struct imap_parser *parser,
				    const unsigned char *data,
				    size_t data_size)
{
	size_t i, prev_size;

	/* expecting digits + "}" */
	for (i = parser->cur_pos; i < data_size; i++) {
		if (data[i] == '}') {
			parser->line_size += i+1;
			i_stream_skip(parser->input, i+1);
			return imap_parser_literal_end(parser);
		}

		if (parser->literal_nonsync) {
			parser->error = "Expecting '}' after '+'";
			return FALSE;
		}

		if (data[i] == '+') {
			parser->literal_nonsync = TRUE;
			continue;
		}

		if (data[i] < '0' || data[i] > '9') {
			parser->error = "Invalid literal size";
			return FALSE;
		}

		prev_size = parser->literal_size;
		parser->literal_size = parser->literal_size*10 + (data[i]-'0');

		if (parser->literal_size < prev_size) {
			/* wrapped around, abort. */
			parser->error = "Literal size too large";
			return FALSE;
		}
	}

	parser->cur_pos = i;
	return FALSE;
}

static int imap_parser_read_literal_data(struct imap_parser *parser,
					 const unsigned char *data,
					 size_t data_size)
{
	if (parser->literal_skip_crlf) {
		/* skip \r\n or \n, anything else gives an error */
		if (data_size == 0)
			return FALSE;

		if (*data == '\r') {
			parser->line_size++;
			data++; data_size--;
			i_stream_skip(parser->input, 1);

			if (data_size == 0)
				return FALSE;
		}

		if (*data != '\n') {
			parser->error = "Missing LF after literal size";
			return FALSE;
		}

		parser->line_size++;
		data++; data_size--;
		i_stream_skip(parser->input, 1);

		parser->literal_skip_crlf = FALSE;

		i_assert(parser->cur_pos == 0);
	}

	if ((parser->flags & IMAP_PARSE_FLAG_LITERAL_SIZE) == 0) {
		/* now we just wait until we've read enough data */
		if (data_size < parser->literal_size)
			return FALSE;
		else {
			imap_parser_save_arg(parser, data,
					     (size_t)parser->literal_size);
			parser->cur_pos = (size_t)parser->literal_size;
			return TRUE;
		}
	} else {
		/* we want to save only literal size, not the literal itself. */
		parser->eol = TRUE;
		imap_parser_save_arg(parser, NULL, 0);
		return TRUE;
	}
}

/* Returns TRUE if argument was fully processed. Also returns TRUE if
   an argument inside a list was processed. */
static int imap_parser_read_arg(struct imap_parser *parser)
{
	const unsigned char *data;
	size_t data_size;

	data = i_stream_get_data(parser->input, &data_size);
	if (data_size == 0)
		return FALSE;

	while (parser->cur_type == ARG_PARSE_NONE) {
		/* we haven't started parsing yet */
		if (!imap_parser_skip_to_next(parser, &data, &data_size))
			return FALSE;
		i_assert(parser->cur_pos == 0);

		switch (data[0]) {
		case '\r':
		case '\n':
			/* unexpected end of line */
			parser->eol = TRUE;
			return FALSE;
		case '"':
			parser->cur_type = ARG_PARSE_STRING;
			parser->str_first_escape = -1;
			break;
		case '{':
			parser->cur_type = ARG_PARSE_LITERAL;
			parser->literal_size = 0;
			parser->literal_nonsync = FALSE;
			break;
		case '(':
			imap_parser_open_list(parser);
			break;
		case ')':
			if (!imap_parser_close_list(parser))
				return FALSE;

			if (parser->list_arg == NULL) {
				/* end of argument */
				parser->cur_pos++;
				return TRUE;
			}
			break;
		default:
			if (!is_valid_atom_char(parser, data[0]))
				return FALSE;
			parser->cur_type = ARG_PARSE_ATOM;
			break;
		}

		parser->cur_pos++;
	}

	i_assert(data_size > 0);

	switch (parser->cur_type) {
	case ARG_PARSE_ATOM:
		if (!imap_parser_read_atom(parser, data, data_size))
			return FALSE;
		break;
	case ARG_PARSE_STRING:
		if (!imap_parser_read_string(parser, data, data_size))
			return FALSE;
		break;
	case ARG_PARSE_LITERAL:
		if (!imap_parser_read_literal(parser, data, data_size))
			return FALSE;

		/* pass through to parsing data. since input->skip was
		   modified, we need to get the data start position again. */
		data = i_stream_get_data(parser->input, &data_size);

		/* fall through */
	case ARG_PARSE_LITERAL_DATA:
		if (!imap_parser_read_literal_data(parser, data, data_size))
			return FALSE;
		break;
	default:
                i_unreached();
	}

	i_assert(parser->cur_type == ARG_PARSE_NONE);
	return TRUE;
}

/* ARG_PARSE_NONE checks that last argument isn't only partially parsed. */
#define IS_UNFINISHED(parser) \
        ((parser)->cur_type != ARG_PARSE_NONE || \
	 (parser)->cur_list != &parser->root_list)

static int finish_line(struct imap_parser *parser, unsigned int count,
		       const struct imap_arg **args_r)
{
	struct imap_arg *arg;

	parser->line_size += parser->cur_pos;
	i_stream_skip(parser->input, parser->cur_pos);
	parser->cur_pos = 0;

	if (parser->list_arg != NULL) {
		parser->error = "Missing ')'";
		*args_r = NULL;
		return -1;
	}

	/* fill the missing parameters with NILs */
	while (count > array_count(&parser->root_list)) {
		arg = array_append_space(&parser->root_list);
		arg->type = IMAP_ARG_NIL;
	}
	arg = array_append_space(&parser->root_list);
	arg->type = IMAP_ARG_EOL;

	*args_r = array_get(&parser->root_list, &count);
	return count;
}

int imap_parser_read_args(struct imap_parser *parser, unsigned int count,
			  enum imap_parser_flags flags,
			  const struct imap_arg **args_r)
{
	parser->flags = flags;

	while (!parser->eol && (count == 0 || IS_UNFINISHED(parser) ||
				array_count(&parser->root_list) < count)) {
		if (!imap_parser_read_arg(parser))
			break;

		if (parser->line_size > parser->max_line_size) {
			parser->error = "IMAP command line too large";
			break;
		}
	}

	if (parser->error != NULL) {
		/* error, abort */
		parser->line_size += parser->cur_pos;
		i_stream_skip(parser->input, parser->cur_pos);
		parser->cur_pos = 0;
		*args_r = NULL;
		return -1;
	} else if ((!IS_UNFINISHED(parser) && count > 0 &&
		    array_count(&parser->root_list) >= count) || parser->eol) {
		/* all arguments read / end of line. */
                return finish_line(parser, count, args_r);
	} else {
		/* need more data */
		*args_r = NULL;
		return -2;
	}
}

int imap_parser_finish_line(struct imap_parser *parser, unsigned int count,
			    enum imap_parser_flags flags,
			    const struct imap_arg **args_r)
{
	const unsigned char *data;
	size_t data_size;
	int ret;

	ret = imap_parser_read_args(parser, count, flags, args_r);
	if (ret == -2) {
		/* we should have noticed end of everything except atom */
		if (parser->cur_type == ARG_PARSE_ATOM) {
			data = i_stream_get_data(parser->input, &data_size);
			imap_parser_save_arg(parser, data, data_size);
		}
	}
	return finish_line(parser, count, args_r);
}

const char *imap_parser_read_word(struct imap_parser *parser)
{
	const unsigned char *data;
	size_t i, data_size;

	data = i_stream_get_data(parser->input, &data_size);

	for (i = 0; i < data_size; i++) {
		if (data[i] == ' ' || data[i] == '\r' || data[i] == '\n')
			break;
	}

	if (i < data_size) {
		data_size = i + (data[i] == ' ' ? 1 : 0);
		parser->line_size += data_size;
		i_stream_skip(parser->input, data_size);
		return p_strndup(parser->pool, data, i);
	} else {
		return NULL;
	}
}

const char *imap_arg_string(const struct imap_arg *arg)
{
	switch (arg->type) {
	case IMAP_ARG_NIL:
		return "";

	case IMAP_ARG_ATOM:
	case IMAP_ARG_STRING:
	case IMAP_ARG_LITERAL:
		return arg->_data.str;

	default:
		return NULL;
	}
}

char *imap_arg_str_error(const struct imap_arg *arg)
{
	i_panic("Tried to access imap_arg type %d as string", arg->type);
#ifndef ATTRS_DEFINED
	return NULL;
#endif
}

uoff_t imap_arg_literal_size_error(const struct imap_arg *arg)
{
	i_panic("Tried to access imap_arg type %d as literal size", arg->type);
#ifndef ATTRS_DEFINED
	return 0;
#endif
}

ARRAY_TYPE(imap_arg_list) *imap_arg_list_error(const struct imap_arg *arg)
{
	i_panic("Tried to access imap_arg type %d as list", arg->type);
#ifndef ATTRS_DEFINED
	return NULL;
#endif
}
