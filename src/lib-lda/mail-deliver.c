/* Copyright (c) 2005-2017 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "str.h"
#include "str-sanitize.h"
#include "time-util.h"
#include "unichar.h"
#include "var-expand.h"
#include "message-address.h"
#include "lda-settings.h"
#include "mail-storage.h"
#include "mail-namespace.h"
#include "duplicate.h"
#include "mail-deliver.h"

deliver_mail_func_t *deliver_mail = NULL;

static const char *lda_log_wanted_headers[] = {
	"From", "Message-ID", "Subject",
	NULL
};
static enum mail_fetch_field lda_log_wanted_fetch_fields =
	MAIL_FETCH_PHYSICAL_SIZE | MAIL_FETCH_VIRTUAL_SIZE;

const char *mail_deliver_get_address(struct mail *mail, const char *header)
{
	struct message_address *addr;
	const char *str;

	if (mail_get_first_header(mail, header, &str) <= 0)
		return NULL;
	addr = message_address_parse(pool_datastack_create(),
				     (const unsigned char *)str,
				     strlen(str), 1, FALSE);
	return addr == NULL || addr->mailbox == NULL || addr->domain == NULL ||
		*addr->mailbox == '\0' || *addr->domain == '\0' ?
		NULL : t_strconcat(addr->mailbox, "@", addr->domain, NULL);
}

static void
mail_deliver_log_var_expand_table_update_times(struct mail_deliver_context *ctx,
					       struct var_expand_table *tab)
{
#define VAR_EXPAND_DELIVERY_TIME_IDX 7
	int delivery_time_msecs;

	io_loop_time_refresh();
	delivery_time_msecs = timeval_diff_msecs(&ioloop_timeval,
						 &ctx->delivery_time_started);
	tab[VAR_EXPAND_DELIVERY_TIME_IDX].value = dec2str(delivery_time_msecs);
}

static const struct var_expand_table *
mail_deliver_get_log_var_expand_table_full(struct mail_deliver_context *ctx,
					   struct mail *mail,
					   const char *message)
{
	const char *message_id, *subject = NULL, *from_envelope = NULL;
	const char *from, *psize = NULL, *vsize = NULL, *storage_id = NULL;
	const char *session_time = NULL, *to_envelope = NULL;
	uoff_t size;

	if (mail_get_first_header(mail, "Message-ID", &message_id) <= 0)
		message_id = "unspecified";
	else
		message_id = str_sanitize(message_id, 200);

	if (mail_get_first_header_utf8(mail, "Subject", &subject) > 0)
		subject = str_sanitize(subject, 80);
	from = str_sanitize(mail_deliver_get_address(mail, "From"), 80);

	if (mail_get_special(mail, MAIL_FETCH_FROM_ENVELOPE, &from_envelope) > 0)
		from_envelope = str_sanitize(from_envelope, 80);

	if (mail_get_physical_size(mail, &size) == 0)
		psize = dec2str(size);
	if (mail_get_virtual_size(mail, &size) == 0)
		vsize = dec2str(size);
	session_time = dec2str(ctx->session_time_msecs);
	to_envelope = ctx->dest_addr;
	(void)mail_get_special(mail, MAIL_FETCH_STORAGE_ID, &storage_id);

	const struct var_expand_table stack_tab[] = {
		{ '$', message, NULL },
		{ 'm', message_id, "msgid" },
		{ 's', subject, "subject" },
		{ 'f', from, "from" },
		{ 'e', from_envelope, "from_envelope" },
		{ 'p', psize, "size" },
		{ 'w', vsize, "vsize" },
		/* must be VAR_EXPAND_DELIVERY_TIME_IDX */
		{ '\0', NULL, "delivery_time" },
		{ '\0', session_time, "session_time" },
		{ '\0', to_envelope, "to_envelope" },
		{ '\0', storage_id, "storage_id" },
		{ '\0', NULL, NULL }
	};
	struct var_expand_table *tab;

	tab = t_malloc_no0(sizeof(stack_tab));
	memcpy(tab, stack_tab, sizeof(stack_tab));
	mail_deliver_log_var_expand_table_update_times(ctx, tab);
	return tab;
}

const struct var_expand_table *
mail_deliver_ctx_get_log_var_expand_table(struct mail_deliver_context *ctx,
					  const char *message)
{
	struct mail *mail;

	mail = ctx->dest_mail != NULL ? ctx->dest_mail : ctx->src_mail;
	return mail_deliver_get_log_var_expand_table_full(ctx, mail, message);
}

static void
mail_deliver_log_cache_var_expand_table(struct mail_deliver_context *ctx)
{
	const struct var_expand_table *src;
	struct var_expand_table *dest;
	unsigned int i, len;

	src = mail_deliver_ctx_get_log_var_expand_table(ctx, "");
	for (len = 0; src[len].key != '\0' || src[len].long_key != NULL; len++) ;

	dest = p_new(ctx->pool, struct var_expand_table, len + 1);
	for (i = 0; i < len; i++) {
		dest[i] = src[i];
		dest[i].value = p_strdup(ctx->pool, src[i].value);
	}
	ctx->var_expand_table = dest;
}

void mail_deliver_log(struct mail_deliver_context *ctx, const char *fmt, ...)
{
	va_list args;
	string_t *str;
	const char *msg, *error;

	if (*ctx->set->deliver_log_format == '\0')
		return;

	va_start(args, fmt);
	msg = t_strdup_vprintf(fmt, args);

	str = t_str_new(256);

	if (ctx->var_expand_table == NULL)
		mail_deliver_log_cache_var_expand_table(ctx);
	/* update %$ */
	ctx->var_expand_table[0].value = msg;
	mail_deliver_log_var_expand_table_update_times(ctx, ctx->var_expand_table);
	if (var_expand(str, ctx->set->deliver_log_format,
		       ctx->var_expand_table, &error) <= 0) {
		i_error("Failed to expand deliver_log_format=%s: %s",
			ctx->set->deliver_log_format, error);
	}
	ctx->var_expand_table[0].value = "";
	ctx->var_expand_table[VAR_EXPAND_DELIVERY_TIME_IDX].value = "";

	i_info("%s", str_c(str));
	va_end(args);
}

struct mail_deliver_session *mail_deliver_session_init(void)
{
	struct mail_deliver_session *session;
	pool_t pool;

	pool = pool_alloconly_create("mail deliver session", 1024);
	session = p_new(pool, struct mail_deliver_session, 1);
	session->pool = pool;
	return session;
}

void mail_deliver_session_deinit(struct mail_deliver_session **_session)
{
	struct mail_deliver_session *session = *_session;

	*_session = NULL;
	pool_unref(&session->pool);
}

int mail_deliver_save_open(struct mail_deliver_save_open_context *ctx,
			   const char *name, struct mailbox **box_r,
			   enum mail_error *error_r, const char **error_str_r)
{
	struct mail_namespace *ns;
	struct mailbox *box;
	enum mailbox_flags flags =
		MAILBOX_FLAG_SAVEONLY | MAILBOX_FLAG_POST_SESSION;

	*box_r = NULL;
	*error_r = MAIL_ERROR_NONE;
	*error_str_r = NULL;

	if (!uni_utf8_str_is_valid(name)) {
		*error_str_r = "Mailbox name not valid UTF-8";
		*error_r = MAIL_ERROR_PARAMS;
		return -1;
	}

	ns = mail_namespace_find(ctx->user->namespaces, name);
	if (strcmp(name, ns->prefix) == 0 &&
	    (ns->flags & NAMESPACE_FLAG_INBOX_USER) != 0) {
		/* delivering to a namespace prefix means we actually want to
		   deliver to the INBOX instead */
		name = "INBOX";
		ns = mail_namespace_find_inbox(ctx->user->namespaces);
	}

	if (strcasecmp(name, "INBOX") == 0) {
		/* deliveries to INBOX must always succeed,
		   regardless of ACLs */
		flags |= MAILBOX_FLAG_IGNORE_ACLS;
	}

	*box_r = box = mailbox_alloc(ns->list, name, flags);
	if (mailbox_open(box) == 0)
		return 0;

	*error_str_r = mailbox_get_last_error(box, error_r);
	if (!ctx->lda_mailbox_autocreate || *error_r != MAIL_ERROR_NOTFOUND)
		return -1;

	/* try creating it. */
	if (mailbox_create(box, NULL, FALSE) < 0) {
		*error_str_r = mailbox_get_last_error(box, error_r);
		if (*error_r != MAIL_ERROR_EXISTS)
			return -1;
		/* someone else just created it */
	}
	if (ctx->lda_mailbox_autosubscribe) {
		/* (try to) subscribe to it */
		(void)mailbox_set_subscribed(box, TRUE);
	}

	/* and try opening again */
	if (mailbox_open(box) < 0) {
		*error_str_r = mailbox_get_last_error(box, error_r);
		return -1;
	}
	return 0;
}

static bool mail_deliver_check_duplicate(struct mail_deliver_session *session,
					 struct mailbox *box)
{
	struct mailbox_metadata metadata;
	const guid_128_t *guid;

	if (mailbox_get_metadata(box, MAILBOX_METADATA_GUID, &metadata) < 0) {
		/* just play it safe and assume a duplicate */
		return TRUE;
	}

	/* there shouldn't be all that many recipients,
	   so just do a linear search */
	if (!array_is_created(&session->inbox_guids))
		p_array_init(&session->inbox_guids, session->pool, 8);
	array_foreach(&session->inbox_guids, guid) {
		if (memcmp(metadata.guid, *guid, sizeof(metadata.guid)) == 0)
			return TRUE;
	}
	array_append(&session->inbox_guids, &metadata.guid, 1);
	return FALSE;
}

void mail_deliver_deduplicate_guid_if_needed(struct mail_deliver_session *session,
					     struct mail_save_context *save_ctx)
{
	struct mailbox_transaction_context *trans =
		mailbox_save_get_transaction(save_ctx);
	struct mailbox *box = mailbox_transaction_get_mailbox(trans);
	guid_128_t guid;

	if (strcmp(mailbox_get_name(box), "INBOX") != 0)
		return;

	/* avoid storing duplicate GUIDs to delivered mails to INBOX. this
	   happens if mail is delivered to same user multiple times within a
	   session. the problem with this is that if GUIDs are used as POP3
	   UIDLs, some clients can't handle the duplicates well. */
	if (mail_deliver_check_duplicate(session, box)) {
		guid_128_generate(guid);
		mailbox_save_set_guid(save_ctx, guid_128_to_string(guid));
	}
}

static struct mail *
mail_deliver_open_mail(struct mailbox *box,
		       const struct mail_transaction_commit_changes *changes,
		       enum mail_fetch_field wanted_fields,
		       struct mailbox_transaction_context **trans_r)
{
	struct mailbox_transaction_context *t;
	struct mail *mail;
	const struct seq_range *range;

	*trans_r = NULL;

	if (mailbox_sync(box, MAILBOX_SYNC_FLAG_FAST) < 0)
		return NULL;

	range = array_idx(&changes->saved_uids, 0);
	i_assert(range[0].seq1 == range[0].seq2);

	t = mailbox_transaction_begin(box, 0);
	mail = mail_alloc(t, wanted_fields, NULL);

	if (!mail_set_uid(mail, range[0].seq1)) {
		mail_free(&mail);
		mailbox_transaction_rollback(&t);
	}
	*trans_r = t;
	return mail;
}

int mail_deliver_save(struct mail_deliver_context *ctx, const char *mailbox,
		      enum mail_flags flags, const char *const *keywords,
		      struct mail_storage **storage_r)
{
	struct mail_deliver_save_open_context open_ctx;
	struct mailbox *box;
	enum mailbox_transaction_flags trans_flags;
	struct mailbox_transaction_context *t;
	struct mail_save_context *save_ctx;
	struct mailbox_header_lookup_ctx *headers_ctx;
	struct mail_keywords *kw;
	enum mail_error error;
	const char *mailbox_name, *errstr, *guid;
	struct mail_transaction_commit_changes changes;
	bool default_save;
	int ret = 0;

	i_assert(ctx->dest_mail == NULL);

	default_save = strcmp(mailbox, ctx->dest_mailbox_name) == 0;
	if (default_save)
		ctx->tried_default_save = TRUE;

	i_zero(&open_ctx);
	open_ctx.user = ctx->dest_user;
	open_ctx.lda_mailbox_autocreate = ctx->set->lda_mailbox_autocreate;
	open_ctx.lda_mailbox_autosubscribe = ctx->set->lda_mailbox_autosubscribe;

	mailbox_name = str_sanitize(mailbox, 80);
	if (mail_deliver_save_open(&open_ctx, mailbox, &box,
				   &error, &errstr) < 0) {
		if (box != NULL) {
			*storage_r = mailbox_get_storage(box);
			mailbox_free(&box);
		}
		mail_deliver_log(ctx, "save failed to open mailbox %s: %s",
				 mailbox_name, errstr);
		return -1;
	}
	*storage_r = mailbox_get_storage(box);

	trans_flags = MAILBOX_TRANSACTION_FLAG_EXTERNAL;
	if (ctx->save_dest_mail)
		trans_flags |= MAILBOX_TRANSACTION_FLAG_ASSIGN_UIDS;
	t = mailbox_transaction_begin(box, trans_flags);

	kw = str_array_length(keywords) == 0 ? NULL :
		mailbox_keywords_create_valid(box, keywords);
	save_ctx = mailbox_save_alloc(t);
	if (ctx->src_envelope_sender != NULL)
		mailbox_save_set_from_envelope(save_ctx, ctx->src_envelope_sender);
	mailbox_save_set_flags(save_ctx, flags, kw);

	headers_ctx = mailbox_header_lookup_init(box, lda_log_wanted_headers);
	ctx->dest_mail = mailbox_save_get_dest_mail(save_ctx);
	mail_add_temp_wanted_fields(ctx->dest_mail, lda_log_wanted_fetch_fields, NULL);
	mailbox_header_lookup_unref(&headers_ctx);
	mail_deliver_deduplicate_guid_if_needed(ctx->session, save_ctx);

	if (mailbox_save_using_mail(&save_ctx, ctx->src_mail) < 0)
		ret = -1;
	else
		mail_deliver_log_cache_var_expand_table(ctx);
	if (kw != NULL)
		mailbox_keywords_unref(&kw);

	if (ret < 0)
		mailbox_transaction_rollback(&t);
	else
		ret = mailbox_transaction_commit_get_changes(&t, &changes);

	if (ret == 0) {
		ctx->saved_mail = TRUE;
		if (ctx->save_dest_mail) {
			/* copying needs the message body. with maildir we also
			   need to get the GUID in case the message gets
			   expunged */
			ctx->dest_mail = mail_deliver_open_mail(box, &changes,
				MAIL_FETCH_STREAM_BODY | MAIL_FETCH_GUID, &t);
			if (mail_get_special(ctx->dest_mail, MAIL_FETCH_GUID, &guid) < 0) {
				mail_free(&ctx->dest_mail);
				mailbox_transaction_rollback(&t);
			}
			/* might as well get the storage_id */
			(void)mail_get_special(ctx->dest_mail, MAIL_FETCH_STORAGE_ID,
					       &ctx->var_expand_table[10].value);
		} else if (var_has_key(ctx->set->deliver_log_format, '\0', "storage_id")) {
			/* storage ID is available only after commit. */
			struct mail *mail = mail_deliver_open_mail(box, &changes,
				MAIL_FETCH_STORAGE_ID, &t);
			if (mail != NULL) {
				const char *str;

				(void)mail_get_special(mail, MAIL_FETCH_STORAGE_ID, &str);
				ctx->var_expand_table[10].value = t_strdup(str);
				mail_free(&mail);
				(void)mailbox_transaction_commit(&t);
			}
		}
		mail_deliver_log(ctx, "saved mail to %s", mailbox_name);
		pool_unref(&changes.pool);
	} else {
		mail_deliver_log(ctx, "save failed to %s: %s", mailbox_name,
			mail_storage_get_last_error(*storage_r, &error));
	}

	if (ctx->dest_mail == NULL)
		mailbox_free(&box);
	return ret;
}

const char *mail_deliver_get_return_address(struct mail_deliver_context *ctx)
{
	if (ctx->src_envelope_sender != NULL)
		return ctx->src_envelope_sender;

	return mail_deliver_get_address(ctx->src_mail, "Return-Path");
}

const char *mail_deliver_get_new_message_id(struct mail_deliver_context *ctx)
{
	static int count = 0;

	return t_strdup_printf("<dovecot-%s-%s-%d@%s>",
			       dec2str(ioloop_timeval.tv_sec),
			       dec2str(ioloop_timeval.tv_usec),
			       count++, ctx->set->hostname);
}

static bool mail_deliver_is_tempfailed(struct mail_deliver_context *ctx,
				       struct mail_storage *storage)
{
	enum mail_error error;

	if (ctx->tempfail_error != NULL)
		return TRUE;
	if (storage != NULL) {
		(void)mail_storage_get_last_error(storage, &error);
		return error == MAIL_ERROR_TEMP;
	}
	return FALSE;
}

int mail_deliver(struct mail_deliver_context *ctx,
		 struct mail_storage **storage_r)
{
	int ret;

	*storage_r = NULL;
	if (deliver_mail == NULL)
		ret = -1;
	else {
		ctx->dup_ctx = duplicate_init(ctx->dest_user);
		if (deliver_mail(ctx, storage_r) <= 0) {
			/* if message was saved, don't bounce it even though
			   the script failed later. */
			ret = ctx->saved_mail ? 0 : -1;
		} else {
			/* success. message may or may not have been saved. */
			ret = 0;
		}
		duplicate_deinit(&ctx->dup_ctx);
		if (ret < 0 && mail_deliver_is_tempfailed(ctx, *storage_r))
			return -1;
	}

	if (ret < 0 && !ctx->tried_default_save) {
		/* plugins didn't handle this. save into the default mailbox. */
		ret = mail_deliver_save(ctx, ctx->dest_mailbox_name, 0, NULL,
					storage_r);
		if (ret < 0 && mail_deliver_is_tempfailed(ctx, *storage_r))
			return -1;
	}
	if (ret < 0 && strcasecmp(ctx->dest_mailbox_name, "INBOX") != 0) {
		/* still didn't work. try once more to save it
		   to INBOX. */
		ret = mail_deliver_save(ctx, "INBOX", 0, NULL, storage_r);
	}
	return ret;
}

deliver_mail_func_t *mail_deliver_hook_set(deliver_mail_func_t *new_hook)
{
	deliver_mail_func_t *old_hook = deliver_mail;

	deliver_mail = new_hook;
	return old_hook;
}
