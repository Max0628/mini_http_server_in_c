/*
 * Minimal HTTP Server - V1
 *
 * 這是一個教學用途的 HTTP/1.1 server。
 *
 * 目前的限制：
 *   - IPv4
 *   - TCP
 *   - blocking I/O
 *   - single-threaded
 *   - 一條 connection 只處理一個 HTTP request
 *   - 只成功支援 GET /
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 8080
#define REQUEST_HEADER_LIMIT 8192
#define CONNECTION_TIMEOUT_SECONDS 5
#define LISTEN_BACKLOG 16

struct http_request {
    char method[16];
    char target[1024];
    char version[16];
    int has_host_header;
    int has_request_body;
};

enum http_status {
    HTTP_OK = 200,
    HTTP_BAD_REQUEST = 400,
    HTTP_NOT_FOUND = 404,
    HTTP_REQUEST_TIMEOUT = 408,
    HTTP_REQUEST_HEADER_TOO_LARGE = 431,
    HTTP_METHOD_NOT_ALLOWED = 405,
    HTTP_VERSION_NOT_SUPPORTED = 505
};

static int create_server_socket(void);
static int configure_server_socket(int server_fd);
static enum http_status read_request(int client_fd, char *buffer,
                                     size_t buffer_size, size_t *header_length);
static int parse_request(const char *buffer, size_t header_length,
                         struct http_request *request);
static int send_response(int client_fd, enum http_status status);
static int send_all(int client_fd, const char *buffer, size_t length);
static void handle_client(int client_fd);
static const char *reason_phrase(enum http_status status);
static const char *response_body(enum http_status status);
static int set_socket_timeout(int socket_fd, int seconds);
static int set_socket_timeout_from_deadline(int socket_fd,
                                            const struct timespec *deadline);
static int has_expired(const struct timespec *deadline);
static ssize_t find_header_end(const char *buffer, size_t length);
static int is_token_char(unsigned char character);
static int parse_request_line(char *line, struct http_request *request);
static int parse_header_line(char *line, struct http_request *request);

int main(void)
{
    int server_fd;

    /*
     * send() 在對方已經關閉 connection 時，可能觸發 SIGPIPE，
     * 讓整個 process 直接結束。忽略 SIGPIPE 後，send() 會回傳錯誤，
     * 我們就能正常清理該 client connection，並繼續服務其他 client。
     */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    server_fd = create_server_socket();
    if (server_fd == -1) {
        return EXIT_FAILURE;
    }

    if (configure_server_socket(server_fd) == -1) {
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Listening on 0.0.0.0:%d\n", SERVER_PORT);

    for (;;) {
        int client_fd;

        /*
         * accept() 是 system call：從 listening socket 取出一條已建立的
         * TCP connection，並回傳專門和該 client 溝通的新 file descriptor。
         */
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("accept");
            continue;
        }

        handle_client(client_fd);
    }
}

static int create_server_socket(void)
{
    int server_fd;
    int reuse_address = 1;

    /*
     * socket() 是 system call：向 Linux kernel 要一個 socket file
     * descriptor。AF_INET 表示 IPv4，SOCK_STREAM 表示 TCP。
     */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return -1;
    }

    /*
     * setsockopt() 是 system call：設定 socket 行為。
     * SO_REUSEADDR 讓 server 重啟時，比較不會因為舊 connection 的
     * TCP 狀態而暫時無法重新 bind 同一個 port。
     */
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) == -1) {
        perror("setsockopt(SO_REUSEADDR)");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

static int configure_server_socket(int server_fd)
{
    struct sockaddr_in server_address;

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(SERVER_PORT);
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);

    /*
     * bind() 是 system call：把 socket 綁定到指定的 IP address 與 port。
     * INADDR_ANY 代表接受這台 Linux container 所有 IPv4 interface 的流量。
     */
    if (bind(server_fd, (struct sockaddr *)&server_address,
             sizeof(server_address)) == -1) {
        perror("bind");
        return -1;
    }

    /*
     * listen() 是 system call：把 socket 從一般 TCP socket 變成
     * listening socket，等待 client 連入。LISTEN_BACKLOG 是等待佇列大小。
     */
    if (listen(server_fd, LISTEN_BACKLOG) == -1) {
        perror("listen");
        return -1;
    }

    return 0;
}

static void handle_client(int client_fd)
{
    char request_buffer[REQUEST_HEADER_LIMIT];
    size_t header_length = 0;
    struct http_request request;
    enum http_status status;

    /*
     * SO_RCVTIMEO / SO_SNDTIMEO 會讓 socket 的 blocking read/write
     * 不會無限等待。read_request() 另外用 absolute deadline，避免 client
     * 每隔幾秒送一個 byte 就不斷延長整體等待時間。
     */
    if (set_socket_timeout(client_fd, CONNECTION_TIMEOUT_SECONDS) == -1) {
        perror("setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)");
        close(client_fd);
        return;
    }

    status = read_request(client_fd, request_buffer, sizeof(request_buffer),
                          &header_length);
    if (status != HTTP_OK) {
        send_response(client_fd, status);
        close(client_fd);
        return;
    }

    if (parse_request(request_buffer, header_length, &request) == -1) {
        send_response(client_fd, HTTP_BAD_REQUEST);
        close(client_fd);
        return;
    }

    if (strcmp(request.version, "HTTP/1.1") != 0) {
        send_response(client_fd, HTTP_VERSION_NOT_SUPPORTED);
    } else if (strcmp(request.method, "GET") != 0) {
        send_response(client_fd, HTTP_METHOD_NOT_ALLOWED);
    } else if (strcmp(request.target, "/") != 0) {
        send_response(client_fd, HTTP_NOT_FOUND);
    } else if (request.has_request_body) {
        /* v1 不處理 request body，避免把未定義的行為當成成功。 */
        send_response(client_fd, HTTP_BAD_REQUEST);
    } else {
        send_response(client_fd, HTTP_OK);
    }

    /* close() 是 system call：釋放 client connection 的 file descriptor。 */
    close(client_fd);
}

static enum http_status read_request(int client_fd, char *buffer,
                                     size_t buffer_size, size_t *header_length)
{
    struct timespec deadline;
    size_t bytes_read = 0;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == -1) {
        perror("clock_gettime");
        return HTTP_REQUEST_TIMEOUT;
    }

    deadline.tv_sec += CONNECTION_TIMEOUT_SECONDS;

    while (bytes_read < buffer_size) {
        ssize_t received;
        ssize_t end_position;
        size_t remaining = buffer_size - bytes_read;

        if (set_socket_timeout_from_deadline(client_fd, &deadline) == -1) {
            perror("setsockopt(SO_RCVTIMEO)");
            return HTTP_REQUEST_TIMEOUT;
        }

        /*
         * recv() 是 system call：從 TCP receive buffer 讀取 client 傳來的
         * bytes。一次 recv() 不一定拿到完整 HTTP request，所以必須迴圈讀。
         */
        received = recv(client_fd, buffer + bytes_read, remaining, 0);
        if (received == 0) {
            /* client 在 header 完成前關閉 connection。 */
            return HTTP_BAD_REQUEST;
        }

        if (received == -1) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return HTTP_REQUEST_TIMEOUT;
            }

            perror("recv");
            return HTTP_BAD_REQUEST;
        }

        bytes_read += (size_t)received;
        end_position = find_header_end(buffer, bytes_read);
        if (end_position >= 0) {
            *header_length = (size_t)end_position;
            return HTTP_OK;
        }

        if (has_expired(&deadline)) {
            return HTTP_REQUEST_TIMEOUT;
        }
    }

    /* buffer 已經填滿，仍然找不到 \r\n\r\n。 */
    return HTTP_REQUEST_HEADER_TOO_LARGE;
}

static int parse_request(const char *buffer, size_t header_length,
                         struct http_request *request)
{
    char header_copy[REQUEST_HEADER_LIMIT + 1];
    char *line_start;
    char *line_end;
    int line_number = 0;

    if (header_length > REQUEST_HEADER_LIMIT) {
        return -1;
    }

    memcpy(header_copy, buffer, header_length);
    header_copy[header_length] = '\0';

    memset(request, 0, sizeof(*request));
    line_start = header_copy;

    while ((line_end = strstr(line_start, "\r\n")) != NULL) {
        *line_end = '\0';

        if (line_number == 0) {
            if (parse_request_line(line_start, request) == -1) {
                return -1;
            }
        } else if (*line_start != '\0') {
            if (parse_header_line(line_start, request) == -1) {
                return -1;
            }
        }

        line_number++;
        line_start = line_end + 2;
    }

    if (line_number == 0 || request->has_host_header == 0) {
        return -1;
    }

    /* 不是 HTTP request version 的格式，就屬於 malformed request。 */
    if (strncmp(request->version, "HTTP/", 5) != 0) {
        return -1;
    }

    return 0;
}

static int parse_request_line(char *line, struct http_request *request)
{
    char *first_space;
    char *second_space;
    char *extra_space;
    size_t method_length;
    size_t target_length;
    size_t version_length;

    first_space = strchr(line, ' ');
    if (first_space == NULL) {
        return -1;
    }

    second_space = strchr(first_space + 1, ' ');
    if (second_space == NULL || second_space == first_space + 1) {
        return -1;
    }

    extra_space = strchr(second_space + 1, ' ');
    if (extra_space != NULL || *(second_space + 1) == '\0') {
        return -1;
    }

    method_length = (size_t)(first_space - line);
    target_length = (size_t)(second_space - first_space - 1);
    version_length = strlen(second_space + 1);

    if (method_length == 0 || method_length >= sizeof(request->method) ||
        target_length == 0 || target_length >= sizeof(request->target) ||
        version_length == 0 || version_length >= sizeof(request->version)) {
        return -1;
    }

    for (size_t index = 0; index < method_length; index++) {
        if (!is_token_char((unsigned char)line[index])) {
            return -1;
        }
    }

    memcpy(request->method, line, method_length);
    request->method[method_length] = '\0';
    memcpy(request->target, first_space + 1, target_length);
    request->target[target_length] = '\0';
    memcpy(request->version, second_space + 1, version_length);
    request->version[version_length] = '\0';

    return 0;
}

static int parse_header_line(char *line, struct http_request *request)
{
    char *colon;
    char *name_end;
    char *value_start;
    char *value_end;
    size_t name_length;
    size_t value_length;

    colon = strchr(line, ':');
    if (colon == NULL || colon == line) {
        return -1;
    }

    name_end = colon;
    while (name_end > line && name_end[-1] == ' ') {
        name_end--;
    }

    name_length = (size_t)(name_end - line);
    if (name_length == 0) {
        return -1;
    }

    for (size_t index = 0; index < name_length; index++) {
        if (!is_token_char((unsigned char)line[index])) {
            return -1;
        }
    }

    value_start = colon + 1;
    while (*value_start == ' ' || *value_start == '\t') {
        value_start++;
    }

    value_end = value_start + strlen(value_start);
    while (value_end > value_start &&
           (value_end[-1] == ' ' || value_end[-1] == '\t')) {
        value_end--;
    }

    value_length = (size_t)(value_end - value_start);
    if (value_length == 0) {
        return -1;
    }

    if (name_length == 4 && strncasecmp(line, "Host", name_length) == 0) {
        if (request->has_host_header) {
            return -1;
        }

        request->has_host_header = 1;
    } else if (name_length == 14 &&
               strncasecmp(line, "Content-Length", name_length) == 0) {
        /* v1 不支援 request body；只要出現非零 Content-Length 就拒絕。 */
        if (value_length != 1 || value_start[0] != '0') {
            request->has_request_body = 1;
        }
    } else if (name_length == 17 &&
               strncasecmp(line, "Transfer-Encoding", name_length) == 0) {
        request->has_request_body = 1;
    }

    return 0;
}

static int send_response(int client_fd, enum http_status status)
{
    const char *body = response_body(status);
    const char *allow_header = "";
    char response[1024];
    int response_length;

    if (status == HTTP_METHOD_NOT_ALLOWED) {
        allow_header = "Allow: GET\r\n";
    }

    response_length = snprintf(
        response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, reason_phrase(status), strlen(body), allow_header, body);

    if (response_length < 0 || (size_t)response_length >= sizeof(response)) {
        return -1;
    }

    return send_all(client_fd, response, (size_t)response_length);
}

static int send_all(int client_fd, const char *buffer, size_t length)
{
    size_t bytes_sent = 0;

    while (bytes_sent < length) {
        ssize_t sent;

        /*
         * send() 是 system call：把 application buffer 的 bytes 交給
         * Linux kernel 的 TCP socket buffer。一次 send() 可能只送出一部分，
         * 所以要持續送到完整 response 完成。
         */
        sent = send(client_fd, buffer + bytes_sent, length - bytes_sent, 0);
        if (sent == -1) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EPIPE) {
                return -1;
            }

            perror("send");
            return -1;
        }

        bytes_sent += (size_t)sent;
    }

    return 0;
}

static const char *reason_phrase(enum http_status status)
{
    switch (status) {
    case HTTP_OK:
        return "OK";
    case HTTP_BAD_REQUEST:
        return "Bad Request";
    case HTTP_NOT_FOUND:
        return "Not Found";
    case HTTP_REQUEST_TIMEOUT:
        return "Request Timeout";
    case HTTP_REQUEST_HEADER_TOO_LARGE:
        return "Request Header Fields Too Large";
    case HTTP_METHOD_NOT_ALLOWED:
        return "Method Not Allowed";
    case HTTP_VERSION_NOT_SUPPORTED:
        return "HTTP Version Not Supported";
    default:
        return "Internal Server Error";
    }
}

static const char *response_body(enum http_status status)
{
    switch (status) {
    case HTTP_OK:
        return "Hello";
    case HTTP_BAD_REQUEST:
        return "Bad Request";
    case HTTP_NOT_FOUND:
        return "Not Found";
    case HTTP_REQUEST_TIMEOUT:
        return "Request Timeout";
    case HTTP_REQUEST_HEADER_TOO_LARGE:
        return "Request Header Fields Too Large";
    case HTTP_METHOD_NOT_ALLOWED:
        return "Method Not Allowed";
    case HTTP_VERSION_NOT_SUPPORTED:
        return "HTTP Version Not Supported";
    default:
        return "Internal Server Error";
    }
}

static int set_socket_timeout(int socket_fd, int seconds)
{
    struct timeval timeout;

    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) == -1) {
        return -1;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) == -1) {
        return -1;
    }

    return 0;
}

static int set_socket_timeout_from_deadline(int socket_fd,
                                            const struct timespec *deadline)
{
    struct timespec now;
    struct timeval timeout;
    time_t seconds;
    long nanoseconds;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return -1;
    }

    seconds = deadline->tv_sec - now.tv_sec;
    nanoseconds = deadline->tv_nsec - now.tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000L;
    }

    if (seconds < 0 || (seconds == 0 && nanoseconds <= 0)) {
        timeout.tv_sec = 0;
        timeout.tv_usec = 1;
    } else {
        timeout.tv_sec = seconds;
        timeout.tv_usec = (suseconds_t)(nanoseconds / 1000);
        if (timeout.tv_usec == 0) {
            timeout.tv_usec = 1;
        }
    }

    return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout));
}

static int has_expired(const struct timespec *deadline)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return 1;
    }

    if (now.tv_sec > deadline->tv_sec) {
        return 1;
    }

    return now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec;
}

static ssize_t find_header_end(const char *buffer, size_t length)
{
    if (length < 4) {
        return -1;
    }

    for (size_t index = 0; index <= length - 4; index++) {
        if (buffer[index] == '\r' && buffer[index + 1] == '\n' &&
            buffer[index + 2] == '\r' && buffer[index + 3] == '\n') {
            return (ssize_t)(index + 4);
        }
    }

    return -1;
}

static int is_token_char(unsigned char character)
{
    if ((character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9')) {
        return 1;
    }

    switch (character) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return 1;
    default:
        return 0;
    }
}
