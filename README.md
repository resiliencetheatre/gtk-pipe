# GTK Pipe

GTK Pipe is a small, live, two-way webcam and microphone link for Linux. It
uses GTK 3 for one start/stop window and GStreamer RTP streams over UDP. Short
UTF-8 text messages use a third UDP channel. Run the
same program on both computers; each peer sends to and listens on the same three
ports.

The UDP traffic is **not encrypted or authenticated**. Anyone able to observe
the path can read the text and recover the audio and video. For secrecy, carry
all three UDP ports through an encrypted UDP-capable tunnel or proxy, such as
WireGuard, and give GTK Pipe the peer's tunnel address. Do not expose the ports
directly to the public Internet. The program does not perform NAT traversal.

## Debian 13 dependencies and build

On each Debian 13 (Trixie) host:

```sh
sudo apt update
sudo apt install build-essential pkg-config libgtk-3-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-gtk3 gstreamer1.0-tools
make
make check
```

The development packages provide the C headers and libraries. The plugin
packages provide webcam/audio handling, UDP/RTP, VP8, Opus, and the GTK video
sink. `gstreamer1.0-tools` provides `gst-inspect-1.0`, used by `make check`.

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

Allow inbound UDP ports 5000, 5002, and 5004 in each host firewall. The text
channel starts immediately, independently of the media stream. A green dot by
the peer address means another GTK Pipe instance has answered a recent UDP
heartbeat. Enter sends a message, as does the **Send** button, even before the
media stream starts.

Click **Start stream** on both to exchange webcam video and microphone audio.
The remote picture appears in the window and remote audio plays through the
default output. Headphones avoid acoustic feedback.

## UFW firewall

If UFW is active, permit the three default UDP ports on each host:

```sh
sudo ufw allow 5000/udp comment 'gtk-pipe video'
sudo ufw allow 5002/udp comment 'gtk-pipe audio'
sudo ufw allow 5004/udp comment 'gtk-pipe text'
sudo ufw status numbered
```

These generic rules do not assume a particular peer address, tunnel technology,
network interface, or proxy arrangement. Apply them on both hosts. If the
deployment has stable peer addresses or a dedicated tunnel interface, the
administrator may choose narrower source- or interface-specific rules.

If UFW is not enabled yet, ensure remote administration such as SSH is allowed
before running `sudo ufw enable`, to avoid locking yourself out. Opening a port
does not encrypt it; UFW access rules and an encrypted tunnel serve different
purposes.

Use `--video-port PORT`, `--audio-port PORT`, and `--text-port PORT` on both
peers to change ports.
Only numeric IPv4 and IPv6 addresses are accepted. At roughly 600 kbit/s video
plus 32 kbit/s audio, actual network use is normally under 1 Mbit/s per peer,
but scenes with motion and protocol overhead vary.

## Design

- VP8 video is RTP payload type 96; Opus audio is RTP payload type 97.
- UTF-8 text is sent as one datagram per message on UDP port 5004, with a
  maximum encoded size of 1024 bytes.
- The text channel is available while media is stopped. It sends a heartbeat
  every two seconds and marks the peer reachable after a GTK Pipe `PING` or
  `PONG`; the indicator returns to gray after six seconds without a response.
- Separate UDP ports make firewall rules and troubleshooting straightforward.
- A 120 ms RTP jitter buffer trades a little delay for smoother playback.
- Start creates media capture, transmit, receive, and playback. Stop releases
  the camera, microphone, media sockets, and audio output while leaving text
  messaging and peer detection active.
- UDP/RTP provides no delivery guarantee. Packet loss may cause temporary
  glitches or a missing text message, which is expected for this minimal
  real-time design. Text messages are neither acknowledged nor retransmitted.

The UI and build style follow the native C/GTK3/GStreamer approach used by the
[VisualPTT project](https://github.com/resiliencetheatre/visualptt), while the
transport is live RTP/UDP instead of completed files.
