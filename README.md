# STR Plugin Messaging API

Shared messaging broker for Skyrim Together Reborn compatibility mods.

Current development version: **v0.7.0**.

STRPluginMessagingAPI gives SKSE mods one common messaging layer over the
**official Skyrim Together Reborn 1.8.0 connection**. It is intended for mods
such as OStimTogether, MorphSyncTogether, IEDSyncTogether and TradeTogether so
each project does not need to implement its own networking transport.

## Architecture

```text
consumer SKSE mod
    ↓
STRPluginMessagingAPI.dll
    ↓
STRPluginMessagingBridge.dll
    ↓
official STR 1.8.0 chat/network path
    ↓
official STR server + strpm-chat-relay Lua resource
    ↓
remote STR client
    ↓
TransportService::OnConsume interception
    ├─ STRPM packet → bridge decoder → registered callback
    └─ ordinary STR packet → original STR processing
```

STRPM payloads are encoded inside reserved `STRPM|v2|...` envelopes. The server
resource intercepts them, adds the authenticated STR sender identity and relays
them to the requested target. No custom STR executable, custom opcode or
separate client UDP port is required.

## Current Status

The native STR transport is validated in game on two clients.

Validated:

- SKSE loading on Skyrim 1.6.1170 / SKSE64 2.2.6;
- dynamic runtime discovery against official STR 1.8.0;
- exact UTF-16 `OverlayClient::ProcessChatMessage` anchor resolution;
- `TransportService::Send` resolution and live instance capture;
- server Lua relay using the official STR scripting API;
- Player1 → Player2 payload delivery;
- Player2 → Player1 payload delivery;
- loopback delivery;
- sender connection ID and sender name metadata;
- reliable/ordered flags;
- public API channel registration and callback delivery;
- automatic bidirectional E2E handshake in the diagnostic client.

### v0.7.0

v0.7.0 moves STRPM receive interception **before STR's normal message dispatcher**.
This replaces all experimental 0.6.x overlay/UI suppression hooks.

The v0.6.4 two-client logs proved that the bridge correctly recognized incoming
reserved envelopes, but the dynamically selected `OverlayApp::ExecuteAsync`
entry was never executed: both clients logged `STRPM chat UI raw envelope
observed`, while neither logged an `ExecuteAsync('message') hit`. The yellow
technical messages therefore remained visible even though E2E messaging still
completed successfully.

The official STR 1.8.0 receive path is:

```text
TiltedConnect::Client::HandleMessage
        ↓
TransportService::OnConsume(apData, aSize)
        ↓
ServerMessageFactory::Extract
        ↓
NotifyChatMessageBroadcast::DeserializeRaw
        ↓
TransportService message handler
        ↓
entt dispatcher
        ↓
OverlayService::OnChatMessageReceived
        ↓
OverlayApp / CEF / Angular chat UI
```

v0.7.0 intercepts the first STR-specific stage instead:

```text
TransportService::OnConsume
        ↓
packet starts with NotifyChatMessageBroadcast opcode?
        ↓ yes
parse MessageType + PlayerName + ChatMessage
        ↓
ChatMessage starts with "STRPM|v2|"?
        ├─ yes → decode/reassemble → public STRPM callback → return immediately
        └─ no  → execute original TransportService::OnConsume unchanged
```

Important properties:

- reserved STRPM packets never reach `ServerMessageFactory`, the dispatcher or
  the overlay;
- ordinary user chat and every other STR server packet continue through the
  original STR code;
- fragment reassembly, sender metadata, sequence numbers and message flags are
  preserved from the already validated receive implementation;
- the filter operates at the first instruction of a `void` virtual function, so
  an STRPM packet can be consumed by emulating its return before STR creates any
  local objects;
- no `OverlayService` or `OverlayApp` breakpoint is installed in v0.7.0.

`TransportService::OnConsume` is resolved dynamically from the official
`TransportService` RTTI/vftable. The exact TiltedConnect revision used by STR
1.8.0 declares the virtual order as destructor, `OnConsume`, `OnConnected`,
`OnDisconnected`, `OnUpdate`; the bridge validates executable vftable entries
before arming the breakpoint.

### v0.6.x history

v0.6.0–v0.6.4 explored suppression after normal STR chat deserialization. The
attempts progressed from candidate `OverlayService::OnChatMessageReceived`
entries to a common `OverlayApp::ExecuteAsync("message", ...)` endpoint.
Two-client testing showed that these source-level functions do not provide a
reliable executable interception point in the optimized public 1.8.0 runtime.
They are retained only as development history; v0.7.0 does not load those hooks.

### v0.6.1 safety work

The v0.6.1 receive resolver established the safe memory-scanning policy still
used by the project: runtime regions are copied with `ReadProcessMemory` in
bounded chunks and all signature/RTTI comparisons are performed on local
snapshots rather than dereferencing mappings that STR may remap during startup.

## Public API

The public header is:

```text
include/STRPluginMessagingAPI/STRPluginMessagingAPI.h
```

Main export:

```cpp
STR_QueryPluginMessagingInterface(version, outInterface)
```

Consumer mods can:

- register/unregister a namespaced channel;
- send opaque payload bytes;
- target server, host, one player or all players;
- receive sender ID/name, flags and sequence metadata;
- set their local display name.

The STR-version-specific bridge uses the private transport export:

```text
STRPM_QueryTransportInterface
```

Consumer mods never need STR internal addresses.

## Compatibility Target

```text
Skyrim Together Reborn: 1.8.0
TiltedEvolution tag:    v1.8.0
TiltedEvolution commit: 9c23efa422bbc1e5c06eef5522ca73971a513e35
TiltedConnect commit:   c20165c35c4d024bb456430eeb0abb554e34c7f4
```

Official Nexus `SkyrimTogether.exe` used for analysis:

```text
Size:    7,058,432 bytes
SHA-256: 77f23c9c82c412252b5c4491a09d7ab4349cbc6c77992c4766882f54798cb99d
```

## Server Resource

Install/enable:

```text
extras/str-server-resources/strpm-chat-relay/
```

The server console should report:

```text
[STRPM] Chat relay v2 loaded
```

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Normal Vortex package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Output:

```text
dist/STRPluginMessagingAPI-v0.7.0-Vortex.zip
```

For E2E regression testing, include the diagnostic consumer:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1 -IncludeDiagnostic
```

Test output:

```text
dist/STRPluginMessagingAPI-v0.7.0-test-Vortex.zip
```

## v0.7.0 Vortex Layout

Normal package:

```text
Data/SKSE/Plugins/STRPluginMessagingAPI.dll
Data/SKSE/Plugins/STRPluginMessagingAPI.ini
Data/SKSE/Plugins/STRPluginMessagingBridge.dll
```

The `-IncludeDiagnostic` test package additionally installs:

```text
Data/SKSE/Plugins/STRPluginMessagingDiagnostic.dll
```

The diagnostic client remains in the source/build graph as a regression target
but is not included in the normal package.

## Expected Runtime Logs

After STR runtime resolution, v0.7.0 should report entries similar to:

```text
TransportService::Send resolved: 0x...
temporary TransportService::Send capture breakpoint armed
TransportService vftable = 0x...
  vftable[0] = 0x...
  vftable[1] = 0x... [OnConsume]
  vftable[2] = 0x...
  vftable[3] = 0x...
  vftable[4] = 0x...
TransportService::OnConsume = 0x...
receive breakpoint armed for TransportService::OnConsume
STRPM chat suppression active before STR ServerMessageFactory/dispatcher
STRPM receive path resolved and armed
```

F2 connection traffic can itself exercise `TransportService::Send`; a manual
chat message is not required if the bridge already reports:

```text
TransportService instance captured: ...
STRPM bridge ready: native STR send captured and receive hook armed
```

When reserved traffic arrives, the bridge should add:

```text
STRPM packet consumed before STR dispatcher thread=... bytes=...
```

With the `-IncludeDiagnostic` test package:

- both diagnostic logs should reach `E2E BIDIRECTIONAL HANDSHAKE COMPLETE`;
- yellow technical `STRPM|v2|...` lines should not appear in the STR chat;
- ordinary messages such as `test_p1` and `test_p2` must still appear normally.

## Remaining Work

- validate v0.7.0 pre-dispatch suppression on both clients;
- discover/report the local STR connection ID directly;
- finalize `Host` target semantics;
- reduce pre-ready `send()` noise/retries;
- migrate OStimTogether, MorphSyncTogether, IEDSyncTogether and TradeTogether to
  the shared API;
- remove obsolete experimental UI-suppression source files after runtime
  validation;
- remove temporary diagnostic-only code when no longer useful.

## Design Rule

All STR-version-specific discovery and hooking belongs inside
`STRPluginMessagingBridge.dll`. The public API and consumer mods must remain
independent of STR internal addresses so future STR updates only require a
bridge compatibility update.
