#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Templates/SharedPointer.h"

#define UAI_LONG_RUNNING_SESSION_ACTIVATION_V1 1

namespace UE::UAI::Composition::LongRunningSession
{
	struct UEAGENTINTERFACE_API
	FUeAgentLongRunningSessionActivationProfile final
	{
		explicit FUeAgentLongRunningSessionActivationProfile(
			Contract::FUeAgentDescriptorIdentity InDescriptorIdentity)
			: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		{
		}

		FString CommandId;
		FString Namespace;
		FString Method;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;
		FString HandlerBinding;
		FString CommandKind;

		bool IsValid() const;
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
		bool IsStartCommand() const;
		bool IsLifecycleCommand() const;
	};

	UEAGENTINTERFACE_API bool TryGetLongRunningSessionActivationProfiles(
		const TArray<FUeAgentLongRunningSessionActivationProfile>*& OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateLongRunningSessionDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	/**
	 * Builds the outermost production registry containing the current
	 * production contracts plus the thirteen LongRunningSession contracts.
	 */
	UEAGENTINTERFACE_API bool
	TryCreateLongRunningSessionProductionCommandContractRegistry(
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);
}
