# STR Plugin Messaging API

Shared messaging broker for Skyrim Together Reborn compatibility mods.

Current development version: **v0.6.2**.

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
STRPluginMessagingBridge.dll
    ↓
registered public STRPM callback
```

STRPM payloads are encoded inside reserved `STRPM|v2|...` envelopes. The server
resource intercepts them, adds the authenticated STR sender identity and relays
them to the requested target. No custom STR executable, custom opcode or
separate client UDP port is required.

## Current Status

The native STR transport is now validated in game on two clients.

Validated:

- SKSE loading on Skyrim 1.6.1170 / SKSE64 2.2.6;
- dynamic runtime discovery against official STR 1.8.0;
- exact UTF-16 `OverlayClient::ProcessChatMessage` anchor resolution;
- `TransportService::Send` resolution and live instance capture;
- RTTI-based `NotifyChatMessageBroadcast::DeserializeRaw` receive hook;
- server Lua relay using the official STR scripting API;
- Player1 → Player2 payload delivery;
- Player2 → Player1 payload delivery;
- loopback delivery;
- sender connection ID and sender name metadata;
- reliable/ordered flags;
- public API channel registration and callback delivery;
- automatic bidirectional E2E handshake in the diagnostic client.

### v0.6.2

v0.6.2 changes only the **STRPM chat UI suppression probe**. The validated send,
receive and server-relay transport paths are intentionally left unchanged.

The v0.6.1 two-client test showed that the bridge resolved three overlay
candidates and completed the full bidirectional E2E handshake, but none of the
candidate breakpoints identified itself as `OverlayService::OnChatMessageReceived`.
The previous detector assumed an exact in-memory layout for
`TiltedPhoques::String` and a fixed `ChatMessage` member offset.

v0.6.2 removes those assumptions. At each candidate breakpoint it now:

- treats `RDX` as the `NotifyChatMessageBroadcast&` argument, matching the
  official STR 1.8.0 `OverlayService::OnChatMessageReceived` signature;
- snapshots only the first `0x100` bytes of the message object with
  `ReadProcessMemory`;
- searches the local snapshot for inline/SSO `STRPM|v2|` bytes;
- inspects aligned pointer-sized fields and safely follows them for a heap-backed
  string beginning with the exact reserved prefix;
- fails closed if any read is invalid;
- logs the first few hits of every candidate with its address, `RDX` value and
  whether the envelope prefix was detected.

This makes callback identification independent of the private allocator/string
layout while retaining the exact `STRPM|v2|` filter condition. Ordinary STR chat
continues through the original code path.

### v0.6.1

v0.6.1 hardens the STR receive resolver against runtime remapping during Skyrim
startup.

The crash signature was an access violation inside `VCRUNTIME140!memcmp` while
searching the 32-byte MSVC RTTI name
`.?AUNotifyChatMessageBroadcast@@`. The receive resolver previously enumerated
readable regions with `VirtualQuery` and then scanned those live process spans
in place. STR can unmap or replace a region between enumeration and the later
read, which made an otherwise valid-looking span unsafe.

The receive resolver now:

- snapshots process memory with `ReadProcessMemory` in bounded 1 MiB chunks;
- compares RTTI names only against local snapshot bytes;
- parses `CompleteObjectLocator64` records from local snapshots;
- scans vftable locator slots from local snapshots;
- reads vftable targets and the breakpoint byte through checked
  `ReadProcessMemory` calls;
- skips stale/unreadable chunks instead of dereferencing them.

The transport wire format and public API are unchanged.

### v0.6.0

v0.6.0 introduced **STRPM chat UI suppression**.

A separate bridge helper dynamically resolves candidate client functions
referencing the exact CEF `"message\0"` event literal. The initial implementation
attempted to identify `OverlayService::OnChatMessageReceived` using a hard-coded
MSVC/TiltedPhoques string layout. v0.6.2 supersedes that layout-specific detector
with bounded runtime probing.

The helper excludes the bridge DLL's own copy of the STR runtime anchor and
resolves only the external mapped STR allocation. Candidate functions are not
silently truncated: an unexpectedly broad candidate set fails closed and leaves
normal chat behavior unchanged.

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
dist/STRPluginMessagingAPI-v0.6.2-Vortex.zip
```

For E2E/UI-suppression regression testing, temporarily include the diagnostic
consumer:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1 -IncludeDiagnostic
```

Test output:

```text
dist/STRPluginMessagingAPI-v0.6.2-test-Vortex.zip
```

## v0.6.2 Vortex Layout

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
but is not included in the normal v0.6.2 package.

## Expected Runtime Logs

After STR runtime resolution:

```text
TransportService::Send resolved: ...
temporary TransportService::Send capture breakpoint armed
NotifyChatMessageBroadcast::DeserializeRaw = ...
receive breakpoint armed for NotifyChatMessageBroadcast::DeserializeRaw
```

F2 connection traffic can itself exercise `TransportService::Send`; a manual
`test` chat message is therefore not required if the log already reports:

```text
TransportService instance captured: ...
STRPM bridge ready: native STR send captured and receive hook armed
```

For v0.6.2 UI suppression, the bridge should additionally report candidate hits:

```text
STRPM chat UI suppression bootstrap started
STRPM chat UI suppression candidates armed: 3
STRPM chat UI candidate hit index=... address=... RDX=... envelope=...
```

On successful identification, one hit should report `envelope=yes`, followed by:

```text
STRPM chat UI filter identified OverlayService::OnChatMessageReceived = ...
```

With the `-IncludeDiagnostic` test package, the diagnostic logs should still
reach `E2E BIDIRECTIONAL HANDSHAKE COMPLETE`, while the yellow technical
`STRPM|v2|...` chat entries should no longer appear. Ordinary user chat must
remain visible normally.

## Remaining Work

- validate v0.6.2 UI suppression on both clients;
- discover/report the local STR connection ID directly;
- finalize `Host` target semantics;
- reduce pre-ready `send()` noise/retries;
- migrate OStimTogether, MorphSyncTogether, IEDSyncTogether and TradeTogether to
  the shared API;
- remove temporary diagnostic-only code when no longer useful.

## Design Rule

All STR-version-specific discovery and hooking belongs inside
`STRPluginMessagingBridge.dll`. The public API and consumer mods must remain
independent of STR internal addresses so future STR updates only require a
bridge compatibility update.