#pragma once

// The ping counter's wire format and arithmetic (hle/net.cpp), as code the
// P2P port and tests/ping_test.cpp share. No sockets and no clock: every time
// is passed in, in ns of the steady clock.
//
// A ping is a hole-punch probe with a kind in byte 5 - 'P' a ping, 'Q' its
// pong - then a version byte, a pad byte, the sequence (u32) and the pinger's
// steady clock in ns (u64), both little-endian:
//   [0xfe 'b' 'b' 'h' 'p'][kind][version][0][seq u32 LE][clock u64 LE]
// The pong echoes the ping's sequence and clock, so the round trip is the
// pinger's own clock difference and the two machines' clocks never meet.
//
// Best effort: a client without the counter drops both unseen. Every P2P
// reader on The Hunter's Dream drops a datagram of 8 bytes or more that starts
// with the probe's five bytes before its game sees it - a bbhost from before
// the counter, and the clients whose readers were adapted from bbhost's
// (LanternEngine e6b943d engine/gpu/bbnet/thd/thd_net.cpp, bloodborne_pc
// 0fc2921 gpu/bbnet20/thd/thd_net.cpp). The game's own datagrams are never
// touched: pings are datagrams of their own.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <utility>

namespace net::ping {

constexpr std::uint8_t kProbePrefix[5] = {0xfe, 'b', 'b', 'h', 'p'};
constexpr std::uint8_t kPing = 'P', kPong = 'Q', kVersion = 1;
constexpr std::size_t kLen = 20;
constexpr int kIntervalMs = 5000;
// Pings to a peer that has never answered before giving up on it: no more
// than a hole-punch sends, to a client that drops them.
constexpr std::uint32_t kTries = 3;
// A peer whose last pong is older than this reads as lost.
constexpr std::int64_t kStaleNs = 12'000'000'000;
// A pong whose clock is further back than this is not from this run's pings.
constexpr std::int64_t kMaxRttNs = 10'000'000'000;
// Pongs sent a second at most: a pong is the size of its ping, so no
// amplifier; this caps what a flood of pings can cost.
constexpr int kPongsPerSecond = 64;

inline void build(std::uint8_t (&pkt)[kLen], std::uint8_t kind, std::uint32_t seq, std::uint64_t t_ns) {
    std::memcpy(pkt, kProbePrefix, sizeof(kProbePrefix));
    pkt[5] = kind;
    pkt[6] = kVersion;
    pkt[7] = 0;
    for (int i = 0; i < 4; ++i) pkt[8 + i] = static_cast<std::uint8_t>(seq >> (8 * i));
    for (int i = 0; i < 8; ++i) pkt[12 + i] = static_cast<std::uint8_t>(t_ns >> (8 * i));
}

struct Packet {
    std::uint8_t kind = 0;  // kPing or kPong
    std::uint32_t seq = 0;
    std::uint64_t t_ns = 0;
};
// A ping or a pong of this version; false for anything else, a plain
// hole-punch probe included.
inline bool parse(const std::uint8_t* d, std::size_t n, Packet* out) {
    if (n < kLen || std::memcmp(d, kProbePrefix, sizeof(kProbePrefix)) != 0) return false;
    if ((d[5] != kPing && d[5] != kPong) || d[6] != kVersion) return false;
    out->kind = d[5];
    out->seq = 0;
    for (int i = 0; i < 4; ++i) out->seq |= static_cast<std::uint32_t>(d[8 + i]) << (8 * i);
    out->t_ns = 0;
    for (int i = 0; i < 8; ++i) out->t_ns |= static_cast<std::uint64_t>(d[12 + i]) << (8 * i);
    return true;
}

// A pong's round trip in ms, or -1 when it is no news: only an answer to a
// ping of ours still unanswered - a newer sequence than the last answered, up
// to the last sent - with a clock from the last kMaxRttNs.
inline int pong_ms(const Packet& p, std::uint32_t answered_seq, std::uint32_t last_sent_seq, std::int64_t now_ns) {
    if (p.kind != kPong || p.seq <= answered_seq || p.seq > last_sent_seq) return -1;
    const std::int64_t rtt = now_ns - static_cast<std::int64_t>(p.t_ns);
    if (rtt < 0 || rtt > kMaxRttNs) return -1;
    return static_cast<int>((rtt + 500'000) / 1'000'000);
}

// Whether a peer gets another ping: one that answered always does (it may
// answer again), one that never did only for its first kTries.
inline bool keep_pinging(std::uint32_t sent, std::uint32_t answered) { return answered > 0 || sent < kTries; }

struct PongLimiter {
    std::int64_t window_ns = 0;
    int count = 0;
    bool allow(std::int64_t now_ns) {
        if (now_ns - window_ns >= 1'000'000'000) {
            window_ns = now_ns;
            count = 0;
        }
        return ++count <= kPongsPerSecond;
    }
};

// The round trip read from the game's own traffic, for a peer that does not
// answer pings. The game's reliable transport, both vports (40 and 30), frames
// its datagrams as messages laid end to end:
//   [0x01][length, u16 BE, of what follows][seq u16 BE][ack u16 BE][kind][0xff]...
// kind 0x01 connects, 0x03 carries data, 0x30 is a bare acknowledgement of
// `ack`, 0x33 data with one. A message with a data bit (kind & 0x0f) and a
// nonzero seq waits for the peer to acknowledge that seq; the time between is
// a round trip, plus however long the peer held its acknowledgement (about
// 200 ms at most when it has nothing to send back). The lowest sample over a
// few seconds is the network's round trip. Read from a dump of a session with
// a player on a release (2026-11-01): 53 round trips on vport 30 with a floor
// of 236 ms, 35 on vport 40 with 254. A datagram that is not exactly such
// messages (the session's other stream, messages of kind 0x02 with no 0xff)
// is left alone, as is a sequence sent twice: its acknowledgement cannot say
// which copy it answers.
constexpr std::int64_t kAckWindowNs = 10'000'000'000;
constexpr std::int64_t kAckWaitNs = 5'000'000'000;  // an unacknowledged seq is forgotten after this
constexpr int kMaxMsgs = 32;

struct GameMsg {
    std::uint16_t seq = 0, ack = 0;
    std::uint8_t kind = 0;
};
// The messages of one game datagram (its payload after the vport header); 0
// when it is not exactly messages, or more than `max`.
inline int game_msgs(const std::uint8_t* d, std::size_t n, GameMsg* out, int max) {
    int count = 0;
    std::size_t i = 0;
    while (i < n) {
        if (i + 9 > n || d[i] != 0x01 || d[i + 8] != 0xff || count == max) return 0;
        const std::size_t len = static_cast<std::size_t>((d[i + 1] << 8) | d[i + 2]);
        if (len < 6 || i + 3 + len > n) return 0;
        out[count].seq = static_cast<std::uint16_t>((d[i + 3] << 8) | d[i + 4]);
        out[count].ack = static_cast<std::uint16_t>((d[i + 5] << 8) | d[i + 6]);
        out[count].kind = d[i + 7];
        ++count;
        i += 3 + len;
    }
    return count;
}

// One peer, one vport: one sequence space, and the round trips read from it.
class AckChannel {
public:
    // Our messages to the peer: each data sequence's first sending.
    void on_sent(const GameMsg* m, int k, std::int64_t now_ns) {
        touched_ns_ = now_ns;
        for (int i = 0; i < k; ++i) {
            if (!(m[i].kind & 0x0f) || !m[i].seq) continue;
            const std::uint16_t seq = m[i].seq;
            auto it = sent_.find(seq);
            const bool resent_before = std::find(resent_.begin(), resent_.end(), seq) != resent_.end();
            if (it != sent_.end() || resent_before) {
                if (it != sent_.end()) sent_.erase(it);
                if (!resent_before) {
                    resent_.push_back(seq);
                    if (resent_.size() > kResentKept) resent_.pop_front();
                }
                continue;
            }
            if (sent_.size() >= kSentKept) sent_.clear();  // a peer that never acknowledges: start over
            sent_[seq] = now_ns;
        }
    }
    // The peer's messages: an acknowledgement of a sequence waiting is a sample.
    void on_received(const GameMsg* m, int k, std::int64_t now_ns) {
        touched_ns_ = now_ns;
        for (int i = 0; i < k; ++i) {
            if ((m[i].kind & 0x30) != 0x30) continue;
            auto it = sent_.find(m[i].ack);
            if (it == sent_.end()) continue;
            const std::int64_t rtt = now_ns - it->second;
            sent_.erase(it);
            if (rtt < 0 || rtt > kAckWaitNs) continue;
            samples_.emplace_back(now_ns, static_cast<int>((rtt + 500'000) / 1'000'000));
            if (samples_.size() > kSamplesKept) samples_.pop_front();
        }
    }
    // Drops samples older than the window and sequences waited on too long.
    void expire(std::int64_t now_ns) {
        while (!samples_.empty() && now_ns - samples_.front().first > kAckWindowNs) samples_.pop_front();
        for (auto s = sent_.begin(); s != sent_.end();) s = now_ns - s->second > kAckWaitNs ? sent_.erase(s) : std::next(s);
    }
    // The lowest sample kept, -1 for none.
    int lowest_ms() const {
        int lowest = -1;
        for (const auto& [t, ms] : samples_) {
            if (lowest < 0 || ms < lowest) lowest = ms;
        }
        return lowest;
    }
    // Samples taken after `since_ns`.
    std::uint32_t samples_after(std::int64_t since_ns) const {
        std::uint32_t n = 0;
        for (const auto& [t, ms] : samples_) {
            if (t > since_ns) ++n;
        }
        return n;
    }
    std::int64_t touched_ns() const { return touched_ns_; }
    std::size_t waiting() const { return sent_.size(); }

    static constexpr std::size_t kSentKept = 512, kResentKept = 64, kSamplesKept = 256;

private:
    std::map<std::uint16_t, std::int64_t> sent_;        // seq -> when we first sent it
    std::deque<std::uint16_t> resent_;                  // the last sequences we sent more than once
    std::deque<std::pair<std::int64_t, int>> samples_;  // (when, ms)
    std::int64_t touched_ns_ = 0;
};

}  // namespace net::ping
