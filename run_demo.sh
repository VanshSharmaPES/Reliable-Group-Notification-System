#!/bin/bash
set -e

echo "======================================"
echo "    Aggressively Cleaning Up Ports    "
echo "======================================"
# Completely obliterate any ghost processes
pkill -9 server 2>/dev/null ; true
pkill -9 client 2>/dev/null ; true
fuser -k -9 9000/udp 2>/dev/null ; true
fuser -k -9 9001/udp 2>/dev/null ; true
pkill -9 -f benchmark.py 2>/dev/null ; true
sleep 1

echo "======================================"
echo "    Compiling Project                 "
echo "======================================"
make clean all

# Create named pipe
rm -f /tmp/pipe
mkfifo /tmp/pipe

echo "======================================"
echo "    Starting Reliable Server Test     "
echo "======================================"
./server < /tmp/pipe &
SERVER_PID=$!
sleep 1

# Tell python to output with no buffering
python3 -u benchmark.py --mode reliable --subscribers 5 --messages 20 > /tmp/pipe

kill -9 $SERVER_PID 2>/dev/null ; true
rm -f /tmp/pipe

echo "======================================"
echo "    Test Fully Completed!             "
echo "======================================"
