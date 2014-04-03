/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010-2013 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */


#include "private-libwebsockets.h"

int lws_context_init_server(struct lws_context_creation_info *info,
			    struct libwebsocket_context *context)
{
	int n;
	int sockfd;
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	int opt = 1;
	struct libwebsocket *wsi;
#ifdef LWS_USE_IPV6
	struct sockaddr_in6 serv_addr6;
#endif
	struct sockaddr_in serv_addr4;
	struct sockaddr *v;

	/* set up our external listening socket we serve on */

	if (info->port == CONTEXT_PORT_NO_LISTEN)
		return 0;

#ifdef LWS_USE_IPV6
	if (LWS_IPV6_ENABLED(context))
		sockfd = socket(AF_INET6, SOCK_STREAM, 0);
	else
#endif
		sockfd = socket(AF_INET, SOCK_STREAM, 0);

	if (sockfd < 0) {
		lwsl_err("ERROR opening socket\n");
		return 1;
	}

	/*
	 * allow us to restart even if old sockets in TIME_WAIT
	 */
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
				      (const void *)&opt, sizeof(opt));

	lws_plat_set_socket_options(context, sockfd);

#ifdef LWS_USE_IPV6
	if (LWS_IPV6_ENABLED(context)) {
		v = (struct sockaddr *)&serv_addr6;
		n = sizeof(struct sockaddr_in6);
		bzero((char *) &serv_addr6, sizeof(serv_addr6));
		serv_addr6.sin6_addr = in6addr_any;
		serv_addr6.sin6_family = AF_INET6;
		serv_addr6.sin6_port = htons(info->port);
	} else
#endif
	{
		v = (struct sockaddr *)&serv_addr4;
		n = sizeof(serv_addr4);
		bzero((char *) &serv_addr4, sizeof(serv_addr4));
		serv_addr4.sin_addr.s_addr = INADDR_ANY;
		serv_addr4.sin_family = AF_INET;
		serv_addr4.sin_port = htons(info->port);

		if (info->iface) {
			if (interface_to_sa(context, info->iface,
				   (struct sockaddr_in *)v, n) < 0) {
				lwsl_err("Unable to find interface %s\n",
							info->iface);
				compatible_close(sockfd);
				return 1;
			}
		}
	} /* ipv4 */

	n = bind(sockfd, v, n);
	if (n < 0) {
		lwsl_err("ERROR on binding to port %d (%d %d)\n",
					      info->port, n, LWS_ERRNO);
		compatible_close(sockfd);
		return 1;
	}
	
	if (getsockname(sockfd, (struct sockaddr *)&sin, &len) == -1)
		lwsl_warn("getsockname: %s\n", strerror(LWS_ERRNO));
	else
		info->port = ntohs(sin.sin_port);

	context->listen_port = info->port;

	wsi = (struct libwebsocket *)malloc(sizeof(struct libwebsocket));
	if (wsi == NULL) {
		lwsl_err("Out of mem\n");
		compatible_close(sockfd);
		return 1;
	}
	memset(wsi, 0, sizeof(struct libwebsocket));
	wsi->sock = sockfd;
	wsi->mode = LWS_CONNMODE_SERVER_LISTENER;

	insert_wsi_socket_into_fds(context, wsi);

	context->listen_service_modulo = LWS_LISTEN_SERVICE_MODULO;
	context->listen_service_count = 0;
	context->listen_service_fd = sockfd;

	listen(sockfd, LWS_SOMAXCONN);
	lwsl_notice(" Listening on port %d\n", info->port);
	
	return 0;
}

#ifdef LWS_OPENSSL_SUPPORT

static void
libwebsockets_decode_ssl_error(void)
{
	char buf[256];
	u_long err;

	while ((err = ERR_get_error()) != 0) {
		ERR_error_string_n(err, buf, sizeof(buf));
		lwsl_err("*** %lu %s\n", err, buf);
	}
}
#endif

struct libwebsocket *
libwebsocket_create_new_server_wsi(struct libwebsocket_context *context)
{
	struct libwebsocket *new_wsi;

	new_wsi = (struct libwebsocket *)malloc(sizeof(struct libwebsocket));
	if (new_wsi == NULL) {
		lwsl_err("Out of memory for new connection\n");
		return NULL;
	}

	memset(new_wsi, 0, sizeof(struct libwebsocket));
	new_wsi->pending_timeout = NO_PENDING_TIMEOUT;

	/* intialize the instance struct */

	new_wsi->state = WSI_STATE_HTTP;
	new_wsi->mode = LWS_CONNMODE_HTTP_SERVING;
	new_wsi->hdr_parsing_completed = 0;

	if (lws_allocate_header_table(new_wsi)) {
		free(new_wsi);
		return NULL;
	}

	/*
	 * these can only be set once the protocol is known
	 * we set an unestablished connection's protocol pointer
	 * to the start of the supported list, so it can look
	 * for matching ones during the handshake
	 */
	new_wsi->protocol = context->protocols;
	new_wsi->user_space = NULL;
	new_wsi->ietf_spec_revision = 0;

	/*
	 * outermost create notification for wsi
	 * no user_space because no protocol selection
	 */
	context->protocols[0].callback(context, new_wsi,
			LWS_CALLBACK_WSI_CREATE, NULL, NULL, 0);

	return new_wsi;
}

int lws_server_socket_service(struct libwebsocket_context *context,
			struct libwebsocket *wsi, struct libwebsocket_pollfd *pollfd)
{
	struct libwebsocket *new_wsi;
	int accept_fd;
	socklen_t clilen;
	struct sockaddr_in cli_addr;
	int n;
	int len;
#ifdef LWS_OPENSSL_SUPPORT
	int m;
#ifndef USE_CYASSL
	BIO *bio;
#endif
#endif

	switch (wsi->mode) {

	case LWS_CONNMODE_HTTP_SERVING:
	case LWS_CONNMODE_HTTP_SERVING_ACCEPTED:

		/* handle http headers coming in */

		/* pending truncated sends have uber priority */

		if (wsi->truncated_send_malloc) {
			if (pollfd->revents & LWS_POLLOUT)
				lws_issue_raw(wsi, wsi->truncated_send_malloc +
					wsi->truncated_send_offset,
							wsi->truncated_send_len);
			/*
			 * we can't afford to allow input processing send
			 * something new, so spin around he event loop until
			 * he doesn't have any partials
			 */
			break;
		}

		/* any incoming data ready? */

		if (pollfd->revents & LWS_POLLIN) {

	#ifdef LWS_OPENSSL_SUPPORT
			if (wsi->ssl)
				len = SSL_read(wsi->ssl,
					context->service_buffer,
					       sizeof(context->service_buffer));
			else
	#endif
				len = recv(pollfd->fd,
					context->service_buffer,
					sizeof(context->service_buffer), 0);

			if (len < 0) {
				lwsl_debug("Socket read returned %d\n", len);
				if (LWS_ERRNO != LWS_EINTR && LWS_ERRNO != LWS_EAGAIN)
					libwebsocket_close_and_free_session(
						context, wsi,
						LWS_CLOSE_STATUS_NOSTATUS);
				return 0;
			}
			if (!len) {
				lwsl_info("lws_server_skt_srv: read 0 len\n");
				/* lwsl_info("   state=%d\n", wsi->state); */
				if (!wsi->hdr_parsing_completed)
					free(wsi->u.hdr.ah);
				libwebsocket_close_and_free_session(
				       context, wsi, LWS_CLOSE_STATUS_NOSTATUS);
				return 0;
			}

			/* hm this may want to send (via HTTP callback for example) */

			n = libwebsocket_read(context, wsi,
						context->service_buffer, len);
			if (n < 0)
				/* we closed wsi */
				return 0;

			/* hum he may have used up the writability above */
			break;
		}

		/* this handles POLLOUT for http serving fragments */

		if (!(pollfd->revents & LWS_POLLOUT))
			break;

		/* one shot */
		if (lws_change_pollfd(wsi, LWS_POLLOUT, 0))
			goto fail;
#ifdef LWS_USE_LIBEV
		if (LWS_LIBEV_ENABLED(context))
			ev_io_stop(context->io_loop,
				   (struct ev_io *)&wsi->w_write);
#endif /* LWS_USE_LIBEV */

		if (wsi->state != WSI_STATE_HTTP_ISSUING_FILE) {
			n = user_callback_handle_rxflow(
					wsi->protocol->callback,
					wsi->protocol->owning_server,
					wsi, LWS_CALLBACK_HTTP_WRITEABLE,
					wsi->user_space,
					NULL,
					0);
			if (n < 0)
				libwebsocket_close_and_free_session(
				       context, wsi, LWS_CLOSE_STATUS_NOSTATUS);
			break;
		}

		/* nonzero for completion or error */
		if (libwebsockets_serve_http_file_fragment(context, wsi))
			libwebsocket_close_and_free_session(context, wsi,
					       LWS_CLOSE_STATUS_NOSTATUS);
		break;

	case LWS_CONNMODE_SERVER_LISTENER:

		/* pollin means a client has connected to us then */

		if (!(pollfd->revents & LWS_POLLIN))
			break;

		/* listen socket got an unencrypted connection... */

		clilen = sizeof(cli_addr);
		lws_latency_pre(context, wsi);
		accept_fd  = accept(pollfd->fd, (struct sockaddr *)&cli_addr,
								       &clilen);
		lws_latency(context, wsi,
			"unencrypted accept LWS_CONNMODE_SERVER_LISTENER",
						     accept_fd, accept_fd >= 0);
		if (accept_fd < 0) {
			if (LWS_ERRNO == LWS_EAGAIN || LWS_ERRNO == LWS_EWOULDBLOCK) {
				lwsl_debug("accept asks to try again\n");
				break;
			}
			lwsl_warn("ERROR on accept: %s\n", strerror(LWS_ERRNO));
			break;
		}

		lws_plat_set_socket_options(context, accept_fd);

		/*
		 * look at who we connected to and give user code a chance
		 * to reject based on client IP.  There's no protocol selected
		 * yet so we issue this to protocols[0]
		 */

		if ((context->protocols[0].callback)(context, wsi,
				LWS_CALLBACK_FILTER_NETWORK_CONNECTION,
					   NULL, (void *)(long)accept_fd, 0)) {
			lwsl_debug("Callback denied network connection\n");
			compatible_close(accept_fd);
			break;
		}

		new_wsi = libwebsocket_create_new_server_wsi(context);
		if (new_wsi == NULL) {
			compatible_close(accept_fd);
			break;
		}

		new_wsi->sock = accept_fd;

		/* the transport is accepted... give him time to negotiate */
		libwebsocket_set_timeout(new_wsi,
			PENDING_TIMEOUT_ESTABLISH_WITH_SERVER,
							AWAITING_TIMEOUT);

		/*
		 * A new connection was accepted. Give the user a chance to
		 * set properties of the newly created wsi. There's no protocol
		 * selected yet so we issue this to protocols[0]
		 */

		(context->protocols[0].callback)(context, new_wsi,
			LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED, NULL, NULL, 0);

#ifdef LWS_USE_LIBEV
		if (LWS_LIBEV_ENABLED(context)) {
		        new_wsi->w_read.context = context;
		        new_wsi->w_write.context = context;
		        /*
		        new_wsi->w_read.wsi = new_wsi;
		        new_wsi->w_write.wsi = new_wsi;
		        */
		        struct ev_io* w_read = (struct ev_io*)&(new_wsi->w_read);
		        struct ev_io* w_write = (struct ev_io*)&(new_wsi->w_write);
		        ev_io_init(w_read,libwebsocket_accept_cb,accept_fd,EV_READ);
		        ev_io_init(w_write,libwebsocket_accept_cb,accept_fd,EV_WRITE);
		}
#endif /* LWS_USE_LIBEV */

#ifdef LWS_OPENSSL_SUPPORT
		new_wsi->ssl = NULL;
		if (!context->use_ssl) {
#endif

			lwsl_debug("accepted new conn  port %u on fd=%d\n",
					  ntohs(cli_addr.sin_port), accept_fd);

			insert_wsi_socket_into_fds(context, new_wsi);
			break;
#ifdef LWS_OPENSSL_SUPPORT
		}

		new_wsi->ssl = SSL_new(context->ssl_ctx);
		if (new_wsi->ssl == NULL) {
			lwsl_err("SSL_new failed: %s\n",
			    ERR_error_string(SSL_get_error(
			    new_wsi->ssl, 0), NULL));
			    libwebsockets_decode_ssl_error();
			free(new_wsi);
			compatible_close(accept_fd);
			break;
		}

		SSL_set_ex_data(new_wsi->ssl,
			openssl_websocket_private_data_index, context);

		SSL_set_fd(new_wsi->ssl, accept_fd);
#ifndef USE_CYASSL
		SSL_set_mode(new_wsi->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#endif
		#ifdef USE_CYASSL
		CyaSSL_set_using_nonblock(new_wsi->ssl, 1);
		#else
		bio = SSL_get_rbio(new_wsi->ssl);
		if (bio)
			BIO_set_nbio(bio, 1); /* nonblocking */
		else
			lwsl_notice("NULL rbio\n");
		bio = SSL_get_wbio(new_wsi->ssl);
		if (bio)
			BIO_set_nbio(bio, 1); /* nonblocking */
		else
			lwsl_notice("NULL rbio\n");
		#endif

		/*
		 * we are not accepted yet, but we need to enter ourselves
		 * as a live connection.  That way we can retry when more
		 * pieces come if we're not sorted yet
		 */

		wsi = new_wsi;
		wsi->mode = LWS_CONNMODE_SSL_ACK_PENDING;
		insert_wsi_socket_into_fds(context, wsi);

		libwebsocket_set_timeout(wsi, PENDING_TIMEOUT_SSL_ACCEPT,
							AWAITING_TIMEOUT);

		lwsl_info("inserted SSL accept into fds, trying SSL_accept\n");

		/* fallthru */

	case LWS_CONNMODE_SSL_ACK_PENDING:

		if (lws_change_pollfd(wsi, LWS_POLLOUT, 0))
			goto fail;
#ifdef LWS_USE_LIBEV
		if (LWS_LIBEV_ENABLED(context))
			ev_io_stop(context->io_loop,
					   (struct ev_io *)&wsi->w_write);
#endif /* LWS_USE_LIBEV */

		lws_latency_pre(context, wsi);

		n = recv(wsi->sock, context->service_buffer,
			sizeof(context->service_buffer), MSG_PEEK);

		/*
		 * optionally allow non-SSL connect on SSL listening socket
		 * This is disabled by default, if enabled it goes around any
		 * SSL-level access control (eg, client-side certs) so leave
		 * it disabled unless you know it's not a problem for you
		 */

		if (context->allow_non_ssl_on_ssl_port && n >= 1 &&
					context->service_buffer[0] >= ' ') {
			/*
			 * TLS content-type for Handshake is 0x16
			 * TLS content-type for ChangeCipherSpec Record is 0x14
			 *
			 * A non-ssl session will start with the HTTP method in
			 * ASCII.  If we see it's not a legit SSL handshake
			 * kill the SSL for this connection and try to handle
			 * as a HTTP connection upgrade directly.
			 */
			wsi->use_ssl = 0;
			SSL_shutdown(wsi->ssl);
			SSL_free(wsi->ssl);
			wsi->ssl = NULL;
			goto accepted;
		}

		/* normal SSL connection processing path */

		n = SSL_accept(wsi->ssl);
		lws_latency(context, wsi,
			"SSL_accept LWS_CONNMODE_SSL_ACK_PENDING\n", n, n == 1);

		if (n != 1) {
			m = SSL_get_error(wsi->ssl, n);
			lwsl_debug("SSL_accept failed %d / %s\n",
						  m, ERR_error_string(m, NULL));

			if (m == SSL_ERROR_WANT_READ) {
				if (lws_change_pollfd(wsi, 0, LWS_POLLIN))
					goto fail;
#ifdef LWS_USE_LIBEV
				if (LWS_LIBEV_ENABLED(context))
		                    ev_io_start(context->io_loop,
						(struct ev_io *)&wsi->w_read);
#endif /* LWS_USE_LIBEV */
				lwsl_info("SSL_ERROR_WANT_READ\n");
				break;
			}
			if (m == SSL_ERROR_WANT_WRITE) {
				if (lws_change_pollfd(wsi, 0, LWS_POLLOUT))
					goto fail;
#ifdef LWS_USE_LIBEV
				if (LWS_LIBEV_ENABLED(context))
					ev_io_start(context->io_loop,
						(struct ev_io *)&wsi->w_write);
#endif /* LWS_USE_LIBEV */
				break;
			}
			lwsl_debug("SSL_accept failed skt %u: %s\n",
				  pollfd->fd,
				  ERR_error_string(m, NULL));
			libwebsocket_close_and_free_session(context, wsi,
						 LWS_CLOSE_STATUS_NOSTATUS);
			break;
		}

accepted:
		/* OK, we are accepted... give him some time to negotiate */
		libwebsocket_set_timeout(wsi,
			PENDING_TIMEOUT_ESTABLISH_WITH_SERVER,
							AWAITING_TIMEOUT);

		wsi->mode = LWS_CONNMODE_HTTP_SERVING;

		lwsl_debug("accepted new SSL conn\n");
		break;
#endif

	default:
		break;
	}
	return 0;
	
fail:
	libwebsocket_close_and_free_session(context, wsi,
						 LWS_CLOSE_STATUS_NOSTATUS);
	return 1;
}


static const char *err400[] = {
	"Bad Request",
	"Unauthorized",
	"Payment Required",
	"Forbidden",
	"Not Found",
	"Method Not Allowed",
	"Not Acceptable",
	"Proxy Auth Required",
	"Request Timeout",
	"Conflict",
	"Gone",
	"Length Required",
	"Precondition Failed",
	"Request Entity Too Large",
	"Request URI too Long",
	"Unsupported Media Type",
	"Requested Range Not Satisfiable",
	"Expectation Failed"
};

static const char *err500[] = {
	"Internal Server Error",
	"Not Implemented",
	"Bad Gateway",
	"Service Unavailable",
	"Gateway Timeout",
	"HTTP Version Not Supported"
};

/**
 * libwebsockets_return_http_status() - Return simple http status
 * @context:		libwebsockets context
 * @wsi:		Websocket instance (available from user callback)
 * @code:		Status index, eg, 404
 * @html_body:		User-readable HTML description, or NULL
 *
 *	Helper to report HTTP errors back to the client cleanly and
 *	consistently
 */
LWS_VISIBLE int libwebsockets_return_http_status(
		struct libwebsocket_context *context, struct libwebsocket *wsi,
				       unsigned int code, const char *html_body)
{
	int n, m;
	const char *description = "";

	if (!html_body)
		html_body = "";

	if (code >= 400 && code < (400 + ARRAY_SIZE(err400)))
		description = err400[code - 400];
	if (code >= 500 && code < (500 + ARRAY_SIZE(err500)))
		description = err500[code - 500];

	n = sprintf((char *)context->service_buffer,
		"HTTP/1.0 %u %s\x0d\x0a"
		"Server: libwebsockets\x0d\x0a"
		"Content-Type: text/html\x0d\x0a\x0d\x0a"
		"<h1>%u %s</h1>%s",
		code, description, code, description, html_body);

	lwsl_info((const char *)context->service_buffer);

	m = libwebsocket_write(wsi, context->service_buffer, n, LWS_WRITE_HTTP);

	return m;
}

/**
 * libwebsockets_serve_http_file() - Send a file back to the client using http
 * @context:		libwebsockets context
 * @wsi:		Websocket instance (available from user callback)
 * @file:		The file to issue over http
 * @content_type:	The http content type, eg, text/html
 * @other_headers:	NULL or pointer to \0-terminated other header string
 *
 *	This function is intended to be called from the callback in response
 *	to http requests from the client.  It allows the callback to issue
 *	local files down the http link in a single step.
 *
 *	Returning <0 indicates error and the wsi should be closed.  Returning
 *	>0 indicates the file was completely sent and the wsi should be closed.
 *	==0 indicates the file transfer is started and needs more service later,
 *	the wsi should be left alone.
 */

LWS_VISIBLE int libwebsockets_serve_http_file(
		struct libwebsocket_context *context,
			struct libwebsocket *wsi, const char *file,
			   const char *content_type, const char *other_headers)
{
	unsigned char *p = context->service_buffer;
	int ret = 0;
	int n;

	wsi->u.http.fd = lws_plat_open_file(file, &wsi->u.http.filelen);

	if (wsi->u.http.fd == LWS_INVALID_FILE) {
		lwsl_err("Unable to open '%s'\n", file);
		libwebsockets_return_http_status(context, wsi,
						HTTP_STATUS_NOT_FOUND, NULL);
		return -1;
	}

	p += sprintf((char *)p,
"HTTP/1.0 200 OK\x0d\x0aServer: libwebsockets\x0d\x0a""Content-Type: %s\x0d\x0a",
								  content_type);
	if (other_headers) {
		n = strlen(other_headers);
		memcpy(p, other_headers, n);
		p += n;
	}
	p += sprintf((char *)p,
		"Content-Length: %lu\x0d\x0a\x0d\x0a", wsi->u.http.filelen);

	ret = libwebsocket_write(wsi, context->service_buffer,
				   p - context->service_buffer, LWS_WRITE_HTTP);
	if (ret != (p - context->service_buffer)) {
		lwsl_err("_write returned %d from %d\n", ret, (p - context->service_buffer));
		return -1;
	}

	wsi->u.http.filepos = 0;
	wsi->state = WSI_STATE_HTTP_ISSUING_FILE;

	return libwebsockets_serve_http_file_fragment(context, wsi);
}

