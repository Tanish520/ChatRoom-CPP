# ChatRoom-CPP Learning Journal

This file is the long-term study notebook for the project.

Its purpose is not just to say "what changed".
Its purpose is to help future-you understand:
- what each version of the server did
- what problem existed in the previous version
- how the new version tried to solve that problem
- what limitations still remained
- what OS / networking concepts were learned
- why the next version became necessary

If you open this file months later, you should be able to understand the evolution of the project without re-reading the entire chat history.

## How This File Will Be Maintained
From now on, every substantial version should contain:
- version name
- what this version does
- what problem existed before this version
- how this version solves that problem
- limitations of this version
- what we learned from this version
- why the next version is needed

## Project Snapshot
- Project: `ChatRoom-CPP`
- Current version: Version 12
- Current implementation: Linux `epoll` + thread-pool server with authentication, protected rooms, and owner-only room controls
- Current development platform: macOS
- Long-term target platform for `epoll`: Linux

## Big Picture Roadmap
This project is being built in layers.

The overall progression is:
1. understand the basic TCP socket lifecycle
2. make the server stay alive
3. improve concurrency with threads
4. understand non-blocking sockets
5. move to event-driven I/O with `epoll`
6. combine event-driven I/O with a thread pool
7. add a basic command protocol and shared chat semantics
8. add room-based routing
9. add direct user-to-user private messaging
10. add buffered command framing over TCP
11. add user authentication and password-protected rooms
12. add owner-only room controls
13. later add persistence, stronger output buffering, robustness, and evaluation

That means every version is intentionally incomplete.
Each version exists mainly to teach one important systems concept at a time.

## Version Timeline

### Version 1
Name: Single-Client Blocking Echo

What this version does:
- the server creates a TCP listening socket
- it binds to a port
- it starts listening
- it accepts one client
- it reads one message from that client
- it sends one reply
- then both the client connection and the server process exit

In simple words:
- one client connects
- one message goes in
- one reply comes out
- server stops

Why we built this version:
- this is the smallest useful TCP server/client pair
- before learning concurrency or scalability, we needed to understand the basic socket lifecycle
- without this version, later ideas like `epoll`, non-blocking mode, or thread pools would feel abstract

What problem existed before this version:
- there was no working server at all
- we first needed a concrete foundation

How this version solves that problem:
- it gives us a real end-to-end TCP example
- it lets us observe the exact order of system calls:
  - `socket()`
  - `bind()`
  - `listen()`
  - `accept()`
  - `recv()`
  - `send()`
  - `close()`

Key OS / networking ideas learned from this version:

1. File descriptor
- the OS represents sockets using file descriptors
- a file descriptor is a small integer handle to an OS-managed resource

2. Socket lifecycle
- `socket()` creates the communication endpoint
- `bind()` gives it a local address and port
- `listen()` marks it as a passive listening socket
- `accept()` creates a new connected socket for a specific client

3. Listening socket vs connected socket
- the listening socket is for receiving new connection requests
- the connected client socket is for reading and writing actual data

4. Blocking I/O
- `accept()` blocks until a client arrives
- `recv()` blocks until data arrives or the peer closes the connection

5. TCP stream behavior
- TCP is a stream, not a message queue
- our code treated one `recv()` as one full message only for learning simplicity

Why this version is limited:
- the server stops after serving one client
- it is not persistent
- it cannot behave like a real server process

Main limitation in one sentence:
- server lifetime and client lifetime are incorrectly tied together

What we learned from this version:
- how a TCP server is born
- how the kernel and application cooperate during connection setup
- why the listening socket and connected socket must be treated differently
- what blocking means in practice

Why the next version was needed:
- a real server should stay alive even after one client disconnects
- we needed to separate "the server process is alive" from "this particular client session is alive"

Summary of Version 1:
- Version 1 taught the TCP socket lifecycle
- it was correct, but too short-lived to behave like a real server

### Version 2
Name: Persistent Sequential Blocking Server

What this version does:
- the server no longer exits after one client
- it keeps the listening socket open
- after one client is handled, it goes back and waits for another client
- it can serve multiple clients over time
- but it still serves them one at a time

In simple words:
- the server stays alive forever
- but it works sequentially

What problem existed in Version 1:
- the server died after the first client
- that made it impossible to study long-lived server behavior

How Version 2 solves that problem:
- it wraps the client-handling path in an infinite loop
- it closes only the per-client socket after each client finishes
- it keeps the listening socket alive for future clients

Important architecture change from Version 1:
- Version 1:
  - one client handled
  - then server exits
- Version 2:
  - one client handled
  - client socket closes
  - server returns to `accept()`

What this version teaches:

1. Server lifetime vs client lifetime
- the listening socket should remain open for the full life of the server
- each connected client socket has a shorter lifetime

2. Sequential server behavior
- the server can now handle many clients over time
- but only one client interaction is processed at a time

3. Blocking limitation becomes visible
- after a client is accepted, the server calls blocking `recv()`
- if that client does not send data, the whole server thread becomes stuck there

Important experiment we performed:
- Client 1 connected and stayed silent
- the server blocked on `recv()` for Client 1
- Client 2 connected and sent a message
- the server did not respond immediately
- only after Client 1 finally sent data did the server continue
- later the server processed Client 2

What this experiment taught:
- the server was single-threaded
- that single thread handled everything:
  - `accept()`
  - `recv()`
  - `send()`
  - cleanup
- once blocked on one client, it could not actively process another client

Important precision:
It is better to say:
- "the single-threaded blocking server cannot process multiple clients concurrently"

than to say:
- "the server cannot receive more clients at all"

Why this precision matters:
- the kernel may still queue incoming connections because the listening socket is active
- the kernel may even buffer data
- but the application thread cannot actively serve multiple clients at once

Why this version is limited:
- one slow client can stall the server thread
- responsiveness is poor when more than one client is active
- concurrency is still missing

Root cause of the limitation:
- one thread
- blocking I/O
- same thread handles both accepting clients and talking to them

What we learned from this version:
- how a real server remains alive across many client sessions
- why sequential blocking design is simple but weak
- how kernel-side queuing and application-side progress are different ideas
- why “persistent” is not the same as “concurrent”

Why the next version was needed:
- we wanted multiple clients to be processed at the same time
- the most natural next step was one thread per client

Summary of Version 2:
- Version 2 fixed the “server exits too early” problem
- but exposed the “single blocking thread” problem very clearly

### Version 3
Name: One-Thread-Per-Client Blocking Server

What this version does:
- the main thread keeps running the `accept()` loop
- every accepted client gets its own worker thread
- that worker thread performs blocking `recv()`, `send()`, and cleanup for that client
- the main thread immediately goes back to waiting for more connections

In simple words:
- one client no longer occupies the entire server
- each client gets its own execution path

What problem existed in Version 2:
- one silent or slow client blocked the only server thread
- that prevented timely handling of other clients

How Version 3 solves that problem:
- it separates responsibilities:
  - main thread accepts clients
  - worker threads communicate with clients
- if one worker thread blocks on `recv()`, only that client thread is blocked
- the main thread can still accept more clients
- other client threads can still run

Important architecture change from Version 2:
- Version 2:
  - one thread does everything
- Version 3:
  - main thread accepts
  - worker thread handles a client

What this version teaches:

1. What a thread is
- a thread is an execution path within a process
- threads share process resources like heap memory and file descriptors
- but each thread has its own stack and scheduling state

2. Why concurrency improves
- Client 1 can be blocked in one thread
- Client 2 can still be served by another thread
- the main thread is free to keep accepting more connections

3. Blocking is still present, but isolated
- Version 3 did not remove blocking I/O
- it made blocking local to one client thread instead of the whole server

Useful mental model:
- Version 2: one worker handles the whole shop
- Version 3: one receptionist admits customers, separate workers handle each customer

Expected experiment for Version 3:
- keep Client 1 silent
- send from Client 2
- server should still respond to Client 2 without waiting for Client 1

Why this version is limited:
- one OS thread is created for every connected client
- memory usage grows with the number of clients
- the scheduler must context-switch between many threads
- shutdown and lifecycle management become harder, especially with detached threads

Root cause of the limitation:
- thread count grows linearly with client count
- each thread has stack memory and scheduling overhead
- concurrency is bought by creating more OS threads

What we learned from this version:
- threads improve concurrency
- blocking I/O is not always fatal if it is isolated per client
- thread-per-client is intuitive and easy to explain
- but it is not the scalable answer for very large numbers of clients

Why the next version was needed:
- we wanted concurrency without one thread per client
- that required understanding non-blocking sockets
- before event loops and `epoll`, we had to learn what happens when a socket no longer sleeps the thread

Summary of Version 3:
- Version 3 fixed the “one slow client blocks everyone” problem
- but replaced it with the “too many threads become expensive” problem

### Version 4
Name: Non-Blocking Socket Busy-Poll Experiment

What this version does:
- the accepted client socket is switched into non-blocking mode
- `recv()` is called repeatedly in a loop
- if no data is available, `recv()` returns immediately
- the program checks for `EAGAIN` / `EWOULDBLOCK`
- once data arrives, the message is read and a reply is sent

In simple words:
- the thread no longer sleeps inside `recv()`
- instead, the program keeps asking whether the socket is ready yet

What problem existed in Version 3:
- thread-per-client improved concurrency
- but scalability was still poor because every client needed its own thread
- to move toward a scalable event-driven design, we needed to stop relying on sleeping threads per socket

How Version 4 solves part of that problem:
- it introduces non-blocking socket behavior
- now the application can keep control instead of getting put to sleep by `recv()`
- this is the prerequisite for event-driven socket management

Important note:
- Version 4 does not solve scalability completely
- it solves the “sleeping inside recv()” part
- it does not yet solve efficient waiting

Why Version 4 is a bridge:
Version 4 sits between:
- the blocking socket mindset
- and the event-driven socket mindset

In the blocking mindset:
- "socket not ready" means the kernel puts the thread to sleep

In Version 4:
- "socket not ready" becomes a normal control-flow result in the application
- `recv()` returns immediately with `EAGAIN` / `EWOULDBLOCK`

In the event-driven mindset:
- once readiness becomes visible to the application, the next question is:
  "how do I wait efficiently without polling every socket myself?"
- that question leads directly to `epoll`

So Version 4 is the bridge because it teaches:
- readiness is separate from doing I/O
- a socket can be "not ready yet" without sleeping the thread
- the application now needs a smarter waiting mechanism

What this version teaches:

1. What non-blocking really means
- non-blocking does not mean faster networking
- it means syscalls return immediately instead of sleeping when the socket is not ready

2. How `fcntl()` changes socket behavior
- `fcntl(fd, F_GETFL, 0)` reads the current file descriptor flags
- `fcntl(fd, F_SETFL, current_flags | O_NONBLOCK)` enables non-blocking behavior

3. The meaning of `EAGAIN` / `EWOULDBLOCK`
- these are not fatal errors in this context
- they mean:
  - “there is no data available right now”
  - “if this were a blocking socket, the call would have slept”

4. Polling as application strategy
- if the kernel no longer sleeps the thread, the program has to decide what to do next
- Version 4 chooses the simplest possible strategy:
  - keep trying `recv()` again and again

What problem in earlier versions this directly addresses:
- earlier versions treated waiting as something the kernel handled by blocking the thread
- Version 4 exposes waiting as something the application must now reason about explicitly

Why this version is limited:
- the loop around `recv()` is busy polling
- the program keeps asking:
  - “is data ready now?”
  - “what about now?”
  - “what about now?”
- this wastes CPU, especially when sockets are mostly idle

Why a small sleep was added:
- without a sleep, the demo would spin too fast and hammer the CPU
- the sleep makes the output readable
- but it does not change the fundamental architecture
- it is still polling, just less aggressively

Root cause of the limitation:
- the application is still responsible for repeatedly checking readiness itself
- the kernel is not yet being asked to notify the application only when something becomes ready

What we learned from this version:
- non-blocking mode changes syscall behavior, not application intent
- `recv()` no longer sleeps until data arrives
- “no data yet” becomes explicit application state
- polling works, but it is inefficient
- efficient waiting requires a kernel notification mechanism

Why the next version is needed:
- we now understand the prerequisites for event-driven I/O
- the next step is to stop polling sockets manually
- on Linux, `epoll` is the mechanism that tells us which sockets are ready

Summary of Version 4:
- Version 4 fixed the “thread must sleep inside recv()” mindset
- but exposed the “manual polling wastes CPU” problem

### Version 5
Name: Linux epoll Event-Loop Server

What this version does:
- the server creates an `epoll` instance in the kernel
- it registers the listening socket with `epoll`
- every accepted client socket is switched to non-blocking mode
- each client socket is also registered with `epoll`
- the server calls `epoll_wait()` to sleep until some FD becomes ready
- when the listening socket is ready, the server accepts new clients
- when a client socket is ready, the server reads data and sends a reply

In simple words:
- the server stops asking sockets manually whether they are ready
- instead, the kernel tells the server exactly which sockets are ready

What problem existed in Version 4:
- Version 4 removed blocking sleep inside `recv()`
- but the application still had to keep checking readiness itself
- this created busy polling
- busy polling wastes CPU, especially when most sockets are idle

How Version 5 solves that problem:
- it replaces manual polling with kernel-driven readiness notification
- `epoll_wait()` blocks efficiently until some watched FD becomes ready
- the server no longer loops asking sockets “ready yet?”
- it only touches sockets that the kernel has already marked as ready

Important architecture change from Version 4:
- Version 4:
  - application repeatedly calls `recv()` to test readiness
- Version 5:
  - application calls `epoll_wait()`
  - kernel returns only the ready file descriptors

What this version teaches:

1. What an epoll instance is
- `epoll_create1()` returns a file descriptor representing a kernel-managed event set
- that kernel object tracks watched file descriptors and their readiness state

2. What `epoll_ctl()` does
- `epoll_ctl()` adds, modifies, or removes watched file descriptors
- we use it to register the listening socket and each client socket

3. What `epoll_wait()` does
- `epoll_wait()` sleeps efficiently until one or more watched FDs become ready
- this is different from busy polling because the kernel decides when to wake the application

4. Why non-blocking still matters
- event loops must not block on one socket
- `epoll` tells us which sockets are likely ready
- non-blocking mode ensures one socket cannot unexpectedly stall the event loop

5. How one thread can now monitor many sockets
- the main event loop watches the listening socket and all client sockets
- one thread can now react to many connections without one thread per client

Why this version is a major milestone:
- this is the first real event-driven architecture in the project
- it establishes the core systems idea behind scalable Linux network servers

Why this version is still limited:
- business logic still runs in the same event-loop thread
- if message processing becomes expensive, the loop can still become slow
- we are still doing a very simple one-message echo-style interaction
- there is no thread pool yet
- there is no message framing, room management, or output buffering yet

Root cause of the remaining limitation:
- `epoll` solves readiness detection
- it does not solve expensive application work
- if the event-loop thread does too much computation, responsiveness still suffers

What we learned from this version:
- efficient waiting should be delegated to the kernel
- `epoll` is the Linux mechanism for readiness notification at scale
- event-driven servers depend on non-blocking sockets plus kernel notification
- one thread can now observe many sockets without one-thread-per-client overhead

Why the next version will be needed:
- now that readiness detection is efficient, we need to separate I/O management from business logic
- the next natural step is `epoll` + thread pool
- that will let the event loop stay responsive while worker threads perform more expensive processing

Summary of Version 5:
- Version 5 fixed the “manual polling wastes CPU” problem
- but it still keeps all application work in the event-loop thread

### Version 6
Name: Linux epoll + Thread-Pool Server

What this version does:
- the server still uses `epoll` to detect which sockets are ready
- but the event loop no longer performs all client work itself
- when a client socket becomes readable, the event loop creates a task
- that task is pushed into a shared queue
- one of a fixed number of worker threads takes the task and processes it

In simple words:
- `epoll` answers: "which socket is ready?"
- the thread pool answers: "which worker should process the work?"

What problem existed in Version 5:
- Version 5 solved readiness detection well
- but all processing still happened inside the event-loop thread
- if handling a message becomes expensive, the event loop can become slow
- when the event loop is slow, readiness handling for other clients is delayed

How Version 6 solves that problem:
- it separates I/O management from application work
- the event loop stays focused on:
  - waiting for readiness
  - accepting new clients
  - dispatching tasks
- worker threads focus on:
  - reading data
  - building replies
  - sending results

Important architecture change from Version 5:
- Version 5:
  - epoll detects readiness
  - same thread also processes the ready socket
- Version 6:
  - epoll detects readiness
  - task queue stores work
  - fixed worker threads process tasks

What this version teaches:

1. Separation of responsibilities
- readiness detection and business logic are different concerns
- keeping them separate improves responsiveness and architecture clarity

2. Thread pool vs thread-per-client
- a thread pool uses a fixed number of reusable worker threads
- thread-per-client creates a new thread for every client
- thread pools bound concurrency and reduce thread explosion

3. Producer-consumer design
- the event loop acts like a producer of tasks
- worker threads act like consumers of tasks
- the task queue is the shared handoff point

4. Why condition variables matter
- workers should not spin while waiting for tasks
- they sleep efficiently until new work arrives

5. Why this hybrid architecture is powerful
- `epoll` handles many sockets efficiently
- the thread pool handles parallel work efficiently
- together they avoid both:
  - one-thread-per-client overhead
  - one-event-loop-thread-does-everything bottleneck

Why this version is a major milestone:
- this is much closer to the final intended systems architecture
- it combines efficient waiting with bounded parallel processing
- this is the first version that really resembles the final design goal

Why this version is still limited:
- shared-state design is still minimal
- there is no room manager, command parser, or per-client output buffering yet
- we are still processing only a simple echo-style workload
- partial writes and richer protocol handling are not fully engineered yet
- there is still substantial robustness work left

Root cause of the remaining limitations:
- the architecture is now much stronger, but application features and edge-case handling are still simple
- scalable structure alone does not complete a chat system

What we learned from this version:
- event loops and worker pools solve different problems
- bounded worker threads are more scalable than one-thread-per-client
- a task queue is a core systems design tool
- responsiveness improves when the event loop stays lightweight

Why the next version will be needed:
- now that the core architecture exists, we need real chat-system features
- the next versions should focus on:
  - protocol design and message framing
  - rooms and private messaging
  - robustness and edge-case handling
  - logging, metrics, and evaluation

Summary of Version 6:
- Version 6 fixed the “event loop does all the work itself” problem
- but the project still needs real chat features, robustness work, and measurement

### Version 7
Name: Basic Chat Protocol Layer

What this version does:
- keeps the Version 6 architecture:
  - `epoll`
  - thread pool
  - task queue
- adds the first real application-layer protocol on top of TCP
- gives each client a meaningful identity using usernames
- lets users interact with the server using commands instead of raw arbitrary text
- upgrades the client into an interactive terminal chat client

Supported commands in this version:
- `LOGIN <username>`
- `MSG <message>`
- `USERS`
- `QUIT`

In simple words:
- Version 6 gave us the server engine
- Version 7 gives that engine its first real chat behavior

What problem existed in Version 6:
- the architecture was strong
- but the application behavior was still basically echo-style
- there was no user identity
- no command protocol
- no multi-user chat semantics

How Version 7 solves that problem:
- it defines a small application-layer protocol
- it introduces shared chat state
- it lets clients log in with usernames
- it lets a client send broadcast messages to other logged-in users
- it lets clients ask who is online
- it makes the server remember who a client is, not just which FD sent bytes

Why this version is a big conceptual step:
- before Version 7, the server knew how to move bytes
- after Version 7, the server starts understanding meaning
- this is the shift from:
  - "socket server"
  to:
  - "chat application running on top of a socket server"

Important architecture change from Version 6:
- Version 6:
  - ready socket -> worker thread -> echo-style reply
- Version 7:
  - ready socket -> worker thread -> parse command -> consult/update shared state -> send direct or broadcast responses

Detailed explanation:

#### 1. What is an application-layer protocol?
TCP does not know anything about:
- usernames
- chat messages
- commands
- rooms
- quitting

TCP only gives us a stream of bytes.

That means the application must define its own rules for interpreting those bytes.
Those rules are called the application-layer protocol.

In Version 7, our protocol is command-based.
That means the server expects each incoming line to begin with a command word such as:
- `LOGIN`
- `MSG`
- `USERS`
- `QUIT`

Examples:
- `LOGIN tanish`
- `MSG hello everyone`
- `USERS`
- `QUIT`

So the protocol answers questions like:
- what does a client have to send to log in?
- how does a client send a chat message?
- how does a client ask who is online?
- what does the server send back in response?

This is the first version where the server has clear language-level rules instead of simply echoing bytes back.

#### 2. What is a session in this project?
A session is just the server's memory about one connected client.

Very simple meaning:
- one client connects
- the server creates a small record for that client
- that record is the session

In Version 7, the session is still small.
Right now it mainly stores:
- the username for that client, if the client has successfully logged in

So when we say:
- "client session"
we mean:
- "the server-side record for one connected client"

Example:
- client connects
- server creates a session for that file descriptor
- before login, the session exists but the username is empty
- after `LOGIN alice`, the same session now says username = `alice`

Why this is needed:
- the server should remember who a client is across multiple messages
- otherwise a client could log in once, then the server would forget that identity when the next message arrives

So the important idea is:
- the socket gives transport identity
- the session gives application identity

Version 7 also now follows a stricter rule:
- one session is allowed to log in only once
- after a username is set for that session, sending `LOGIN` again returns:
  - `ERR already_logged_in`

That means:
- a client cannot keep changing usernames inside the same session anymore
- username is now stable for the whole lifetime of that connected session

#### 3. What is shared state here?
Shared state means:
- data that many worker threads may access

In Version 7, the main shared state is:
- the map from file descriptor to session
- the set of usernames currently in use

Why this data is shared:
- any client can send a command at any time
- different worker threads may process different clients at the same time
- all of them may need to read or update the same user information

Examples:
- when a new client connects, we add an empty session
- when a client logs in, we store the username in that session
- when a client asks for `USERS`, we read all logged-in usernames
- when a client disconnects, we remove that session and free the username

How duplicate username checking is implemented:
- the server keeps a shared set of active usernames
- when `LOGIN <name>` arrives, the server checks whether `<name>` is already in that set
- if the name is already present, login fails with:
  - `ERR username_taken`
- if the name is not present, the server:
  - stores it in the session
  - inserts it into the username set

Why this is useful:
- the session map answers:
  - "who is this client?"
- the username set answers:
  - "is this username already taken?"

Why synchronization is needed:
- two worker threads may try to access or update the same shared data at nearly the same time
- for example, two clients may try `LOGIN alice` concurrently
- without protection, both threads might check before either inserts, and both might incorrectly succeed

That is why Version 7 uses a mutex inside `ChatState`:
- only one thread at a time can do the check-and-update logic
- this keeps the shared state consistent

So the important lesson is:
- once your application has shared meaning, you usually also need shared memory structures
- once you have shared memory structures in a multi-threaded server, you need synchronization

#### 4. What does `LOGIN` really do?
`LOGIN <username>` is the command that turns an anonymous connected socket into a named user.

Before login:
- the client is connected
- the server knows its FD
- but the client has no identity in the chat system

After successful login:
- the session stores the username
- the username is reserved so no one else can use it
- the client can now participate as a named user

This command teaches a very important systems/application concept:
- a network connection and an application identity are not the same thing

The socket gives transport identity:
- file descriptor
- IP/port

The protocol gives application identity:
- username

#### 5. What does `MSG` do?
`MSG <message>` is the first real chat behavior.

When the server receives it:
1. it checks whether the client is logged in
2. if not, it rejects the request with `ERR login_required`
3. if yes, it gets the sender's username from shared state
4. it builds a broadcast message like:
   - `MSG alice hello everyone`
5. it sends that message to all logged-in clients

This is the first place where the server is doing message routing.

Earlier versions only did:
- read data
- send one reply

Version 7 starts doing:
- read data
- interpret meaning
- find recipients
- distribute output

That is much closer to a real communication system.

#### 6. What does `USERS` do?
`USERS` asks:
- who is currently logged in?

The server:
- reads the set of known usernames from shared state
- sorts them
- sends them back in one response

This is useful because it shows that the server is no longer only reactive.
It is now also maintaining application-level information that clients can query.

#### 7. What does `QUIT` do?
`QUIT` is a clean application-level exit.

The server:
- sends `BYE`
- removes the session from shared state
- closes the socket

This teaches another useful concept:
- disconnect handling has two layers

Transport-layer disconnect:
- socket closes

Application-layer cleanup:
- remove username
- remove session
- update shared server state

#### 8. Why the client had to change
The old client was not enough for a real chat protocol because it:
- sent one message
- waited for one reply
- exited

That works for an echo server, but not for chat.

Chat is full-duplex:
- you may send commands at any time
- the server may also send messages to you at any time

So the client now uses:
- one reader thread for incoming server messages
- one main thread for user input

Why this matters:
- if another user sends a broadcast, you should see it even while you are still typing commands
- that is normal chat behavior

#### 9. What is still simplified in Version 7
Version 7 is the first real chat version, but it is still intentionally simple.

Very important simplifications:

1. One `recv()` is treated like one command
- this is not fully correct for real TCP
- TCP is a stream, not a message protocol
- later we must handle:
  - partial commands
  - multiple commands in one read

2. Broadcast space is global
- every logged-in user is in one shared chat space
- there are no rooms yet

3. No private messaging
- all `MSG` commands are broadcast

4. Output handling is simple
- we are not yet managing strong output buffering or partial send handling

So Version 7 is best understood as:
- the first semantic chat version
- not yet the fully robust feature-complete version

What this version teaches:

1. Application-layer protocol design
- TCP only gives a byte stream
- the application must define what messages mean
- Version 7 introduces the first command language for the chat server

2. Shared state management
- once users have identities, the server must remember them
- Version 7 introduces a shared map from file descriptor to user session
- shared state requires synchronization because multiple worker threads may access it

3. Why server architecture alone is not enough
- scalable I/O and worker management are necessary
- but a real product also needs protocol semantics and state transitions

4. Interactive client design
- the client now has:
  - a reader thread for incoming server messages
  - a main input loop for user commands
- this makes the client behave more like a real chat terminal

5. Sessions as application state
- a connected client is not just an FD anymore
- it is an application participant with identity and state
- the server must maintain that state correctly across messages and disconnects

Why this version is an important transition:
- this is the first version where the system starts to behave like a chat application rather than a networking demonstration
- it connects low-level systems architecture to user-visible application behavior

Why this version is still limited:
- commands are still minimal
- we do not have chat rooms yet
- there is no private messaging yet
- message framing is still simplified
- one `recv()` is still treated like one complete command in the learning version
- partial reads / multiple commands in one read are not fully engineered yet
- send-side buffering and partial write handling are still minimal

Root cause of the remaining limitations:
- Version 7 introduces protocol semantics, but still in a deliberately simple learning form
- real chat systems require stronger framing, buffering, routing rules, and robustness

What we learned from this version:
- how application protocols sit on top of TCP
- why usernames and user lists require shared state
- how worker threads interact with shared chat state
- why message routing is a separate layer above event-driven I/O
- why sessions are necessary once clients gain persistent identity
- why the client must also become more interactive once the server supports asynchronous broadcasts

Why the next version will be needed:
- Version 7 has the first real chat semantics, but it still only supports a single shared broadcast space
- the next natural step is to add rooms and more realistic routing behavior
- after that, private messaging and stronger protocol robustness become the next major milestones

Summary of Version 7:
- Version 7 fixed the “architecture exists but chat semantics do not” problem
- but the project still needs rooms, richer routing, stronger framing, and more robustness

### Version 8
Name: Room-Based Routing Layer

What this version does:
- keeps the Version 7 architecture and command model
- adds room membership using:
  - `JOIN <room>`
  - `ROOMS`
- extends `MSG <text>` so messages are no longer broadcast to every logged-in user
- instead, a message is sent only to users in the same room as the sender
- stores each user's current room in session state

In simple words:
- Version 7 gave all users one shared global chat space
- Version 8 splits that chat space into rooms

What problem existed in Version 7:
- all logged-in users were effectively in one big common chat
- there was no concept of grouping or routing by chat context
- every `MSG` was broadcast globally to all logged-in users

How Version 8 solves that problem:
- it adds room membership to the client session
- it lets clients explicitly join a room
- it keeps a shared set of known room names
- when a client sends `MSG`, the server looks at the sender's current room
- the message is then broadcast only to users in that same room

Important architecture change from Version 7:
- Version 7:
  - logged-in user -> global broadcast space
- Version 8:
  - logged-in user -> current room -> room-scoped broadcast

What this version teaches:

1. Room membership as application state
- a user is no longer defined only by username
- the session now also needs to remember which room that user is in

2. Routing rules become richer
- server is no longer asking only:
  - "who sent this message?"
- it is also asking:
  - "which room does this sender belong to?"
  - "which clients should receive messages from this room?"

3. Shared state grows with features
- Version 7 only needed usernames and sessions
- Version 8 also needs room names and room membership
- this shows how application features increase shared-state complexity

4. Protocol evolution
- adding one feature often requires both:
  - new commands (`JOIN`, `ROOMS`)
  - new server-side state
- this is a very common pattern in protocol design

Why this version is important:
- this is the first version where message routing starts to feel realistic
- it moves the project away from one global chat and toward real chat-application behavior

Why this version is still limited:
- each user has only one current room
- there is no private messaging yet
- there is no room history yet
- room creation is implicit when a client joins a new room
- framing is still simplified
- output buffering and stronger partial-write handling are still basic

Root cause of the remaining limitations:
- Version 8 improves routing semantics, but the overall protocol is still intentionally minimal
- more realistic chat behavior will require more commands, richer session state, and stronger robustness handling

What we learned from this version:
- rooms are application-level grouping constructs built on top of the same transport architecture
- session state often grows as product features grow
- message routing rules become more complex once recipients depend on context, not just login status

Why the next version will be needed:
- rooms solve grouping, but users still cannot directly address one specific user
- the next natural step is private messaging and stronger protocol semantics

Summary of Version 8:
- Version 8 fixed the “all users share one global broadcast space” problem
- but the project still needs private messaging, stronger framing, and more robustness

### Version 9
Name: Private Messaging Layer

What this version does:
- keeps the Version 8 architecture:
  - Linux `epoll`
  - fixed-size thread pool
  - command-based chat protocol
- keeps room-based broadcast messaging through:
  - `MSG <text>`
- adds direct user-to-user messaging through:
  - `PM <username> <text>`
- uses shared session state to find which connected client currently owns the target username
- sends a private message only to that target user instead of to the whole room

In simple words:
- Version 8 answered:
  - "which room should receive this message?"
- Version 9 also answers:
  - "which exact user should receive this message?"

What problem existed in Version 8:
- room-based routing was useful, but it was still incomplete
- users could talk to everyone in their room
- users could not directly talk to one specific person
- that meant the protocol still felt more like a classroom broadcast channel than a realistic chat system

How Version 9 solves that problem:
- it adds a new protocol command:
  - `PM <username> <text>`
- when the server receives `PM`, it:
  - identifies the sender from the sender's session
  - looks up the target username in shared server state
  - finds the target client's file descriptor
  - sends the private message only to that one target client
- the sender also receives a direct acknowledgement:
  - `OK PM <username>`

Important architecture change from Version 8:
- Version 8:
  - sender -> sender room -> everyone in that room
- Version 9:
  - sender -> target username -> one target client

This is a very important change because routing is no longer based only on group context.
Now routing can also be based on user identity.

What this version teaches:

#### 1. Routing rules can depend on different kinds of state
By Version 8, the server already knew how to route by room.
That means:
- read sender session
- find sender room
- send to everyone in that room

Version 9 adds another routing style:
- read sender session
- read target username from the command
- search server state for the client that owns that username
- send only to that client

So now the protocol has two different routing models:
- `MSG` uses room membership
- `PM` uses username lookup

This teaches an important server design idea:
- application commands may share the same transport layer
- but each command can have its own routing logic

#### 2. Shared state is now used not just to remember users, but to find users
In Version 7 and Version 8, shared state already stored:
- sessions
- usernames
- rooms

But in Version 9, shared state becomes even more useful.
It is not only storing information for bookkeeping.
It is actively used to answer a routing question:
- "which connected client currently has username `alice`?"

That means shared state now helps with:
- identity
- validation
- routing

This is a major step toward a more realistic chat server.

#### 3. Private messaging introduces a different kind of protocol validation
For room broadcast, the main checks were:
- is the user logged in?
- has the user joined a room?
- did the user actually provide message text?

For private messaging, the checks are different:
- is the sender logged in?
- did the sender provide a target username?
- did the sender provide message text?
- does that target user currently exist?

If any of those checks fail, the server must return a meaningful error such as:
- `ERR login_required`
- `ERR target_required`
- `ERR message_required`
- `ERR user_not_found`

This teaches another important protocol idea:
- once features grow, validation rules also grow
- protocol design is not just about adding commands
- it is also about defining correct and incorrect states clearly

#### 4. Sessions now matter even more
Earlier, the session mainly helped the server remember:
- who this client is
- which room this client is in

In Version 9, that sender identity is required for private messaging too.

Example:
- Client A logs in as `alice`
- Client B logs in as `bob`
- Alice sends:
  - `PM bob hello`

The server checks Alice's session to know:
- the sender is `alice`

Then the server searches shared state to find:
- which client session belongs to `bob`

Then it can deliver:
- `PM alice hello`

So sessions are now doing more than identity storage.
They are part of the logic that makes directed communication possible.

#### 5. The protocol is now starting to feel like a real chat protocol
By this point, the server supports:
- login
- user listing
- room joining
- room listing
- room broadcast
- direct private messaging
- quit

That means the protocol is no longer just demonstrating one concept.
It is beginning to look like a compact but real chat command language.

Why this version is important:
- this is the first version where users can communicate both publicly and privately
- the protocol now supports both group communication and one-to-one communication
- that makes the server behavior much closer to a real chat application

Why this version is still limited:
- users can be in only one room at a time
- there is still no chat history
- there is still no message persistence
- framing is still simplified:
  - one `recv()` is still treated like one complete command for learning purposes
- output buffering is still basic
- partial writes and stronger backpressure handling are not fully engineered yet

Root cause of the remaining limitations:
- Version 9 improves protocol semantics, but the I/O layer is still intentionally simplified
- richer chat behavior now needs stronger transport-level correctness work, not just new commands

What we learned from this version:
- message routing can be based on identity, not only group membership
- shared state becomes more valuable as command semantics become richer
- protocol design gets harder as commands need different validation and routing rules
- adding one new feature often means both:
  - more protocol cases
  - more state lookup logic

Why the next version will be needed:
- the chat semantics are now much richer
- but the transport handling is still simplified
- the next natural step is to improve message framing, buffering, and robustness so the protocol becomes safer and more production-like

Summary of Version 9:
- Version 9 fixed the “users can only broadcast to rooms” problem
- but the project still needs stronger framing, output buffering, and robustness work

### Version 10
Name: Buffered Command Framing Layer

What this version does:
- keeps the Version 9 chat protocol:
  - `LOGIN`
  - `JOIN`
  - `MSG`
  - `PM`
  - `USERS`
  - `ROOMS`
  - `QUIT`
- changes how incoming TCP data is handled
- adds a per-client input buffer inside each session
- stops assuming that one `recv()` call contains exactly one full command
- collects bytes until a newline arrives, then extracts complete commands and processes them one by one

In simple words:
- earlier versions treated TCP like a message queue
- Version 10 starts treating TCP more correctly as a byte stream

What problem existed in Version 9:
- the chat features were getting richer
- but the transport handling was still too naive
- the server assumed:
  - one `recv()` call = one complete command

That assumption is unsafe because TCP does not preserve message boundaries for the application.

Examples of what can go wrong:
- one command may arrive split across multiple `recv()` calls
- two commands may arrive together in a single `recv()` call
- a command may arrive partially now and partially later

So Version 9 had richer semantics, but weak framing.

How Version 10 solves that problem:
- each client session now stores an input buffer
- when bytes arrive from `recv()`, the server appends them to that client's buffer
- the server checks whether the buffer contains a newline
- if no newline exists yet:
  - the command is incomplete
  - the server keeps the partial bytes and waits for more data later
- if one or more newlines exist:
  - the server extracts each full line as one complete command
  - then processes those commands one by one

This is the first version where input handling begins to respect TCP stream behavior more realistically.

Important architecture change from Version 9:
- Version 9:
  - receive bytes
  - immediately assume they form exactly one command
- Version 10:
  - receive bytes
  - append to per-client buffer
  - extract only newline-complete commands
  - leave incomplete bytes buffered for later

What this version teaches:

#### 1. TCP is a byte stream, not a command stream
This is the most important lesson of Version 10.

The kernel gives your application bytes.
It does not promise:
- one `send()` on the client equals one `recv()` on the server
- one command arrives all at once
- commands stay neatly separated

That means the server itself must define how commands begin and end.

In our project, the framing rule is:
- one command = one newline-terminated line

So examples of complete commands are:
- `LOGIN alice\n`
- `JOIN systems\n`
- `PM bob hello\n`

This is called message framing.

#### 2. Per-client buffering is required for real protocols
Once TCP is understood as a stream, buffering becomes necessary.

Why:
- if half of `LOGIN alice\n` arrives now, the server cannot parse it safely yet
- it must store the partial bytes somewhere
- when the rest arrives later, the server combines them and then parses the full command

That "somewhere" is the per-client input buffer stored in the session.

So Version 10 strengthens the meaning of session again:
- it no longer stores only user identity and room state
- it also stores incomplete input state for that connection

#### 3. One recv() may contain multiple commands
The reverse problem is also important.

Suppose the client sends quickly:
- `USERS\n`
- `ROOMS\n`

TCP may deliver both in one `recv()` call.

Earlier versions would have treated that chunk incorrectly as one combined message.
Version 10 instead:
- appends the bytes to the buffer
- extracts `USERS`
- extracts `ROOMS`
- processes both commands separately

This is a major correctness improvement.

#### 4. Protocol correctness depends on transport correctness
By Version 9, the server already had:
- usernames
- rooms
- private messaging

But without correct command framing, those higher-level features were built on a shaky foundation.

Version 10 teaches that:
- protocol semantics are not enough
- the transport boundary between bytes and commands must also be designed carefully

This is one of the biggest differences between a demo server and a stronger systems project.

Why this version is important:
- it does not add flashy user-visible features
- but it makes the server substantially more correct
- it is the first step toward production-like command parsing

Why this version is still limited:
- input buffering is better, but output buffering is still simple
- partial writes are still not fully handled with per-client outgoing queues
- backpressure handling is still limited
- authentication and access control are not added yet
- persistence is not added yet

Root cause of the remaining limitations:
- Version 10 improves how commands are assembled from input bytes
- but real robustness also requires stronger handling of outgoing data, authentication state, persistence, and operational controls

What we learned from this version:
- command framing is an application responsibility on top of TCP
- per-client session state may need to include protocol parsing buffers
- transport correctness and protocol correctness are tightly connected
- a systems project becomes much stronger when it handles stream boundaries explicitly

Why the next version will be needed:
- now that incoming command framing is less fragile, the project is ready for stronger product semantics
- the next good step is authenticated users and protected resources such as password-protected rooms

Summary of Version 10:
- Version 10 fixed the “one recv() is treated like one full command” problem
- but the project still needs authentication, access control, output buffering, and robustness work

### Version 11
Name: Authentication And Protected Rooms

What this version does:
- keeps the Version 10 line-based buffering model
- adds account registration through:
  - `REGISTER <username> <password>`
- upgrades login to:
  - `LOGIN <username> <password>`
- makes room creation explicit through:
  - `CREATE_ROOM <room> <password_or_dash>`
- adds protected room entry through:
  - `JOIN_ROOM <room> <password_or_dash>`
- keeps both user-list queries:
  - `USERS` for all authenticated users
  - `ROOM_USERS` for users in the caller's current room
- keeps room messaging and private messaging:
  - `MSG <text>`
  - `PM <username> <text>`

In simple words:
- before Version 11, a client could claim a username directly
- after Version 11, a client must first have an account and then authenticate
- rooms are no longer created implicitly by joining them
- rooms can now be public or password-protected

What problem existed in Version 10:
- the transport layer was becoming stronger
- but the identity model was still too weak
- any client could become any username just by sending `LOGIN alice`
- there was no password verification
- there was no difference between:
  - a registered user account
  - a currently logged-in session
- rooms were also too weak:
  - joining a room implicitly created it
  - there was no access control
  - there was no concept of protected rooms

So the server had messaging semantics, but not real authentication or authorization.

How Version 11 solves that problem:
- it adds a registered account store inside shared server state
- each account stores:
  - username
  - salt
  - password hash
- `REGISTER` creates a new account
- `LOGIN` checks the supplied password against the stored salted hash
- only after successful login does a session gain authenticated identity
- room creation is now an explicit action
- a room can be:
  - public, using `-` as the password token
  - protected, using a real room password
- `JOIN_ROOM` verifies the room password before allowing entry
- `ROOM_USERS` uses the caller's current room to return just the room-local member list

Important architecture change from Version 10:
- Version 10:
  - connected session -> username -> room
- Version 11:
  - registered account -> authenticated session -> room membership

That is a major conceptual upgrade.
The server now separates:
- who exists as an account
- who is currently connected and authenticated

What this version teaches:

#### 1. Registered account is different from active session
This is the most important new idea.

A registered account means:
- this username exists in the system permanently while the server is running
- it has credential information stored in server memory

A session means:
- one current client connection
- maybe authenticated
- maybe not authenticated yet

So:
- accounts are long-lived identity records
- sessions are connection-specific runtime state

Example:
- `REGISTER alice secret123`
  - creates the account
- later `LOGIN alice secret123`
  - authenticates one session as `alice`

This separation is important because authentication always connects:
- stored identity
to
- current connection state

#### 2. Authentication means verifying identity, not just claiming identity
Before Version 11, sending `LOGIN alice` was enough to become `alice`.

That was only identity claiming.

Version 11 upgrades that to identity verification.
Now the server checks:
- does account `alice` exist?
- does the supplied password match the stored hashed password?

Only then does the session become authenticated.

This is a very important step toward a real multi-user system.

#### 3. Passwords should not be stored in plain text
This version introduces password hashing with salt.

In the current learning implementation:
- the project uses a salted hash approach in code
- this is much better than storing raw passwords

Important honesty note:
- this implementation is still a teaching-level design
- it is not production-grade password storage
- in a real production system, you would use a dedicated password hashing algorithm such as bcrypt, scrypt, Argon2, or PBKDF2

But for this project, the architectural lesson is still valuable:
- raw passwords should not be stored directly
- verification should happen through stored hash data

#### 4. Authorization is different from authentication
Authentication answers:
- "who are you?"

Authorization answers:
- "are you allowed to do this?"

Version 11 introduces the first real authorization rule set.

Examples:
- only an authenticated user can create a room
- only an authenticated user can join a room
- if a room is protected, the user must provide the correct room password

So this version is not just about login.
It is also about permission checks on actions.

#### 5. Room metadata becomes richer
In earlier versions, room state was basically just:
- the room name

Now a room also stores:
- owner username
- whether the room is protected
- salt
- room password hash

That means room state is no longer only about routing.
It is now also about security and control.

This is how real systems often evolve:
- first a resource exists
- then later permissions and metadata grow around that resource

Why this version is important:
- this is the first version where the server behaves like a true multi-user system rather than a nickname-based demo
- identity, authentication, and protected resources are now part of the design
- it creates much stronger interview discussion around:
  - password handling
  - session state
  - account state
  - authorization

Why this version is still limited:
- account data is still only in memory
- there is no persistence across server restart yet
- password hashing is still demonstration-grade, not production-grade
- room owner metadata exists, but owner privileges are not enforced yet
- output buffering and stronger partial-write handling still need work
- rate limiting and failed-attempt tracking are not added yet

Root cause of the remaining limitations:
- Version 11 adds secure-ish semantics and access control checks
- but the server still needs stronger operational robustness and richer admin behavior

What we learned from this version:
- registered identity and active session identity are different concepts
- authentication and authorization are different layers
- password-protected resources require richer metadata and validation logic
- once accounts exist, the server state model becomes much closer to a real messaging backend

Why the next version will be needed:
- now that rooms have owners, the next natural step is to give ownership meaning
- the project can next move toward owner/admin controls, persistence, or stronger output robustness

Summary of Version 11:
- Version 11 fixed the “usernames can be claimed without verification and rooms have no access control” problem
- but the project still needs persistence, owner/admin behavior, stronger output handling, and operational safeguards

### Version 12
Name: Owner-Controlled Room Management

What this version does:
- keeps Version 11 account registration and password-checked login
- keeps public and protected room creation/joining
- makes room owner metadata actually matter through:
  - `SET_ROOM_PASS <room> <new_password_or_dash>`
  - `DELETE_ROOM <room>`
- allows only the room owner to change room protection or delete the room

In simple words:
- Version 11 stored room ownership
- Version 12 starts using room ownership for permission checks

What problem existed in Version 11:
- rooms already had an `owner_username`
- but that owner information was mostly passive metadata
- there were no owner-only actions yet
- so room ownership existed in data, but not in behavior

How Version 12 solves that problem:
- it adds owner-only room management commands
- when a client calls `SET_ROOM_PASS`, the server checks:
  - is the client logged in?
  - does the room exist?
  - is this client the room owner?
- only then can the room be changed between:
  - protected
  - public
- when a client calls `DELETE_ROOM`, the server performs the same ownership checks
- if deletion succeeds:
  - the room metadata is removed
  - any sessions currently in that room are detached from it

Important architecture change from Version 11:
- Version 11:
  - room owner existed as metadata
- Version 12:
  - room owner now affects which commands are legal

That means room ownership is no longer just descriptive.
It is now part of the authorization model.

What this version teaches:

#### 1. Ownership is a form of authorization
Authentication answers:
- who are you?

Ownership-based authorization answers:
- are you allowed to manage this resource?

Version 12 is the first version where the server checks permissions based on resource ownership.

#### 2. Stored metadata becomes meaningful only when commands enforce it
Just storing an `owner_username` is not enough.
The system becomes interesting when that metadata changes behavior.

Version 12 is a good example of a common systems pattern:
- first add metadata
- then later enforce policy based on that metadata

#### 3. Resource deletion affects other session state
Deleting a room is not only about removing one map entry.
The server must also think about active users who are still inside that room.

In this version:
- if a room is deleted, sessions currently pointing at that room are cleared out of it

That teaches an important state-management lesson:
- deleting one shared object may require updating other connected state too

Why this version is important:
- the server now has its first real owner-only actions
- this makes the access-control model more realistic
- it creates stronger interview discussion around:
  - resource ownership
  - authorization checks
  - cascading state updates when shared resources are deleted

Why this version is still limited:
- owner actions are still minimal
- there is no `KICK`, `MUTE`, or moderator role yet
- state is still in-memory only
- room/account metadata is not persisted across restart
- output buffering and stronger partial-write handling still need work

Root cause of the remaining limitations:
- Version 12 strengthens command permissions
- but the project still needs persistence and operational robustness to feel more complete

What we learned from this version:
- ownership is a natural next step after authentication
- authorization often depends on both:
  - who the caller is
  - which resource the caller is trying to control
- state cleanup matters when shared resources are removed

Why the next version will be needed:
- room and account state are now richer and more important
- the next strong step is to make important state survive restart through persistence planning and implementation

Summary of Version 12:
- Version 12 fixed the “room ownership exists in metadata but not in behavior” problem
- but the project still needs persistence, richer admin controls, and stronger runtime robustness

## Cross-Version Lessons So Far

### 1. Basic TCP lifecycle comes first
Before talking about scalable architecture, you must understand:
- `socket()`
- `bind()`
- `listen()`
- `accept()`
- `recv()`
- `send()`
- `close()`

### 2. Persistence is not concurrency
A server can stay alive forever and still be poor at handling multiple active clients.

### 3. Concurrency is not scalability
Thread-per-client improves concurrency, but that does not mean it scales well.

### 4. Non-blocking is not event-driven by itself
Non-blocking sockets stop the thread from sleeping, but they do not tell you how to wait efficiently.

### 5. Every version solved one problem and revealed the next one
- Version 1 solved “no working server”
- Version 2 solved “server exits after one client”
- Version 3 solved “one blocked client stalls everyone”
- Version 4 solved “I/O always sleeps the thread”
- Version 5 solved “manual polling wastes CPU”
- Version 6 solved “the event loop does all processing itself”
- Version 7 solved “the architecture exists, but the server still lacks real chat semantics”
- Version 8 solved “all users share one global chat space”
- Version 9 solved “users can talk to rooms, but not directly to one specific user”
- Version 10 solved “the protocol exists, but TCP byte boundaries are still handled too naively”
- Version 11 solved “identity and room access exist, but without real verification or protection”
- Version 12 solved “room ownership exists, but owner permissions do not affect behavior yet”

And each of those solutions exposed the next systems problem to learn.

## Tools And APIs Used So Far

### POSIX socket APIs
Used APIs:
- `socket`
- `bind`
- `listen`
- `accept`
- `connect`
- `recv`
- `send`
- `close`
- `inet_pton`
- `inet_ntop`
- `htons`
- `htonl`
- `ntohs`

Why we are using them:
- they expose the real Unix/Linux networking model directly
- they help build systems understanding from the ground up

Why we are not using higher-level networking libraries yet:
- they hide low-level behavior that we explicitly want to learn first

### Thread support
Used APIs / library:
- `std::thread`
- `std::mutex`
- `std::condition_variable`

Why they were introduced:
- first to learn thread-per-client concurrency
- later to build a fixed-size thread pool and task queue
- to understand producer-consumer coordination

### File descriptor control
Used API:
- `fcntl`

Why it was introduced:
- to change socket behavior into non-blocking mode
- to understand how readiness and syscall behavior are related

### Event notification
Used Linux APIs:
- `epoll_create1`
- `epoll_ctl`
- `epoll_wait`

Why they were introduced:
- to let the kernel notify the server about readiness
- to replace manual polling with efficient waiting
- to form the basis of an event-driven server architecture

### CMake
Used for long-term project structure.

Why keep it:
- it scales better as the project grows
- it is standard in C++ projects

## Commands Used So Far

### Compile server on macOS
```bash
clang++ -std=c++17 -Wall -Wextra -Wpedantic src/main.cpp -o chat_server
```

### Compile client on macOS
```bash
clang++ -std=c++17 -Wall -Wextra -Wpedantic client/simple_client.cpp -o simple_client
```

### Run server
```bash
./chat_server
```

### Run client
```bash
./simple_client
```

## Environment Notes

### macOS
- current TCP code works on macOS
- Versions 1 through 4 are good learning steps on macOS

### Linux
- `epoll` is Linux-specific
- when we move to the `epoll` version, we will need Linux, Docker, a VM, or another Linux environment

### Docker on macOS
- Docker lets us keep macOS as the development host while running the Linux-specific server inside a Linux container
- this does not change the project architecture
- it mainly changes how Linux versions are built and executed
- this is the preferred way to continue the `epoll` path on a Mac without rewriting the project for `kqueue`

## What Is Pending

### Next version
Version 13:
- persistence for accounts and room metadata

What Version 13 should solve:
- stop losing all registered users and rooms on restart
- decide how account and room metadata should be stored and reloaded
- make the server feel less like a single-process temporary demo

What we expect to learn:
- persistence boundaries
- serialization and reload flow
- what state should and should not survive process restart

### Later versions
- `epoll` + thread-pool server
- command protocol and message framing
- rooms and private messaging
- logging and metrics
- rate limiting
- idle timeout
- stress testing

## Rule For Future Updates
For every future version, write the section so that a reader can answer:
- what did the previous version fail at?
- what does this version change?
- what exactly improved?
- what still remains broken?
- what concept did we learn?
- why is the next version necessary?
