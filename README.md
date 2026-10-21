
# CS422 TCP Congestion Control Using eBPF

A custom TCP congestion-control algorithm implemented using Linux eBPF `struct_ops` for Purdue CS422.

The algorithm adjusts the TCP congestion window (`cwnd`) based on packet loss, queueing delay, and acknowledged-byte throughput.

## Requirements

- Ubuntu 22.04+ on x86-64 Linux or WSL2
- Linux kernel with BTF and TCP BPF `struct_ops` support
- Internet connection for initial setup
- `sudo` privileges

WSL1, Windows, and native macOS are not supported.

## Project Structure

```text
cs422-assignment3/
├── Dockerfile
├── README.md
├── congestion_control.py
├── main.py
├── requirements.txt
├── setup.sh
└── src/
    ├── cs422.bpf.c
    └── loader.c
```

The setup script automatically generates `src/vmlinux.h`, `build/`, and `deps/`. These generated files do not need to be committed.

## Setup and Running

Clone the repository:

```bash
git clone <YOUR_REPOSITORY_URL> cs422-assignment3
cd cs422-assignment3
```

Run:

```bash
chmod +x setup.sh
./setup.sh
```

The script automatically:

1. Checks the environment and installs missing dependencies.
2. Downloads and builds libbpf and bpftool if needed.
3. Generates kernel headers and compiles the eBPF program and loader.
4. Registers `cs422` and allows unprivileged applications to select it.

Once you see `CS422 READY`, **leave this terminal running**. Press Ctrl+C to unregister the algorithm and restore the original allowed congestion-control configuration.

Subsequent runs reuse installed dependencies and compiled tools, avoiding unnecessary downloads.

## Testing

Open another terminal while `setup.sh` is running.

### Remote Test

Use a public or private iperf3 server that supports TCP upload testing:

```bash
iperf3 -c SERVER_IP -p PORT -t 15 -C cs422
iperf3 -c SERVER_IP -p PORT -t 15 -C cubic
iperf3 -c SERVER_IP -p PORT -t 15 -C reno
```

Use the same server, port, and duration for comparisons.

Do not use `-R` (remote sender) or `-u` (UDP), since the objective is to evaluate the local TCP sender's congestion control.

## Algorithm Design

The algorithm uses the following parameters:

| Parameter | Value |
|---|---|
| Minimum congestion window | 1 segment |
| Queueing delay threshold | 10 ms |
| RTT history | 6 buckets, 5 measurements per bucket |
| Measurement interval | Approximately 1 RTT |

Queueing delay is estimated using the minimum RTT observed in a sliding measurement window:

```text
queue_delay = smoothed_RTT - base_RTT
```

The congestion window is adjusted according to these conditions:

| Condition | Adjustment |
|---|---|
| Newly detected packet loss | Decrease by 50% |
| High queueing delay, throughput not improving | Decrease by 50% |
| High queueing delay, throughput improving | Decrease by 2% |
| Low queueing delay, throughput improving | Increase by 4% |
| Low queueing delay, throughput not improving | Increase by 2% |

Percentage adjustments are rounded down to whole segments, with a minimum change of one segment.

During slow start, the congestion window increases with newly acknowledged packets. Congestion detection exits slow start, and the algorithm then applies its congestion-avoidance adjustments.

All congestion-window updates respect the kernel's `snd_cwnd_clamp`.

## Known Limitations

- The 10 ms queueing-delay threshold is fixed rather than adaptive.
- Packet loss is inferred from changes in `tp->lost`, which is not a cumulative loss counter and may miss loss events.
- Throughput is estimated from acknowledged bytes over approximately one RTT, making measurements sensitive to short-term fluctuations.
- The RTT baseline uses a finite measurement history rather than a time-based window.
- Performance varies with network conditions and may be worse than established algorithms such as CUBIC and Reno.

This is an experimental implementation intended for coursework, not production use.

## Troubleshooting

**Missing `/sys/kernel/btf/vmlinux`:** The kernel does not expose the required BTF information. On WSL2, try updating WSL using `wsl --update` followed by `wsl --shutdown` in PowerShell.

**Failed to load BPF object:** Check the libbpf verifier logs. The kernel may lack required BPF functionality.

**Failed to attach struct_ops (`-17`):** `cs422` may already be registered. Stop the previous loader before retrying.

**Unable to select `cs422` in iperf3:** Confirm that setup is still running and check:

```bash
sysctl net.ipv4.tcp_available_congestion_control
sysctl net.ipv4.tcp_allowed_congestion_control
```

**Public iperf3 server unavailable or busy:** Try another port or server.

## Unloading

Press Ctrl+C in the terminal running `setup.sh`. This detaches the BPF congestion-control implementation and restores the original allowed congestion-control list.
