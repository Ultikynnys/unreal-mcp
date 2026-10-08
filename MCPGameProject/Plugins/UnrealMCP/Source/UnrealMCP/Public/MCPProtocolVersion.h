#pragma once

#include "CoreMinimal.h"

// Contract version between this bridge and the Python server, which reads this header and
// refuses any reply reporting a different value - so a stale editor fails loudly instead of
// answering with an out-of-date envelope. Bump on any envelope or parameter change.
#define MCP_PROTOCOL_VERSION TEXT("2")
