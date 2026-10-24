#include "Domains/Deformer/Authoring/UeAgentDeformerAuthoringApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Domains/Deformer/Authoring/UeAgentDeformerAuthoring.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundDeformerProduction.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Dom/JsonValue.h"
#include "OptimusDeformer.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UeAgentDeformerAuthoring
{
	using namespace UE::UAI;
	using namespace UeAgentAuthoringOperationApply;
	using namespace Composition::GraphCompound;
	namespace
	{
		const FString ProviderId = TEXT("deformer.semantic.apply");
		const FString InputId = TEXT("uai.apply_input.deformer.semantic");
		const FString WorkspaceId = TEXT("uai.apply_workspace.deformer.semantic");

		bool Fail(FOperationFailure& Failure, const UE::UAI::Contract::FDiagnostic& Diagnostic)
		{ Failure.Diagnostic = Diagnostic; return false; }

		using FPropertyEdits = TMap<FString, TArray<TSharedPtr<FJsonValue>>>;
		using FPropertyIntents = TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>;

		Contract::FDiagnostic AuthoringFailure(const TCHAR* Code, const FString& Reason, const FString& Path,
			const TSharedPtr<FJsonValue>& Actual = nullptr, const TSharedPtr<FJsonValue>& Expected = nullptr)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Path;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			return Error;
		}

		void LocateWorkspaceFailure(const HjsonWorkspace::FResolvedSemanticWorkspace& Workspace,
			const FString& Unit, Contract::FDiagnostic& Error, const FString& FallbackPointer = FString())
		{
			if (Error.Source.SourceFile.IsEmpty() && Error.SourceKind != TEXT("native_result"))
			{
				if (Error.Source.SourceUnit.IsEmpty()) Error.Source.SourceUnit = Unit;
				if (Error.Source.InternalJsonPath.IsEmpty())
					Error.Source.InternalJsonPath = !Error.Source.JsonPath.IsEmpty() ? Error.Source.JsonPath
						: !Error.SemanticPointer.IsEmpty() ? Error.SemanticPointer : FallbackPointer;
				if (!Error.Source.InternalJsonPath.IsEmpty())
				{
					Error.Source.JsonPath.Reset();
					Workspace.LocateDocumentDiagnostic({}, {}, Error, Unit);
				}
			}
			if (Error.PublicCause)
			{
				auto Cause = *Error.PublicCause;
				LocateWorkspaceFailure(Workspace, Unit, Cause, Error.Source.InternalJsonPath);
				Error.PublicCause = MakeShared<Contract::FDiagnostic>(MoveTemp(Cause));
			}
			for (auto& Child : Error.Diagnostics)
				LocateWorkspaceFailure(Workspace, Unit, Child, Error.Source.InternalJsonPath);
		}

		bool ApplyProperties(UOptimusDeformer& Asset, const FString& Path, const FPropertyEdits& Edits,
			const FPropertyEdits& InlineEdits, const TMap<FString, FName>& NodeBindings,
			const FDeformerSemanticGraphEdit& GraphEdit, FPropertyIntents& OutIntents, FOperationFailure& Failure)
		{
			OutIntents.Reset();
			FPropertyEdits BoundEdits = Edits;
			for (const auto& Pair : InlineEdits)
			{
				const FName* Name = NodeBindings.Find(Pair.Key);
				if (!Name || Name->IsNone())
				{
					auto Error = AuthoringFailure(TEXT("deformer_property_owner_unresolved"),
						TEXT("The node owning the inline properties has no native binding."), Path);
					Error.ActualEvidence = MakeShared<FJsonValueString>(Pair.Key);
					if (const auto* Node = GraphEdit.Nodes.FindByPredicate([&Pair](const auto& Entry) { return Entry.Identity == Pair.Key; }))
					{
						Error.Source.SourceUnit = GraphEdit.SourceUnit;
						Error.Source.InternalJsonPath = Node->InputPointer + TEXT("/properties");
						Error.SourceKind = TEXT("related_input");
						if (GraphEdit.LocateDiagnostic) GraphEdit.LocateDiagnostic(Error);
					}
					return Fail(Failure, Error);
				}
				BoundEdits.FindOrAdd(Name->ToString()).Append(Pair.Value);
			}
			if (BoundEdits.IsEmpty()) return true;
			TMap<FString, TSharedPtr<FJsonObject>> Roots, Nodes;
			UE::UAI::Contract::FDiagnostic Error;
			for (const auto& Pair : BoundEdits)
			{
				TSharedPtr<FJsonObject> Document;
				if (!UeAgentDetailsAuthoringService::NormalizePropertyPayload(
					MakeShared<FJsonValueArray>(Pair.Value), Document, Error))
					return Fail(Failure, Error);
				if (Pair.Key.IsEmpty()) Roots.Add(Path, Document);
				else Nodes.Add(Pair.Key, Document);
			}
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			auto Options = [](const UeAgentAuthoringState::FReadRequest&) {
				UeAgentAuthoringObjectApply::FOptions Value; Value.bCompactValues = true; return Value;
			};
			return (BuildExportSnapshotReadRequests(Asset, Requests, Error, FPackageName::ObjectPathToPackageName(Path), Path)
				&& UeAgentDetailsAuthoringService::ApplyScopedProperties(Roots, Requests, TEXT("core.property"),
					TEXT("owner_path"), TEXT("deformer"), Options, Error, nullptr, {}, &OutIntents)
				&& UeAgentDetailsAuthoringService::ApplyScopedProperties(Nodes, Requests, NodePropertySemanticId,
					TEXT("node_id"), TEXT("deformer"), Options, Error, nullptr, {}, &OutIntents))
				|| Fail(Failure, Error);
		}

		void Release(TStrongObjectPtr<UOptimusDeformer>& Asset)
		{
			if (Asset.IsValid())
			{ Asset->ClearFlags(RF_Public | RF_Standalone); Asset->MarkAsGarbage(); Asset.Reset(); }
		}

		bool ValidatePlan(UOptimusDeformer& Asset, const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			FOperationFailure& Failure)
		{
			const FString Owner = Asset.GetOutermost()->GetName();
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UE::UAI::Contract::FDiagnostic Error;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!BuildExportSnapshotReadRequests(Asset, Requests, Error)
				|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				return Fail(Failure, Error.IsEmpty() ? Capture.Diagnostic : Error);
			auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
			StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
			HjsonWorkspace::FAuthoringModelPtr Model;
			HjsonWorkspace::FWorkspaceError ProjectionError;
			if (!HjsonWorkspace::TryBuildCanonicalAuthoringCapture(CompositionKind, TEXT("llm_authoring"),
				Capture.Snapshot, Resources, Snapshot, Model, ProjectionError))
				return Fail(Failure, ProjectionError.GetDiagnostic());
			for (const auto& Change : Plan->GetChangeSet().GetChanges())
			{
				const auto& Identity = Change.GetIdentity();
				const FString AssetPath = Identity.GetSelector(TEXT("asset_path"));
				if (!AssetPath.IsEmpty())
				{
					if (AssetPath != Owner) return Fail(Failure, AuthoringFailure(TEXT("deformer_semantic_plan_target_mismatch"),
						TEXT("The planned asset package does not match the Deformer package."), Asset.GetPathName(),
						MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(Owner)));
				}
				else if (!Identity.GetSelector(TEXT("owner_path")).IsEmpty())
				{
					if (!Identity.GetSelector(TEXT("owner_path")).Equals(Asset.GetPathName(), ESearchCase::CaseSensitive))
						return Fail(Failure, AuthoringFailure(TEXT("deformer_semantic_plan_target_mismatch"),
							TEXT("The planned object owner does not match the Deformer."), Asset.GetPathName(),
							MakeShared<FJsonValueString>(Identity.GetSelector(TEXT("owner_path"))), MakeShared<FJsonValueString>(Asset.GetPathName())));
				}
				else if (Identity.GetTargetKind() != TEXT("authoring_structural_edit")
					|| !Model.IsValid() || !Model->FindUnit(Identity.GetOwnerId()))
				{
					auto PlanError = AuthoringFailure(TEXT("deformer_semantic_plan_target_mismatch"),
						Identity.GetTargetKind() != TEXT("authoring_structural_edit") ? TEXT("The planned change has an unsupported target kind.")
							: !Model.IsValid() ? TEXT("The captured Deformer authoring model is unavailable.")
							: TEXT("The planned structural owner is absent from the captured Deformer model."), Asset.GetPathName());
					if (Identity.GetTargetKind() != TEXT("authoring_structural_edit"))
					{
						PlanError.ActualValue = MakeShared<FJsonValueString>(Identity.GetTargetKind());
						PlanError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("authoring_structural_edit"));
					}
					PlanError.SubjectId = Identity.GetOwnerId();
					return Fail(Failure, PlanError);
				}
			}
			if (!Snapshot.IsValid() || Snapshot->GetRevision() != Plan->GetObservedRevision())
			{
				auto PlanError = AuthoringFailure(TEXT("deformer_semantic_revision_conflict"),
					!Snapshot.IsValid() ? TEXT("The current Deformer snapshot is unavailable.")
						: TEXT("The Deformer changed after this edit plan was prepared."), Asset.GetPathName());
				PlanError.ExpectedEvidence = MakeShared<FJsonValueObject>(StateSemantic::FUeAgentStateRevisionJson::ToJson(Plan->GetObservedRevision()));
				if (Snapshot.IsValid()) PlanError.ActualEvidence = MakeShared<FJsonValueObject>(StateSemantic::FUeAgentStateRevisionJson::ToJson(Snapshot->GetRevision()));
				return Fail(Failure, PlanError);
			}
			return true;
		}

		class FGraphState final : public IPreparedOperationState
		{
		public:
			FGraphState(FString InPath, FDeformerSemanticGraphEdit InEdit,
				PlanningPreflight::FPlanningPreflightPlanRef InPlan, UOptimusDeformer* InActual, UOptimusDeformer* InProposed,
				FPropertyEdits InProperties, FPropertyEdits InInlineProperties, FPropertyIntents InProposedIntents)
				: Path(MoveTemp(InPath)), Edit(MoveTemp(InEdit)), Plan(MoveTemp(InPlan)), Actual(InActual), Proposed(InProposed),
				  Properties(MoveTemp(InProperties)), InlineProperties(MoveTemp(InInlineProperties)), ProposedIntents(MoveTemp(InProposedIntents)) {}
			~FGraphState() override { Release(Proposed); }
			FString GetTargetKey() const override { return Path; }
			UeAgentApplyInputContract::EOperation GetResolvedOperation() const override
			{ return UeAgentApplyInputContract::EOperation::Update; }
			void CollectRollbackObjects(TArray<UObject*>& Objects) const override
			{ if (Actual) Objects.Add(Actual); }
			bool CaptureRollback(FOperationFailure&) override { return true; }
			bool RequiresCheckpointRollback() const override { return true; }
			bool ApplyProposed(FOperationFailure&) override { return true; }
			bool BuildExpectedReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& R, FOperationFailure& E) const override
			{ return Read(Proposed.Get(), ProposedIntents, R, E); }
			bool BuildActualReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& R, FOperationFailure& E) const override
			{ return Read(Actual, ActualIntents, R, E); }
			bool ApplyLifecycle(FOperationFailure& Failure) override
			{
				if (!Actual) return Fail(Failure, AuthoringFailure(TEXT("deformer_actual_missing"), TEXT("The Deformer target is unavailable for apply."), Path));
				if (!ValidatePlan(*Actual, Plan, Failure)) return false;
				Actual->Modify();
				FGraphCompoundRuntimeError Error;
				TMap<FString, FName> NodeBindings;
				if (!TryApplyDeformerSemanticGraph(*Actual, Edit, NodeBindings, Error)) return Fail(Failure, Error);
				if (!ApplyProperties(*Actual, Path, Properties, InlineProperties, NodeBindings, Edit, ActualIntents, Failure)) return false;
				if (!TryApplyDeformerSemanticConnections(*Actual, Edit, NodeBindings, Error)) return Fail(Failure, Error);
				Actual->MarkPackageDirty();
				return true;
			}
			bool Compile(FOperationFailure& Failure) override
			{
				if (!Actual || !Actual->Compile()) return Fail(Failure, AuthoringFailure(TEXT("deformer_compile_failed"),
					!Actual ? TEXT("The Deformer target is unavailable for compilation.") : TEXT("The Deformer failed to compile after applying its edits."), Path));
				TArray<UeAgentAuthoringState::FReadRequest> Reads;
				return Read(Actual, ActualIntents, Reads, Failure);
			}
			bool Save(FOperationFailure&) override { return true; }
			bool BuildDurablePackagePlans(TArray<FPackageDurablePlan>& Plans, FOperationFailure& Failure) const override
			{ return BuildPackageDurablePlan(Path, FPackageName::ObjectPathToPackageName(Path), Actual,
				EPackageDurableIntent::Save, Plans, Failure); }
			bool Rollback(FOperationFailure& Failure) override
			{ return Fail(Failure, AuthoringFailure(TEXT("deformer_checkpoint_rollback_required"), TEXT("Deformer rollback requires restoring the transaction checkpoint."), Path)); }
			bool SupportsPersistentReadback() const override { return true; }
			bool ReleaseForPersistentReadback(FOperationFailure&) override
			{ Properties.Reset(); InlineProperties.Reset(); Actual = nullptr; Release(Proposed); return true; }
			bool RebindAfterPersistentReadback(FOperationFailure& Failure) override
			{ Actual = LoadObject<UOptimusDeformer>(nullptr, *Path);
				return Actual || Fail(Failure, AuthoringFailure(TEXT("deformer_reload_failed"), TEXT("The saved Deformer could not be reloaded."), Path)); }
		private:
			bool Read(UOptimusDeformer* Asset, const FPropertyIntents& Intents,
				TArray<UeAgentAuthoringState::FReadRequest>& R, FOperationFailure& E) const
			{
				if (!Asset) return Fail(E, AuthoringFailure(TEXT("deformer_readback_target_missing"), TEXT("The Deformer target is unavailable for readback."), Path));
				UE::UAI::Contract::FDiagnostic Error;
				return (BuildExportSnapshotReadRequests(*Asset, R, Error,
					FPackageName::ObjectPathToPackageName(Path), Path)
					&& UeAgentDetailsAuthoringService::VerifyScopedPropertyIntents(Intents, R, Error))
					|| Fail(E, Error);
			}
			FString Path;
			FDeformerSemanticGraphEdit Edit;
			PlanningPreflight::FPlanningPreflightPlanRef Plan;
			UOptimusDeformer* Actual = nullptr;
			TStrongObjectPtr<UOptimusDeformer> Proposed;
			FPropertyEdits Properties, InlineProperties;
			FPropertyIntents ProposedIntents, ActualIntents;
		};

		class FGraphProvider final : public IApplyProvider
		{
		public:
			FGraphProvider(FString InPath, FDeformerSemanticGraphEdit InEdit, PlanningPreflight::FPlanningPreflightPlanRef InPlan,
				FPropertyEdits InProperties, FPropertyEdits InInlineProperties)
				: Path(MoveTemp(InPath)), Edit(MoveTemp(InEdit)), Plan(MoveTemp(InPlan)),
				  Properties(MoveTemp(InProperties)), InlineProperties(MoveTemp(InInlineProperties)) {}
			FString GetProviderId() const override { return ProviderId; }
			bool PrepareOperation(const FPrepareRequest& Input, TUniquePtr<IPreparedOperationState>& Out,
				FOperationFailure& Failure) const override
			{
				UOptimusDeformer* Actual = LoadObject<UOptimusDeformer>(nullptr, *Path);
				if (!Input.IsValid() || !Actual || !Actual->GetUpdateGraph())
					return Fail(Failure, AuthoringFailure(TEXT("deformer_prepare_target_invalid"),
						!Input.Contract ? TEXT("The Deformer prepare request has no input contract.")
							: !Input.Unit ? TEXT("The Deformer prepare request has no workspace unit.")
							: !Input.Input ? TEXT("The Deformer prepare request has no operation input.")
							: !Actual ? TEXT("The Deformer target could not be loaded for preparation.")
							: TEXT("The Deformer update graph is unavailable for preparation."), Path));
				if (!ValidatePlan(*Actual, Plan, Failure)) return false;
				TStrongObjectPtr<UOptimusDeformer> Proposed(DuplicateObject<UOptimusDeformer>(Actual, GetTransientPackage()));
				if (!Proposed.IsValid()) return Fail(Failure, AuthoringFailure(TEXT("deformer_prepare_copy_failed"), TEXT("A temporary Deformer copy could not be created for preparation."), Path));
				Proposed->ClearFlags(RF_Public | RF_Standalone);
				FGraphCompoundRuntimeError Error;
				TMap<FString, FName> NodeBindings;
				if (!TryApplyDeformerSemanticGraph(*Proposed, Edit, NodeBindings, Error))
				{ Release(Proposed); return Fail(Failure, Error); }
				FPropertyIntents ProposedIntents;
				if (!ApplyProperties(*Proposed, Path, Properties, InlineProperties, NodeBindings, Edit, ProposedIntents, Failure))
				{ Release(Proposed); return false; }
				if (!TryApplyDeformerSemanticConnections(*Proposed, Edit, NodeBindings, Error))
				{ Release(Proposed); return Fail(Failure, Error); }
				Out = MakeUnique<FGraphState>(Path, Edit, Plan, Actual, Proposed.Get(), Properties, InlineProperties, MoveTemp(ProposedIntents));
				Proposed.Reset();
				return true;
			}
		private:
			FString Path;
			FDeformerSemanticGraphEdit Edit;
			PlanningPreflight::FPlanningPreflightPlanRef Plan;
			FPropertyEdits Properties, InlineProperties;
		};

		bool BuildEdit(const HjsonWorkspace::FAuthoringModel& Model, UOptimusDeformer& Asset,
			FDeformerSemanticGraphEdit& Edit, FPropertyEdits& InlineProperties,
			HjsonWorkspace::FResolvedSemanticWorkspaceRef Workspace, UE::UAI::Contract::FDiagnostic& Error)
		{
			Edit.TargetPath = Asset.GetPathName();
			TSharedPtr<FJsonObject> Graph;
			FString GraphUnitId;
			for (const auto& Unit : Model.GetUnits())
			{
				if (Unit.GetSemanticTypeRef() != GraphSemanticId) continue;
				GraphUnitId = Unit.GetUnitId();
				Graph = MakeShared<FJsonObject>();
				for (const auto& Field : Unit.GetValues())
				{
					TSharedPtr<FJsonValue> Value;
					if (!StateSemantic::CanonicalJson::TryToJsonValue(Field.Value.Get(), Value, Error))
					{
						LocateWorkspaceFailure(Workspace.Get(), GraphUnitId, Error,
							TEXT("/") + HjsonWorkspace::EscapeJsonPointerSegment(Field.Key));
						if (Error.TargetPath.IsEmpty()) Error.TargetPath = Edit.TargetPath;
						return false;
					}
					Graph->SetField(Field.Key, Value);
				}
				break;
			}
			const TSharedPtr<FJsonObject>* Nodes = nullptr;
			const TSharedPtr<FJsonObject>* Edges = nullptr;
			Edit.SourceUnit = GraphUnitId;
			Edit.LocateDiagnostic = [Workspace, GraphUnitId](Contract::FDiagnostic& Issue) {
				LocateWorkspaceFailure(Workspace.Get(), GraphUnitId, Issue);
			};
			auto Reject = [&](const TCHAR* Code, const FString& Reason, const FString& Pointer,
				const TSharedPtr<FJsonValue>& Actual, const FString& Expected) {
				Error = AuthoringFailure(Code, Reason, Edit.TargetPath, Actual, MakeShared<FJsonValueString>(Expected));
				Error.Stage = TEXT("authoring");
				Error.SourceKind = TEXT("command_input");
				Error.ActualValuePresent = Actual.IsValid();
				Error.Source.SourceUnit = GraphUnitId;
				Error.Source.InternalJsonPath = Pointer;
				Edit.LocateDiagnostic(Error);
				return false;
			};
			if (!Graph.IsValid()) return Reject(TEXT("deformer_semantic_graph_invalid"),
				TEXT("The authoring model contains no Deformer graph unit."), FString(), nullptr, TEXT("Deformer graph unit"));
			if (!Graph->TryGetObjectField(TEXT("nodes"), Nodes)) return Reject(TEXT("deformer_semantic_graph_invalid"),
				TEXT("The Deformer graph 'nodes' value must be an object."), TEXT("/nodes"), Graph->TryGetField(TEXT("nodes")), TEXT("object"));
			if (!Graph->TryGetObjectField(TEXT("edges"), Edges)) return Reject(TEXT("deformer_semantic_graph_invalid"),
				TEXT("The Deformer graph 'edges' value must be an object."), TEXT("/edges"), Graph->TryGetField(TEXT("edges")), TEXT("object"));
			if (!Asset.GetUpdateGraph())
			{
				Error = AuthoringFailure(TEXT("deformer_semantic_graph_invalid"), TEXT("The native Deformer update graph is unavailable."), Edit.TargetPath);
				return false;
			}
			auto Connections = MakeShared<HjsonWorkspace::FGraphAuthoringConnectionContext>();
			Connections->bHasResolvedIntent = true;
			Edit.ConnectionContext = Connections;
			for (const auto& Edge : (*Edges)->Values)
			{
				const FString EdgePointer = TEXT("/edges/") + HjsonWorkspace::EscapeJsonPointerSegment(Edge.Key);
				if (!Edge.Value.IsValid() || Edge.Value->Type != EJson::Object)
					return Reject(TEXT("deformer_semantic_connection_invalid"), TEXT("A connection entry must be an object."), EdgePointer, Edge.Value, TEXT("object"));
				const auto Value = Edge.Value->AsObject();
				FString Operation;
				Value->TryGetStringField(TEXT("operation"), Operation);
				if (Operation == TEXT("remove"))
				{
					const TSharedPtr<FJsonObject>* From = nullptr;
					const TSharedPtr<FJsonObject>* To = nullptr;
					FDeformerSemanticGraphEdit::FConnection Removed;
					Removed.InputPointer = EdgePointer;
					if (Value->TryGetObjectField(TEXT("from"), From) && Value->TryGetObjectField(TEXT("to"), To)
						&& (*From)->TryGetStringField(TEXT("node"), Removed.FromNode) && (*From)->TryGetStringField(TEXT("pin"), Removed.FromPin)
						&& (*To)->TryGetStringField(TEXT("node"), Removed.ToNode) && (*To)->TryGetStringField(TEXT("pin"), Removed.ToPin))
						Edit.RemovedConnections.Add(MoveTemp(Removed));
					continue;
				}
				if (Operation != TEXT("create") && Operation != TEXT("update"))
					return Reject(TEXT("deformer_semantic_connection_operation_invalid"), TEXT("The connection operation is unsupported."),
						EdgePointer + TEXT("/operation"), Value->TryGetField(TEXT("operation")), TEXT("create, update or remove"));
				const TSharedPtr<FJsonObject>* From = nullptr;
				const TSharedPtr<FJsonObject>* To = nullptr;
				FDeformerSemanticGraphEdit::FConnection Connection;
				Connection.InputPointer = EdgePointer;
				if (!Value->TryGetObjectField(TEXT("from"), From)) return Reject(TEXT("deformer_semantic_connection_endpoint_missing"),
					TEXT("The connection 'from' endpoint must be an object."), EdgePointer + TEXT("/from"), Value->TryGetField(TEXT("from")), TEXT("object"));
				if (!Value->TryGetObjectField(TEXT("to"), To)) return Reject(TEXT("deformer_semantic_connection_endpoint_missing"),
					TEXT("The connection 'to' endpoint must be an object."), EdgePointer + TEXT("/to"), Value->TryGetField(TEXT("to")), TEXT("object"));
				auto ReadEndpoint = [&](const FJsonObject& Object, const TCHAR* Field, const TCHAR* Side, FString& Out) {
					return Object.TryGetStringField(Field, Out) || Reject(TEXT("deformer_semantic_connection_endpoint_missing"),
						FString::Printf(TEXT("Connection field '%s.%s' must be a string."), Side, Field),
						EdgePointer + TEXT("/") + Side + TEXT("/") + Field, Object.TryGetField(Field), TEXT("string"));
				};
				if (!ReadEndpoint(**From, TEXT("node"), TEXT("from"), Connection.FromNode)
					|| !ReadEndpoint(**From, TEXT("pin"), TEXT("from"), Connection.FromPin)
					|| !ReadEndpoint(**To, TEXT("node"), TEXT("to"), Connection.ToNode)
					|| !ReadEndpoint(**To, TEXT("pin"), TEXT("to"), Connection.ToPin)) return false;
				if (Operation == TEXT("update"))
					Connections->RecordUnchanged(Connection.FromNode, Connection.FromPin, Connection.ToNode, Connection.ToPin);
				Edit.Connections.Add(MoveTemp(Connection));
			}
			for (const auto& Item : (*Nodes)->Values)
			{
				const FString NodePointer = TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Item.Key);
				if (!Item.Value.IsValid() || Item.Value->Type != EJson::Object)
					return Reject(TEXT("deformer_semantic_node_invalid"), TEXT("A Deformer node entry must be an object."), NodePointer, Item.Value, TEXT("object"));
				const auto Node = Item.Value->AsObject();
				const TSharedPtr<FJsonObject>* Inputs = nullptr;
				if (Node->HasField(TEXT("inputs")) && (!Node->TryGetObjectField(TEXT("inputs"), Inputs)
					|| !(*Inputs)->Values.IsEmpty()))
					return Reject(TEXT("deformer_semantic_pin_value_edit_not_supported"), TEXT("Deformer pin value edits are unsupported in this graph operation."),
						NodePointer + TEXT("/inputs"), Node->TryGetField(TEXT("inputs")), TEXT("absent or empty inputs object"));
				FDeformerSemanticNodeEdit Entry;
				Entry.InputPointer = NodePointer;
				if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Node,
					{{TEXT("pos"), EJson::Object}}, Error, TEXT("deformer_node")))
				{
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, NodePointer);
					LocateWorkspaceFailure(Workspace.Get(), GraphUnitId, Error, NodePointer);
					return false;
				}
				Node->TryGetStringField(TEXT("id"), Entry.Identity);
				FString Operation;
				Node->TryGetStringField(TEXT("operation"), Operation);
				if (Operation == TEXT("create"))
				{
					const TSharedPtr<FJsonObject>* Candidate = nullptr;
					if (!Node->TryGetObjectField(TEXT("candidate"), Candidate))
						return Reject(TEXT("deformer_candidate_missing"), TEXT("A new Deformer node requires a candidate object."),
							NodePointer + TEXT("/candidate"), Node->TryGetField(TEXT("candidate")), TEXT("candidate object"));
					HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
					HjsonWorkspace::FWorkspaceError SelectorError;
					if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(*Candidate,
						NodePointer + TEXT("/candidate"), Selector, SelectorError))
					{ Error = SelectorError.GetDiagnostic(); LocateWorkspaceFailure(Workspace.Get(), GraphUnitId, Error, NodePointer + TEXT("/candidate")); return false; }
					if (!Selector.Select.IsEmpty())
						return Reject(TEXT("deformer_candidate_select_not_supported"), TEXT("Deformer node candidates do not support 'select' qualifiers."),
							NodePointer + TEXT("/candidate/select"), (*Candidate)->TryGetField(TEXT("select")), TEXT("absent or empty select"));
					for (UClass* Class : UOptimusNode::GetAllNodeClasses())
					{
						const auto* Default = Class->GetDefaultObject<UOptimusNode>();
						if (!Default || !Default->GetDisplayName().ToString().Equals(Selector.Name, ESearchCase::CaseSensitive)) continue;
						if (Entry.CreateClass)
						{
							Reject(TEXT("deformer_candidate_ambiguous"), TEXT("The candidate name matches more than one Deformer node class."),
								NodePointer + TEXT("/candidate/name"), MakeShared<FJsonValueString>(Selector.Name), TEXT("unique candidate name"));
							TArray<TSharedPtr<FJsonValue>> Choices = {MakeShared<FJsonValueString>(Entry.CreateClass->GetPathName()), MakeShared<FJsonValueString>(Class->GetPathName())};
							Error.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Choices));
							return false;
						}
						Entry.CreateClass = Class;
					}
					if (!Entry.CreateClass) return Reject(TEXT("deformer_candidate_unknown"), TEXT("The candidate name matches no registered Deformer node class."),
						NodePointer + TEXT("/candidate/name"), MakeShared<FJsonValueString>(Selector.Name), TEXT("registered Deformer node candidate name"));
				}
				else if (Operation == TEXT("update") || Operation == TEXT("remove"))
				{
					FString Name;
					if (!Node->TryGetStringField(TEXT("node_name"), Name) || Name.IsEmpty())
						return Reject(TEXT("deformer_existing_node_identity_missing"), TEXT("Updating or removing a node requires a non-empty 'node_name'."),
							NodePointer + TEXT("/node_name"), Node->TryGetField(TEXT("node_name")), TEXT("non-empty node name"));
					Entry.ExistingName = FName(*Name);
					Entry.bRemove = Operation == TEXT("remove");
					if (Node->HasField(TEXT("candidate")))
						return Reject(TEXT("deformer_node_replacement_not_supported"), TEXT("An existing node cannot be replaced by supplying a candidate."),
							NodePointer + TEXT("/candidate"), Node->TryGetField(TEXT("candidate")), TEXT("no candidate on an update or remove operation"));
				}
				else return Reject(TEXT("deformer_node_operation_invalid"), TEXT("The Deformer node operation is unsupported."),
					NodePointer + TEXT("/operation"), Node->TryGetField(TEXT("operation")), TEXT("create, update or remove"));
				const TSharedPtr<FJsonObject>* Position = nullptr;
				if (Node->TryGetObjectField(TEXT("pos"), Position))
				{
					const bool bX = (*Position)->TryGetNumberField(TEXT("x"), Entry.Position.X);
					const bool bY = bX && (*Position)->TryGetNumberField(TEXT("y"), Entry.Position.Y);
					if (!bX || !bY || !FMath::IsFinite(Entry.Position.X) || !FMath::IsFinite(Entry.Position.Y))
					{
						const TCHAR* Axis = !bX || (bY && !FMath::IsFinite(Entry.Position.X)) ? TEXT("x") : TEXT("y");
						return Reject(TEXT("deformer_node_position_invalid"), FString::Printf(TEXT("Node position '%s' must be a finite number."), Axis),
							NodePointer + TEXT("/pos/") + Axis, (*Position)->TryGetField(Axis), TEXT("finite number"));
					}
					Entry.bSetPosition = true;
				}
				Workspace->AttachInlinePropertySources(GraphUnitId, NodePointer, *Node);
				TSharedPtr<FJsonObject> Properties;
				if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Node, TEXT("properties"), Properties, Error))
				{
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, NodePointer + TEXT("/properties"));
					LocateWorkspaceFailure(Workspace.Get(), GraphUnitId, Error, NodePointer + TEXT("/properties"));
					return false;
				}
				if (Properties && !Properties->GetArrayField(TEXT("properties")).IsEmpty())
				{
					if (Entry.bRemove) return Reject(TEXT("deformer_property_owner_removed"), TEXT("Properties cannot be applied to a node that is being removed."),
						NodePointer + TEXT("/properties"), Node->TryGetField(TEXT("properties")), TEXT("no property edits on a removed node"));
					InlineProperties.FindOrAdd(Entry.Identity).Append(Properties->GetArrayField(TEXT("properties")));
				}
				Edit.Nodes.Add(MoveTemp(Entry));
			}
			return true;
		}

		UeAgentApplyInputContract::FContract MakeInputContract()
		{
			using namespace UeAgentApplyInputContract;
			FContract Result;
			Result.Schema = TEXT("uai.apply_input_contract.v1");
			Result.ContractId = InputId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("deformer.semantic");
			Result.TargetKind = TargetKind;
			Result.SemanticDescriptorRefs = {GraphSemanticId};
			Result.ReflectionBaseline.RootKinds = {TEXT("uobject"), TEXT("ustruct")};
			Result.ReflectionBaseline.RecursiveNodes = {TEXT("scalar"), TEXT("struct"), TEXT("owned_object"),
				TEXT("reference"), TEXT("array"), TEXT("set"), TEXT("map"), TEXT("fixed_array"), TEXT("optional")};
			Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
			Result.ReflectionBaseline.ExternalReferencePolicy = TEXT("replace_reference_only");
			Result.ReflectionBaseline.OwnedObjectPolicy = TEXT("create_or_locate_with_real_outer_then_recurse");
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
			Result.InputFields = {
				{TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path"), {EOperation::Update}},
				{TEXT("operation"), TEXT("operation_control"), TEXT("apply_operation"), {EOperation::Update}},
				{TEXT("plan_fingerprint"), TEXT("provider_extension"), TEXT("string")}};
			FOperationDescriptor Operation;
			Operation.Operation = EOperation::Update;
			Operation.IdentityOneOf = {{TEXT("asset_path")}};
			Operation.AcceptedSections = {TEXT("target"), TEXT("provider_extension")};
			Operation.OmittedFieldPolicy = TEXT("preserve_current");
			Operation.NativeDefaultPolicy = TEXT("forbidden");
			Result.Operations = {Operation};
			Result.ProviderId = ProviderId;
			Result.UnknownInputFieldPolicy = TEXT("reject");
			Result.Verification.StateSource = TEXT("shared_state_snapshot");
			Result.Verification.CompareSource = TEXT("shared_semantic_compare");
			return Result;
		}

		UeAgentApplyWorkspaceContract::FContract MakeWorkspaceContract()
		{
			using namespace UeAgentApplyWorkspaceContract;
			UeAgentApplyWorkspaceContract::FContract Result;
			Result.Schema = TEXT("uai.apply_workspace_contract.v1");
			Result.ContractId = WorkspaceId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("deformer.semantic");
			Result.Sources = {
				{ESourceKind::Directory, TEXT("physical"), TEXT("index")},
				{ESourceKind::SingleFile, TEXT("virtual"), TEXT("object")},
				{ESourceKind::Inline, TEXT("virtual"), TEXT("object")}};
			Result.Entry.DirectoryEntry = TEXT("index.hjson");
			Result.Entry.VirtualUnitId = TEXT("graph");
			FUnitDescriptor Unit;
			Unit.UnitId = TEXT("graph");
			Unit.PathPattern = TEXT("index.hjson");
			Unit.Role = TEXT("object");
			Unit.Cardinality = TEXT("one");
			Unit.bRequired = true;
			Unit.RouteId = TEXT("deformer.semantic");
			Unit.ApplyContractRef = InputId;
			Unit.MergePolicy = TEXT("replace");
			Unit.AbsentSemantics = TEXT("error");
			Unit.EmptySemantics = TEXT("error");
			Result.Units = {Unit};
			Result.ReferencePolicy.Base = TEXT("workspace_root");
			Result.ReferencePolicy.RootContainment = TEXT("required");
			Result.ReferencePolicy.ExternalReferences = TEXT("reject");
			Result.ReferencePolicy.CaseCollision = TEXT("reject");
			Result.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
			Result.DependencyPolicy.CyclePolicy = TEXT("reject");
			Result.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
			Result.DependencyPolicy.Order = TEXT("stable_topological");
			Result.Preflight.Scope = TEXT("whole_workspace");
			Result.Preflight.MutationBeforeSuccess = TEXT("forbid");
			Result.Preflight.bDiagnosticsIncludeSource = true;
			Result.Transaction.Mode = TEXT("atomic");
			Result.Transaction.RollbackOn = {TEXT("mutation_failed"), TEXT("compile_failed"),
				TEXT("save_failed"), TEXT("verification_failed")};
			Result.Execution.Compile = TEXT("provider_hook");
			Result.Execution.Save = TEXT("caller_controlled");
			Result.Execution.Verification = TEXT("shared_snapshot_required");
			Result.UnknownFilePolicy = TEXT("reject");
			Result.DuplicateTargetPolicy = TEXT("reject");
			return Result;
		}
	}

	bool ApplySemanticWorkspace(const FString& TargetPath, const FString& RequestId,
		const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		const FBackendOptions& Options, FApplyWorkspaceResult& OutResult, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UOptimusDeformer* Asset = LoadObject<UOptimusDeformer>(nullptr, *TargetPath);
		FDeformerSemanticGraphEdit Edit;
		if (!Prepared.IsValid())
		{
			OutError = Prepared.GetValidationDiagnostic();
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetPath;
			return false;
		}
		if (!Asset || RequestId.IsEmpty())
		{
			OutError = AuthoringFailure(TEXT("deformer_semantic_request_invalid"),
				!Asset ? TEXT("The Deformer target could not be loaded for semantic apply.")
					: TEXT("Semantic apply requires a non-empty request identifier."), TargetPath);
			return false;
		}
		const auto& Plan = Prepared.GetPreflightResult().GetPlan();
		if (!Plan.IsValid()) { OutError = AuthoringFailure(TEXT("deformer_semantic_plan_missing"), TEXT("The prepared Deformer import has no execution plan."), TargetPath); return false; }
		FPropertyEdits InlineProperties;
		if (!BuildEdit(Prepared.GetWorkspace()->GetModel().Get(), *Asset, Edit, InlineProperties,
			Prepared.GetWorkspace(), OutError)) return false;
		FPropertyEdits Properties;
		if (!Projection.RootObjectProperties.IsEmpty()) Properties.Add(FString(), Projection.RootObjectProperties);
		for (const auto& Pair : Projection.PropertyChangesByUnitId)
		{
			const auto* Unit = Prepared.GetWorkspace()->GetModel()->FindUnit(Pair.Key);
			if (!Unit)
			{
				OutError = AuthoringFailure(TEXT("deformer_property_unit_missing"), TEXT("A projected property change refers to a unit absent from the authoring model."), TargetPath);
				OutError.SubjectId = Pair.Key;
				return false;
			}
			const auto* Change = Prepared.GetChangeSetDraft().GetChanges().FindByPredicate([Unit](const auto& Candidate)
			{
				return Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetCanonicalKey()
					|| Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetSelector(HjsonWorkspace::CanonicalAuthoringTargetIdentitySelector);
			});
			if (!Change)
			{
				OutError = AuthoringFailure(TEXT("deformer_property_change_missing"), TEXT("The projected property unit has no matching change in the prepared plan."), TargetPath);
				OutError.SubjectId = Pair.Key;
				return false;
			}
			const FString NodeId = Change->GetIdentity().GetSelector(TEXT("node_id"));
			if (NodeId.IsEmpty()) continue; // Root descriptors above are the same projection.
			if (Edit.Nodes.ContainsByPredicate([&NodeId](const auto& Node) { return Node.ExistingName.ToString() == NodeId && Node.bRemove; }))
			{
				OutError = AuthoringFailure(TEXT("deformer_property_owner_removed"), TEXT("Properties cannot be applied to a node that is being removed."),
					TargetPath, MakeShared<FJsonValueString>(NodeId));
				OutError.SourceKind = TEXT("related_input");
				LocateWorkspaceFailure(Prepared.GetWorkspace().Get(), Unit->GetUnitId(), OutError, TEXT("/"));
				return false;
			}
			Properties.FindOrAdd(NodeId).Add(Pair.Value);
		}
		const auto WorkspaceContract = MakeWorkspaceContract();
		UeAgentApplyInputContract::ERegistrationResult Registered;
		UeAgentAuthoringOperationApply::ERegistrationResult ProviderRegistered;
		FProviderRegistry Providers;
		if (!RegisterAuthoring(OutError)
			|| !UeAgentApplyInputContract::FRegistry::Get().RegisterContract(MakeInputContract(), Registered, OutError)
			|| !Providers.RegisterProvider(MakeShared<FGraphProvider>(TargetPath, MoveTemp(Edit), Plan.ToSharedRef(),
				MoveTemp(Properties), MoveTemp(InlineProperties)), ProviderRegistered, OutError))
			return false;
		const auto Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("asset_path"), TargetPath);
		const auto Extension = MakeShared<FJsonObject>();
		Extension->SetStringField(TEXT("plan_fingerprint"), Prepared.GetWorkspace()->GetModel()->GetSemanticFingerprint());
		const auto Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("operation"), TEXT("update"));
		Document->SetObjectField(TEXT("target"), Target);
		Document->SetObjectField(TEXT("provider_extension"), Extension);
		UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
		if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(WorkspaceContract,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(Document, TEXT("inline://deformer-semantic")), Load))
		{ OutError = Load.Diagnostic; return false; }
		Asset = nullptr;
		UeAgentAuthoringAssetWorkspaceApply::FFailure Failure;
		const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(WorkspaceContract,
			Load.Workspace, Providers, RequestId, true, Options, OutResult, Failure);
		if (!bApplied) OutError = Failure.Diagnostic;
		return bApplied;
	}
}
