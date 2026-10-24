#include "Misc/AutomationTest.h"

#include "Curves/CurveFloat.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/UObjectIterator.h"
#include "UObject/SavePackage.h"

namespace {
using namespace UE::UAI::Tests::PublicCommand;

struct FSaveFixture {
  FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
  FString Mount = TEXT("/UaiSaveFixture_") + Id + TEXT("/");
  FString Disk = FPaths::ConvertRelativePathToFull(FPaths::Combine(
      FPaths::ProjectSavedDir(), TEXT("UeAgentInterface/Tests/SaveResources"), Id));
  FString EngineBefore = GEngineIni, GameBefore = GGameIni;
  FString EngineFile = FPaths::Combine(Disk, TEXT("SaveFixtureEngine.ini"));
  FString GameFile = FPaths::Combine(Disk, TEXT("SaveFixtureGame.ini"));
  FString BlockedFile = FPaths::Combine(Disk, TEXT("SaveFixtureBlocked.ini"));
  UWorld* World = nullptr;
  FSaveFixture() {
    IFileManager::Get().MakeDirectory(*Disk, true);
    FPackageName::RegisterMountPoint(Mount, Disk + TEXT("/"));
    GConfig->AddNewBranch(EngineFile);
    GConfig->AddNewBranch(GameFile);
    GConfig->AddNewBranch(BlockedFile);
    GEngineIni = EngineFile;
    GGameIni = GameFile;
  }
  ~FSaveFixture() {
    GEngineIni = EngineBefore;
    GGameIni = GameBefore;
    GConfig->Remove(EngineFile);
    GConfig->Remove(GameFile);
    GConfig->Remove(BlockedFile);
    if (World) { World->RemoveFromRoot(); World->DestroyWorld(false); }
    for (TObjectIterator<UPackage> It; It; ++It)
      if (It->GetName().StartsWith(Mount)) It->SetDirtyFlag(false);
    FPackageName::UnRegisterMountPoint(Mount, Disk + TEXT("/"));
    IFileManager::Get().DeleteDirectory(*Disk, false, true);
  }
  UCurveFloat* Curve(const TCHAR* Name) {
    UPackage* Package = CreatePackage(*(Mount + Name));
    UCurveFloat* Asset = NewObject<UCurveFloat>(Package,
        *FPackageName::GetLongPackageAssetName(Package->GetName()), RF_Public | RF_Standalone);
    Asset->FloatCurve.AddKey(0.0f, 17.0f);
    Asset->MarkPackageDirty();
    return Asset;
  }
};

TSharedPtr<FJsonObject> Parse(const FString& Text) {
  TSharedPtr<FJsonObject> Object;
  FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
  return Object;
}

FString ItemField(const TSharedPtr<FJsonObject>& Payload, const FString& Ref, const TCHAR* Field) {
  const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
  if (!Payload || !Payload->TryGetArrayField(TEXT("resources"), Items)) return FString();
  for (const auto& Item : *Items) {
    const auto Object = Item->AsObject();
    FString Value;
    if (Object && Object->GetStringField(TEXT("resource_ref")) == Ref
        && Object->TryGetStringField(Field, Value)) return Value;
  }
  return FString();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentEditorResourceSaveTest,
    "UeAgentInterface.Composition.LongRunningSession.E3.EditorResourceSavePersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorResourceSaveTest::RunTest(const FString&) {
  if (!TestTrue(TEXT("ready native cache"), GConfig && GConfig->IsReadyForUse())) return false;
  UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
  if (!TestNotNull(TEXT("editor world"), EditorWorld)) return false;
  FScopedPublicCommandComposition Composition(TEXT("save-resource-persistence"));
  FString Error;
  if (!TestTrue(TEXT("production command composition"), Composition.TryStart(*EditorWorld->GetWorldSettings(), Error))) {
    AddError(Error);
    return false;
  }
  // Scratch packages are deliberately removed at scope exit. Do not enqueue
  // unrelated domain validators for files that will disappear before the next tick.
  FScopedDisableValidateOnSave DisableScratchDomainValidation;
  FSaveFixture Fixture;
  auto Save = [&](const TArray<FString>& Refs, bool bExpectedOk = true) {
    const auto Params = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const auto& Ref : Refs) Values.Add(MakeShared<FJsonValueString>(Ref));
    Params->SetArrayField(TEXT("resource_refs"), Values);
    FString Json;
    FJsonSerializer::Serialize(Params, TJsonWriterFactory<>::Create(&Json));
    TSharedPtr<FJsonObject> Result;
    FString InvokeError;
    if (!TryInvokeCommand(Composition.GetRouteHost(), Composition.GetConfig(),
        TEXT("editor.save_resources"), Json, Result, InvokeError) || !Result.IsValid()) {
      AddError(TEXT("public save invocation failed: ") + InvokeError);
      return TSharedPtr<FJsonObject>();
    }
    if (Result->GetBoolField(TEXT("ok")) != bExpectedOk) AddError(TEXT("Unexpected public save: ") + PublicErrorText(Result));
    return Result->GetObjectField(TEXT("payload"));
  };

  auto* EmptyConfig = GConfig->FindConfigFile(Fixture.EngineFile);
  EmptyConfig->Dirty = true;
  auto EmptyPayload = Save({TEXT("config:engine")});
  TestEqual(TEXT("absent native empty output is unchanged"), ItemField(EmptyPayload, TEXT("config:engine"), TEXT("status")), FString(TEXT("unchanged")));
  TestFalse(TEXT("confirmed empty config clean"), EmptyConfig->Dirty);
  FFileHelper::SaveStringToFile(TEXT(";obsolete saved config"), *Fixture.EngineFile);
  EmptyConfig->Dirty = true;
  Save({TEXT("config:engine")});
  TestFalse(TEXT("native empty output removes obsolete file"), IFileManager::Get().FileExists(*Fixture.EngineFile));

  UCurveFloat* Asset = Fixture.Curve(TEXT("Curve"));
  UCurveFloat* Unselected = Fixture.Curve(TEXT("Unselected"));
  const FString Ref = Asset->GetOutermost()->GetName();
  auto Payload = Save({Asset->GetPathName(), Ref});
  TestEqual(TEXT("new mounted asset saved"), ItemField(Payload, Ref, TEXT("status")), FString(TEXT("saved")));
  TestFalse(TEXT("saved package clean"), Asset->GetOutermost()->IsDirty());
  TestTrue(TEXT("unselected asset remains dirty"), Unselected->GetOutermost()->IsDirty());
  TestFalse(TEXT("unselected asset not written"), FPackageName::DoesPackageExist(Unselected->GetOutermost()->GetName()));
  if (Payload) TestEqual(TEXT("object/package refs deduplicated"), Payload->GetArrayField(TEXT("resources")).Num(), 1);

  Asset->FloatCurve.AddKey(1.0f, 29.0f);
  Asset->MarkPackageDirty();
  Save({Ref});
  UPackage* Package = Asset->GetOutermost();
  Asset = nullptr;
  FText UnloadError;
  TestTrue(TEXT("unload saved asset"), UPackageTools::UnloadPackages({Package}, UnloadError));
  Package = nullptr;
  TestNull(TEXT("asset package unloaded"), FindPackage(nullptr, *Ref));
  Payload = Save({Ref});
  TestEqual(TEXT("cold package resolved without rewriting"), ItemField(Payload, Ref, TEXT("status")), FString(TEXT("unchanged")));
  Asset = LoadObject<UCurveFloat>(nullptr, *(Ref + TEXT(".Curve")));
  if (TestNotNull(TEXT("independent persisted asset reload"), Asset))
    TestEqual(TEXT("edited curve persisted"), Asset->FloatCurve.Eval(1.0f), 29.0f);

  UPackage* MapPackage = CreatePackage(*(Fixture.Mount + TEXT("Map")));
  Fixture.World = UWorld::CreateWorld(EWorldType::Editor, true, TEXT("Map"), MapPackage);
  if (!TestNotNull(TEXT("owned map fixture"), Fixture.World)) return false;
  Fixture.World->ClearFlags(RF_Transient);
  Fixture.World->SetFlags(RF_Public | RF_Standalone);
  MapPackage->SetPackageFlags(PKG_ContainsMap);
  UMapBuildDataRegistry* BuildData = Fixture.World->PersistentLevel->GetOrCreateMapBuildData();
  BuildData->AllocateMeshBuildData(FGuid::NewGuid(), true);
  UPackage* BuildPackage = BuildData->GetOutermost();
  MapPackage->MarkPackageDirty();
  BuildPackage->MarkPackageDirty();
  Save({MapPackage->GetName()});
  TestTrue(TEXT("owned map build data written"), FPackageName::DoesPackageExist(BuildPackage->GetName()));
  TestFalse(TEXT("map saved clean"), MapPackage->IsDirty());
  BuildPackage->MarkPackageDirty();
  Save({MapPackage->GetName()});
  TestFalse(TEXT("clean parent saves dirty build data"), BuildPackage->IsDirty());

  GConfig->SetString(TEXT("SaveFixture"), TEXT("Value"), TEXT("persisted"), Fixture.EngineFile);
  GConfig->SetString(TEXT("SaveFixture"), TEXT("Value"), TEXT("unselected"), Fixture.GameFile);
  Save({TEXT("config:engine")});
  FConfigFile Persisted;
  Persisted.Read(Fixture.EngineFile);
  FString Value;
  TestTrue(TEXT("config independently parsed"), Persisted.GetString(TEXT("SaveFixture"), TEXT("Value"), Value));
  TestEqual(TEXT("native config content persisted"), Value, FString(TEXT("persisted")));
  TestFalse(TEXT("persisted config clean"), GConfig->FindConfigFile(Fixture.EngineFile)->Dirty);
  TestTrue(TEXT("unselected config remains dirty"), GConfig->FindConfigFile(Fixture.GameFile)->Dirty);
  TestFalse(TEXT("unselected config not written"), IFileManager::Get().FileExists(*Fixture.GameFile));

  auto* Config = GConfig->FindConfigFile(Fixture.EngineFile);
  Config->NoSave = true;
  Config->Dirty = true;
  Payload = Save({TEXT("config:engine")}, false);
  TestEqual(TEXT("native NoSave reports failure"), ItemField(Payload, TEXT("config:engine"), TEXT("reason")), FString(TEXT("config_save_disabled")));
  TestTrue(TEXT("disabled save retains dirty"), Config->Dirty);
  Config->NoSave = false;
  Save({TEXT("config:engine")});

  GEngineIni = Fixture.BlockedFile;
  GConfig->SetString(TEXT("SaveFixture"), TEXT("Value"), TEXT("blocked"), Fixture.BlockedFile);
  IFileManager::Get().MakeDirectory(*Fixture.BlockedFile, true);
  AddExpectedError(TEXT("SaveFixtureBlocked"), EAutomationExpectedErrorFlags::Contains, 0);
  Payload = Save({TEXT("config:engine"), TEXT("config:game")}, false);
  TestEqual(TEXT("IO failure exposed"), ItemField(Payload, TEXT("config:engine"), TEXT("status")), FString(TEXT("failed")));
  TestTrue(TEXT("failed config retains dirty"), GConfig->FindConfigFile(Fixture.BlockedFile)->Dirty);
  TestFalse(TEXT("independent requested config still saved"), GConfig->FindConfigFile(Fixture.GameFile)->Dirty);
  GEngineIni = Fixture.EngineFile;

  const FString BlockedParent = FPaths::Combine(Fixture.Disk, TEXT("BlockedParent"));
  FFileHelper::SaveStringToFile(TEXT("A file prevents creation of the package directory"), *BlockedParent);
  UCurveFloat* Blocked = Fixture.Curve(TEXT("BlockedParent/SaveFixtureBlockedAsset"));
  const FString BlockedRef = Blocked->GetOutermost()->GetName();
  Payload = Save({BlockedRef, Ref, Fixture.Mount + TEXT("Missing")}, false);
  TestEqual(TEXT("package IO failure exposed"), ItemField(Payload, BlockedRef, TEXT("status")), FString(TEXT("failed")));
  TestTrue(TEXT("failed package retains dirty"), Blocked->GetOutermost()->IsDirty());
  TestEqual(TEXT("missing package reason"), ItemField(Payload, Fixture.Mount + TEXT("Missing"), TEXT("reason")), FString(TEXT("package_not_found")));
  TestEqual(TEXT("independent package survives failed batch"), ItemField(Payload, Ref, TEXT("status")), FString(TEXT("unchanged")));
  UCurveFloat* Raw = Fixture.Curve(TEXT("RawFinal"));
  UPackage* RawPackage = Raw->GetOutermost();
  FSavePackageArgs Args;
  Args.TopLevelFlags = RF_Public | RF_Standalone;
  const FString RawFile = FPackageName::LongPackageNameToFilename(RawPackage->GetName(), FPackageName::GetAssetPackageExtension());
  TestTrue(TEXT("native final writer confirmed"), UE::UAI::AssetDurable::TrySaveLoadedPackage(*RawPackage, Raw, RawFile, Args, Error));
  TestTrue(TEXT("native final output exists"), IFileManager::Get().FileExists(*RawFile));
  TestFalse(TEXT("native final package clean"), RawPackage->IsDirty());
  Raw->MarkPackageDirty();
  Args.SaveFlags = SAVE_KeepDirty | SAVE_FromAutosave;
  const FString Scratch = FPaths::Combine(Fixture.Disk, TEXT("RawScratch.uasset"));
  TestTrue(TEXT("durable scratch writer confirmed"), UE::UAI::AssetDurable::TrySaveLoadedPackage(*RawPackage, Raw, Scratch, Args, Error));
  TestTrue(TEXT("scratch output exists"), IFileManager::Get().FileExists(*Scratch));
  TestTrue(TEXT("durable scratch intentionally keeps dirty"), RawPackage->IsDirty());
  Args.SaveFlags = SAVE_None;
  TestFalse(TEXT("native writer IO failure propagated"), UE::UAI::AssetDurable::TrySaveLoadedPackage(*RawPackage, Raw,
      FPaths::Combine(BlockedParent, TEXT("SaveFixtureBlockedRaw.uasset")), Args, Error));
  TestTrue(TEXT("native failed writer retains dirty"), RawPackage->IsDirty());
  IConsoleVariable* Tracking = IConsoleManager::Get().FindConsoleVariable(TEXT("ini.UseNewSaveTracking"));
  if (TestNotNull(TEXT("native config tracking mode"), Tracking)) {
    const int32 OriginalMode = Tracking->GetInt();
    ON_SCOPE_EXIT { Tracking->Set(OriginalMode, ECVF_SetByCode); GEngineIni = Fixture.EngineFile; };
    for (int32 Mode : {1, 2}) {
      Tracking->Set(Mode, ECVF_SetByCode);
      const FString ModeFile = FPaths::Combine(Fixture.Disk, FString::Printf(TEXT("Mode%d.ini"), Mode));
      FConfigBranch& Branch = GConfig->AddNewBranch(ModeFile);
      ON_SCOPE_EXIT { GConfig->Remove(ModeFile); };
      GEngineIni = ModeFile;
      GConfig->SetString(TEXT("SaveFixture"), TEXT("Value"), TEXT("tracking"), ModeFile);
      // Mode 1 serializes SavedLayer, not the unmerged in-memory file.
      if (Mode == 1) Branch.SavedLayer.FindOrAdd(TEXT("SaveFixture")).Emplace(
          FName(TEXT("Value")), FConfigValue(FString(TEXT("tracking")), FConfigValue::EValueType::Set));
      Save({TEXT("config:engine")});
      TestTrue(TEXT("tracking mode output written"), IFileManager::Get().FileExists(*ModeFile));
      if (Mode == 2) {
        GConfig->SetArray(TEXT("SaveFixture"), TEXT("Array"), {TEXT("a"), TEXT("b")}, ModeFile);
        Branch.FinalCombinedLayers = Branch.InMemoryFile;
        GConfig->SetArray(TEXT("SaveFixture"), TEXT("Array"), {TEXT("b"), TEXT("a")}, ModeFile);
        Save({TEXT("config:engine")});
        TestFalse(TEXT("native multiset empty delta removes obsolete output"), IFileManager::Get().FileExists(*ModeFile));
        Branch.InMemoryFile.Dirty = true;
        Save({TEXT("config:engine")});
        TestFalse(TEXT("absent multiset empty delta clears dirty"), Branch.InMemoryFile.Dirty);
      }
    }
  }
  if (Asset) {
    UPackage* EmptyPackage = Asset->GetOutermost();
    Asset->ClearFlags(RF_Public | RF_Standalone);
    Asset->MarkAsGarbage();
    Asset = nullptr;
    EmptyPackage->MarkPackageDirty();
    Save({Ref});
    TestFalse(TEXT("native empty package cleanup persisted"), FPackageName::DoesPackageExist(Ref));
  }
  return !HasAnyErrors();
}
