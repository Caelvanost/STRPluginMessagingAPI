# Migration Notes For My Mods

These notes describe how my current STR-based mods could move from their own
UDP sockets to the proposed STR plugin messaging API.

## Shared Plan

Each mod can keep its existing payload format at first and only replace the
transport layer.

Current transport responsibilities to remove later:

- LAN discovery.
- Manual peer IP and port config.
- Shared secret signing.
- Peer cache and expiration.
- Relay handling.
- Per-mod UDP port.

Replacement responsibilities:

- Register one or more STR plugin channels.
- Send payloads to `kAllPlayers` or a specific `ConnectionID`.
- Use callback payloads as the existing packet entry point.
- Resolve the sender's remote actor using sender display name first, then
  connection ID once STR exposes a stable actor mapping.

## OStimTogether

Suggested channel: `chaos.ostim_together.scene.v1`.

Use reliable and ordered messages for scene start, node, speed, and stop events.
Furniture alignment and actor pose data can stay in the same payload format
initially.

## MorphSyncTogether

Suggested channels:

- `chaos.morph_sync_together.body_morph.v1`
- `chaos.morph_sync_together.overlay.v1`

Snapshots can remain chunked if they approach the payload limit. Periodic
snapshots may use default delivery, while multi-chunk snapshots should request
reliable ordered delivery if STR supports it.

## IEDSyncTogether

Suggested channel: `chaos.ied_sync_together.slots.v1`.

The current `plugin + local FormID` payload can stay unchanged. The receiver
still applies the visual slot data only to STR remote-player proxies.

## TradeTogether

Suggested channel: `chaos.trade_together.offer.v1`.

Trade requests should target a specific `ConnectionID` and use reliable ordered
delivery. The consent dialog and inventory opening logic can remain entirely
inside the mod.

## Fallback

Until STR exposes the API, each mod should keep its current UDP transport. The
future migration can choose the STR transport first and fall back to UDP only
when the API is missing or too old.
