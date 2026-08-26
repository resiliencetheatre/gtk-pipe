# GTK Pipe

GTK Pipe is a small, live, two-way webcam and microphone link for Linux. It
uses GTK 3 for one start/stop window and GStreamer RTP streams over UDP. Run the
same program on both computers; each peer sends to and listens on the same two
ports.

This is deliberately a direct LAN/VPN tool. UDP traffic is **not encrypted or
authenticated**, and the program does not perform NAT traversal. Do not expose
its ports directly to the public Internet; use a trusted LAN or a VPN such as
WireGuard.

## Dependencies and build

On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libgtk-3-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-gtk3
make
make check
```

`vp8enc` may be packaged in `gstreamer1.0-plugins-good` or
`gstreamer1.0-plugins-bad`, depending on the distribution.

## Use on two computers

Assume computer A is `192.168.1.10` and computer B is `192.168.1.20`.

On A:

```sh
./gtk-pipe --peer 192.168.1.20
```

On B:

```sh
./gtk-pipe --peer 192.168.1.10
```

Click **Start stream** on both. Allow inbound UDP ports 5000 and 5002 in each
host firewall. The remote picture appears in the window and remote audio plays
through the default output. Headphones avoid acoustic feedback.

Use `--video-port PORT` and `--audio-port PORT` on both peers to change ports.
Only numeric IPv4 and IPv6 addresses are accepted. At roughly 600 kbit/s video
plus 32 kbit/s audio, actual network use is normally under 1 Mbit/s per peer,
but scenes with motion and protocol overhead vary.

## Design

- VP8 video is RTP payload type 96; Opus audio is RTP payload type 97.
- Separate UDP ports make firewall rules and troubleshooting straightforward.
- A 120 ms RTP jitter buffer trades a little delay for smoother playback.
- Start creates capture, transmit, receive, and playback together. Stop releases
  the camera, microphone, sockets, and audio output.
- UDP/RTP provides no delivery guarantee. Packet loss may cause temporary
  glitches, which is expected for this minimal real-time design.

The UI and build style follow the native C/GTK3/GStreamer approach used by the
[VisualPTT project](https://github.com/resiliencetheatre/visualptt), while the
transport is live RTP/UDP instead of completed files.
