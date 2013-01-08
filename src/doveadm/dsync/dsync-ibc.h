#ifndef DSYNC_IBC_H
#define DSYNC_IBC_H

/* dsync inter-brain communicator */

#include "ioloop.h"
#include "guid.h"
#include "dsync-brain.h"

struct dsync_mailbox;
struct dsync_mailbox_state;
struct dsync_mailbox_node;
struct dsync_mailbox_delete;
struct dsync_mail;
struct dsync_mail_change;
struct dsync_mail_request;

enum dsync_ibc_send_ret {
	DSYNC_IBC_SEND_RET_OK	= 1,
	/* send queue is full, stop sending more */
	DSYNC_IBC_SEND_RET_FULL	= 0
};

enum dsync_ibc_recv_ret {
	DSYNC_IBC_RECV_RET_FINISHED	= -1,
	/* try again / error (the error handling delayed until io callback) */
	DSYNC_IBC_RECV_RET_TRYAGAIN	= 0,
	DSYNC_IBC_RECV_RET_OK		= 1
};

struct dsync_ibc_settings {
	/* if non-NULL, sync only this namespace */
	const char *sync_ns_prefix;
	/* if non-NULL, sync only this mailbox name */
	const char *sync_box;

	enum dsync_brain_sync_type sync_type;
	enum dsync_brain_flags brain_flags;
};

void dsync_ibc_init_pipe(struct dsync_ibc **ibc1_r,
			 struct dsync_ibc **ibc2_r);
struct dsync_ibc *
dsync_ibc_init_stream(struct istream *input, struct ostream *output,
		      const char *name, const char *temp_path_prefix);
void dsync_ibc_deinit(struct dsync_ibc **ibc);

/* I/O callback is called whenever new data is available. It's also called on
   errors, so check first the error status. */
void dsync_ibc_set_io_callback(struct dsync_ibc *ibc,
			       io_callback_t *callback, void *context);

void dsync_ibc_send_handshake(struct dsync_ibc *ibc,
			      const struct dsync_ibc_settings *set);
enum dsync_ibc_recv_ret
dsync_ibc_recv_handshake(struct dsync_ibc *ibc,
			 const struct dsync_ibc_settings **set_r);

enum dsync_ibc_send_ret ATTR_NOWARN_UNUSED_RESULT
dsync_ibc_send_end_of_list(struct dsync_ibc *ibc);

enum dsync_ibc_send_ret ATTR_NOWARN_UNUSED_RESULT
dsync_ibc_send_mailbox_state(struct dsync_ibc *ibc,
			     const struct dsync_mailbox_state *state);
enum dsync_ibc_recv_ret
dsync_ibc_recv_mailbox_state(struct dsync_ibc *ibc,
			     struct dsync_mailbox_state *state_r);

enum dsync_ibc_send_ret ATTR_NOWARN_UNUSED_RESULT
dsync_ibc_send_mailbox_tree_node(struct dsync_ibc *ibc,
				 const char *const *name,
				 const struct dsync_mailbox_node *node);
enum dsync_ibc_recv_ret
dsync_ibc_recv_mailbox_tree_node(struct dsync_ibc *ibc,
				 const char *const **name_r,
				 const struct dsync_mailbox_node **node_r);

enum dsync_ibc_send_ret ATTR_NOWARN_UNUSED_RESULT
dsync_ibc_send_mailbox_deletes(struct dsync_ibc *ibc,
			       const struct dsync_mailbox_delete *deletes,
			       unsigned int count, char hierarchy_sep);
enum dsync_ibc_recv_ret
dsync_ibc_recv_mailbox_deletes(struct dsync_ibc *ibc,
			       const struct dsync_mailbox_delete **deletes_r,
			       unsigned int *count_r, char *hierarchy_sep_r);

enum dsync_ibc_send_ret ATTR_NOWARN_UNUSED_RESULT
dsync_ibc_send_mailbox(struct dsync_ibc *ibc,
		       const struct dsync_mailbox *dsync_box);
enum dsync_ibc_recv_ret
dsync_ibc_recv_mailbox(struct dsync_ibc *ibc,
		       const struct dsync_mailbox **dsync_box_r);

enum dsync_ibc_send_ret ATTR_NOWARN_UNUSED_RESULT
dsync_ibc_send_change(struct dsync_ibc *ibc,
		      const struct dsync_mail_change *change);
enum dsync_ibc_recv_ret
dsync_ibc_recv_change(struct dsync_ibc *ibc,
		      const struct dsync_mail_change **change_r);

enum dsync_ibc_send_ret ATTR_NOWARN_UNUSED_RESULT
dsync_ibc_send_mail_request(struct dsync_ibc *ibc,
			    const struct dsync_mail_request *request);
enum dsync_ibc_recv_ret
dsync_ibc_recv_mail_request(struct dsync_ibc *ibc,
			    const struct dsync_mail_request **request_r);

enum dsync_ibc_send_ret ATTR_NOWARN_UNUSED_RESULT
dsync_ibc_send_mail(struct dsync_ibc *ibc, const struct dsync_mail *mail);
enum dsync_ibc_recv_ret
dsync_ibc_recv_mail(struct dsync_ibc *ibc, struct dsync_mail **mail_r);

bool dsync_ibc_has_failed(struct dsync_ibc *ibc);
bool dsync_ibc_is_send_queue_full(struct dsync_ibc *ibc);
bool dsync_ibc_has_pending_data(struct dsync_ibc *ibc);

#endif
