#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/SemanticModel/UeAgentUnifiedHjsonSemanticModel.h"

namespace UE::UAI::HjsonWorkspace
{
	enum class EImportSourceCarrier : uint8
	{
		Directory,
		MultiFile,
		SingleFile,
		Inline,
	};

	enum class EImportChangeKind : uint8
	{
		Create,
		Delete,
		Replace,
		Clear,
		Reorder,
		Connect,
		Disconnect,
		SetValue,
		SetNull,
		RestoreDefault,
		Remove,
	};

	enum class EInitializationNodeRole : uint8
	{
		RootTarget,
		OwnedObject,
		Value,
		ContainerItem,
		ExistingReference,
		PendingReference,
	};

	enum class EInitializationEdgeKind : uint8
	{
		Ownership,
		CreationDependency,
		Reference,
	};

	UEAGENTINTERFACE_API FString LexToString(EImportSourceCarrier Carrier);
	UEAGENTINTERFACE_API FString LexToString(EImportChangeKind Kind);
	UEAGENTINTERFACE_API FString LexToString(EInitializationNodeRole Role);
	UEAGENTINTERFACE_API FString LexToString(EInitializationEdgeKind Kind);

	class UEAGENTINTERFACE_API FImportChange final
	{
	public:
		FImportChange(
			FString InChangeId,
			EImportChangeKind InKind,
			FString InTargetIdentity,
			FString InCandidateName = FString(),
			FString InCandidateBindingRevision = FString());

		const FString& GetChangeId() const;
		EImportChangeKind GetKind() const;
		const FString& GetTargetIdentity() const;
		const FString& GetCandidateName() const;
		const FString& GetCandidateBindingRevision() const;
		bool HasCandidate() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FString ChangeId;
		EImportChangeKind Kind = EImportChangeKind::SetValue;
		FString TargetIdentity;
		FString CandidateName;
		FString CandidateBindingRevision;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FInitializationNode final
	{
	public:
		FInitializationNode(
			FString InNodeId,
			EInitializationNodeRole InRole,
			FString InTypeIdentity,
			FString InInitializationContractId);

		const FString& GetNodeId() const;
		EInitializationNodeRole GetRole() const;
		const FString& GetTypeIdentity() const;
		const FString& GetInitializationContractId() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FString NodeId;
		EInitializationNodeRole Role = EInitializationNodeRole::Value;
		FString TypeIdentity;
		FString InitializationContractId;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FInitializationEdge final
	{
	public:
		FInitializationEdge(
			FString InFromNodeId,
			FString InToNodeId,
			EInitializationEdgeKind InKind);

		const FString& GetFromNodeId() const;
		const FString& GetToNodeId() const;
		EInitializationEdgeKind GetKind() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FString FromNodeId;
		FString ToNodeId;
		EInitializationEdgeKind Kind = EInitializationEdgeKind::Reference;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FImportInitializationPlanningRequest final
	{
	public:
		FImportInitializationPlanningRequest(
			EImportSourceCarrier InCarrier,
			FUnifiedHjsonSemanticModelRef InModel,
			FString InTargetIdentity,
			FUeAgentStateRevision InExpectedTargetRevision,
			FString InSourceBaselineFingerprint,
			FString InManifestFingerprint,
			TArray<FImportChange> InChanges,
			TArray<FInitializationNode> InNodes,
			TArray<FInitializationEdge> InEdges,
			TArray<FString> InImpactScopes,
			FString InAuthorizationId);

		EImportSourceCarrier GetCarrier() const;
		const FUnifiedHjsonSemanticModelRef& GetModel() const;
		const FString& GetTargetIdentity() const;
		const FUeAgentStateRevision& GetExpectedTargetRevision() const;
		const FString& GetSourceBaselineFingerprint() const;
		const FString& GetManifestFingerprint() const;
		const TArray<FImportChange>& GetChanges() const;
		const TArray<FInitializationNode>& GetNodes() const;
		const TArray<FInitializationEdge>& GetEdges() const;
		const TArray<FString>& GetImpactScopes() const;
		const FString& GetAuthorizationId() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		EImportSourceCarrier Carrier = EImportSourceCarrier::Inline;
		FUnifiedHjsonSemanticModelRef Model;
		FString TargetIdentity;
		FUeAgentStateRevision ExpectedTargetRevision;
		FString SourceBaselineFingerprint;
		FString ManifestFingerprint;
		TArray<FImportChange> Changes;
		TArray<FInitializationNode> Nodes;
		TArray<FInitializationEdge> Edges;
		TArray<FString> ImpactScopes;
		FString AuthorizationId;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API IImportInitializationPreflight
	{
	public:
		virtual ~IImportInitializationPreflight() = default;

		virtual bool TryReadCurrentTargetRevision(
			const FString& TargetIdentity,
			FUeAgentStateRevision& OutRevision,
			FWorkspaceError& OutError) = 0;
		virtual bool TryResolveCandidate(
			const FString& CandidateName,
			const FString& BindingRevision,
			FString& OutResolvedName,
			FWorkspaceError& OutError) = 0;
		virtual bool SupportsChange(
			const FImportChange& Change,
			FWorkspaceError& OutError) = 0;
		virtual bool TryAuthorizeImpact(
			const TArray<FString>& ImpactScopes,
			const FString& PlanFingerprint,
			const FString& AuthorizationId,
			FWorkspaceError& OutError) = 0;
	};

	class FImportInitializationPlan;
	using FImportInitializationPlanPtr =
		TSharedPtr<const FImportInitializationPlan>;
	using FImportInitializationPlanRef =
		TSharedRef<const FImportInitializationPlan>;

	class UEAGENTINTERFACE_API FImportInitializationPlan final
	{
	public:
		const FString& GetSourceModelFingerprint() const;
		const FString& GetTargetIdentity() const;
		const FUeAgentStateRevision& GetExpectedTargetRevision() const;
		const TArray<FImportChange>& GetChanges() const;
		int32 CountChanges(EImportChangeKind Kind) const;
		const TArray<FString>& GetCreationOrder() const;
		const TArray<FInitializationEdge>& GetReferenceEdges() const;
		const TMap<FString, FString>& GetResolvedCandidates() const;
		const TArray<FString>& GetImpactScopes() const;
		const FString& GetPlanFingerprint() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FImportInitializationPlan(
			FString InSourceModelFingerprint,
			FString InTargetIdentity,
			FUeAgentStateRevision InExpectedTargetRevision,
			TArray<FImportChange> InChanges,
			TArray<FString> InCreationOrder,
			TArray<FInitializationEdge> InReferenceEdges,
			TMap<FString, FString> InResolvedCandidates,
			TArray<FString> InImpactScopes,
			FString InPlanFingerprint);

		FString SourceModelFingerprint;
		FString TargetIdentity;
		FUeAgentStateRevision ExpectedTargetRevision;
		TArray<FImportChange> Changes;
		TArray<FString> CreationOrder;
		TArray<FInitializationEdge> ReferenceEdges;
		TMap<FString, FString> ResolvedCandidates;
		TArray<FString> ImpactScopes;
		FString PlanFingerprint;
		Contract::FDiagnostic ValidationDiagnostic;

		friend UEAGENTINTERFACE_API bool TryPlanHjsonImportInitialization(
			const FImportInitializationPlanningRequest&,
			const TSharedRef<IImportInitializationPreflight>&,
			FUeAgentResourceTracker&,
			FImportInitializationPlanPtr&,
			FWorkspaceError&);
	};

	UEAGENTINTERFACE_API bool TryPlanHjsonImportInitialization(
		const FImportInitializationPlanningRequest& Request,
		const TSharedRef<IImportInitializationPreflight>& Preflight,
		FUeAgentResourceTracker& Resources,
		FImportInitializationPlanPtr& OutPlan,
		FWorkspaceError& OutError);
}
