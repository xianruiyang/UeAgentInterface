#pragma once

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
		HjsonImport,
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

	struct UEAGENTINTERFACE_API FGraphCompoundRouteError
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsValid() const { return !Code.IsEmpty(); }
	};

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
			TArray<FString> InAllowedCompositionKinds,
			FGraphCompoundExecutionContract InExecutionContract);

		const FString& GetCapabilityId() const;
		const FString& GetDescriptorId() const;
		const FString& GetDomainId() const;
		const FString& GetFamilyId() const;
		EGraphCompoundDomainRouteKind GetRouteKind() const;
		EGraphCompoundTypedInputKind GetTypedInputKind() const;
		const FString& GetTargetKind() const;
		const TArray<FString>& GetAllowedCompositionKinds() const;
		const FGraphCompoundExecutionContract& GetExecutionContract() const;
		bool IsValid() const;

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
		TArray<FString> AllowedCompositionKinds;
		FGraphCompoundExecutionContract ExecutionContract;
		bool bValid = false;
	};

	UEAGENTINTERFACE_API const TArray<FGraphCompoundDomainRouteProfile>&
	GetBlueprintMaterialGraphRouteProfiles();

	UEAGENTINTERFACE_API const FGraphCompoundDomainRouteProfile*
	FindBlueprintMaterialGraphRouteProfile(const FString& CapabilityId);

	UEAGENTINTERFACE_API bool
	TryCreateBlueprintMaterialGraphDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError);

	class UEAGENTINTERFACE_API FGraphCompoundEventQueryRequest final
	{
	public:
		FGraphCompoundEventQueryRequest() = default;
		FGraphCompoundEventQueryRequest(
			FString InTargetRef,
			FString InOwnerItemRef);
		FGraphCompoundEventQueryRequest(
			FString InTargetRef,
			FString InOwnerItemRef,
			FString InQuery,
			int32 InLimit,
			bool bInOnlyUnbound,
			bool bInIncludeHidden,
			bool bInCompileBeforeQuery);

		const FString& GetTargetRef() const;
		const FString& GetOwnerItemRef() const;
		const FString& GetQuery() const;
		int32 GetLimit() const;
		bool IsOnlyUnbound() const;
		bool IsIncludeHidden() const;
		bool IsCompileBeforeQuery() const;
		bool IsValid() const;

	private:
		FString TargetRef;
		FString OwnerItemRef;
		FString Query;
		int32 Limit = 200;
		bool bOnlyUnbound = false;
		bool bIncludeHidden = false;
		bool bCompileBeforeQuery = true;
		bool bValid = false;
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
		FGraphCompoundAssetCreateRequest() = default;
		FGraphCompoundAssetCreateRequest(
			EGraphCompoundTypedInputKind InInputKind,
			FString InAssetName,
			FString InPackagePath,
			FString InTargetRef,
			FString InParentClassRef,
			FString InParentMaterialRef,
			FString InSourceRef);

		EGraphCompoundTypedInputKind GetInputKind() const;
		const FString& GetAssetName() const;
		const FString& GetPackagePath() const;
		const FString& GetTargetRef() const;
		const FString& GetParentClassRef() const;
		const FString& GetParentMaterialRef() const;
		const FString& GetSourceRef() const;
		bool IsValid() const;

	private:
		EGraphCompoundTypedInputKind InputKind =
			EGraphCompoundTypedInputKind::None;
		FString AssetName;
		FString PackagePath;
		FString TargetRef;
		FString ParentClassRef;
		FString ParentMaterialRef;
		FString SourceRef;
		bool bValid = false;
	};

	UEAGENTINTERFACE_API bool TryDecodeGraphCompoundAssetCreateRequest(
		const FGraphCompoundDomainRouteProfile& Profile,
		const Contract::FUeAgentContractValue& Input,
		ContextResource::FUeAgentResourceTracker& Resources,
		FGraphCompoundAssetCreateRequest& OutRequest,
		FGraphCompoundRouteError& OutError);

	enum class EGraphCompoundHjsonSourceKind : uint8
	{
		Invalid,
		Directory,
		SingleFile,
		Inline,
	};

	class UEAGENTINTERFACE_API FGraphCompoundHjsonSourceRequest final
	{
	public:
		FGraphCompoundHjsonSourceRequest() = default;
		FGraphCompoundHjsonSourceRequest(
			FString InCompositionKind,
			FString InTargetRef,
			EGraphCompoundHjsonSourceKind InSourceKind,
			FString InSourceLocator,
			FString InInlineHjson,
			PlanningPreflight::EChangeIntent InIntent,
			FString InExpectedRevision,
			bool bInDryRun);

		const FString& GetCompositionKind() const;
		const FString& GetTargetRef() const;
		EGraphCompoundHjsonSourceKind GetSourceKind() const;
		const FString& GetSourceLocator() const;
		const FString& GetInlineHjson() const;
		PlanningPreflight::EChangeIntent GetIntent() const;
		const FString& GetExpectedRevision() const;
		bool IsDryRun() const;
		bool IsValid() const;

	private:
		FString CompositionKind;
		FString TargetRef;
		EGraphCompoundHjsonSourceKind SourceKind =
			EGraphCompoundHjsonSourceKind::Invalid;
		FString SourceLocator;
		FString InlineHjson;
		PlanningPreflight::EChangeIntent Intent =
			PlanningPreflight::EChangeIntent::Update;
		FString ExpectedRevision;
		bool bDryRun = false;
		bool bValid = false;
	};

	UEAGENTINTERFACE_API bool TryDecodeGraphCompoundHjsonSourceRequest(
		const FGraphCompoundDomainRouteProfile& Profile,
		const Contract::FUeAgentContractValue& Input,
		ContextResource::FUeAgentResourceTracker& Resources,
		FGraphCompoundHjsonSourceRequest& OutRequest,
		FGraphCompoundRouteError& OutError);

	/**
	 * Stable native selector for the four Niagara stack typed mutations. Desired
	 * values are deliberately excluded so C4 can capture the exact before state
	 * without reading the requested after state back from the command payload.
	 */
	class UEAGENTINTERFACE_API FGraphCompoundNiagaraMutationSelector final
	{
	public:
		FGraphCompoundNiagaraMutationSelector() = default;
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
		bool bValid = false;
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
