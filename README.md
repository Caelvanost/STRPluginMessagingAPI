# STR Plugin Messaging API

Shared messaging broker for Skyrim Together Reborn compatibility mods.

The project provides one common API for SKSE mods that need to exchange small,
namespaced messages between Skyrim Together players. The current development
path targets the **official Skyrim Together Reborn 1.8.0 client and server** and
uses STR's existing connection rather than opening a separate UDP transport.

## Why

Several STR compatibility mods need essentially the same networking layer:

- OStimTogether: scene start/node/stop, actor poses, furniture alignment.
- MorphSyncTogether: RaceMenu BodyMorph and overlay snapshots.
- IEDSyncTogether: Immersive Equipment Displays visual equipment slots.
- TradeTogether: consent-based targeted trade requests.

Historically, implementing those features independently means duplicating UDP
transport, discovery, shared-secret handling, peer caches, relay logic and
firewall-visible ports. STRPluginMessagingAPI centralizes the contract so each
mod only has to register a channel and exchange payloads.

## Architecture

The intended runtime path is now:

```text
OStimTogether / MorphSyncTogether / IEDSyncTogether / TradeTogether
                              |
                              v
                 STRPluginMessagingAPI.dll
                              |
                              v
                STRPluginMessagingBridge.dll
                              |
                              v
             Skyrim Together Reborn 1.8.0
                              |
                    existing STR connection
                              |
                              v
               official STR server 1.8.0
                              |
                              v
                 strpm-chat-relay resource
                              |
                              v
                    target STR client
                              |
                              v
                STRPluginMessagingBridge
                              |
                              v
                 receiving SKSE mod
```

The server relay uses STR's official Lua scripting API and the existing chat
transport as an internal tunnel. STRPM messages use a reserved `STRPM|v2|...`
envelope and are intercepted by the relay rather than being treated as ordinary
player chat.

This approach is intended to avoid:

- a custom Skyrim Together protocol fork;
- custom STR opcodes;
- a separately compiled STR server;
- extra UDP ports or port forwarding for individual compatibility mods.

## Public API

The broker exposes the C ABI entry point:

```cpp
STR_QueryPluginMessagingInterface(version, outInterface)
```

A client mod can then:

1. Query the interface from `STRPluginMessagingAPI.dll`.
2. Register a namespaced channel such as `chaos.ostim_together.scene.v1`.
3. Send opaque payload bytes to a player, host/server, or all players.
4. Receive callbacks containing sender identity and payload bytes.
5. Optionally call `setLocalDisplayName()` when the Skyrim player name is known.

The public header is:

```text
include/STRPluginMessagingAPI/STRPluginMessagingAPI.h
```

The transport-facing private ABI is exposed through:

```text
STRPM_QueryTransportInterface
```

This separation is deliberate: mods depend on the stable messaging API, while
STR-version-specific integration remains isolated inside the bridge.

## Current Status

The project is **under active development and is not yet a finished networking
replacement for the client mods**.

Implemented:

- public C messaging API;
- Windows loader/runtime;
- private `STRPM_QueryTransportInterface` transport ABI;
- optional diagnostics through `STR_QueryPluginMessagingDiagnostics`;
- example client;
- STR-only packaged configuration;
- official-server Lua relay using `STRPM|v2|...` envelopes;
- targeted and broadcast relay handling;
- message fragmentation support in the chat-tunnel design;
- initial `STRPluginMessagingBridge.dll` target;
- runtime probing/resolution infrastructure for STR 1.8.0;
- analysis and fingerprinting of the official Nexus STR 1.8.0 binary.

Still in progress:

- resolving the final STR 1.8.0 client send path into `TransportService::Send`;
- intercepting `NotifyChatMessageBroadcast` reliably on the receiving client;
- enabling real send/receive in `STRPluginMessagingBridge.dll` after all runtime
  validations pass;
- end-to-end Player1/Player2 testing;
- migrating the individual compatibility mods onto the shared API.

The bridge currently remains fail-safe: an unsupported or unresolved STR build
must produce `kNotConnected` rather than calling an uncertain address.

## Skyrim Together Reborn Compatibility

The current bridge work explicitly targets the official public:

```text
Skyrim Together Reborn 1.8.0
TiltedEvolution tag: v1.8.0
TiltedEvolution commit: 9c23efa422bbc1e5c06eef5522ca73971a513e35
```

The official Nexus `SkyrimTogether.exe` used for binary analysis has:

```text
Size:    7,058,432 bytes
SHA-256: 77f23c9c82c412252b5c4491a09d7ab4349cbc6c77992c4766882f54798cb99d
```

The public executable is packed/restructured and does not expose a conventional
`.text`/`.rdata` layout on disk. For that reason, the bridge is being designed
to validate the supported build and resolve required structures/functions from
the mapped runtime image rather than relying on fragile absolute addresses.

Relevant research is documented in:

```text
docs/OFFICIAL_STR_CHAT_TUNNEL.md
docs/STR_1_8_0_BINARY_FINGERPRINT.md
docs/STR_1_8_0_RTTI_NOTES.md
```

## Server Resource

The experimental relay is located at:

```text
extras/str-server-resources/strpm-chat-relay/
```

It is designed for the official STR server scripting system. It recognizes
STRPM envelopes, validates routing fields, derives the sender identity from the
actual STR connection, and relays the message through STR's existing chat
functions.

It is part of the current transport design and should be installed on the STR
server when performing end-to-end chat-tunnel tests.

## Build

Standard CMake build:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The packaged configuration is STR-only. The legacy UDP backend remains in the
source for development/testing and can be explicitly enabled with:

```powershell
cmake -S . -B build -DSTRPM_ENABLE_UDP_BACKEND=ON
```

It is **not** the packaged default and must not be used as a silent fallback.

### Vortex package

Use:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The helper builds the project, stages the Vortex layout, validates required
archive entries, and writes the generated archive to:

```text
dist/STRPluginMessagingAPI-v<version>-Vortex.zip
```

`dist/` is ignored by Git.

## Vortex Layout

The base package installs:

```text
Data/SKSE/Plugins/STRPluginMessagingAPI.dll
Data/SKSE/Plugins/STRPluginMessagingAPI.ini
```

The packaged build does not open a legacy UDP port.

The default transport configuration is STR mode. During bridge development,
missing or unresolved STR integration intentionally results in `kNotConnected`
instead of falling back to UDP.

## Repository Layout

- `include/STRPluginMessagingAPI/` — public API for mod authors.
- `src/` — broker runtime and STR bridge implementation/probe.
- `package/` — default Vortex install files.
- `examples/` — minimal client usage example.
- `docs/` — migration, transport design, STR 1.8.0 binary and RTTI research.
- `extras/str-server-resources/` — official STR server Lua relay resource.
- `dist/` — locally generated release archives; ignored by Git.

## Design Rule

Client mods should never depend directly on Skyrim Together internal addresses.
All STR-version-specific work belongs in `STRPluginMessagingBridge.dll`. This
allows the public messaging API and mods using it to remain stable when STR
changes and confines compatibility updates to the bridge layer.
