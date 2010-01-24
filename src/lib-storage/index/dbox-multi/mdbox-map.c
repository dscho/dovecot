/* Copyright (c) 2007-2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "ostream.h"
#include "mkdir-parents.h"
#include "mdbox-storage.h"
#include "mdbox-file.h"
#include "mdbox-map-private.h"

#define MAX_BACKWARDS_LOOKUPS 10

#define DBOX_FORCE_PURGE_MIN_BYTES (1024*1024*10)
#define DBOX_FORCE_PURGE_MIN_RATIO 0.5

#define MAP_STORAGE(map) (&(map)->storage->storage.storage)

struct dbox_map_transaction_context {
	struct dbox_map *map;
	struct mail_index_transaction *trans;
	struct mail_index_sync_ctx *sync_ctx;

	unsigned int changed:1;
	unsigned int success:1;
};

void dbox_map_set_corrupted(struct dbox_map *map, const char *format, ...)
{
	va_list args;

	map->storage->storage.files_corrupted = TRUE;

	va_start(args, format);
	mail_storage_set_critical(MAP_STORAGE(map),
				  "dbox map %s corrupted: %s",
				  map->index->filepath,
				  t_strdup_vprintf(format, args));
	va_end(args);
}

struct dbox_map *
dbox_map_init(struct mdbox_storage *storage, struct mailbox_list *root_list,
	      const char *path)
{
	struct dbox_map *map;
	gid_t tmp_gid;
	const char *tmp_origin;

	map = i_new(struct dbox_map, 1);
	map->storage = storage;
	map->set = storage->set;
	map->path = i_strdup(path);
	map->index = mail_index_alloc(path, MDBOX_GLOBAL_INDEX_PREFIX);
	map->map_ext_id = mail_index_ext_register(map->index, "map",
				sizeof(struct dbox_map_mail_index_header),
				sizeof(struct dbox_map_mail_index_record),
				sizeof(uint32_t));
	map->ref_ext_id = mail_index_ext_register(map->index, "ref", 0,
				sizeof(uint16_t), sizeof(uint16_t));
	map->created_uid_validity = ioloop_time;

	mailbox_list_get_permissions(root_list, NULL, &map->create_mode,
				     &map->create_gid, &map->create_gid_origin);
	mailbox_list_get_dir_permissions(root_list, NULL, &map->create_dir_mode,
					 &tmp_gid, &tmp_origin);
	mail_index_set_permissions(map->index, map->create_mode,
				   map->create_gid, map->create_gid_origin);
	return map;
}

void dbox_map_deinit(struct dbox_map **_map)
{
	struct dbox_map *map = *_map;

	*_map = NULL;

	if (array_is_created(&map->ref0_file_ids))
		array_free(&map->ref0_file_ids);
	if (map->view != NULL)
		mail_index_view_close(&map->view);
	mail_index_free(&map->index);
	i_free(map->path);
	i_free(map);
}

static int dbox_map_mkdir_storage(struct dbox_map *map)
{
	if (mkdir_parents_chgrp(map->path, map->create_dir_mode,
				map->create_gid, map->create_gid_origin) < 0 &&
	    errno != EEXIST) {
		mail_storage_set_critical(MAP_STORAGE(map),
					  "mkdir(%s) failed: %m", map->path);
		return -1;
	}
	return 0;
}

int dbox_map_open(struct dbox_map *map, bool create_missing)
{
	enum mail_index_open_flags open_flags;
	int ret;

	if (map->view != NULL) {
		/* already opened */
		return 0;
	}

	open_flags = MAIL_INDEX_OPEN_FLAG_NEVER_IN_MEMORY |
		mail_storage_settings_to_index_flags(MAP_STORAGE(map)->set);
	if (create_missing) {
		open_flags |= MAIL_INDEX_OPEN_FLAG_CREATE;
		if (dbox_map_mkdir_storage(map) < 0)
			return -1;
	}
	ret = mail_index_open(map->index, open_flags,
			      MAP_STORAGE(map)->set->parsed_lock_method);
	if (ret < 0) {
		mail_storage_set_internal_error(MAP_STORAGE(map));
		mail_index_reset_error(map->index);
		return -1;
	}
	if (ret == 0) {
		/* index not found - for now just return failure */
		return -1;
	}

	map->view = mail_index_view_open(map->index);
	return 0;
}

int dbox_map_refresh(struct dbox_map *map)
{
	struct mail_index_view_sync_ctx *ctx;
	bool delayed_expunges;

	/* some open files may have read partially written mails. now that
	   map syncing makes the new mails visible, we need to make sure the
	   partial data is flushed out of memory */
	mdbox_files_sync_input(map->storage);

	if (mail_index_refresh(map->view->index) < 0) {
		mail_storage_set_internal_error(MAP_STORAGE(map));
		mail_index_reset_error(map->index);
		return -1;
	}
	ctx = mail_index_view_sync_begin(map->view,
				MAIL_INDEX_VIEW_SYNC_FLAG_FIX_INCONSISTENT);
	if (mail_index_view_sync_commit(&ctx, &delayed_expunges) < 0) {
		mail_storage_set_internal_error(MAP_STORAGE(map));
		mail_index_reset_error(map->index);
		return -1;
	}
	return 0;
}

static int dbox_map_lookup_seq(struct dbox_map *map, uint32_t seq,
			       uint32_t *file_id_r, uoff_t *offset_r,
			       uoff_t *size_r)
{
	const struct dbox_map_mail_index_record *rec;
	const void *data;
	uint32_t uid;
	bool expunged;

	mail_index_lookup_ext(map->view, seq, map->map_ext_id,
			      &data, &expunged);
	rec = data;

	if (rec == NULL || rec->file_id == 0) {
		mail_index_lookup_uid(map->view, seq, &uid);
		dbox_map_set_corrupted(map, "file_id=0 for map_uid=%u", uid);
		return -1;
	}

	*file_id_r = rec->file_id;
	*offset_r = rec->offset;
	*size_r = rec->size;
	return 0;
}

static int
dbox_map_get_seq(struct dbox_map *map, uint32_t map_uid, uint32_t *seq_r)
{
	if (!mail_index_lookup_seq(map->view, map_uid, seq_r)) {
		/* not found - try again after a refresh */
		if (dbox_map_refresh(map) < 0)
			return -1;
		if (!mail_index_lookup_seq(map->view, map_uid, seq_r))
			return 0;
	}
	return 1;
}

int dbox_map_lookup(struct dbox_map *map, uint32_t map_uid,
		    uint32_t *file_id_r, uoff_t *offset_r)
{
	uint32_t seq;
	uoff_t size;
	int ret;

	if (dbox_map_open(map, TRUE) < 0)
		return -1;

	if ((ret = dbox_map_get_seq(map, map_uid, &seq)) <= 0)
		return ret;

	if (dbox_map_lookup_seq(map, seq, file_id_r, offset_r, &size) < 0)
		return -1;
	return 1;
}

int dbox_map_view_lookup_rec(struct dbox_map *map, struct mail_index_view *view,
			     uint32_t seq, struct dbox_mail_lookup_rec *rec_r)
{
	const uint16_t *ref16_p;
	const void *data;
	bool expunged;

	memset(rec_r, 0, sizeof(*rec_r));
	mail_index_lookup_uid(view, seq, &rec_r->map_uid);

	mail_index_lookup_ext(view, seq, map->map_ext_id, &data, &expunged);
	if (data == NULL) {
		dbox_map_set_corrupted(map, "missing map extension");
		return -1;
	}
	memcpy(&rec_r->rec, data, sizeof(rec_r->rec));

	mail_index_lookup_ext(view, seq, map->ref_ext_id, &data, &expunged);
	if (data == NULL) {
		dbox_map_set_corrupted(map, "missing ref extension");
		return -1;
	}
	ref16_p = data;
	rec_r->refcount = *ref16_p;
	return 0;
}

int dbox_map_get_file_msgs(struct dbox_map *map, uint32_t file_id,
			   ARRAY_TYPE(dbox_map_file_msg) *recs)
{
	const struct mail_index_header *hdr;
	struct dbox_mail_lookup_rec rec;
	struct dbox_map_file_msg msg;
	uint32_t seq;

	if (dbox_map_refresh(map) < 0)
		return -1;
	hdr = mail_index_get_header(map->view);

	memset(&msg, 0, sizeof(msg));
	for (seq = 1; seq <= hdr->messages_count; seq++) {
		if (dbox_map_view_lookup_rec(map, map->view, seq, &rec) < 0)
			return -1;

		if (rec.rec.file_id == file_id) {
			msg.map_uid = rec.map_uid;
			msg.offset = rec.rec.offset;
			msg.refcount = rec.refcount;
			array_append(recs, &msg, 1);
		}
	}
	return 0;
}

struct dbox_file_size {
	uoff_t file_size;
	uoff_t ref0_size;
};

static void dbox_map_filter_zero_refs(struct dbox_map *map)
{
	ARRAY_TYPE(seq_range) new_ref0_file_ids;
	struct hash_table *hash;
	struct dbox_file_size *size;
	struct seq_range_iter iter;
	const struct mail_index_header *hdr;
	const struct dbox_map_mail_index_record *rec;
	const uint16_t *ref16_p;
	const void *data;
	uint32_t seq, file_id;
	unsigned int i;
	bool expunged;
	pool_t pool;

	pool = pool_alloconly_create("dbox zero ref count", 8*1024);
	hash = hash_table_create(default_pool, pool, 0, NULL, NULL);

	/* count file sizes */
	hdr = mail_index_get_header(map->view);
	for (seq = 1; seq <= hdr->messages_count; seq++) {
		mail_index_lookup_ext(map->view, seq, map->map_ext_id,
				      &data, &expunged);
		if (data == NULL || expunged)
			continue;
		rec = data;

		if (!seq_range_exists(&map->ref0_file_ids, rec->file_id))
			continue;

		/* this file has at least some zero references. count how many
		   bytes it has in total and how much of it has refcount=0. */
		mail_index_lookup_ext(map->view, seq, map->ref_ext_id,
				      &data, &expunged);
		if (data == NULL || expunged)
			continue;
		ref16_p = data;

		size = hash_table_lookup(hash, POINTER_CAST(rec->file_id));
		if (size == NULL) {
			size = p_new(pool, struct dbox_file_size, 1);
			hash_table_insert(hash, POINTER_CAST(rec->file_id),
					  size);
		}
		if (*ref16_p == 0)
			size->ref0_size += rec->size;
		if (size->file_size < rec->offset + rec->size)
			size->file_size = rec->offset + rec->size;
	}

	/* now drop the files that don't have enough deleted space */
	seq_range_array_iter_init(&iter, &map->ref0_file_ids); i = 0;
	p_array_init(&new_ref0_file_ids, pool, 
		     array_count(&map->ref0_file_ids));
	while (seq_range_array_iter_nth(&iter, i++, &file_id)) {
		size = hash_table_lookup(hash, POINTER_CAST(file_id));
		if (size->ref0_size*100 / size->file_size >=
		    map->set->mdbox_purge_min_percentage)
			seq_range_array_add(&new_ref0_file_ids, 0, file_id);
	}
	seq_range_array_intersect(&map->ref0_file_ids, &new_ref0_file_ids);

	hash_table_destroy(&hash);
	pool_unref(&pool);
}

bool dbox_map_want_purge(struct dbox_map *map)
{
	const struct mail_index_header *hdr;
	const struct dbox_map_mail_index_record *rec;
	const uint16_t *ref16_p;
	const void *data;
	uoff_t ref0_size, total_size;
	bool expunged;
	uint32_t seq;

	if (map->set->mdbox_purge_min_percentage >= 100) {
		/* we never purge anything */
		return FALSE;
	}

	ref0_size = total_size = 0;
	hdr = mail_index_get_header(map->view);
	for (seq = 1; seq <= hdr->messages_count; seq++) {
		mail_index_lookup_ext(map->view, seq, map->map_ext_id,
				      &data, &expunged);
		if (data == NULL || expunged)
			continue;
		rec = data;

		mail_index_lookup_ext(map->view, seq, map->ref_ext_id,
				      &data, &expunged);
		if (data == NULL || expunged)
			continue;
		ref16_p = data;

		if (*ref16_p == 0)
			ref0_size += rec->size;
		total_size += rec->size;
	}

	if (ref0_size < DBOX_FORCE_PURGE_MIN_BYTES)
		return FALSE;
	if ((float)ref0_size / (float)total_size < DBOX_FORCE_PURGE_MIN_RATIO)
		return FALSE;
	return TRUE;
}

const ARRAY_TYPE(seq_range) *dbox_map_get_zero_ref_files(struct dbox_map *map)
{
	const struct mail_index_header *hdr;
	const struct dbox_map_mail_index_record *rec;
	const uint16_t *ref16_p;
	const void *data;
	uint32_t seq;
	bool expunged;

	if (array_is_created(&map->ref0_file_ids))
		array_clear(&map->ref0_file_ids);
	else
		i_array_init(&map->ref0_file_ids, 64);

	if (map->set->mdbox_purge_min_percentage >= 100) {
		/* we're never purging anything */
		return &map->ref0_file_ids;
	}

	if (dbox_map_open(map, FALSE) < 0) {
		/* some internal error */
		return &map->ref0_file_ids;
	}
	(void)dbox_map_refresh(map);

	hdr = mail_index_get_header(map->view);
	for (seq = 1; seq <= hdr->messages_count; seq++) {
		mail_index_lookup_ext(map->view, seq, map->ref_ext_id,
				      &data, &expunged);
		if (data != NULL && !expunged) {
			ref16_p = data;
			if (*ref16_p != 0)
				continue;
		}

		mail_index_lookup_ext(map->view, seq, map->map_ext_id,
				      &data, &expunged);
		if (data != NULL && !expunged) {
			rec = data;
			seq_range_array_add(&map->ref0_file_ids, 0,
					    rec->file_id);
		}
	}
	if (map->set->mdbox_purge_min_percentage > 0 &&
	    array_count(&map->ref0_file_ids) > 0)
		dbox_map_filter_zero_refs(map);
	return &map->ref0_file_ids;
}

struct dbox_map_transaction_context *
dbox_map_transaction_begin(struct dbox_map *map, bool external)
{
	struct dbox_map_transaction_context *ctx;
	enum mail_index_transaction_flags flags =
		MAIL_INDEX_TRANSACTION_FLAG_FSYNC;

	if (external)
		flags |= MAIL_INDEX_TRANSACTION_FLAG_EXTERNAL;

	ctx = i_new(struct dbox_map_transaction_context, 1);
	ctx->map = map;
	if (dbox_map_open(map, FALSE) == 0 &&
	    dbox_map_refresh(map) == 0)
		ctx->trans = mail_index_transaction_begin(map->view, flags);
	return ctx;
}

static void
dbox_map_sync_handle(struct dbox_map *map, struct mail_index_sync_ctx *sync_ctx)
{
	struct mail_index_sync_rec sync_rec;
	uint32_t seq1, seq2;
	uoff_t offset1, offset2;

	mail_index_sync_get_offsets(sync_ctx, &seq1, &offset1, &seq2, &offset2);
	if (offset1 != offset2 || seq1 != seq2) {
		/* something had crashed. need a full resync. */
		i_warning("dbox %s: Inconsistency in map index "
			  "(%u,%"PRIuUOFF_T" != %u,%"PRIuUOFF_T")",
			  map->path, seq1, offset1, seq2, offset2);
		map->storage->storage.files_corrupted = TRUE;
	} else {
		while (mail_index_sync_next(sync_ctx, &sync_rec)) ;
	}
}

int dbox_map_transaction_commit(struct dbox_map_transaction_context *ctx)
{
	struct dbox_map *map = ctx->map;
	struct mail_index_view *view;
	struct mail_index_transaction *sync_trans;
	int ret;

	if (!ctx->changed)
		return 0;

	/* use syncing to lock the transaction log, so that we always see
	   log's head_offset = tail_offset */
	ret = mail_index_sync_begin(map->index, &ctx->sync_ctx,
				    &view, &sync_trans, 0);
	if (ret <= 0) {
		i_assert(ret != 0);
		mail_storage_set_internal_error(MAP_STORAGE(map));
		mail_index_reset_error(map->index);
		mail_index_transaction_rollback(&ctx->trans);
		return -1;
	}
	dbox_map_sync_handle(map, ctx->sync_ctx);

	if (mail_index_transaction_commit(&ctx->trans) < 0) {
		mail_storage_set_internal_error(MAP_STORAGE(map));
		mail_index_reset_error(map->index);
		return -1;
	}
	ctx->success = TRUE;
	return 0;
}

void dbox_map_transaction_free(struct dbox_map_transaction_context **_ctx)
{
	struct dbox_map_transaction_context *ctx = *_ctx;
	struct dbox_map *map = ctx->map;

	*_ctx = NULL;
	if (ctx->success) {
		if (mail_index_sync_commit(&ctx->sync_ctx) < 0) {
			mail_storage_set_internal_error(MAP_STORAGE(map));
			mail_index_reset_error(map->index);
		}
	} else if (ctx->sync_ctx != NULL) {
		mail_index_sync_rollback(&ctx->sync_ctx);
	}
	if (ctx->trans != NULL)
		mail_index_transaction_rollback(&ctx->trans);
	i_free(ctx);
}

int dbox_map_update_refcounts(struct dbox_map_transaction_context *ctx,
			      const ARRAY_TYPE(uint32_t) *map_uids, int diff)
{
	struct dbox_map *map = ctx->map;
	const uint32_t *uidp;
	unsigned int i, count;
	const void *data;
	uint32_t seq;
	bool expunged;
	int cur_diff;

	if (ctx->trans == NULL)
		return -1;

	count = array_count(map_uids);
	for (i = 0; i < count; i++) {
		uidp = array_idx(map_uids, i);
		if (!mail_index_lookup_seq(map->view, *uidp, &seq)) {
			/* we can't refresh map here since view has a
			   transaction open. */
			dbox_map_set_corrupted(map,
				"refcount update lost map_uid=%u", *uidp);
			return -1;
		}
		mail_index_lookup_ext(map->view, seq, map->ref_ext_id,
				      &data, &expunged);
		cur_diff = data == NULL ? 0 : *((const uint16_t *)data);
		ctx->changed = TRUE;
		cur_diff += mail_index_atomic_inc_ext(ctx->trans, seq,
						      map->ref_ext_id, diff);
		i_assert(cur_diff >= 0);
		if (cur_diff >= 32768) {
			/* we're getting close to the 64k limit. fail early
			   to make it less likely that two processes increase
			   the refcount enough times to cross the limit */
			mail_storage_set_error(MAP_STORAGE(map),
				MAIL_ERROR_NOTPOSSIBLE,
				"Message has been copied too many times");
			return -1;
		}
	}
	return 0;
}

int dbox_map_remove_file_id(struct dbox_map *map, uint32_t file_id)
{
	struct dbox_map_transaction_context *map_trans;
	const struct mail_index_header *hdr;
	const struct dbox_map_mail_index_record *rec;
	const void *data;
	bool expunged;
	uint32_t seq;
	int ret = 0;

	/* make sure the map is refreshed, otherwise we might be expunging
	   messages that have already been moved to other files. */

	/* we need a per-file transaction, otherwise we can't refresh the map */
	map_trans = dbox_map_transaction_begin(map, TRUE);

	hdr = mail_index_get_header(map->view);
	for (seq = 1; seq <= hdr->messages_count; seq++) {
		mail_index_lookup_ext(map->view, seq, map->map_ext_id,
				      &data, &expunged);
		if (data == NULL) {
			dbox_map_set_corrupted(map, "missing map extension");
			ret = -1;
			break;
		}

		rec = data;
		if (rec->file_id == file_id) {
			map_trans->changed = TRUE;
			mail_index_expunge(map_trans->trans, seq);
		}
	}
	if (ret == 0)
		(void)dbox_map_transaction_commit(map_trans);
	dbox_map_transaction_free(&map_trans);
	return ret;
}

struct dbox_map_append_context *
dbox_map_append_begin(struct dbox_map *map)
{
	struct dbox_map_append_context *ctx;

	ctx = i_new(struct dbox_map_append_context, 1);
	ctx->map = map;
	ctx->first_new_file_id = (uint32_t)-1;
	i_array_init(&ctx->file_appends, 64);
	i_array_init(&ctx->files, 64);
	i_array_init(&ctx->appends, 128);

	if (dbox_map_open(ctx->map, TRUE) < 0)
		ctx->failed = TRUE;
	else {
		/* refresh the map so we can try appending to the
		   latest files */
		(void)dbox_map_refresh(ctx->map);
	}
	return ctx;
}

static time_t day_begin_stamp(unsigned int days)
{
	struct tm tm;
	time_t stamp;

	if (days == 0)
		return 0;

	/* get beginning of today */
	tm = *localtime(&ioloop_time);
	tm.tm_hour = 0;
	tm.tm_min = 0;
	tm.tm_sec = 0;
	stamp = mktime(&tm);
	if (stamp == (time_t)-1)
		i_panic("mktime(today) failed");

	return stamp - (3600*24 * (days-1));
}

static bool
dbox_map_file_try_append(struct dbox_map_append_context *ctx,
			 uint32_t file_id, time_t stamp, uoff_t mail_size,
			 struct dbox_file_append_context **file_append_r,
			 struct ostream **output_r, bool *retry_later_r)
{
	struct dbox_map *map = ctx->map;
	struct mdbox_storage *storage = map->storage;
	struct dbox_file *file;
	struct dbox_file_append_context *file_append;
	struct stat st;
	bool deleted, file_too_old = FALSE;
	int ret;

	*file_append_r = NULL;
	*output_r = NULL;
	*retry_later_r = FALSE;

	file = mdbox_file_init(storage, file_id);
	if (dbox_file_open(file, &deleted) <= 0 || deleted) {
		dbox_file_unref(&file);
		return TRUE;
	}
	if (file->lock != NULL) {
		/* already locked, we're possibly in the middle of purging it
		   in which case we really don't want to write there. */
		dbox_file_unref(&file);
		return TRUE;
	}

	if (file->create_time < stamp)
		file_too_old = TRUE;
	else if ((ret = dbox_file_try_lock(file)) <= 0) {
		/* locking failed */
		*retry_later_r = ret == 0;
	} else if (stat(file->cur_path, &st) < 0) {
		if (errno != ENOENT)
			i_error("stat(%s) failed: %m", file->cur_path);
		/* the file was unlinked between opening and locking it. */
	} else {
		file_append = dbox_file_append_init(file);
		if (dbox_file_get_append_stream(file_append, output_r) <= 0) {
			/* couldn't append to this file */
		} else if ((*output_r)->offset + mail_size > map->set->mdbox_rotate_size) {
			/* file was too large after all */
		} else {
			/* success */
			*file_append_r = file_append;
			return TRUE;
		}
		dbox_file_append_rollback(&file_append);
	}

	/* failure */
	dbox_file_unlock(file);
	dbox_file_unref(&file);
	return !file_too_old;
}

static bool
dbox_map_is_appending(struct dbox_map_append_context *ctx, uint32_t file_id)
{
	struct dbox_file_append_context *const *file_appends;
	unsigned int i, count;

	/* there shouldn't be many files open, don't bother with anything
	   faster. */
	file_appends = array_get(&ctx->file_appends, &count);
	for (i = 0; i < count; i++) {
		struct mdbox_file *mfile =
			(struct mdbox_file *)file_appends[i]->file;

		if (mfile->file_id == file_id)
			return TRUE;
	}
	return FALSE;
}

static int
dbox_map_find_appendable_file(struct dbox_map_append_context *ctx,
			      uoff_t mail_size,
			      struct dbox_file_append_context **file_append_r,
			      struct ostream **output_r, bool *existing_r)
{
	struct dbox_map *map = ctx->map;
	ARRAY_TYPE(seq_range) checked_file_ids;
	struct dbox_file_append_context *const *file_appends;
	const struct mail_index_header *hdr;
	unsigned int i, count, backwards_lookup_count;
	uint32_t seq, seq1, uid, file_id;
	uoff_t offset, append_offset, size;
	time_t stamp;
	bool retry_later;

	*existing_r = FALSE;

	if (mail_size >= map->set->mdbox_rotate_size)
		return 0;

	/* first try to use files already used in this append */
	file_appends = array_get(&ctx->file_appends, &count);
	for (i = count; i > ctx->files_nonappendable_count; i--) {
		append_offset = file_appends[i-1]->output->offset;
		if (append_offset + mail_size <= map->set->mdbox_rotate_size &&
		    dbox_file_get_append_stream(file_appends[i-1], output_r) > 0) {
			*file_append_r = file_appends[i-1];
			*existing_r = TRUE;
			return 1;
		}
		/* can't append to this file anymore. we could close it
		   otherwise, except that would also lose our lock too early. */
	}
	ctx->files_nonappendable_count = count;

	/* try to find an existing appendable file */
	stamp = day_begin_stamp(map->set->mdbox_rotate_days);
	hdr = mail_index_get_header(map->view);

	ctx->orig_next_uid = hdr->next_uid;
	backwards_lookup_count = 0;
	t_array_init(&checked_file_ids, 16);
	for (seq = hdr->messages_count; seq > 0; seq--) {
		if (dbox_map_lookup_seq(map, seq, &file_id, &offset, &size) < 0)
			return -1;

		if (seq_range_exists(&checked_file_ids, file_id))
			continue;
		seq_range_array_add(&checked_file_ids, 0, file_id);

		if (++backwards_lookup_count > MAX_BACKWARDS_LOOKUPS) {
			/* we've wasted enough time here */
			break;
		}

		/* first lookup: this should be enough usually, but we can't
		   be sure until after locking. also if messages were recently
		   moved, this message might not be the last one in the file. */
		if (offset + size + mail_size >= map->set->mdbox_rotate_size)
			continue;

		if (dbox_map_is_appending(ctx, file_id)) {
			/* already checked this */
			continue;
		}

		mail_index_lookup_uid(map->view, seq, &uid);
		if (!dbox_map_file_try_append(ctx, file_id, stamp, mail_size,
					      file_append_r, output_r,
					      &retry_later)) {
			/* file is too old. the rest of the files are too. */
			break;
		}
		/* NOTE: we've now refreshed map view. there are no guarantees
		   about sequences anymore. */
		if (*file_append_r != NULL)
			return 1;
		/* FIXME: use retry_later somehow */
		if (uid == 1 ||
		    !mail_index_lookup_seq_range(map->view, 1, uid-1,
						 &seq1, &seq))
			break;
		seq++;
	}
	return 0;
}

int dbox_map_append_next(struct dbox_map_append_context *ctx, uoff_t mail_size,
			 struct dbox_file_append_context **file_append_ctx_r,
			 struct ostream **output_r)
{
	struct dbox_file *file;
	struct dbox_map_append *append;
	struct dbox_file_append_context *file_append;
	bool existing;
	int ret;

	if (ctx->failed)
		return -1;

	ret = dbox_map_find_appendable_file(ctx, mail_size, &file_append,
					    output_r, &existing);
	if (ret > 0)
		file = file_append->file;
	else if (ret < 0)
		return -1;
	else {
		/* create a new file */
		file = mdbox_file_init(ctx->map->storage, 0);
		file_append = dbox_file_append_init(file);

		ret = dbox_file_get_append_stream(file_append, output_r);
		if (ret <= 0) {
			i_assert(ret < 0);
			dbox_file_append_rollback(&file_append);
			dbox_file_unref(&file);
			return -1;
		}
	}

	append = array_append_space(&ctx->appends);
	append->file_append = file_append;
	append->offset = (*output_r)->offset;
	append->size = (uint32_t)-1;
	if (!existing) {
		i_assert(file_append->first_append_offset == 0);
		file_append->first_append_offset = file_append->output->offset;
		array_append(&ctx->file_appends, &file_append, 1);
		array_append(&ctx->files, &file, 1);
	}
	*file_append_ctx_r = file_append;
	return 0;
}

void dbox_map_append_finish(struct dbox_map_append_context *ctx)
{
	struct dbox_map_append *appends;
	unsigned int count;
	uoff_t cur_offset;

	appends = array_get_modifiable(&ctx->appends, &count);
	i_assert(count > 0 && appends[count-1].size == (uint32_t)-1);
	cur_offset = appends[count-1].file_append->output->offset;
	i_assert(cur_offset >= appends[count-1].offset);
	appends[count-1].size = cur_offset - appends[count-1].offset;
}

static int
dbox_map_get_next_file_id(struct dbox_map *map, struct mail_index_view *view,
			  uint32_t *file_id_r)
{
	const struct dbox_map_mail_index_header *hdr;
	const void *data;
	size_t data_size;

	mail_index_get_header_ext(view, map->map_ext_id, &data, &data_size);
	if (data_size != sizeof(*hdr)) {
		if (data_size != 0) {
			dbox_map_set_corrupted(map, "hdr size=%"PRIuSIZE_T,
					       data_size);
			return -1;
		}
		/* first file */
		*file_id_r = 1;
	} else {
		hdr = data;
		*file_id_r = hdr->highest_file_id + 1;
	}
	return 0;
}

static int dbox_map_assign_file_ids(struct dbox_map_append_context *ctx,
				    bool separate_transaction)
{
	struct dbox_file_append_context *const *file_appends;
	unsigned int i, count;
	uint32_t first_file_id, file_id;
	int ret;

	/* start the syncing. we'll need it even if there are no file ids to
	   be assigned. */
	ret = mail_index_sync_begin(ctx->map->index, &ctx->sync_ctx,
				    &ctx->sync_view, &ctx->sync_trans, 0);
	if (ret <= 0) {
		i_assert(ret != 0);
		mail_storage_set_internal_error(MAP_STORAGE(ctx->map));
		mail_index_reset_error(ctx->map->index);
		return -1;
	}
	dbox_map_sync_handle(ctx->map, ctx->sync_ctx);

	if (dbox_map_get_next_file_id(ctx->map, ctx->sync_view, &file_id) < 0) {
		mail_index_sync_rollback(&ctx->sync_ctx);
		return -1;
	}

	/* assign file_ids for newly created files */
	first_file_id = file_id;
	file_appends = array_get(&ctx->file_appends, &count);
	for (i = 0; i < count; i++) {
		struct mdbox_file *mfile =
			(struct mdbox_file *)file_appends[i]->file;

		if (dbox_file_append_flush(file_appends[i]) < 0) {
			ret = -1;
			break;
		}

		if (mfile->file_id == 0) {
			if (mdbox_file_assign_file_id(mfile, file_id++) < 0) {
				ret = -1;
				break;
			}
		}
	}

	if (ret < 0) {
		mail_index_sync_rollback(&ctx->sync_ctx);
		return -1;
	}

	ctx->trans = !separate_transaction ? NULL :
		mail_index_transaction_begin(ctx->map->view,
					MAIL_INDEX_TRANSACTION_FLAG_FSYNC);

	/* update the highest used file_id */
	if (first_file_id != file_id) {
		file_id--;
		mail_index_update_header_ext(ctx->trans != NULL ? ctx->trans :
					     ctx->sync_trans,
					     ctx->map->map_ext_id,
					     0, &file_id, sizeof(file_id));
	}
	return 0;
}

int dbox_map_append_assign_map_uids(struct dbox_map_append_context *ctx,
				    uint32_t *first_map_uid_r,
				    uint32_t *last_map_uid_r)
{
	const struct dbox_map_append *appends;
	const struct mail_index_header *hdr;
	struct dbox_map_mail_index_record rec;
	unsigned int i, count;
	ARRAY_TYPE(seq_range) uids;
	const struct seq_range *range;
	uint32_t seq;
	uint16_t ref16;
	int ret = 0;

	if (array_count(&ctx->appends) == 0) {
		*first_map_uid_r = 0;
		*last_map_uid_r = 0;
		return 0;
	}

	if (dbox_map_assign_file_ids(ctx, TRUE) < 0)
		return -1;

	/* append map records to index */
	memset(&rec, 0, sizeof(rec));
	ref16 = 1;
	appends = array_get(&ctx->appends, &count);
	for (i = 0; i < count; i++) {
		struct mdbox_file *mfile =
			(struct mdbox_file *)appends[i].file_append->file;

		i_assert(appends[i].offset <= (uint32_t)-1);
		i_assert(appends[i].size <= (uint32_t)-1);

		rec.file_id = mfile->file_id;
		rec.offset = appends[i].offset;
		rec.size = appends[i].size;

		mail_index_append(ctx->trans, 0, &seq);
		mail_index_update_ext(ctx->trans, seq, ctx->map->map_ext_id,
				      &rec, NULL);
		mail_index_update_ext(ctx->trans, seq, ctx->map->ref_ext_id,
				      &ref16, NULL);
	}

	/* assign map UIDs for appended records */
	hdr = mail_index_get_header(ctx->sync_view);
	t_array_init(&uids, 1);
	mail_index_append_finish_uids(ctx->trans, hdr->next_uid, &uids);
	range = array_idx(&uids, 0);
	i_assert(range[0].seq2 - range[0].seq1 + 1 == count);

	if (hdr->uid_validity == 0) {
		/* we don't really care about uidvalidity, but it can't be 0 */
		uint32_t uid_validity = ioloop_time;
		mail_index_update_header(ctx->trans,
			offsetof(struct mail_index_header, uid_validity),
			&uid_validity, sizeof(uid_validity), TRUE);
	}

	if (mail_index_transaction_commit(&ctx->trans) < 0) {
		mail_storage_set_internal_error(MAP_STORAGE(ctx->map));
		mail_index_reset_error(ctx->map->index);
		return -1;
	}

	*first_map_uid_r = range[0].seq1;
	*last_map_uid_r = range[0].seq2;
	return ret;
}

int dbox_map_append_move(struct dbox_map_append_context *ctx,
			 const ARRAY_TYPE(uint32_t) *map_uids,
			 const ARRAY_TYPE(seq_range) *expunge_map_uids)
{
	const struct dbox_map_append *appends;
	struct dbox_map_mail_index_record rec;
	struct seq_range_iter iter;
	const uint32_t *uids;
	unsigned int i, j, map_uids_count, appends_count;
	uint32_t uid, seq;

	if (dbox_map_assign_file_ids(ctx, FALSE) < 0)
		return -1;

	memset(&rec, 0, sizeof(rec));
	appends = array_get(&ctx->appends, &appends_count);

	uids = array_get(map_uids, &map_uids_count);
	for (i = j = 0; i < map_uids_count; i++) {
		struct mdbox_file *mfile =
			(struct mdbox_file *)appends[j].file_append->file;

		i_assert(j < appends_count);
		rec.file_id = mfile->file_id;
		rec.offset = appends[j].offset;
		rec.size = appends[j].size;
		j++;

		if (!mail_index_lookup_seq(ctx->sync_view, uids[i], &seq))
			i_unreached();
		mail_index_update_ext(ctx->sync_trans, seq,
				      ctx->map->map_ext_id, &rec, NULL);
	}

	seq_range_array_iter_init(&iter, expunge_map_uids); i = 0;
	while (seq_range_array_iter_nth(&iter, i++, &uid)) {
		if (!mail_index_lookup_seq(ctx->sync_view, uid, &seq))
			i_unreached();
		mail_index_expunge(ctx->sync_trans, seq);
	}
	return 0;
}

int dbox_map_append_commit(struct dbox_map_append_context *ctx)
{
	struct dbox_map *map = ctx->map;
	struct dbox_file_append_context **file_appends;
	unsigned int i, count;

	i_assert(ctx->trans == NULL);

	file_appends = array_get_modifiable(&ctx->file_appends, &count);
	for (i = 0; i < count; i++) {
		if (dbox_file_append_commit(&file_appends[i]) < 0)
			return -1;
	}

	if (ctx->sync_ctx != NULL) {
		if (mail_index_sync_commit(&ctx->sync_ctx) < 0) {
			mail_storage_set_internal_error(MAP_STORAGE(map));
			mail_index_reset_error(map->index);
			return -1;
		}
	}

	ctx->committed = TRUE;
	return 0;
}

void dbox_map_append_free(struct dbox_map_append_context **_ctx)
{
	struct dbox_map_append_context *ctx = *_ctx;
	struct dbox_file_append_context **file_appends;
	struct dbox_file **files;
	unsigned int i, count;

	*_ctx = NULL;

	if (ctx->trans != NULL)
		mail_index_transaction_rollback(&ctx->trans);
	if (ctx->sync_ctx != NULL)
		mail_index_sync_rollback(&ctx->sync_ctx);

	file_appends = array_get_modifiable(&ctx->file_appends, &count);
	for (i = 0; i < count; i++) {
		if (file_appends[i] != NULL)
			dbox_file_append_rollback(&file_appends[i]);
	}

	files = array_get_modifiable(&ctx->files, &count);
	for (i = 0; i < count; i++) {
		dbox_file_unlock(files[i]);
		dbox_file_unref(&files[i]);
	}

	array_free(&ctx->appends);
	array_free(&ctx->file_appends);
	array_free(&ctx->files);
	i_free(ctx);
}

uint32_t dbox_map_get_uid_validity(struct dbox_map *map)
{
	const struct mail_index_header *hdr;

	i_assert(map->view != NULL);

	hdr = mail_index_get_header(map->view);
	if (hdr->uid_validity != 0)
		return hdr->uid_validity;

	/* refresh index in case it was just changed */
	(void)dbox_map_refresh(map);
	hdr = mail_index_get_header(map->view);
	return hdr->uid_validity != 0 ? hdr->uid_validity :
		map->created_uid_validity;
}
