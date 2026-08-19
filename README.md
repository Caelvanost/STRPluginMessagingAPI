# STR Plugin Messaging API

Shared messaging and STR player-proxy resolution for Skyrim Together Reborn compatibility mods.

Current development version: **v0.8.2**.

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

## v0.8.2 — session/log cleanup

v0.8.2 is a small cleanup patch on top of the validated v0.8.1 ProxyResolver.

Two runtime behaviors are changed:

1. the optional diagnostic client now treats each STR connection as a separate E2E session. A disconnect clears peer/ACK state and the next `OnConnected` restarts at `probe=1`;
2. repeated internal ProxyResolver flushes no longer log `mapping ready` when `ConnectionID -> FormID` is unchanged. Public mapping events were already change-only; v0.8.2 makes the private bridge report path change-only as well.

Expected reconnect diagnostics now look like:

```text
E2E SESSION 1 START: ...
E2E SESSION 1 aborted: STR disconnected ...
E2E SESSION 1 reset after disconnect
E2E SESSION 2 START: ...
...
E2E SESSION 2 BIDIRECTIONAL HANDSHAKE COMPLETE
E2E SESSION 2 PROXY RESOLVE OK senderId=... formId=0xFF......
```

The validated transport and ProxyResolver discovery logic are otherwise unchanged.

## ProxyResolver — validated baseline

The v0.8.1 two-client runtime test validated ProxyResolver in both directions:

```text
Player1 resolving Player2:
ConnectionID -> Elir local proxy FormID 0xFF001D44

Player2 resolving Player1:
ConnectionID -> Kahel local proxy FormID 0xFF000C81
```

Both clients also completed:

```text
E2E BIDIRECTIONAL HANDSHAKE COMPLETE
PROXY RESOLVE OK
```

A reconnect test additionally confirmed that STR connection IDs are session-scoped and that ProxyResolver clears mappings on `OnDisconnected` before accepting the new session identity.

### Resolution model

Consumer mods must **not** scan Skyrim `ProcessLists`, compare actor names or cache guessed dynamic FormIDs. STRPM owns STR-version-specific resolution and publishes:

```text
STR ConnectionID
      ↕ authenticated relay metadata
STR PlayerId
      ↕ official STR player lifecycle
local Skyrim proxy FormID
```

The v3 server relay appends authenticated `sender` and `senderPlayerId` metadata. The bridge observes STR's remote-player lifecycle and joins that identity with the local proxy FormID.

### Public API

Load the resolver independently of the messaging interface:

```cpp
const auto* resolver = STRPM::LoadProxyResolverFromModule();
```

Resolve the sender of an STRPM message:

```cpp
STRPM::ProxyFormID formID{};
const auto result = resolver->resolve(message->sender.connectionID, &formID);
if (result == STRPM::Result::kOk) {
    // formID is the local Skyrim Actor proxy for that remote STR player.
}
```

Consumers can also subscribe to mapping lifecycle changes:

```cpp
resolver->registerListener(&OnProxyMappingChanged, userData);
```

Events are:

- `kAdded`
- `kUpdated`
- `kRemoved`
- `kCleared`

A listener registered after mappings already exist receives an immediate snapshot through `kAdded` events.

**Important:** ProxyResolver returns a FormID, not a raw `Actor*`. Resolver callbacks can originate from an STR/bridge thread. Consumer mods should perform Skyrim object lookup/game mutations on the game thread.

### AnimSync usage contract

AnimSyncTogether should use the sender `ConnectionID` from the received STRPM message and ask ProxyResolver for the local proxy FormID. If the mapping is temporarily unavailable, AnimSync should queue the synchronization event briefly or wait for a ProxyResolver mapping event instead of searching `ProcessLists`.

## Validated transport baseline

The v0.7.0+ transport has been validated on two clients with:

- Skyrim 1.6.1170 / SKSE64 2.2.6;
- official STR 1.8.0 runtime discovery;
- `TransportService::Send` resolution and live instance capture;
- Player1 -> Player2 and Player2 -> Player1 delivery;
- loopback delivery;
- public channel registration/callback delivery;
- authenticated sender connection ID/name metadata;
- reserved packet consumption before STR's dispatcher;
- no yellow STRPM technical messages;
- normal STR chat remains visible;
- bidirectional E2E handshake completes on both clients.

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

v0.8.x requires relay v3. The server console must report:

```text
[STRPM] Chat relay v3 loaded (ProxyResolver identity metadata enabled)
```

The wire prefix remains `STRPM|v2|`; relay v3 adds the authenticated STR PlayerId metadata required by ProxyResolver.

## FOMOD

The Vortex package contains a FOMOD with three installation modes:

```text
Client + Server   Recommended for the player hosting the STR server
Client Only       For players joining another server
Server Files Only For a dedicated/server-only update
```

The server option deploys the resource under:

```text
Data/SkyrimTogetherReborn/resources/strpm-chat-relay/
```

including both `main.lua` and `strpm-chat-relay.manifest`.

## Build

Normal Vortex/FOMOD package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Output:

```text
dist/STRPluginMessagingAPI-v0.8.2-Vortex.zip
```

E2E + ProxyResolver regression package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1 -IncludeDiagnostic
```

Output:

```text
dist/STRPluginMessagingAPI-v0.8.2-test-Vortex.zip
```

The normal package installs:

```text
Data/SKSE/Plugins/STRPluginMessagingAPI.dll
Data/SKSE/Plugins/STRPluginMessagingAPI.ini
Data/SKSE/Plugins/STRPluginMessagingBridge.dll
```

The test package additionally contains:

```text
Data/SKSE/Plugins/STRPluginMessagingDiagnostic.dll
```

## Expected v0.8.2 bridge logs

```text
ProxyResolver loaded resolver: anchorCopies=... flexibleXrefs=... functions=...
ProxyResolver unloaded resolver: anchorCopies=... flexibleXrefs=... functions=...
ProxyResolver native lifecycle hooks armed: ...
ProxyResolver call step-over controller armed
ProxyResolver sender identity observer attached to validated OnConsume breakpoint
ProxyResolver observed STR transport connected
ProxyResolver proxy observed playerId=... formId=0xFF......
ProxyResolver mapping ready connection=... formId=0xFF......
```

`mapping ready` should now appear only when a mapping is added or actually changed, not once per periodic flush.

On disconnect:

```text
ProxyResolver observed STR transport disconnected; clearing mappings
```

## Remaining work

- runtime-regression-test the v0.8.2 reconnect/log cleanup;
- discover/report the local STR connection ID directly;
- finalize `Host` target semantics;
- migrate AnimSyncTogether first to STRPM messaging + ProxyResolver;
- then migrate OStimTogether, MorphSyncTogether, IEDSyncTogether and TradeTogether;
- remove obsolete experimental 0.6.x UI-suppression source files after the regression cycle.

## Design rule

All STR-version-specific discovery and hooking belongs inside `STRPluginMessagingBridge.dll`. The public API and consumer mods remain independent of STR internal addresses so a future STR update only requires a bridge compatibility update.
