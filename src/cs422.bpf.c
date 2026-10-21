#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

#define MIN_CWND 1
#define RTT_THRESHOLD_US 10000
#define RTT_BUCKETS 6
#define MEASUREMENTS_PER_BUCKET 5

struct rtt_window {
    u32 minimums[RTT_BUCKETS];
    u32 current_bucket;
    u32 samples_in_bucket;
};

struct cs422_state {
    struct rtt_window rtt;
    u64 previous_throughput;
    u64 previous_bytes_acked;
    u64 previous_time_us;
    u32 previous_lost;
};

_Static_assert(
    sizeof(struct cs422_state) <=
    sizeof(((struct inet_connection_sock *)0)->icsk_ca_priv),
    "cs422_state exceeds TCP congestion-control private storage"
);

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
    __builtin_memset(&state->rtt, 0, sizeof(state->rtt));
    state->previous_throughput = 0;
    state->previous_bytes_acked = 0;
    state->previous_time_us = 0;
    state->previous_lost = 0;
}

static __always_inline u32
update_rtt_window(struct rtt_window *window, u32 rtt_us)
{
    // Work on a local copy to avoid variable-offset
    // accesses into kernel socket memory.
    struct rtt_window local = *window;

    u32 index = local.current_bucket;

    if (index >= RTT_BUCKETS)
        index = 0;

    // Update the minimum RTT in the current bucket.
    // Constant indices keep the eBPF verifier happy.
    #define UPDATE_BUCKET(i)                         \
        case i:                                      \
            if (local.samples_in_bucket == 0 ||      \
                rtt_us < local.minimums[i])          \
                local.minimums[i] = rtt_us;          \
            break;

    switch (index) {
        UPDATE_BUCKET(0)
        UPDATE_BUCKET(1)
        UPDATE_BUCKET(2)
        UPDATE_BUCKET(3)
        UPDATE_BUCKET(4)
        UPDATE_BUCKET(5)
    }

    #undef UPDATE_BUCKET

    local.samples_in_bucket++;

    // Advance to the next bucket after 5 measurements.
    if (local.samples_in_bucket == MEASUREMENTS_PER_BUCKET) {
        local.samples_in_bucket = 0;
        local.current_bucket = (index + 1) % RTT_BUCKETS;

        // Clear the expired bucket using constant indices.
        switch (local.current_bucket) {
            case 0: local.minimums[0] = 0; break;
            case 1: local.minimums[1] = 0; break;
            case 2: local.minimums[2] = 0; break;
            case 3: local.minimums[3] = 0; break;
            case 4: local.minimums[4] = 0; break;
            case 5: local.minimums[5] = 0; break;
        }
    } else {
        local.current_bucket = index;
    }

    // Find the minimum RTT across all 6 buckets.
    u32 base_rtt = rtt_us;

    #pragma unroll
    for (int i = 0; i < RTT_BUCKETS; i++) {
        u32 sample = local.minimums[i];

        if (sample != 0 && sample < base_rtt)
            base_rtt = sample;
    }

    // Save the updated window back to connection state.
    *window = local;

    return base_rtt;
}

static __always_inline void
cs422_measure(struct tcp_sock *tp,
              struct cs422_state *state,
              u64 now_us,
              struct cs422_measurement *m)
{
    m->rtt_us = tp->srtt_us >> 3;

    u32 base_rtt_us =
    update_rtt_window(&state->rtt, m->rtt_us);

    m->queue_delay_us = m->rtt_us - base_rtt_us;

    u32 current_lost = tp->lost;

    m->newly_lost = current_lost >= state->previous_lost ? current_lost - state->previous_lost : 0;

    u64 elapsed_us = now_us - state->previous_time_us;
    u64 newly_acked = tp->bytes_acked - state->previous_bytes_acked;

    m->throughput = elapsed_us > 0 ? (newly_acked * 1000000ULL) / elapsed_us : 0;
}

static __always_inline void
cs422_update_cwnd(struct tcp_sock *tp,
                  struct cs422_state *state,
                  struct cs422_measurement *m)
{
    u32 cwnd = tp->snd_cwnd;

    if (m->newly_lost > 0) {
        cwnd = cwnd / 2;

        if (cwnd < MIN_CWND)
            cwnd = MIN_CWND;
    }
    else if (m->queue_delay_us > RTT_THRESHOLD_US) {
        if (m->throughput <= state->previous_throughput) {
            cwnd = cwnd / 2;

            if (cwnd < MIN_CWND)
                cwnd = MIN_CWND;
        }
        else {
            // High queue delay, but throughput improving: -2%
            u32 decrease = cwnd / 50;

            if (decrease < 1)
                decrease = 1;

            cwnd = cwnd > decrease ? cwnd - decrease : MIN_CWND;
        }
    }
    else {
        if (m->throughput > state->previous_throughput) {
            // Low queue delay, throughput improving: +4%
            u32 increase = cwnd / 25;

            if (increase < 1)
                increase = 1;

            cwnd += increase;
        }
        else {
            // Low queue delay, throughput not improving: +2%
            u32 increase = cwnd / 50;

            if (increase < 1)
                increase = 1;

            cwnd += increase;
        }
    }

    if (cwnd > tp->snd_cwnd_clamp)
        cwnd = tp->snd_cwnd_clamp;

    tp->snd_cwnd = cwnd;
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

    if (rtt_us == 0)
        return;

    if (state->previous_time_us == 0) {
        state->previous_time_us = now_us;
        state->previous_bytes_acked = tp->bytes_acked;
        state->previous_lost = tp->lost;
    }

    if (now_us - state->previous_time_us >= rtt_us) {
        struct cs422_measurement m = {};

        cs422_measure(tp, state, now_us, &m);

        bool congestion_detected =
            m.newly_lost > 0 ||
            m.queue_delay_us > RTT_THRESHOLD_US;

        if (congestion_detected) {
            bool was_in_slow_start =
                tp->snd_cwnd < tp->snd_ssthresh;

            cs422_update_cwnd(tp, state, &m);

            if (was_in_slow_start)
                tp->snd_ssthresh = tp->snd_cwnd;
        }
        else if (tp->snd_cwnd >= tp->snd_ssthresh) {
            cs422_update_cwnd(tp, state, &m);
        }
        else {
            state->previous_throughput = m.throughput;
        }

        state->previous_bytes_acked = tp->bytes_acked;
        state->previous_lost = tp->lost;
        state->previous_time_us = now_us;
    }

    if (tp->snd_cwnd < tp->snd_ssthresh) {
        u32 limit = tp->snd_ssthresh;

        if (limit > tp->snd_cwnd_clamp)
            limit = tp->snd_cwnd_clamp;

        if (tp->snd_cwnd < limit) {
            u32 increase = acked;

            if (increase > limit - tp->snd_cwnd)
                increase = limit - tp->snd_cwnd;

            tp->snd_cwnd += increase;
        }
    }
}

SEC("struct_ops/cs422_ssthresh")
u32 BPF_PROG(cs422_ssthresh, struct sock *sk)
{
    struct tcp_sock *tp = (struct tcp_sock *)sk;

    u32 threshold = tp->snd_cwnd / 2;

    if (threshold < 2)
        threshold = 2;

    return threshold;
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