#pragma once

#include "CoreMinimal.h"

// The commit this plugin was BUILT from, injected by UnrealMCP.Build.cs as MCP_REVISION. The
// Python server reads the same value off every reply and refuses a call when its own checkout is
// on a different commit, so a stale editor fails loudly instead of answering an old contract.
#ifndef MCP_REVISION
#define MCP_REVISION TEXT("unknown")
#endif

// 1 when the module tree had uncommitted changes at build time. Reported for diagnosis only: a
// commit cannot identify a dirty build's code, but refusing those would block the edit-build loop.
#ifndef MCP_REVISION_DIRTY
#define MCP_REVISION_DIRTY 0
#endif
