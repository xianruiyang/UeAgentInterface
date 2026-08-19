#pragma once

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "CoreMinimal.h"

class UBlueprint;

namespace UE::UAI::DomainAdapters::BlueprintCommands
{
	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API bool IsQuery(const FString& CommandId);
	UEAGENTINTERFACE_API bool TryGetActivationProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>&
			OutProfiles,
		FString& OutError);

	struct UEAGENTINTERFACE_API FBlueprintComponentEventDiscoveryRequest final
	{
		FString ComponentName;
		FString Query;
		int32 Limit = 200;
		bool bOnlyUnbound = false;
		bool bIncludeHidden = false;
		bool bCompileBeforeQuery = true;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FBlueprintComponentEventRecord final
	{
		FString EventRef;
		FString NativeFieldName;
		FString SignatureRef;
		FString DisplayName;
		FString Category;
		bool bHidden = false;
		bool bAlreadyBound = false;
	};

	struct UEAGENTINTERFACE_API FBlueprintComponentEventDiscoveryResult final
	{
		FString AssetPath;
		FString ComponentName;
		FString TargetClassRef;
		bool bCompiledBeforeQuery = false;
		bool bComponentPropertyFound = false;
		bool bCanAuthorComponentBoundEvent = false;
		int32 TotalEventCount = 0;
		int32 MatchedEventCount = 0;
		bool bTruncated = false;
		TArray<FBlueprintComponentEventRecord> Events;
	};

	UEAGENTINTERFACE_API bool TryDiscoverComponentEvents(
		UBlueprint& Blueprint,
		const FBlueprintComponentEventDiscoveryRequest& Request,
		FBlueprintComponentEventDiscoveryResult& OutResult,
		FString& OutError);
}
