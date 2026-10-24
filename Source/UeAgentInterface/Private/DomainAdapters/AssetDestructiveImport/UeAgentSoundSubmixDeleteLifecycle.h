#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "AudioDeviceHandle.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	// Request-scoped native registration state; durability remains owned by asset delete.
	class UEAGENTINTERFACE_API FSoundSubmixDeleteLifecycle final
	{
	public:
		bool Prepare(UObject& Object, UE::UAI::Contract::FDiagnostic& Error);
		bool Release(const TArray<FString>& SelectedPaths, UE::UAI::Contract::FDiagnostic& Error);
		bool Restore(UE::UAI::Contract::FDiagnostic& Error);
	private:
		struct FEntry
		{
			FString Path;
			TArray<FAudioDeviceHandle> Devices;
			bool bRooted = false;
			bool bSelected = false;
		};
		TArray<FEntry> Entries;
	};
}
