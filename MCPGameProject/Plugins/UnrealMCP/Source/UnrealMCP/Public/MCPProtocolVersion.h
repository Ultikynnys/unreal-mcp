#pragma once

#include "CoreMinimal.h"

/**
 * Contract version between this C++ bridge and the sanctioned Python server
 * (Python/unreal_mcp_server.py).
 *
 * The server reads this header to learn the version it expects and refuses any call whose
 * reply does not report the same value, so a stale editor - one that was not rebuilt after
 * the contract changed - fails loudly with an instruction to rebuild the plugin instead of
 * silently answering with an out-of-date envelope or parameter set.
 *
 * Bump this whenever the reply envelope or a command's parameters change.
 */
#define MCP_PROTOCOL_VERSION TEXT("2")
