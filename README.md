# Quark
An ultra-lite C web framework
# Building a Lightweight Web Framework in C

A development roadmap and feature guide. The goal is to build understanding from the socket layer up, making the hard architectural decisions early and letting the ergonomics evolve.

---

## Foundational Decisions (make these first)

Three choices shape every API you write and are painful to retrofit. Decide them before Phase 3.

### 1. Memory model
Since the framework is "lightweight," memory strategy is the single most important design decision in C.

- **Per-request arena/pool allocator (recommended).** Allocate one block when a request arrives; carve all per-request allocations (parsed headers, params, response buffers) out of it; free the whole arena at once when the response is sent. This eliminates most leaks and fragmentation and makes cleanup a single call.
- **Manual `malloc`/`free`.** More flexible, but every code path becomes a place to leak or double-free.

A minimal arena looks like:

```c
typedef struct {
    char   *base;      // start of the block
    size_t  size;      // total capacity
    size_t  offset;    // bump pointer
} arena_t;

void *arena_alloc(arena_t *a, size_t n); // bump offset, return pointer
void  arena_reset(arena_t *a);           // set offset = 0, reuse block
```

### 2. Concurrency model
- **Thread-per-connection with a pool** — simplest to reason about, fine for moderate load.
- **Event loop (epoll/kqueue)** — scales to many idle connections, more complex control flow.
- **Hybrid (event loop + worker threads)** — what production servers do; add only if you need it.

Start simple (Phase 1 is single-threaded), introduce a thread pool in Phase 4, and only move to an event loop in Phase 6 if performance demands it.

### 3. Handler API signature
This is your public API and the hardest thing to change once users depend on it. A common shape:

```c
void handler(request_t *req, response_t *res);
```

Keep `request_t` and `response_t` opaque-ish (accessed via helper functions) so you can change their internals later without breaking handlers.

---

## Core Feature Landscape

| Area | Features |
|------|----------|
| HTTP handling | Request parsing (request line, headers, body), response building, `Content-Length`, chunked transfer encoding, keep-alive |
| Connection layer | Blocking sockets → non-blocking + event loop |
| Routing | Static paths, dynamic segments (`/users/:id`), wildcards, method dispatch |
| Middleware | Composable before/after chain (logging, auth, CORS) via function pointers |
| Ergonomics | Query/header parsing, path params, body access, status helpers, JSON responses, static file serving |
| Concurrency | Thread pool, event loop, or hybrid |
| Supporting | Config, structured logging, graceful shutdown, test harness |

---

## Phase 1 — Blocking, Single-Threaded Server

**Goal:** Understand the socket lifecycle end to end. No parsing yet.

**Build:**
- Create a TCP socket, `bind`, `listen`, `accept` a single connection.
- `read` the raw bytes into a buffer and print them.
- Write back a hardcoded, well-formed HTTP/1.1 response.
- Loop to accept the next connection after closing the current one.

**Key details:**
- Set `SO_REUSEADDR` so you can restart the server without waiting for the socket to time out.
- A minimal valid response is a status line, a `Content-Length` header, a blank line, then the body.

**Milestone:** `curl localhost:8080` returns your response.

**Pitfalls:** Forgetting the blank line (`\r\n\r\n`) between headers and body; forgetting `Content-Length` (clients hang waiting for more data).

---

## Phase 2 — HTTP Parsing

**Goal:** Turn raw bytes into structured data and build correct responses. Most subtle bugs live here.

**Build:**
- Parse the request line into method, path, and version.
- Parse headers into a key/value structure (case-insensitive keys).
- Read the body using `Content-Length` (and later, chunked encoding).
- A response builder that sets the status line, headers, and body correctly.

**Suggested structs:**

```c
typedef struct { char *name, *value; } header_t;

typedef struct {
    char     *method, *path, *version;
    header_t *headers;
    size_t    header_count;
    char     *body;
    size_t    body_len;
} request_t;

typedef struct {
    int       status;
    header_t *headers;
    size_t    header_count;
    char     *body;
    size_t    body_len;
} response_t;
```

**Key details:**
- HTTP headers are case-insensitive — normalize on lookup.
- A single `read()` may not return the whole request; loop until you have the full headers, then the full body.
- Guard against malformed input from the start: missing headers, no request line, bad `Content-Length`.

**Testing:** Write unit tests against RFC edge cases — missing headers, malformed request lines, bodies shorter/longer than `Content-Length`, header lines with no colon.

**Milestone:** You can inspect a parsed request struct and reflect its fields back in the response.

---

## Phase 3 — Routing and Handlers

**Goal:** Map `(method, path)` to a handler function.

**Build:**
- A route table mapping method + path pattern to a handler.
- Static route matching first (`/health`, `/users`).
- Then dynamic segments (`/users/:id`) that populate path params on the request.
- A 404 fallback and a 405 (method not allowed) response.

**Suggested structs:**

```c
typedef struct {
    const char *method;
    const char *pattern;   // "/users/:id"
    void (*handler)(request_t *, response_t *);
} route_t;
```

**Key details:**
- Decide how params are exposed, e.g. `req_param(req, "id")`.
- For dynamic matching, split the pattern and path on `/` and compare segment by segment; a segment starting with `:` binds a param.
- Keep matching order predictable (first match wins, or most-specific wins — pick one and document it).

**Milestone:** `GET /users/42` routes to a handler that can read `id = "42"`.

---

## Phase 4 — Concurrency

**Goal:** Stop slow requests from blocking every other client.

**Build:**
- A fixed-size thread pool.
- A synchronized job queue: the accept loop pushes accepted client sockets; worker threads pop and handle them.
- Per-request arena allocation so each worker has isolated memory.

**Key details:**
- Protect the queue with a mutex + condition variable.
- Ensure each connection's state is thread-local or heap-allocated per request — never share request buffers across threads.
- Handle the case where the queue is full (block, or drop with a 503).

**Testing:** From here on, run **everything** under Valgrind and AddressSanitizer (ASan). Concurrency exposes use-after-free, data races, and lifetime bugs that single-threaded testing hides. Add ThreadSanitizer (TSan) for race detection.

**Milestone:** A handler with an artificial `sleep()` no longer stalls concurrent requests.

---

## Phase 5 — Middleware and Ergonomics

**Goal:** Make the framework pleasant to actually build on.

**Build:**
- A middleware chain — an array or linked list of function pointers, each receiving the request, response, and a `next()` to pass control along. Enables logging, auth, CORS, compression.
- Query string parsing (`?page=2&sort=name`).
- Body parsing helpers (form-encoded, JSON).
- Response helpers: `res_json()`, `res_status()`, `res_header()`, `res_send_file()`.
- Static file serving with correct MIME types.

**Middleware shape:**

```c
typedef void (*middleware_fn)(request_t *, response_t *, next_fn next);
```

**Key details:**
- Decide whether middleware can short-circuit (e.g. auth returns 401 without calling `next`).
- For static files, guard against path traversal (`../../etc/passwd`) — normalize and confine paths to a root directory.

**Milestone:** You can write a real handler that reads JSON, checks an auth header via middleware, and returns a JSON response — with little boilerplate.

---

## Phase 6 — Performance

**Goal:** Scale up and measure.

**Build:**
- Replace thread-per-connection with an **event loop** using `epoll` (Linux) or `kqueue` (BSD/macOS).
- Non-blocking sockets; track per-connection state in a state machine (reading headers → reading body → writing response).
- Optionally combine the event loop with a worker pool for CPU-bound handlers (hybrid model).

**Key details:**
- `sendfile()` (Linux) serves static files without copying data through userspace.
- Tune read/write buffer sizes and the allocator based on profiling, not guesswork.

**Testing / benchmarking:**
- Load test with `wrk` or `bombardier`.
- Profile with `perf` to find hotspots.
- Compare requests/sec and latency percentiles before and after each change.

**Milestone:** Measurable throughput improvement over the Phase 4 thread pool under high connection counts.

---

## Phase 7 — Hardening

**Goal:** Make it safe to expose to untrusted input.

**Build:**
- Request timeouts (header, body, and idle) to prevent hung connections.
- Request size limits (max header size, max body size) to prevent memory exhaustion.
- Slowloris protection — drop connections that trickle bytes slowly.
- Graceful shutdown on `SIGTERM`/`SIGINT`: stop accepting, drain in-flight requests, then exit.
- TLS by integrating **OpenSSL** (or LibreSSL/mbedTLS) — never roll your own crypto.

**Testing:**
- Fuzz the parser with malformed input (AFL++ or libFuzzer). The HTTP parser is the highest-value fuzz target.
- Verify limits actually reject oversized/slow requests.

**Milestone:** The server survives a fuzzing run and hostile clients without crashing or leaking.

---

## Tooling Checklist

Set these up early; they pay for themselves quickly in a language with no safety net.

- **Build:** a `Makefile` or CMake with `-Wall -Wextra -Werror`.
- **Sanitizers:** ASan (memory), UBSan (undefined behavior), TSan (races) in debug builds.
- **Valgrind:** for leak and memory-error detection.
- **Test harness:** a minimal assertion framework (or a lightweight one like Unity/greatest). Test the parser and router in isolation.
- **Load testing:** `wrk`, `bombardier`.
- **Fuzzing:** AFL++ or libFuzzer against the parser.
- **CI:** run the test suite under sanitizers on every commit.

---

## Summary Ordering

1. **Phase 1** — Blocking single-threaded socket server.
2. **Phase 2** — HTTP request parsing + response building.
3. **Phase 3** — Routing and handler dispatch.
4. **Phase 4** — Thread pool concurrency (start using sanitizers).
5. **Phase 5** — Middleware chain + ergonomics.
6. **Phase 6** — Event loop + performance tuning.
7. **Phase 7** — Timeouts, limits, TLS, fuzzing.

Lock in the **memory model**, **concurrency model**, and **handler signature** up front. Everything else can evolve as you go.
