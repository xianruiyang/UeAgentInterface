#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonImportInitialization.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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

		Contract::FDiagnostic InitializationFailure(const TCHAR* Code, FString Reason, FString Path)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, MoveTemp(Reason), TEXT("import_initialization"));
			Error.PropertyPath = MoveTemp(Path);
			Error.SourceKind = TEXT("native_result");
			return Error;
		}

		void SetFallbackError(
			FWorkspaceError& Error,
			const TCHAR* Code,
			const TCHAR* Reason,
			const FString& Source = FString())
		{
			if (Error.GetCode().IsEmpty())
			{
				Error.SetFailure(Contract::FDiagnostic::Failure(Code, Reason, TEXT("import_initialization")), Source);
			}
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			const TMap<EUeAgentResourceDimension, int64>& Amounts,
			FWorkspaceError& OutError)
		{
			FUeAgentResourceChargeError ResourceError;
			if (Resources.TryChargeMany(
				Amounts,
				TEXT("hjson.import.initialization_plan"),
				ResourceError))
			{
				return true;
			}
			OutError.SetFailure(MoveTemp(ResourceError));
			return false;
		}

		TSharedRef<FJsonValue> RevisionEvidence(const FUeAgentStateRevision& Revision)
		{
			auto Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("source_id"), Revision.GetSourceId());
			Object->SetStringField(TEXT("generation"), Revision.GetGeneration());
			Object->SetStringField(TEXT("sequence"), FString::Printf(TEXT("%lld"), Revision.GetSequence()));
			return MakeShared<FJsonValueObject>(Object);
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
		if (ChangeId.IsEmpty())
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_change_invalid"), TEXT("An import change requires a non-empty identifier."), TEXT("/change_id"));
		else if (!IsChangeKindValid(Kind))
		{
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_change_invalid"), TEXT("The import change kind is unsupported."), TEXT("/kind"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Kind));
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("create, delete, replace, clear, reorder, connect, disconnect, set_value, set_null, restore_default, remove"));
		}
		else if (TargetIdentity.IsEmpty())
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_change_invalid"), TEXT("An import change requires a target."), TEXT("/target"));
		else if (CandidateName.IsEmpty() != CandidateBindingRevision.IsEmpty())
		{
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_change_invalid"),
				CandidateName.IsEmpty() ? TEXT("A candidate binding revision requires its candidate name.") : TEXT("A named candidate requires its binding revision."),
				CandidateName.IsEmpty() ? TEXT("/candidate_name") : TEXT("/candidate_binding_revision"));
			ValidationDiagnostic.CandidateName = CandidateName;
		}
		if (!ValidationDiagnostic.IsEmpty())
		{
			ValidationDiagnostic.TargetPath = TargetIdentity;
			ValidationDiagnostic.SubjectId = ChangeId;
		}
	}

	const FString& FImportChange::GetChangeId() const { return ChangeId; }
	EImportChangeKind FImportChange::GetKind() const { return Kind; }
	const FString& FImportChange::GetTargetIdentity() const { return TargetIdentity; }
	const FString& FImportChange::GetCandidateName() const { return CandidateName; }
	const FString& FImportChange::GetCandidateBindingRevision() const { return CandidateBindingRevision; }
	bool FImportChange::HasCandidate() const { return !CandidateName.IsEmpty(); }
	bool FImportChange::IsValid() const { return ValidationDiagnostic.IsEmpty(); }

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
		if (NodeId.IsEmpty())
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_initialization_node_invalid"), TEXT("An initialization node requires a non-empty identifier."), TEXT("/node_id"));
		else if (!IsNodeRoleValid(Role))
		{
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_initialization_node_invalid"), TEXT("The initialization node role is unsupported."), TEXT("/role"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Role));
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("root_target, owned_object, value, container_item, existing_reference, pending_reference"));
		}
		else if (TypeIdentity.IsEmpty())
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_initialization_node_invalid"), TEXT("An initialization node requires a type."), TEXT("/type_identity"));
		else if (InitializationContractId.IsEmpty())
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_initialization_node_invalid"), TEXT("An initialization node requires an initialization contract."), TEXT("/initialization_contract_id"));
		if (!ValidationDiagnostic.IsEmpty()) ValidationDiagnostic.SubjectId = NodeId;
	}

	const FString& FInitializationNode::GetNodeId() const { return NodeId; }
	EInitializationNodeRole FInitializationNode::GetRole() const { return Role; }
	const FString& FInitializationNode::GetTypeIdentity() const { return TypeIdentity; }
	const FString& FInitializationNode::GetInitializationContractId() const { return InitializationContractId; }
	bool FInitializationNode::IsValid() const { return ValidationDiagnostic.IsEmpty(); }

	FInitializationEdge::FInitializationEdge(
		FString InFromNodeId,
		FString InToNodeId,
		const EInitializationEdgeKind InKind)
		: FromNodeId(Normalize(MoveTemp(InFromNodeId)))
		, ToNodeId(Normalize(MoveTemp(InToNodeId)))
		, Kind(InKind)
	{
		if (FromNodeId.IsEmpty() || ToNodeId.IsEmpty())
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_initialization_edge_invalid"),
				FromNodeId.IsEmpty() ? TEXT("An initialization dependency requires a source node.") : TEXT("An initialization dependency requires a destination node."),
				FromNodeId.IsEmpty() ? TEXT("/from_node_id") : TEXT("/to_node_id"));
		else if (!IsEdgeKindValid(Kind))
		{
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_initialization_edge_invalid"), TEXT("The initialization dependency kind is unsupported."), TEXT("/kind"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Kind));
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("ownership, creation_dependency, reference"));
		}
		else if (Kind != EInitializationEdgeKind::Reference && FromNodeId == ToNodeId)
		{
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_initialization_edge_invalid"), TEXT("An ownership or creation dependency cannot reference its own node."), TEXT("/to_node_id"));
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(ToNodeId);
		}
	}

	const FString& FInitializationEdge::GetFromNodeId() const { return FromNodeId; }
	const FString& FInitializationEdge::GetToNodeId() const { return ToNodeId; }
	EInitializationEdgeKind FInitializationEdge::GetKind() const { return Kind; }
	bool FInitializationEdge::IsValid() const { return ValidationDiagnostic.IsEmpty(); }

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
		if (!IsCarrierValid(Carrier))
		{
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_planning_request_invalid"), TEXT("The import source carrier must be directory, multi_file, single_file or inline."), TEXT("/carrier"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Carrier));
		}
		else if (!Model->IsValid())
			ValidationDiagnostic = Contract::FDiagnostic::WrapFailure(TEXT("hjson_import_planning_request_invalid"), Model->GetValidationDiagnostic());
		else if (TargetIdentity.IsEmpty())
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_planning_request_invalid"), TEXT("The import planning request requires a target."), TEXT("/target"));
		else if (!ExpectedTargetRevision.IsValid())
			ValidationDiagnostic = Contract::FDiagnostic::WrapFailure(TEXT("hjson_import_planning_request_invalid"), ExpectedTargetRevision.GetValidationDiagnostic());
		else if (!IsFingerprint(SourceBaselineFingerprint) || !IsFingerprint(ManifestFingerprint))
		{
			const bool bBaselineInvalid = !IsFingerprint(SourceBaselineFingerprint);
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_planning_request_invalid"),
				bBaselineInvalid ? TEXT("The import source baseline fingerprint is not a SHA-256 digest.") : TEXT("The import manifest fingerprint is not a SHA-256 digest."),
				bBaselineInvalid ? TEXT("/source_baseline_fingerprint") : TEXT("/manifest_fingerprint"));
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(bBaselineInvalid ? SourceBaselineFingerprint : ManifestFingerprint);
		}
		else if (AuthorizationId.IsEmpty())
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_planning_request_invalid"), TEXT("The import planning request requires an authorization identifier."), TEXT("/authorization_id"));
		if (!ValidationDiagnostic.IsEmpty() && ValidationDiagnostic.TargetPath.IsEmpty()) ValidationDiagnostic.TargetPath = TargetIdentity;
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
	bool FImportInitializationPlanningRequest::IsValid() const { return ValidationDiagnostic.IsEmpty(); }

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
		if (!IsFingerprint(SourceModelFingerprint))
		{
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_plan_invalid"), TEXT("The import plan source model fingerprint is not a SHA-256 digest."), TEXT("/source_model_fingerprint"));
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(SourceModelFingerprint);
		}
		else if (TargetIdentity.IsEmpty())
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_plan_invalid"), TEXT("The import plan requires a target."), TEXT("/target"));
		else if (!ExpectedTargetRevision.IsValid())
			ValidationDiagnostic = Contract::FDiagnostic::WrapFailure(TEXT("hjson_import_plan_invalid"), ExpectedTargetRevision.GetValidationDiagnostic());
		else if (!IsFingerprint(PlanFingerprint))
		{
			ValidationDiagnostic = InitializationFailure(TEXT("hjson_import_plan_invalid"), TEXT("The import plan fingerprint is not a SHA-256 digest."), TEXT("/plan_fingerprint"));
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(PlanFingerprint);
		}
		if (!ValidationDiagnostic.IsEmpty() && ValidationDiagnostic.TargetPath.IsEmpty()) ValidationDiagnostic.TargetPath = TargetIdentity;
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
	bool FImportInitializationPlan::IsValid() const { return ValidationDiagnostic.IsEmpty(); }

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
			OutError.SetFailure(Request.GetValidationDiagnostic());
			return false;
		}

		auto RecordError = [&](Contract::FDiagnostic Diagnostic, const FString& Prefix = FString(), const FString& Subject = FString())
		{
			FWorkspaceError Item;
			if (!Prefix.IsEmpty()) Diagnostic.PropertyPath = Prefix + (Diagnostic.PropertyPath.IsEmpty() || Diagnostic.PropertyPath.StartsWith(TEXT("/")) ? Diagnostic.PropertyPath : TEXT("/") + Diagnostic.PropertyPath);
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Request.GetTargetIdentity();
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			Item.SetFailure(MoveTemp(Diagnostic));
			OutError.Append(Item);
		};
		TMap<FString, int32> ChangeIds;
		for (int32 Index = 0; Index < Request.GetChanges().Num(); ++Index)
		{
			const FImportChange& Change = Request.GetChanges()[Index];
			const FString Path = FString::Printf(TEXT("/changes/%d"), Index);
			if (!Change.IsValid())
			{
				RecordError(Change.GetValidationDiagnostic(), Path, Change.GetChangeId());
				continue;
			}
			if (const int32* PreviousIndex = ChangeIds.Find(Change.GetChangeId()))
			{
				auto Error = InitializationFailure(TEXT("hjson_import_change_duplicate"), FString::Printf(TEXT("The import change duplicates change %d."), *PreviousIndex), Path + TEXT("/change_id"));
				Error.ActualEvidence = MakeShared<FJsonValueString>(Change.GetChangeId());
				RecordError(MoveTemp(Error), FString(), Change.GetChangeId());
				continue;
			}
			ChangeIds.Add(Change.GetChangeId(), Index);
		}

		TArray<FString> NodeIds;
		TMap<FString, int32> NodeIndex;
		TSet<FString> InvalidNodeIds;
		for (int32 Index = 0; Index < Request.GetNodes().Num(); ++Index)
		{
			const FInitializationNode& Node = Request.GetNodes()[Index];
			if (!Node.IsValid())
			{
				RecordError(Node.GetValidationDiagnostic(), FString::Printf(TEXT("/nodes/%d"), Index), Node.GetNodeId());
				InvalidNodeIds.Add(Node.GetNodeId());
				continue;
			}
			if (NodeIndex.Contains(Node.GetNodeId()))
			{
				auto Error = InitializationFailure(TEXT("hjson_import_initialization_node_duplicate"),
					FString::Printf(TEXT("The initialization node duplicates node %d."), NodeIndex.FindChecked(Node.GetNodeId())), FString::Printf(TEXT("/nodes/%d/node_id"), Index));
				Error.ActualEvidence = MakeShared<FJsonValueString>(Node.GetNodeId());
				RecordError(MoveTemp(Error), FString(), Node.GetNodeId());
				InvalidNodeIds.Add(Node.GetNodeId());
				continue;
			}
			NodeIndex.Add(Node.GetNodeId(), Index);
			NodeIds.Add(Node.GetNodeId());
		}
		NodeIds.Sort();

		TSet<FString> EdgeKeys;
		TMap<FString, FString> OwnerByChild;
		TArray<FInitializationEdge> ReferenceEdges;
		for (int32 Index = 0; Index < Request.GetEdges().Num(); ++Index)
		{
			const FInitializationEdge& Edge = Request.GetEdges()[Index];
			const FString Path = FString::Printf(TEXT("/edges/%d"), Index);
			if (Edge.IsValid() && (InvalidNodeIds.Contains(Edge.GetFromNodeId())
				|| InvalidNodeIds.Contains(Edge.GetToNodeId())))
			{
				OutError.MarkUnchecked(TEXT("initialization_edge"), EdgeKey(Edge), TEXT("invalid_node"));
				continue;
			}
			if (!Edge.IsValid()
				|| !NodeIndex.Contains(Edge.GetFromNodeId())
				|| !NodeIndex.Contains(Edge.GetToNodeId()))
			{
				if (!Edge.IsValid()) RecordError(Edge.GetValidationDiagnostic(), Path, Edge.GetFromNodeId());
				else
				{
					const bool bSourceMissing = !NodeIndex.Contains(Edge.GetFromNodeId());
					auto Error = InitializationFailure(TEXT("hjson_import_initialization_edge_invalid"),
						bSourceMissing ? TEXT("The initialization dependency source node is absent from the plan.") : TEXT("The initialization dependency destination node is absent from the plan."),
						Path + (bSourceMissing ? TEXT("/from_node_id") : TEXT("/to_node_id")));
					Error.ActualEvidence = MakeShared<FJsonValueString>(bSourceMissing ? Edge.GetFromNodeId() : Edge.GetToNodeId());
					RecordError(MoveTemp(Error));
				}
				continue;
			}
			const FString Key = EdgeKey(Edge);
			if (EdgeKeys.Contains(Key))
			{
				auto Error = InitializationFailure(TEXT("hjson_import_initialization_edge_duplicate"), TEXT("The same initialization dependency occurs more than once."), Path);
				Error.ActualEvidence = MakeShared<FJsonValueString>(Key);
				RecordError(MoveTemp(Error), FString(), Edge.GetFromNodeId());
				continue;
			}
			EdgeKeys.Add(Key);
			if (Edge.GetKind() == EInitializationEdgeKind::Ownership)
			{
				if (OwnerByChild.Contains(Edge.GetToNodeId()))
				{
					auto Error = InitializationFailure(TEXT("hjson_import_multiple_owners"), TEXT("An initialization node has more than one owner."), Path + TEXT("/to_node_id"));
					Error.ExpectedValue = MakeShared<FJsonValueNumber>(1);
					Error.ActualValue = MakeShared<FJsonValueNumber>(2);
					RecordError(MoveTemp(Error), FString(), Edge.GetToNodeId());
					continue;
				}
				OwnerByChild.Add(Edge.GetToNodeId(), Edge.GetFromNodeId());
			}
			else if (Edge.GetKind() == EInitializationEdgeKind::Reference)
			{
				ReferenceEdges.Add(Edge);
			}
		}

		TArray<FString> ImpactScopes = Request.GetImpactScopes();
		for (int32 Index = 0; Index < ImpactScopes.Num(); ++Index)
		{
			if (ImpactScopes[Index].IsEmpty())
			{
				auto Error = InitializationFailure(TEXT("hjson_import_impact_scope_invalid"), TEXT("An import impact scope is empty."), FString::Printf(TEXT("/impact_scopes/%d"), Index));
				Error.ActualValue = MakeShared<FJsonValueString>(ImpactScopes[Index]);
				RecordError(MoveTemp(Error));
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
			auto Error = InitializationFailure(TEXT("hjson_import_impact_scope_required"), TEXT("An import plan containing changes must declare their impact scopes."), TEXT("/impact_scopes"));
			Error.ActualValue = MakeShared<FJsonValueNumber>(0);
			RecordError(MoveTemp(Error));
		}
		if (OutError.HasErrors())
		{
			OutError.MarkUnchecked(TEXT("initialization_preflight"), FString(), TEXT("invalid_plan_structure"));
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
			SetFallbackError(OutError, TEXT("hjson_import_target_version_read_failed"),
				TEXT("The target's current revision could not be read."), Request.GetTargetIdentity());
			return false;
		}
		if (!Current.IsValid())
		{
			auto Diagnostic = Contract::FDiagnostic::WrapFailure(TEXT("hjson_import_target_revision_invalid"), Current.GetValidationDiagnostic(), TEXT("import_initialization"));
			Diagnostic.ActualEvidence = RevisionEvidence(Current);
			OutError.SetFailure(MoveTemp(Diagnostic), Request.GetTargetIdentity());
			return false;
		}
		if (Current != Request.GetExpectedTargetRevision())
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("hjson_import_target_version_conflict"),
				TEXT("The target changed after this import was prepared."), TEXT("import_initialization"));
			Diagnostic.ExpectedEvidence = RevisionEvidence(Request.GetExpectedTargetRevision());
			Diagnostic.ActualEvidence = RevisionEvidence(Current);
			OutError.SetFailure(MoveTemp(Diagnostic), Request.GetTargetIdentity());
			return false;
		}

		TMap<FString, FString> ResolvedCandidates;
		for (const FImportChange& Change : Request.GetChanges())
		{
			FWorkspaceError ItemError;
			if (!Preflight->SupportsChange(Change, ItemError))
			{
				SetFallbackError(
					ItemError,
					TEXT("hjson_import_change_unsupported"),
					TEXT("The target does not support this import change."),
					Change.GetChangeId());
				OutError.Append(ItemError);
				if (Change.HasCandidate()) OutError.MarkUnchecked(TEXT("candidate_resolution"), Change.GetChangeId(), TEXT("unsupported_change"));
				continue;
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
					ItemError))
			{
				SetFallbackError(
					ItemError,
					TEXT("hjson_import_candidate_invalid"),
					TEXT("The requested candidate could not be resolved for this import change."),
					Change.GetCandidateName());
				if (ItemError.GetDiagnostic().CandidateName.IsEmpty())
				{
					auto Diagnostic = ItemError.GetDiagnostic();
					Diagnostic.CandidateName = Change.GetCandidateName();
					ItemError.SetFailure(MoveTemp(Diagnostic));
				}
				OutError.Append(ItemError);
				continue;
			}
			Resolved = Normalize(MoveTemp(Resolved));
			if (Resolved != Change.GetCandidateName())
			{
				FWorkspaceError Item;
				auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("hjson_import_candidate_identity_changed"),
					TEXT("Candidate resolution returned a different candidate name."), TEXT("import_initialization"));
				Diagnostic.CandidateName = Change.GetCandidateName();
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Change.GetCandidateName());
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(Resolved);
				Item.SetFailure(MoveTemp(Diagnostic), Change.GetChangeId());
				OutError.Append(Item);
				continue;
			}
			ResolvedCandidates.Add(Change.GetChangeId(), MoveTemp(Resolved));
		}
		if (OutError.HasErrors())
		{
			OutError.MarkUnchecked(TEXT("initialization_order"), FString(), TEXT("preflight_failed"));
			return false;
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
			auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("hjson_import_ownership_cycle"),
				TEXT("Ownership dependencies contain a cycle, so the affected objects cannot be initialized in order."), TEXT("import_initialization"));
			Diagnostic.CyclePath = MoveTemp(Cycle);
			OutError.SetFailure(MoveTemp(Diagnostic), Request.GetTargetIdentity());
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
			auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("hjson_import_creation_dependency_cycle"),
				TEXT("Creation dependencies contain a cycle, so the affected objects cannot be created in order."), TEXT("import_initialization"));
			Diagnostic.CyclePath = MoveTemp(Cycle);
			OutError.SetFailure(MoveTemp(Diagnostic), Request.GetTargetIdentity());
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
			SetFallbackError(OutError, TEXT("hjson_import_unauthorized"),
				TEXT("The import's declared impact is not authorized."), Request.GetTargetIdentity());
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
			OutError.SetFailure(Candidate->GetValidationDiagnostic());
			return false;
		}
		OutPlan = MoveTemp(Candidate);
		return true;
	}
}
