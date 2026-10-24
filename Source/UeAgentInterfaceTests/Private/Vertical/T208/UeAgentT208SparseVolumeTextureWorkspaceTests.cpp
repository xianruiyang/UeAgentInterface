#include "Misc/AutomationTest.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "SparseVolumeTexture/SparseVolumeTextureData.h"
#include "SparseVolumeTexture/ISparseVolumeTextureStreamingManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionSparseVolumeTextureObject.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RenderGraphBuilder.h"
#include "RHICommandList.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
TSharedPtr<FJsonObject> FindSvtProperty(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type != EJson::Object) continue;
		const auto Child = Field.Value->AsObject();
		if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
		if (auto Found = FindSvtProperty(Child, Name)) return Found;
	}
	return nullptr;
}

bool InitializeSvt(UStreamableSparseVolumeTexture& Texture, int32 Frames, TextureAddress Address)
{
	if (!Texture.BeginInitialize(Frames)) return false;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		UE::SVT::FTextureDataCreateInfo Info;
		Info.VirtualVolumeAABBMin = FIntVector(0);
		Info.VirtualVolumeAABBMax = FIntVector(32);
		Info.AttributesFormats[0] = PF_R32_FLOAT;
		TArray64<uint8> Voxels;
		Voxels.SetNumUninitialized(32 * 32 * 32 * sizeof(float));
		for (int32 Index = 0; Index < 32 * 32 * 32; ++Index)
		{
			const float Value = float((Index + Frame * 7) % 61 + 1) / 62.f;
			FMemory::Memcpy(Voxels.GetData() + Index * sizeof(float), &Value, sizeof(float));
		}
		UE::SVT::FTextureData Data;
		if (!Data.CreateFromDense(Info, MakeArrayView(Voxels), TArrayView64<uint8>())
			|| !Texture.AppendFrame(Data, FTransform(FVector(2, 3, Frame)))) return false;
	}
	Texture.AddressX = Address;
	return Texture.EndInitialize();
}

bool SameSvtFrames(UStreamableSparseVolumeTexture& Texture, UStreamableSparseVolumeTexture& Expected)
{
	if (Texture.GetNumFrames() != Expected.GetNumFrames() || Texture.VolumeResolution != FIntVector(32)
		|| Texture.FormatA != PF_R32_FLOAT || Texture.FormatB != PF_Unknown || Texture.NumMipLevels < 2
		|| Texture.GetNumMipLevels() != Expected.GetNumMipLevels())
	{
		UE_LOG(LogTemp, Display, TEXT("T208 SVT frame metadata mismatch frames=%d expected=%d resolution=%s formats=%d/%d mips=%d"),
			Texture.GetNumFrames(), Expected.GetNumFrames(), *Texture.VolumeResolution.ToString(), int32(Texture.FormatA), int32(Texture.FormatB), Texture.NumMipLevels);
		return false;
	}
	for (int32 Index = 0; Index < Texture.GetNumFrames(); ++Index)
	{
		auto* Frame = Texture.GetFrame(Index);
		auto* Other = Expected.GetFrame(Index);
		if (!Frame || !Other) return false;
		const bool TransformMatches = Frame->GetFrameTransform().Equals(Other->GetFrameTransform());
		const auto* ActualResource = Frame->GetResources();
		const auto* ExpectedResource = Other->GetResources();
		// FResources::NumMipLevels is set by Build but not serialized on a DDC
		// hit in UE 5.6. The persisted page topology is the per-frame mip oracle.
		const int32 ActualMips = ActualResource->Topology.MipInfo.Num();
		const int32 ExpectedMips = ExpectedResource->Topology.MipInfo.Num();
		if (!TransformMatches || ActualResource->RootData.IsEmpty()
			|| ActualResource->RootData != ExpectedResource->RootData
			|| ActualMips != ExpectedMips || ActualMips != Texture.GetNumMipLevels())
		{
			UE_LOG(LogTemp, Display, TEXT("T208 SVT frame %d mismatch transform=%d root-bytes=%lld/%lld mips=%d/%d"), Index,
				TransformMatches, int64(ActualResource->RootData.Num()), int64(ExpectedResource->RootData.Num()), ActualMips, ExpectedMips);
			return false;
		}
	}
	return true;
}

bool StreamSvt(UStreamableSparseVolumeTexture& Texture)
{
	for (int32 Frame = 0; Frame < Texture.GetNumFrames(); ++Frame)
		USparseVolumeTextureFrame::GetFrameAndIssueStreamingRequest(&Texture, 208, 24.f, float(Frame), 0.f, true, true);
	UE::SVT::GetStreamingManager().Update_GameThread();
	bool Resident = false;
	FString Detail;
	const FName Name = Texture.GetFName();
	const int32 Frames = Texture.GetNumFrames();
	ENQUEUE_RENDER_COMMAND(UaiSvtReadStreaming)([Name, Frames, &Resident, &Detail](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		const auto* Info = UE::SVT::GetStreamingManager().GetStreamingDebugInfo(GraphBuilder);
		Detail = FString::Printf(TEXT("registered=%d target-found=0"), Info->NumSVTs);
		for (int32 Index = 0; Index < Info->NumSVTs; ++Index)
		{
			const auto& Svt = Info->SVTs[Index];
			if (Svt.AssetName != Name || Svt.NumFrames != Frames) continue;
			Resident = true;
			Detail = FString::Printf(TEXT("registered=%d target-found=1 frames=%d"), Info->NumSVTs, Frames);
			for (int32 Frame = 0; Frame < Frames; ++Frame)
			{
				Resident = Resident && Svt.FrameResidencyPercentages[Frame] >= 0.999f;
				Detail += FString::Printf(TEXT(" frame%d-resident=%g-streaming=%g"), Frame,
					Svt.FrameResidencyPercentages[Frame], Svt.FrameStreamingPercentages[Frame]);
			}
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();
	UE_LOG(LogTemp, Display, TEXT("T208 SVT streaming %s"), *Detail);
	return Resident;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208SparseVolumeTextureWorkspaceTest,
	"UAI.Root.T208.SparseVolumeTexture.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208SparseVolumeTextureWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-sparse-volume-texture"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	for (const bool Animated : {false, true})
	{
		const int32 Frames = Animated ? 2 : 1;
		const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = TEXT("/Game/UAI/Transient/T208SparseVolumeTexture/SVT_") + Token;
		const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
		const FString ObjectPath = PackageName + TEXT(".") + Name;
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-sparse-volume-texture/native") / Token);
		UClass* Class = Animated ? UAnimatedSparseVolumeTexture::StaticClass() : UStaticSparseVolumeTexture::StaticClass();
		auto* Texture = NewObject<UStreamableSparseVolumeTexture>(CreatePackage(*PackageName), Class, *Name, RF_Public | RF_Standalone);
		TStrongObjectPtr<UStreamableSparseVolumeTexture> Expected(NewObject<UStreamableSparseVolumeTexture>(GetTransientPackage(), Class));
		TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(GetTransientPackage()));
		FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
		ON_SCOPE_EXIT
		{
			Material.Reset(); Expected.Reset(); Texture = nullptr;
			FlushRenderingCommands();
			if (FPackageName::DoesPackageExist(PackageName))
			{
				auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)}); Args->SetBoolField(TEXT("recursive"), false);
				for (const bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
				TestFalse(TEXT("owned SVT removed"), FPackageName::DoesPackageExist(PackageName));
			}
		};
		if (!TestTrue(TEXT("initialize real native SVT frames"), InitializeSvt(*Texture, Frames, TA_Clamp)
			&& InitializeSvt(*Expected, Frames, TA_Wrap))
			|| !TestTrue(TEXT("save source fixture"), UPackage::SavePackage(Texture->GetPackage(), Texture, *Filename, Save))) return false;
		// EndInitialize only creates frame render resources. Build the independent
		// expected data through the native editor notification before authoring;
		// never repair the edited target to make its readback pass.
		Expected->PostEditChange();
		if (!TestTrue(TEXT("independent expected native data built"), SameSvtFrames(*Expected, *Expected))) return false;
		// EndInitialize loads the child frames only. The asset's own PostLoad
		// registers it with the streamer; enter that real load lifecycle before
		// testing authoring, instead of repairing resources after an import.
		Texture = nullptr;
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
		Texture = LoadObject<UStreamableSparseVolumeTexture>(nullptr, *ObjectPath);
		if (!TestNotNull(TEXT("native fixture loaded for streaming"), Texture)) return false;
		auto* Expression = NewObject<UMaterialExpressionSparseVolumeTextureObject>(Material.Get());
		Expression->SparseVolumeTexture = Texture;
		Material->GetExpressionCollection().AddExpression(Expression);
		Material->PostEditChange();
		if (!TestTrue(TEXT("material has real SVT reference"), Material->GetReferencedTextures().Contains(Texture))) return false;
		const FGuid BeforeMaterial = Material->StateId;
		if (!TestTrue(TEXT("baseline full-resolution streaming"), StreamSvt(*Texture))) return false;
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
				if (auto Row = FindSvtProperty(Root, Field))
				{
					Row->SetField(TEXT("value"), Value); ++Count;
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
						|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				}
			}
			return TestEqual(TEXT("one editable SVT field"), Count, 1);
		};
		auto Import = [&](const FString& Stage)
		{
			auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
			Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args);
		};
		if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), TEXT("AddressX"), MakeShared<FJsonValueString>(TEXT("TA_Wrap")))
			|| !Edit(TEXT("edit"), TEXT("StreamingPoolSizeFactor"), MakeShared<FJsonValueNumber>(4.0))
			|| !Edit(TEXT("edit"), TEXT("NumberOfPrefetchFrames"), MakeShared<FJsonValueNumber>(1))) return false;
		const auto Applied = Import(TEXT("edit"));
		if (!TestTrue(TEXT("public SVT workspace import"), Applied.IsOk())) { AddError(Applied.GetErrorJson()); return false; }
		Texture = LoadObject<UStreamableSparseVolumeTexture>(nullptr, *ObjectPath);
		if (!TestNotNull(TEXT("SVT exists after import reload"), Texture)) return false;
		bool ReadbackValid = TestTrue(TEXT("native concrete class preserved"), Texture->GetClass() == Class);
		ReadbackValid &= TestTrue(TEXT("reloaded package fully loaded"), Texture->GetPackage()->IsFullyLoaded());
		ReadbackValid &= TestTrue(TEXT("native addressing edited and other axes preserved"), Texture->AddressX == TA_Wrap && Texture->AddressY == TA_Clamp && Texture->AddressZ == TA_Clamp);
		ReadbackValid &= TestTrue(TEXT("native streaming settings edited and other fields preserved"), Texture->StreamingPoolSizeFactor == 4.f && Texture->NumberOfPrefetchFrames == 1
			&& Texture->PrefetchPercentageStepSize == 20.f && Texture->PrefetchPercentageBias == 20.f);
		ReadbackValid &= TestTrue(TEXT("native frame content matches independently built expected"), SameSvtFrames(*Texture, *Expected));
		ReadbackValid &= TestTrue(TEXT("material expression rebound to reloaded asset"), Expression->SparseVolumeTexture == Texture);
		ReadbackValid &= TestTrue(TEXT("native material notification changed StateId"), Material->StateId != BeforeMaterial);
		if (!ReadbackValid) return false;
		TestTrue(TEXT("edited native streaming reaches full residency"), StreamSvt(*Texture));
		TArray<uint8> Before, After;
		if (!FFileHelper::LoadFileToArray(Before, *Filename) || !Export(TEXT("invalid"))
			|| !Edit(TEXT("invalid"), TEXT("AddressX"), MakeShared<FJsonValueString>(TEXT("invalid-address")))) return false;
		TestFalse(TEXT("invalid address rejected"), Import(TEXT("invalid")).IsOk());
		TestTrue(TEXT("rejection preserves native data and disk"), SameSvtFrames(*Texture, *Expected)
			&& FFileHelper::LoadFileToArray(After, *Filename) && !Before.IsEmpty() && Before == After);
		if (Animated)
		{
			UeAgentReflectionAuthoring::FAssetApplyRequest Late;
			Late.TypeId = TEXT("sparse_volume_texture"); Late.AssetPath = PackageName;
			Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
			auto Patch = MakeShared<FJsonObject>(); Patch->SetStringField(TEXT("property_path"), TEXT("AddressX")); Patch->SetStringField(TEXT("value"), TEXT("TA_Mirror"));
			Late.Properties = {MakeShared<FJsonValueObject>(Patch)};
			bool Published = false;
			Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
			{
				auto* Current = LoadObject<UStreamableSparseVolumeTexture>(nullptr, *ObjectPath);
				Published = Current && Current->AddressX == TA_Mirror && Expression->SparseVolumeTexture == Current && StreamSvt(*Current);
				Failure.ErrorCode = TEXT("t208_svt_late_rejection"); return false;
			};
			Texture = nullptr;
			UeAgentReflectionAuthoring::FAssetApplyResult Result;
			TestFalse(TEXT("late SVT rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, Result));
			Texture = LoadObject<UStreamableSparseVolumeTexture>(nullptr, *ObjectPath);
			TestTrue(TEXT("late rejection observed published frame streaming"), Published);
			TestTrue(TEXT("late recovery restores references, native frames and disk"), Result.bRollbackSucceeded
				&& Result.ErrorCode == TEXT("t208_svt_late_rejection") && Texture && Texture->AddressX == TA_Wrap
				&& Expression->SparseVolumeTexture == Texture && SameSvtFrames(*Texture, *Expected) && StreamSvt(*Texture)
				&& FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
		}
		Expression->SparseVolumeTexture = nullptr;
		Material.Reset(); Texture = nullptr;
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
		Texture = LoadObject<UStreamableSparseVolumeTexture>(nullptr, *ObjectPath);
		TestTrue(TEXT("independent disk reload preserves frames and settings"), Texture && SameSvtFrames(*Texture, *Expected)
			&& Texture->AddressX == TA_Wrap && Texture->StreamingPoolSizeFactor == 4.f && Texture->NumberOfPrefetchFrames == 1);
		TestTrue(TEXT("independent final workspace export"), Export(TEXT("readback")));
	}
	return !HasAnyErrors();
}

// A native dense fixture supplies real imported-style data to the separate SDK
// consumer. UE's SVT factory cannot create initialized assets without a VDB file.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208SparseVolumeTextureSdkFixtureTest,
	"UAI.Root.T208.SparseVolumeTexture.SDKFixture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUeAgentT208SparseVolumeTextureSdkFixtureTest::RunTest(const FString&)
{
	const FString Name = TEXT("SVT_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208SparseVolumeTextureSDK/") + Name;
	auto* Texture = NewObject<UStaticSparseVolumeTexture>(CreatePackage(*PackageName), *Name, RF_Public | RF_Standalone);
	bool Published = false;
	ON_SCOPE_EXIT { if (!Published) ObjectTools::DeleteObjects({Texture}, false); };
	if (!TestTrue(TEXT("native initialized SDK fixture"), InitializeSvt(*Texture, 1, TA_Clamp))) return false;
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!TestTrue(TEXT("SDK fixture persisted"), UPackage::SavePackage(Texture->GetPackage(), Texture, *Filename, Save))) return false;
	const FString Output = FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-sparse-volume-texture/sdk-fixture.json");
	const FString Document = FString::Printf(TEXT("{\"target\":\"%s.%s\",\"native_initialized_frames\":1}"), *PackageName, *Name);
	Published = FFileHelper::SaveStringToFile(Document, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return TestTrue(TEXT("SDK fixture public target handoff"), Published);
}
