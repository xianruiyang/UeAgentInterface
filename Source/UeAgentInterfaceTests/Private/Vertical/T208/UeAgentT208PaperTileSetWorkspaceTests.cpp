// Copyright Epic Games, Inc. All Rights Reserved.
#include "Misc/AutomationTest.h"
#include "PaperTileSet.h"
#include "PaperTileMapComponent.h"
#include "PaperTileMap.h"
#include "PaperTileLayer.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Editor.h"
#include "RenderingThread.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace {
TSharedPtr<FJsonObject> TileSetRow(const TSharedPtr<FJsonObject>& Object, const FString& Name) {
  for (const auto& Field : Object->Values) if (Field.Value->Type == EJson::Object) {
    const auto Child = Field.Value->AsObject();
    if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
    if (auto Found = TileSetRow(Child, Name)) return Found;
  }
  return nullptr;
}
TSharedPtr<FJsonValue> PointValue(int32 X, int32 Y) {
  auto Fields = MakeShared<FJsonObject>(); Fields->SetNumberField(TEXT("X"), X); Fields->SetNumberField(TEXT("Y"), Y);
  auto Value = MakeShared<FJsonObject>(); Value->SetObjectField(TEXT("fields"), Fields);
  return MakeShared<FJsonValueObject>(Value);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiT208PaperTileSetWorkspace,
  "UAI.Root.T208.PaperTileSet.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiT208PaperTileSetWorkspace::RunTest(const FString&) {
  using namespace UE::UAI;
  UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
  if (!TestNotNull(TEXT("editor world"), World)) return false;
  const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
  const FString Base = TEXT("/Game/UAI/Transient/T208PaperTileSet/") + Token;
  const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-paper-tileset/native") / Token);
  const TArray<FString> Names{TEXT("Tiles"), TEXT("Source"), TEXT("Replacement"), TEXT("OtherTiles"), TEXT("Mixed")};
  TArray<FString> Paths, Packages, Filenames;
  for (const auto& Name : Names) { Packages.Add(Base / Name); Paths.Add(Packages.Last() + TEXT(".") + Name); Filenames.Add(FPackageName::LongPackageNameToFilename(Packages.Last(), FPackageName::GetAssetPackageExtension())); }
  FString Error;
  TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
  TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
  if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
  auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
    return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args, TEXT("t208-paper-tileset"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
  auto Tiles = [&](int32 Index = 0) { return LoadObject<UPaperTileSet>(nullptr, *Paths[Index]); };
  auto Texture = [&](int32 Index) { return LoadObject<UTexture2D>(nullptr, *Paths[Index]); };
  auto Save = [&](int32 Index) { auto* Object = LoadObject<UObject>(nullptr, *Paths[Index]); FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError; return Object && UPackage::SavePackage(Object->GetPackage(), Object, *Filenames[Index], Args); };
  TStrongObjectPtr<UPaperTileMapComponent> Component(NewObject<UPaperTileMapComponent>());
  TStrongObjectPtr<UPaperTileMapComponent> OtherComponent(NewObject<UPaperTileMapComponent>());
  ON_SCOPE_EXIT {
    for (auto* Value : {Component.Get(), OtherComponent.Get()}) { Value->SetTileMap(nullptr); Value->DestroyComponent(); }
    Component.Reset(); OtherComponent.Reset(); FlushRenderingCommands();
    TArray<TSharedPtr<FJsonValue>> Targets;
    for (int32 Index = 0; Index < Paths.Num(); ++Index) {
      if (auto* Object = FindObject<UObject>(nullptr, *Paths[Index]); Object && Object->GetPackage()->IsDirty()) TestTrue(TEXT("save disposable fixture"), Save(Index));
      if (FPackageName::DoesPackageExist(Packages[Index])) Targets.Add(MakeShared<FJsonValueString>(Paths[Index]));
    }
    auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), Targets); Args->SetBoolField(TEXT("recursive"), false);
    for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
  };
  for (int32 Index : {1, 2, 4}) {
    auto* Value = NewObject<UTexture2D>(CreatePackage(*Packages[Index]), *Names[Index], RF_Public | RF_Standalone | RF_Transactional);
    const int32 Width = Index == 2 ? 64 : 32, Height = Index == 1 ? 16 : Index == 2 ? 32 : 64;
    TArray<uint8> Pixels; Pixels.Init(255, Width * Height * 4);
    Value->Source.Init(Width, Height, 1, 1, TSF_BGRA8, Pixels.GetData()); Value->UpdateResource();
  }
  for (int32 Index : {0, 3}) {
    auto* Value = NewObject<UPaperTileSet>(CreatePackage(*Packages[Index]), *Names[Index], RF_Public | RF_Standalone | RF_Transactional);
    Value->SetTileSheetTexture(Texture(1)); Value->SetTileSize(FIntPoint(16, 16)); Value->PostEditChange();
  }
  for (int32 Index = 0; Index < Paths.Num(); ++Index) if (!Save(Index)) return false;
  for (int32 Index = 0; Index < 2; ++Index) {
    auto* Value = Index ? OtherComponent.Get() : Component.Get(); Value->CreateNewTileMap(1, 1, 16, 16);
    FPaperTileInfo Info; Info.TileSet = Tiles(Index ? 3 : 0); Info.PackedTileIndex = 0; Value->SetTile(0, 0, 0, Info); Value->RegisterComponentWithWorld(World);
  }
  FlushRenderingCommands();
  TArray<TWeakObjectPtr<UObject>> Others; TArray<TArray<uint8>> OtherDisk;
  for (int32 Index = 1; Index < Paths.Num(); ++Index) { Others.Add(LoadObject<UObject>(nullptr, *Paths[Index])); OtherDisk.AddDefaulted(); FFileHelper::LoadFileToArray(OtherDisk.Last(), *Filenames[Index]); }
  auto NonTargets = [&]() {
    for (int32 Index = 1; Index < Paths.Num(); ++Index) {
      UObject* Value = LoadObject<UObject>(nullptr, *Paths[Index]); TArray<uint8> Bytes;
      if (Others[Index - 1].Get() != Value || Value->GetPackage()->IsDirty() || !FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) || Bytes != OtherDisk[Index - 1]) return false;
    }
    return OtherComponent->IsRegistered() && OtherComponent->GetTile(0, 0, 0).TileSet == Tiles(3) && Tiles(3)->GetTileCount() == 2;
  };
  // No profile means no newly authorized root or recursive member access.
  UeAgentAuthoringState::FEditablePropertyReadOptions DefaultRead;
  TArray<UeAgentAuthoringState::FReadRequest> DefaultRequests;
  TestTrue(TEXT("default capture"), UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Tiles(), DefaultRead, DefaultRequests, Error));
  TestFalse(TEXT("default root qualification remains strict"), DefaultRequests.ContainsByPredicate([](const auto& Request) { return Request.PropertyPath == TEXT("Terrains"); }));
  auto Denied = MakeShared<FJsonObject>(); Denied->SetStringField(TEXT("property_path"), TEXT("Terrains")); Denied->SetArrayField(TEXT("value"), {});
  UeAgentAuthoringObjectApply::FOptions DefaultApply; DefaultApply.bCompactValues = true;
  UeAgentAuthoringObjectApply::FPreparedApply Prepared; UeAgentAuthoringObjectApply::FFailure Failure;
  TestFalse(TEXT("default apply qualification remains strict"), UeAgentAuthoringObjectApply::PrepareObject(Tiles(), {MakeShared<FJsonValueObject>(Denied)}, DefaultApply, Prepared, Failure));
  FPaperTileSetTerrain DefaultTerrain;
  auto DeniedCenter = MakeShared<FJsonObject>(); DeniedCenter->SetStringField(TEXT("property_path"), TEXT("CenterTileIndex")); DeniedCenter->SetNumberField(TEXT("value"), 1);
  TestFalse(TEXT("default recursive field qualification remains strict"), UeAgentAuthoringObjectApply::PrepareStruct(
      FPaperTileSetTerrain::StaticStruct(), &DefaultTerrain, Tiles(), {MakeShared<FJsonValueObject>(DeniedCenter)}, DefaultApply, Prepared, Failure));
  TestEqual(TEXT("recursive denial is property qualification"), Failure.Diagnostic.Code, FString(TEXT("read_only_or_non_editable")));
  auto Export = [&](const FString& Stage) { auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[0]); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring")); const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
  auto Import = [&](const FString& Stage) { auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[0]); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args); };
  TSharedPtr<FJsonObject> TerrainTemplate, MetadataTemplate;
  auto Edit = [&](const FString& Stage, const TMap<FString, TSharedPtr<FJsonValue>>& Values, bool CaptureTemplates = false) {
    TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
    TMap<FString, TSharedPtr<FJsonObject>> Definitions, Rows;
    TArray<TPair<FString, TSharedPtr<FJsonObject>>> Documents;
    for (const auto& File : Files) {
      if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
      FString Text; TSharedPtr<FJsonObject> Doc;
      if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
      Documents.Add({File, Doc});
      const TSharedPtr<FJsonObject>* Defs;
      if (Doc->TryGetObjectField(TEXT("definitions"), Defs)) for (const auto& Entry : (*Defs)->Values) Definitions.Add(Entry.Key, Entry.Value->AsObject());
      for (const FString Name : {TEXT("Terrains"), TEXT("PerTileData")}) if (auto Row = TileSetRow(Doc, Name)) Rows.Add(Name, Row);
    }
    if (CaptureTemplates) {
      for (const FString Name : {TEXT("Terrains"), TEXT("PerTileData")}) {
        if (!TestTrue(TEXT("public collection row and definition"), Rows.Contains(Name) && Definitions.Contains(Rows[Name]->GetStringField(TEXT("definition"))))) return false;
        auto Template = Definitions[Rows[Name]->GetStringField(TEXT("definition"))]->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("element_template"));
        if (Name == TEXT("Terrains")) TerrainTemplate = Template; else MetadataTemplate = Template;
      }
      if (!TestTrue(TEXT("terrain template exposes both declared fields"), TerrainTemplate->GetObjectField(TEXT("fields"))->HasField(TEXT("TerrainName")) && TerrainTemplate->GetObjectField(TEXT("fields"))->HasField(TEXT("CenterTileIndex")))) return false;
      TestFalse(TEXT("membership not authorized by terrain declaration"), MetadataTemplate->GetObjectField(TEXT("fields"))->HasField(TEXT("TerrainMembership")));
    }
    int32 Found = 0;
    for (const auto& Pair : Documents) {
      bool Changed = false;
      for (const auto& Value : Values) if (auto Row = TileSetRow(Pair.Value, Value.Key)) { Row->SetField(TEXT("value"), Value.Value); Changed = true; ++Found; }
      FString Text;
      if (Changed && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Pair.Value.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *Pair.Key, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
    }
    return TestEqual(TEXT("ordinary edited rows"), Found, Values.Num());
  };
  if (!Export(TEXT("first")) || !Edit(TEXT("first"), {}, true)) return false;
  auto Terrain = [&](const FString& Name, int32 Center) {
    auto Item = MakeShared<FJsonObject>(); Item->Values = TerrainTemplate->Values;
    auto Fields = MakeShared<FJsonObject>(); Fields->Values = Item->GetObjectField(TEXT("fields"))->Values;
    Fields->SetStringField(TEXT("TerrainName"), Name); Fields->SetNumberField(TEXT("CenterTileIndex"), Center); Item->SetObjectField(TEXT("fields"), Fields); return MakeShared<FJsonValueObject>(Item);
  };
  TArray<TSharedPtr<FJsonValue>> Metadata;
  for (int32 Index = 0; Index < 9; ++Index) {
    auto Item = MakeShared<FJsonObject>(); Item->Values = MetadataTemplate->Values;
    auto Fields = MakeShared<FJsonObject>(); Fields->Values = Item->GetObjectField(TEXT("fields"))->Values;
    Fields->SetStringField(TEXT("UserDataName"), Index == 8 ? TEXT("last") : Index == 0 ? TEXT("first") : TEXT("None"));
    Item->SetObjectField(TEXT("fields"), Fields); Metadata.Add(MakeShared<FJsonValueObject>(Item));
  }
  auto Margin = MakeShared<FJsonObject>(); auto MarginFields = MakeShared<FJsonObject>();
  for (const FString Name : {TEXT("Left"), TEXT("Right"), TEXT("Top"), TEXT("Bottom")}) MarginFields->SetNumberField(Name, Name == TEXT("Left") || Name == TEXT("Right") ? 2 : 1);
  Margin->SetObjectField(TEXT("fields"), MarginFields);
  if (!Edit(TEXT("first"), {{TEXT("TileSheet"), MakeShared<FJsonValueString>(Paths[2])}, {TEXT("TileSize"), PointValue(16, 8)},
      {TEXT("BorderMargin"), MakeShared<FJsonValueObject>(Margin)}, {TEXT("PerTileSpacing"), PointValue(2, 2)},
      {TEXT("PerTileData"), MakeShared<FJsonValueArray>(Metadata)}, {TEXT("Terrains"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Terrain(TEXT("Grass"), 8), Terrain(TEXT("Stone"), 0)})}})) return false;
  auto Native = [&](int32 Width, const FString& FirstTerrain, bool Dirty) {
    FlushRenderingCommands(); auto* Value = Tiles(); FVector2D UV;
    const int32 CountX = Width == 16 ? 3 : 6;
    const int32 Last = 2 * CountX + 2;
    int32 Triangles = 0, Batches = 0; Component->GetRenderingStats(Triangles, Batches);
    return Value && Value->GetTileSheetTexture() == Texture(2) && Value->GetTileCountX() == CountX && Value->GetTileCountY() == 3
      && Value->GetTileUV(Last, UV) && UV == FVector2D(2 + 2 * (Width + 2), 21)
      && Value->GetTileXYFromTextureUV(UV, false) == FIntPoint(2, 2)
      && Value->GetTileUserData(0) == TEXT("first") && Value->GetTileUserData(Last) == TEXT("last")
      && !Value->GetTileMetadata(Value->GetTileCount()) && Value->GetTileMetadata(Last)->TerrainMembership[0] == 255
      && (FirstTerrain.IsEmpty() ? Value->GetNumTerrains() == 0 : Value->GetNumTerrains() == 2 && Value->GetTerrain(0).TerrainName == FirstTerrain)
      && Value->GetPackage()->IsDirty() == Dirty && Component->IsRegistered() && Component->GetTile(0, 0, 0).TileSet == Value
      && Triangles == 2 && Batches == 1 && NonTargets();
  };
  TWeakObjectPtr<UPaperTileSet> Before(Tiles()); auto Result = Import(TEXT("first"));
  if (!TestTrue(TEXT("public texture layout terrain and metadata same import"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
  TestFalse(TEXT("independent reload rebinds consumer"), Before.IsValid());
  TestFalse(TEXT("explicit profile never changes native edit flags"), FindFProperty<FProperty>(FPaperTileSetTerrain::StaticStruct(), TEXT("CenterTileIndex"))->HasAnyPropertyFlags(CPF_Edit));
  if (!TestTrue(TEXT("native grid pixel mapping metadata and registered render consumer"), Native(16, TEXT("Grass"), false))) return false;
  if (!Export(TEXT("resize")) || !Edit(TEXT("resize"), {{TEXT("TileSize"), PointValue(8, 8)}, {TEXT("Terrains"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Terrain(TEXT("Stone"), 0), Terrain(TEXT("Grass"), 14)})}})) return false;
  Result = Import(TEXT("resize"));
  if (!TestTrue(TEXT("resize preserves metadata by grid coordinate and reorders terrain"), Result.IsOk() && Native(8, TEXT("Stone"), false))) { if (!Result.IsOk()) AddError(Result.GetErrorJson()); return false; }
  for (const FString Stage : {TEXT("type"), TEXT("reference"), TEXT("center")}) {
    if (!Export(Stage)) return false;
    TMap<FString, TSharedPtr<FJsonValue>> Changes;
    if (Stage == TEXT("type")) Changes.Add(TEXT("TileSize"), MakeShared<FJsonValueString>(TEXT("invalid")));
    else if (Stage == TEXT("reference")) Changes.Add(TEXT("TileSheet"), MakeShared<FJsonValueString>(Paths[3]));
    else Changes.Add(TEXT("Terrains"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Terrain(TEXT("Bad"), 200)}));
    if (!Edit(Stage, Changes)) return false;
    auto* Proxy = Component->GetSceneProxy(); auto* OtherProxy = OtherComponent->GetSceneProxy();
    Result = Import(Stage);
    if (!TestTrue(TEXT("invalid input rejects without real preview notification or write"), !Result.IsOk() && Native(8, TEXT("Stone"), false)
        && Component->GetSceneProxy() == Proxy && OtherComponent->GetSceneProxy() == OtherProxy)) return false;
  }
  if (!Export(TEXT("clear")) || !Edit(TEXT("clear"), {{TEXT("Terrains"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{})}})) return false;
  Result = Import(TEXT("clear")); if (!TestTrue(TEXT("terrain clear keeps grid metadata"), Result.IsOk() && Native(8, TEXT(""), false))) return false;
  TArray<uint8> PublishedDisk; FFileHelper::LoadFileToArray(PublishedDisk, *Filenames[0]);
  Tiles()->SetDrawingOffset(FIntPoint(3, 4)); Tiles()->PostEditChange(); Tiles()->MarkPackageDirty();
  UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId = TEXT("paper_tileset"); Late.AssetPath = Packages[0]; Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
  auto Patch = MakeShared<FJsonObject>(); Patch->SetStringField(TEXT("property_path"), TEXT("DrawingOffset")); Patch->SetField(TEXT("value"), PointValue(7, 9)); Late.Properties.Add(MakeShared<FJsonValueObject>(Patch));
  bool SawPublished = false;
  Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) { SawPublished = Native(8, TEXT(""), false) && Tiles()->GetDrawingOffset() == FIntPoint(7, 9); Failure.Diagnostic.Code = TEXT("t208_tileset_late_rejected"); return false; };
  UeAgentReflectionAuthoring::FAssetApplyResult LateResult; TestFalse(TEXT("late rejection"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
  TArray<uint8> RestoredDisk; FFileHelper::LoadFileToArray(RestoredDisk, *Filenames[0]);
  TestTrue(TEXT("actual saved native result observed before rejection"), SawPublished);
  TestTrue(TEXT("dirty native state disk and consumers restored"), LateResult.bRollbackSucceeded && Native(8, TEXT(""), true)
      && Tiles()->GetDrawingOffset() == FIntPoint(3, 4) && PublishedDisk == RestoredDisk);
  if (!LateResult.bRollbackSucceeded) AddError(LateResult.ErrorCode);
  // A real native callback invalidates the just-settled first stage. The
  // dependent metadata stage must not run, and checkpoint restores dirty memory.
  auto StageFailure = Late; StageFailure.Properties.Reset(); StageFailure.FinalReadback = nullptr;
  auto StageSize = MakeShared<FJsonObject>(); StageSize->SetStringField(TEXT("property_path"), TEXT("TileSize")); StageSize->SetField(TEXT("value"), PointValue(4, 8));
  StageFailure.Properties.Add(MakeShared<FJsonValueObject>(StageSize));
  TArray<TSharedPtr<FJsonValue>> Thirty;
  for (int32 Index = 0; Index < 30; ++Index) {
    auto Item = MakeShared<FJsonObject>(); Item->Values = MetadataTemplate->Values;
    auto Fields = MakeShared<FJsonObject>(); Fields->Values = Item->GetObjectField(TEXT("fields"))->Values;
    Fields->SetStringField(TEXT("UserDataName"), TEXT("dependent-stage")); Item->SetObjectField(TEXT("fields"), Fields); Thirty.Add(MakeShared<FJsonValueObject>(Item));
  }
  auto StageData = MakeShared<FJsonObject>(); StageData->SetStringField(TEXT("property_path"), TEXT("PerTileData")); StageData->SetArrayField(TEXT("value"), Thirty);
  StageFailure.Properties.Add(MakeShared<FJsonValueObject>(StageData));
  bool Injected = false, SawLayoutOnly = false;
  const auto NativeHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent&) {
    if (Injected || !Object || Object->GetPathName() != Paths[0]) return;
    auto* Value = CastChecked<UPaperTileSet>(Object);
    if (Value->GetTileSize() != FIntPoint(4, 8)) return;
    Injected = true; SawLayoutOnly = Value->GetTileCount() == 30 && Value->GetTileMetadata(29)
        && Value->GetTileUserData(0) == TEXT("first");
    Value->SetTileSize(FIntPoint(5, 8));
  });
  UeAgentReflectionAuthoring::FAssetApplyResult StageResult;
  TestFalse(TEXT("native stage verification rejects changed actual state"), UeAgentReflectionAuthoring::ApplyAsset(StageFailure, StageResult));
  FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(NativeHandle);
  FFileHelper::LoadFileToArray(RestoredDisk, *Filenames[0]);
  TestTrue(TEXT("stage failure restores before dependent writes"), Injected && SawLayoutOnly && StageResult.bRollbackSucceeded
      && StageResult.ErrorCode == TEXT("readback_mismatch") && Native(8, TEXT(""), true) && PublishedDisk == RestoredDisk);

  // Opposite axis changes cannot be made safe by swapping property order.
  // Hidden membership is a native fixture fact, not a newly editable field.
  FPaperTileSetTerrain HiddenTerrain; HiddenTerrain.TerrainName = TEXT("Hidden"); HiddenTerrain.CenterTileIndex = 16;
  Tiles()->AddTerrainDescription(HiddenTerrain);
  Tiles()->GetMutableTileMetadata(16)->TerrainMembership[0] = 0;
  Tiles()->MarkPackageDirty();
  TArray<TSharedPtr<FJsonValue>> MixedMetadata;
  for (int32 Index = 0; Index < 15; ++Index) {
    auto Item = MakeShared<FJsonObject>(); Item->Values = MetadataTemplate->Values;
    auto Fields = MakeShared<FJsonObject>(); Fields->Values = Item->GetObjectField(TEXT("fields"))->Values;
    Fields->SetStringField(TEXT("UserDataName"), Index == 12 ? TEXT("last") : TEXT("mixed"));
    Item->SetObjectField(TEXT("fields"), Fields); MixedMetadata.Add(MakeShared<FJsonValueObject>(Item));
  }
  if (!Export(TEXT("mixed-axes")) || !Edit(TEXT("mixed-axes"), {
      {TEXT("TileSheet"), MakeShared<FJsonValueString>(Paths[4])}, {TEXT("TileSize"), PointValue(4, 18)},
      {TEXT("PerTileData"), MakeShared<FJsonValueArray>(MixedMetadata)},
      {TEXT("Terrains"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Terrain(TEXT("Hidden"), 14)})}})) return false;
  Result = Import(TEXT("mixed-axes"));
  if (!TestTrue(TEXT("mixed axes and explicit metadata use settled native coordinate baseline"), Result.IsOk()
      && Tiles()->GetTileCountX() == 5 && Tiles()->GetTileCountY() == 3
      && Tiles()->GetTileMetadata(14)->TerrainMembership[0] == 0 && Tiles()->GetTileMetadata(13)->TerrainMembership[0] == 255
      && Tiles()->GetTileUserData(12) == TEXT("last") && NonTargets())) { if (!Result.IsOk()) AddError(Result.GetErrorJson()); return false; }
  // The final smaller sheet still fits the old (2,2) tile when tile size also
  // shrinks. An intermediate sheet-only allocation must not erase that data.
  if (!Export(TEXT("simultaneous-shrink")) || !Edit(TEXT("simultaneous-shrink"), {
      {TEXT("TileSheet"), MakeShared<FJsonValueString>(Paths[1])}, {TEXT("TileSize"), PointValue(4, 2)}})) return false;
  Result = Import(TEXT("simultaneous-shrink"));
  TestTrue(TEXT("simultaneous final layout preserves overlapping coordinate metadata"), Result.IsOk()
      && Tiles()->GetTileCountX() == 5 && Tiles()->GetTileCountY() == 4 && Tiles()->GetTileUserData(12) == TEXT("last"));
  if (!Result.IsOk()) AddError(Result.GetErrorJson());
  return !HasAnyErrors();
}
