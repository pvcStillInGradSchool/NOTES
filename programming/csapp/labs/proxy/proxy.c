#include <stdio.h>
#include <assert.h>

#include "csapp.h"
#include "lru.h"  // the LRU cache

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

static lru_t *lru = NULL;

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

// #define CONCURRENT

#ifndef CONCURRENT
#define PRINTF(...) (printf(__VA_ARGS__))
#else
#define PRINTF(...) ((void)0)
#endif

void check_one_line(char const *line, ssize_t n) {
    assert(n >= 2);
    assert(line[n - 2] == '\r');
    assert(line[n - 1] == '\n');
}

int read_one_line(rio_t *rio, char *line) {
    ssize_t n = Rio_readlineb(rio, line, MAXLINE);
    check_one_line(line, n);
    PRINTF("[C >> P] %s", line);
    return n;
}

/**
 * @brief Returns an error message to the client.
 * 
 * Borrowed from `tiny.c`.
 * 
 */
void clienterror(int fd, char *cause, char *errnum, 
                 char *shortmsg, char *longmsg)
{
    char buf[MAXLINE];

    /* Print the HTTP response headers */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n\r\n");
    Rio_writen(fd, buf, strlen(buf));

    /* Print the HTTP response body */
    sprintf(buf, "<html><title>Tiny Proxy Error</title>");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<body bgcolor=""ffffff"">\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<hr><em>The Tiny Proxy server</em>\r\n");
    Rio_writen(fd, buf, strlen(buf));
}

/**
 * @brief Parse the URI from the client into the hostname and the URI to the server.
 * @return the address of the first char in the URI to the server
 */
char const *parse_uri(char const *uri_from_client, char *hostname) 
{
    char const *ptr, *uri_to_server;

    assert(strncmp(uri_from_client, "http", 4) == 0);

    /* `strchr(str, ch)` finds the first occurrence of `ch` in `str` */
    ptr = strchr(uri_from_client, '/');
    ptr += 2;
    uri_to_server = strchr(ptr, '/');
    size_t len = uri_to_server - ptr;
    strncpy(hostname, ptr, len);
    hostname[len] = '\0';

    return uri_to_server;
}

size_t min(size_t x, size_t y) {
  return x < y ? x : y;
}

int has_prefix(char const *line, char const *prefix) {
    return !strncmp(line, prefix, min(strlen(line), strlen(prefix)));
}

void forward_request(int server_fd, char const *method, char const *uri,
        char const *hostname, char const *buf) {
    char line[MAXLINE];

    // Send the request line:
    sprintf(line, "%s %s HTTP/1.0\r\n", method, uri);
    PRINTF("[P >> S] %s", line);
    Rio_writen(server_fd, line, strlen(line));
    // Send the HOST header:
    sprintf(line, "Host: %s\r\n", hostname);
    PRINTF("[P >> S] %s", line);
    Rio_writen(server_fd, line, strlen(line));
    // Send the User-Agent header:
    PRINTF("[P >> S] %s", user_agent_hdr);
    Rio_writen(server_fd, (void *)user_agent_hdr, strlen(user_agent_hdr));
    // Send the Connection header:
    sprintf(line, "%s\r\n", "Connection: close");
    PRINTF("[P >> S] %s", line);
    Rio_writen(server_fd, line, strlen(line));
    // Send the Connection header:
    sprintf(line, "%s\r\n", "Proxy-Connection: close");
    PRINTF("[P >> S] %s", line);
    Rio_writen(server_fd, line, strlen(line));
    // Send other headers:
    char *ptr;
    do {
        ptr = strstr(buf, "\r\n");
        size_t len = ptr - buf + 2;
        strncpy(line, buf, len);
        line[len] = '\0';
        if (has_prefix(line, "GET") || has_prefix(line, "Host") ||
                has_prefix(line, "User-Agent") ||
                has_prefix(line, "Connection") ||
                has_prefix(line, "Proxy-Connection")) {
            PRINTF("[ignore] %s", line);
        } else {
            PRINTF("[P >> S] %s", line);
            Rio_writen(server_fd, line, strlen(line));
        }
        if (buf == ptr) {
            break;
        }
        buf = ptr + 2;
    } while (1);
    assert(strcmp(line, "\r\n") == 0);
    assert(strcmp(buf, "\r\n") == 0);
    assert(buf == ptr);
}

void forward_response(rio_t *server_rio, int server_fd,
        int client_fd, char const *uri, char *header) {
    int n, content_length = -1;
    char *line = header;
    // forward the headers
    do {
      n = Rio_readlineb(server_rio, line, MAXLINE);
      check_one_line(line, n);
      PRINTF("[S >> C] %s", line);
      if (content_length < 0 && line[0] == 'C') {
          assert(strlen("Content-Length: ") == 16);
          if (!strncasecmp(line, "Content-Length: ", 16)) {
              content_length = atoi(line + 16);
              PRINTF("content_length = %d\r\n", content_length);
          }
      }
      line += n;
    } while (n != 2);
    int header_length = line - header;
    Rio_writen(client_fd, header, header_length);
    // forward the content
    char *content;
    int total_length = header_length + content_length;
    if (total_length <= MAX_CACHE_SIZE) {
        content = Malloc(total_length);
        memcpy(content, header, header_length);
        content += header_length;
    } else {
        content = Malloc(content_length);
    }
    n = Rio_readnb(server_rio, content, content_length);
    assert(n == content_length);
    Rio_writen(client_fd, content, n);
    if (total_length <= MAX_OBJECT_SIZE) {
        PRINTF("Cache the response.\n");
        lru_emplace(lru, uri, content - header_length, total_length);
    }
}

void serve(int client_fd) {
    // Read the entire HTTP request from the client and check whether the it is valid.
    size_t n = 0;
    char buf[MAXLINE], *line = buf;
    rio_t client_rio; Rio_readinitb(&client_rio, client_fd);
    // Read the first line:
    char method[MAXLINE], uri_from_client[MAXLINE], version[MAXLINE];
    n = read_one_line(&client_rio, line);
    sscanf(line, "%s %s %s", method, uri_from_client, version);
    if (strcasecmp(method, "GET")) {
        clienterror(client_fd, method, "501", "Not Implemented",
                    "Tiny Proxy does not implement this method");
        return;
    }
    PRINTF("* method = \"%s\"\n", method);
    PRINTF("* version = \"%s\"\n", version);
    PRINTF("* uri_from_client = \"%s\"\n", uri_from_client);
    // Already cached?
    item_t *item = lru_find(lru, uri_from_client);
    if (item) {
        PRINTF("Use the response from the cache.\n");
        Rio_writen(client_fd,
            (void *)item_data(item), item_size(item));
        return;
    }
    // Parse the URI from the client
    char hostname[MAXLINE];
    char const *uri_to_server = parse_uri(uri_from_client, hostname);
    PRINTF("  * hostname = \"%s\"\n", hostname);
    PRINTF("  * uri_to_server = \"%s\"\n", uri_to_server);
    // Read other lines:
    do {
      line += n;  // n does not account '\0'
      n = read_one_line(&client_rio, line);
    } while (n != 2);
    assert(strcmp(line, "\r\n") == 0);
    assert(strlen(buf) <= MAXLINE);
    PRINTF("Length of the request: %ld\n", strlen(buf));
    // Connect to the appropriate web server.
    int server_fd;
    char *port = strchr(hostname, ':');
    if (port) {  // port explicitly given, use it
      *port++ = '\0';
    }
    server_fd = Open_clientfd(hostname, port ? port : "80");
    if (port) {  // port explicitly given, use it
      *--port = ':';
    }
    PRINTF("Connected to (%s)\n", hostname);
    // Request the object the client specified.
    rio_t server_rio; Rio_readinitb(&server_rio, server_fd);
    forward_request(server_fd, method, uri_to_server, hostname, buf);
    // Read the server's response and forward it to the client.
    PRINTF("Forward response from server (%s) to client\n", hostname);
    forward_response(&server_rio, server_fd, client_fd, uri_from_client, buf);
    Close(server_fd);
}

void serve_by_iteration(int client_fd) {
    serve(client_fd);
    Close(client_fd);
}

/**
 * @brief The routine run in a thread.
 */
void *routine(void *vargp) {
    int client_fd = *((int *)vargp);
    Pthread_detach(Pthread_self());
    Free(vargp);
    serve(client_fd);
    Close(client_fd);
    return NULL;
}

void serve_by_thread(int client_fd) {
    int *client_fd_ptr = Malloc(sizeof(int));
    *client_fd_ptr = client_fd;

    pthread_t tid;
    Pthread_create(&tid, NULL, routine, client_fd_ptr);
}

int main(int argc, char **argv)
{
    struct sockaddr_storage client_addr;  /* Enough space for any address */
    socklen_t client_len = sizeof(struct sockaddr_storage);
    char client_host[MAXLINE], client_port[MAXLINE];

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }

    lru = lru_construct(MAX_CACHE_SIZE);
    int listen_fd = Open_listenfd(argv[1]);
    while (1) {
        int client_fd = Accept(listen_fd, (SA *)&client_addr, &client_len);
        Getnameinfo((SA *) &client_addr, client_len,
            client_host, MAXLINE, client_port, MAXLINE, 0);
        printf("Connected to (%s, %s)\n", client_host, client_port);
#ifdef CONCURRENT
        serve_by_thread(client_fd);
#else
        serve_by_iteration(client_fd);
#endif
    }

    lru_destruct(lru);
    exit(0);
}
