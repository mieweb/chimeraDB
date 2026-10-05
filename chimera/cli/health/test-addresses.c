/* Compile the actual probe with controlled resolver/socket/clock syscalls.
 * A pending first address consumes exactly its poll budget; only the second
 * address can answer. No external route, firewall or wall-clock race is used. */
#define main health_probe_main
#define getaddrinfo fixture_getaddrinfo
#define freeaddrinfo fixture_freeaddrinfo
#define socket fixture_socket
#define connect fixture_connect
#define getsockopt fixture_getsockopt
#define fcntl fixture_fcntl
#define poll fixture_poll
#define send fixture_send
#define recv fixture_recv
#define close fixture_close
#define clock_gettime fixture_clock_gettime
#include "main.c"
#undef main

static int64_t fixture_time;
static int socket_count;
static int first_wait;
static int single_address;
static int connecting;
static size_t response_offset;
static uint8_t response[128];
static size_t response_length;
static struct sockaddr addresses[2];
static struct addrinfo resolved[2];

int fixture_getaddrinfo(const char *host, const char *service,
                        const struct addrinfo *hints, struct addrinfo **result) {
  if (strcmp(host, "multiple-addresses.invalid") || strcmp(service, "27017") ||
      hints->ai_family != AF_UNSPEC) abort();
  memset(resolved, 0, sizeof(resolved));
  for (int i = 0; i < 2; ++i) {
    resolved[i].ai_family = AF_INET;
    resolved[i].ai_socktype = SOCK_STREAM;
    resolved[i].ai_addr = &addresses[i];
    resolved[i].ai_addrlen = sizeof(addresses[i]);
  }
  if (!single_address) resolved[0].ai_next = &resolved[1];
  *result = resolved;
  fixture_time += 500; /* DNS time belongs to the same overall budget. */
  return 0;
}

void fixture_freeaddrinfo(struct addrinfo *address) { (void)address; }
int fixture_socket(int family, int type, int protocol) {
  (void)family; (void)type; (void)protocol;
  return ++socket_count;
}
int fixture_connect(int fd, const struct sockaddr *address, socklen_t size) {
  (void)address; (void)size;
  if (fd == 1) { connecting = 1; errno = EINPROGRESS; return -1; }
  if (fd != 2) abort();
  return 0;
}
int fixture_fcntl(int fd, int command, ...) { (void)fd; (void)command; return 0; }
int fixture_getsockopt(int fd, int level, int option, void *value, socklen_t *size) {
  (void)fd; (void)level; (void)option; (void)size;
  *(int *)value = 0;
  return 0;
}
int fixture_poll(struct pollfd *descriptors, nfds_t count, int timeout) {
  if (count != 1 || timeout <= 0) abort();
  if (connecting) {
    connecting = 0;
    first_wait = timeout;
    /* The sole address takes 400ms to connect, so an arbitrary 250ms cap would
     * reject it even though DNS, connect and ping fit the original 2s budget. */
    if (!single_address || timeout < 400) {
      fixture_time += timeout;
      return 0;
    }
    fixture_time += 400;
  }
  /* glibc's poll declaration marks this buffer write-only under fortification;
   * the controlled peer is ready for either operation, so no input is needed. */
  descriptors[0].revents = POLLIN | POLLOUT;
  return 1;
}
ssize_t fixture_send(int fd, const void *data, size_t size, int flags) {
  (void)data; (void)flags;
  if (fd != (single_address ? 1 : 2)) abort();
  return (ssize_t)size;
}
ssize_t fixture_recv(int fd, void *data, size_t size, int flags) {
  (void)flags;
  if (fd != (single_address ? 1 : 2) || response_offset + size > response_length) abort();
  memcpy(data, response + response_offset, size);
  response_offset += size;
  return (ssize_t)size;
}
int fixture_close(int fd) { (void)fd; return 0; }
int fixture_clock_gettime(clockid_t clock, struct timespec *now) {
  if (clock != CLOCK_MONOTONIC) abort();
  now->tv_sec = fixture_time / 1000;
  now->tv_nsec = (fixture_time % 1000) * 1000000;
  return 0;
}

int main(void) {
  bson_t reply;
  bson_init(&reply);
  BSON_APPEND_DOUBLE(&reply, "ok", 1.0);
  response_length = 21 + reply.len;
  write_le32(response, (uint32_t)response_length);
  write_le32(response + 8, REQUEST_ID);
  write_le32(response + 12, OP_MSG);
  memcpy(response + 21, bson_get_data(&reply), reply.len);
  bson_destroy(&reply);
  char *arguments[] = {"chimeradb-health", "multiple-addresses.invalid", "27017", NULL};
  int result = health_probe_main(3, arguments);
  if (result != 0 || socket_count != 2 || first_wait != 750 || response_offset != response_length ||
      fixture_time >= TIMEOUT_MS) {
    fprintf(stderr, "address fallback failed: result=%d sockets=%d first_wait=%d elapsed=%lld\n",
            result, socket_count, first_wait, (long long)fixture_time);
    return 1;
  }
  puts("PASS: pending first DNS address leaves time for the healthy second address");
  single_address = 1;
  fixture_time = 0;
  socket_count = 0;
  first_wait = 0;
  response_offset = 0;
  result = health_probe_main(3, arguments);
  if (result != 0 || socket_count != 1 || first_wait != 1500 ||
      response_offset != response_length || fixture_time != 900) {
    fprintf(stderr, "sole-address budget failed: result=%d sockets=%d first_wait=%d elapsed=%lld\n",
            result, socket_count, first_wait, (long long)fixture_time);
    return 1;
  }
  puts("PASS: sole DNS address retains the full budget after resolution");
  return 0;
}
