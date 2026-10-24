#include "Misc/AutomationTest.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "IMediaModule.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "ExternalTexture.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace
{
TSharedPtr<FJsonObject> FindMediaTextureRow(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
    for (const auto& Field : Object->Values) if (Field.Value->Type == EJson::Object) {
        const auto Child = Field.Value->AsObject();
        if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
        if (auto Found = FindMediaTextureRow(Child, Name)) return Found;
    }
    return nullptr;
}
bool MediaTexturePixels(UMediaTexture& Texture, FColor Expected, FColor* Observed = nullptr)
{
    FModuleManager::LoadModuleChecked<IMediaModule>(TEXT("Media")).TickPostEngine();
    FlushRenderingCommands();
    if (!Texture.GetResource()) return false;
    TArray<FColor> Pixels;
    auto* Resource = Texture.GetResource();
    ENQUEUE_RENDER_COMMAND(UaiMediaTextureReadPixels)([Resource, &Pixels](FRHICommandListImmediate& RHICmdList) {
        if (Resource->TextureRHI.IsValid()) RHICmdList.ReadSurfaceData(Resource->TextureRHI, FIntRect(0, 0, 1, 1), Pixels, FReadSurfaceDataFlags(RCM_UNorm));
    });
    FlushRenderingCommands();
    if (Observed && Pixels.Num() == 1) *Observed = Pixels[0];
    return Pixels.Num() == 1 && Pixels[0] == Expected;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208MediaTextureWorkspaceTest,
    "UAI.Root.T208.MediaTexture.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208MediaTextureWorkspaceTest::RunTest(const FString&)
{
    using namespace UE::UAI;
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Base = TEXT("/Game/UAI/Transient/T208MediaTexture/") + Token;
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-media-texture/native") / Token);
    const TArray<FString> Names{TEXT("Texture"), TEXT("PlayerA"), TEXT("PlayerB"), TEXT("OtherTexture")};
    TArray<FString> Paths, Packages, Filenames;
    for (const auto& Name : Names) { Packages.Add(Base / Name); Paths.Add(Packages.Last() + TEXT(".") + Name); Filenames.Add(FPackageName::LongPackageNameToFilename(Packages.Last(), FPackageName::GetAssetPackageExtension())); }
    FString Error;
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
    auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
        return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args, TEXT("t208-media-texture"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
    auto Texture = [&](int32 Index = 0) { return LoadObject<UMediaTexture>(nullptr, *Paths[Index]); };
    auto Player = [&](int32 Index) { return LoadObject<UMediaPlayer>(nullptr, *Paths[Index]); };
    auto Save = [&](int32 Index) { auto* Object = LoadObject<UObject>(nullptr, *Paths[Index]); FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError; return Object && UPackage::SavePackage(Object->GetPackage(), Object, *Filenames[Index], Args); };
    ON_SCOPE_EXIT {
        TArray<TSharedPtr<FJsonValue>> Targets;
        for (int32 Index = 0; Index < Paths.Num(); ++Index) {
            if (auto* Object = FindObject<UObject>(nullptr, *Paths[Index]); Object && Object->GetPackage()->IsDirty()) TestTrue(TEXT("save own disposable fixture"), Save(Index));
            if (FPackageName::DoesPackageExist(Packages[Index])) Targets.Add(MakeShared<FJsonValueString>(Paths[Index]));
        }
        auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), Targets); Args->SetBoolField(TEXT("recursive"), false);
        for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
        FlushRenderingCommands();
    };
    for (int32 Index = 0; Index < Paths.Num(); ++Index) NewObject<UObject>(CreatePackage(*Packages[Index]), Index == 0 || Index == 3 ? UMediaTexture::StaticClass() : UMediaPlayer::StaticClass(), *Names[Index], RF_Public | RF_Standalone | RF_Transactional);
    Texture()->SetDefaultMediaPlayer(Player(1)); Texture()->AutoClear = true; Texture()->ClearColor = FLinearColor::Red; Texture()->UpdateResource();
    Texture(3)->AutoClear = true; Texture(3)->ClearColor = FLinearColor::Blue; Texture(3)->UpdateResource();
    for (int32 Index = 0; Index < Paths.Num(); ++Index) if (!Save(Index)) return false;
    if (!TestTrue(TEXT("baseline real red and blue RHI textures"), MediaTexturePixels(*Texture(), FColor::Red) && MediaTexturePixels(*Texture(3), FColor::Blue))) return false;
    const FSamplerStateRHIRef InitialSampler = Texture()->GetResource()->SamplerStateRHI;
    TArray<TWeakObjectPtr<UObject>> OtherObjects; TArray<TArray<uint8>> OtherDisk;
    for (int32 Index = 1; Index < Paths.Num(); ++Index) { OtherObjects.Add(LoadObject<UObject>(nullptr, *Paths[Index])); OtherDisk.AddDefaulted(); FFileHelper::LoadFileToArray(OtherDisk.Last(), *Filenames[Index]); }
    auto NonTargets = [&]() {
        for (int32 Index = 1; Index < Paths.Num(); ++Index) {
            auto* Object = LoadObject<UObject>(nullptr, *Paths[Index]); TArray<uint8> Bytes;
            if (OtherObjects[Index - 1].Get() != Object || Object->GetPackage()->IsDirty() || !FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) || Bytes != OtherDisk[Index - 1]) return false;
        }
        return Texture(3)->GetMediaPlayer() == nullptr && MediaTexturePixels(*Texture(3), FColor::Blue)
            && Player(1)->GetUrl().IsEmpty() && Player(2)->GetUrl().IsEmpty() && !Player(1)->IsPlaying() && !Player(2)->IsPlaying();
    };
    auto Native = [&](int32 PlayerIndex, FColor Color) {
        auto* Value = Texture();
        FColor Observed = FColor::Transparent;
        const bool bPixels = Value && MediaTexturePixels(*Value, Color, &Observed);
        const bool bPlayer = Value && Value->GetMediaPlayer() == Player(PlayerIndex);
        const bool bGuid = Value && Value->GetExternalTextureGuid() == Player(PlayerIndex)->GetGuid();
        if (!Value || !bPlayer || !Value->AutoClear || !bPixels || !bGuid) {
            AddInfo(FString::Printf(TEXT("MediaTexture native: player=%d autoclear=%d pixels=%d observed=%s expected=%s guid=%d"), bPlayer, Value ? Value->AutoClear : false, bPixels, *Observed.ToString(), *Color.ToString(), bGuid)); return false;
        }
        bool bRegistered = false, bNew = false, bTexture = false, bSampler = false, bOld = false; const FGuid Current = Player(PlayerIndex)->GetGuid(), Previous = Player(PlayerIndex == 1 ? 2 : 1)->GetGuid();
        auto* Resource = Value->GetResource();
        const FTextureRHIRef ExpectedExternal = static_cast<FTextureRHIRef>(Value->TextureReference.TextureReferenceRHI);
        ENQUEUE_RENDER_COMMAND(UaiMediaTextureCheckRegistration)([Resource, ExpectedExternal, Current, Previous, &bRegistered, &bNew, &bTexture, &bSampler, &bOld](FRHICommandListImmediate&) {
            FTextureRHIRef Registered, Old; FSamplerStateRHIRef Sampler, OldSampler;
            bNew = FExternalTextureRegistry::Get().GetExternalTexture(nullptr, Current, Registered, Sampler);
            bTexture = Registered == ExpectedExternal; bSampler = Sampler == Resource->SamplerStateRHI;
            bOld = FExternalTextureRegistry::Get().GetExternalTexture(nullptr, Previous, Old, OldSampler);
            bRegistered = bNew && bTexture && bSampler && !bOld;
        });
        FlushRenderingCommands(); if (!bRegistered) AddInfo(FString::Printf(TEXT("MediaTexture registry: current=%d texture=%d sampler=%d previous=%d"), bNew, bTexture, bSampler, bOld)); return bRegistered;
    };
    auto Export = [&](const FString& Stage) {
        auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[0]); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
        const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
    };
    auto Edit = [&](const FString& Stage, const FString& Name, const TSharedRef<FJsonValue>& Value) {
        TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false); int32 Count = 0;
        for (const auto& File : Files) {
            if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
            FString Text; TSharedPtr<FJsonObject> Document;
            if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
            if (auto Row = FindMediaTextureRow(Document, Name)) {
                if (Name == TEXT("ClearColor") && !TestTrue(TEXT("retain exported color component shape"), Row->HasTypedField<EJson::Array>(TEXT("value")) && Row->GetArrayField(TEXT("value")).Num() == 4)) return false;
                Row->SetField(TEXT("value"), Value); ++Count;
                if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
            }
        }
        return TestEqual(TEXT("one ordinary property row"), Count, 1);
    };
    auto Import = [&](const FString& Stage) { auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[0]); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args); };
    auto ColorValue = [](const FLinearColor& Color) { return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(Color.R), MakeShared<FJsonValueNumber>(Color.G), MakeShared<FJsonValueNumber>(Color.B), MakeShared<FJsonValueNumber>(Color.A)}); };
    if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), TEXT("MediaPlayer"), MakeShared<FJsonValueString>(Paths[2]))
        || !Edit(TEXT("edit"), TEXT("ClearColor"), ColorValue(FLinearColor::Green))
        || !Edit(TEXT("edit"), TEXT("NewStyleOutput"), MakeShared<FJsonValueBoolean>(false))
        || !Edit(TEXT("edit"), TEXT("AddressX"), MakeShared<FJsonValueString>(TEXT("TA_Wrap")))) return false;
    const TWeakObjectPtr<UMediaTexture> Before(Texture());
    bool SawNativePlayerNotification = false;
    const auto Notification = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent& Event) {
        if (Object && Object->GetPathName() == Paths[0] && Event.Property && Event.Property->GetFName() == TEXT("MediaPlayer"))
            SawNativePlayerNotification |= CastChecked<UMediaTexture>(Object)->GetMediaPlayer() == Player(2);
    });
    auto Result = Import(TEXT("edit"));
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Notification);
    if (!TestTrue(TEXT("ordinary player and texture settings import"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
    const bool bNotify = TestTrue(TEXT("actual native notification updates CurrentPlayer before reload"), SawNativePlayerNotification);
    const bool bReload = TestFalse(TEXT("texture independently reloaded"), Before.IsValid());
    const bool bNative = TestTrue(TEXT("native player queue guid and actual green resource agree"), Native(2, FColor::Green));
    const bool bSampler = TestTrue(TEXT("native address change rebuilds sampler"), Texture()->AddressX == TA_Wrap && Texture()->GetResource()->SamplerStateRHI != InitialSampler);
    const bool bOther = TestTrue(TEXT("non-target players texture and packages unchanged"), !Texture()->GetPackage()->IsDirty() && NonTargets());
    if (!bNotify || !bReload || !bNative || !bSampler || !bOther) return false;
    TArray<uint8> PublishedDisk; FFileHelper::LoadFileToArray(PublishedDisk, *Filenames[0]);
    if (!Export(TEXT("invalid")) || !Edit(TEXT("invalid"), TEXT("MediaPlayer"), MakeShared<FJsonValueString>(Paths[3]))) return false;
    Result = Import(TEXT("invalid"));
    if (!TestTrue(TEXT("wrong player reference refuses before native mutation"), !Result.IsOk() && Native(2, FColor::Green) && NonTargets())) return false;
    UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId = TEXT("media_texture"); Late.AssetPath = Packages[0]; Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
    auto PlayerPatch = MakeShared<FJsonObject>(); PlayerPatch->SetStringField(TEXT("property_path"), TEXT("MediaPlayer")); PlayerPatch->SetStringField(TEXT("value"), Paths[1]); Late.Properties.Add(MakeShared<FJsonValueObject>(PlayerPatch));
    auto ColorPatch = MakeShared<FJsonObject>(); ColorPatch->SetStringField(TEXT("property_path"), TEXT("ClearColor")); ColorPatch->SetField(TEXT("value"), ColorValue(FLinearColor::Red)); Late.Properties.Add(MakeShared<FJsonValueObject>(ColorPatch));
    bool SawPublished = false;
    Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
        SawPublished = Native(1, FColor::Red); Failure.Diagnostic.Code = TEXT("t208_media_texture_late_rejected"); return false; };
    UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
    TestFalse(TEXT("late rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
    TArray<uint8> RestoredDisk; FFileHelper::LoadFileToArray(RestoredDisk, *Filenames[0]);
    TestTrue(TEXT("published resource observed before rejection"), SawPublished);
    TestTrue(TEXT("checkpoint restores resource player queue disk and unrelated resources"), LateResult.bRollbackSucceeded && LateResult.Diagnostic.Code == TEXT("t208_media_texture_late_rejected")
        && Native(2, FColor::Green) && NonTargets() && RestoredDisk == PublishedDisk);
    TestTrue(TEXT("independent final re-export"), Export(TEXT("readback")));
    return !HasAnyErrors();
}
