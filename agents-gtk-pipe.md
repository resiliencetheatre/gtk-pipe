# gtk-pipe handoff: configurable RTP packet size for hsmproxy

## Task

Modify gtk-pipe so its outgoing video RTP UDP payloads can be limited to
**1100 bytes**, allowing video to pass through the existing hsmproxy deployment.
Inspect the current gtk-pipe source before implementing; the observations below
describe the reference previously inspected by hsmproxy and the user's live
capture, not necessarily the current checkout.

This handoff authorizes the gtk-pipe packet-sizing change. No hsmproxy protocol,
cryptography, or WireGuard configuration changes are needed for this approach.

## Confirmed failure

Text works over hsmproxy, but video does not display. The deployment uses:

```ini
[local]
listen_address = 127.0.0.2
target_address = 127.0.0.1
video_port = 5000
audio_port = 5002
text_port = 5004

[tunnel]
bind = 10.0.0.10:5500
peer = 10.0.0.14:5500
max_payload = 1100
```

The WireGuard interface `wg0` has MTU 1200. The IPv4 tunnel budget is:

```text
1200 - 20 IPv4 - 8 UDP - 48 hsmproxy = 1124 bytes
Configured application UDP payload limit = 1100 bytes
1100-byte application payload becomes a 1176-byte IP packet on wg0
```

Do not subtract WireGuard's external encapsulation again from the configured
`wg0` MTU. hsmproxy negotiates the payload limit between peers, drops application
datagrams exceeding that limit, and has no fragmentation/reassembly mechanism.
Reducing its limit does not cause gtk-pipe to send smaller packets.

The user's hsmproxy counters included `oversize=981`, with `truncation=0`,
`send_drops=0`, `delivery_drops=0`, and `mtu_drops=0`. A local video capture showed
UDP payload lengths of **1400, 1211, 1389, and 1286 bytes**, alongside smaller
packets. All those larger packets exceed 1100 and are dropped before encryption.
Losing parts of encoded frames can prevent decoding even when smaller packets
are delivered. The oversize counter is cumulative and aggregates all channels.

## Implementation guidance

1. Locate video sender pipeline construction and the RTP payloader. The
   previously inspected reference used `make_pipeline()`, VP8 RTP video, Opus
   RTP audio, and separate GStreamer `udpsink`/`udpsrc` elements. It did not set
   an explicit video RTP MTU. Verify the current implementation.
2. Add a documented runtime option such as `--rtp-mtu BYTES`, following the
   project's existing argument/configuration conventions. Preserve existing
   behavior when omitted; use `--rtp-mtu 1100` for this deployment. Validate
   values against the supported payloader property and report invalid input
   clearly. Document that this value is a complete RTP packet size, not an IP
   interface MTU or an encoded-frame size.
3. Set the sender RTP payloader's `mtu` property using the configured value,
   before playback. Verify the actual payloader's property semantics in the
   installed GStreamer version. The target is **the entire outgoing UDP
   payload, including RTP headers and payload-format headers, at most 1100
   bytes**. Do not subtract those headers twice.
4. Apply the setting consistently to every video sender pipeline, including
   recreated pipelines after stop/start or source changes. Inspect audio as
   well: if the option applies to both media channels, document that scope and
   verify the audio payloader supports it. Do not assume audio packet sizes
   are safe without measuring them.
5. Preserve codecs, RTP framing, timestamps, receiver compatibility, local
   bind/peer behavior, and the video/audio/text port layout. Let the RTP
   payloader packetize frames; do not truncate packets or manually split
   already formed RTP datagrams. Changing bitrate or resolution alone does
   not establish a packet-size ceiling.
6. Update gtk-pipe usage/help and deployment documentation with the option
   and the hsmproxy example. No proxy-specific crypto integration is required.

If the current checkout already exposes an equivalent setting, use or repair
that setting instead of adding a duplicate option. Report the exact supported
invocation in the handoff result.

## Validation and acceptance

- Build and run the project's relevant existing checks. Verify option parsing,
  invalid-value handling, and preservation of default behavior using the
  project's normal testing approach.
- Run gtk-pipe with the new limit of 1100, binding receivers to `127.0.0.1`
  and sending to peer `127.0.0.2` through hsmproxy. Configure the sender limit
  on both hosts for bidirectional video. Keep both proxies at
  `max_payload = 1100`.
- Capture outgoing video on each sending host:

  ```bash
  sudo tcpdump -ni lo -q 'udp dst port 5000 and dst host 127.0.0.2'
  ```

  All observed `UDP, length` values must be at most 1100, including during
  startup, keyframes, scene motion, and stream restart. These are UDP payload
  lengths; do not compare the limit against Ethernet frame length. Check
  audio similarly with destination port 5002.
- Verify video actually renders at the remote endpoint in both directions,
  and that audio and text still work. Packet-size compliance alone does not
  prove successful decoding.
- Compare hsmproxy counters before and during the test. `oversize` must stop
  increasing for the tested traffic; an existing nonzero cumulative value
  need not reset. Check that `mtu_drops`, `send_drops`, and `delivery_drops`
  also do not increase. hsmproxy prints status about every ten seconds;
  its `tx`/`rx` triplets are video/audio/text.
- Report changed files, exact launch arguments, checks performed, measured
  maximum UDP payload sizes, and whether real two-host media testing was
  completed. If hardware or a remote endpoint is unavailable, state that
  limitation rather than claiming end-to-end success.

## Supporting hsmproxy references

These paths belong to the hsmproxy repository; the handoff above is intended
to remain usable when copied into the gtk-pipe workspace on its own.

- `TESTING.md`, “MTU failure and correction”: MTU 1200, payload 1100, and
  diagnostic success do not establish media compatibility.
- `DESIGN.md`, “MTU and compatibility”: 76-byte IPv4 overhead and no proxy
  fragmentation/reassembly.
- `src/peer.c`, `peer_local()`: local payloads above the negotiated maximum
  increment `oversize` and are dropped.
