#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::DomainAdapters::AssetSimple
{
	struct FReflectionAssetHjsonConfig
	{
		FString ProfileId;
		FString RootType;
		FString ProviderName;
		TArray<FString> OwnedProperties;
	};
	UEAGENTINTERFACE_API TSharedRef<HjsonWorkspace::IImportInitializationDomainProvider>
		CreateReflectionAssetHjsonProvider(FReflectionAssetHjsonConfig Config);
}
