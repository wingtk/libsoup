/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-message.c: Asyncronous Callback-based SOAP Request Queue.
 *
 * Authors:
 *      Alex Graveley (alex@helixcode.com)
 *
 * Copyright (C) 2000, Helix Code, Inc.
 */

#include "soup-auth.h"
#include "soup-error.h"
#include "soup-message.h"
#include "soup-misc.h"
#include "soup-context.h"
#include "soup-private.h"
#include "soup-queue.h"
#include "soup-transfer.h"

typedef struct {
	SoupHandlerEvent   type;
	gchar             *name;
	SoupHandlerWhen    order;
	SoupHandlerFilter  filter;
	SoupHandlerFn      handler_cb;
	gpointer           user_data;

	SoupMessage       *msg;	
	guint              timeout_tag;
} SoupHandlerData;

/**
 * soup_message_new:
 * @context: a %SoupContext for the destination endpoint.
 * @method: a string which will be used as the HTTP method for the created
 * request, if NULL a GET request will be made.
 * 
 * Creates a new empty %SoupMessage, which will connect to the URL represented
 * by @context.  A reference will be added to @context.
 * 
 * The new message has a status of @SOUP_STATUS_IDLE.
 *
 * Return value: the new %SoupMessage.
 */
SoupMessage *
soup_message_new (SoupContext *context, const gchar *method) 
{
	SoupMessage *ret;

	g_return_val_if_fail (context, NULL);

	ret          = g_new0 (SoupMessage, 1);
	ret->priv    = g_new0 (SoupMessagePrivate, 1);
	ret->status  = SOUP_STATUS_IDLE;
	ret->context = context;
	ret->method  = method ? method : SOUP_METHOD_GET;

	ret->request_headers = g_hash_table_new (soup_str_case_hash, 
						 soup_str_case_equal);

	ret->response_headers = g_hash_table_new (soup_str_case_hash, 
						  soup_str_case_equal);

	ret->priv->http_version = SOUP_HTTP_1_1;

	soup_context_ref (context);

	return ret;
}

/**
 * soup_message_new_full:
 * @context: a %SoupContext for the destination endpoint.
 * @method: a string which will be used as the HTTP method for the created
 * request, if NULL a GET request will be made..
 * @req_owner: the %SoupOwnership of the passed data buffer.
 * @req_body: a data buffer containing the body of the message request.
 * @req_length: the byte length of @req_body.
 * 
 * Creates a new %SoupMessage, which will connect to the URL represented by
 * @context.  A reference is added to @context.  The request data
 * buffer will be filled from @req_owner, @req_body, and @req_length
 * respectively.
 *
 * The new message has a status of @SOUP_STATUS_IDLE.
 *
 * Return value: the new %SoupMessage.
 */
SoupMessage *
soup_message_new_full (SoupContext   *context,
		       const gchar   *method,
		       SoupOwnership  req_owner,
		       gchar         *req_body,
		       gulong         req_length)
{
	SoupMessage *ret = soup_message_new (context, method);

	ret->request.owner = req_owner;
	ret->request.body = req_body;
	ret->request.length = req_length;

	return ret;
}

/**
 * soup_message_cleanup:
 * @req: a %SoupMessage.
 * 
 * Frees any temporary resources created in the processing of @req.  Also
 * releases the active connection, if one exists. Request and response data
 * buffers are left intact. 
 */
void 
soup_message_cleanup (SoupMessage *req)
{
	g_return_if_fail (req != NULL);

	if (req->priv->read_tag) {
		soup_transfer_read_cancel (req->priv->read_tag);
		req->priv->read_tag = 0;
	}

	if (req->priv->write_tag) {
		soup_transfer_write_cancel (req->priv->write_tag);
		req->priv->write_tag = 0;
	}

	if (req->priv->connect_tag) {
		soup_context_cancel_connect (req->priv->connect_tag);
		req->priv->connect_tag = NULL;
	}
	if (req->connection) {
		soup_connection_release (req->connection);
		req->connection = NULL;
	}

	soup_active_requests = g_slist_remove (soup_active_requests, req);
}

static void
handler_free (SoupHandlerData *data)
{
	if (data->filter.type == SOUP_FILTER_HEADER)
		g_free ((gchar *) data->filter.data.header);

	if (data->timeout_tag)
		g_source_remove (data->timeout_tag);

	data->msg->priv->content_handlers = 
		g_slist_remove (data->msg->priv->content_handlers, data);

	g_free (data->name);
	g_free (data);
}

static void
finalize_message (SoupMessage *req)
{
	soup_context_unref (req->context);

	if (req->request.owner == SOUP_BUFFER_SYSTEM_OWNED)
		g_free (req->request.body);
	if (req->response.owner == SOUP_BUFFER_SYSTEM_OWNED)
		g_free (req->response.body);

	if (req->priv->req_header) 
		g_string_free (req->priv->req_header, TRUE);

	soup_message_clear_headers (req->request_headers);
	g_hash_table_destroy (req->request_headers);

	soup_message_clear_headers (req->response_headers);
	g_hash_table_destroy (req->response_headers);

	while (req->priv->content_handlers) {
		SoupHandlerData *data = req->priv->content_handlers->data;
		handler_free (data);
	}

	g_free ((gchar *) req->errorphrase);
	g_free (req->priv);
	g_free (req);
}

/**
 * soup_message_free:
 * @req: a %SoupMessage to destroy.
 * 
 * Destroys the %SoupMessage pointed to by @req. Request and response headers
 * are freed. Request and response data buffers are also freed if their
 * ownership is %SOUP_BUFFER_SYSTEM_OWNED. The message's destination context
 * will be de-referenced.
 */
void 
soup_message_free (SoupMessage *req)
{
	g_return_if_fail (req != NULL);

	soup_message_cleanup (req);

	finalize_message (req);
}

/**
 * soup_message_issue_callback:
 * @req: a %SoupMessage currently being processed.
 * @error: a %SoupErrorCode to be passed to %req's completion callback.
 * 
 * Finalizes the message request, by first freeing any temporary resources, then
 * issuing the callback function pointer passed in %soup_message_new or
 * %soup_message_new_full. If, after returning from the callback, the message
 * has not been requeued, @msg is destroyed using %soup_message_free.
 */
void
soup_message_issue_callback (SoupMessage *req)
{
	g_return_if_fail (req != NULL);

	/* 
	 * Make sure we don't have some icky recursion if the callback 
	 * runs the main loop, and the connection has some data or error 
	 * which causes the callback to be run again.
	 */
	soup_message_cleanup (req);

	if (req->priv->callback) {
		(*req->priv->callback) (req, req->priv->user_data);

		if (req->status != SOUP_STATUS_QUEUED)
			finalize_message (req);
	}
}

/**
 * soup_message_cancel:
 * @req: a %SoupMessage currently being processed.
 * 
 * Cancel a running message, and issue completion callback with a
 * %SoupTransferStatus of %SOUP_ERROR_CANCELLED. If not requeued by the
 * completion callback, the @msg will be destroyed.
 */
void 
soup_message_cancel (SoupMessage *msg) 
{
	soup_message_set_error (msg, SOUP_ERROR_CANCELLED);
	soup_message_issue_callback (msg);
}

static gboolean 
foreach_free_header_list (gchar *name, GSList *vals, gpointer notused)
{
	g_free (name);
	g_slist_foreach (vals, (GFunc) g_free, NULL);
	g_slist_free (vals);

	return TRUE;
}

void
soup_message_clear_headers       (GHashTable        *hash)
{
	g_return_if_fail (hash != NULL);

	g_hash_table_foreach_remove (hash, 
				     (GHRFunc) foreach_free_header_list, 
				     NULL);
}

void 
soup_message_remove_header (GHashTable  *hash,
			    const gchar *name)
{
	gchar *stored_key;
	GSList *vals;

	g_return_if_fail (hash != NULL);
	g_return_if_fail (name != NULL || name [0] != '\0');

	if (g_hash_table_lookup_extended (hash, 
					  name, 
					  (gpointer *) &stored_key, 
					  (gpointer *) &vals)) {
		g_hash_table_remove (hash, name);
		foreach_free_header_list (stored_key, vals, NULL);
	}
}

void 
soup_message_add_header (GHashTable  *hash,
			 const gchar *name,
			 const gchar *value) 
{
	GSList *old_value;

	g_return_if_fail (hash != NULL);
	g_return_if_fail (name != NULL || name [0] != '\0');
	g_return_if_fail (value != NULL);

	old_value = g_hash_table_lookup (hash, name);

	if (old_value)
		g_slist_append (old_value, g_strdup (value));
	else
		g_hash_table_insert (hash, 
				     g_strdup (name), 
				     g_slist_append (NULL, 
						     g_strdup (value)));
}

/**
 * soup_message_get_header:
 * @req: a %SoupMessage.
 * @name: header name.
 * 
 * Lookup the first transport header with a key equal to @name.
 *
 * Return value: the header's value or NULL if not found.
 */
const gchar *
soup_message_get_header (GHashTable *hash,
			 const gchar *name)
{
	GSList *vals;

	g_return_val_if_fail (hash != NULL, NULL);
	g_return_val_if_fail (name != NULL || name [0] != '\0', NULL);	

	vals = g_hash_table_lookup (hash, name);
	if (vals) 
		return vals->data;

	return NULL;
}

/**
 * soup_message_get_header_list:
 * @req: a %SoupMessage.
 * @name: header name.
 * 
 * Lookup the all transport request headers with a key equal to @name.
 *
 * Return value: a const pointer to a GSList of header values or NULL if not
 * found.  
 */
const GSList *
soup_message_get_header_list (GHashTable  *hash,
			      const gchar *name)
{
	g_return_val_if_fail (hash != NULL, NULL);
	g_return_val_if_fail (name != NULL || name [0] != '\0', NULL);	

	return g_hash_table_lookup (hash, name);
}

typedef struct {
	GHFunc   func;
	gpointer user_data;
} ForeachData;

static void 
foreach_value_in_list (gchar *name, GSList *vals, ForeachData *data)
{
	while (vals) {
		gchar *v = vals->data;

		(*data->func) (name, v, data->user_data);

		vals = vals->next;
	}
}

void
soup_message_foreach_header      (GHashTable        *hash,
				  GHFunc             func,
				  gpointer           user_data)
{
	ForeachData data = { func, user_data };

	g_return_if_fail (hash != NULL);
	g_return_if_fail (func != NULL);

	g_hash_table_foreach (hash, (GHFunc) foreach_value_in_list, &data);
}

typedef struct {
	GHRFunc   func;
	gpointer user_data;
} ForeachRemoveData;

static gboolean 
foreach_remove_value_in_list (gchar             *name, 
			      GSList            *vals, 
			      ForeachRemoveData *data)
{
	GSList *iter = vals;

	while (iter) {
		gchar *v = iter->data;
		gboolean ret = FALSE;

		ret = (*data->func) (name, v, data->user_data);
		if (ret) {
			GSList *next = iter->next;

			vals = g_slist_remove (vals, v);
			g_free (v);

			iter = next;
		} else
			iter = iter->next;
	}

	if (!vals) {
		g_free (name);
		return TRUE;
	} 

	return FALSE;
}

void
soup_message_foreach_remove_header (GHashTable        *hash,
				    GHRFunc            func,
				    gpointer           user_data)
{
	ForeachRemoveData data = { func, user_data };

	g_return_if_fail (hash != NULL);
	g_return_if_fail (func != NULL);

	g_hash_table_foreach_remove (hash, 
				     (GHRFunc) foreach_remove_value_in_list, 
				     &data);
}

/**
 * soup_message_queue:
 * @req: a %SoupMessage.
 * @callback: a %SoupCallbackFn which will be called after the message completes
 * or when an unrecoverable error occurs.
 * @user_data: a pointer passed to @callback.
 * 
 * Queues the message @req for sending. All messages are processed while the
 * glib main loop runs. If this %SoupMessage has been processed before, any
 * resources related to the time it was last sent are freed.
 *
 * If the response %SoupDataBuffer has an owner of %SOUP_BUFFER_USER_OWNED, the
 * message will not be queued, and @callback will be called with a
 * %SoupErrorCode of %SOUP_ERROR_CANCELLED.
 *
 * Upon message completetion, the callback specified in @callback will be
 * invoked. If after returning from this callback the message has not been
 * requeued using %soup_message_queue, %soup_message_free will be called on
 * @req.
 */
void 
soup_message_queue (SoupMessage    *req,
		    SoupCallbackFn  callback, 
		    gpointer        user_data)
{
	soup_queue_message (req, callback, user_data);
}

/**
 * soup_message_send:
 * @msg: a %SoupMessage.
 * 
 * Syncronously send @msg. This call will not return until the transfer is
 * finished successfully or there is an unrecoverable error. 
 *
 * @msg is not free'd upon return.
 *
 * Return value: the %SoupErrorClass of the error encountered while sending or
 * reading the response.
 */
SoupErrorClass
soup_message_send (SoupMessage *msg)
{
	soup_message_queue (msg, NULL, NULL);

	while (1) {
		g_main_iteration (TRUE); 
		if (msg->status == SOUP_STATUS_FINISHED || 
		    SOUP_ERROR_IS_TRANSPORT (msg->errorcode))
			break;
	}

	return msg->errorclass;
}

static SoupHandlerResult 
authorize_handler (SoupMessage *msg, gpointer user_data)
{
	gboolean proxy = GPOINTER_TO_INT (user_data);
	const GSList *vals;
	SoupAuth *auth, *old_auth;
	SoupContext *ctx;
	const SoupUri *uri;

	ctx = proxy ? soup_get_proxy () : msg->context;
	uri = soup_context_get_uri (ctx);

	vals = soup_message_get_header_list (msg->response_headers, 
					     proxy ? 
					             "Proxy-Authenticate" : 
					             "WWW-Authenticate");
	if (!vals) goto THROW_CANT_AUTHENTICATE;

        auth = soup_auth_new_from_header_list (vals);
	if (!auth) {
		soup_message_set_error_full (
			msg, 
			proxy ? 
			        SOUP_ERROR_CANT_AUTHENTICATE_PROXY : 
			        SOUP_ERROR_CANT_AUTHENTICATE,
			proxy ? 
			        "Unknown authentication scheme required by "
			        "proxy" :
			        "Unknown authentication scheme required");
		return SOUP_HANDLER_RESTART;
	}

	/*
	 * Call registered authenticate handler
	 */
	if (!uri->user && soup_auth_fn)
		(*soup_auth_fn) (auth->type,
				 (SoupUri *) uri,
				 auth->realm, 
				 soup_auth_fn_user_data);

	if (!uri->user) {
		soup_auth_free (auth);
		goto THROW_CANT_AUTHENTICATE;
	}

	/*
	 * Initialize with auth data (possibly returned from auth callback).
	 */
	soup_auth_initialize (auth, uri);

	old_auth = soup_auth_lookup (ctx);
	if (old_auth) {
		if (!soup_auth_invalidates_prior (auth, old_auth)) {
			soup_auth_free (auth);
			goto THROW_CANT_AUTHENTICATE;
		}
	}

	soup_auth_set_context (auth, ctx);

        return SOUP_HANDLER_RESEND;

 THROW_CANT_AUTHENTICATE:
	soup_message_set_error (msg, 
				proxy ? 
			                SOUP_ERROR_CANT_AUTHENTICATE_PROXY : 
			                SOUP_ERROR_CANT_AUTHENTICATE);
	return SOUP_HANDLER_RESTART;
}

static SoupHandlerResult  
redirect_handler (SoupMessage *msg, gpointer user_data)
{
	const gchar *new_loc;

	if (msg->errorclass != SOUP_ERROR_CLASS_REDIRECT || 
	    msg->priv->msg_flags & SOUP_MESSAGE_NO_REDIRECT) 
		return SOUP_HANDLER_CONTINUE;

	new_loc = soup_message_get_header (msg->response_headers, "Location");

	if (new_loc) {
		const SoupUri *old_uri;
		SoupUri *new_uri;
		SoupContext *new_ctx;

		old_uri = soup_context_get_uri (msg->context);

		new_uri = soup_uri_new (new_loc);
		if (!new_uri) 
			goto INVALID_REDIRECT;

		/* 
		 * Copy auth info from original URI.
		 */
		if (old_uri->user && !new_uri->user)
			soup_uri_set_auth (new_uri,
					   old_uri->user, 
					   old_uri->passwd, 
					   old_uri->authmech);

		new_ctx = soup_context_from_uri (new_uri);

		soup_uri_free (new_uri);

		if (!new_ctx)
			goto INVALID_REDIRECT;

		soup_message_set_context (msg, new_ctx);
		soup_context_unref (new_ctx);

		return SOUP_HANDLER_RESEND;
	}

	return SOUP_HANDLER_CONTINUE;

 INVALID_REDIRECT:
	soup_message_set_error_full (msg, 
				     SOUP_ERROR_MALFORMED,
				     "Invalid Redirect URL");
	return SOUP_HANDLER_RESTART;
}

static SoupHandlerData global_handlers [] = {
	/* 
	 * Handle redirect response codes 300, 301, 302, 303, and 305.
	 */
	{
		SOUP_HANDLER_HEADERS,
		"redirect",
		0,
		{ 
			SOUP_FILTER_HEADER,
			{
				(guint) "Location"
			},
		},
		redirect_handler, 
		NULL, 
	},
	/* 
	 * Handle authorization.
	 */
	{
		SOUP_HANDLER_HEADERS,
		"authenticate",
		0,
		{ 
			SOUP_FILTER_ERROR_CODE,
			{
				401
			},
		},
		authorize_handler, 
		GINT_TO_POINTER (FALSE), 
	},
	/* 
	 * Handle proxy authorization.
	 */
	{
		SOUP_HANDLER_HEADERS,
		"proxy-authenticate",
		0,
		{ 
			SOUP_FILTER_ERROR_CODE,
			{
				407
			},
		},
		authorize_handler, 
		GINT_TO_POINTER (TRUE), 
	},
	{ 0 }
};

static inline SoupHandlerResult 
run_handler (SoupMessage      *msg, 
	     SoupHandlerEvent  invoke_type, 
	     SoupHandlerEvent  when, 
	     SoupHandlerData  *data)
{
	SoupHandlerResult result;

	if (data->type != invoke_type || data->order != when) 
		return SOUP_HANDLER_CONTINUE;

	switch (data->filter.type) {
	case SOUP_FILTER_HEADER:
		if (!soup_message_get_header (msg->response_headers,
					      data->filter.data.header))
			return SOUP_HANDLER_CONTINUE;
		break;
	case SOUP_FILTER_ERROR_CODE:
		if (msg->errorcode != data->filter.data.errorcode) 
			return SOUP_HANDLER_CONTINUE;
		break;
	case SOUP_FILTER_ERROR_CLASS:
		if (msg->errorclass != data->filter.data.errorclass) 
			return SOUP_HANDLER_CONTINUE;
		break;
	case SOUP_FILTER_TIMEOUT:
		return SOUP_HANDLER_CONTINUE;
	default:
		break;
	}

	result = (*data->handler_cb) (msg, data->user_data);

	switch (result) {
	case SOUP_HANDLER_STOP:
		if (invoke_type == SOUP_HANDLER_FINISHED && 
		    msg->errorclass != SOUP_ERROR_CLASS_INFORMATIONAL)
			soup_message_issue_callback (msg);
		break;
	case SOUP_HANDLER_KILL:
		soup_message_issue_callback (msg);
		break;
	case SOUP_HANDLER_RESEND:
		if (msg->status != SOUP_STATUS_QUEUED)
			soup_message_queue (msg,
					    msg->priv->callback,
					    msg->priv->user_data);
		break;
	default:
		if (msg->status == SOUP_STATUS_QUEUED)
			result = SOUP_HANDLER_RESEND;
		break;
	}

	return result;
}

#define PROCESS_HANDLER_RESULT(result) ({ \
	switch (result) {                 \
	case SOUP_HANDLER_STOP:           \
		return FALSE;             \
	case SOUP_HANDLER_KILL:           \
	case SOUP_HANDLER_RESEND:         \
		return TRUE;              \
	case SOUP_HANDLER_RESTART:        \
		goto RESTART;             \
	default:                          \
		break;                    \
	}                                 \
})

/*
 * Run each handler with matching criteria (first per-message then global
 * handlers). If a handler requeues a message, we stop processing and terminate
 * the current request. 
 *
 * After running all handlers, if there is an error set or the invoke type was
 * post_body, issue the final callback.  
 *
 * FIXME: If the errorcode is changed by a handler, we should restart the
 * processing.  
 */
gboolean
soup_message_run_handlers (SoupMessage *msg, SoupHandlerEvent invoke_type)
{
	GSList *list;
	SoupHandlerData *data;
	SoupHandlerResult result;

	g_return_val_if_fail (msg != NULL, FALSE);

 RESTART:
	/*
	 * Pre-Global handlers
	 */
	for (list = msg->priv->content_handlers; list; list = list->next) {
		data = list->data;
		result = run_handler (msg, 
				      invoke_type, 
				      SOUP_HANDLER_FIRST, 
				      data);
		PROCESS_HANDLER_RESULT (result);
	}

	/*
	 * Global handlers
	 */
	for (data = global_handlers; data->type; data++) {
		result = run_handler (msg, 
				      invoke_type, 
				      0, 
				      data);
		PROCESS_HANDLER_RESULT (result);
	}

	/*
	 * Post-Global handlers
	 */
	for (list = msg->priv->content_handlers; list; list = list->next) {
		data = list->data;
		result = run_handler (msg, 
				      invoke_type, 
				      SOUP_HANDLER_LAST, 
				      data);
		PROCESS_HANDLER_RESULT (result);
	}

	/*
	 * Issue final callback if the invoke_type is FINISHED and the error
	 * class is not INFORMATIONAL. 
	 */
	if (invoke_type == SOUP_HANDLER_FINISHED && 
	    msg->errorclass != SOUP_ERROR_CLASS_INFORMATIONAL) {
		soup_message_issue_callback (msg);
		return TRUE;
	}

	return FALSE;
}

static gboolean
timeout_handler (gpointer user_data)
{
	SoupHandlerData *data = user_data;
	SoupMessage *msg = data->msg;
	SoupHandlerResult result;
	GSList *iter;

	switch (data->type) {
	case SOUP_HANDLER_PREPARE:
		if (msg->status >= SOUP_STATUS_SENDING_REQUEST)
			goto REMOVE_SOURCE;
	case SOUP_HANDLER_HEADERS:
	case SOUP_HANDLER_DATA:
		if (msg->status >= SOUP_STATUS_READING_RESPONSE &&
		    g_hash_table_size (msg->response_headers) > 0)
			goto REMOVE_SOURCE;
	case SOUP_HANDLER_FINISHED:
		if (msg->status == SOUP_STATUS_FINISHED)
			goto REMOVE_SOURCE;
	case SOUP_HANDLER_DATA_SENT:
		iter = msg->priv->content_handlers; 
		while (iter) {
			SoupHandlerData *hd = iter->data;
			if (!g_strcasecmp (hd->name, "server-message"))
				goto REMOVE_SOURCE;
		}
	}

	result = (*data->handler_cb) (msg, data->user_data);

	switch (result) {
	case SOUP_HANDLER_KILL:
		soup_message_cancel (msg);
		break;
	case SOUP_HANDLER_RESEND:
		soup_message_queue (msg,
				    msg->priv->callback,
				    msg->priv->user_data);
		break;
	default:
		break;
	}

 REMOVE_SOURCE:
	data->timeout_tag = 0;
	return FALSE;
}

void 
soup_message_add_handler_full (SoupMessage       *msg,
			       const gchar       *name,
			       SoupHandlerEvent   type,
			       SoupHandlerWhen    order,
			       SoupHandlerFilter *filter,
			       SoupHandlerFn      handler_cb,
			       gpointer           user_data)
{
	SoupHandlerData *data;

	g_return_if_fail (msg != NULL);
	g_return_if_fail (type != 0);
	g_return_if_fail (order != 0);
	g_return_if_fail (handler_cb != NULL);

	data = g_new0 (SoupHandlerData, 1);
	data->type = type;
	data->handler_cb = handler_cb;
	data->user_data = user_data;
	data->name = g_strdup (name);
	data->order = order;
	data->msg = msg;

	if (filter) {
		data->filter.type = filter->type;
	
		switch (filter->type) {
		case SOUP_FILTER_HEADER:
			data->filter.data.header = 
				g_strdup (filter->data.header);
			break;
		case SOUP_FILTER_ERROR_CODE:
			data->filter.data.errorcode = filter->data.errorcode;
			break;
		case SOUP_FILTER_ERROR_CLASS:
			data->filter.data.errorclass = filter->data.errorclass;
			break;
		case SOUP_FILTER_TIMEOUT:
			data->filter.data.timeout = filter->data.timeout;
			data->timeout_tag = 
				g_timeout_add (filter->data.timeout * 1000,
					       timeout_handler,
					       data);
			break;
		default:
			break;
		}
	}

	msg->priv->content_handlers = 
		g_slist_append (msg->priv->content_handlers, data);
}

void 
soup_message_add_handler (SoupMessage       *msg,
			  SoupHandlerEvent   type,
			  SoupHandlerFilter *filter,
			  SoupHandlerFn      handler_cb,
			  gpointer           user_data)
{
	g_return_if_fail (msg != NULL);
	g_return_if_fail (type != 0);
	g_return_if_fail (handler_cb != NULL);

	soup_message_add_handler_full (msg,
				       NULL,
				       type,
				       SOUP_HANDLER_LAST,
				       filter,
				       handler_cb,
				       user_data);
}

GSList *
soup_message_list_handlers (SoupMessage *msg)
{
	GSList *ret = NULL, *list;

	g_return_val_if_fail (msg != NULL, NULL);

	for (list = msg->priv->content_handlers; list; list = list->next) {
		SoupHandlerData *data = list->data;
		if (data->name)
			ret = g_slist_append (ret, data->name);
	}

	return ret;
}

void 
soup_message_remove_handler (SoupMessage       *msg, 
			     gchar             *name)
{
	GSList *iter;

	g_return_if_fail (msg != NULL);
	g_return_if_fail (name != NULL);

	iter = msg->priv->content_handlers;
	while (iter) {
		SoupHandlerData *data = iter->data;

		if (data->name && !g_strcasecmp (data->name, name)) {
			handler_free (data);
			break;
		}

		iter = iter->next;
	}
}

void 
soup_message_remove_handler_by_func (SoupMessage       *msg, 
				     SoupHandlerFn      handler_cb)
{
	GSList *iter;

	g_return_if_fail (msg != NULL);
	g_return_if_fail (handler_cb != NULL);

	iter = msg->priv->content_handlers;
	while (iter) {
		SoupHandlerData *data = iter->data;

		if (data->handler_cb == handler_cb) {
			handler_free (data);
			break;
		}

		iter = iter->next;
	}
}

void 
soup_message_remove_handler_by_func_and_data (SoupMessage       *msg, 
					      SoupHandlerFn      handler_cb,
					      gpointer           user_data)
{
	GSList *iter;

	g_return_if_fail (msg != NULL);
	g_return_if_fail (handler_cb != NULL);

	iter = msg->priv->content_handlers;
	while (iter) {
		SoupHandlerData *data = iter->data;

		if (data->handler_cb == handler_cb && 
		    data->user_data == user_data) {
			handler_free (data);
			break;
		}

		iter = iter->next;
	}
}

static inline gboolean
ADDED_FLAG (SoupMessage *msg, guint newflags, SoupMessageFlags find)
{
	return ((newflags & find) && !(msg->priv->msg_flags & find));
}

static inline gboolean
REMOVED_FLAG (SoupMessage *msg, guint newflags, SoupMessageFlags find)
{
	return (!(newflags & find) && (msg->priv->msg_flags & find));
}

void
soup_message_set_flags (SoupMessage *msg, guint flags)
{
	g_return_if_fail (msg != NULL);

	msg->priv->msg_flags = flags;
}

guint
soup_message_get_flags (SoupMessage *msg)
{
	g_return_val_if_fail (msg != NULL, 0);

	return msg->priv->msg_flags;
}

void 
soup_message_set_http_version  (SoupMessage *msg, SoupHttpVersion version)
{
	g_return_if_fail (msg != NULL);

	msg->priv->http_version = version;
}

SoupHttpVersion
soup_message_get_http_version (SoupMessage *msg)
{
	g_return_val_if_fail (msg != NULL, SOUP_HTTP_1_0);

	return msg->priv->http_version;
}

void
soup_message_set_context (SoupMessage       *msg,
			  SoupContext       *new_ctx)
{
	g_return_if_fail (msg != NULL);
	g_return_if_fail (new_ctx != NULL);

	soup_context_unref (msg->context);
	soup_context_ref (new_ctx);

	msg->context = new_ctx;
}

SoupContext *
soup_message_get_context (SoupMessage       *msg)
{
	g_return_val_if_fail (msg != NULL, NULL);

	soup_context_ref (msg->context);
	return msg->context;
}

void
soup_message_set_error (SoupMessage *msg, SoupKnownErrorCode errcode)
{
	g_return_if_fail (msg != NULL);
	g_return_if_fail (errcode != 0);

	g_free ((gchar *) msg->errorphrase);

	msg->errorcode = errcode;
	msg->errorclass = soup_error_get_class (errcode);
	msg->errorphrase = g_strdup (soup_error_get_phrase (errcode));
}

void
soup_message_set_error_full (SoupMessage *msg, 
			     guint        errcode, 
			     const gchar *errphrase)
{
	g_return_if_fail (msg != NULL);
	g_return_if_fail (errcode != 0);
	g_return_if_fail (errphrase != NULL);

	g_free ((gchar *) msg->errorphrase);

	msg->errorcode = errcode;
	msg->errorclass = soup_error_get_class (errcode);
	msg->errorphrase = g_strdup (errphrase);
}

void
soup_message_set_handler_error (SoupMessage *msg, 
				guint        errcode, 
				const gchar *errphrase)
{
	g_return_if_fail (msg != NULL);
	g_return_if_fail (errcode != 0);
	g_return_if_fail (errphrase != NULL);

	g_free ((gchar *) msg->errorphrase);

	msg->errorcode = errcode;
	msg->errorclass = SOUP_ERROR_CLASS_HANDLER;
	msg->errorphrase = g_strdup (errphrase);
}
