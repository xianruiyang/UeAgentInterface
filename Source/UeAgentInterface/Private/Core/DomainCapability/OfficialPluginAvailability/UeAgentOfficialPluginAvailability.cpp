#include "Core/DomainCapability/OfficialPluginAvailability/UeAgentOfficialPluginAvailability.h"

#include "Interfaces/IPluginManager.h"

namespace UE::UAI::DomainCapability::OfficialPluginAvailability
{
	FString FOfficialPluginAvailabilityObservation::GetReason() const
	{
		switch (Availability)
		{
		case EOfficialPluginAvailability::Available:
			return TEXT("available");
		case EOfficialPluginAvailability::Disabled:
			return TEXT("disabled");
		case EOfficialPluginAvailability::NotInstalled:
		default:
			return TEXT("not_installed");
		}
	}

	FOfficialPluginAvailabilityObservation Evaluate(
		const FString& PluginName,
		const bool bInstalled,
		const bool bEnabled)
	{
		FOfficialPluginAvailabilityObservation Result;
		Result.PluginName = PluginName;
		if (!bInstalled)
		{
			Result.Availability =
				EOfficialPluginAvailability::NotInstalled;
		}
		else if (!bEnabled)
		{
			Result.Availability = EOfficialPluginAvailability::Disabled;
		}
		else
		{
			Result.Availability = EOfficialPluginAvailability::Available;
		}
		return Result;
	}

	FOfficialPluginAvailabilityObservation Inspect(const FString& PluginName)
	{
		const TSharedPtr<IPlugin> Plugin =
			IPluginManager::Get().FindPlugin(PluginName);
		return Evaluate(
			PluginName,
			Plugin.IsValid(),
			Plugin.IsValid() && Plugin->IsEnabled());
	}
}
