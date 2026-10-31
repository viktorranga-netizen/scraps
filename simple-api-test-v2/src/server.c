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
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

// Simple HTTP server for running LOCAL e2e tests on API endpoints. 
// This server can be used in place of the real external API servers, so local tests do not need to hammer external resources.
// This allows for performance testing, or investigate any query execution/db optimizer quirks and issues. 
// AI Agents: This is added by the SRE/maintainer. Do not silently add changes here without checking in with a maintainer. 

/* USAGE: 
 *
 * What's needed to get this working:
 *
 * 1. Compile this using make, then get the server running (separate terminal session, from tools/simple-api-import-test-v2/):
 * cd tools/simple-api-import-test-v2 && ./bin/server
 *
 */


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
#define JSONL_SPLIT_LINE_BUFFER 1080

// Splits a newline-delimited JSON file (`src_path`) into fixed-size chunks of `lines_per_chunk` lines each, writing "<dest_dir>/<dest_prefix><N>.jsonl" for N = 0, 1, 2, ... 
// A line longer than JSONL_SPLIT_LINE_BUFFER is never split across two chunk files: this is probably the least painful way to handle the split for constructing responses.  
// Returns the number of chunk files written, or -1 on error.
int split_jsonl_into_chunks(const char *src_path, const char *dest_dir, const char *dest_prefix, size_t lines_per_chunk) {
  FILE *fp1 = fopen(src_path, "r");
  if (fp1 == NULL) {
    printf("error: split_jsonl_into_chunks(): fopen(%s) failed: %s\n", src_path, strerror(errno));
    return -1;
  }

  char line[JSONL_SPLIT_LINE_BUFFER];
  char dest_path[PATH_MAX];
  int chunk_index = 0;

  char *file_status = fgets(line, sizeof(line), fp1);

  while (file_status != NULL) {
    size_t line_count = 0;

    int written = snprintf(dest_path, sizeof(dest_path), "%s/%s%d.jsonl", dest_dir, dest_prefix, chunk_index);

    if (written < 0 || (size_t)written >= sizeof(dest_path)) {
      printf("error: split_jsonl_into_chunks(): destination path too long\n");
      fclose(fp1);
      return -1;
    }

    FILE *fp2 = fopen(dest_path, "w");

    if (fp2 == NULL) {
      printf("error: split_jsonl_into_chunks(): fopen(%s) failed: %s\n", dest_path, strerror(errno));
      fclose(fp1);
      return -1;
    }

    while (file_status != NULL) {
      fputs(line, fp2);
      size_t read_len = strlen(line);
      bool ends_with_newline = (read_len > 0 && line[read_len - 1] == '\n');

      if (ends_with_newline) {
        ++line_count;
      }

      file_status = fgets(line, sizeof(line), fp1);

      if (ends_with_newline && line_count >= lines_per_chunk) {
        break;
      }
    }

    fclose(fp2);
    ++chunk_index;
  }

  fclose(fp1);
  return chunk_index;
}

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

// ENDPOINT01 mock: serve this file's bytes verbatim (e.g. if already compressed) for
// GET /api/v1/endpoint01/Websites/all, so it works unmodified against a local fixture instead of a real API call.
// Full data set NOT checked into the repo (see src/resources/README.md) place a captured response there yourself before running this against endpoint01_import.pl.
// Resolved at startup relative to the binary's own location (via argv[0]), not the process's working directory, so it works no matter where `bin/server` is invoked from.
char endpoint01_fixture_path[PATH_MAX];

// Endpoint02 mock: GET /api/v1/endpoint02/marketplace/catalogue serves "<endpoint02_pages_dir>/page_<cursor>.jsonl" wrapped in the same envelope endpoint02_import.pl expects.
// The cursor is just the chunk index as a string. A missing/absent cursor defaults to page 0.
// endpoint02_pages_dir is populated lazily, on the first request, by splitting endpoint02_catalogue_src_path.
// Like the ENDPOINT01 fixture above, s full endpoint02_catalogue_src_path is not checked into the repo: capture one and drop it there yourself.
char endpoint02_pages_dir[PATH_MAX];
char endpoint02_catalogue_src_path[PATH_MAX];

// Endpoint03 mock: GET /api/v1/endpoint03/marketplace serves "<endpoint03_pages_dir>/page_<page-1>.jsonl" wrapped in the
// {data, meta:{total, last_page, current_page}} envelope endpoint03_import.pl expects. 
// Endpoint03 paginates by plain page number, not a cursor like Endpoint02/Endpoint05, so this walks meta.last_page instead of following a nextCursor/hasMore flag. 
// Kept as its own envelope builder rather than folded into Endpoint02's for that reason, even though both share the underlying split_jsonl_into_chunks()/append_jsonl_page_items() helpers.
// A full data file is not checked into the repo: capture one and drop it there yourself.
char endpoint03_pages_dir[PATH_MAX];
char endpoint03_catalogue_src_path[PATH_MAX];

// Endpoint04 mock: POST /api/v1/endpoint04/marketplace/search serves "<endpoint04_pages_dir>/page_<page-1>.jsonl" wrapped in the {results, total} envelope endpoint04_import.pl's fetch_page() expects.
// Endpoint04 paginates by plain page number like Endpoint03, but: the page number travels in the POST body (not a query string), and there is no last_page/hasMore signal at all.
// The importer script just stops once a page comes back shorter than its (hardcoded, non-configurable) page size of 100, so the mock's chunk size has to match that constant exactly rather than being an arbitrary internal choice like Endpoint02/Endpoint03's.
// A full data file is not checked into the repo: capture one and drop it there yourself.
#define ENDPOINT04_PAGE_SIZE 100
char endpoint04_pages_dir[PATH_MAX];
char endpoint04_catalogue_src_path[PATH_MAX];

// Endpoint05 mock: GET /api/v1/endpoint05/marketplace serves "<endpoint05_pages_dir>/page_<cursor>.jsonl" wrapped in the {data, meta:{nextCursor, total}} envelope endpoint05_import.pl expects. 
// Same cursor/stop-on-empty-nextCursor pagination as Endpoint02 (the cursor is just the chunk index as a string, missing/absent cursor defaults to page 0), but a different response envelope shape because it has no top-level "success" field, and nextCursor lives directly under "meta" rather than inside its own "pagination" object.
// Page size is a fixed 50 items with no override parameter.
// A full data file is not checked into the repo: capture one and drop it there yourself.
#define ENDPOINT05_PAGE_SIZE 50
char endpoint05_pages_dir[PATH_MAX];
char endpoint05_catalogue_src_path[PATH_MAX];

// Looks for `key=<value>` in a & joined GET query string (the part of a request path after '?') and parses an integer out of it,
// e.g. query_get_int_param("perPage=500&cursor=3", "cursor", &out) sets *out=3 and returns true.
bool query_get_int_param(const char *query, const char *key, long *out_value) {
  if (query == NULL) {
    return false;
  }

  size_t key_len = strlen(key);
  const char *field = query;

  while (field != NULL && *field != '\0') {
    if (strncmp(field, key, key_len) == 0 && field[key_len] == '=') {
      const char *value_start = field + key_len + 1;
      char *endptr = NULL;
      errno = 0;
      long value = strtol(value_start, &endptr, 10);
      if (endptr == value_start || errno != 0) {
        return false;
      }
      *out_value = value;
      return true;
    }

    field = strchr(field, '&');
    if (field != NULL) {
      ++field; // skip the '&'
    }
  }

  return false;
}

// Looks for `"key":<value>` in a JSON request body (optional whitespace after the colon) and parses an integer out of it, e.g. json_body_get_int_field("{\"page\":3,\"limit\":100}", "page", &out) sets *out=3 and returns true.
// DELIBERATELY kept simple, this way it stays dependency free keeping it portable. 
bool json_body_get_int_field(const char *body, const char *key, long *out_value) {
  if (body == NULL) {
    return false;
  }

  char needle[64];
  int needle_len = snprintf(needle, sizeof(needle), "\"%s\":", key);
  if (needle_len < 0 || (size_t)needle_len >= sizeof(needle)) {
    return false;
  }

  const char *match = strstr(body, needle);
  if (match == NULL) {
    return false;
  }

  const char *value_start = match + needle_len;
  while (*value_start == ' ') {
    ++value_start;
  }

  char *endptr = NULL;
  errno = 0;
  long value = strtol(value_start, &endptr, 10);
  if (endptr == value_start || errno != 0) {
    return false;
  }

  *out_value = value;
  return true;
}

// Populates `pages_dir` from `src_path` by splitting it into page_<N>.jsonl chunks of `lines_per_chunk` lines each, but only if page_0.jsonl does not yet exist.
// Shared by the Endpoint02, Endpoint03 and Endpoint04 mocks (see their builders below) so nothing besides `make && ./bin/server` and dropping a captured catalogue dump at src_path is needed, no separate CLI invocation to pre-split it.
void ensure_pages_split(const char *src_path, const char *pages_dir, size_t lines_per_chunk) {
  char first_page_path[PATH_MAX];
  snprintf(first_page_path, sizeof(first_page_path), "%s/page_0.jsonl", pages_dir);

  FILE *first_page = fopen(first_page_path, "r");
  if (first_page != NULL) {
    fclose(first_page);
    return; // already split
  }

  if (mkdir(pages_dir, 0755) != 0 && errno != EEXIST) {
    printf("error: ensure_pages_split(): mkdir(%s) failed: %s\n", pages_dir, strerror(errno));
    return;
  }

  if (split_jsonl_into_chunks(src_path, pages_dir, "page_", lines_per_chunk) < 0) {
    printf("error: ensure_pages_split(): split_jsonl_into_chunks() failed\n");
  }
}

// Appends the items from "<pages_dir>/page_<chunk_index>.jsonl" to `body` as comma-joined JSON array entries.
// A missing page file is treated as an empty page, not an error: both callers use that to mean "no more pages".
// Reassembles a line across multiple reads the same way split_jsonl_into_chunks's writer does, so an item longer than JSONL_SPLIT_LINE_BUFFER is never cut into two JSON array entries.
// NOT a candidate to fold into split_jsonl_into_chunks despite the similar-looking loop: that one writes lines straight to a file as it goes, this one buffers a line into an in-memory Bstring before deciding whether to comma-join it into the response. 
// Sharing a helper between "append to a FILE*" and "append to a Bstring" would need an abstraction over the destination for the two call sites that use it, which isn't worth it for this little logic.
// Returns true if the page file existed (regardless of whether it had any items), false if it was missing.
bool append_jsonl_page_items(struct Bstring *body, const char *pages_dir, long chunk_index, bool *first_item) {
  char page_path[PATH_MAX];
  snprintf(page_path, sizeof(page_path), "%s/page_%ld.jsonl", pages_dir, chunk_index);

  FILE *fp = fopen(page_path, "r");
  if (fp == NULL) {
    return false;
  }

  char line[JSONL_SPLIT_LINE_BUFFER];
  struct Bstring *item = NULL;

  char *file_status = fgets(line, sizeof(line), fp);
  while (file_status != NULL) {
    if (item == NULL) {
      item = bstring_init(0, NULL);
    }

    size_t len = strlen(line);
    bool ends_with_newline = (len > 0 && line[len - 1] == '\n');
    if (ends_with_newline) {
      line[--len] = '\0';
      if (len > 0 && line[len - 1] == '\r') {
        line[--len] = '\0';
      }
    }

    bstring_append(item, line);

    file_status = fgets(line, sizeof(line), fp);

    if (ends_with_newline) {
      if (item->length > 0) {
        if (!*first_item) {
          bstring_append(body, ",");
        }
        bstring_append(body, item->data);
        *first_item = false;
      }
      bstring_free(item);
      item = NULL;
    }
  }

  // EOF without a trailing newline on the last line, still a full item.
  if (item != NULL) {
    if (item->length > 0) {
      if (!*first_item) {
        bstring_append(body, ",");
      }
      bstring_append(body, item->data);
    }
    bstring_free(item);
  }

  fclose(fp);
  return true;
}

// Counts the page_<N>.jsonl chunk files under `pages_dir` (N = 0, 1, 2, ... until one is missing) and the total number of items across all of them.
// E.g. this is used to compute Endpoint03's meta.total/last_page once per server run: unlike ensure_pages_split()'s line count, chunk files from a previous run of this server are typically still sitting on disk, so this scan keeps the logic simpler.
void count_jsonl_pages(const char *pages_dir, long *out_last_page, long *out_total_items) {
  long last_page = 0;
  long total_items = 0;

  for (long chunk_index = 0;; ++chunk_index) {
    char page_path[PATH_MAX];
    snprintf(page_path, sizeof(page_path), "%s/page_%ld.jsonl", pages_dir, chunk_index);
    FILE *fp = fopen(page_path, "r");
    if (fp == NULL) {
      break;
    }

    char line[JSONL_SPLIT_LINE_BUFFER];
    char *file_status = fgets(line, sizeof(line), fp);
    while (file_status != NULL) {
      size_t len = strlen(line);
      if (len > 0 && line[len - 1] == '\n') {
        ++total_items;
      }
      file_status = fgets(line, sizeof(line), fp);
    }
    fclose(fp);

    last_page = chunk_index + 1; // chunk N == page N+1, Endpoint03's pages are 1-indexed
  }

  *out_last_page = last_page;
  *out_total_items = total_items;
}

// Builds the JSON body for one page of the Endpoint02 catalogue mock: wraps "<pages_dir>/page_<page_index>.jsonl" in the envelope the real API returns.
// The cursor walk needs to stop once there is nothing left, so "no next chunk file" becomes hasMore=false rather than an error.
// Returns a Bstring the caller owns (bstring_free it), or NULL on allocation failure.
struct Bstring *build_endpoint02_catalogue_page(const char *pages_dir, long page_index) {
  ensure_pages_split(endpoint02_catalogue_src_path, pages_dir, 500);

  struct Bstring *body = bstring_init(0, "{\"success\":true,\"data\":[");
  if (body == NULL) {
    return NULL;
  }

  bool first_item = true;
  bool page_exists = append_jsonl_page_items(body, pages_dir, page_index, &first_item);
  bool has_more = false;

  if (page_exists) {
    char next_page_path[PATH_MAX];
    snprintf(next_page_path, sizeof(next_page_path), "%s/page_%ld.jsonl", pages_dir, page_index + 1);
    FILE *next_fp = fopen(next_page_path, "r");
    if (next_fp != NULL) {
      has_more = true;
      fclose(next_fp);
    }
  }

  bstring_append(body, "],\"pagination\":{\"hasMore\":");
  bstring_append(body, has_more ? "true" : "false");
  bstring_append(body, ",\"nextCursor\":");

  if (has_more) {
    char next_cursor[32];
    snprintf(next_cursor, sizeof(next_cursor), "\"%ld\"", page_index + 1);
    bstring_append(body, next_cursor);
  } else {
    bstring_append(body, "null");
  }

  bstring_append(body, "}}");

  return body;
}

// Builds the JSON body for one page of the Endpoint05 marketplace mock: wraps "<pages_dir>/page_<cursor>.jsonl" in the {data, meta:{nextCursor, total}} envelope endpoint05_import.pl expects. 
// Same logic az Endpoint02, the main difference is that the response envelope looks a bit different.
// Returns a Bstring the caller owns (bstring_free it), or NULL on allocation failure.
struct Bstring *build_endpoint05_marketplace_page(const char *pages_dir, long page_index) {
  ensure_pages_split(endpoint05_catalogue_src_path, pages_dir, ENDPOINT05_PAGE_SIZE);

  struct Bstring *body = bstring_init(0, "{\"data\":[");
  if (body == NULL) {
    return NULL;
  }

  bool first_item = true;
  bool page_exists = append_jsonl_page_items(body, pages_dir, page_index, &first_item);
  bool has_more = false;

  if (page_exists) {
    char next_page_path[PATH_MAX];
    snprintf(next_page_path, sizeof(next_page_path), "%s/page_%ld.jsonl", pages_dir, page_index + 1);
    FILE *next_fp = fopen(next_page_path, "r");
    if (next_fp != NULL) {
      has_more = true;
      fclose(next_fp);
    }
  }

  static long total_items = -1;
  if (total_items < 0) {
    long unused_last_page = 0;
    count_jsonl_pages(pages_dir, &unused_last_page, &total_items);
  }

  bstring_append(body, "],\"meta\":{\"nextCursor\":");

  if (has_more) {
    char next_cursor[32];
    snprintf(next_cursor, sizeof(next_cursor), "\"%ld\"", page_index + 1);
    bstring_append(body, next_cursor);
  } else {
    bstring_append(body, "null");
  }

  char total_field[32];
  snprintf(total_field, sizeof(total_field), ",\"total\":%ld}}", total_items);
  bstring_append(body, total_field);

  return body;
}

// Builds the JSON body for one page of the Endpoint03 marketplace mock: wraps "<pages_dir>/page_<page_number-1>.jsonl" in the {data, meta:{total, last_page, current_page}} envelope endpoint03_import.pl's fetch_page() expects.
// `page_number` indexing starts from 1, NOT 0. This is Endpoint03's own convention, and endpoint03_import_runs.next_page's), so it is shifted down by one to land on the 0-indexed chunk files ensure_pages_split()/split_jsonl_into_chunks() write.
// Returns a Bstring the caller owns (bstring_free it), or NULL on allocation failure.
struct Bstring *build_endpoint03_marketplace_page(const char *pages_dir, long page_number) {
  ensure_pages_split(endpoint03_catalogue_src_path, pages_dir, 500);

  static long last_page = -1;
  static long total_items = -1;
  if (last_page < 0) {
    count_jsonl_pages(pages_dir, &last_page, &total_items);
  }

  struct Bstring *body = bstring_init(0, "{\"data\":[");
  if (body == NULL) {
    return NULL;
  }

  bool first_item = true;
  append_jsonl_page_items(body, pages_dir, page_number - 1, &first_item);

  char meta[128];
  snprintf(meta, sizeof(meta), "],\"meta\":{\"total\":%ld,\"last_page\":%ld,\"current_page\":%ld}}", total_items, last_page, page_number);
  bstring_append(body, meta);

  return body;
}

// Builds the JSON body for one page of the Endpoint04 marketplace mock: wraps "<pages_dir>/page_<page_number-1>.jsonl" in the {results, total} envelope endpoint04_import.pl's fetch_page() expects. 
// No last_page/hasMore field exists here, and the importer script itself decides it's done once `results` comes back shorter than its page size, so the mock doesn't need to track or report anything beyond the raw item count.
// `page_number` is indexing starts from 1, NOT 0, (endpoint04_import_runs.next_page's own convention). 
// Returns a Bstring the caller owns (bstring_free it), or NULL on allocation failure.
struct Bstring *build_endpoint04_search_page(const char *pages_dir, long page_number) {
  ensure_pages_split(endpoint04_catalogue_src_path, pages_dir, ENDPOINT04_PAGE_SIZE);

  struct Bstring *body = bstring_init(0, "{\"results\":[");
  if (body == NULL) {
    return NULL;
  }

  bool first_item = true;
  append_jsonl_page_items(body, pages_dir, page_number - 1, &first_item);

  static long total_items = -1;
  if (total_items < 0) {
    long unused_last_page = 0;
    count_jsonl_pages(pages_dir, &unused_last_page, &total_items);
  }

  char meta[64];
  snprintf(meta, sizeof(meta), "],\"total\":%ld}", total_items);
  bstring_append(body, meta);

  return body;
}

// Fallback mechanism, mostly intended for CI: if a real sized file for reading does not exist, fall back to the committed small sized files for reading.
// Resolves "<bin_dir>/../src/resources/<filename>" into `out` if that file exists, otherwise falls back to "<bin_dir>/../src/resources/small/<filename>". 
// The small/ files (a handful of rows per fixture) are deliberately committed, unlike the full dumps from staging gitignored for size) a dev captures locally, so CI, which only has small/, runs this same binary unmodified against it. 
//The fallback happens here, once, at startup, rather than through an argv/env flag, so nothing about how a mock is invoked needs to differ between a dev machine and CI.
// Returns true if it fell back to small/. The caller MUST fold that into its pages_dir (see main()): ensure_pages_split()
// only checks whether page_0.jsonl already exists, with no idea whether a prior run's leftover chunks came from this same source file.
bool resolve_catalogue_path(char *out, size_t out_size, const char *bin_dir, const char *filename) {
  snprintf(out, out_size, "%s/../src/resources/%s", bin_dir, filename);

  FILE *fp = fopen(out, "r");
  if (fp != NULL) {
    fclose(fp);
    return false;
  }

  snprintf(out, out_size, "%s/../src/resources/small/%s", bin_dir, filename);
  printf("%s not found, falling back to small/%s\n", filename, filename);
  return true;
}

int main(int argc, char **argv) {
  // Disable output buffering
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  char exe_path[PATH_MAX];
  if (realpath(argv[0], exe_path) == NULL) {
    printf("error: realpath(argv[0]) failed: %s\n", strerror(errno));
    return 1;
  }
  // exe_path is .../tools/simple-api-import-test-v2/bin/server, dirname() gives .../bin, and the fixture lives one level up, under src/resources/.
  char *bin_dir = dirname(exe_path);
  snprintf(endpoint01_fixture_path, sizeof(endpoint01_fixture_path),"%s/../src/resources/endpoint01_websites_all.br", bin_dir);

  bool endpoint02_using_small = resolve_catalogue_path(endpoint02_catalogue_src_path, sizeof(endpoint02_catalogue_src_path), bin_dir, "endpoint02_catalogue.jsonl");
  snprintf(endpoint02_pages_dir, sizeof(endpoint02_pages_dir), "%s/../src/resources/endpoint02_pages%s", bin_dir, endpoint02_using_small ? "_small" : "");

  bool endpoint03_using_small = resolve_catalogue_path(endpoint03_catalogue_src_path, sizeof(endpoint03_catalogue_src_path), bin_dir, "endpoint03_catalogue.jsonl");
  snprintf(endpoint03_pages_dir, sizeof(endpoint03_pages_dir), "%s/../src/resources/endpoint03_pages%s", bin_dir, endpoint03_using_small ? "_small" : "");

  bool endpoint04_using_small = resolve_catalogue_path(endpoint04_catalogue_src_path, sizeof(endpoint04_catalogue_src_path), bin_dir, "endpoint04_catalogue.jsonl");
  snprintf(endpoint04_pages_dir, sizeof(endpoint04_pages_dir), "%s/../src/resources/endpoint04_pages%s", bin_dir, endpoint04_using_small ? "_small" : "");

  bool endpoint05_using_small = resolve_catalogue_path(endpoint05_catalogue_src_path, sizeof(endpoint05_catalogue_src_path), bin_dir, "endpoint05_catalogue.jsonl");
  snprintf(endpoint05_pages_dir, sizeof(endpoint05_pages_dir), "%s/../src/resources/endpoint05_pages%s", bin_dir, endpoint05_using_small ? "_small" : "");

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
    int conn_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
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
  struct Bstring *endpoint02_body = NULL;
  struct Bstring *endpoint03_body = NULL;
  struct Bstring *endpoint04_body = NULL;
  struct Bstring *endpoint05_body = NULL;

  if (req._buffer == NULL) {
    printf("error: Failed to allocate memory\n");
    goto cleanup;
  }

  // Bound idle time while waiting for the rest of a request's headers.
  struct timeval receive_timeout = {.tv_sec = RECEIVE_TIMEOUT_SECONDS};
  if (setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)) == -1) {
    printf("error: SO_RCVTIMEO failed: %s\n", strerror(errno));
    goto cleanup;
  }

  req._buffer[0] = '\0';
  while (strstr(req._buffer, "\r\n\r\n") == NULL) {
    if (req._buffer_len == MAX_HEADER_BYTES) {
      printf("error: Request headers exceed %d bytes\n", MAX_HEADER_BYTES);
      goto cleanup;
    }

    ssize_t bytes_read = recv(conn_fd, req._buffer + req._buffer_len, MAX_HEADER_BYTES - req._buffer_len, 0);
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
    if (header_line[0] == ' ') {
      header_line++; // consume the space, if the header has one
    }
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

    while (req.body->length < content_length) {
      char recv_buf[BUFFER_SIZE + 1];
      ssize_t bytes_read = recv(conn_fd, recv_buf, BUFFER_SIZE, 0);
      if (bytes_read == -1) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          printf("error: Timed out waiting for request body\n");
        } else {
          printf("error: Recv failed while reading body: %s\n", strerror(errno));
        }
        break;
      }
      if (bytes_read == 0) {
        printf("error: Connection closed before request body was complete\n");
        break;
      }

      recv_buf[bytes_read] = '\0';
      bstring_append(req.body, recv_buf);
    }

    // content lenth mismatch
    if (req.body->length != content_length) {
      printf("error: Content-Length mismatch: declared %zu, received %zu\n", content_length, req.body->length);
    }
  }

  // send response
  int bytes_sent = -1;

  // Split off the query string (if any) so route matching below compares against the bare
  // path, e.g. "/api/v1/endpoint02/marketplace/catalogue" rather than "...?perPage=500&cursor=3".
  char *query = strchr(req.path, '?');
  if (query != NULL) {
    *query = '\0';
    ++query;
  }

  // add more API paths here as desired
  if (req.method == HTTP_GET && strcmp(req.path, "/api/v1/endpoint01/Websites/all") == 0) {
    errno = 0;
    fp = fopen(endpoint01_fixture_path, "r");
    if (fp == NULL) {
      perror("error: fopen(endpoint01_fixture_path)");
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
  } else if (req.method == HTTP_GET && strcmp(req.path, "/api/v1/endpoint02/marketplace/catalogue") == 0) {
    long page_index = 0;
    query_get_int_param(query, "cursor", &page_index); // missing/invalid cursor -> page 0

    endpoint02_body = build_endpoint02_catalogue_page(endpoint02_pages_dir, page_index);
    if (endpoint02_body == NULL) {
      printf("error: build_endpoint02_catalogue_page() failed\n");
      const char res[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
      bytes_sent = send(conn_fd, res, sizeof(res) - 1, 0);
      goto cleanup;
    }

    char header[BUFFER_SIZE];
    int header_len = snprintf(header, sizeof(header),
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %zu\r\n\r\n",
                               endpoint02_body->length);
    bytes_sent = send(conn_fd, header, (size_t)header_len, 0);
    bytes_sent = send(conn_fd, endpoint02_body->data, endpoint02_body->length, 0);
  } else if (req.method == HTTP_GET && strcmp(req.path, "/api/v1/endpoint05/marketplace") == 0) {
    long page_index = 0;
    query_get_int_param(query, "cursor", &page_index); // missing/invalid cursor -> page 0

    endpoint05_body = build_endpoint05_marketplace_page(endpoint05_pages_dir, page_index);
    if (endpoint05_body == NULL) {
      printf("error: build_endpoint05_marketplace_page() failed\n");
      const char res[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
      bytes_sent = send(conn_fd, res, sizeof(res) - 1, 0);
      goto cleanup;
    }

    char header[BUFFER_SIZE];
    int header_len = snprintf(header, sizeof(header),
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %zu\r\n\r\n",
                               endpoint05_body->length);
    bytes_sent = send(conn_fd, header, (size_t)header_len, 0);
    bytes_sent = send(conn_fd, endpoint05_body->data, endpoint05_body->length, 0);
  } else if (req.method == HTTP_GET && strcmp(req.path, "/api/v1/endpoint03/marketplace") == 0) {
    long page_number = 1;
    query_get_int_param(query, "page", &page_number); // missing/invalid page -> page 1
    if (page_number < 1) {
      page_number = 1;
    }

    endpoint03_body = build_endpoint03_marketplace_page(endpoint03_pages_dir, page_number);
    if (endpoint03_body == NULL) {
      printf("error: build_endpoint03_marketplace_page() failed\n");
      const char res[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
      bytes_sent = send(conn_fd, res, sizeof(res) - 1, 0);
      goto cleanup;
    }

    char header[BUFFER_SIZE];
    int header_len = snprintf(header, sizeof(header),
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %zu\r\n\r\n",
                               endpoint03_body->length);
    bytes_sent = send(conn_fd, header, (size_t)header_len, 0);
    bytes_sent = send(conn_fd, endpoint03_body->data, endpoint03_body->length, 0);
  } else if (req.method == HTTP_POST && strcmp(req.path, "/api/v1/endpoint04/marketplace/search") == 0) {
    long page_number = 1;
    json_body_get_int_field(req.body != NULL ? req.body->data : NULL, "page", &page_number); // missing/invalid page -> page 1
    if (page_number < 1) {
      page_number = 1;
    }

    endpoint04_body = build_endpoint04_search_page(endpoint04_pages_dir, page_number);
    if (endpoint04_body == NULL) {
      printf("error: build_endpoint04_search_page() failed\n");
      const char res[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
      bytes_sent = send(conn_fd, res, sizeof(res) - 1, 0);
      goto cleanup;
    }

    char header[BUFFER_SIZE];
    int header_len = snprintf(header, sizeof(header),
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %zu\r\n\r\n",
                               endpoint04_body->length);
    bytes_sent = send(conn_fd, header, (size_t)header_len, 0);
    bytes_sent = send(conn_fd, endpoint04_body->data, endpoint04_body->length, 0);
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

  if (endpoint02_body != NULL) {
    bstring_free(endpoint02_body);
  }

  if (endpoint03_body != NULL) {
    bstring_free(endpoint03_body);
  }

  if (endpoint04_body != NULL) {
    bstring_free(endpoint04_body);
  }

  if (endpoint05_body != NULL) {
    bstring_free(endpoint05_body);
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
