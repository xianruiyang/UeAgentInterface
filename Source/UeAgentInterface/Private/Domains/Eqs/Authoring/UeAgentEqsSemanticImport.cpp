#include "Domains/Eqs/Authoring/UeAgentEqsGraphDocument.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"

#include "AIGraphNode.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentGraphReferenceBinding.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Eqs/Authoring/UeAgentEqsAuthoringState.h"
#include "Domains/Eqs/Authoring/UeAgentEqsNativeGraph.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQueryGraph.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "EnvironmentQueryGraphNode_Root.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/UeAgentDetailsAuthoring.h"

namespace UeAgentEqsAuthoring
{
	bool TryResolveCapturedNodeReference(UEnvQuery& Query, const FString& Reference,
		UAIGraphNode*& OutNode, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError)
	{
		using namespace UE::UAI;
		OutNode = nullptr;
		OutError.Reset();
		UE::UAI::Contract::FDiagnostic Detail;
		const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("eqs"));
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!Profile)
		{
			Detail = Contract::FDiagnostic::Failure(TEXT("eqs_authoring_profile_missing"),
				TEXT("The EQS authoring profile is not registered."), TEXT("candidate_resolve"));
			Detail.SourceKind = TEXT("native_result");
			Detail.TargetPath = Query.GetPathName();
			Detail.PropertyPath = TEXT("authoring.profile.available");
			Detail.CandidateName = TEXT("eqs");
			Detail.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		if (!UeAgentReflectionAuthoring::RegisterAuthoring(Detail))
		{
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		if (!UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, Query, Requests, Detail))
		{
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError.SetFailure(MoveTemp(Capture.Diagnostic));
			return false;
		}
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
		HjsonWorkspace::FAuthoringModelPtr Model;
		if (!HjsonWorkspace::TryBuildCanonicalAuthoringCapture(
			HjsonWorkspace::ResolveTargetCompositionKind(Profile->TargetKind, Profile->TypeId),
			TEXT("llm_authoring"), Capture.Snapshot, Resources, Snapshot, Model, OutError)) return false;
		FString NodeId;
		for (const auto& Unit : Model->GetUnits())
		{
			if (Unit.GetSemanticTypeRef() != GraphSemanticId) continue;
			const auto* Rows = Unit.GetValues().Find(TEXT("nodes"));
			if (!Rows)
			{
				Detail = Contract::FDiagnostic::Failure(TEXT("eqs_candidate_parent_unresolved"),
					TEXT("The captured EQS graph has no node collection."), TEXT("candidate_resolve"));
				Detail.TargetPath = Query.GetPathName();
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
			if (!CaptureGraph(Query, Native, Detail))
			{ OutError.SetFailure(MoveTemp(Detail)); return false; }
			for (const auto& Node : Native.Nodes)
				if (Node.Id == NodeId) OutNode = const_cast<UAIGraphNode*>(Cast<UAIGraphNode>(Node.NativeNode));
		}
		if (!OutNode)
		{
			Detail = Contract::FDiagnostic::Failure(TEXT("eqs_candidate_parent_unresolved"),
				TEXT("The parent reference does not identify an available native EQS node."), TEXT("candidate_resolve"));
			Detail.TargetPath = Query.GetPathName();
			Detail.ActualValue = MakeShared<FJsonValueString>(Reference);
			Detail.SourceKind = TEXT("native_result");
			Detail.InternalContext = NodeId;
			OutError.SetFailure(Detail);
			return false;
		}
		return true;
	}

	bool BuildGraphDocumentFromSemanticModel(UEnvQuery& Query,
		const UE::UAI::HjsonWorkspace::FAuthoringModelRef& Model,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		TSharedPtr<FJsonObject>& OutDocument, UE::UAI::HjsonWorkspace::FWorkspaceError& Error,
		TMap<FString, UE::UAI::Contract::FSourceLocation>* OutSourceLocations,
		TArray<UE::UAI::HjsonWorkspace::FGraphNodeReferenceBinding>* OutReferenceBindings)
	{
		using namespace UE::UAI;
		using namespace QueryOwners::AiBehavior;
		Error.Reset();
		TMap<FString, Contract::FSourceLocation> LocalSources;
		auto& Sources = OutSourceLocations ? *OutSourceLocations : LocalSources;
		Sources.Reset();
		if (OutReferenceBindings) OutReferenceBindings->Reset();
		auto Forward = [&](Contract::FDiagnostic Diagnostic, const HjsonWorkspace::FSemanticUnit* Unit = nullptr, bool bNativeDocument = false) {
			TArray<Contract::FDiagnostic*> Pending{&Diagnostic};
			while (!Pending.IsEmpty())
			{
				auto& Issue = *Pending.Pop(EAllowShrinking::No);
				if (Issue.TargetPath.IsEmpty()) Issue.TargetPath = Query.GetPathName();
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
			Error.SetFailure(MoveTemp(Diagnostic));
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
		UE::UAI::Contract::FDiagnostic Detail;
		TSharedPtr<FJsonValue> Captured;
		if (!ReadGraphDocument(Query, Captured, Detail)) { Error.SetFailure(Detail); return false; }
		const HjsonWorkspace::FSemanticUnit* GraphUnit = nullptr;
		for (const auto& Unit : Model->GetUnits())
			if (Unit.GetSemanticTypeRef() == GraphSemanticId)
			{
				if (GraphUnit) return FailNative(TEXT("eqs_semantic_graph_ambiguous"), TEXT("The semantic model contains more than one EQS graph."));
				GraphUnit = &Unit;
			}
		if (!GraphUnit) return FailNative(TEXT("eqs_semantic_graph_missing"), TEXT("The semantic model has no EQS graph."));
		const auto Graph = MakeShared<FJsonObject>();
		for (const auto& Pair : GraphUnit->GetValues())
		{
			TSharedPtr<FJsonValue> Value;
			if (!StateSemantic::CanonicalJson::TryToJsonValue(Pair.Value.Get(), Value, Detail))
			{
				UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Detail, TEXT("/") + HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key));
				return Forward(Detail, GraphUnit);
			}
			Graph->SetField(Pair.Key, Value);
		}
		const TSharedPtr<FJsonObject>* Rows = nullptr;
		const TSharedPtr<FJsonObject>* Edges = nullptr;
		if (!Graph->TryGetObjectField(TEXT("nodes"), Rows))
			return Fail(TEXT("eqs_semantic_graph_shape_invalid"), TEXT("The graph node collection must be an object."), *GraphUnit,
				TEXT("/nodes"), Graph->TryGetField(TEXT("nodes")), MakeShared<FJsonValueString>(TEXT("object")));
		if (!Graph->TryGetObjectField(TEXT("edges"), Edges))
			return Fail(TEXT("eqs_semantic_graph_shape_invalid"), TEXT("The graph connection collection must be an object."), *GraphUnit,
				TEXT("/edges"), Graph->TryGetField(TEXT("edges")), MakeShared<FJsonValueString>(TEXT("object")));
		const auto Existing = Captured->AsObject()->GetObjectField(TEXT("nodes"));
		const auto Desired = MakeShared<FJsonObject>();
		TMap<FString, FString> NativeByRef;
		TArray<FString> Refs;
		(*Rows)->Values.GetKeys(Refs);
		for (const FString& Ref : Refs)
			if (!(*Rows)->Values[Ref] || (*Rows)->Values[Ref]->Type != EJson::Object)
				return Fail(TEXT("eqs_semantic_node_invalid"), TEXT("A graph node declaration must be an object."), *GraphUnit,
					TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Ref), (*Rows)->Values[Ref], MakeShared<FJsonValueString>(TEXT("object")));
		Refs.StableSort([&](const FString& A, const FString& B) {
			FString AP, BP;
			(*Rows)->Values[A]->AsObject()->TryGetStringField(TEXT("parent"), AP);
			(*Rows)->Values[B]->AsObject()->TryGetStringField(TEXT("parent"), BP);
			return AP.IsEmpty() && !BP.IsEmpty();
		});
		TStrongObjectPtr<UEnvironmentQueryGraph> ProspectiveGraph;
		for (const FString& Ref : Refs)
		{
			const FString NodePointer = TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Ref);
			const auto Source = (*Rows)->Values[Ref]->AsObject();
			FString Operation, Id, ParentRef;
			Source->TryGetStringField(TEXT("operation"), Operation);
			if (Operation == TEXT("remove")) continue;
			Source->TryGetStringField(TEXT("id"), Id);
			Source->TryGetStringField(TEXT("parent"), ParentRef);
			const FString Parent = ParentRef.IsEmpty() ? FString() : NativeByRef.FindRef(ParentRef);
			if (!ParentRef.IsEmpty() && Parent.IsEmpty())
				return Fail(TEXT("eqs_semantic_parent_unresolved"), TEXT("The parent reference does not identify an available node in this graph."), *GraphUnit,
					NodePointer + TEXT("/parent"), Source->TryGetField(TEXT("parent")), MakeShared<FJsonValueString>(TEXT("available EQS option reference")));
			const TSharedPtr<FJsonObject>* ExistingNode = nullptr;
			TSharedPtr<FJsonObject> Node;
			if (!Id.IsEmpty() && Existing->TryGetObjectField(Id, ExistingNode)) Node = UeAgentDetailsAuthoring::CloneObjectTree(*ExistingNode);
			else
			{
				Node = MakeShared<FJsonObject>();
				FString Role;
				if (Source->TryGetStringField(TEXT("role"), Role) && Role == TEXT("root"))
				{
					Id = TEXT("node:root");
					Node->SetStringField(TEXT("node_class"), UEnvironmentQueryGraphNode_Root::StaticClass()->GetPathName());
				}
				else
				{
					const TSharedPtr<FJsonObject>* CandidateSpec = nullptr;
					HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
					const FString CandidatePointer = TEXT("/nodes/")
						+ HjsonWorkspace::EscapeJsonPointerSegment(Ref) + TEXT("/candidate");
					if (!Source->TryGetObjectField(TEXT("candidate"), CandidateSpec))
					{
						auto CandidateDetail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("eqs_semantic_candidate_required"),
							TEXT("A new EQS node requires a candidate selector object."), TEXT("document_restore"));
						CandidateDetail.ActualValue = Source->TryGetField(TEXT("candidate"));
						CandidateDetail.ActualValuePresent = CandidateDetail.ActualValue.IsValid();
						CandidateDetail.ExpectedValue = MakeShared<FJsonValueString>(TEXT("candidate selector object"));
						CandidateDetail.TargetPath = Query.GetPathName();
						CandidateDetail.Source.InternalJsonPath = CandidatePointer;
						return Forward(MoveTemp(CandidateDetail), GraphUnit);
					}
					if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(*CandidateSpec,
						CandidatePointer, Selector, Error)) return Forward(Error.GetDiagnostic(), GraphUnit);
					FAiBehaviorCandidateContext Context;
					TStrongObjectPtr<UEnvironmentQueryGraphNode_Option> ProspectiveParent;
					if (!Parent.IsEmpty())
					{
						if (Desired->GetObjectField(Parent)->GetStringField(TEXT("node_class")) != UEnvironmentQueryGraphNode_Option::StaticClass()->GetPathName())
							return Fail(TEXT("eqs_semantic_parent_invalid"), TEXT("An EQS test must be attached to an option node."), *GraphUnit,
								NodePointer + TEXT("/parent"), Source->TryGetField(TEXT("parent")), MakeShared<FJsonValueString>(TEXT("EQS option reference")));
						auto* NativeGraph = Cast<UEnvironmentQueryGraph>(Query.EdGraph);
						if (!NativeGraph)
						{
							if (!ProspectiveGraph) ProspectiveGraph.Reset(NewObject<UEnvironmentQueryGraph>(GetTransientPackage()));
							NativeGraph = ProspectiveGraph.Get();
						}
						ProspectiveParent.Reset(NewObject<UEnvironmentQueryGraphNode_Option>(NativeGraph, NAME_None, RF_Transient));
						Context.Parent = ProspectiveParent.Get();
						if (!Source->TryGetStringField(TEXT("attachment_kind"), Context.AttachmentKind) || Context.AttachmentKind != TEXT("test"))
							return Fail(TEXT("eqs_semantic_attachment_kind_required"), TEXT("A node attached to an EQS option requires attachment_kind test."), *GraphUnit,
								NodePointer + TEXT("/attachment_kind"), Source->TryGetField(TEXT("attachment_kind")), MakeShared<FJsonValueString>(TEXT("test")));
					}
					FAiBehaviorCandidate Candidate;
					FAiBehaviorCandidateQueryError Failure;
					if (!TryResolveAiBehaviorCandidate(Query, Selector.Name, FString(), Candidate, Failure, Selector.Select, Context))
					{ const FString Code = Failure.Code.IsEmpty() ? TEXT("eqs_semantic_candidate_kind_unsupported") : Failure.Code;
						auto CandidateDetail = UeAgentAuthoringDiagnostics::CandidateFailure(Code, Selector.Name, Query.GetPathName(), TEXT("EQS"), TEXT("ai.behavior.candidates"), Failure);
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(CandidateDetail, CandidatePointer);
						return Forward(MoveTemp(CandidateDetail), GraphUnit); }
					if (!Candidate.bSupported)
					{
						auto CandidateDetail = Contract::FDiagnostic::WrapFailure(TEXT("eqs_semantic_candidate_kind_unsupported"), Candidate.SupportDiagnostic);
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(CandidateDetail, CandidatePointer);
						return Forward(MoveTemp(CandidateDetail), GraphUnit);
					}
					Id = TEXT("node:") + FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
					Node->SetStringField(TEXT("node_class"), Candidate.GraphNodeType);
					Node->SetStringField(TEXT("payload_class"), Candidate.PayloadType);
					Node->SetStringField(TEXT("candidate_name"), Selector.Name);
					const auto Select = MakeShared<FJsonObject>();
					for (const auto& Field : Selector.Select) Select->SetStringField(Field.Key, Field.Value);
					Node->SetObjectField(TEXT("candidate_select"), Select);
				}
				Prepared.GetWorkspace()->AttachInlinePropertySources(GraphUnit->GetUnitId(),
					TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Ref), *Source);
				TSharedPtr<FJsonObject> InitialProperties;
				if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(
					Source, TEXT("properties"), InitialProperties, Detail))
				{
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Detail,
						TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Ref) + TEXT("/properties"));
					return Forward(Detail, GraphUnit);
				}
				if (InitialProperties) Node->SetObjectField(TEXT("properties"), InitialProperties);
			}
			if (Desired->HasField(Id)) return Fail(TEXT("eqs_semantic_node_duplicate"), TEXT("This declaration identifies a node already declared elsewhere in this graph."), *GraphUnit, NodePointer);
			if (Parent.IsEmpty()) Node->RemoveField(TEXT("parent"));
			else
			{
				Node->SetStringField(TEXT("parent"), Parent);
				if (OutReferenceBindings)
					OutReferenceBindings->Add({GraphUnit->GetUnitId(), Id, TEXT("parent"), ParentRef, Parent});
			}
			for (const TCHAR* Field : {TEXT("pos"), TEXT("enabled"), TEXT("order")})
				if (auto Value = Source->TryGetField(Field)) Node->SetField(Field, Value);
			if (!Node->HasField(TEXT("pos")))
			{
				const auto Position = MakeShared<FJsonObject>();
				Position->SetNumberField(TEXT("x"), 0);
				Position->SetNumberField(TEXT("y"), 0);
				Node->SetObjectField(TEXT("pos"), Position);
			}
			if (!Parent.IsEmpty())
			{
				if (!Node->HasField(TEXT("enabled"))) Node->SetBoolField(TEXT("enabled"), true);
				if (!Node->HasField(TEXT("order"))) Node->SetNumberField(TEXT("order"), Desired->Values.Num());
			}
			Desired->SetObjectField(Id, Node);
			Prepared.GetWorkspace()->ProjectValueSources(GraphUnit->GetUnitId(),
				TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Ref),
				TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Id), Sources);
			// A captured property document is before-state, not an edit. Existing
			// native owners receive only the projected property_changes below.
			if (Existing->HasField(Id)) Node->RemoveField(TEXT("properties"));
			NativeByRef.Add(Ref, Id);
			NativeByRef.Add(Id, Id);
		}
		TArray<TSharedPtr<FJsonValue>> Connections;
		for (const auto& Pair : (*Edges)->Values)
		{
			const FString EdgePointer = TEXT("/edges/") + HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key);
			if (!Pair.Value || Pair.Value->Type != EJson::Object)
				return Fail(TEXT("eqs_semantic_connection_invalid"), TEXT("A graph connection must be an object."), *GraphUnit,
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
				if (!Source->TryGetObjectField(Field, Endpoint))
					return Fail(TEXT("eqs_semantic_endpoint_unresolved"), TEXT("A connection endpoint must be an object."), *GraphUnit,
						EndpointPointer, Source->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("endpoint object")));
				if (!(*Endpoint)->TryGetStringField(TEXT("node"), Ref))
					return Fail(TEXT("eqs_semantic_endpoint_unresolved"), TEXT("A connection endpoint requires a node reference string."), *GraphUnit,
						EndpointPointer + TEXT("/node"), (*Endpoint)->TryGetField(TEXT("node")), MakeShared<FJsonValueString>(TEXT("node reference string")));
				if (!NativeByRef.Contains(Ref))
					return Fail(TEXT("eqs_semantic_endpoint_unresolved"), TEXT("The connection endpoint references a node absent from this graph."), *GraphUnit,
						EndpointPointer + TEXT("/node"), (*Endpoint)->TryGetField(TEXT("node")), MakeShared<FJsonValueString>(TEXT("available graph node reference")));
				const auto Native = UeAgentDetailsAuthoring::CloneObjectTree(*Endpoint);
				Native->SetStringField(TEXT("node"), NativeByRef.FindChecked(Ref));
				Edge->SetObjectField(Field, Native);
			}
			Prepared.GetWorkspace()->ProjectValueSources(GraphUnit->GetUnitId(), EdgePointer,
				TEXT("/connections/") + LexToString(Connections.Num()), Sources);
			Connections.Add(MakeShared<FJsonValueObject>(Edge));
		}
		for (const auto& Pair : Projection.PropertyChangesByUnitId)
		{
			const auto* Unit = Prepared.GetWorkspace()->GetModel()->GetUnits().FindByPredicate([&](const auto& Candidate) { return Candidate.GetUnitId() == Pair.Key; });
			if (!Unit) return FailNative(TEXT("eqs_semantic_property_unit_missing"), TEXT("A projected property change has no matching semantic unit."), Pair.Key);
			const auto* Change = Prepared.GetChangeSetDraft().GetChanges().FindByPredicate([&](const auto& Candidate) {
				return Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetCanonicalKey()
					|| Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetSelector(HjsonWorkspace::CanonicalAuthoringTargetIdentitySelector);
			});
			if (!Change) return FailNative(TEXT("eqs_semantic_property_change_missing"), TEXT("The property unit has no matching prepared change."), Unit->GetTargetIdentityKey());
			const FString Id = Change->GetIdentity().GetSelector(TEXT("node_id"));
			if (Id.IsEmpty()) continue;
			if (Change->GetIdentity().GetSelector(TEXT("graph_name")) != TEXT("main"))
				return FailNative(TEXT("eqs_semantic_property_node_missing"), TEXT("The property change targets a graph other than the main EQS graph."), Change->GetIdentity().GetSelector(TEXT("graph_name")));
			if (!Desired->HasField(Id))
				return FailNative(TEXT("eqs_semantic_property_node_missing"), TEXT("The property change targets a node absent from the desired EQS graph."), Id);
			const auto Node = Desired->GetObjectField(Id);
			const TArray<TSharedPtr<FJsonValue>>* Previous = nullptr;
			Node->TryGetArrayField(TEXT("property_changes"), Previous);
			TArray<TSharedPtr<FJsonValue>> Changes = Previous ? *Previous : TArray<TSharedPtr<FJsonValue>>();
			Changes.Add(Pair.Value);
			Node->SetArrayField(TEXT("property_changes"), MoveTemp(Changes));
		}
		const auto Document = MakeShared<FJsonObject>();
		if (!Query.EdGraph && !Desired->Values.IsEmpty() && !Desired->HasField(TEXT("node:root"))
			&& !AppendDefaultGraphNodes(Desired, Detail)) { Error.SetFailure(Detail); return false; }
		Document->SetObjectField(TEXT("nodes"), Desired);
		Document->SetArrayField(TEXT("connections"), MoveTemp(Connections));
		UeAgentAuthoringDiagnostics::FScopedSourceLocations SourceScope(&Document.Get(), [&Sources](const FString& Pointer) {
			return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Sources, Pointer);
		});
		if (!ValidateGraphDocument(MakeShared<FJsonValueObject>(Document), Detail))
		{
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Detail, *Document);
			return Forward(Detail, GraphUnit, true);
		}
		OutDocument = Document;
		return true;
	}
}
