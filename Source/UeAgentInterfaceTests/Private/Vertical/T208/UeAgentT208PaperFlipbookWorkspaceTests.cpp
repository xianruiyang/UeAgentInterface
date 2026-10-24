#include "Misc/AutomationTest.h"
#include "PaperFlipbook.h"
#include "PaperFlipbookComponent.h"
#include "PaperSprite.h"
#include "Editor.h"
#include "Engine/World.h"
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

namespace
{
TSharedPtr<FJsonObject> FindFlipbookRow(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
    for (const auto& Field : Object->Values) if (Field.Value->Type == EJson::Object) {
        const auto Child = Field.Value->AsObject();
        if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
        if (auto Found = FindFlipbookRow(Child, Name)) return Found;
    }
    return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208PaperFlipbookWorkspaceTest,
    "UAI.Root.T208.PaperFlipbook.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208PaperFlipbookWorkspaceTest::RunTest(const FString&)
{
    using namespace UE::UAI;
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world for real registered components"), World)) return false;
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Base = TEXT("/Game/UAI/Transient/T208PaperFlipbook/") + Token;
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-paper-flipbook/native") / Token);
    const TArray<FString> Names{TEXT("Flipbook"), TEXT("SpriteA"), TEXT("SpriteB"), TEXT("OtherFlipbook")};
    TArray<FString> Paths, Packages, Filenames;
    for (const auto& Name : Names) { Packages.Add(Base / Name); Paths.Add(Packages.Last() + TEXT(".") + Name); Filenames.Add(FPackageName::LongPackageNameToFilename(Packages.Last(), FPackageName::GetAssetPackageExtension())); }
    FString Error;
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
    auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
        return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args, TEXT("t208-paper-flipbook"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
    auto Load = [&](int32 Index = 0) { return LoadObject<UPaperFlipbook>(nullptr, *Paths[Index]); };
    auto Sprite = [&](int32 Index) { return LoadObject<UPaperSprite>(nullptr, *Paths[Index]); };
    const auto* CachedFrame = FindFProperty<FIntProperty>(UPaperFlipbookComponent::StaticClass(), TEXT("CachedFrameIndex"));
    if (!TestNotNull(TEXT("native reflected component frame cache"), CachedFrame)) return false;
    auto Save = [&](int32 Index) { auto* Object = LoadObject<UObject>(nullptr, *Paths[Index]); FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError; return Object && UPackage::SavePackage(Object->GetPackage(), Object, *Filenames[Index], Args); };
    TStrongObjectPtr<UPaperFlipbookComponent> Component(NewObject<UPaperFlipbookComponent>());
    TStrongObjectPtr<UPaperFlipbookComponent> OtherComponent(NewObject<UPaperFlipbookComponent>());
    ON_SCOPE_EXIT {
        for (auto* Value : {Component.Get(), OtherComponent.Get()}) { Value->SetFlipbook(nullptr); Value->DestroyComponent(); }
        Component.Reset(); OtherComponent.Reset(); FlushRenderingCommands();
        TArray<TSharedPtr<FJsonValue>> Targets;
        for (int32 Index = 0; Index < Paths.Num(); ++Index) {
            if (auto* Object = FindObject<UObject>(nullptr, *Paths[Index]); Object && Object->GetPackage()->IsDirty()) TestTrue(TEXT("save own disposable fixture"), Save(Index));
            if (FPackageName::DoesPackageExist(Packages[Index])) Targets.Add(MakeShared<FJsonValueString>(Paths[Index]));
        }
        auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), Targets); Args->SetBoolField(TEXT("recursive"), false);
        for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
    };
    for (int32 Index = 0; Index < Paths.Num(); ++Index) NewObject<UObject>(CreatePackage(*Packages[Index]), Index == 0 || Index == 3 ? UPaperFlipbook::StaticClass() : UPaperSprite::StaticClass(), *Names[Index], RF_Public | RF_Standalone | RF_Transactional);
    { FScopedFlipbookMutator Mutator(Load(3)); Mutator.FramesPerSecond = 5; FPaperFlipbookKeyFrame Frame; Frame.Sprite = Sprite(2); Frame.FrameRun = 5; Mutator.KeyFrames.Add(Frame); }
    for (int32 Index = 0; Index < Paths.Num(); ++Index) if (!Save(Index)) return false;
    if (!TestEqual(TEXT("target starts empty without preseed"), Load()->GetNumKeyFrames(), 0)) return false;
    const auto* Material = Load()->GetDefaultMaterial(); const auto Collision = Load()->GetCollisionSource();
    Component->SetFlipbook(Load()); Component->Stop(); Component->RegisterComponentWithWorld(World);
    OtherComponent->SetFlipbook(Load(3)); OtherComponent->Stop(); OtherComponent->RegisterComponentWithWorld(World); OtherComponent->SetPlaybackPosition(0.3f, false);
    FlushRenderingCommands();
    TArray<TWeakObjectPtr<UObject>> OtherObjects; TArray<TArray<uint8>> OtherDisk;
    for (int32 Index = 1; Index < Paths.Num(); ++Index) { OtherObjects.Add(LoadObject<UObject>(nullptr, *Paths[Index])); OtherDisk.AddDefaulted(); FFileHelper::LoadFileToArray(OtherDisk.Last(), *Filenames[Index]); }
    auto NonTargets = [&]() {
        for (int32 Index = 1; Index < Paths.Num(); ++Index) {
            auto* Object = LoadObject<UObject>(nullptr, *Paths[Index]); TArray<uint8> Bytes;
            if (OtherObjects[Index - 1].Get() != Object || Object->GetPackage()->IsDirty() || !FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) || Bytes != OtherDisk[Index - 1]) return false;
        }
        return OtherComponent->IsRegistered() && OtherComponent->GetFlipbook() == Load(3) && !OtherComponent->IsPlaying()
            && FMath::IsNearlyEqual(OtherComponent->GetPlaybackPosition(), 0.3f) && CachedFrame->GetPropertyValue_InContainer(OtherComponent.Get()) == 0;
    };
    auto Native = [&](const TArray<int32>& Sprites, const TArray<int32>& Runs, float Fps, float Position) {
        FlushRenderingCommands(); auto* Book = Load();
        if (!Book || Book->GetNumKeyFrames() != Sprites.Num() || Book->GetFramesPerSecond() != Fps || Book->GetDefaultMaterial() != Material || Book->GetCollisionSource() != Collision) return false;
        int32 Sum = 0;
        for (int32 Index = 0; Index < Sprites.Num(); ++Index) {
            if (Book->GetSpriteAtFrame(Index) != Sprite(Sprites[Index]) || Book->GetKeyFrameChecked(Index).FrameRun != Runs[Index]) return false;
            const float Middle = (Sum + 0.5f * Runs[Index]) / Fps;
            if (Book->GetKeyFrameIndexAtTime(Middle) != Index || Book->GetSpriteAtTime(Middle) != Sprite(Sprites[Index])) return false;
            Sum += Runs[Index];
        }
        return Book->GetNumFrames() == Sum && FMath::IsNearlyEqual(Book->GetTotalDuration(), Sum / Fps)
            && Component->IsRegistered() && Component->GetFlipbook() == Book && !Component->IsPlaying()
            && FMath::IsNearlyEqual(Component->GetPlaybackPosition(), Position)
            && CachedFrame->GetPropertyValue_InContainer(Component.Get()) == Book->GetKeyFrameIndexAtTime(Position)
            && NonTargets();
    };
    auto Export = [&](const FString& Stage) { auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[0]); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring")); const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
    auto Import = [&](const FString& Stage) { auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[0]); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args); };
    auto Edit = [&](const FString& Stage, const TArray<int32>& Order, const TArray<int32>& Runs, float Fps, const FString& Invalid = FString()) {
        TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
        TSharedPtr<FJsonObject> Document, Frames, Rate, Template; FString PropertyFile;
        TMap<FString, TSharedPtr<FJsonObject>> Definitions;
        for (const auto& File : Files) {
            if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
            FString Text; TSharedPtr<FJsonObject> Parsed; if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Parsed, Error)) return false;
            if (auto Found = FindFlipbookRow(Parsed, TEXT("KeyFrames"))) { Frames = Found; Rate = FindFlipbookRow(Parsed, TEXT("FramesPerSecond")); Document = Parsed; PropertyFile = File; }
            const TSharedPtr<FJsonObject>* Values = nullptr;
            if (Parsed->TryGetObjectField(TEXT("definitions"), Values)) for (const auto& Entry : (*Values)->Values) Definitions.Add(Entry.Key, Entry.Value->AsObject());
        }
        if (!TestTrue(TEXT("ordinary frames and fps property rows"), Frames && Rate && Definitions.Contains(Frames->GetStringField(TEXT("definition"))))) return false;
        auto Schema = Definitions.FindChecked(Frames->GetStringField(TEXT("definition")))->GetObjectField(TEXT("value_definition")); Template = Schema->GetObjectField(TEXT("element_template"));
        if (!TestTrue(TEXT("copyable keyframe fields and native default supplied publicly"), Template && Template->HasTypedField<EJson::Object>(TEXT("fields"))
            && Template->GetObjectField(TEXT("fields"))->HasField(TEXT("Sprite")) && Template->GetObjectField(TEXT("fields"))->GetNumberField(TEXT("FrameRun")) == 1)) return false;
        const auto Existing = Frames->GetArrayField(TEXT("value")); TArray<TSharedPtr<FJsonValue>> Desired;
        for (int32 Index = 0; Index < Order.Num(); ++Index) {
            auto Item = MakeShared<FJsonObject>(); Item->Values = Existing.IsEmpty() ? Template->Values : Existing[Order[Index]]->AsObject()->Values;
            auto Fields = MakeShared<FJsonObject>(); Fields->Values = Item->GetObjectField(TEXT("fields"))->Values; Item->SetObjectField(TEXT("fields"), Fields);
            if (Existing.IsEmpty()) Fields->SetStringField(TEXT("Sprite"), Paths[Order[Index] + 1]);
            Fields->SetNumberField(TEXT("FrameRun"), Runs[Index]);
            if (Invalid == TEXT("type")) Fields->SetStringField(TEXT("FrameRun"), TEXT("invalid-run"));
            if (Invalid == TEXT("reference")) Fields->SetStringField(TEXT("Sprite"), Paths[3]);
            Desired.Add(MakeShared<FJsonValueObject>(Item));
        }
        Frames->SetArrayField(TEXT("value"), Desired); Rate->SetNumberField(TEXT("value"), Fps);
        FString Text; return FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error) && FFileHelper::SaveStringToFile(Text, *PropertyFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    };
    if (!Export(TEXT("first")) || !Edit(TEXT("first"), {0, 1}, {2, 3}, 10)) return false;
    TWeakObjectPtr<UPaperFlipbook> Before(Load()); auto Result = Import(TEXT("first"));
    if (!TestTrue(TEXT("first frames created through public element template"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
    TestFalse(TEXT("real independent replacement despite registered consumer"), Before.IsValid());
    if (!TestTrue(TEXT("native first frames fps and component cache agree"), Native({1, 2}, {2, 3}, 10, 0))) return false;
    Component->SetPlaybackPosition(0.15f, false);
    if (!Export(TEXT("reordered")) || !Edit(TEXT("reordered"), {1, 0}, {1, 4}, 20)) return false;
    Result = Import(TEXT("reordered")); if (!TestTrue(TEXT("reorder runs and fps import together"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
    if (!TestTrue(TEXT("native time indexing and existing component cached frame follow reordered data"), Native({2, 1}, {1, 4}, 20, 0.15f))) return false;
    for (const FString Invalid : {FString(TEXT("type")), FString(TEXT("reference")), FString(TEXT("bounds"))}) {
        if (!Export(Invalid) || !Edit(Invalid, {0, 1}, {1, 4}, Invalid == TEXT("bounds") ? -1 : 20, Invalid)) return false;
        Result = Import(Invalid); if (!TestTrue(TEXT("invalid frame input rejects without native or non-target changes"), !Result.IsOk() && Native({2, 1}, {1, 4}, 20, 0.15f))) return false;
    }
    TArray<uint8> PublishedDisk; FFileHelper::LoadFileToArray(PublishedDisk, *Filenames[0]);
    UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId = TEXT("paper_flipbook"); Late.AssetPath = Packages[0]; Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
    auto Patch = MakeShared<FJsonObject>(); Patch->SetStringField(TEXT("property_path"), TEXT("FramesPerSecond")); Patch->SetNumberField(TEXT("value"), 4); Late.Properties.Add(MakeShared<FJsonValueObject>(Patch));
    bool SawPublished = false;
    Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
        SawPublished = Native({2, 1}, {1, 4}, 4, 0.15f); Failure.Diagnostic.Code = TEXT("t208_flipbook_late_rejected"); return false; };
    UeAgentReflectionAuthoring::FAssetApplyResult LateResult; TestFalse(TEXT("late rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
    TArray<uint8> RestoredDisk; FFileHelper::LoadFileToArray(RestoredDisk, *Filenames[0]);
    if (!TestTrue(TEXT("late callback observes persisted fps and changed component frame"), SawPublished)
        || !TestTrue(TEXT("checkpoint restores native fps index cache reference and disk"), LateResult.bRollbackSucceeded && LateResult.Diagnostic.Code == TEXT("t208_flipbook_late_rejected")
            && Native({2, 1}, {1, 4}, 20, 0.15f) && PublishedDisk == RestoredDisk)) return false;
    if (!Export(TEXT("clear")) || !Edit(TEXT("clear"), {}, {}, 12)) return false;
    Result = Import(TEXT("clear")); TestTrue(TEXT("clear frames refreshes component cache and persists"), Result.IsOk() && Native({}, {}, 12, 0.15f));
    TestTrue(TEXT("independent final re-export"), Export(TEXT("readback")));
    return !HasAnyErrors();
}
