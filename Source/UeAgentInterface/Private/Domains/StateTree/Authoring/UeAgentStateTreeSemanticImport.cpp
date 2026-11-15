#include "Domains/StateTree/Authoring/UeAgentStateTreeDocument.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeNodeInitialization.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorTypes.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreePropertyFunctionBase.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "UObject/StrongObjectPtr.h"

namespace UeAgentStateTreeAuthoring
{
	namespace
	{
		bool BuildNewItem(UStateTree& Target, const FString& Key, const TSharedPtr<FJsonObject>& Source,
			TSharedPtr<FJsonObject>& Item, TSharedPtr<FJsonObject>& Parameters, UE::UAI::HjsonWorkspace::FWorkspaceError& Error,
			const UE::UAI::HjsonWorkspace::FHjsonNode* InitialCategorySource)
		{
			using namespace UE::UAI;
			const FString RowPointer = TEXT("/tree_items/") + HjsonWorkspace::EscapeJsonPointerSegment(Key);
			const FString CandidatePointer = RowPointer + TEXT("/candidate");
			auto Fail = [&](const FString& Code, const FString& Field, const FString& Reason)
			{
				auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("document_restore"));
				Diagnostic.TargetPath = Target.GetPathName();
				Diagnostic.ActualValue = Source->TryGetField(Field);
				Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
				Error.SetFailure(MoveTemp(Diagnostic), FString(), RowPointer + TEXT("/") + Field);
				return false;
			};
			const TSharedPtr<FJsonObject>* Spec = nullptr;
			HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
			Source->TryGetObjectField(TEXT("candidate"), Spec);
			if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(Spec ? *Spec : TSharedPtr<FJsonObject>(),
					CandidatePointer, Selector, Error)) return false;
			QueryOwners::AiBehavior::FAiBehaviorCandidate Candidate;
			QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError Failure;
			QueryOwners::AiBehavior::FAiBehaviorCandidateContext Context;
			FString Placement, Parent;
			Source->TryGetStringField(TEXT("placement"), Placement);
			if (QueryOwners::AiBehavior::IsStateTreeNodePlacement(Placement)) {
				const bool bGlobal = Placement == TEXT("global_task") || Placement == TEXT("evaluator");
				// Existing parent references are normalized by the keyed document.
				// A same-request new parent is validated on the proposed tree.
				if (bGlobal || (Source->TryGetStringField(TEXT("parent"), Parent) && FGuid::Parse(Parent, Context.StateTreeParentId)))
					Context.AttachmentKind = Placement;
			}
			auto InitialSelection = Selector.Select;
			if (Placement == TEXT("property_function")) InitialSelection.Remove(TEXT("output"));
			if (!QueryOwners::AiBehavior::TryResolveAiBehaviorCandidate(Target, Selector.Name, FString(), Candidate, Failure, InitialSelection, Context))
			{ auto Detail = UeAgentAuthoringDiagnostics::CandidateFailure(Failure.Code, Selector.Name, Target.GetPathName(), TEXT("StateTree"), TEXT("ai.behavior.candidates"), Failure);
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Detail, CandidatePointer);
				Error.SetFailure(Detail); return false; }
			if (!Candidate.bSupported)
			{
				auto Detail = Contract::FDiagnostic::WrapFailure(TEXT("state_tree_candidate_unsupported"), Candidate.SupportDiagnostic);
				if (Detail.TargetPath.IsEmpty()) Detail.TargetPath = Target.GetPathName();
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Detail, CandidatePointer);
				Error.SetFailure(MoveTemp(Detail));
				return false;
			}
			if (Candidate.Kind == TEXT("state_tree_binding"))
			{
				if (Source->HasField(TEXT("categories"))) return Fail(TEXT("state_tree_binding_has_no_initial_properties"), TEXT("categories"), TEXT("Binding candidates do not accept initial property categories."));
				Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("id"), TEXT("new_binding:") + Key);
				Item->SetStringField(TEXT("kind"), TEXT("binding"));
				Item->SetStringField(TEXT("label"), TEXT("Binding"));
				Item->SetNumberField(TEXT("index"), 0);
				UeAgentPropertyValueCodec::FOptions Options;
				const FPropertyBindingPath EmptyPath;
				for (const TCHAR* Side : {TEXT("source"), TEXT("target")})
				{
					if (!Source->HasField(Side)) return Fail(TEXT("state_tree_binding_endpoint_required"), Side, TEXT("A StateTree binding requires both source and target endpoints."));
					Item->SetStringField(Side, TEXT(""));
					Item->SetStringField(FString(Side) + TEXT("_path"), TEXT(""));
					Item->SetField(FString(Side) + TEXT("_path_state"), UeAgentPropertyValueCodec::EncodeStructValue(
						FPropertyBindingPath::StaticStruct(), &EmptyPath, nullptr, Options));
				}
				return true;
			}
			const TSharedPtr<FJsonObject>* Categories = nullptr;
			if (Source->HasField(TEXT("categories")) && !Source->TryGetObjectField(TEXT("categories"), Categories))
			{ return Fail(TEXT("hjson_initial_categories_invalid"), TEXT("categories"), TEXT("Initial property categories must be an object.")); }
			return BuildTreeItemDefaults(Target, Key, Candidate.Kind,
				LoadObject<UStruct>(nullptr, *Candidate.PayloadType), Placement, Item, Parameters, Error,
				Categories ? *Categories : nullptr, InitialCategorySource);
		}
	}

	bool BuildTreeItemDefaults(UStateTree& Target, const FString& Key, const FString& Kind,
		const UStruct* Type, FString Placement, TSharedPtr<FJsonObject>& Item,
		TSharedPtr<FJsonObject>& Parameters, UE::UAI::HjsonWorkspace::FWorkspaceError& Error,
		const TSharedPtr<FJsonObject>& InitialCategories, const UE::UAI::HjsonWorkspace::FHjsonNode* InitialCategorySource)
	{
			// Read defaults from native objects through the same capture used for the
			// target. The temporary tree never writes or saves the target asset.
			TStrongObjectPtr<UStateTree> Template(NewObject<UStateTree>(GetTransientPackage()));
			auto* Data = NewObject<UStateTreeEditorData>(Template.Get()); Template->EditorData = Data;
			Data->Schema = DuplicateObject<UStateTreeSchema>(CastChecked<UStateTreeEditorData>(Target.EditorData)->Schema, Data);
			auto& Root = Data->AddSubTree(FName(*Key));
			FGuid ItemId = Root.ID;
			UE::UAI::Contract::FDiagnostic Detail;
			auto RejectCandidate = [&](const FString& Code, const FString& Reason, const FString& Field,
				const TSharedPtr<FJsonValue>& Actual, const FString& Expected) {
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("document_restore"));
				Diagnostic.TargetPath = Target.GetPathName();
				Diagnostic.ActualValue = Actual;
				Diagnostic.ActualValuePresent = Actual.IsValid();
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
				Error.SetFailure(MoveTemp(Diagnostic), FString(), TEXT("/tree_items/")
					+ UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Key) + TEXT("/") + Field);
				return false;
			};
			if (Kind == TEXT("state_tree_color"))
			{
				FStateTreeEditorColor Color;
				Color.DisplayName = Key;
				ItemId = Color.ColorRef.ID;
				Data->Colors.Add(Color);
			}
			else if (Kind == TEXT("state_tree_transition"))
				ItemId = Root.AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::Succeeded).ID;
			else if (Kind == TEXT("state_tree_property_function"))
			{
				const auto* Struct = Cast<UScriptStruct>(Type);
				if (!Struct || !Struct->IsChildOf(FStateTreePropertyFunctionBase::StaticStruct()) || !Data->Schema->IsStructAllowed(Struct)
					|| (!Placement.IsEmpty() && Placement != TEXT("property_function")))
				{ return RejectCandidate(TEXT("state_tree_function_candidate_invalid"),
					!Struct || !Struct->IsChildOf(FStateTreePropertyFunctionBase::StaticStruct())
						? TEXT("The selected candidate is not a StateTree property-function struct.")
						: !Data->Schema->IsStructAllowed(Struct) ? TEXT("The StateTree schema does not allow this property-function type.")
						: TEXT("A property-function candidate requires property_function placement."),
					!Placement.IsEmpty() && Placement != TEXT("property_function") ? TEXT("placement") : TEXT("candidate"),
					MakeShared<FJsonValueString>(!Placement.IsEmpty() && Placement != TEXT("property_function") ? Placement : GetPathNameSafe(Type)),
					TEXT("schema-supported property-function type with property_function placement")); }
				ItemId = Data->EditorBindings.AddFunctionBinding(Struct, {}, FPropertyBindingPath(Root.Parameters.ID)).GetStructID();
			}
			else if (Kind != TEXT("state_tree_state"))
			{
				if (Placement.IsEmpty())
					Placement = Kind == TEXT("state_tree_task") ? TEXT("task")
						: Kind == TEXT("state_tree_condition") ? TEXT("enter_condition")
						: Kind == TEXT("state_tree_evaluator") ? TEXT("evaluator") : TEXT("consideration");
				const bool ValidPlacement = (Kind == TEXT("state_tree_task")
					&& (Placement == TEXT("task") || Placement == TEXT("single_task") || Placement == TEXT("global_task")))
					|| (Kind == TEXT("state_tree_condition") && (Placement == TEXT("enter_condition") || Placement == TEXT("transition_condition")))
					|| (Kind == TEXT("state_tree_evaluator") && Placement == TEXT("evaluator"))
					|| (Kind == TEXT("state_tree_consideration") && Placement == TEXT("consideration"));
				if (!ValidPlacement || (Placement == TEXT("enter_condition") && !Data->Schema->AllowEnterConditions())
					|| (Placement == TEXT("task") && !Data->Schema->AllowMultipleTasks())
					|| (Placement == TEXT("single_task") && Data->Schema->AllowMultipleTasks()))
				{ return RejectCandidate(TEXT("state_tree_candidate_placement_invalid"),
					!ValidPlacement ? TEXT("The requested placement is incompatible with the selected candidate kind.")
						: TEXT("The StateTree schema does not allow the requested placement."), TEXT("placement"),
					MakeShared<FJsonValueString>(Placement), TEXT("placement allowed for ") + Kind + TEXT(" by ") + GetPathNameSafe(Data->Schema->GetClass())); }
				FStateTreeEditorNode Node;
				const UClass* Class = Cast<UClass>(Type);
				const UScriptStruct* Struct = Cast<UScriptStruct>(Type);
				if ((!Class || !Data->Schema->IsClassAllowed(Class)) && (!Struct || !Data->Schema->IsStructAllowed(Struct)))
				{ return RejectCandidate(TEXT("state_tree_candidate_schema_rejected"),
					TEXT("The StateTree schema does not allow the selected candidate type."), TEXT("candidate"),
					Type ? MakeShared<FJsonValueString>(Type->GetPathName()) : TSharedPtr<FJsonValue>(),
					TEXT("type allowed by ") + Data->Schema->GetClass()->GetPathName()); }
				if (!InitializeNode(Node, Root, Type, Detail)) {
					Detail.TargetPath = Target.GetPathName(); Detail.PropertyPath = TEXT("candidate");
					Detail.Source.InternalJsonPath = TEXT("/tree_items/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Key) + TEXT("/candidate");
					Error.SetFailure(MoveTemp(Detail), FString(), TEXT("/tree_items/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Key) + TEXT("/candidate")); return false;
				}
				const UScriptStruct* NodeType = Node.Node.GetScriptStruct();
				const bool MatchingType = (Kind == TEXT("state_tree_task") && NodeType->IsChildOf(FStateTreeTaskBase::StaticStruct()))
					|| (Kind == TEXT("state_tree_condition") && NodeType->IsChildOf(FStateTreeConditionBase::StaticStruct()))
					|| (Kind == TEXT("state_tree_evaluator") && NodeType->IsChildOf(FStateTreeEvaluatorBase::StaticStruct()))
					|| (Kind == TEXT("state_tree_consideration") && NodeType->IsChildOf(FStateTreeConsiderationBase::StaticStruct()));
				if (!MatchingType) { return RejectCandidate(TEXT("state_tree_candidate_type_mismatch"),
					TEXT("The initialized candidate type does not match the requested StateTree item kind."), TEXT("candidate"),
					MakeShared<FJsonValueString>(GetPathNameSafe(NodeType)), Kind); }
				Node.Node.GetMutable<FStateTreeNodeBase>().Name = FName(*Key);
				ItemId = Node.ID;
				if (Placement == TEXT("global_task")) Data->GlobalTasks.Add(Node);
				else if (Placement == TEXT("evaluator")) Data->Evaluators.Add(Node);
				else if (Placement == TEXT("single_task")) Root.SingleTask = Node;
				else if (Placement == TEXT("task")) Root.Tasks.Add(Node);
				else if (Placement == TEXT("enter_condition")) Root.EnterConditions.Add(Node);
				else if (Placement == TEXT("consideration")) Root.Considerations.Add(Node);
				else Root.AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::Succeeded).Conditions.Add(Node);
			}
			TSharedPtr<FJsonValue> Defaults;
			if (!ReadTreeDocument(*Template, Defaults, Detail)) { Error.SetFailure(Detail, Key); return false; }
			const auto Items = Defaults->AsObject()->GetObjectField(TEXT("items"));
			const FString SelectedId = (Kind == TEXT("state_tree_color") ? FString(TEXT("color:")) : FString())
				+ ItemId.ToString(EGuidFormats::DigitsWithHyphensLower);
			Item = Items->GetObjectField(SelectedId);
			if (Kind != TEXT("state_tree_color")) Item->SetStringField(TEXT("parent"), TEXT(""));
			if (Kind == TEXT("state_tree_property_function"))
			{
				Item->SetStringField(TEXT("node_name"), Key);
				Item->SetStringField(TEXT("label"), Key);
			}
			if (Kind == TEXT("state_tree_state"))
				Parameters = Items->GetObjectField(Root.Parameters.ID.ToString(EGuidFormats::DigitsWithHyphensLower));
			if (InitialCategories)
			{
				using namespace UE::UAI;
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				if (!AppendStructuralReadRequests(*Data, Template->GetPathName(), Requests, Detail))
				{ Error.SetFailure(Detail, Key); return false; }
				Requests.RemoveAll([&](const auto& Request) {
					return Request.SemanticId != ItemPropertySemanticId || !Request.SnapshotIdentityOverride.IsSet()
						|| Request.SnapshotIdentityOverride->GetSelector(TEXT("node_id")) != SelectedId;
				});
				UeAgentAuthoringState::FCaptureResult Capture;
				if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				{ Error.SetFailure(Capture.Diagnostic, Key); return false; }
				auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
				FString Text;
				HjsonWorkspace::FHjsonDocumentPtr Parsed;
				if (!InitialCategorySource && !CommandContract::Json::TryCanonicalize(InitialCategories, Text, Detail))
				{ Error.SetFailure(Detail, Key); return false; }
				if (!InitialCategorySource && !HjsonWorkspace::TryParseHjsonDocument(Text, Key, Resources, Parsed, Error)) return false;
				TArray<TSharedPtr<FJsonValue>> Changes;
				Composition::HjsonImport::FSemanticWorkspaceImportError Failure;
				if (!Composition::HjsonImport::TryProjectReflectedPropertyValues(Capture.Snapshot,
					InitialCategorySource ? *InitialCategorySource : *Parsed->GetRoot(), Resources, Changes, Failure))
				{ Error.SetFailure(Failure, Key); return false; }
				const auto Properties = Item->GetObjectField(TEXT("properties"));
				for (const auto& Change : Changes)
				{
					const FString Path = Change->AsObject()->GetStringField(TEXT("property_path"));
					const auto* Descriptor = Properties->GetArrayField(TEXT("properties")).FindByPredicate([&](const auto& Value) {
						return Value->AsObject()->GetStringField(TEXT("property_path")) == Path;
					});
					if (!Descriptor) {
						auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("state_tree_initial_property_missing"),
							TEXT("The initial property is missing from the native default descriptor."), TEXT("document_restore"));
						Diagnostic.PropertyPath = Path; Diagnostic.TargetPath = Target.GetPathName(); Diagnostic.SourceKind = TEXT("native_result");
						Error.SetFailure(MoveTemp(Diagnostic)); return false;
					}
					(*Descriptor)->AsObject()->SetField(TEXT("value"), UeAgentAuthoringState::CloneCanonicalValue(Change->AsObject()->TryGetField(TEXT("value"))));
					if (const auto Source = Change->AsObject()->TryGetField(TEXT("authoring_source")))
						(*Descriptor)->AsObject()->SetField(TEXT("authoring_source"), UeAgentAuthoringState::CloneCanonicalValue(Source));
				}
				// Decode and verify against the native template before the target's
				// structural transaction starts. Projection alone is not value validation.
				if (!ApplyTreeDocumentProperties(*Template, Defaults, Detail))
				{ Error.SetFailure(Detail, Key); return false; }
				TSharedPtr<FJsonValue> Normalized;
				if (!ReadTreeDocument(*Template, Normalized, Detail))
				{ Error.SetFailure(Detail, Key); return false; }
				Item->SetField(TEXT("properties"), UeAgentAuthoringState::CloneCanonicalValue(
					Normalized->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(SelectedId)->TryGetField(TEXT("properties"))));
				HjsonWorkspace::AttachProjectedPropertySources(Changes, *Item->GetObjectField(TEXT("properties")));
				if (UeAgentNativeNormalizationIntent::IsActive())
				{
					Requests.Reset();
					if (!AppendStructuralReadRequests(*Data, Template->GetPathName(), Requests, Detail)
						|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
					{ Error.SetFailure(Detail.IsEmpty() ? Capture.Diagnostic : Detail, Key); return false; }
					const auto Entries = Capture.Snapshot.CopyEntries();
					for (const auto& Change : Changes)
					{
						const FString Path = Change->AsObject()->GetStringField(TEXT("property_path"));
						const auto* Entry = Entries.FindByPredicate([&](const auto& Candidate) {
							return Candidate.SemanticId == ItemPropertySemanticId && Candidate.TargetIdentity.GetSelector(TEXT("node_id")) == SelectedId
								&& Candidate.TargetIdentity.GetSelector(TEXT("property_path")) == Path;
						});
						if (!Entry) {
							auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("state_tree_initial_property_missing"),
								TEXT("The initialized property is missing from the native normalization capture."), TEXT("document_restore"));
							Diagnostic.PropertyPath = Path; Diagnostic.TargetPath = Target.GetPathName(); Diagnostic.SourceKind = TEXT("native_result");
							Diagnostic.SubjectId = SelectedId;
							Error.SetFailure(MoveTemp(Diagnostic)); return false;
						}
						auto Selectors = Entry->TargetIdentity.Selectors;
						Selectors.Add(TEXT("asset_path"), Target.GetOutermost()->GetName());
						UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
						if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(Entry->TargetIdentity.ModelId,
							Entry->TargetIdentity.TargetKind, Selectors, Identity, Detail)) { Error.SetFailure(Detail); return false; }
						UeAgentNativeNormalizationIntent::RecordCreatedProperty(*Entry, Identity,
							TEXT("/tree_items/") + HjsonWorkspace::EscapeJsonPointerSegment(Key) + TEXT("/categories/") + HjsonWorkspace::EscapeJsonPointerSegment(Path),
							Change->AsObject());
					}
				}
			}
			return true;
		}
	bool BuildTreeDocumentFromSemanticModel(UStateTree& Tree,
		const UE::UAI::HjsonWorkspace::FAuthoringModelRef& Model,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		TSharedPtr<FJsonObject>& OutDocument, UE::UAI::HjsonWorkspace::FWorkspaceError& Error)
	{
		using namespace UE::UAI;
		OutDocument.Reset(); Error.Reset();
		FString SourceUnitId, SourcePointer;
		auto FailDiagnostic = [&](Contract::FDiagnostic Diagnostic) {
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Tree.GetPathName();
			if (Diagnostic.SourceKind == TEXT("native_result")) { Error.SetFailure(MoveTemp(Diagnostic), FString(), FString()); return false; }
			if (Diagnostic.Source.SourceFile.IsEmpty() && !Model->FindUnit(Diagnostic.Source.SourceUnit))
				Diagnostic.Source.SourceUnit = SourceUnitId;
			if (Diagnostic.SemanticPointer.IsEmpty() && Diagnostic.Source.JsonPath.IsEmpty()
				&& (Diagnostic.Source.InternalJsonPath.IsEmpty() || Diagnostic.Source.InternalJsonPath == TEXT("/")))
			{
				Diagnostic.SemanticPointer = SourcePointer;
				Diagnostic.Source.InternalJsonPath = SourcePointer;
				Diagnostic.SourceKind = TEXT("related_input");
			}
			Error.SetFailure(MoveTemp(Diagnostic), SourceUnitId, SourcePointer); return false;
		};
		auto Fail = [&](const FString& Code, const FString& Field, const FString& Reason,
			const TSharedPtr<FJsonValue>& Actual = nullptr, const TSharedPtr<FJsonValue>& Expected = nullptr) {
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("document_restore"));
			Diagnostic.ActualValue = Actual; Diagnostic.ExpectedValue = Expected;
			if (Actual || Expected) Diagnostic.ActualValuePresent = Actual.IsValid();
			if (!Field.IsEmpty()) Diagnostic.PropertyPath = Field;
			if (!Field.IsEmpty()) Diagnostic.SemanticPointer = SourcePointer + TEXT("/") + HjsonWorkspace::EscapeJsonPointerSegment(Field);
			return FailDiagnostic(MoveTemp(Diagnostic));
		};
		UE::UAI::Contract::FDiagnostic Detail;
		TSharedPtr<FJsonValue> Captured;
		if (!ReadTreeDocument(Tree, Captured, Detail)) { Error.SetFailure(Detail); return false; }
		const HjsonWorkspace::FSemanticUnit* TreeUnit = nullptr;
		for (const auto& Unit : Model->GetUnits())
			if (Unit.GetSemanticTypeRef() == TreeSemanticId)
			{
				if (TreeUnit) return Fail(TEXT("state_tree_semantic_tree_ambiguous"), FString(), TEXT("The semantic model contains more than one StateTree document."));
				TreeUnit = &Unit;
			}
		if (!TreeUnit) return Fail(TEXT("state_tree_semantic_tree_missing"), FString(), TEXT("The semantic model does not contain a StateTree document."));
		SourceUnitId = TreeUnit->GetUnitId();
		SourcePointer = TEXT("/tree_items");
		const auto* CanonicalRows = TreeUnit->GetValues().Find(TEXT("tree_items"));
		TSharedPtr<FJsonValue> RowsValue;
		if (CanonicalRows && !StateSemantic::CanonicalJson::TryToJsonValue(CanonicalRows->Get(), RowsValue, Detail)) return FailDiagnostic(Detail);
		if (!RowsValue.IsValid() || RowsValue->Type != EJson::Object) return Fail(TEXT("state_tree_semantic_items_invalid"), FString(), TEXT("StateTree tree_items must be an object."), RowsValue, MakeShared<FJsonValueString>(TEXT("object")));
		const auto Existing = Captured->AsObject()->GetObjectField(TEXT("items"));
		const auto Desired = MakeShared<FJsonObject>();
		TMap<FString, FString> NativeByNewKey;
		TMap<FString, FString> SourceKeyByNativeId;
		TArray<FString> ChangedBindings;
		int32 NewBindingCount = 0;
		for (const auto& Row : RowsValue->AsObject()->Values)
		{
			SourcePointer = TEXT("/tree_items/") + HjsonWorkspace::EscapeJsonPointerSegment(Row.Key);
			if (!Row.Value.IsValid() || Row.Value->Type != EJson::Object) return Fail(TEXT("state_tree_semantic_item_invalid"), FString(), TEXT("Each StateTree item must be an object."), Row.Value, MakeShared<FJsonValueString>(TEXT("object")));
			const auto Source = Row.Value->AsObject();
			FString Operation, Id;
			Source->TryGetStringField(TEXT("operation"), Operation);
			if (Operation == TEXT("remove")) continue;
			if (Operation != TEXT("create") && Source->HasField(TEXT("categories")))
				return Fail(TEXT("state_tree_initial_categories_require_create"), TEXT("categories"), TEXT("Initial categories may only be supplied when creating a StateTree item."));
			Source->TryGetStringField(TEXT("id"), Id);
			const TSharedPtr<FJsonObject>* Previous = nullptr;
			TSharedPtr<FJsonObject> Item;
			if (Operation == TEXT("create"))
			{
				TSharedPtr<FJsonObject> Parameters;
				if (!BuildNewItem(Tree, Row.Key, Source, Item, Parameters, Error,
					Prepared.GetWorkspace()->FindValueNode(TreeUnit->GetUnitId(), TEXT("/tree_items/")
						+ HjsonWorkspace::EscapeJsonPointerSegment(Row.Key) + TEXT("/categories")))) return FailDiagnostic(Error.GetDiagnostic());
				Id = Item->GetStringField(TEXT("id"));
				NativeByNewKey.Add(Row.Key, Id);
				if (Item->GetStringField(TEXT("kind")) == TEXT("binding"))
				{
					Item->SetNumberField(TEXT("index"), CastChecked<UStateTreeEditorData>(Tree.EditorData)->EditorBindings.GetNumBindings() + NewBindingCount++);
					ChangedBindings.AddUnique(Id);
				}
				if (Parameters) Desired->SetObjectField(Parameters->GetStringField(TEXT("id")), Parameters);
			}
			else if (!Existing->TryGetObjectField(Id, Previous)) return Fail(TEXT("state_tree_semantic_existing_item_missing"), TEXT("id"), TEXT("The edited StateTree item is no longer present in the native tree."), MakeShared<FJsonValueString>(Row.Key));
			else Item = UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(*Previous))->AsObject();
			if (Desired->HasField(Id)) return Fail(TEXT("state_tree_semantic_item_duplicate"), FString(), TEXT("The same native StateTree item is addressed by more than one authored row."));
			SourceKeyByNativeId.Add(Id, Row.Key);
			for (const TCHAR* Field : {TEXT("kind"), TEXT("placement"), TEXT("source_type")})
				if (const auto Value = Source->TryGetField(Field))
					if (UeAgentAuthoringState::MakeStableCanonicalValueKey(Value)
						!= UeAgentAuthoringState::MakeStableCanonicalValueKey(Item->TryGetField(Field)))
						return Fail(TEXT("state_tree_semantic_field_requires_native_resolution"), Field, TEXT("The declared kind, placement, or native type cannot be changed directly on an existing StateTree item."), Value, Item->TryGetField(Field));
			for (const TCHAR* Field : {TEXT("label"), TEXT("parent"), TEXT("index"), TEXT("target"), TEXT("transition_type"),
				TEXT("parameter_overrides"), TEXT("linked_state"), TEXT("linked_state_type"), TEXT("color_ref")})
				if (const auto Value = Source->TryGetField(Field)) Item->SetField(Field, UeAgentAuthoringState::CloneCanonicalValue(Value));
			if (Item->GetStringField(TEXT("kind")) == TEXT("node") && Source->HasField(TEXT("label")))
				if (Operation == TEXT("create") || Source->GetStringField(TEXT("label")) != (*Previous)->GetStringField(TEXT("label")))
					Item->SetField(TEXT("node_name"), UeAgentAuthoringState::CloneCanonicalValue(Source->TryGetField(TEXT("label"))));
			// Binding endpoint changes require resolving the corresponding native path;
			// never accept a readable endpoint while retaining a contradictory path state.
			if (Item->GetStringField(TEXT("kind")) == TEXT("binding"))
			{
				for (const TCHAR* Field : {TEXT("source"), TEXT("target")})
					if (const auto Value = Source->TryGetField(Field))
						if (Operation == TEXT("create") || UeAgentAuthoringState::MakeStableCanonicalValueKey(Value)
							!= UeAgentAuthoringState::MakeStableCanonicalValueKey((*Previous)->TryGetField(Field)))
						{
							ChangedBindings.AddUnique(Id);
							Item->SetField(Field, UeAgentAuthoringState::CloneCanonicalValue(Value));
						}
				for (const TCHAR* Field : {TEXT("source_path"), TEXT("target_path")})
					if (const auto Value = Source->TryGetField(Field))
					{
						if (Value->Type != EJson::String) return Fail(TEXT("state_tree_binding_path_invalid"), Field, TEXT("The binding property path must be a valid string path on the selected endpoint."), Value, MakeShared<FJsonValueString>(TEXT("string")));
						if (Value->AsString() != Item->GetStringField(Field))
						{
							ChangedBindings.AddUnique(Id);
							Item->SetStringField(Field, Value->AsString());
						}
					}
			}
			Desired->SetObjectField(Id, Item);
		}
		for (const auto& Row : Desired->Values)
			for (const TCHAR* Field : {TEXT("parent"), TEXT("target"), TEXT("source"), TEXT("linked_state"), TEXT("color_ref")})
				if (const auto Value = Row.Value->AsObject()->TryGetField(Field); Value && Value->Type == EJson::Object)
				{
					SourcePointer = SourceKeyByNativeId.Contains(Row.Key)
						? TEXT("/tree_items/") + HjsonWorkspace::EscapeJsonPointerSegment(SourceKeyByNativeId.FindChecked(Row.Key)) : TEXT("/tree_items");
					FString Key;
					if (!Value->AsObject()->TryGetStringField(TEXT("new_item"), Key) || !NativeByNewKey.Contains(Key))
						return Fail(TEXT("state_tree_semantic_new_reference_unresolved"), Field, TEXT("The new_item reference does not identify an item created by this import."), Value->AsObject()->TryGetField(TEXT("new_item")), MakeShared<FJsonValueString>(TEXT("created item key")));
					Row.Value->AsObject()->SetStringField(Field, NativeByNewKey.FindChecked(Key));
				}
		// Native palette editing restores its default entry when the set becomes empty.
		SourcePointer = TEXT("/tree_items");
		// Capture a native default instead of maintaining another default-color constant.
		bool HasColor = false;
		for (const auto& Row : Desired->Values)
			HasColor |= Row.Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("color");
		if (!HasColor)
		{
			TStrongObjectPtr<UStateTree> Defaults(NewObject<UStateTree>(GetTransientPackage()));
			Defaults->EditorData = NewObject<UStateTreeEditorData>(Defaults.Get());
			TSharedPtr<FJsonValue> DefaultDocument;
			if (!ReadTreeDocument(*Defaults, DefaultDocument, Detail)) return FailDiagnostic(Detail);
			for (const auto& Row : DefaultDocument->AsObject()->GetObjectField(TEXT("items"))->Values)
				if (Row.Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("color"))
					Desired->SetField(Row.Key, UeAgentAuthoringState::CloneCanonicalValue(Row.Value));
		}
		const FString DefaultColorId = TEXT("color:") + FGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		for (const auto& Row : Desired->Values)
			if (Row.Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("state"))
			{
				FString Color;
				if (Row.Value->AsObject()->TryGetStringField(TEXT("color_ref"), Color) && Color == DefaultColorId)
					Row.Value->AsObject()->SetStringField(TEXT("color_ref"), TEXT(""));
			}
		const auto ParameterEdits = MakeShared<FJsonObject>();
		for (const auto& Pair : Projection.PropertyChangesByUnitId)
		{
			SourceUnitId = Pair.Key; SourcePointer = TEXT("/value");
			const auto* Unit = Prepared.GetWorkspace()->GetModel()->GetUnits().FindByPredicate([&](const auto& Candidate) { return Candidate.GetUnitId() == Pair.Key; });
			if (!Unit) return Fail(TEXT("state_tree_semantic_property_unit_missing"), FString(), TEXT("The projected property change has no matching semantic property unit."));
			const auto* Change = Prepared.GetChangeSetDraft().GetChanges().FindByPredicate([&](const auto& Candidate) {
				return Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetCanonicalKey()
					|| Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetSelector(HjsonWorkspace::CanonicalAuthoringTargetIdentitySelector);
			});
			if (!Change) return Fail(TEXT("state_tree_semantic_property_change_missing"), FString(), TEXT("The semantic property unit has no matching prepared change."));
			const FString Id = Change->GetIdentity().GetSelector(TEXT("node_id"));
			if (Id.IsEmpty()) continue;
			if (!Desired->HasField(Id)) return Fail(TEXT("state_tree_semantic_property_owner_missing"), FString(),
				TEXT("The native owner of the projected property edit is not present in the desired StateTree."),
				MakeShared<FJsonValueString>(Change->GetIdentity().GetSelector(TEXT("property_path"))));
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) return Fail(TEXT("state_tree_semantic_property_owner_missing"), FString(),
				TEXT("The projected property edit must be an object."), Pair.Value, MakeShared<FJsonValueString>(TEXT("object")));
			const auto Item = Desired->GetObjectField(Id);
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Item->TryGetObjectField(TEXT("properties"), Properties)) return Fail(TEXT("state_tree_semantic_properties_missing"), FString(), TEXT("The selected StateTree item has no native properties collection."));
			const FString Path = Change->GetIdentity().GetSelector(TEXT("property_path"));
			bool Found = false;
			for (const auto& Descriptor : (*Properties)->GetArrayField(TEXT("properties")))
				if (Descriptor->AsObject()->GetStringField(TEXT("property_path")) == Path)
				{
					const auto Value = Pair.Value->AsObject()->TryGetField(TEXT("value"));
					if (!Value.IsValid()) return Fail(TEXT("state_tree_semantic_property_value_missing"), FString(), TEXT("The projected StateTree property edit has no value."), nullptr, MakeShared<FJsonValueString>(TEXT("value for ") + Path));
					if (Item->GetStringField(TEXT("kind")) == TEXT("parameters")
						&& !Item->GetStringField(TEXT("parent")).IsEmpty() && Path == TEXT("parameters.Parameters"))
					{
						TSharedPtr<FJsonValue> Before;
						if (!Change->GetExpectedBefore().IsValid())
							return Fail(TEXT("state_tree_parameter_edit_before_missing"), FString(), TEXT("The captured parameter value required to preserve unchanged fields is missing."));
						if (!StateSemantic::CanonicalJson::TryToJsonValue(*Change->GetExpectedBefore(), Before, Detail)) return FailDiagnostic(Detail);
						const TSharedPtr<FJsonObject>* OldFields = nullptr;
						const TSharedPtr<FJsonObject>* NewFields = nullptr;
						if (!Before.IsValid() || Before->Type != EJson::Object || Value->Type != EJson::Object
							|| !Before->AsObject()->TryGetObjectField(TEXT("fields"), OldFields)
							|| !Value->AsObject()->TryGetObjectField(TEXT("fields"), NewFields))
							return Fail(TEXT("state_tree_parameter_edit_invalid"), FString(), TEXT("The current and proposed parameter values must be objects containing a fields object."), Value, MakeShared<FJsonValueString>(TEXT("object with fields object")));
						TArray<TSharedPtr<FJsonValue>> Fields;
						for (const auto& Field : (*NewFields)->Values)
							if (UeAgentAuthoringState::MakeStableCanonicalValueKey(Field.Value)
								!= UeAgentAuthoringState::MakeStableCanonicalValueKey((*OldFields)->TryGetField(Field.Key)))
								Fields.Add(MakeShared<FJsonValueString>(Field.Key));
						const auto Edit = MakeShared<FJsonObject>();
						Edit->SetArrayField(TEXT("fields"), Fields);
						Edit->SetBoolField(TEXT("schema"),
							UeAgentAuthoringState::MakeStableCanonicalValueKey(Before->AsObject()->TryGetField(TEXT("schema")))
							!= UeAgentAuthoringState::MakeStableCanonicalValueKey(Value->AsObject()->TryGetField(TEXT("schema"))));
						ParameterEdits->SetObjectField(Item->GetStringField(TEXT("parent")), Edit);
					}
					Descriptor->AsObject()->SetField(TEXT("value"), UeAgentAuthoringState::CloneCanonicalValue(Value));
					if (const auto Source = Pair.Value->AsObject()->TryGetField(TEXT("authoring_source")))
						Descriptor->AsObject()->SetField(TEXT("authoring_source"), UeAgentAuthoringState::CloneCanonicalValue(Source));
					Found = true; break;
				}
			if (!Found) return Fail(TEXT("state_tree_semantic_property_path_missing"), FString(), TEXT("The requested property path does not exist in the selected StateTree item."), MakeShared<FJsonValueString>(Path));
		}
		SourceUnitId = TreeUnit->GetUnitId(); SourcePointer = TEXT("/tree_items");
		for (const auto& Source : SourceKeyByNativeId) {
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (Desired->TryGetObjectField(Source.Key, Item))
				Prepared.GetWorkspace()->AttachInlinePropertySources(TreeUnit->GetUnitId(),
					TEXT("/tree_items/") + HjsonWorkspace::EscapeJsonPointerSegment(Source.Value), **Item, FString());
		}
		if (!NormalizeTreeRemovalContext(Existing, Desired, Detail)) return FailDiagnostic(Detail);
		Captured->AsObject()->SetObjectField(TEXT("items"), Desired);
		if (!ParameterEdits->Values.IsEmpty()) Captured->AsObject()->SetObjectField(TEXT("authoring_parameter_edits"), ParameterEdits);
		for (const auto& Id : ChangedBindings)
		{
			SourcePointer = SourceKeyByNativeId.Contains(Id)
				? TEXT("/tree_items/") + HjsonWorkspace::EscapeJsonPointerSegment(SourceKeyByNativeId.FindChecked(Id)) : TEXT("/tree_items");
			const auto Item = Desired->GetObjectField(Id);
			const TSharedPtr<FJsonObject>* Original = nullptr;
			Existing->TryGetObjectField(Id, Original);
			for (const TCHAR* Side : {TEXT("source"), TEXT("target")})
			{
				FGuid Endpoint;
				if (!FGuid::Parse(Item->GetStringField(Side), Endpoint) || !Desired->HasField(Item->GetStringField(Side)))
					return Fail(TEXT("state_tree_binding_endpoint_invalid"), Side, TEXT("The binding endpoint does not resolve to an item in the desired StateTree."));
				const FString StateField = FString(Side) + TEXT("_path_state");
				FPropertyBindingPath Path;
				if (!UeAgentPropertyValueCodec::DecodeStructValue(FPropertyBindingPath::StaticStruct(), Item->TryGetField(StateField), &Path, Detail)) return FailDiagnostic(Detail);
				const FString PathField = FString(Side) + TEXT("_path");
				if (Original && Path.GetStructID() == Endpoint && Item->GetStringField(PathField) == (*Original)->GetStringField(PathField)) continue;
				Path.SetStructID(Endpoint);
				if (!Path.FromString(Item->GetStringField(PathField))) return Fail(TEXT("state_tree_binding_path_invalid"), PathField, TEXT("The binding property path must be a valid string path on the selected endpoint."), Item->TryGetField(PathField));
				UeAgentPropertyValueCodec::FOptions Options;
				Item->SetField(StateField, UeAgentPropertyValueCodec::EncodeStructValue(FPropertyBindingPath::StaticStruct(), &Path, nullptr, Options));
			}
		}
		SourcePointer = TEXT("/tree_items");
		if (!ValidateTreeDocument(Captured, Detail)) return FailDiagnostic(Detail);
		const auto ParameterResets = MakeShared<FJsonObject>();
		for (const auto& Row : Desired->Values)
		{
			const auto Item = Row.Value->AsObject();
			const TSharedPtr<FJsonObject>* Previous = nullptr;
			if (Item->GetStringField(TEXT("kind")) != TEXT("state") || !Existing->TryGetObjectField(Row.Key, Previous)) continue;
			TArray<TSharedPtr<FJsonValue>> Removed;
			const auto& Overrides = Item->GetArrayField(TEXT("parameter_overrides"));
			for (const auto& Old : (*Previous)->GetArrayField(TEXT("parameter_overrides")))
				if (!Overrides.ContainsByPredicate([&](const auto& Current) { return Current->AsString() == Old->AsString(); }))
					Removed.Add(UeAgentAuthoringState::CloneCanonicalValue(Old));
			if (!Removed.IsEmpty()) ParameterResets->SetArrayField(Row.Key, Removed);
		}
		if (!ParameterResets->Values.IsEmpty()) Captured->AsObject()->SetObjectField(TEXT("authoring_parameter_resets"), ParameterResets);
		if (!ChangedBindings.IsEmpty())
		{
			TStrongObjectPtr<UStateTree> Proposed(DuplicateObject<UStateTree>(&Tree, GetTransientPackage(),
				MakeUniqueObjectName(GetTransientPackage(), UStateTree::StaticClass(), TEXT("StateTreeBindingValidation"))));
			if (!InitializeTreeAuthoringPreview(Tree, *Proposed, Detail)
				|| !ApplyTreeDocument(*Proposed, Captured, Detail)) { Error.SetFailure(Detail); return false; }
			auto* Data = CastChecked<UStateTreeEditorData>(Proposed->EditorData);
			for (const auto& Id : ChangedBindings)
			{
				SourcePointer = SourceKeyByNativeId.Contains(Id)
					? TEXT("/tree_items/") + HjsonWorkspace::EscapeJsonPointerSegment(SourceKeyByNativeId.FindChecked(Id)) : TEXT("/tree_items");
				const auto Item = Desired->GetObjectField(Id);
				const TSharedPtr<FJsonObject>* Original = nullptr;
				Existing->TryGetObjectField(Id, Original);
				for (const TCHAR* Field : {TEXT("source_path"), TEXT("target_path")})
				{
					const TCHAR* Side = FCString::Strcmp(Field, TEXT("source_path")) == 0 ? TEXT("source") : TEXT("target");
					if (Original && Item->GetStringField(Field) == (*Original)->GetStringField(Field)
						&& Item->GetStringField(Side) == (*Original)->GetStringField(Side)) continue;
					const FString StateField = FString(Field) + TEXT("_state");
					FPropertyBindingPath Path;
					if (!UeAgentPropertyValueCodec::DecodeStructValue(FPropertyBindingPath::StaticStruct(),
						Item->TryGetField(StateField), &Path, Detail)) { Error.SetFailure(Detail); return false; }
					FPropertyBindingDataView View;
					FString NativeDetail;
					const bool bHasView = Data->GetBindingDataViewByID(Path.GetStructID(), View);
					const bool bParsedPath = bHasView && Path.FromString(Item->GetStringField(Field));
					if (!bParsedPath || !Path.UpdateSegmentsFromValue(View, &NativeDetail))
					{ auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("state_tree_binding_path_invalid"),
						SourcePointer + TEXT("/") + FString(Field), nullptr, Item->TryGetField(Field), TEXT("binding_resolve"),
						!bHasView ? TEXT("The binding endpoint has no native data view.")
							: !bParsedPath ? TEXT("The binding property path has invalid path syntax.")
							: NativeDetail.IsEmpty() ? TEXT("The binding path does not resolve to a property on the selected endpoint.") : NativeDetail);
						Diagnostic.Source.InternalJsonPath = Diagnostic.Source.JsonPath;
						Diagnostic.SubjectId = Id;
						return FailDiagnostic(MoveTemp(Diagnostic)); }
					UeAgentPropertyValueCodec::FOptions Options;
					Item->SetField(StateField, UeAgentPropertyValueCodec::EncodeStructValue(FPropertyBindingPath::StaticStruct(), &Path, nullptr, Options));
				}
				// Once the proposed owners and endpoints exist, validate a new function against
				// the same receiving-field menu used for discovery (also supports same-edit targets).
				const FString FunctionId = Item->GetStringField(TEXT("source"));
				const FString* FunctionKey = SourceKeyByNativeId.Find(FunctionId);
				const TSharedPtr<FJsonObject>* FunctionItem = nullptr;
				FString FunctionPlacement;
				if (FunctionKey && NativeByNewKey.Contains(*FunctionKey) && Desired->TryGetObjectField(FunctionId, FunctionItem)
					&& (*FunctionItem)->TryGetStringField(TEXT("placement"), FunctionPlacement) && FunctionPlacement == TEXT("property_function"))
				{
					const auto FunctionRow = RowsValue->AsObject()->GetObjectField(*FunctionKey);
					const auto CandidateSpec = FunctionRow->GetObjectField(TEXT("candidate"));
					const FString Pointer = TEXT("/tree_items/") + HjsonWorkspace::EscapeJsonPointerSegment(*FunctionKey) + TEXT("/candidate");
					HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
					if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(CandidateSpec, Pointer, Selector, Error)) return false;
					QueryOwners::AiBehavior::FAiBehaviorCandidateContext Context;
					Context.AttachmentKind = TEXT("property_function");
					Context.TargetPath = Item->GetStringField(TEXT("target_path"));
					Context.FunctionOutput = Item->GetStringField(TEXT("source_path"));
					FGuid::Parse(Item->GetStringField(TEXT("target")), Context.StateTreeParentId);
					QueryOwners::AiBehavior::FAiBehaviorCandidate Candidate;
					if (!QueryOwners::AiBehavior::TryResolveAiBehaviorCandidate(*Proposed, Selector.Name, {}, Candidate, Detail, Selector.Select, Context))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Detail, Pointer);
						return FailDiagnostic(Detail);
					}
				}
			}
			if (!ApplyTreeDocumentForAuthoring(*Proposed, Captured, Detail)) { Error.SetFailure(Detail); return false; }
			for (const auto& Id : ChangedBindings)
			{
				SourcePointer = SourceKeyByNativeId.Contains(Id)
					? TEXT("/tree_items/") + HjsonWorkspace::EscapeJsonPointerSegment(SourceKeyByNativeId.FindChecked(Id)) : TEXT("/tree_items");
				FPropertyBindingPath Target;
				if (!UeAgentPropertyValueCodec::DecodeStructValue(FPropertyBindingPath::StaticStruct(),
					Desired->GetObjectField(Id)->TryGetField(TEXT("target_path_state")), &Target, Detail)) { Error.SetFailure(Detail); return false; }
				if (!Data->EditorBindings.GetBindingSource(Target))
					return Fail(TEXT("state_tree_binding_rejected_by_native_authoring"), TEXT("target_path"), TEXT("Native StateTree authoring did not retain the requested property binding."), Desired->GetObjectField(Id)->TryGetField(TEXT("target_path")));
			}
			FStateTreeCompilerLog CompileLog;
			if (!UStateTreeEditingSubsystem::CompileStateTree(Proposed.Get(), CompileLog))
			{
				UeAgentAuthoringDiagnostics::FDiagnostic Errors;
				for (const auto& Message : CompileLog.ToTokenizedMessages())
					if (Message->GetSeverity() == EMessageSeverity::Error)
					{
						auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("state_tree_binding_compile_failed"), Message->ToText().ToString(), TEXT("native_compile"));
						Diagnostic.TargetPath = Tree.GetPathName();
						Diagnostic.SourceKind = TEXT("native_result");
						Errors.Append(Diagnostic);
					}
				if (!Errors.Code.IsEmpty()) { Error.SetFailure(Errors, FString()); return false; }
				return Fail(TEXT("state_tree_binding_compile_failed"), FString(), TEXT("StateTree binding compilation failed without an error message from the compiler."));
			}
		}
		if (!HjsonWorkspace::TryRecordKeyedCreationArguments(TEXT("tree_items"), RowsValue,
			Desired, NativeByNewKey, Detail)) { Error.SetFailure(Detail); return false; }
		OutDocument = Captured->AsObject();
		return true;
	}
}
