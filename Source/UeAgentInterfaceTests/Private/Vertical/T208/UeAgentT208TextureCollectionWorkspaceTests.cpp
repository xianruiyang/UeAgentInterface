#include "Misc/AutomationTest.h"
#include "Engine/TextureCollection.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "TextureResource.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureCollection.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIResourceCollection.h"
#include "ObjectTools.h"
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
bool SaveCollectionFixture(UObject* Object)
{
	FAssetRegistryModule::AssetCreated(Object);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	const FString Filename = FPackageName::LongPackageNameToFilename(Object->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Object->GetPackage(), Object, *Filename, Save)) return false;
	FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({Filename}, true);
	return true;
}

bool PrepareCollectionTextures(const FString& Base, TArray<TStrongObjectPtr<UTexture2D>>& Textures)
{
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FString Name = FString::Printf(TEXT("Source%d"), Index);
		auto* Texture = NewObject<UTexture2D>(CreatePackage(*(Base / Name)), *Name, RF_Public | RF_Standalone);
		Textures.Emplace(Texture);
		TArray<FColor> Pixels; Pixels.Init(FColor(40 + Index * 70, 80, 150, 255), 16 * 16);
		Texture->Source.Init(16, 16, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->UpdateResource();
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	FlushRenderingCommands();
	for (const auto& Texture : Textures)
		if (!Texture->GetResource() || !SaveCollectionFixture(Texture.Get())) return false;
	return true;
}

TSharedPtr<FJsonObject> FindCollectionProperty(const TSharedPtr<FJsonObject>& Object)
{
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type != EJson::Object) continue;
		const auto Child = Field.Value->AsObject();
		if (Field.Key == TEXT("Textures") && Child->HasField(TEXT("value"))) return Child;
		if (auto Found = FindCollectionProperty(Child)) return Found;
	}
	return nullptr;
}

bool CollectionMatches(UTextureCollection* Collection, const TArray<UTexture*>& Expected)
{
	if (!Collection || Collection->Textures.Num() != Expected.Num()) return false;
	TArray<FRHITextureReference*> ExpectedReferences;
	for (int32 Index = 0; Index < Expected.Num(); ++Index)
	{
		if (Collection->Textures[Index] != Expected[Index]) return false;
		UTexture* Texture = Expected[Index] ? Expected[Index] : GEngine->DefaultTexture;
		if (!Texture || !Texture->GetResource()) return false;
		ExpectedReferences.Add(Texture->GetResource()->GetTextureReference());
		if (!ExpectedReferences.Last()) return false;
	}
	bool Matches = false;
	ENQUEUE_RENDER_COMMAND(UaiCollectionReadback)([Collection, ExpectedReferences, &Matches](FRHICommandListImmediate&)
	{
		const auto* Resource = Collection->GetResource();
		const auto* Rhi = Resource ? Resource->GetRHI() : nullptr;
		Matches = Rhi && Rhi->GetBindlessHandle().IsValid() && Rhi->Members.Num() == ExpectedReferences.Num();
		if (Matches) for (int32 Index = 0; Index < ExpectedReferences.Num(); ++Index)
			Matches = Matches && Rhi->Members[Index].Type == FRHIResourceCollectionMember::EType::TextureReference
				&& Rhi->Members[Index].Resource == ExpectedReferences[Index];
	});
	FlushRenderingCommands();
	return Matches;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208TextureCollectionWorkspaceTest,
	"UAI.Root.T208.TextureCollection.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208TextureCollectionWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const auto Bindless = RHIGetRuntimeBindlessResourcesConfiguration(GMaxRHIShaderPlatform);
	UE_LOG(LogTemp, Display, TEXT("T208 collection platform=%d bindless-config=%d support=%d"), int32(GMaxRHIShaderPlatform), int32(Bindless), int32(GRHIGlobals.BindlessSupport));
	if (!TestTrue(TEXT("runtime supports native bindless resource collection"), Bindless != ERHIBindlessConfiguration::Disabled && GRHIGlobals.BindlessSupport != ERHIBindlessSupport::Unsupported)) return false;
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-texture-collection"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Base = TEXT("/Game/UAI/Transient/T208TextureCollection/") + Token;
	const FString PackageName = Base / TEXT("Collection");
	const FString ObjectPath = PackageName + TEXT(".Collection");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-texture-collection/native") / Token);
	TArray<TStrongObjectPtr<UTexture2D>> Textures;
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(GetTransientPackage()));
	UTextureCollection* Collection = nullptr;
	ON_SCOPE_EXIT
	{
		Material.Reset(); Textures.Reset(); Collection = nullptr; FlushRenderingCommands();
		auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(Base)}); Args->SetBoolField(TEXT("recursive"), true);
		for (const bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
		for (const FString& Name : {TEXT("Collection"), TEXT("Source0"), TEXT("Source1"), TEXT("Source2")}) TestFalse(TEXT("owned collection fixture deleted"), FPackageName::DoesPackageExist(Base / Name));
	};
	if (!TestTrue(TEXT("initialized native texture inputs"), PrepareCollectionTextures(Base, Textures))) return false;
	Collection = NewObject<UTextureCollection>(CreatePackage(*PackageName), TEXT("Collection"), RF_Public | RF_Standalone);
	Collection->Textures = {Textures[0].Get(), Textures[1].Get()};
	Collection->PostEditChange();
	if (!TestTrue(TEXT("baseline native collection resources"), CollectionMatches(Collection, {Textures[0].Get(), Textures[1].Get()})) || !SaveCollectionFixture(Collection)) return false;
	auto* Expression = NewObject<UMaterialExpressionTextureCollection>(Material.Get());
	Expression->TextureCollection = Collection;
	Material->GetExpressionCollection().AddExpression(Expression); Material->PostEditChange();
	if (!TestTrue(TEXT("material has real collection reference"), Material->GetReferencedTextureCollections().Contains(Collection))) return false;
	const FGuid BeforeMaterial = Material->StateId;
	TArray<TArray<uint8>> SourceBytes; SourceBytes.SetNum(Textures.Num());
	for (int32 Index = 0; Index < Textures.Num(); ++Index)
		if (!FFileHelper::LoadFileToArray(SourceBytes[Index], *FPackageName::LongPackageNameToFilename(Textures[Index]->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()))) return false;
	auto Export = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Edit = [&](const FString& Stage, const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		int32 Count = 0;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
			if (auto Row = FindCollectionProperty(Root))
			{
				Row->SetArrayField(TEXT("value"), Values); ++Count;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
		}
		return TestEqual(TEXT("one collection field"), Count, 1);
	};
	auto Import = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args);
	};
	const TArray<UTexture*> Expected{Textures[1].Get(), nullptr, Textures[0].Get(), Textures[1].Get()};
	TArray<TSharedPtr<FJsonValue>> Values;
	for (UTexture* Texture : Expected) Values.Add(Texture ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Texture->GetPathName())) : StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), Values)) return false;
	const auto Applied = Import(TEXT("edit"));
	if (!TestTrue(TEXT("public collection import"), Applied.IsOk())) { AddError(Applied.GetErrorJson()); return false; }
	Collection = LoadObject<UTextureCollection>(nullptr, *ObjectPath);
	TestTrue(TEXT("ordered duplicate and null fallback RHI members"), CollectionMatches(Collection, Expected));
	TestTrue(TEXT("material reference redirected and notified"), Expression->TextureCollection == Collection && Material->StateId != BeforeMaterial);
	TArray<uint8> Before, After;
	if (!FFileHelper::LoadFileToArray(Before, *Filename) || !Export(TEXT("invalid")) || !Edit(TEXT("invalid"), {MakeShared<FJsonValueString>(ObjectPath)})) return false;
	TestFalse(TEXT("wrong member type rejected"), Import(TEXT("invalid")).IsOk());
	TestTrue(TEXT("rejected membership preserves disk and RHI"), FFileHelper::LoadFileToArray(After, *Filename) && Before == After && CollectionMatches(Collection, Expected));
	UeAgentReflectionAuthoring::FAssetApplyRequest Late;
	Late.TypeId = TEXT("texture_collection"); Late.AssetPath = PackageName; Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
	auto Patch = MakeShared<FJsonObject>(); Patch->SetStringField(TEXT("property_path"), TEXT("Textures")); Patch->SetArrayField(TEXT("value"), {MakeShared<FJsonValueString>(Textures[2]->GetPathName())});
	Late.Properties = {MakeShared<FJsonValueObject>(Patch)};
	bool Published = false;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		auto* Current = LoadObject<UTextureCollection>(nullptr, *ObjectPath);
		Published = Expression->TextureCollection == Current && CollectionMatches(Current, {Textures[2].Get()});
		Failure.ErrorCode = TEXT("t208_collection_late_rejection"); return false;
	};
	Collection = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
	Collection = LoadObject<UTextureCollection>(nullptr, *ObjectPath);
	TestTrue(TEXT("late readback observed new RHI member"), Published);
	TestTrue(TEXT("late recovery restores membership resource references and disk"), LateResult.bRollbackSucceeded && LateResult.ErrorCode == TEXT("t208_collection_late_rejection")
		&& Expression->TextureCollection == Collection && CollectionMatches(Collection, Expected) && FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
	Expression->TextureCollection = nullptr; Material.Reset(); Collection = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Collection = LoadObject<UTextureCollection>(nullptr, *ObjectPath);
	TestTrue(TEXT("independent reload retains RHI membership"), CollectionMatches(Collection, Expected));
	if (!Export(TEXT("remove")) || !Edit(TEXT("remove"), {MakeShared<FJsonValueString>(Textures[1]->GetPathName()), MakeShared<FJsonValueString>(Textures[0]->GetPathName())})) return false;
	const auto Removed = Import(TEXT("remove"));
	if (!TestTrue(TEXT("public member removal"), Removed.IsOk())) { AddError(Removed.GetErrorJson()); return false; }
	Collection = LoadObject<UTextureCollection>(nullptr, *ObjectPath);
	TestTrue(TEXT("removed members absent from RHI"), CollectionMatches(Collection, {Textures[1].Get(), Textures[0].Get()}));
	if (!Export(TEXT("clear")) || !Edit(TEXT("clear"), {})) return false;
	const auto Cleared = Import(TEXT("clear"));
	if (!TestTrue(TEXT("public collection clear"), Cleared.IsOk())) { AddError(Cleared.GetErrorJson()); return false; }
	Collection = LoadObject<UTextureCollection>(nullptr, *ObjectPath);
	TestTrue(TEXT("empty native resource collection remains valid"), CollectionMatches(Collection, {}));
	for (int32 Index = 0; Index < Textures.Num(); ++Index)
		TestTrue(TEXT("source texture package unchanged"), FFileHelper::LoadFileToArray(After, *FPackageName::LongPackageNameToFilename(Textures[Index]->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension())) && SourceBytes[Index] == After);
	TestTrue(TEXT("independent final export"), Export(TEXT("readback")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208TextureCollectionSdkFixtureTest,
	"UAI.Root.T208.TextureCollection.SDKFixture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208TextureCollectionSdkFixtureTest::RunTest(const FString&)
{
	const FString Base = TEXT("/Game/UAI/Transient/T208TextureCollectionSDK/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<TStrongObjectPtr<UTexture2D>> Textures;
	bool Published = false;
	ON_SCOPE_EXIT
	{
		if (!Published)
		{
			TArray<UObject*> Assets; for (const auto& Texture : Textures) Assets.Add(Texture.Get());
			Textures.Reset(); ObjectTools::DeleteObjects(Assets, false);
		}
	};
	if (!TestTrue(TEXT("native texture inputs for public SDK collection creation"), PrepareCollectionTextures(Base, Textures))) return false;
	const FString Directory = FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-texture-collection");
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Document = FString::Printf(TEXT("{\"target\":\"%s/Collection.Collection\",\"source_a\":\"%s\",\"source_b\":\"%s\",\"source_c\":\"%s\"}"),
		*Base, *Textures[0]->GetPathName(), *Textures[1]->GetPathName(), *Textures[2]->GetPathName());
	Published = FFileHelper::SaveStringToFile(Document, *(Directory / TEXT("sdk-fixture.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return TestTrue(TEXT("public fixture paths saved; SDK creates collection"), Published);
}
