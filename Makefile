CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread -I.
TARGETS = server server_baseline client

.PHONY: all clean run-server run-baseline run-client server-loss

all: $(TARGETS)

server: server.c packet.h
	$(CC) $(CFLAGS) $< -o $@

# Target for 10% simulated application-level packet loss
server-loss: server.c packet.h
	$(CC) $(CFLAGS) -DSIMULATED_LOSS=10 $< -o server_loss


server_baseline: server_baseline.c packet.h
	$(CC) $(CFLAGS) $< -o $@

client: client.c packet.h
	$(CC) $(CFLAGS) $< -o $@

# ─── Quick test targets ───────────────────────────────────────────────────────
run-server:
	./server

run-baseline:
	./server_baseline

# Usage: make run-client SERVER=127.0.0.1 GROUP=1
run-client:
	./client $(SERVER) $(GROUP)

# ─── Network simulation helpers (requires root / sudo) ────────────────────────
netem-10pct:
	sudo tc qdisc add dev lo root netem loss 10% delay 20ms
	@echo "Applied: 10% loss, 20ms delay on loopback"

netem-20pct:
	sudo tc qdisc add dev lo root netem loss 20% delay 20ms
	@echo "Applied: 20% loss, 20ms delay on loopback"

netem-off:
	sudo tc qdisc del dev lo root
	@echo "Removed tc netem rules"

# ─── Benchmark shortcut ───────────────────────────────────────────────────────
bench:
	python3 benchmark.py --mode reliable --subscribers 5 \
		--messages 20 --loss 0.10 --sweep

clean:
	rm -f $(TARGETS)
