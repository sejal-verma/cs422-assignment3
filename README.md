
# CS422 TCP Congestion Control Using eBPF

A custom TCP congestion-control algorithm implemented using Linux eBPF `struct_ops` for Purdue CS422.

The algorithm adjusts TCP's congestion window (`cwnd`) based on:

- Round-trip time (RTT)
- Estimated queueing delay
- A TCP loss signal
- Acknowledged-byte throughput

It uses additive increases and multiplicative decreases to adapt the congestion window.

## Requirements

Supported setup target:

- Ubuntu 22.04 or newer on x86-64 Linux
- Ubuntu 22.04 or newer under WSL2
- A Linux kernel with BTF and BPF TCP congestion-control `struct_ops` support
- Internet access for installing dependencies and cloning libbpf
- `sudo` access

WSL1 is not supported.

macOS cannot run this program natively. On a Mac, use a compatible Linux virtual machine.

**Important:** The setup script checks the environment, but not every Linux kernel supports the required BPF functionality. Successful compilation does not guarantee successful loading.

## Project Structure

```text
cs422-assignment3/
├── README.md
├── setup.sh
├── src/
│   ├── cs422.bpf.c
│   ├── loader.c
│   └── vmlinux.h          # Generated during setup
├── deps/                  # Generated during setup
└── build/                 # Generated during setup
    ├── cs422.bpf.o
    └── loader
```

Only `src/cs422.bpf.c`, `src/loader.c`, `setup.sh`, and `README.md` need to be committed.

The setup script generates the other files.

## Installation

### 1. Clone the repository

```bash
git clone <YOUR_REPOSITORY_URL>
cd cs422-assignment3
```

### 2. Run setup

```bash
chmod +x setup.sh
./setup.sh
```

The script will:

1. Check the operating system and CPU architecture.
2. Check the Linux kernel and BTF availability.
3. Install the required Ubuntu packages.
4. Clone and build libbpf locally.
5. Generate `src/vmlinux.h` from the running kernel.
6. Compile the eBPF congestion-control program.
7. Compile the user-space loader.

Setup may take several minutes.

If setup fails, read the error message before proceeding.

## Running the Algorithm

### Terminal 1: Register the algorithm

From the repository root:

```bash
sudo ./build/loader
```

Expected output:

```text
cs422 registered! Press Ctrl+C to unload.
```

Leave this terminal running.

The loader keeps the BPF `struct_ops` attachment alive. Pressing Ctrl+C destroys the attachment and unregisters the algorithm.

### Terminal 2: Verify registration

```bash
sysctl net.ipv4.tcp_available_congestion_control
```

Expected to include:

```text
cs422
```

The exact list of algorithms depends on the kernel.

## Testing with iperf3

### Local loopback test

Start an iperf3 server in a separate terminal:

```bash
iperf3 -s
```

Then run:

```bash
iperf3 -c 127.0.0.1 -t 10 -C cs422
```

Compare against CUBIC:

```bash
iperf3 -c 127.0.0.1 -t 10 -C cubic
```

And Reno:

```bash
iperf3 -c 127.0.0.1 -t 10 -C reno
```

Loopback is useful for confirming that the algorithm runs, but it is not a realistic network congestion test.

### Remote network test

Use an iperf3 server that permits TCP upload testing.

Example:

```bash
iperf3 -c SERVER_IP -p SERVER_PORT -t 15 -C cs422
```

For comparison:

```bash
iperf3 -c SERVER_IP -p SERVER_PORT -t 15 -C cubic
```

Use the same server, port, and duration when comparing algorithms.

Public servers may impose connection limits, duration limits, or restrictions on upload tests.

Avoid `-R` when evaluating the local sender's congestion-control algorithm: reverse mode makes the remote server the sender.

Avoid `-u`: UDP does not use TCP congestion control.

## Inspecting the TCP Connection

While a test is running, inspect its TCP state:

```bash
ss -tin
```

Look for a connection using `cs422`.

The output may contain:

- `rtt`: Smoothed RTT and variation
- `cwnd`: Congestion window in segments
- `mss`: Maximum segment size
- `bytes_acked`: Acknowledged bytes
- `retrans`: Retransmission information

## Algorithm Parameters

The current implementation uses:

```c
#define MIN_CWND 1
#define ALPHA 2
#define RTT_THRESHOLD_US 10000
```

These are experimental parameters.

The algorithm maintains the minimum observed RTT as a baseline and estimates queueing delay as:

```text
queue_delay = current_RTT - base_RTT
```

At approximately one-RTT measurement intervals:

- A positive loss signal halves `cwnd`.
- High queueing delay with non-increasing throughput halves `cwnd`.
- High queueing delay with increasing throughput subtracts `ALPHA`.
- Low queueing delay with increasing throughput adds `ALPHA`.
- Otherwise, it adds `ALPHA / 2`.

The congestion window is constrained by the kernel's `snd_cwnd_clamp`.

## Known Limitations

This is an experimental implementation, not a production TCP congestion-control algorithm.

Known limitations include:

- No explicit slow-start implementation in the congestion-avoidance callback.
- No congestion-window-limited check before increasing `cwnd`.
- `ALPHA = 2` is used to avoid integer truncation of `ALPHA / 2`.
- The minimum RTT baseline does not expire.
- The `tp->lost` difference is not a reliable general-purpose packet loss-rate measurement.
- The RTT threshold is fixed at 10 ms.
- Very large congestion windows may cause iperf3's displayed window size to overflow.
- Performance can be substantially worse than CUBIC or Reno.

## Troubleshooting

### Missing `/sys/kernel/btf/vmlinux`

The running kernel does not expose the BTF data needed by this build.

Check:

```bash
uname -r
ls -l /sys/kernel/btf/vmlinux
```

You may need a different kernel.

### `Failed to load BPF object`

The kernel may not support a required BPF feature, type, callback, or instruction.

The loader enables libbpf diagnostic output. Inspect the verifier error messages.

### `Failed to attach struct_ops`

Possible causes include:

- Unsupported TCP BPF `struct_ops` functionality
- Incompatible kernel interfaces
- An algorithm named `cs422` already registered
- Insufficient permissions

Check:

```bash
sysctl net.ipv4.tcp_available_congestion_control
```

### `TCP congestion control not available`

Confirm that the loader is still running:

```bash
sysctl net.ipv4.tcp_available_congestion_control
```

If `cs422` is listed but the test process cannot select it, inspect:

```bash
sysctl net.ipv4.tcp_allowed_congestion_control
```

On kernels that restrict unprivileged selection, an administrator can temporarily add `cs422` to the allowed list while retaining the existing entries.

Alternatively, run a test with the required privileges.

### WSL2 issues

Check the kernel:

```bash
uname -r
```

WSL2 commonly reports a kernel name containing `microsoft-standard-WSL2`.

The presence of WSL2 alone does not guarantee support for every eBPF feature.

On Windows, WSL can be updated using:

```powershell
wsl --update
wsl --shutdown
```

Then restart the Ubuntu distribution.

## Unloading

Stop the loader using Ctrl+C.

The loader destroys the BPF attachment and closes the object.

Existing TCP connections should be allowed to finish before unloading the algorithm.

## Notes

The BPF program executes inside the Linux kernel.

The loader is a normal user-space program using libbpf to register the congestion-control callbacks.

The setup script regenerates `vmlinux.h` for the local kernel, avoiding reliance on a header generated from another machine.
