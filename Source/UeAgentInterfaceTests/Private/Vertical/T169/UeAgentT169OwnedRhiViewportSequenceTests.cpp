#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "Slate/SceneViewport.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/GarbageCollection.h"
#include "UeAgentInterfaceSettings.h"

#define UI OpenSslUi
#include <openssl/sha.h>
#undef UI

namespace UE::UAI::Tests::Vertical::T169
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;

	constexpr const TCHAR* ValidationMapPackage =
		TEXT("/Game/UAI/Transient/T169/LV_T169_ViewportSequence");

	FString Sha256Bytes(const TArray<uint8>& Bytes)
	{
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		SHA256(Bytes.GetData(), Bytes.Num(), Digest);
		return TEXT("sha256:")
			+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
	}

	FLevelEditorViewportClient* ResolvePerspectiveViewport(
		UWorld* World,
		int32& OutIndex)
	{
		OutIndex = INDEX_NONE;
		if (!GEditor || !World)
		{
			return nullptr;
		}
		const TArray<FLevelEditorViewportClient*>& Clients =
			GEditor->GetLevelViewportClients();
		for (int32 Index = 0; Index < Clients.Num(); ++Index)
		{
			if (Clients[Index]
				&& Clients[Index]->Viewport
				&& Clients[Index]->GetWorld() == World
				&& Clients[Index]->IsPerspective())
			{
				OutIndex = Index;
				return Clients[Index];
			}
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> ViewportParams(
		const FString& ViewportIdentity,
		const TSharedRef<FJsonObject>& Input)
	{
		const TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetStringField(TEXT("viewport_identity"), ViewportIdentity);
		Context->SetStringField(TEXT("world_type"), TEXT("editor"));
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), ViewportIdentity);
		Params->SetObjectField(TEXT("input"), Input);
		Params->SetObjectField(TEXT("context"), Context);
		return Params;
	}

	class FOwnedRhiViewportSequenceLatentCommand final
		: public IAutomationLatentCommand
	{
	public:
		FOwnedRhiViewportSequenceLatentCommand(
			FAutomationTestBase* InTest,
			TSharedPtr<const FUeAgentImmutableCommandRegistry> InRegistry,
			TSharedPtr<const FUeAgentExactCommandDispatch> InDispatch)
			: Test(InTest)
			, Registry(MoveTemp(InRegistry))
			, Dispatch(MoveTemp(InDispatch))
			, StartedAt(FPlatformTime::Seconds())
		{
		}

		~FOwnedRhiViewportSequenceLatentCommand() override
		{
			Cleanup();
		}

		bool Update() override
		{
			if (!Test || !Registry.IsValid() || !Dispatch.IsValid())
			{
				return Finish();
			}
			if (FPlatformTime::Seconds() - StartedAt > 300.0)
			{
				Test->AddError(FString::Printf(
					TEXT("T169 vertical scenario timed out at stage %d"), Stage));
				return Finish();
			}

			if (Stage == 0)
			{
				if (!SetupOwnedSceneAndCatalog())
				{
					return Finish();
				}
				Stage = 1;
				return false;
			}
			if (Stage == 1)
			{
				if (ModeIndex < ModeIds.Num())
				{
					if (!StartCapture(
							TEXT("policy:image-capture:viewport/v1"),
							MakeSingleParameters(ModeIds[ModeIndex])))
					{
						return Finish();
					}
					Stage = 2;
					return false;
				}
				Stage = 3;
				return false;
			}
			if (Stage == 2)
			{
				TSharedPtr<FJsonObject> Status;
				if (!PollUntilTerminal(Status))
				{
					return OperationRef.IsValid() ? false : Finish();
				}
				if (!VerifySingleMode(Status, ModeIds[ModeIndex])
					|| !ReleaseOperation())
				{
					return Finish();
				}
				++ModeIndex;
				Stage = 1;
				return false;
			}
			if (Stage == 3)
			{
				if (!StartFixtureMotion())
				{
					return Finish();
				}
				MovingRotationBefore = MovingActor.IsValid()
					? MovingActor->GetActorRotation() : FRotator::ZeroRotator;
				if (!StartCapture(
						TEXT("policy:image-capture:viewport-sequence/v1"),
						MakeSequenceParameters()))
				{
					return Finish();
				}
				Stage = 4;
				return false;
			}
			if (Stage == 4)
			{
				TSharedPtr<FJsonObject> Status;
				if (!PollUntilTerminal(Status))
				{
					return OperationRef.IsValid() ? false : Finish();
				}
				if (!VerifySequence(Status) || !ReleaseOperation())
				{
					return Finish();
				}
				return Finish();
			}
			return Finish();
		}

	private:
		FUeAgentCommandResult Call(
			const FString& CommandId,
			const TSharedRef<FJsonObject>& Params) const
		{
			return UE::UAI::Tests::PublicCommand::SubmitActiveCommandWithScopes(
				Registry,
				Dispatch,
				CommandId,
				Params,
				TEXT("t169-owned-rhi-viewport-sequence"),
				{TEXT("uai:query"), TEXT("uai:operate"),
					TEXT("uai:operation"), TEXT("uai:capture")});
		}

		bool RequireOk(
			const FString& StageName,
			const FUeAgentCommandResult& Result) const
		{
			if (!Result.IsOk())
			{
				Test->AddError(StageName + TEXT(": ") + Result.GetErrorJson());
				return false;
			}
			return true;
		}

		bool SetupOwnedSceneAndCatalog()
		{
			if (!Test->TestTrue(TEXT("T169 owned worker has an RHI"),
					FApp::CanEverRender())
				|| !Test->TestNotNull(TEXT("T169 has an Editor"), GEditor))
			{
				return false;
			}
			UWorld* const OriginalWorld = GEditor->GetEditorWorldContext().World();
			if (!Test->TestNotNull(TEXT("T169 has an original world"), OriginalWorld)
				|| !OriginalWorld->GetPackage())
			{
				return false;
			}
			OriginalWorldPackage = OriginalWorld->GetPackage()->GetName();
			OriginalWorldFile = FPackageName::LongPackageNameToFilename(
				OriginalWorldPackage, FPackageName::GetMapPackageExtension());
			if (!Test->TestTrue(TEXT("T169 original map is reloadable"),
					IFileManager::Get().FileExists(*OriginalWorldFile)))
			{
				return false;
			}

			ValidationMapFile = FPackageName::LongPackageNameToFilename(
				ValidationMapPackage, FPackageName::GetMapPackageExtension());
			if (IFileManager::Get().FileExists(*ValidationMapFile)
				&& FindPackage(nullptr, ValidationMapPackage) == nullptr)
			{
				if (!Test->TestTrue(
						TEXT("T169 recovers its exact stale crash fixture"),
						IFileManager::Get().Delete(
							*ValidationMapFile, false, true, true)))
				{
					return false;
				}
			}
			if (!Test->TestFalse(TEXT("T169 validation map is initially absent"),
					IFileManager::Get().FileExists(*ValidationMapFile)
						|| FindPackage(nullptr, ValidationMapPackage) != nullptr))
			{
				return false;
			}

			if (UUeAgentInterfaceSettings* Settings =
				GetMutableDefault<UUeAgentInterfaceSettings>())
			{
				bOriginalScreenshots = Settings->bEnableScreenshots;
				Settings->bEnableScreenshots = true;
				bSettingsChanged = true;
			}
			else
			{
				Test->AddError(TEXT("T169 screenshot settings are unavailable"));
				return false;
			}

			UWorld* const World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
			if (!Test->TestNotNull(TEXT("T169 creates one blank Editor map"), World))
			{
				return false;
			}
			if (World->PersistentLevel)
			{
				World->PersistentLevel->SetUseExternalActors(false);
			}
			UStaticMesh* const Cube = LoadObject<UStaticMesh>(
				nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (!Test->TestNotNull(TEXT("T169 loads the Engine cube fixture"), Cube))
			{
				return false;
			}

			auto SpawnCube = [World, Cube](
				const TCHAR* Label,
				const FVector& Location,
				const FVector& Scale)
			{
				AStaticMeshActor* const Actor = World->SpawnActor<AStaticMeshActor>();
				if (!Actor)
				{
					return static_cast<AStaticMeshActor*>(nullptr);
				}
				Actor->SetActorLabel(Label);
				Actor->SetActorLocation(Location);
				Actor->SetActorScale3D(Scale);
				Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
				Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
				Actor->GetStaticMeshComponent()->SetCollisionEnabled(
					ECollisionEnabled::QueryAndPhysics);
				Actor->GetStaticMeshComponent()->UpdateBounds();
				return Actor;
			};
			AStaticMeshActor* const Floor = SpawnCube(
				TEXT("T169_Floor"), FVector(0.0, 0.0, -25.0),
				FVector(12.0, 8.0, 0.5));
			AStaticMeshActor* const Moving = SpawnCube(
				TEXT("T169_RotatingBar"), FVector(0.0, 0.0, 150.0),
				FVector(1.0, 4.0, 1.0));
			if (!Test->TestNotNull(TEXT("T169 creates the floor"), Floor)
				|| !Test->TestNotNull(TEXT("T169 creates the rotating subject"), Moving))
			{
				return false;
			}
			ADirectionalLight* const Sun = World->SpawnActor<ADirectionalLight>();
			if (!Test->TestNotNull(TEXT("T169 creates normal Level lighting"), Sun))
			{
				return false;
			}
			Sun->SetActorRotation(FRotator(-45.0, -35.0, 0.0));
			Sun->GetLightComponent()->SetIntensity(5.0f);
			MovingActor = Moving;
			const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(
				World, ValidationMapPackage);
			bOwnsValidationMap = IFileManager::Get().FileExists(*ValidationMapFile);
			if (!Test->TestTrue(TEXT("T169 saves its exact owned map"),
					bSaved && bOwnsValidationMap))
			{
				return false;
			}

			int32 ViewportIndex = INDEX_NONE;
			FLevelEditorViewportClient* const Client =
				ResolvePerspectiveViewport(World, ViewportIndex);
			if (!Test->TestNotNull(TEXT("T169 resolves one perspective viewport"), Client))
			{
				return false;
			}
			ViewportIdentity = FString::Printf(TEXT("level:%d"), ViewportIndex);
			OriginalViewportSize = Client->Viewport->GetSizeXY();
			if (OriginalViewportSize.X <= 0 || OriginalViewportSize.Y <= 0)
			{
				static_cast<FSceneViewport*>(Client->Viewport)->SetViewportSize(1280, 720);
				bResizedViewport = true;
			}
			Client->SetViewLocation(FVector(-1200.0, -1200.0, 850.0));
			Client->SetViewRotation(
				(FVector(0.0, 0.0, 150.0) - Client->GetViewLocation()).Rotation());
			Client->ViewFOV = 55.0;
			Client->SetViewMode(VMI_Unlit);
			Client->SetRealtime(false);
			Client->SetGameView(true);
			Client->Invalidate();
			BaselineLocation = Client->GetViewLocation();
			BaselineRotation = Client->GetViewRotation();
			BaselineRealtime = Client->IsRealtime();
			BaselineViewMode = Client->GetViewMode();

			const FUeAgentCommandResult Modes = Call(
				TEXT("viewport.view_mode.list"),
				ViewportParams(ViewportIdentity, MakeShared<FJsonObject>()));
			if (!RequireOk(TEXT("dynamic view-mode catalog"), Modes))
			{
				return false;
			}
			for (const FString& Mode : ModeIds)
			{
				if (!Test->TestTrue(TEXT("catalog contains ") + Mode,
						Modes.GetPayloadJson().Contains(Mode)))
				{
					return false;
				}
			}

			EvidenceDirectory = FPaths::ProjectDir()
				/ TEXT("tmp/TROOT-UAI-169-rhi-vertical/artifacts");
			IFileManager::Get().MakeDirectory(*EvidenceDirectory, true);
			return true;
		}

		bool StartFixtureMotion()
		{
			if (!MovingActor.IsValid() || FixtureMotionTicker.IsValid())
			{
				return Test->TestTrue(
					TEXT("T169 fixture motion has one valid owner"),
					MovingActor.IsValid() && !FixtureMotionTicker.IsValid());
			}
			const TWeakObjectPtr<AStaticMeshActor> WeakActor = MovingActor;
			FixtureMotionTicker = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[WeakActor](float)
					{
						if (!WeakActor.IsValid())
						{
							return false;
						}
						WeakActor->AddActorWorldRotation(
							FRotator(0.0, 7.5, 0.0), false, nullptr,
							ETeleportType::TeleportPhysics);
						return true;
					}),
				0.0f);
			return Test->TestTrue(
				TEXT("T169 schedules one real-engine-frame change source"),
				FixtureMotionTicker.IsValid());
		}

		TSharedRef<FJsonObject> MakeSingleParameters(const FString& Mode) const
		{
			const TSharedRef<FJsonObject> Parameters = MakeShared<FJsonObject>();
			Parameters->SetStringField(TEXT("format"), TEXT("png"));
			Parameters->SetNumberField(TEXT("max_size"), 512);
			Parameters->SetNumberField(TEXT("warmup_frames"), 2);
			Parameters->SetStringField(TEXT("view_mode"), Mode);
			Parameters->SetBoolField(
				TEXT("restore_view_mode_after_capture"), true);
			return Parameters;
		}

		TSharedRef<FJsonObject> MakeSequenceParameters() const
		{
			const TSharedRef<FJsonObject> Parameters = MakeShared<FJsonObject>();
			Parameters->SetStringField(TEXT("format"), TEXT("png"));
			Parameters->SetNumberField(TEXT("max_size"), 512);
			Parameters->SetNumberField(TEXT("frame_count"), 4);
			Parameters->SetNumberField(TEXT("interval_frames"), 8);
			Parameters->SetNumberField(TEXT("warmup_frames"), 2);
			Parameters->SetNumberField(TEXT("frame_sheet_padding"), 8);
			Parameters->SetStringField(TEXT("view_mode"), TEXT("lit"));
			return Parameters;
		}

		bool StartCapture(
			const FString& PolicyId,
			const TSharedRef<FJsonObject>& Parameters)
		{
			const TSharedRef<FJsonObject> Capture = MakeShared<FJsonObject>();
			Capture->SetStringField(
				TEXT("subject_ref"), TEXT("capture-subject:viewport:active"));
			Capture->SetStringField(TEXT("policy_id"), PolicyId);
			Capture->SetObjectField(TEXT("parameters"), Parameters);
			TSharedPtr<FJsonObject> Payload;
			return ReadOperationResult(
				Call(TEXT("capture.image.execute"), Capture),
				Payload,
				TEXT("capture start ") + PolicyId);
		}

		bool ReadOperationResult(
			const FUeAgentCommandResult& Result,
			TSharedPtr<FJsonObject>& OutPayload,
			const FString& StageName)
		{
			OutPayload.Reset();
			FString Error;
			const TSharedPtr<FJsonObject>* Ref = nullptr;
			if (!Result.IsOk()
				|| !UE::UAI::CommandContract::Json::TryParseObject(
					Result.GetPayloadJson(), OutPayload, Error)
				|| !OutPayload.IsValid()
				|| !OutPayload->TryGetObjectField(TEXT("operation_ref"), Ref)
				|| !Ref || !Ref->IsValid())
			{
				Test->AddError(StageName + TEXT(": ")
					+ (Result.IsOk() ? Result.GetPayloadJson() : Result.GetErrorJson()));
				OperationRef.Reset();
				return false;
			}
			OperationRef = *Ref;
			return true;
		}

		TSharedRef<FJsonObject> OperationParams() const
		{
			const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			if (OperationRef.IsValid())
			{
				Params->SetObjectField(TEXT("operation_ref"), OperationRef.ToSharedRef());
			}
			return Params;
		}

		bool PollUntilTerminal(TSharedPtr<FJsonObject>& OutStatus)
		{
			if (!ReadOperationResult(
					Call(TEXT("operation.status"), OperationParams()),
					OutStatus,
					TEXT("capture status")))
			{
				return false;
			}
			if (!OutStatus->GetBoolField(TEXT("terminal")))
			{
				const int64 Generation = OutStatus->GetIntegerField(TEXT("generation"));
				const TSharedRef<FJsonObject> Resume = OperationParams();
				Resume->SetNumberField(TEXT("expected_generation"), Generation);
				if (!ReadOperationResult(
						Call(TEXT("operation.resume"), Resume),
						OutStatus,
						TEXT("capture resume")))
				{
					return false;
				}
				if (!OutStatus->GetBoolField(TEXT("terminal")))
				{
					return false;
				}
			}
			FString ErrorCode;
			OutStatus->TryGetStringField(TEXT("error_code"), ErrorCode);
			if (!ErrorCode.IsEmpty())
			{
				Test->AddError(TEXT("capture terminated with ") + ErrorCode);
				OperationRef.Reset();
				return false;
			}
			return true;
		}

		bool ResolveTerminalPayload(
			const TSharedPtr<FJsonObject>& Status,
			TSharedPtr<FJsonObject>& OutPayload) const
		{
			const TSharedPtr<FJsonObject>* Signal = nullptr;
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			if (!Status.IsValid()
				|| !Status->TryGetObjectField(TEXT("latest_signal"), Signal)
				|| !Signal || !Signal->IsValid()
				|| !(*Signal)->TryGetObjectField(TEXT("payload"), Payload)
				|| !Payload || !Payload->IsValid())
			{
				Test->AddError(TEXT("terminal capture payload is missing"));
				return false;
			}
			OutPayload = *Payload;
			return true;
		}

		bool VerifyArtifact(
			const TSharedPtr<FJsonObject>& Artifact,
			const FString& EvidenceName,
			FString& OutLocator)
		{
			FString ContentHash;
			double ByteCount = -1.0;
			if (!Artifact.IsValid()
				|| !Artifact->TryGetStringField(TEXT("locator"), OutLocator)
				|| !Artifact->TryGetStringField(TEXT("content_hash"), ContentHash)
				|| !Artifact->TryGetNumberField(TEXT("byte_count"), ByteCount))
			{
				Test->AddError(TEXT("invalid artifact_ref for ") + EvidenceName);
				return false;
			}
			OutLocator = FPaths::ConvertRelativePathToFull(OutLocator);
			TArray<uint8> Bytes;
			if (!Test->TestTrue(TEXT("artifact is readable: ") + EvidenceName,
					FFileHelper::LoadFileToArray(Bytes, *OutLocator))
				|| !Test->TestEqual(TEXT("artifact byte count: ") + EvidenceName,
					static_cast<int64>(Bytes.Num()), static_cast<int64>(ByteCount))
				|| !Test->TestEqual(TEXT("artifact hash: ") + EvidenceName,
					Sha256Bytes(Bytes).ToLower(), ContentHash.ToLower()))
			{
				return false;
			}
			FImage Image;
			if (!Test->TestTrue(TEXT("artifact decodes: ") + EvidenceName,
					FImageUtils::LoadImage(*OutLocator, Image))
				|| !Test->TestTrue(TEXT("artifact has useful dimensions: ") + EvidenceName,
					Image.SizeX >= 128 && Image.SizeY >= 72))
			{
				return false;
			}
			const FString EvidencePath = EvidenceDirectory / EvidenceName;
			if (!Test->TestEqual(TEXT("preserve task visual: ") + EvidenceName,
					IFileManager::Get().Copy(
						*EvidencePath, *OutLocator, true, true),
					COPY_OK))
			{
				return false;
			}
			PublishedArtifacts.AddUnique(OutLocator);
			return true;
		}

		bool VerifyViewportRestored(const FString& StageName) const
		{
			UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			int32 Index = INDEX_NONE;
			FLevelEditorViewportClient* const Client =
				ResolvePerspectiveViewport(World, Index);
			return Test->TestNotNull(TEXT("viewport remains available: ") + StageName, Client)
				&& Test->TestEqual(TEXT("view mode restored: ") + StageName,
					Client->GetViewMode(), BaselineViewMode)
				&& Test->TestEqual(TEXT("realtime restored: ") + StageName,
					Client->IsRealtime(), BaselineRealtime)
				&& Test->TestTrue(TEXT("camera location unchanged: ") + StageName,
					Client->GetViewLocation().Equals(BaselineLocation, 0.01))
				&& Test->TestTrue(TEXT("camera rotation unchanged: ") + StageName,
					Client->GetViewRotation().Equals(BaselineRotation, 0.01));
		}

		bool VerifySingleMode(
			const TSharedPtr<FJsonObject>& Status,
			const FString& Mode)
		{
			TSharedPtr<FJsonObject> Payload;
			const TSharedPtr<FJsonObject>* Artifact = nullptr;
			const TSharedPtr<FJsonObject>* Receipt = nullptr;
			if (!ResolveTerminalPayload(Status, Payload)
				|| !Payload->TryGetObjectField(TEXT("artifact_ref"), Artifact)
				|| !Artifact || !Artifact->IsValid()
				|| !Payload->TryGetObjectField(TEXT("capture_receipt"), Receipt)
				|| !Receipt || !Receipt->IsValid())
			{
				Test->AddError(TEXT("single capture contract is incomplete: ") + Mode);
				return false;
			}
			FString Locator;
			if (!VerifyArtifact(*Artifact, TEXT("single-") + Mode + TEXT(".png"), Locator))
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Applied = nullptr;
			FString AppliedMode;
			if (!(*Receipt)->TryGetObjectField(TEXT("view_mode_applied"), Applied)
				|| !Applied || !Applied->IsValid()
				|| !(*Applied)->TryGetStringField(TEXT("view_mode_name"), AppliedMode)
				|| !Test->TestEqual(TEXT("receipt applies requested mode"),
					AppliedMode, Mode))
			{
				return false;
			}
			return VerifyViewportRestored(TEXT("single ") + Mode);
		}

		bool VerifySequence(const TSharedPtr<FJsonObject>& Status)
		{
			TSharedPtr<FJsonObject> Payload;
			const TSharedPtr<FJsonObject>* SheetArtifact = nullptr;
			const TSharedPtr<FJsonObject>* Receipt = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Additional = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
			if (!ResolveTerminalPayload(Status, Payload)
				|| !Payload->TryGetObjectField(TEXT("artifact_ref"), SheetArtifact)
				|| !SheetArtifact || !SheetArtifact->IsValid()
				|| !Payload->TryGetArrayField(TEXT("additional_artifact_refs"), Additional)
				|| !Additional
				|| !Payload->TryGetObjectField(TEXT("capture_receipt"), Receipt)
				|| !Receipt || !Receipt->IsValid()
				|| !(*Receipt)->TryGetArrayField(TEXT("frames"), Frames)
				|| !Frames)
			{
				Test->AddError(TEXT("sequence terminal contract is incomplete"));
				return false;
			}
			if (!Test->TestEqual(TEXT("sequence publishes four raw artifacts"),
					Additional->Num(), 4)
				|| !Test->TestEqual(TEXT("sequence receipt maps four frames"),
					Frames->Num(), 4))
			{
				return false;
			}
			FString SheetLocator;
			if (!VerifyArtifact(
					*SheetArtifact, TEXT("sequence-sheet.png"), SheetLocator))
			{
				return false;
			}

			TSet<FString> PixelHashes;
			double PreviousSample = -1.0;
			for (int32 Index = 0; Index < Frames->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* Frame = nullptr;
				const TSharedPtr<FJsonObject>* Artifact = nullptr;
				FString PixelHash;
				FString Mode;
				double FrameIndex = -1.0;
				double RawArtifactIndex = -1.0;
				double Sample = -1.0;
				if (!(*Frames)[Index].IsValid()
					|| !(*Frames)[Index]->TryGetObject(Frame)
					|| !Frame || !Frame->IsValid()
					|| !(*Frame)->TryGetNumberField(TEXT("frame_index"), FrameIndex)
					|| !(*Frame)->TryGetNumberField(
						TEXT("raw_artifact_index"), RawArtifactIndex)
					|| !(*Frame)->TryGetNumberField(TEXT("sample_time_seconds"), Sample)
					|| !(*Frame)->TryGetStringField(TEXT("pixel_sha256"), PixelHash)
					|| !(*Frame)->TryGetStringField(TEXT("view_mode_name"), Mode)
					|| !(*Additional)[Index].IsValid()
					|| !(*Additional)[Index]->TryGetObject(Artifact)
					|| !Artifact || !Artifact->IsValid())
				{
					Test->AddError(FString::Printf(
						TEXT("sequence mapping is invalid at frame %d"), Index));
					return false;
				}
				if (!Test->TestEqual(TEXT("frame index is ordered"),
						static_cast<int32>(FrameIndex), Index)
					|| !Test->TestEqual(TEXT("raw artifact mapping is ordered"),
						static_cast<int32>(RawArtifactIndex), Index)
					|| !Test->TestTrue(TEXT("sample time is strictly monotonic"),
						Sample > PreviousSample)
					|| !Test->TestEqual(TEXT("sequence keeps one lit mode"),
						Mode, FString(TEXT("lit"))))
				{
					return false;
				}
				PreviousSample = Sample;
				PixelHashes.Add(PixelHash);
				FString RawLocator;
				if (!VerifyArtifact(
						*Artifact,
						FString::Printf(TEXT("sequence-frame-%03d.png"), Index),
						RawLocator))
				{
					return false;
				}
			}
			const bool bActorAvailable = MovingActor.IsValid();
			const FRotator MovingRotationAfter = bActorAvailable
				? MovingActor->GetActorRotation() : FRotator::ZeroRotator;
			Test->AddInfo(FString::Printf(
				TEXT("T169 sequence change: unique_hashes=%d rotation=%s -> %s"),
				PixelHashes.Num(),
				*MovingRotationBefore.ToString(),
				*MovingRotationAfter.ToString()));
			if (!Test->TestTrue(TEXT("real editor ticks produce visible frame changes"),
					PixelHashes.Num() >= 2)
				|| !Test->TestTrue(TEXT("rotating subject remains available"),
					bActorAvailable)
				|| !Test->TestFalse(TEXT("real actor transform changes during sequence"),
					MovingRotationAfter.Equals(MovingRotationBefore, 0.1))
				|| !VerifyViewportRestored(TEXT("sequence")))
			{
				return false;
			}
			Test->AddInfo(FString::Printf(
				TEXT("T169 artifacts: %s; unique_frame_hashes=%d"),
				*EvidenceDirectory, PixelHashes.Num()));
			return true;
		}

		bool ReleaseOperation()
		{
			const FUeAgentCommandResult Receipt =
				Call(TEXT("operation.receipt"), OperationParams());
			if (!RequireOk(TEXT("operation receipt"), Receipt)
				|| !Receipt.GetPayloadJson().Contains(
					TEXT("uai.long_running.receipt.v1")))
			{
				return false;
			}
			const FUeAgentCommandResult Released =
				Call(TEXT("operation.release"), OperationParams());
			if (!RequireOk(TEXT("operation release"), Released))
			{
				return false;
			}
			OperationRef.Reset();
			return true;
		}

		bool Finish()
		{
			Cleanup();
			return true;
		}

		void Cleanup()
		{
			if (bCleaned)
			{
				return;
			}
			bCleaned = true;
			if (FixtureMotionTicker.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(FixtureMotionTicker);
				FixtureMotionTicker.Reset();
			}
			if (OperationRef.IsValid() && Registry.IsValid() && Dispatch.IsValid())
			{
				const TSharedRef<FJsonObject> Cancel = OperationParams();
				Cancel->SetStringField(TEXT("reason"), TEXT("t169_vertical_cleanup"));
				Call(TEXT("operation.cancel"), Cancel);
				Call(TEXT("operation.release"), OperationParams());
				OperationRef.Reset();
			}
			for (const FString& Path : PublishedArtifacts)
			{
				IFileManager::Get().Delete(*Path, false, true, true);
				IFileManager::Get().Delete(
					*(Path + TEXT(".uai-artifact-owner.json")), false, true, true);
			}
			if (!OriginalWorldFile.IsEmpty()
				&& IFileManager::Get().FileExists(*OriginalWorldFile)
				&& GEditor)
			{
				UWorld* const Current = GEditor->GetEditorWorldContext().World();
				if (!Current || !Current->GetPackage()
					|| Current->GetPackage()->GetName() != OriginalWorldPackage)
				{
					if (!UEditorLoadingAndSavingUtils::LoadMap(OriginalWorldFile) && Test)
					{
						Test->AddError(TEXT("T169 failed to restore the original worker map"));
					}
				}
			}
			if (bResizedViewport && GEditor)
			{
				UWorld* const Current = GEditor->GetEditorWorldContext().World();
				int32 Index = INDEX_NONE;
				if (FLevelEditorViewportClient* const Client =
					ResolvePerspectiveViewport(Current, Index))
				{
					static_cast<FSceneViewport*>(Client->Viewport)->SetViewportSize(
						OriginalViewportSize.X, OriginalViewportSize.Y);
				}
			}
			if (bSettingsChanged)
			{
				if (UUeAgentInterfaceSettings* Settings =
					GetMutableDefault<UUeAgentInterfaceSettings>())
				{
					Settings->bEnableScreenshots = bOriginalScreenshots;
				}
			}
			MovingActor.Reset();
			if (bOwnsValidationMap)
			{
				if (UPackage* Package = FindPackage(nullptr, ValidationMapPackage))
				{
					Package->SetDirtyFlag(false);
					FText UnloadError;
					UPackageTools::UnloadPackages({Package}, UnloadError, true);
				}
				CollectGarbage(RF_NoFlags, true);
				IFileManager::Get().Delete(*ValidationMapFile, false, true, true);
				const FString TaskDirectory = FPaths::GetPath(ValidationMapFile);
				IFileManager::Get().DeleteDirectory(*TaskDirectory, false, false);
				IFileManager::Get().DeleteDirectory(
					*FPaths::GetPath(TaskDirectory), false, false);
				if (Test)
				{
					Test->TestFalse(TEXT("T169 removes its exact owned map"),
						IFileManager::Get().FileExists(*ValidationMapFile));
				}
			}
		}

		FAutomationTestBase* Test = nullptr;
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		TSharedPtr<FJsonObject> OperationRef;
		TWeakObjectPtr<AStaticMeshActor> MovingActor;
		TArray<FString> PublishedArtifacts;
		const TArray<FString> ModeIds = {
			TEXT("lit"), TEXT("player_collision"), TEXT("shader_complexity")};
		FString OriginalWorldPackage;
		FString OriginalWorldFile;
		FString ValidationMapFile;
		FString ViewportIdentity;
		FString EvidenceDirectory;
		FVector BaselineLocation = FVector::ZeroVector;
		FRotator BaselineRotation = FRotator::ZeroRotator;
		FRotator MovingRotationBefore = FRotator::ZeroRotator;
		FIntPoint OriginalViewportSize = FIntPoint::ZeroValue;
		FTSTicker::FDelegateHandle FixtureMotionTicker;
		EViewModeIndex BaselineViewMode = VMI_Lit;
		double StartedAt = 0.0;
		int32 ModeIndex = 0;
		int32 Stage = 0;
		bool BaselineRealtime = false;
		bool bOriginalScreenshots = false;
		bool bSettingsChanged = false;
		bool bResizedViewport = false;
		bool bOwnsValidationMap = false;
		bool bCleaned = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT169OwnedRhiViewportSequenceTest,
	"UAI.Vertical.T169.OwnedRhiViewportSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT169OwnedRhiViewportSequenceTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::Vertical::T169;
	(void)Parameters;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("T169 active public composition freezes"),
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(
		FOwnedRhiViewportSequenceLatentCommand(this, Registry, Dispatch));
	return true;
}

#endif
