#include "Misc/AutomationTest.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"
#include "Vertical/T208/UeAgentT208WorkspaceValueEdit.h"

namespace
{
TSharedPtr<FJsonValue> SkeletalVectorValue(double X, double Y, double Z)
{
    return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
        MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(Y), MakeShared<FJsonValueNumber>(Z)});
}
TArray<FVector3f> SkeletalPositionsOf(USkeletalMesh& Mesh)
{
    TArray<FVector3f> Result;
    const auto* Model = Mesh.GetImportedModel();
    if (Model) for (const auto& Lod : Model->LODModels)
        for (const auto& Section : Lod.Sections)
            for (const auto& Vertex : Section.SoftVertices) Result.Add(Vertex.Position);
    return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208SkeletalMeshWorkspaceTest,
    "UAI.Root.T208.SkeletalMesh.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208SkeletalMeshWorkspaceTest::RunTest(const FString&)
{
    using namespace UE::UAI;
    using Tests::WorkspaceValueEdit::ReplaceNamedLeaf;
    FString Error;
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
    auto Submit = [&](const FString& Name, const TSharedRef<FJsonObject>& Args) {
        return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Name, Args,
            TEXT("t208-skeletal-mesh"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
    };
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Base = TEXT("/Game/UAI/Transient/T208SkeletalMesh/") + Token;
    const FString MeshPackage = Base / TEXT("Mesh"), MeshPath = MeshPackage + TEXT(".Mesh");
    const FString SkeletonPath = Base / TEXT("Skeleton.Skeleton"), LodPath = Base / TEXT("LodSettings.LodSettings");
    const FString Filename = FPackageName::LongPackageNameToFilename(MeshPackage, FPackageName::GetAssetPackageExtension());
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-skeletal-mesh/native") / Token);
    auto* Source = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    if (!TestNotNull(TEXT("real skinned source"), Source) || !TestNotNull(TEXT("source skeleton"), Source->GetSkeleton())) return false;
    FSkinnedAssetCompilingManager::Get().FinishCompilation({Source});
    auto* Mesh = DuplicateObject<USkeletalMesh>(Source, CreatePackage(*MeshPackage), TEXT("Mesh"));
    auto* Skeleton = DuplicateObject<USkeleton>(Source->GetSkeleton(), CreatePackage(*(Base / TEXT("Skeleton"))), TEXT("Skeleton"));
    auto* LodSettings = NewObject<USkeletalMeshLODSettings>(CreatePackage(*(Base / TEXT("LodSettings"))), TEXT("LodSettings"), RF_Public | RF_Standalone);
    if (!Mesh || !Skeleton || !LodSettings || !Mesh->GetLODInfo(0)) return false;
    ON_SCOPE_EXIT {
        Mesh = nullptr; Skeleton = nullptr; LodSettings = nullptr; Source = nullptr;
        auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(Base)}); Args->SetBoolField(TEXT("recursive"), true);
        for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto R = Submit(TEXT("asset.delete"), Args); if (!R.IsOk()) AddError(R.GetErrorJson()); }
        TestFalse(TEXT("mesh fixture removed"), FPackageName::DoesPackageExist(MeshPackage));
        TestFalse(TEXT("skeleton fixture removed"), FPackageName::DoesPackageExist(Base / TEXT("Skeleton")));
        TestFalse(TEXT("LOD settings fixture removed"), FPackageName::DoesPackageExist(Base / TEXT("LodSettings")));
    };
    Mesh->SetFlags(RF_Public | RF_Standalone); Skeleton->SetFlags(RF_Public | RF_Standalone);
    Mesh->SetLODSettings(nullptr);
    const float OldHysteresis = Mesh->GetLODInfo(0)->LODHysteresis;
    Mesh->GetLODInfo(0)->LODHysteresis = 0.375f;
    if (!TestTrue(TEXT("native LOD settings fixture contains existing mesh LODs"), LodSettings->SetLODSettingsFromMesh(Mesh) > 0)) return false;
    Mesh->GetLODInfo(0)->LODHysteresis = OldHysteresis;
    Mesh->PostEditChange(); FSkinnedAssetCompilingManager::Get().FinishCompilation({Mesh});
    const auto BeforePositions = SkeletalPositionsOf(*Mesh);
    const int32 BeforeLods = Mesh->GetLODNum(), BeforeBones = Mesh->GetRefSkeleton().GetRawBoneNum();
    const auto BeforeMaterials = Mesh->GetMaterials();
    if (!TestTrue(TEXT("real source contains geometry and materials"), !BeforePositions.IsEmpty() && !BeforeMaterials.IsEmpty())) return false;
    for (UObject* Asset : TArray<UObject*>{Mesh, Skeleton, LodSettings})
    {
        FAssetRegistryModule::AssetCreated(Asset);
        const FString File = FPackageName::LongPackageNameToFilename(Asset->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Asset->GetPackage(), Asset, *File, Save)) return false;
        FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({File}, true);
    }
    Mesh = nullptr; Skeleton = nullptr; LodSettings = nullptr; Source = nullptr;
    auto Export = [&](const FString& Stage) {
        auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), MeshPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
        auto R = Submit(TEXT("content_graph.export_workspace"), Args); if (!R.IsOk()) AddError(R.GetErrorJson()); return R.IsOk();
    };
    auto Import = [&](const FString& Stage) {
        auto Args = MakeShared<FJsonObject>(), Src = MakeShared<FJsonObject>(); Src->SetStringField(TEXT("kind"), TEXT("directory")); Src->SetStringField(TEXT("path"), Directory / Stage);
        Args->SetStringField(TEXT("target"), MeshPath); Args->SetObjectField(TEXT("source"), Src); return Submit(TEXT("content_graph.import_workspace"), Args);
    };
    auto Edit = [&](const FString& Stage, TFunctionRef<void(const TSharedPtr<FJsonValue>&)> Change) {
        TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("properties.hjson"), true, false);
        for (const auto& File : Files)
        {
            if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
            FString Text; TSharedPtr<FJsonObject> Root;
            if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) return false;
            Change(MakeShared<FJsonValueObject>(Root));
            if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
                || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
        }
        return true;
    };
    auto Matches = [&]() {
        auto* Actual = LoadObject<USkeletalMesh>(nullptr, *MeshPath); if (!Actual) return false;
        FSkinnedAssetCompilingManager::Get().FinishCompilation({Actual});
        const auto Imported = Actual->GetImportedBounds(), Extended = Actual->GetBounds();
        const auto& Materials = Actual->GetMaterials();
        const bool Good = Actual->GetSkeleton() && Actual->GetSkeleton()->GetPathName() == SkeletonPath
            && Actual->GetLODSettings() && Actual->GetLODSettings()->GetPathName() == LodPath
            && Actual->GetLODNum() == BeforeLods && Actual->GetLODInfo(0)->LODHysteresis == 0.375f
            && Actual->GetRefSkeleton().GetRawBoneNum() == BeforeBones && SkeletalPositionsOf(*Actual) == BeforePositions
            && Materials.Num() == BeforeMaterials.Num() && Materials[0].MaterialSlotName == TEXT("EditedSlot")
            && Materials[0].ImportedMaterialSlotName == BeforeMaterials[0].ImportedMaterialSlotName
            && Imported.Origin == FVector(10,20,30) && Imported.BoxExtent == FVector(60,70,80) && Imported.SphereRadius == 150
            && Actual->GetPositiveBoundsExtension() == FVector(2,4,6) && Actual->GetNegativeBoundsExtension() == FVector::ZeroVector
            && Extended.Origin == FVector(11,22,33) && Extended.BoxExtent == FVector(61,72,83);
        if (!Good) AddInfo(FString::Printf(TEXT("Skeletal readback: materials=%d lods=%d hysteresis=%g bones=%d positions=%d imported=%s/%s radius=%g extended=%s/%s"),
            Materials.Num(), Actual->GetLODNum(), Actual->GetLODInfo(0) ? Actual->GetLODInfo(0)->LODHysteresis : -1.f,
            Actual->GetRefSkeleton().GetRawBoneNum(), SkeletalPositionsOf(*Actual).Num(), *Imported.Origin.ToString(), *Imported.BoxExtent.ToString(),
            Imported.SphereRadius, *Extended.Origin.ToString(), *Extended.BoxExtent.ToString()));
        return Good;
    };
    if (!Export(TEXT("edit"))) return false;
    int32 Edited[8] = {};
    if (!Edit(TEXT("edit"), [&](const auto& V) {
        Edited[0] += ReplaceNamedLeaf(V,TEXT("PositiveBoundsExtension"),SkeletalVectorValue(2,4,6));
        Edited[1] += ReplaceNamedLeaf(V,TEXT("NegativeBoundsExtension"),SkeletalVectorValue(0,0,0));
        Edited[2] += ReplaceNamedLeaf(V,TEXT("MaterialSlotName"),MakeShared<FJsonValueString>(TEXT("EditedSlot")));
        Edited[3] += ReplaceNamedLeaf(V,TEXT("Origin"),SkeletalVectorValue(10,20,30));
        Edited[4] += ReplaceNamedLeaf(V,TEXT("BoxExtent"),SkeletalVectorValue(60,70,80));
        Edited[5] += ReplaceNamedLeaf(V,TEXT("SphereRadius"),MakeShared<FJsonValueNumber>(150));
        Edited[6] += ReplaceNamedLeaf(V,TEXT("lod_settings_ref"),MakeShared<FJsonValueString>(LodPath));
        Edited[7] += ReplaceNamedLeaf(V,TEXT("skeleton_ref"),MakeShared<FJsonValueString>(SkeletonPath));
    })) return false;
    for (int32 Index=0; Index<8; ++Index) if (!TestTrue(FString::Printf(TEXT("ordinary field %d was edited"),Index), Edited[Index]>0)) return false;
    const auto Applied=Import(TEXT("edit")); if (!Applied.IsOk()) { AddError(Applied.GetErrorJson()); return false; }
    if (!TestTrue(TEXT("combined edit native geometry, references, setter-derived LOD and local bounds"),Matches())) return false;
    if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(MeshPackage,Error)) { AddError(Error); return false; }
    if (!TestTrue(TEXT("independent native reload"),Matches())) return false;
    TArray<uint8> BeforeFile, AfterFile; if (!FFileHelper::LoadFileToArray(BeforeFile,*Filename)) return false;
    if (!Export(TEXT("invalid"))) return false;
    int32 Invalid=0;
    if (!Edit(TEXT("invalid"),[&](const auto& V) { Invalid+=ReplaceNamedLeaf(V,TEXT("LODInfo"),MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{})); })
        || !TestTrue(TEXT("fixed-size LOD array removal actually authored"),Invalid>0)) return false;
    const auto Rejected=Import(TEXT("invalid"));
    TestTrue(TEXT("LOD deletion rejected by common fixed-size rule"),!Rejected.IsOk() && Rejected.GetErrorJson().Contains(TEXT("edit_fixed_size_array_length_mismatch")));
    TestTrue(TEXT("invalid edit preserves native mesh and disk"),Matches() && FFileHelper::LoadFileToArray(AfterFile,*Filename) && BeforeFile==AfterFile);
    TSharedPtr<FJsonValue> LateState; bool Visible=false;
    const auto* Profile=UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("skeletal_mesh"));
    Mesh=LoadObject<USkeletalMesh>(nullptr,*MeshPath);
    if (!Profile || !UeAgentReflectionAuthoring::ProfileState::Read(*Profile,*Mesh,MeshPackage,LateState,Visible,Error)) return false;
    if (!TestTrue(TEXT("late bounds mutation authored"),ReplaceNamedLeaf(LateState,TEXT("Origin"),SkeletalVectorValue(-2,20,30))>0)) return false;
    UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId=TEXT("skeletal_mesh"); Late.AssetPath=MeshPackage; Late.bSave=true;
    Late.ProviderExtension=MakeShared<FJsonObject>(); Late.ProviderExtension->SetField(TEXT("mesh"),LateState);
    bool Published=false;
    Late.FinalReadback=[&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
        const auto* Actual=LoadObject<USkeletalMesh>(nullptr,*MeshPath); Published=Actual && Actual->GetImportedBounds().Origin.X==-2;
        Failure.Diagnostic.Code=TEXT("t208_skeletal_mesh_late_rejection"); return false;
    };
    Mesh=nullptr;
    UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
    TestFalse(TEXT("late failure returned"),UeAgentReflectionAuthoring::ApplyAsset(Late,LateResult));
    TestTrue(TEXT("actual publication and common rollback reached"),Published && LateResult.bRollbackSucceeded && LateResult.Diagnostic.Code==TEXT("t208_skeletal_mesh_late_rejection"));
    TestTrue(TEXT("late failure restores native mesh and exact disk"),Matches() && FFileHelper::LoadFileToArray(AfterFile,*Filename) && BeforeFile==AfterFile);
    return !HasAnyErrors();
}
