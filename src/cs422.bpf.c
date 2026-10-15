#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

#define MIN_CWND 1
#define ALPHA 2
#define RTT_THRESHOLD_US 10000  // 10 ms, initial experimental value

struct cs422_state {
    u32 base_rtt_us;
    u64 previous_throughput;
    u64 previous_bytes_acked;
    u64 previous_time_us;
    u32 previous_lost;
};

struct cs422_measurement {
    u32 rtt_us;
    u32 queue_delay_us;
    u32 newly_lost;
    u64 throughput;
};

SEC("struct_ops/cs422_init")
void BPF_PROG(cs422_init, struct sock *sk)
{
    struct inet_connection_sock *icsk =
        (struct inet_connection_sock *)sk;

    struct cs422_state *state =
        (struct cs422_state *)icsk->icsk_ca_priv;

    state->base_rtt_us = 0;
    state->previous_throughput = 0;
    state->previous_bytes_acked = 0;
    state->previous_time_us = 0;
    state->previous_lost = 0;
}

static __always_inline void
cs422_measure(struct tcp_sock *tp,
              struct cs422_state *state,
              u64 now_us,
              struct cs422_measurement *m)
{
    m->rtt_us = tp->srtt_us >> 3;

    if (state->base_rtt_us == 0 ||
        m->rtt_us < state->base_rtt_us) {
        state->base_rtt_us = m->rtt_us;
    }

    m->queue_delay_us =
        m->rtt_us > state->base_rtt_us
            ? m->rtt_us - state->base_rtt_us
            : 0;

    u32 current_lost = tp->lost;

    m->newly_lost =
        current_lost >= state->previous_lost
            ? current_lost - state->previous_lost
            : 0;

    u64 elapsed_us = now_us - state->previous_time_us;
    u64 newly_acked = tp->bytes_acked -
                      state->previous_bytes_acked;

    m->throughput = elapsed_us > 0
        ? (newly_acked * 1000000ULL) / elapsed_us
        : 0;
}

static __always_inline void
cs422_update_cwnd(struct tcp_sock *tp,
                  struct cs422_state *state,
                  struct cs422_measurement *m)
{
    u32 cwnd = tp->snd_cwnd;
    u32 alpha = ALPHA;

    // if loss_rate > 0:
    if (m->newly_lost > 0) {
        cwnd = cwnd / 2;  // beta = 0.5
        if (cwnd < MIN_CWND)
            cwnd = MIN_CWND;
    }

    // elif queue_delay > RTT_threshold:
    else if (m->queue_delay_us > RTT_THRESHOLD_US) {

        // if throughput <= previous_throughput:
        if (m->throughput <= state->previous_throughput) {
            cwnd = cwnd / 2;
            if (cwnd < MIN_CWND)
                cwnd = MIN_CWND;
        }

        // else: cwnd = max(min_cwnd, cwnd - alpha)
        else {
            cwnd = cwnd > alpha ? cwnd - alpha : MIN_CWND;
        }
    }

    // else: queue_delay <= RTT_threshold
    else {
        // if throughput > previous_throughput:
        if (m->throughput > state->previous_throughput) {
            cwnd += alpha;
        }

        // else: cwnd += alpha / 2
        else {
            cwnd += alpha / 2;
        }
    }

    // Respect Linux's congestion-window clamp.
    if (cwnd > tp->snd_cwnd_clamp)
        cwnd = tp->snd_cwnd_clamp;

    tp->snd_cwnd = cwnd;

    // previous_throughput = throughput
    state->previous_throughput = m->throughput;
}

SEC("struct_ops/cs422_cong_avoid")
void BPF_PROG(cs422_cong_avoid,
              struct sock *sk, u32 ack, u32 acked)
{
    struct tcp_sock *tp = (struct tcp_sock *)sk;
    struct inet_connection_sock *icsk =
        (struct inet_connection_sock *)sk;

    struct cs422_state *state =
        (struct cs422_state *)icsk->icsk_ca_priv;

    u64 now_us = bpf_ktime_get_ns() / 1000;
    u32 rtt_us = tp->srtt_us >> 3;

    // Wait until a valid RTT measurement exists.
    if (rtt_us == 0)
        return;

    // Start the first measurement interval.
    if (state->previous_time_us == 0) {
        state->previous_time_us = now_us;
        state->previous_bytes_acked = tp->bytes_acked;
        state->previous_lost = tp->lost;
        state->base_rtt_us = rtt_us;
        return;
    }

    // One measurement interval ~= one RTT.
    if (now_us - state->previous_time_us < rtt_us)
        return;

    struct cs422_measurement m = {};

    cs422_measure(tp, state, now_us, &m);
    cs422_update_cwnd(tp, state, &m);

    // Save measurements for the next interval.
    state->previous_bytes_acked = tp->bytes_acked;
    state->previous_lost = tp->lost;
    state->previous_time_us = now_us;
}

SEC("struct_ops/cs422_ssthresh")
u32 BPF_PROG(cs422_ssthresh, struct sock *sk)
{
    struct tcp_sock *tp = (struct tcp_sock *)sk;

    u32 cwnd = tp->snd_cwnd;
    u32 half = cwnd / 2;

    return half > 2 ? half : 2;
}

SEC("struct_ops/cs422_undo_cwnd")
u32 BPF_PROG(cs422_undo_cwnd, struct sock *sk)
{
    struct tcp_sock *tp = (struct tcp_sock *)sk;

    u32 cwnd = tp->snd_cwnd;
    u32 prior = tp->prior_cwnd;

    return cwnd > prior ? cwnd : prior;
}

SEC(".struct_ops")
struct tcp_congestion_ops cs422 = {
    .name       = "cs422",
    .init       = (void *)cs422_init,
    .ssthresh   = (void *)cs422_ssthresh,
    .cong_avoid = (void *)cs422_cong_avoid,
    .undo_cwnd  = (void *)cs422_undo_cwnd,
};