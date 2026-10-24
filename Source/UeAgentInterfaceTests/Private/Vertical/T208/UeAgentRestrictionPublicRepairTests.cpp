#include "Misc/AutomationTest.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedStruct.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UObject/UnrealType.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PaperSprite.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T208/UeAgentT208WorkspaceValueEdit.h"

namespace {
struct FRestrictionPublicFixture {
  FAutomationTestBase& Test;
  TSharedPtr<const UE::UAI::CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
  TSharedPtr<const UE::UAI::CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
  FString Base = TEXT("/Game/UAI/Transient/RestrictionPublic_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  FString Workspace = FPaths::ProjectSavedDir() / TEXT("RestrictionRepair") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
  TArray<FString> Owned;
  explicit FRestrictionPublicFixture(FAutomationTestBase& InTest) : Test(InTest) {}
  bool Start() {
    FString Error;
    if (UE::UAI::Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return true;
    Test.AddError(Error); return false;
  }
  bool Submit(const TCHAR* Command, const TSharedRef<FJsonObject>& Args, TSharedPtr<FJsonObject>* Payload = nullptr) {
    const auto Result = UE::UAI::Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
      TEXT("restriction-public-repair"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate"), TEXT("uai:operation"), TEXT("uai:editor:session"), TEXT("uai:capture")});
    if (!Result.IsOk()) Test.AddError(Result.GetErrorJson());
    if (Result.IsOk() && Payload) {
      FString Error;
      if (!UE::UAI::CommandContract::Json::TryParseObject(Result.GetPayloadJson(), *Payload, Error)) { Test.AddError(Error); return false; }
    }
    return Result.IsOk();
  }
  FString Target(const TCHAR* Name) { FString Path = Base / Name; Owned.Add(Path); return Path; }
  bool Save(UObject* Asset) {
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    return UPackage::SavePackage(Asset->GetPackage(), Asset,
      *FPackageName::LongPackageNameToFilename(Asset->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()), Args);
  }
  bool Inline(const FString& Path, const TSharedRef<FJsonObject>& Document) {
    auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("inline"));
    Source->SetStringField(TEXT("document"), FUeAgentStructuredHjsonFormatter::WriteObject(Document));
    auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Path); Args->SetObjectField(TEXT("source"), Source);
    return Submit(TEXT("content_graph.import_workspace"), Args);
  }
  bool WorkspaceEdit(const FString& Path, const TMap<FString, TSharedPtr<FJsonValue>>& Changes) {
    const FString Directory = Workspace / FPackageName::GetLongPackageAssetName(Path);
    auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Path);
    Args->SetStringField(TEXT("workspace_path"), Directory); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
    if (!Submit(TEXT("content_graph.export_workspace"), Args)) return false;
    TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.hjson"), true, false);
    TMap<FString, int32> Counts;
    for (const FString& File : Files) {
      if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
      FString Text, Error; TSharedPtr<FJsonObject> Document;
      if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) { Test.AddError(Error); return false; }
      if (Document->HasField(TEXT("definitions"))) continue;
      bool Changed = false;
      for (const auto& Change : Changes) {
        const int32 Count = UE::UAI::Tests::WorkspaceValueEdit::ReplaceNamedLeaf(MakeShared<FJsonValueObject>(Document), Change.Key, Change.Value);
        Counts.FindOrAdd(Change.Key) += Count; Changed |= Count != 0;
      }
      if (Changed && !FFileHelper::SaveStringToFile(FUeAgentStructuredHjsonFormatter::WriteObject(Document.ToSharedRef()), *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
    }
    for (const auto& Change : Changes) if (!Test.TestEqual(TEXT("one public edit location: ") + Change.Key, Counts.FindRef(Change.Key), 1)) return false;
    auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory);
    Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Path); Args->SetObjectField(TEXT("source"), Source);
    return Submit(TEXT("content_graph.import_workspace"), Args);
  }
  ~FRestrictionPublicFixture() {
    // Delete the complete owned reference set through the public dependency-aware owner.
    TArray<TSharedPtr<FJsonValue>> Targets;
    for (const FString& Path : Owned) {
      if (auto* Object = LoadObject<UObject>(nullptr, *(Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path)))) {
        if (Object->GetPackage()->IsDirty()) Save(Object);
        Targets.Add(MakeShared<FJsonValueString>(Path));
      }
    }
    if (!Targets.IsEmpty() && Registry && Dispatch) {
      auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), Targets); Args->SetBoolField(TEXT("recursive"), false);
      for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); if (!Submit(TEXT("asset.delete"), Args)) break; }
    }
  }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiRestrictionPublicAnimationCreate,
  "UAI.Root.Hjson.RestrictionRepair.PublicAnimationCreate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiRestrictionPublicAnimationCreate::RunTest(const FString&) {
  FRestrictionPublicFixture Fixture(*this); if (!Fixture.Start()) return false;
  auto* Source = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Characters/PaladinRootMotion/Animations/sword_and_shield_idle_RM.sword_and_shield_idle_RM"));
  if (!TestTrue(TEXT("native skeleton and sample source"), Source && Source->GetSkeleton())) return false;
  auto Document = MakeShared<FJsonObject>(), Root = MakeShared<FJsonObject>(), Create = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("schema"), TEXT("uai.animation_curve.edit/1")); Document->SetObjectField(TEXT("root"), Root);
  Root->SetStringField(TEXT("type"), TEXT("/Script/Engine.AnimSequence")); Root->SetObjectField(TEXT("create"), Create);
  Create->SetStringField(TEXT("skeleton_ref"), Source->GetSkeleton()->GetPathName());
  const FString SequencePath = Fixture.Target(TEXT("CreatedSequence"));
  if (!Fixture.Inline(SequencePath, Document)) return false;
  auto* Sequence = LoadObject<UAnimSequence>(nullptr, *(SequencePath + TEXT(".CreatedSequence")));
  TestTrue(TEXT("public sequence creation retained skeleton and saved"), Sequence && Sequence->GetSkeleton() == Source->GetSkeleton() && !Sequence->GetPackage()->IsDirty());
  Root->SetStringField(TEXT("type"), TEXT("/Script/Engine.BlendSpace1D")); Create->SetNumberField(TEXT("dimension"), 1);
  auto Axis = MakeShared<FJsonObject>(); Axis->SetNumberField(TEXT("axis_index"), 0); Axis->SetStringField(TEXT("axis_name"), TEXT("Speed"));
  Axis->SetNumberField(TEXT("grid_divisions"), 4); Axis->SetNumberField(TEXT("minimum"), 0); Axis->SetNumberField(TEXT("maximum"), 600);
  Root->SetArrayField(TEXT("axes"), {MakeShared<FJsonValueObject>(Axis)});
  auto Sample = MakeShared<FJsonObject>(); Sample->SetStringField(TEXT("animation_ref"), Source->GetPathName()); Sample->SetStringField(TEXT("intent"), TEXT("create"));
  Sample->SetArrayField(TEXT("coordinates"), {MakeShared<FJsonValueNumber>(0)});
  Root->SetArrayField(TEXT("samples"), {MakeShared<FJsonValueObject>(Sample)});
  const FString BlendPath = Fixture.Target(TEXT("CreatedBlend"));
  if (!Fixture.Inline(BlendPath, Document)) return false;
  auto* Blend = LoadObject<UBlendSpace1D>(nullptr, *(BlendPath + TEXT(".CreatedBlend")));
  TestTrue(TEXT("public create persists requested axis and sample in one import"), Blend && Blend->GetBlendParameter(0).DisplayName == TEXT("Speed")
    && Blend->GetBlendParameter(0).Max == 600 && Blend->GetNumberOfBlendSamples() == 1 && Blend->GetBlendSample(0).Animation == Source && !Blend->GetPackage()->IsDirty());
  return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiRestrictionPublicCollision,
  "UAI.Root.Hjson.RestrictionRepair.PublicCollision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiRestrictionPublicCollision::RunTest(const FString&) {
  FRestrictionPublicFixture Fixture(*this); if (!Fixture.Start()) return false;
  auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
  if (!TestNotNull(TEXT("native mesh source"), Cube)) return false;
  const FString MeshPath = Fixture.Target(TEXT("Mesh"));
  auto* Mesh = DuplicateObject<UStaticMesh>(Cube, CreatePackage(*MeshPath), TEXT("Mesh"));
  Mesh->SetFlags(RF_Public | RF_Standalone);
  if (!Fixture.Save(Mesh) || !Fixture.WorkspaceEdit(MeshPath, {
      {TEXT("bDoubleSidedGeometry"), MakeShared<FJsonValueBoolean>(true)}, {TEXT("LinearDamping"), MakeShared<FJsonValueNumber>(2.75)}})) return false;
  Mesh = LoadObject<UStaticMesh>(nullptr, *(MeshPath + TEXT(".Mesh")));
  TestTrue(TEXT("mesh full native collision settings persist"), Mesh && Mesh->GetBodySetup() && Mesh->GetBodySetup()->bDoubleSidedGeometry
    && Mesh->GetBodySetup()->DefaultInstance.LinearDamping == 2.75f && !Mesh->GetPackage()->IsDirty());
  const FString TexturePath = Fixture.Target(TEXT("Texture"));
  auto* Texture = NewObject<UTexture2D>(CreatePackage(*TexturePath), TEXT("Texture"), RF_Public | RF_Standalone);
  TArray<uint8> Pixels; Pixels.Init(255, 16 * 16 * 4); Texture->Source.Init(16, 16, 1, 1, TSF_BGRA8, Pixels.GetData()); Texture->UpdateResource();
  if (!Fixture.Save(Texture)) return false;
  const FString SpritePath = Fixture.Target(TEXT("Sprite"));
  auto* Sprite = NewObject<UPaperSprite>(CreatePackage(*SpritePath), TEXT("Sprite"), RF_Public | RF_Standalone);
  FSpriteAssetInitParameters Init; Init.Texture = Texture; Init.Offset = FIntPoint::ZeroValue; Init.Dimension = FIntPoint(16, 16); Sprite->InitializeSprite(Init);
  if (!TestNotNull(TEXT("native sprite collision owner"), Sprite->BodySetup.Get()) || !Fixture.Save(Sprite)) return false;
  const int32 GeometryCount = Sprite->BodySetup->AggGeom.GetElementCount();
  if (!Fixture.WorkspaceEdit(SpritePath, {{TEXT("LinearDamping"), MakeShared<FJsonValueNumber>(3.5)}})) return false;
  Sprite = LoadObject<UPaperSprite>(nullptr, *(SpritePath + TEXT(".Sprite")));
  TestTrue(TEXT("sprite body instance survives native rebuild and reload"), Sprite && Sprite->BodySetup
    && Sprite->BodySetup->DefaultInstance.LinearDamping == 3.5f && Sprite->BodySetup->AggGeom.GetElementCount() == GeometryCount && !Sprite->GetPackage()->IsDirty());
  const FString PhysicsPath = Fixture.Target(TEXT("Physics"));
  auto* Physics = NewObject<UPhysicsAsset>(CreatePackage(*PhysicsPath), TEXT("Physics"), RF_Public | RF_Standalone);
  Physics->PhysicalAnimationProfiles.Add(TEXT("HitReaction")); Physics->ConstraintProfiles.Add(TEXT("Loose"));
  if (!Fixture.Save(Physics) || !Fixture.WorkspaceEdit(PhysicsPath, {
      {TEXT("physical_animation_profiles"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(TEXT("HitReaction")), MakeShared<FJsonValueString>(TEXT("Recovery"))})},
      {TEXT("constraint_profiles"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(TEXT("Loose")), MakeShared<FJsonValueString>(TEXT("Tight"))})}})) return false;
  Physics = LoadObject<UPhysicsAsset>(nullptr, *(PhysicsPath + TEXT(".Physics")));
  TestTrue(TEXT("physics profile arrays apply through public structure"), Physics && Physics->PhysicalAnimationProfiles.Contains(TEXT("Recovery"))
    && Physics->ConstraintProfiles.Contains(TEXT("Tight")) && !Physics->GetPackage()->IsDirty());
  return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiRestrictionPublicUserStruct,
  "UAI.Root.Hjson.RestrictionRepair.UserDefinedTableStruct", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiRestrictionPublicUserStruct::RunTest(const FString&) {
  FRestrictionPublicFixture Fixture(*this); if (!Fixture.Start()) return false;
  const FString StructPath = Fixture.Target(TEXT("RowStruct")), TablePath = Fixture.Target(TEXT("Table"));
  auto* Struct = FStructureEditorUtils::CreateUserDefinedStruct(CreatePackage(*StructPath), TEXT("RowStruct"), RF_Public | RF_Standalone);
  if (!TestNotNull(TEXT("native Blueprint user struct"), Struct) || !Fixture.Save(Struct)) return false;
  FString BoolName;
  for (TFieldIterator<FBoolProperty> It(Struct); It; ++It) { BoolName = It->GetName(); break; }
  if (!TestFalse(TEXT("native initialized bool field"), BoolName.IsEmpty())) return false;
  auto Document = MakeShared<FJsonObject>(), Root = MakeShared<FJsonObject>(), Rows = MakeShared<FJsonObject>();
  auto Row = MakeShared<FJsonObject>(), Properties = MakeShared<FJsonObject>(), Value = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("schema"), TEXT("uai.datatable_curvetable.edit/1")); Document->SetObjectField(TEXT("root"), Root);
  Root->SetStringField(TEXT("type"), TEXT("/Script/Engine.DataTable")); Root->SetStringField(TEXT("row_struct_ref"), Struct->GetPathName());
  Value->SetStringField(TEXT("type"), TEXT("bool")); Value->SetBoolField(TEXT("value"), true);
  Properties->SetObjectField(BoolName, Value); Row->SetObjectField(TEXT("properties"), Properties); Rows->SetObjectField(TEXT("First"), Row); Root->SetObjectField(TEXT("rows"), Rows);
  if (!Fixture.Inline(TablePath, Document)) return false;
  auto* Table = LoadObject<UDataTable>(nullptr, *(TablePath + TEXT(".Table")));
  const auto* Property = Table ? FindFProperty<FBoolProperty>(Table->GetRowStruct(), *BoolName) : nullptr;
  uint8* const* Data = Table ? Table->GetRowMap().Find(TEXT("First")) : nullptr;
  TestTrue(TEXT("Blueprint struct and authored row persisted"), Table && Cast<UUserDefinedStruct>(Table->GetRowStruct())
    && Property && Data && Property->GetPropertyValue_InContainer(*Data) && !Table->GetPackage()->IsDirty());
  return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiRestrictionPublicExternalFactory,
  "UAI.Root.Hjson.RestrictionRepair.NativeFactoryFormatAndName", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiRestrictionPublicExternalFactory::RunTest(const FString&) {
  FRestrictionPublicFixture Fixture(*this); if (!Fixture.Start()) return false;
  // Real 2x2 24-bit BMP. Native Formats still advertises PCX in UE5.6,
  // but its ImageWrapper no longer decodes PCX; that is not a plugin failure.
  TArray<uint8> Pixels; Pixels.Init(0, 70);
  Pixels[0] = 'B'; Pixels[1] = 'M'; Pixels[2] = 70; Pixels[10] = 54;
  Pixels[14] = 40; Pixels[18] = 2; Pixels[22] = 2;
  Pixels[26] = 1; Pixels[28] = 24; Pixels[34] = 16;
  for (int32 Index = 54; Index < 70; ++Index) Pixels[Index] = 180;
  IFileManager::Get().MakeDirectory(*Fixture.Workspace, true);
  const FString SourcePath = FPaths::ConvertRelativePathToFull(Fixture.Workspace / TEXT("SourceName.bmp"));
  if (!FFileHelper::SaveArrayToFile(Pixels, *SourcePath)) return false;
  const FString Target = Fixture.Target(TEXT("DifferentDestinationName"));
  auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("source_ref"), SourcePath); Args->SetStringField(TEXT("destination_ref"), Target);
  if (!Fixture.Submit(TEXT("external.texture.import"), Args)) return false;
  auto* Texture = LoadObject<UTexture2D>(nullptr, *(Target + TEXT(".DifferentDestinationName")));
  TestTrue(TEXT("native factory format and independent destination name persist"), Texture && Texture->Source.GetSizeX() == 2
    && Texture->Source.GetSizeY() == 2 && !Texture->GetPackage()->IsDirty());
  return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiRestrictionBlueprintSubclass,
  "UAI.Root.Hjson.RestrictionRepair.BlueprintSubclassRename", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiRestrictionBlueprintSubclass::RunTest(const FString&) {
  FRestrictionPublicFixture Fixture(*this); if (!Fixture.Start()) return false;
  const FString Path = Fixture.Target(TEXT("Widget"));
  auto* Factory = NewObject<UWidgetBlueprintFactory>(); Factory->ParentClass = UUserWidget::StaticClass();
  auto* Blueprint = Cast<UWidgetBlueprint>(Factory->FactoryCreateNew(UWidgetBlueprint::StaticClass(), CreatePackage(*Path), TEXT("Widget"), RF_Public | RF_Standalone, nullptr, GWarn));
  if (!TestNotNull(TEXT("native WidgetBlueprint fixture"), Blueprint)) return false;
  auto* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("Before"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
  FBlueprintEditorUtils::AddFunctionGraph(Blueprint, Graph, true, static_cast<UFunction*>(nullptr));
  auto Spec = MakeShared<FJsonObject>(), Graphs = MakeShared<FJsonObject>(), Edit = MakeShared<FJsonObject>();
  Spec->SetStringField(TEXT("operation"), TEXT("update")); Spec->SetStringField(TEXT("name"), TEXT("After"));
  Spec->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens)); Spec->SetStringField(TEXT("graph_kind"), TEXT("function_graph"));
  Graphs->SetObjectField(Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens), Spec); Edit->SetObjectField(TEXT("graphs"), Graphs);
  FString Error;
  if (!TestTrue(TEXT("shared rename eligibility follows native graph, not exact Blueprint class"), UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::ValidateGraphNames(*Blueprint, Edit, Error))) { AddError(Error); return false; }
  // Blueprint duplication creates sibling generated classes; isolate their namespace too.
  const FString ScratchName = TEXT("WidgetScratch_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  auto* ScratchPackage = CreatePackage(*(TEXT("/Temp/UAIRestriction/") + ScratchName));
  ScratchPackage->SetFlags(RF_Transient);
  auto* Scratch = DuplicateObject<UWidgetBlueprint>(Blueprint, ScratchPackage, *ScratchName);
  Scratch->ClearFlags(RF_Public | RF_Standalone);
  TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
  UeAgentBlueprintAuthoring::FMutationCounters Counters;
  if (!UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(*Scratch, Edit, Mutation, Counters, Error)
      || !Mutation->ApplyTo(*Blueprint, Counters, Error)) { AddError(Error); return false; }
  TestEqual(TEXT("shared native writer renames subclass graph"), Graph->GetName(), FString(TEXT("After")));
  return !HasAnyErrors();
}

namespace {
class FRestrictionPreviewLatent final : public IAutomationLatentCommand {
  FRestrictionPublicFixture Fixture;
  FString Path;
  TSharedPtr<FJsonObject> Ref;
  double Started = FPlatformTime::Seconds();
public:
  explicit FRestrictionPreviewLatent(FAutomationTestBase& Test) : Fixture(Test) {}
  bool Update() override {
    auto& Test = Fixture.Test;
    if (!Ref) {
      if (!Fixture.Start()) return true;
      Path = Fixture.Target(TEXT("Sequence"));
      auto* Sequence = NewObject<ULevelSequence>(CreatePackage(*Path), TEXT("Sequence"), RF_Public | RF_Standalone);
      Sequence->Initialize(); Sequence->GetMovieScene()->SetPlaybackRange(-24000, 48000);
      if (!Fixture.Save(Sequence)) return true;
      auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
      if (!Test.TestTrue(TEXT("native sequence editor opens"), Editors->OpenEditorForAsset(Sequence))) return true;
      if (!Test.TestTrue(TEXT("negative capture start uses public workspace"), Fixture.WorkspaceEdit(Path, {{TEXT("StartTimeSeconds"), MakeShared<FJsonValueNumber>(-0.5)}}))) return true;
      auto Args = MakeShared<FJsonObject>(), Parameters = MakeShared<FJsonObject>();
      Args->SetStringField(TEXT("family_id"), TEXT("asset_editor")); Args->SetStringField(TEXT("subject_ref"), TEXT("asset:") + Path + TEXT(".Sequence"));
      Args->SetStringField(TEXT("policy_id"), TEXT("policy:asset-editor:preview-action/v1"));
      Parameters->SetStringField(TEXT("action"), TEXT("seek")); Parameters->SetNumberField(TEXT("time_seconds"), -0.5); Args->SetObjectField(TEXT("parameters"), Parameters);
      TSharedPtr<FJsonObject> Status;
      if (!Fixture.Submit(TEXT("editor.session"), Args, &Status)) return true;
      Ref = Status->GetObjectField(TEXT("operation_ref"));
      return false;
    }
    auto Args = MakeShared<FJsonObject>(); Args->SetObjectField(TEXT("operation_ref"), Ref);
    TSharedPtr<FJsonObject> Status;
    if (!Fixture.Submit(TEXT("operation.status"), Args, &Status)) return true;
    if (!Status->GetBoolField(TEXT("terminal"))) {
      if (FPlatformTime::Seconds() - Started > 30) {
        Test.AddError(TEXT("preview operation did not finish in the isolated validation window"));
        return true;
      }
      Args->SetNumberField(TEXT("expected_generation"), Status->GetIntegerField(TEXT("generation")));
      if (!Fixture.Submit(TEXT("operation.resume"), Args, &Status)) return true;
      return false; // Let the actual editor scheduler advance before polling again.
    }
    FString Error; Status->TryGetStringField(TEXT("error_code"), Error);
    Test.TestTrue(TEXT("public negative seek finishes"), Error.IsEmpty());
    if (!Error.IsEmpty()) Test.AddError(FUeAgentStructuredHjsonFormatter::WriteObject(Status.ToSharedRef()));
    auto* Sequence = LoadObject<ULevelSequence>(nullptr, *(Path + TEXT(".Sequence")));
    auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    auto* Editor = Sequence ? Editors->FindEditorForAsset(Sequence, false) : nullptr;
    if (Test.TestNotNull(TEXT("editor remains open before public delete"), Editor)) {
      const auto Sequencer = static_cast<ILevelSequenceEditorToolkit*>(Editor)->GetSequencer();
      Test.TestTrue(TEXT("native focused sequence reached negative requested time"), Sequencer && FMath::IsNearlyEqual(Sequencer->GetLocalTime().AsSeconds(), -0.5));
    }
    Fixture.Submit(TEXT("operation.release"), Args);
    // Fixture destruction closes only this clean native editor through public delete.
    return true;
  }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiRestrictionPreviewSeekAndDelete,
  "UAI.Root.Hjson.RestrictionRepair.NegativePreviewAndOpenEditorDelete", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiRestrictionPreviewSeekAndDelete::RunTest(const FString&) {
  FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FRestrictionPreviewLatent>(*this));
  return true;
}
