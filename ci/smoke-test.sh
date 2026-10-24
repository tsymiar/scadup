#!/usr/bin/env bash
# End-to-end smoke test: broker + subscriber + publisher round trip.
# Called by make.sh after a build, and directly by CI.
#
# Usage: ci/smoke-test.sh [build-dir] [binary-dir]
# Exits 0 when one publish() reaches one live subscriber: the broker counts a
# subscriber as reached only after the write to its socket succeeded, so the
# publisher's own report is a reliable end-to-end signal.
set -u

BUILD_DIR=${1:-build}
BIN_DIR=${2:-$BUILD_DIR/test}
# Absolute: the test itself runs from a private working directory
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PWD/$BUILD_DIR" ;; esac
case "$BIN_DIR" in /*) ;; *) BIN_DIR="$PWD/$BIN_DIR" ;; esac
TOPIC=1234 # parsed as hex by the tool: 0x1234
PAYLOAD="smoke-test-$$"

# Locate the tool: the layout depends on the generator -- Visual Studio nests
# it under Release/, Ninja and Make do not. Try the known layouts, then search.
EXE=""
for candidate in "$BIN_DIR/scadup.exe" "$BIN_DIR/Release/scadup.exe"; do
    if [ -x "$candidate" ]; then EXE="$candidate"; break; fi
done
if [ -z "$EXE" ]; then
    found=$(find "$BUILD_DIR" -name scadup.exe -type f 2>/dev/null | head -1)
    [ -n "$found" ] && EXE="$found"
fi
if [ -z "$EXE" ]; then
    echo "smoke: scadup.exe not found under $BUILD_DIR:"
    find "$BUILD_DIR" -name "*.exe" -type f 2>/dev/null | head -5
    exit 1
fi

# Private dir with a loopback cfg: the repo's own cfg may point at a LAN
# broker, and a random port keeps a leftover broker from breaking the run.
WORK=$(mktemp -d)
LOG_DIR="$WORK/log"
mkdir -p "$LOG_DIR"
PORT=$(( 20000 + RANDOM % 20000 ))
printf 'IP=127.0.0.1\nPORT=%s\n' "$PORT" > "$WORK/scadup.cfg"
trap 'rm -rf "$WORK"' EXIT

cd "$WORK" || { echo "smoke: cannot enter $WORK"; exit 1; }

# SIGTERM ends the broker; disown keeps the shell from reporting the kill.
"$EXE" 1 >"$LOG_DIR/broker.log" 2>&1 &
BROKER=$!
disown "$BROKER" 2>/dev/null || true

# Wait for the listener, then watch it briefly: a broker that could not bind
# dies within moments, and a stranger answering on the port must not pass for
# it. (The log line cannot be waited for: stdout is a file, so the broker's
# printf output stays buffered until it exits.)
READY=0
for _ in $(seq 1 50); do
    kill -0 "$BROKER" 2>/dev/null || break
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
        READY=1
        break
    fi
    sleep 0.1
done
if [ "$READY" -eq 1 ]; then
    for _ in $(seq 1 10); do
        kill -0 "$BROKER" 2>/dev/null || { READY=0; break; }
        sleep 0.1
    done
fi
if [ "$READY" -ne 1 ]; then
    echo "smoke: FAILED, broker did not start on port $PORT:"
    cat "$LOG_DIR/broker.log" 2>/dev/null || true
    kill "$BROKER" 2>/dev/null
    exit 1
fi

"$EXE" 2 "$TOPIC" >"$LOG_DIR/subscriber.log" 2>&1 &
SUBSCRIBER=$!
sleep 1

# Runs to completion and flushes its own report on exit.
OUT=$("$EXE" 3 "$TOPIC" "$PAYLOAD" 2>&1)
echo "$OUT"

# Kill the broker only: the subscriber then sees the connection end and exits
# on its own, flushing its log; SIGTERM would cut the buffered output short.
kill "$BROKER" 2>/dev/null
for _ in $(seq 1 50); do
    kill -0 "$SUBSCRIBER" 2>/dev/null || break
    sleep 0.1
done
kill -0 "$SUBSCRIBER" 2>/dev/null && kill -9 "$SUBSCRIBER" 2>/dev/null
wait "$SUBSCRIBER" 2>/dev/null

echo "--- subscriber log ---"
cat "$LOG_DIR/subscriber.log" 2>/dev/null || true

STATUS=0
if ! echo "$OUT" | grep -q "delivered to 1 subscriber"; then
    echo "smoke: FAILED, no delivery report from the publisher"
    STATUS=1
fi
if ! grep -q "$PAYLOAD" "$LOG_DIR/subscriber.log" 2>/dev/null; then
    echo "smoke: FAILED, subscriber never logged the payload"
    STATUS=1
fi
if [ "$STATUS" -ne 0 ]; then
    echo "--- broker log ---"
    cat "$LOG_DIR/broker.log" 2>/dev/null || true
else
    echo "smoke: OK (topic 0x$TOPIC, delivered to 1 subscriber)"
fi
exit "$STATUS"
