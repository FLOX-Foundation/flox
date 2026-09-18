# Sockets across platforms

`flox/net/socket.h` is the socket surface the venue perimeter and the FIX
initiator use, with the platforms' disagreements in one place instead of at
sixty call sites. Nothing under `venue/` or `include/flox/connector/` reaches
a POSIX network header any more; the layer is the only thing that does.

It is deliberately small. Anything the standard library already does
portably is not here, and neither is any operation nothing calls: a layer
that wraps what nobody uses is a layer nobody maintains.

## Differences that do not announce themselves

There were three when this layer was written and a fourth turned up when the
perimeter moved onto it. Most of what separates Berkeley sockets from Winsock
is loud — a type that
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

**Where an error is kept.** (The fourth, found during the move.) A read that comes back empty is either a timeout
or a dead peer, and the perimeter decides between them by asking what went
wrong. POSIX keeps the answer in `errno`. Windows keeps socket errors in a
place `errno` never sees, so `errno == EAGAIN` there compiles and is always
false: every idle tick would read as a healthy socket, and every timeout as
one, until a session that should have been reaped never was.

`lastError()` reads from the right place; `clearLastError()` clears the right
place, which a caller needs before a read whose failure may set nothing at
all -- a clean EOF, for instance, would otherwise be judged by whatever the
previous failing call left behind.

## Names

`openSocket` and `closeSocket`, not `open` and `close`. A caller who writes
`using namespace flox::net` and then `close(fd)` would otherwise get an
ambiguity against the POSIX functions of those names, and the error would
land in their file rather than in this header. The first version used the
short names and the first test written against it did not compile.

## Errors

`wouldBlock(e)` and `interrupted(e)` separate "the socket had nothing to give" from "a signal
arrived, try again" — and they must stay separate, because a retry loop that
treats `EINTR` as emptiness turns into a spin. On Windows `interrupted` is
always false, which is the honest answer rather than an oversight.

## Connections

`bindTo`, `listenOn`, `acceptOne`, `connectTo` and `boundPort` take an
address as a string and a port as a number, so the one place that knows how
an address is laid out stays `parseAddress`. `acceptOne` fills in the peer
only if asked; the length argument it needs is an `int` on one platform and a
`socklen_t` on the other, and it is in/out, so it cannot be a `sizeof` at the
call site.

`boundPort` exists because `bindTo(handle, ip, 0)` asks the system to pick a
free port, and that is the only way the caller learns which one it got. Every
ephemeral listener in the tree is those two calls.

`sendTo` and `receiveFrom` are the datagram pair. The destination is a parsed
address rather than a string: a publisher resolves its group once at open and
sends to it per message, and re-parsing text per send would put a conversion
on the hot path.

`resolveIPv4` is the name lookup, kept separate from `parseAddress` so that a
dotted quad never pays for a resolver.

## What is tested

`tests/test_socket_portability.cpp` exercises the layer over loopback rather
than asserting that it compiles: a receive timeout must elapse in the
milliseconds it was given, a non-blocking receive must return at once and say
why, a peek must not consume, a send to a dead peer must fail rather than end
the process, `pollRead` must see readable and must time out, address parsing
must refuse what is not an address, a multicast join must refuse a unicast
one, a datagram must carry its sender back to the receiver, and a signal
arriving mid-wait must be reported as interrupted rather than as a broken
socket -- that last one is arranged on purpose, with a handler installed
without `SA_RESTART` and a thread that signals the waiter.

The timeout test is the one that matters most: mutating `setReceiveTimeout`
to pass milliseconds where a `timeval` belongs turns it red. Without a test
that measures elapsed time, that mutation compiles, runs, and is wrong.

Every operation added for the perimeter was checked the same way -- each one
mutated in turn, each mutation required to turn a named test red. Three of
them first turned the suite *hung* rather than red, because a test whose
connect had silently not happened sat in `accept` forever. A hang is not a red
test: the accepts in the suite now wait on `pollRead` first, the same bounded
wait the perimeter itself does, so a mutation fails a line instead of parking.

One mutation is not caught, and it is worth saying which rather than leaving
the impression that all of them are. Handing `setMulticastLoop` an eight-byte
value where the option wants one byte passes on macOS: the platform accepts
both and behaves identically, so no test running there can tell them apart.
The shape matters on Windows, where this test does not yet run. What *is*
covered is the option's effect -- a mutation that stops it taking effect at
all turns red, because the test asks whether a sender hears its own multicast
rather than whether `setsockopt` returned zero.

That effect check itself has a platform boundary worth stating. Turning the
option *off* is asserted on macOS only: the test selects the loopback
interface for egress, and Linux and Windows both deliver a packet sent out of
it regardless, because that is what a loopback interface is. The option governs the
kernel's extra copy, not delivery over an interface that is already a loop.
Observing it would need a real NIC address, which a CI runner cannot be
counted on to have, so the assertion runs where it means
something -- the macOS job, which builds these tests.
