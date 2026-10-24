#include "Misc/AutomationTest.h"
#include "AssetExportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/Skeleton.h"
#include "Animation/SkinWeightProfile.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Engine/SkeletalMesh.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "Exporters/SkeletalMeshExporterFBX.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208ExternalSkinWeightWorkspaceTest,
	"UAI.Root.T208.ExternalInput.SkinWeightWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208ExternalSkinWeightWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Run = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Tests/T208SkinWeight/SK_") + Run;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-external-input/skin-weight-native") / Run);
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString SourcePath = Directory / TEXT("Alternate.fbx");
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-skin-weight"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	USkeletalMesh* Original = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	if (!TestNotNull(TEXT("real engine skeletal mesh fixture"), Original)) return false;
	UPackage* Package = CreatePackage(*PackageName);
	USkeletalMesh* Mesh = DuplicateObject<USkeletalMesh>(Original, Package, *FPackageName::GetLongPackageAssetName(PackageName));
	if (!Mesh) return false;
	Mesh->SetFlags(RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Mesh);
	ON_SCOPE_EXIT
	{
		if (auto* Remaining = LoadObject<USkeletalMesh>(nullptr, *ObjectPath); Remaining && Remaining->GetPackage()->IsDirty())
		{
			FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
			TestTrue(TEXT("owned fixture settles before deletion"), UPackage::SavePackage(Remaining->GetPackage(), Remaining, *Filename, Save));
		}
		if (FPackageName::DoesPackageExist(PackageName))
		{
			auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)}); Args->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
			TestFalse(TEXT("owned mesh package removed"), FPackageName::DoesPackageExist(PackageName));
		}
	};
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("owned mesh is persisted"), UPackage::SavePackage(Package, Mesh, *Filename, Save))) return false;
	UAssetExportTask* Task = NewObject<UAssetExportTask>();
	Task->Object = Mesh; Task->Exporter = NewObject<USkeletalMeshExporterFBX>(); Task->Filename = SourcePath;
	Task->bAutomated = true; Task->bPrompt = false; Task->bReplaceIdentical = true; Task->Options = NewObject<UFbxExportOption>();
	if (!TestTrue(TEXT("UE exports a real skinned FBX"), UExporter::RunAssetExportTask(Task) && IFileManager::Get().FileSize(*SourcePath) > 0)) return false;
	Task->Object = nullptr;
	const int32 InitialProfiles = Mesh->GetSkinWeightProfiles().Num();
	const int32 InitialLods = Mesh->GetLODNum();
	const FString SkeletonPath = Mesh->GetSkeleton() ? Mesh->GetSkeleton()->GetPathName() : FString();
	auto Prepare = [&](const FString& Stage, const FString& Profile)
	{
		auto SourceInfo = MakeShared<FJsonObject>(); SourceInfo->SetStringField(TEXT("identity"), SourcePath); SourceInfo->SetStringField(TEXT("format"), TEXT("fbx"));
		auto Parameters = MakeShared<FJsonObject>(); Parameters->SetStringField(TEXT("target_ref"), ObjectPath); Parameters->SetStringField(TEXT("profile_name"), Profile); Parameters->SetNumberField(TEXT("lod_index"), 0);
		auto Policy = MakeShared<FJsonObject>(); Policy->SetStringField(TEXT("command_id"), TEXT("external.skin_weight_profile.import")); Policy->SetObjectField(TEXT("parameters"), Parameters);
		auto External = MakeShared<FJsonObject>(); External->SetObjectField(TEXT("source"), SourceInfo); External->SetObjectField(TEXT("target_policy"), Policy); External->SetObjectField(TEXT("options"), MakeShared<FJsonObject>());
		auto Root = MakeShared<FJsonObject>(); Root->SetStringField(TEXT("type"), TEXT("uai.external_input.skin_weight_profile")); Root->SetObjectField(TEXT("external_input"), External);
		auto Document = MakeShared<FJsonObject>(); Document->SetStringField(TEXT("schema"), TEXT("uai.hjson.external_input/1")); Document->SetObjectField(TEXT("root"), Root);
		FString Text; if (!CommandContract::Json::TryCanonicalize(Document, Text, Error)) { AddError(Error); return false; }
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("inline")); Source->SetStringField(TEXT("document"), Text);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetObjectField(TEXT("source"), Source);
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Import = [&](const FString& Stage)
	{
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Submit(TEXT("content_graph.import_workspace"), Args);
	};
	if (!Prepare(TEXT("initial"), TEXT("AutomationProfile"))) return false;
	TestEqual(TEXT("preparation does not add a profile"), Mesh->GetSkinWeightProfiles().Num(), InitialProfiles);
	Mesh = nullptr; Package = nullptr; Original = nullptr;
	const auto Imported = Import(TEXT("initial"));
	if (!TestTrue(TEXT("HJSON invokes real alternate skin weight importer"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	Mesh = LoadObject<USkeletalMesh>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("persisted mesh independently reloads"), Mesh)) return false;
	TestEqual(TEXT("exactly one profile added"), Mesh->GetSkinWeightProfiles().Num(), InitialProfiles + 1);
	TestTrue(TEXT("requested profile is present"), Mesh->GetSkinWeightProfiles().ContainsByPredicate([](const auto& Profile) { return Profile.Name == TEXT("AutomationProfile"); }));
	const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
	const FImportedSkinWeightProfileData* Weights = ImportedModel && ImportedModel->LODModels.IsValidIndex(0)
		? ImportedModel->LODModels[0].SkinWeightProfiles.Find(FName(TEXT("AutomationProfile"))) : nullptr;
	bool bHasRealWeights = false;
	if (Weights)
		for (const FRawSkinWeight& Vertex : Weights->SkinWeights)
			for (uint16 Weight : Vertex.InfluenceWeights) bHasRealWeights |= Weight > 0;
	TestTrue(TEXT("persisted profile contains actual nonzero bone weights"), bHasRealWeights);
	TestEqual(TEXT("non-target LOD count retained"), Mesh->GetLODNum(), InitialLods);
	TestEqual(TEXT("non-target skeleton retained"), Mesh->GetSkeleton() ? Mesh->GetSkeleton()->GetPathName() : FString(), SkeletonPath);
	if (!Prepare(TEXT("source-change"), TEXT("RejectedProfile"))) return false;
	TArray<uint8> Bytes; if (!FFileHelper::LoadFileToArray(Bytes, *SourcePath)) return false;
	Bytes.Add(0); if (!FFileHelper::SaveArrayToFile(Bytes, *SourcePath)) return false;
	const auto Rejected = Import(TEXT("source-change"));
	TestFalse(TEXT("source change is rejected"), Rejected.IsOk());
	TestTrue(TEXT("source rejection has exact code"), Rejected.GetErrorJson().Contains(TEXT("hjson_external_input_source_digest_mismatch")));
	Mesh = LoadObject<USkeletalMesh>(nullptr, *ObjectPath);
	TestTrue(TEXT("source rejection retains existing profile set"), Mesh && Mesh->GetSkinWeightProfiles().Num() == InitialProfiles + 1
		&& !Mesh->GetSkinWeightProfiles().ContainsByPredicate([](const auto& Profile) { return Profile.Name == TEXT("RejectedProfile"); }));
	return !HasAnyErrors();
}
