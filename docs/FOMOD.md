# STRPM FOMOD installation

STRPluginMessagingAPI v0.10.1 ships as a single Vortex/FOMOD archive containing both supported Skyrim runtime variants plus the shared STR server resource.

## Installation flow

### 1. Installation Type

Choose one:

```text
Client + Server
Client Only
Server Files Only
```

`Client + Server` is recommended for the player hosting the Skyrim Together Reborn server.

### 2. Skyrim Runtime

This step is shown only when client files are being installed.

Choose the runtime that matches `SkyrimSE.exe`:

```text
Skyrim 1.6.1170
Skyrim 1.7.104
```

The archive contains dedicated SKSE DLL variants for both runtimes, compiled from the same STRPM v0.10.1 source. The selected runtime installs:

```text
Data/SKSE/Plugins/STRPluginMessagingAPI.dll
Data/SKSE/Plugins/STRPluginMessagingBridge.dll
Data/SKSE/Plugins/STRPluginMessagingAPI.ini
```

The `-test` package additionally installs the matching:

```text
Data/SKSE/Plugins/STRPluginMessagingDiagnostic.dll
```

## Server files

When server files are selected, the FOMOD installs:

```text
Data/SkyrimTogetherReborn/resources/strpm-chat-relay/main.lua
Data/SkyrimTogetherReborn/resources/strpm-chat-relay/strpm-chat-relay.manifest
```

The server relay is shared by the Skyrim 1.6.1170 and 1.7.104 client variants.

## Why this path is correct

STR 1.8.4's server resource loader uses a `resources` directory relative to the server process working directory. The standard STR archive places the server under `Data/SkyrimTogetherReborn`, therefore the FOMOD destination is:

```text
Data/SkyrimTogetherReborn/resources/strpm-chat-relay
```

After starting the server, verify:

```text
[STRPM] Chat relay v4 loaded (authenticated identity payload enabled)
```
