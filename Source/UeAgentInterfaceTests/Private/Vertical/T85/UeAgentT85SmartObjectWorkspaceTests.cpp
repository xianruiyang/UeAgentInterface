#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Annotations/SmartObjectSlotEntranceAnnotation.h"
#include "Annotations/SmartObjectSlotLinkAnnotation.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectDocument.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectAuthoringState.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectNativeCapture.h"
#include "GameplayInteractionSmartObjectBehaviorDefinition.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PropertyBindingBindingCollectionOwner.h"
#include "SmartObjectDefinition.h"
#include "StateTree.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "WorldConditionQuery.h"
#include "WorldConditions/SmartObjectWorldConditionObjectTagQuery.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SmartObjectWorkspaceTest,
	"UAI.Root.T85.SmartObject.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SmartObjectWorkspaceTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Name = TEXT("SmartObject_Workspace_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SmartObjectWorkspace/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString StateName = Name + TEXT("_State");
	const FString StatePackageName = PackageName + TEXT("_State");
	const FString StateObjectPath = StatePackageName + TEXT(".") + StateName;
	const FString OtherStateName = StateName + TEXT("_Edited");
	const FString OtherStatePackageName = StatePackageName + TEXT("_Edited");
	const FString OtherStateObjectPath = OtherStatePackageName + TEXT(".") + OtherStateName;
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		if (UObject* Asset = LoadObject<UObject>(nullptr, *StateObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		if (UObject* Asset = LoadObject<UObject>(nullptr, *OtherStateObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* StatePackage = CreatePackage(*StatePackageName);
	auto* StateTree = NewObject<UStateTree>(StatePackage, *StateName, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(StateTree);
	UPackage* OtherStatePackage = CreatePackage(*OtherStatePackageName);
	auto* OtherStateTree = NewObject<UStateTree>(OtherStatePackage, *OtherStateName, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(OtherStateTree);
	UPackage* Package = CreatePackage(*PackageName);
	auto* Definition = NewObject<USmartObjectDefinition>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Definition);
	Definition->DebugAddSlot();
	auto& Slot = Definition->GetMutableSlot(0);
	Slot.ID = FGuid::NewGuid(); Slot.Name = TEXT("Interaction"); Slot.bEnabled = false;
	Slot.Offset = FVector3f(21.0f, -13.0f, 7.0f);
	const FGuid SlotId = Slot.ID;
	auto& OriginalData = Slot.DefinitionData.AddDefaulted_GetRef(); OriginalData.ID = FGuid::NewGuid();
	const FGuid OriginalDataId = OriginalData.ID;
	OriginalData.Data.InitializeAs<FSmartObjectSlotEntranceAnnotation>();
	OriginalData.Data.GetMutable<FSmartObjectSlotEntranceAnnotation>().Offset = FVector3f(5,8,13);
	Slot.SelectionPreconditions.SetSchemaClass(Definition->GetWorldConditionSchemaClass());
	Slot.SelectionPreconditions.Initialize(Definition);
	auto* Behavior = NewObject<UGameplayInteractionSmartObjectBehaviorDefinition>(Definition, NAME_None, RF_Transactional);
	Behavior->SetStateTree(StateTree);
	Slot.BehaviorDefinitions.Add(Behavior);
	const auto* ParametersProperty = FindFProperty<FStructProperty>(Definition->GetClass(), TEXT("Parameters"));
	const auto* ParametersIdProperty = FindFProperty<FStructProperty>(Definition->GetClass(), TEXT("ParametersID"));
	const auto* BindingsProperty = FindFProperty<FStructProperty>(Definition->GetClass(), TEXT("BindingCollection"));
	if (!ParametersProperty || !ParametersIdProperty || !BindingsProperty) return false;
	auto& ParameterBag = *ParametersProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Definition);
	FPropertyBagPropertyDesc DrawSizeDesc(TEXT("DrawSize"), EPropertyBagPropertyType::Float);
	DrawSizeDesc.ID = FGuid::NewGuid();
	DrawSizeDesc.MetaData.Add({TEXT("ToolTip"), TEXT("Retained native parameter metadata")});
	ParameterBag.AddProperties({DrawSizeDesc});
	ParameterBag.SetValueFloat(TEXT("DrawSize"), 12.5f);
	const FGuid ParameterId = *ParametersIdProperty->ContainerPtrToValuePtr<FGuid>(Definition);
	const FPropertyBindingPath BindingSource(ParameterId, TEXT("DrawSize"));
	const FPropertyBindingPath BindingTarget(SlotId, TEXT("DEBUG_DrawSize"));
	auto& Bindings = *BindingsProperty->ContainerPtrToValuePtr<FSmartObjectBindingCollection>(Definition);
	Bindings.AddBindableStruct(FSmartObjectDefinitionBindableStructDescriptor(
		TEXT("Parameters"), ParameterBag.GetPropertyBagStruct(), ParameterId, FSmartObjectDefinitionDataHandle::Parameters));
	Bindings.AddBindableStruct(FSmartObjectDefinitionBindableStructDescriptor(
		TEXT("Interaction"), FSmartObjectSlotDefinition::StaticStruct(), SlotId, FSmartObjectDefinitionDataHandle(0)));
	Bindings.AddSmartObjectBinding(FSmartObjectDefinitionPropertyBinding(BindingSource, BindingTarget));
	static_cast<IPropertyBindingBindingCollectionOwner&>(*Definition).OnPropertyBindingChanged(BindingSource, BindingTarget);
	if (!TestEqual(TEXT("native fixture binding copies initial parameter"), Definition->GetSlots()[0].DEBUG_DrawSize, 12.5f)) return false;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString StateFilename = FPackageName::LongPackageNameToFilename(StatePackageName, FPackageName::GetAssetPackageExtension());
	const FString OtherStateFilename = FPackageName::LongPackageNameToFilename(OtherStatePackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(StatePackage, StateTree, *StateFilename, Save)
		|| !UPackage::SavePackage(OtherStatePackage, OtherStateTree, *OtherStateFilename, Save)
		|| !UPackage::SavePackage(Package, Definition, *Filename, Save)) return false;
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
		/ TEXT("tmp/TROOT-UAI-208-graph-common-foundation/smart-object-common-r1/workspace") / Name);
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName); Export->SetStringField(TEXT("workspace_path"), Workspace);
	FString Params, Error, Payload; HjsonWorkspace::FWorkspaceError Failure; bool ReadOnly = false;
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"), Params, Payload, ReadOnly, Failure))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	TestTrue(TEXT("ordinary SmartObject export is read-only"), ReadOnly && !Package->IsDirty());
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	auto PrepareImport = [&]() {
		const auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved; FString Revision, Authorization;
		if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources,
			Resolved, Revision, Authorization, Failure)) return false;
		const auto Import = MakeShared<FJsonObject>(); Import->SetStringField(TEXT("target"), PackageName);
		Import->SetStringField(TEXT("expected_revision"), Revision); Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
		if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
		return CommandContract::Json::TryCanonicalize(Import, Params, Error);
	};
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	bool SlotEdited = false, BehaviorObserved = false, BehaviorEdited = false, ParameterEdited = false;
	for (const FString& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text; HjsonWorkspace::FHjsonDocumentPtr Parsed; TSharedPtr<FJsonValue> Value;
		if (!FFileHelper::LoadFileToString(Text, *File)
			|| !HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, Failure)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Value, Failure)) return false;
		if (!Value.IsValid() || Value->Type != EJson::Object) continue;
		bool FileChanged = false;
		TFunction<void(const TSharedPtr<FJsonValue>&)> Visit = [&](const TSharedPtr<FJsonValue>& Node)
		{
			if (!Node.IsValid()) return;
			if (Node->Type == EJson::Array)
			{
				for (const auto& Child : Node->AsArray()) Visit(Child);
				return;
			}
			if (Node->Type != EJson::Object) return;
			for (const auto& Field : Node->AsObject()->Values)
			{
				if (Field.Key == TEXT("slot.bEnabled") && Field.Value->Type == EJson::Object)
				{
					Field.Value->AsObject()->SetBoolField(TEXT("value"), true);
					SlotEdited = true; FileChanged = true;
				}
				if (Field.Key == TEXT("behavior.StateTreeReference"))
				{
					BehaviorObserved |= UeAgentAuthoringState::MakeStableCanonicalValueKey(Field.Value).Contains(StatePackageName);
					Field.Value->AsObject()->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))
						->SetStringField(TEXT("StateTree"), OtherStateObjectPath);
					BehaviorEdited = true; FileChanged = true;
				}
				if (Field.Key == TEXT("parameters.Parameters"))
				{
					Field.Value->AsObject()->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))
						->SetNumberField(TEXT("DrawSize"), 42.5);
					ParameterEdited = true; FileChanged = true;
				}
				Visit(Field.Value);
			}
		};
		Visit(Value);
		if (FileChanged && !CommandContract::Json::TryCanonicalize(Value->AsObject(), Text, Error)) return false;
		if (FileChanged && !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	}
	if (!TestTrue(TEXT("ordinary workspace edits slot, owned behavior and parameter properties"), SlotEdited && BehaviorObserved && BehaviorEdited && ParameterEdited)) return false;
	if (!PrepareImport()) return false;
	TWeakObjectPtr<USmartObjectDefinition> Before = Definition;
	int32 SlotNotifications = 0, BehaviorNotifications = 0, ParameterNotifications = 0;
	const FDelegateHandle NotificationHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
		[&](UObject* Object, FPropertyChangedEvent& Event) {
			if (!Object || !Event.MemberProperty || Event.ChangeType != EPropertyChangeType::ValueSet) return;
			if (Object->GetPathName() == ObjectPath && Event.MemberProperty->GetFName() == TEXT("Slots")) ++SlotNotifications;
			if (Object->GetPathName() == ObjectPath && Event.MemberProperty->GetFName() == TEXT("Parameters")) ++ParameterNotifications;
			if (Object->GetOuter() && Object->GetOuter()->GetPathName() == ObjectPath
				&& Object->IsA<UGameplayInteractionSmartObjectBehaviorDefinition>()
				&& Event.MemberProperty->GetFName() == TEXT("StateTreeReference")) ++BehaviorNotifications;
		});
	ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(NotificationHandle); };
	Definition = nullptr; Behavior = nullptr; StateTree = nullptr; OtherStateTree = nullptr; Package = nullptr;
	if (!DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
		PackageName, TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"), Params, Payload, ReadOnly, Failure))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetSourceUnit() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	TestTrue(TEXT("real slot, owned behavior and parameter edits dispatch native notifications"), SlotNotifications > 0 && BehaviorNotifications > 0 && ParameterNotifications > 0);
	AddInfo(FString::Printf(TEXT("native_notifications slots=%d owned_behavior=%d parameters=%d"), SlotNotifications, BehaviorNotifications, ParameterNotifications));
	Definition = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("import unloads and reloads a clean persisted definition"),
		!Before.IsValid() && Definition && !Definition->GetPackage()->IsDirty()
		&& Definition->GetSlots().Num() == 1 && Definition->GetSlots()[0].bEnabled)) return false;
	const auto* ReloadedBehavior = Definition->GetSlots()[0].BehaviorDefinitions.Num()
		? Cast<UGameplayInteractionSmartObjectBehaviorDefinition>(Definition->GetSlots()[0].BehaviorDefinitions[0]) : nullptr;
	TestTrue(TEXT("owned behavior StateTree edit survives ordinary import and reload"),
		ReloadedBehavior && ReloadedBehavior->GetStateTree() && ReloadedBehavior->GetStateTree()->GetPathName() == OtherStateObjectPath);
	TestTrue(TEXT("unedited slot identity, name and nondefault offset survive"),
		Definition->GetSlots()[0].ID == SlotId && Definition->GetSlots()[0].Name == TEXT("Interaction")
		&& Definition->GetSlots()[0].Offset == FVector3f(21.0f, -13.0f, 7.0f));
	UeAgentSmartObjectAuthoring::FDefinitionCapture PersistedCapture;
	TestTrue(TEXT("parameter value and its native binding result survive reload"),
		UeAgentSmartObjectAuthoring::CaptureDefinition(*Definition, PersistedCapture, Error)
		&& PersistedCapture.Parameters->GetValueFloat(TEXT("DrawSize")).GetValue() == 42.5f
		&& PersistedCapture.Bindings->GetNumBindings() == 1 && PersistedCapture.Slots[0].DEBUG_DrawSize == 42.5f);

	using namespace DomainAdapters::ContentGraphRead;
	using namespace ContextResource;
	using namespace Contract;
	using namespace StateSemantic;
	using namespace Discovery;
	FUeAgentContentGraphCapture ReadBefore, ReadAfter; FUeAgentContentGraphReadError ReadError;
	if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Definition, ReadBefore, ReadError))
	{ AddError(ReadError.Code + ReadError.Detail); return false; }
	Definition->GetMutableSlot(0).bEnabled = false;
	if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Definition, ReadAfter, ReadError)) return false;
	Definition->GetMutableSlot(0).bEnabled = true;
	TestTrue(TEXT("read revision covers property-only change with unchanged dirty flag"), ReadBefore.Revision != ReadAfter.Revision);
	const FString NativeSlotId = SlotId.ToString(EGuidFormats::DigitsWithHyphensLower);
	const auto* ReadSlot = ReadBefore.Items.FindByPredicate([&](const auto& Item) { return Item.ItemId == NativeSlotId; });
	if (!TestNotNull(TEXT("canonical read captures real slot owner"), ReadSlot)) return false;
	const FString PublicSlotName = ReadSlot->SemanticIdentity.Selectors.FindRef(TEXT("public_name"));
	TestTrue(TEXT("slot properties bind to actual native slot identity"), ReadBefore.Properties.ContainsByPredicate(
		[&](const auto& Property) { return Property.OwnerId == NativeSlotId && Property.FieldId == TEXT("slot.bEnabled"); }));
	const FUeAgentDescriptorIdentity ReadIdentity(TEXT("uai.query.content_graph.compound"), 1,
		TEXT("sha256:5115bd528cefb896dccc1a6f5861f0e2fe99d12ce218790184c16571903a2f36"));
	FUeAgentExecutionContextInput ContextInput;
	ContextInput.ProjectInstanceId = TEXT("t208-smart-object"); ContextInput.EditorInstanceId = TEXT("t208-editor");
	ContextInput.StatePlane = EUeAgentStatePlane::AuthoringMemory; ContextInput.WorldType = EUeAgentExecutionWorldType::Editor;
	ContextInput.ContextRevision = TEXT("t208-read");
	const FUeAgentExecutionContextPolicy ContextPolicy({EUeAgentStatePlane::AuthoringMemory}, {},
		EUeAgentContextResolutionPolicy::ResolveCurrentThenFreeze, EUeAgentTransientContextPolicy::ReadOnly);
	FUeAgentExecutionContextRefPtr ExecutionContext; FUeAgentExecutionContextError ContextError;
	if (!FUeAgentExecutionContextRef::TryCreate(ContextInput, ReadIdentity, EUeAgentRequestKind::Query,
		ContextPolicy, ExecutionContext, ContextError)) return false;
	const FUeAgentQueryRequest ReadRequest(ReadIdentity, TEXT("t208-smart-object-read"), FUeAgentContractValue(TEXT("{}")));
	const FUeAgentCallerContext ReadCaller(TEXT("test.agent"), {TEXT("content_graph:read")}, TEXT("t208-read"));
	const auto Deadline = FUeAgentOperationDeadline::FromTimeout(30.0, []() { return 100.0; });
	const FUeAgentCancellationSource Cancellation([]() { return 100.0; });
	TSharedPtr<FUeAgentOperationContext> OperationContext; FUeAgentOperationContextError OperationError;
	if (!FUeAgentOperationContext::TryCreate(ReadRequest, ReadCaller, ExecutionContext.ToSharedRef(), HjsonWorkspace::MakeSemanticWorkspaceResourceTrackerOwner(),
		Deadline, Cancellation.GetToken(), OperationContext, OperationError)) return false;
	const FUeAgentCandidateBinding ReadTarget(TEXT("t208-smart-object"), FUeAgentCandidateSummary(
		FUeAgentCanonicalIdentity(TEXT("content_graph.target:t208"), ReadBefore.DomainId, ObjectPath, Name, TEXT("target"), ReadBefore.Revision),
		ReadIdentity, ReadBefore.DomainId, Name, TEXT("SmartObject native read"), {TEXT("ContentGraphRead")}),
		FUeAgentCatalogRevision(TEXT("t208-catalog"), ReadBefore.Revision, 1), ObjectPath, TEXT("content-graph-read"),
		{EUeAgentCandidateOperation::Read}, TEXT("t208-smart-object"));
	TSharedPtr<IUeAgentContentGraphReadHook> ReadHook;
	if (!FUeAgentContentGraphReadNativeHookFactory::TryCreate(TEXT("content_graph.read.items"),
		MakeShared<FUeAgentContentGraphReadProductionEnvironment>(), ReadHook, ReadError)) return false;
	const auto Selection = MakeShared<FJsonObject>(); Selection->SetArrayField(TEXT("items"), {MakeShared<FJsonValueString>(PublicSlotName)});
	FString ReadInput; if (!CommandContract::Json::TryCanonicalize(Selection, ReadInput, Error)) return false;
	TSharedPtr<const FUeAgentCanonicalSnapshot> SelectedSnapshot;
	if (!ReadHook->TryCapture(FUeAgentContractValue(ReadInput), ReadTarget, *OperationContext, SelectedSnapshot, ReadError))
	{ AddError(ReadError.Code + ReadError.Detail); return false; }
	TestTrue(TEXT("shared read resolves original public item name to one native item"), SelectedSnapshot.IsValid()
		&& SelectedSnapshot->GetEntries().Num() == 1 && SelectedSnapshot->GetEntries()[0].GetIdentity().GetSelector(TEXT("node_id")) == NativeSlotId);
	AddInfo(TEXT("read_original_name selected_native_owner=1 property_revision=1"));

	// Create from the queried original name, with only readable parent references.
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryResult Candidates;
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError CandidateError;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Definition, TEXT("Slot"), Candidates, CandidateError))
	{ AddError(CandidateError.Code + CandidateError.PublicMessage); return false; }
	const auto* SlotCandidate = Candidates.Candidates.FindByPredicate([](const auto& Candidate) { return Candidate.Kind == TEXT("smart_object_slot"); });
	if (!TestNotNull(TEXT("native fixed slot candidate is queryable by original name"), SlotCandidate)) return false;
	const FString SlotCandidateName = SlotCandidate->Name;
	FString OrderingFile;
	auto ReadRows = [&](TSharedPtr<FJsonObject>& Document, TSharedPtr<FJsonObject>& Rows) {
		if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
			|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
				PackageName, TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"), Params, Payload, ReadOnly, Failure))
		{ AddError(TEXT("structure_export:") + Failure.GetCode()); return false; }
		TArray<FString> StructureFiles;
		IFileManager::Get().FindFilesRecursive(StructureFiles, *(Workspace / TEXT("structures")), TEXT("structure.hjson"), true, false);
		for (const auto& File : StructureFiles)
		{
			FString Text; HjsonWorkspace::FHjsonDocumentPtr Parsed; TSharedPtr<FJsonValue> Value;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, Failure)
				|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Value, Failure))
			{ AddError(TEXT("structure_read:") + Failure.GetCode()); return false; }
			if (Value->Type == EJson::Object && Value->AsObject()->HasTypedField<EJson::Object>(TEXT("smart_object_items")))
			{ OrderingFile = File; Document = Value->AsObject(); Rows = Document->GetObjectField(TEXT("smart_object_items")); return true; }
		}
		AddError(TEXT("smart_object_structure_file_missing")); return false;
	};
	auto WriteRows = [&](const TSharedPtr<FJsonObject>& Document) {
		FString Text;
		if (!CommandContract::Json::TryCanonicalize(Document, Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *OrderingFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			|| !PrepareImport()) { AddError(TEXT("structure_prepare:") + Failure.GetCode() + TEXT(":") + Failure.GetSourceUnit() + TEXT(":") + Failure.GetJsonPointer() + TEXT(":") + Error); return false; }
		const TWeakObjectPtr<USmartObjectDefinition> PreviousDefinition = Definition;
		Definition = nullptr;
		if (!DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"), Params, Payload, ReadOnly, Failure))
		{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetSourceUnit() + TEXT(":") + Failure.GetJsonPointer()); return false; }
		Definition = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath);
		return TestTrue(TEXT("workspace edit replaces the resident object with a clean persisted reload"),
			Definition && !PreviousDefinition.IsValid() && !Definition->GetPackage()->IsDirty());
	};
	TSharedPtr<FJsonObject> StructureDocument, Rows;
	if (!ReadRows(StructureDocument, Rows)) return false;
	FString RootName, OriginalSlotName;
	for (const auto& Pair : Rows->Values)
	{
		if (Pair.Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("settings")) RootName = Pair.Key;
		if (Pair.Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("slot")) OriginalSlotName = Pair.Key;
	}
	if (RootName.IsEmpty() || OriginalSlotName.IsEmpty()) return false;
	TSharedPtr<FJsonValue> FixedBefore, FixedAfter;
	TArray<uint8> FixedDiskBefore, FixedDiskAfter;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, FixedBefore, Error)
		|| !FFileHelper::LoadFileToArray(FixedDiskBefore, *Filename)) return false;
	Rows->GetObjectField(RootName)->SetNumberField(TEXT("index"), 1);
	FString UnsupportedText;
	if (!CommandContract::Json::TryCanonicalize(StructureDocument, UnsupportedText, Error)
		|| !FFileHelper::SaveStringToFile(UnsupportedText, *OrderingFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !PrepareImport()) return false;
	TestFalse(TEXT("fixed owner reordering is rejected before apply"), DomainAdapters::AiBehavior::TryExecuteContentGraph(
		TEXT("content_graph.import_workspace"), PackageName, TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"), Params, Payload, ReadOnly, Failure));
	if (!TestEqual(TEXT("fixed owner has an explicit structural diagnostic"), Failure.GetCode(), FString(TEXT("smart_object_fixed_owner_structure_read_only")))
		|| !TestTrue(TEXT("unsupported structure preserves complete memory and disk"), !Definition->GetPackage()->IsDirty()
			&& UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, FixedAfter, Error)
			&& UeAgentAuthoringState::MakeStableCanonicalValueKey(FixedBefore) == UeAgentAuthoringState::MakeStableCanonicalValueKey(FixedAfter)
			&& FFileHelper::LoadFileToArray(FixedDiskAfter, *Filename) && FixedDiskAfter == FixedDiskBefore)) return false;
	Rows->GetObjectField(RootName)->SetNumberField(TEXT("index"), 0);
	const auto NewSlot = MakeShared<FJsonObject>();
	NewSlot->SetStringField(TEXT("name"), SlotCandidateName);
	NewSlot->SetStringField(TEXT("parent"), RootName); NewSlot->SetNumberField(TEXT("index"), 0);
	Rows->GetObjectField(OriginalSlotName)->SetNumberField(TEXT("index"), 1);
	Rows->SetObjectField(TEXT("Additional"), NewSlot);
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Definition, FSmartObjectSlotEntranceAnnotation::StaticStruct()->GetDisplayNameText().ToString(), Candidates, CandidateError)) { AddError(CandidateError.Code + CandidateError.PublicMessage); return false; }
	const auto* DataCandidate = Candidates.Candidates.FindByPredicate([](const auto& Entry) {
		return Entry.PayloadType == FSmartObjectSlotEntranceAnnotation::StaticStruct()->GetPathName(); });
	if (!TestNotNull(TEXT("native annotation candidate is available"), DataCandidate)) return false;
	const auto NewData = MakeShared<FJsonObject>();
	NewData->SetStringField(TEXT("name"), DataCandidate->Name);
	NewData->SetStringField(TEXT("parent"), TEXT("Additional")); Rows->SetObjectField(TEXT("Additional entrance"), NewData);
	if (!WriteRows(StructureDocument)) return false;
	if (!TestTrue(TEXT("created slot and explicit order persist after reload"), !Definition->GetPackage()->IsDirty()
		&& Definition->GetSlots().Num() == 2 && Definition->GetSlots()[0].Name == TEXT("Additional")
		&& Definition->GetSlots()[0].ID.IsValid() && Definition->GetSlots()[0].ID != SlotId
		&& Definition->GetSlots()[1].ID == SlotId && Definition->GetSlots()[1].DEBUG_DrawSize == 42.5f
		&& Definition->GetSlots()[0].DefinitionData.Num() == 1
		&& Definition->GetSlots()[0].DefinitionData[0].Data.GetScriptStruct() == FSmartObjectSlotEntranceAnnotation::StaticStruct()
		&& Definition->GetSlots()[1].DefinitionData[0].ID == OriginalDataId
		&& Definition->GetSlots()[1].DefinitionData[0].Data.Get<FSmartObjectSlotEntranceAnnotation>().Offset == FVector3f(5,8,13))) return false;
	const FGuid AdditionalId = Definition->GetSlots()[0].ID;
	if (!ReadRows(StructureDocument, Rows)) return false;
	FString ExistingBehaviorName;
	for (const auto& Pair : Rows->Values)
		if (Pair.Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("behavior")) ExistingBehaviorName = Pair.Key;
	if (ExistingBehaviorName.IsEmpty()) return false;
	Rows->GetObjectField(ExistingBehaviorName)->SetStringField(TEXT("parent"), TEXT("Additional"));
	if (!WriteRows(StructureDocument)) return false;
	const auto* MovedBehavior = Definition->GetSlots()[0].BehaviorDefinitions.Num() == 1
		? Cast<UGameplayInteractionSmartObjectBehaviorDefinition>(Definition->GetSlots()[0].BehaviorDefinitions[0]) : nullptr;
	if (!TestTrue(TEXT("readable parent move preserves owned behavior values"), MovedBehavior && MovedBehavior->GetStateTree()
		&& MovedBehavior->GetStateTree()->GetPathName() == OtherStateObjectPath && Definition->GetSlots()[1].BehaviorDefinitions.IsEmpty())) return false;
	if (!ReadRows(StructureDocument, Rows)) return false;
	for (const auto& Pair : Rows->Values)
		if (Pair.Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("behavior"))
			Pair.Value->AsObject()->SetStringField(TEXT("parent"), OriginalSlotName);
	// Shared keyed documents require dependent rows to be removed or reparented
	// in the same edit, rather than retaining a dangling parent reference.
	TArray<FString> RemovedChildren;
	for (const auto& Pair : Rows->Values)
	{
		FString Parent; Pair.Value->AsObject()->TryGetStringField(TEXT("parent"), Parent);
		if (Parent == TEXT("Additional")) RemovedChildren.Add(Pair.Key);
	}
	for (const auto& Child : RemovedChildren) Rows->RemoveField(Child);
	Rows->RemoveField(TEXT("Additional"));
	if (!WriteRows(StructureDocument)) return false;
	if (!TestTrue(TEXT("slot deletion and child reparent preserve all original slot state"), Definition->GetSlots().Num() == 1
		&& Definition->GetSlots()[0].ID == SlotId && Definition->GetSlots()[0].Name == TEXT("Interaction")
		&& Definition->GetSlots()[0].Offset == FVector3f(21,-13,7) && Definition->GetSlots()[0].BehaviorDefinitions.Num() == 1
		&& Definition->GetSlots()[0].DEBUG_DrawSize == 42.5f && Definition->GetSlots()[0].DefinitionData.Num() == 1
		&& Definition->GetSlots()[0].DefinitionData[0].ID == OriginalDataId
		&& Definition->GetSlots()[0].DefinitionData[0].Data.Get<FSmartObjectSlotEntranceAnnotation>().Offset == FVector3f(5,8,13))) return false;
	AddInfo(TEXT("structure_success original_name_candidate=1 create=1 reorder=1 readable_reparent=1 delete=1 persisted_reload=1"));
	auto CheckPreservedNativeState = [&]() {
		UeAgentSmartObjectAuthoring::FDefinitionCapture Native;
		if (!Definition || !UeAgentSmartObjectAuthoring::CaptureDefinition(*Definition, Native, Error)
			|| Native.Slots.IsEmpty() || Native.Slots[0].BehaviorDefinitions.Num() != 1
			|| Native.Slots[0].DefinitionData.Num() != 1) return false;
		const auto* KeptBehavior = Cast<UGameplayInteractionSmartObjectBehaviorDefinition>(Native.Slots[0].BehaviorDefinitions[0]);
		bool BindingPreserved = false;
		Native.Bindings->ForEachBinding([&](const FPropertyBindingBinding& Binding) {
			BindingPreserved |= Binding.GetSourcePath().GetStructID() == ParameterId
				&& Binding.GetSourcePath().ToString() == BindingSource.ToString()
				&& Binding.GetTargetPath().GetStructID() == SlotId
				&& Binding.GetTargetPath().ToString() == BindingTarget.ToString();
		});
		const auto Value = Native.Parameters->GetValueFloat(TEXT("DrawSize"));
		return Value.IsValid() && Value.GetValue() == 42.5f && Native.Bindings->GetNumBindings() == 1 && BindingPreserved
			&& KeptBehavior && KeptBehavior->GetStateTree() && KeptBehavior->GetStateTree()->GetPathName() == OtherStateObjectPath
			&& Native.Slots[0].DefinitionData[0].ID == OriginalDataId
			&& Native.Slots[0].DefinitionData[0].Data.GetScriptStruct() == FSmartObjectSlotEntranceAnnotation::StaticStruct()
			&& Native.Slots[0].DefinitionData[0].Data.Get<FSmartObjectSlotEntranceAnnotation>().Offset == FVector3f(5,8,13);
	};
	if (!TestTrue(TEXT("structural success preserves native owned value, data, parameter and binding endpoints"), CheckPreservedNativeState())) return false;

	// Ordinary schema edits exercise the public schema/fields pair in a bag that
	// already owns a native binding. New IDs never appear in the workspace input.
	auto EditParameterSchema = [&](const FString& DrawKind, bool bIncludeSize) {
		if (!ReadRows(StructureDocument, Rows)) return false;
		bool Found = false;
		for (const FString& File : Files) {
			FString Normalized = File; FPaths::NormalizeFilename(Normalized);
			if (Normalized.Contains(TEXT("/.uai/"))) continue;
			FString Text; HjsonWorkspace::FHjsonDocumentPtr Parsed; TSharedPtr<FJsonValue> Value;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, Failure)
				|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Value, Failure)) return false;
			bool Changed = false;
			TFunction<void(const TSharedPtr<FJsonValue>&)> Visit = [&](const TSharedPtr<FJsonValue>& Node) {
				if (!Node.IsValid()) return;
				if (Node->Type == EJson::Array) { for (const auto& Child : Node->AsArray()) Visit(Child); return; }
				if (Node->Type != EJson::Object) return;
				for (const auto& Pair : Node->AsObject()->Values) {
					if (Pair.Key == TEXT("parameters.Parameters")) {
						const auto Bag = MakeShared<FJsonObject>();
						TArray<TSharedPtr<FJsonValue>> Schema;
						const auto Draw = MakeShared<FJsonObject>(); Draw->SetStringField(TEXT("name"), TEXT("DrawSize")); Draw->SetStringField(TEXT("kind"), DrawKind);
						Schema.Add(MakeShared<FJsonValueObject>(Draw));
						const auto Fields = MakeShared<FJsonObject>(); Fields->SetNumberField(TEXT("DrawSize"), 42.5);
						if (bIncludeSize) {
							const auto Size = MakeShared<FJsonObject>(); Size->SetStringField(TEXT("name"), TEXT("Size")); Size->SetStringField(TEXT("kind"), TEXT("float"));
							Schema.Add(MakeShared<FJsonValueObject>(Size)); Fields->SetNumberField(TEXT("Size"), 19.5);
						}
						Bag->SetArrayField(TEXT("schema"), Schema); Bag->SetObjectField(TEXT("fields"), Fields);
						Pair.Value->AsObject()->SetObjectField(TEXT("value"), Bag); Changed = true; Found = true;
					} else Visit(Pair.Value);
				}
			}; Visit(Value);
			if (Changed && (!CommandContract::Json::TryCanonicalize(Value->AsObject(), Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		}
		if (!Found || !PrepareImport()) return false;
		TWeakObjectPtr<USmartObjectDefinition> Previous = Definition; Definition = nullptr;
		if (!DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"), PackageName,
			TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"), Params, Payload, ReadOnly, Failure))
		{ AddError(TEXT("parameter_schema:") + Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
		Definition = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath);
		if (!Definition || Previous.IsValid() || Definition->GetPackage()->IsDirty()) return false;
		const auto& Bag = *ParametersProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Definition);
		const auto* Draw = Bag.FindPropertyDescByName(TEXT("DrawSize"));
		return TestTrue(TEXT("schema edit persists native ID, metadata and binding result"), Draw && Draw->ID == DrawSizeDesc.ID
			&& Draw->PropertyFlags == DrawSizeDesc.PropertyFlags && Draw->MetaData == DrawSizeDesc.MetaData
			&& Draw->ValueType == (DrawKind == TEXT("double") ? EPropertyBagPropertyType::Double : EPropertyBagPropertyType::Float)
			&& (Bag.FindPropertyDescByName(TEXT("Size")) != nullptr) == bIncludeSize
			&& (!bIncludeSize || Bag.GetValueFloat(TEXT("Size")).GetValue() == 19.5f)
			&& Definition->GetSlots()[0].DEBUG_DrawSize == 42.5f && CheckPreservedNativeState());
	};
	if (!EditParameterSchema(TEXT("float"), true)) return false;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Definition, TEXT("Binding"), Candidates, CandidateError)) return false;
	const auto* BindingCandidate = Candidates.Candidates.FindByPredicate([](const auto& Entry) { return Entry.Kind == TEXT("smart_object_binding"); });
	if (!TestNotNull(TEXT("Binding original candidate name is queryable"), BindingCandidate)) return false;
	const FString BindingName = BindingCandidate->Name;
	if (!ReadRows(StructureDocument, Rows)) return false;
	FString ParametersName;
	for (const auto& Pair : Rows->Values) if (Pair.Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("parameters")) ParametersName = Pair.Key;
	auto NewBindingRow = [&](const FString& SourcePath, const FString& TargetPath) {
		const auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("name"), BindingName);
		Row->SetStringField(TEXT("source"), ParametersName); Row->SetStringField(TEXT("target"), OriginalSlotName);
		Row->SetStringField(TEXT("source_path"), SourcePath); Row->SetStringField(TEXT("target_path"), TargetPath); return Row;
	};
	auto FindBindingRow = [&](const FString& TargetPath) {
		for (const auto& Pair : Rows->Values) {
			FString Path; Pair.Value->AsObject()->TryGetStringField(TEXT("target_path"), Path);
			if (Path == TargetPath) return Pair.Key;
		} return FString();
	};
	auto RejectRows = [&](const FString& ExpectedCode) {
		TSharedPtr<FJsonValue> MemoryBefore, MemoryAfter; TArray<uint8> BytesBefore, BytesAfter;
		if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, MemoryBefore, Error)
			|| !FFileHelper::LoadFileToArray(BytesBefore, *Filename)) return false;
		FString Text;
		if (!CommandContract::Json::TryCanonicalize(StructureDocument, Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *OrderingFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{ AddError(TEXT("rejection_input_write_failed:") + Error); return false; }
		const TWeakObjectPtr<USmartObjectDefinition> Unmodified = Definition;
		// Dangling readable references can be rejected while preparing the public
		// import, before domain dispatch. Both rejection stages must leave memory/disk intact.
		const bool Prepared = PrepareImport();
		const bool Applied = Prepared && DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"), PackageName,
			TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"), Params, Payload, ReadOnly, Failure);
		Definition = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath);
		if (Failure.GetCode() != ExpectedCode) AddError(TEXT("rejection_code:") + Failure.GetCode() + TEXT(" expected:") + ExpectedCode);
		return TestTrue(TEXT("invalid native binding is rejected before actual memory/disk writes"), !Applied
			&& Failure.GetCode() == ExpectedCode && Unmodified.IsValid() && Definition && !Definition->GetPackage()->IsDirty()
			&& UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, MemoryAfter, Error)
			&& UeAgentAuthoringState::MakeStableCanonicalValueKey(MemoryBefore) == UeAgentAuthoringState::MakeStableCanonicalValueKey(MemoryAfter)
			&& FFileHelper::LoadFileToArray(BytesAfter, *Filename) && BytesBefore == BytesAfter);
	};
	Rows->SetObjectField(TEXT("Invalid Binding"), NewBindingRow(TEXT("DrawSize"), TEXT("MissingProperty")));
	if (!RejectRows(TEXT("smart_object_binding_path_invalid")) || !ReadRows(StructureDocument, Rows)) return false;
	Rows->SetObjectField(TEXT("Invalid Binding"), NewBindingRow(TEXT("DrawSize"), TEXT("UserTagFilter")));
	if (!RejectRows(TEXT("smart_object_binding_type_incompatible")) || !ReadRows(StructureDocument, Rows)) return false;
	Rows->GetObjectField(FindBindingRow(TEXT("DEBUG_DrawSize")))->SetStringField(TEXT("source_path"), TEXT("Size"));
	if (!RejectRows(TEXT("smart_object_binding_target_not_bindable")) || !ReadRows(StructureDocument, Rows)) return false;
	Rows->SetObjectField(TEXT("Workspace Binding"), NewBindingRow(TEXT("DrawSize"), TEXT("Offset.X")));
	if (!WriteRows(StructureDocument)) return false;
	if (!TestEqual(TEXT("new native binding copies parameter into legal slot target after reload"), Definition->GetSlots()[0].Offset.X, 42.5f)) return false;
	TSharedPtr<FJsonValue> WithBinding; TSharedPtr<FJsonObject> LateBinding;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, WithBinding, Error)) return false;
	for (const auto& Pair : WithBinding->AsObject()->GetObjectField(TEXT("items"))->Values) {
		FString Path; if (Pair.Value->AsObject()->TryGetStringField(TEXT("target_path"), Path) && Path == TEXT("Offset.X"))
			LateBinding = UeAgentAuthoringState::CloneCanonicalValue(Pair.Value)->AsObject();
	}
	if (!LateBinding || !ReadRows(StructureDocument, Rows)) return false;
	Rows->SetObjectField(TEXT("Duplicate Binding"), NewBindingRow(TEXT("Size"), TEXT("Offset.X")));
	if (!RejectRows(TEXT("smart_object_binding_target_duplicate")) || !ReadRows(StructureDocument, Rows)) return false;
	Rows->GetObjectField(FindBindingRow(TEXT("Offset.X")))->SetStringField(TEXT("source_path"), TEXT("Size"));
	if (!WriteRows(StructureDocument) || !TestEqual(TEXT("binding source path edit persists and propagates"), Definition->GetSlots()[0].Offset.X, 19.5f)
		|| !ReadRows(StructureDocument, Rows)) return false;
	TSharedPtr<FJsonValue> BoundBefore, BoundAfter;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, BoundBefore, Error)) return false;
	const auto MissingParameter = UeAgentAuthoringState::CloneCanonicalValue(BoundBefore);
	for (const auto& Descriptor : MissingParameter->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(TEXT("parameters"))->GetObjectField(TEXT("properties"))->GetArrayField(TEXT("properties"))) {
		const auto Bag = Descriptor->AsObject()->GetObjectField(TEXT("value"));
		Bag->GetObjectField(TEXT("fields"))->RemoveField(TEXT("Size"));
		auto Schema=Bag->GetArrayField(TEXT("schema")); Schema.RemoveAll([](const auto& Entry){return Entry->AsObject()->GetStringField(TEXT("name"))==TEXT("Size");});
		Bag->SetArrayField(TEXT("schema"),Schema);
	}
	TArray<uint8> BoundDiskBefore, BoundDiskAfter; if (!FFileHelper::LoadFileToArray(BoundDiskBefore,*Filename)) return false;
	UeAgentReflectionAuthoring::FAssetApplyRequest MissingRequest;
	MissingRequest.TypeId=TEXT("smart_object"); MissingRequest.AssetPath=PackageName; MissingRequest.bSave=true; MissingRequest.bCompactValues=true;
	MissingRequest.ProviderExtension=MakeShared<FJsonObject>(); MissingRequest.ProviderExtension->SetObjectField(TEXT("smart_object"),MissingParameter->AsObject());
	UeAgentReflectionAuthoring::FAssetApplyResult MissingResult;
	const bool MissingApplied = UeAgentReflectionAuthoring::ApplyAsset(MissingRequest,MissingResult,TEXT("smart-object-missing-parameter-")+Name);
	Definition=LoadObject<USmartObjectDefinition>(nullptr,*ObjectPath);
	if (!TestFalse(TEXT("deleting a bound parameter without deleting its binding is rejected"), MissingApplied)
		|| !TestTrue(TEXT("bound parameter rejection preserves complete actual memory and disk"), Definition && !Definition->GetPackage()->IsDirty()
			&& UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition,BoundAfter,Error)
			&& UeAgentAuthoringState::MakeStableCanonicalValueKey(BoundBefore)==UeAgentAuthoringState::MakeStableCanonicalValueKey(BoundAfter)
			&& FFileHelper::LoadFileToArray(BoundDiskAfter,*Filename) && BoundDiskAfter==BoundDiskBefore)) return false;
	Rows->GetObjectField(FindBindingRow(TEXT("Offset.X")))->SetStringField(TEXT("target_path"), TEXT("Offset.Y"));
	if (!WriteRows(StructureDocument) || !TestTrue(TEXT("binding target path edit preserves prior copied value"),
		Definition->GetSlots()[0].Offset.X == 19.5f && Definition->GetSlots()[0].Offset.Y == 19.5f && Definition->GetSlots()[0].Offset.Z == 7.0f)
		|| !ReadRows(StructureDocument, Rows)) return false;
	Rows->RemoveField(FindBindingRow(TEXT("Offset.Y")));
	if (!WriteRows(StructureDocument)) return false;
	if (!TestTrue(TEXT("deleting new binding preserves existing native binding and values"), CheckPreservedNativeState()
		&& Definition->GetSlots()[0].Offset == FVector3f(19.5f,19.5f,7.0f))) return false;
	TSharedPtr<FJsonValue> ResetOffset;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, ResetOffset, Error)) return false;
	for (const auto& Descriptor : ResetOffset->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(NativeSlotId)->GetObjectField(TEXT("properties"))->GetArrayField(TEXT("properties")))
		if (Descriptor->AsObject()->GetStringField(TEXT("property_path")).EndsWith(TEXT("Offset"))) {
			Descriptor->AsObject()->GetObjectField(TEXT("value"))->SetArrayField(TEXT("components"), {
				MakeShared<FJsonValueNumber>(21), MakeShared<FJsonValueNumber>(-13), MakeShared<FJsonValueNumber>(7)});
		}
	UeAgentReflectionAuthoring::FAssetApplyRequest ResetRequest;
	ResetRequest.TypeId=TEXT("smart_object"); ResetRequest.AssetPath=PackageName; ResetRequest.bSave=true; ResetRequest.bCompactValues=true;
	ResetRequest.ProviderExtension=MakeShared<FJsonObject>(); ResetRequest.ProviderExtension->SetObjectField(TEXT("smart_object"),ResetOffset->AsObject());
	UeAgentReflectionAuthoring::FAssetApplyResult ResetResult; Definition=nullptr;
	if (!UeAgentReflectionAuthoring::ApplyAsset(ResetRequest,ResetResult,TEXT("smart-object-reset-offset-")+Name)) { AddError(ResetResult.Diagnostic.Code); return false; }
	Definition=LoadObject<USmartObjectDefinition>(nullptr,*ObjectPath);
	if (!Definition || !EditParameterSchema(TEXT("double"), true) || !EditParameterSchema(TEXT("float"), false)) return false;
	AddInfo(TEXT("binding_success original_name=1 add=1 source_path_edit=1 target_path_edit=1 delete=1 native_rejections=1 saved_reload=1"));
	AddInfo(TEXT("parameter_schema add=1 bound_retype=1 delete=1 native_identity_metadata=1 binding_copy=1 saved_reload=1"));

	// Author one real schema-supported condition through ordinary property input.
	// Only the picked path is model-authored; native SmartObject derives its runtime copy.
	const FGameplayTagQuery QueryA = FGameplayTagQuery::BuildQuery(FGameplayTagQueryExpression().AnyTagsMatch(), TEXT("Condition parameter A"));
	const FGameplayTagQuery QueryB = FGameplayTagQuery::BuildQuery(FGameplayTagQueryExpression().AllTagsMatch(), TEXT("Condition parameter B"));
	const FGameplayTagQuery QueryC = FGameplayTagQuery::BuildQuery(FGameplayTagQueryExpression().NoTagsMatch(), TEXT("Condition parameter C"));
	const FString ConditionPath = TEXT("SelectionPreconditions.EditableConditions[0].Condition.TagQuery");
	auto EditConditionProperties = [&](const FGameplayTagQuery& ParameterQuery, bool bCreateCondition) {
		FInstancedPropertyBag DesiredBag = *ParametersProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Definition);
		DesiredBag.AddProperty(TEXT("ConditionQuery"), EPropertyBagPropertyType::Struct, FGameplayTagQuery::StaticStruct());
		if (DesiredBag.SetValueStruct(TEXT("ConditionQuery"), ParameterQuery) != EPropertyBagResult::Success) return false;
		FWorldConditionQueryDefinition DesiredConditions;
		DesiredConditions.SetSchemaClass(Definition->GetWorldConditionSchemaClass());
		FSmartObjectWorldConditionObjectTagQuery Condition; Condition.TagQuery = QueryC;
		DesiredConditions.AddCondition(FWorldConditionEditable(0, EWorldConditionOperator::And, FConstStructView::Make(Condition)));
		bool BagFound = false, ConditionFound = !bCreateCondition;
		TArray<FString> PropertyFiles; IFileManager::Get().FindFilesRecursive(PropertyFiles, *Workspace, TEXT("*.hjson"), true, false);
		for (const FString& File : PropertyFiles) {
			FString Normalized = File; FPaths::NormalizeFilename(Normalized); if (Normalized.Contains(TEXT("/.uai/"))) continue;
			FString Text; HjsonWorkspace::FHjsonDocumentPtr Parsed; TSharedPtr<FJsonValue> Value;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, Failure)
				|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Value, Failure)) return false;
			bool Changed = false;
			TFunction<void(const TSharedPtr<FJsonValue>&)> Visit = [&](const TSharedPtr<FJsonValue>& Node) {
				if (!Node.IsValid()) return;
				if (Node->Type == EJson::Array) { for (const auto& Child : Node->AsArray()) Visit(Child); return; }
				if (Node->Type != EJson::Object) return;
				for (const auto& Pair : Node->AsObject()->Values) {
					if (Pair.Key == TEXT("parameters.Parameters")) {
						Pair.Value->AsObject()->SetField(TEXT("value"), UeAgentPropertyValueCodec::EncodePropertyBag(DesiredBag, nullptr, {}));
						BagFound = Changed = true;
					} else if (bCreateCondition && Pair.Key == TEXT("slot.SelectionPreconditions")) {
						Pair.Value->AsObject()->SetField(TEXT("value"), UeAgentPropertyValueCodec::EncodeStructValue(FWorldConditionQueryDefinition::StaticStruct(), &DesiredConditions, nullptr, {}));
						ConditionFound = Changed = true;
					} else Visit(Pair.Value);
				}
			}; Visit(Value);
			if (Changed && (!CommandContract::Json::TryCanonicalize(Value->AsObject(), Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		}
		return TestTrue(TEXT("ordinary properties expose condition and compatible parameter"), BagFound && ConditionFound);
	};
	auto CheckCondition = [&](USmartObjectDefinition* Actual, const FGameplayTagQuery& Expected, int32 ExpectedBindings) {
		if (!Actual || Actual->GetSlots().Num() != 1) return false;
		const auto& Conditions = Actual->GetSlots()[0].SelectionPreconditions;
		const auto* EditableProperty = FindFProperty<FArrayProperty>(FWorldConditionQueryDefinition::StaticStruct(), TEXT("EditableConditions"));
		const auto& Editable = *EditableProperty->ContainerPtrToValuePtr<TArray<FWorldConditionEditable>>(&Conditions);
		const auto* Shared = Conditions.GetSharedDefinition();
		if (Editable.Num() != 1 || !Shared || Shared->GetConditions().Num() != 1
			|| Editable[0].Condition.GetScriptStruct() != FSmartObjectWorldConditionObjectTagQuery::StaticStruct()
			|| Shared->GetConditions()[0].GetScriptStruct() != FSmartObjectWorldConditionObjectTagQuery::StaticStruct()) return false;
		int32 Picked = 0, Retargeted = 0;
		static_cast<IPropertyBindingBindingCollectionOwner&>(*Actual).GetEditorPropertyBindings()->ForEachBinding([&](const FPropertyBindingBinding& Binding) {
			const auto Status = static_cast<const FSmartObjectDefinitionPropertyBinding&>(Binding).TargetPathRetargetingStatus;
			Picked += Status == ESmartObjectPropertyPathRetargetingStatus::PickedPath;
			Retargeted += Status == ESmartObjectPropertyPathRetargetingStatus::RetargetedPath;
		});
		return Conditions.IsValid() && Shared->IsLinked() && Picked == ExpectedBindings && Retargeted == ExpectedBindings
			&& Editable[0].Condition.Get<FSmartObjectWorldConditionObjectTagQuery>().TagQuery == Expected
			&& Shared->GetConditions()[0].Get<const FSmartObjectWorldConditionObjectTagQuery>().TagQuery == Expected;
	};
	if (!ReadRows(StructureDocument, Rows) || !EditConditionProperties(QueryA, true)) return false;
	Rows->SetObjectField(TEXT("Condition Binding"), NewBindingRow(TEXT("ConditionQuery"), ConditionPath));
	if (!WriteRows(StructureDocument)
		|| !TestTrue(TEXT("ordinary condition binding persists picked and retargeted values"), CheckCondition(Definition, QueryA, 1))) return false;
	TSharedPtr<FJsonValue> ConditionBoundDocument;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, ConditionBoundDocument, Error)
		|| !ReadRows(StructureDocument, Rows)) return false;
	int32 FrozenDerivedBindings = 0;
	for (const auto& Pair : ConditionBoundDocument->AsObject()->GetObjectField(TEXT("items"))->Values) {
		FString Path; Pair.Value->AsObject()->TryGetStringField(TEXT("target_path"), Path);
		FrozenDerivedBindings += Path.Contains(TEXT("SharedDefinition")) && Pair.Value->AsObject()->HasField(TEXT("binding_state"));
	}
	if (!TestEqual(TEXT("complete frozen document retains the derived binding state"), FrozenDerivedBindings, 1)) return false;
	int32 PublicConditionBindings = 0;
	for (const auto& Pair : Rows->Values) {
		FString Path; Pair.Value->AsObject()->TryGetStringField(TEXT("target_path"), Path);
		PublicConditionBindings += Path == ConditionPath;
		if (!TestFalse(TEXT("ordinary binding excludes derived runtime path"), Path.Contains(TEXT("SharedDefinition")))) return false;
	}
	if (!TestEqual(TEXT("ordinary export contains one picked condition binding"), PublicConditionBindings, 1)
		|| !EditConditionProperties(QueryB, false) || !WriteRows(StructureDocument)
		|| !TestTrue(TEXT("parameter edit updates both persisted condition copies"), CheckCondition(Definition, QueryB, 1))
		|| !ReadRows(StructureDocument, Rows)) return false;
	Rows->RemoveField(FindBindingRow(ConditionPath));
	if (!WriteRows(StructureDocument) || !ReadRows(StructureDocument, Rows)
		|| !EditConditionProperties(QueryC, false) || !WriteRows(StructureDocument)
		|| !TestTrue(TEXT("binding deletion stops parameter propagation and keeps both condition values"), CheckCondition(Definition, QueryB, 0))
		|| !TestTrue(TEXT("condition edits preserve unrelated native slot and owned state"), CheckPreservedNativeState())) return false;

	Definition->GetMutableSlot(0).bEnabled = false; Definition->MarkPackageDirty();
	TSharedPtr<FJsonValue> ConditionDirty;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, ConditionDirty, Error)) return false;
	const FString ConditionDirtyKey = UeAgentAuthoringState::MakeStableCanonicalValueKey(ConditionDirty);
	TArray<uint8> ConditionDiskBefore; if (!FFileHelper::LoadFileToArray(ConditionDiskBefore, *Filename)) return false;
	const auto ConditionDesired = UeAgentAuthoringState::CloneCanonicalValue(ConditionDirty);
	for (const auto& Pair : ConditionBoundDocument->AsObject()->GetObjectField(TEXT("items"))->Values) {
		FString Path; Pair.Value->AsObject()->TryGetStringField(TEXT("target_path"), Path);
		if (Path == ConditionPath) ConditionDesired->AsObject()->GetObjectField(TEXT("items"))->SetField(Pair.Key, UeAgentAuthoringState::CloneCanonicalValue(Pair.Value));
	}
	UeAgentReflectionAuthoring::FAssetApplyRequest ConditionLateRequest;
	ConditionLateRequest.TypeId = TEXT("smart_object"); ConditionLateRequest.AssetPath = PackageName;
	ConditionLateRequest.bSave = true; ConditionLateRequest.bCompactValues = true;
	ConditionLateRequest.ProviderExtension = MakeShared<FJsonObject>();
	ConditionLateRequest.ProviderExtension->SetObjectField(TEXT("smart_object"), ConditionDesired->AsObject());
	bool ConditionLateObserved = false; TWeakObjectPtr<USmartObjectDefinition> ConditionRejected;
	ConditionLateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Rejection) {
		auto* Actual = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath); ConditionRejected = Actual;
		TArray<uint8> ChangedDisk;
		ConditionLateObserved = Actual && !Actual->GetPackage()->IsDirty() && CheckCondition(Actual, QueryC, 1)
			&& FFileHelper::LoadFileToArray(ChangedDisk, *Filename) && ChangedDisk != ConditionDiskBefore;
		AddInfo(FString::Printf(TEXT("condition_late_stage saved_reload_both_values=%d"), ConditionLateObserved));
		Rejection.Diagnostic.Code = TEXT("t208_smart_object_condition_late_rejected"); Rejection.Diagnostic.PublicMessage = Rejection.Diagnostic.Code; return false;
	};
	Definition = nullptr; UeAgentReflectionAuthoring::FAssetApplyResult ConditionLateResult;
	if (!TestFalse(TEXT("late condition binding is rejected"), UeAgentReflectionAuthoring::ApplyAsset(ConditionLateRequest, ConditionLateResult, TEXT("smart-object-condition-late-") + Name))
		|| !TestTrue(TEXT("new condition binding reached saved reload before rejection"), ConditionLateObserved)
		|| !TestTrue(TEXT("condition checkpoint recovery succeeds"), ConditionLateResult.bRollbackAttempted && ConditionLateResult.bRollbackSucceeded && !ConditionRejected.IsValid())) return false;
	Definition = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath);
	TSharedPtr<FJsonValue> ConditionRestored; TArray<uint8> ConditionDiskAfter;
	if (!TestTrue(TEXT("complete dirty condition document and different original disk recover"), Definition && Definition->GetPackage()->IsDirty()
		&& UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, ConditionRestored, Error)
		&& UeAgentAuthoringState::MakeStableCanonicalValueKey(ConditionRestored) == ConditionDirtyKey
		&& FFileHelper::LoadFileToArray(ConditionDiskAfter, *Filename) && ConditionDiskAfter == ConditionDiskBefore
		&& CheckCondition(Definition, QueryB, 0) && CheckPreservedNativeState())) return false;
	AddInfo(TEXT("condition_binding ordinary_input=1 picked_only=1 parameter_update=1 delete=1 saved_reload_both_values=1 late_restore_complete=1"));

	// SlotLink is a structural relation, not a property binding. New target names
	// and later edits must all use the same public keyed reference mechanism.
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Definition,
		FSmartObjectSlotLinkAnnotation::StaticStruct()->GetDisplayNameText().ToString(), Candidates, CandidateError))
	{ AddError(TEXT("slot_link_candidate:") + CandidateError.Code + TEXT(":") + CandidateError.PublicMessage); return false; }
	const auto* LinkCandidate = Candidates.Candidates.FindByPredicate([](const auto& Entry) {
		return Entry.PayloadType == FSmartObjectSlotLinkAnnotation::StaticStruct()->GetPathName(); });
	if (!TestNotNull(TEXT("native SlotLink candidate is available by original name"), LinkCandidate)) return false;
	const FString LinkCandidateName = LinkCandidate->Name;
	const FString LinkedTargetName = TEXT("Linked target");
	auto AddLinkedTarget = [&]() {
		const auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("name"), SlotCandidateName);
		Row->SetStringField(TEXT("parent"), RootName); Row->SetNumberField(TEXT("index"), 1);
		Rows->SetObjectField(LinkedTargetName, Row);
	};
	auto FindLinkRow = [&]() {
		for (const auto& Pair : Rows->Values) if (Pair.Value->AsObject()->HasField(TEXT("linked_slot"))) return Pair.Key;
		return FString();
	};
	auto FindNativeLink = [](const USmartObjectDefinition* Actual) -> const FSmartObjectSlotLinkAnnotation* {
		if (Actual) for (const auto& NativeSlot : Actual->GetSlots()) for (const auto& Data : NativeSlot.DefinitionData)
			if (Data.Data.IsValid() && Data.Data.GetScriptStruct() == FSmartObjectSlotLinkAnnotation::StaticStruct())
				return &Data.Data.Get<FSmartObjectSlotLinkAnnotation>();
		return nullptr;
	};
	auto CheckSlotLink = [&](USmartObjectDefinition* Actual, const FGuid& TargetId, int32 TargetIndex) {
		const auto* Link = FindNativeLink(Actual);
		if (!Link || Link->LinkedSlot.GetSlotID() != TargetId || Link->LinkedSlot.GetIndex() != TargetIndex) return false;
		const auto* KeptSlot = Actual->GetSlots().FindByPredicate([&](const auto& Entry) { return Entry.ID == SlotId; });
		if (!KeptSlot || KeptSlot->BehaviorDefinitions.Num() != 1 || KeptSlot->DefinitionData.Num() != 2) return false;
		const auto* KeptData = KeptSlot->DefinitionData.FindByPredicate([&](const auto& Data) { return Data.ID == OriginalDataId; });
		const auto* KeptBehavior = Cast<UGameplayInteractionSmartObjectBehaviorDefinition>(KeptSlot->BehaviorDefinitions[0]);
		return KeptSlot->Name == TEXT("Interaction") && KeptSlot->Offset == FVector3f(21,-13,7) && KeptSlot->DEBUG_DrawSize == 42.5f
			&& KeptData && KeptData->Data.Get<FSmartObjectSlotEntranceAnnotation>().Offset == FVector3f(5,8,13)
			&& KeptBehavior && KeptBehavior->GetStateTree() && KeptBehavior->GetStateTree()->GetPathName() == OtherStateObjectPath
			&& ParametersProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Actual)->GetValueFloat(TEXT("DrawSize")).GetValue() == 42.5f
			&& static_cast<IPropertyBindingBindingCollectionOwner&>(*Actual).GetEditorPropertyBindings()->GetNumBindings() == 1;
	};
	if (!ReadRows(StructureDocument, Rows)) return false;
	AddLinkedTarget();
	const auto LinkRow = MakeShared<FJsonObject>(); LinkRow->SetStringField(TEXT("name"), LinkCandidateName);
	LinkRow->SetStringField(TEXT("parent"), OriginalSlotName); LinkRow->SetStringField(TEXT("linked_slot"), LinkedTargetName);
	Rows->SetObjectField(TEXT("Slot relation"), LinkRow);
	if (!WriteRows(StructureDocument)) return false;
	FGuid LinkedTargetId = Definition->GetSlots()[1].ID;
	AddInfo(TEXT("slot_link_stage same_edit_import_complete"));
	if (!TestTrue(TEXT("same-edit new annotation resolves new slot identity and native index"), CheckSlotLink(Definition, LinkedTargetId, 1))
		|| !ReadRows(StructureDocument, Rows)) return false;
	if (!TestEqual(TEXT("ordinary relation reexports readable target name"), Rows->GetObjectField(FindLinkRow())->GetStringField(TEXT("linked_slot")), LinkedTargetName)) return false;
	TArray<FString> LinkFiles; IFileManager::Get().FindFilesRecursive(LinkFiles, *Workspace, TEXT("*.hjson"), true, false);
	for (const auto& File : LinkFiles) {
		FString Normalized = File; FPaths::NormalizeFilename(Normalized); if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text; if (!FFileHelper::LoadFileToString(Text, *File)) { AddError(TEXT("slot_link_property_file_read_failed")); return false; }
		if (!TestFalse(TEXT("raw LinkedSlot property exits the ordinary editable surface"), Text.Contains(TEXT("data.LinkedSlot")))) return false;
	}
	TSharedPtr<FJsonValue> LinkFrozen; FString LinkItemId;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, LinkFrozen, Error)) { AddError(TEXT("slot_link_frozen:") + Error); return false; }
	bool FrozenLink = false;
	for (const auto& Pair : LinkFrozen->AsObject()->GetObjectField(TEXT("items"))->Values) {
		if (!Pair.Value->AsObject()->HasField(TEXT("linked_slot"))) continue;
		LinkItemId = Pair.Key;
		for (const auto& Property : Pair.Value->AsObject()->GetObjectField(TEXT("properties"))->GetArrayField(TEXT("properties"))) {
			if (!Property->AsObject()->GetStringField(TEXT("property_path")).EndsWith(TEXT(".LinkedSlot"))) continue;
			FSmartObjectSlotReference Ref;
			FrozenLink = UeAgentPropertyValueCodec::DecodeStructValue(FSmartObjectSlotReference::StaticStruct(),
				Property->AsObject()->TryGetField(TEXT("value")), &Ref, Error) && Ref.GetSlotID() == LinkedTargetId && Ref.GetIndex() == 1;
		}
	}
	if (!TestTrue(TEXT("complete frozen link property retains native identity and derived index"), FrozenLink)) return false;
	Rows->GetObjectField(FindLinkRow())->SetStringField(TEXT("linked_slot"), ParametersName);
	if (!RejectRows(TEXT("smart_object_slot_link_target_invalid")) || !ReadRows(StructureDocument, Rows)) return false;
	Rows->GetObjectField(FindLinkRow())->SetStringField(TEXT("linked_slot"), OriginalSlotName);
	if (!WriteRows(StructureDocument) || !TestTrue(TEXT("changing linked slot resolves native target"), CheckSlotLink(Definition, SlotId, 0))
		|| !ReadRows(StructureDocument, Rows)) return false;
	Rows->GetObjectField(FindLinkRow())->SetField(TEXT("linked_slot"), MakeShared<FJsonValueNull>());
	if (!WriteRows(StructureDocument) || !TestTrue(TEXT("null reference clears native identity and index"), CheckSlotLink(Definition, FGuid(), INDEX_NONE))
		|| !ReadRows(StructureDocument, Rows)) return false;
	Rows->GetObjectField(FindLinkRow())->SetStringField(TEXT("linked_slot"), LinkedTargetName);
	Rows->GetObjectField(LinkedTargetName)->SetNumberField(TEXT("index"), 0);
	Rows->GetObjectField(OriginalSlotName)->SetNumberField(TEXT("index"), 1);
	if (!WriteRows(StructureDocument) || !TestTrue(TEXT("slot reorder preserves linked identity and refreshes native index"), CheckSlotLink(Definition, LinkedTargetId, 0))
		|| !ReadRows(StructureDocument, Rows)) return false;
	Rows->RemoveField(LinkedTargetName);
	if (!RejectRows(TEXT("keyed_reference_target_missing")) || !ReadRows(StructureDocument, Rows)) return false;
	Rows->RemoveField(LinkedTargetName);
	Rows->GetObjectField(OriginalSlotName)->SetNumberField(TEXT("index"), 0);
	Rows->GetObjectField(FindLinkRow())->SetStringField(TEXT("linked_slot"), FString());
	if (!WriteRows(StructureDocument) || !TestTrue(TEXT("atomic target deletion and explicit clear succeeds"), CheckSlotLink(Definition, FGuid(), INDEX_NONE))
		|| !ReadRows(StructureDocument, Rows)) return false;
	AddLinkedTarget(); Rows->GetObjectField(FindLinkRow())->SetStringField(TEXT("linked_slot"), LinkedTargetName);
	if (!WriteRows(StructureDocument)) return false;
	LinkedTargetId = Definition->GetSlots()[1].ID;
	if (!TestTrue(TEXT("existing annotation can reference a same-edit new target"), CheckSlotLink(Definition, LinkedTargetId, 1))) return false;
	Definition->GetMutableSlot(0).bEnabled = true; Definition->MarkPackageDirty();
	TSharedPtr<FJsonValue> LinkDirty;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, LinkDirty, Error)) return false;
	const FString LinkDirtyKey = UeAgentAuthoringState::MakeStableCanonicalValueKey(LinkDirty);
	TArray<uint8> LinkDiskBefore; if (!FFileHelper::LoadFileToArray(LinkDiskBefore, *Filename)) return false;
	const auto LinkDesired = UeAgentAuthoringState::CloneCanonicalValue(LinkDirty);
	LinkDesired->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(LinkItemId)->SetStringField(TEXT("linked_slot"), NativeSlotId);
	UeAgentReflectionAuthoring::FAssetApplyRequest LinkLateRequest;
	LinkLateRequest.TypeId = TEXT("smart_object"); LinkLateRequest.AssetPath = PackageName;
	LinkLateRequest.bSave = true; LinkLateRequest.bCompactValues = true; LinkLateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LinkLateRequest.ProviderExtension->SetObjectField(TEXT("smart_object"), LinkDesired->AsObject());
	bool LinkLateObserved = false; TWeakObjectPtr<USmartObjectDefinition> LinkRejected;
	LinkLateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Rejection) {
		auto* Actual = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath); LinkRejected = Actual;
		TArray<uint8> ChangedDisk;
		LinkLateObserved = Actual && !Actual->GetPackage()->IsDirty() && CheckSlotLink(Actual, SlotId, 0)
			&& FFileHelper::LoadFileToArray(ChangedDisk, *Filename) && ChangedDisk != LinkDiskBefore;
		AddInfo(FString::Printf(TEXT("slot_link_late_stage saved_reload_relation=%d"), LinkLateObserved));
		Rejection.Diagnostic.Code = TEXT("t208_smart_object_slot_link_late_rejected"); Rejection.Diagnostic.PublicMessage = Rejection.Diagnostic.Code; return false;
	};
	Definition = nullptr; UeAgentReflectionAuthoring::FAssetApplyResult LinkLateResult;
	if (!TestFalse(TEXT("late slot link edit is rejected"), UeAgentReflectionAuthoring::ApplyAsset(LinkLateRequest, LinkLateResult, TEXT("smart-object-slot-link-late-") + Name))
		|| !TestTrue(TEXT("new slot relation persisted before late rejection"), LinkLateObserved)
		|| !TestTrue(TEXT("slot link checkpoint recovery succeeds"), LinkLateResult.bRollbackAttempted && LinkLateResult.bRollbackSucceeded && !LinkRejected.IsValid())) return false;
	Definition = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath);
	TSharedPtr<FJsonValue> LinkRestored; TArray<uint8> LinkDiskAfter;
	if (!TestTrue(TEXT("full dirty slot link and independent original disk recover"), Definition && Definition->GetPackage()->IsDirty()
		&& UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, LinkRestored, Error)
		&& UeAgentAuthoringState::MakeStableCanonicalValueKey(LinkRestored) == LinkDirtyKey
		&& FFileHelper::LoadFileToArray(LinkDiskAfter, *Filename) && LinkDiskAfter == LinkDiskBefore
		&& CheckSlotLink(Definition, LinkedTargetId, 1)) || !ReadRows(StructureDocument, Rows)) return false;
	Rows->RemoveField(FindLinkRow()); Rows->RemoveField(LinkedTargetName);
	if (!WriteRows(StructureDocument) || !TestTrue(TEXT("slot link cleanup preserves original owned, parameter and binding state"), CheckPreservedNativeState())) return false;
	AddInfo(TEXT("slot_link ordinary_reference=1 same_edit_create=1 retarget=1 clear=1 reorder=1 dangling_rejected=1 atomic_delete=1 frozen_native=1 late_restore=1"));

	// Preserve a dirty in-memory checkpoint while proving rejection occurs only after save/reload.
	Definition->GetMutableSlot(0).bEnabled = false; Definition->MarkPackageDirty();
	TSharedPtr<FJsonValue> DirtyDocument;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, DirtyDocument, Error)) return false;
	const FString DirtyKey = UeAgentAuthoringState::MakeStableCanonicalValueKey(DirtyDocument);
	TArray<uint8> DiskBefore; if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	const auto Desired = UeAgentAuthoringState::CloneCanonicalValue(DirtyDocument);
	Desired->AsObject()->GetObjectField(TEXT("items"))->SetObjectField(LateBinding->GetStringField(TEXT("id")), LateBinding);
	TSharedPtr<FJsonObject> LateSlot;
	if (!UeAgentSmartObjectAuthoring::BuildDefinitionItemDefaults(*Definition, TEXT("RejectedSlot"), TEXT("slot"),
		FSmartObjectSlotDefinition::StaticStruct(), LateSlot, Error)) return false;
	LateSlot->SetNumberField(TEXT("index"), 1); LateSlot->SetStringField(TEXT("parent"), TEXT("root"));
	Desired->AsObject()->GetObjectField(TEXT("items"))->SetObjectField(LateSlot->GetStringField(TEXT("id")), LateSlot);
	for (const auto& Pair : Desired->AsObject()->GetObjectField(TEXT("items"))->Values) {
		if (Pair.Value->AsObject()->GetStringField(TEXT("kind")) != TEXT("parameters")) continue;
		for (const auto& Property : Pair.Value->AsObject()->GetObjectField(TEXT("properties"))->GetArrayField(TEXT("properties"))) {
			if (!Property->AsObject()->GetStringField(TEXT("property_path")).EndsWith(TEXT("Parameters"))) continue;
			const auto Bag = Property->AsObject()->GetObjectField(TEXT("value"));
			const auto Added = MakeShared<FJsonObject>(); Added->SetStringField(TEXT("name"), TEXT("RejectedParameter")); Added->SetStringField(TEXT("kind"), TEXT("float"));
			auto Schema = Bag->GetArrayField(TEXT("schema")); Schema.Add(MakeShared<FJsonValueObject>(Added)); Bag->SetArrayField(TEXT("schema"), Schema);
			Bag->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("RejectedParameter"), 29.5);
		}
	}
	UeAgentReflectionAuthoring::FAssetApplyRequest LateRequest;
	LateRequest.TypeId = TEXT("smart_object"); LateRequest.AssetPath = PackageName;
	LateRequest.bSave = true; LateRequest.bCompactValues = true;
	LateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LateRequest.ProviderExtension->SetObjectField(TEXT("smart_object"), Desired->AsObject());
	TWeakObjectPtr<USmartObjectDefinition> Rejected;
	bool LateObserved = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Rejection) {
		auto* Actual = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath); Rejected = Actual;
		TArray<uint8> ChangedDisk;
		const bool bClean = Actual && !Actual->GetPackage()->IsDirty();
		const bool bValue = Actual && Actual->GetSlots().Num() == 2 && !Actual->GetSlots()[0].bEnabled && Actual->GetSlots()[1].Name == TEXT("RejectedSlot")
			&& ParametersProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Actual)->FindPropertyDescByName(TEXT("RejectedParameter"))
			&& static_cast<IPropertyBindingBindingCollectionOwner&>(*Actual).GetEditorPropertyBindings()->GetNumBindings() == 2
			&& Actual->GetSlots()[0].Offset.X == 42.5f;
		const bool bDiskRead = FFileHelper::LoadFileToArray(ChangedDisk, *Filename);
		const bool bDiskChanged = bDiskRead && ChangedDisk != DiskBefore;
		LateObserved = bClean && bValue && bDiskChanged;
		AddInfo(FString::Printf(TEXT("late_stage actual=%d clean=%d value=%d disk_read=%d disk_changed=%d"),
			Actual != nullptr, bClean, bValue, bDiskRead, bDiskChanged));
		Rejection.Diagnostic.Code = TEXT("t208_smart_object_late_rejected"); Rejection.Diagnostic.PublicMessage = Rejection.Diagnostic.Code;
		return false;
	};
	Definition = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late SmartObject change is rejected"),
		UeAgentReflectionAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("smart-object-late-") + Name));
	if (!TestTrue(TEXT("changed SmartObject reached saved reload before rejection"), LateObserved)
		|| !TestTrue(TEXT("SmartObject checkpoint recovery succeeds"), LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded)) return false;
	TestFalse(TEXT("rejected SmartObject exits during recovery"), Rejected.IsValid());
	Definition = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath);
	TSharedPtr<FJsonValue> Restored; TArray<uint8> DiskAfter;
	TestTrue(TEXT("dirty in-memory SmartObject restored independently"), Definition && Definition->GetPackage()->IsDirty()
		&& UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, Restored, Error)
		&& UeAgentAuthoringState::MakeStableCanonicalValueKey(Restored) == DirtyKey);
	TestTrue(TEXT("original SmartObject disk restored"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	using namespace Composition::GraphCompound;
	using namespace Contract;
	class FCancellation final : public IUeAgentCancellationSignal { public: bool IsCancelled() const override { return false; } };
	const FGuid CompoundSlotId = FGuid::NewGuid();
	const auto Edit = MakeShared<FJsonObject>();
	Edit->SetStringField(TEXT("slot_guid"), CompoundSlotId.ToString(EGuidFormats::DigitsWithHyphensLower));
	Edit->SetStringField(TEXT("intent"), TEXT("create"));
	Edit->SetStringField(TEXT("behavior_type_ref"), UGameplayInteractionSmartObjectBehaviorDefinition::StaticClass()->GetPathName());
	Edit->SetStringField(TEXT("data_type_ref"), FSmartObjectSlotDefinition::StaticStruct()->GetPathName());
	const auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("target_ref"), ObjectPath);
	Input->SetArrayField(TEXT("slots"), {MakeShared<FJsonValueObject>(Edit)});
	if (!CommandContract::Json::TryCanonicalize(Input, Payload, Error)) return false;
	FGraphCompoundRuntimeConfig RuntimeConfig;
	FGraphCompoundPcgAiProductionConfig ProductionConfig;
	TSharedPtr<FGraphCompoundRuntime> Runtime; FGraphCompoundRuntimeError RuntimeError;
	if (!TryCreatePcgAiGraphProductionRuntime(RuntimeConfig, ProductionConfig, Runtime, RuntimeError))
	{ AddError(RuntimeError.Code); return false; }
	const auto* Profile = FindPcgAnimationGraphRouteProfile(TEXT("ai.smart_object_definition.apply"));
	if (!TestNotNull(TEXT("SmartObject production route"), Profile)) return false;
	Before = Definition; Definition = nullptr;
	const auto Result = Runtime->GetOperationPort()->Submit(FUeAgentOperationRequest(
		Profile->GetExecutionContract().GetDescriptorIdentity(), TEXT("smart_object:") + Name, TEXT("smart_object:") + Name,
		FUeAgentContractValue(Payload)), FUeAgentCallerContext(TEXT("test.agent"),
		{TEXT("pcg_ai_node_graph.read"), TEXT("pcg_ai_node_graph.write")}, TEXT("t208.smart_object")), MakeShared<FCancellation>());
	TSharedPtr<FJsonObject> Response;
	if (!Result.IsValid() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result.GetValue().GetJsonValue()), Response)) return false;
	if (Response->HasField(TEXT("operation_error"))) { AddError(Result.GetValue().GetJsonValue()); return false; }
	Definition = LoadObject<USmartObjectDefinition>(nullptr, *ObjectPath);
	TestTrue(TEXT("GraphCompound uses full document and persistent reload"), !Before.IsValid() && Definition
		&& !Definition->GetPackage()->IsDirty() && Definition->GetSlots().Num() == 2
		&& Definition->GetSlots()[1].ID == CompoundSlotId && Definition->GetSlots()[1].BehaviorDefinitions.Num() == 1);
	TestTrue(TEXT("GraphCompound preserves dirty original values, owned behavior, data and bindings"), Definition
		&& !Definition->GetSlots()[0].bEnabled && Definition->GetSlots()[0].ID == SlotId
		&& Definition->GetSlots()[0].Offset == FVector3f(21,-13,7) && Definition->GetSlots()[0].BehaviorDefinitions.Num() == 1
		&& Definition->GetSlots()[0].DefinitionData.Num() == 1 && Definition->GetSlots()[0].DefinitionData[0].ID == OriginalDataId
		&& Definition->GetSlots()[0].DEBUG_DrawSize == 42.5f);
	AddInfo(TEXT("graph_compound_smart_object full_document_reflection=1 saved_reload=1 original_values_preserved=1"));
	TestTrue(TEXT("GraphCompound independently preserves native owned value, data, parameter and binding endpoints"), CheckPreservedNativeState());
	return !HasAnyErrors();
}
