#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Domains/Blackboard/Authoring/UeAgentBlackboardDocument.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85BlackboardWorkspaceTest,
	"UAI.Root.T85.Blackboard.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BlackboardWorkspaceTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Name = TEXT("Blackboard_Workspace_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85BlackboardWorkspace/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString ParentName = Name + TEXT("_Parent");
	const FString ParentPackageName = PackageName + TEXT("_Parent");
	const FString ParentObjectPath = ParentPackageName + TEXT(".") + ParentName;
	const FString ChildName = Name + TEXT("_Child");
	const FString ChildPackageName = PackageName + TEXT("_Child");
	const FString ChildObjectPath = ChildPackageName + TEXT(".") + ChildName;
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = FindObject<UObject>(nullptr, *ChildObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		if (UObject* Asset = FindObject<UObject>(nullptr, *ParentObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	auto* Blackboard = NewObject<UBlackboardData>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Blackboard);
	FBlackboardEntry Entry;
	Entry.EntryName = TEXT("FocusTarget");
	Entry.EntryDescription = TEXT("BlackboardOriginalDescription");
	Entry.EntryCategory = TEXT("Perception");
	Entry.bInstanceSynced = true;
	auto* KeyType = NewObject<UBlackboardKeyType_Object>(Blackboard, NAME_None, RF_Transactional);
	KeyType->BaseClass = APawn::StaticClass();
	Entry.KeyType = KeyType;
	Blackboard->Keys.Add(Entry);
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateIfHasSynchronizedKeys();
	const int32 InitialKeyCount = Blackboard->Keys.Num();
	const FBlackboard::FKey InitialFocusId = Blackboard->GetKeyID(TEXT("FocusTarget"));
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Blackboard, *Filename, Save)) return false;
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
		/ TEXT("tmp/TROOT-UAI-208-graph-common-foundation/blackboard-common-r1/workspace") / Name);
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	FString Params, Error, Payload;
	HjsonWorkspace::FWorkspaceError Failure;
	bool ReadOnly = false;
	auto ExportTo = [&](const FString& Path)
	{
		Export->SetStringField(TEXT("workspace_path"), Path);
		return CommandContract::Json::TryCanonicalize(Export, Params, Error)
			&& DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
				PackageName, TEXT("/Script/AIModule.BlackboardData"), Params, Payload, ReadOnly, Failure);
	};
	if (!ExportTo(Workspace))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	TestTrue(TEXT("ordinary export is read-only and preserves clean source"), ReadOnly && !Package->IsDirty());
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	bool EditedDescription = false, EditedClass = false;
	for (const FString& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		const bool Description = Text.ReplaceInline(TEXT("BlackboardOriginalDescription"), TEXT("BlackboardEditedDescription")) > 0;
		const bool Class = Text.ReplaceInline(TEXT("/Script/Engine.Pawn"), TEXT("/Script/Engine.Character")) > 0;
		EditedDescription |= Description; EditedClass |= Class;
		if ((Description || Class) && !FFileHelper::SaveStringToFile(Text, *File,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	}
	if (!TestTrue(TEXT("ordinary workspace exposes entry metadata and owned key properties"), EditedDescription && EditedClass)) return false;
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
	FString Revision, Authorization;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources,
		Resolved, Revision, Authorization, Failure))
	{ AddError(Failure.GetCode()); return false; }
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), PackageName); Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	TWeakObjectPtr<UBlackboardData> Before(Blackboard);
	Blackboard = nullptr; KeyType = nullptr; Entry.KeyType = nullptr; Package = nullptr;
	if (!DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
		PackageName, TEXT("/Script/AIModule.BlackboardData"), Params, Payload, ReadOnly, Failure))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetSourceUnit() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	TestFalse(TEXT("successful import releases original asset"), Before.IsValid());
	Blackboard = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("independent persisted reload"), Blackboard)) return false;
	TestFalse(TEXT("persisted asset is clean"), Blackboard->GetOutermost()->IsDirty());
	TestEqual(TEXT("unmodified keys preserved"), Blackboard->Keys.Num(), InitialKeyCount);
	TestEqual(TEXT("ordinary metadata/property edits preserve native key order"), Blackboard->GetKeyID(TEXT("FocusTarget")), InitialFocusId);
	const auto* Actual = Blackboard->Keys.FindByPredicate([](const FBlackboardEntry& Key) { return Key.EntryName == TEXT("FocusTarget"); });
	if (!TestNotNull(TEXT("original named key survives"), Actual)) return false;
	TestEqual(TEXT("entry description persisted"), Actual->EntryDescription, FString(TEXT("BlackboardEditedDescription")));
	TestEqual(TEXT("entry category preserved"), Actual->EntryCategory, FName(TEXT("Perception")));
	TestTrue(TEXT("synchronization preserved"), Actual->bInstanceSynced && Blackboard->HasSynchronizedKeys());
	const auto* ActualType = Cast<UBlackboardKeyType_Object>(Actual->KeyType);
	TestTrue(TEXT("owned key class property persisted"), ActualType && ActualType->BaseClass == ACharacter::StaticClass()
		&& ActualType->GetOuter() == Blackboard);
	const FString Readback = Workspace + TEXT("-readback");
	if (!ExportTo(Readback)) { AddError(Failure.GetCode()); return false; }
	FString PropertyText;
	if (!FFileHelper::LoadFileToString(PropertyText,
		*(Readback / TEXT("structures/blackboard/items/focustarget/properties.hjson")))) return false;
	TestTrue(TEXT("independent ordinary export contains persisted class"), PropertyText.Contains(TEXT("/Script/Engine.Character")));
	const FString StructureFile = Readback / TEXT("structures/blackboard/structure.hjson");
	FString StructureText;
	HjsonWorkspace::FHjsonDocumentPtr Parsed;
	TSharedPtr<FJsonValue> Structure;
	if (!FFileHelper::LoadFileToString(StructureText, *StructureFile)
		|| !HjsonWorkspace::TryParseHjsonDocument(StructureText, StructureFile, Resources, Parsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Structure, Failure)) return false;
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryResult Candidates;
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError CandidateError;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Blackboard, TEXT("Float"), Candidates, CandidateError))
	{ AddError(CandidateError.Code); return false; }
	const auto* FloatCandidate = Candidates.Candidates.FindByPredicate([](const auto& Candidate) {
		return Candidate.Kind == TEXT("blackboard_key") && Candidate.bSupported
			&& Candidate.PayloadType == TEXT("/Script/AIModule.BlackboardKeyType_Float");
	});
	if (!TestNotNull(TEXT("native Float key is discoverable"), FloatCandidate)) return false;
	const auto NewKey = MakeShared<FJsonObject>();
	NewKey->SetStringField(TEXT("name"), FloatCandidate->Name);
	NewKey->SetStringField(TEXT("description"), TEXT("CreatedViaWorkspace"));
	const auto InitialValue = MakeShared<FJsonObject>();
	InitialValue->SetStringField(TEXT("type"), TEXT("float")); InitialValue->SetNumberField(TEXT("value"), 32.5);
	const auto InitialFields = MakeShared<FJsonObject>(); InitialFields->SetObjectField(TEXT("DefaultValue"), InitialValue);
	const auto InitialCategories = MakeShared<FJsonObject>(); InitialCategories->SetObjectField(TEXT("Blackboard"), InitialFields);
	NewKey->SetObjectField(TEXT("categories"), InitialCategories);
	Structure->AsObject()->GetObjectField(TEXT("blackboard_keys"))->SetObjectField(TEXT("Distance"), NewKey);
	if (!CommandContract::Json::TryCanonicalize(Structure->AsObject(), StructureText, Error)
		|| !FFileHelper::SaveStringToFile(StructureText, *StructureFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	auto ImportFrom = [&](const FString& Path) {
		Source->SetStringField(TEXT("path"), Path);
		if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources,
			Resolved, Revision, Authorization, Failure)) return false;
		Import->SetStringField(TEXT("expected_revision"), Revision);
		Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
		if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
		else Import->RemoveField(TEXT("authorization_id"));
		return CommandContract::Json::TryCanonicalize(Import, Params, Error)
			&& DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
				PackageName, TEXT("/Script/AIModule.BlackboardData"), Params, Payload, ReadOnly, Failure);
	};
	Blackboard = nullptr;
	if (!ImportFrom(Readback))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Blackboard = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	const auto* Added = Blackboard ? Blackboard->Keys.FindByPredicate([](const auto& Key) { return Key.EntryName == TEXT("Distance"); }) : nullptr;
	if (!TestTrue(TEXT("ordinary native candidate creates and persists a Float key"), Added && Added->KeyType
		&& Added->KeyType->GetClass()->GetPathName() == TEXT("/Script/AIModule.BlackboardKeyType_Float")
		&& Added->EntryDescription == TEXT("CreatedViaWorkspace") && Blackboard->Keys.Num() == InitialKeyCount + 1
		&& !Blackboard->GetPackage()->IsDirty())) return false;
	TestTrue(TEXT("new key initial property uses common categorized projection"),
		Cast<UBlackboardKeyType_Float>(Added->KeyType)->DefaultValue == 32.5f);
	const FString DeleteWorkspace = Workspace + TEXT("-delete");
	if (!ExportTo(DeleteWorkspace)) { AddError(Failure.GetCode()); return false; }
	const FString DeleteFile = DeleteWorkspace / TEXT("structures/blackboard/structure.hjson");
	if (!FFileHelper::LoadFileToString(StructureText, *DeleteFile)
		|| !HjsonWorkspace::TryParseHjsonDocument(StructureText, DeleteFile, Resources, Parsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Structure, Failure)) return false;
	Structure->AsObject()->GetObjectField(TEXT("blackboard_keys"))->RemoveField(TEXT("Distance"));
	if (!CommandContract::Json::TryCanonicalize(Structure->AsObject(), StructureText, Error)
		|| !FFileHelper::SaveStringToFile(StructureText, *DeleteFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Blackboard = nullptr;
	if (!ImportFrom(DeleteWorkspace))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Blackboard = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("ordinary omission deletes only the chosen key"), Blackboard && Blackboard->Keys.Num() == InitialKeyCount
		&& Blackboard->GetKeyID(TEXT("Distance")) == FBlackboard::InvalidKey && !Blackboard->GetPackage()->IsDirty())) return false;
	UPackage* ChildPackage = CreatePackage(*ChildPackageName);
	auto* ChildBoard = NewObject<UBlackboardData>(ChildPackage, *ChildName, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(ChildBoard);
	ChildBoard->Parent = Blackboard;
	FBlackboardEntry ChildEntry;
	ChildEntry.EntryName = TEXT("ChildDistance");
	auto* ChildType = NewObject<UBlackboardKeyType_Float>(ChildBoard, NAME_None, RF_Transactional);
	ChildType->DefaultValue = 75.5f; ChildEntry.KeyType = ChildType;
	ChildBoard->Keys.Add(ChildEntry); ChildBoard->UpdateParentKeys(); ChildBoard->UpdateIfHasSynchronizedKeys();
	const FString ChildFilename = FPackageName::LongPackageNameToFilename(ChildPackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(ChildPackage, ChildBoard, *ChildFilename, Save)) return false;
	TArray<uint8> ChildDiskBefore;
	if (!FFileHelper::LoadFileToArray(ChildDiskBefore, *ChildFilename)) return false;
	ChildBoard->Modify(); ChildType->DefaultValue = 102.25f;
	TSharedPtr<FJsonValue> ChildDocument;
	FString ChildDirtyBefore;
	if (!UeAgentBlackboardAuthoring::ReadDocument(*ChildBoard, ChildDocument, Error)
		|| !CommandContract::Json::TryCanonicalize(ChildDocument->AsObject(), ChildDirtyBefore, Error)) return false;
	ChildBoard = nullptr; ChildType = nullptr;
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	Blackboard->Modify();
	auto* DirtyKey = Blackboard->Keys.FindByPredicate([](const auto& Key) { return Key.EntryName == TEXT("FocusTarget"); });
	if (!DirtyKey) return false;
	DirtyKey->EntryDescription = TEXT("DirtyBeforeRejection");
	TSharedPtr<FJsonValue> Desired;
	FString DirtyBefore;
	if (!UeAgentBlackboardAuthoring::ReadDocument(*Blackboard, Desired, Error)
		|| !CommandContract::Json::TryCanonicalize(Desired->AsObject(), DirtyBefore, Error)) return false;
	Desired->AsObject()->GetObjectField(TEXT("blackboard_keys"))->RemoveField(TEXT("FocusTarget"));
	TWeakObjectPtr<UBlackboardData> Original(Blackboard), Rejected;
	UeAgentReflectionAuthoring::FAssetApplyRequest LateRequest;
	LateRequest.TypeId = TEXT("blackboard"); LateRequest.AssetPath = PackageName;
	LateRequest.bSave = true; LateRequest.bCompactValues = true;
	LateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LateRequest.ProviderExtension->SetObjectField(TEXT("blackboard"), Desired->AsObject());
	bool LateObserved = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Rejection) {
		auto* Reloaded = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
		Rejected = Reloaded;
		TArray<uint8> Written;
		LateObserved = !Original.IsValid(true, true) && Reloaded && !Reloaded->GetPackage()->IsDirty()
			&& Reloaded->GetKeyID(TEXT("FocusTarget")) == FBlackboard::InvalidKey
			&& !Reloaded->HasSynchronizedKeys() && Reloaded->Keys.Num() == InitialKeyCount - 1
			&& FFileHelper::LoadFileToArray(Written, *Filename) && Written != DiskBefore;
		auto* LoadedChild = FindObject<UBlackboardData>(nullptr, *ChildObjectPath);
		AddInfo(FString::Printf(TEXT("late derived observation: original_released=%d parent_persisted=%d child_loaded=%d child_rebound=%d child_dirty=%d child_synced=%d child_has_removed_key=%d"),
			!Original.IsValid(true, true), LateObserved, LoadedChild != nullptr, LoadedChild && LoadedChild->Parent == Reloaded,
			LoadedChild && LoadedChild->GetPackage()->IsDirty(), LoadedChild && LoadedChild->HasSynchronizedKeys(),
			LoadedChild && LoadedChild->GetKeyID(TEXT("FocusTarget")) != FBlackboard::InvalidKey));
		LateObserved = LateObserved && LoadedChild && LoadedChild->Parent == Reloaded
			&& LoadedChild->GetPackage()->IsDirty() && !LoadedChild->HasSynchronizedKeys()
			&& LoadedChild->GetKeyID(TEXT("FocusTarget")) == FBlackboard::InvalidKey;
		Rejection.ErrorCode = TEXT("t208_blackboard_late_rejected");
		Rejection.ErrorDetail = Rejection.ErrorCode; return false;
	};
	Blackboard = nullptr; DirtyKey = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late Blackboard deletion is rejected before commit"),
		UeAgentReflectionAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("blackboard-late-") + Name));
	if (!TestTrue(TEXT("deletion and synchronized cache persisted before rejection"), LateObserved)
		|| !TestEqual(TEXT("late diagnostic retained"), LateResult.ErrorCode, FString(TEXT("t208_blackboard_late_rejected")))
		|| !TestTrue(TEXT("Blackboard checkpoint recovery confirmed"), LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded))
	{ AddError(LateResult.Phase + TEXT(":") + LateResult.ErrorDetail); return false; }
	Blackboard = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	TSharedPtr<FJsonValue> Restored;
	FString RestoredText;
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("complete dirty Blackboard document restored"), Blackboard && Blackboard->GetPackage()->IsDirty()
		&& UeAgentBlackboardAuthoring::ReadDocument(*Blackboard, Restored, Error)
		&& CommandContract::Json::TryCanonicalize(Restored->AsObject(), RestoredText, Error) && RestoredText == DirtyBefore);
	TestTrue(TEXT("original disk restored independently of dirty memory"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	TestFalse(TEXT("rejected target released during recovery"), Rejected.IsValid(true, true));
	ChildBoard = FindObject<UBlackboardData>(nullptr, *ChildObjectPath);
	FString ChildRestoredText;
	TArray<uint8> ChildDiskAfter;
	TestTrue(TEXT("loaded derived Blackboard restores parent binding, dirty local values and inherited cache"),
		ChildBoard && ChildBoard->Parent == Blackboard && ChildBoard->GetPackage()->IsDirty()
		&& ChildBoard->HasSynchronizedKeys() && ChildBoard->GetKeyID(TEXT("FocusTarget")) != FBlackboard::InvalidKey
		&& UeAgentBlackboardAuthoring::ReadDocument(*ChildBoard, ChildDocument, Error)
		&& CommandContract::Json::TryCanonicalize(ChildDocument->AsObject(), ChildRestoredText, Error)
		&& ChildRestoredText == ChildDirtyBefore
		&& FFileHelper::LoadFileToArray(ChildDiskAfter, *ChildFilename) && ChildDiskAfter == ChildDiskBefore);
	ChildBoard = nullptr;
	using namespace Composition::GraphCompound;
	using namespace Contract;
	class FCancellation final : public IUeAgentCancellationSignal { public: bool IsCancelled() const override { return false; } };
	const auto Edit = MakeShared<FJsonObject>();
	const FString KeyClass = TEXT("/Script/AIModule.BlackboardKeyType_Float");
	Edit->SetStringField(TEXT("entry_name"), TEXT("CompoundDistance"));
	Edit->SetStringField(TEXT("intent"), TEXT("create"));
	Edit->SetStringField(TEXT("key_type_ref"), KeyClass); Edit->SetStringField(TEXT("ownership"), TEXT("local"));
	const auto Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("target_ref"), ObjectPath);
	Input->SetArrayField(TEXT("entries"), {MakeShared<FJsonValueObject>(Edit)});
	if (!CommandContract::Json::TryCanonicalize(Input, Payload, Error)) return false;
	FGraphCompoundRuntimeConfig RuntimeConfig;
	FGraphCompoundPcgAiProductionConfig ProductionConfig;
	TSharedPtr<FGraphCompoundRuntime> Runtime;
	FGraphCompoundRuntimeError RuntimeError;
	if (!TryCreatePcgAiGraphProductionRuntime(RuntimeConfig, ProductionConfig, Runtime, RuntimeError))
	{ AddError(RuntimeError.Code); return false; }
	const auto* Profile = FindPcgAnimationGraphRouteProfile(TEXT("ai.blackboard.schema.apply"));
	if (!TestNotNull(TEXT("Blackboard production route"), Profile)) return false;
	Before = Blackboard; Blackboard = nullptr;
	const auto Result = Runtime->GetOperationPort()->Submit(FUeAgentOperationRequest(
		Profile->GetExecutionContract().GetDescriptorIdentity(), TEXT("blackboard:") + Name, TEXT("blackboard:") + Name,
		FUeAgentContractValue(Payload)), FUeAgentCallerContext(TEXT("test.agent"),
		{TEXT("pcg_ai_node_graph.read"), TEXT("pcg_ai_node_graph.write")}, TEXT("t208.blackboard")), MakeShared<FCancellation>());
	TSharedPtr<FJsonObject> Response;
	if (!Result.IsValid() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result.GetValue().GetJsonValue()), Response)) return false;
	if (Response->HasField(TEXT("operation_error"))) { AddError(Result.GetValue().GetJsonValue()); return false; }
	Blackboard = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	TestTrue(TEXT("GraphCompound uses persistent reload and creates native key"), !Before.IsValid() && Blackboard
		&& !Blackboard->GetPackage()->IsDirty() && Blackboard->Keys.Num() == InitialKeyCount + 1
		&& Blackboard->GetKeyID(TEXT("CompoundDistance")) != FBlackboard::InvalidKey);
	const auto* Preserved = Blackboard ? Blackboard->Keys.FindByPredicate([](const auto& Key) { return Key.EntryName == TEXT("FocusTarget"); }) : nullptr;
	const auto* PreservedType = Preserved ? Cast<UBlackboardKeyType_Object>(Preserved->KeyType) : nullptr;
	TestTrue(TEXT("GraphCompound preserves complete existing key state"), Preserved && PreservedType
		&& Preserved->EntryDescription == TEXT("DirtyBeforeRejection") && Preserved->EntryCategory == TEXT("Perception")
		&& Preserved->bInstanceSynced && PreservedType->BaseClass == ACharacter::StaticClass());
	ChildBoard = FindObject<UBlackboardData>(nullptr, *ChildObjectPath);
	const auto* ChildLocal = ChildBoard ? ChildBoard->Keys.FindByPredicate([](const auto& Key) {
		return Key.EntryName == TEXT("ChildDistance");
	}) : nullptr;
	const auto* PreservedChildType = ChildLocal ? Cast<UBlackboardKeyType_Float>(ChildLocal->KeyType) : nullptr;
	TestTrue(TEXT("committed parent edit rebinds dirty child and preserves local state and disk"),
		ChildBoard && ChildBoard->Parent == Blackboard && ChildBoard->GetPackage()->IsDirty()
		&& ChildBoard->GetKeyID(TEXT("CompoundDistance")) != FBlackboard::InvalidKey && ChildBoard->HasSynchronizedKeys()
		&& PreservedChildType && PreservedChildType->DefaultValue == 102.25f
		&& FFileHelper::LoadFileToArray(ChildDiskAfter, *ChildFilename) && ChildDiskAfter == ChildDiskBefore);
	if (ChildBoard) ObjectTools::DeleteObjectsUnchecked({ChildBoard});
	ChildBoard = nullptr;
	using namespace DomainAdapters::ContentGraphRead;
	FUeAgentContentGraphCapture CaptureBefore, CaptureAfter;
	FUeAgentContentGraphReadError CaptureError;
	if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Blackboard, CaptureBefore, CaptureError))
	{ AddError(CaptureError.Code + TEXT(":") + CaptureError.Detail); return false; }
	TSharedPtr<const StateSemantic::FUeAgentCanonicalSnapshot> ReadSnapshot;
	const auto ReadResources = HjsonWorkspace::MakeSemanticWorkspaceResourceTrackerOwner();
	if (!TryBuildContentGraphAuthoringSnapshot(CaptureBefore, ReadResources, ReadSnapshot, CaptureError))
	{ AddError(CaptureError.Code + TEXT(":") + CaptureError.Detail); return false; }
	const auto* FocusItem = CaptureBefore.Items.FindByPredicate([](const auto& Item) {
		return Item.SemanticIdentity.Selectors.FindRef(TEXT("key_name")) == TEXT("FocusTarget");
	});
	if (!TestNotNull(TEXT("read side retains semantic key identity"), FocusItem)) return false;
	TestTrue(TEXT("read side exposes native name for public owner selection"), FocusItem->State.IsValid()
		&& FocusItem->State->GetObjectItems().ContainsByPredicate([](const auto& Field) {
			return Field.Key == TEXT("name") && Field.Value->GetText() == TEXT("FocusTarget");
		}));
	TestTrue(TEXT("read side binds BaseClass to the key rather than an unowned UObject"),
		CaptureBefore.Properties.ContainsByPredicate([&](const auto& Property) {
			return Property.OwnerId == FocusItem->ItemId && Property.FieldId == TEXT("BaseClass");
		}));
	auto* ChangedType = const_cast<UBlackboardKeyType_Object*>(PreservedType);
	ChangedType->BaseClass = APawn::StaticClass();
	if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Blackboard, CaptureAfter, CaptureError))
	{ AddError(CaptureError.Code); return false; }
	TestTrue(TEXT("read revision changes when key property changes without count or dirty changes"),
		CaptureBefore.CanonicalRevision.IsSet() && CaptureAfter.CanonicalRevision.IsSet()
		&& CaptureBefore.CanonicalRevision.GetValue() != CaptureAfter.CanonicalRevision.GetValue()
		&& CaptureBefore.Revision != CaptureAfter.Revision);
	ChangedType->BaseClass = ACharacter::StaticClass();
	auto* ParentPackage = CreatePackage(*ParentPackageName);
	auto* ParentBoard = NewObject<UBlackboardData>(ParentPackage, *ParentName, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(ParentBoard);
	FBlackboardEntry Inherited;
	Inherited.EntryName = TEXT("InheritedDistance"); Inherited.bInstanceSynced = true;
	Inherited.KeyType = NewObject<UBlackboardKeyType_Float>(ParentBoard, NAME_None, RF_Transactional);
	ParentBoard->Keys.Add(Inherited); ParentBoard->UpdateParentKeys(); ParentBoard->UpdateIfHasSynchronizedKeys();
	const FString ParentFilename = FPackageName::LongPackageNameToFilename(ParentPackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(ParentPackage, ParentBoard, *ParentFilename, Save)) return false;
	auto ChangeParent = [&](const FString& Path, const FString& Parent) {
		if (!ExportTo(Path)) return false;
		const FString File = Path / TEXT("structures/blackboard/structure.hjson");
		if (!FFileHelper::LoadFileToString(StructureText, *File)
			|| !HjsonWorkspace::TryParseHjsonDocument(StructureText, File, Resources, Parsed, Failure)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Structure, Failure)) return false;
		Structure->AsObject()->SetStringField(TEXT("parent"), Parent);
		if (!CommandContract::Json::TryCanonicalize(Structure->AsObject(), StructureText, Error)
			|| !FFileHelper::SaveStringToFile(StructureText, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		Blackboard = nullptr;
		return ImportFrom(Path);
	};
	if (!ChangeParent(Workspace + TEXT("-parent"), ParentPackageName))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Blackboard = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("Parent import persists reference and inherited key cache"), Blackboard && Blackboard->Parent == ParentBoard
		&& Blackboard->GetKeyID(TEXT("InheritedDistance")) != FBlackboard::InvalidKey
		&& Blackboard->ParentKeys.ContainsByPredicate([](const auto& Key) { return Key.EntryName == TEXT("InheritedDistance"); })
		&& !Blackboard->Keys.ContainsByPredicate([](const auto& Key) { return Key.EntryName == TEXT("InheritedDistance") || Key.EntryName == TEXT("SelfActor"); })
		&& Blackboard->HasSynchronizedKeys() && !Blackboard->GetPackage()->IsDirty())) return false;
	FUeAgentContentGraphCapture InheritedCapture;
	if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Blackboard, InheritedCapture, CaptureError)
		|| !TryBuildContentGraphAuthoringSnapshot(InheritedCapture, ReadResources, ReadSnapshot, CaptureError))
	{ AddError(CaptureError.Code + TEXT(":") + CaptureError.Detail); return false; }
	TestTrue(TEXT("inherited key read exposes native name"), InheritedCapture.Items.ContainsByPredicate([](const auto& Item) {
		return Item.State.IsValid() && Item.State->GetObjectItems().ContainsByPredicate([](const auto& Field) {
			return Field.Key == TEXT("name") && Field.Value->GetText() == TEXT("InheritedDistance");
		});
	}));
	if (!ChangeParent(Workspace + TEXT("-unparent"), TEXT("")))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Blackboard = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	TestTrue(TEXT("detaching Parent clears inherited cache and restores native root SelfActor"), Blackboard && !Blackboard->Parent
		&& Blackboard->ParentKeys.IsEmpty() && Blackboard->GetKeyID(TEXT("InheritedDistance")) == FBlackboard::InvalidKey
		&& Blackboard->GetKeyID(TEXT("SelfActor")) != FBlackboard::InvalidKey
		&& Blackboard->GetKeyID(TEXT("FocusTarget")) != FBlackboard::InvalidKey && !Blackboard->GetPackage()->IsDirty());
	const FString TypeWorkspace = Workspace + TEXT("-type");
	if (!ExportTo(TypeWorkspace)) return false;
	const FString TypeFile = TypeWorkspace / TEXT("structures/blackboard/structure.hjson");
	if (!FFileHelper::LoadFileToString(StructureText, *TypeFile)
		|| !HjsonWorkspace::TryParseHjsonDocument(StructureText, TypeFile, Resources, Parsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Structure, Failure)) return false;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Blackboard, TEXT("Object"), Candidates, CandidateError)) return false;
	const auto* ObjectCandidate = Candidates.Candidates.FindByPredicate([](const auto& Candidate) {
		return Candidate.Kind == TEXT("blackboard_key") && Candidate.PayloadType == TEXT("/Script/AIModule.BlackboardKeyType_Object");
	});
	if (!TestNotNull(TEXT("Object key candidate available for type replacement"), ObjectCandidate)) return false;
	const auto Replacement = Structure->AsObject()->GetObjectField(TEXT("blackboard_keys"))->GetObjectField(TEXT("CompoundDistance"));
	const auto TypeSelector = MakeShared<FJsonObject>(); TypeSelector->SetStringField(TEXT("name"), ObjectCandidate->Name);
	Replacement->SetObjectField(TEXT("candidate"), TypeSelector);
	InitialValue->SetStringField(TEXT("type"), TEXT("class_ref")); InitialValue->SetStringField(TEXT("value"), TEXT("/Script/Engine.Character"));
	InitialFields->RemoveField(TEXT("DefaultValue")); InitialFields->SetObjectField(TEXT("BaseClass"), InitialValue);
	Replacement->SetObjectField(TEXT("categories"), InitialCategories);
	const FBlackboard::FKey BeforeTypeId = Blackboard->GetKeyID(TEXT("CompoundDistance"));
	if (!CommandContract::Json::TryCanonicalize(Structure->AsObject(), StructureText, Error)
		|| !FFileHelper::SaveStringToFile(StructureText, *TypeFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Blackboard = nullptr;
	if (!ImportFrom(TypeWorkspace)) { AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Blackboard = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	const auto* Replaced = Blackboard ? Blackboard->Keys.FindByPredicate([](const auto& Key) { return Key.EntryName == TEXT("CompoundDistance"); }) : nullptr;
	const auto* ReplacedType = Replaced ? Cast<UBlackboardKeyType_Object>(Replaced->KeyType) : nullptr;
	TestTrue(TEXT("existing key type replacement uses candidate and native default properties"), ReplacedType
		&& ReplacedType->BaseClass == ACharacter::StaticClass() && ReplacedType->GetOuter() == Blackboard
		&& Blackboard->GetKeyID(TEXT("CompoundDistance")) == BeforeTypeId && !Blackboard->GetPackage()->IsDirty());
	const FString EnumWorkspace = Workspace + TEXT("-enum");
	if (!ExportTo(EnumWorkspace)) return false;
	const FString EnumFile = EnumWorkspace / TEXT("structures/blackboard/structure.hjson");
	if (!FFileHelper::LoadFileToString(StructureText, *EnumFile)
		|| !HjsonWorkspace::TryParseHjsonDocument(StructureText, EnumFile, Resources, Parsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Structure, Failure)) return false;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Blackboard, TEXT("BlackboardKeyType_Enum"), Candidates, CandidateError)) return false;
	const auto* EnumCandidate = Candidates.Candidates.FindByPredicate([](const auto& Candidate) {
		return Candidate.PayloadType == TEXT("/Script/AIModule.BlackboardKeyType_Enum");
	});
	if (!TestNotNull(TEXT("Enum key candidate available"), EnumCandidate)) return false;
	const auto EnumKey = MakeShared<FJsonObject>(); EnumKey->SetStringField(TEXT("name"), EnumCandidate->Name);
	const auto EnumNameValue = MakeShared<FJsonObject>();
	EnumNameValue->SetStringField(TEXT("type"), TEXT("string")); EnumNameValue->SetStringField(TEXT("value"), TEXT("ECollisionChannel"));
	const auto EnumFields = MakeShared<FJsonObject>(); EnumFields->SetObjectField(TEXT("EnumName"), EnumNameValue);
	const auto EnumCategories = MakeShared<FJsonObject>(); EnumCategories->SetObjectField(TEXT("Blackboard"), EnumFields);
	EnumKey->SetObjectField(TEXT("categories"), EnumCategories);
	Structure->AsObject()->GetObjectField(TEXT("blackboard_keys"))->SetObjectField(TEXT("Mode"), EnumKey);
	if (!CommandContract::Json::TryCanonicalize(Structure->AsObject(), StructureText, Error)
		|| !FFileHelper::SaveStringToFile(StructureText, *EnumFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Blackboard = nullptr;
	if (!ImportFrom(EnumWorkspace)) { AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	auto CheckEnum = [&](const FString& Expected) {
		Blackboard = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
		const auto* Key = Blackboard ? Blackboard->Keys.FindByPredicate([](const auto& Entry) { return Entry.EntryName == TEXT("Mode"); }) : nullptr;
		const auto* Type = Key ? Cast<UBlackboardKeyType_Enum>(Key->KeyType) : nullptr;
		return Type && Type->EnumType && Type->EnumType->GetName() == Expected && Type->EnumName == Expected
			&& Type->bIsEnumNameValid && !Blackboard->GetPackage()->IsDirty();
	};
	if (!TestTrue(TEXT("new EnumName resolves native EnumType before persistence"), CheckEnum(TEXT("ECollisionChannel")))) return false;
	const FString EnumEditWorkspace = Workspace + TEXT("-enum-edit");
	if (!ExportTo(EnumEditWorkspace)) return false;
	Files.Reset(); IFileManager::Get().FindFilesRecursive(Files, *EnumEditWorkspace, TEXT("properties.hjson"), true, false);
	bool EnumEdited = false;
	for (const FString& File : Files) {
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		// Only the string value changes; keep the exported EnumType reference as-is.
		const int32 Changed = Text.ReplaceInline(TEXT("value: \"ECollisionChannel\""), TEXT("value: \"ECollisionResponse\""));
		if (Changed) {
			EnumEdited = true;
			if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		}
	}
	if (!TestTrue(TEXT("ordinary enum name field was edited"), EnumEdited)) return false;
	Blackboard = nullptr;
	if (!ImportFrom(EnumEditWorkspace)) { AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	TestTrue(TEXT("editing only EnumName updates native EnumType"), CheckEnum(TEXT("ECollisionResponse")));
	return !HasAnyErrors();
}
