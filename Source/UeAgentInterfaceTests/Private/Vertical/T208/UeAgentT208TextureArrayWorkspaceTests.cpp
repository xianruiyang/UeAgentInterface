#include "Misc/AutomationTest.h"
#include "Engine/Texture2DArray.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RHI.h"
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
bool SaveArrayFixture(UObject* Object)
{
	FAssetRegistryModule::AssetCreated(Object);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	const FString Filename = FPackageName::LongPackageNameToFilename(Object->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Object->GetPackage(), Object, *Filename, Save)) return false;
	FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({Filename}, true);
	return true;
}

bool PrepareArrayTextures(const FString& Base, TArray<TStrongObjectPtr<UTexture2D>>& Textures)
{
	const FColor Colors[]{FColor::Red, FColor::Green, FColor::Blue};
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FString Name = FString::Printf(TEXT("Source%d"), Index);
		auto* Texture = NewObject<UTexture2D>(CreatePackage(*(Base / Name)), *Name, RF_Public | RF_Standalone);
		Textures.Emplace(Texture);
		if (Index < 3)
		{
			const int32 Size = Index == 2 ? 8 : 16;
			TArray<FColor> Pixels; Pixels.Init(Colors[Index], Size * Size);
			Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
			Texture->SRGB = Index != 2;
			Texture->MipGenSettings = TMGS_NoMipmaps;
			Texture->UpdateResource();
		}
	}
	FAssetCompilingManager::Get().FinishAllCompilation(); FlushRenderingCommands();
	for (int32 Index = 0; Index < Textures.Num(); ++Index)
		if ((Index < 3 && !Textures[Index]->GetResource()) || !SaveArrayFixture(Textures[Index].Get())) return false;
	return true;
}

TSharedPtr<FJsonObject> FindArrayProperty(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type != EJson::Object) continue;
		const auto Child = Field.Value->AsObject();
		if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
		if (auto Found = FindArrayProperty(Child, Name)) return Found;
	}
	return nullptr;
}

bool ArrayMatches(UTexture2DArray* Texture, const TArray<UTexture2D*>& Expected, const bool Converted = false)
{
	if (!Texture || Texture->SourceTextures.Num() != Expected.Num()) return false;
	for (int32 Index = 0; Index < Expected.Num(); ++Index)
		if (Texture->SourceTextures[Index] != Expected[Index]) return false;
	FAssetCompilingManager::Get().FinishAllCompilation(); FlushRenderingCommands();
	if (Expected.IsEmpty()) return !Texture->Source.IsValid() && !Texture->GetResource();
	if (!Texture->Source.IsValid() || Texture->Source.GetNumSlices() != Expected.Num()
		|| Texture->Source.GetSizeX() != 16 || Texture->Source.GetSizeY() != 16 || Texture->Source.GetNumMips() != 1) return false;
	TArray64<uint8> Actual, ExpectedBytes;
	if (!Texture->Source.GetMipData(Actual, 0)) return false;
	if (Converted)
	{
		// Known solid green sRGB and solid blue linear inputs remain exact primary
		// colors after conversion/resizing; this oracle does not rebuild the target.
		if (Expected.Num() != 2 || Texture->Source.GetFormat() != TSF_RGBA32F) return false;
		for (const FLinearColor Color : {FLinearColor::Green, FLinearColor::Blue})
			for (int32 Pixel = 0; Pixel < 16 * 16; ++Pixel)
				ExpectedBytes.Append(reinterpret_cast<const uint8*>(&Color), sizeof(Color));
	}
	else
	{
		if (Texture->Source.GetFormat() != TSF_BGRA8) return false;
		for (auto* Source : Expected)
		{
			TArray64<uint8> Bytes;
			if (!Source->Source.GetMipData(Bytes, 0)) return false;
			ExpectedBytes.Append(Bytes);
		}
	}
	const auto* Resource = Texture->GetResource();
	const auto* Rhi = Resource ? Resource->GetTexture2DArrayRHI() : nullptr;
	return Actual == ExpectedBytes && Rhi && Rhi->GetDesc().Extent == FIntPoint(16, 16)
		&& Rhi->GetDesc().ArraySize == Expected.Num() && Rhi->GetDesc().NumMips > 0;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208TextureArrayWorkspaceTest,
	"UAI.Root.T208.TextureArray.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208TextureArrayWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-texture-array"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Base = TEXT("/Game/UAI/Transient/T208TextureArray/") + Token;
	const FString PackageName = Base / TEXT("Array");
	const FString ObjectPath = PackageName + TEXT(".Array");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-texture-array/native") / Token);
	TArray<TStrongObjectPtr<UTexture2D>> Textures;
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(GetTransientPackage()));
	UTexture2DArray* Array = nullptr;
	ON_SCOPE_EXIT
	{
		Material.Reset(); Textures.Reset(); Array = nullptr; FlushRenderingCommands();
		auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(Base)}); Args->SetBoolField(TEXT("recursive"), true);
		for (const bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
		for (const FString& Name : {TEXT("Array"), TEXT("Source0"), TEXT("Source1"), TEXT("Source2"), TEXT("Source3")}) TestFalse(TEXT("owned array fixture deleted"), FPackageName::DoesPackageExist(Base / Name));
	};
	if (!TestTrue(TEXT("native texture inputs"), PrepareArrayTextures(Base, Textures))) return false;
	Array = NewObject<UTexture2DArray>(CreatePackage(*PackageName), TEXT("Array"), RF_Public | RF_Standalone);
	Array->SourceTextures = {Textures[0].Get(), Textures[1].Get()};
	Array->MipGenSettings = TMGS_NoMipmaps; Array->NeverStream = true;
	if (!Array->UpdateSourceFromSourceTextures(false) || !ArrayMatches(Array, {Textures[0].Get(), Textures[1].Get()}) || !SaveArrayFixture(Array)) return false;
	auto* Expression = NewObject<UMaterialExpressionTextureObject>(Material.Get());
	Expression->Texture = Array; Material->GetExpressionCollection().AddExpression(Expression); Material->PostEditChange();
	TArray<TArray<uint8>> SourceBytes; SourceBytes.SetNum(Textures.Num());
	for (int32 Index = 0; Index < Textures.Num(); ++Index)
		if (!FFileHelper::LoadFileToArray(SourceBytes[Index], *FPackageName::LongPackageNameToFilename(Textures[Index]->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()))) return false;
	auto Export = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Edit = [&](const FString& Stage, const TArray<TSharedPtr<FJsonValue>>& Values, const int32 FormatSettings = 0)
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		int32 Count = 0;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
			bool Changed = false;
			if (auto Row = FindArrayProperty(Root, TEXT("SourceTextures"))) { Row->SetArrayField(TEXT("value"), Values); ++Count; Changed = true; }
			if (FormatSettings)
			{
				if (auto Row = FindArrayProperty(Root, TEXT("SRGB"))) { Row->SetBoolField(TEXT("value"), FormatSettings == 2); Changed = true; }
				if (auto Row = FindArrayProperty(Root, TEXT("CompressionSettings"))) { Row->SetStringField(TEXT("value"), FormatSettings == 2 ? TEXT("TC_Grayscale") : TEXT("TC_Masks")); Changed = true; }
				if (auto Row = FindArrayProperty(Root, TEXT("MipGenSettings"))) { Row->SetStringField(TEXT("value"), FormatSettings == 2 ? TEXT("TMGS_NoMipmaps") : TEXT("TMGS_SimpleAverage")); Changed = true; }
			}
			if (Changed && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		}
		return TestEqual(TEXT("one source array field"), Count, 1);
	};
	auto Import = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args);
	};
	const TArray<UTexture2D*> Expected{Textures[1].Get(), Textures[0].Get(), Textures[1].Get()};
	TArray<TSharedPtr<FJsonValue>> Values;
	for (auto* Texture : Expected) Values.Add(MakeShared<FJsonValueString>(Texture->GetPathName()));
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), Values, true)) return false;
	const auto Applied = Import(TEXT("edit"));
	if (!TestTrue(TEXT("public array import"), Applied.IsOk())) { AddError(Applied.GetErrorJson()); return false; }
	Array = LoadObject<UTexture2DArray>(nullptr, *ObjectPath);
	TestTrue(TEXT("ordered duplicate pixel slices and RHI"), ArrayMatches(Array, Expected));
	TestTrue(TEXT("explicit format applied after native source defaults"), !Array->SRGB && Array->CompressionSettings == TC_Masks && Array->MipGenSettings == TMGS_SimpleAverage);
	TestTrue(TEXT("resident material reference redirected"), Expression->Texture == Array);
	TArray<uint8> Before, After;
	if (!FFileHelper::LoadFileToArray(Before, *Filename)) return false;
	for (int32 Case = 0; Case < 3; ++Case)
	{
		const FString Stage = FString::Printf(TEXT("invalid-%d"), Case);
		TSharedPtr<FJsonValue> Invalid = Case == 0 ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>())
			: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Case == 1 ? ObjectPath : Textures[3]->GetPathName()));
		if (!Export(Stage) || !Edit(Stage, {MakeShared<FJsonValueString>(Textures[0]->GetPathName()), Invalid})) return false;
		const auto Rejected = Import(Stage);
		TestFalse(TEXT("null wrong-class or missing-source rejected"), Rejected.IsOk());
		if (Case != 1) TestTrue(TEXT("invalid source accurately diagnosed"), Rejected.GetErrorJson().Contains(TEXT("texture_array_source_invalid")));
		Array = LoadObject<UTexture2DArray>(nullptr, *ObjectPath);
		TestTrue(TEXT("invalid inputs preserve source RHI and disk"), ArrayMatches(Array, Expected) && FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
	}
	UeAgentReflectionAuthoring::FAssetApplyRequest Late;
	Late.TypeId = TEXT("texture_array"); Late.AssetPath = PackageName; Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
	auto Patch = MakeShared<FJsonObject>(); Patch->SetStringField(TEXT("property_path"), TEXT("SourceTextures"));
	Patch->SetArrayField(TEXT("value"), {MakeShared<FJsonValueString>(Textures[1]->GetPathName()), MakeShared<FJsonValueString>(Textures[2]->GetPathName())});
	Late.Properties = {MakeShared<FJsonValueObject>(Patch)};
	bool Published = false;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		auto* Current = LoadObject<UTexture2DArray>(nullptr, *ObjectPath);
		Published = Expression->Texture == Current && ArrayMatches(Current, {Textures[1].Get(), Textures[2].Get()}, true);
		Failure.ErrorCode = TEXT("t208_array_late_rejection"); return false;
	};
	Array = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
	Array = LoadObject<UTexture2DArray>(nullptr, *ObjectPath);
	TestTrue(TEXT("late readback observed native resized linear float slices"), Published);
	TestTrue(TEXT("late recovery restores source RHI references and disk"), LateResult.bRollbackSucceeded && LateResult.ErrorCode == TEXT("t208_array_late_rejection")
		&& Expression->Texture == Array && ArrayMatches(Array, Expected) && FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
	Expression->Texture = nullptr; Material.Reset(); Array = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Array = LoadObject<UTexture2DArray>(nullptr, *ObjectPath);
	TestTrue(TEXT("independent reload retains source and RHI"), ArrayMatches(Array, Expected));
	if (!Export(TEXT("convert")) || !Edit(TEXT("convert"), {MakeShared<FJsonValueString>(Textures[1]->GetPathName()), MakeShared<FJsonValueString>(Textures[2]->GetPathName())})) return false;
	const auto Converted = Import(TEXT("convert"));
	if (!TestTrue(TEXT("public mixed size gamma conversion"), Converted.IsOk())) { AddError(Converted.GetErrorJson()); return false; }
	Array = LoadObject<UTexture2DArray>(nullptr, *ObjectPath);
	TestTrue(TEXT("converted resized exact source bytes and RHI"), ArrayMatches(Array, {Textures[1].Get(), Textures[2].Get()}, true));
	if (!Export(TEXT("remove")) || !Edit(TEXT("remove"), {MakeShared<FJsonValueString>(Textures[0]->GetPathName())}, 2)) return false;
	const auto Removed = Import(TEXT("remove"));
	if (!TestTrue(TEXT("public remove to single source"), Removed.IsOk())) { AddError(Removed.GetErrorJson()); return false; }
	Array = LoadObject<UTexture2DArray>(nullptr, *ObjectPath);
	TestTrue(TEXT("single source rebuilt and explicit settings preserved"), ArrayMatches(Array, {Textures[0].Get()}) && Array->SRGB && Array->CompressionSettings == TC_Grayscale && Array->MipGenSettings == TMGS_NoMipmaps);
	if (!Export(TEXT("clear")) || !Edit(TEXT("clear"), {})) return false;
	const auto Cleared = Import(TEXT("clear"));
	if (!TestTrue(TEXT("public array clear"), Cleared.IsOk())) { AddError(Cleared.GetErrorJson()); return false; }
	Array = LoadObject<UTexture2DArray>(nullptr, *ObjectPath);
	TestTrue(TEXT("clear invalidates source and RHI"), ArrayMatches(Array, {}));
	for (int32 Index = 0; Index < Textures.Num(); ++Index)
		TestTrue(TEXT("source texture package unchanged"), FFileHelper::LoadFileToArray(After, *FPackageName::LongPackageNameToFilename(Textures[Index]->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension())) && SourceBytes[Index] == After);
	TestTrue(TEXT("independent final export"), Export(TEXT("readback")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208TextureArraySdkFixtureTest,
	"UAI.Root.T208.TextureArray.SDKFixture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208TextureArraySdkFixtureTest::RunTest(const FString&)
{
	const FString Base = TEXT("/Game/UAI/Transient/T208TextureArraySDK/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
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
	if (!TestTrue(TEXT("native array inputs for public SDK"), PrepareArrayTextures(Base, Textures))) return false;
	const FString Directory = FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-texture-array");
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Document = FString::Printf(TEXT("{\"target\":\"%s/Array.Array\",\"source_a\":\"%s\",\"source_b\":\"%s\",\"source_c\":\"%s\",\"source_invalid\":\"%s\"}"),
		*Base, *Textures[0]->GetPathName(), *Textures[1]->GetPathName(), *Textures[2]->GetPathName(), *Textures[3]->GetPathName());
	Published = FFileHelper::SaveStringToFile(Document, *(Directory / TEXT("sdk-fixture.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return TestTrue(TEXT("public fixture paths saved; SDK creates array"), Published);
}
