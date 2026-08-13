# STR Plugin Messaging API

Shared messaging broker for Skyrim Together Reborn compatibility mods.

The long-term goal is still to route these payloads through the existing STR
connection if the STR team accepts that feature. Until then, this project is a
functional standalone SKSE plugin: it opens one shared UDP port and exposes a
single API that my other STR-based mods can use instead of each opening their
own port.

## Why

My current STR-based mods each need their own UDP transport, discovery, shared
secret handling, peer cache, relay logic, and firewall-visible port:

- OStimTogether: scene start/node/stop, actor poses, furniture alignment.
- MorphSyncTogether: RaceMenu BodyMorph and overlay snapshots.
- IEDSyncTogether: Immersive Equipment Displays visual equipment slots.
- TradeTogether: consent-based targeted trade requests.

Those systems all need the same basic thing: send a small namespaced payload to
one player or to the whole party. This broker centralizes that transport into
one plugin and one configurable port.

## Runtime Shape

The broker exposes one C ABI entry point:

```cpp
STR_QueryPluginMessagingInterface(version, outInterface)
```

Mods would then:

1. Query the interface from `STRPluginMessagingAPI.dll`.
2. Register a namespaced channel like `chaos.ostim_together.scene.v1`.
3. Send opaque bytes to a target player, the host/server, or all players.
4. Receive callbacks with sender identity and payload bytes.
5. Optionally call `setLocalDisplayName()` once they know the real Skyrim
   player name.

The public API lives in
`include/STRPluginMessagingAPI/STRPluginMessagingAPI.h`.

## Current Status

- API contract drafted.
- Windows loader implemented.
- Functional UDP broker implemented.
- LAN broadcast discovery implemented.
- Optional manual peers and relay-host mode implemented.
- Optional HMAC-SHA256 shared secret implemented.
- Optional diagnostics export implemented:
  `STR_QueryPluginMessagingDiagnostics`.
- Strict peer-send mode defaults on, so `send(AllPlayers)` returns
  `kTargetNotFound` when no peer is known instead of reporting a false success
  after a LAN broadcast fallback.
- Example client included.
- No STR internals are touched.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Or use the packaged helper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

## Vortex Layout

The Vortex archive installs:

```text
Data/SKSE/Plugins/STRPluginMessagingAPI.dll
Data/SKSE/Plugins/STRPluginMessagingAPI.ini
```

Default port: `27990`.

The transport is UDP and best-effort. `kMessageReliable` and
`kMessageOrdered` are accepted as channel metadata for future compatibility,
but the current standalone broker does not implement retransmission yet.

`Network/RequireKnownPeer=1` is enabled by default. With that setting, the
broker still sends one broadcast fallback packet for LAN discovery, but reports
`kTargetNotFound` to the caller if no peer is known/configured. Set it to `0`
only when you intentionally want optimistic LAN broadcast sends to count as
successful.

## Repository Layout

- `include/STRPluginMessagingAPI/`: public API for mod authors.
- `src/`: client-side loader/helper implementation.
- `package/`: default Vortex install files.
- `examples/`: minimal usage sketch.
- `docs/`: proposal and migration notes for STR developers.
