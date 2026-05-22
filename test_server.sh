#!/bin/bash

# ─────────────────────────────────────────────
# ft_irc test script — Person 1 (server core)
# usage: ./test_server.sh [port] [password]
# ─────────────────────────────────────────────

PORT=${1:-6667}
PASS=${2:-secret}
BINARY="./ircserv"
PASS_COUNT=0
FAIL_COUNT=0
SERVER_PID=""

# ── colors ────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

ok()   { echo -e "${GREEN}[PASS]${NC} $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
fail() { echo -e "${RED}[FAIL]${NC} $1"; FAIL_COUNT=$((FAIL_COUNT + 1)); }
info() { echo -e "${YELLOW}[TEST]${NC} $1"; }

# ── helpers ───────────────────────────────────
start_server() {
    $BINARY $PORT $PASS &
    SERVER_PID=$!
    sleep 0.3
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        fail "Server failed to start"
        exit 1
    fi
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill $SERVER_PID 2>/dev/null
        wait $SERVER_PID 2>/dev/null
        SERVER_PID=""
    fi
}

# send data to server, capture response, timeout 1s
send_and_recv() {
    printf "%b" "$1" | nc -q1 127.0.0.1 $PORT 2>/dev/null
}

# ── check binary exists ───────────────────────
echo "================================================"
echo " ft_irc server test — port=$PORT pass=$PASS"
echo "================================================"
echo ""

if [ ! -f "$BINARY" ]; then
    fail "Binary '$BINARY' not found — run 'make' first"
    exit 1
fi
ok "Binary exists"

# ═════════════════════════════════════════════
# TEST 1 — argument validation
# ═════════════════════════════════════════════
echo ""
info "── Argument validation ──"

$BINARY 2>/dev/null
[ $? -ne 0 ] && ok "No args → exits with error" || fail "No args → should exit with error"

$BINARY abc secret 2>/dev/null
[ $? -ne 0 ] && ok "Bad port (abc) → exits with error" || fail "Bad port should fail"

$BINARY 80 secret 2>/dev/null
[ $? -ne 0 ] && ok "Port 80 (too low) → exits with error" || fail "Port 80 should fail"

$BINARY 99999 secret 2>/dev/null
[ $? -ne 0 ] && ok "Port 99999 (too high) → exits with error" || fail "Port 99999 should fail"

$BINARY 6667 2>/dev/null
[ $? -ne 0 ] && ok "Missing password → exits with error" || fail "Missing password should fail"

# ═════════════════════════════════════════════
# TEST 2 — server starts and binds
# ═════════════════════════════════════════════
echo ""
info "── Server startup ──"

start_server
ok "Server started (pid=$SERVER_PID)"

# check it's listening on the port
ss -tlnp 2>/dev/null | grep ":$PORT" > /dev/null
if [ $? -eq 0 ]; then
    ok "Server listening on port $PORT"
else
    # fallback: try netstat
    netstat -tlnp 2>/dev/null | grep ":$PORT" > /dev/null
    [ $? -eq 0 ] && ok "Server listening on port $PORT" || fail "Server not listening on port $PORT"
fi

# ═════════════════════════════════════════════
# TEST 3 — basic connection
# ═════════════════════════════════════════════
echo ""
info "── Basic connection ──"

(sleep 0.5) | nc -q1 127.0.0.1 $PORT > /dev/null 2>&1
[ $? -eq 0 ] && ok "Client can connect" || fail "Client cannot connect"

kill -0 $SERVER_PID 2>/dev/null
[ $? -eq 0 ] && ok "Server still running after connection" || fail "Server crashed after connection"

# ═════════════════════════════════════════════
# TEST 4 — multiple simultaneous clients
# ═════════════════════════════════════════════
echo ""
info "── Multiple simultaneous clients ──"

(sleep 0.4) | nc -q1 127.0.0.1 $PORT > /dev/null 2>&1 &
PID1=$!
(sleep 0.4) | nc -q1 127.0.0.1 $PORT > /dev/null 2>&1 &
PID2=$!
(sleep 0.4) | nc -q1 127.0.0.1 $PORT > /dev/null 2>&1 &
PID3=$!
sleep 0.5

kill -0 $SERVER_PID 2>/dev/null
[ $? -eq 0 ] && ok "Server handles 3 simultaneous clients" || fail "Server crashed with multiple clients"

wait $PID1 $PID2 $PID3 2>/dev/null

# ═════════════════════════════════════════════
# TEST 5 — partial message buffering (subject test)
# ═════════════════════════════════════════════
echo ""
info "── Partial message buffering (subject nc test) ──"

# send "NICK" then "alice\r\n" in two separate writes
(printf "NI"; sleep 0.1; printf "CK alice\r\n"; sleep 0.2) \
    | nc -q1 127.0.0.1 $PORT > /dev/null 2>&1 &
sleep 0.5

kill -0 $SERVER_PID 2>/dev/null
[ $? -eq 0 ] && ok "Server handles partial messages without crashing" \
              || fail "Server crashed on partial message"

# ═════════════════════════════════════════════
# TEST 6 — multiple commands in one send
# ═════════════════════════════════════════════
echo ""
info "── Multiple commands in one TCP packet ──"

printf "NICK alice\r\nUSER alice 0 * :Alice\r\nPING test\r\n" \
    | nc -q1 127.0.0.1 $PORT > /dev/null 2>&1
kill -0 $SERVER_PID 2>/dev/null
[ $? -eq 0 ] && ok "Server handles multiple commands in one packet" \
              || fail "Server crashed on multiple commands"

# ═════════════════════════════════════════════
# TEST 7 — unknown command sends error reply
# ═════════════════════════════════════════════
echo ""
info "── Unknown command handling ──"

REPLY=$(printf "UNKNOWN test\r\n" | nc -q1 127.0.0.1 $PORT 2>/dev/null)
echo "$REPLY" | grep -q "421" 2>/dev/null
[ $? -eq 0 ] && ok "Unknown command → 421 ERR_UNKNOWNCOMMAND" \
              || fail "Unknown command → no 421 reply (stubs may be blocking — ok for now)"

# ═════════════════════════════════════════════
# TEST 8 — kill client unexpectedly (POLLHUP)
# ═════════════════════════════════════════════
echo ""
info "── Unexpected client disconnect ──"

nc -q1 127.0.0.1 $PORT > /dev/null 2>&1 &
NC_PID=$!
sleep 0.2
kill -9 $NC_PID 2>/dev/null
sleep 0.3

kill -0 $SERVER_PID 2>/dev/null
[ $? -eq 0 ] && ok "Server survives unexpected client kill (SIGKILL)" \
              || fail "Server crashed after client was killed"

# connect a new client after the kill
(sleep 0.2) | nc -q1 127.0.0.1 $PORT > /dev/null 2>&1
kill -0 $SERVER_PID 2>/dev/null
[ $? -eq 0 ] && ok "New client can connect after previous was killed" \
              || fail "Server not accepting new clients after kill"

# ═════════════════════════════════════════════
# TEST 9 — kill nc with half a command sent
# ═════════════════════════════════════════════
echo ""
info "── Kill client mid-command ──"

(printf "NICK ali"; sleep 10) | nc -q0 127.0.0.1 $PORT > /dev/null 2>&1 &
NC_PID=$!
sleep 0.2
kill -9 $NC_PID 2>/dev/null
sleep 0.3

kill -0 $SERVER_PID 2>/dev/null
[ $? -eq 0 ] && ok "Server survives client killed mid-command" \
              || fail "Server crashed when client was killed mid-command"

# ═════════════════════════════════════════════
# TEST 10 — stop client with Ctrl+Z then flood
# ═════════════════════════════════════════════
echo ""
info "── Stopped client + flood (write buffer test) ──"

# open a persistent connection and pause it
exec 3<>/dev/tcp/127.0.0.1/$PORT 2>/dev/null
if [ $? -eq 0 ]; then
    # flood server from another client while fd 3 is paused
    for i in $(seq 1 20); do
        printf "NICK flood$i\r\n" | nc -q0 127.0.0.1 $PORT > /dev/null 2>&1
    done
    sleep 0.3
    exec 3>&-   # close the paused connection
    sleep 0.2

    kill -0 $SERVER_PID 2>/dev/null
    [ $? -eq 0 ] && ok "Server not blocked by stopped client during flood" \
                  || fail "Server hung during flood with stopped client"
else
    info "Skipped (dev/tcp not available in this shell)"
fi

# ═════════════════════════════════════════════
# TEST 11 — PING/PONG
# ═════════════════════════════════════════════
echo ""
info "── PING / PONG ──"

REPLY=$(printf "PING testtoken\r\n" | nc -q1 127.0.0.1 $PORT 2>/dev/null)
echo "$REPLY" | grep -qi "PONG" 2>/dev/null
[ $? -eq 0 ] && ok "PING → PONG reply received" \
              || fail "PING → no PONG reply"

# ═════════════════════════════════════════════
# TEST 12 — clean shutdown
# ═════════════════════════════════════════════
echo ""
info "── Clean shutdown ──"

kill -INT $SERVER_PID 2>/dev/null
sleep 0.5

kill -0 $SERVER_PID 2>/dev/null
[ $? -ne 0 ] && ok "Server shut down cleanly on SIGINT" \
              || { fail "Server still running after SIGINT"; kill -9 $SERVER_PID 2>/dev/null; }
SERVER_PID=""

# ═════════════════════════════════════════════
# TEST 13 — port reuse after restart
# ═════════════════════════════════════════════
echo ""
info "── Port reuse (SO_REUSEADDR) ──"

start_server
stop_server
sleep 0.1
start_server
kill -0 $SERVER_PID 2>/dev/null
[ $? -eq 0 ] && ok "Server restarts on same port immediately (SO_REUSEADDR works)" \
              || fail "Server failed to restart on same port"
stop_server

# ═════════════════════════════════════════════
# TEST 14 — memory leaks (valgrind if available)
# ═════════════════════════════════════════════
echo ""
info "── Memory leaks ──"

if command -v valgrind > /dev/null 2>&1; then
    valgrind --leak-check=full --error-exitcode=1 \
        $BINARY $PORT $PASS > /tmp/valgrind_out.txt 2>&1 &
    VAL_PID=$!
    sleep 0.3

    # connect and disconnect a few clients
    for i in 1 2 3; do
        (sleep 0.1) | nc -q1 127.0.0.1 $PORT > /dev/null 2>&1
    done
    sleep 0.2
    kill -INT $VAL_PID 2>/dev/null
    wait $VAL_PID 2>/dev/null
    EXIT=$?

    grep -q "no leaks are possible\|ERROR SUMMARY: 0" /tmp/valgrind_out.txt 2>/dev/null
    [ $? -eq 0 ] && ok "No memory leaks detected" || {
        if [ $EXIT -ne 0 ]; then
            fail "Memory leaks detected — check /tmp/valgrind_out.txt"
        else
            ok "No memory leaks detected"
        fi
    }
else
    info "valgrind not found — skipping leak check"
fi

# ═════════════════════════════════════════════
# SUMMARY
# ═════════════════════════════════════════════
echo ""
echo "================================================"
echo -e " Results: ${GREEN}$PASS_COUNT passed${NC} / ${RED}$FAIL_COUNT failed${NC}"
echo "================================================"

# cleanup
stop_server 2>/dev/null
exit $FAIL_COUNT