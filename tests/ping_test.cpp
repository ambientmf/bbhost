// net/ping_wire.h: the ping counter's datagrams and arithmetic. First that a
// ping and a pong are dropped unseen by a client without the counter - the
// rule every P2P reader on The Hunter's Dream keeps (bbhost's own before the
// counter, LanternEngine's, bloodborne_pc's) - and never read as a game
// datagram, a STUN answer or a relay frame; then the pong's checks, the pong
// cap, giving up on a silent peer, and the round trip read from the game's
// own messages and acknowledgements.
#include "net/ping_wire.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(c)                                                      \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                 \
        }                                                             \
    } while (0)

using namespace net::ping;

constexpr std::int64_t kMs = 1'000'000;

// The other clients' reader, as each has it (and bbhost's own): a datagram
// with no P2P header that is 8 bytes or more and starts with the probe's five
// is a hole-punch probe, dropped before the game sees it.
bool other_client_drops(const std::uint8_t* d, std::size_t n) {
    static const std::uint8_t probe[8] = {0xfe, 'b', 'b', 'h', 'p', 0, 0, 0};
    return n >= 8 && std::memcmp(d, probe, 5) == 0;
}

void wire_format() {
    std::uint8_t pkt[kLen];
    build(pkt, kPing, 0x01020304u, 0x1122334455667788ull);
    const std::uint8_t want[kLen] = {0xfe, 'b',  'b',  'h',  'p',  'P',  1,    0,    0x04, 0x03,
                                     0x02, 0x01, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    CHECK(std::memcmp(pkt, want, kLen) == 0);

    Packet p;
    CHECK(parse(pkt, kLen, &p));
    CHECK(p.kind == kPing && p.seq == 0x01020304u && p.t_ns == 0x1122334455667788ull);
    build(pkt, kPong, 7, 99);
    CHECK(parse(pkt, kLen, &p));
    CHECK(p.kind == kPong && p.seq == 7 && p.t_ns == 99);
}

void old_clients_never_see_one() {
    for (std::uint8_t kind : {kPing, kPong}) {
        std::uint8_t pkt[kLen];
        build(pkt, kind, 1, 1);
        CHECK(other_client_drops(pkt, sizeof(pkt)));
        // Not a game datagram (the PS4's P2P header starts 0xff), not a
        // STUN answer (0x0101), not a relay frame (0xfb).
        CHECK(pkt[0] != 0xff);
        CHECK(!(pkt[0] == 0x01 && pkt[1] == 0x01));
        CHECK(pkt[0] != 0xfb);
    }
}

void parse_rejects() {
    std::uint8_t pkt[kLen];
    build(pkt, kPing, 1, 1);
    Packet p;
    CHECK(!parse(pkt, kLen - 1, &p));  // short
    // A plain hole-punch probe is no ping.
    const std::uint8_t probe[8] = {0xfe, 'b', 'b', 'h', 'p', 0, 0, 0};
    CHECK(!parse(probe, sizeof(probe), &p));
    std::uint8_t bad[kLen];
    std::memcpy(bad, pkt, kLen);
    bad[6] = kVersion + 1;  // a later version: left alone, not misread
    CHECK(!parse(bad, kLen, &p));
    std::memcpy(bad, pkt, kLen);
    bad[5] = 'X';  // another kind
    CHECK(!parse(bad, kLen, &p));
    std::memcpy(bad, pkt, kLen);
    bad[0] = 0xff;  // not a probe at all
    CHECK(!parse(bad, kLen, &p));
}

void pong_checks() {
    const std::int64_t sent_at = 50'000 * kMs;
    Packet pong{kPong, 3, static_cast<std::uint64_t>(sent_at)};
    // Answered 2, sent up to 4: seq 3 is news, 42 ms after (rounded).
    CHECK(pong_ms(pong, 2, 4, sent_at + 42 * kMs + 400'000) == 42);
    CHECK(pong_ms(pong, 2, 4, sent_at + 42 * kMs + 600'000) == 43);
    CHECK(pong_ms(pong, 3, 4, sent_at + 42 * kMs) == -1);  // already answered (a duplicate)
    CHECK(pong_ms(pong, 4, 4, sent_at + 42 * kMs) == -1);  // older than the last answered
    CHECK(pong_ms(pong, 0, 2, sent_at + 42 * kMs) == -1);  // a sequence never sent
    CHECK(pong_ms(pong, 2, 4, sent_at - kMs) == -1);       // a clock from the future
    CHECK(pong_ms(pong, 2, 4, sent_at + kMaxRttNs + kMs) == -1);  // too old: not this run's
    Packet ping{kPing, 3, static_cast<std::uint64_t>(sent_at)};
    CHECK(pong_ms(ping, 2, 4, sent_at + 42 * kMs) == -1);  // a ping is not an answer
}

void pong_cap() {
    PongLimiter lim;
    const std::int64_t t0 = 5'000 * kMs;
    int allowed = 0;
    for (int i = 0; i < 200; ++i) allowed += lim.allow(t0 + i * kMs) ? 1 : 0;  // 200 in 200 ms
    CHECK(allowed == kPongsPerSecond);
    CHECK(!lim.allow(t0 + 999 * kMs));
    CHECK(lim.allow(t0 + 1000 * kMs));  // a new second
}

void gives_up_on_silence() {
    CHECK(keep_pinging(0, 0));
    CHECK(keep_pinging(kTries - 1, 0));
    CHECK(!keep_pinging(kTries, 0));    // never answered: no more pings
    CHECK(keep_pinging(kTries, 1));     // answered once: keeps going
    CHECK(keep_pinging(1000, 1));
}

// One game message: [0x01][len u16 BE][seq][ack][kind][0xff] + payload.
void msg(std::vector<std::uint8_t>& v, std::uint16_t seq, std::uint16_t ack, std::uint8_t kind, std::size_t payload = 0) {
    const std::size_t len = 6 + payload;
    v.push_back(0x01);
    v.push_back(static_cast<std::uint8_t>(len >> 8));
    v.push_back(static_cast<std::uint8_t>(len));
    v.push_back(static_cast<std::uint8_t>(seq >> 8));
    v.push_back(static_cast<std::uint8_t>(seq));
    v.push_back(static_cast<std::uint8_t>(ack >> 8));
    v.push_back(static_cast<std::uint8_t>(ack));
    v.push_back(kind);
    v.push_back(0xff);
    for (std::size_t i = 0; i < payload; ++i) v.push_back(static_cast<std::uint8_t>(i + 1));
}

void game_message_parsing() {
    GameMsg m[kMaxMsgs];
    std::vector<std::uint8_t> d;
    msg(d, 0x1234, 0x0007, 0x03, 20);
    msg(d, 0, 0x1200, 0x30);
    CHECK(game_msgs(d.data(), d.size(), m, kMaxMsgs) == 2);
    CHECK(m[0].seq == 0x1234 && m[0].ack == 0x0007 && m[0].kind == 0x03);
    CHECK(m[1].seq == 0 && m[1].ack == 0x1200 && m[1].kind == 0x30);

    CHECK(game_msgs(d.data(), d.size() - 1, m, kMaxMsgs) == 0);  // truncated
    std::vector<std::uint8_t> e = d;
    e[8] = 0x00;  // no 0xff marker
    CHECK(game_msgs(e.data(), e.size(), m, kMaxMsgs) == 0);
    e = d;
    e[0] = 0x02;  // the session's other stream
    CHECK(game_msgs(e.data(), e.size(), m, kMaxMsgs) == 0);
    e = d;
    e[1] = 0;
    e[2] = 5;  // a length below the header's
    CHECK(game_msgs(e.data(), e.size(), m, kMaxMsgs) == 0);
    e = d;
    e.push_back(0x01);  // trailing bytes that are not a message
    CHECK(game_msgs(e.data(), e.size(), m, kMaxMsgs) == 0);
    CHECK(game_msgs(d.data(), 0, m, kMaxMsgs) == 0);
    CHECK(game_msgs(d.data(), d.size(), m, 1) == 0);  // more messages than room for

    // A ping is no game datagram either.
    std::uint8_t pkt[kLen];
    build(pkt, kPing, 1, 1);
    CHECK(game_msgs(pkt, sizeof(pkt), m, kMaxMsgs) == 0);
}

void sent(AckChannel& ch, std::uint16_t seq, std::uint8_t kind, std::int64_t at) {
    GameMsg m{seq, 0, kind};
    ch.on_sent(&m, 1, at);
}
void acked(AckChannel& ch, std::uint16_t ack, std::uint8_t kind, std::int64_t at) {
    GameMsg m{0, ack, kind};
    ch.on_received(&m, 1, at);
}

void ack_round_trips() {
    const std::int64_t t0 = 100'000 * kMs;
    {
        AckChannel ch;
        sent(ch, 10, 0x03, t0);
        acked(ch, 10, 0x30, t0 + 120 * kMs);
        CHECK(ch.lowest_ms() == 120);
        CHECK(ch.samples_after(t0) == 1);
        acked(ch, 10, 0x30, t0 + 300 * kMs);  // the same ack again: no second sample
        CHECK(ch.samples_after(t0) == 1);
        sent(ch, 11, 0x03, t0 + 1000 * kMs);
        acked(ch, 11, 0x33, t0 + 1090 * kMs);  // data with an ack counts too
        CHECK(ch.lowest_ms() == 90);
        CHECK(ch.samples_after(t0) == 2);
    }
    {
        AckChannel ch;  // a sequence sent twice gives no sample, then or later
        sent(ch, 20, 0x03, t0);
        sent(ch, 20, 0x03, t0 + 200 * kMs);
        acked(ch, 20, 0x30, t0 + 250 * kMs);
        CHECK(ch.lowest_ms() == -1);
        sent(ch, 20, 0x03, t0 + 400 * kMs);
        acked(ch, 20, 0x30, t0 + 450 * kMs);
        CHECK(ch.lowest_ms() == -1);
    }
    {
        AckChannel ch;  // what waits for no ack
        sent(ch, 0, 0x03, t0);     // seq 0
        sent(ch, 30, 0x30, t0);    // a bare ack carries no data
        CHECK(ch.waiting() == 0);
        sent(ch, 31, 0x03, t0);
        acked(ch, 31, 0x03, t0 + 50 * kMs);  // data without the ack bits acknowledges nothing
        CHECK(ch.lowest_ms() == -1);
        acked(ch, 99, 0x30, t0 + 60 * kMs);  // an ack of something never sent
        CHECK(ch.lowest_ms() == -1);
    }
    {
        AckChannel ch;  // too late an ack is no sample; old ones leave the window
        sent(ch, 40, 0x03, t0);
        acked(ch, 40, 0x30, t0 + kAckWaitNs + kMs);
        CHECK(ch.lowest_ms() == -1);
        sent(ch, 41, 0x03, t0);
        acked(ch, 41, 0x30, t0 + 80 * kMs);
        sent(ch, 42, 0x03, t0 + 5000 * kMs);
        acked(ch, 42, 0x30, t0 + 5200 * kMs);
        ch.expire(t0 + 80 * kMs + kAckWindowNs + kMs);  // the 80 ms sample ages out
        CHECK(ch.lowest_ms() == 200);
        sent(ch, 43, 0x03, t0 + 20'000 * kMs);
        ch.expire(t0 + 20'000 * kMs + kAckWaitNs + kMs);  // waited on too long
        CHECK(ch.waiting() == 0);
    }
    {
        AckChannel ch;  // a peer that never acknowledges cannot grow it without bound
        for (std::uint16_t s = 1; s <= 2000; ++s) sent(ch, s, 0x03, t0);
        CHECK(ch.waiting() <= AckChannel::kSentKept);
    }
}

}  // namespace

int main() {
    wire_format();
    old_clients_never_see_one();
    parse_rejects();
    pong_checks();
    pong_cap();
    gives_up_on_silence();
    game_message_parsing();
    ack_round_trips();
    if (g_fail) {
        std::printf("ping_test: %d failed\n", g_fail);
        return 1;
    }
    std::printf("ping_test: ok\n");
    return 0;
}
