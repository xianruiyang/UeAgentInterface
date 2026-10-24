#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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
		UE::UAI::Contract::FDiagnostic& OutError);

	struct UEAGENTINTERFACE_API FBlueprintComponentEventDiscoveryRequest final
	{
		FString ComponentName;
		UE::UAI::Contract::FSourceLocation ComponentNameSource;
		FString ComponentNameSourceKind = TEXT("native_result");
		FString Query;
		bool bOnlyUnbound = false;
		bool bIncludeHidden = false;
		bool bCompileBeforeQuery = true;
		bool bIncludeQueryContext = false;

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
		bool bQueryMatched = true;
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
		TArray<FBlueprintComponentEventRecord> Events;
	};

	UEAGENTINTERFACE_API bool TryDiscoverComponentEvents(
		UBlueprint& Blueprint,
		const FBlueprintComponentEventDiscoveryRequest& Request,
		FBlueprintComponentEventDiscoveryResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError);

	/** Enumerate readable component selectors for the shared component-event owner. */
	UEAGENTINTERFACE_API bool TryGetBlueprintComponentNames(
		UBlueprint& Blueprint,
		TArray<FString>& OutComponentNames,
		UE::UAI::Contract::FDiagnostic& OutError);
}
