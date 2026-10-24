#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"
#include "Vertical/T208/UeAgentT208WorkspaceValueEdit.h"

namespace
{
TSharedPtr<FJsonObject> PhysicsValue(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value) return nullptr;
    if (Value->Type == EJson::Array)
    {
        for (const auto& Item : Value->AsArray()) if (auto Found = PhysicsValue(Item)) return Found;
    }
    if (Value->Type != EJson::Object) return nullptr;
    const auto Object = Value->AsObject();
    if (Object->HasField(TEXT("bodies")) && Object->HasField(TEXT("constraints"))) return Object;
    for (const auto& Field : Object->Values) if (auto Found = PhysicsValue(Field.Value)) return Found;
    return nullptr;
}
TSharedPtr<FJsonObject> ClonePhysicsRecord(const TSharedPtr<FJsonObject>& Object)
{
    return UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(Object))->AsObject();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208PhysicsAssetWorkspaceTest,
    "UAI.Root.T208.PhysicsAsset.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208PhysicsAssetWorkspaceTest::RunTest(const FString&)
{
    using namespace UE::UAI;
    using Tests::WorkspaceValueEdit::ReplaceNamedLeaf;
    FString Error;
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
    auto Submit = [&](const FString& Name, const TSharedRef<FJsonObject>& Args) {
        return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Name, Args,
            TEXT("t208-physics-asset"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
    };
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackagePath = TEXT("/Game/UAI/Transient/T208PhysicsAsset/") + Token + TEXT("/Physics");
    const FString ObjectPath = PackagePath + TEXT(".Physics");
    const FString Filename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-physics-asset/native") / Token);
    auto* Asset = NewObject<UPhysicsAsset>(CreatePackage(*PackagePath), TEXT("Physics"), RF_Public | RF_Standalone);
    ON_SCOPE_EXIT {
        Asset = nullptr;
        auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)});
        for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); auto R = Submit(TEXT("asset.delete"), Args); if (!R.IsOk()) AddError(R.GetErrorJson()); }
        TestFalse(TEXT("fixture removed"), FPackageName::DoesPackageExist(PackagePath));
    };
    Asset->PhysicalAnimationProfiles = {TEXT("Drive")}; Asset->ConstraintProfiles = {TEXT("Drive")};
    for (const TCHAR* Name : {TEXT("Root"), TEXT("Tip"), TEXT("Remove")})
    {
        auto* Body = NewObject<USkeletalBodySetup>(Asset, FName(Name), RF_Transactional);
        Body->BoneName = Name; Body->bConsiderForBounds = true; Body->AggGeom.SphereElems.AddDefaulted_GetRef().Radius = 10;
        Body->AddPhysicalAnimationProfile(TEXT("Drive"));
        Body->FindPhysicalAnimationProfile(TEXT("Drive"))->PhysicalAnimationData.OrientationStrength = 11;
        Asset->SkeletalBodySetups.Add(Body);
    }
    auto* Joint = NewObject<UPhysicsConstraintTemplate>(Asset, TEXT("Joint"), RF_Transactional);
    Joint->DefaultInstance.JointName = TEXT("Joint");
    Joint->DefaultInstance.ConstraintBone1 = TEXT("Root"); Joint->DefaultInstance.ConstraintBone2 = TEXT("Tip");
    Joint->DefaultInstance.SetRefFrame(EConstraintFrame::Frame1, FTransform(FVector(1,2,3)));
    Joint->DefaultInstance.SetLinearXMotion(LCM_Limited); Joint->DefaultInstance.SetLinearLimitSize(15);
    Joint->SetDefaultProfile(Joint->DefaultInstance); Joint->AddConstraintProfile(TEXT("Drive"));
    Joint->ProfileHandles[0].ProfileProperties.LinearLimit.Limit = 90;
    Asset->ConstraintSetup.Add(Joint); Asset->EditorBodyFlags = {7,9,13};
    Asset->CollisionDisableTable.Add(FRigidBodyIndexPair(0,1), true);
    Asset->UpdateBodySetupIndexMap(); Asset->UpdateBoundsBodiesArray();
    FAssetRegistryModule::AssetCreated(Asset);
    FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Asset->GetPackage(), Asset, *Filename, Save)) return false;
    FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({Filename}, true);
    // Authoring must export stored defaults independently of the active editor selection.
    Asset->CurrentConstraintProfileName = TEXT("Drive"); Joint->ApplyConstraintProfile(TEXT("Drive"), Joint->DefaultInstance, true);
    Asset = nullptr; Joint = nullptr;
    auto Export = [&](const FString& Stage) {
        auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
        Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
        auto R = Submit(TEXT("content_graph.export_workspace"), Args); if (!R.IsOk()) AddError(R.GetErrorJson()); return R.IsOk();
    };
    auto Import = [&](const FString& Stage) {
        auto Args = MakeShared<FJsonObject>(), Source = MakeShared<FJsonObject>();
        Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
        Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
        return Submit(TEXT("content_graph.import_workspace"), Args);
    };
    auto Edit = [&](const FString& Stage, TFunctionRef<void(const TSharedPtr<FJsonValue>&)> Change) {
        TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("properties.hjson"), true, false);
        for (const auto& File : Files)
        {
            if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
            FString Text; TSharedPtr<FJsonObject> Root;
            if (!FFileHelper::LoadFileToString(Text,*File) || !FUeAgentStructuredHjsonParser::ParseObject(Text,File,Root,Error)) return false;
            Change(MakeShared<FJsonValueObject>(Root));
            if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(),Text,Error)
                || !FFileHelper::SaveStringToFile(Text,*File,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
        }
        return true;
    };
    if (!Export(TEXT("edit"))) return false;
    int32 RootEdits=0, StructureEdits=0, RadiusEdits=0, ProfileEdits=0, LimitEdits=0;
    if (!Edit(TEXT("edit"), [&](const auto& Value) {
        RootEdits += ReplaceNamedLeaf(Value,TEXT("bNotForDedicatedServer"),MakeShared<FJsonValueBoolean>(true));
        auto Physics = PhysicsValue(Value); if (!Physics) return;
        auto Bodies = Physics->GetObjectField(TEXT("bodies"));
        auto Root = Bodies->GetObjectField(TEXT("Root"));
        RadiusEdits += ReplaceNamedLeaf(Root->GetField<EJson::Object>(TEXT("settings")), TEXT("Radius"), MakeShared<FJsonValueNumber>(22));
        ProfileEdits += ReplaceNamedLeaf(Root->GetField<EJson::Object>(TEXT("profiles")), TEXT("OrientationStrength"), MakeShared<FJsonValueNumber>(17));
        Bodies->RemoveField(TEXT("Remove")); Bodies->SetObjectField(TEXT("Extra"), ClonePhysicsRecord(Root));
        auto Constraints = Physics->GetObjectField(TEXT("constraints")); auto JointRecord = Constraints->GetObjectField(TEXT("Joint"));
        LimitEdits += ReplaceNamedLeaf(JointRecord->GetField<EJson::Object>(TEXT("settings")), TEXT("Limit"), MakeShared<FJsonValueNumber>(25));
        auto Added = ClonePhysicsRecord(JointRecord);
        ReplaceNamedLeaf(Added->GetField<EJson::Object>(TEXT("settings")),TEXT("ConstraintBone2"),MakeShared<FJsonValueString>(TEXT("Extra")));
        Constraints->SetObjectField(TEXT("ExtraJoint"), Added); ++StructureEdits;
    })) return false;
    if (!TestTrue(TEXT("ordinary root and structure edits authored"),RootEdits>0 && StructureEdits==1 && RadiusEdits>0 && ProfileEdits>0 && LimitEdits>0)) return false;
    const auto Applied=Import(TEXT("edit")); if (!Applied.IsOk()) { AddError(Applied.GetErrorJson()); return false; }
    auto Matches = [&]() {
        auto* Actual=LoadObject<UPhysicsAsset>(nullptr,*ObjectPath); if (!Actual) return false;
        const int32 RootIndex=Actual->FindBodyIndex(TEXT("Root")), TipIndex=Actual->FindBodyIndex(TEXT("Tip"));
        const int32 ExtraIndex=Actual->FindBodyIndex(TEXT("Extra")), JointIndex=Actual->FindConstraintIndex(TEXT("Joint"));
        if (RootIndex<0 || TipIndex<0 || ExtraIndex<0 || JointIndex<0) return false;
        auto* Body=Actual->SkeletalBodySetups[RootIndex].Get(); auto* Constraint=Actual->ConstraintSetup[JointIndex].Get();
        Body->CreatePhysicsMeshes();
        return Actual->bNotForDedicatedServer && Actual->SkeletalBodySetups.Num()==3 && Actual->ConstraintSetup.Num()==2
            && Body->bCreatedPhysicsMeshes && !Body->bFailedToCreatePhysicsMeshes
            && Actual->FindBodyIndex(TEXT("Remove"))==INDEX_NONE && RootIndex==0 && TipIndex==1
            && Body->AggGeom.SphereElems.Num()==1 && Body->AggGeom.SphereElems[0].Radius==22
            && Body->FindPhysicalAnimationProfile(TEXT("Drive"))->PhysicalAnimationData.OrientationStrength==17
            && Constraint->GetConstraintProfilePropertiesOrDefault(NAME_None).LinearLimit.Limit==25
            && Constraint->GetConstraintProfilePropertiesOrDefault(TEXT("Drive")).LinearLimit.Limit==90
            && Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame1).GetLocation()==FVector(1,2,3)
            && Actual->CollisionDisableTable.Contains(FRigidBodyIndexPair(RootIndex,TipIndex))
            && Actual->EditorBodyFlags==TArray<int32>({7,9,0}) && Actual->BoundsBodies.Num()==3;
    };
    if (!TestTrue(TEXT("native structure, profiles, local constraint frame and caches"),Matches())) return false;
    if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackagePath,Error)) { AddError(Error); return false; }
    if (!TestTrue(TEXT("independent reload preserves stored defaults and named profiles"),Matches())) return false;
    TArray<uint8> BeforeFile, AfterFile; if (!FFileHelper::LoadFileToArray(BeforeFile,*Filename)) return false;
    if (!Export(TEXT("invalid"))) return false;
    int32 InvalidEdits=0;
    if (!Edit(TEXT("invalid"),[&](const auto& Value) {
        InvalidEdits+=ReplaceNamedLeaf(Value,TEXT("ConstraintBone2"),MakeShared<FJsonValueString>(TEXT("MissingBody")));
    }) || !TestTrue(TEXT("invalid association authored"),InvalidEdits>0)) return false;
    const auto Rejected=Import(TEXT("invalid"));
    TestTrue(TEXT("dangling constraint refused"),!Rejected.IsOk() && Rejected.GetErrorJson().Contains(TEXT("physics_constraint_body_missing")));
    TestTrue(TEXT("invalid association preserves native state and disk"),Matches() && FFileHelper::LoadFileToArray(AfterFile,*Filename) && BeforeFile==AfterFile);
    const auto* Profile=UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("physics_asset"));
    TSharedPtr<FJsonValue> LateState; bool Visible=false; Asset=LoadObject<UPhysicsAsset>(nullptr,*ObjectPath);
    if (!Profile || !UeAgentReflectionAuthoring::ProfileState::Read(*Profile,*Asset,PackagePath,LateState,Visible,Error)) return false;
    ReplaceNamedLeaf(LateState,TEXT("Radius"),MakeShared<FJsonValueNumber>(31));
    UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId=TEXT("physics_asset"); Late.AssetPath=PackagePath; Late.bSave=true;
    Late.ProviderExtension=MakeShared<FJsonObject>(); Late.ProviderExtension->SetField(TEXT("physics"),LateState);
    bool Published=false;
    Late.FinalReadback=[&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
        auto* Actual=LoadObject<UPhysicsAsset>(nullptr,*ObjectPath);
        Published=Actual && Actual->SkeletalBodySetups[Actual->FindBodyIndex(TEXT("Root"))]->AggGeom.SphereElems[0].Radius==31;
        Failure.Diagnostic.Code=TEXT("t208_physics_late_rejection"); return false;
    };
    Asset=nullptr;
    UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
    TestFalse(TEXT("late rejection returned"),UeAgentReflectionAuthoring::ApplyAsset(Late,LateResult));
    TestTrue(TEXT("published state and common rollback reached"),Published && LateResult.bRollbackSucceeded && LateResult.Diagnostic.Code==TEXT("t208_physics_late_rejection"));
    TestTrue(TEXT("late rejection restores native state and exact disk"),Matches() && FFileHelper::LoadFileToArray(AfterFile,*Filename) && BeforeFile==AfterFile);
    return !HasAnyErrors();
}
