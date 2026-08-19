#pragma once

#include "CoreMinimal.h"

#define UAI_CAPTURE_MEDIA_LONG_JOB_CONTRACT_V1 1

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	enum class EUeAgentCaptureMediaLongJobFamily : uint8
	{
		ImageCapture,
		MediaRecording,
		RenderResource,
	};

	class UEAGENTINTERFACE_API
	FUeAgentCaptureMediaLongJobFamilyContract final
	{
	public:
		FUeAgentCaptureMediaLongJobFamilyContract(
			EUeAgentCaptureMediaLongJobFamily InFamily,
			FString InFamilyId,
			FString InSubjectKind,
			FString InPolicyKind,
			FString InSignalTypeId);

		EUeAgentCaptureMediaLongJobFamily GetFamily() const;
		const FString& GetFamilyId() const;
		const FString& GetSubjectKind() const;
		const FString& GetPolicyKind() const;
		const FString& GetSignalTypeId() const;

	private:
		EUeAgentCaptureMediaLongJobFamily Family;
		FString FamilyId;
		FString SubjectKind;
		FString PolicyKind;
		FString SignalTypeId;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentCaptureMediaLongJobFamilyContract>&
	GetCaptureMediaLongJobFamilyContracts();

	UEAGENTINTERFACE_API const
	FUeAgentCaptureMediaLongJobFamilyContract*
	FindCaptureMediaLongJobFamilyContract(
		EUeAgentCaptureMediaLongJobFamily Family);
}
