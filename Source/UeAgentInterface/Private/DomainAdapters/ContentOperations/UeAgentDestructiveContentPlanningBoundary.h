#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FJsonObject;
class UWorld;

namespace UE::UAI::DomainAdapters::ContentOperations
{
	struct UEAGENTINTERFACE_API FUeAgentDestructiveContentPlan final
	{
		FString PlanId;
		FString PlanFingerprint;
		FString AuthorizationId;
		TArray<FString> ImpactScope;
		TArray<FString> RevisionSnapshot;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool TryBuildDestructiveContentPlan(
		const FString& CommandId,
		const TSharedPtr<FJsonObject>& Params,
		UWorld* ExplicitWorld,
		FUeAgentDestructiveContentPlan& OutPlan,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCollectOwnedWorldExternalPackages(
		const FString& WorldPackageName,
		int32 MaxPackages,
		TArray<FString>& OutPackageNames,
		FString& OutError);
}
