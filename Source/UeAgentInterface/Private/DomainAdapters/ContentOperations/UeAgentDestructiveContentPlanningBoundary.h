#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FJsonObject;
class UWorld;

namespace UE::UAI::DomainAdapters::ContentOperations
{
	struct UEAGENTINTERFACE_API FUeAgentDestructiveContentPlanTarget final
	{
		FString TargetRef;
		FString PackageName;
		FString ObjectPath;
		TArray<FString> Sources;
		UE::UAI::Contract::FDiagnostic BlockerDiagnostic;
	};

	struct UEAGENTINTERFACE_API FUeAgentDestructiveContentPlan final
	{
		FString PlanId;
		FString PlanFingerprint;
		FString AuthorizationId;
		FString CanonicalParamsJson;
		TArray<FString> ImpactScope;
		TArray<FString> RevisionSnapshot;
		TArray<FUeAgentDestructiveContentPlanTarget> Targets;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool TryBuildDestructiveContentPlan(
		const FString& CommandId,
		const TSharedPtr<FJsonObject>& Params,
		UWorld* ExplicitWorld,
		FUeAgentDestructiveContentPlan& OutPlan,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCollectOwnedWorldExternalPackages(
		const FString& WorldPackageName,
		TArray<FString>& OutPackageNames,
		UE::UAI::Contract::FDiagnostic& OutError);
}
