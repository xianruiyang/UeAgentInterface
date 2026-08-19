#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobContract.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	FUeAgentCaptureMediaLongJobFamilyContract::
	FUeAgentCaptureMediaLongJobFamilyContract(
		const EUeAgentCaptureMediaLongJobFamily InFamily,
		FString InFamilyId,
		FString InSubjectKind,
		FString InPolicyKind,
		FString InSignalTypeId)
		: Family(InFamily)
		, FamilyId(MoveTemp(InFamilyId))
		, SubjectKind(MoveTemp(InSubjectKind))
		, PolicyKind(MoveTemp(InPolicyKind))
		, SignalTypeId(MoveTemp(InSignalTypeId))
	{
	}

	EUeAgentCaptureMediaLongJobFamily
	FUeAgentCaptureMediaLongJobFamilyContract::GetFamily() const
	{
		return Family;
	}

	const FString&
	FUeAgentCaptureMediaLongJobFamilyContract::GetFamilyId() const
	{
		return FamilyId;
	}

	const FString&
	FUeAgentCaptureMediaLongJobFamilyContract::GetSubjectKind() const
	{
		return SubjectKind;
	}

	const FString&
	FUeAgentCaptureMediaLongJobFamilyContract::GetPolicyKind() const
	{
		return PolicyKind;
	}

	const FString&
	FUeAgentCaptureMediaLongJobFamilyContract::GetSignalTypeId() const
	{
		return SignalTypeId;
	}

	const TArray<FUeAgentCaptureMediaLongJobFamilyContract>&
	GetCaptureMediaLongJobFamilyContracts()
	{
		static const TArray<
			FUeAgentCaptureMediaLongJobFamilyContract> Contracts = {
				{
					EUeAgentCaptureMediaLongJobFamily::ImageCapture,
					TEXT("image_capture"),
					TEXT("capture_subject_ref"),
					TEXT("image_capture_policy_ref"),
					TEXT("uai.capture.image.progress/v1"),
				},
				{
					EUeAgentCaptureMediaLongJobFamily::MediaRecording,
					TEXT("media_recording"),
					TEXT("media_source_ref"),
					TEXT("media_recording_policy_ref"),
					TEXT("uai.capture.media.progress/v1"),
				},
				{
					EUeAgentCaptureMediaLongJobFamily::RenderResource,
					TEXT("render_resource"),
					TEXT("render_resource_ref"),
					TEXT("render_operation_policy_ref"),
					TEXT("uai.capture.render.progress/v1"),
				},
			};
		return Contracts;
	}

	const FUeAgentCaptureMediaLongJobFamilyContract*
	FindCaptureMediaLongJobFamilyContract(
		const EUeAgentCaptureMediaLongJobFamily Family)
	{
		return GetCaptureMediaLongJobFamilyContracts().FindByPredicate(
			[Family](
				const FUeAgentCaptureMediaLongJobFamilyContract& Contract)
			{
				return Contract.GetFamily() == Family;
			});
	}
}
