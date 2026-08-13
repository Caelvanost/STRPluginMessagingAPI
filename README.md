# STR Plugin Messaging API

Prototype API for routing custom SKSE mod payloads through the existing
Skyrim Together Reborn connection.

This repository is intentionally independent from STR for now. It defines the
client-facing contract I would need for my Skyrim Together compatibility mods,
plus a tiny loader that can discover the API if STR exposes it later.

## Why

My current STR-based mods each need their own UDP transport, discovery, shared
secret handling, peer cache, relay logic, and firewall-visible port:

- OStimTogether: scene start/node/stop, actor poses, furniture alignment.
- MorphSyncTogether: RaceMenu BodyMorph and overlay snapshots.
- IEDSyncTogether: Immersive Equipment Displays visual equipment slots.
- TradeTogether: consent-based targeted trade requests.

Those systems all need the same basic thing: send a small namespaced payload to
one STR player or to the whole party. STR does not need to understand the
payload; it only needs to relay it safely over the connection it already owns.

## Proposed Shape

STR would expose one C ABI entry point:

```cpp
STR_QueryPluginMessagingInterface(version, outInterface)
```

Mods would then:

1. Query the interface from the STR module.
2. Register a namespaced channel like `chaos.ostim_together.scene.v1`.
3. Send opaque bytes to a target player, the host/server, or all players.
4. Receive callbacks with sender identity and payload bytes.

The public API lives in
`include/STRPluginMessagingAPI/STRPluginMessagingAPI.h`.

## Current Status

- API contract drafted.
- Windows loader implemented.
- Example client included.
- No STR internals are touched yet.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The build only validates the standalone API/client helper. It does not produce
an STR plugin yet.

## Repository Layout

- `include/STRPluginMessagingAPI/`: public API for mod authors.
- `src/`: client-side loader/helper implementation.
- `examples/`: minimal usage sketch.
- `docs/`: proposal and migration notes for STR developers.
