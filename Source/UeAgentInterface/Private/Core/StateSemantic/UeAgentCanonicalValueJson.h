#pragma once

#include "CoreMinimal.h"

class FJsonValue;

namespace UE::UAI::ContextResource
{
	class FUeAgentResourceTracker;
}

namespace UE::UAI::StateSemantic
{
	class FUeAgentCanonicalValue;

	namespace CanonicalJson
	{
		UEAGENTINTERFACE_API bool TryFromJsonValue(
			const TSharedPtr<FJsonValue>& Value,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			TSharedPtr<const FUeAgentCanonicalValue>& OutValue,
			FString& OutError);

		UEAGENTINTERFACE_API bool TryToJsonValue(
			const FUeAgentCanonicalValue& Value,
			TSharedPtr<FJsonValue>& OutValue,
			FString& OutError);
	}
}
