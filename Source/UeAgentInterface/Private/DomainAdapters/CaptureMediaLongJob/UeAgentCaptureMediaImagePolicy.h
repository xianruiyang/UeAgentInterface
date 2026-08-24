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
			FString InMechanicsId,
			FString InParameterSchemaJson = FString(),
			FString InParameterGuidance = FString(),
			FString InResultGuidance = FString(),
			FString InMinimumExample = FString());

		const FString& GetPolicyId() const;
		const FString& GetMechanicsId() const;
		const FString& GetParameterSchemaJson() const;
		const FString& GetParameterGuidance() const;
		const FString& GetResultGuidance() const;
		const FString& GetMinimumExample() const;
		bool HasDiscoveryContract() const;
		bool IsValid() const;

	private:
		FString PolicyId;
		FString MechanicsId;
		FString ParameterSchemaJson;
		FString ParameterGuidance;
		FString ResultGuidance;
		FString MinimumExample;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentCaptureMediaImagePolicyDescriptor>&
	GetCaptureMediaImagePolicies();

	UEAGENTINTERFACE_API const FUeAgentCaptureMediaImagePolicyDescriptor*
	FindCaptureMediaImagePolicy(const FString& PolicyId);

	UEAGENTINTERFACE_API bool TryNormalizeTransformCandidateParameters(
		const TSharedPtr<FJsonObject>& Parameters,
		FString& OutError);

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
