/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 wardenlym
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#include <sys/poll.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

#include <nghttp2/nghttp2.h>

#define APNS2_TEST_VERSION "0.1.1"

enum {
    IO_NONE,
    WANT_READ,
    WANT_WRITE
};

#define MAKE_NV(NAME, VALUE)                                                   \
  {                                                                            \
    (uint8_t *) NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, sizeof(VALUE) - 1,   \
        NGHTTP2_NV_FLAG_NONE                                                   \
  }

#define MAKE_NV_CS(NAME, VALUE)                                                \
  {                                                                            \
    (uint8_t *) NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, strlen(VALUE),       \
        NGHTTP2_NV_FLAG_NONE                                                   \
  }

struct connection_t {
    int fd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    nghttp2_session *session;
    int want_io;
};

struct opt_t {
  char* uri;
  uint16_t port;
  char *token;
  char *topic;
  char *cert;
  char *pkey;
  char *prefix;
  char *payload;
  char *message;
  char *path;
};

struct loop_t {
    int epfd;
};

static int g_debug_flag = 0;

#define debug  if(g_debug_flag) printf

static char*
alloc_string(const char* s);

static void
die(const char *msg)
{
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(EXIT_FAILURE);
}

static void
diec(const char *msg,int i)
{
    fprintf(stderr, "FATAL: %s %d\n", msg,i);
    exit(EXIT_FAILURE);
}

static bool
file_exsit(const char *f)
{
    return 0 == access(f, 0) ? true : (fprintf(stderr,"file not exsit: %s\n",f),false);
}

static char*
make_path(const char *prefix, const char *token)
{
    char *path = malloc(strlen(prefix)+strlen(token)+1);
    memset(path,0,strlen(prefix)+strlen(token)+1);
    strcat(path,prefix);
    strcat(path,token);
    return path;
}

static void
init_global_library()
{
    SSL_library_init();
    SSL_load_error_strings();
}

static int
connect_to_url(const char *url, uint16_t port)
{
    int sockfd;
    int rv;
    struct addrinfo hints, *res, *ressave;
    char port_str[6];

    bzero(&hints, sizeof(struct addrinfo));
    bzero(port_str, sizeof(port_str));
    snprintf(port_str, 6, "%d", port);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    debug("ns looking up ...\n");
    rv = getaddrinfo(url, port_str, &hints, &res);
    if (rv != 0) {
        freeaddrinfo(res);
        return -1;
    }

    ressave = res;
    do {
        sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if(sockfd < 0) {
            continue;
        }
        struct in_addr a = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
        const char *p = inet_ntoa(a);
        debug("connecting to : %s\n",p);
        while ((rv = connect(sockfd, res->ai_addr, res->ai_addrlen)) == -1 &&
                errno == EINTR)
            ;
        if (0 == rv) {
            freeaddrinfo(ressave);
            return sockfd;
        } else {
            close(sockfd);
        }
    } while ((res = res->ai_next) != NULL);

    freeaddrinfo(ressave);
    return -1;
}

static bool
socket_connect(const char *url, uint16_t port, struct connection_t *conn)
{
    int fd;
    fd = connect_to_url(url,port);
    if (fd > 0) {
        conn->fd = fd;
        debug("socket connect ok: fd=%d, host: %s:%d\n", conn->fd, url, port);
        return true;
    }
    die("socket connect fail.");
    return false;
}

static X509*
read_x509_certificate(const char* path)
{
    BIO  *bio = NULL;
    X509 *x509 = NULL;
    if (NULL == (bio = BIO_new_file(path, "r"))) {
        return NULL;
    }
    x509 = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return x509;
}

static char*
get_topic (const char* path)
{
  X509 *x509 = NULL;
  if (NULL == (x509 = read_x509_certificate(path))) {
      die("read_x509_certificate fail.");
  }

  const char* nb = (char*)X509_get_notBefore(x509)->data;
  const char* na = (char*)X509_get_notAfter(x509)->data;
  debug("notBefore : %s\nnotAfter  : %s\n", nb, na);

  X509_NAME *xn = NULL;
  ASN1_STRING *d = NULL;

  int cnt = 0, pos = -1;
  xn = X509_get_subject_name(x509);
  cnt = X509_NAME_entry_count(xn);

  pos = X509_NAME_get_index_by_NID(xn, NID_userId, -1);
  if (pos >=0 && pos <= cnt) {
      d = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(xn, pos));
      debug("%s = %s [%d]\n", SN_userId, d->data, d->length);
      return alloc_string((char*)d->data);
  }

  die("get topic not done ");
  return NULL;
}

/*
 * Callback function for TLS NPN. Since this program only supports
 * HTTP/2 protocol, if server does not offer HTTP/2 the nghttp2
 * library supports, we terminate program.
 */
static int
select_next_proto_cb(SSL *ssl, unsigned char **out,
                     unsigned char *outlen, const unsigned char *in,
                     unsigned int inlen, void *arg)
{
    int rv;
  /* nghttp2_select_next_protocol() selects HTTP/2 protocol the
     nghttp2 library supports. */
    rv = nghttp2_select_next_protocol(out, outlen, in, inlen);
    if (rv <= 0) {
        die("Server did not advertise HTTP/2 protocol");
    }
    return SSL_TLSEXT_ERR_OK;
}

static void
init_ssl_ctx(SSL_CTX *ssl_ctx)
{
  /* Disable SSLv2 and enable all workarounds for buggy servers */
    SSL_CTX_set_options(ssl_ctx, SSL_OP_ALL | SSL_OP_NO_SSLv2);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
  /* Set NPN callback */
    SSL_CTX_set_next_proto_select_cb(ssl_ctx, select_next_proto_cb, NULL);
}

static bool
ssl_allocate(struct connection_t *conn, const char *cert, const char *pkey)
{
    int rv;
    X509 *x509 = NULL;
    SSL_CTX *ssl_ctx = NULL;
    SSL *ssl = NULL;

    if (NULL == (x509 = read_x509_certificate(cert))) {
        return false;
    }

    ssl_ctx = SSL_CTX_new(SSLv23_client_method());
    if (ssl_ctx == NULL) {
        X509_free(x509);
    }
    init_ssl_ctx(ssl_ctx);

    rv = SSL_CTX_use_certificate(ssl_ctx, x509);
    X509_free(x509);
    if (rv != 1) {
        SSL_CTX_free(ssl_ctx);
        return false;
    }

    rv = SSL_CTX_use_PrivateKey_file(ssl_ctx, cert, SSL_FILETYPE_PEM);
    if (rv != 1) {
        SSL_CTX_free(ssl_ctx);
        return false;
    }

    rv = SSL_CTX_check_private_key(ssl_ctx);
    if (rv != 1) {
        SSL_CTX_free(ssl_ctx);
        return false;
    }

    ssl = SSL_new(ssl_ctx);
    if (ssl == NULL) {
        SSL_CTX_free(ssl_ctx);
        return false;
    }

    conn->ssl_ctx = ssl_ctx;
    conn->ssl = ssl;
    return true;
}

static bool
ssl_handshake(SSL *ssl, int fd)
{
    int rv;
    if (SSL_set_fd(ssl, fd) == 0) {
        return false;
    }
    ERR_clear_error();
    //rv = SSL_connect(ssl);
    SSL_set_connect_state(ssl);
    rv = SSL_do_handshake(ssl);

    if(rv==1) {
            debug("Connected with encryption: %s\n", SSL_get_cipher(ssl));
    }
    if (rv <= 0) {
	debug("rv = %d\n",rv);
	unsigned long ssl_err = SSL_get_error(ssl,rv);
	int geterror = ERR_peek_error();
	int reason = ERR_GET_REASON(geterror);
	debug("rv %d, ssl_error %lu, get_err %d, reason %d \n",rv, ssl_err, geterror ,reason);
	debug("errmsg: %s\n", ERR_error_string(ERR_get_error(), NULL));
	debug("errmsg msg: %s\n", ERR_reason_error_string(ERR_peek_error()));
	debug("Error: %s\n", ERR_reason_error_string(ERR_get_error()));
	    switch(reason)
	    {
	        case SSL_R_SSLV3_ALERT_CERTIFICATE_EXPIRED: /*,define in <openssl/ssl.h> "sslv3 alert certificate expired"},*/
	          reason = X509_V_ERR_CERT_HAS_EXPIRED;
	            printf("X509_V_ERR_CERT_HAS_EXPIRED\n");
	            break;
	        case SSL_R_SSLV3_ALERT_CERTIFICATE_REVOKED: /*,"sslv3 alert certificate revoked"},*/
	          reason = X509_V_ERR_CERT_REVOKED;
	            printf("X509_V_ERR_CERT_REVOKED\n");
	            break;
	    }

        fprintf(stderr, "%s\n", ERR_error_string(ERR_get_error(), NULL));
        return false;
    }
    return true;
}

static bool
ssl_connect(const char *cert, const char *pkey, struct connection_t *conn)
{
    if (ssl_allocate(conn, cert, pkey)) {
        debug("ssl allocation ok\n");
    } else {
        fprintf(stderr, "ssl allocation error\n");
        return false;
    }

    debug("ssl handshaking ...\n");
    if (ssl_handshake(conn->ssl, conn->fd)) {
	debug("ssl handshake ok\n");
    } else {
        fprintf(stderr, "ssl handshake error\n");
        return false;
    }

    return true;
}

// callback impelement
#define _U_
/*
 * The implementation of nghttp2_send_callback type. Here we write
 * |data| with size |length| to the network and return the number of
 * bytes actually written. See the documentation of
 * nghttp2_send_callback for the details.
 */
static ssize_t send_callback(nghttp2_session *session _U_, const uint8_t *data,
                             size_t length, int flags _U_, void *user_data) {

  int rv;
  struct connection_t *conn = user_data;
  conn->want_io = IO_NONE;
  ERR_clear_error();
  rv = SSL_write(conn->ssl, data, (int)length);
  if (rv <= 0) {
    int err = SSL_get_error(conn->ssl, rv);
    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
      conn->want_io =
          (err == SSL_ERROR_WANT_READ ? WANT_READ : WANT_WRITE);
      rv = NGHTTP2_ERR_WOULDBLOCK;
    } else {
      rv = NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  }
  return rv;
}

/*
 * The implementation of nghttp2_recv_callback type. Here we read data
 * from the network and write them in |buf|. The capacity of |buf| is
 * |length| bytes. Returns the number of bytes stored in |buf|. See
 * the documentation of nghttp2_recv_callback for the details.
 */
static ssize_t recv_callback(nghttp2_session *session _U_, uint8_t *buf,
                             size_t length, int flags _U_, void *user_data) {

  struct connection_t *conn;
  int rv;
  conn = (struct connection_t *)user_data;
  conn->want_io = IO_NONE;
  ERR_clear_error();
  rv = SSL_read(conn->ssl, buf, (int)length);
  if (rv < 0) {
    int err = SSL_get_error(conn->ssl, rv);
    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
      conn->want_io =
          (err == SSL_ERROR_WANT_READ ? WANT_READ : WANT_WRITE);
      rv = NGHTTP2_ERR_WOULDBLOCK;
    } else {
      rv = NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  } else if (rv == 0) {
    rv = NGHTTP2_ERR_EOF;
  }
  return rv;
}

static int on_frame_send_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame,
                                  void *user_data _U_) {
  size_t i;
  switch (frame->hd.type) {
  case NGHTTP2_HEADERS:
    if (nghttp2_session_get_stream_user_data(session, frame->hd.stream_id)) {
      const nghttp2_nv *nva = frame->headers.nva;
      debug("[INFO] C ----------------------------> S (HEADERS)\n");
      for (i = 0; i < frame->headers.nvlen; ++i) {
        fwrite(nva[i].name, nva[i].namelen, 1, stdout);
        printf(": ");
        fwrite(nva[i].value, nva[i].valuelen, 1, stdout);
        printf("\n");
      }
    }
    break;
  case NGHTTP2_RST_STREAM:
    debug("[INFO] C ----------------------------> S (RST_STREAM)\n");
    break;
  case NGHTTP2_GOAWAY:
    debug("[INFO] C ----------------------------> S (GOAWAY)\n");
    break;
  }
  return 0;
}

static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame,
                                  void *user_data _U_) {
  switch (frame->hd.type) {
  case NGHTTP2_HEADERS:
    if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
      struct connection_t *conn = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
      if (conn) {
	  debug("[INFO] C <---------------------------- S (HEADERS end)\n");
      }
    } else {
	debug("other header: %d",frame->headers.cat);
    }
    break;
  case NGHTTP2_RST_STREAM:
    debug("[INFO] C <---------------------------- S (RST_STREAM)\n");
    break;
  case NGHTTP2_GOAWAY:
    debug("[INFO] C <---------------------------- S (GOAWAY)\n");
    break;
  }
  return 0;
}

static int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame,
                              const uint8_t *name, size_t namelen,
                              const uint8_t *value, size_t valuelen,
                              uint8_t flags, void *user_data) {

  if (frame->hd.type == NGHTTP2_HEADERS) {
        fwrite(name, namelen, 1, stdout);
        printf(": ");
        fwrite(value, valuelen, 1, stdout);
        printf("\n");

  }
  return 0;
}

static int on_begin_headers_callback(nghttp2_session *session,
                                                 const nghttp2_frame *frame,
                                                 void *user_data) {
  printf("\n");
  debug("[INFO] C <---------------------------- S (HEADERS begin)\n");
  return 0;
}

/*
 * The implementation of nghttp2_on_stream_close_callback type. We use
 * this function to know the response is fully received. Since we just
 * fetch 1 resource in this program, after reception of the response,
 * we submit GOAWAY and close the session.
 */
static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                                    uint32_t error_code _U_,
                                    void *user_data _U_) {
  struct connection_t *conn = nghttp2_session_get_stream_user_data(session, stream_id);
  if (conn) {
    int rv;
    rv = nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);

    if (rv != 0) {
      diec("nghttp2_session_terminate_session", rv);
    }
  }
  return 0;
}

/*
 * The implementation of nghttp2_on_data_chunk_recv_callback type. We
 * use this function to print the received response body.
 */
static int on_data_chunk_recv_callback(nghttp2_session *session,
                                       uint8_t flags _U_, int32_t stream_id,
                                       const uint8_t *data, size_t len,
                                       void *user_data _U_) {
  debug("%s\n",__FUNCTION__);
  char buf[1024] = {0};
  memcpy(buf,data,len);
  buf[len]=0;
  printf("%s\n",buf);
  return 0;
}

/*
 * Setup callback functions. nghttp2 API offers many callback
 * functions, but most of them are optional. The send_callback is
 * always required. Since we use nghttp2_session_recv(), the
 * recv_callback is also required.
 */
static void
setup_nghttp2_callbacks(nghttp2_session_callbacks *callbacks)
{
  nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
  nghttp2_session_callbacks_set_recv_callback(callbacks, recv_callback);
  nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, on_frame_send_callback);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
  nghttp2_session_callbacks_set_on_header_callback(callbacks,on_header_callback);
  nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks,on_begin_headers_callback);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);

}

static bool
set_nghttp2_session_info(struct connection_t *conn)
{
    int rv;
    nghttp2_session_callbacks *callbacks;

    rv = nghttp2_session_callbacks_new(&callbacks);
    if (rv != 0) {
        fprintf(stderr, "nghttp2_session_callbacks_new");
    }
    setup_nghttp2_callbacks(callbacks);
    rv = nghttp2_session_client_new(&conn->session, callbacks, conn);
    if (rv != 0) {
        fprintf(stderr, "nghttp2_session_client_new");
    }
    nghttp2_session_callbacks_del(callbacks);

    rv = nghttp2_submit_settings(conn->session, NGHTTP2_FLAG_NONE, NULL, 0);
    if (rv != 0) {
	fprintf(stderr, "nghttp2_submit_settings %d",rv);
    }
    return true;
}

static int
set_nonblocking(int fd)
{
    int flags, rv;
    while ((flags = fcntl(fd, F_GETFL, 0)) == -1 && errno == EINTR)
        ;
    if (flags == -1) {
        return -1;
    }
    while ((rv = fcntl(fd, F_SETFL, flags | O_NONBLOCK)) == -1 && errno == EINTR)
        ;
    if (rv == -1) {
        return -1;
    }
    return 0;
}

static int
set_tcp_nodelay(int fd)
{
    int val = 1;
    if(-1 == setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, (socklen_t)sizeof(val))) {
        return -1;
    }
    return 0;
}

ssize_t data_prd_read_callback(
    nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,
    uint32_t *data_flags, nghttp2_data_source *source, void *user_data) {

  const char *d = source->ptr;
  size_t len = strlen(d);

  memcpy(buf,d,len);
  *data_flags |= NGHTTP2_DATA_FLAG_EOF;

  debug("[INFO] C ----------------------------> S (DATA post body)\n");
  char payload[4096];
  memcpy(payload,d,len);
  payload[len]=0;
  printf("%s\n",payload);
  return strlen(d);
}

static int32_t
submit_request(struct connection_t *conn, const struct opt_t* opt)
{
    int32_t stream_id;



    const nghttp2_nv nva[] = {
	      MAKE_NV(":method", "POST"),
	      MAKE_NV_CS(":path", opt->path),
	      MAKE_NV_CS("apns-topic", opt->topic)
	     // MAKE_NV("apns-id", "e77a3d12-bc9f-f410-a127-43f212597a9c")
    };

    nghttp2_data_provider data_prd;
    data_prd.source.ptr = (void*)opt->payload;
    data_prd.read_callback = data_prd_read_callback;

    stream_id = nghttp2_submit_request(conn->session, NULL, nva,
                                       sizeof(nva) / sizeof(nva[0]), &data_prd, conn);
    return stream_id;
}

static void exec_io(struct connection_t *connection) {
  int rv;
  rv = nghttp2_session_recv(connection->session);
  if (rv != 0) {
    diec("nghttp2_session_recv", rv);
  }
  rv = nghttp2_session_send(connection->session);
  if (rv != 0) {
    diec("nghttp2_session_send", rv);
  }
}

static void ctl_poll(struct pollfd *pollfd, struct connection_t *connection) {
  pollfd->events = 0;
  if (nghttp2_session_want_read(connection->session) ||
      connection->want_io == WANT_READ) {
    pollfd->events |= POLLIN;
  }
  if (nghttp2_session_want_write(connection->session) ||
      connection->want_io == WANT_WRITE) {
    pollfd->events |= POLLOUT;
  }
}

static void
event_loop(struct loop_t *loop, struct connection_t *conn)
{

  nfds_t npollfds = 1;
  struct pollfd pollfds[1];

  pollfds[0].fd = conn->fd;
  ctl_poll(pollfds, conn);

  while (nghttp2_session_want_read(conn->session) ||
         nghttp2_session_want_write(conn->session)) {
    int nfds = poll(pollfds, npollfds, -1);
    if (nfds == -1) {
      diec("poll", errno);
    }
    if (pollfds[0].revents & (POLLIN | POLLOUT)) {
      exec_io(conn);
    }
    if ((pollfds[0].revents & POLLHUP) || (pollfds[0].revents & POLLERR)) {
      die("Connection error");
    }
    ctl_poll(pollfds, conn);
  }
}

static bool
blocking_post(struct loop_t *loop, struct connection_t *conn, const struct opt_t *opt)
{
    set_nonblocking(conn->fd);
    set_tcp_nodelay(conn->fd);

    int32_t stream_id;
    stream_id = submit_request(conn, opt);
    if (stream_id < 0) {
	printf("stream id error: %d\n",stream_id);
	return false;
    }

    debug("[INFO] Stream ID = %d\n", stream_id);

    /* maybe running in a thread */
    event_loop(loop,conn);

    close(loop->epfd);
    loop->epfd = -1;
    debug("over.\n");
    return true;
}

static void
connection_cleanup(struct connection_t *conn)
{
  if (conn->session &&
      conn->ssl &&
      conn->ssl_ctx) {
    nghttp2_session_del(conn->session);
    SSL_shutdown(conn->ssl);
    SSL_free(conn->ssl);
    SSL_CTX_free(conn->ssl_ctx);
    shutdown(conn->fd, SHUT_WR);
    close(conn->fd);
  }
}

void
usage()
{
    printf("usage: apns2-test -cert -token [-dev] [-topic|-message|-payload|-uri|-port|-pkey|-prefix] [-debug]\n");
    printf("\nExample:\n./apns2-test -cert cert.pem -token aabbccdd33fa744403fb4447e0f3a054d43f433b80e48c5bcaa62b501fd0f956\n");
}

static bool
string_eq(const char* a, const char *b)
{
  return (0 == strcmp(a,b)) ? true : false;
}

static char*
alloc_string(const char* s)
{
  size_t n = strlen(s) +1;
  char *m = malloc(n);
  memcpy(m,s,n);
  return m;
}

static void
check_and_make_opt(int argc, const char *argv[], struct opt_t *opt)
{
  bzero(opt,sizeof(*opt));

  opt->uri      = alloc_string("api.push.apple.com");
  opt->port     = 2197;
  opt->token    = NULL;
  opt->topic    = NULL;
  opt->cert     = NULL;
  opt->pkey     = NULL;
  opt->prefix   = alloc_string("/3/device/");
  opt->message  = alloc_string("{\"aps\":{\"alert\":\"%s\",\"sound\":\"default\"}}");
  opt->payload  = alloc_string("{\"aps\":{\"alert\":\"apns2 test.\",\"sound\":\"default\"}}");

  int i=0;
  for (i=0;i<argc;i++) {
      debug("%d %s\n",i,argv[i]);
      const char *s = argv[i];
      const char *next_arg = argv[i+1];

      if (string_eq(s,"-debug")) {
	  g_debug_flag = 1;
      } else if (string_eq(s,"-dev")) {
	  opt->uri      = alloc_string("api.development.push.apple.com");
      } else if (string_eq(s,"-url")) {
	  opt->uri      = alloc_string(next_arg);
      } else if (string_eq(s,"-port")) {
	  opt->port = (uint16_t)atoi(next_arg);
      } else if (string_eq(s,"-token")) {
	  opt->token    = alloc_string(next_arg);
      } else if (string_eq(s,"-topic")) {
	  opt->topic    = alloc_string(next_arg);
      } else if (string_eq(s,"-cert")) {
	  opt->cert     = alloc_string(next_arg);
	  if (!file_exsit(opt->cert)) exit(0);
      } else if (string_eq(s,"-pkey")) {
	  opt->pkey     = alloc_string(next_arg);
      } else if (string_eq(s,"-prefix")) {
	  opt->prefix   = alloc_string(next_arg);
      } else if (string_eq(s,"-message")) {
	  char buf[4096] = {0};
	  snprintf(buf,4096,opt->message,next_arg);
	  opt->payload  = alloc_string(buf);
      }else if (string_eq(s,"-payload")) {
	  opt->payload  = alloc_string(next_arg);
      }
  }

  if (opt->cert == NULL ||
      opt->token == NULL) {
      usage();
      exit(0);
  }
  if (opt->topic == NULL) {
      opt->topic = get_topic(opt->cert);
  }
  opt->path = make_path(opt->prefix, opt->token);
  printf("\n");
}

int
main(int argc, const char *argv[])
{
    struct connection_t conn;
    struct loop_t loop;
    struct opt_t opt;

    check_and_make_opt(argc, argv, &opt);

    debug("apns2-test version: %s\n", APNS2_TEST_VERSION);
    debug("nghttp2 version: %s\n", NGHTTP2_VERSION);
    debug("tls/ssl version: %s\n", SSL_TXT_TLSV1_2);

    init_global_library();

    socket_connect(opt.uri, opt.port, &conn);
    if(!ssl_connect(opt.cert, opt.pkey, &conn))
      die("ssl connect fail.");
    set_nghttp2_session_info(&conn);

    blocking_post(&loop, &conn, &opt);

    connection_cleanup(&conn);

    return 0;
}
