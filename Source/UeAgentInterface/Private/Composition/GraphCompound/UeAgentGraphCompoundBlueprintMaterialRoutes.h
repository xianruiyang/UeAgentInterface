#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundCanonical.h"
#include "Core/Contract/UeAgentContract.h"

#define UAI_GRAPH_COMPOUND_BLUEPRINT_MATERIAL_ROUTES_V1 1

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::Composition::GraphCompound
{
	enum class EGraphCompoundDomainRouteKind : uint8
	{
		Invalid,
		Query,
		TypedOperation,
	};

	enum class EGraphCompoundTypedInputKind : uint8
	{
		None,
		BlueprintAssetCreate,
		UmgAssetCreate,
		MaterialAssetCreate,
		MaterialInstanceAssetCreate,
		NiagaraEmitterAssetCreate,
		NiagaraSystemAssetCreate,
		GraphEdits,
		MaterialInstanceParameters,
		NiagaraAssetDuplicate,
		NiagaraStackReorder,
		NiagaraStackIssueFix,
		NiagaraCompoundInput,
		NiagaraRendererEventParameter,
		NiagaraCompileRefreshSettle,
	};

	using FGraphCompoundRouteError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FGraphCompoundDomainRouteProfile final
	{
	public:
		FGraphCompoundDomainRouteProfile(
			FString InCapabilityId,
			FString InDescriptorId,
			FString InDomainId,
			FString InFamilyId,
			EGraphCompoundDomainRouteKind InRouteKind,
			EGraphCompoundTypedInputKind InTypedInputKind,
			FString InTargetKind,
			FGraphCompoundExecutionContract InExecutionContract);

		const FString& GetCapabilityId() const;
		const FString& GetDescriptorId() const;
		const FString& GetDomainId() const;
		const FString& GetFamilyId() const;
		EGraphCompoundDomainRouteKind GetRouteKind() const;
		EGraphCompoundTypedInputKind GetTypedInputKind() const;
		const FString& GetTargetKind() const;
		const FGraphCompoundExecutionContract& GetExecutionContract() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		FString CapabilityId;
		FString DescriptorId;
		FString DomainId;
		FString FamilyId;
		EGraphCompoundDomainRouteKind RouteKind =
			EGraphCompoundDomainRouteKind::Invalid;
		EGraphCompoundTypedInputKind TypedInputKind =
			EGraphCompoundTypedInputKind::None;
		FString TargetKind;
		FGraphCompoundExecutionContract ExecutionContract;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	UEAGENTINTERFACE_API bool TryGetBlueprintMaterialGraphRouteProfiles(
		const TArray<FGraphCompoundDomainRouteProfile>*& OutProfiles, Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API const FGraphCompoundDomainRouteProfile*
	FindBlueprintMaterialGraphRouteProfile(const FString& CapabilityId);

	UEAGENTINTERFACE_API bool
	TryCreateBlueprintMaterialGraphDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	class UEAGENTINTERFACE_API FGraphCompoundEventQueryRequest final
	{
	public:
		FGraphCompoundEventQueryRequest();
		FGraphCompoundEventQueryRequest(
			FString InTargetRef,
			FString InOwnerItemRef);
		FGraphCompoundEventQueryRequest(
			FString InTargetRef,
			FString InOwnerItemRef,
			FString InQuery,
			bool bInOnlyUnbound,
			bool bInIncludeHidden,
			bool bInCompileBeforeQuery);

		const FString& GetTargetRef() const;
		const FString& GetOwnerItemRef() const;
		const FString& GetQuery() const;
		bool IsOnlyUnbound() const;
		bool IsIncludeHidden() const;
		bool IsCompileBeforeQuery() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		FString TargetRef;
		FString OwnerItemRef;
		FString Query;
		bool bOnlyUnbound = false;
		bool bIncludeHidden = false;
		bool bCompileBeforeQuery = true;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	UEAGENTINTERFACE_API bool TryDecodeGraphCompoundEventQueryRequest(
		const FGraphCompoundDomainRouteProfile& Profile,
		const Contract::FUeAgentContractValue& Input,
		ContextResource::FUeAgentResourceTracker& Resources,
		FGraphCompoundEventQueryRequest& OutRequest,
		FGraphCompoundRouteError& OutError);

	/**
	 * Exact typed mechanics for asset-create routes. Public JSON is decoded at
	 * the binding boundary; native execution consumes this value and a C6 plan.
	 */
	class UEAGENTINTERFACE_API FGraphCompoundAssetCreateRequest final
	{
	public:
		FGraphCompoundAssetCreateRequest();
		FGraphCompoundAssetCreateRequest(
			EGraphCompoundTypedInputKind InInputKind,
			FString InAssetName,
			FString InPackagePath,
			FString InTargetRef,
			FString InMaterialKind,
			FString InParentClassRef,
			FString InParentMaterialRef,
			FString InSourceRef);

		EGraphCompoundTypedInputKind GetInputKind() const;
		const FString& GetAssetName() const;
		const FString& GetPackagePath() const;
		const FString& GetTargetRef() const;
		const FString& GetMaterialKind() const;
		const FString& GetParentClassRef() const;
		const FString& GetParentMaterialRef() const;
		const FString& GetSourceRef() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		EGraphCompoundTypedInputKind InputKind =
			EGraphCompoundTypedInputKind::None;
		FString AssetName;
		FString PackagePath;
		FString TargetRef;
		FString MaterialKind;
		FString ParentClassRef;
		FString ParentMaterialRef;
		FString SourceRef;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	UEAGENTINTERFACE_API bool TryDecodeGraphCompoundAssetCreateRequest(
		const FGraphCompoundDomainRouteProfile& Profile,
		const Contract::FUeAgentContractValue& Input,
		ContextResource::FUeAgentResourceTracker& Resources,
		FGraphCompoundAssetCreateRequest& OutRequest,
		FGraphCompoundRouteError& OutError);

	/**
	 * Stable native selector for the four Niagara stack typed mutations. Desired
	 * values are deliberately excluded so C4 can capture the exact before state
	 * without reading the requested after state back from the command payload.
	 */
	class UEAGENTINTERFACE_API FGraphCompoundNiagaraMutationSelector final
	{
	public:
		FGraphCompoundNiagaraMutationSelector();
		FGraphCompoundNiagaraMutationSelector(
			EGraphCompoundTypedInputKind InInputKind,
			FString InTargetRef,
			FString InEmitterHandleId,
			FString InStageRef,
			FString InItemRef,
			FString InPredecessorRef,
			FString InIssueId,
			FString InFixId,
			FString InModuleRef,
			FString InInputName,
			FString InRendererRef,
			FString InParameterName,
			FString InTypeRef);

		EGraphCompoundTypedInputKind GetInputKind() const;
		const FString& GetTargetRef() const;
		const FString& GetEmitterHandleId() const;
		const FString& GetStageRef() const;
		const FString& GetItemRef() const;
		const FString& GetPredecessorRef() const;
		const FString& GetIssueId() const;
		const FString& GetFixId() const;
		const FString& GetModuleRef() const;
		const FString& GetInputName() const;
		const FString& GetRendererRef() const;
		const FString& GetParameterName() const;
		const FString& GetTypeRef() const;
		FString GetStableSemanticId() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		EGraphCompoundTypedInputKind InputKind =
			EGraphCompoundTypedInputKind::None;
		FString TargetRef;
		FString EmitterHandleId;
		FString StageRef;
		FString ItemRef;
		FString PredecessorRef;
		FString IssueId;
		FString FixId;
		FString ModuleRef;
		FString InputName;
		FString RendererRef;
		FString ParameterName;
		FString TypeRef;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	UEAGENTINTERFACE_API bool TryDecodeGraphCompoundNiagaraMutationSelector(
		const FGraphCompoundDomainRouteProfile& Profile,
		const Contract::FUeAgentContractValue& Input,
		ContextResource::FUeAgentResourceTracker& Resources,
		FGraphCompoundNiagaraMutationSelector& OutSelector,
		FGraphCompoundRouteError& OutError);

	UEAGENTINTERFACE_API bool TryDecodeGraphCompoundTypedOperation(
		const FGraphCompoundDomainRouteProfile& Profile,
		const Contract::FUeAgentContractValue& Input,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& Snapshot,
		ContextResource::FUeAgentResourceTracker& Resources,
		FGraphCompoundCanonicalPayload& OutPayload,
		FGraphCompoundRouteError& OutError);

	/**
	 * Validates one typed-operation payload against its exact Descriptor schema
	 * and returns the explicit target used for independent C4 capture. This does
	 * not build state, plan changes or native mechanics.
	 */
	UEAGENTINTERFACE_API bool TryDecodeGraphCompoundTypedOperationTargetRef(
		const FGraphCompoundDomainRouteProfile& Profile,
		const Contract::FUeAgentContractValue& Input,
		ContextResource::FUeAgentResourceTracker& Resources,
		FString& OutTargetRef,
		FGraphCompoundRouteError& OutError);

	UEAGENTINTERFACE_API bool TryBuildGraphCompoundTypedOperationDraft(
		const FGraphCompoundDomainRouteProfile& Profile,
		const Contract::FUeAgentDescriptorIdentity& DescriptorIdentity,
		const Contract::FUeAgentContractValue& Input,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& Snapshot,
		const FString& RequestId,
		const FString& IdempotencyKey,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<PlanningPreflight::FChangeSetDraft>& OutDraft,
		FGraphCompoundRouteError& OutError);
}
