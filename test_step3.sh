#!/bin/bash
./ircserv 6667 secret &
PID=$!
sleep 0.3

echo "=== Test: 3 simultaneous clients ==="
nc -q1 127.0.0.1 6667 <<< $'NICK alice\r' &
nc -q1 127.0.0.1 6667 <<< $'NICK bob\r' &
nc -q1 127.0.0.1 6667 <<< $'NICK carol\r' &
sleep 0.5

echo "=== Test: partial message ==="
(printf "NI"; sleep 0.1; printf "CK dave\r\n") | nc -q1 127.0.0.1 6667 &
sleep 0.5

echo "=== All done, killing server ==="
kill $PID
wait $PID 2>/dev/null