# ChatRoom-CPP

A step-by-step C++ systems project for learning computer networks and operating systems by building a chat server from scratch.

## Current Status
Step 1 is complete:
- blocking single-client TCP server
- simple terminal client
- CMake build setup
- project context file for future chats
- code has been rewritten in a slower, teaching-first style

## Project Goal
The long-term goal is to build a scalable multi-client chat server in C++ on Linux using:
- TCP sockets
- non-blocking I/O
- `epoll`
- thread pool
- room-based messaging
- logging, metrics, and robust connection handling

We are starting with the simplest possible server first because high-performance systems are much easier to understand when you first understand the basic socket lifecycle and the exact point where the naive design breaks.

## Learning Workflow For This Repository
We are following a strict incremental style:
- create one file at a time
- add a small amount of code
- explain what that code does
- then add the next small amount

This means the code may look more verbose than production code.
That is intentional.

The main goal right now is not speed of implementation.
The goal is understanding:
- what the operating system is doing
- what each system call does
- what data moves between user space and kernel space
- why each later optimization becomes necessary

## How A Basic Chat Server Works Internally
At the operating system level, a chat server is a program that moves bytes between:
- the network interface and kernel buffers
- kernel buffers and your application's memory

On Linux and Unix-like systems, network sockets are represented as file descriptors. That means the kernel gives your program a small integer handle for each open socket, and your program uses that handle in system calls.

### Basic server lifecycle
1. `socket()`
Creates a socket endpoint and returns a file descriptor.

What the OS does:
- allocates kernel-side state for the socket
- prepares send and receive buffering metadata
- returns a file descriptor such as `3`, `4`, or `5`

At this point, the socket exists but is not yet attached to an IP address or port.

2. `bind()`
Associates the socket with a local IP address and port.

Why this matters:
- the kernel needs to know which process should receive packets sent to a given port
- if your server binds to port `5555`, incoming traffic for that port can be routed to this socket

3. `listen()`
Marks the socket as a passive listening socket.

What changes here:
- the socket stops being treated like an active endpoint for outgoing connections
- the kernel starts queueing incoming connection requests for it

4. `accept()`
Removes one completed connection from the kernel's pending connection queue and returns a brand new file descriptor for that client.

This is one of the most important concepts:
- the original listening socket stays open and keeps waiting for future clients
- each connected client gets its own separate file descriptor

So after one client connects, the server typically has:
- one listening socket FD
- one connected client socket FD

5. `recv()` and `send()`
These move bytes between user space and kernel space.

`recv()`:
- copies bytes from the kernel's receive buffer into your program's memory

`send()`:
- copies bytes from your program's memory into the kernel's send buffer so the network stack can transmit them

6. `close()`
Releases the file descriptor and tells the kernel that your process is done with that socket.

## Important Terms Used In This Project
### File descriptor
A file descriptor is a small integer used by the OS to represent an open resource.

Examples:
- a regular file
- a socket
- a pipe

Why this matters:
- the same Unix design idea lets programs interact with many resources using a similar model

### Kernel space vs user space
User space is where your application code runs.
Kernel space is where the operating system runs and manages hardware, memory, files, and networking.

Networking system calls such as `recv()` and `send()` cross this boundary.

### Socket
A socket is an OS-managed communication endpoint.

For this project we are using:
- IPv4 sockets with `AF_INET`
- TCP stream sockets with `SOCK_STREAM`

### TCP
TCP is a reliable, connection-oriented transport protocol.

Properties that matter for us:
- data arrives in order
- lost packets are retransmitted
- it behaves like a byte stream, not a message queue

That last point is very important:
- one `send()` does not guarantee one matching `recv()`
- later we will need message framing so the application can separate messages correctly

### Port
A port is a logical communication endpoint on a machine.

Why it exists:
- many programs can use the network at the same time
- the IP address identifies the machine
- the port identifies the service on that machine

### Backlog queue
The backlog is the queue of pending incoming connections managed by the kernel for a listening socket.

Why it matters:
- if many clients try to connect at once, the kernel can queue some of them until your application calls `accept()`
- if the queue fills up, additional connection attempts may be delayed or rejected

### Blocking I/O
A blocking system call puts the calling thread to sleep until the operation can make progress.

Examples:
- `accept()` blocks until a client arrives
- `recv()` blocks until data arrives or the connection closes

This is simple to reason about, which is why we start with it.

### Non-blocking I/O
In non-blocking mode, a socket operation returns immediately instead of sleeping.

If no data is ready:
- `recv()` usually returns `-1`
- `errno` is set to `EAGAIN` or `EWOULDBLOCK`

This is the foundation required for `epoll`.

### Context switch
A context switch happens when the OS pauses one thread and runs another.

The kernel must save and restore thread state such as:
- registers
- instruction pointer
- scheduling state

Context switches are necessary, but too many of them waste CPU time.

### Thread stack
Each thread gets its own stack memory for function calls, local variables, and control flow.

Why it matters:
- thousands of threads can consume huge amounts of memory even if they are mostly idle

### I/O multiplexing
I/O multiplexing means one thread can monitor many file descriptors and respond only to those that are ready.

`epoll` is Linux's high-performance I/O multiplexing mechanism.

## Why A Basic Single-Threaded Server Fails
If we write a server with one thread and blocking sockets, the design stops scaling almost immediately.

Example:
- the server calls `recv()` on Client A
- Client A sends nothing
- the thread sleeps inside `recv()`

While that thread is sleeping:
- it cannot call `accept()` for new clients
- it cannot read from Client B
- it cannot send responses to Client C

So the entire server becomes unresponsive even though only one client is idle.

## The Historical Workaround: One Thread Per Client
One traditional way to avoid that blocking problem is:
- main thread accepts new clients
- each client gets its own thread
- each thread blocks on `recv()` for its own client

This works for small systems, but it does not scale well.

### Why it fails at scale
1. Memory overhead
Each thread has its own stack and scheduler metadata.

If you create thousands of threads:
- memory usage grows quickly
- much of that memory may be wasted on idle connections

2. Context switching cost
The CPU cannot run all threads at once.

So the OS must constantly:
- pause one thread
- save its state
- load another thread's state
- resume execution

At high thread counts, a lot of CPU time is spent managing threads instead of processing useful work.

This family of scaling issues is often discussed under the broader "C10k problem":
- how to handle ten thousand concurrent clients efficiently

## Why Non-Blocking Sockets Alone Are Not Enough
Suppose we switch all sockets to non-blocking mode.

Now `recv()` does not sleep. That sounds better, but it creates a new problem.

If you have 10,000 sockets and keep looping over all of them asking:
- "do you have data?"
- "do you have data now?"
- "what about now?"

then your program wastes CPU scanning mostly idle sockets.

That style is called busy polling, and it becomes inefficient very quickly.

## How `epoll` Solves The Problem
`epoll` is Linux's event notification mechanism for large numbers of file descriptors.

It changes the architecture from:
- application repeatedly checking every socket

to:
- kernel notifying the application only about sockets that are ready

### Core `epoll` workflow
1. `epoll_create1()`
Creates an `epoll` instance and returns an FD representing it.

Internally, the kernel creates data structures to track:
- which FDs you are interested in
- which of them are currently ready

2. `epoll_ctl()`
Registers, modifies, or removes a file descriptor from the `epoll` instance.

Example:
- register a client socket
- ask to be notified when it becomes readable with `EPOLLIN`
- also watch for hang-up or error conditions

3. `epoll_wait()`
Puts the thread to sleep until one or more registered FDs become ready.

When that happens, the kernel wakes your thread and returns only the active file descriptors.

This is the key efficiency win:
- idle sockets cost almost nothing in the event loop
- the application handles work only when the kernel says progress is possible

### Important event flags
- `EPOLLIN`: data is available to read
- `EPOLLOUT`: socket is ready to accept more outgoing data
- `EPOLLHUP`: the peer hung up
- `EPOLLERR`: an error occurred on the FD

## Why `epoll` Is Better Than `select()` Or `poll()`
`epoll` is usually preferred on Linux for large-scale socket servers because:
- it scales better with many FDs
- you do not have to rescan every FD on every loop
- the kernel returns only the FDs that are actually ready
- it avoids some of the fixed-size limitations associated with `select()`

We will study the exact differences later when we reach the `epoll` step.

## Why We Still Need A Thread Pool Even With `epoll`
`epoll` tells us which sockets are ready for I/O, but it does not perform application work for us.

The server still has to do things like:
- parse commands
- update room membership
- route messages
- write logs
- enforce rate limits

If we do too much work directly inside the event loop:
- the loop becomes slow
- other ready clients wait longer
- overall responsiveness drops

That is why the long-term architecture combines:
- `epoll` for efficient readiness notification
- a thread pool for bounded parallel processing

This is a strong design because:
- one thread does not block on idle clients
- we avoid creating one thread per client
- a fixed number of worker threads keeps resource usage controlled

## Why The Project Starts Simple
Before building the scalable design, we first need to understand the basic TCP socket lifecycle:
- `socket`
- `bind`
- `listen`
- `accept`
- `recv`
- `send`
- `close`

That foundation will make the later `epoll` design much easier to understand.

## Build
```bash
cmake -S . -B build
cmake --build build
```

## Docker For Linux Versions
If you are on macOS and want to run the Linux `epoll` versions, use Docker.

Why:
- `epoll` is Linux-only
- Docker gives this project a Linux runtime and build environment
- you can keep editing on macOS while compiling/running inside Linux

### Build the Docker image
```bash
docker compose build
```

### Start an interactive Linux container
```bash
docker compose run --service-ports --rm chat-dev
```

Inside the container, compile the project with:
```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic src/main.cpp -o chat_server
g++ -std=c++17 -Wall -Wextra -Wpedantic client/simple_client.cpp -o simple_client
```

Then run the server inside the container:
```bash
./chat_server
```

From another macOS terminal in the same project folder, you can either:
- enter another container shell and run `./simple_client`, or
- use the same `docker compose run --service-ports --rm chat-dev` command and run the client there

Example client flow in a second terminal:
```bash
docker compose run --service-ports --rm chat-dev
./simple_client
```

### Why port `5555` is exposed
- the server listens on port `5555`
- `docker-compose.yml` maps container port `5555` to macOS host port `5555`
- this makes local testing straightforward

## Run Step 1
In terminal 1:
```bash
./build/chat_server
```

In terminal 2:
```bash
./build/simple_client
```

## Files
- `PROJECT_CONTEXT.md`: persistent context and progress tracker
- `LEARNING_JOURNAL.md`: learning log for concepts, outcomes, failures, and fixes
- `src/main.cpp`: Step 1 learning-focused TCP server
- `client/simple_client.cpp`: tiny client for local testing

## Next Step
Step 2 will support multiple clients in a naive design so we can see why a blocking or thread-per-client model does not scale well.
