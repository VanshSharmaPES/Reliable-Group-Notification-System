# Reliable Group Notification System (RGNS)

This project implements a reliable, UDP-based group notification/multicast system in C. It supports subscribing to groups, reliable message delivery (with acknowledgments, timeouts, and retransmissions), and includes tools for benchmarking performance under simulated network conditions (packet loss and delay).

## Project Structure

- `server.c`: The main reliable server implementation.
- `server_baseline.c`: A baseline/unreliable implementation for comparison.
- `client.c`: The client implementation that subscribes to groups and receives notifications.
- `packet.h`: Shared protocol definitions, message types (JOIN, LEAVE, NOTIFY, ACK, PING, PONG), and constants.
- `benchmark.py`: A Python script to run automated benchmarks and tests.
- `run_demo.sh`: A shell script to compile the project and run an automated demo.
- `Makefile`: Build rules and network simulation shortcuts.

## Requirements

- **GCC** (for compiling C code)
- **Make**
- **Python 3** (for running `benchmark.py`)
- **Linux environment** (Optional, required for `tc` network emulation targets)

## Setup Steps

1. Clone or navigate to the project repository:
   ```bash
   cd rgns_final
   ```

2. Compile the project files using `make`:
   ```bash
   make all
   ```
   This will build the `server`, `client`, and `server_baseline` executables.

## Usage Instructions

### 1. Running the Automated Demo
The easiest way to see the system in action is to run the included demo script. It automatically compiles the project, starts the server, and runs the Python benchmark with 5 subscribers and 20 messages.
```bash
./run_demo.sh
```

### 2. Manual Execution

**Start the Server:**
You can start the reliable server on the default ports (UDP 9000 and 9001).
```bash
./server
# Or using make:
make run-server
```

*(Alternatively, to run the baseline/unreliable version: `make run-baseline`)*

**Start the Client:**
You can start a client and subscribe it to a specific server IP and group ID.
```bash
./client <SERVER_IP> <GROUP_ID>
# Example (Localhost, Group 1):
./client 127.0.0.1 1
# Or using make:
make run-client SERVER=127.0.0.1 GROUP=1
```

### 3. Benchmarking and Network Simulation

You can run custom benchmarks via the Python script directly. Make sure the server is running in a separate terminal.
```bash
python3 benchmark.py --mode reliable --subscribers 5 --messages 20 --loss 0.10 --sweep
```
*(There is also a handy Makefile target for this: `make bench`)*

**Testing with Network Loss/Delay (Linux only):**
To simulate real-world unreliable networks, you can use the built-in `netem` targets. These interact with the Linux traffic control (`tc`) system and require `root/sudo` privileges.

- Apply 10% packet loss and 20ms delay on the loopback interface:
  ```bash
  make netem-10pct
  ```
- Apply 20% packet loss and 20ms delay:
  ```bash
  make netem-20pct
  ```
- Remove network emulation rules to return to normal:
  ```bash
  make netem-off
  ```

## Cleanup

To clean up all compiled binaries and object files, run:
```bash
make clean
```
