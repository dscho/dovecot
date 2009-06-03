/* Copyright (C) 2007 Timo Sirainen, LGPLv2.1 */

#include "lib.h"
#include "mail-storage.h"
#include "mail-namespace.h"
#include "autocreate-plugin.h"

#include <stdlib.h>

const char *autocreate_plugin_version = PACKAGE_VERSION;

static void (*autocreate_next_hook_mail_namespaces_created)
	(struct mail_namespace *ns);

static void
autocreate_mailbox(struct mail_namespace *namespaces, const char *name)
{
	struct mail_namespace *ns;
	struct mail_storage *storage;
	const char *str;
	enum mail_error error;

	ns = mail_namespace_find(namespaces, &name);
	if (ns == NULL) {
		if (namespaces->mail_set->mail_debug)
			i_info("autocreate: No namespace found for %s", name);
		return;
	}

	storage = mail_namespace_get_default_storage(ns);
	if (mail_storage_mailbox_create(storage, ns, name, FALSE) < 0) {
		str = mail_storage_get_last_error(storage, &error);
		if (error != MAIL_ERROR_EXISTS && ns->mail_set->mail_debug) {
			i_info("autocreate: Failed to create mailbox %s: %s",
			       name, str);
		}
	}
}

static void autocreate_mailboxes(struct mail_namespace *namespaces)
{
	struct mail_user *user = namespaces->user;
	char env_name[20];
	const char *name;
	unsigned int i;

	i = 1;
	name = mail_user_plugin_getenv(user, "autocreate");
	while (name != NULL) {
		autocreate_mailbox(namespaces, name);

		i_snprintf(env_name, sizeof(env_name), "autocreate%d", ++i);
		name = mail_user_plugin_getenv(user, env_name);
	}
}

static void autosubscribe_mailboxes(struct mail_namespace *namespaces)
{
	struct mail_user *user = namespaces->user;
	struct mail_namespace *ns;
	char env_name[20];
	const char *name;
	unsigned int i;

	i = 1;
	name = mail_user_plugin_getenv(user, "autosubscribe");
	while (name != NULL) {
		ns = mail_namespace_find(namespaces, &name);
		if (ns != NULL)
			(void)mailbox_list_set_subscribed(ns->list, name, TRUE);

		i_snprintf(env_name, sizeof(env_name), "autosubscribe%d", ++i);
		name = mail_user_plugin_getenv(user, env_name);
	}
}

static void
autocreate_mail_namespaces_created(struct mail_namespace *namespaces)
{
	autocreate_mailboxes(namespaces);
	autosubscribe_mailboxes(namespaces);

	if (autocreate_next_hook_mail_namespaces_created != NULL)
		autocreate_next_hook_mail_namespaces_created(namespaces);
}

void autocreate_plugin_init(void)
{
	autocreate_next_hook_mail_namespaces_created =
		hook_mail_namespaces_created;
	hook_mail_namespaces_created = autocreate_mail_namespaces_created;
}

void autocreate_plugin_deinit(void)
{
	hook_mail_namespaces_created =
		autocreate_next_hook_mail_namespaces_created;
}
