#include "Misc/AutomationTest.h"
#include "Vertical/T208/UeAgentT208WorkspaceValueEdit.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "MeshDescription.h"
#include "MeshDescriptionBuilder.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "PhysicsEngine/BodySetup.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
using UE::UAI::Tests::WorkspaceValueEdit::ReplaceNamedLeaf;
bool EditMeshSockets(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value) return false;
    if (Value->Type == EJson::Array) { for (const auto& Item : Value->AsArray()) if (EditMeshSockets(Item)) return true; }
    if (Value->Type != EJson::Object) return false;
    const auto Object = Value->AsObject();
    if (Object->HasField(TEXT("KeepSocket")) && Object->HasField(TEXT("RemoveSocket")))
    {
        Object->RemoveField(TEXT("RemoveSocket"));
        Object->SetObjectField(TEXT("AddedSocket"), MakeShared<FJsonObject>());
        return ReplaceNamedLeaf(Object->TryGetField(TEXT("KeepSocket")), TEXT("Tag"), MakeShared<FJsonValueString>(TEXT("edited"))) == 1;
    }
    for (const auto& Field : Object->Values) if (EditMeshSockets(Field.Value)) return true;
    return false;
}
bool MeshStateMatches(UStaticMesh* Mesh)
{
    if (!Mesh) return false;
    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
    const auto* Geometry = Mesh->GetMeshDescription(0);
    const auto* Socket = Mesh->FindSocket(TEXT("KeepSocket"));
    TSet<FVector3f> Positions;
    if (Geometry)
    {
        const auto VertexPositions = Geometry->GetVertexPositions();
        for (const auto Vertex : Geometry->Vertices().GetElementIDs()) Positions.Add(VertexPositions[Vertex]);
    }
    const bool Matches = Mesh->bAllowCPUAccess && Mesh->GetNumSourceModels() == 1 && Geometry && Geometry->Vertices().Num() == 3
        && Positions.Num() == 3 && Positions.Contains(FVector3f::ZeroVector)
        && Positions.Contains(FVector3f(100, 0, 0)) && Positions.Contains(FVector3f(0, 100, 0))
        && Geometry->Triangles().Num() == 1 && Mesh->GetStaticMaterials().Num() == 1
        && Mesh->GetStaticMaterials()[0].MaterialSlotName == TEXT("EditedSlot")
        && Mesh->GetStaticMaterials()[0].ImportedMaterialSlotName == TEXT("Original")
        && Mesh->GetBodySetup() && Mesh->GetBodySetup()->CollisionTraceFlag == CTF_UseSimpleAsComplex
        && Mesh->GetBodySetup()->AggGeom.BoxElems.Num() == 1 && Mesh->GetBodySetup()->AggGeom.BoxElems[0].X == 10
        && Mesh->GetSourceModel(0).BuildSettings.bComputeWeightedNormals
        && Socket && Socket->Tag == TEXT("edited") && Socket->bSocketCreatedAtImport
        && Socket->RelativeLocation == FVector(1, 2, 3) && Mesh->Sockets.Num() == 2
        && Mesh->FindSocket(TEXT("AddedSocket")) && !Mesh->FindSocket(TEXT("RemoveSocket"));
    if (!Matches)
    {
        const auto& Materials = Mesh->GetStaticMaterials();
        const auto* Body = Mesh->GetBodySetup();
        UE_LOG(LogTemp, Display, TEXT("T208 StaticMesh mismatch: cpu=%d lods=%d vertices=%d triangles=%d positions=%d/%d/%d/%d materials=%d slot=%s imported=%s collision=%d boxes=%d box_x=%g weighted=%d sockets=%d keep=%d tag=%s imported_socket=%d location=%s added=%d removed=%d"),
            Mesh->bAllowCPUAccess, Mesh->GetNumSourceModels(), Geometry ? Geometry->Vertices().Num() : -1,
            Geometry ? Geometry->Triangles().Num() : -1, Positions.Num(), Positions.Contains(FVector3f::ZeroVector),
            Positions.Contains(FVector3f(100, 0, 0)), Positions.Contains(FVector3f(0, 100, 0)), Materials.Num(),
            Materials.IsEmpty() ? TEXT("missing") : *Materials[0].MaterialSlotName.ToString(),
            Materials.IsEmpty() ? TEXT("missing") : *Materials[0].ImportedMaterialSlotName.ToString(),
            Body ? int32(Body->CollisionTraceFlag) : -1, Body ? Body->AggGeom.BoxElems.Num() : -1,
            Body && !Body->AggGeom.BoxElems.IsEmpty() ? Body->AggGeom.BoxElems[0].X : -1.0,
            Mesh->GetNumSourceModels() ? Mesh->GetSourceModel(0).BuildSettings.bComputeWeightedNormals : false,
            Mesh->Sockets.Num(), Socket != nullptr, Socket ? *Socket->Tag : TEXT("missing"),
            Socket ? Socket->bSocketCreatedAtImport : false, Socket ? *Socket->RelativeLocation.ToString() : TEXT("missing"),
            Mesh->FindSocket(TEXT("AddedSocket")) != nullptr, Mesh->FindSocket(TEXT("RemoveSocket")) != nullptr);
    }
    return Matches;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208StaticMeshWorkspaceTest,
    "UAI.Root.T208.StaticMesh.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208StaticMeshWorkspaceTest::RunTest(const FString&)
{
    using namespace UE::UAI;
    FString Error;
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
    auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
        return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
            TEXT("t208-static-mesh"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
    };
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Base = TEXT("/Game/UAI/Transient/T208StaticMesh/") + Token;
    const FString PackageName = Base / TEXT("Mesh"), ObjectPath = PackageName + TEXT(".Mesh");
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-static-mesh/native") / Token);
    UStaticMesh* Mesh = NewObject<UStaticMesh>(CreatePackage(*PackageName), TEXT("Mesh"), RF_Public | RF_Standalone | RF_Transactional);
    ON_SCOPE_EXIT {
        Mesh = nullptr;
        auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(Base)}); Args->SetBoolField(TEXT("recursive"), true);
        for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
        TestFalse(TEXT("fixture removed"), FPackageName::DoesPackageExist(PackageName));
    };
    Mesh->CreateBodySetup(); Mesh->AddSourceModel();
    FMeshDescription Geometry; FStaticMeshAttributes Attributes(Geometry); Attributes.Register();
    FMeshDescriptionBuilder Builder; Builder.SetMeshDescription(&Geometry); Builder.SetNumUVLayers(1);
    const auto Group = Builder.AppendPolygonGroup(TEXT("Original"));
    Builder.AppendTriangle(Builder.AppendVertex(FVector::ZeroVector), Builder.AppendVertex(FVector(100, 0, 0)), Builder.AppendVertex(FVector(0, 100, 0)), Group);
    Mesh->CreateMeshDescription(0, Geometry); Mesh->CommitMeshDescription(0);
    Mesh->SetStaticMaterials({FStaticMaterial(nullptr, TEXT("Original"), TEXT("Original"))});
    auto& Build = Mesh->GetSourceModel(0).BuildSettings;
    Build.bRecomputeNormals = true; Build.bRecomputeTangents = true; Build.bGenerateLightmapUVs = false; Build.bUseMikkTSpace = false;
    FKBoxElem Box; Box.X = 10; Box.Y = 20; Box.Z = 30; Mesh->GetBodySetup()->AggGeom.BoxElems.Add(Box);
    for (const TCHAR* Name : {TEXT("KeepSocket"), TEXT("RemoveSocket")})
    {
        auto* Socket = NewObject<UStaticMeshSocket>(Mesh, NAME_None, RF_Transactional);
        Socket->SocketName = Name; Socket->RelativeLocation = FVector(1, 2, 3); Socket->bSocketCreatedAtImport = true; Mesh->Sockets.Add(Socket);
    }
    Mesh->PostEditChange(); FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
    FAssetRegistryModule::AssetCreated(Mesh);
    FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Mesh->GetPackage(), Mesh, *Filename, Save)) return false;
    FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({Filename}, true);
    auto Export = [&](const FString& Stage) {
        auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
        auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
    };
    auto Import = [&](const FString& Stage) {
        auto Args = MakeShared<FJsonObject>(), Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
        Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args);
    };
    if (!Export(TEXT("edit"))) return false;
    TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / TEXT("edit")), TEXT("*.hjson"), true, false);
    int32 RootEdits = 0, MaterialEdits = 0, CollisionEdits = 0, BuildEdits = 0, SocketEdits = 0;
    for (const auto& File : Files)
    {
        if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
        if (FPaths::GetCleanFilename(File) != TEXT("properties.hjson")) continue;
        FString Text; TSharedPtr<FJsonObject> Root;
        if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
        auto Value = MakeShared<FJsonValueObject>(Root);
        RootEdits += ReplaceNamedLeaf(Value, TEXT("bAllowCPUAccess"), MakeShared<FJsonValueBoolean>(true));
        MaterialEdits += ReplaceNamedLeaf(Value, TEXT("MaterialSlotName"), MakeShared<FJsonValueString>(TEXT("EditedSlot")));
        CollisionEdits += ReplaceNamedLeaf(Value, TEXT("CollisionTraceFlag"), MakeShared<FJsonValueString>(TEXT("CTF_UseSimpleAsComplex")));
        BuildEdits += ReplaceNamedLeaf(Value, TEXT("bComputeWeightedNormals"), MakeShared<FJsonValueBoolean>(true));
        SocketEdits += EditMeshSockets(Value) ? 1 : 0;
        if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
            || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
    }
    if (!TestTrue(TEXT("all five ordinary surfaces edited"), RootEdits > 0 && MaterialEdits > 0 && CollisionEdits > 0 && BuildEdits > 0 && SocketEdits == 1))
    { AddInfo(FString::Printf(TEXT("root=%d materials=%d collision=%d build=%d sockets=%d"), RootEdits, MaterialEdits, CollisionEdits, BuildEdits, SocketEdits)); return false; }
    Mesh = nullptr;
    auto Applied = Import(TEXT("edit")); if (!Applied.IsOk()) { AddError(Applied.GetErrorJson()); return false; }
    TestTrue(TEXT("combined root and native structure with non-target preservation"), MeshStateMatches(LoadObject<UStaticMesh>(nullptr, *ObjectPath)));
    if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
    TestTrue(TEXT("independent geometry and structure reload"), MeshStateMatches(LoadObject<UStaticMesh>(nullptr, *ObjectPath)));
    if (!Export(TEXT("readback"))) return false;
    TArray<uint8> BeforeFile, AfterFile;
    if (!FFileHelper::LoadFileToArray(BeforeFile, *Filename)) return false;
    Files.Reset(); IFileManager::Get().FindFilesRecursive(Files, *(Directory / TEXT("readback")), TEXT("properties.hjson"), true, false);
    int32 InvalidFields = 0;
    for (const auto& File : Files)
    {
        if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
        FString Text; TSharedPtr<FJsonObject> Root;
        if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) return false;
        const int32 Changed = ReplaceNamedLeaf(MakeShared<FJsonValueObject>(Root), TEXT("Tag"), MakeShared<FJsonValueNumber>(123));
        InvalidFields += Changed;
        if (Changed && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
            || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
    }
    if (!TestTrue(TEXT("invalid structural value actually authored"), InvalidFields > 0)) return false;
    TestFalse(TEXT("public invalid socket value rejected"), Import(TEXT("readback")).IsOk());
    TestTrue(TEXT("invalid edit preserves complete mesh and disk"), MeshStateMatches(LoadObject<UStaticMesh>(nullptr, *ObjectPath))
        && FFileHelper::LoadFileToArray(AfterFile, *Filename) && BeforeFile == AfterFile);
    TSharedPtr<FJsonValue> LateState;
    bool AuthoringVisible = false;
    Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
    const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("static_mesh"));
    if (!Profile || !UeAgentReflectionAuthoring::ProfileState::Read(*Profile, *Mesh, PackageName, LateState, AuthoringVisible, Error))
    { AddError(Error); return false; }
    if (!TestTrue(TEXT("late input changes socket settings"), ReplaceNamedLeaf(LateState, TEXT("Tag"), MakeShared<FJsonValueString>(TEXT("late"))) > 0)) return false;
    UeAgentReflectionAuthoring::FAssetApplyRequest Late;
    Late.TypeId = TEXT("static_mesh"); Late.AssetPath = PackageName; Late.bSave = true; Late.bEmitEditorNotifications = true;
    Late.ProviderExtension = MakeShared<FJsonObject>(); Late.ProviderExtension->SetField(TEXT("mesh"), LateState);
    bool Published = false;
    Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
        const auto* PublishedMesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
        const auto* Socket = PublishedMesh ? PublishedMesh->FindSocket(TEXT("KeepSocket")) : nullptr;
        Published = Socket && Socket->Tag == TEXT("late");
        Failure.ErrorCode = TEXT("t208_static_mesh_late_rejection"); return false;
    };
    Mesh = nullptr;
    UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
    TestFalse(TEXT("late rejection returned"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
    AddInfo(FString::Printf(TEXT("StaticMesh late: phase=%s code=%s detail=%s rollback=%d"),
        *LateResult.Phase, *LateResult.ErrorCode, *LateResult.ErrorDetail, LateResult.bRollbackSucceeded));
    TestTrue(TEXT("late actual publication observed and common recovery confirmed"), Published
        && LateResult.ErrorCode == TEXT("t208_static_mesh_late_rejection") && LateResult.bRollbackSucceeded);
    TestTrue(TEXT("late rejection restores geometry, structures and exact disk"), MeshStateMatches(LoadObject<UStaticMesh>(nullptr, *ObjectPath))
        && FFileHelper::LoadFileToArray(AfterFile, *Filename) && BeforeFile == AfterFile);
    return !HasAnyErrors();
}
