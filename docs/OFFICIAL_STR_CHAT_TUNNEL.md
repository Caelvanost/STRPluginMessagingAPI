# Official STR chat-tunnel transport

This transport keeps the stock Skyrim Together client protocol and stock server binary.
The server-side relay uses only the documented STR scripting API.

## Confirmed stock STR path

Outgoing chat in the official client is converted to `SendChatMessageRequest` by
`OverlayClient::ProcessChatMessage()` and passed to `TransportService::Send()`.
Incoming server chat is decoded as `NotifyChatMessageBroadcast` and dispatched to
`OverlayService::OnChatMessageReceived()` before being forwarded to the CEF overlay.

The stock server scripting API exposes `onChatMessage`, `cancelEvent`,
`GameServer:SendChatMessage(connectionId, message)` and
`GameServer:SendGlobalChatMessage(message)`. This is enough to implement routing without
changing STR opcodes or rebuilding the server.

## Remaining client-side boundary

The official STR client does not expose a documented C/SKSE ABI for either
`TransportService::Send()` or the `NotifyChatMessageBroadcast` dispatcher. Therefore a
client bridge cannot call or subscribe to those facilities through a supported public
interface today.

The practical stock-client implementation is a thin, version-specific bridge that:

1. Locates the official client's outgoing chat path.
2. Injects an STRPM envelope as an ordinary `SendChatMessageRequest`.
3. Observes `NotifyChatMessageBroadcast` before the message reaches the overlay.
4. Consumes `STRPM|v2|...` envelopes and converts them to STRPM receive callbacks.
5. Leaves all non-STRPM chat untouched.

All version-specific discovery/hooking must stay in `STRPluginMessagingBridge.dll`; the
public STRPM API and the server resource remain version-independent.

## Envelope v2

Each transport fragment is an ASCII chat payload:

```text
STRPM|v2|msg=<id>|seq=<n>|channel=<channel>|target=<target>|flags=<n>|part=<i>|parts=<n>|payload=<hex>
```

The relay appends authenticated sender metadata derived from the STR server session:

```text
|sender=<connectionId>|senderName=<name>|serverTick=<tick>
```

Targets supported by the scripting relay are:

- `all`
- `id:<connectionId>`
- `server`

`host` cannot be resolved through the documented scripting API and must not silently
fall back to broadcast.

## Fragmentation

The resource accepts individual envelopes up to 4096 characters and at most 64
fragments per logical message. The client bridge is responsible for fragmentation and
reassembly. Fragment identifiers should be unique for the local session and sequence.

## Visibility

The server calls `cancelEvent()` for client-originated STRPM envelopes so they are not
rebroadcast as normal chat. Server-to-client relays still arrive as standard STR chat
packets, so the client bridge must intercept them before `OverlayService` forwards them
to the CEF UI. Without that client interception, transport envelopes will be visible.

## Compatibility conclusion

The server can stay completely stock. The network protocol and opcode tables can stay
completely stock. A small client-side compatibility bridge is still required because
STR currently exposes no public plugin messaging or chat injection/subscription ABI.
