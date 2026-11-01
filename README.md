# STR Plugin Messaging API

Shared messaging and STR player-proxy resolution for Skyrim Together Reborn compatibility mods.

Current development version: **v0.11.6**.

STRPluginMessagingAPI gives SKSE mods one common layer over the **Skyrim Together Reborn 1.8.4 connection**. It is intended for mods such as AnimSyncTogether, OStimTogether, MorphSyncTogether, IEDSyncTogether and TradeTogether so each project does not need to duplicate networking or guess which Skyrim Actor represents a remote STR player.

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
STR 1.8.4 runtime
    ↓
official STR server + strpm-chat-relay Lua resource
```

STRPM payloads use reserved `STRPM|v2|...` chat envelopes. The bridge intercepts those packets before STR's normal chat UI so transport packets do not appear as yellow chat messages. Ordinary STR chat remains untouched.

## v0.11.6 — Rendered custom-name serialization fix

v0.11.6 serializes the custom name exactly as Skyrim renders it for the concrete inventory instance (`ExtraDataList::GetDisplayName(object)`), then trims it to `customNameLength`. This preserves the player's visible capitalization while still excluding tempering suffixes. The receiver continues to rebuild the name through `ExtraTextDisplayData::SetName()`.

## v0.11.5 — Custom-name casing and apply verification fix

v0.11.5 fixes reconstruction of player-renamed items on the receiving client. STRPM now creates or reuses `ExtraTextDisplayData` and calls Skyrim's `SetName()` path instead of constructing `ExtraTextDisplayData` directly from the string. This preserves the original casing and makes the post-add exact-instance verification observe the same logical custom name, preventing the spurious `transport error` after a successful transfer.

## v0.11.4 — Custom-name exact matching fix

v0.11.4 fixes player-renamed item matching in Inventory Bridge v2. Skyrim's `ExtraTextDisplayData` can expose a backing display-name buffer longer than the logical player-set name indicated by `customNameLength`. STRPM now serializes the exact logical custom-name slice, applies the same control-character sanitization used by TradeTogether, and ignores quest/text-display variants that are not player-set custom names.

## v0.11.3 — Extended and tempered item transfer fixes

v0.11.3 fixes transfers of forge-improved/tempered and other ExtraData-backed equipment. Exact `ExtraDataList` removal is used for per-instance metadata, and post-removal inventory verification is deferred to the next game-task tick so Skyrim has time to reflect the mutation before STRPM decides whether to commit or roll back.

This applies to native instance data such as charge, enchantment, temper/health, poison and soul, as well as the STRPM-owned custom-name/ownership/rank/`CannotWear` extensions.

## v0.11.2 — ExtraDataList allocation fix

v0.11.2 fixes the extended Inventory Bridge build/runtime path by allocating empty instance-data lists through STR's own native `GetExtraDataFromItem` path instead of the non-linkable CommonLibSSE `ExtraDataList` constructor. STRPM seeds a temporary health node only to make STR allocate the list, removes that sentinel immediately, then applies the STRPM-owned metadata.

## v0.11.1 — Extended inventory instance metadata

v0.11.1 extends Inventory Interface v2 with instance metadata that stock STR 1.8.4 does not serialize in `Inventory::Entry`.

Added support:

- player-set custom display names via `ExtraTextDisplayData`;
- portable ownership via `ExtraOwnership` and STR `GameId` mapping;
- owner rank via `ExtraRank`;
- `CannotWear` via `ExtraCannotWear`;
- exact-instance removal for extended entries so two otherwise identical items with different metadata are not confused;
- reconstruction of STR-native ExtraData followed by STRPM-owned metadata on the receiving client;
- rejection rather than silent downgrade when instance metadata cannot be transported.

The extended ABI remains `InventoryInterface v2` and uses the reserved `strpm.inventory.v2` channel.

The inventory bridge is **not runtime-validated yet**. The existing STRPM 0.10.1 messaging and ProxyResolver behavior remains the validated baseline.

## v0.11.0 — Initial Inventory Bridge

v0.11.0 introduced the first versioned inventory service: portable STR `GameId` inventory snapshots, STR-native charge/temper/poison/soul/custom-enchant extraction, owner-to-owner PREPARE/READY/COMMIT/APPLIED transfers, and authenticated server mod-table mapping.

The initial inventory layout did not yet preserve custom display names, ownership/rank or `CannotWear`.

## v0.10.1 — Skyrim Together Reborn 1.8.4 compatibility

v0.10.1 targets **Skyrim Together Reborn 1.8.4** on both supported STRPM runtime profiles. The package is now built as a **single multi-runtime FOMOD** containing dedicated SKSE DLL variants for Skyrim **1.6.1170** and **1.7.104**, while keeping one shared STRPM version number and one shared wire protocol.

The public chat protocol itself did not change between STR 1.8.3 and 1.8.4:

```text
SendChatMessageRequest:       37
NotifyChatMessageBroadcast:   35
```

The chat message classes, chat UI anchor, remote-player lifecycle anchors, and server Lua bindings used by STRPM are unchanged.

However, STR 1.8.4 changed the native `TransportService::Send` ABI from:

```cpp
Send(const ClientMessage& message)
```

to:

```cpp
Send(const ClientMessage& message, EPacketFlags flags = kReliable)
```

STRPM calls this native function directly, so v0.10.1 now passes `kReliable` explicitly. In the STR 1.8.4 TiltedConnect revision, `kReliable` is enum value `0`.

The package contains these runtime profiles:

```text
Skyrim SE 1.6.1170
SKSE 2.2.8
Skyrim Together Reborn 1.8.4
STRPM 0.10.1
Status: runtime-validated

Skyrim SE 1.7.104
SKSE 2.3.1
Skyrim Together Reborn 1.8.4
STRPM 0.10.1
Status: source-audited; runtime validation pending
```

Both variants are compiled from the same v0.10.1 source and retain the validated STR 1.8.4 transport fix, including the explicit `kReliable` argument to `TransportService::Send`. Only the SKSE runtime metadata is different between the packaged DLL variants.

## v0.10.0 — Skyrim Together Reborn 1.8.3 compatibility

v0.10.0 retargets the native bridge from STR 1.8.0 to **STR 1.8.3**.

TiltedEvolution removed one client opcode and one server opcode between 1.8.0 and 1.8.3. The chat message classes and `TransportService` ABI used by STRPM are otherwise unchanged, but the numeric chat opcodes moved:

```text
SendChatMessageRequest:       38 -> 37
NotifyChatMessageBroadcast:   36 -> 35
```

Those STR-version-specific values are now centralized in `STRPMSTRCompatibility.h` so future STR updates do not require duplicated edits across the send bridge, receive bridge and ProxyResolver metadata observer.

STRPM 0.10.0 SKSE metadata allows these Skyrim runtimes:

```text
1.6.1170
1.6.1179
1.7.104
```

The STRPM DLLs themselves can load on all three runtimes. Skyrim Together Reborn 1.8.3 also provides an official optional client build for the Skyrim 1.6.x branch. STRPM 0.10.0 has been runtime-validated with that STR 1.8.3 build on **Skyrim 1.6.1170 + SKSE 2.2.8** using two connected clients.

Validated on 1.6.1170:
- both players connected successfully;
- native send and receive hooks resolved;
- bidirectional E2E messaging completed;
- ProxyResolver resolved both remote player proxies;
- STRPM transport packets were suppressed from the yellow STR chat UI;
- no STRPM-related crash or connection instability was observed during the validation session.

Skyrim 1.6.1179 remains accepted by STRPM metadata but has **not** been runtime-tested by this project.


## v0.9.3 — authenticated identity heartbeat and validated ProxyResolver cold start

v0.9.3 finalizes the ProxyResolver cold-start fix and the chat-suppression regression discovered during two-client testing.

The server relay is now **v4 / 0.4.0**. For the reserved `strpm.identity.v1` heartbeat, the server replaces the empty payload with the authenticated STR `PlayerId` of the sender while retaining the authenticated sender `ConnectionID` metadata. The receiving bridge registers the reserved identity channel through the normal STRPM API and joins:

```text
ConnectionID
    ↓ authenticated relay metadata
PlayerId
    ↓ STR remote-player lifecycle
local proxy FormID
```

This removes the previous dependency on VEH handler ordering when discovering `ConnectionID -> PlayerId`.

The validated two-client regression test confirmed:

- Player1 resolves Player2 to the correct local dynamic proxy FormID;
- Player2 resolves Player1 to the correct local dynamic proxy FormID;
- both diagnostic clients complete the bidirectional handshake;
- both diagnostic clients finish with `PROXY RESOLVE OK`;
- identity bootstrap works before consumer traffic;
- the server no longer prints the transient `Dropped envelope from unknown character` message during the validated test;
- STRPM technical envelopes no longer appear as yellow chat messages;
- ordinary STR chat remains unaffected.

The dedicated OverlayApp chat UI suppression bootstrap, accidentally omitted from the v0.9.1 startup path, is active again. The identity heartbeat remains periodic, but duplicate heartbeat observations are no longer spammed into the bridge log; only first discovery or an identity change is logged.

## v0.9.0 — fixed-buffer receive capture outside the VEH

v0.9.0 changes the STR receive architecture after TradeTogether exposed a receiver freeze that remained possible with v0.8.3.

v0.8.3 deferred the final consumer callback, but the `TransportService::OnConsume` vectored exception handler still performed the expensive part of reception synchronously: raw packet copying into dynamic vectors, chat-string parsing, fragment reconstruction, `unordered_map`/mutex work, hex decoding and delivery into the bridge callback thunk. That work can run while STR's receive path is in an exception/breakpoint context and may participate in lock/reentrancy cycles with gameplay/UI work on another thread.

v0.9.0 moves the entire dynamic receive pipeline out of that context.

The VEH now performs only bounded operations:

```text
TransportService::OnConsume breakpoint / VEH
        ↓
check opcode 36
        ↓
copy raw packet with ReadProcessMemory
into a preallocated fixed slot
        ↓
minimal local prefix check for STRPM|v2|
        ↓
suppress the STRPM chat packet
        ↓
return immediately
```

A dedicated receiver thread then performs the normal dynamic processing:

```text
fixed raw-packet queue
        ↓
raw STR chat decode
        ↓
fragment reconstruction
        ↓
hex payload decode
        ↓
v0.8.3 completed-message FIFO
        ↓
registered consumer callback
```

Important implementation properties:

- 64 preallocated raw capture slots;
- 16 KiB per slot, comfortably above the bridge's 3500-character chat-envelope maximum;
- no `std::string`, `std::vector`, `unordered_map`, mutex, file logging or consumer callback from the receive VEH;
- multiple STR receive threads can reserve slots through atomic state transitions;
- captured packets carry an atomic order number so the dispatcher processes them in capture order;
- if the fixed queue is saturated, the packet is not suppressed and STR is allowed to process it normally rather than silently losing it;
- the public API and `STRPM|v2|` wire prefix remain unchanged.

Consumer callbacks are still **not Skyrim's game thread**. Consumer mods must continue to schedule Skyrim object lookup, UI work and game-state mutation through their normal SKSE/game-thread path.

## v0.8.3 — completed-message callback dispatch

v0.8.3 introduced the first receive-thread safety layer. Completed STRPM messages were copied into a bounded FIFO and consumer callbacks moved onto a bridge dispatcher thread instead of being invoked directly by the `OnConsume` interception callback.

That was necessary but incomplete: raw chat decoding and fragment reconstruction still occurred in the VEH. v0.9.0 retains the v0.8.3 completed-message queue and adds the earlier fixed-buffer raw-packet stage.

## v0.8.2 — session/log cleanup

v0.8.2 is a small cleanup patch on top of the validated v0.8.1 ProxyResolver.

Two runtime behaviors changed:

1. the optional diagnostic client treats each STR connection as a separate E2E session. A disconnect clears peer/ACK state and the next `OnConnected` restarts at `probe=1`;
2. repeated internal ProxyResolver flushes no longer log `mapping ready` when `ConnectionID -> FormID` is unchanged. Public mapping events were already change-only.

## ProxyResolver

Consumer mods must **not** scan Skyrim `ProcessLists`, compare actor names or cache guessed dynamic FormIDs. STRPM owns STR-version-specific resolution and publishes:

```text
STR ConnectionID
      ↕ authenticated relay metadata
STR PlayerId
      ↕ official STR player lifecycle
local Skyrim proxy FormID
```

Relay v4 appends authenticated sender metadata and provides the authenticated sender PlayerId through the reserved identity heartbeat. The bridge joins that identity with the local proxy FormID observed from STR's remote-player lifecycle.

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

## Compatibility target

```text
Skyrim runtime:         1.6.1170        1.7.104
SKSE:                   2.2.8           2.3.1
Skyrim Together Reborn: 1.8.4           1.8.4
STRPM:                  0.11.5          0.11.5
Client chat opcode:     37              37
Server chat opcode:     35              35
Transport Send flags:   kReliable = 0   kReliable = 0
```

Shared upstream target:

```text
TiltedEvolution tag:    v1.8.4
TiltedEvolution commit: 54b3a858e228dc8807ed65bc3f9ac3c3f81b610d
TiltedConnect commit:   e6f5759364a64462e717db1153781a7eae26a13e
```

The **1.6.1170** profile is runtime-validated. The **1.7.104** profile uses the same STRPM API, bridge logic and wire protocol, but remains pending two-client runtime validation.

## Server resource

v0.11.5 continues to use relay **v4 / 0.4.0**. The server console must report:

```text
[STRPM] Chat relay v4 loaded (authenticated identity payload enabled)
```

The wire prefix remains `STRPM|v2|`.

## FOMOD

v0.11.5 is packaged as one archive for both Skyrim runtimes. The installer first asks for the installation type:

```text
Client + Server
Client Only
Server Files Only
```

If client files are being installed, the next step asks for the Skyrim runtime:

```text
Skyrim 1.6.1170
Skyrim 1.7.104
```

The selected runtime installs only its matching SKSE DLL variant. The server relay is shared between both runtimes and is deployed under:

```text
Data/SkyrimTogetherReborn/resources/strpm-chat-relay/
```

## Build

### Normal release package

```powershell
.\build-vortex.ps1
```

Output:

```text
dist/STRPluginMessagingAPI-v0.11.5.zip
```

The public release archive intentionally excludes `STRPluginMessagingDiagnostic.dll`.

### Diagnostic test package

```powershell
.\build-test-vortex.ps1
```

or:

```bat
build-test-vortex.bat
```

Output:

```text
dist/STRPluginMessagingAPI-v0.11.5-test.zip
```

The test archive additionally contains a matching `STRPluginMessagingDiagnostic.dll` in **both runtime payloads**, so either FOMOD choice can be tested with the same test ZIP.

The archive contains separate runtime payloads for 1.6.1170 and 1.7.104. The FOMOD installs only the selected variant into `SKSE/Plugins`.

## Remaining work

- runtime-test STRPM 0.11.5 on Skyrim 1.7.104 + SKSE 2.3.1 + STR 1.8.4;
- validate bidirectional E2E messaging, ProxyResolver mappings and yellow-chat suppression on two clients;
- runtime-test Skyrim 1.6.1179 when a suitable test environment is available;
- discover/report the local STR connection ID directly;
- finalize `Host` target semantics;
- migrate remaining consumers fully to STRPM messaging + ProxyResolver;
- remove obsolete experimental UI-suppression source files after the validated 0.9.3 release cycle.

## Design rule

All STR-version-specific discovery and hooking belongs inside `STRPluginMessagingBridge.dll`. The public API and consumer mods remain independent of STR internal addresses so a future STR update only requires a bridge compatibility update.
