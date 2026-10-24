# Architecture

MatchFrame Multistream is an OBS Studio module (`matchframe-multistream`) built with libobs,
obs-frontend-api and Qt 6 from the official
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) (commit `3e7d7ac`).
It uses OBS's installed RTMP transport (`rtmp_output` from obs-outputs, `rtmp_custom` from
rtmp-services); it contains no RTMP implementation and starts no external processes.

```
                OBS Program (main view, composed) ──┐        OBS audio mixer track N ─┐
                                                    ▼                                 ▼
                         ┌──────────────────────────────────────────────────────────────┐
                         │ SharedEncoderGroup: one H.264 obs_encoder + one AAC encoder   │
                         │ (plugin-owned; never the native streaming encoders)           │
                         └──────────────┬───────────────┬───────────────┬────────────────┘
                                        │ shared packets (late joiners start at a keyframe)
                         ┌──────────────▼──┐  ┌─────────▼───────┐  ┌────▼────────────┐
                         │ attempt: output │  │ attempt: output │  │ attempt: output │ … ≤ 8
                         │ + rtmp_custom   │  │ + rtmp_custom   │  │ + rtmp_custom   │
                         └─────▲───────────┘  └─────▲───────────┘  └─────▲───────────┘
             LifecycleWorker   │                    │                    │   (one thread per destination)
                         ┌─────┴───────────┐  ┌─────┴───────────┐  ┌─────┴───────────┐
   UI thread             │ DestinationSess.│  │ DestinationSess.│  │ DestinationSess.│
                         └─────▲───────────┘  └─────▲───────────┘  └─────▲───────────┘
                               └──────────── OutputManager ──────────────┘
                                                 ▲
                PluginContext (persistence, secrets, frontend events) ◄── MultistreamDock (Qt view)
```

## Source layout

| Path | Contents |
| --- | --- |
| `src/core/` | Qt-free core, linked into the plugin and the tests. |
| `src/ui/` | Qt dock, destination card, editor, encoding dialog, Qt dispatcher. |
| `src/plugin/` | Module entry points and `PluginContext` (frontend events, persistence, secrets). |
| `tests/unit/` | State machines and logic with a fake transport and virtual time. |
| `tests/integration/` | Headless libobs 32.2.2 with real OBS modules streaming to local receivers. |
| `tests/ui-driver/`, `tests/ui/` | Test-only OBS module that drives the real dock, and its orchestrator. |
| `packaging/windows/` | Install, uninstall and stored-key removal scripts. |

## Components

**DestinationConfig / EncoderProfile / ProfileConfig** (`destination-config`, `encoder-profile`,
`profile-config`). Versioned JSON schema (`schemaVersion` 1, migration chain in
`profile-config.cpp`). The configuration holds metadata and *credential references* (Credential
Manager target names) only. A file written by a newer schema is loaded read-only and never saved over.

**ConfigRepository** — one file per OBS profile directory in
`<OBS config>/plugin_config/matchframe-multistream/profiles/`. Saves use libobs's
temp-file + replace with a `.bak`; an unreadable file is restored from `.bak` or moved aside as
`.unreadable-<time>`, never overwritten. Each configuration carries a random `profileId`, which
scopes credential names: a duplicated OBS profile starts with an empty plugin configuration and
can never share keys, and an OBS profile rename moves the file and keeps the keys.

**CredentialStore / SecretVault** — `WindowsCredentialStore` (generic credentials,
`CRED_PERSIST_LOCAL_MACHINE`, targets `MatchFrameMultistream/v1/<profileId>/<destinationId>/<field>`).
If a write fails the editor asks the operator before keeping the value session-only in memory.
Other platforms have no secure store in this release (phase two) and are session-only. Secrets
are cleared from memory when no longer needed (`secure_clear`).

**Redaction** — `redact_url()` keeps scheme, host, port and a short application segment and
replaces credentials, extra path segments, query and fragment. `sanitize_message()` scrubs known
secrets (plain and percent-encoded), redacts embedded URLs, strips control characters and bounds
length. All plugin log lines go through `MF_LOG` with literal format strings.
OBS's own RTMP output logs the server URL (never the key) — a reason the editor rejects a server
URL that contains the stream key.

**SharedEncoderGroup** — creates the encoders lazily for the current profile with
`obs_video_encoder_create`/`obs_audio_encoder_create`, attaches them to `obs_get_video()` (the
program mix) and `obs_get_audio()` with the selected mixer index (Track N → index N-1). Encoder
IDs are enumerated at runtime; only properties the encoder actually exposes (`bitrate`,
`rate_control`=`CBR`, `keyint_sec`) are set, everything else keeps the encoder's defaults. Every
connection attempt *acquires* the group before its output exists and *releases* it after the output
was released, so the encoder objects are retained until the last output is gone. The profile can
only change while nothing holds the group, so a running shared encoder is never mutated; the
outputs' dynamic bitrate is forced off for the same reason, and `obs_service_apply_encoder_settings`
is never called. libobs shuts an encoder instance down (releasing any hardware session) when its
last output detaches; this is verified in the integration tests.

**Transport** (`obs-rtmp-transport.cpp`) — one `obs_output` + private `rtmp_custom` service per
connection attempt; OBS's built-in reconnect disabled (`obs_output_set_reconnect_settings(…, 0, 0)`);
`max_shutdown_time_sec` 5. Signals are connected with `OBSSignal` and disconnected before release,
so no callback can run with a freed context. A connecting output is stopped with
`obs_output_force_stop` because `obs_output_stop` is a no-op until the RTMP connect has finished.

**LifecycleWorker** — one serial thread per destination. `obs_output_start/stop/release` can
block (OBS 32.2.2's `rtmp_stream_stop` joins a connect thread that may sit in a handshake for up
to its 30 s receive timeout), so they never run on the UI thread, and a slow destination delays
only itself. FIFO order guarantees the previous attempt's output is released before the next
attempt's output is created. The worker also samples output statistics once a second.

**DestinationSession** — the per-destination state machine on the UI thread:
`Disabled | Idle → Connecting → Sending`, `Connecting/Sending → Reconnecting → Sending | Error`,
any active state `→ Stopping → Idle`. Each attempt has a process-unique **attempt id**; every OBS
signal and worker result is posted to the UI thread with it and ignored if stale, so a stopped
attempt cannot resurrect a session and a late "start" during a stop is discarded. A **generation**
counter guards reconnect timers. It is the only reconnect authority: 2, 4, 8, 16, then 30 s, up to
the configured attempts (default 20). A first connection failure is reported, not retried (as in
OBS); losses after a successful connection, and failures during a reconnect cycle, are retried.
Encoder failures (`OBS_OUTPUT_ENCODE_ERROR`, or `obs_output_start` failing in encoder
initialisation) are group errors and never retried.

**OutputManager** — owns the sessions and the group; enforces the locks (URL/credentials/enabled/
delete while busy; encoding while any output exists), the 8-output limit, Start Enabled (eligible
idle/errored destinations, 200 ms stagger, failures isolated), Stop All (cancels queued starts and
reconnects, plugin outputs only), a bounded synchronous stop for profile changes, and shutdown
(soft budget, hard cap, then abandon). Platform constraint warnings come from the running OBS's own
service database through a temporary private `rtmp_common` service.

**Dispatcher** — the UI-thread abstraction. `QtDispatcher` posts with
`QMetaObject::invokeMethod(…, Qt::QueuedConnection)` onto a context object; deleting it at unload
discards anything still queued. Tests use `ManualDispatcher` (virtual or real time).

**PluginContext / MultistreamDock** — the dock is a view; hiding it changes nothing.

## Lifecycle and OBS events

| Event | Plugin action |
| --- | --- |
| `obs_module_load` | Create dispatcher, credential store, repository, manager; register the dock (so OBS restores its position). |
| `FINISHED_LOADING` | Load the current profile's configuration, all destinations idle. Nothing auto-starts. |
| `PROFILE_CHANGING` | Stop plugin outputs synchronously (bounded, 15 s) because OBS resets video next and the event cannot be vetoed. |
| `PROFILE_CHANGED` / `PROFILE_RENAMED` | Decided on the next event-loop turn: a rename moves the file and keeps live outputs; any other change stops outputs (if still running) and loads that profile's configuration. |
| `EXIT` | Save, then stop every output (soft 5 s, hard 40 s cap) and release the shared encoders while libobs is fully alive (`obs_shutdown` unloads modules before freeing encoders). |
| `obs_module_unload` | Destroy remaining objects. |

Native OBS streaming, recording and replay buffer events are deliberately not acted on.

## Known limits of the pinned OBS release

* Stopping a destination whose server accepted TCP but never answers the RTMP handshake takes up to
  OBS's RTMP receive timeout (30 s; measured 24.5 s). The UI shows *Stopping* meanwhile and stays
  responsive; teardown on exit waits for it, bounded by the hard cap.
* OBS logs the RTMP server URL (not the key) when connecting.
* A first-run Windows Credential Manager refusal (for example a locked-down account) means keys
  are session-only on that machine.
