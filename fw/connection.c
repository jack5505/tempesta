/**
 *		Tempesta FW
 *
 * Generic connection management.
 *
 * Copyright (C) 2014 NatSys Lab. (info@natsys-lab.com).
 * Copyright (C) 2015-2024 Tempesta Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include "connection.h"
#include "gfsm.h"
#include "log.h"
#include "sync_socket.h"
#include "http.h"
#include "websocket.h"

TfwConnHooks *conn_hooks[TFW_CONN_MAX_PROTOS];

/*
 * Initialize the connection structure.
 * It's not on any list yet, so it's safe to do so without locks.
 */
void
tfw_connection_init(TfwConn *conn)
{
	memset(conn, 0, sizeof(*conn));
	INIT_LIST_HEAD(&conn->list);
}

void
tfw_connection_link_peer(TfwConn *conn, TfwPeer *peer)
{
	BUG_ON(conn->peer || !list_empty(&conn->list));
	conn->peer = peer;
	tfw_peer_add_conn(peer, &conn->list);
}

/**
 * Publish the "connection is established" event via TfwConnHooks.
 */
int
tfw_connection_new(TfwConn *conn)
{
	return TFW_CONN_HOOK_CALL(conn, conn_init);
}

/**
 * Call connection repairing via TfwConnHooks.
 */
void
tfw_connection_repair(TfwConn *conn)
{
	TFW_CONN_HOOK_CALL(conn, conn_repair);
}

int
tfw_connection_shutdown(TfwConn *conn, bool sync)
{
	int r;

	tfw_connection_get(conn);
	r = TFW_CONN_HOOK_CALL(conn, conn_shutdown, sync);
	tfw_connection_put(conn);

	return r;
}

int
tfw_connection_close(TfwConn *conn, bool sync)
{
	int r;

	/*
	 * When connection is closed from process context (when tempesta
	 * is shutdowning) there is a race between `ss_close` and socket
	 * and connection destruction in softirq. We should increment
	 * connection reference counter here to prevent conenction
	 * destruction in running in parallel softirq.
	 */
	tfw_connection_get(conn);
	r = TFW_CONN_HOOK_CALL(conn, conn_close, sync);
	tfw_connection_put(conn);

	return r;
}

void
tfw_connection_abort(TfwConn *conn)
{
	int r;

	/*
	 * Same as for tfw_connection_close() we should increment connection
	 * reference counter here.
	 */
	tfw_connection_get(conn);
	r = TFW_CONN_HOOK_CALL(conn, conn_abort);
	WARN_ON(r);
	tfw_connection_put(conn);
}

/**
 * Publish the "connection is dropped" event via TfwConnHooks.
 */
void
tfw_connection_drop(TfwConn *conn)
{
	/* Ask higher levels to free resources at connection close. */
	TFW_CONN_HOOK_CALL(conn, conn_drop);
}

/*
 * Publish the "connection is released" event via TfwConnHooks.
 */
void
tfw_connection_release(TfwConn *conn)
{
	/* Ask higher levels to free resources at connection release. */
	TFW_CONN_HOOK_CALL(conn, conn_release);
	BUG_ON((TFW_CONN_TYPE(conn) & Conn_Clnt)
	       && !list_empty(&((TfwCliConn *)conn)->seq_queue));
}

/*
 * Send @msg through connection @conn. Code architecture decisions
 * ensure that conn->sk remains valid for the life of @conn instance.
 * The socket itself may have been closed, but not deleted. ss_send()
 * makes sure that data is sent only on an active socket.
 *
 * Return value:
 *   0		- @msg had been sent successfully;
 *   -EBADF	- connection is broken;
 *   -EBUSY	- transmission work queue is full;
 *   -ENOMEM	- out-of-memory error occurred.
 */
int
tfw_connection_send(TfwConn *conn, TfwMsg *msg)
{
	/*
	 * NOTE: after `tfw_connection_send` returns, `msg` should not be used!
	 * See `tfw_tls_conn_send` for reference.
	 */
	return TFW_CONN_HOOK_CALL(conn, conn_send, msg);
}

int
tfw_connection_recv(TfwConn *conn, struct sk_buff *skb)
{
	int r = T_OK;
	struct sk_buff *next, *splitted;

	if (unlikely(tfw_connection_stop_rcv(conn))) {
		__kfree_skb(skb);
		return 0;
	}

	if (skb->prev)
		skb->prev->next = NULL;
	for (next = skb->next; skb;
	     skb = next, next = next ? next->next : NULL)
	{
		if (likely(r == T_OK || r == T_POSTPONE || r == T_DROP)) {
			splitted = skb->next = skb->prev = NULL;
			if (unlikely(TFW_CONN_PROTO(conn) == TFW_FSM_WS
				     || TFW_CONN_PROTO(conn) == TFW_FSM_WSS))
				r = tfw_ws_msg_process(conn, skb);
			else
				r = tfw_http_msg_process(conn, skb, &splitted);
			if (r == T_DROP && splitted) {
				/*
				 * In the case when the current skb contains
				 * multiple requests, we split this skb along
				 * the request boundary. If the request was
				 * dropped we save skb with the next request
				 * in the `splitted` pointer.
				 */
				splitted->next = next;
				next = splitted;
			}
		} else {
			__kfree_skb(skb);
		}
	}

	/*
	 * T_BLOCK is error code for high level modules (like frang),
	 * here we should deal with error code, which accurately
	 * determine further closing behavior.
	 */
	BUG_ON(r == T_BLOCK);
	return r <= T_BAD || r == T_OK ? r : T_BAD;
}

void
tfw_connection_hooks_register(TfwConnHooks *hooks, int type)
{
	unsigned hid = TFW_CONN_TYPE2IDX(type);

	BUG_ON(hid >= TFW_CONN_MAX_PROTOS || conn_hooks[hid]);

	conn_hooks[hid] = hooks;
}

void
tfw_connection_hooks_unregister(int type)
{
	conn_hooks[TFW_CONN_TYPE2IDX(type)] = NULL;
}
