#include "Misc/AutomationTest.h"
#include "Particles/SubUVAnimation.h"
#include "Engine/Texture2D.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "ObjectTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
TSharedPtr<FJsonObject> FindSubUVProperty(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type != EJson::Object) continue;
		const auto Child = Field.Value->AsObject();
		if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
		if (auto Found = FindSubUVProperty(Child, Name)) return Found;
	}
	return nullptr;
}

bool SaveSubUVFixture(UObject* Object)
{
	// Match native asset creation: folder discovery consumes the registry,
	// not the asynchronous filesystem notification from saving a package.
	FAssetRegistryModule::AssetCreated(Object);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	const FString Filename = FPackageName::LongPackageNameToFilename(Object->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Object->GetPackage(), Object, *Filename, Save)) return false;
	// Automation executes save, import, GC and deletion in one editor frame.
	// Establish the on-disk inventory before authoring, as other native fixtures do.
	FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({Filename}, true);
	return true;
}

bool PrepareSubUVFixture(const FString& Base, USubUVAnimation*& Animation, UTexture2D*& A, UTexture2D*& B)
{
	for (int32 Variant = 0; Variant < 2; ++Variant)
	{
		const FString Name = Variant ? TEXT("SourceB") : TEXT("SourceA");
		auto* Texture = NewObject<UTexture2D>(CreatePackage(*(Base / Name)), *Name, RF_Public | RF_Standalone);
		TArray<FColor> Pixels; Pixels.SetNum(32 * 32);
		for (int32 Y = 0; Y < 32; ++Y) for (int32 X = 0; X < 32; ++X)
		{
			const int32 Distance = FMath::Abs(X % 16 - 8) + FMath::Abs(Y % 16 - 8);
			Pixels[Y * 32 + X] = FColor(255, 255, 255, Distance < (Variant ? 7 : 4) ? 255 : 0);
		}
		Texture->Source.Init(32, 32, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		if (Variant) B = Texture; else A = Texture;
		if (!SaveSubUVFixture(Texture)) return false;
	}
	Animation = NewObject<USubUVAnimation>(CreatePackage(*(Base / TEXT("Animation"))), TEXT("Animation"), RF_Public | RF_Standalone);
	Animation->SubUVTexture = A;
	Animation->SubImages_Horizontal = 1; Animation->SubImages_Vertical = 1;
	Animation->BoundingMode = BVC_FourVertices;
	Animation->OpacitySourceMode = OSM_Alpha;
	Animation->AlphaThreshold = 0.25f;
	Animation->PostEditChange();
	FlushRenderingCommands();
	return SaveSubUVFixture(Animation) && Animation->IsBoundingGeometryValid();
}

bool MatchesSubUVGeometry(const USubUVAnimation& Animation, const FSubUVDerivedData& Expected)
{
	if (!Animation.IsBoundingGeometryValid() || !Animation.GetBoundingGeometrySRV()
		|| Expected.BoundingGeometry.Num() != Animation.GetNumFrames() * Animation.GetNumBoundingVertices()) return false;
	for (int32 Frame = 0; Frame < Animation.GetNumFrames(); ++Frame)
		for (int32 Vertex = 0; Vertex < Animation.GetNumBoundingVertices(); ++Vertex)
			if (!Animation.GetFrameData(Frame)[Vertex].Equals(Expected.BoundingGeometry[Frame * Animation.GetNumBoundingVertices() + Vertex], 1.e-6f)) return false;
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208SubUVWorkspaceTest,
	"UAI.Root.T208.SubUVAnimation.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208SubUVWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-subuv"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Base = TEXT("/Game/UAI/Transient/T208SubUV/") + Token;
	const FString PackageName = Base / TEXT("Animation");
	const FString ObjectPath = PackageName + TEXT(".Animation");
	const FString SourceBPath = Base / TEXT("SourceB.SourceB");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-subuv/native") / Token);
	USubUVAnimation* Animation = nullptr; UTexture2D* A = nullptr; UTexture2D* B = nullptr;
	ON_SCOPE_EXIT
	{
		Animation = nullptr; A = nullptr; B = nullptr;
		FlushRenderingCommands();
		TArray<FAssetData> Registered;
		FAssetRegistryModule::GetRegistry().GetAssetsByPath(FName(*Base), Registered, true, false);
		TestEqual(TEXT("created fixtures remain discoverable before folder deletion"), Registered.Num(), 3);
		auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(Base)}); Args->SetBoolField(TEXT("recursive"), true);
		for (const bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
		for (const FString& Name : {TEXT("Animation"), TEXT("SourceA"), TEXT("SourceB")}) TestFalse(TEXT("owned SubUV fixture deleted"), FPackageName::DoesPackageExist(Base / Name));
	};
	if (!TestTrue(TEXT("real source pixels and native baseline geometry"), PrepareSubUVFixture(Base, Animation, A, B))) return false;
	TArray<FAssetData> Persisted;
	FAssetRegistryModule::GetRegistry().GetAssetsByPath(FName(*Base), Persisted, true, true);
	if (!TestEqual(TEXT("three source fixtures indexed on disk before authoring"), Persisted.Num(), 3)) return false;
	TArray<uint8> SourceABytes, SourceBBytes;
	const FString AFile = FPackageName::LongPackageNameToFilename(Base / TEXT("SourceA"), FPackageName::GetAssetPackageExtension());
	const FString BFile = FPackageName::LongPackageNameToFilename(Base / TEXT("SourceB"), FPackageName::GetAssetPackageExtension());
	if (!FFileHelper::LoadFileToArray(SourceABytes, *AFile) || !FFileHelper::LoadFileToArray(SourceBBytes, *BFile)) return false;
	FSubUVDerivedData Expected;
	Expected.Build(B, 2, 2, BVC_EightVertices, 0.4f, OSM_Alpha);
	if (!TestEqual(TEXT("independent native cutout has four eight-vertex frames"), Expected.BoundingGeometry.Num(), 32)) return false;
	auto Export = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Edit = [&](const FString& Stage, const FString& Field, const TSharedRef<FJsonValue>& Value)
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		int32 Count = 0;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
			if (auto Row = FindSubUVProperty(Root, Field))
			{
				Row->SetField(TEXT("value"), Value); ++Count;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
		}
		return TestEqual(TEXT("one editable SubUV field"), Count, 1);
	};
	auto Import = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args);
	};
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), TEXT("SubUVTexture"), MakeShared<FJsonValueString>(SourceBPath))
		|| !Edit(TEXT("edit"), TEXT("SubImages_Horizontal"), MakeShared<FJsonValueNumber>(2))
		|| !Edit(TEXT("edit"), TEXT("SubImages_Vertical"), MakeShared<FJsonValueNumber>(2))
		|| !Edit(TEXT("edit"), TEXT("BoundingMode"), MakeShared<FJsonValueString>(TEXT("BVC_EightVertices")))
		|| !Edit(TEXT("edit"), TEXT("AlphaThreshold"), MakeShared<FJsonValueNumber>(0.4))) return false;
	const auto Applied = Import(TEXT("edit"));
	if (!TestTrue(TEXT("public SubUV import"), Applied.IsOk())) { AddError(Applied.GetErrorJson()); return false; }
	Animation = LoadObject<USubUVAnimation>(nullptr, *ObjectPath);
	FlushRenderingCommands();
	if (!TestTrue(TEXT("source grid and derived geometry rebuilt"), Animation && Animation->SubUVTexture && Animation->SubUVTexture->GetPathName() == SourceBPath
		&& Animation->SubImages_Horizontal == 2 && Animation->SubImages_Vertical == 2 && Animation->BoundingMode == BVC_EightVertices
		&& Animation->OpacitySourceMode == OSM_Alpha && FMath::IsNearlyEqual(Animation->AlphaThreshold, 0.4f)
		&& MatchesSubUVGeometry(*Animation, Expected))) return false;
	TArray<uint8> Before, After;
	if (!FFileHelper::LoadFileToArray(Before, *Filename) || !Export(TEXT("invalid"))
		|| !Edit(TEXT("invalid"), TEXT("SubUVTexture"), MakeShared<FJsonValueString>(ObjectPath))) return false;
	TestFalse(TEXT("wrong source type rejected"), Import(TEXT("invalid")).IsOk());
	TestTrue(TEXT("invalid reference preserves target"), FFileHelper::LoadFileToArray(After, *Filename) && Before == After && MatchesSubUVGeometry(*Animation, Expected));
	UeAgentReflectionAuthoring::FAssetApplyRequest Late;
	Late.TypeId = TEXT("subuv_animation"); Late.AssetPath = PackageName; Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
	auto Patch = MakeShared<FJsonObject>(); Patch->SetStringField(TEXT("property_path"), TEXT("SubImages_Horizontal")); Patch->SetNumberField(TEXT("value"), 1);
	Late.Properties = {MakeShared<FJsonValueObject>(Patch)};
	bool Published = false;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		auto* Current = LoadObject<USubUVAnimation>(nullptr, *ObjectPath); FlushRenderingCommands();
		Published = Current && Current->GetNumFrames() == 2 && Current->IsBoundingGeometryValid() && Current->GetBoundingGeometrySRV();
		Failure.ErrorCode = TEXT("t208_subuv_late_rejection"); return false;
	};
	Animation = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
	Animation = LoadObject<USubUVAnimation>(nullptr, *ObjectPath); FlushRenderingCommands();
	TestTrue(TEXT("late readback observed changed geometry"), Published);
	TestTrue(TEXT("late recovery restores derived data and disk"), LateResult.bRollbackSucceeded && LateResult.ErrorCode == TEXT("t208_subuv_late_rejection")
		&& Animation && MatchesSubUVGeometry(*Animation, Expected) && FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
	Animation = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Animation = LoadObject<USubUVAnimation>(nullptr, *ObjectPath); FlushRenderingCommands();
	TestTrue(TEXT("independent reload retains source and geometry"), Animation && Animation->SubUVTexture && Animation->SubUVTexture->GetPathName() == SourceBPath && MatchesSubUVGeometry(*Animation, Expected));
	TestTrue(TEXT("source texture packages unchanged"), FFileHelper::LoadFileToArray(After, *AFile) && SourceABytes == After && FFileHelper::LoadFileToArray(After, *BFile) && SourceBBytes == After);
	TestTrue(TEXT("final independent export"), Export(TEXT("readback")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208SubUVSdkFixtureTest,
	"UAI.Root.T208.SubUVAnimation.SDKFixture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208SubUVSdkFixtureTest::RunTest(const FString&)
{
	const FString Base = TEXT("/Game/UAI/Transient/T208SubUVSDK/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	USubUVAnimation* Animation = nullptr; UTexture2D* A = nullptr; UTexture2D* B = nullptr;
	bool Published = false;
	ON_SCOPE_EXIT
	{
		if (!Published)
		{
			TArray<UObject*> Assets;
			if (Animation) Assets.Add(Animation);
			if (A) Assets.Add(A);
			if (B) Assets.Add(B);
			ObjectTools::DeleteObjects(Assets, false);
		}
	};
	if (!TestTrue(TEXT("native source and SubUV SDK fixture"), PrepareSubUVFixture(Base, Animation, A, B))) return false;
	const FString Directory = FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-subuv");
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Document = FString::Printf(TEXT("{\"target\":\"%s\",\"source_a\":\"%s\",\"source_b\":\"%s\"}"),
		*Animation->GetPathName(), *A->GetPathName(), *B->GetPathName());
	Published = FFileHelper::SaveStringToFile(Document, *(Directory / TEXT("sdk-fixture.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return TestTrue(TEXT("SDK fixture public target handoff"), Published);
}
