#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::DomainCapability::OfficialPluginAvailability
{
	enum class EOfficialPluginAvailability : uint8
	{
		Available,
		NotInstalled,
		Disabled,
	};

	struct UEAGENTINTERFACE_API FOfficialPluginAvailabilityObservation
	{
		FString PluginName;
		EOfficialPluginAvailability Availability =
			EOfficialPluginAvailability::NotInstalled;

		bool IsAvailable() const
		{
			return Availability == EOfficialPluginAvailability::Available;
		}

		FString GetReason() const;
	};

	/** Pure status normalization used by production inspection and tests. */
	UEAGENTINTERFACE_API FOfficialPluginAvailabilityObservation Evaluate(
		const FString& PluginName,
		bool bInstalled,
		bool bEnabled);

	/** Reads the process plugin manager without loading, enabling, or mutating it. */
	UEAGENTINTERFACE_API FOfficialPluginAvailabilityObservation Inspect(
		const FString& PluginName);
}
