# ChatRoom-CPP Project Context

## Goal
Build a resume-worthy multi-client chat server in C++ on Linux while learning core computer networks and operating systems concepts deeply.

The target end-state is:
- TCP chat server
- many clients
- non-blocking sockets
- `epoll`-based event loop
- thread pool for business logic
- rooms, private messaging, authentication, protected channels, logging, metrics, and robustness features

## Learning Style For This Project
This repository is being built step by step from scratch.

Important expectations for future work:
- explain concepts before and during code changes
- prefer simple versions first, then evolve them
- keep code heavily commented for learning
- document what each tool/API does and why we chose it
- update this file as progress changes

## Current Step
Version 12: Linux `epoll` + thread-pool server with authentication, protected rooms, and owner-only room controls.

### Why this step exists
We are now moving from basic access control into owner-based authorization behavior.

Reason:
- first learn the lifecycle of a TCP server
- then learn how threads affect concurrency
- then learn how non-blocking sockets change syscall behavior
- then replace inefficient polling with kernel-driven readiness notification
- then keep expensive processing out of the event-loop thread
- then add application-layer protocol behavior
- now add room membership, room-scoped routing, direct private messaging, safer TCP command framing, authenticated/protected resources, and owner-only room controls

## Current Architecture
Linux hybrid architecture using `epoll` + thread pool + authenticated line-framed chat protocol:

1. Server creates a TCP socket.
2. Server binds it to `0.0.0.0:5555`.
3. Server switches the listening socket to non-blocking mode.
4. Server creates an `epoll` instance.
5. Server registers the listening socket with `epoll`.
6. Server creates a fixed-size thread pool.
7. Server enters `epoll_wait()`.
8. When the listening socket is ready, server accepts new clients.
9. Each new client socket is made non-blocking and registered with `epoll`.
10. When a client socket is ready, the event loop enqueues a task.
11. Each client session keeps an input buffer so TCP bytes can be accumulated until newline-terminated commands are complete.
12. A worker thread extracts full commands such as `REGISTER`, `LOGIN`, `CREATE_ROOM`, `JOIN_ROOM`, `ROOM_USERS`, `SET_ROOM_PASS`, `DELETE_ROOM`, `MSG`, `PM`, `USERS`, `ROOMS`, and `QUIT`.
13. Shared chat state tracks registered accounts, authenticated usernames, room metadata, ownership, current room membership, and per-client buffered input state.
14. Worker threads authenticate users, enforce room password checks, enforce owner-only room management, and send direct replies, room broadcasts, or private messages.

This version is intentionally still simple in persistence and output handling, but authentication, room protection, and owner-only controls are now part of the protocol.

## Tools / APIs Used So Far

### CMake
Used to build the project cleanly and prepare for a larger multi-file codebase.

Why CMake:
- standard for C++ projects
- easy to scale as files/modules grow
- useful in interviews and on GitHub

Why not plain `g++` commands only:
- becomes messy as source files increase
- harder to reproduce builds
- weaker project structure

### POSIX socket APIs
Used to build the server and client.

APIs currently used:
- `socket`
- `bind`
- `listen`
- `accept`
- `connect`
- `recv`
- `send`
- `close`
- `inet_pton`
- `htons`

Why POSIX sockets:
- core Linux/Unix networking interface
- gives direct understanding of how TCP servers work
- required foundation before higher-level abstractions

Why not Boost.Asio or some higher-level library:
- those are useful later, but they hide the low-level concepts
- the learning goal here is operating systems and computer networks fundamentals

### Docker
Used to run Linux-specific versions of the project on macOS.

Why Docker:
- later versions depend on Linux `epoll`
- macOS does not provide `epoll`
- Docker gives us a Linux build and runtime environment without changing the project architecture

Why not switch the project immediately to macOS `kqueue`:
- the current project goal is explicitly Linux-oriented
- Docker lets us keep that goal intact

## Concepts Learned So Far

### File descriptor
A small integer returned by the OS that represents an open resource such as:
- a socket
- a file
- a pipe

Sockets are treated like file descriptors in Unix-like systems.

### TCP socket
A TCP socket is an endpoint for reliable byte-stream communication between two processes over a network.

Important property:
- TCP is a **stream**, not a message queue
- later this will matter because one `recv` does not guarantee one complete application message

### `sockaddr_in`
This structure stores an IPv4 socket address:
- IP address
- port
- address family

### `htons`
Converts a port number from host byte order to network byte order.

Why needed:
- different CPU architectures store integers differently
- network protocols standardize on big-endian order

### Blocking I/O
Current server uses blocking system calls.

Meaning:
- if `accept` waits for a client, the thread sleeps there
- if `recv` waits for data, the thread sleeps there

Why this is okay now:
- simplest model for learning

Why this will become a problem later:
- one blocked thread cannot handle many clients efficiently

## Progress Log

### Completed
- created CMake build setup
- created Step 1 blocking TCP server
- created simple TCP client for local testing
- documented project direction and learning goals
- added detailed comments to code for study
- upgraded the server to stay alive and accept clients repeatedly
- upgraded the server to one-thread-per-client behavior
- added a non-blocking socket experiment to study `EAGAIN` / `EWOULDBLOCK`
- added a Linux `epoll` event-loop server
- added a Linux `epoll` + thread-pool server
- added a basic command protocol with login, user listing, broadcast messaging, and quit support
- added room membership and room-scoped broadcast messaging
- added direct private messaging between logged-in users
- added buffered line-based command framing per client session
- added user registration, password-checked login, and password-protected rooms
- added `ROOM_USERS` for current-room member listing and improved the terminal client prompt/output layout
- added owner-only `SET_ROOM_PASS` and `DELETE_ROOM` controls

### Pending Near-Term
- run and study the Version 12 protocol on Linux/Docker
- verify owner-only room management, protected room join, room broadcast, and private message flow
- use Docker on macOS to run Linux-specific versions locally
- prepare the next step toward persistence for accounts and room metadata
- define a cleaner source/include layout
- improve README with commands and architecture evolution

## Remaining Version Roadmap
These are the major planned versions still left after Version 12.

### Version 13
- persistence for accounts and room metadata
- save important state to file
- reload that state on restart

### Version 14
- stronger output buffering and partial-write handling
- move beyond the current simple `send()` approach

### Version 15
- richer admin controls
- examples: `KICK`, `MUTE`, maybe moderator roles

### Version 16
- operational safeguards
- rate limiting
- failed-attempt handling
- idle timeout cleanup

### Version 17
- logging and metrics
- counters, event logs, and observability hooks

### Version 18
- benchmarking and analysis
- throughput, latency, CPU, memory, and architecture comparison

Stopping point guidance:
- Version 15 or Version 16 is already a strong advanced project
- Version 17 or Version 18 pushes the project toward a stronger M.Tech-level systems story

### Pending Long-Term
- non-blocking sockets
- `epoll`
- thread pool
- command protocol
- rooms and private messaging
- authentication and password-protected rooms
- owner/admin controls
- persistence
- output buffering
- logging and metrics
- rate limiting
- idle timeout
- stress testing

## How To Continue In Future Chats
If a future chat starts fresh:
- read this file first
- read `LEARNING_JOURNAL.md`
- check `README.md`
- inspect `src/main.cpp` and `client/simple_client.cpp`
- continue from the next unfinished milestone

## Notes For Future Refactors
As the project grows, we should move toward:
- `include/` for headers
- `src/` for implementation
- `client/` for terminal client
- `tests/` for load/stress tests
- `docs/` for architecture notes if needed
