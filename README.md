# STR Plugin Messaging API

Shared messaging and STR player-proxy resolution for Skyrim Together Reborn compatibility mods.

Current development version: **v0.8.0**.

STRPluginMessagingAPI gives SKSE mods one common layer over the **official Skyrim Together Reborn 1.8.0 connection**. It is intended for mods such as AnimSyncTogether, OStimTogether, MorphSyncTogether, IEDSyncTogether and TradeTogether so each project does not need to duplicate networking or guess which Skyrim Actor represents a remote STR player.

## Architecture

```text
consumer SKSE mod
    ↓
STRPluginMessagingAPI.dll
    ├─ messaging API
    └─ ProxyResolver API: ConnectionID -> local proxy FormID
    ↓
STRPluginMessagingBridge.dll
    ↓
official STR 1.8.0 runtime
    ↓
official STR server + strpm-chat-relay Lua resource
```

STRPM payloads use reserved `STRPM|v2|...` chat envelopes. v0.7.0 and later consume those packets at `TransportService::OnConsume` before STR's normal dispatcher, so transport packets never appear as yellow chat messages. Ordinary STR chat remains untouched.

## v0.8.0 — ProxyResolver

v0.8.0 adds a public **ProxyResolver** intended for animation, appearance and equipment synchronization mods.

The design rule is explicit: consumer mods must **not** scan Skyrim `ProcessLists`, compare actor names or cache guessed dynamic FormIDs. STRPM owns STR-version-specific resolution and publishes a stable mapping:

```text
STR ConnectionID
      ↕ authenticated relay metadata
STR PlayerId
      ↕ official STR player lifecycle
local Skyrim proxy FormID
```

The two halves come from canonical STR data:

1. the v3 server relay appends both the authenticated `sender` connection ID and `senderPlayerId = Player::GetId()` to reserved envelopes;
2. the bridge observes STR's native remote-player 3D lifecycle, where `PlayerComponent::Id` and the temporary proxy Actor FormID are used together;
3. once both halves are known, `STRPluginMessagingAPI.dll` publishes `ConnectionID -> FormID` to consumers.

The bridge also observes STR connect/disconnect lifecycle. Proxy mappings are cleared on disconnect and removed when the corresponding remote player component is unloaded.

### Public ProxyResolver API

Load it independently of the messaging interface:

```cpp
const auto* resolver = STRPM::LoadProxyResolverFromModule();
```

Resolve a sender received through STRPM:

```cpp
STRPM::ProxyFormID formID{};
const auto result = resolver->resolve(message->sender.connectionID, &formID);
if (result == STRPM::Result::kOk) {
    // formID is the local Skyrim Actor proxy for that remote STR player.
}
```

Consumers can also register for mapping lifecycle events:

```cpp
resolver->registerListener(&OnProxyMappingChanged, userData);
```

Events are:

- `kAdded`
- `kUpdated`
- `kRemoved`
- `kCleared`

A listener registered after mappings already exist receives an immediate snapshot through `kAdded` events.

**Important:** ProxyResolver deliberately returns a FormID, not a raw `Actor*`. Resolver callbacks can originate from an STR/bridge thread. Consumer mods should resolve/use the Actor and perform game mutations on the Skyrim game thread.

### AnimSync usage contract

AnimSyncTogether should use the sender ID carried by the STRPM message and ask ProxyResolver for the local Actor proxy. If the mapping is not available yet, it should retain the synchronization event briefly or subscribe to ProxyResolver notifications instead of searching `ProcessLists`.

This keeps all STR 1.8.0 internals inside `STRPluginMessagingBridge.dll`.

## v0.8.0 diagnostic cleanup

The optional diagnostic client no longer begins E2E sends merely because the DLLs have loaded. The bridge exports an internal regression-only connection signal driven by the real `TransportService::OnConnected`/`OnDisconnected` lifecycle. Diagnostic probes wait for that signal and pause again if STR disconnects.

After the bidirectional messaging handshake, the diagnostic also checks the ProxyResolver mapping for the remote sender and reports either:

```text
PROXY RESOLVE OK senderId=... formId=0xFF......
```

or a bounded timeout with bridge diagnostics.

The diagnostic DLL remains excluded from normal Vortex packages and is included only with `-IncludeDiagnostic` or the CI test artifact.

## Validated transport baseline

The v0.7.0 transport baseline has been validated in game on two clients:

- Skyrim 1.6.1170 / SKSE64 2.2.6;
- official STR 1.8.0 runtime discovery;
- `TransportService::Send` resolution and live instance capture;
- Player1 -> Player2 and Player2 -> Player1 delivery;
- loopback delivery;
- public channel registration/callback delivery;
- authenticated sender connection ID/name metadata;
- reserved packet consumption before STR's dispatcher;
- no yellow STRPM technical messages;
- normal chat remains visible;
- bidirectional E2E handshake completes on both clients.

v0.8.0 deliberately leaves that validated send/receive/suppression implementation unchanged and adds ProxyResolver as an isolated bridge subsystem.

## Public API

Header:

```text
include/STRPluginMessagingAPI/STRPluginMessagingAPI.h
```

Messaging export:

```cpp
STR_QueryPluginMessagingInterface(version, outInterface)
```

ProxyResolver export:

```cpp
STR_QueryPluginMessagingProxyResolver(version, outInterface)
```

The STR-version-specific bridge continues to use the private transport export:

```text
STRPM_QueryTransportInterface
```

Consumer mods never need STR internal addresses.

## Compatibility target

```text
Skyrim Together Reborn: 1.8.0
TiltedEvolution tag:    v1.8.0
TiltedEvolution commit: 9c23efa422bbc1e5c06eef5522ca73971a513e35
TiltedConnect commit:   c20165c35c4d024bb456430eeb0abb554e34c7f4
```

Official Nexus executable used for runtime validation:

```text
Size:    7,058,432 bytes
SHA-256: 77f23c9c82c412252b5c4491a09d7ab4349cbc6c77992c4766882f54798cb99d
```

## Server resource

v0.8.0 requires the updated resource:

```text
extras/str-server-resources/strpm-chat-relay/
```

Replace the previous relay on the STR server. The server console must report:

```text
[STRPM] Chat relay v3 loaded (ProxyResolver identity metadata enabled)
```

The wire prefix remains `STRPM|v2|`; relay v3 only adds authenticated identity metadata required by ProxyResolver.

## Build

Normal Vortex package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Output:

```text
dist/STRPluginMessagingAPI-v0.8.0-Vortex.zip
```

For E2E + ProxyResolver regression testing:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1 -IncludeDiagnostic
```

Output:

```text
dist/STRPluginMessagingAPI-v0.8.0-test-Vortex.zip
```

Normal package:

```text
Data/SKSE/Plugins/STRPluginMessagingAPI.dll
Data/SKSE/Plugins/STRPluginMessagingAPI.ini
Data/SKSE/Plugins/STRPluginMessagingBridge.dll
```

Test package additionally contains:

```text
Data/SKSE/Plugins/STRPluginMessagingDiagnostic.dll
```

## Expected v0.8.0 bridge logs

In addition to the already validated transport lines, expect:

```text
ProxyResolver native lifecycle hooks armed: ...
ProxyResolver sender identity observer attached to validated OnConsume breakpoint
ProxyResolver observed STR transport connected
ProxyResolver proxy observed playerId=... formId=0xFF......
ProxyResolver mapping ready connection=... formId=0xFF......
```

On disconnect:

```text
ProxyResolver observed STR transport disconnected; clearing mappings
```

With the diagnostic build, both clients should still reach:

```text
E2E BIDIRECTIONAL HANDSHAKE COMPLETE
PROXY RESOLVE OK senderId=... formId=0xFF......
```

Yellow `STRPM|v2|...` lines must remain absent and ordinary messages such as `test_p1` / `test_p2` must remain visible.

## Remaining work

- runtime-validate the new v0.8.0 ProxyResolver on both clients;
- discover/report the local STR connection ID directly;
- finalize `Host` target semantics;
- migrate AnimSyncTogether first, then OStimTogether, MorphSyncTogether, IEDSyncTogether and TradeTogether to the shared API;
- remove obsolete experimental 0.6.x UI-suppression source files after the current regression cycle.

## Design rule

All STR-version-specific discovery and hooking belongs inside `STRPluginMessagingBridge.dll`. The public API and consumer mods remain independent of STR internal addresses so a future STR update only requires a bridge compatibility update.
