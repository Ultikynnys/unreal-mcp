#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IPluginManager.h"

// The plugin's OWN version, read from its descriptor at runtime. Project-independent by design:
// the same build reports the same version in any host, so the server never compares a checkout.
inline FString UnrealMCPPluginVersion()
{
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP")))
	{
		return Plugin->GetDescriptor().VersionName;
	}
	return TEXT("unknown");
}
