#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	UEAGENTINTERFACE_API bool IsExternalLocalizationContentGraphRoute(
		const FString& ProviderBinding,
		const FString& Capability);

	UEAGENTINTERFACE_API bool TryExecuteExternalLocalizationContentGraph(
		const FString& Capability,
		const TSharedPtr<FJsonObject>& Parameters,
		TSharedPtr<FJsonObject>& OutObservation,
		FString& OutError);
}
