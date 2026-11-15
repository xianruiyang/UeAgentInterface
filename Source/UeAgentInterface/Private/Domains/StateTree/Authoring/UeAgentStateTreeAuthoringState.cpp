#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeNativeCapture.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeEditorTypes.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"

namespace UeAgentStateTreeAuthoring
{
	namespace
	{
		constexpr const TCHAR* AccessorId = TEXT("state_tree.structure.state");
		constexpr const TCHAR* TreeKind = TEXT("state_tree.tree");
		constexpr const TCHAR* ItemKind = TEXT("state_tree.item");
		constexpr const TCHAR* PropertyKind = TEXT("state_tree.item_property");
		FString IdentityModel(const FString& Kind) { return Kind + TEXT(".identity"); }
		FString Id(const FGuid& Value) { return Value.IsValid() ? Value.ToString(EGuidFormats::DigitsWithHyphensLower) : FString(); }
		FString ColorId(const FGuid& Value) { return TEXT("color:") + Value.ToString(EGuidFormats::DigitsWithHyphensLower); }
		FString ItemPointer(const FString& Id) { return TEXT("/items/") + Id.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); }

		UE::UAI::Contract::FDiagnostic DocumentFailure(const TCHAR* Code, const TCHAR* Reason, const UStateTree& Tree,
			const FString& InternalPointer, const FJsonObject* Input = nullptr, const FString& Field = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("document_apply"));
			Error.TargetPath = Tree.GetPathName();
			Error.Source.InternalJsonPath = InternalPointer;
			Error.PropertyPath = Field;
			if (Input)
			{
				Input->TryGetStringField(TEXT("label"), Error.CandidateName);
				if (!Field.IsEmpty()) Error.ActualValuePresent = Input->HasField(Field);
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input, Field);
			}
			return Error;
		}

		const TCHAR* PlacementName(ENodePlacement Placement)
		{
			switch (Placement)
			{
			case ENodePlacement::Evaluator: return TEXT("evaluator");
			case ENodePlacement::GlobalTask: return TEXT("global_task");
			case ENodePlacement::Task: return TEXT("task");
			case ENodePlacement::SingleTask: return TEXT("single_task");
			case ENodePlacement::EnterCondition: return TEXT("enter_condition");
			case ENodePlacement::Consideration: return TEXT("consideration");
			case ENodePlacement::TransitionCondition: return TEXT("transition_condition");
			case ENodePlacement::PropertyFunction: return TEXT("property_function");
			}
			checkNoEntry();
			return TEXT("");
		}

		UeAgentAuthoringState::EReadOutcome ReadState(const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			using EOutcome = UeAgentAuthoringState::EReadOutcome;
			OutValue = {};
			OutError.Reset();
			const auto FailRead = [&](const TCHAR* Code, const FString& Reason, const FString& ExpectedType = FString())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("state_capture"));
				OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
				OutError.CandidateName = Request.Context.FindRef(TEXT("label"));
				OutError.PropertyPath = Request.PropertyPath;
				OutError.SubjectId = Request.TargetIdentity.GetSelector(TEXT("node_id"));
				OutError.SemanticId = Request.SemanticId;
				OutError.SourceKind = TEXT("native_result");
				if (!ExpectedType.IsEmpty())
				{
					OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedType);
					OutError.ActualValue = MakeShared<FJsonValueString>(Request.RootObject ? Request.RootObject->GetClass()->GetPathName()
						: Request.RootStruct ? Request.RootStruct->GetPathName() : TEXT("no native data"));
				}
				return EOutcome::Failed;
			};
			if (Request.SemanticId == ItemPropertySemanticId)
			{
				UeAgentAuthoringState::FStateAccessor PropertyAccessor;
				if (!UeAgentAuthoringState::FStateAccessorRegistry::Get().CopyAccessor(TEXT("core.property"), PropertyAccessor))
					return FailRead(TEXT("state_tree_property_accessor_missing"), TEXT("StateTree property capture requires the core property accessor, which is not registered."));
				const EOutcome Outcome = PropertyAccessor(Request, OutValue, OutError);
				if (Outcome != EOutcome::Resolved || !Request.Context.Contains(TEXT("state_tree_property_function"))) return Outcome;
				UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
				UeAgentAuthoringTargetLocator::FResolveResult Result;
				const bool bResolved = Request.RootObject
					? UeAgentAuthoringTargetLocator::ResolvePropertyPath(const_cast<UObject*>(Request.RootObject), Request.PropertyPath, Target, Result)
					: UeAgentAuthoringTargetLocator::ResolvePropertyPath(Request.RootStruct, const_cast<void*>(Request.RootData),
						Request.RootOwnerPath, const_cast<UObject*>(Request.NotificationOwner), Request.PropertyPath, Target, Result);
				if (!bResolved) { OutError = Result.Diagnostic; return EOutcome::Failed; }
				if (UE::StateTree::GetUsageFromMetaData(Target.Property) == EStateTreePropertyUsage::Output)
					OutValue.bAuthoringVisible = false;
				return Outcome;
			}
			const auto Value = MakeShared<FJsonObject>();
			if (Request.SemanticId == TreeSemanticId)
			{
				const auto* Data = Cast<UStateTreeEditorData>(Request.RootObject);
				if (!Data) return FailRead(TEXT("state_tree_data_missing"), TEXT("StateTree capture requires editor data; the supplied native object has a different type or is missing."), UStateTreeEditorData::StaticClass()->GetPathName());
				Value->SetStringField(TEXT("schema_class"), Data->Schema ? Data->Schema->GetClass()->GetPathName() : FString());
			}
			else if (Request.SemanticId == ItemSemanticId)
			{
				for (const auto& Field : Request.Context) Value->SetStringField(Field.Key, Field.Value);
				Value->SetStringField(TEXT("id"), Request.TargetIdentity.GetSelector(TEXT("node_id")));
				Value->SetNumberField(TEXT("index"), FCString::Atoi(*Request.Context.FindRef(TEXT("index"))));
				const FString Kind = Request.Context.FindRef(TEXT("kind"));
				// Structural items are not all nodes. Retain the native carrier type
				// in the frozen capture for read-side identity validation.
				Value->SetStringField(TEXT("class_path"), Request.RootObject
					? Request.RootObject->GetClass()->GetPathName()
					: Request.RootStruct ? Request.RootStruct->GetPathName() : FString());
				if (Kind == TEXT("state"))
				{
					const auto* State = Cast<UStateTreeState>(Request.RootObject);
					if (!State) return FailRead(TEXT("state_tree_state_missing"), TEXT("The captured state item has no native StateTree state object."), UStateTreeState::StaticClass()->GetPathName());
					Value->SetStringField(TEXT("label"), State->Name.ToString());
					Value->SetStringField(TEXT("color_ref"), State->ColorRef.ID.IsValid() ? ColorId(State->ColorRef.ID) : FString());
					Value->SetStringField(TEXT("linked_state"), Id(State->LinkedSubtree.ID));
					Value->SetStringField(TEXT("linked_state_type"), StaticEnum<EStateTreeTransitionType>()->GetNameStringByValue(static_cast<int64>(State->LinkedSubtree.LinkType)));
					TArray<TSharedPtr<FJsonValue>> Overrides;
					for (const FGuid& OverrideId : State->Parameters.PropertyOverrides)
					{
						const auto* Field = State->Parameters.Parameters.FindPropertyDescByID(OverrideId);
						if (!Field)
						{
							FailRead(TEXT("state_tree_parameter_override_unresolved"),
								TEXT("A stored parameter override refers to a parameter that is absent from this state's parameter bag."));
							OutError.CandidateName = State->Name.ToString();
							OutError.PropertyPath = FString::Printf(TEXT("parameter_overrides[%d]"), Overrides.Num());
							OutError.ActualEvidence = MakeShared<FJsonValueString>(Id(OverrideId));
							return EOutcome::Failed;
						}
						Overrides.Add(MakeShared<FJsonValueString>(Field->Name.ToString()));
					}
					Value->SetArrayField(TEXT("parameter_overrides"), Overrides);
				}
				else if (Kind == TEXT("color") && Request.RootStruct == FStateTreeEditorColor::StaticStruct() && Request.RootData)
				{
					Value->SetStringField(TEXT("label"), static_cast<const FStateTreeEditorColor*>(Request.RootData)->DisplayName);
				}
				else if ((Kind == TEXT("parameters") || Kind == TEXT("binding_source") || Kind == TEXT("settings")) && Request.RootObject)
				{
					// Collection identity is native; its readable label and owning state
					// come from the same captured hierarchy used for all other items.
					Value->SetStringField(TEXT("label"), Request.Context.FindRef(TEXT("label")));
				}
				else if (Kind == TEXT("binding") && Request.RootData)
				{
					const auto& Binding = *static_cast<const FPropertyBindingBinding*>(Request.RootData);
					Value->SetStringField(TEXT("label"), TEXT("Binding"));
					Value->SetStringField(TEXT("source"), Id(Binding.GetSourcePath().GetStructID()));
					Value->SetStringField(TEXT("target"), Id(Binding.GetTargetPath().GetStructID()));
					Value->SetStringField(TEXT("source_path"), Binding.GetSourcePath().ToString());
					Value->SetStringField(TEXT("target_path"), Binding.GetTargetPath().ToString());
					// The readable path omits instance types and may use display names.
					// Keep the persistent native segments in the captured machine state
					// so a sparse edit can retain the original binding exactly.
					UeAgentPropertyValueCodec::FOptions PathOptions;
					Value->SetField(TEXT("source_path_state"), UeAgentPropertyValueCodec::EncodeStructValue(
						FPropertyBindingPath::StaticStruct(), &Binding.GetSourcePath(), nullptr, PathOptions));
					Value->SetField(TEXT("target_path_state"), UeAgentPropertyValueCodec::EncodeStructValue(
						FPropertyBindingPath::StaticStruct(), &Binding.GetTargetPath(), nullptr, PathOptions));
				}
				else if (Kind == TEXT("node") && Request.RootStruct == FStateTreeEditorNode::StaticStruct() && Request.RootData)
				{
					const auto& Node = *static_cast<const FStateTreeEditorNode*>(Request.RootData);
					Value->SetStringField(TEXT("label"), Node.GetName().ToString());
					Value->SetStringField(TEXT("node_name"), Node.Node.IsValid() ? Node.Node.Get<FStateTreeNodeBase>().Name.ToString() : FString());
					Value->SetStringField(TEXT("node_type"), Node.Node.IsValid() ? Node.Node.GetScriptStruct()->GetPathName() : FString());
					Value->SetStringField(TEXT("instance_type"), Node.Instance.IsValid() ? Node.Instance.GetScriptStruct()->GetPathName() : FString());
					Value->SetStringField(TEXT("instance_object_class"), Node.InstanceObject ? Node.InstanceObject->GetClass()->GetPathName() : FString());
				}
				else if (Kind == TEXT("transition") && Request.RootStruct == FStateTreeTransition::StaticStruct() && Request.RootData)
				{
					const auto& Transition = *static_cast<const FStateTreeTransition*>(Request.RootData);
					Value->SetStringField(TEXT("label"), TEXT("Transition"));
					Value->SetStringField(TEXT("target"), Id(Transition.State.ID));
					Value->SetStringField(TEXT("transition_type"), StaticEnum<EStateTreeTransitionType>()->GetNameStringByValue(static_cast<int64>(Transition.State.LinkType)));
				}
				else return FailRead(TEXT("state_tree_item_binding_invalid"),
					FString::Printf(TEXT("The captured '%s' item does not have the native data required for that item kind."), *Kind),
					TEXT("native data matching the item kind"));
			}
			else return FailRead(TEXT("state_tree_semantic_unsupported"), TEXT("The StateTree accessor supports tree, item and item-property capture; the requested semantic category is unsupported."));
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			OutValue.Provenance = AccessorId;
			OutValue.Value = MakeShared<FJsonValueObject>(Value);
			OutValue.bAvailable = true;
			return EOutcome::Resolved;
		}
	}

	bool RegisterTreeState(UE::UAI::Contract::FDiagnostic& OutError)
	{
		struct FContract { const TCHAR* Semantic; const TCHAR* Kind; TArray<FString> Keys; };
		const TArray<FContract> Contracts = {
			{TreeSemanticId, TreeKind, {TEXT("asset_path"), TEXT("graph_name")}},
			{ItemSemanticId, ItemKind, {TEXT("asset_path"), TEXT("graph_name"), TEXT("node_id")}},
			{ItemPropertySemanticId, PropertyKind, {TEXT("asset_path"), TEXT("graph_name"), TEXT("node_id"), TEXT("property_path")}}
		};
		for (const auto& Contract : Contracts)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Identity;
			Identity.ModelId = IdentityModel(Contract.Kind);
			Identity.TargetKind = Contract.Kind;
			int32 Priority = 100;
			for (const auto& Key : Contract.Keys) Identity.Selectors.Add({Key, Priority++, false});
			Identity.KeyPatterns = {{TEXT("scoped"), 100, Contract.Keys}};
			UeAgentAuthoringIdentity::ERegistrationResult IdentityResult;
			if (!UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Identity, IdentityResult, OutError)) return false;
			const bool bProperty = FString(Contract.Kind) == PropertyKind;
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = Contract.Semantic;
			Semantic.DomainId = UeAgentReflectionAuthoring::DomainId;
			Semantic.TargetKind = Contract.Kind;
			Semantic.ValueType = bProperty ? TEXT("*") : TEXT("object");
			Semantic.OwnerModel = TEXT("state_tree_editor_data");
			Semantic.IdentityModel = Identity.ModelId;
			Semantic.StateAccessor = AccessorId;
			Semantic.WriteCapability = bProperty ? UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment
				: UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation;
			Semantic.ConstraintSource = TEXT("state_tree.schema");
			Semantic.AvailabilitySource = Semantic.StateAccessor;
			Semantic.Comparator = TEXT("uai.compare.canonical_value");
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(Semantic, Result, OutError)) return false;
		}
		UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
		Accessor.AccessorId = AccessorId;
		// Property requests keep their native locator identity until snapshot projection.
		Accessor.TargetKinds = {TreeKind, ItemKind, TEXT("property")};
		UeAgentAuthoringState::ERegistrationResult Result;
		return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(Accessor, ReadState, Result, OutError);
	}

	bool AppendStructuralReadRequests(const UStateTreeEditorData& EditorData, const FString& AssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FTreeCapture Capture;
		if (!CaptureTree(EditorData, Capture, OutError)) return false;
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		auto Add = [&](const TCHAR* Semantic, const TCHAR* Kind, const FString& ItemId, const UObject* Object,
			UStruct* Struct, const void* Data, const TMap<FString, FString>& Context) {
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = Semantic;
			TMap<FString, FString> Selectors{{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), TEXT("main")}};
			if (!ItemId.IsEmpty()) Selectors.Add(TEXT("node_id"), ItemId);
			if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel(Kind), Kind, Selectors, Request.TargetIdentity, OutError)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = Object;
			Request.RootStruct = Struct;
			Request.RootData = Data;
			Request.Context = Context;
			Requests.Add(MoveTemp(Request));
			return true;
		};
		auto Options = [&](const FString& ItemId, const FString& Part) {
			UeAgentAuthoringState::FEditablePropertyReadOptions Result;
			Result.SemanticId = ItemPropertySemanticId;
			Result.Limits.bShowDefaultsOnly = true;
			Result.BuildSnapshotIdentity = [AssetPath, ItemId, Part](const FString& Path,
				UeAgentAuthoringIdentity::FCanonicalIdentity& Identity, UE::UAI::Contract::FDiagnostic& Error) {
				return UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel(PropertyKind), PropertyKind,
					{{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), TEXT("main")},
						{TEXT("node_id"), ItemId}, {TEXT("property_path"), Part + TEXT(".") + Path}}, Identity, Error);
			};
			return Result;
		};
		if (!Add(TreeSemanticId, TreeKind, {}, &EditorData, nullptr, nullptr, {})) return false;
		if (!Add(ItemSemanticId, ItemKind, TEXT("editor_settings"), &EditorData, nullptr, nullptr,
			{{TEXT("kind"), TEXT("settings")}, {TEXT("label"), TEXT("Tree Settings")},
			{TEXT("placement"), TEXT("editor")}, {TEXT("index"), TEXT("0")}})) return false;
		auto TreeOptions = Options(TEXT("editor_settings"), TEXT("editor"));
		TreeOptions.ExcludedProperties = {TEXT("Schema"), TEXT("RootParameterPropertyBag"), TEXT("Evaluators"),
			TEXT("GlobalTasks"), TEXT("SubTrees"), TEXT("EditorBindings"), TEXT("Colors")};
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(&EditorData, TreeOptions, Requests, OutError)) return false;
		for (const FStateTreeEditorColor& Color : EditorData.Colors)
		{
			const FString Key = ColorId(Color.ColorRef.ID);
			if (!Add(ItemSemanticId, ItemKind, Key, nullptr, FStateTreeEditorColor::StaticStruct(), &Color,
				{{TEXT("kind"), TEXT("color")}, {TEXT("index"), TEXT("0")}})) return false;
			auto ColorOptions = Options(Key, TEXT("color"));
			ColorOptions.ExcludedProperties = {TEXT("ColorRef"), TEXT("DisplayName")};
			if (!UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(FStateTreeEditorColor::StaticStruct(), &Color,
				AssetPath + TEXT(".") + Key, &EditorData, ColorOptions, Requests, OutError)) return false;
		}
		if (EditorData.Schema)
		{
			if (!Add(ItemSemanticId, ItemKind, TEXT("schema_settings"), EditorData.Schema, nullptr, nullptr,
				{{TEXT("kind"), TEXT("settings")}, {TEXT("label"), TEXT("Schema Settings")},
				{TEXT("placement"), TEXT("schema")}, {TEXT("index"), TEXT("0")}})) return false;
			if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(EditorData.Schema,
				Options(TEXT("schema_settings"), TEXT("schema")), Requests, OutError)) return false;
		}
		for (const auto& Source : Capture.BindingSources)
			if (!Add(ItemSemanticId, ItemKind, Id(Source.Id), &EditorData, nullptr, nullptr,
				{{TEXT("kind"), TEXT("binding_source")}, {TEXT("label"), Source.Name}, {TEXT("parent"), Id(Source.OwnerId)},
				{TEXT("placement"), Source.Placement}, {TEXT("source_type"), Source.Type->GetPathName()}, {TEXT("index"), TEXT("0")}})) return false;
		if (!Add(ItemSemanticId, ItemKind, Id(EditorData.GetRootParametersGuid()), &EditorData, nullptr, nullptr,
			{{TEXT("kind"), TEXT("parameters")}, {TEXT("label"), TEXT("Root Parameters")},
			{TEXT("parent"), FString()}, {TEXT("index"), TEXT("0")}})) return false;
		auto RootParameters = Options(Id(EditorData.GetRootParametersGuid()), TEXT("parameters"));
		RootParameters.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) {
			return Property->GetFName() == TEXT("RootParameterPropertyBag");
		};
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(&EditorData, RootParameters, Requests, OutError)) return false;
		for (const auto& Entry : Capture.States)
		{
			if (!Add(ItemSemanticId, ItemKind, Id(Entry.State->ID), Entry.State, nullptr, nullptr,
				{{TEXT("kind"), TEXT("state")}, {TEXT("parent"), Entry.Parent ? Id(Entry.Parent->ID) : FString()},
				{TEXT("index"), FString::FromInt(Entry.Order)}})) return false;
			auto StateOptions = Options(Id(Entry.State->ID), TEXT("state"));
			StateOptions.ExcludedProperties = {TEXT("ID"), TEXT("Name"), TEXT("Children"), TEXT("Parent"), TEXT("Tasks"),
				TEXT("SingleTask"), TEXT("EnterConditions"), TEXT("Considerations"), TEXT("Transitions"), TEXT("Parameters"), TEXT("LinkedSubtree"), TEXT("ColorRef")};
			if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Entry.State, StateOptions, Requests, OutError)) return false;
			if (!Add(ItemSemanticId, ItemKind, Id(Entry.State->Parameters.ID), Entry.State, nullptr, nullptr,
				{{TEXT("kind"), TEXT("parameters")}, {TEXT("label"), Entry.State->Name.ToString() + TEXT(" Parameters")},
				{TEXT("parent"), Id(Entry.State->ID)}, {TEXT("index"), TEXT("0")}})) return false;
			auto ParameterOptions = Options(Id(Entry.State->Parameters.ID), TEXT("parameters"));
			ParameterOptions.ExcludedProperties = {TEXT("ID")};
			if (!UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(FStateTreeStateParameters::StaticStruct(),
				&Entry.State->Parameters, AssetPath + TEXT(".parameters:") + Id(Entry.State->ID), Entry.State,
				ParameterOptions, Requests, OutError)) return false;
		}
		for (const auto& Entry : Capture.Nodes)
		{
			if (!Add(ItemSemanticId, ItemKind, Id(Entry.Node->ID), nullptr, FStateTreeEditorNode::StaticStruct(), Entry.Node,
				{{TEXT("kind"), TEXT("node")}, {TEXT("parent"), Id(Entry.OwnerId)}, {TEXT("placement"), PlacementName(Entry.Placement)},
				{TEXT("index"), FString::FromInt(Entry.Order)}})) return false;
			if (!AppendNodePropertyReadRequests(Entry, [&](const FString& Part) { return Options(Id(Entry.Node->ID), Part); }, Requests, OutError)) return false;
			auto EditorOptions = Options(Id(Entry.Node->ID), TEXT("editor"));
			EditorOptions.ExcludedProperties = {TEXT("ID"), TEXT("Node"), TEXT("Instance"), TEXT("InstanceObject")};
			if (!UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(FStateTreeEditorNode::StaticStruct(), Entry.Node,
				AssetPath + TEXT(".node:") + Id(Entry.Node->ID), Entry.NotificationOwner, EditorOptions, Requests, OutError)) return false;
		}
		for (const auto& Entry : Capture.Transitions)
		{
			if (!Add(ItemSemanticId, ItemKind, Id(Entry.Transition->ID), nullptr, FStateTreeTransition::StaticStruct(), Entry.Transition,
				{{TEXT("kind"), TEXT("transition")}, {TEXT("parent"), Id(Entry.State->ID)}, {TEXT("index"), FString::FromInt(Entry.Order)}})) return false;
			auto TransitionOptions = Options(Id(Entry.Transition->ID), TEXT("transition"));
			TransitionOptions.ExcludedProperties = {TEXT("ID"), TEXT("State"), TEXT("Conditions")};
			if (!UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(FStateTreeTransition::StaticStruct(), Entry.Transition,
				AssetPath + TEXT(".transition:") + Id(Entry.Transition->ID), Entry.State, TransitionOptions, Requests, OutError)) return false;
		}
		for (int32 Index = 0; Index < Capture.Bindings.Num(); ++Index)
		{
			const FPropertyBindingBinding& Binding = *Capture.Bindings[Index];
			const FString Key = TEXT("binding:") + Id(Binding.GetTargetPath().GetStructID()) + TEXT(":") + Binding.GetTargetPath().ToString();
			if (!Add(ItemSemanticId, ItemKind, Key, &EditorData, nullptr, &Binding,
				{{TEXT("kind"), TEXT("binding")}, {TEXT("index"), FString::FromInt(Index)}})) return false;
		}
		OutRequests.Append(MoveTemp(Requests));
		return true;
	}

	bool ReadTreeDocument(UStateTree& Tree, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ReadTreeDocument(Tree, OutValue, OutError, nullptr);
	}

	bool ReadTreeDocument(UStateTree& Tree, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError,
		TOptional<UeAgentAuthoringState::FStateSnapshot>* OutStructure)
	{
		OutValue.Reset();
		OutError.Reset();
		const auto Document = MakeShared<FJsonObject>();
		const auto Items = MakeShared<FJsonObject>();
		const auto* Data = Cast<UStateTreeEditorData>(Tree.EditorData);
		Document->SetBoolField(TEXT("has_editor_data"), Data != nullptr);
		Document->SetStringField(TEXT("schema_class"), Data && Data->Schema ? Data->Schema->GetClass()->GetPathName() : FString());
		if (Data)
		{
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			const FString AssetPath = OutStructure ? Tree.GetOutermost()->GetName() : Tree.GetPathName();
			if (!AppendStructuralReadRequests(*Data, AssetPath, Requests, OutError)) return false;
			for (auto& Request : Requests)
				if (Request.SemanticId == ItemPropertySemanticId) Request.Limits.bApplyAuthoringStructFieldPolicy = false;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
			{
				OutError = Capture.Diagnostic;
				return false;
			}
			const auto Entries = Capture.Snapshot.CopyEntries();
			for (const auto& Entry : Entries)
				if (Entry.SemanticId == ItemSemanticId)
					Items->SetField(Entry.TargetIdentity.GetSelector(TEXT("node_id")), UeAgentAuthoringState::CloneCanonicalValue(Entry.Value));
			TMap<FString, TArray<UeAgentAuthoringState::FStateEntry>> Properties;
			UeAgentDetailsAuthoringService::FProjectionPolicy Checkpoint;
			Checkpoint.bRequireEditable = false;
			for (const auto& Entry : Entries)
				if (Entry.SemanticId == ItemPropertySemanticId)
					Properties.FindOrAdd(Entry.TargetIdentity.GetSelector(TEXT("node_id"))).Add(Entry);
			for (const auto& Pair : Properties)
			{
				const TSharedPtr<FJsonObject>* Item = nullptr;
				if (!Items->TryGetObjectField(Pair.Key, Item))
				{
					OutError = DocumentFailure(TEXT("state_tree_property_owner_missing"),
						TEXT("Captured properties have no corresponding structural item in this StateTree."), Tree, ItemPointer(Pair.Key) + TEXT("/properties"));
					OutError.Stage = TEXT("state_capture");
					OutError.SourceKind = TEXT("native_result");
					OutError.SubjectId = Pair.Key;
					if (!Pair.Value.IsEmpty()) OutError.PropertyPath = Pair.Value[0].TargetIdentity.GetSelector(TEXT("property_path"));
					return false;
				}
				TSharedPtr<FJsonObject> PropertyDocument;
				if (!UeAgentDetailsAuthoringService::BuildPropertiesDocument(Pair.Value, Checkpoint, PropertyDocument, OutError)) return false;
				(*Item)->SetObjectField(TEXT("properties"), PropertyDocument);
			}
			if (OutStructure) OutStructure->Emplace(MoveTemp(Capture.Snapshot));
		}
		else if (OutStructure) OutStructure->Emplace();
		Document->SetObjectField(TEXT("items"), Items);
		OutValue = MakeShared<FJsonValueObject>(Document);
		return true;
	}

	bool ApplyTreeDocumentProperties(UStateTree& Tree, const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError,
		UPackage* TextIdentityPackage)
	{
		OutError.Reset();
		const TSharedPtr<FJsonObject>* Items = nullptr;
		if (!Document.IsValid() || Document->Type != EJson::Object)
		{
			OutError = DocumentFailure(TEXT("state_tree_document_items_invalid"), TEXT("The StateTree document must be an object containing an 'items' object."), Tree, TEXT("/"));
			return false;
		}
		if (!Document->AsObject()->TryGetObjectField(TEXT("items"), Items))
		{
			OutError = DocumentFailure(TEXT("state_tree_document_items_invalid"), TEXT("StateTree document field 'items' must be an object."), Tree, TEXT("/items"), Document->AsObject().Get(), TEXT("items"));
			return false;
		}
		auto* Data = Cast<UStateTreeEditorData>(Tree.EditorData);
		if (!Data)
		{
			OutError = DocumentFailure(TEXT("state_tree_data_missing"), TEXT("StateTree properties cannot be applied because the target has no editor data."), Tree, TEXT("/"));
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!AppendStructuralReadRequests(*Data, Tree.GetPathName(), Requests, OutError)) return false;
		TMap<FString, TSharedPtr<FJsonObject>> PropertiesByOwner;
		for (const auto& Item : (*Items)->Values)
		{
			if (!Item.Value.IsValid() || Item.Value->Type != EJson::Object)
			{
				OutError = DocumentFailure(TEXT("state_tree_document_item_invalid"), TEXT("Each StateTree document item must be an object."), Tree, ItemPointer(Item.Key), Items->Get(), Item.Key);
				OutError.PropertyPath = TEXT("items");
				OutError.ActualEvidence = Item.Value;
				return false;
			}
			TSharedPtr<FJsonObject> Properties;
			if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Item.Value->AsObject(), TEXT("properties"), Properties, OutError))
			{
				UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, ItemPointer(Item.Key));
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Item.Value->AsObject(), TEXT("document_apply"), Tree.GetPathName(), TEXT("properties"));
				return false;
			}
			if (Properties) PropertiesByOwner.Add(Item.Key, Properties);
		}
		return UeAgentDetailsAuthoringService::ApplyScopedProperties(PropertiesByOwner, Requests,
			ItemPropertySemanticId, TEXT("node_id"), TEXT("state_tree"), [](const auto& Binding) {
				UeAgentAuthoringObjectApply::FOptions Options;
				Options.bAllowReadOnly = true;
				Options.bShowDefaultsOnly = true;
				Options.bAllowDomainAuthorizedOwners = true;
				Options.bCompactValues = true;
				// Schema settings rebuild derived context; state Type notifications would
				// reset linked parameters before structural reconstruction is complete.
				Options.bEmitEditorNotifications = Binding.RootObject && Binding.RootObject->IsA<UStateTreeSchema>();
				return Options;
			}, OutError, TextIdentityPackage);
	}
}
