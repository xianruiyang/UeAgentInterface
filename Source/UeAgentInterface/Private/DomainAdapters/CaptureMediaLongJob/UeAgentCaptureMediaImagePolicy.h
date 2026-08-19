#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobAdapter.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	class UEAGENTINTERFACE_API
	FUeAgentCaptureMediaImagePolicyDescriptor final
	{
	public:
		FUeAgentCaptureMediaImagePolicyDescriptor(
			FString InPolicyId,
			FString InMechanicsId);

		const FString& GetPolicyId() const;
		const FString& GetMechanicsId() const;
		bool IsValid() const;

	private:
		FString PolicyId;
		FString MechanicsId;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentCaptureMediaImagePolicyDescriptor>&
	GetCaptureMediaImagePolicies();

	/**
	 * Routes one validated image policy to the existing native screenshot
	 * mechanics. The adapter owns policy/subject translation and output
	 * confinement only; the selected command member remains the sole UE
	 * capture implementation.
	 */
	UEAGENTINTERFACE_API bool TryExecuteCaptureMediaImagePolicy(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		const FString& PreferredOutputPath,
		FString& OutProducedPath,
		FString& OutRequestedFinalPath,
		FString& OutFormat,
		FString& OutError);
}
