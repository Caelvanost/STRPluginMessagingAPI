#pragma once

#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

// Lightweight declaration for bridge components that only need to query the
// authoritative STR connection lifecycle. Do not include the full
// STRPMProxyResolverBridge.h from multiple translation units: it intentionally
// owns the ProxyResolver hook implementation.
extern "C" bool STRPM_CALL STRPM_IsSTRSessionConnected();
