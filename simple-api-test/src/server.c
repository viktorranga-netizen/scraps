#include <assert.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// Bstring: growable string declarations

struct Bstring {
  char *data;
  size_t length;
  size_t capacity;
};

#define BSTRING_INIT_CAPACITY 16

struct Bstring *bstring_init(size_t capacity, const char *const s);
bool bstring_append(struct Bstring *self, const char *const s);
void bstring_free(struct Bstring *self);

// HTTP server related declarations

enum HttpMethodTyp {
  HTTP_UNKNOWN = 0,
  HTTP_GET,
  HTTP_POST,
  HTTP_PUT,
  HTTP_PATCH,
  HTTP_DELETE,
};

struct HttpMethod {
  const char *const str;
  enum HttpMethodTyp typ;
};

const char *const STR_HTTP_GET = "GET";
const char *const STR_HTTP_POST = "POST";
const char *const STR_HTTP_PUT = "PUT";
const char *const STR_HTTP_PATCH = "PATCH";
const char *const STR_HTTP_DELETE = "DELETE";

struct HttpMethod KNOWN_HTTP_METHODS[] = {
    {.str = STR_HTTP_GET, .typ = HTTP_GET},
    {.str = STR_HTTP_POST, .typ = HTTP_POST},
    {.str = STR_HTTP_PUT, .typ = HTTP_PUT},
    {.str = STR_HTTP_PATCH, .typ = HTTP_PATCH},
    {.str = STR_HTTP_DELETE, .typ = HTTP_DELETE},
};

size_t KNOWN_HTTP_METHODS_LEN = sizeof(KNOWN_HTTP_METHODS) / sizeof(KNOWN_HTTP_METHODS[0]);

#define BUFFER_SIZE 1024
#define MAX_HEADERS 128
#define MAX_HEADER_BYTES (8 * 1024)
#define RECEIVE_TIMEOUT_SECONDS 5

struct HttpHeader {
  char *key;
  char *value;
};

struct HttpRequest {
  enum HttpMethodTyp method;
  char *path;

  // headers
  struct HttpHeader *headers;
  size_t headers_len;

  // the request buffer
  char *_buffer;
  size_t _buffer_len;

  struct Bstring *body;
};

void handle_connection(int conn_fd);

int server_fd = -1;
struct sockaddr_in client_addr;
socklen_t client_addr_len = sizeof(client_addr);

// ESBO mock: serve this file's bytes verbatim (already brotli-compressed) for
// GET /api/v1/esbo/Websites/all, so esbo_import.pl's `brotli -d -c` decode works unmodified against a local fixture instead of a real API call.
// Not checked into the repo (see src/resources/README.md) place a captured response there yourself before running this against esbo_import.pl.
// Resolved at startup relative to the binary's own location (via argv[0]), not the process's working directory, so it works no matter where `bin/server` is invoked from.
char esbo_fixture_path[PATH_MAX];

int main(int argc, char **argv) {
  // Disable output buffering
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  char exe_path[PATH_MAX];
  if (realpath(argv[0], exe_path) == NULL) {
    printf("error: realpath(argv[0]) failed: %s\n", strerror(errno));
    return 1;
  }
  // dirname() gives .../bin, and the fixture lives one level up, under src/resources/.
  char *bin_dir = dirname(exe_path);
  snprintf(esbo_fixture_path, sizeof(esbo_fixture_path),
           "%s/../src/resources/esbo_websites_all.br", bin_dir);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    printf("error: Socket creation failed: %s...\n", strerror(errno));
    goto cleanup;
  }

  // Lose the "Address already in use" error message. Even after the server is closed, the port will still be hanging around for a while, and restarting results in an "Address already in use" error message
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    printf("error: SO_REUSEADDR failed: %s \n", strerror(errno));
    goto cleanup;
  }

  struct sockaddr_in serv_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(4221),
      .sin_addr = {htonl(INADDR_ANY)},
  };

  if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
    printf("error: Bind failed: %s \n", strerror(errno));
    goto cleanup;
  }

  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    printf("error: Listen failed: %s \n", strerror(errno));
    goto cleanup;
  }

  printf("Waiting for a client to connect...\n");

  while (true) {
    int conn_fd =
        accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
    if (conn_fd == -1) {
      printf("error: Accept failed: %s \n", strerror(errno));
      continue;
    }

    handle_connection(conn_fd);
  }

cleanup:
  if (server_fd != -1) {
    close(server_fd);
  }

  return 0;
}

char *http_get_header(struct HttpRequest *self, char *header) {
  assert(self != NULL);
  assert(header != NULL);

  for (size_t i = 0; i < self->headers_len; ++i) {
    if (strcasecmp(header, self->headers[i].key) == 0) {
      return self->headers[i].value;
    }
  }

  return NULL;
}

void *_handle_connection(void *conn_fd_ptr) {
  assert(conn_fd_ptr != NULL);

  int conn_fd = *(int *)conn_fd_ptr;

  printf("processing connection with fd %d\n", conn_fd);

  // read data from the client
  struct HttpRequest req = {0};
  req._buffer = malloc(MAX_HEADER_BYTES + 1);

  FILE *fp = NULL;

  if (req._buffer == NULL) {
    printf("error: Failed to allocate memory\n");
    goto cleanup;
  }

  // Bound idle time while waiting for the rest of a request's headers.
  struct timeval receive_timeout = {.tv_sec = RECEIVE_TIMEOUT_SECONDS};
  if (setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                 sizeof(receive_timeout)) == -1) {
    printf("error: SO_RCVTIMEO failed: %s\n", strerror(errno));
    goto cleanup;
  }

  req._buffer[0] = '\0';
  while (strstr(req._buffer, "\r\n\r\n") == NULL) {
    if (req._buffer_len == MAX_HEADER_BYTES) {
      printf("error: Request headers exceed %d bytes\n", MAX_HEADER_BYTES);
      goto cleanup;
    }

    ssize_t bytes_read = recv(conn_fd, req._buffer + req._buffer_len,
                              MAX_HEADER_BYTES - req._buffer_len, 0);
    if (bytes_read == -1) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        printf("error: Timed out waiting for request headers\n");
      } else {
        printf("error: Recv failed: %s\n", strerror(errno));
      }
      goto cleanup;
    }
    if (bytes_read == 0) {
      printf("error: Connection closed before request headers were complete\n");
      goto cleanup;
    }

    req._buffer_len += (size_t)bytes_read;
    req._buffer[req._buffer_len] = '\0';
  }

  char *parse_buffer = req._buffer;

  // parse method & store it to req
  char *method = strsep(&parse_buffer, " ");
  for (size_t i = 0; i < KNOWN_HTTP_METHODS_LEN; ++i) {
    if (strcmp(method, KNOWN_HTTP_METHODS[i].str) == 0) {
      req.method = KNOWN_HTTP_METHODS[i].typ;

      break;
    }
  }

  // unknowm method
  if (req.method == HTTP_UNKNOWN) {
    printf("error(parse): unknown method %s\n", method);
    goto cleanup;
  }

  // parse path & store it to req
  req.path = strsep(&parse_buffer, " ");

  // parse http version
  strsep(&parse_buffer, "\r");
  // char *http_version = strsep(&parse_buffer, "\r");

  // the \r was consumed but the \n is still remaining so consume it
  parse_buffer++;

  // parse headers & store it to req
  req.headers = malloc(sizeof(struct HttpHeader) * MAX_HEADERS);
  req.headers_len = 0;

  for (size_t i = 0; i < MAX_HEADERS; ++i) {
    char *header_line = strsep(&parse_buffer, "\r");

    parse_buffer++; // consume \n

    // if the line was just "\r\n" i.e the end of headers section
    if (header_line[0] == '\0') {
      break;
    }

    char *key = strsep(&header_line, ":");
    header_line++; // consume the space
    char *value = strsep(&header_line, "\0");

    req.headers[i].key = key;
    req.headers[i].value = value;

    ++req.headers_len;
  }

  // get content length and parse request body
  size_t content_length = 0;
  char *content_length_header = http_get_header(&req, "content-length");
  if (content_length_header != NULL) {
    errno = 0;
    size_t result = strtoul(content_length_header, NULL, 10);
    // store result to content_length if there was no error
    if (errno == 0) {
      content_length = result;
    }
  }

  if (content_length > 0) {
    req.body = bstring_init(content_length + 1, NULL);
    bstring_append(req.body, parse_buffer);

    // content lenth mismatch
    if (req.body->length != content_length) {
    }
  }

  // send response
  int bytes_sent = -1;
  // add more API paths here as desired
  if (req.method == HTTP_GET && strcmp(req.path, "/api/v1/esbo/Websites/all") == 0) {
    errno = 0;
    fp = fopen(esbo_fixture_path, "r");
    if (fp == NULL) {
      perror("error: fopen(esbo_fixture_path)");
      const char res[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
      bytes_sent = send(conn_fd, res, sizeof(res) - 1, 0);
      goto cleanup;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
      perror("error: fseek()");
      goto cleanup;
    }

    long file_size = ftell(fp);
    if (file_size < 0) {
      perror("error: ftell()");
      goto cleanup;
    }

    rewind(fp);

    char header[BUFFER_SIZE];
    int header_len =
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/octet-stream\r\n"
                 "Content-Length: %ld\r\n\r\n",
                 file_size);
    bytes_sent = send(conn_fd, header, (size_t)header_len, 0);

    char buffer[BUFFER_SIZE];
    while (file_size > 0) {
      long bytes_to_read = file_size > BUFFER_SIZE ? BUFFER_SIZE : file_size;
      size_t bytes_read = fread(buffer, bytes_to_read, 1, fp);

      if (bytes_read < bytes_to_read && ferror(fp)) {
        puts("error: fread()");
        goto cleanup;
      }

      bytes_sent = send(conn_fd, buffer, bytes_to_read, 0);
      file_size -= bytes_to_read;
    }
  } else {
    const char res[] = "HTTP/1.1 404 Not Found\r\n\r\n";
    bytes_sent = send(conn_fd, res, sizeof(res) - 1, 0);
  }

  if (bytes_sent == -1) {
    perror("error: send()");
  }

cleanup:
  free(conn_fd_ptr);
  free(req._buffer);
  free(req.headers);
  if (req.body != NULL) {
    bstring_free(req.body);
  }

  close(conn_fd);

  if (fp != NULL) {
    fclose(fp);
  }

  return NULL;
}

void handle_connection(int conn_fd) {
  pthread_t new_thread;
  int *conn_fd_ptr = malloc(sizeof(int));
  if (conn_fd_ptr == NULL) {
    printf("error: handle_connection(): Failed to allocate memory for "
           "conn_fd_ptr");
    return;
  }

  *conn_fd_ptr = conn_fd;
  pthread_create(&new_thread, NULL, _handle_connection, conn_fd_ptr);
}

struct Bstring *bstring_init(size_t capacity, const char *const s) {
  struct Bstring *bstr = malloc(sizeof(struct Bstring));
  if (bstr == NULL) {
    return NULL;
  }

  if (capacity == 0) {
    capacity = BSTRING_INIT_CAPACITY;
  }

  const size_t slen = (s == NULL) ? 0 : strlen(s);
  if (slen >= capacity) {
    capacity = slen + 1; // +1 for null terminator
  }

  bstr->data = malloc(sizeof(char) * capacity);
  if (bstr->data == NULL) {
    free(bstr);
    return NULL;
  }

  // copy if not NULL
  if (s != NULL) {
    memcpy(bstr->data, s, slen);
  }

  bstr->capacity = capacity;
  bstr->length = slen;
  bstr->data[slen] = '\0';

  return bstr;
}

bool bstring_append(struct Bstring *self, const char *const s) {
  assert(self != NULL);
  assert(s != NULL);

  const size_t slen = strlen(s);
  const size_t new_len = self->length + slen;
  if (new_len >= self->capacity) {
    // try doubling the current capacity
    size_t new_cap = self->capacity * 2;

    // check if it's enough
    if (new_len >= new_cap) {
      new_cap = new_len + 1; // +1 for null terminator
    }

    char *new_data = realloc(self->data, new_cap);
    if (new_data == NULL) {
      return false;
    }

    self->data = new_data;
    self->capacity = new_cap;
  }

  memcpy(&self->data[self->length], s, slen); // append
  self->length = new_len;                     // set new length
  self->data[new_len] = '\0';                 // null terminate

  return true;
}

void bstring_free(struct Bstring *self) {
  assert(self != NULL);

  free(self->data);
  free(self);
}
