# Scadup

**S**hare **c**ommand **a**nd **d**ata under _`scadup`_ **p**roxy

A lightweight Pub/Sub message broker system.

## Quick Start

```bash
# Build
./make.sh

# Start Broker
./scadup.exe 1

# Subscribe to topic
./scadup.exe 2 1234

# Publish message
./scadup.exe 3 1234 "Hello!"
```

## Architecture

```
Publisher ──┐            ┌─── Subscriber
Publisher ──┼──▶ Broker──┼──▶ Subscriber
Publisher ──┘            └─── Subscriber
```

![message_broker](doc/message_broker.png)

## Core API

```cpp
// Broker
Broker::instance().setup(9999);
Broker::instance().broker();

// Subscriber
Subscriber sub;
sub.setup("192.168.1.100", 9999);
sub.subscribe(0x1234, [](const Message& msg) {
    printf("%s\n", msg.payload.content);
});

// Publisher
Publisher pub;
pub.setup("192.168.1.100", 9999);
pub.publish(0x1234, "message");
```

## Lifecycle

* `Broker::broker()` blocks until `Broker::exit()` is called. It polls with a 200 ms
  `select()` timeout, so it returns within ~200 ms and its thread can be joined promptly.
* `broker()` returns `0` on a normal exit and `-1` only when the listening socket itself is
  gone. A single failed `accept()` — a peer that aborted the handshake, a transient
  `EMFILE` — is logged and retried, so one bad connection cannot silently stop the broker.
* Restart is supported: `setup()` → `exit()` → `setup()` on the same port works, because the
  listening socket sets `SO_REUSEADDR`. That option is POSIX only — on Windows it also acts
  as `SO_REUSEPORT` and would let another process take over the port, so it is not set there.
* `Subscriber::setup()` clears the static flags left by the previous round (`m_exit` and the
  global abandon flag), so one process can run several subscribe → stop → subscribe sessions.
  Stop and start must not overlap: let `quit()` / `exit()` finish before the next `setup()`.
* `Broker::exit()` tears down the queue, the peer sockets and the listener. Call it from a
  different thread than the one running `broker()`; the accept loop is written to survive the
  listener being closed underneath it.

## Configuration

`scadup.cfg`:
```ini
IP=192.168.1.100
PORT=9999
```

## Build

```bash
# Linux / macOS
./make.sh

# Windows (cmd / PowerShell)
make.bat

# Android
cmake -DANDROID=1 -DANDROID_ABI=arm64-v8a ..
```

For Windows, the build links `WS2_32` automatically. Winsock is started once per process
(`WSAStartup` in `Scadup::makeSocket`); `socket` handles are unsigned, so validity is
tested with `sockValid()` / `INVALID_FD` rather than `< 0`, and socket errors are read
through `SOCK_ERRNO` + `sockError()` (`WSAGetLastError()` + `FormatMessage`) instead of
`errno` + `strerror`.

## Usage

* Test case: [test](test)
* Example project: [Device2Device](https://github.com/tsymiar/Device2Device/tree/main/app/src/main/cpp)
