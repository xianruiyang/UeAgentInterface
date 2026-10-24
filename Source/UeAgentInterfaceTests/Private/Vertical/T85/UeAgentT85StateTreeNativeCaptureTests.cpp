#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Components/StateTreeAIComponentSchema.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Conditions/StateTreeCommonConditions.h"
#include "Considerations/StateTreeCommonConsiderations.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeNativeCapture.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeDocument.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Serialization/ObjectWriter.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeSchema.h"
#include "GameFramework/Actor.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85StateTreeNativeCaptureTest,
	"UAI.Root.T85.StateTree.NativeCapture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85StateTreeNativeCaptureTest::RunTest(const FString& Parameters)
{
	using namespace UeAgentStateTreeAuthoring;
	auto* Tree = NewObject<UStateTree>(GetTransientPackage());
	auto* Data = NewObject<UStateTreeEditorData>(Tree);
	Tree->EditorData = Data;
	Data->Schema = NewObject<UStateTreeAIComponentSchema>(Data);
	const auto* RootBagProperty = FindFProperty<FStructProperty>(Data->GetClass(), TEXT("RootParameterPropertyBag"));
	if (!TestNotNull(TEXT("native root parameter property"), RootBagProperty)) return false;
	auto* RootBag = RootBagProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Data);
	RootBag->AddProperty(TEXT("Threshold"), EPropertyBagPropertyType::Float);
	RootBag->SetValueFloat(TEXT("Threshold"), 42.5f);
	UStateTreeState& Root = Data->AddSubTree(TEXT("Root"));
	UStateTreeState& Child = Root.AddChildState(TEXT("Child"));
	UStateTreeState& Second = Data->AddSubTree(TEXT("Second"));
	Second.Type = EStateTreeStateType::Subtree;
	UStateTreeState& Linked = Root.AddChildState(TEXT("Linked"));
	Linked.Type = EStateTreeStateType::Linked;
	Linked.LinkedSubtree.ID = Second.ID;
	Linked.LinkedSubtree.Name = Second.Name;
	Linked.LinkedSubtree.LinkType = EStateTreeTransitionType::GotoState;
	Linked.SetLinkedState(Linked.LinkedSubtree);
	Child.bEnabled = false;
	Child.Description = TEXT("StateTree capture marker");
	Child.Parameters.Parameters.AddProperty(TEXT("Speed"), EPropertyBagPropertyType::Float);
	Child.Parameters.Parameters.SetValueFloat(TEXT("Speed"), 245.f);
	Child.Parameters.PropertyOverrides.Add(Child.Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"))->ID);

	UScriptStruct* Delay = FindObject<UScriptStruct>(nullptr, TEXT("/Script/StateTreeModule.StateTreeDelayTask"));
	if (!TestNotNull(TEXT("native Delay task type"), Delay)) return false;
	auto InitializeNode = [](FStateTreeEditorNode& Node, const UScriptStruct* Type) {
		Node.ID = FGuid::NewGuid();
		Node.Node.InitializeAs(Type);
		const UScriptStruct* Instance = Cast<const UScriptStruct>(Node.Node.Get<FStateTreeNodeBase>().GetInstanceDataType());
		if (Instance) Node.Instance.InitializeAs(Instance);
	};
	InitializeNode(Child.Tasks.AddDefaulted_GetRef(), Delay);
	InitializeNode(Second.SingleTask, Delay);
	InitializeNode(Data->GlobalTasks.AddDefaulted_GetRef(), Delay);
	InitializeNode(Data->Evaluators.AddDefaulted_GetRef(), FStateTreeEvaluatorBase::StaticStruct());
	Child.AddEnterCondition<FStateTreeCompareIntCondition>().GetInstanceData().Right = 19;
	InitializeNode(Child.Considerations.AddDefaulted_GetRef(), FStateTreeConstantConsideration::StaticStruct());
	Child.Considerations[0].Instance.GetMutable<FStateTreeConstantConsiderationInstanceData>().Constant = .75f;
	Child.Tasks[0].Node.GetMutable<FStateTreeTaskBase>().bTaskEnabled = false;
	FFloatProperty* Duration = FindFProperty<FFloatProperty>(Child.Tasks[0].Instance.GetScriptStruct(), TEXT("Duration"));
	if (!TestNotNull(TEXT("native Delay instance field"), Duration)) return false;
	Duration->SetPropertyValue_InContainer(Child.Tasks[0].Instance.GetMutableMemory(), 7.25f);
	FStateTreeTransition& Transition = Child.AddTransition(EStateTreeTransitionTrigger::OnStateCompleted,
		EStateTreeTransitionType::GotoState, &Second);
	Transition.bTransitionEnabled = false;
	Transition.bDelayTransition = true;
	Transition.DelayDuration = 2.5f;
	Transition.AddCondition<FStateTreeCompareIntCondition>().GetInstanceData().Right = 37;
	if (!TestTrue(TEXT("native property binding"), Data->AddPropertyBinding(
		Child.Tasks[0], TEXT("Duration"), Second.SingleTask, TEXT("Duration")))) return false;
	Data->AddPropertyBinding(FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Threshold")),
		FPropertyBindingPath(Child.Tasks[0].ID, TEXT("Duration")));

	// A binding visitor would prepare this event payload; capture must leave it untouched.
	Child.bHasRequiredEventToEnter = true;
	Child.RequiredEventToEnter.PayloadStruct = FStateTreeCompareIntConditionInstanceData::StaticStruct();
	FPropertyBindingPath EventPath(Child.GetEventID());
	if (!TestTrue(TEXT("native event payload path"), EventPath.FromString(TEXT("Payload.Right")))) return false;
	EventPath.GetMutableSegments()[0].SetInstanceStruct(FStateTreeCompareIntConditionInstanceData::StaticStruct());
	Data->AddPropertyBinding(EventPath, FPropertyBindingPath(Child.EnterConditions[0].ID, TEXT("Right")));
	const auto Contexts = Data->Schema->GetContextDataDescs();
	const FStateTreeExternalDataDesc* ActorContext = nullptr;
	for (const auto& Context : Contexts)
		if (Context.Struct && Context.Struct->IsChildOf(AActor::StaticClass())) { ActorContext = &Context; break; }
	if (!TestNotNull(TEXT("native actor context source"), ActorContext)) return false;
	Data->AddPropertyBinding(FPropertyBindingPath(ActorContext->ID, TEXT("CustomTimeDilation")),
		FPropertyBindingPath(Data->GlobalTasks[0].ID, TEXT("Duration")));
	auto StateBytes = [&]() {
		TArray<uint8> Bytes;
		for (UObject* Object : TArray<UObject*>{Tree, Data, &Root, &Child, &Second, &Linked})
		{
			TArray<uint8> Part;
			FObjectWriter Writer(Object, Part);
			Bytes.Append(Part);
		}
		return Bytes;
	};
	const TArray<uint8> Before = StateBytes();
	FTreeCapture Capture;
	UE::UAI::Contract::FDiagnostic Error;
	const bool bCaptured = CaptureTree(*Data, Capture, Error);
	if (!TestTrue(*FString::Printf(TEXT("native capture: %s"), *Error.ToPublicDetail()), bCaptured)) return false;
	TestEqual(TEXT("multiple roots, child and linked state retained"), Capture.States.Num(), 4);
	TestTrue(TEXT("native hierarchy and sibling positions retained"),
		Capture.States[0].State == &Root && Capture.States[0].Order == 0
		&& Capture.States[1].State == &Child && Capture.States[1].Parent == &Root
		&& Capture.States[2].State == &Linked && Capture.States[2].Parent == &Root && Capture.States[2].Order == 1
		&& Capture.States[3].State == &Second && Capture.States[3].Order == 1);
	TestEqual(TEXT("all populated node placements retained"), Capture.Nodes.Num(), 7);
	TestEqual(TEXT("disabled transition retained"), Capture.Transitions.Num(), 1);
	TestTrue(TEXT("transition retains native destination, condition and delay"),
		Capture.Transitions[0].Transition == &Transition && Transition.State.ID == Second.ID
		&& Transition.Conditions[0].Instance.Get<FStateTreeCompareIntConditionInstanceData>().Right == 37
		&& Transition.DelayDuration == 2.5f && !Transition.bTransitionEnabled);
	TestEqual(TEXT("node, root parameter, event and context bindings retained"), Capture.Bindings.Num(), 4);
	TestEqual(TEXT("static context and event sources captured without runtime values"), Capture.BindingSources.Num(), Contexts.Num() + 1);
	TArray<UeAgentAuthoringState::FReadRequest> Requests;
	for (const FNodeCapture& Node : Capture.Nodes)
	{
		if (!TestTrue(*FString::Printf(TEXT("node property requests: %s"), *Error.ToPublicDetail()),
			AppendNodePropertyReadRequests(Node, [](const FString&) {
				return UeAgentAuthoringState::FEditablePropertyReadOptions{};
			}, Requests, Error))) return false;
	}
	const auto* TaskDuration = Requests.FindByPredicate([&](const auto& Request) {
		return Request.RootData == Child.Tasks[0].Instance.GetMemory() && Request.PropertyPath == TEXT("Duration");
	});
	const auto* SingleDuration = Requests.FindByPredicate([&](const auto& Request) {
		return Request.RootData == Second.SingleTask.Instance.GetMemory() && Request.PropertyPath == TEXT("Duration");
	});
	if (!TestNotNull(TEXT("task instance bound to public property reader"), TaskDuration)
		|| !TestNotNull(TEXT("single task instance bound separately"), SingleDuration)) return false;
	TestTrue(TEXT("same property in different nodes has distinct identity"),
		TaskDuration->TargetIdentity.CanonicalKey != SingleDuration->TargetIdentity.CanonicalKey);
	TestTrue(TEXT("task property notification belongs to its native state"), TaskDuration->NotificationOwner == &Child);
	UeAgentAuthoringState::FCaptureResult Properties;
	const bool bProperties = UeAgentAuthoringState::CaptureSnapshot(Requests, Properties);
	if (!TestTrue(*FString::Printf(TEXT("common property capture: %s %s"),
		*Properties.Diagnostic.Code, *Properties.Diagnostic.InternalContext), bProperties)) return false;
	UeAgentAuthoringState::FStateEntry DurationEntry;
	if (!TestTrue(TEXT("captured duration entry"), Properties.Snapshot.FindEntry(TaskDuration->SemanticId,
		TaskDuration->TargetIdentity.CanonicalKey, DurationEntry))
		|| !TestTrue(TEXT("captured native property storage"), DurationEntry.NativeValue.IsValid())) return false;
	const auto* CapturedFloat = CastField<FFloatProperty>(DurationEntry.NativeValue->GetProperty());
	if (!TestNotNull(TEXT("captured duration type"), CapturedFloat)) return false;
	TestEqual(TEXT("nondefault task duration captured"), CapturedFloat->GetPropertyValue(DurationEntry.NativeValue->GetValuePtr()), 7.25f);
	const FString FrozenCanonical = UeAgentAuthoringState::MakeStableCanonicalValueKey(DurationEntry.Value);
	Duration->SetPropertyValue_InContainer(Child.Tasks[0].Instance.GetMutableMemory(), 8.5f);
	TestEqual(TEXT("snapshot native value is frozen, not a borrowed node pointer"),
		CapturedFloat->GetPropertyValue(DurationEntry.NativeValue->GetValuePtr()), 7.25f);
	TestEqual(TEXT("canonical value remains frozen after native edit"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(DurationEntry.Value), FrozenCanonical);
	Duration->SetPropertyValue_InContainer(Child.Tasks[0].Instance.GetMutableMemory(), 7.25f);
	TArray<UeAgentAuthoringState::FReadRequest> ExportRequests;
	const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("state_tree"));
	if (!TestNotNull(TEXT("StateTree profile"), Profile)) return false;
	const bool bExportRequests = UeAgentReflectionAuthoring::RegisterAuthoring(Error)
		&& UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, *Tree, ExportRequests, Error);
	if (!TestTrue(*FString::Printf(TEXT("ordinary export requests: %s"), *Error.ToPublicDetail()), bExportRequests)) return false;
	UeAgentAuthoringState::FCaptureResult ExportCapture;
	const bool bExportCapture = UeAgentAuthoringState::CaptureSnapshot(ExportRequests, ExportCapture);
	if (!TestTrue(*FString::Printf(TEXT("structural export snapshot: %s %s"),
		*ExportCapture.Diagnostic.Code, *ExportCapture.Diagnostic.InternalContext), bExportCapture)) return false;
	int32 ItemCount = 0;
	for (const auto& Entry : ExportCapture.Snapshot.CopyEntries()) if (Entry.SemanticId == ItemSemanticId) ++ItemCount;
	TestEqual(TEXT("all structure, default palette, settings, parameter collections, binding sources and bindings reach export"), ItemCount, 25 + Contexts.Num());
	TSharedPtr<FJsonValue> TreeDocument;
	if (!TestTrue(*FString::Printf(TEXT("domain document: %s"), *Error.ToPublicDetail()), ReadTreeDocument(*Tree, TreeDocument, Error))) return false;
	const auto DocumentItems = TreeDocument->AsObject()->GetObjectField(TEXT("items"));
	TestEqual(TEXT("domain document preserves all captured items"), DocumentItems->Values.Num(), ItemCount);
	TestTrue(TEXT("editor settings have common properties"), DocumentItems->GetObjectField(TEXT("editor_settings"))->HasField(TEXT("properties")));
	TestTrue(TEXT("schema settings have common properties"), DocumentItems->GetObjectField(TEXT("schema_settings"))->HasField(TEXT("properties")));
	const FString FrozenDocument = UeAgentAuthoringState::MakeStableCanonicalValueKey(TreeDocument);
	TestTrue(TEXT("domain document retains nondefault node and parameter values"), FrozenDocument.Contains(TEXT("7.25"))
		&& FrozenDocument.Contains(TEXT("245")) && FrozenDocument.Contains(TEXT("42.5")));
	Duration->SetPropertyValue_InContainer(Child.Tasks[0].Instance.GetMutableMemory(), 8.5f);
	TestEqual(TEXT("domain document has no borrowed native values"), UeAgentAuthoringState::MakeStableCanonicalValueKey(TreeDocument), FrozenDocument);
	Duration->SetPropertyValue_InContainer(Child.Tasks[0].Instance.GetMutableMemory(), 7.25f);
	int32 RestoredBindings = 0;
	for (const auto& Entry : ExportCapture.Snapshot.CopyEntries())
	{
		if (Entry.SemanticId != ItemSemanticId || !Entry.Value.IsValid()) continue;
		const auto Value = Entry.Value->AsObject();
		if (!Value.IsValid() || Value->GetStringField(TEXT("kind")) != TEXT("binding")) continue;
		FPropertyBindingPath Source, Target;
		if (!TestTrue(*FString::Printf(TEXT("decode captured binding paths: %s"), *Error.ToPublicDetail()),
			UeAgentPropertyValueCodec::DecodeStructValue(FPropertyBindingPath::StaticStruct(),
				Value->TryGetField(TEXT("source_path_state")), &Source, Error)
			&& UeAgentPropertyValueCodec::DecodeStructValue(FPropertyBindingPath::StaticStruct(),
				Value->TryGetField(TEXT("target_path_state")), &Target, Error))) return false;
		const auto* Original = Capture.Bindings.FindByPredicate([&](const auto* Binding) {
			return Binding->GetTargetPath() == Target;
		});
		if (!TestNotNull(TEXT("decoded binding target matches native binding"), Original)) return false;
		TestTrue(TEXT("source path identity and segments survive common codec"), (*Original)->GetSourcePath() == Source);
		if (Source.GetStructID() == Child.GetEventID())
		{
			TestTrue(TEXT("event payload instance type retained"),
				Source.GetSegments()[0].GetInstanceStruct() == FStateTreeCompareIntConditionInstanceData::StaticStruct());
			TestEqual(TEXT("event payload access mode retained"), Source.GetSegments()[0].GetInstancedStructAccessType(),
				EventPath.GetSegments()[0].GetInstancedStructAccessType());
		}
		++RestoredBindings;
	}
	TestEqual(TEXT("every captured binding can be restored without readable-path parsing"), RestoredBindings, 4);
	using namespace UE::UAI;
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
	HjsonWorkspace::FAuthoringModelPtr Model;
	HjsonWorkspace::FWorkspaceError WorkspaceError;
	HjsonWorkspace::FExportWorkspacePlanPtr Plan;
	const bool bPlan = HjsonWorkspace::TryBuildCanonicalAuthoringCapture(
		HjsonWorkspace::ResolveTargetCompositionKind(Profile->TargetKind, Profile->TypeId), TEXT("llm_authoring"),
		ExportCapture.Snapshot, Resources, CanonicalSnapshot, Model, WorkspaceError)
		&& HjsonWorkspace::TryBuildCompositionExportPlan(Model.ToSharedRef(), {}, Resources, Plan, WorkspaceError);
	if (!TestTrue(*FString::Printf(TEXT("ordinary export plan: %s"), *WorkspaceError.GetCode()), bPlan)) return false;
	FString Ordinary;
	for (const auto& File : Plan->GetFiles())
	{
		if (File.GetRelativePath().StartsWith(TEXT(".uai/"))) continue;
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()), File.GetCanonicalUtf8().Num());
		Ordinary += FString(Text.Length(), Text.Get());
	}
	TestTrue(TEXT("ordinary hierarchy and instance value projected"), Ordinary.Contains(TEXT("tree_items"))
		&& Ordinary.Contains(TEXT("Root")) && Ordinary.Contains(TEXT("Second")) && Ordinary.Contains(TEXT("Child"))
		&& Ordinary.Contains(TEXT("Transition")) && Ordinary.Contains(TEXT("7.25")));
	TestTrue(TEXT("state parameters and readable overrides projected"), Ordinary.Contains(TEXT("245"))
		&& Ordinary.Contains(TEXT("Speed")) && Ordinary.Contains(TEXT("parameter_overrides")));
	TestTrue(TEXT("root parameters and binding endpoints projected"), Ordinary.Contains(TEXT("42.5"))
		&& Ordinary.Contains(TEXT("Threshold")) && Ordinary.Contains(TEXT("source_path"))
		&& Ordinary.Contains(TEXT("target_path")) && Ordinary.Contains(TEXT("Binding")));
	TestTrue(TEXT("event, context and linked subtree references projected"), Ordinary.Contains(TEXT("Payload.Right"))
		&& Ordinary.Contains(TEXT("CustomTimeDilation")) && Ordinary.Contains(TEXT("linked_state"))
		&& Ordinary.Contains(TEXT("Linked")) && Ordinary.Contains(TEXT("Enter Event")));
	TestFalse(TEXT("native source path metadata stays outside ordinary files"), Ordinary.Contains(TEXT("source_path_state")));
	TestFalse(TEXT("native target path metadata stays outside ordinary files"), Ordinary.Contains(TEXT("target_path_state")));
	TestTrue(TEXT("editor and schema settings reach ordinary export"), Ordinary.Contains(TEXT("GlobalTasksCompletion"))
		&& Ordinary.Contains(TEXT("Schema Settings")) && Ordinary.Contains(TEXT("Tree Settings")));
	for (const auto& Source : Capture.BindingSources)
		TestFalse(TEXT("binding source identity stays hidden"), Ordinary.Contains(Source.Id.ToString(EGuidFormats::DigitsWithHyphensLower)));
	TestFalse(TEXT("root parameter collection identity stays hidden"),
		Ordinary.Contains(Data->GetRootParametersGuid().ToString(EGuidFormats::DigitsWithHyphensLower)));
	for (const auto& Entry : Capture.States)
		TestFalse(TEXT("state parameter collection identity stays hidden"),
			Ordinary.Contains(Entry.State->Parameters.ID.ToString(EGuidFormats::DigitsWithHyphensLower)));
	TestFalse(TEXT("parameter field identity remains hidden"), Ordinary.Contains(
		Child.Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"))->ID.ToString(EGuidFormats::DigitsWithHyphensLower)));
	for (const auto& Entry : Capture.States)
		TestFalse(TEXT("state identities remain outside ordinary files"), Ordinary.Contains(Entry.State->ID.ToString(EGuidFormats::DigitsWithHyphensLower)));
	for (const auto& Entry : Capture.Nodes)
		TestFalse(TEXT("node identities remain outside ordinary files"), Ordinary.Contains(Entry.Node->ID.ToString(EGuidFormats::DigitsWithHyphensLower)));
	TestTrue(TEXT("capture does not mutate serialized native state or temporary event payload"), StateBytes() == Before);
	Root.Children.Add(&Root);
	TestFalse(TEXT("invalid cyclic hierarchy rejected without partial capture"), CaptureTree(*Data, Capture, Error));
	TestTrue(TEXT("failed capture exposes no partial tree"), Capture.States.IsEmpty() && Capture.Nodes.IsEmpty());
	Root.Children.Pop();
	const FGuid SpeedId = Child.Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"))->ID;
	FInstancedPropertyBag DesiredParameters = Child.Parameters.Parameters;
	DesiredParameters.SetValueFloat(TEXT("Speed"), 310.f);
	const auto Descriptor = MakeShared<FJsonObject>();
	Descriptor->SetStringField(TEXT("property_path"), TEXT("Parameters"));
	Descriptor->SetField(TEXT("value"), UeAgentPropertyValueCodec::EncodePropertyBag(DesiredParameters, &Child, {}));
	UeAgentAuthoringObjectApply::FOptions ApplyOptions;
	ApplyOptions.bShowDefaultsOnly = true;
	UeAgentAuthoringObjectApply::FPreparedApply Prepared;
	UeAgentAuthoringObjectApply::FFailure Failure;
	const bool bPrepared = UeAgentAuthoringObjectApply::PrepareStruct(FStateTreeStateParameters::StaticStruct(),
		&Child.Parameters, &Child, {MakeShared<FJsonValueObject>(Descriptor)}, ApplyOptions, Prepared, Failure);
	if (!TestTrue(*FString::Printf(TEXT("prepare parameter edit: %s %s"), *Failure.Diagnostic.Code, *Failure.Diagnostic.InternalContext), bPrepared)) return false;
	TestEqual(TEXT("prepare leaves original parameter value unchanged"), Child.Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 245.f);
	UeAgentAuthoringObjectApply::FApplyResult Applied;
	if (!TestTrue(TEXT("apply parameter edit"), UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Applied))) return false;
	TestEqual(TEXT("parameter value changed"), Child.Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 310.f);
	TestEqual(TEXT("parameter descriptor identity retained"), Child.Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"))->ID, SpeedId);
	TestTrue(TEXT("native StateTree parameter override remains effective"),
		Child.IsParametersPropertyOverridden(Child.Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"))->ID));
	TestTrue(TEXT("verify parameter edit"), UeAgentAuthoringObjectApply::VerifyPrepared(Prepared, Applied));
	TestTrue(TEXT("rollback parameter edit"), UeAgentAuthoringObjectApply::RollbackPrepared(Prepared, Applied));
	TestEqual(TEXT("rollback restores original parameter value"), Child.Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 245.f);
	TestEqual(TEXT("rollback retains parameter descriptor identity"), Child.Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"))->ID, SpeedId);
	const auto EditedDocument = UeAgentAuthoringState::CloneCanonicalValue(TreeDocument);
	const auto TaskProperties = EditedDocument->AsObject()->GetObjectField(TEXT("items"))
		->GetObjectField(Child.Tasks[0].ID.ToString(EGuidFormats::DigitsWithHyphensLower))->GetObjectField(TEXT("properties"));
	bool bFoundDuration = false;
	for (const auto& Property : TaskProperties->GetArrayField(TEXT("properties")))
	{
		if (Property->AsObject()->GetStringField(TEXT("property_path")) != TEXT("instance.Duration")) continue;
		const float DesiredDuration = 9.75f;
		Property->AsObject()->SetField(TEXT("value"), UeAgentPropertyValueCodec::Encode(Duration, &DesiredDuration, &Child, {}));
		bFoundDuration = true;
	}
	if (!TestTrue(TEXT("domain document contains scoped task instance property"), bFoundDuration)) return false;
	if (!TestTrue(*FString::Printf(TEXT("apply domain document properties: %s"), *Error.ToPublicDetail()),
		ApplyTreeDocumentProperties(*Tree, EditedDocument, Error))) return false;
	TestEqual(TEXT("domain property apply reaches native task instance"),
		Duration->GetPropertyValue_InContainer(Child.Tasks[0].Instance.GetMemory()), 9.75f);
	TestEqual(TEXT("domain property apply retains state parameter identity"),
		Child.Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"))->ID, SpeedId);
	TestEqual(TEXT("domain property apply retains binding collection"), Data->EditorBindings.GetNumBindings(), 4);
	if (!TestTrue(*FString::Printf(TEXT("restore original domain properties: %s"), *Error.ToPublicDetail()),
		ApplyTreeDocumentProperties(*Tree, TreeDocument, Error))) return false;
	TestEqual(TEXT("original domain document restores task instance"),
		Duration->GetPropertyValue_InContainer(Child.Tasks[0].Instance.GetMemory()), 7.25f);
	const auto ReducedDocument = UeAgentAuthoringState::CloneCanonicalValue(TreeDocument);
	const auto ReducedItems = ReducedDocument->AsObject()->GetObjectField(TEXT("items"));
	const FGuid LinkedId = Linked.ID;
	const FGuid TaskId = Child.Tasks[0].ID;
	const FString TaskKey = TaskId.ToString(EGuidFormats::DigitsWithHyphensLower);
	ReducedItems->RemoveField(LinkedId.ToString(EGuidFormats::DigitsWithHyphensLower));
	ReducedItems->RemoveField(Linked.Parameters.ID.ToString(EGuidFormats::DigitsWithHyphensLower));
	ReducedItems->RemoveField(TaskKey);
	TArray<FString> RemovedBindings;
	for (const auto& Item : ReducedItems->Values)
	{
		const auto Spec = Item.Value->AsObject();
		FString Source, Target;
		if (Spec->GetStringField(TEXT("kind")) == TEXT("binding")
			&& ((Spec->TryGetStringField(TEXT("source"), Source) && Source == TaskKey)
				|| (Spec->TryGetStringField(TEXT("target"), Target) && Target == TaskKey))) RemovedBindings.Add(Item.Key);
	}
	for (const auto& Key : RemovedBindings) ReducedItems->RemoveField(Key);
	const auto InvalidDocument = UeAgentAuthoringState::CloneCanonicalValue(TreeDocument);
	InvalidDocument->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(Child.ID.ToString(EGuidFormats::DigitsWithHyphensLower))
		->RemoveField(TEXT("parameter_overrides"));
	const auto BeforeInvalidApply = StateBytes();
	TestFalse(TEXT("missing required state field rejected before application"), ApplyTreeDocument(*Tree, InvalidDocument, Error));
	TestEqual(TEXT("missing field has a domain validation error"), Error.Code, FString(TEXT("state_tree_document_state_fields_invalid")));
	TestTrue(TEXT("invalid document leaves native objects unchanged"), StateBytes() == BeforeInvalidApply);
	if (!TestTrue(*FString::Printf(TEXT("apply reduced tree structure: %s"), *Error.ToPublicDetail()), ApplyTreeDocument(*Tree, ReducedDocument, Error))) return false;
	TestEqual(TEXT("tree document removes child state"), Root.Children.Num(), 1);
	TestTrue(TEXT("tree document removes task and its bindings"), Child.Tasks.IsEmpty() && Data->EditorBindings.GetNumBindings() == 2);
	if (!TestTrue(*FString::Printf(TEXT("restore full tree structure: %s"), *Error.ToPublicDetail()), ApplyTreeDocument(*Tree, TreeDocument, Error))) return false;
	TestEqual(TEXT("tree document recreates deleted child"), Root.Children.Num(), 2);
	TestEqual(TEXT("recreated child keeps its original identity"), Root.Children[1]->ID, LinkedId);
	TestEqual(TEXT("recreated linked child targets original subtree"), Root.Children[1]->LinkedSubtree.ID, Second.ID);
	if (!TestEqual(TEXT("task recreated"), Child.Tasks.Num(), 1)) return false;
	TestEqual(TEXT("recreated task retains native identity"), Child.Tasks[0].ID, TaskId);
	TestEqual(TEXT("recreated task restores nondefault instance property"),
		Duration->GetPropertyValue_InContainer(Child.Tasks[0].Instance.GetMemory()), 7.25f);
	TSharedPtr<FJsonValue> RestoredDocument;
	if (!TestTrue(TEXT("capture restored full tree"), ReadTreeDocument(*Tree, RestoredDocument, Error))) return false;
	TestEqual(TEXT("full tree document restored independently"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(RestoredDocument), FrozenDocument);
	return !HasAnyErrors();
}
