/* Copyright (c) 2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "str.h"
#include "hex-binary.h"
#include "network.h"
#include "istream.h"
#include "mailbox-log.h"
#include "mail-user.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-search-build.h"
#include "dsync-worker-private.h"

struct local_dsync_worker_mailbox_iter {
	struct dsync_worker_mailbox_iter iter;
	struct mailbox_list_iterate_context *list_iter;
	struct hash_iterate_context *deleted_iter;
};

struct local_dsync_worker_subs_iter {
	struct dsync_worker_subs_iter iter;
	struct mailbox_list_iterate_context *list_iter;
	struct hash_iterate_context *deleted_iter;
};

struct local_dsync_worker_msg_iter {
	struct dsync_worker_msg_iter iter;
	mailbox_guid_t *mailboxes;
	unsigned int mailbox_idx, mailbox_count;

	struct mail_search_context *search_ctx;
	struct mail *mail;

	string_t *tmp_guid_str;
	ARRAY_TYPE(mailbox_expunge_rec) expunges;
	unsigned int expunge_idx;
	unsigned int expunges_set:1;
};

struct local_dsync_mailbox {
	struct mail_namespace *ns;
	mailbox_guid_t guid;
	const char *storage_name;
};

struct local_dsync_mailbox_change {
	mailbox_guid_t guid;
	time_t last_renamed;
	unsigned int deleted_mailbox:1;
	unsigned int deleted_dir:1;
};
struct local_dsync_subscription_change {
	mailbox_guid_t name_sha1;
	struct mailbox_list *list;
	time_t last_change;
	unsigned int unsubscribed:1;
};

struct local_dsync_worker {
	struct dsync_worker worker;
	struct mail_user *user;

	pool_t pool;
	/* mailbox_guid_t -> struct local_dsync_mailbox* */
	struct hash_table *mailbox_hash;
	/* mailbox_guid_t -> struct local_dsync_mailbox_change* */
	struct hash_table *mailbox_changes_hash;
	/* mailbox_guid_t -> struct local_dsync_subscription_change */
	struct hash_table *subscription_changes_hash;

	mailbox_guid_t selected_box_guid;
	struct mailbox *selected_box;
	struct mail *mail, *ext_mail;

	mailbox_guid_t get_mailbox;
	struct mail *get_mail;

	struct io *save_io;
	struct mail_save_context *save_ctx;
	struct istream *save_input;

	dsync_worker_finish_callback_t *finish_callback;
	void *finish_context;

	unsigned int reading_mail:1;
};

extern struct dsync_worker_vfuncs local_dsync_worker;

static void local_worker_mailbox_close(struct local_dsync_worker *worker);
static void local_worker_msg_box_close(struct local_dsync_worker *worker);

static int mailbox_guid_cmp(const void *p1, const void *p2)
{
	const mailbox_guid_t *g1 = p1, *g2 = p2;

	return memcmp(g1->guid, g2->guid, sizeof(g1->guid));
}

static unsigned int mailbox_guid_hash(const void *p)
{
	const mailbox_guid_t *guid = p;
        const uint8_t *s = guid->guid;
	unsigned int i, g, h = 0;

	for (i = 0; i < sizeof(guid->guid); i++) {
		h = (h << 4) + s[i];
		if ((g = h & 0xf0000000UL)) {
			h = h ^ (g >> 24);
			h = h ^ g;
		}
	}
	return h;
}

struct dsync_worker *dsync_worker_init_local(struct mail_user *user)
{
	struct local_dsync_worker *worker;
	pool_t pool;

	pool = pool_alloconly_create("local dsync worker", 10240);
	worker = p_new(pool, struct local_dsync_worker, 1);
	worker->worker.v = local_dsync_worker;
	worker->user = user;
	worker->pool = pool;
	worker->mailbox_hash =
		hash_table_create(default_pool, pool, 0,
				  mailbox_guid_hash, mailbox_guid_cmp);
	return &worker->worker;
}

static void local_worker_deinit(struct dsync_worker *_worker)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;

	i_assert(worker->save_input == NULL);

	local_worker_msg_box_close(worker);
	local_worker_mailbox_close(worker);
	hash_table_destroy(&worker->mailbox_hash);
	if (worker->mailbox_changes_hash != NULL)
		hash_table_destroy(&worker->mailbox_changes_hash);
	if (worker->subscription_changes_hash != NULL)
		hash_table_destroy(&worker->subscription_changes_hash);
	pool_unref(&worker->pool);
}

static bool local_worker_is_output_full(struct dsync_worker *worker ATTR_UNUSED)
{
	return FALSE;
}

static int local_worker_output_flush(struct dsync_worker *worker ATTR_UNUSED)
{
	return 1;
}

static void
dsync_worker_save_mailbox_change(struct local_dsync_worker *worker,
				 const struct mailbox_log_record *rec)
{
	struct local_dsync_mailbox_change *change;

	change = hash_table_lookup(worker->mailbox_changes_hash,
				   rec->mailbox_guid);
	if (change == NULL) {
		change = i_new(struct local_dsync_mailbox_change, 1);
		memcpy(change->guid.guid, rec->mailbox_guid,
		       sizeof(change->guid.guid));
		hash_table_insert(worker->mailbox_changes_hash,
				  change->guid.guid, change);
	}
	switch (rec->type) {
	case MAILBOX_LOG_RECORD_DELETE_MAILBOX:
		change->deleted_mailbox = TRUE;
		break;
	case MAILBOX_LOG_RECORD_DELETE_DIR:
		change->deleted_dir = TRUE;
		break;
	case MAILBOX_LOG_RECORD_RENAME:
		change->last_renamed =
			mailbox_log_record_get_timestamp(rec);
		break;
	case MAILBOX_LOG_RECORD_SUBSCRIBE:
	case MAILBOX_LOG_RECORD_UNSUBSCRIBE:
		i_unreached();
	}
	if (change->deleted_dir && change->deleted_mailbox) {
		/* same GUID shouldn't be both. something's already
		   broken, but change this so we don't get into more
		   problems later. */
		change->deleted_dir = FALSE;
	}
}

static void
dsync_worker_save_subscription_change(struct local_dsync_worker *worker,
				      struct mailbox_list *list,
				      const struct mailbox_log_record *rec)
{
	struct local_dsync_subscription_change *change, new_change;

	memset(&new_change, 0, sizeof(new_change));
	new_change.list = list;
	memcpy(new_change.name_sha1.guid, rec->mailbox_guid,
	       sizeof(new_change.name_sha1.guid));

	change = hash_table_lookup(worker->subscription_changes_hash,
				   &new_change);
	if (change == NULL) {
		change = i_new(struct local_dsync_subscription_change, 1);
		*change = new_change;
		hash_table_insert(worker->subscription_changes_hash,
				  change, change);
	}
	switch (rec->type) {
	case MAILBOX_LOG_RECORD_DELETE_MAILBOX:
	case MAILBOX_LOG_RECORD_DELETE_DIR:
	case MAILBOX_LOG_RECORD_RENAME:
		i_unreached();
	case MAILBOX_LOG_RECORD_SUBSCRIBE:
		change->unsubscribed = FALSE;
		break;
	case MAILBOX_LOG_RECORD_UNSUBSCRIBE:
		change->unsubscribed = TRUE;
		break;
	}
	change->last_change = mailbox_log_record_get_timestamp(rec);
}

static int
dsync_worker_get_list_mailbox_log(struct local_dsync_worker *worker,
				  struct mailbox_list *list)
{
	struct mailbox_log *log;
	struct mailbox_log_iter *iter;
	const struct mailbox_log_record *rec;

	log = mailbox_list_get_changelog(list);
	iter = mailbox_log_iter_init(log);
	while ((rec = mailbox_log_iter_next(iter)) != NULL) {
		switch (rec->type) {
		case MAILBOX_LOG_RECORD_DELETE_MAILBOX:
		case MAILBOX_LOG_RECORD_DELETE_DIR:
		case MAILBOX_LOG_RECORD_RENAME:
			dsync_worker_save_mailbox_change(worker, rec);
			break;
		case MAILBOX_LOG_RECORD_SUBSCRIBE:
		case MAILBOX_LOG_RECORD_UNSUBSCRIBE:
			dsync_worker_save_subscription_change(worker,
							      list, rec);
			break;
		}
	}
	return mailbox_log_iter_deinit(&iter);
}

static unsigned int mailbox_log_record_hash(const void *p)
{
	const uint8_t *guid = p;

	return ((unsigned int)guid[0] << 24) |
		((unsigned int)guid[1] << 16) |
		((unsigned int)guid[2] << 8) |
		(unsigned int)guid[3];
}

static int mailbox_log_record_cmp(const void *p1, const void *p2)
{
	return memcmp(p1, p2, MAIL_GUID_128_SIZE);
}

static unsigned int subscription_change_hash(const void *p)
{
	const struct local_dsync_subscription_change *change = p;

	return mailbox_log_record_hash(change->name_sha1.guid) ^
		POINTER_CAST_TO(change->list, unsigned int);
}

static int subscription_change_cmp(const void *p1, const void *p2)
{
	const struct local_dsync_subscription_change *c1 = p1, *c2 = p2;

	if (c1->list != c2->list)
		return 1;

	return memcmp(c1->name_sha1.guid, c2->name_sha1.guid,
		      MAIL_GUID_128_SIZE);
}

static int dsync_worker_get_mailbox_log(struct local_dsync_worker *worker)
{
	struct mail_namespace *ns;
	int ret = 0;

	if (worker->mailbox_changes_hash != NULL)
		return 0;

	worker->mailbox_changes_hash =
		hash_table_create(default_pool, worker->pool, 0,
				  mailbox_log_record_hash,
				  mailbox_log_record_cmp);
	worker->subscription_changes_hash =
		hash_table_create(default_pool, worker->pool, 0,
				  subscription_change_hash,
				  subscription_change_cmp);
	for (ns = worker->user->namespaces; ns != NULL; ns = ns->next) {
		if (ns->alias_for != NULL)
			continue;

		if (dsync_worker_get_list_mailbox_log(worker, ns->list) < 0)
			ret = -1;
	}
	return ret;
}

static struct dsync_worker_mailbox_iter *
local_worker_mailbox_iter_init(struct dsync_worker *_worker)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct local_dsync_worker_mailbox_iter *iter;
	enum mailbox_list_iter_flags list_flags =
		MAILBOX_LIST_ITER_VIRTUAL_NAMES |
		MAILBOX_LIST_ITER_SKIP_ALIASES |
		MAILBOX_LIST_ITER_NO_AUTO_INBOX;
	static const char *patterns[] = { "*", NULL };

	iter = i_new(struct local_dsync_worker_mailbox_iter, 1);
	iter->iter.worker = _worker;
	iter->list_iter =
		mailbox_list_iter_init_namespaces(worker->user->namespaces,
						  patterns, list_flags);
	(void)dsync_worker_get_mailbox_log(worker);
	return &iter->iter;
}

static void
local_dsync_worker_add_mailbox(struct local_dsync_worker *worker,
			       struct mail_namespace *ns,
			       const char *storage_name,
			       const mailbox_guid_t *guid)
{
	struct local_dsync_mailbox *lbox;

	lbox = p_new(worker->pool, struct local_dsync_mailbox, 1);
	lbox->ns = ns;
	memcpy(lbox->guid.guid, guid->guid, sizeof(lbox->guid.guid));
	lbox->storage_name = p_strdup(worker->pool, storage_name);

	hash_table_insert(worker->mailbox_hash, &lbox->guid, lbox);
}

static int
iter_next_deleted(struct local_dsync_worker_mailbox_iter *iter,
		  struct local_dsync_worker *worker,
		  struct dsync_mailbox *dsync_box_r)
{
	const struct local_dsync_mailbox_change *change;
	void *key, *value;

	if (iter->deleted_iter == NULL) {
		iter->deleted_iter =
			hash_table_iterate_init(worker->mailbox_changes_hash);
	}
	while (hash_table_iterate(iter->deleted_iter, &key, &value)) {
		change = value;
		if (change->deleted_mailbox) {
			/* the name doesn't matter */
			dsync_box_r->name = "";
			dsync_box_r->mailbox_guid = change->guid;
			dsync_box_r->flags |=
				DSYNC_MAILBOX_FLAG_DELETED_MAILBOX;
			return 1;
		}
		if (change->deleted_dir) {
			/* the name doesn't matter */
			dsync_box_r->name = "";
			dsync_box_r->dir_guid = change->guid;
			dsync_box_r->flags |= DSYNC_MAILBOX_FLAG_DELETED_DIR;
			return 1;
		}
	}
	hash_table_iterate_deinit(&iter->deleted_iter);
	return -1;
}

static int
local_worker_mailbox_iter_next(struct dsync_worker_mailbox_iter *_iter,
			       struct dsync_mailbox *dsync_box_r)
{
	struct local_dsync_worker_mailbox_iter *iter =
		(struct local_dsync_worker_mailbox_iter *)_iter;
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_iter->worker;
	enum mailbox_flags flags =
		MAILBOX_FLAG_READONLY | MAILBOX_FLAG_KEEP_RECENT;
	const struct mailbox_info *info;
	const char *storage_name;
	struct mailbox *box;
	struct mailbox_status status;
	struct local_dsync_mailbox_change *change;
	const char *const *fields;
	unsigned int i, field_count;

	memset(dsync_box_r, 0, sizeof(*dsync_box_r));

	info = mailbox_list_iter_next(iter->list_iter);
	if (info == NULL)
		return iter_next_deleted(iter, worker, dsync_box_r);

	storage_name = mail_namespace_get_storage_name(info->ns, info->name);
	dsync_box_r->name = info->name;
	if (mailbox_list_get_guid(info->ns->list, storage_name,
				  dsync_box_r->dir_guid.guid) < 0) {
		i_error("Failed to get dir GUID for mailbox %s: %s", info->name,
			mailbox_list_get_last_error(info->ns->list, NULL));
		_iter->failed = TRUE;
		return -1;
	}

	/* get last rename timestamp */
	change = hash_table_lookup(worker->mailbox_changes_hash,
				   dsync_box_r->dir_guid.guid);
	if (change != NULL) {
		/* it shouldn't be marked as deleted, but drop it to be sure */
		change->deleted_dir = FALSE;
		dsync_box_r->last_renamed = change->last_renamed;
	}

	storage_name = mail_namespace_get_storage_name(info->ns, info->name);
	if ((info->flags & MAILBOX_NOSELECT) != 0) {
		local_dsync_worker_add_mailbox(worker, info->ns, storage_name,
					       &dsync_box_r->dir_guid);
		return 1;
	}

	box = mailbox_alloc(info->ns->list, storage_name, NULL, flags);
	if (mailbox_sync(box, 0, 0, NULL) < 0) {
		struct mail_storage *storage = mailbox_get_storage(box);

		i_error("Failed to sync mailbox %s: %s", info->name,
			mail_storage_get_last_error(storage, NULL));
		mailbox_close(&box);
		_iter->failed = TRUE;
		return -1;
	}

	mailbox_get_status(box, STATUS_UIDNEXT | STATUS_UIDVALIDITY |
			   STATUS_HIGHESTMODSEQ | STATUS_GUID |
			   STATUS_CACHE_FIELDS, &status);

	change = hash_table_lookup(worker->mailbox_changes_hash,
				   status.mailbox_guid);
	if (change != NULL) {
		/* it shouldn't be marked as deleted, but drop it to be sure */
		change->deleted_mailbox = FALSE;
	}

	memcpy(dsync_box_r->mailbox_guid.guid, status.mailbox_guid,
	       sizeof(dsync_box_r->mailbox_guid.guid));
	dsync_box_r->uid_validity = status.uidvalidity;
	dsync_box_r->uid_next = status.uidnext;
	dsync_box_r->highest_modseq = status.highest_modseq;

	fields = array_get(status.cache_fields, &field_count);
	t_array_init(&dsync_box_r->cache_fields, field_count);
	for (i = 0; i < field_count; i++) {
		const char *field_name = t_strdup(fields[i]);
		array_append(&dsync_box_r->cache_fields, &field_name, 1);
	}

	local_dsync_worker_add_mailbox(worker, info->ns, storage_name,
				       &dsync_box_r->mailbox_guid);
	mailbox_close(&box);
	return 1;
}

static int
local_worker_mailbox_iter_deinit(struct dsync_worker_mailbox_iter *_iter)
{
	struct local_dsync_worker_mailbox_iter *iter =
		(struct local_dsync_worker_mailbox_iter *)_iter;
	int ret = _iter->failed ? -1 : 0;

	if (mailbox_list_iter_deinit(&iter->list_iter) < 0)
		ret = -1;
	i_free(iter);
	return ret;
}

static struct dsync_worker_subs_iter *
local_worker_subs_iter_init(struct dsync_worker *_worker)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct local_dsync_worker_subs_iter *iter;
	enum mailbox_list_iter_flags list_flags =
		MAILBOX_LIST_ITER_VIRTUAL_NAMES |
		MAILBOX_LIST_ITER_SELECT_SUBSCRIBED;
	static const char *patterns[] = { "*", NULL };

	iter = i_new(struct local_dsync_worker_subs_iter, 1);
	iter->iter.worker = _worker;
	iter->list_iter =
		mailbox_list_iter_init_namespaces(worker->user->namespaces,
						  patterns, list_flags);
	(void)dsync_worker_get_mailbox_log(worker);
	return &iter->iter;
}

static int
local_worker_subs_iter_next(struct dsync_worker_subs_iter *_iter,
			    struct dsync_worker_subscription *rec_r)
{
	struct local_dsync_worker_subs_iter *iter =
		(struct local_dsync_worker_subs_iter *)_iter;
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_iter->worker;
	struct local_dsync_subscription_change *change, change_lookup;
	const struct mailbox_info *info;
	const char *storage_name;

	memset(rec_r, 0, sizeof(*rec_r));

	info = mailbox_list_iter_next(iter->list_iter);
	if (info == NULL)
		return -1;

	storage_name = mail_namespace_get_storage_name(info->ns, info->name);
	dsync_str_sha_to_guid(storage_name, &change_lookup.name_sha1);
	change_lookup.list = info->ns->list;

	change = hash_table_lookup(worker->subscription_changes_hash,
				   &change_lookup);
	if (change != NULL) {
		/* it shouldn't be marked as unsubscribed, but drop it to
		   be sure */
		change->unsubscribed = FALSE;
		rec_r->last_change = change->last_change;
	}
	rec_r->ns_prefix = info->ns->prefix;
	rec_r->vname = info->name;
	rec_r->storage_name = storage_name;
	return 1;
}

static int
local_worker_subs_iter_next_un(struct dsync_worker_subs_iter *_iter,
			       struct dsync_worker_unsubscription *rec_r)
{
	struct local_dsync_worker_subs_iter *iter =
		(struct local_dsync_worker_subs_iter *)_iter;
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_iter->worker;
	void *key, *value;

	if (iter->deleted_iter == NULL) {
		iter->deleted_iter =
			hash_table_iterate_init(worker->subscription_changes_hash);
	}
	while (hash_table_iterate(iter->deleted_iter, &key, &value)) {
		const struct local_dsync_subscription_change *change = value;

		if (change->unsubscribed) {
			/* the name doesn't matter */
			struct mail_namespace *ns =
				mailbox_list_get_namespace(change->list);
			memset(rec_r, 0, sizeof(*rec_r));
			rec_r->name_sha1 = change->name_sha1;
			rec_r->ns_prefix = ns->prefix;
			rec_r->last_change = change->last_change;
			return 1;
		}
	}
	hash_table_iterate_deinit(&iter->deleted_iter);
	return -1;
}

static int
local_worker_subs_iter_deinit(struct dsync_worker_subs_iter *_iter)
{
	struct local_dsync_worker_subs_iter *iter =
		(struct local_dsync_worker_subs_iter *)_iter;
	int ret = _iter->failed ? -1 : 0;

	if (mailbox_list_iter_deinit(&iter->list_iter) < 0)
		ret = -1;
	i_free(iter);
	return ret;
}

static void
local_worker_set_subscribed(struct dsync_worker *_worker,
			    const char *name, bool set)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mail_namespace *ns;
	const char *storage_name;

	storage_name = name;
	ns = mail_namespace_find(worker->user->namespaces, &storage_name);
	if (ns == NULL) {
		i_error("Can't find namespace for mailbox %s", name);
		return;
	}

	if (mailbox_list_set_subscribed(ns->list, storage_name, set) < 0) {
		dsync_worker_set_failure(_worker);
		i_error("Can't update subscription %s: %s", name,
			mailbox_list_get_last_error(ns->list, NULL));
	}
}

static int local_mailbox_open(struct local_dsync_worker *worker,
			      const mailbox_guid_t *guid,
			      struct mailbox **box_r)
{
	enum mailbox_flags flags = MAILBOX_FLAG_KEEP_RECENT;
	struct local_dsync_mailbox *lbox;
	struct mailbox *box;
	struct mailbox_status status;

	lbox = hash_table_lookup(worker->mailbox_hash, guid);
	if (lbox == NULL) {
		i_error("Trying to open a non-listed mailbox with guid=%s",
			binary_to_hex(guid->guid, sizeof(guid->guid)));
		return -1;
	}

	box = mailbox_alloc(lbox->ns->list, lbox->storage_name, NULL, flags);
	if (mailbox_sync(box, 0, 0, NULL) < 0) {
		struct mail_storage *storage = mailbox_get_storage(box);

		i_error("Failed to sync mailbox %s: %s", lbox->storage_name,
			mail_storage_get_last_error(storage, NULL));
		mailbox_close(&box);
		return -1;
	}

	mailbox_get_status(box, STATUS_GUID, &status);
	if (memcmp(status.mailbox_guid, guid->guid, sizeof(guid->guid)) != 0) {
		i_error("Mailbox %s changed its GUID (%s -> %s)",
			lbox->storage_name, dsync_guid_to_str(guid),
			binary_to_hex(status.mailbox_guid,
				      sizeof(status.mailbox_guid)));
		mailbox_close(&box);
		return -1;
	}
	*box_r = box;
	return 0;
}

static int iter_local_mailbox_open(struct local_dsync_worker_msg_iter *iter)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)iter->iter.worker;
	mailbox_guid_t *guid;
	struct mailbox *box;
	struct mailbox_transaction_context *trans;
	struct mail_search_args *search_args;

	if (iter->mailbox_idx == iter->mailbox_count) {
		/* no more mailboxes */
		return -1;
	}

	guid = &iter->mailboxes[iter->mailbox_idx];
	if (local_mailbox_open(worker, guid, &box) < 0) {
		i_error("msg iteration failed: Couldn't open mailbox");
		iter->iter.failed = TRUE;
		return -1;
	}

	search_args = mail_search_build_init();
	mail_search_build_add_all(search_args);

	trans = mailbox_transaction_begin(box, 0);
	iter->search_ctx = mailbox_search_init(trans, search_args, NULL);
	iter->mail = mail_alloc(trans, MAIL_FETCH_FLAGS | MAIL_FETCH_GUID,
				NULL);
	return 0;
}

static void
iter_local_mailbox_close(struct local_dsync_worker_msg_iter *iter)
{
	struct mailbox *box = iter->mail->box;
	struct mailbox_transaction_context *trans = iter->mail->transaction;

	iter->expunges_set = FALSE;
	mail_free(&iter->mail);
	if (mailbox_search_deinit(&iter->search_ctx) < 0) {
		struct mail_storage *storage =
			mailbox_get_storage(iter->mail->box);

		i_error("msg search failed: %s",
			mail_storage_get_last_error(storage, NULL));
		iter->iter.failed = TRUE;
	}
	(void)mailbox_transaction_commit(&trans);
	mailbox_close(&box);
}

static struct dsync_worker_msg_iter *
local_worker_msg_iter_init(struct dsync_worker *worker,
			   const mailbox_guid_t mailboxes[],
			   unsigned int mailbox_count)
{
	struct local_dsync_worker_msg_iter *iter;
	unsigned int i;

	iter = i_new(struct local_dsync_worker_msg_iter, 1);
	iter->iter.worker = worker;
	iter->mailboxes = mailbox_count == 0 ? NULL :
		i_new(mailbox_guid_t, mailbox_count);
	iter->mailbox_count = mailbox_count;
	for (i = 0; i < mailbox_count; i++) {
		memcpy(iter->mailboxes[i].guid, &mailboxes[i],
		       sizeof(iter->mailboxes[i].guid));
	}
	i_array_init(&iter->expunges, 32);
	iter->tmp_guid_str = str_new(default_pool, MAIL_GUID_128_SIZE * 2 + 1);
	(void)iter_local_mailbox_open(iter);
	return &iter->iter;
}

static bool
iter_local_mailbox_next_expunge(struct local_dsync_worker_msg_iter *iter,
				uint32_t prev_uid, struct dsync_message *msg_r)
{
	struct mailbox *box = iter->mail->box;
	struct mailbox_status status;
	const struct mailbox_expunge_rec *expunges;
	unsigned int count;

	if (iter->expunges_set) {
		expunges = array_get(&iter->expunges, &count);
		if (iter->expunge_idx == count)
			return FALSE;

		memset(msg_r, 0, sizeof(*msg_r));
		str_truncate(iter->tmp_guid_str, 0);
		binary_to_hex_append(iter->tmp_guid_str,
				     expunges[iter->expunge_idx].guid_128,
				     MAIL_GUID_128_SIZE);
		msg_r->guid = str_c(iter->tmp_guid_str);
		msg_r->uid = expunges[iter->expunge_idx].uid;
		msg_r->flags = DSYNC_MAIL_FLAG_EXPUNGED;
		iter->expunge_idx++;
		return TRUE;
	}

	iter->expunge_idx = 0;
	array_clear(&iter->expunges);
	iter->expunges_set = TRUE;

	mailbox_get_status(box, STATUS_UIDNEXT, &status);
	if (prev_uid + 1 >= status.uidnext) {
		/* no expunged messages at the end of mailbox */
		return FALSE;
	}

	T_BEGIN {
		ARRAY_TYPE(seq_range) uids_filter;

		t_array_init(&uids_filter, 1);
		seq_range_array_add_range(&uids_filter, prev_uid + 1,
					  status.uidnext - 1);
		(void)mailbox_get_expunges(box, 0, &uids_filter,
					   &iter->expunges);
	} T_END;
	return iter_local_mailbox_next_expunge(iter, prev_uid, msg_r);
}

static int
local_worker_msg_iter_next(struct dsync_worker_msg_iter *_iter,
			   unsigned int *mailbox_idx_r,
			   struct dsync_message *msg_r)
{
	struct local_dsync_worker_msg_iter *iter =
		(struct local_dsync_worker_msg_iter *)_iter;
	const char *guid;
	uint32_t prev_uid;

	if (_iter->failed || iter->search_ctx == NULL)
		return -1;

	prev_uid = iter->mail->uid;
	if (!mailbox_search_next(iter->search_ctx, iter->mail)) {
		if (iter_local_mailbox_next_expunge(iter, prev_uid, msg_r)) {
			*mailbox_idx_r = iter->mailbox_idx;
			return 1;
		}
		iter_local_mailbox_close(iter);
		iter->mailbox_idx++;
		if (iter_local_mailbox_open(iter) < 0)
			return -1;
		return local_worker_msg_iter_next(_iter, mailbox_idx_r, msg_r);
	}
	*mailbox_idx_r = iter->mailbox_idx;

	if (mail_get_special(iter->mail, MAIL_FETCH_GUID, &guid) < 0) {
		if (!iter->mail->expunged) {
			struct mail_storage *storage =
				mailbox_get_storage(iter->mail->box);

			i_error("msg guid lookup failed: %s",
				mail_storage_get_last_error(storage, NULL));
			_iter->failed = TRUE;
			return -1;
		}
		return local_worker_msg_iter_next(_iter, mailbox_idx_r, msg_r);
	}

	memset(msg_r, 0, sizeof(*msg_r));
	msg_r->guid = guid;
	msg_r->uid = iter->mail->uid;
	msg_r->flags = mail_get_flags(iter->mail);
	msg_r->keywords = mail_get_keywords(iter->mail);
	msg_r->modseq = mail_get_modseq(iter->mail);
	return 1;
}

static int
local_worker_msg_iter_deinit(struct dsync_worker_msg_iter *_iter)
{
	struct local_dsync_worker_msg_iter *iter =
		(struct local_dsync_worker_msg_iter *)_iter;
	int ret = _iter->failed ? -1 : 0;

	if (iter->mail != NULL)
		iter_local_mailbox_close(iter);
	array_free(&iter->expunges);
	str_free(&iter->tmp_guid_str);
	i_free(iter->mailboxes);
	i_free(iter);
	return ret;
}

static void
local_worker_copy_mailbox_update(const struct dsync_mailbox *dsync_box,
				 struct mailbox_update *update_r)
{
	memset(update_r, 0, sizeof(*update_r));
	memcpy(update_r->mailbox_guid, dsync_box->mailbox_guid.guid,
	       sizeof(update_r->mailbox_guid));
	update_r->uid_validity = dsync_box->uid_validity;
	update_r->min_next_uid = dsync_box->uid_next;
	update_r->min_highest_modseq = dsync_box->highest_modseq;
}

static struct mailbox *
local_worker_mailbox_alloc(struct local_dsync_worker *worker,
			   const struct dsync_mailbox *dsync_box)
{
	struct mail_namespace *ns;
	const char *name;

	name = dsync_box->name;
	ns = mail_namespace_find(worker->user->namespaces, &name);
	if (ns == NULL) {
		i_error("Can't find namespace for mailbox %s", dsync_box->name);
		return NULL;
	}

	return mailbox_alloc(ns->list, name, NULL, 0);
}

static void
local_worker_create_mailbox(struct dsync_worker *_worker,
			    const struct dsync_mailbox *dsync_box)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mailbox *box;
	struct mailbox_update update;
	int ret;

	box = local_worker_mailbox_alloc(worker, dsync_box);
	if (box == NULL) {
		dsync_worker_set_failure(_worker);
		return;
	}
	local_worker_copy_mailbox_update(dsync_box, &update);

	if (strcasecmp(dsync_box->name, "INBOX") == 0)
		ret = mailbox_update(box, &update);
	else {
		ret = mailbox_create(box, &update,
				     dsync_box->uid_validity == 0);
	}
	if (ret < 0) {
		dsync_worker_set_failure(_worker);
		i_error("Can't create mailbox %s: %s", dsync_box->name,
			mail_storage_get_last_error(mailbox_get_storage(box),
						    NULL));
	} else {
		local_dsync_worker_add_mailbox(worker,
					       mailbox_get_namespace(box),
					       mailbox_get_name(box),
					       &dsync_box->mailbox_guid);
	}
	mailbox_close(&box);
}

static void
local_worker_delete_mailbox(struct dsync_worker *_worker,
			    const mailbox_guid_t *mailbox)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct local_dsync_mailbox *lbox;

	lbox = hash_table_lookup(worker->mailbox_hash, mailbox);
	if (lbox == NULL) {
		i_error("Trying to delete a non-listed mailbox with guid=%s",
			binary_to_hex(mailbox->guid, sizeof(mailbox->guid)));
		dsync_worker_set_failure(_worker);
		return;
	}

	if (mailbox_list_delete_mailbox(lbox->ns->list,
					lbox->storage_name) < 0) {
		i_error("Can't delete mailbox %s: %s", lbox->storage_name,
			mailbox_list_get_last_error(lbox->ns->list, NULL));
		dsync_worker_set_failure(_worker);
	}
}

static void
local_worker_rename_children(struct local_dsync_worker *worker,
			     const char *oldname, const char *newname, char sep)
{
	struct hash_iterate_context *iter;
	const char *oldprefix;
	void *key, *value;
	unsigned int oldprefix_len;

	oldprefix = t_strdup_printf("%s%c", oldname, sep);
	oldprefix_len = strlen(oldprefix);

	iter = hash_table_iterate_init(worker->mailbox_hash);
	while (hash_table_iterate(iter, &key, &value)) {
		struct local_dsync_mailbox *lbox = value;

		if (strncmp(lbox->storage_name, oldprefix, oldprefix_len) != 0)
			continue;

		lbox->storage_name =
			p_strdup_printf(worker->pool, "%s%c%s", newname, sep,
					lbox->storage_name + oldprefix_len);
	}
	hash_table_iterate_deinit(&iter);
}

static void
local_worker_rename_mailbox(struct dsync_worker *_worker,
			    const mailbox_guid_t *mailbox, const char *name)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct local_dsync_mailbox *lbox;
	const char *oldname;

	lbox = hash_table_lookup(worker->mailbox_hash, mailbox);
	if (lbox == NULL) {
		i_error("Trying to rename a non-listed mailbox with guid=%s",
			binary_to_hex(mailbox->guid, sizeof(mailbox->guid)));
		dsync_worker_set_failure(_worker);
		return;
	}

	if (mailbox_list_rename_mailbox(lbox->ns->list, lbox->storage_name,
					lbox->ns->list, name, TRUE) < 0) {
		i_error("Can't rename mailbox %s to %s: %s", lbox->storage_name,
			name, mailbox_list_get_last_error(lbox->ns->list, NULL));
		dsync_worker_set_failure(_worker);
	} else {
		oldname = lbox->storage_name;
		lbox->storage_name = p_strdup(worker->pool, name);
		local_worker_rename_children(worker, oldname, name,
					     lbox->ns->sep);
	}
}

static void local_worker_mailbox_close(struct local_dsync_worker *worker)
{
	struct mailbox_transaction_context *trans, *ext_trans;

	i_assert(worker->save_input == NULL);

	if (worker->selected_box != NULL) {
		trans = worker->mail->transaction;
		ext_trans = worker->ext_mail->transaction;
		mail_free(&worker->mail);
		mail_free(&worker->ext_mail);
		if (mailbox_transaction_commit(&ext_trans) < 0)
			dsync_worker_set_failure(&worker->worker);
		if (mailbox_transaction_commit(&trans) < 0 ||
		    mailbox_sync(worker->selected_box,
				 MAILBOX_SYNC_FLAG_FULL_WRITE, 0, NULL) < 0)
			dsync_worker_set_failure(&worker->worker);

		mailbox_close(&worker->selected_box);
	}
	memset(&worker->selected_box_guid, 0,
	       sizeof(worker->selected_box_guid));
}

static void
local_worker_update_mailbox(struct dsync_worker *_worker,
			    const struct dsync_mailbox *dsync_box)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mailbox *box;
	struct mailbox_update update;
	bool selected = FALSE;

	/* if we're updating a selected mailbox, close it first so that all
	   pending changes get committed. */
	selected = worker->selected_box != NULL &&
		dsync_guid_equals(&dsync_box->mailbox_guid,
				  &worker->selected_box_guid);
	if (selected)
		local_worker_mailbox_close(worker);

	box = local_worker_mailbox_alloc(worker, dsync_box);
	if (box == NULL) {
		dsync_worker_set_failure(_worker);
		return;
	}

	local_worker_copy_mailbox_update(dsync_box, &update);
	if (mailbox_update(box, &update) < 0) {
		dsync_worker_set_failure(_worker);
		i_error("Can't update mailbox %s: %s", dsync_box->name,
			mail_storage_get_last_error(mailbox_get_storage(box),
						    NULL));
	}
	mailbox_close(&box);

	if (selected)
		dsync_worker_select_mailbox(_worker, dsync_box);
}

static void
local_worker_set_cache_fields(struct local_dsync_worker *worker,
			      const ARRAY_TYPE(const_string) *cache_fields)
{
	struct mailbox_update update;
	const char *const *fields, **new_fields;
	unsigned int count;

	fields = array_get(cache_fields, &count);
	new_fields = t_new(const char *, count + 1);
	memcpy(new_fields, fields, sizeof(const char *) * count);

	memset(&update, 0, sizeof(update));
	update.cache_fields = new_fields;
	mailbox_update(worker->selected_box, &update);
}

static void
local_worker_select_mailbox(struct dsync_worker *_worker,
			    const mailbox_guid_t *mailbox,
			    const ARRAY_TYPE(const_string) *cache_fields)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mailbox_transaction_context *trans, *ext_trans;

	if (worker->selected_box != NULL) {
		if (dsync_guid_equals(&worker->selected_box_guid, mailbox)) {
			/* already selected */
			return;
		}
		local_worker_mailbox_close(worker);
	}
	worker->selected_box_guid = *mailbox;

	if (local_mailbox_open(worker, mailbox, &worker->selected_box) < 0) {
		dsync_worker_set_failure(_worker);
		return;
	}
	if (cache_fields != NULL && array_is_created(cache_fields))
		local_worker_set_cache_fields(worker, cache_fields);

	ext_trans = mailbox_transaction_begin(worker->selected_box,
					MAILBOX_TRANSACTION_FLAG_EXTERNAL |
					MAILBOX_TRANSACTION_FLAG_ASSIGN_UIDS);
	trans = mailbox_transaction_begin(worker->selected_box, 0);
	worker->mail = mail_alloc(trans, 0, NULL);
	worker->ext_mail = mail_alloc(ext_trans, 0, NULL);
}

static void
local_worker_msg_update_metadata(struct dsync_worker *_worker,
				 const struct dsync_message *msg)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mail_keywords *keywords;

	if (msg->modseq > 1) {
		(void)mailbox_enable(worker->mail->box,
				     MAILBOX_FEATURE_CONDSTORE);
	}

	if (!mail_set_uid(worker->mail, msg->uid))
		dsync_worker_set_failure(_worker);
	else {
		mail_update_flags(worker->mail, MODIFY_REPLACE, msg->flags);

		keywords = mailbox_keywords_create_valid(worker->mail->box,
							 msg->keywords);
		mail_update_keywords(worker->mail, MODIFY_REPLACE, keywords);
		mailbox_keywords_unref(worker->mail->box, &keywords);
		mail_update_modseq(worker->mail, msg->modseq);
	}
}

static void
local_worker_msg_update_uid(struct dsync_worker *_worker,
			    uint32_t old_uid, uint32_t new_uid)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;

	if (!mail_set_uid(worker->ext_mail, old_uid))
		dsync_worker_set_failure(_worker);
	else
		mail_update_uid(worker->ext_mail, new_uid);
}

static void local_worker_msg_expunge(struct dsync_worker *_worker, uint32_t uid)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;

	if (mail_set_uid(worker->mail, uid))
		mail_expunge(worker->mail);
}

static void
local_worker_msg_save_set_metadata(struct mailbox *box,
				   struct mail_save_context *save_ctx,
				   const struct dsync_message *msg)
{
	struct mail_keywords *keywords;

	if (msg->modseq > 1)
		(void)mailbox_enable(box, MAILBOX_FEATURE_CONDSTORE);

	keywords = str_array_length(msg->keywords) == 0 ? NULL :
		mailbox_keywords_create_valid(box, msg->keywords);
	mailbox_save_set_flags(save_ctx, msg->flags, keywords);
	if (keywords != NULL)
		mailbox_keywords_unref(box, &keywords);
	mailbox_save_set_uid(save_ctx, msg->uid);
	mailbox_save_set_save_date(save_ctx, msg->save_date);
	mailbox_save_set_min_modseq(save_ctx, msg->modseq);
}

static void
local_worker_msg_copy(struct dsync_worker *_worker,
		      const mailbox_guid_t *src_mailbox, uint32_t src_uid,
		      const struct dsync_message *dest_msg,
		      dsync_worker_copy_callback_t *callback, void *context)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mailbox *src_box;
	struct mailbox_transaction_context *src_trans;
	struct mail *src_mail;
	struct mail_save_context *save_ctx;
	int ret;

	if (local_mailbox_open(worker, src_mailbox, &src_box) < 0) {
		callback(FALSE, context);
		return;
	}

	src_trans = mailbox_transaction_begin(src_box, 0);
	src_mail = mail_alloc(src_trans, 0, NULL);
	if (!mail_set_uid(src_mail, src_uid))
		ret = -1;
	else {
		save_ctx = mailbox_save_alloc(worker->ext_mail->transaction);
		local_worker_msg_save_set_metadata(worker->mail->box,
						   save_ctx, dest_msg);
		ret = mailbox_copy(&save_ctx, src_mail);
	}

	mail_free(&src_mail);
	(void)mailbox_transaction_commit(&src_trans);
	mailbox_close(&src_box);

	callback(ret == 0, context);
}

static void dsync_worker_try_finish(struct local_dsync_worker *worker)
{
	if (worker->finish_callback == NULL)
		return;
	if (worker->save_io != NULL || worker->reading_mail)
		return;

	worker->finish_callback(!worker->worker.failed, worker->finish_context);
}

static void
local_worker_save_msg_continue(struct local_dsync_worker *worker)
{
	int ret;

	while ((ret = i_stream_read(worker->save_input)) > 0) {
		if (mailbox_save_continue(worker->save_ctx) < 0)
			break;
	}
	if (ret == 0) {
		if (worker->save_io != NULL)
			return;
		worker->save_io =
			io_add(i_stream_get_fd(worker->save_input), IO_READ,
			       local_worker_save_msg_continue, worker);
		return;
	}
	i_assert(ret == -1);

	/* drop save_io before destroying save_input, so that save_input's
	   destroy callback can add io back to its fd. */
	if (worker->save_io != NULL)
		io_remove(&worker->save_io);
	if (worker->save_input->stream_errno != 0) {
		errno = worker->save_input->stream_errno;
		i_error("read(msg input) failed: %m");
		mailbox_save_cancel(&worker->save_ctx);
		ret = -1;
	} else {
		i_assert(worker->save_input->eof);
		ret = mailbox_save_finish(&worker->save_ctx);
	}
	if (ret < 0)
		dsync_worker_set_failure(&worker->worker);
	i_stream_unref(&worker->save_input);
	dsync_worker_try_finish(worker);
}

static void
local_worker_msg_save(struct dsync_worker *_worker,
		      const struct dsync_message *msg,
		      const struct dsync_msg_static_data *data)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mail_save_context *save_ctx;

	i_assert(worker->save_input == NULL);

	save_ctx = mailbox_save_alloc(worker->ext_mail->transaction);
	mailbox_save_set_guid(save_ctx, msg->guid);
	local_worker_msg_save_set_metadata(worker->mail->box, save_ctx, msg);
	mailbox_save_set_pop3_uidl(save_ctx, data->pop3_uidl);

	mailbox_save_set_received_date(save_ctx, data->received_date, 0);

	if (mailbox_save_begin(&save_ctx, data->input) < 0) {
		dsync_worker_set_failure(_worker);
		return;
	}

	worker->save_input = data->input;
	worker->save_ctx = save_ctx;
	i_stream_ref(worker->save_input);
	local_worker_save_msg_continue(worker);
}

static void local_worker_msg_save_cancel(struct dsync_worker *_worker)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;

	if (worker->save_input == NULL)
		return;

	if (worker->save_io != NULL)
		io_remove(&worker->save_io);
	mailbox_save_cancel(&worker->save_ctx);
	i_stream_unref(&worker->save_input);
}

static void local_worker_msg_get_done(struct local_dsync_worker *worker)
{
	worker->reading_mail = FALSE;
	dsync_worker_try_finish(worker);
}

static void local_worker_msg_box_close(struct local_dsync_worker *worker)
{
	struct mailbox_transaction_context *trans;
	struct mailbox *box;

	if (worker->get_mail == NULL)
		return;

	box = worker->get_mail->box;
	trans = worker->get_mail->transaction;

	mail_free(&worker->get_mail);
	(void)mailbox_transaction_commit(&trans);
	mailbox_close(&box);
	memset(&worker->get_mailbox, 0, sizeof(worker->get_mailbox));
}

static void
local_worker_msg_get(struct dsync_worker *_worker,
		     const mailbox_guid_t *mailbox, uint32_t uid,
		     dsync_worker_msg_callback_t *callback, void *context)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct dsync_msg_static_data data;
	struct mailbox_transaction_context *trans;
	struct mailbox *box;

	i_assert(!worker->reading_mail);

	if (!dsync_guid_equals(&worker->get_mailbox, mailbox)) {
		local_worker_msg_box_close(worker);
		if (local_mailbox_open(worker, mailbox, &box) < 0) {
			callback(DSYNC_MSG_GET_RESULT_FAILED, NULL, context);
			return;
		}
		worker->get_mailbox = *mailbox;

		trans = mailbox_transaction_begin(box, 0);
		worker->get_mail = mail_alloc(trans, 0, NULL);
	}

	if (!mail_set_uid(worker->get_mail, uid)) {
		callback(DSYNC_MSG_GET_RESULT_EXPUNGED, NULL, context);
		return;
	}

	memset(&data, 0, sizeof(data));
	if (mail_get_special(worker->get_mail, MAIL_FETCH_UIDL_BACKEND,
			     &data.pop3_uidl) < 0 ||
	    mail_get_received_date(worker->get_mail, &data.received_date) < 0 ||
	    mail_get_stream(worker->get_mail, NULL, NULL, &data.input) < 0) {
		if (worker->get_mail->expunged)
			callback(DSYNC_MSG_GET_RESULT_EXPUNGED, NULL, context);
		else
			callback(DSYNC_MSG_GET_RESULT_FAILED, NULL, context);
	} else {
		worker->reading_mail = TRUE;
		data.input = i_stream_create_limit(data.input, (uoff_t)-1);
		i_stream_set_destroy_callback(data.input,
					      local_worker_msg_get_done,
					      worker);
		callback(DSYNC_MSG_GET_RESULT_SUCCESS, &data, context);
	}
}

static void
local_worker_finish(struct dsync_worker *_worker,
		    dsync_worker_finish_callback_t *callback, void *context)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;

	worker->finish_callback = callback;
	worker->finish_context = context;

	dsync_worker_try_finish(worker);
}

struct dsync_worker_vfuncs local_dsync_worker = {
	local_worker_deinit,

	local_worker_is_output_full,
	local_worker_output_flush,

	local_worker_mailbox_iter_init,
	local_worker_mailbox_iter_next,
	local_worker_mailbox_iter_deinit,

	local_worker_subs_iter_init,
	local_worker_subs_iter_next,
	local_worker_subs_iter_next_un,
	local_worker_subs_iter_deinit,
	local_worker_set_subscribed,

	local_worker_msg_iter_init,
	local_worker_msg_iter_next,
	local_worker_msg_iter_deinit,

	local_worker_create_mailbox,
	local_worker_delete_mailbox,
	local_worker_rename_mailbox,
	local_worker_update_mailbox,

	local_worker_select_mailbox,
	local_worker_msg_update_metadata,
	local_worker_msg_update_uid,
	local_worker_msg_expunge,
	local_worker_msg_copy,
	local_worker_msg_save,
	local_worker_msg_save_cancel,
	local_worker_msg_get,
	local_worker_finish
};
