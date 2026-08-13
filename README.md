# STR Plugin Messaging API

Shared messaging broker for Skyrim Together Reborn compatibility mods.

The long-term goal is to route these payloads through the existing STR
connection if the STR team accepts that feature. The default private build is
now STR-only: it does not open the legacy UDP backend or fall back to a
firewall-visible port.

## Why

My current STR-based mods each need their own UDP transport, discovery, shared
secret handling, peer cache, relay logic, and firewall-visible port:

- OStimTogether: scene start/node/stop, actor poses, furniture alignment.
- MorphSyncTogether: RaceMenu BodyMorph and overlay snapshots.
- IEDSyncTogether: Immersive Equipment Displays visual equipment slots.
- TradeTogether: consent-based targeted trade requests.

Those systems all need the same basic thing: send a small namespaced payload
to one player or to the whole party. This broker centralizes that contract into
one plugin API while the actual private transport is expected to be provided by
STR.

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
- Experimental internal STR transport ABI implemented:
  `STRPM_QueryTransportInterface`.
- Runtime defaults to `STR` mode and requires an internal STR bridge.
- Legacy UDP broker remains in the source for development builds only, behind
  `STRPM_ENABLE_UDP_BACKEND=ON`.
- Optional diagnostics export implemented:
  `STR_QueryPluginMessagingDiagnostics`.
- Example client included.
- No installed STR binary is hooked or patched by this package.

The STR developers pointed at the official server scripting guide. That
confirms server resources can intercept and send chat messages, but it does not
document a generic binary plugin-message transport. Notes and a private
experimental server resource are included under:

```text
docs/STR_INTERNAL_CONNECTION.md
extras/str-server-resources/strpm-chat-relay/
```

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

By default this builds the private STR-only DLL:

```powershell
cmake -S . -B build -DSTRPM_ENABLE_UDP_BACKEND=OFF
```

The legacy standalone UDP backend can still be built for local development with
`-DSTRPM_ENABLE_UDP_BACKEND=ON`, but that is not the packaged default.

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

The packaged private build does not open a UDP port.

The default transport mode is:

```ini
[Transport]
Mode=STR
STRBridgeModule=Data\SkyrimTogetherReborn\STRPluginMessagingBridge.dll
```

If the bridge is missing, `send()` returns `kNotConnected`. This is intentional:
the private build must not silently fall back to UDP or require port
forwarding.

## Repository Layout

- `include/STRPluginMessagingAPI/`: public API for mod authors.
- `src/`: client-side loader/helper implementation.
- `package/`: default Vortex install files.
- `examples/`: minimal usage sketch.
- `docs/`: proposal, migration notes, and internal STR bridge notes.
- `extras/str-server-resources/`: optional STR server-side experiments.
