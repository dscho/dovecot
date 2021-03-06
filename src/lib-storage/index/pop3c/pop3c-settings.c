/* Copyright (c) 2011-2015 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "settings-parser.h"
#include "mail-storage-settings.h"
#include "pop3c-settings.h"

#include <stddef.h>

#undef DEF
#define DEF(type, name) \
	{ type, #name, offsetof(struct pop3c_settings, name), NULL }

static const struct setting_define pop3c_setting_defines[] = {
	DEF(SET_STR, pop3c_host),
	DEF(SET_IN_PORT, pop3c_port),

	DEF(SET_STR_VARS, pop3c_user),
	DEF(SET_STR_VARS, pop3c_master_user),
	DEF(SET_STR, pop3c_password),

	DEF(SET_ENUM, pop3c_ssl),
	DEF(SET_BOOL, pop3c_ssl_verify),

	DEF(SET_STR, pop3c_rawlog_dir),
	DEF(SET_BOOL, pop3c_quick_received_date),

	SETTING_DEFINE_LIST_END
};

static const struct pop3c_settings pop3c_default_settings = {
	.pop3c_host = "",
	.pop3c_port = 110,

	.pop3c_user = "%u",
	.pop3c_master_user = "",
	.pop3c_password = "",

	.pop3c_ssl = "no:pop3s:starttls",
	.pop3c_ssl_verify = TRUE,

	.pop3c_rawlog_dir = "",
	.pop3c_quick_received_date = FALSE
};

static const struct setting_parser_info pop3c_setting_parser_info = {
	.module_name = "pop3c",
	.defines = pop3c_setting_defines,
	.defaults = &pop3c_default_settings,

	.type_offset = (size_t)-1,
	.struct_size = sizeof(struct pop3c_settings),

	.parent_offset = (size_t)-1,
	.parent = &mail_user_setting_parser_info,
};

const struct setting_parser_info *pop3c_get_setting_parser_info(void)
{
	return &pop3c_setting_parser_info;
}
