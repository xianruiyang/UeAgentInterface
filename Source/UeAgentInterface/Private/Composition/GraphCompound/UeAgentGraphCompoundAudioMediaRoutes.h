#pragma once

#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"

#define UAI_GRAPH_COMPOUND_AUDIO_MEDIA_ROUTES_V1 1

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::Composition::GraphCompound
{
	enum class EAudioMediaGraphInputKind : uint8
	{
		Invalid,
		AssetCreate,
		MetaSoundInterface,
		MetaSoundNode,
		MetaSoundLink,
		MetaSoundDefault,
		MetaSoundCompileSettle,
		SoundCueNode,
		SoundCueRelation,
		SoundCueRoot,
		SoundCueCompileSettle,
		TextureGraphNode,
		TextureGraphLink,
		TextureGraphParameter,
		TextureGraphValidateEvaluateSettle,
		TextureGraphArtifactCapture,
	};

	class UEAGENTINTERFACE_API FAudioMediaGraphRouteProfile final
	{
	public:
		FAudioMediaGraphRouteProfile(
			FString InCapabilityId,
			FString InDescriptorId,
			FString InDomainId,
			FString InFamilyId,
			EGraphCompoundDomainRouteKind InRouteKind,
			EAudioMediaGraphInputKind InInputKind,
			FString InTargetKind,
			bool bInRequiresNativeBackendAvailability,
			FString InUnavailableErrorCode,
			FGraphCompoundExecutionContract InExecutionContract);

		const FString& GetCapabilityId() const;
		const FString& GetDescriptorId() const;
		const FString& GetDomainId() const;
		const FString& GetFamilyId() const;
		EGraphCompoundDomainRouteKind GetRouteKind() const;
		EAudioMediaGraphInputKind GetInputKind() const;
		const FString& GetTargetKind() const;
		bool RequiresNativeBackendAvailability() const;
		const FString& GetUnavailableErrorCode() const;
		const FGraphCompoundExecutionContract& GetExecutionContract() const;
		bool IsValid() const;

	private:
		FString CapabilityId;
		FString DescriptorId;
		FString DomainId;
		FString FamilyId;
		EGraphCompoundDomainRouteKind RouteKind =
			EGraphCompoundDomainRouteKind::Invalid;
		EAudioMediaGraphInputKind InputKind = EAudioMediaGraphInputKind::Invalid;
		FString TargetKind;
		bool bRequiresNativeBackendAvailability = false;
		FString UnavailableErrorCode;
		FGraphCompoundExecutionContract ExecutionContract;
		bool bValid = false;
	};

	UEAGENTINTERFACE_API const TArray<FAudioMediaGraphRouteProfile>&
	GetAudioMediaGraphRouteProfiles();

	UEAGENTINTERFACE_API const FAudioMediaGraphRouteProfile*
	FindAudioMediaGraphRouteProfile(const FString& CapabilityId);

	UEAGENTINTERFACE_API bool TryCreateAudioMediaGraphRouteDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError);

	class UEAGENTINTERFACE_API FAudioMediaGraphArtifactOutputSelector final
	{
	public:
		FAudioMediaGraphArtifactOutputSelector() = default;
		FAudioMediaGraphArtifactOutputSelector(
			FString InElementId,
			FString InOutputName);

		const FString& GetElementId() const;
		const FString& GetOutputName() const;
		bool IsValid() const;

	private:
		FString ElementId;
		FString OutputName;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FAudioMediaGraphArtifactQueryRequest final
	{
	public:
		FAudioMediaGraphArtifactQueryRequest() = default;
		FAudioMediaGraphArtifactQueryRequest(
			FString InTargetRef,
			FString InExpectedSettledRevision,
			TArray<FAudioMediaGraphArtifactOutputSelector> InOutputs);

		const FString& GetTargetRef() const;
		const FString& GetExpectedSettledRevision() const;
		const TArray<FAudioMediaGraphArtifactOutputSelector>& GetOutputs() const;
		bool IsValid() const;

	private:
		FString TargetRef;
		FString ExpectedSettledRevision;
		TArray<FAudioMediaGraphArtifactOutputSelector> Outputs;
		bool bValid = false;
	};

	UEAGENTINTERFACE_API bool TryDecodeAudioMediaGraphArtifactQueryRequest(
		const FAudioMediaGraphRouteProfile& Profile,
		const Contract::FUeAgentContractValue& Input,
		ContextResource::FUeAgentResourceTracker& Resources,
		FAudioMediaGraphArtifactQueryRequest& OutRequest,
		FGraphCompoundRouteError& OutError);

	UEAGENTINTERFACE_API bool TryDecodeAudioMediaGraphTypedOperation(
		const FAudioMediaGraphRouteProfile& Profile,
		const Contract::FUeAgentContractValue& Input,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& Snapshot,
		ContextResource::FUeAgentResourceTracker& Resources,
		FGraphCompoundCanonicalPayload& OutPayload,
		FGraphCompoundRouteError& OutError);

	UEAGENTINTERFACE_API bool TryBuildAudioMediaGraphTypedOperationDraft(
		const FAudioMediaGraphRouteProfile& Profile,
		const Contract::FUeAgentDescriptorIdentity& DescriptorIdentity,
		const Contract::FUeAgentContractValue& Input,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& Snapshot,
		const FString& RequestId,
		const FString& IdempotencyKey,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<PlanningPreflight::FChangeSetDraft>& OutDraft,
		FGraphCompoundRouteError& OutError);
}
