#include "Misc/AutomationTest.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
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
const TCHAR* FaceNames[] = {TEXT("positive_x"), TEXT("negative_x"), TEXT("positive_y"), TEXT("negative_y"), TEXT("positive_z"), TEXT("negative_z")};
bool SaveCubeFixture(UObject& Object)
{
    FAssetRegistryModule::AssetCreated(&Object);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    const FString Filename = FPackageName::LongPackageNameToFilename(Object.GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Object.GetPackage(), &Object, *Filename, Args)) return false;
    FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({Filename}, true);
    return true;
}
TSharedPtr<FJsonObject> FindCubeField(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
    for (const auto& Pair : Object->Values)
    {
        if (Pair.Value->Type != EJson::Object) continue;
        auto Child = Pair.Value->AsObject();
        if (Pair.Key == Name && Child->HasField(TEXT("value"))) return Child;
        if (auto Found = FindCubeField(Child, Name)) return Found;
    }
    return nullptr;
}
bool CubeMatches(UTextureCube* Cube, const TArray<UTexture2D*>& Sources)
{
    if (!Cube || !Cube->Source.IsValid() || Cube->Source.GetNumSlices() != 6 || Cube->Source.GetSizeX() != 16
        || Cube->Source.GetSizeY() != 16 || Cube->Source.GetFormat() != TSF_BGRA8) return false;
    TArray64<uint8> Expected, Actual;
    for (UTexture2D* Source : Sources) { TArray64<uint8> Pixels; if (!Source->Source.GetMipData(Pixels, 0)) return false; Expected.Append(Pixels); }
    if (!Cube->Source.GetMipData(Actual, 0) || Actual != Expected) return false;
    FAssetCompilingManager::Get().FinishAllCompilation(); FlushRenderingCommands();
    auto* Resource = Cube->GetResource();
    return Resource && Resource->TextureRHI.IsValid() && Resource->TextureRHI->GetDesc().Dimension == ETextureDimension::TextureCube
        && Resource->TextureRHI->GetDesc().Extent == FIntPoint(16, 16);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208TextureCubeWorkspaceTest,
    "UAI.Root.T208.TextureCube.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FUeAgentT208TextureCubeWorkspaceTest::RunTest(const FString&)
{
    using namespace UE::UAI;
    FString Error;
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
    auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
        return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
            TEXT("t208-texture-cube"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
    };
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Base = TEXT("/Game/UAI/Transient/T208TextureCube/") + Token;
    const FString PackageName = Base / TEXT("Cube"), ObjectPath = PackageName + TEXT(".Cube");
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-texture-cube/native") / Token);
    TArray<TStrongObjectPtr<UTexture2D>> Owners;
    TArray<UTexture2D*> Sources;
    UTextureCube* Cube = nullptr;
    ON_SCOPE_EXIT {
        Cube = nullptr; Sources.Reset(); Owners.Reset(); FlushRenderingCommands();
        auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(Base)}); Args->SetBoolField(TEXT("recursive"), true);
        for (const bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
        for (const FString& Name : {TEXT("Cube"), TEXT("Face0"), TEXT("Face1"), TEXT("Face2"), TEXT("Face3"), TEXT("Face4"), TEXT("Face5")})
            TestFalse(TEXT("fixture removed"), FPackageName::DoesPackageExist(Base / Name));
    };
    TArray<TArray<uint8>> SourceFiles;
    for (int32 Face = 0; Face < 6; ++Face) {
        const FString Name = FString::Printf(TEXT("Face%d"), Face);
        auto* Source = NewObject<UTexture2D>(CreatePackage(*(Base / Name)), *Name, RF_Public | RF_Standalone);
        Owners.Emplace(Source); Sources.Add(Source);
        TArray<FColor> Pixels; for (int32 Pixel = 0; Pixel < 256; ++Pixel) Pixels.Add(FColor(20 + Face * 30, Pixel % 16, Pixel / 16, 255));
        Source->Source.Init(16, 16, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
        if (!SaveCubeFixture(*Source)) return false;
        SourceFiles.AddDefaulted();
        if (!FFileHelper::LoadFileToArray(SourceFiles.Last(), *FPackageName::LongPackageNameToFilename(Base / Name, FPackageName::GetAssetPackageExtension()))) return false;
    }
    Cube = NewObject<UTextureCube>(CreatePackage(*PackageName), TEXT("Cube"), RF_Public | RF_Standalone);
    Cube->NeverStream = true; Cube->MipGenSettings = TMGS_NoMipmaps;
    if (!SaveCubeFixture(*Cube)) return false;
    auto Export = [&](const FString& Stage) {
        auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
        auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
    };
    auto Input = [&](bool Reverse, bool Invalid) {
        auto Value = MakeShared<FJsonObject>();
        for (int32 Face = 0; Face < 6; ++Face) {
            if (Invalid && Face == 2) Value->SetField(FaceNames[Face], MakeShared<FJsonValueNull>());
            else Value->SetStringField(FaceNames[Face], Sources[Reverse ? 5 - Face : Face]->GetPathName());
        }
        return Value;
    };
    auto Edit = [&](const FString& Stage, const TSharedRef<FJsonObject>& InputValue, bool Format) {
        TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
        int32 Inputs = 0;
        for (const FString& File : Files) {
            if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
            FString Text; TSharedPtr<FJsonObject> Root;
            if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
            bool Changed = false;
            if (auto Row = FindCubeField(Root, TEXT("source_input"))) { Row->SetObjectField(TEXT("value"), InputValue); ++Inputs; Changed = true; }
            if (Format) {
                if (auto Row = FindCubeField(Root, TEXT("SRGB"))) { Row->SetBoolField(TEXT("value"), false); Changed = true; }
                if (auto Row = FindCubeField(Root, TEXT("CompressionSettings"))) { Row->SetStringField(TEXT("value"), TEXT("TC_Masks")); Changed = true; }
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
    if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), Input(false, false), true)) return false;
    auto Applied = Import(TEXT("edit"));
    if (!Applied.IsOk()) { AddError(Applied.GetErrorJson()); return false; }
    Cube = LoadObject<UTextureCube>(nullptr, *ObjectPath);
    TestTrue(TEXT("public six-face source and native cube RHI"), CubeMatches(Cube, Sources));
    TestTrue(TEXT("explicit properties and generation agree"), Cube && !Cube->SRGB && Cube->CompressionSettings == TC_Masks);
    if (!Export(TEXT("readback"))) return false;
    TArray<FString> ReadFiles; IFileManager::Get().FindFilesRecursive(ReadFiles, *(Directory / TEXT("readback")), TEXT("*.hjson"), true, false);
    for (const FString& File : ReadFiles) {
        if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
        FString Text; if (!FFileHelper::LoadFileToString(Text, *File)) return false;
        for (auto* Source : Sources) TestFalse(TEXT("generation references not persisted in authoring view"), Text.Contains(Source->GetPathName()));
        TestFalse(TEXT("internal source digest not exposed"), Text.Contains(TEXT("top_mip_sha1")));
    }
    TArray<uint8> Before, After; if (!FFileHelper::LoadFileToArray(Before, *Filename)) return false;
    if (!Export(TEXT("invalid")) || !Edit(TEXT("invalid"), Input(false, true), false)) return false;
    TestFalse(TEXT("incomplete source rejected"), Import(TEXT("invalid")).IsOk());
    Cube = LoadObject<UTextureCube>(nullptr, *ObjectPath);
    TestTrue(TEXT("rejection preserves source and disk"), CubeMatches(Cube, Sources) && FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
    UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId = TEXT("texture_cube"); Late.AssetPath = PackageName;
    Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
    Late.ProviderExtension = MakeShared<FJsonObject>(); Late.ProviderExtension->SetObjectField(TEXT("source_input"), Input(true, false));
    TArray<UTexture2D*> Reverse; for (int32 Face = 5; Face >= 0; --Face) Reverse.Add(Sources[Face]);
    bool Published = false;
    Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
        Published = CubeMatches(LoadObject<UTextureCube>(nullptr, *ObjectPath), Reverse);
        Failure.Diagnostic.Code = TEXT("t208_cube_late_rejection"); return false;
    };
    Cube = nullptr; UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
    TestFalse(TEXT("late failure returned"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
    TestTrue(TEXT("late stage observed generated replacement"), Published);
    Cube = LoadObject<UTextureCube>(nullptr, *ObjectPath);
    TestTrue(TEXT("common recovery restores source and disk"), LateResult.bRollbackSucceeded && LateResult.Diagnostic.Code == TEXT("t208_cube_late_rejection")
        && CubeMatches(Cube, Sources) && FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
    Cube = nullptr;
    if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
    TestTrue(TEXT("independent reload"), CubeMatches(LoadObject<UTextureCube>(nullptr, *ObjectPath), Sources));
    for (int32 Face = 0; Face < 6; ++Face)
        TestTrue(TEXT("non-target source package preserved"), FFileHelper::LoadFileToArray(After, *FPackageName::LongPackageNameToFilename(Sources[Face]->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension())) && SourceFiles[Face] == After);
    return !HasAnyErrors();
}
