# CS422 TCP Congestion Control Using eBPF

A custom TCP congestion-control algorithm implemented using Linux eBPF `struct_ops` for Purdue CS422. The algorithm adjusts the congestion window based on packet loss, queueing delay, and acknowledged-byte throughput.

## Requirements

- Ubuntu 22.04+ on x86-64 Linux or WSL2, with kernel BTF and TCP BPF `struct_ops` support
- `sudo` privileges and internet access for initial setup
- Python 3 with dependencies from `requirements.txt`

## Setup

```bash
git clone <YOUR_REPOSITORY_URL> cs422-assignment3
cd cs422-assignment3
chmod +x setup.sh
./setup.sh
```

The setup script installs required dependencies, builds the eBPF program and loader, and registers the `cs422` congestion-control algorithm.

**Keep this terminal running after `CS422 READY` appears.** Press Ctrl+C to unregister the algorithm.

## Running Experiments

Open another terminal in the repository directory.

Install the Python dependencies if they are not already installed:

```bash
pip3 install -r requirements.txt
```

### Compare Congestion-Control Algorithms

Run five 10-second tests each for CS422, CUBIC, Reno, and BBR against the same iperf3 server:

```bash
python3 main.py --compare --host SERVER --port PORT --duration 10 --repetitions 5
```

The client uses a 200 ms TCP statistics sampling interval and rotates algorithm order between repetitions to reduce time-order bias. Tests run sequentially, with a 2-second delay between them.

### Test a Single Algorithm

```bash
python3 main.py --host SERVER --port PORT --algorithm cs422 --duration 10 --repetitions 5
```

Supported algorithms: `cs422`, `cubic`, `reno`, `bbr`.

### Test Using iperf3 Directly

```bash
iperf3 -c SERVER -p PORT -t 15 -C cs422
```

Use TCP upload tests, not UDP (`-u`) or reverse mode (`-R`).

## Results

Comparison experiments generate the following files in the repository directory:

| File | Contents |
|---|---|
| `comparison_tcp_stats_trace.csv` | Per-interval goodput, RTT, cwnd, and retransmissions |
| `comparison_summary_table.csv` | Statistics for each successful run |
| `comparison_aggregate_summary.csv` | Statistics aggregated across repetitions |
| `comparison_time_series.pdf` | Comparative TCP metrics over time |
| `comparison_scatter.pdf` | Relationships between TCP metrics and goodput |
| `comparison_destination_log.txt` | Successful and failed tests |

Plots show the first successful run of each algorithm. Aggregate statistics use all successful repetitions.

**Note:** Running another comparison overwrites these files. Save or rename results before starting another experiment.

## Algorithm Overview

The algorithm adjusts the congestion window approximately once per RTT using queueing delay, acknowledged-byte throughput, and packet-loss signals.

Queueing delay is estimated as:

```text
queue_delay = smoothed_RTT - base_RTT
```

The algorithm uses a 10 ms queueing-delay threshold.

- **Packet loss:** Decrease cwnd by 50%.
- **High queueing delay:** Decrease cwnd by 50% if throughput is not improving; otherwise decrease by 2%.
- **Low queueing delay:** Increase cwnd by 4% if throughput is improving; otherwise increase by 2%.

Slow start increases cwnd using acknowledgments until congestion is detected. All cwnd adjustments respect the kernel's congestion-window clamp.

## Limitations

- The queueing-delay threshold is fixed.
- Loss detection uses `tp->lost`, which is not a cumulative counter.
- RTT and throughput estimates are sensitive to network fluctuations.
- The algorithm prioritizes congestion avoidance and may achieve lower throughput than established algorithms.

This is an experimental implementation intended for coursework, not production use.

## Unloading

Press Ctrl+C in the terminal running `setup.sh` to detach the eBPF implementation and restore the original allowed congestion-control configuration.