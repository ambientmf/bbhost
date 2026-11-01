// What the session layer asks of the P2P port (hle/net.cpp) for players
// behind NAT: the reflexive address of the port through a STUN
// Binding exchange on the port's own socket, and hole-punch probes toward a
// peer so this side's NAT opens for the peer's datagrams before the game's
// own traffic starts.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/stun.h"

// Sends a STUN Binding Request from the P2P port (opened if the game has not
// bound it yet) to host:port and waits up to timeout_ms for the response.
// True with the mapped address (network byte order) and port (host order).
// With `relay`, the request carries the relay HELLO (net/stun.h) and the
// answer's BBHOST-RELAY lands there; a relay in the answer turns framing on
// for datagrams to that server's other ports, an answer without one off.
bool hle_net_p2p_stun(const char* host, std::uint16_t port, int timeout_ms, std::uint32_t* mapped_addr,
                      std::uint16_t* mapped_port, net::stun::Relay* relay = nullptr);

// Whether datagrams go through the server's relay: its address (network
// order) and our port on it.
bool hle_net_p2p_relay(std::uint32_t* server, std::uint16_t* vport);

// Starts sending small probe datagrams from the P2P port to addr:port and,
// when different, local_addr:local_port (network byte order, host order),
// every 500 ms for up to 20 s or until anything arrives from either. The
// probes carry their own magic and the receiving port drops them; the game
// never sees them. `label` names the peer in the log. A peer behind the
// relay gets none: the relay needs no hole.
void hle_net_p2p_punch(const char* label, std::uint32_t addr, std::uint16_t port, std::uint32_t local_addr,
                       std::uint16_t local_port);

// The ping counter (host setting ping_counter, host/window.cpp). Once the P2P
// port has opened, a thread pings each other player in the game's Matching2
// room (the world's co-operators and invaders, not the players nearby the game
// connects to for ghosts and signs) every 5 s, at the address the game sends
// to (through the relay when the game's datagrams go that way); a bbhost with
// the counter answers on its port's reader, one without drops the ping as a
// hole-punch probe. For every peer the round trip is also read from the game's
// own reliable messages and their acknowledgements (P2pPing::game_ms). `net:
// ping` lines in the log: the first answer, silence, a summary a minute.
void hle_net_p2p_pinger_start();
struct P2pPing {
    std::string online_id;
    int last_ms = -1;   // the last answered ping's round trip (-1: none answered yet)
    int avg_ms = -1;    // smoothed over the last few
    bool lost = false;  // three pings and no answer yet, or none for 12 s
    // The round trip read from the game's own messages and their
    // acknowledgements (any peer, with or without the counter): the lowest
    // over the last 10 s, as of the last 5 s pass. -1: none in that time.
    int game_ms = -1;
};
std::vector<P2pPing> hle_net_p2p_pings();
