#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonImportInitialization.h"

#include "Core/ContextResource/UeAgentContextResource.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;

	namespace
	{
		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool IsFingerprint(const FString& Value)
		{
			if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71)
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsHexDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
		}

		FString Sha256(const FString& Material)
		{
			const FTCHARToUTF8 Utf8(*Material, Material.Len());
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Utf8.Get()),
				static_cast<size_t>(Utf8.Length()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		bool IsCarrierValid(const EImportSourceCarrier Carrier)
		{
			switch (Carrier)
			{
			case EImportSourceCarrier::Directory:
			case EImportSourceCarrier::MultiFile:
			case EImportSourceCarrier::SingleFile:
			case EImportSourceCarrier::Inline:
				return true;
			default:
				return false;
			}
		}

		bool IsChangeKindValid(const EImportChangeKind Kind)
		{
			return !LexToString(Kind).IsEmpty();
		}

		bool IsNodeRoleValid(const EInitializationNodeRole Role)
		{
			return !LexToString(Role).IsEmpty();
		}

		bool IsEdgeKindValid(const EInitializationEdgeKind Kind)
		{
			return !LexToString(Kind).IsEmpty();
		}

		void SetFallbackError(
			FWorkspaceError& Error,
			const TCHAR* Code,
			const FString& Source = FString())
		{
			if (Error.GetCode().IsEmpty())
			{
				Error.Set(Code, Source);
			}
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			const TMap<EUeAgentResourceDimension, int64>& Amounts,
			FWorkspaceError& OutError)
		{
			FUeAgentResourceBudgetExceeded ResourceError;
			if (Resources.TryChargeMany(
				Amounts,
				TEXT("hjson.import.initialization_plan"),
				ResourceError))
			{
				return true;
			}
			OutError.Set(
				ResourceError.Code,
				ResourceError.Path,
				TEXT("/"),
				ResourceError.Observed,
				ResourceError.bStoppedEarly);
			return false;
		}

		FString EdgeKey(const FInitializationEdge& Edge)
		{
			return LexToString(Edge.GetKind())
				+ TEXT("\x1f") + Edge.GetFromNodeId()
				+ TEXT("\x1f") + Edge.GetToNodeId();
		}

		bool TryTopologicalOrder(
			const TArray<FString>& NodeIds,
			const TArray<FInitializationEdge>& Edges,
			const bool bOwnershipOnly,
			TArray<FString>& OutOrder,
			TArray<FString>& OutCycle)
		{
			OutOrder.Reset();
			OutCycle.Reset();
			TMap<FString, int32> InDegree;
			TMap<FString, TArray<FString>> Children;
			for (const FString& NodeId : NodeIds)
			{
				InDegree.Add(NodeId, 0);
			}
			for (const FInitializationEdge& Edge : Edges)
			{
				const bool bIncluded = bOwnershipOnly
					? Edge.GetKind() == EInitializationEdgeKind::Ownership
					: Edge.GetKind() != EInitializationEdgeKind::Reference;
				if (!bIncluded)
				{
					continue;
				}
				Children.FindOrAdd(Edge.GetFromNodeId()).Add(Edge.GetToNodeId());
				++InDegree.FindChecked(Edge.GetToNodeId());
			}
			for (TPair<FString, TArray<FString>>& Pair : Children)
			{
				Pair.Value.Sort();
			}

			TArray<FString> Ready;
			for (const TPair<FString, int32>& Pair : InDegree)
			{
				if (Pair.Value == 0)
				{
					Ready.Add(Pair.Key);
				}
			}
			Ready.Sort();
			while (!Ready.IsEmpty())
			{
				const FString Current = Ready[0];
				Ready.RemoveAt(0, 1, EAllowShrinking::No);
				OutOrder.Add(Current);
				const TArray<FString>* DirectChildren = Children.Find(Current);
				if (DirectChildren == nullptr)
				{
					continue;
				}
				for (const FString& Child : *DirectChildren)
				{
					int32& Degree = InDegree.FindChecked(Child);
					--Degree;
					if (Degree == 0)
					{
						Ready.Add(Child);
						Ready.Sort();
					}
				}
			}
			if (OutOrder.Num() == NodeIds.Num())
			{
				return true;
			}
			for (const TPair<FString, int32>& Pair : InDegree)
			{
				if (Pair.Value > 0)
				{
					OutCycle.Add(Pair.Key);
				}
			}
			OutCycle.Sort();
			if (!OutCycle.IsEmpty())
			{
				const FString FirstCycleNode = OutCycle[0];
				OutCycle.Add(FirstCycleNode);
			}
			return false;
		}

		FString BuildPlanFingerprint(
			const FImportInitializationPlanningRequest& Request,
			const TArray<FString>& CreationOrder,
			const TMap<FString, FString>& ResolvedCandidates,
			const TArray<FString>& ImpactScopes)
		{
			TArray<FString> ChangeRows;
			for (const FImportChange& Change : Request.GetChanges())
			{
				ChangeRows.Add(
					Change.GetChangeId() + TEXT("|")
					+ LexToString(Change.GetKind()) + TEXT("|")
					+ Change.GetTargetIdentity() + TEXT("|")
					+ Change.GetCandidateName() + TEXT("|")
					+ Change.GetCandidateBindingRevision() + TEXT("|")
					+ ResolvedCandidates.FindRef(Change.GetChangeId()));
			}
			ChangeRows.Sort();

			TArray<FString> NodeRows;
			for (const FInitializationNode& Node : Request.GetNodes())
			{
				NodeRows.Add(
					Node.GetNodeId() + TEXT("|")
					+ LexToString(Node.GetRole()) + TEXT("|")
					+ Node.GetTypeIdentity() + TEXT("|")
					+ Node.GetInitializationContractId());
			}
			NodeRows.Sort();

			TArray<FString> EdgeRows;
			for (const FInitializationEdge& Edge : Request.GetEdges())
			{
				EdgeRows.Add(EdgeKey(Edge));
			}
			EdgeRows.Sort();

			const FUeAgentStateRevision& Revision =
				Request.GetExpectedTargetRevision();
			const TArray<FString> MaterialParts{
				Request.GetModel()->GetModelFingerprint(),
				Request.GetTargetIdentity(),
				Revision.GetSourceId(),
				Revision.GetGeneration(),
				FString::Printf(TEXT("%lld"), Revision.GetSequence()),
				Request.GetSourceBaselineFingerprint(),
				Request.GetManifestFingerprint(),
				FString::Join(ChangeRows, TEXT("\x1e")),
				FString::Join(NodeRows, TEXT("\x1e")),
				FString::Join(EdgeRows, TEXT("\x1e")),
				FString::Join(CreationOrder, TEXT("\x1e")),
				FString::Join(ImpactScopes, TEXT("\x1e")),
			};
			return Sha256(FString::Join(MaterialParts, TEXT("\x1d")));
		}
	}

	FString LexToString(const EImportSourceCarrier Carrier)
	{
		switch (Carrier)
		{
		case EImportSourceCarrier::Directory: return TEXT("directory");
		case EImportSourceCarrier::MultiFile: return TEXT("multi_file");
		case EImportSourceCarrier::SingleFile: return TEXT("single_file");
		case EImportSourceCarrier::Inline: return TEXT("inline");
		default: return FString();
		}
	}

	FString LexToString(const EImportChangeKind Kind)
	{
		switch (Kind)
		{
		case EImportChangeKind::Create: return TEXT("create");
		case EImportChangeKind::Delete: return TEXT("delete");
		case EImportChangeKind::Replace: return TEXT("replace");
		case EImportChangeKind::Clear: return TEXT("clear");
		case EImportChangeKind::Reorder: return TEXT("reorder");
		case EImportChangeKind::Connect: return TEXT("connect");
		case EImportChangeKind::Disconnect: return TEXT("disconnect");
		case EImportChangeKind::SetValue: return TEXT("set_value");
		case EImportChangeKind::SetNull: return TEXT("set_null");
		case EImportChangeKind::RestoreDefault: return TEXT("restore_default");
		case EImportChangeKind::Remove: return TEXT("remove");
		default: return FString();
		}
	}

	FString LexToString(const EInitializationNodeRole Role)
	{
		switch (Role)
		{
		case EInitializationNodeRole::RootTarget: return TEXT("root_target");
		case EInitializationNodeRole::OwnedObject: return TEXT("owned_object");
		case EInitializationNodeRole::Value: return TEXT("value");
		case EInitializationNodeRole::ContainerItem: return TEXT("container_item");
		case EInitializationNodeRole::ExistingReference: return TEXT("existing_reference");
		case EInitializationNodeRole::PendingReference: return TEXT("pending_reference");
		default: return FString();
		}
	}

	FString LexToString(const EInitializationEdgeKind Kind)
	{
		switch (Kind)
		{
		case EInitializationEdgeKind::Ownership: return TEXT("ownership");
		case EInitializationEdgeKind::CreationDependency: return TEXT("creation_dependency");
		case EInitializationEdgeKind::Reference: return TEXT("reference");
		default: return FString();
		}
	}

	FImportChange::FImportChange(
		FString InChangeId,
		const EImportChangeKind InKind,
		FString InTargetIdentity,
		FString InCandidateName,
		FString InCandidateBindingRevision)
		: ChangeId(Normalize(MoveTemp(InChangeId)))
		, Kind(InKind)
		, TargetIdentity(Normalize(MoveTemp(InTargetIdentity)))
		, CandidateName(Normalize(MoveTemp(InCandidateName)))
		, CandidateBindingRevision(
			Normalize(MoveTemp(InCandidateBindingRevision)))
	{
		bValid = !ChangeId.IsEmpty()
			&& IsChangeKindValid(Kind)
			&& !TargetIdentity.IsEmpty()
			&& CandidateName.IsEmpty() == CandidateBindingRevision.IsEmpty();
	}

	const FString& FImportChange::GetChangeId() const { return ChangeId; }
	EImportChangeKind FImportChange::GetKind() const { return Kind; }
	const FString& FImportChange::GetTargetIdentity() const { return TargetIdentity; }
	const FString& FImportChange::GetCandidateName() const { return CandidateName; }
	const FString& FImportChange::GetCandidateBindingRevision() const { return CandidateBindingRevision; }
	bool FImportChange::HasCandidate() const { return !CandidateName.IsEmpty(); }
	bool FImportChange::IsValid() const { return bValid; }

	FInitializationNode::FInitializationNode(
		FString InNodeId,
		const EInitializationNodeRole InRole,
		FString InTypeIdentity,
		FString InInitializationContractId)
		: NodeId(Normalize(MoveTemp(InNodeId)))
		, Role(InRole)
		, TypeIdentity(Normalize(MoveTemp(InTypeIdentity)))
		, InitializationContractId(
			Normalize(MoveTemp(InInitializationContractId)))
	{
		bValid = !NodeId.IsEmpty()
			&& IsNodeRoleValid(Role)
			&& !TypeIdentity.IsEmpty()
			&& !InitializationContractId.IsEmpty();
	}

	const FString& FInitializationNode::GetNodeId() const { return NodeId; }
	EInitializationNodeRole FInitializationNode::GetRole() const { return Role; }
	const FString& FInitializationNode::GetTypeIdentity() const { return TypeIdentity; }
	const FString& FInitializationNode::GetInitializationContractId() const { return InitializationContractId; }
	bool FInitializationNode::IsValid() const { return bValid; }

	FInitializationEdge::FInitializationEdge(
		FString InFromNodeId,
		FString InToNodeId,
		const EInitializationEdgeKind InKind)
		: FromNodeId(Normalize(MoveTemp(InFromNodeId)))
		, ToNodeId(Normalize(MoveTemp(InToNodeId)))
		, Kind(InKind)
	{
		bValid = !FromNodeId.IsEmpty()
			&& !ToNodeId.IsEmpty()
			&& IsEdgeKindValid(Kind)
			&& (Kind == EInitializationEdgeKind::Reference
				|| FromNodeId != ToNodeId);
	}

	const FString& FInitializationEdge::GetFromNodeId() const { return FromNodeId; }
	const FString& FInitializationEdge::GetToNodeId() const { return ToNodeId; }
	EInitializationEdgeKind FInitializationEdge::GetKind() const { return Kind; }
	bool FInitializationEdge::IsValid() const { return bValid; }

	FImportInitializationPlanningRequest::FImportInitializationPlanningRequest(
		const EImportSourceCarrier InCarrier,
		FUnifiedHjsonSemanticModelRef InModel,
		FString InTargetIdentity,
		FUeAgentStateRevision InExpectedTargetRevision,
		FString InSourceBaselineFingerprint,
		FString InManifestFingerprint,
		TArray<FImportChange> InChanges,
		TArray<FInitializationNode> InNodes,
		TArray<FInitializationEdge> InEdges,
		TArray<FString> InImpactScopes,
		FString InAuthorizationId)
		: Carrier(InCarrier)
		, Model(MoveTemp(InModel))
		, TargetIdentity(Normalize(MoveTemp(InTargetIdentity)))
		, ExpectedTargetRevision(MoveTemp(InExpectedTargetRevision))
		, SourceBaselineFingerprint(
			Normalize(MoveTemp(InSourceBaselineFingerprint)))
		, ManifestFingerprint(Normalize(MoveTemp(InManifestFingerprint)))
		, Changes(MoveTemp(InChanges))
		, Nodes(MoveTemp(InNodes))
		, Edges(MoveTemp(InEdges))
		, ImpactScopes(MoveTemp(InImpactScopes))
		, AuthorizationId(Normalize(MoveTemp(InAuthorizationId)))
	{
		for (FString& Scope : ImpactScopes)
		{
			Scope = Normalize(MoveTemp(Scope));
		}
		bValid = IsCarrierValid(Carrier)
			&& Model->IsValid()
			&& !TargetIdentity.IsEmpty()
			&& ExpectedTargetRevision.IsValid()
			&& IsFingerprint(SourceBaselineFingerprint)
			&& IsFingerprint(ManifestFingerprint)
			&& !AuthorizationId.IsEmpty();
	}

	EImportSourceCarrier FImportInitializationPlanningRequest::GetCarrier() const { return Carrier; }
	const FUnifiedHjsonSemanticModelRef& FImportInitializationPlanningRequest::GetModel() const { return Model; }
	const FString& FImportInitializationPlanningRequest::GetTargetIdentity() const { return TargetIdentity; }
	const FUeAgentStateRevision& FImportInitializationPlanningRequest::GetExpectedTargetRevision() const { return ExpectedTargetRevision; }
	const FString& FImportInitializationPlanningRequest::GetSourceBaselineFingerprint() const { return SourceBaselineFingerprint; }
	const FString& FImportInitializationPlanningRequest::GetManifestFingerprint() const { return ManifestFingerprint; }
	const TArray<FImportChange>& FImportInitializationPlanningRequest::GetChanges() const { return Changes; }
	const TArray<FInitializationNode>& FImportInitializationPlanningRequest::GetNodes() const { return Nodes; }
	const TArray<FInitializationEdge>& FImportInitializationPlanningRequest::GetEdges() const { return Edges; }
	const TArray<FString>& FImportInitializationPlanningRequest::GetImpactScopes() const { return ImpactScopes; }
	const FString& FImportInitializationPlanningRequest::GetAuthorizationId() const { return AuthorizationId; }
	bool FImportInitializationPlanningRequest::IsValid() const { return bValid; }

	FImportInitializationPlan::FImportInitializationPlan(
		FString InSourceModelFingerprint,
		FString InTargetIdentity,
		FUeAgentStateRevision InExpectedTargetRevision,
		TArray<FImportChange> InChanges,
		TArray<FString> InCreationOrder,
		TArray<FInitializationEdge> InReferenceEdges,
		TMap<FString, FString> InResolvedCandidates,
		TArray<FString> InImpactScopes,
		FString InPlanFingerprint)
		: SourceModelFingerprint(MoveTemp(InSourceModelFingerprint))
		, TargetIdentity(MoveTemp(InTargetIdentity))
		, ExpectedTargetRevision(MoveTemp(InExpectedTargetRevision))
		, Changes(MoveTemp(InChanges))
		, CreationOrder(MoveTemp(InCreationOrder))
		, ReferenceEdges(MoveTemp(InReferenceEdges))
		, ResolvedCandidates(MoveTemp(InResolvedCandidates))
		, ImpactScopes(MoveTemp(InImpactScopes))
		, PlanFingerprint(MoveTemp(InPlanFingerprint))
	{
		bValid = IsFingerprint(SourceModelFingerprint)
			&& !TargetIdentity.IsEmpty()
			&& ExpectedTargetRevision.IsValid()
			&& IsFingerprint(PlanFingerprint);
	}

	const FString& FImportInitializationPlan::GetSourceModelFingerprint() const { return SourceModelFingerprint; }
	const FString& FImportInitializationPlan::GetTargetIdentity() const { return TargetIdentity; }
	const FUeAgentStateRevision& FImportInitializationPlan::GetExpectedTargetRevision() const { return ExpectedTargetRevision; }
	const TArray<FImportChange>& FImportInitializationPlan::GetChanges() const { return Changes; }
	int32 FImportInitializationPlan::CountChanges(const EImportChangeKind Kind) const
	{
		int32 Count = 0;
		for (const FImportChange& Change : Changes)
		{
			Count += Change.GetKind() == Kind ? 1 : 0;
		}
		return Count;
	}
	const TArray<FString>& FImportInitializationPlan::GetCreationOrder() const { return CreationOrder; }
	const TArray<FInitializationEdge>& FImportInitializationPlan::GetReferenceEdges() const { return ReferenceEdges; }
	const TMap<FString, FString>& FImportInitializationPlan::GetResolvedCandidates() const { return ResolvedCandidates; }
	const TArray<FString>& FImportInitializationPlan::GetImpactScopes() const { return ImpactScopes; }
	const FString& FImportInitializationPlan::GetPlanFingerprint() const { return PlanFingerprint; }
	bool FImportInitializationPlan::IsValid() const { return bValid; }

	bool TryPlanHjsonImportInitialization(
		const FImportInitializationPlanningRequest& Request,
		const TSharedRef<IImportInitializationPreflight>& Preflight,
		FUeAgentResourceTracker& Resources,
		FImportInitializationPlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		OutPlan.Reset();
		OutError.Reset();
		if (!Request.IsValid())
		{
			OutError.Set(TEXT("hjson_import_planning_request_invalid"));
			return false;
		}

		TSet<FString> ChangeIds;
		for (const FImportChange& Change : Request.GetChanges())
		{
			if (!Change.IsValid())
			{
				OutError.Set(TEXT("hjson_import_change_invalid"), Change.GetChangeId());
				return false;
			}
			if (ChangeIds.Contains(Change.GetChangeId()))
			{
				OutError.Set(TEXT("hjson_import_change_duplicate"), Change.GetChangeId());
				return false;
			}
			ChangeIds.Add(Change.GetChangeId());
		}

		TArray<FString> NodeIds;
		TMap<FString, int32> NodeIndex;
		for (int32 Index = 0; Index < Request.GetNodes().Num(); ++Index)
		{
			const FInitializationNode& Node = Request.GetNodes()[Index];
			if (!Node.IsValid())
			{
				OutError.Set(TEXT("hjson_import_initialization_node_invalid"), Node.GetNodeId());
				return false;
			}
			if (NodeIndex.Contains(Node.GetNodeId()))
			{
				OutError.Set(TEXT("hjson_import_initialization_node_duplicate"), Node.GetNodeId());
				return false;
			}
			NodeIndex.Add(Node.GetNodeId(), Index);
			NodeIds.Add(Node.GetNodeId());
		}
		NodeIds.Sort();

		TSet<FString> EdgeKeys;
		TMap<FString, FString> OwnerByChild;
		TArray<FInitializationEdge> ReferenceEdges;
		for (const FInitializationEdge& Edge : Request.GetEdges())
		{
			if (!Edge.IsValid()
				|| !NodeIndex.Contains(Edge.GetFromNodeId())
				|| !NodeIndex.Contains(Edge.GetToNodeId()))
			{
				OutError.Set(TEXT("hjson_import_initialization_edge_invalid"), EdgeKey(Edge));
				return false;
			}
			const FString Key = EdgeKey(Edge);
			if (EdgeKeys.Contains(Key))
			{
				OutError.Set(TEXT("hjson_import_initialization_edge_duplicate"), Key);
				return false;
			}
			EdgeKeys.Add(Key);
			if (Edge.GetKind() == EInitializationEdgeKind::Ownership)
			{
				if (OwnerByChild.Contains(Edge.GetToNodeId()))
				{
					OutError.Set(TEXT("hjson_import_multiple_owners"), Edge.GetToNodeId());
					return false;
				}
				OwnerByChild.Add(Edge.GetToNodeId(), Edge.GetFromNodeId());
			}
			else if (Edge.GetKind() == EInitializationEdgeKind::Reference)
			{
				ReferenceEdges.Add(Edge);
			}
		}

		TArray<FString> ImpactScopes = Request.GetImpactScopes();
		for (const FString& Scope : ImpactScopes)
		{
			if (Scope.IsEmpty())
			{
				OutError.Set(TEXT("hjson_import_impact_scope_invalid"));
				return false;
			}
		}
		ImpactScopes.Sort();
		for (int32 Index = ImpactScopes.Num() - 1; Index > 0; --Index)
		{
			if (ImpactScopes[Index] == ImpactScopes[Index - 1])
			{
				ImpactScopes.RemoveAt(Index, 1, EAllowShrinking::No);
			}
		}
		if (!Request.GetChanges().IsEmpty() && ImpactScopes.IsEmpty())
		{
			OutError.Set(TEXT("hjson_import_impact_scope_required"));
			return false;
		}

		if (!Charge(
				Resources,
				{
					{EUeAgentResourceDimension::Nodes, Request.GetNodes().Num()},
					{EUeAgentResourceDimension::Items, Request.GetChanges().Num()},
					{EUeAgentResourceDimension::ReferenceEdges, Request.GetEdges().Num()},
					{EUeAgentResourceDimension::WorkUnits,
						Request.GetNodes().Num() * 3
						+ Request.GetEdges().Num() * 3
						+ Request.GetChanges().Num() * 2},
					{EUeAgentResourceDimension::EstimatedMemoryBytes,
						Request.GetNodes().Num() * 192
						+ Request.GetEdges().Num() * 96
						+ Request.GetChanges().Num() * 160},
				},
				OutError))
		{
			return false;
		}

		FUeAgentStateRevision Current(TEXT(""), TEXT(""), -1);
		if (!Preflight->TryReadCurrentTargetRevision(
				Request.GetTargetIdentity(),
				Current,
				OutError))
		{
			SetFallbackError(OutError, TEXT("hjson_import_target_version_read_failed"));
			return false;
		}
		if (!Current.IsValid())
		{
			OutError.Set(TEXT("hjson_import_target_revision_invalid"));
			return false;
		}
		if (Current != Request.GetExpectedTargetRevision())
		{
			OutError.Set(TEXT("hjson_import_target_version_conflict"));
			return false;
		}

		TMap<FString, FString> ResolvedCandidates;
		for (const FImportChange& Change : Request.GetChanges())
		{
			if (!Preflight->SupportsChange(Change, OutError))
			{
				SetFallbackError(
					OutError,
					TEXT("hjson_import_change_unsupported"),
					Change.GetChangeId());
				return false;
			}
			if (!Change.HasCandidate())
			{
				continue;
			}
			FString Resolved;
			if (!Preflight->TryResolveCandidate(
					Change.GetCandidateName(),
					Change.GetCandidateBindingRevision(),
					Resolved,
					OutError))
			{
				SetFallbackError(
					OutError,
					TEXT("hjson_import_candidate_invalid"),
					Change.GetCandidateName());
				return false;
			}
			Resolved = Normalize(MoveTemp(Resolved));
			if (Resolved != Change.GetCandidateName())
			{
				OutError.Set(
					TEXT("hjson_import_candidate_identity_changed"),
					Change.GetCandidateName());
				return false;
			}
			ResolvedCandidates.Add(Change.GetChangeId(), MoveTemp(Resolved));
		}

		TArray<FString> OwnershipOrder;
		TArray<FString> Cycle;
		if (!TryTopologicalOrder(
				NodeIds,
				Request.GetEdges(),
				true,
				OwnershipOrder,
				Cycle))
		{
			OutError.Set(
				TEXT("hjson_import_ownership_cycle"),
				FString(),
				TEXT("/"),
				0,
				false,
				MoveTemp(Cycle));
			return false;
		}

		TArray<FString> CreationOrder;
		if (!TryTopologicalOrder(
				NodeIds,
				Request.GetEdges(),
				false,
				CreationOrder,
				Cycle))
		{
			OutError.Set(
				TEXT("hjson_import_creation_dependency_cycle"),
				FString(),
				TEXT("/"),
				0,
				false,
				MoveTemp(Cycle));
			return false;
		}

		ReferenceEdges.Sort([](
			const FInitializationEdge& Left,
			const FInitializationEdge& Right)
		{
			return EdgeKey(Left) < EdgeKey(Right);
		});
		const FString PlanFingerprint = BuildPlanFingerprint(
			Request,
			CreationOrder,
			ResolvedCandidates,
			ImpactScopes);
		if (!Preflight->TryAuthorizeImpact(
				ImpactScopes,
				PlanFingerprint,
				Request.GetAuthorizationId(),
				OutError))
		{
			SetFallbackError(OutError, TEXT("hjson_import_unauthorized"));
			return false;
		}

		FImportInitializationPlanPtr Candidate = MakeShareable(
			new FImportInitializationPlan(
				Request.GetModel()->GetModelFingerprint(),
				Request.GetTargetIdentity(),
				Request.GetExpectedTargetRevision(),
				Request.GetChanges(),
				MoveTemp(CreationOrder),
				MoveTemp(ReferenceEdges),
				MoveTemp(ResolvedCandidates),
				MoveTemp(ImpactScopes),
				PlanFingerprint));
		if (!Candidate->IsValid())
		{
			OutError.Set(TEXT("hjson_import_plan_invalid"));
			return false;
		}
		OutPlan = MoveTemp(Candidate);
		return true;
	}
}
