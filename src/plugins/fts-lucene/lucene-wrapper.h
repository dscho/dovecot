#ifndef LUCENE_WRAPPER_H
#define LUCENE_WRAPPER_H

#include "fts-api-private.h"
#include "mail-types.h"

struct hash_table;
struct mailbox_list;
struct fts_expunge_log;

#define MAILBOX_GUID_HEX_LENGTH (MAIL_GUID_128_SIZE*2)

struct lucene_index_record {
	mail_guid_128_t mailbox_guid;
	uint32_t uid;
};

struct lucene_index *lucene_index_init(const char *path,
				       struct mailbox_list *list,
				       const char *textcat_dir,
				       const char *textcat_conf);
void lucene_index_deinit(struct lucene_index *index);

void lucene_index_select_mailbox(struct lucene_index *index,
				 const wchar_t guid[MAILBOX_GUID_HEX_LENGTH]);
void lucene_index_unselect_mailbox(struct lucene_index *index);
int lucene_index_get_last_uid(struct lucene_index *index, uint32_t *last_uid_r);
int lucene_index_get_doc_count(struct lucene_index *index, uint32_t *count_r);

int lucene_index_build_init(struct lucene_index *index);
int lucene_index_build_more(struct lucene_index *index, uint32_t uid,
			    const unsigned char *data, size_t size,
			    const char *hdr_name);
int lucene_index_build_deinit(struct lucene_index *index);

void lucene_index_close(struct lucene_index *index);
int lucene_index_rescan(struct lucene_index *index);
int lucene_index_expunge_from_log(struct lucene_index *index,
				  struct fts_expunge_log *log);
int lucene_index_optimize(struct lucene_index *index);

int lucene_index_lookup(struct lucene_index *index, 
			struct mail_search_arg *args, bool and_args,
			struct fts_result *result);

int lucene_index_lookup_multi(struct lucene_index *index,
			      struct hash_table *guids,
			      struct mail_search_arg *args, bool and_args,
			      struct fts_multi_result *result);

struct lucene_index_iter *
lucene_index_iter_init(struct lucene_index *index);
const struct lucene_index_record *
lucene_index_iter_next(struct lucene_index_iter *iter);
int lucene_index_iter_deinit(struct lucene_index_iter **iter);

/* internal: */
void lucene_utf8_n_to_tchar(const unsigned char *src, size_t srcsize,
			    wchar_t *dest, size_t destsize);

void lucene_shutdown(void);

#endif
