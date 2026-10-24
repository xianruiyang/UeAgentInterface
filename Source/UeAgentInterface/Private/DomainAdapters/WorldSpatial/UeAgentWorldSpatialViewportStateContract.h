#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	// The state snapshot and patch replace this subset, not all engine show flags.
	// Native read/apply and descriptor generation consume the same contract.
	inline const TArray<FString>& GetViewportStateShowFlags()
	{
		static const TArray<FString> Flags{TEXT("Lighting"), TEXT("StaticMeshes")};
		return Flags;
	}
}
