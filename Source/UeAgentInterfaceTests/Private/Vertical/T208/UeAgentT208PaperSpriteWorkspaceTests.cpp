// Copyright Epic Games, Inc. All Rights Reserved.
#include "Misc/AutomationTest.h"
#include "PaperSprite.h"
#include "PaperSpriteAtlas.h"
#include "PaperSpriteComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Editor.h"
#include "PhysicsEngine/BodySetup.h"
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
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

namespace {
TSharedPtr<FJsonObject> SpriteRow(const TSharedPtr<FJsonObject>& Object, const FString& Name) {
  for (const auto& Field : Object->Values) if (Field.Value->Type == EJson::Object) {
    const auto Child = Field.Value->AsObject();
    if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
    if (auto Found = SpriteRow(Child, Name)) return Found;
  }
  return nullptr;
}
TSharedPtr<FJsonValue> VectorValue(double X, double Y) {
  return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(Y)});
}
struct FSpriteTestAccess : UPaperSprite {
  static auto Previous() { return &FSpriteTestAccess::PreviousAtlasGroup; }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiT208PaperSpriteWorkspace,
  "UAI.Root.T208.PaperSprite.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiT208PaperSpriteWorkspace::RunTest(const FString&) {
  using namespace UE::UAI;
  UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
  if (!TestNotNull(TEXT("editor world"), World)) return false;
  const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
  const FString Base = TEXT("/Game/UAI/Transient/T208PaperSprite/") + Token;
  const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-paper-sprite/native") / Token);
  const TArray<FString> Names{TEXT("Sprite"), TEXT("Source"), TEXT("Replacement"), TEXT("OtherSprite"), TEXT("Atlas")};
  TArray<FString> Paths, Packages, Filenames;
  for (const auto& Name : Names) { Packages.Add(Base / Name); Paths.Add(Packages.Last() + TEXT(".") + Name); Filenames.Add(FPackageName::LongPackageNameToFilename(Packages.Last(), FPackageName::GetAssetPackageExtension())); }
  FString Error;
  TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
  TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
  if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
  auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
    return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args, TEXT("t208-paper-sprite"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
  auto Sprite = [&](int32 Index = 0) { return LoadObject<UPaperSprite>(nullptr, *Paths[Index]); };
  auto Texture = [&](int32 Index) { return LoadObject<UTexture2D>(nullptr, *Paths[Index]); };
  auto Save = [&](int32 Index) { auto* Object = LoadObject<UObject>(nullptr, *Paths[Index]); FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError; return Object && UPackage::SavePackage(Object->GetPackage(), Object, *Filenames[Index], Args); };
  TStrongObjectPtr<UPaperSpriteComponent> Component(NewObject<UPaperSpriteComponent>());
  TStrongObjectPtr<UPaperSpriteComponent> OtherComponent(NewObject<UPaperSpriteComponent>());
  ON_SCOPE_EXIT {
    for (auto* Value : {Component.Get(), OtherComponent.Get()}) { Value->SetSprite(nullptr); Value->DestroyComponent(); }
    Component.Reset(); OtherComponent.Reset(); FlushRenderingCommands();
    TArray<TSharedPtr<FJsonValue>> Targets;
    for (int32 Index = 0; Index < Paths.Num(); ++Index) {
      if (auto* Object = FindObject<UObject>(nullptr, *Paths[Index]); Object && Object->GetPackage()->IsDirty()) TestTrue(TEXT("save own disposable fixture"), Save(Index));
      if (FPackageName::DoesPackageExist(Packages[Index])) Targets.Add(MakeShared<FJsonValueString>(Paths[Index]));
    }
    auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), Targets); Args->SetBoolField(TEXT("recursive"), false);
    for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
  };
  for (int32 Index : {1, 2}) {
    auto* Value = NewObject<UTexture2D>(CreatePackage(*Packages[Index]), *Names[Index], RF_Public | RF_Standalone | RF_Transactional);
    const int32 Width = Index == 1 ? 64 : 128, Height = Index == 1 ? 32 : 64;
    TArray<uint8> Pixels; Pixels.Init(255, Width * Height * 4);
    Value->Source.Init(Width, Height, 1, 1, TSF_BGRA8, Pixels.GetData()); Value->UpdateResource();
  }
  for (int32 Index : {0, 3}) {
    auto* Value = NewObject<UPaperSprite>(CreatePackage(*Packages[Index]), *Names[Index], RF_Public | RF_Standalone | RF_Transactional);
    FSpriteAssetInitParameters Init; Init.Texture = Texture(1); Init.Offset = FIntPoint(0, 0); Init.Dimension = FIntPoint(16, 12); Value->InitializeSprite(Init);
  }
  NewObject<UPaperSpriteAtlas>(CreatePackage(*Packages[4]), *Names[4], RF_Public | RF_Standalone | RF_Transactional);
  for (int32 Index = 0; Index < Paths.Num(); ++Index) if (!Save(Index)) return false;
  Component->SetSprite(Sprite()); Component->RegisterComponentWithWorld(World);
  OtherComponent->SetSprite(Sprite(3)); OtherComponent->RegisterComponentWithWorld(World); FlushRenderingCommands();
  TArray<TWeakObjectPtr<UObject>> Others; TArray<TArray<uint8>> OtherDisk;
  for (int32 Index = 1; Index < Paths.Num(); ++Index) { Others.Add(LoadObject<UObject>(nullptr, *Paths[Index])); OtherDisk.AddDefaulted(); FFileHelper::LoadFileToArray(OtherDisk.Last(), *Filenames[Index]); }
  const auto OtherRender = Sprite(3)->BakedRenderData;
  auto NonTargets = [&]() {
    for (int32 Index = 1; Index < Paths.Num(); ++Index) {
      UObject* Value = LoadObject<UObject>(nullptr, *Paths[Index]); TArray<uint8> Bytes;
      if (Others[Index - 1].Get() != Value || Value->GetPackage()->IsDirty() || !FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) || Bytes != OtherDisk[Index - 1]) return false;
    }
    return Sprite(3)->BakedRenderData == OtherRender && OtherComponent->IsRegistered() && OtherComponent->GetSprite() == Sprite(3);
  };
  auto Native = [&](float Ppu, double SourceX, bool Dirty) {
    FlushRenderingCommands(); auto* Value = Sprite();
    if (!Value || Value->GetAtlasGroup() || Value->GetSourceTexture() != Texture(2) || Value->GetSourceUV() != FVector2D(SourceX, 12)
        || Value->GetSourceSize() != FVector2D(16, 10) || Value->GetPivotPosition() != FVector2D(10, 15)
        || Value->GetPixelsPerUnrealUnit() != Ppu || Value->GetPackage()->IsDirty() != Dirty || Value->BakedRenderData.IsEmpty()) return false;
    double MinX = MAX_dbl, MaxX = -MAX_dbl, MinZ = MAX_dbl, MaxZ = -MAX_dbl, MinU = MAX_dbl, MaxU = -MAX_dbl, MinV = MAX_dbl, MaxV = -MAX_dbl;
    for (const auto& Vertex : Value->BakedRenderData) {
      MinX = FMath::Min(MinX, Vertex.X); MaxX = FMath::Max(MaxX, Vertex.X); MinZ = FMath::Min(MinZ, Vertex.Y); MaxZ = FMath::Max(MaxZ, Vertex.Y);
      MinU = FMath::Min(MinU, Vertex.Z); MaxU = FMath::Max(MaxU, Vertex.Z);
      MinV = FMath::Min(MinV, Vertex.W); MaxV = FMath::Max(MaxV, Vertex.W);
    }
    return FMath::IsNearlyEqual(MinX, (SourceX - 10) / Ppu) && FMath::IsNearlyEqual(MaxX, (SourceX + 6) / Ppu)
        && FMath::IsNearlyEqual(MinZ, -7.0 / Ppu) && FMath::IsNearlyEqual(MaxZ, 3.0 / Ppu)
        && FMath::IsNearlyEqual(MinU, SourceX / 128) && FMath::IsNearlyEqual(MaxU, (SourceX + 16) / 128)
        && FMath::IsNearlyEqual(MinV, 12.0 / 64) && FMath::IsNearlyEqual(MaxV, 22.0 / 64)
        && Value->BodySetup && Value->BodySetup->AggGeom.GetElementCount() > 0
        && Component->IsRegistered() && Component->GetSprite() == Value && Component->IsPhysicsStateCreated()
        // Native render bounds also include the thickness endpoints at the origin.
        && FMath::IsNearlyEqual(Component->Bounds.BoxExtent.X,
          (FMath::Max(0.0, (SourceX + 6.0) / Ppu) - FMath::Min(0.0, (SourceX - 10.0) / Ppu)) * 0.5)
        && FMath::IsNearlyEqual(Component->Bounds.BoxExtent.Z, 5.0 / Ppu)
        && NonTargets();
  };
  auto Export = [&](const FString& Stage) { auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[0]); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring")); auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
  auto Import = [&](const FString& Stage) { auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[0]); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args); };
  auto Edit = [&](const FString& Stage, const TMap<FString, TSharedPtr<FJsonValue>>& Values) {
    TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
    int32 Found = 0;
    for (const auto& File : Files) {
      if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
      FString Text; TSharedPtr<FJsonObject> Document;
      if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
      bool Changed = false;
      for (const auto& Pair : Values) if (auto Row = SpriteRow(Document, Pair.Key)) { Row->SetField(TEXT("value"), Pair.Value); Changed = true; ++Found; }
      if (Changed && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
    }
    return TestEqual(TEXT("ordinary public property rows"), Found, Values.Num());
  };
  if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), {
      {TEXT("SourceTexture"), MakeShared<FJsonValueString>(Paths[2])}, {TEXT("SourceUV"), VectorValue(8.4, 12.4)}, {TEXT("SourceDimension"), VectorValue(16.3, 10.2)},
      {TEXT("PivotMode"), MakeShared<FJsonValueString>(TEXT("Custom"))}, {TEXT("CustomPivotPoint"), VectorValue(10, 15)},
      {TEXT("PixelsPerUnrealUnit"), MakeShared<FJsonValueNumber>(2)}, {TEXT("SpriteCollisionDomain"), MakeShared<FJsonValueString>(TEXT("Use3DPhysics"))}})) return false;
  TWeakObjectPtr<UPaperSprite> Before(Sprite()); auto Result = Import(TEXT("edit"));
  if (!TestTrue(TEXT("ordinary source region pivot import"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
  TestFalse(TEXT("independent replacement with live component"), Before.IsValid());
  if (!TestTrue(TEXT("native pixel to geometry UV collision and component resources"), Native(2, 8, false))) return false;
  if (!Export(TEXT("readback"))) return false;
  Result = Import(TEXT("readback"));
  if (!TestTrue(TEXT("unchanged workspace follows no-explicit-changes contract"), !Result.IsOk()
      && Result.GetErrorJson().Contains(TEXT("hjson_import_no_explicit_changes")) && Native(2, 8, false))) return false;
  if (!Edit(TEXT("readback"), {{TEXT("SourceUV"), VectorValue(8.4, 12.4)}})) return false;
  Result = Import(TEXT("readback"));
  if (!TestTrue(TEXT("repeated fractional request normalizes stably"), Result.IsOk() && Native(2, 8, false))) {
    if (!Result.IsOk()) AddError(Result.GetErrorJson()); return false;
  }
  for (const FString Stage : {FString(TEXT("wrongtype")), FString(TEXT("wrongreference")), FString(TEXT("proposedatlas"))}) {
    const FString Key = Stage == TEXT("wrongtype") ? TEXT("SourceUV") : Stage == TEXT("wrongreference") ? TEXT("SourceTexture") : TEXT("AtlasGroup");
    const FString Value = Stage == TEXT("wrongtype") ? TEXT("invalid-vector") : Stage == TEXT("wrongreference") ? Paths[3] : Paths[4];
    if (!Export(Stage) || !Edit(Stage, {{Key, MakeShared<FJsonValueString>(Value)}})) return false;
    Result = Import(Stage); if (!TestTrue(TEXT("invalid or unresolved atlas input rejects before write"), !Result.IsOk() && Native(2, 8, false))) return false;
    if (Stage == TEXT("proposedatlas")) TestTrue(TEXT("explicit unresolved native atlas reason"), Result.GetErrorJson().Contains(TEXT("paper_sprite_atlas_native_reentry_unresolved")));
  }
  for (bool PreviousOnly : {false, true}) {
    auto* Atlas = LoadObject<UPaperSpriteAtlas>(nullptr, *Paths[4]);
    auto* Group = FindFProperty<FObjectProperty>(UPaperSprite::StaticClass(), TEXT("AtlasGroup"));
    if (PreviousOnly) Sprite()->*FSpriteTestAccess::Previous() = Atlas;
    else Group->SetObjectPropertyValue_InContainer(Sprite(), Atlas);
    const FString Stage = PreviousOnly ? TEXT("previousatlas") : TEXT("currentatlas");
    if (!Export(Stage) || !Edit(Stage, {{TEXT("SourceUV"), VectorValue(9, 12)}})) return false;
    Result = Import(Stage);
    if (PreviousOnly) Sprite()->*FSpriteTestAccess::Previous() = nullptr;
    else Group->SetObjectPropertyValue_InContainer(Sprite(), nullptr);
    if (!TestTrue(TEXT("current and previous atlas guard never invokes generator"), !Result.IsOk()
        && Result.GetErrorJson().Contains(TEXT("paper_sprite_atlas_native_reentry_unresolved"))
        && Atlas->GeneratedTextures.IsEmpty() && Native(2, 8, false))) return false;
  }
  TArray<uint8> PublishedDisk; FFileHelper::LoadFileToArray(PublishedDisk, *Filenames[0]);
  auto* Ppu = FindFProperty<FFloatProperty>(UPaperSprite::StaticClass(), TEXT("PixelsPerUnrealUnit"));
  Ppu->SetPropertyValue_InContainer(Sprite(), 4); FPropertyChangedEvent Event(Ppu); Sprite()->PostEditChangeProperty(Event); Sprite()->MarkPackageDirty();
  if (!TestTrue(TEXT("dirty memory differs from disk before late failure"), Native(4, 8, true))) return false;
  UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId = TEXT("paper_sprite"); Late.AssetPath = Packages[0]; Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
  auto Patch = MakeShared<FJsonObject>(); Patch->SetStringField(TEXT("property_path"), TEXT("SourceUV")); Patch->SetField(TEXT("value"), VectorValue(20, 12)); Late.Properties.Add(MakeShared<FJsonValueObject>(Patch));
  bool SawPublished = false, ReachedLateReadback = false;
  Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
    ReachedLateReadback = true; SawPublished = Native(4, 20, false);
    if (!SawPublished) {
      auto* Value = Sprite();
      AddInfo(FString::Printf(TEXT("late native uv=%s size=%s pivot=%s ppu=%f dirty=%d body=%d registered=%d physics=%d bounds=%s non_targets=%d"),
          *Value->GetSourceUV().ToString(), *Value->GetSourceSize().ToString(), *Value->GetPivotPosition().ToString(), Value->GetPixelsPerUnrealUnit(),
          Value->GetPackage()->IsDirty(), Value->BodySetup ? Value->BodySetup->AggGeom.GetElementCount() : -1, Component->IsRegistered(),
          Component->IsPhysicsStateCreated(), *Component->Bounds.BoxExtent.ToString(), NonTargets()));
      for (const auto& Vertex : Value->BakedRenderData) AddInfo(TEXT("late vertex ") + Vertex.ToString());
    }
    Failure.Diagnostic.Code = TEXT("t208_sprite_late_rejected"); return false;
  };
  UeAgentReflectionAuthoring::FAssetApplyResult LateResult; TestFalse(TEXT("late rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
  TArray<uint8> RestoredDisk; FFileHelper::LoadFileToArray(RestoredDisk, *Filenames[0]);
  TestTrue(TEXT("late callback sees changed native saved geometry"), SawPublished);
  if (!ReachedLateReadback) AddError(TEXT("late readback not reached: ") + LateResult.Diagnostic.Code);
  TestTrue(TEXT("restore dirty native memory original disk and non-target consumers"), LateResult.bRollbackSucceeded && Native(4, 8, true) && PublishedDisk == RestoredDisk);
  if (!LateResult.bRollbackSucceeded) AddError(LateResult.Diagnostic.Code);
  return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiT208PaperSpriteStrictDefault,
  "UAI.Root.T208.PaperSprite.NativeNormalizationStrictDefault", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiT208PaperSpriteStrictDefault::RunTest(const FString&) {
  using namespace UeAgentAuthoringObjectApply;
  const FString Prefix = TEXT("/Temp/UAI_SpriteStrict_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  const auto Authorization = RegisterDomainAuthorizedOwnerPackagePrefix(Prefix);
  auto* Sprite = NewObject<UPaperSprite>(CreatePackage(*Prefix), TEXT("Sprite"), RF_Public | RF_Standalone);
  ON_SCOPE_EXIT { UnregisterDomainAuthorizedOwnerPackagePrefix(Authorization); Sprite->ClearFlags(RF_Public | RF_Standalone); Sprite->MarkAsGarbage(); };
  auto Descriptor = MakeShared<FJsonObject>(); Descriptor->SetStringField(TEXT("property_path"), TEXT("SourceUV")); Descriptor->SetField(TEXT("value"), VectorValue(8.4, 12.4));
  FOptions Options; Options.bCompactValues = true; Options.bAllowDomainAuthorizedOwners = true;
  FPreparedApply Prepared; FFailure Failure; FApplyResult Result;
  if (!TestTrue(TEXT("shared strict default prepares"), PrepareObject(Sprite, {MakeShared<FJsonValueObject>(Descriptor)}, Options, Prepared, Failure))
      || !TestTrue(TEXT("shared native apply executes"), ApplyPrepared(Prepared, Result))) return false;
  TestFalse(TEXT("default verification never silently accepts native changes"), VerifyPrepared(Prepared, Result));
  TestEqual(TEXT("strict default retains mismatch reason"), Result.Diagnostic.Code, FString(TEXT("readback_mismatch")));
  TArray<TSharedPtr<FJsonValue>> Normalized;
  TestTrue(TEXT("explicit isolated preview capture"), Prepared.CaptureNativeNormalizedDescriptors(Normalized, Failure));
  FPreparedApply Canonical;
  TestTrue(TEXT("captured canonical descriptor verifies strictly"), PrepareObject(Sprite, Normalized, Options, Canonical, Failure)
      && ApplyPrepared(Canonical, Result) && VerifyPrepared(Canonical, Result));
  TestEqual(TEXT("native coordinates captured"), Sprite->GetSourceUV(), FVector2D(8, 12));
  return !HasAnyErrors();
}
