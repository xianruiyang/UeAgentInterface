#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeGraphDocument.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentGraphReferenceBinding.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeAuthoringState.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeNativeGraph.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "UObject/StrongObjectPtr.h"

namespace UeAgentBehaviorTreeAuthoring
{
	static bool CaptureCandidateModel(UBehaviorTree& Tree,
		UE::UAI::HjsonWorkspace::FAuthoringModelPtr& Model, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError)
	{
		using namespace UE::UAI;
		OutError.Reset();
		UE::UAI::Contract::FDiagnostic Detail;
		const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("behavior_tree"));
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!Profile)
		{
			Detail = Contract::FDiagnostic::Failure(TEXT("behavior_tree_authoring_profile_missing"),
				TEXT("The Behavior Tree authoring profile is not registered."), TEXT("candidate_resolve"));
			Detail.SourceKind = TEXT("native_result");
			OutError.SetFailure(Detail, Tree.GetPathName());
			return false;
		}
		if (!UeAgentReflectionAuthoring::RegisterAuthoring(Detail)
			|| !UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, Tree, Requests, Detail)
			|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError.SetFailure(Detail.IsEmpty() ? Capture.Diagnostic : Detail, Tree.GetPathName());
			return false;
		}
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
		return HjsonWorkspace::TryBuildCanonicalAuthoringCapture(
			HjsonWorkspace::ResolveTargetCompositionKind(Profile->TargetKind, Profile->TypeId),
			TEXT("llm_authoring"), Capture.Snapshot, Resources, Snapshot, Model, OutError);
	}

	bool TryResolveCapturedGraphOwnerReference(UBehaviorTree& Tree, const FString& Reference,
		UEdGraph*& OutGraph, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError)
	{
		using namespace UE::UAI;
		OutGraph = nullptr;
		HjsonWorkspace::FAuthoringModelPtr Model;
		if (!CaptureCandidateModel(Tree, Model, OutError)) return false;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FWorkspaceLayoutPlanPtr Layout;
		if (!HjsonWorkspace::TryPlanWorkspaceLayout(Model.ToSharedRef(), 2,
			HjsonWorkspace::EWorkspaceSourceMode::Directory, {}, Resources, Layout, OutError)) return false;
		const FString* OwnerId = Layout->GetMachineManifest().PublicRefMapping.FindKey(Reference);
		const auto* Owner = OwnerId ? Model->FindUnit(*OwnerId) : nullptr;
		auto RejectOwner = [&](const FString& Reason, const bool bNativeResult)
		{
			auto Detail = Contract::FDiagnostic::Failure(TEXT("behavior_tree_candidate_graph_owner_unresolved"), Reason, TEXT("candidate_resolve"));
			Detail.TargetPath = Tree.GetPathName();
			Detail.ActualValue = MakeShared<FJsonValueString>(Reference);
			Detail.SourceKind = bNativeResult ? TEXT("native_result") : TEXT("command_input");
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		};
		if (!Owner || Owner->GetUnitKind() != HjsonWorkspace::ESemanticUnitKind::ItemState)
			return RejectOwner(TEXT("The graph-owner reference does not identify a captured Behavior Tree item."), false);
		const auto* Identity = Owner->GetValues().Find(TEXT("id"));
		TSharedPtr<FJsonValue> IdentityJson;
		UE::UAI::Contract::FDiagnostic Detail;
		FGraphCapture Native;
		if (!Identity) return RejectOwner(TEXT("The captured graph owner has no native identity."), true);
		if (!StateSemantic::CanonicalJson::TryToJsonValue(Identity->Get(), IdentityJson, Detail))
		{ OutError.SetFailure(Detail, Tree.GetPathName()); return false; }
		if (!IdentityJson.IsValid() || IdentityJson->Type != EJson::String)
			return RejectOwner(TEXT("The captured graph owner identity is not a string."), true);
		if (!CaptureGraph(Tree, Native, Detail))
		{ OutError.SetFailure(Detail, Tree.GetPathName()); return false; }
		for (const auto& Graph : Native.OwnedGraphs)
			if (Graph.OwnerId == IdentityJson->AsString()) OutGraph = const_cast<UEdGraph*>(Graph.NativeGraph);
		if (!OutGraph) return RejectOwner(TEXT("The captured item no longer has the requested native graph."), true);
		return true;
	}

	bool TryResolveCapturedNodeReference(UBehaviorTree& Tree, const FString& Reference,
		UBehaviorTreeGraphNode*& OutNode, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError)
	{
		using namespace UE::UAI;
		OutNode = nullptr;
		HjsonWorkspace::FAuthoringModelPtr Model;
		if (!CaptureCandidateModel(Tree, Model, OutError)) return false;
		UE::UAI::Contract::FDiagnostic Detail;
		FString NodeId;
		for (const auto& Unit : Model->GetUnits())
		{
			if (Unit.GetSemanticTypeRef() != GraphSemanticId) continue;
			const auto* Owner = Model->FindUnit(Unit.GetOwnerRef());
			if (Owner && Owner->GetUnitKind() == HjsonWorkspace::ESemanticUnitKind::ItemState) continue;
			const auto* Rows = Unit.GetValues().Find(TEXT("nodes"));
			if (!Rows)
			{
				Detail = Contract::FDiagnostic::Failure(TEXT("behavior_tree_candidate_parent_unresolved"),
					TEXT("The captured Behavior Tree graph has no node collection."), TEXT("candidate_resolve"));
				Detail.TargetPath = Tree.GetPathName();
				Detail.SourceKind = TEXT("native_result");
				OutError.SetFailure(Detail);
				return false;
			}
			if (!HjsonWorkspace::TryResolveCapturedGraphNodeReference(Rows->Get(), Reference, NodeId, OutError)) return false;
			break;
		}
		FGraphCapture Native;
		if (!NodeId.IsEmpty())
		{
			if (!CaptureGraph(Tree, Native, Detail))
			{ OutError.SetFailure(Detail, Tree.GetPathName()); return false; }
			for (const auto& Node : Native.Nodes)
				if (Node.Id == NodeId) OutNode = const_cast<UBehaviorTreeGraphNode*>(Cast<UBehaviorTreeGraphNode>(Node.NativeNode));
		}
		if (!OutNode)
		{
			Detail = Contract::FDiagnostic::Failure(TEXT("behavior_tree_candidate_parent_unresolved"),
				TEXT("The parent reference does not identify an available native Behavior Tree node."), TEXT("candidate_resolve"));
			Detail.TargetPath = Tree.GetPathName();
			Detail.ActualValue = MakeShared<FJsonValueString>(Reference);
			Detail.SourceKind = TEXT("native_result");
			Detail.InternalContext = NodeId;
			OutError.SetFailure(Detail);
			return false;
		}
		return true;
	}

	bool BuildGraphDocumentFromSemanticModel(UBehaviorTree& Tree,
		const UE::UAI::HjsonWorkspace::FAuthoringModelRef& Model,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		TSharedPtr<FJsonObject>& OutDocument, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError,
		TMap<FString, UE::UAI::Contract::FSourceLocation>* OutSourceLocations,
		TArray<UE::UAI::HjsonWorkspace::FGraphNodeReferenceBinding>* OutReferenceBindings)
	{
		using namespace UE::UAI;
		OutError.Reset();
		TMap<FString, Contract::FSourceLocation> LocalSources;
		auto& Sources = OutSourceLocations ? *OutSourceLocations : LocalSources;
		Sources.Reset();
		if (OutReferenceBindings) OutReferenceBindings->Reset();
		auto Forward = [&](Contract::FDiagnostic Diagnostic, const HjsonWorkspace::FSemanticUnit* Unit = nullptr, bool bNativeDocument = false) {
			TArray<Contract::FDiagnostic*> Pending{&Diagnostic};
			while (!Pending.IsEmpty())
			{
				auto& Issue = *Pending.Pop(EAllowShrinking::No);
				if (Issue.TargetPath.IsEmpty()) Issue.TargetPath = Tree.GetPathName();
				if (Unit && bNativeDocument && Issue.SourceKind != TEXT("native_result") && Issue.SourceKind != TEXT("command_input"))
					Prepared.GetWorkspace()->LocateDocumentDiagnostic({}, {}, Issue, Unit->GetUnitId());
				if (Unit && !bNativeDocument && Issue.SourceKind != TEXT("native_result") && Issue.SourceKind != TEXT("command_input")
					&& Issue.Source.SourceUnit.IsEmpty())
				{
					Issue.Source.SourceUnit = Unit->GetUnitId();
					if (Issue.Source.InternalJsonPath.IsEmpty())
						Issue.Source.InternalJsonPath = !Issue.SemanticPointer.IsEmpty() ? Issue.SemanticPointer : Issue.Source.JsonPath;
				}
				for (auto& Child : Issue.Diagnostics) Pending.Add(&Child);
				if (Issue.PublicCause)
				{
					TSharedPtr<Contract::FDiagnostic> Cause = MakeShared<Contract::FDiagnostic>(*Issue.PublicCause);
					Issue.PublicCause = Cause;
					Pending.Add(Cause.Get());
				}
			}
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		};
		auto Fail = [&](const TCHAR* Code, const FString& Reason, const HjsonWorkspace::FSemanticUnit& Unit,
			const FString& Path, TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr) {
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("document_restore"));
			Diagnostic.Source.InternalJsonPath = Path;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			if (Diagnostic.ExpectedValue) Diagnostic.ExpectedValuePresent = true;
			if (Diagnostic.ActualValue || Diagnostic.ExpectedValue) Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
			return Forward(MoveTemp(Diagnostic), &Unit);
		};
		auto FailNative = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Context = FString()) {
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("document_restore"));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.InternalContext = Context;
			return Forward(MoveTemp(Diagnostic));
		};
		auto RejectInjected = [&](const StateSemantic::CanonicalJson::FValueComparison& Difference,
			const HjsonWorkspace::FSemanticUnit& Unit, const FString& NativeNodeId = FString()) {
			auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("behavior_tree_semantic_injected_read_only"),
				NativeNodeId.IsEmpty() ? TEXT("An injected decorator graph cannot be modified.")
					: TEXT("An injected node cannot be modified or removed while its parent is retained."), TEXT("document_restore"));
			// Native document differences retain opaque identities only as internal evidence.
			Diagnostic.ExpectedEvidence = Difference.ExpectedValue;
			Diagnostic.ActualEvidence = Difference.ActualValue;
			Diagnostic.Source.InternalJsonPath = Difference.Path;
			const auto& Baseline = Prepared.GetInputBaseline();
			const HjsonWorkspace::FSemanticUnit* BeforeUnit = nullptr;
			if (Baseline)
				for (const auto& Candidate : Baseline->GetUnits())
					if (Candidate.GetSemanticTypeRef() == GraphSemanticId && Candidate.GetTargetIdentityKey() == Unit.GetTargetIdentityKey())
					{ BeforeUnit = &Candidate; break; }
			// Compare the same authored node through the existing public projection;
			// never classify diagnostic values by field-name filtering.
			auto ProjectPublicValue = [&](const HjsonWorkspace::FSemanticUnit& GraphUnit,
				TSharedPtr<FJsonValue>& Value, FString& Alias, HjsonWorkspace::FWorkspaceError& ProjectionError) {
				TSharedRef<FJsonObject> Visible = MakeShared<FJsonObject>();
				if (!HjsonWorkspace::TryProjectGraphAuthoringConsumerDocument(GraphUnit.GetUnitId(), GraphUnit.GetValues(), Visible, ProjectionError)) return false;
				if (NativeNodeId.IsEmpty()) { Value = MakeShared<FJsonValueObject>(Visible); return true; }
				const auto* Rows = GraphUnit.GetValues().Find(TEXT("nodes"));
				TSharedPtr<FJsonValue> RowsJson;
				Contract::FDiagnostic Conversion;
				if (Rows && !StateSemantic::CanonicalJson::TryToJsonValue(Rows->Get(), RowsJson, Conversion))
				{ ProjectionError.SetFailure(Conversion); return false; }
				if (RowsJson && RowsJson->Type == EJson::Object)
					for (const auto& Row : RowsJson->AsObject()->Values)
					{
						FString Id;
						if (Row.Value && Row.Value->Type == EJson::Object && Row.Value->AsObject()->TryGetStringField(TEXT("id"), Id) && Id == NativeNodeId)
						{
							Alias = Row.Key;
							const TSharedPtr<FJsonObject>* VisibleNodes = nullptr;
							if (Visible->TryGetObjectField(TEXT("nodes"), VisibleNodes)) Value = (*VisibleNodes)->TryGetField(Alias);
							break;
						}
					}
				return true;
			};
			if (BeforeUnit)
			{
				TSharedPtr<FJsonValue> Before, After;
				FString BeforeAlias, AfterAlias;
				HjsonWorkspace::FWorkspaceError ProjectionError;
				if (ProjectPublicValue(*BeforeUnit, Before, BeforeAlias, ProjectionError)
					&& ProjectPublicValue(Unit, After, AfterAlias, ProjectionError))
				{
					const FString Prefix = NativeNodeId.IsEmpty() ? FString() : TEXT("/nodes/")
						+ HjsonWorkspace::EscapeJsonPointerSegment(AfterAlias.IsEmpty() ? BeforeAlias : AfterAlias);
					const auto PublicDifference = StateSemantic::CanonicalJson::CompareJsonValues(Before, After, Prefix);
					if (!PublicDifference.bEquivalent) PublicDifference.ApplyTo(Diagnostic);
				}
				else
				{
					auto ProjectionDiagnostic = ProjectionError.GetDiagnostic();
					ProjectionDiagnostic.SourceKind = TEXT("native_result");
					Diagnostic.Diagnostics.Add(MoveTemp(ProjectionDiagnostic));
				}
			}
			Prepared.GetWorkspace()->LocateDocumentDiagnostic({}, {}, Diagnostic, Unit.GetUnitId());
			return Forward(MoveTemp(Diagnostic));
		};
		UE::UAI::Contract::FDiagnostic Detail;
		TSharedPtr<FJsonValue> Captured;
		if (!ReadGraphDocument(Tree, Captured, Detail)) { OutError.SetFailure(Detail); return false; }
		FGraphCapture NativeCapture;
		if (!CaptureGraph(Tree, NativeCapture, Detail)) { OutError.SetFailure(Detail); return false; }
		const auto FullDocument = Captured->AsObject();
		auto BuildContents = [&](const HjsonWorkspace::FSemanticUnit& GraphUnit,
			const FString& GraphId, const TSharedPtr<FJsonObject>& Document, bool bOwned,
			const TSharedPtr<FJsonObject>* InjectedBaseline = nullptr) -> bool {
			const FString GraphPointer = bOwned ? TEXT("/owned_graphs/") + HjsonWorkspace::EscapeJsonPointerSegment(GraphId) : FString();
			const auto Desired = MakeShared<FJsonObject>();
			const auto Existing = Document->GetObjectField(TEXT("nodes"));
			const auto Graph = MakeShared<FJsonObject>();
			for (const auto& Pair : GraphUnit.GetValues())
			{
				TSharedPtr<FJsonValue> Value;
				if (!StateSemantic::CanonicalJson::TryToJsonValue(Pair.Value.Get(), Value, Detail))
				{
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Detail, TEXT("/") + HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key));
					return Forward(Detail, &GraphUnit);
				}
				Graph->SetField(Pair.Key, Value);
			}
			if (bOwned)
			{
				FString Owner;
				if (!Graph->TryGetStringField(TEXT("owner"), Owner) || Owner != GraphUnit.GetOwnerRef())
					return Fail(TEXT("behavior_tree_semantic_graph_owner_changed"), TEXT("The captured owner of this decorator graph cannot be changed."), GraphUnit, TEXT("/owner"));
			}
			const TSharedPtr<FJsonObject>* Rows = nullptr;
			const TSharedPtr<FJsonObject>* Edges = nullptr;
			if (!Graph->TryGetObjectField(TEXT("nodes"), Rows) || !Rows || !Rows->IsValid())
				return Fail(TEXT("behavior_tree_semantic_graph_shape_invalid"), TEXT("The graph node collection must be an object."), GraphUnit,
					TEXT("/nodes"), Graph->TryGetField(TEXT("nodes")), MakeShared<FJsonValueString>(TEXT("object")));
			if (!Graph->TryGetObjectField(TEXT("edges"), Edges) || !Edges || !Edges->IsValid())
				return Fail(TEXT("behavior_tree_semantic_graph_shape_invalid"), TEXT("The graph connection collection must be an object."), GraphUnit,
					TEXT("/edges"), Graph->TryGetField(TEXT("edges")), MakeShared<FJsonValueString>(TEXT("object")));
			TMap<FString, FString> NativeByRef;
			TArray<FString> OrderedRefs;
			(*Rows)->Values.GetKeys(OrderedRefs);
			for (const FString& Ref : OrderedRefs)
				if (!(*Rows)->Values[Ref].IsValid() || (*Rows)->Values[Ref]->Type != EJson::Object)
					return Fail(TEXT("behavior_tree_semantic_node_invalid"), TEXT("A graph node declaration must be an object."), GraphUnit,
						TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Ref), (*Rows)->Values[Ref], MakeShared<FJsonValueString>(TEXT("object")));
			OrderedRefs.StableSort([&](const FString& Left, const FString& Right) {
				FString LeftParent, RightParent;
				(*Rows)->Values[Left]->AsObject()->TryGetStringField(TEXT("parent"), LeftParent);
				(*Rows)->Values[Right]->AsObject()->TryGetStringField(TEXT("parent"), RightParent);
				return LeftParent.IsEmpty() && !RightParent.IsEmpty();
			});
			TStrongObjectPtr<UBehaviorTreeGraph> ProspectiveGraph;
			for (const FString& Ref : OrderedRefs)
			{
				const auto Pair = TPair<FString, TSharedPtr<FJsonValue>>(Ref, (*Rows)->Values.FindChecked(Ref));
				const FString NodePointer = TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Ref);
				const auto Source = Pair.Value->AsObject();
				FString Operation;
				Source->TryGetStringField(TEXT("operation"), Operation);
				if (Operation == TEXT("remove")) continue;
				FString Id;
				Source->TryGetStringField(TEXT("id"), Id);
				const TSharedPtr<FJsonObject>* ExistingNode = nullptr;
				TSharedPtr<FJsonObject> Node;
				if (!Id.IsEmpty() && Existing->TryGetObjectField(Id, ExistingNode) && ExistingNode && ExistingNode->IsValid())
				{
					Node = UeAgentDetailsAuthoring::CloneObjectTree(*ExistingNode);
					FString RequestedKind, CapturedKind;
					if (Source->TryGetStringField(TEXT("attachment_kind"), RequestedKind)
						&& (!Node->TryGetStringField(TEXT("attachment_kind"), CapturedKind) || RequestedKind != CapturedKind))
						return Fail(TEXT("behavior_tree_semantic_attachment_kind_changed"), TEXT("An existing attachment must retain its captured attachment kind."), GraphUnit,
							NodePointer + TEXT("/attachment_kind"), Source->TryGetField(TEXT("attachment_kind")), Node->TryGetField(TEXT("attachment_kind")));
				}
				else
				{
					Node = MakeShared<FJsonObject>();
					FString Role, Name;
					Source->TryGetStringField(TEXT("role"), Role);
					Source->TryGetStringField(TEXT("name"), Name);
					if (!bOwned && Role == TEXT("root"))
					{
						Id = TEXT("node:root");
						Node->SetStringField(TEXT("node_class"), UBehaviorTreeGraphNode_Root::StaticClass()->GetPathName());
					}
					else
					{
						const TSharedPtr<FJsonObject>* CandidateObject = nullptr;
						HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
						const FString CandidatePointer = TEXT("/nodes/")
							+ HjsonWorkspace::EscapeJsonPointerSegment(Ref) + TEXT("/candidate");
						if (!Source->TryGetObjectField(TEXT("candidate"), CandidateObject) || !CandidateObject || !CandidateObject->IsValid())
						{
							auto CandidateDetail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("behavior_tree_semantic_candidate_missing"),
								TEXT("A new Behavior Tree node requires a candidate selector object."), TEXT("document_restore"));
							CandidateDetail.ActualValue = Source->TryGetField(TEXT("candidate"));
							CandidateDetail.ActualValuePresent = CandidateDetail.ActualValue.IsValid();
							CandidateDetail.ExpectedValue = MakeShared<FJsonValueString>(TEXT("candidate selector object"));
							CandidateDetail.TargetPath = Tree.GetPathName();
							CandidateDetail.Source.InternalJsonPath = CandidatePointer;
							return Forward(MoveTemp(CandidateDetail), &GraphUnit);
						}
						if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
							*CandidateObject, CandidatePointer, Selector, OutError)) return Forward(OutError.GetDiagnostic(), &GraphUnit);
						Name = Selector.Name;
						QueryOwners::AiBehavior::FAiBehaviorCandidate Candidate;
						QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError Error;
						QueryOwners::AiBehavior::FAiBehaviorCandidateContext Context;
						if (bOwned)
						{
							for (const auto& Owned : NativeCapture.OwnedGraphs)
								if (Owned.Id == GraphId) Context.Graph = const_cast<UEdGraph*>(Owned.NativeGraph);
							if (!Context.Graph) return FailNative(TEXT("behavior_tree_semantic_graph_unresolved"), TEXT("The captured decorator graph is no longer available."), GraphId);
							if (Source->HasField(TEXT("parent")) || Source->HasField(TEXT("attachment_kind")))
							{
								const FString Field = Source->HasField(TEXT("parent")) ? TEXT("parent") : TEXT("attachment_kind");
								return Fail(TEXT("behavior_tree_semantic_parent_invalid"), TEXT("Nodes inside a decorator graph do not accept parent or attachment_kind fields."), GraphUnit,
									NodePointer + TEXT("/") + Field, Source->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("field omitted")));
							}
						}
						FString ParentRef;
						TStrongObjectPtr<UBehaviorTreeGraphNode> ProspectiveParent;
						if (Source->TryGetStringField(TEXT("parent"), ParentRef) && !ParentRef.IsEmpty())
						{
							const FString* ParentId = NativeByRef.Find(ParentRef);
							if (!ParentId) return Fail(TEXT("behavior_tree_semantic_parent_unresolved"), TEXT("The parent reference does not identify an available node in this graph."), GraphUnit,
								NodePointer + TEXT("/parent"), Source->TryGetField(TEXT("parent")), MakeShared<FJsonValueString>(TEXT("available graph node reference")));
							const auto ParentRow = Desired->GetObjectField(*ParentId);
							if (!ParentRow->GetStringField(TEXT("parent")).IsEmpty())
								return Fail(TEXT("behavior_tree_semantic_parent_invalid"), TEXT("An attachment cannot be the parent of another attachment."), GraphUnit,
									NodePointer + TEXT("/parent"), Source->TryGetField(TEXT("parent")), MakeShared<FJsonValueString>(TEXT("top-level graph node reference")));
							UBehaviorTreeGraph* NativeGraph = Cast<UBehaviorTreeGraph>(Tree.BTGraph);
							if (!NativeGraph)
							{
								if (!ProspectiveGraph.IsValid()) ProspectiveGraph.Reset(NewObject<UBehaviorTreeGraph>(GetTransientPackage()));
								NativeGraph = ProspectiveGraph.Get();
							}
							UClass* ParentClass = LoadObject<UClass>(nullptr, *ParentRow->GetStringField(TEXT("node_class")));
							if (!ParentClass || !ParentClass->IsChildOf<UBehaviorTreeGraphNode>())
								return FailNative(TEXT("behavior_tree_semantic_parent_invalid"), TEXT("The captured parent class cannot be loaded as a Behavior Tree graph node."), ParentRow->GetStringField(TEXT("node_class")));
							ProspectiveParent.Reset(NewObject<UBehaviorTreeGraphNode>(NativeGraph, ParentClass, NAME_None, RF_Transient));
							Context.Parent = ProspectiveParent.Get();
							if (!Source->TryGetStringField(TEXT("attachment_kind"), Context.AttachmentKind))
								return Fail(TEXT("behavior_tree_semantic_attachment_kind_required"), TEXT("A node with a parent requires an attachment_kind string."), GraphUnit,
									NodePointer + TEXT("/attachment_kind"), Source->TryGetField(TEXT("attachment_kind")), MakeShared<FJsonValueString>(TEXT("attachment kind string")));
						}
						else if (Source->HasField(TEXT("attachment_kind")))
							return Fail(TEXT("behavior_tree_semantic_attachment_parent_required"), TEXT("An attachment_kind requires a nonempty parent reference."), GraphUnit,
								NodePointer + TEXT("/parent"), Source->TryGetField(TEXT("parent")), MakeShared<FJsonValueString>(TEXT("nonempty parent reference")));
						if (!QueryOwners::AiBehavior::TryResolveAiBehaviorCandidate(Tree, Name, FString(), Candidate, Error, Selector.Select, Context))
						{
							const FString Code = Error.Code.IsEmpty() ? TEXT("behavior_tree_semantic_candidate_kind_unsupported") : Error.Code;
							auto CandidateDetail = UeAgentAuthoringDiagnostics::CandidateFailure(Code, Name, Tree.GetPathName(), TEXT("BehaviorTree"), TEXT("ai.behavior.candidates"), Error);
							UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(CandidateDetail, CandidatePointer);
							return Forward(MoveTemp(CandidateDetail), &GraphUnit);
						}
						if (!Candidate.bSupported)
						{
							auto CandidateDetail = Contract::FDiagnostic::WrapFailure(TEXT("behavior_tree_semantic_candidate_kind_unsupported"), Candidate.SupportDiagnostic);
							UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(CandidateDetail, CandidatePointer);
							return Forward(MoveTemp(CandidateDetail), &GraphUnit);
						}
						UClass* NodeClass = Candidate.GraphNodeType.IsEmpty() ? nullptr
							: FindObject<UClass>(nullptr, *Candidate.GraphNodeType);
						if (!NodeClass || !NodeClass->IsChildOf(bOwned ? UEdGraphNode::StaticClass() : UBehaviorTreeGraphNode::StaticClass()))
							return FailNative(TEXT("behavior_tree_semantic_candidate_kind_unsupported"), TEXT("The resolved candidate does not provide a loadable graph-node class for this graph."), Candidate.GraphNodeType);
						Id = TEXT("node:") + FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
						Node->SetStringField(TEXT("node_class"), NodeClass->GetPathName());
						if (!Candidate.LogicMode.IsEmpty())
						{
							Node->SetStringField(TEXT("logic"), Candidate.LogicMode);
							Node->SetNumberField(TEXT("input_count"), Candidate.DefaultInputCount);
						}
						Node->SetStringField(TEXT("payload_class"), Candidate.PayloadType);
						Node->SetStringField(TEXT("candidate_name"), Name);
						if (Context.Parent) Node->SetStringField(TEXT("attachment_kind"), Context.AttachmentKind);
						const auto Select = MakeShared<FJsonObject>();
						for (const auto& Field : Selector.Select) Select->SetStringField(Field.Key, Field.Value);
						Node->SetObjectField(TEXT("candidate_select"), Select);
					}
					Node->SetStringField(TEXT("parent"), FString());
					Prepared.GetWorkspace()->AttachInlinePropertySources(GraphUnit.GetUnitId(),
						TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Ref), *Source);
					TSharedPtr<FJsonObject> InitialProperties;
					if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(
						Source, TEXT("properties"), InitialProperties, Detail))
					{
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Detail,
							TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Ref) + TEXT("/properties"));
						return Forward(Detail, &GraphUnit);
					}
					if (InitialProperties) Node->SetObjectField(TEXT("properties"), InitialProperties);
				}
				if (Desired->HasField(Id)) return Fail(TEXT("behavior_tree_semantic_node_duplicate"), TEXT("This declaration identifies a node already declared elsewhere in this graph."), GraphUnit, NodePointer);
				for (const TCHAR* Flag : {TEXT("injected"), TEXT("read_only")})
				{
					bool CapturedFlag = false, RequestedFlag = false;
					Node->TryGetBoolField(Flag, CapturedFlag);
					if (Source->HasField(Flag) && (!Source->TryGetBoolField(Flag, RequestedFlag) || RequestedFlag != CapturedFlag))
						return Fail(TEXT("behavior_tree_semantic_read_only_changed"), TEXT("The captured injected/read_only flag cannot be changed."), GraphUnit,
							NodePointer + TEXT("/") + Flag, Source->TryGetField(Flag), MakeShared<FJsonValueBoolean>(CapturedFlag));
				}
				if (bOwned && Source->HasField(TEXT("logic"))) Node->SetField(TEXT("logic"), Source->TryGetField(TEXT("logic")));
				if (Source->HasField(TEXT("pin_groups")))
				{
					const TSharedPtr<FJsonObject>* Groups = nullptr;
					FString Logic;
					int32 Inputs = 0;
					if (!bOwned || !Node->TryGetStringField(TEXT("logic"), Logic) || (Logic != TEXT("And") && Logic != TEXT("Or")))
						return Fail(TEXT("behavior_tree_semantic_pin_groups_invalid"), TEXT("pin_groups is only supported by And/Or nodes inside a decorator graph."), GraphUnit,
							NodePointer + TEXT("/pin_groups"), Source->TryGetField(TEXT("pin_groups")), MakeShared<FJsonValueString>(TEXT("And/Or decorator graph node")));
					if (!Source->TryGetObjectField(TEXT("pin_groups"), Groups) || (*Groups)->Values.Num() != 1)
						return Fail(TEXT("behavior_tree_semantic_pin_groups_invalid"), TEXT("pin_groups must be an object with the single field inputs."), GraphUnit,
							NodePointer + TEXT("/pin_groups"), Source->TryGetField(TEXT("pin_groups")), MakeShared<FJsonValueString>(TEXT("object containing only inputs")));
					if (!UeAgentAuthoringJsonFields::ReadInt32Field(**Groups, TEXT("inputs"), Inputs, Detail))
					{
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Detail, NodePointer + TEXT("/pin_groups"));
						return Forward(Detail, &GraphUnit);
					}
					if (!(*Groups)->HasField(TEXT("inputs")) || Inputs < 2 || Inputs > MAX_uint16)
						return Fail(TEXT("behavior_tree_semantic_pin_groups_invalid"), TEXT("The inputs count must be an integer from 2 through 65535."), GraphUnit,
							NodePointer + TEXT("/pin_groups/inputs"), (*Groups)->TryGetField(TEXT("inputs")), MakeShared<FJsonValueString>(TEXT("integer in [2, 65535]")));
					Node->SetNumberField(TEXT("input_count"), Inputs);
				}
				if (Source->HasField(TEXT("order"))) Node->SetField(TEXT("order"), Source->TryGetField(TEXT("order")));
				const TSharedPtr<FJsonObject>* Position = nullptr;
				if (Source->TryGetObjectField(TEXT("pos"), Position) && Position && Position->IsValid())
					Node->SetObjectField(TEXT("pos"), UeAgentDetailsAuthoring::CloneObjectTree(*Position));
				if (!Node->HasField(TEXT("pos")))
				{
					const auto Pos = MakeShared<FJsonObject>();
					Pos->SetNumberField(TEXT("x"), 0);
					Pos->SetNumberField(TEXT("y"), 0);
					Node->SetObjectField(TEXT("pos"), Pos);
				}
				Desired->SetObjectField(Id, Node);
				const FString NativeNodePointer = GraphPointer + TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Id);
				Prepared.GetWorkspace()->ProjectValueSources(GraphUnit.GetUnitId(), NodePointer, NativeNodePointer, Sources);
				if (Source->HasField(TEXT("pin_groups")))
					Prepared.GetWorkspace()->ProjectValueSources(GraphUnit.GetUnitId(), NodePointer + TEXT("/pin_groups/inputs"),
						NativeNodePointer + TEXT("/input_count"), Sources);
				NativeByRef.Add(Pair.Key, Id);
				NativeByRef.Add(Id, Id);
			}
			// Every graph item, including owned attachments, participates in the common desired set.
			for (const auto& Pair : (*Rows)->Values)
			{
				const FString* Id = NativeByRef.Find(Pair.Key);
				if (!Id) continue;
				FString Parent;
				if (Pair.Value->AsObject()->TryGetStringField(TEXT("parent"), Parent) && !Parent.IsEmpty())
				{
					const FString* ParentId = NativeByRef.Find(Parent);
					if (!ParentId) return Fail(TEXT("behavior_tree_semantic_parent_unresolved"), TEXT("The parent reference does not identify an available node in this graph."), GraphUnit,
						TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key) + TEXT("/parent"),
						MakeShared<FJsonValueString>(Parent), MakeShared<FJsonValueString>(TEXT("available graph node reference")));
					Desired->GetObjectField(*Id)->SetStringField(TEXT("parent"), *ParentId);
					if (OutReferenceBindings)
						OutReferenceBindings->Add({GraphUnit.GetUnitId(), *Id, TEXT("parent"), Parent, *ParentId});
				}
			}
			TArray<TSharedPtr<FJsonValue>> Connections;
			for (const auto& Pair : (*Edges)->Values)
			{
				const FString EdgePointer = TEXT("/edges/") + HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key);
				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
					return Fail(TEXT("behavior_tree_semantic_connection_invalid"), TEXT("A graph connection must be an object."), GraphUnit,
						EdgePointer, Pair.Value, MakeShared<FJsonValueString>(TEXT("object")));
				const auto Source = Pair.Value->AsObject();
				FString Operation;
				Source->TryGetStringField(TEXT("operation"), Operation);
				if (Operation == TEXT("remove")) continue;
				const auto Edge = MakeShared<FJsonObject>();
				for (const TCHAR* Field : {TEXT("from"), TEXT("to")})
				{
					const TSharedPtr<FJsonObject>* Endpoint = nullptr;
					FString Ref;
					const FString EndpointPointer = EdgePointer + TEXT("/") + Field;
					if (!Source->TryGetObjectField(Field, Endpoint) || !Endpoint || !Endpoint->IsValid())
						return Fail(TEXT("behavior_tree_semantic_endpoint_unresolved"), TEXT("A connection endpoint must be an object."), GraphUnit,
							EndpointPointer, Source->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("endpoint object")));
					if (!(*Endpoint)->TryGetStringField(TEXT("node"), Ref))
						return Fail(TEXT("behavior_tree_semantic_endpoint_unresolved"), TEXT("A connection endpoint requires a node reference string."), GraphUnit,
							EndpointPointer + TEXT("/node"), (*Endpoint)->TryGetField(TEXT("node")), MakeShared<FJsonValueString>(TEXT("node reference string")));
					if (!NativeByRef.Contains(Ref))
						return Fail(TEXT("behavior_tree_semantic_endpoint_unresolved"), TEXT("The connection endpoint references a node absent from this graph."), GraphUnit,
							EndpointPointer + TEXT("/node"), (*Endpoint)->TryGetField(TEXT("node")), MakeShared<FJsonValueString>(TEXT("available graph node reference")));
					const auto Native = UeAgentDetailsAuthoring::CloneObjectTree(*Endpoint);
					Native->SetStringField(TEXT("node"), NativeByRef.FindChecked(Ref));
					Edge->SetObjectField(Field, Native);
				}
				Prepared.GetWorkspace()->ProjectValueSources(GraphUnit.GetUnitId(), EdgePointer,
					GraphPointer + TEXT("/connections/") + LexToString(Connections.Num()), Sources);
				Connections.Add(MakeShared<FJsonValueObject>(Edge));
			}
			for (const auto& Pair : Projection.PropertyChangesByUnitId)
			{
				const auto* Unit = Prepared.GetWorkspace()->GetModel()->GetUnits().FindByPredicate(
					[&](const auto& Candidate) { return Candidate.GetUnitId() == Pair.Key; });
				if (!Unit) return FailNative(TEXT("behavior_tree_semantic_property_unit_missing"), TEXT("A projected property change has no matching semantic unit."), Pair.Key);
				const auto* Change = Prepared.GetChangeSetDraft().GetChanges().FindByPredicate([&](const auto& Candidate) {
					return Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetCanonicalKey()
						|| Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetSelector(HjsonWorkspace::CanonicalAuthoringTargetIdentitySelector);
				});
				if (!Change) return FailNative(TEXT("behavior_tree_semantic_property_change_missing"), TEXT("The property unit has no matching prepared change."), Unit->GetTargetIdentityKey());
				const FString Id = Change->GetIdentity().GetSelector(TEXT("node_id"));
				if (Id.IsEmpty()) continue;
				if (Change->GetIdentity().GetSelector(TEXT("graph_name")) != GraphId) continue;
				const TSharedPtr<FJsonObject>* Node = nullptr;
				if (!Desired->TryGetObjectField(Id, Node) || !Node || !Node->IsValid())
					return FailNative(TEXT("behavior_tree_semantic_property_node_missing"), TEXT("The property change targets a node absent from the desired graph."), Id);
				const TArray<TSharedPtr<FJsonValue>>* ExistingChanges = nullptr;
				(*Node)->TryGetArrayField(TEXT("property_changes"), ExistingChanges);
				TArray<TSharedPtr<FJsonValue>> Changes = ExistingChanges ? *ExistingChanges : TArray<TSharedPtr<FJsonValue>>();
				Changes.Add(Pair.Value);
				(*Node)->SetArrayField(TEXT("property_changes"), MoveTemp(Changes));
			}
			for (const auto& Pair : Existing->Values)
			{
				const auto Before = Pair.Value->AsObject();
				bool bInjected = false;
				if (!Before->TryGetBoolField(TEXT("injected"), bInjected) || !bInjected) continue;
				FString Parent;
				Before->TryGetStringField(TEXT("parent"), Parent);
				if (!Desired->HasField(Parent) && !Desired->HasField(Pair.Key)) continue;
				const auto Difference = StateSemantic::CanonicalJson::CompareJsonValues(Pair.Value, Desired->TryGetField(Pair.Key),
					TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key));
				if (!Difference.bEquivalent) return RejectInjected(Difference, GraphUnit, Pair.Key);
			}
			Document->SetObjectField(TEXT("nodes"), Desired);
			Document->SetArrayField(TEXT("connections"), MoveTemp(Connections));
			Prepared.GetWorkspace()->ProjectValueSources(GraphUnit.GetUnitId(), TEXT("/blackboard"), GraphPointer + TEXT("/blackboard"), Sources);
			FString Blackboard;
			if (Graph->TryGetStringField(TEXT("blackboard"), Blackboard)) Document->SetStringField(TEXT("blackboard"), Blackboard);
			else if (const auto* Field = Graph->Values.Find(TEXT("blackboard")); Field && (*Field)->Type == EJson::Null)
				Document->SetStringField(TEXT("blackboard"), FString());
			// Compare the full native candidate before converting existing property
			// snapshots into the sparse apply document.
			if (InjectedBaseline)
			{
				const auto Difference = StateSemantic::CanonicalJson::CompareJsonValues(
					MakeShared<FJsonValueObject>(*InjectedBaseline), MakeShared<FJsonValueObject>(Document), GraphPointer);
				if (!Difference.bEquivalent) return RejectInjected(Difference, GraphUnit);
			}
			for (const auto& Pair : Desired->Values)
			{
				if (Existing->HasField(Pair.Key))
					Pair.Value->AsObject()->RemoveField(TEXT("properties"));
			}
			return true;
		};
		const HjsonWorkspace::FSemanticUnit* Main = nullptr;
		TArray<const HjsonWorkspace::FSemanticUnit*> OwnedUnits;
		for (const auto& Unit : Model->GetUnits())
		{
			if (Unit.GetSemanticTypeRef() != GraphSemanticId) continue;
			const auto* Owner = Model->FindUnit(Unit.GetOwnerRef());
			if (Owner && Owner->GetUnitKind() == HjsonWorkspace::ESemanticUnitKind::ItemState) OwnedUnits.Add(&Unit);
			else
			{
				if (Main) return FailNative(TEXT("behavior_tree_semantic_graph_ambiguous"), TEXT("The semantic model contains more than one main Behavior Tree graph."));
				Main = &Unit;
			}
		}
		if (!Main) return FailNative(TEXT("behavior_tree_semantic_graph_missing"), TEXT("The semantic model has no main Behavior Tree graph."));
		if (!BuildContents(*Main, TEXT("main"), FullDocument, false)) return false;
		const auto ExistingOwned = FullDocument->GetObjectField(TEXT("owned_graphs"));
		const auto DesiredOwned = MakeShared<FJsonObject>();
		for (const auto* Unit : OwnedUnits)
		{
			const auto* Owner = Model->FindUnit(Unit->GetOwnerRef());
			const auto* OwnerId = Owner ? Owner->GetValues().Find(TEXT("id")) : nullptr;
			if (!OwnerId || (*OwnerId)->GetKind() != StateSemantic::EUeAgentValueKind::String)
				return FailNative(TEXT("behavior_tree_semantic_graph_owner_unresolved"), TEXT("The decorator graph owner is missing or has no valid native identity."), Unit->GetOwnerRef());
			const FString Id = (*OwnerId)->GetText();
			if (!FullDocument->GetObjectField(TEXT("nodes"))->HasField(Id)) continue;
			const FString GraphId = TEXT("decorator:") + Id;
			const TSharedPtr<FJsonObject>* Graph = nullptr;
			if (!ExistingOwned->TryGetObjectField(GraphId, Graph)) return FailNative(TEXT("behavior_tree_semantic_owned_graph_missing"), TEXT("The captured decorator owner has no matching owned graph."), GraphId);
			bool bInjected = false;
			FullDocument->GetObjectField(TEXT("nodes"))->GetObjectField(Id)->TryGetBoolField(TEXT("injected"), bInjected);
			const auto DesiredGraph = UeAgentDetailsAuthoring::CloneObjectTree(*Graph);
			if (!BuildContents(*Unit, GraphId, DesiredGraph, true, bInjected ? Graph : nullptr)) return false;
			DesiredOwned->SetObjectField(GraphId, DesiredGraph);
		}
		for (const auto& Pair : FullDocument->GetObjectField(TEXT("nodes"))->Values)
		{
			if (Pair.Value->AsObject()->GetStringField(TEXT("node_class")) != UBehaviorTreeGraphNode_CompositeDecorator::StaticClass()->GetPathName()) continue;
			const FString GraphId = TEXT("decorator:") + Pair.Key;
			if (DesiredOwned->HasField(GraphId)) continue;
			if (ExistingOwned->HasField(GraphId)) return FailNative(TEXT("behavior_tree_semantic_owned_graph_missing"), TEXT("A retained decorator node is missing its existing owned graph in the desired model."), GraphId);
			TSharedPtr<FJsonObject> DefaultGraph;
			if (!BuildDefaultOwnedGraphDocument(Pair.Key, DefaultGraph, Detail)) { OutError.SetFailure(Detail); return false; }
			DesiredOwned->SetObjectField(GraphId, DefaultGraph);
		}
		FullDocument->SetObjectField(TEXT("owned_graphs"), DesiredOwned);
		UeAgentAuthoringDiagnostics::FScopedSourceLocations SourceScope(FullDocument.Get(), [&Sources](const FString& Pointer) {
			return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Sources, Pointer);
		});
		if (!ValidateGraphDocument(MakeShared<FJsonValueObject>(FullDocument), Detail))
		{
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Detail, *FullDocument);
			return Forward(Detail, Main, true);
		}
		OutDocument = FullDocument;
		return true;
	}
}
