#include "Misc/AutomationTest.h"
#include "Sound/SoundClass.h"
#include "SoundClassGraph/SoundClassGraph.h"
#include "SoundClassGraph/SoundClassGraphNode.h"
#include "SoundClassGraph/SoundClassGraphSchema.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace
{
int32 EditSoundClassField(const TSharedPtr<FJsonObject>& Object, const FString& Name, const TSharedPtr<FJsonValue>& Value)
{
	int32 Count = 0;
	for (auto& Field : Object->Values)
	{
		if (Field.Key == Name)
		{
			if (Field.Value->Type == EJson::Object && Field.Value->AsObject()->HasField(TEXT("value")))
				Field.Value->AsObject()->SetField(TEXT("value"), Value);
			else Field.Value = Value;
			++Count;
		}
		else if (Field.Value->Type == EJson::Object) Count += EditSoundClassField(Field.Value->AsObject(), Name, Value);
	}
	return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208SoundClassWorkspaceTest,
	"UAI.Root.T208.SoundClass.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208SoundClassWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Base = TEXT("/Game/UAI/Transient/T208SoundClass/") + Token;
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-sound-class/native") / Token);
	const TArray<FString> Names{TEXT("Root"), TEXT("OldParent"), TEXT("Moved"), TEXT("Removed"), TEXT("Added"), TEXT("CleanGraph"), TEXT("DirtyGraph")};
	TArray<FString> Paths, Packages, Filenames;
	for (const auto& Name : Names)
	{
		Packages.Add(Base / Name); Paths.Add(Packages.Last() + TEXT(".") + Name);
		Filenames.Add(FPackageName::LongPackageNameToFilename(Packages.Last(), FPackageName::GetAssetPackageExtension()));
	}
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-sound-class"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
	auto Load = [&](int32 Index) { return LoadObject<USoundClass>(nullptr, *Paths[Index]); };
	auto Save = [&](int32 Index) {
		auto* Object = Load(Index); FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
		return Object && UPackage::SavePackage(Object->GetPackage(), Object, *Filenames[Index], Args); };
	ON_SCOPE_EXIT
	{
		TArray<TSharedPtr<FJsonValue>> Targets;
		for (int32 Index = 0; Index < Paths.Num(); ++Index)
		{
			if (auto* Object = FindObject<USoundClass>(nullptr, *Paths[Index]); Object && Object->GetPackage()->IsDirty())
				TestTrue(TEXT("settle own disposable fixture after assertions"), Save(Index));
			if (FPackageName::DoesPackageExist(Packages[Index])) Targets.Add(MakeShared<FJsonValueString>(Paths[Index]));
		}
		if (!Targets.IsEmpty())
		{
			auto Delete = MakeShared<FJsonObject>(); Delete->SetArrayField(TEXT("targets"), Targets); Delete->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true}) { Delete->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Delete); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
		}
	};
	// Native graph and parentage are fixture setup, never public authoring proof.
	for (int32 Index = 0; Index < Paths.Num(); ++Index)
	{
		auto* Object = NewObject<USoundClass>(CreatePackage(*Packages[Index]), *Names[Index], RF_Public | RF_Standalone | RF_Transactional);
		Object->Properties.Pitch = 1.1f + 0.1f * Index;
	}
	Load(0)->ChildClasses.Add(Load(3)); Load(3)->ParentClass = Load(0);
	Load(1)->ChildClasses.Add(Load(2)); Load(2)->ParentClass = Load(1);
	for (int32 Index = 0; Index < Paths.Num(); ++Index) if (!Save(Index)) return false;
	TArray<USoundClassGraph*> UnrelatedGraphs;
	TArray<UEdGraphNode*> UnrelatedNodes;
	for (int32 Index : {5, 6})
	{
		auto* Object = Load(Index);
		auto* Graph = NewObject<USoundClassGraph>(Object, NAME_None, RF_Transactional);
		Graph->Schema = USoundClassGraphSchema::StaticClass(); Graph->SetRootSoundClass(Object);
		Object->SoundClassGraph = Graph; Graph->RebuildGraph();
		if (!TestTrue(TEXT("native unrelated derived graph exists"), Graph->Nodes.Num() == 1)) return false;
		Graph->Nodes[0]->NodePosX = 137 + Index; Graph->Nodes[0]->NodePosY = -219 - Index;
		UnrelatedGraphs.Add(Graph); UnrelatedNodes.Add(Graph->Nodes[0]);
	}
	Load(6)->Properties.Volume = 0.82f; Load(6)->MarkPackageDirty();
	TArray<TArray<uint8>> InitialDisk; InitialDisk.SetNum(Paths.Num());
	for (int32 Index = 0; Index < Paths.Num(); ++Index) if (!FFileHelper::LoadFileToArray(InitialDisk[Index], *Filenames[Index])) return false;
	auto UnrelatedPreserved = [&]() {
		bool Good = !Load(5)->GetPackage()->IsDirty() && Load(6)->GetPackage()->IsDirty() && Load(6)->Properties.Volume == 0.82f;
		for (int32 Index : {5, 6})
		{
			const int32 Local = Index - 5; auto* Graph = UnrelatedGraphs[Local]; TArray<uint8> Bytes;
			Good &= Load(Index)->SoundClassGraph == Graph && Graph->Nodes.Num() == 1 && Graph->Nodes[0] == UnrelatedNodes[Local]
				&& Graph->Nodes[0]->NodePosX == 137 + Index && Graph->Nodes[0]->NodePosY == -219 - Index
				&& FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) && Bytes == InitialDisk[Index];
		}
		return TestTrue(TEXT("unrelated clean and dirty graph packages layout identity and disk preserved"), Good);
	};
	auto Export = [&](int32 Index, const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[Index]); Args->SetStringField(TEXT("workspace_path"), Directory / Stage);
		Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring")); const auto Result = Submit(TEXT("content_graph.export_workspace"), Args);
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
	auto Edit = [&](const FString& Stage, const TArray<int32>& Children, TOptional<float> Volume) {
		TArray<TSharedPtr<FJsonValue>> References; for (int32 Index : Children) References.Add(MakeShared<FJsonValueString>(Paths[Index]));
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		int32 Relations = 0, Scalars = 0;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) || File.EndsWith(TEXT("definitions.hjson"))) continue;
			FString Text; TSharedPtr<FJsonObject> Object;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Object, Error)) { AddError(Error); return false; }
			const int32 ChangedRelations = EditSoundClassField(Object, TEXT("child_classes"), MakeShared<FJsonValueArray>(References));
			const int32 ChangedScalars = Volume.IsSet() ? EditSoundClassField(Object, TEXT("Volume"), MakeShared<FJsonValueNumber>(Volume.GetValue())) : 0;
			Relations += ChangedRelations; Scalars += ChangedScalars;
			if (ChangedRelations || ChangedScalars)
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Object.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { AddError(Error); return false; }
		}
		return TestEqual(TEXT("one ordinary child relationship surface"), Relations, 1)
			&& (!Volume.IsSet() || TestEqual(TEXT("one editable root volume"), Scalars, 1));
	};
	auto Import = [&](int32 Index, const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), Paths[Index]); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args); };
	if (!Export(0, TEXT("relations")) || !Edit(TEXT("relations"), {2, 4}, 0.45f)) return false;
	TArray<TWeakObjectPtr<USoundClass>> BeforeObjects;
	for (int32 Index = 0; Index < 5; ++Index) BeforeObjects.Add(Load(Index));
	auto Result = Import(0, TEXT("relations"));
	if (!TestTrue(TEXT("public root settings and multiple child relationship changes"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	auto Published = [&]() { return Load(0)->ChildClasses.Num() == 2 && Load(0)->ChildClasses[0] == Load(2)
		&& Load(0)->ChildClasses[1] == Load(4) && Load(2)->ParentClass == Load(0) && Load(4)->ParentClass == Load(0)
		&& Load(1)->ChildClasses.IsEmpty() && !Load(3)->ParentClass; };
	if (!TestTrue(TEXT("native parent child removal adoption and order agree"), Published()) || !UnrelatedPreserved()) return false;
	for (int32 Index = 0; Index < 5; ++Index)
		if (!TestTrue(TEXT("each affected package independently replaced and persisted"), !BeforeObjects[Index].IsValid()
			&& !Load(Index)->GetPackage()->IsDirty() && FMath::IsNearlyEqual(Load(Index)->Properties.Pitch, 1.1f + 0.1f * Index))) return false;
	if (!TestTrue(TEXT("root setting persists with relationships"), Load(0)->Properties.Volume == 0.45f)) return false;
	if (!Export(0, TEXT("readback"))) return false;
	TArray<TArray<uint8>> PublishedDisk; PublishedDisk.SetNum(5);
	for (int32 Index = 0; Index < 5; ++Index) if (!FFileHelper::LoadFileToArray(PublishedDisk[Index], *Filenames[Index])) return false;
	if (!Export(2, TEXT("cycle")) || !Edit(TEXT("cycle"), {0}, {})) return false;
	Result = Import(2, TEXT("cycle"));
	if (!TestTrue(TEXT("cycle is rejected before write"), !Result.IsOk() && Result.GetErrorJson().Contains(TEXT("sound_class_cycle")))
		|| !TestTrue(TEXT("cycle preserves native relationships"), Published()) || !UnrelatedPreserved()) return false;
	for (int32 Index = 0; Index < 5; ++Index) { TArray<uint8> Bytes; TestTrue(TEXT("cycle preserves all affected disk"), FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) && Bytes == PublishedDisk[Index]); }
	// A real late failure after multi-package persistence proves the shared
	// checkpoint restores dirty memory separately from the previous disk state.
	Load(0)->Properties.Volume = 0.33f; Load(0)->MarkPackageDirty();
	Load(2)->Properties.Pitch = 0.77f; Load(2)->MarkPackageDirty();
	UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId = TEXT("sound_class"); Late.AssetPath = Packages[0]; Late.bSave = true; Late.bCompactValues = true;
	Late.ProviderExtension = MakeShared<FJsonObject>(); Late.ProviderExtension->SetArrayField(TEXT("child_classes"), {MakeShared<FJsonValueString>(Paths[3])});
	bool SawPersisted = false;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		SawPersisted = Load(0)->ChildClasses.Num() == 1 && Load(0)->ChildClasses[0] == Load(3) && Load(3)->ParentClass == Load(0)
			&& !Load(2)->ParentClass && !Load(4)->ParentClass;
		for (int32 Index : {0, 2, 3, 4}) { TArray<uint8> Bytes; SawPersisted &= !Load(Index)->GetPackage()->IsDirty()
			&& FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) && Bytes != PublishedDisk[Index]; }
		Failure.Diagnostic.Code = TEXT("t208_sound_class_late_rejected"); return false;
	};
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late failure returns failure"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
	if (!TestTrue(TEXT("late hook observes persisted new graph before rejection"), SawPersisted)
		|| !TestTrue(TEXT("common transaction confirms late rollback"), LateResult.Diagnostic.Code == TEXT("t208_sound_class_late_rejected") && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded)
		|| !TestTrue(TEXT("dirty multi-package memory and relationships restored"), Published() && Load(0)->Properties.Volume == 0.33f
			&& Load(2)->Properties.Pitch == 0.77f && Load(0)->GetPackage()->IsDirty() && Load(2)->GetPackage()->IsDirty()
			&& !Load(3)->GetPackage()->IsDirty() && !Load(4)->GetPackage()->IsDirty()) || !UnrelatedPreserved()) return false;
	for (int32 Index = 0; Index < 5; ++Index) { TArray<uint8> Bytes; TestTrue(TEXT("late rollback restores original disk bytes"), FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) && Bytes == PublishedDisk[Index]); }
	if (!Export(0, TEXT("clear")) || !Edit(TEXT("clear"), {}, {})) return false;
	Result = Import(0, TEXT("clear"));
	if (!TestTrue(TEXT("public explicit empty child set"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	TestTrue(TEXT("clear persists both directions"), Load(0)->ChildClasses.IsEmpty() && !Load(2)->ParentClass && !Load(4)->ParentClass);
	UnrelatedPreserved();
	return !HasAnyErrors();
}
