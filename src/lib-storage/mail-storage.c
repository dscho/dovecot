/* Copyright (c) 2002-2008 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "str.h"
#include "var-expand.h"
#include "mail-index-private.h"
#include "mailbox-list-private.h"
#include "mail-storage-private.h"
#include "mail-namespace.h"
#include "mail-search.h"

#include <stdlib.h>
#include <time.h>
#include <ctype.h>

/* Message to show to users when critical error occurs */
#define CRITICAL_MSG \
	"Internal error occurred. Refer to server log for more information."
#define CRITICAL_MSG_STAMP CRITICAL_MSG " [%Y-%m-%d %H:%M:%S]"

#define DEFAULT_MAX_KEYWORD_LENGTH 50

struct mail_storage_module_register mail_storage_module_register = { 0 };
struct mail_module_register mail_module_register = { 0 };

struct mail_storage_mail_index_module mail_storage_mail_index_module =
	MODULE_CONTEXT_INIT(&mail_index_module_register);

void (*hook_mail_storage_created)(struct mail_storage *storage);
void (*hook_mailbox_opened)(struct mailbox *box) = NULL;

static ARRAY_DEFINE(storages, struct mail_storage *);

void mail_storage_init(void)
{
	mailbox_lists_init();
	i_array_init(&storages, 8);
}

void mail_storage_deinit(void)
{
	if (array_is_created(&storages))
		array_free(&storages);
	mailbox_lists_deinit();
}

void mail_storage_class_register(struct mail_storage *storage_class)
{
	if (storage_class->v.class_init != NULL)
		storage_class->v.class_init();

	/* append it after the list, so the autodetection order is correct */
	array_append(&storages, &storage_class, 1);
}

void mail_storage_class_unregister(struct mail_storage *storage_class)
{
	struct mail_storage *const *classes;
	unsigned int i, count;

	classes = array_get(&storages, &count);
	for (i = 0; i < count; i++) {
		if (classes[i] == storage_class) {
			array_delete(&storages, i, 1);
			break;
		}
	}

	storage_class->v.class_deinit();
}

void mail_storage_parse_env(enum mail_storage_flags *flags_r,
			    enum file_lock_method *lock_method_r)
{
	const char *str;

	*flags_r = 0;
	if (getenv("FULL_FILESYSTEM_ACCESS") != NULL)
		*flags_r |= MAIL_STORAGE_FLAG_FULL_FS_ACCESS;
	if (getenv("DEBUG") != NULL)
		*flags_r |= MAIL_STORAGE_FLAG_DEBUG;
	if (getenv("MMAP_DISABLE") != NULL)
		*flags_r |= MAIL_STORAGE_FLAG_MMAP_DISABLE;
	if (getenv("MMAP_NO_WRITE") != NULL)
		*flags_r |= MAIL_STORAGE_FLAG_MMAP_NO_WRITE;
	if (getenv("DOTLOCK_USE_EXCL") != NULL)
		*flags_r |= MAIL_STORAGE_FLAG_DOTLOCK_USE_EXCL;
	if (getenv("MAIL_SAVE_CRLF") != NULL)
		*flags_r |= MAIL_STORAGE_FLAG_SAVE_CRLF;
	if (getenv("FSYNC_DISABLE") != NULL)
		*flags_r |= MAIL_STORAGE_FLAG_FSYNC_DISABLE;
	if (getenv("MAIL_NFS_STORAGE") != NULL)
		*flags_r |= MAIL_STORAGE_FLAG_NFS_FLUSH_STORAGE;
	if (getenv("MAIL_NFS_INDEX") != NULL) {
		*flags_r |= MAIL_STORAGE_FLAG_NFS_FLUSH_INDEX;
		if ((*flags_r & MAIL_STORAGE_FLAG_MMAP_DISABLE) == 0)
			i_fatal("mail_nfs_index=yes requires mmap_disable=yes");
		if ((*flags_r & MAIL_STORAGE_FLAG_FSYNC_DISABLE) != 0)
			i_fatal("mail_nfs_index=yes requires fsync_disable=no");
	}

	str = getenv("POP3_UIDL_FORMAT");
	if (str != NULL && (str = strchr(str, '%')) != NULL &&
	    str != NULL && var_get_key(str + 1) == 'm')
		*flags_r |= MAIL_STORAGE_FLAG_KEEP_HEADER_MD5;

	str = getenv("LOCK_METHOD");
	if (str == NULL || strcmp(str, "fcntl") == 0)
		*lock_method_r = FILE_LOCK_METHOD_FCNTL;
	else if (strcmp(str, "flock") == 0)
		*lock_method_r = FILE_LOCK_METHOD_FLOCK;
	else if (strcmp(str, "dotlock") == 0)
		*lock_method_r = FILE_LOCK_METHOD_DOTLOCK;
	else
		i_fatal("Unknown lock_method: %s", str);
}

static struct mail_storage *mail_storage_find(const char *name)
{
	struct mail_storage *const *classes;
	unsigned int i, count;

	i_assert(name != NULL);

	classes = array_get(&storages, &count);
	for (i = 0; i < count; i++) {
		if (strcasecmp(classes[i]->name, name) == 0)
			return classes[i];
	}
	return NULL;
}

static struct mail_storage *
mail_storage_autodetect(const char *data, enum mail_storage_flags flags)
{
	struct mail_storage *const *classes;
	unsigned int i, count;

	classes = array_get(&storages, &count);
	for (i = 0; i < count; i++) {
		if (classes[i]->v.autodetect != NULL &&
		    classes[i]->v.autodetect(data, flags))
			return classes[i];
	}
	return NULL;
}

static void
mail_storage_set_autodetection(const char **data, const char **driver,
			       enum mail_storage_flags *flags)
{
	const char *p;

	/* check if data is in driver:data format (eg. mbox:~/mail) */
	p = *data;
	while (i_isalnum(*p)) p++;

	if (*p == ':' && p != *data) {
		/* no autodetection if the storage format is given. */
		*flags |= MAIL_STORAGE_FLAG_NO_AUTODETECTION;

		*driver = t_strdup_until(*data, p);
		*data = p + 1;
	}
}

int mail_storage_create(struct mail_namespace *ns, const char *driver,
			const char *data, const char *user,
			enum mail_storage_flags flags,
			enum file_lock_method lock_method,
			const char **error_r)
{
	struct mail_storage *storage_class, *storage;
	struct mail_storage *const *classes;
	const char *home, *value;
	unsigned int i, count;

	if (data == NULL)
		data = "";
	else if (driver == NULL)
		mail_storage_set_autodetection(&data, &driver, &flags);

	if (*data == '\0' && driver == NULL) {
		/* use the first driver that works */
		classes = array_get(&storages, &count);
	} else if (driver == NULL) {
		storage_class = mail_storage_autodetect(data, flags);
		if (storage_class == NULL) {
			*error_r = t_strdup_printf(
				"Ambiguous mail location setting, "
				"don't know what to do with it: %s "
				"(try prefixing it with mbox: or maildir:)",
				data);
			return -1;
		}
		classes = &storage_class;
		count = 1;
	} else {
		storage_class = mail_storage_find(driver);
		if (storage_class == NULL) {
			*error_r = t_strdup_printf(
				"Unknown mail storage driver %s", driver);
			return -1;
		}
		classes = &storage_class;
		count = 1;
	}

	for (i = 0; i < count; i++) {
		storage = classes[i]->v.alloc();
		storage->flags = flags;
		storage->lock_method = lock_method;
		storage->user = p_strdup(storage->pool, user);
		storage->ns = ns;

		storage->callbacks =
			p_new(storage->pool, struct mail_storage_callbacks, 1);
		p_array_init(&storage->module_contexts, storage->pool, 5);

		if (classes[i]->v.create(storage, data, error_r) == 0)
			break;

		if ((flags & MAIL_STORAGE_FLAG_DEBUG) != 0 && count > 1) {
			i_info("%s: Couldn't create mail storage %s: %s",
			       classes[i]->name, data, *error_r);
		}

		/* try the next one */
		pool_unref(&storage->pool);
	}
	if (i == count) {
		if (count <= 1) {
			*error_r = t_strdup_printf("%s: %s", classes[0]->name,
						   *error_r);
			return -1;
		}

		home = getenv("HOME");
		if (home == NULL || *home == '\0') home = "(not set)";

		*error_r = t_strdup_printf(
			"Mail storage autodetection failed with home=%s", home);
		return -1;
	}

	value = getenv("MAIL_MAX_KEYWORD_LENGTH");
	storage->keyword_max_len = value != NULL ?
		atoi(value) : DEFAULT_MAX_KEYWORD_LENGTH;
	
	if (hook_mail_storage_created != NULL) {
		T_BEGIN {
			hook_mail_storage_created(storage);
		} T_END;
	}

	ns->storage = storage;
        mail_namespace_init_storage(ns);
	return 0;
}

void mail_storage_destroy(struct mail_storage **_storage)
{
	struct mail_storage *storage = *_storage;

	i_assert(storage != NULL);

	*_storage = NULL;

	storage->v.destroy(storage);
	mailbox_list_deinit(storage->list);
	i_free(storage->error_string);
	pool_unref(&storage->pool);
}

void mail_storage_clear_error(struct mail_storage *storage)
{
	i_free_and_null(storage->error_string);

	storage->error = MAIL_ERROR_NONE;
}

void mail_storage_set_error(struct mail_storage *storage,
			    enum mail_error error, const char *string)
{
	i_free(storage->error_string);
	storage->error_string = i_strdup(string);
	storage->error = error;
}

void mail_storage_set_internal_error(struct mail_storage *storage)
{
	struct tm *tm;
	char str[256];

	tm = localtime(&ioloop_time);

	i_free(storage->error_string);
	storage->error_string =
		strftime(str, sizeof(str), CRITICAL_MSG_STAMP, tm) > 0 ?
		i_strdup(str) : i_strdup(CRITICAL_MSG);
	storage->error = MAIL_ERROR_TEMP;
}

void mail_storage_set_critical(struct mail_storage *storage,
			       const char *fmt, ...)
{
	va_list va;

	mail_storage_clear_error(storage);
	if (fmt != NULL) {
		va_start(va, fmt);
		i_error("%s", t_strdup_vprintf(fmt, va));
		va_end(va);

		/* critical errors may contain sensitive data, so let user
		   see only "Internal error" with a timestamp to make it
		   easier to look from log files the actual error message. */
		mail_storage_set_internal_error(storage);
	}
}

char mail_storage_get_hierarchy_sep(struct mail_storage *storage)
{
	return mailbox_list_get_hierarchy_sep(storage->list);
}

struct mailbox_list *mail_storage_get_list(struct mail_storage *storage)
{
	return storage->list;
}

struct mail_namespace *mail_storage_get_namespace(struct mail_storage *storage)
{
	return storage->ns;
}

void mail_storage_set_callbacks(struct mail_storage *storage,
				struct mail_storage_callbacks *callbacks,
				void *context)
{
	*storage->callbacks = *callbacks;
	storage->callback_context = context;
}

int mail_storage_mailbox_create(struct mail_storage *storage, const char *name,
				bool directory)
{
	mail_storage_clear_error(storage);

	if (!mailbox_list_is_valid_create_name(storage->list, name)) {
		mail_storage_set_error(storage, MAIL_ERROR_PARAMS,
				       "Invalid mailbox name");
		return -1;
	}

	return storage->v.mailbox_create(storage, name, directory);
}

const char *mail_storage_get_last_error(struct mail_storage *storage,
					enum mail_error *error_r)
{
	/* We get here only in error situations, so we have to return some
	   error. If storage->error is NONE, it means we forgot to set it at
	   some point.. */
	if (storage->error == MAIL_ERROR_NONE) {
		*error_r = MAIL_ERROR_TEMP;
		return storage->error_string != NULL ? storage->error_string :
			"BUG: Unknown internal error";
	}

	if (storage->error_string == NULL) {
		/* This shouldn't happen.. */
		storage->error_string =
			i_strdup_printf("BUG: Unknown 0x%x error",
					storage->error);
	}

	*error_r = storage->error;
	return storage->error_string;
}

const char *mail_storage_get_mailbox_path(struct mail_storage *storage,
					  const char *name, bool *is_file_r)
{
	*is_file_r = storage->mailbox_is_file;

	if (*name == '\0')
		name = NULL;

	return mailbox_list_get_path(storage->list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
}

const char *mail_storage_get_mailbox_control_dir(struct mail_storage *storage,
						 const char *name)
{
	if (*name == '\0')
		name = NULL;

	return mailbox_list_get_path(storage->list, name,
				     MAILBOX_LIST_PATH_TYPE_CONTROL);
}

const char *mail_storage_get_mailbox_index_dir(struct mail_storage *storage,
					       const char *name)
{
	if (*name == '\0')
		name = NULL;

	return mailbox_list_get_path(storage->list, name,
				     MAILBOX_LIST_PATH_TYPE_INDEX);
}

enum mailbox_list_flags
mail_storage_get_list_flags(enum mail_storage_flags storage_flags)
{
	enum mailbox_list_flags list_flags = 0;

	if ((storage_flags & MAIL_STORAGE_FLAG_DEBUG) != 0)
		list_flags |= MAILBOX_LIST_FLAG_DEBUG;
	if ((storage_flags & MAIL_STORAGE_FLAG_FULL_FS_ACCESS) != 0)
		list_flags |= MAILBOX_LIST_FLAG_FULL_FS_ACCESS;
	if ((storage_flags & MAIL_STORAGE_FLAG_DOTLOCK_USE_EXCL) != 0)
		list_flags |= MAILBOX_LIST_FLAG_DOTLOCK_USE_EXCL;
	if ((storage_flags & MAIL_STORAGE_FLAG_NFS_FLUSH_STORAGE) != 0)
		list_flags |= MAILBOX_LIST_FLAG_NFS_FLUSH;
	return list_flags;
}

bool mail_storage_set_error_from_errno(struct mail_storage *storage)
{
	const char *error_string;
	enum mail_error error;

	if (!mail_error_from_errno(&error, &error_string))
		return FALSE;

	mail_storage_set_error(storage, error, error_string);
	return TRUE;
}

struct mailbox *mailbox_open(struct mail_storage *storage, const char *name,
			     struct istream *input,
			     enum mailbox_open_flags flags)
{
	struct mailbox *box;

	mail_storage_clear_error(storage);

	if (!mailbox_list_is_valid_existing_name(storage->list, name)) {
		mail_storage_set_error(storage, MAIL_ERROR_PARAMS,
				       "Invalid mailbox name");
		return NULL;
	}

	T_BEGIN {
		box = storage->v.mailbox_open(storage, name, input, flags);
		if (hook_mailbox_opened != NULL && box != NULL)
			hook_mailbox_opened(box);
	} T_END;
	return box;
}

int mailbox_close(struct mailbox **_box)
{
	struct mailbox *box = *_box;

	if (box->transaction_count != 0) {
		i_panic("Trying to close mailbox %s with open transactions",
			box->name);
	}

	*_box = NULL;
	return box->v.close(box);
}

struct mail_storage *mailbox_get_storage(struct mailbox *box)
{
	return box->storage;
}

const char *mailbox_get_name(struct mailbox *box)
{
	return box->name;
}

bool mailbox_is_readonly(struct mailbox *box)
{
	return box->v.is_readonly(box);
}

bool mailbox_allow_new_keywords(struct mailbox *box)
{
	return box->v.allow_new_keywords(box);
}

void mailbox_get_status(struct mailbox *box,
			enum mailbox_status_items items,
			struct mailbox_status *status_r)
{
	box->v.get_status(box, items, status_r);
}

struct mailbox_sync_context *
mailbox_sync_init(struct mailbox *box, enum mailbox_sync_flags flags)
{
	return box->v.sync_init(box, flags);
}

bool mailbox_sync_next(struct mailbox_sync_context *ctx,
		       struct mailbox_sync_rec *sync_rec_r)
{
	return ctx->box->v.sync_next(ctx, sync_rec_r);
}

int mailbox_sync_deinit(struct mailbox_sync_context **_ctx,
			enum mailbox_status_items status_items,
			struct mailbox_status *status_r)
{
	struct mailbox_sync_context *ctx = *_ctx;

	*_ctx = NULL;
	return ctx->box->v.sync_deinit(ctx, status_items, status_r);
}

int mailbox_sync(struct mailbox *box, enum mailbox_sync_flags flags,
		 enum mailbox_status_items status_items,
		 struct mailbox_status *status_r)
{
	struct mailbox_sync_context *ctx;
        struct mailbox_sync_rec sync_rec;

	/* we don't care about mailbox's current state, so we might as well
	   fix inconsistency state */
	flags |= MAILBOX_SYNC_FLAG_FIX_INCONSISTENT;

	ctx = mailbox_sync_init(box, flags);
	while (mailbox_sync_next(ctx, &sync_rec))
		;
	return mailbox_sync_deinit(&ctx, status_items, status_r);
}

#undef mailbox_notify_changes
void mailbox_notify_changes(struct mailbox *box, unsigned int min_interval,
			    mailbox_notify_callback_t *callback, void *context)
{
	box->notify_min_interval = min_interval;
	box->notify_callback = callback;
	box->notify_context = context;

	box->v.notify_changes(box);
}

void mailbox_notify_changes_stop(struct mailbox *box)
{

	mailbox_notify_changes(box, 0, NULL, NULL);
}

int mailbox_keywords_create(struct mailbox *box, const char *const keywords[],
			    struct mail_keywords **keywords_r)
{
	const char *empty_keyword_list = NULL;

	if (keywords == NULL)
		keywords = &empty_keyword_list;
	return box->v.keywords_create(box, keywords, keywords_r, FALSE);
}

struct mail_keywords *
mailbox_keywords_create_valid(struct mailbox *box,
			      const char *const keywords[])
{
	const char *empty_keyword_list = NULL;
	struct mail_keywords *kw;

	if (keywords == NULL)
		keywords = &empty_keyword_list;
	if (box->v.keywords_create(box, keywords, &kw, TRUE) < 0)
		i_unreached();
	return kw;
}

void mailbox_keywords_free(struct mailbox *box,
			   struct mail_keywords **_keywords)
{
	struct mail_keywords *keywords = *_keywords;

	*_keywords = NULL;
	box->v.keywords_free(keywords);
}

void mailbox_get_uids(struct mailbox *box, uint32_t uid1, uint32_t uid2,
		      uint32_t *seq1_r, uint32_t *seq2_r)
{
	box->v.get_uids(box, uid1, uid2, seq1_r, seq2_r);
}

struct mailbox_header_lookup_ctx *
mailbox_header_lookup_init(struct mailbox *box, const char *const headers[])
{
	return box->v.header_lookup_init(box, headers);
}

void mailbox_header_lookup_deinit(struct mailbox_header_lookup_ctx **_ctx)
{
	struct mailbox_header_lookup_ctx *ctx = *_ctx;

	*_ctx = NULL;
	ctx->box->v.header_lookup_deinit(ctx);
}

struct mail_search_context *
mailbox_search_init(struct mailbox_transaction_context *t,
		    const char *charset, struct mail_search_arg *args,
		    const enum mail_sort_type *sort_program)
{
	mail_search_args_simplify(args);
	return t->box->v.search_init(t, charset, args, sort_program);
}

int mailbox_search_deinit(struct mail_search_context **_ctx)
{
	struct mail_search_context *ctx = *_ctx;

	*_ctx = NULL;
	return ctx->transaction->box->v.search_deinit(ctx);
}

int mailbox_search_next(struct mail_search_context *ctx, struct mail *mail)
{
	bool tryagain;
	int ret;

	while ((ret = mailbox_search_next_nonblock(ctx, mail,
						   &tryagain)) == 0) {
		if (!tryagain)
			break;
	}

	return ret;
}

int mailbox_search_next_nonblock(struct mail_search_context *ctx,
				 struct mail *mail, bool *tryagain_r)
{
	return ctx->transaction->box->v.
		search_next_nonblock(ctx, mail, tryagain_r);
}

struct mailbox_transaction_context *
mailbox_transaction_begin(struct mailbox *box,
			  enum mailbox_transaction_flags flags)
{
	struct mailbox_transaction_context *trans;

	box->transaction_count++;
	trans = box->v.transaction_begin(box, flags);
	trans->flags = flags;
	return trans;
}

int mailbox_transaction_commit(struct mailbox_transaction_context **t)
{
	uint32_t tmp;

	return mailbox_transaction_commit_get_uids(t, &tmp, &tmp, &tmp);
}

int mailbox_transaction_commit_get_uids(struct mailbox_transaction_context **_t,
					uint32_t *uid_validity_r,
					uint32_t *first_saved_uid_r,
					uint32_t *last_saved_uid_r)
{
	struct mailbox_transaction_context *t = *_t;

	t->box->transaction_count--;

	*_t = NULL;
	return t->box->v.transaction_commit(t, uid_validity_r,
					    first_saved_uid_r,
					    last_saved_uid_r);
}

void mailbox_transaction_rollback(struct mailbox_transaction_context **_t)
{
	struct mailbox_transaction_context *t = *_t;

	t->box->transaction_count--;

	*_t = NULL;
	t->box->v.transaction_rollback(t);
}

unsigned int mailbox_transaction_get_count(struct mailbox *box)
{
	return box->transaction_count;
}

struct mailbox *
mailbox_transaction_get_mailbox(struct mailbox_transaction_context *t)
{
	return t->box;
}

int mailbox_save_init(struct mailbox_transaction_context *t,
		      enum mail_flags flags, struct mail_keywords *keywords,
		      time_t received_date, int timezone_offset,
		      const char *from_envelope, struct istream *input,
		      struct mail *dest_mail, struct mail_save_context **ctx_r)
{
	if (t->box->v.save_init == NULL) {
		mail_storage_set_error(t->box->storage, MAIL_ERROR_NOTPOSSIBLE,
				       "Saving messages not supported");
		return -1;
	}
	if (t->box->v.save_init(t, flags, keywords,
				received_date, timezone_offset,
				from_envelope, input, dest_mail, ctx_r) < 0)
		return -1;

	(*ctx_r)->dest_mail = dest_mail;
	return 0;
}

int mailbox_save_continue(struct mail_save_context *ctx)
{
	return ctx->transaction->box->v.save_continue(ctx);
}

int mailbox_save_finish(struct mail_save_context **_ctx)
{
	struct mail_save_context *ctx = *_ctx;

	*_ctx = NULL;
	return ctx->transaction->box->v.save_finish(ctx);
}

void mailbox_save_cancel(struct mail_save_context **_ctx)
{
	struct mail_save_context *ctx = *_ctx;

	*_ctx = NULL;
	ctx->transaction->box->v.save_cancel(ctx);
}

int mailbox_copy(struct mailbox_transaction_context *t, struct mail *mail,
		 enum mail_flags flags, struct mail_keywords *keywords,
		 struct mail *dest_mail)
{
	return t->box->v.copy(t, mail, flags, keywords, dest_mail);
}

bool mailbox_is_inconsistent(struct mailbox *box)
{
	return box->mailbox_deleted || box->v.is_inconsistent(box);
}

void mailbox_set_deleted(struct mailbox *box)
{
	mail_storage_set_error(box->storage, MAIL_ERROR_NOTFOUND,
			       "Mailbox was deleted under us");
	box->mailbox_deleted = TRUE;
}

const char *mail_storage_eacces_msg(const char *func, const char *path)
{
	const char *prev_path = path, *dir = "/", *p;
	string_t *errmsg = t_str_new(256);
	struct stat st;
	int ret = -1;

	str_printfa(errmsg, "%s(%s) failed: Permission denied (euid=%s egid=%s",
		    func, path, dec2str(geteuid()), dec2str(getegid()));
	while ((p = strrchr(prev_path, '/')) != NULL) {
		dir = t_strdup_until(prev_path, p);
		ret = stat(dir, &st);
		if (ret == 0 || errno != EACCES)
			break;
		prev_path = dir;
		dir = "/";
	}

	if (ret == 0) {
		if (access(dir, X_OK) < 0 && errno == EACCES)
			str_printfa(errmsg, " missing +x perm: %s", dir);
		else if (prev_path == path &&
			 access(path, R_OK) < 0 && errno == EACCES)
			str_printfa(errmsg, " missing +r perm: %s", path);
	}
	str_append_c(errmsg, ')');
	return str_c(errmsg);
}
