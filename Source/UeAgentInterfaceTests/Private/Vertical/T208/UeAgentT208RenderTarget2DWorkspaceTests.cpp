#include "Misc/AutomationTest.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
TSharedPtr<FJsonObject> FindRenderTarget2DProperty(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	if (!Object) return nullptr;
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type != EJson::Object) continue;
		const auto Child = Field.Value->AsObject();
		if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
		if (auto Found = FindRenderTarget2DProperty(Child, Name)) return Found;
	}
	return nullptr;
}

TMap<FString, FString> RenderTarget2DNonTargetState(UTextureRenderTarget2D& RenderTarget)
{
	TMap<FString, FString> State;
	for (TFieldIterator<FProperty> It(RenderTarget.GetClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Edit) || It->GetName() == TEXT("SizeX") || It->GetName() == TEXT("SizeY") || It->GetName() == TEXT("RenderTargetFormat") || It->GetName() == TEXT("ClearColor") || It->GetName() == TEXT("bAutoGenerateMips")) continue;
		FString Value;
		It->ExportText_InContainer(0, Value, &RenderTarget, &RenderTarget, &RenderTarget, PPF_None);
		State.Add(It->GetName(), Value);
	}
	return State;
}
bool ReadRenderTargetResource(UTextureRenderTarget2D& Target, int32 X, int32 Y, EPixelFormat Format, int32 Mips, FColor Color)
{
	FlushRenderingCommands();
	auto* Resource = Target.GetResource();
	if (!Resource) return false;
	bool Matches = false;
	TArray<FColor> Pixels;
	ENQUEUE_RENDER_COMMAND(UaiReadRenderTargetResource)([Resource, X, Y, Format, Mips, &Matches, &Pixels](FRHICommandListImmediate& Commands)
	{
		// Drive the native deferred-render phase before observing pixels. This only
		// consumes resources already queued by UE; it does not recreate or enqueue
		// the target and cannot repair a missing property/resource notification.
		FDeferredUpdateResource::ResetNeedsUpdate();
		FDeferredUpdateResource::UpdateResources(Commands);
		const auto& Texture = Resource->TextureRHI;
		if (!Texture.IsValid()) return;
		Matches = Texture->GetSizeX() == X && Texture->GetSizeY() == Y && Texture->GetFormat() == Format && Texture->GetNumMips() == Mips;
		Commands.ReadSurfaceData(Texture, FIntRect(0, 0, 1, 1), Pixels, FReadSurfaceDataFlags(RCM_UNorm));
	});
	FlushRenderingCommands();
	return Matches && Pixels.Num() == 1 && Pixels[0] == Color;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208RenderTarget2DRootWorkspaceTest,
	"UAI.Root.T208.RenderTarget2D.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208RenderTarget2DRootWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208RenderTarget2D/RT_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-render-target/native") / Token);
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-render-target"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	// Native creation supplies a fixture; editing goes through the public workspace commands.
	auto* RenderTarget = NewObject<UTextureRenderTarget2D>(CreatePackage(*PackageName), *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone);
	RenderTarget->RenderTargetFormat = RTF_RGBA8;
	RenderTarget->ClearColor = FLinearColor::Red;
	RenderTarget->InitAutoFormat(16, 16);
	RenderTarget->PostEditChange();
	RenderTarget->UpdateResourceImmediate(true);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	ON_SCOPE_EXIT
	{
		if (auto* Current = FindObject<UTextureRenderTarget2D>(nullptr, *ObjectPath); Current && Current->GetPackage()->IsDirty())
			TestTrue(TEXT("settle only this fixture after assertions"), UPackage::SavePackage(Current->GetPackage(), Current, *Filename, Save));
		if (FPackageName::DoesPackageExist(PackageName))
		{
			auto Delete = MakeShared<FJsonObject>();
			Delete->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)});
			Delete->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true})
			{
				Delete->SetBoolField(TEXT("confirm"), Confirm);
				const auto Result = Submit(TEXT("asset.delete"), Delete);
				if (!Result.IsOk()) AddError(Result.GetErrorJson());
			}
			TestFalse(TEXT("own fixture package removed"), FPackageName::DoesPackageExist(PackageName));
		}
	};
	if (!TestTrue(TEXT("native fixture saved"), UPackage::SavePackage(RenderTarget->GetPackage(), RenderTarget, *Filename, Save))) return false;
	const auto NonTarget = RenderTarget2DNonTargetState(*RenderTarget);
	auto Export = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args);
		if (!Result.IsOk()) AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	auto Edit = [&](const FString& Stage, const FString& Name, const TSharedRef<FJsonValue>& Value)
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		int32 Count = 0;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
			if (auto Row = FindRenderTarget2DProperty(Root, Name))
			{
				Row->SetField(TEXT("value"), Value); ++Count;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { AddError(Error); return false; }
			}
		}
		return TestEqual(TEXT("one editable property row"), Count, 1);
	};
	auto Import = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Submit(TEXT("content_graph.import_workspace"), Args);
	};
	auto ColorValue = [](const FLinearColor& Color) { return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
		MakeShared<FJsonValueNumber>(Color.R), MakeShared<FJsonValueNumber>(Color.G), MakeShared<FJsonValueNumber>(Color.B), MakeShared<FJsonValueNumber>(Color.A)}); };
	if (!TestTrue(TEXT("native baseline resource exists and is red"), ReadRenderTargetResource(*RenderTarget, 16, 16, PF_B8G8R8A8, 1, FColor::Red))) return false;
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), TEXT("SizeX"), MakeShared<FJsonValueNumber>(64))
		|| !Edit(TEXT("edit"), TEXT("SizeY"), MakeShared<FJsonValueNumber>(32))
		|| !Edit(TEXT("edit"), TEXT("RenderTargetFormat"), MakeShared<FJsonValueString>(TEXT("RTF_RGBA16f")))
		|| !Edit(TEXT("edit"), TEXT("ClearColor"), ColorValue(FLinearColor::Green))
		|| !Edit(TEXT("edit"), TEXT("bAutoGenerateMips"), MakeShared<FJsonValueBoolean>(true))) return false;
	RenderTarget = nullptr;
	const auto Applied = Import(TEXT("edit"));
	if (!TestTrue(TEXT("public RenderTarget2D workspace import"), Applied.IsOk())) { AddError(Applied.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	RenderTarget = LoadObject<UTextureRenderTarget2D>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("native settings and non-target fields persist"), RenderTarget && RenderTarget->SizeX == 64 && RenderTarget->SizeY == 32
		&& RenderTarget->RenderTargetFormat == RTF_RGBA16f && RenderTarget->ClearColor == FLinearColor::Green && RenderTarget->bAutoGenerateMips
		&& !RenderTarget->GetPackage()->IsDirty() && RenderTarget2DNonTargetState(*RenderTarget).OrderIndependentCompareEqual(NonTarget))) return false;
	if (!TestTrue(TEXT("reloaded RHI size format mip count and clear pixels agree"), ReadRenderTargetResource(*RenderTarget, 64, 32, PF_FloatRGBA, 7, FColor::Green))) return false;
	if (!Export(TEXT("invalid"))) return false;
	TArray<uint8> Before, After;
	if (!FFileHelper::LoadFileToArray(Before, *Filename) || !Edit(TEXT("invalid"), TEXT("RenderTargetFormat"), MakeShared<FJsonValueString>(TEXT("not-a-format")))) return false;
	TestFalse(TEXT("invalid format rejected"), Import(TEXT("invalid")).IsOk());
	TestTrue(TEXT("invalid input preserves native resource and disk"), !RenderTarget->GetPackage()->IsDirty()
		&& ReadRenderTargetResource(*RenderTarget, 64, 32, PF_FloatRGBA, 7, FColor::Green)
		&& FFileHelper::LoadFileToArray(After, *Filename) && !Before.IsEmpty() && Before == After);
	for (const int32 Size : {0})
	{
		const FString Stage = FString::Printf(TEXT("rejected-size-%d"), Size);
		if (!Export(Stage) || !Edit(Stage, TEXT("SizeX"), MakeShared<FJsonValueNumber>(Size))) return false;
		const auto Rejected = Import(Stage);
		TestFalse(TEXT("zero size is invalid"), Rejected.IsOk());
		TestTrue(TEXT("specific size boundary is reported"), Rejected.GetErrorJson().Contains(TEXT("render_target_dimensions_invalid")));
		TestTrue(TEXT("size rejection preserves fields resource and disk"), RenderTarget->SizeX == 64 && RenderTarget->SizeY == 32
			&& !RenderTarget->GetPackage()->IsDirty() && ReadRenderTargetResource(*RenderTarget, 64, 32, PF_FloatRGBA, 7, FColor::Green)
			&& FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
	}
	if (!Export(TEXT("large")) || !Edit(TEXT("large"), TEXT("SizeX"), MakeShared<FJsonValueNumber>(2049))) return false;
	RenderTarget = nullptr;
	const auto Large = Import(TEXT("large"));
	if (!TestTrue(TEXT("size beyond old confirmation threshold imports"), Large.IsOk())) { AddError(Large.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	RenderTarget = LoadObject<UTextureRenderTarget2D>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("large dimensions persist and reach the RHI without shrinking"), RenderTarget
		&& RenderTarget->SizeX == 2049 && ReadRenderTargetResource(*RenderTarget, 2049, 32, PF_FloatRGBA, 12, FColor::Green))) return false;
	if (!Export(TEXT("large-color")) || !Edit(TEXT("large-color"), TEXT("ClearColor"), ColorValue(FLinearColor::Blue))) return false;
	RenderTarget = nullptr;
	if (!TestTrue(TEXT("unrelated edit of a large target imports"), Import(TEXT("large-color")).IsOk())) return false;
	RenderTarget = LoadObject<UTextureRenderTarget2D>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("unrelated edit preserves large resource dimensions"), RenderTarget
		&& ReadRenderTargetResource(*RenderTarget, 2049, 32, PF_FloatRGBA, 12, FColor::Blue))) return false;
	if (!Export(TEXT("restore-size")) || !Edit(TEXT("restore-size"), TEXT("SizeX"), MakeShared<FJsonValueNumber>(64))
		|| !Edit(TEXT("restore-size"), TEXT("ClearColor"), ColorValue(FLinearColor::Green))) return false;
	RenderTarget = nullptr;
	if (!TestTrue(TEXT("restore fixture dimensions"), Import(TEXT("restore-size")).IsOk())) return false;
	RenderTarget = LoadObject<UTextureRenderTarget2D>(nullptr, *ObjectPath);
	if (!FFileHelper::LoadFileToArray(Before, *Filename)) return false;
	UeAgentReflectionAuthoring::FAssetApplyRequest Late;
	Late.TypeId = TEXT("render_target"); Late.AssetPath = PackageName; Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
	auto SizePatch = MakeShared<FJsonObject>(); SizePatch->SetStringField(TEXT("property_path"), TEXT("SizeX")); SizePatch->SetNumberField(TEXT("value"), 32);
	auto ColorPatch = MakeShared<FJsonObject>(); ColorPatch->SetStringField(TEXT("property_path"), TEXT("ClearColor")); ColorPatch->SetField(TEXT("value"), ColorValue(FLinearColor::Blue));
	Late.Properties = {MakeShared<FJsonValueObject>(SizePatch), MakeShared<FJsonValueObject>(ColorPatch)};
	bool SawPublished = false;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		auto* Current = LoadObject<UTextureRenderTarget2D>(nullptr, *ObjectPath);
		SawPublished = Current && ReadRenderTargetResource(*Current, 32, 32, PF_FloatRGBA, 6, FColor::Blue);
		Failure.Diagnostic.Code = TEXT("t208_render_target_late_rejection"); return false;
	};
	RenderTarget = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
	RenderTarget = LoadObject<UTextureRenderTarget2D>(nullptr, *ObjectPath);
	TestTrue(TEXT("published native resource was observed before rejection"), SawPublished);
	TestTrue(TEXT("checkpoint restores native resource and original disk"), LateResult.bRollbackSucceeded
		&& LateResult.Diagnostic.Code == TEXT("t208_render_target_late_rejection") && RenderTarget
		&& ReadRenderTargetResource(*RenderTarget, 64, 32, PF_FloatRGBA, 7, FColor::Green)
		&& RenderTarget2DNonTargetState(*RenderTarget).OrderIndependentCompareEqual(NonTarget)
		&& FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
	TestTrue(TEXT("independent final workspace export"), Export(TEXT("readback")));
	return !HasAnyErrors();
}
