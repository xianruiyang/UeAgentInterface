#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"

#define UAI_GRAPH_COMPOUND_PCG_ANIMATION_ROUTES_V1 1

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::Composition::GraphCompound
{
	enum class EPcgAnimationGraphInputKind : uint8
	{
		Invalid,
		AssetCreateSimple,
		AssetCreateAnimBlueprint,
		AssetCreateBlendSpace,
		AssetCreateSkeletonBound,
		AssetCreateDeformerGraph,
		AssetCreateConcreteClass,
		AssetCreateMlModel,
		AssetCreateRetargeter,
		AssetCreateIkRig,
		GraphEdits,
		BlackboardSchema,
		SmartObjectDefinition,
		PcgGraphInstance,
		PcgParameters,
		PcgComponentBinding,
		PcgDataAsset,
		PcgDataFromLevel,
		PcgWorldBinding,
		BlendSpaceDefinition,
		SkeletonCompatibility,
		SkeletonVirtualBone,
		ControlRigShapeLibrary,
		DeformerSourceBindings,
		MeshDeformerCollectionEntries,
		MlDeformerConfiguration,
		IkRigDefinition,
		IkRigAutoGenerate,
		IkRetargeterDefinition,
		IkRetargeterSettings,
	};

	class UEAGENTINTERFACE_API FPcgAnimationGraphRouteProfile final
	{
	public:
		FPcgAnimationGraphRouteProfile(
			FString InCapabilityId,
			FString InDescriptorId,
			FString InDomainId,
			FString InFamilyId,
			EPcgAnimationGraphInputKind InInputKind,
			FString InTargetKind,
			FGraphCompoundExecutionContract InExecutionContract);

		const FString& GetCapabilityId() const;
		const FString& GetDescriptorId() const;
		const FString& GetDomainId() const;
		const FString& GetFamilyId() const;
		EPcgAnimationGraphInputKind GetInputKind() const;
		const FString& GetTargetKind() const;
		const FGraphCompoundExecutionContract& GetExecutionContract() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		FString CapabilityId;
		FString DescriptorId;
		FString DomainId;
		FString FamilyId;
		EPcgAnimationGraphInputKind InputKind =
			EPcgAnimationGraphInputKind::Invalid;
		FString TargetKind;
		FGraphCompoundExecutionContract ExecutionContract;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	UEAGENTINTERFACE_API bool TryGetPcgAnimationGraphRouteProfiles(
		const TArray<FPcgAnimationGraphRouteProfile>*& OutProfiles, Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API const FPcgAnimationGraphRouteProfile*
	FindPcgAnimationGraphRouteProfile(const FString& CapabilityId);

	UEAGENTINTERFACE_API bool TryCreatePcgAnimationGraphDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryDecodePcgAnimationGraphTypedOperation(
		const FPcgAnimationGraphRouteProfile& Profile,
		const Contract::FUeAgentContractValue& Input,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& Snapshot,
		ContextResource::FUeAgentResourceTracker& Resources,
		FGraphCompoundCanonicalPayload& OutPayload,
		FGraphCompoundRouteError& OutError);

	UEAGENTINTERFACE_API bool TryBuildPcgAnimationGraphTypedOperationDraft(
		const FPcgAnimationGraphRouteProfile& Profile,
		const Contract::FUeAgentDescriptorIdentity& DescriptorIdentity,
		const Contract::FUeAgentContractValue& Input,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& Snapshot,
		const FString& RequestId,
		const FString& IdempotencyKey,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<PlanningPreflight::FChangeSetDraft>& OutDraft,
		FGraphCompoundRouteError& OutError);
}

