# STR Plugin Messaging API

Shared messaging broker for Skyrim Together Reborn compatibility mods.

Current development version: **v0.4.4**.

The project provides one common API for SKSE mods that need to exchange small,
namespaced messages between Skyrim Together players. The current implementation
targets the **official Skyrim Together Reborn 1.8.0 client and server** and uses
STR's existing connection rather than opening a separate UDP transport.

## Why

Several STR compatibility mods need essentially the same networking layer:

- OStimTogether: scene start/node/stop, actor poses, furniture alignment.
- MorphSyncTogether: RaceMenu BodyMorph and overlay snapshots.
- IEDSyncTogether: Immersive Equipment Displays visual equipment slots.
- TradeTogether: consent-based targeted trade requests.

STRPluginMessagingAPI centralizes that contract so each mod can register a
channel and exchange payloads without implementing its own discovery, relay,
peer cache or firewall-visible UDP port.

## Architecture

```text
SKSE compatibility mod
        |
        v
STRPluginMessagingAPI.dll
        |
        v
STRPluginMessagingBridge.dll
        |
        v
official Skyrim Together Reborn 1.8.0 client
        |
        | existing STR chat/network connection
        v
official STR 1.8.0 server
        |
        v
strpm-chat-relay Lua resource
        |
        v
target official STR client
        |
        v
STRPluginMessagingBridge.dll
        |
        v
registered STRPM channel callback
```

STRPM payloads are encoded inside reserved `STRPM|v2|...` chat envelopes. The
server resource intercepts those envelopes and performs explicit STRPM routing;
normal player chat is left untouched.

This avoids a custom STR protocol fork, custom opcodes, a custom server binary,
and separate client-mod UDP ports.

## Public API

The broker exposes:

```cpp
STR_QueryPluginMessagingInterface(version, outInterface)
```

Client mods can register a namespaced channel, send opaque bytes, receive sender
metadata and payload callbacks, and optionally set the local Skyrim display
name. The stable public header is:

```text
include/STRPluginMessagingAPI/STRPluginMessagingAPI.h
```

The version-specific bridge uses the private transport ABI:

```text
STRPM_QueryTransportInterface
```

Client mods never need to know STR internal addresses.

## Current Status

The project is now at the **first native STR 1.8.0 send/receive prototype**.
It compiles with MSVC / Visual Studio 2026 and still requires in-game two-client
validation before it should be treated as production-ready.

Implemented:

- public C messaging API and runtime;
- STR-only packaged configuration;
- private bridge ABI and diagnostics;
- official-server Lua relay;
- targeted and all-player routing;
- `kMessageAllowLoopback` handling on the server relay;
- payload fragmentation and reassembly;
- runtime resolution of `OverlayClient::ProcessChatMessage` and
  `TransportService::Send` from the mapped STR 1.8.0 image;
- temporary breakpoint capture of the live `TransportService` instance;
- ABI-compatible chat-message proxy that lets STR itself perform the normal
  `ClientMessage::Serialize` and network send path;
- RTTI-based resolution of `NotifyChatMessageBroadcast::DeserializeRaw`;
- non-destructive receive parsing directly from STR's `Buffer::Reader`;
- authenticated sender ID/name extraction from server-appended metadata;
- delivery of completed payloads through the STRPM receive callback;
- lazy bootstrap so the bridge can load before the player connects through F2;
- v0.4.3 startup guard: the receive RTTI resolver remains deferred until the
  send resolver has positively identified the mapped STR 1.8.0 runtime;
- v0.4.4 startup scan guard: memory comparisons fail closed if STR remaps or
  releases a region while the send resolver is scanning it;
- fail-safe behavior when runtime resolution is incomplete;
- Windows CI build validation and DLL artifact generation.

Still to validate or complete:

- real Player1 -> Player2 and Player2 -> Player1 runtime tests;
- suppressing relayed `STRPM|v2|...` envelopes from the visible STR chat UI
  (the receive prototype currently consumes them for STRPM but still lets STR
  continue its normal chat processing);
- local STR connection-ID discovery;
- final host-target semantics;
- migrating the individual compatibility mods onto the shared API.

Until the native runtime has been resolved and captured, `send()` returns
`kNotConnected`. Unsupported/ambiguous builds fail closed rather than calling an
uncertain address.

## STR 1.8.0 Compatibility Target

```text
Skyrim Together Reborn 1.8.0
TiltedEvolution tag: v1.8.0
TiltedEvolution commit: 9c23efa422bbc1e5c06eef5522ca73971a513e35
```

Official Nexus `SkyrimTogether.exe` used for analysis:

```text
Size:    7,058,432 bytes
SHA-256: 77f23c9c82c412252b5c4491a09d7ab4349cbc6c77992c4766882f54798cb99d
```

The public executable is packed/restructured, so the bridge resolves the
required client code from the **mapped runtime image** instead of relying on
absolute file offsets.

Research notes:

```text
docs/OFFICIAL_STR_CHAT_TUNNEL.md
docs/STR_1_8_0_BINARY_FINGERPRINT.md
docs/STR_1_8_0_RTTI_NOTES.md
```

## Server Resource

The relay lives in:

```text
extras/str-server-resources/strpm-chat-relay/
```

It uses STR's documented server scripting API. Client-originated STRPM messages
are cancelled as normal chat, validated, tagged with the authenticated STR
sender identity and relayed to the requested target.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The default build is STR-only. The legacy UDP backend is retained only for
explicit development builds:

```powershell
cmake -S . -B build -DSTRPM_ENABLE_UDP_BACKEND=ON
```

### Vortex package

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The generated archive is written to:

```text
dist/STRPluginMessagingAPI-v0.4.4-Vortex.zip
```

`dist/` is ignored by Git.

## Vortex Layout

```text
Data/SKSE/Plugins/STRPluginMessagingAPI.dll
Data/SKSE/Plugins/STRPluginMessagingAPI.ini
Data/SKSE/Plugins/STRPluginMessagingBridge.dll
```

Both DLLs are valid SKSE plugins. The broker resolves the bridge by module name
relative to its own directory, so startup does not depend on the SKSE plugin
load order or on the process current working directory.

The default INI points to:

```ini
[Transport]
Mode=STR
STRBridgeModule=STRPluginMessagingBridge.dll
```

No legacy UDP port is opened by the packaged STR-only build.

## Runtime Startup Sequence

The bridge can initialize before Skyrim Together is connected. Its bootstrap
retries periodically and follows this order:

```text
SKSE loads STRPM
        |
        v
send resolver waits for mapped STR 1.8.0 runtime
        |
        v
TransportService::Send resolved
        |
        +--> temporary capture breakpoint armed
        |
        v
receive RTTI resolver starts
        |
        v
NotifyChatMessageBroadcast receive hook armed
```

The receive resolver is deliberately not allowed to scan before the send
resolver confirms that the STR runtime is present. During early startup the send
resolver also treats transient memory-remap faults as a failed scan and retries
later instead of letting an access violation escape into Skyrim.

## Repository Layout

- `include/STRPluginMessagingAPI/` — stable public API for mod authors.
- `src/` — broker runtime and STR 1.8.0 bridge implementation.
- `package/` — default Vortex install files.
- `examples/` — minimal API usage example.
- `docs/` — migration, transport design, binary and RTTI research.
- `extras/str-server-resources/` — official STR server Lua relay resource.
- `.github/workflows/` — Windows MSVC build validation.
- `dist/` — locally generated release archives; ignored by Git.

## Design Rule

All STR-version-specific discovery and hooking belongs inside
`STRPluginMessagingBridge.dll`. The public API and mods using it must remain
independent of STR internal addresses so a future STR update only requires a
bridge compatibility update.
