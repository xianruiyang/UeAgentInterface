#include "Misc/AutomationTest.h"
#include "Engine/Texture2D.h"
#include "Engine/VolumeTexture.h"
#include "TextureResource.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
bool SaveVolumeFixture(UObject& Object)
{
    FAssetRegistryModule::AssetCreated(&Object);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    const FString Filename = FPackageName::LongPackageNameToFilename(Object.GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Object.GetPackage(), &Object, *Filename, Args)) return false;
    FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({Filename}, true);
    return true;
}
TSharedPtr<FJsonObject> FindVolumeField(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
    for (const auto& Pair : Object->Values)
    {
        if (Pair.Value->Type != EJson::Object) continue;
        auto Child = Pair.Value->AsObject();
        if (Pair.Key == Name && Child->HasField(TEXT("value"))) return Child;
        if (auto Found = FindVolumeField(Child, Name)) return Found;
    }
    return nullptr;
}
bool VolumeMatches(UVolumeTexture* Volume, const TArray<UTexture2D*>& Sources)
{
    if (!Volume || !Volume->Source.IsValid() || Volume->Source.GetNumSlices() != Sources.Num()
        || Volume->Source.GetSizeX() != 16 || Volume->Source.GetSizeY() != 8 || Volume->Source.GetFormat() != TSF_BGRA8
        || Volume->Source2DTexture || Volume->Source2DTileSizeX != 0 || Volume->Source2DTileSizeY != 0) return false;
    TArray64<uint8> Expected, Actual;
    for (auto* Source : Sources) { TArray64<uint8> Pixels; if (!Source->Source.GetMipData(Pixels, 0)) return false; Expected.Append(Pixels); }
    if (!Volume->Source.GetMipData(Actual, 0) || Actual != Expected) return false;
    FAssetCompilingManager::Get().FinishAllCompilation(); FlushRenderingCommands();
    auto* Resource = Volume->GetResource();
    return Resource && Resource->TextureRHI.IsValid() && Resource->TextureRHI->GetDesc().Dimension == ETextureDimension::Texture3D
        && Resource->TextureRHI->GetDesc().Extent == FIntPoint(16, 8) && Resource->TextureRHI->GetDesc().Depth == Sources.Num();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208VolumeTextureWorkspaceTest,
    "UAI.Root.T208.VolumeTexture.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FUeAgentT208VolumeTextureWorkspaceTest::RunTest(const FString&)
{
    using namespace UE::UAI;
    FString Error;
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
    auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
        return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
            TEXT("t208-volume-texture"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
    };
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Base = TEXT("/Game/UAI/Transient/T208VolumeTexture/") + Token;
    const FString PackageName = Base / TEXT("Volume"), ObjectPath = PackageName + TEXT(".Volume");
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-volume-texture/native") / Token);
    TArray<TStrongObjectPtr<UTexture2D>> Owners;
    TArray<UTexture2D*> Sources;
    UVolumeTexture* Volume = nullptr;
    ON_SCOPE_EXIT {
        Volume = nullptr; Sources.Reset(); Owners.Reset(); FlushRenderingCommands();
        auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(Base)}); Args->SetBoolField(TEXT("recursive"), true);
        for (const bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
        for (const FString& Name : {TEXT("Volume"), TEXT("Slice0"), TEXT("Slice1"), TEXT("Slice2"), TEXT("Slice3")})
            TestFalse(TEXT("fixture removed"), FPackageName::DoesPackageExist(Base / Name));
    };
    TArray<TArray<uint8>> SourceFiles;
    for (int32 Slice = 0; Slice < 4; ++Slice) {
        const FString Name = FString::Printf(TEXT("Slice%d"), Slice);
        auto* Source = NewObject<UTexture2D>(CreatePackage(*(Base / Name)), *Name, RF_Public | RF_Standalone);
        Owners.Emplace(Source); Sources.Add(Source);
        TArray<FColor> Pixels; for (int32 Pixel = 0; Pixel < 128; ++Pixel) Pixels.Add(FColor(20 + Slice * 40, Pixel % 16, Pixel / 16, 255));
        Source->Source.Init(16, 8, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
        if (!SaveVolumeFixture(*Source)) return false;
        SourceFiles.AddDefaulted();
        if (!FFileHelper::LoadFileToArray(SourceFiles.Last(), *FPackageName::LongPackageNameToFilename(Base / Name, FPackageName::GetAssetPackageExtension()))) return false;
    }
    Volume = NewObject<UVolumeTexture>(CreatePackage(*PackageName), TEXT("Volume"), RF_Public | RF_Standalone);
    Volume->NeverStream = true; Volume->MipGenSettings = TMGS_NoMipmaps;
    Volume->CompressionSettings = TC_VectorDisplacementmap;
    Volume->Source2DTexture = Sources[0]; Volume->Source2DTileSizeX = 8; Volume->Source2DTileSizeY = 8;
    if (!TestTrue(TEXT("native legacy source fixture"), Volume->UpdateSourceFromSourceTexture()) || !SaveVolumeFixture(*Volume)) return false;
    TArray64<uint8> OriginalPixels, RestoredPixels;
    TArray<uint8> OriginalFile, ReadFile;
    if (!Volume->Source.GetMipData(OriginalPixels, 0) || !FFileHelper::LoadFileToArray(OriginalFile, *Filename)) return false;
    auto Input = [&](bool Invalid) {
        auto Value = MakeShared<FJsonObject>(); TArray<TSharedPtr<FJsonValue>> Slices;
        for (int32 Index = 0; Index < 4; ++Index) Slices.Add(Invalid && Index == 1 ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Sources[Index]->GetPathName())));
        Value->SetArrayField(TEXT("slices"), Slices); return Value;
    };
    auto Export = [&](const FString& Stage) {
        auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
        auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
    };
    if (!Export(TEXT("before-late"))) return false;
    // Observe the new state at the actual final-readback boundary, then reject it.
    UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId = TEXT("volume_texture"); Late.AssetPath = PackageName;
    Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
    Late.ProviderExtension = MakeShared<FJsonObject>(); Late.ProviderExtension->SetObjectField(TEXT("source_input"), Input(false));
    bool Published = false;
    Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
        Published = VolumeMatches(LoadObject<UVolumeTexture>(nullptr, *ObjectPath), Sources);
        Failure.Diagnostic.Code = TEXT("t208_volume_late_rejection"); return false;
    };
    Volume = nullptr; UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
    TestFalse(TEXT("late failure returned"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
    AddInfo(FString::Printf(TEXT("Volume late result: phase=%s code=%s detail=%s rollback_attempted=%d rollback_succeeded=%d"),
        *LateResult.Phase, *LateResult.Diagnostic.Code, *LateResult.Diagnostic.PublicMessage, LateResult.bRollbackAttempted, LateResult.bRollbackSucceeded));
    TestEqual(TEXT("late failure reached requested rejection boundary"), LateResult.Diagnostic.Code, FString(TEXT("t208_volume_late_rejection")));
    TestTrue(TEXT("late stage observed 3D source and cleared old source metadata"), Published);
    Volume = LoadObject<UVolumeTexture>(nullptr, *ObjectPath);
    TestTrue(TEXT("common recovery restores source metadata and bytes"), LateResult.bRollbackSucceeded && LateResult.Diagnostic.Code == TEXT("t208_volume_late_rejection")
        && Volume && Volume->Source2DTexture == Sources[0] && Volume->Source2DTileSizeX == 8 && Volume->Source2DTileSizeY == 8
        && Volume->Source.GetMipData(RestoredPixels, 0) && RestoredPixels == OriginalPixels
        && FFileHelper::LoadFileToArray(ReadFile, *Filename) && ReadFile == OriginalFile);
    auto Edit = [&](const FString& Stage, bool Invalid) {
        TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false); int32 Inputs = 0;
        for (const FString& File : Files) {
            if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
            FString Text; TSharedPtr<FJsonObject> Root;
            if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
            bool Changed = false;
            if (auto Row = FindVolumeField(Root, TEXT("source_input"))) { Row->SetObjectField(TEXT("value"), Input(Invalid)); ++Inputs; Changed = true; }
            if (!Invalid) {
                if (auto Row = FindVolumeField(Root, TEXT("SRGB"))) { Row->SetBoolField(TEXT("value"), false); Changed = true; }
                if (auto Row = FindVolumeField(Root, TEXT("CompressionSettings"))) { Row->SetStringField(TEXT("value"), TEXT("TC_VectorDisplacementmap")); Changed = true; }
            }
            if (Changed && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
                || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
        }
        return TestEqual(TEXT("one generated input field"), Inputs, 1);
    };
    auto Import = [&](const FString& Stage) {
        auto Args = MakeShared<FJsonObject>(), Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
        Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args);
    };
    if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), false)) return false;
    auto Applied = Import(TEXT("edit")); if (!Applied.IsOk()) { AddError(Applied.GetErrorJson()); return false; }
    Volume = LoadObject<UVolumeTexture>(nullptr, *ObjectPath);
    TestTrue(TEXT("public ordered non-square slices and native 3D resource"), VolumeMatches(Volume, Sources));
    TestTrue(TEXT("explicit format preserved"), Volume && !Volume->SRGB && Volume->CompressionSettings == TC_VectorDisplacementmap);
    if (!FFileHelper::LoadFileToArray(OriginalFile, *Filename) || !Export(TEXT("invalid")) || !Edit(TEXT("invalid"), true)) return false;
    TestFalse(TEXT("null slice rejected"), Import(TEXT("invalid")).IsOk());
    TestTrue(TEXT("rejected source and disk preserved"), VolumeMatches(LoadObject<UVolumeTexture>(nullptr, *ObjectPath), Sources)
        && FFileHelper::LoadFileToArray(ReadFile, *Filename) && ReadFile == OriginalFile);
    Volume = nullptr;
    if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
    TestTrue(TEXT("independent 3D source reload"), VolumeMatches(LoadObject<UVolumeTexture>(nullptr, *ObjectPath), Sources));
    for (int32 Slice = 0; Slice < 4; ++Slice)
        TestTrue(TEXT("source package preserved"), FFileHelper::LoadFileToArray(ReadFile, *FPackageName::LongPackageNameToFilename(Sources[Slice]->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension())) && SourceFiles[Slice] == ReadFile);
    return !HasAnyErrors();
}
