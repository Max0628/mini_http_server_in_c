
# Minimal HTTP Server — V1 Specification

## 1. Project Overview

### 1.1 Project Name

Minimal HTTP Server

### 1.2 Goal

Build a minimal HTTP/1.1 server in C to understand how a user-space application communicates with the Linux kernel through TCP sockets.

The project focuses on:

- Linux Socket Programming
- TCP Connection Lifecycle
- System Calls
- Blocking I/O
- HTTP Request Parsing
- HTTP Response Generation
- File Descriptor Management
- User Space and Kernel Space Interaction

The purpose is educational.

This project is not intended for production use.

### 1.3 Core Principle

Keep the implementation minimal.

The server must:

1. Accept a TCP connection.
2. Receive one HTTP request.
3. Parse the HTTP request.
4. Generate a fixed HTTP response.
5. Send the response.
6. Close the connection.
7. Continue accepting new connections.

Do not introduce unnecessary frameworks, libraries, or abstractions.

---

## 2. Technology Stack

| Category | Specification |
|---|---|
| Programming Language | C11 |
| Operating System | Linux ubuntu:24.04|
| Host OS | macOS |
| Development Environment | podman version 6.1.3 |
| Compiler | GCC |
| Build Tool | Make |
| Transport Protocol | TCP / IPv4 |
| Application Protocol | HTTP/1.1 subset |
| Server Port | 8080 |
| Server Bind Address | 0.0.0.0 |
| Host Access Address | 127.0.0.1:8080 |
| I/O Model | Blocking I/O |
| Concurrency Model | Single-threaded |
| External HTTP Libraries | Not allowed |
| Database | Not required |
| Application Framework | Not allowed |

The server must run inside a Linux container.

The implementation must not depend on macOS-specific networking APIs.

---

## 3. System Architecture

### 3.1 High-Level Architecture

```text
Client (curl / Browser)
         |
         | HTTP/1.1 over TCP
         v
Docker Published Port
         |
         v
Linux Kernel
         |
         | TCP/IP Stack
         | Socket Buffers
         v
Minimal HTTP Server (C)
         |
         +-- Connection Management
         |
         +-- Request Receiving
         |
         +-- HTTP Parsing
         |
         +-- Request Handling
         |
         +-- Response Generation
         |
         +-- Connection Cleanup
```

### 3.2 Responsibility Boundary

The Linux kernel is responsible for:

- TCP connection establishment
- TCP packet processing
- TCP retransmission
- TCP flow control
- Socket buffers
- Network device communication

The HTTP server is responsible for:

- Requesting a listening TCP socket
- Accepting connections
- Reading bytes from the socket
- Parsing HTTP requests
- Producing HTTP responses
- Writing bytes to the socket
- Closing connections
- Releasing application resources

The HTTP server must not implement the TCP protocol itself.

---

## 4. Functional Requirements

### FR-001: Server Startup

The server must:

- Start as a Linux user-space process.
- Create a TCP listening socket.
- Bind to IPv4 address `0.0.0.0`.
- Listen on TCP port `8080`.
- Continue running until explicitly terminated or a fatal error occurs.

Expected behavior:

```text
Server starts
    |
    v
Listening on 0.0.0.0:8080
    |
    v
Waiting for connections
```

The server must report startup failures, including port binding errors.

### FR-002: TCP Connection Acceptance

The server must:

- Accept incoming TCP connections.
- Process one accepted connection at a time.
- Continue accepting subsequent connections.
- Close each accepted connection after processing.

A client connection must not terminate the server process.

### FR-003: HTTP Request Reception

The server must receive HTTP request data through the TCP connection.

Requirements:

- Support HTTP/1.1 requests.
- Support requests split across multiple TCP reads.
- Read until the complete HTTP header section is received.
- Detect the end of HTTP headers using `\r\n\r\n`.
- Enforce a maximum request header size of 8192 bytes.
- Do not assume one read operation equals one HTTP request.

Requests exceeding the header-size limit must receive:

```http
HTTP/1.1 431 Request Header Fields Too Large
```

If the request is malformed, the server should return an appropriate HTTP error response when possible.

### FR-004: HTTP Request Parsing

The server must parse:

- HTTP method
- Request target
- HTTP version
- Host header

Supported request:

```http
GET / HTTP/1.1
Host: localhost:8080

```

The server must recognize a valid HTTP/1.1 request line and require a valid, non-duplicate Host header.

Only the origin-form request target `/` is supported.

Other paths must return `404 Not Found`.

Valid but unsupported HTTP methods must return `405 Method Not Allowed`.

Malformed request syntax must return `400 Bad Request`.

Unsupported HTTP versions must return `505 HTTP Version Not Supported` when the version can be identified.

Request bodies are not supported.

The server must not attempt to implement a complete HTTP/1.1 parser.

### FR-005: Request Handling

The server supports exactly one successful endpoint:

| Method | Path | Response |
|---|---|---|
| GET | / | 200 OK |

The handler must return the fixed text:

```text
Hello
```

No routing framework is required.

No database access is required.

No external services are required.

### FR-006: HTTP Response Generation

For a valid `GET /` request, the server must return:

```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 5
Connection: close

Hello
```

All HTTP header lines must use CRLF (`\r\n`).

The blank line separating headers from the body must also use CRLF.

`Content-Length` must accurately describe the response body length in bytes.

The successful response body must contain exactly five bytes:

```text
Hello
```

No trailing newline is allowed in the successful response body.

Error responses must also use valid HTTP response framing, including a correct Content-Length and `Connection: close`.

### FR-007: HTTP Error Handling

The server must handle the following cases:

| HTTP Status | Condition |
|---|---|
| 200 OK | Valid GET / |
| 400 Bad Request | Malformed HTTP request |
| 404 Not Found | Unsupported path |
| 405 Method Not Allowed | Valid but unsupported HTTP method |
| 408 Request Timeout | Request not completed before timeout |
| 431 Request Header Fields Too Large | Request headers exceed size limit |
| 505 HTTP Version Not Supported | Unsupported recognizable HTTP version |

For `405`, include:

```http
Allow: GET
```

If the client disconnects before the request is complete, the server may close the connection without an HTTP response.

If a socket error prevents sending an HTTP response, the server must release the connection resources and continue running.

### FR-008: Connection Lifecycle

Each TCP connection must process at most one HTTP request.

The lifecycle is:

```text
ACCEPTED
    |
    v
READING_REQUEST
    |
    v
PARSING_REQUEST
    |
    v
GENERATING_RESPONSE
    |
    v
WRITING_RESPONSE
    |
    v
CLOSING_CONNECTION
```

HTTP persistent connections are not supported.

Every HTTP response must include:

```http
Connection: close
```

The server must close the connection after sending the response.

After closing the connection, it must return to accepting the next connection.

### FR-009: Blocking I/O

The server must use blocking socket I/O.

Requirements:

- Only one application thread handles connections.
- No worker threads.
- No event loop.
- No non-blocking sockets.
- No asynchronous I/O framework.

The server may block while accepting, reading, or writing socket data.

Blocking operations must not result in indefinite waiting on an incomplete client request.

### FR-010: Connection Timeout

The server must enforce a maximum of 5 seconds to receive a complete HTTP request header after accepting a connection.

A client must not be able to extend this deadline indefinitely by sending individual bytes slowly.

The server must also prevent an unresponsive client from blocking response transmission indefinitely.

On request timeout:

- Return `408 Request Timeout` when feasible.
- Otherwise close the connection.
- Continue accepting new connections.

### FR-011: Partial I/O Handling

The server must handle partial reads and writes.

Requirements:

- A complete HTTP request may arrive in multiple reads.
- A write operation may send fewer bytes than requested.
- All response bytes must be sent unless a connection error or timeout occurs.
- Interrupted system calls must be handled appropriately.
- Client disconnection must not crash the server.
- A broken connection must not terminate the process because of SIGPIPE.

### FR-012: Resource Management

The server must:

- Close accepted socket descriptors.
- Release allocated memory.
- Avoid file descriptor leaks.
- Release resources when a connection fails.
- Report unrecoverable errors.

A malformed request or disconnected client must not cause the entire server process to terminate.

---

## 5. Non-Functional Requirements

### NFR-001: Simplicity

The implementation must remain minimal.

Avoid:

- Frameworks
- Plugin systems
- Generic dependency injection
- Complex module architectures
- Unnecessary design patterns

Prioritize code readability and understanding of Linux networking behavior.

### NFR-002: Portability

The server must compile and execute on Linux.

Linux-specific system calls may be used.

macOS compatibility is not required.

### NFR-003: Security Boundary

The server is intended for local development only.

Requirements:

- Docker must publish the port on host loopback only.
- Do not expose the server directly to the public Internet.
- Enforce request-size limits.
- Enforce connection timeouts.
- Prevent obvious buffer overflows.
- Handle malformed input safely.

This project does not claim production-grade HTTP security.

### NFR-004: Reliability

The server must remain operational after:

- Invalid HTTP requests
- Client disconnections
- Request timeouts
- Unsupported paths
- Unsupported methods

Fatal startup errors may terminate the process.

### NFR-005: Observability

The developer must be able to inspect:

- Server process
- Listening socket
- Established TCP connections
- Relevant Linux system calls
- Connection closure

Recommended tools:

- curl
- ss
- strace
- tcpdump
- gdb

These tools are for learning and debugging, not mandatory application dependencies.

---

## 6. Development Environment

### ENV-001: Containerized Development

The project must use a Linux container.

The container must provide:

- GCC
- Make
- Required C standard and system libraries
- Tools needed to execute the server

Debugging tools may be included in the development image.

### ENV-002: Source Code Management

Source code must be stored in a local Git repository.

The development container may access source code through a Docker bind mount.

Deleting the container must not delete the source code.

### ENV-003: Network Configuration

Inside the container:

```text
0.0.0.0:8080
```

On the macOS host:

```text
127.0.0.1:8080
```

Required Docker port publishing:

```text
127.0.0.1:8080:8080
```

The host must be able to send HTTP requests to:

```text
http://127.0.0.1:8080/
```

### ENV-004: Container Isolation

The container must not require privileged mode for normal server execution.

Do not mount sensitive host directories.

Use reasonable CPU and memory limits.

Debugging sessions may require additional permissions for specific inspection tools.

---

## 7. Acceptance Criteria

### AC-001: Successful Startup

**Given** the server container is running

**When** the server starts

**Then** it listens on TCP port 8080

**And** it accepts incoming IPv4 connections.

### AC-002: Valid HTTP Request

**Given** the server is running

**When** a client sends:

```http
GET / HTTP/1.1
Host: localhost:8080

```

**Then** the server returns HTTP status 200

**And** the response body is exactly `Hello`

**And** Content-Length is 5.

### AC-003: Connection Closure

**Given** a client has sent a valid HTTP request

**When** the server finishes sending the response

**Then** the server closes that connection

**And** continues accepting new connections.

### AC-004: Unsupported Path

**When** a client sends:

```http
GET /unknown HTTP/1.1
Host: localhost:8080

```

**Then** the server responds with HTTP 404.

### AC-005: Unsupported Method

**When** a client sends a valid POST request without a body

**Then** the server responds with HTTP 405

**And** includes `Allow: GET`.

### AC-006: Partial Request Delivery

**Given** the client sends an HTTP request in multiple TCP writes

**When** the complete header section arrives within the deadline

**Then** the server parses the request correctly

**And** returns the expected response.

### AC-007: Oversized Request Headers

**Given** the request header section exceeds 8192 bytes

**When** the server detects the excess size

**Then** the server returns HTTP 431 when feasible

**And** closes the connection.

### AC-008: Request Timeout

**Given** a client opens a TCP connection

**And** does not complete its HTTP request within 5 seconds

**Then** the server ends the connection

**And** remains available for subsequent clients.

### AC-009: Client Disconnection

**Given** a client connects to the server

**When** the client disconnects before completing its request

**Then** the server releases the connection resources

**And** continues operating.

### AC-010: Sequential Connections

**Given** multiple clients attempt to connect

**When** the server receives their requests

**Then** the application processes accepted connections sequentially

**And** can continue servicing later connections after earlier ones finish.

The operating system may queue pending connections.

The server is not required to process multiple connections concurrently.

### AC-011: Partial Response Write

**Given** sending a response requires multiple write operations

**When** a write sends fewer bytes than requested

**Then** the server continues sending the remaining bytes

**Unless** a connection error or timeout prevents completion.

### AC-012: No Resource Leak

**Given** the server processes repeated connections

**When** connections are completed or terminated

**Then** their accepted socket descriptors are released

**And** application memory allocated for those connections is released.

---

## 8. Out of Scope

The following features must not be implemented in V1:

### Network and Protocol

- HTTPS / TLS
- HTTP/2
- HTTP/3
- IPv6
- HTTP Keep-Alive
- Multiple HTTP requests per connection
- HTTP Pipelining
- Chunked Transfer Encoding
- Request Body Processing
- File Upload
- WebSocket
- Reverse Proxy
- Static File Serving
- Compression

### Concurrency

- Multithreading
- Thread Pool
- fork-per-connection
- Non-blocking I/O
- epoll
- select
- poll
- Asynchronous I/O

### Application Features

- Complex Routing
- Middleware
- Authentication
- Session Management
- Database
- JDBC
- Business Services
- Application Frameworks

### Infrastructure

- Kubernetes
- Cloud Deployment
- Load Balancer
- Distributed Systems
- Production Monitoring Stack

---

## 9. Learning Objectives

After completing V1, the developer should be able to explain:

### Linux Fundamentals

1. What is a Linux process?
2. What is User Space?
3. What is Kernel Space?
4. What is a System Call?
5. What is a File Descriptor?

### TCP Networking

6. What is a TCP socket?
7. What is the difference between a listening socket and a connected socket?
8. Who performs the TCP three-way handshake?
9. What happens when a TCP connection is accepted?
10. Where does the kernel store received TCP data?

### Blocking I/O

11. What happens when the server reads from a socket with no available data?
12. What does Blocking I/O mean?
13. Does a blocked thread continuously consume CPU?
14. How does a blocked thread become runnable again?
15. What is the relationship between a blocked thread and the Linux scheduler?

### HTTP

16. What is the relationship between HTTP and TCP?
17. Why is TCP a byte stream rather than a message protocol?
18. Why can a single HTTP request require multiple socket reads?
19. How does a server recognize the end of HTTP headers?
20. What makes an HTTP response valid?

### Connection Lifecycle

21. Who decides when an HTTP connection should close?
22. What happens when a client disconnects?
23. Why must socket file descriptors be closed?
24. What is the difference between receiving bytes and parsing an HTTP request?
25. Why can a single-threaded blocking server delay other clients?

---

## 10. Deliverables

The project is complete when the following deliverables exist:

- [ ] Compilable C HTTP Server
- [ ] Makefile
- [ ] Dockerfile
- [ ] Docker run instructions
- [ ] README.md
- [ ] spec.md
- [ ] Automated tests for required acceptance criteria
- [ ] Successful HTTP request demonstration
- [ ] Linux socket inspection results
- [ ] System call investigation notes

---

## 11. Definition of Done

V1 is complete only when:

1. The program compiles successfully on Linux.
2. The server runs inside a Docker container.
3. The host can connect through `127.0.0.1:8080`.
4. A valid `GET /` returns the specified HTTP response.
5. The server correctly handles partial request reads and response writes.
6. The server closes each connection after one response.
7. Invalid requests do not crash the server.
8. Request and response operations have bounded waiting times.
9. Repeated connections do not cause socket descriptor leaks.
10. All mandatory acceptance tests pass.
11. The developer can explain the Linux socket lifecycle without relying on application frameworks.

---

## 12. Future Extensions

The following are potential future projects and are not part of V1.

### V2: Thread-per-connection Server

Research:

- POSIX Threads
- Linux Thread Scheduling
- Concurrency
- Context Switching

### V3: Thread Pool Server

Research:

- Worker Threads
- Task Queue
- Mutex
- Condition Variable
- Thread Pool Saturation

### V4: epoll-based Server

Research:

- Non-blocking I/O
- I/O Multiplexing
- epoll
- Event Loop
- Connection State Machine

### V5: Java NIO Comparison

Research:

- ServerSocketChannel
- SocketChannel
- Selector
- Tomcat Acceptor
- Tomcat Poller
- Worker Thread

These extensions must remain independent of the V1 completion criteria.
