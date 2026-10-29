/* Bounded Mongo ping for package readiness. Uses the plugin's existing BSON
 * dependency, with no driver, interpreter or database process dependency. */
#include <bson/bson.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

enum { REQUEST_ID = 424242, OP_MSG = 2013, TIMEOUT_MS = 2000, MAX_REPLY = 65536 };

static void expired(int signal_number) {
  (void)signal_number;
  static const char message[] = "Mongo ping timed out after 2 seconds\n";
  /* A best-effort diagnostic: do not retry or use stdio in this signal handler.
   * Assign before discarding for glibc's fortified warn_unused_result write. */
  const ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
  (void)written;
  _exit(1);
}

static int64_t milliseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write_le32(uint8_t *p, uint32_t value) {
  for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static int wait_socket(int fd, short events, int64_t deadline) {
  for (;;) {
    int64_t now = milliseconds();
    if (now < 0 || now >= deadline) return 0;
    struct pollfd descriptor = {fd, events, 0};
    int ready = poll(&descriptor, 1, (int)(deadline - now));
    if (ready > 0) return !(descriptor.revents & POLLNVAL);
    if (ready == 0 || errno != EINTR) return 0;
  }
}

static SSL *tls = NULL;

static int transfer(int fd, uint8_t *data, size_t length, int writing, int64_t deadline) {
  while (length) {
    ssize_t count;
    if (tls) {
      count = writing ? SSL_write(tls, data, (int)length) : SSL_read(tls, data, (int)length);
      if (count <= 0) {
        const int error = SSL_get_error(tls, (int)count);
        if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            wait_socket(fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline)) continue;
        return 0;
      }
    } else {
      if (!wait_socket(fd, writing ? POLLOUT : POLLIN, deadline)) return 0;
      count = writing ? send(fd, data, length, 0) : recv(fd, data, length, 0);
    }
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0) return 0;
    data += count;
    length -= (size_t)count;
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: chimeradb-health HOST PORT\n");
    return 2;
  }
  char *end = NULL;
  long port = strtol(argv[2], &end, 10);
  if (!*argv[1] || !*argv[2] || *end || port < 1 || port > 65535) {
    fprintf(stderr, "invalid Mongo endpoint\n");
    return 2;
  }
  /* The alarm bounds DNS resolution as well as the monotonic socket deadline.
   * A TCP accept without a usable Mongo response is never considered healthy. */
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = expired;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGALRM, &action, NULL) != 0) return 1;
  signal(SIGPIPE, SIG_IGN);
  alarm(TIMEOUT_MS / 1000);
  int64_t now = milliseconds();
  if (now < 0) return 1;
  int64_t deadline = now + TIMEOUT_MS;

  struct addrinfo hints, *addresses = NULL;
  memset(&hints, 0, sizeof(hints));
  /* Explicit endpoints may be IPv6 forwarders for the IPv4 server listener. */
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  if (getaddrinfo(argv[1], argv[2], &hints, &addresses) != 0) {
    fprintf(stderr, "cannot resolve Mongo endpoint\n");
    return 1;
  }
  int fd = -1;
  size_t addresses_left = 0;
  for (struct addrinfo *address = addresses; address; address = address->ai_next) ++addresses_left;
  for (struct addrinfo *address = addresses; address; address = address->ai_next, --addresses_left) {
    fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) continue;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
      int64_t started = milliseconds();
      if (started < 0 || started >= deadline) { close(fd); fd = -1; break; }
      int64_t budget = (deadline - started) / (int64_t)addresses_left;
      if (budget < 1) budget = 1;
      int64_t attempt_deadline = started + budget;
      int connected = connect(fd, address->ai_addr, address->ai_addrlen);
      if (connected == 0) break;
      /* A blackholed first address must leave time to try the remaining DNS
       * results. A sole/final address receives the full remaining budget. The
       * original alarm/deadline still bound DNS plus every connect/write/read. */
      if (errno == EINPROGRESS && wait_socket(fd, POLLOUT, attempt_deadline)) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0) break;
      }
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);
  if (fd < 0) {
    fprintf(stderr, "cannot connect to Mongo endpoint\n");
    return 1;
  }

  SSL_CTX *tls_context = NULL;
  const char *ca = getenv("CHIMERA_MONGO_TLS_CA");
  const char *tls_mode = getenv("CHIMERA_MONGO_TLS");
  if ((ca && *ca) || (tls_mode && strcmp(tls_mode, "ON") == 0)) {
    tls_context = SSL_CTX_new(TLS_client_method());
    if (!tls_context) { close(fd); return 1; }
    SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_min_proto_version(tls_context, TLS1_2_VERSION) != 1 ||
        ((ca && *ca) ? SSL_CTX_load_verify_locations(tls_context, ca, NULL) :
                     SSL_CTX_set_default_verify_paths(tls_context)) != 1) {
      SSL_CTX_free(tls_context); close(fd); return 1;
    }
    tls = SSL_new(tls_context);
    const char *name = getenv("CHIMERA_MONGO_TLS_SERVER_NAME");
    if (!name || !*name) name = argv[1];
    unsigned char address_bytes[16];
    const int ip_name = inet_pton(AF_INET, name, address_bytes) == 1 ||
                        inet_pton(AF_INET6, name, address_bytes) == 1;
    if (!tls || SSL_set_fd(tls, fd) != 1 ||
        SSL_set_tlsext_host_name(tls, name) != 1 ||
        (ip_name ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(tls), name) :
                   SSL_set1_host(tls, name)) != 1) {
      SSL_free(tls); SSL_CTX_free(tls_context); close(fd); return 1;
    }
    for (;;) {
      const int connected = SSL_connect(tls);
      if (connected == 1) break;
      const int error = SSL_get_error(tls, connected);
      if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
          wait_socket(fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline)) continue;
      fprintf(stderr, "Mongo TLS handshake or certificate verification failed\n");
      SSL_free(tls); SSL_CTX_free(tls_context); close(fd); return 1;
    }
  }
  int result = 1;
  uint8_t *body = NULL;
  bson_t ping;
  bson_init(&ping);
  BSON_APPEND_INT32(&ping, "ping", 1);
  BSON_APPEND_UTF8(&ping, "$db", "admin");
  uint8_t request[21] = {0};
  write_le32(request, (uint32_t)sizeof(request) + ping.len);
  write_le32(request + 4, REQUEST_ID);
  write_le32(request + 12, OP_MSG);
  if (!transfer(fd, request, sizeof(request), 1, deadline) ||
      !transfer(fd, (uint8_t *)bson_get_data(&ping), ping.len, 1, deadline)) goto done;
  uint8_t header[16];
  if (!transfer(fd, header, sizeof(header), 0, deadline)) goto done;
  uint32_t length = read_le32(header);
  if (length < 26 || length > MAX_REPLY || read_le32(header + 8) != REQUEST_ID ||
      read_le32(header + 12) != OP_MSG) goto done;
  size_t body_length = length - sizeof(header);
  body = malloc(body_length);
  if (!body || !transfer(fd, body, body_length, 0, deadline)) goto done;
  /* Ping uses one kind-0 body and requests no checksums or exhaust replies. */
  if (read_le32(body) != 0 || body[4] != 0 || read_le32(body + 5) != body_length - 5) goto done;
  bson_t reply;
  if (!bson_init_static(&reply, body + 5, body_length - 5)) goto done;
  bson_iter_t field;
  if (bson_validate(&reply, BSON_VALIDATE_NONE, NULL) &&
      bson_iter_init_find(&field, &reply, "ok") && BSON_ITER_HOLDS_NUMBER(&field) &&
      bson_iter_as_double(&field) == 1.0) result = 0;
  bson_destroy(&reply);
done:
  alarm(0);
  if (result) fprintf(stderr, "Mongo endpoint did not return a valid successful ping\n");
  free(body);
  bson_destroy(&ping);
  SSL_free(tls);
  SSL_CTX_free(tls_context);
  close(fd);
  return result;
}
