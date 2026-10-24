# MatchFrame Multistream — Operator Guide

MatchFrame Multistream sends the **same finished OBS Program feed** — camera cuts, transitions,
graphics, scoreboards, replays and the selected audio mix — to several RTMP/RTMPS destinations at
once from this computer. It does not switch cameras or build scenes; it distributes what OBS's
Program output shows.

Open it from **Docks > MatchFrame Multistream**. Closing or hiding the dock never stops an output.

## Before the match

### 1. Check the shared encoding settings

Press **Encoding...**. All plugin destinations share **one** H.264 encoder and **one** AAC encoder:

| Setting | Starting point | Notes |
| --- | --- | --- |
| Video encoder | Automatic | Uses a hardware H.264 encoder (NVENC/QSV/AMF) when OBS offers one, otherwise x264. Pick x264 if a hardware encoder fails. |
| Video bitrate | 6000 kbps, CBR | One value for every destination. Per-destination bitrates are not possible in this version. With Facebook among the destinations stay within its limits from OBS's service database: 4000 kbps at 720p30, 6000 kbps at 1080p30 or 720p60, 9000 kbps at 1080p60 (YouTube accepts more). The dock warns when the shared settings exceed a platform's limit. |
| Keyframe interval | 2 s | Recommended by YouTube, Facebook and Twitch. |
| Audio | AAC 128 kbps | The most Facebook accepts; also YouTube's stereo recommendation |
| OBS audio track | Track 1 | The track numbering matches OBS's Advanced Audio Properties. Only sources routed to this track are heard. Monitoring audio is not included unless it is routed to the track. |

Resolution and frame rate come from **OBS Settings > Video** and are shown read-only (for example
1920x1080 @ 30 fps). The plugin never changes OBS settings. It needs OBS's colour format set to NV12
(or I420), an SDR colour space and Stereo audio; the dock tells you if a setting blocks starting.
48 kHz audio is recommended.

Encoding settings are locked while any plugin destination is active.

### 2. Add destinations

Press **Add Destination** for each place you stream to. Several destinations on the same platform
are fine (for example KGB Sports Media's YouTube channel and the home club's YouTube channel).

* **Display name** — what you will see during the match.
* **Platform** — YouTube, Facebook, Twitch, Kick or Custom RTMP/RTMPS. The label only provides setup
  help and dashboard links; it does not check the account or create events.
* **Server URL** — copy the *current* ingest URL from the platform dashboard. These can be region- or
  account-specific. Prefer the `rtmps://` URL. The plugin never invents or changes URLs and never
  downgrades RTMPS to RTMP.
* **Stream key** — masked; **Show** reveals it for 10 seconds. It is stored in Windows Credential
  Manager (on macOS: the login Keychain), never in plugin files, logs, exports or diagnostics.
* **Server requires a username and password** (custom servers only).
* **Enabled** — enabled destinations are started by **Start Enabled Outputs**.
* **Reconnect automatically** and the maximum number of attempts (default 20).

The editor checks the fields locally (scheme, host, port, application path, key format, key not
pasted into the URL). There is deliberately no "test connection" button: a real connection would
send the Program feed and could go live.

**Duplicate** copies a destination's metadata only: the copy has no key and is disabled.

### 3. Plan the upload bandwidth

The dock shows a planning estimate for the enabled destinations:

`payload = destinations × (video kbps + audio kbps) / 1000`, recommended upload = payload × 1.3.

Example: 4 destinations at 6000 + 128 kbps need about 24.5 Mbps of payload; plan for about 32 Mbps
of upload. This counts plugin outputs only and is not a speed test. OBS's own streaming or
recording uploads and any other traffic on the venue connection need additional capacity. Every
destination uses its own upload bandwidth even though the encoder is shared.

### 4. Platform notes

* **YouTube** — YouTube Studio may still require **Go Live**, unless auto-start is enabled for the key.
* **Facebook** — Live Producer shows a preview first; press **Go Live** there. Use the `rtmps://` URL.
* **Twitch / Kick** — confirm in the creator dashboard that the channel shows as live.

The **Dashboard** button on a card opens the platform's dashboard.

## Going live

1. Press **Start Enabled Outputs**. Destinations start one after another (200 ms apart). A
   destination that fails shows its error and does not stop the others.
2. Each card moves **Connecting → Sending to server**.
3. **"Sending to server" only means the ingest server accepted the stream.** It does not prove the
   platform has published the broadcast. Confirm in every platform's dashboard.

You can also start or stop a single destination with its own **Start/Stop** button. Starting this
plugin does not start OBS's own **Start Streaming**, recording or replay buffer, and those OBS
controls do not start or stop plugin destinations.

## During the match

Each card shows:

| Field | Meaning |
| --- | --- |
| State | Disabled, Idle, Connecting, Sending to server, Reconnecting n/m in s, Stopping, Error |
| Bitrate | Measured outbound rate to that server |
| Time | How long the current connection has been sending |
| Dropped | Frames OBS dropped for that destination because the network could not keep up (and %) |
| Reconnects | Connection losses since you started it |

The summary line shows active / configured destinations and the measured total outbound rate. A red
**DEGRADED** marker appears whenever an enabled destination is reconnecting or in error.

While a destination is active, its server, key, platform and reconnect settings are locked and it
cannot be deleted or disabled; you can still rename it. Other, stopped destinations can be added
and edited while others are live.

### When a connection drops

If **Reconnect automatically** is on, a destination that loses its connection retries after 2, 4,
8 and 16 seconds, then every 30 seconds, up to the configured number of attempts, and then shows
**Error**. Other destinations are not affected. A destination that fails on its very first
connection reports the error immediately instead of retrying (as OBS does), so a wrong key or URL
is obvious at once. Press **Stop** at any time to cancel pending retries immediately.

### What errors mean

| Message | What to do |
| --- | --- |
| The server refused the stream (key invalid or expired, or wrong application path) | Re-copy the stream key and server URL from the dashboard. |
| Could not establish the RTMPS connection … OBS reported: invalid SSL certificate | The server's certificate failed verification. Check the URL host. The plugin never bypasses certificate checks. |
| Could not connect to the server | Check the URL, the venue network and firewall. |
| Connection to the server was lost | Network loss; automatic reconnect follows if enabled. |
| The shared encoder could not run … | Affects every plugin destination. Open **Encoding...** and choose x264 if a hardware encoder is unavailable. |
| Gave up after N reconnect attempts | Fix the cause, then press **Start**. |

**Dismiss** clears an error without retrying.

## Ending

Press **Stop All Plugin Outputs**. The confirmation lists exactly which destinations will stop; it
also cancels pending reconnects and queued starts. Native OBS streaming, recording and replay buffer
are not affected.

## Other behaviour to know

* **OBS profiles** — destinations belong to the current OBS profile. Switching profile stops plugin
  outputs first (OBS reconfigures video during a switch) and then shows the other profile's
  destinations. Creating a new profile while plugin outputs are live may leave OBS unable to apply the
  new profile's video settings until outputs are stopped and settings are re-applied.
* **Exiting OBS** stops plugin outputs. Nothing restarts automatically when OBS starts again.
* **Export / Import** (dock **More** menu) moves destination metadata and encoding settings between
  machines. Keys and credentials are never exported; imported destinations need their keys entered.
  Query parameters (anything after `?` in a server URL) are also left out because they can contain
  tokens. An invalid file is rejected without changing the current list, and importing never starts
  a destination.
* **Copy diagnostics** (More menu) copies a support summary without keys, passwords or full URLs.
* Up to 32 destinations can be saved per profile; at most 8 can run at once.
* Each platform buffers differently, so viewers on different platforms are not perfectly in sync.
