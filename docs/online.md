# Online play

Bloodborne's online features - co-op, invasions, summon signs, messages,
bloodstains, the play log behind ghosts - are built on the PS4's
PlayStation Network libraries (NP Matching2, Signaling, WebAPI, Auth) and
FromSoftware's own servers. bbhost implements those libraries against a
private server, so the game's online code runs unchanged and bbhost players
can play with each other.

PS4 cross-play is not a goal: bbhost players meet only other bbhost players.

## Connecting

The setup window chooses the server (the public one, a playtest server, or
your own) and links an account. In `bbhost.toml`, the public server's
settings, which are the defaults:

```toml
[online]
host = "thehuntersdream.com"   # replaces FromSoftware's hosts in the game's requests
scheme = "https"               # the public server takes only https
verify_tls = true              # and its certificate is checked
require_account = true         # stay offline until this PC is signed in
auth_server = "https://thehuntersdream.com"   # where the account calls go
```

A configuration written by an older bbhost that still sends the public server
plain http is updated to these settings at the next start.

Accounts have no passwords: a PC is linked to an account (with Discord, or
with an account held by that PC plus a recovery code), and the account's name
is the in-game online name. F10 has the same account screen in game.

## How a session connects

- **Matchmaking**: the game's Matching2 calls (create, search and join
  rooms, member events) go to the server's session service. The game's own
  rules decide who can be summoned or invade; bbhost only carries them.
- **Peer to peer**: players exchange game traffic over UDP directly, as on
  the console. Each instance has its own P2P port (`online.p2p_port`, default
  9307; a second instance on one machine takes the next free one).
- **NAT**: the P2P port learns its public address from a STUN server
  (`online.stun_server`) and both sides punch holes towards each other. When
  that fails, the server can relay the traffic.

## Beyond the console

- **Five players**: the late areas (Mensis, Mergo's Loft, the Nightmare
  Frontier, The Old Hunters' areas) take two invaders, as the chalice
  dungeons do. A PC enhancement, on by default; switch it off for the game
  as it shipped.
- **Faster play-log uploads**: the game sends its play log every 30 seconds
  instead of every five minutes, so the server's map and profiles stay
  current (`online.playlog_upload_seconds`; the server asks for no less than
  30).
- **Shorter summon timeouts**: a summon sign that cannot be answered gives up
  after 30 seconds instead of three minutes (`online.sign_timeout_seconds`).
- **Modded worlds stay apart**: sessions with gameplay plugins (the
  randomizer, the boss rush, mutators) announce their rules to the server,
  which keeps their messages, bloodstains, signs and statistics separate from
  vanilla players. A player can still be summoned into a randomizer host's
  world and play by the host's rules (see [plugins.md](plugins.md)).

## Ping counter

The Ping counter (F10, Display) lists the other players in your world -
co-operators and invaders, not the players nearby whose ghosts and signs the
game also connects to - in the top right corner, under the FPS counter, with
the round trip of a ping to each, refreshed every 5 seconds: green below 100 ms,
yellow to 199, red from 200. While it is on, bbhost sends the pings itself over
the P2P port, as hole-punch probes with a ping or pong kind, to the address
the game sends to (through the relay when the game's traffic goes that way).
The other player's bbhost answers from its port without the game seeing
either. A client without the counter - an older bbhost, LanternEngine or
bloodborne_pc - drops them unseen, and after three unanswered pings bbhost stops pinging
that player. For such a player the counter reads the round trip from the game's own traffic instead, shown as
`~250 ms`: the game's reliable messages carry a sequence number and
acknowledge the other side's, and the time from one of ours to its
acknowledgement, at its lowest over 10 seconds, is the round trip - rough,
since the other game may hold an acknowledgement back for up to about 200 ms
when it has nothing to send. `--` means neither has a figure yet; a player
who answered pings and stopped for 12 seconds shows `timeout`. The log's
`net: ping` lines give the first answer from each player, silences, and,
once a minute from when a player joins, what their pings and the game's
acknowledgements came to over that minute.

## Running a server

The server is a separate project. Point `online.host` (and, if the account
service lives elsewhere, `online.auth_server`) at your own instance; with
`scheme = "http"` a server on the local network needs no certificate.
