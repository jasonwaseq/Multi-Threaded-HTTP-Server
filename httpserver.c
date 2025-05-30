// Standard library and POSIX includes for I/O, threading, signal handling, etc.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

// Custom modules for HTTP server functionality
#include "connection.h"
#include "iowrapper.h"
#include "listener_socket.h"
#include "protocol.h"
#include "queue.h"
#include "request.h"
#include "response.h"
#include "rwlock.h"

#define DEFAULT_THREADS 4 // Default number of worker threads
#define TEMP_PATTERN    "/tmp/httpXXXXXX" // Template for creating temporary file names

static queue_t *request_queue = NULL; // Queue to hold incoming socket connections
static pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER; // Global mutex for shared state
static volatile sig_atomic_t running = 1; // Server running flag for graceful shutdown

// Structure for linked list of URI-based locks
struct lock_node {
    char *uri;
    rwlock_t *lock;
    struct lock_node *next;
};
static struct lock_node *locks = NULL; // Head of linked list for URI locks

// Fetches or creates a reader-writer lock for a specific URI
static rwlock_t *fetch_lock(const char *uri) {
    struct lock_node *cur;
    pthread_mutex_lock(&global_mutex); // Lock global mutex for thread-safe access
    for (cur = locks; cur; cur = cur->next) {
        if (strcmp(cur->uri, uri) == 0) { // Check if URI already has a lock
            pthread_mutex_unlock(&global_mutex);
            return cur->lock;
        }
    }
    // Create a new lock if not found
    cur = calloc(1, sizeof(*cur));
    if (!cur || !(cur->uri = strdup(uri))) {
        free(cur);
        pthread_mutex_unlock(&global_mutex);
        return NULL;
    }
    cur->lock = rwlock_new(N_WAY, 1); // Create new RW lock
    cur->next = locks;
    locks = cur;
    pthread_mutex_unlock(&global_mutex);
    return cur->lock;
}

// Logs request method, URI, response code, and request ID
static void audit(const char *method, conn_t *c, int code) {
    const char *uri = conn_get_uri(c);
    uri = uri ? uri : "<unknown>";
    const char *rid = conn_get_header(c, "Request-Id");
    rid = rid ? rid : "0";
    pthread_mutex_lock(&global_mutex);
    fprintf(stderr, "%s,%s,%d,%s\n", method, uri, code, rid);
    pthread_mutex_unlock(&global_mutex);
}

// Handles HTTP GET requests
static void do_get(conn_t *c) {
    const char *uri = conn_get_uri(c);
    uri = uri ? uri : "<unknown>";
    int fd = open(uri, O_RDONLY); // Open file for reading
    const Response_t *resp;
    if (fd < 0) {
        // Determine appropriate error response
        if (errno == EACCES)
            resp = &RESPONSE_FORBIDDEN;
        else if (errno == ENOENT)
            resp = &RESPONSE_NOT_FOUND;
        else
            resp = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(c, resp);
        audit("GET", c, response_get_code(resp));
        return;
    }
    rwlock_t *lk = fetch_lock(uri); // Get read lock for URI
    if (lk)
        reader_lock(lk);

    struct stat st;
    // Check file properties and send response
    if (!lk || fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) {
        resp = &RESPONSE_FORBIDDEN;
        conn_send_response(c, resp);
        audit("GET", c, response_get_code(resp));
    } else {
        const Response_t *send_res = conn_send_file(c, fd, st.st_size); // Send file contents
        int code = response_get_code(send_res ? send_res : &RESPONSE_OK);
        audit("GET", c, code);
    }
    close(fd); // Close file
    if (lk)
        reader_unlock(lk); // Release read lock
}

// Handles HTTP PUT requests
static void do_put(conn_t *c) {
    const char *uri = conn_get_uri(c);
    uri = uri ? uri : "<unknown>";
    char tmpl[] = TEMP_PATTERN;
    int tmpfd = mkstemp(tmpl); // Create temporary file
    const Response_t *resp;
    if (tmpfd < 0) {
        resp = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(c, resp);
        audit("PUT", c, response_get_code(resp));
        return;
    }
    resp = conn_recv_file(c, tmpfd); // Receive file from client
    if (resp) {
        close(tmpfd);
        unlink(tmpl);
        conn_send_response(c, resp);
        audit("PUT", c, response_get_code(resp));
        return;
    }
    lseek(tmpfd, 0, SEEK_SET); // Rewind temporary file
    rwlock_t *lk = fetch_lock(uri); // Get write lock for URI
    if (lk)
        writer_lock(lk);

    int existed = (access(uri, F_OK) == 0); // Check if file previously existed
    int outfd = open(uri, O_WRONLY | O_CREAT | O_TRUNC, 0644); // Open/overwrite target file
    if (outfd < 0) {
        if (errno == EACCES)
            resp = &RESPONSE_FORBIDDEN;
        else
            resp = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(c, resp);
        audit("PUT", c, response_get_code(resp));
    } else {
        // Write content from temp file to actual file
        char buf[4096];
        ssize_t n;
        while ((n = read(tmpfd, buf, sizeof(buf))) > 0) {
            if (write(outfd, buf, n) != n) {
                resp = &RESPONSE_INTERNAL_SERVER_ERROR;
                break;
            }
        }
        close(outfd);
        resp = existed ? &RESPONSE_OK : &RESPONSE_CREATED;
        conn_send_response(c, resp);
        audit("PUT", c, response_get_code(resp));
    }
    close(tmpfd);
    unlink(tmpl); // Remove temporary file
    if (lk)
        writer_unlock(lk); // Release write lock
}

// Worker thread function: handles queued client connections
static void *worker(void *_) {
    while (running) {
        uintptr_t val;
        if (queue_pop(request_queue, (void **) &val) && val != (uintptr_t) -1) {
            conn_t *c = conn_new((int) val); // Wrap raw socket FD into connection object
            const Response_t *pr = conn_parse(c); // Parse HTTP request
            if (pr) {
                conn_send_response(c, pr); // If parsing failed, send error
                audit("OTHER", c, response_get_code(pr));
            } else {
                const Request_t *rq = conn_get_request(c); // Get request type
                if (rq == &REQUEST_GET)
                    do_get(c); // Handle GET
                else if (rq == &REQUEST_PUT)
                    do_put(c); // Handle PUT
                else {
                    conn_send_response(c, &RESPONSE_NOT_IMPLEMENTED); // Unsupported method
                    audit("OTHER", c, 501);
                }
            }
            conn_delete(&c); // Free connection
            close((int) val); // Close socket
        }
    }
    return NULL;
}

// Signal handler for graceful shutdown
static void on_sigterm(int _) {
    running = 0;
}

// Main server loop: initializes resources and starts worker threads
int start_server(int port, int threads) {
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE
    signal(SIGTERM, on_sigterm); // Handle SIGTERM for shutdown

    request_queue = queue_new(threads); // Create thread-safe request queue
    Listener_Socket_t *ls = ls_new(port); // Create listening socket on specified port

    pthread_t *tids = calloc(threads, sizeof(*tids)); // Allocate thread IDs
    for (int i = 0; i < threads; ++i)
        pthread_create(&tids[i], NULL, worker, NULL); // Start worker threads

    while (running) {
        int fd = ls_accept(ls); // Accept new connection
        if (fd >= 0 && !queue_push(request_queue, (void *) (uintptr_t) fd))
            close(fd); // Push to queue or close on failure
    }
    return 0;
}

// Entry point: parses arguments and launches server
int main(int argc, char **argv) {
    int threads = DEFAULT_THREADS, opt;
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        if (opt == 't')
            threads = atoi(optarg); // Set thread count from -t option
    }
    if (optind >= argc) {
        fprintf(stderr, "Usage: %s [-t n] <port>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[optind]); // Get port number
    if (port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port\n");
        return 1;
    }
    return start_server(port, threads); // Start the server
}
