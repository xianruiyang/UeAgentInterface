#include "Misc/AutomationTest.h"
#include "Components/RuntimeVirtualTextureComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "VT/RuntimeVirtualTexture.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "VirtualTexturing.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
TSharedPtr<FJsonObject> FindRvtProperty(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	if (!Object) return nullptr;
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type != EJson::Object) continue;
		const auto Child = Field.Value->AsObject();
		if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
		if (auto Found = FindRvtProperty(Child, Name)) return Found;
	}
	return nullptr;
}

TMap<FString, FString> RvtNonTargetState(URuntimeVirtualTexture& Texture)
{
	TMap<FString, FString> State;
	for (TFieldIterator<FProperty> It(Texture.GetClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Edit) || It->GetName() == TEXT("TileCount")
			|| It->GetName() == TEXT("MaterialType") || It->GetName() == TEXT("bCompressTextures")) continue;
		FString Value;
		It->ExportText_InContainer(0, Value, &Texture, &Texture, &Texture, PPF_None);
		State.Add(It->GetName(), Value);
	}
	return State;
}

bool ReadRvtAllocation(URuntimeVirtualTexture& Texture, uint32 Tiles, uint32 Layers, EPixelFormat Format, FString& Detail)
{
	bool Matches = false;
	ENQUEUE_RENDER_COMMAND(UaiReadRvtAllocation)([&Texture, Tiles, Layers, Format, &Matches, &Detail](FRHICommandListImmediate&)
	{
		const bool ProducerValid = Texture.GetProducerHandle().IsValid();
		const auto* Allocation = Texture.GetAllocatedVirtualTexture();
		if (!Allocation) { Detail = FString::Printf(TEXT("producer=%d allocation=absent"), ProducerValid); return; }
		const auto* Physical = Allocation->GetNumTextureLayers() ? Allocation->GetPhysicalTexture(0) : nullptr;
		Detail = FString::Printf(TEXT("producer=%d tiles=%ux%u tile-size=%u layers=%u physical-format=%d"), ProducerValid,
			Allocation->GetWidthInTiles(), Allocation->GetHeightInTiles(), Allocation->GetVirtualTileSize(), Allocation->GetNumTextureLayers(), Physical ? int32(Physical->GetFormat()) : -1);
		Matches = ProducerValid && Physical && Allocation->GetNumTextureLayers() > 0;
		if (Tiles) Matches = Matches && Allocation->GetWidthInTiles() == Tiles && Allocation->GetHeightInTiles() == Tiles
			&& Allocation->GetVirtualTileSize() == 256 && Allocation->GetNumTextureLayers() == Layers && Physical->GetFormat() == Format;
	});
	FlushRenderingCommands();
	return Matches;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208RuntimeVirtualTextureWorkspaceTest,
	"UAI.Root.T208.RuntimeVirtualTexture.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208RuntimeVirtualTextureWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208RuntimeVirtualTexture/RVT_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-runtime-virtual-texture/native") / Token);
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-runtime-virtual-texture"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	auto* Texture = NewObject<URuntimeVirtualTexture>(CreatePackage(*PackageName), *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	UWorld* World = nullptr;
	URuntimeVirtualTextureComponent* Component = nullptr;
	ON_SCOPE_EXIT
	{
		if (Component) { if (Component->IsRegistered()) Component->UnregisterComponent(); Component->SetVirtualTexture(nullptr); }
		if (World)
		{
			if (GEngine) GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false); World->MarkAsGarbage();
		}
		FlushRenderingCommands();
		if (auto* Current = FindObject<URuntimeVirtualTexture>(nullptr, *ObjectPath); Current && Current->GetPackage()->IsDirty())
			TestTrue(TEXT("settle only owned fixture after assertions"), UPackage::SavePackage(Current->GetPackage(), Current, *Filename, Save));
		if (FPackageName::DoesPackageExist(PackageName))
		{
			auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)}); Args->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
			TestFalse(TEXT("owned RVT package removed"), FPackageName::DoesPackageExist(PackageName));
		}
	};
	if (!TestTrue(TEXT("native fixture saved"), UPackage::SavePackage(Texture->GetPackage(), Texture, *Filename, Save))) return false;
	const auto NonTarget = RvtNonTargetState(*Texture);
	World = UWorld::CreateWorld(EWorldType::Editor, false, FName(*(TEXT("T208RvtWorld_") + Token)), GetTransientPackage());
	if (!TestNotNull(TEXT("owned Editor world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	auto* Actor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("owned RVT actor"), Actor)) return false;
	Component = NewObject<URuntimeVirtualTextureComponent>(Actor, TEXT("RVTComponent"), RF_Transactional);
	Actor->AddInstanceComponent(Component);
	Component->SetVirtualTexture(Texture);
	Component->RegisterComponent();
	World->SendAllEndOfFrameUpdates(); FlushRenderingCommands();
	if (!TestTrue(TEXT("real RVT scene state enabled in this environment"), Component->IsRenderStateCreated())) return false;
	FString ResourceDetail;
	if (!TestTrue(TEXT("baseline producer and physical allocation exist"), ReadRvtAllocation(*Texture, 0, 0, PF_Unknown, ResourceDetail))) { AddError(ResourceDetail); return false; }
	auto Export = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
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
			if (auto Row = FindRvtProperty(Root, Name))
			{
				Row->SetField(TEXT("value"), Value); ++Count;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { AddError(Error); return false; }
			}
		}
		return TestEqual(TEXT("one editable RVT property"), Count, 1);
	};
	auto Import = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args);
	};
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), TEXT("TileCount"), MakeShared<FJsonValueNumber>(2))
		|| !Edit(TEXT("edit"), TEXT("MaterialType"), MakeShared<FJsonValueString>(TEXT("BaseColor")))
		|| !Edit(TEXT("edit"), TEXT("bCompressTextures"), MakeShared<FJsonValueBoolean>(false))) return false;
	const TWeakObjectPtr<URuntimeVirtualTexture> BeforeImportTexture(Texture);
	Texture = nullptr;
	const auto Applied = Import(TEXT("edit"));
	if (!TestTrue(TEXT("public RVT workspace import"), Applied.IsOk())) { AddError(Applied.GetErrorJson()); return false; }
	Texture = LoadObject<URuntimeVirtualTexture>(nullptr, *ObjectPath);
	World->SendAllEndOfFrameUpdates(); FlushRenderingCommands();
	UE_LOG(LogTemp, Display, TEXT("T208 RVT readback present=%d replaced=%d fully-loaded=%d component-bound=%d registered=%d"),
		Texture != nullptr, BeforeImportTexture.Get() != Texture, Texture && Texture->GetPackage()->IsFullyLoaded(),
		Component->GetVirtualTexture() == Texture, Component->IsRegistered());
	if (!TestTrue(TEXT("reloaded native settings and component reference"), Texture && BeforeImportTexture.Get() != Texture
		&& Texture->GetPackage()->IsFullyLoaded() && Component->GetVirtualTexture() == Texture
		&& Texture->GetTileCount() == 4 && Texture->GetSize() == 1024 && Texture->GetMaterialType() == ERuntimeVirtualTextureMaterialType::BaseColor
		&& !Texture->GetCompressTextures() && Texture->GetLayerCount() == 1 && Texture->GetLayerFormat(0) == PF_B8G8R8A8
		&& RvtNonTargetState(*Texture).OrderIndependentCompareEqual(NonTarget))) return false;
	if (!TestTrue(TEXT("public import publishes native VT allocation"), ReadRvtAllocation(*Texture, 4, 1, PF_B8G8R8A8, ResourceDetail))) { AddError(ResourceDetail); return false; }
	TArray<uint8> Before, After;
	if (!FFileHelper::LoadFileToArray(Before, *Filename) || !Export(TEXT("invalid"))
		|| !Edit(TEXT("invalid"), TEXT("MaterialType"), MakeShared<FJsonValueString>(TEXT("not-a-material-type")))) return false;
	TestFalse(TEXT("invalid material enum rejected"), Import(TEXT("invalid")).IsOk());
	TestTrue(TEXT("rejection preserves native allocation and disk"), ReadRvtAllocation(*Texture, 4, 1, PF_B8G8R8A8, ResourceDetail)
		&& FFileHelper::LoadFileToArray(After, *Filename) && !Before.IsEmpty() && Before == After);
	UeAgentReflectionAuthoring::FAssetApplyRequest Late;
	Late.TypeId = TEXT("runtime_virtual_texture"); Late.AssetPath = PackageName; Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
	auto Patch = MakeShared<FJsonObject>(); Patch->SetStringField(TEXT("property_path"), TEXT("TileCount")); Patch->SetNumberField(TEXT("value"), 3);
	Late.Properties = {MakeShared<FJsonValueObject>(Patch)};
	bool SawPublished = false;
	bool EnteredFinalReadback = false;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		EnteredFinalReadback = true;
		auto* Current = LoadObject<URuntimeVirtualTexture>(nullptr, *ObjectPath);
		// Complete the original update's native frame before rejecting it. Recovery
		// cannot rely on that already-consumed dirty notification to rebuild again.
		World->SendAllEndOfFrameUpdates(); FlushRenderingCommands();
		SawPublished = Current && Component->GetVirtualTexture() == Current && ReadRvtAllocation(*Current, 8, 1, PF_B8G8R8A8, ResourceDetail);
		Failure.Diagnostic.Code = TEXT("t208_rvt_late_rejection"); return false;
	};
	Texture = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
	AddInfo(FString::Printf(TEXT("RVT late phase=%s code=%s detail=%s applied=%d final-readback=%d rollback=%d resource=%s"),
		*LateResult.Phase, *LateResult.Diagnostic.Code, *LateResult.Diagnostic.PublicMessage, LateResult.MutationsApplied,
		EnteredFinalReadback, LateResult.bRollbackSucceeded, *ResourceDetail));
	Texture = LoadObject<URuntimeVirtualTexture>(nullptr, *ObjectPath);
	World->SendAllEndOfFrameUpdates(); FlushRenderingCommands();
	TestTrue(TEXT("published allocation observed before rejection"), SawPublished);
	const bool RestoredAllocation = Texture && ReadRvtAllocation(*Texture, 4, 1, PF_B8G8R8A8, ResourceDetail);
	TestTrue(TEXT("checkpoint restores component binding allocation and original disk"), LateResult.bRollbackSucceeded
		&& LateResult.Diagnostic.Code == TEXT("t208_rvt_late_rejection") && Texture && Component->GetVirtualTexture() == Texture && RestoredAllocation
		&& RvtNonTargetState(*Texture).OrderIndependentCompareEqual(NonTarget) && FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
	if (!RestoredAllocation) AddError(ResourceDetail);
	Component->UnregisterComponent(); Component->SetVirtualTexture(nullptr); FlushRenderingCommands();
	Texture = nullptr;
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Texture = LoadObject<URuntimeVirtualTexture>(nullptr, *ObjectPath);
	TestTrue(TEXT("independent reload preserves native settings"), Texture && Texture->GetTileCount() == 4
		&& Texture->GetLayerCount() == 1 && !Texture->GetCompressTextures() && RvtNonTargetState(*Texture).OrderIndependentCompareEqual(NonTarget));
	TestTrue(TEXT("independent final workspace export"), Export(TEXT("readback")));
	return !HasAnyErrors();
}
