# Sockets across platforms

`flox/net/socket.h` is the socket surface the venue perimeter uses, with the
platforms' disagreements in one place instead of at sixty call sites.

It is deliberately small. Anything the standard library already does
portably is not here, and neither is any operation nothing calls: a layer
that wraps what nobody uses is a layer nobody maintains.

## Three differences that do not announce themselves

Most of what separates Berkeley sockets from Winsock is loud — a type that
is not `int`, a close that is not `close`, errors that are not in `errno`.
Those fail to compile and get fixed. Three do not.

**The receive timeout.** Both platforms spell the option `SO_RCVTIMEO`. POSIX
wants a `timeval`; Windows wants a count of milliseconds in a `DWORD`. Pass
one where the other is expected and `setsockopt` succeeds, returning zero,
having set a timeout nobody asked for. Nothing downstream can tell. The
perimeter leans on these timeouts for idle disconnects and for bounding how
long a connection's lock is held, so the failure is not a crash — it is
sessions that stop being reaped, or a poll loop that spins.

`setReceiveTimeout(handle, milliseconds)` and `setSendTimeout` take the unit
in their name and produce whatever the platform wants.

**Suppressing SIGPIPE.** Writing to a peer that has gone away raises SIGPIPE
and, unhandled, ends the process. macOS answers with a socket option
(`SO_NOSIGPIPE`), Linux with a send flag (`MSG_NOSIGNAL`), Windows with
nothing, because it has no such signal. Three answers to one question, and
the wrong one on macOS is fatal rather than wrong.

`suppressSigPipe(handle)` at setup and `sendNoSignal` at the send: together
they cover all three, and each does nothing where nothing is needed.

**Non-blocking mode.** POSIX reaches it through the file descriptor
(`fcntl`), Windows through the socket handle (`ioctlsocket`). The two APIs
share no argument. `setNonBlocking(handle, bool)`.

## Names

`openSocket` and `closeSocket`, not `open` and `close`. A caller who writes
`using namespace flox::net` and then `close(fd)` would otherwise get an
ambiguity against the POSIX functions of those names, and the error would
land in their file rather than in this header. The first version used the
short names and the first test written against it did not compile.

## Errors

`lastError()` reads from the right place. `wouldBlock(e)` and
`interrupted(e)` separate "the socket had nothing to give" from "a signal
arrived, try again" — and they must stay separate, because a retry loop that
treats `EINTR` as emptiness turns into a spin. On Windows `interrupted` is
always false, which is the honest answer rather than an oversight.

## What is tested

`tests/test_socket_portability.cpp` exercises the layer over loopback rather
than asserting that it compiles: a receive timeout must elapse in the
milliseconds it was given, a non-blocking receive must return at once and say
why, a peek must not consume, a send to a dead peer must fail rather than end
the process, `pollRead` must see readable and must time out, address parsing
must refuse what is not an address, and a multicast join must refuse a
unicast one.

The timeout test is the one that matters most: mutating `setReceiveTimeout`
to pass milliseconds where a `timeval` belongs turns it red. Without a test
that measures elapsed time, that mutation compiles, runs, and is wrong.
