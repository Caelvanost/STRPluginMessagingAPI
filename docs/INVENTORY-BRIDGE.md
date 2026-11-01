# STRPM Inventory Bridge v2 (STRPM 0.11.5)

The inventory bridge is an experimental STRPM service that exposes Skyrim Together Reborn 1.8.4's canonical `Inventory::Entry` representation to consumer SKSE mods without exposing Skyrim `ExtraDataList` objects.

It is developed on the `inventory-service` branch as **STRPM 0.11.5**.

## Responsibility split

STRPM owns:

- translating the local Skyrim inventory into portable STR-style entries;
- preserving supported instance data such as temper/health, charge, poison, soul, custom enchant effects, custom display names, ownership/rank and `CannotWear`;
- translating local FormIDs through the authenticated STR server mod table;
- owner-to-owner transfer coordination;
- applying local inventory mutations through STR 1.8.4's own `TESObjectREFR::AddOrRemoveItem(Inventory::Entry)` path.

A consumer such as TradeTogether owns:

- trade UI;
- offer/ready/accept/cancel state;
- deciding which items are allowed in a trade;
- user-facing error handling;
- higher-level trade transaction policy.

The consumer never needs to inspect or reconstruct `ExtraDataList`.

## Public interface

Load the service from the bridge DLL:

```cpp
const auto* inventory = STRPM::LoadInventoryFromModule();
if (!inventory) {
    // Inventory bridge not ready.
}
```

### Read the local inventory

The snapshot API uses a two-call buffer pattern:

```cpp
std::size_t count = 0;
auto result = inventory->getLocalInventory(nullptr, 0, &count);
if (result != STRPM::Result::kOk) {
    // Handle not-connected / not-available.
}

std::vector<STRPM::InventoryItem> items(count);
result = inventory->getLocalInventory(items.data(), items.size(), &count);
```

Each `InventoryItem` contains:

- `localBaseFormID`: local-only FormID for UI/game lookup;
- `entry`: portable STRPM/STR inventory representation.

The portable entry contains the STR `GameId`, count, charge, enchantment ID and charge, temporary enchant effects, temper/health, poison, soul, custom display name, portable owner `GameId`, owner rank, `CannotWear` and instance flags.

The snapshot must currently be requested from Skyrim's game thread.

### Transfer an entry

```cpp
STRPM::InventoryTransferID transferID = 0;
const auto result = inventory->transfer(
    remoteConnectionID,
    &item.entry,
    &transferID);
```

The transfer is asynchronous. Register a listener to observe progress:

```cpp
inventory->registerTransferListener(&OnInventoryTransfer, userData);
```

Possible states include:

```text
Prepared
RemovingLocal
Committing
ApplyingRemote
Complete
Failed
RolledBack
```

## Transfer protocol

STR's stock server rejects a client attempting to mutate a different player's owned actor inventory. STRPM therefore coordinates two owner-local changes:

```text
P1                         P2
 |---- PREPARE ----------->|
 |<--- READY --------------|
 | remove locally via STR  |
 |---- COMMIT ------------>|
 |                         | add locally via STR
 |<--- APPLIED ------------|
 | complete                |
```

Both local mutations pass through STR's native inventory path, allowing stock STR InventoryService hooks to synchronize each owner inventory normally.

Duplicate COMMIT messages are idempotent on the recipient. Once a COMMIT has successfully entered the transport, STRPM does not automatically re-add the sender's item on a later missing ACK because that could duplicate an item that was already applied remotely.

## Canonical FormID mapping

STRPM observes the authenticated STR `AuthenticationResponse` and captures its server mod table. Local Skyrim FormIDs are mapped to STR `GameId` values through that table, so consumers do not need to assume identical load-order indices between clients.

## Current restrictions

The bridge deliberately rejects:

- temporary **base** forms (`GameId.ModId == UINT32_MAX`), because stock STR 1.8.4 does not carry enough recipe data to reconstruct arbitrary crafted temporary base objects on another client;
- equipped/worn inventory instances;
- quest inventory instances;
- temporary poison forms;
- custom enchantments with more than 32 effects.

Temporary/custom enchantments on normal base items are supported through STR's own `EnchantData` representation.

These restrictions can be relaxed later as individual paths are validated.

## Runtime status

The underlying STRPM 0.10.1 messaging/ProxyResolver baseline remains runtime-validated on Skyrim 1.6.1170 + SKSE 2.2.8 + STR 1.8.4.

The Inventory Bridge itself is **source-audited and compile-tested only until a two-client runtime test is completed**. Do not treat it as release-ready before that validation.

## Extended instance data

Inventory Interface v2 adds STRPM-owned fields that stock STR 1.8.4 does not carry in `Inventory::Entry`:

- player-set custom display name (`ExtraTextDisplayData`);
- ownership (`ExtraOwnership`) using a portable STR `GameId`;
- owner rank (`ExtraRank`);
- `CannotWear` (`ExtraCannotWear`).

For entries using any of these fields, STRPM removes the exact matching local `ExtraDataList` rather than asking STR to remove an ambiguous equivalent stack. On the recipient, STRPM asks STR to reconstruct the normal `Inventory::Entry` extra data, augments that list with the STRPM-owned fields, then adds the item through Skyrim's container path so STR's inventory hooks still observe the mutation.

The inventory wire channel for this ABI is `strpm.inventory.v2`. Builds using the earlier experimental v1 layout are intentionally incompatible.
