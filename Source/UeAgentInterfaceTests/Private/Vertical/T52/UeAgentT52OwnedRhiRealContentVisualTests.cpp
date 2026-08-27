#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "EngineUtils.h"
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

#include <initializer_list>

namespace UE::UAI::Tests::Vertical::T52
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;

	constexpr const TCHAR* ValidationMapPackage =
		TEXT("/Game/UAIValidation/T46/LV_T46_BackgroundWhitebox");

	struct FPrimitiveSpec final
	{
		FString Label;
		FString CommandId;
		FVector SizeCm = FVector::ZeroVector;
		FVector BaseCenter = FVector::ZeroVector;
		bool bCylinder = false;
	};

	const TArray<FPrimitiveSpec>& PrimitiveSpecs()
	{
		static const TArray<FPrimitiveSpec> Specs = {
			{TEXT("T46_Floor"), TEXT("mesh.primitive.box.create"),
				FVector(1200.0, 800.0, 40.0), FVector(0.0, 0.0, 0.0), false},
			{TEXT("T46_Pad"), TEXT("mesh.primitive.box.create"),
				FVector(320.0, 320.0, 80.0), FVector(0.0, 0.0, 40.0), false},
			{TEXT("T46_Pylon_L"), TEXT("mesh.primitive.box.create"),
				FVector(160.0, 160.0, 360.0), FVector(-300.0, 0.0, 40.0), false},
			{TEXT("T46_Pylon_R"), TEXT("mesh.primitive.box.create"),
				FVector(160.0, 160.0, 360.0), FVector(300.0, 0.0, 40.0), false},
			{TEXT("T46_Core"), TEXT("mesh.primitive.cylinder.create"),
				FVector(200.0, 200.0, 240.0), FVector(0.0, 0.0, 120.0), true},
			{TEXT("T46_Beam"), TEXT("mesh.primitive.box.create"),
				FVector(760.0, 160.0, 120.0), FVector(0.0, 0.0, 400.0), false},
			{TEXT("T46_Keystone"), TEXT("mesh.primitive.box.create"),
				FVector(200.0, 200.0, 80.0), FVector(0.0, 0.0, 520.0), false},
		};
		return Specs;
	}

	TArray<TSharedPtr<FJsonValue>> NumberValues(
		const std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(static_cast<int32>(Values.size()));
		for (const double Value : Values)
		{
			Result.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return Result;
	}

	TSharedRef<FJsonObject> TemporalPoint(const FString& Tick)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), TEXT("editor"));
		Result->SetStringField(TEXT("tick"), Tick);
		Result->SetStringField(TEXT("rate_numerator"), TEXT("60"));
		Result->SetStringField(TEXT("rate_denominator"), TEXT("1"));
		Result->SetStringField(TEXT("state"), TEXT("captured"));
		return Result;
	}

	TSharedRef<FJsonObject> SpatialFrame(const FString& Tick)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("frame_id"), TEXT("unreal.world"));
		Result->SetStringField(TEXT("basis_frame_id"), TEXT("unreal.world"));
		Result->SetNumberField(TEXT("dimension"), 3);
		Result->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Result->SetStringField(
			TEXT("basis_evidence_id"), TEXT("t52-explicit-unreal-world"));
		Result->SetObjectField(TEXT("observed_at"), TemporalPoint(Tick));
		return Result;
	}

	TSharedRef<FJsonObject> BuildPlacement(const FVector& Translation)
	{
		const TSharedRef<FJsonObject> Placement = MakeShared<FJsonObject>();
		Placement->SetStringField(TEXT("semantic_frame_id"), TEXT("ue.world"));
		Placement->SetStringField(TEXT("write_frame_id"), TEXT("ue.world"));
		Placement->SetStringField(TEXT("read_frame_id"), TEXT("ue.world"));
		Placement->SetStringField(TEXT("verify_frame_id"), TEXT("ue.world"));
		Placement->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Placement->SetArrayField(
			TEXT("translation"),
			NumberValues({Translation.X, Translation.Y, Translation.Z}));
		Placement->SetArrayField(
			TEXT("rotation_quaternion_xyzw"),
			NumberValues({0.0, 0.0, 0.0, 1.0}));
		Placement->SetArrayField(
			TEXT("scale"), NumberValues({1.0, 1.0, 1.0}));
		Placement->SetStringField(
			TEXT("basis_evidence"),
			TEXT("T52 exact T46 UE world placement contract"));
		Placement->SetStringField(
			TEXT("conversion_chain"),
			TEXT("T46 semantic world -> UE world -> public primitive -> "
				"public and independent readback"));
		return Placement;
	}

	TSharedRef<FJsonObject> BuildPrimitiveParams(const FPrimitiveSpec& Spec)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), Spec.Label);
		Params->SetObjectField(TEXT("placement"), BuildPlacement(Spec.BaseCenter));
		Params->SetStringField(TEXT("anchor"), TEXT("base_center"));
		if (Spec.bCylinder)
		{
			Params->SetNumberField(TEXT("radius_cm"), Spec.SizeCm.X * 0.5);
			Params->SetNumberField(TEXT("height_cm"), Spec.SizeCm.Z);
			Params->SetNumberField(TEXT("radial_segments"), 32);
			Params->SetNumberField(TEXT("height_segments"), 1);
			Params->SetBoolField(TEXT("cap_ends"), true);
		}
		else
		{
			Params->SetArrayField(
				TEXT("size_cm"),
				NumberValues({Spec.SizeCm.X, Spec.SizeCm.Y, Spec.SizeCm.Z}));
			Params->SetArrayField(
				TEXT("subdivisions"), NumberValues({1.0, 1.0, 1.0}));
		}
		return Params;
	}

	TSharedRef<FJsonObject> WorldParams(
		const FString& Target,
		const TSharedRef<FJsonObject>& Input)
	{
		const TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetStringField(TEXT("world_type"), TEXT("editor"));
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("input"), Input);
		Params->SetObjectField(TEXT("context"), Context);
		return Params;
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

	FString ViewModeId(const FLevelEditorViewportClient& Client)
	{
		switch (Client.GetViewMode())
		{
		case VMI_Unlit: return TEXT("unlit");
		case VMI_BrushWireframe:
		case VMI_Wireframe: return TEXT("wireframe");
		default: return TEXT("lit");
		}
	}

	FString WidgetModeId(const UE::Widget::EWidgetMode Mode)
	{
		switch (Mode)
		{
		case UE::Widget::WM_Translate: return TEXT("translate");
		case UE::Widget::WM_Rotate: return TEXT("rotate");
		case UE::Widget::WM_Scale: return TEXT("scale");
		default: return TEXT("none");
		}
	}

	TSharedRef<FJsonObject> CameraTransform(
		const FString& ViewportIdentity,
		const FVector& Location,
		const FRotator& Rotation,
		const FString& Tick)
	{
		const FQuat Quat = Rotation.Quaternion();
		const TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
		Transform->SetStringField(
			TEXT("subject"), ViewportIdentity + TEXT(".camera"));
		Transform->SetStringField(TEXT("quantity"), TEXT("transform"));
		Transform->SetObjectField(TEXT("frame"), SpatialFrame(Tick));
		TArray<TSharedPtr<FJsonValue>> Components;
		for (const double Value : {
			Location.X, Location.Y, Location.Z,
			Quat.X, Quat.Y, Quat.Z, Quat.W,
			1.0, 1.0, 1.0})
		{
			Components.Add(MakeShared<FJsonValueString>(
				FString::SanitizeFloat(Value, 9)));
		}
		Transform->SetArrayField(TEXT("components"), MoveTemp(Components));
		Transform->SetArrayField(
			TEXT("conversion_evidence_ids"),
			{MakeShared<FJsonValueString>(TEXT("t52-identity-conversion"))});
		return Transform;
	}

	TSharedRef<FJsonObject> CaptureViewportState(
		const FString& ViewportIdentity,
		FLevelEditorViewportClient& Client)
	{
		const TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
		Camera->SetObjectField(
			TEXT("camera_transform"),
			CameraTransform(
				ViewportIdentity,
				Client.GetViewLocation(),
				Client.GetViewRotation(),
				TEXT("0")));
		Camera->SetNumberField(TEXT("fov_degrees"), Client.ViewFOV);
		Camera->SetStringField(TEXT("view_mode"), ViewModeId(Client));
		Camera->SetObjectField(TEXT("observed_at"), TemporalPoint(TEXT("0")));

		TArray<TSharedPtr<FJsonValue>> ShowFlags;
		if (Client.EngineShowFlags.Lighting)
		{
			ShowFlags.Add(MakeShared<FJsonValueString>(TEXT("Lighting")));
		}
		if (Client.EngineShowFlags.StaticMeshes)
		{
			ShowFlags.Add(MakeShared<FJsonValueString>(TEXT("StaticMeshes")));
		}
		const TSharedRef<FJsonObject> Interaction = MakeShared<FJsonObject>();
		Interaction->SetBoolField(
			TEXT("mode_widgets"), Client.EngineShowFlags.ModeWidgets != 0);
		Interaction->SetBoolField(
			TEXT("selection"), Client.EngineShowFlags.Selection != 0);
		Interaction->SetBoolField(
			TEXT("pivot"), Client.EngineShowFlags.Pivot != 0);
		Interaction->SetBoolField(
			TEXT("editor_primitives"), Client.EngineShowFlags.Editor != 0);
		Interaction->SetBoolField(
			TEXT("game_primitives"), Client.EngineShowFlags.Game != 0);
		Interaction->SetStringField(
			TEXT("widget_mode"), WidgetModeId(Client.GetWidgetMode()));

		const TSharedRef<FJsonObject> Revision = MakeShared<FJsonObject>();
		Revision->SetStringField(TEXT("source_id"), ViewportIdentity);
		Revision->SetStringField(TEXT("generation"), TEXT("t52-baseline"));
		Revision->SetNumberField(TEXT("sequence"), 1);
		const TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetStringField(TEXT("viewport_ref"), ViewportIdentity);
		Snapshot->SetObjectField(TEXT("camera"), Camera);
		Snapshot->SetBoolField(TEXT("realtime"), Client.IsRealtime());
		Snapshot->SetBoolField(TEXT("game_view"), Client.IsInGameView());
		Snapshot->SetArrayField(TEXT("show_flags"), MoveTemp(ShowFlags));
		Snapshot->SetObjectField(TEXT("interaction"), Interaction);
		Snapshot->SetObjectField(TEXT("observed_revision"), Revision);
		return Snapshot;
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

	ADynamicMeshActor* FindActorByLabel(UWorld& World, const FString& Label)
	{
		for (TActorIterator<ADynamicMeshActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel() == Label)
			{
				return *It;
			}
		}
		return nullptr;
	}

	FString Sha256Bytes(const TArray<uint8>& Bytes)
	{
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		SHA256(Bytes.GetData(), Bytes.Num(), Digest);
		return TEXT("sha256:")
			+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
	}

	class FOwnedRhiRealContentVisualLatentCommand final
		: public IAutomationLatentCommand
	{
	public:
		FOwnedRhiRealContentVisualLatentCommand(
			FAutomationTestBase* InTest,
			TSharedPtr<const FUeAgentImmutableCommandRegistry> InRegistry,
			TSharedPtr<const FUeAgentExactCommandDispatch> InDispatch)
			: Test(InTest)
			, Registry(MoveTemp(InRegistry))
			, Dispatch(MoveTemp(InDispatch))
			, StartedAt(FPlatformTime::Seconds())
		{
		}

		~FOwnedRhiRealContentVisualLatentCommand() override
		{
			Cleanup();
		}

		bool Update() override
		{
			if (!Test || !Registry.IsValid() || !Dispatch.IsValid())
			{
				Cleanup();
				return true;
			}
			if (FPlatformTime::Seconds() - StartedAt > 300.0)
			{
				Test->AddError(FString::Printf(
					TEXT("T52 vertical scenario timed out at stage %d"), Stage));
				return Finish();
			}

			if (Stage == 0)
			{
				if (!SetupExactSceneAndStartCapture())
				{
					return Finish();
				}
				Stage = 1;
				return false;
			}

			TSharedPtr<FJsonObject> Status;
			if (!ReadOperationResult(
					Call(TEXT("operation.status"), OperationParams()),
					Status,
					TEXT("capture status")))
			{
				return Finish();
			}
			if (!Status->GetBoolField(TEXT("terminal")))
			{
				const int64 Generation =
					Status->GetIntegerField(TEXT("generation"));
				const TSharedRef<FJsonObject> Resume = OperationParams();
				Resume->SetNumberField(TEXT("expected_generation"), Generation);
				if (!ReadOperationResult(
						Call(TEXT("operation.resume"), Resume),
						Status,
						TEXT("capture resume")))
				{
					return Finish();
				}
				if (!Status->GetBoolField(TEXT("terminal")))
				{
					return false;
				}
			}

			FString ErrorCode;
			Status->TryGetStringField(TEXT("error_code"), ErrorCode);
			if (!ErrorCode.IsEmpty())
			{
				Test->AddError(
					TEXT("T52 capture terminated with ") + ErrorCode);
				return Finish();
			}
			if (!VerifyReceiptReleaseAndImage(Status))
			{
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
				TEXT("t52-owned-rhi-real-content-visual"),
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

		bool SetupExactSceneAndStartCapture()
		{
			if (!Test->TestTrue(
					TEXT("T52 owned worker has an RHI"), FApp::CanEverRender())
				|| !Test->TestNotNull(TEXT("T52 has an Editor"), GEditor))
			{
				return false;
			}

			UWorld* const OriginalWorld = GEditor->GetEditorWorldContext().World();
			if (!Test->TestNotNull(TEXT("T52 has an original worker world"), OriginalWorld)
				|| !OriginalWorld->GetPackage())
			{
				return false;
			}
			OriginalWorldPackage = OriginalWorld->GetPackage()->GetName();
			OriginalWorldFile = FPackageName::LongPackageNameToFilename(
				OriginalWorldPackage, FPackageName::GetMapPackageExtension());
			if (!Test->TestTrue(
					TEXT("T52 original worker map is reloadable"),
					IFileManager::Get().FileExists(*OriginalWorldFile)))
			{
				return false;
			}

			ValidationMapFile = FPackageName::LongPackageNameToFilename(
				ValidationMapPackage, FPackageName::GetMapPackageExtension());
			if (!Test->TestFalse(
					TEXT("T52 exact validation map is absent before ownership"),
					IFileManager::Get().FileExists(*ValidationMapFile)
						|| FindPackage(nullptr, ValidationMapPackage) != nullptr))
			{
				return false;
			}

			FLevelEditorViewportClient* OriginalClient =
				ResolvePerspectiveViewport(OriginalWorld, OriginalViewportIndex);
			if (!Test->TestNotNull(
					TEXT("T52 resolves the original perspective level viewport"),
					OriginalClient))
			{
				return false;
			}
			ViewportIdentity = FString::Printf(
				TEXT("level:%d"), OriginalViewportIndex);
			OriginalViewportSize = OriginalClient->Viewport->GetSizeXY();
			BaselineViewportState = CaptureViewportState(
				ViewportIdentity, *OriginalClient);

			if (UUeAgentInterfaceSettings* Settings =
				GetMutableDefault<UUeAgentInterfaceSettings>())
			{
				bOriginalScreenshots = Settings->bEnableScreenshots;
				Settings->bEnableScreenshots = true;
				bSettingsChanged = true;
			}
			else
			{
				Test->AddError(TEXT("T52 screenshot settings are unavailable"));
				return false;
			}

			UWorld* const ValidationWorld =
				UEditorLoadingAndSavingUtils::NewBlankMap(false);
			if (!Test->TestNotNull(
					TEXT("T52 creates one owned blank Editor map"), ValidationWorld))
			{
				return false;
			}
			if (ValidationWorld->PersistentLevel)
			{
				ValidationWorld->PersistentLevel->SetUseExternalActors(false);
			}
			const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(
				ValidationWorld, ValidationMapPackage);
			bOwnsValidationMap = IFileManager::Get().FileExists(*ValidationMapFile);
			if (!Test->TestTrue(
					TEXT("T52 persists the exact dedicated validation map"),
					bSaved && bOwnsValidationMap))
			{
				return false;
			}
			if (!Test->TestEqual(
					TEXT("T52 current map has the exact T46 identity"),
					ValidationWorld->GetPackage()->GetName(),
					FString(ValidationMapPackage)))
			{
				return false;
			}

			int32 ValidationViewportIndex = INDEX_NONE;
			FLevelEditorViewportClient* Client =
				ResolvePerspectiveViewport(ValidationWorld, ValidationViewportIndex);
			if (!Test->TestNotNull(
					TEXT("T52 resolves the owned perspective validation viewport"),
					Client)
				|| !Test->TestEqual(
					TEXT("T52 keeps the same owned viewport identity"),
					ValidationViewportIndex, OriginalViewportIndex))
			{
				return false;
			}
			if (Client->Viewport->GetSizeXY().X <= 0
				|| Client->Viewport->GetSizeXY().Y <= 0)
			{
				static_cast<FSceneViewport*>(Client->Viewport)->SetViewportSize(1280, 720);
				Client->Invalidate();
				bResizedViewport = true;
			}
			if (!Test->TestTrue(
					TEXT("T52 owned viewport has a positive RHI size"),
					Client->Viewport->GetSizeXY().X > 0
						&& Client->Viewport->GetSizeXY().Y > 0))
			{
				return false;
			}

			for (const FPrimitiveSpec& Spec : PrimitiveSpecs())
			{
				const FUeAgentCommandResult Created = Call(
					Spec.CommandId, BuildPrimitiveParams(Spec));
				if (!RequireOk(TEXT("public primitive ") + Spec.Label, Created))
				{
					return false;
				}
			}

			UWorld* const AuthoredWorld = GEditor->GetEditorWorldContext().World();
			if (!Test->TestNotNull(
					TEXT("T52 public authoring leaves an Editor world"), AuthoredWorld)
				|| !AuthoredWorld->GetPackage()
				|| !Test->TestEqual(
					TEXT("T52 public authoring remains in the exact map"),
					AuthoredWorld->GetPackage()->GetName(),
					FString(ValidationMapPackage))
				|| !VerifyExactStructure(*AuthoredWorld))
			{
				return false;
			}

			if (!ApplyViewportAndFrame(*AuthoredWorld))
			{
				return false;
			}

			CaptureOutputPath = FPaths::ProjectSavedDir()
				/ TEXT("UAI/T52/owned-rhi-real-content.png");
			IFileManager::Get().MakeDirectory(
				*FPaths::GetPath(CaptureOutputPath), true);
			IFileManager::Get().Delete(*CaptureOutputPath, false, true, true);
			IFileManager::Get().Delete(
				*(CaptureOutputPath + TEXT(".uai-artifact-owner.json")),
				false, true, true);
			const TSharedRef<FJsonObject> CaptureParameters = MakeShared<FJsonObject>();
			CaptureParameters->SetStringField(TEXT("format"), TEXT("png"));
			CaptureParameters->SetNumberField(TEXT("max_size"), 512);
			CaptureParameters->SetStringField(
				TEXT("output_file"), CaptureOutputPath);
			CaptureParameters->SetStringField(TEXT("view_mode"), TEXT("unlit"));
			CaptureParameters->SetBoolField(
				TEXT("restore_view_mode_after_capture"), true);
			const TSharedRef<FJsonObject> Capture = MakeShared<FJsonObject>();
			Capture->SetStringField(
				TEXT("subject_ref"), TEXT("capture-subject:viewport:active"));
			Capture->SetStringField(
				TEXT("policy_id"), TEXT("policy:image-capture:viewport/v1"));
			Capture->SetObjectField(TEXT("parameters"), CaptureParameters);
			TSharedPtr<FJsonObject> StartPayload;
			return ReadOperationResult(
				Call(TEXT("capture.image.execute"), Capture),
				StartPayload,
				TEXT("capture start"));
		}

		bool VerifyExactStructure(UWorld& World)
		{
			int32 DynamicMeshCount = 0;
			for (TActorIterator<ADynamicMeshActor> It(&World); It; ++It)
			{
				++DynamicMeshCount;
			}
			if (!Test->TestEqual(
					TEXT("T52 exact map owns seven DynamicMeshActors"),
					DynamicMeshCount, PrimitiveSpecs().Num()))
			{
				return false;
			}

			ActorPaths.Reset();
			for (int32 Index = 0; Index < PrimitiveSpecs().Num(); ++Index)
			{
				const FPrimitiveSpec& Spec = PrimitiveSpecs()[Index];
				ADynamicMeshActor* const Actor = FindActorByLabel(World, Spec.Label);
				if (!Test->TestNotNull(
						*FString::Printf(TEXT("T52 resolves %s"), *Spec.Label), Actor))
				{
					return false;
				}
				ActorPaths.Add(Actor->GetPathName());
				const FBox Bounds = Actor->GetComponentsBoundingBox(true);
				const FVector ExpectedCenter =
					Spec.BaseCenter + FVector(0.0, 0.0, Spec.SizeCm.Z * 0.5);
				if (!Test->TestTrue(
						*FString::Printf(
							TEXT("T52 %s preserves base-center placement"), *Spec.Label),
						Actor->GetActorLocation().Equals(Spec.BaseCenter, 0.1))
					|| !Test->TestTrue(
						*FString::Printf(
							TEXT("T52 %s has exact independent bounds size"), *Spec.Label),
						Bounds.IsValid && Bounds.GetSize().Equals(Spec.SizeCm, 1.0))
					|| !Test->TestTrue(
						*FString::Printf(
							TEXT("T52 %s has exact independent bounds center"), *Spec.Label),
						Bounds.IsValid && Bounds.GetCenter().Equals(ExpectedCenter, 1.0)))
				{
					Test->AddInfo(FString::Printf(
						TEXT("%s actual location=%s bounds-center=%s bounds-size=%s"),
						*Spec.Label,
						*Actor->GetActorLocation().ToString(),
						*Bounds.GetCenter().ToString(),
						*Bounds.GetSize().ToString()));
					return false;
				}
			}
			return ActorPaths.Num() == PrimitiveSpecs().Num();
		}

		bool ApplyViewportAndFrame(UWorld& World)
		{
			int32 Index = INDEX_NONE;
			FLevelEditorViewportClient* const Client =
				ResolvePerspectiveViewport(&World, Index);
			if (!Test->TestNotNull(
					TEXT("T52 resolves the viewport for the authored map"), Client)
				|| !Test->TestEqual(
					TEXT("T52 authored viewport identity stays stable"),
					Index, OriginalViewportIndex))
			{
				return false;
			}

			const FVector Target(0.0, 0.0, 280.0);
			const FVector CameraLocation(-1800.0, -1800.0, 1200.0);
			const FRotator CameraRotation =
				(Target - CameraLocation).Rotation();
			const TSharedRef<FJsonObject> Interaction = MakeShared<FJsonObject>();
			Interaction->SetBoolField(TEXT("mode_widgets"), false);
			Interaction->SetBoolField(TEXT("selection"), false);
			Interaction->SetBoolField(TEXT("pivot"), false);
			Interaction->SetBoolField(TEXT("editor_primitives"), false);
			Interaction->SetBoolField(TEXT("game_primitives"), true);
			Interaction->SetStringField(TEXT("widget_mode"), TEXT("none"));
			const TSharedRef<FJsonObject> Patch = MakeShared<FJsonObject>();
			Patch->SetObjectField(
				TEXT("camera_transform"),
				CameraTransform(
					ViewportIdentity,
					CameraLocation,
					CameraRotation,
					TEXT("20")));
			Patch->SetNumberField(TEXT("fov_degrees"), 50.0);
			Patch->SetStringField(TEXT("view_mode"), TEXT("unlit"));
			Patch->SetBoolField(TEXT("realtime"), false);
			Patch->SetBoolField(TEXT("game_view"), true);
			Patch->SetObjectField(TEXT("interaction"), Interaction);
			const TSharedRef<FJsonObject> UpdateInput = MakeShared<FJsonObject>();
			UpdateInput->SetObjectField(TEXT("state_patch"), Patch);
			if (!RequireOk(
					TEXT("public viewport state update"),
					Call(
						TEXT("viewport.state.update"),
						ViewportParams(ViewportIdentity, UpdateInput))))
			{
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> ActorRefs;
			for (const FString& ActorPath : ActorPaths)
			{
				ActorRefs.Add(MakeShared<FJsonValueString>(ActorPath));
			}
			const TSharedRef<FJsonObject> Policy = MakeShared<FJsonObject>();
			Policy->SetNumberField(TEXT("padding_ratio"), 0.20);
			Policy->SetNumberField(TEXT("minimum_distance_cm"), 100.0);
			Policy->SetNumberField(TEXT("maximum_distance_cm"), 10000.0);
			Policy->SetBoolField(TEXT("preserve_rotation"), true);
			Policy->SetBoolField(TEXT("collision_aware"), false);
			Policy->SetBoolField(TEXT("look_at"), false);
			Policy->SetBoolField(TEXT("auto_fallback"), false);
			const TSharedRef<FJsonObject> FrameInput = MakeShared<FJsonObject>();
			FrameInput->SetArrayField(TEXT("subject_refs"), MoveTemp(ActorRefs));
			FrameInput->SetObjectField(TEXT("frame_policy"), Policy);
			if (!RequireOk(
					TEXT("public all-actor viewport frame"),
					Call(
						TEXT("viewport.camera.frame"),
						ViewportParams(ViewportIdentity, FrameInput))))
			{
				return false;
			}
			const FUeAgentCommandResult Read = Call(
				TEXT("viewport.state.read"),
				ViewportParams(ViewportIdentity, MakeShared<FJsonObject>()));
			return RequireOk(TEXT("public viewport state read"), Read)
				&& Test->TestTrue(
					TEXT("T52 public read observes the prepared unlit viewport"),
					Read.GetPayloadJson().Contains(TEXT("unlit")));
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

		bool VerifyReceiptReleaseAndImage(
			const TSharedPtr<FJsonObject>& Status)
		{
			const TSharedPtr<FJsonObject>* Signal = nullptr;
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			const TSharedPtr<FJsonObject>* Artifact = nullptr;
			FString Locator;
			FString ContentHash;
			double ByteCount = -1.0;
			if (!Status.IsValid()
				|| !Status->TryGetObjectField(TEXT("latest_signal"), Signal)
				|| !Signal || !Signal->IsValid()
				|| !(*Signal)->TryGetObjectField(TEXT("payload"), Payload)
				|| !Payload || !Payload->IsValid()
				|| !(*Payload)->TryGetObjectField(TEXT("artifact_ref"), Artifact)
				|| !Artifact || !Artifact->IsValid()
				|| !(*Artifact)->TryGetStringField(TEXT("locator"), Locator)
				|| !(*Artifact)->TryGetStringField(TEXT("content_hash"), ContentHash)
				|| !(*Artifact)->TryGetNumberField(TEXT("byte_count"), ByteCount))
			{
				Test->AddError(TEXT("T52 terminal capture has no canonical artifact_ref"));
				return false;
			}
			Locator = FPaths::ConvertRelativePathToFull(Locator);
			PublishedArtifact = Locator;

			const FUeAgentCommandResult Receipt = Call(
				TEXT("operation.receipt"), OperationParams());
			TSharedPtr<FJsonObject> ReceiptPayload;
			FString ParseError;
			if (!Receipt.IsOk()
				|| !UE::UAI::CommandContract::Json::TryParseObject(
					Receipt.GetPayloadJson(), ReceiptPayload, ParseError)
				|| !ReceiptPayload.IsValid()
				|| ReceiptPayload->GetStringField(TEXT("schema"))
					!= TEXT("uai.long_running.receipt.v1"))
			{
				Test->AddError(TEXT("T52 terminal receipt is invalid: ")
					+ Receipt.GetErrorJson());
				return false;
			}
			const FUeAgentCommandResult Released = Call(
				TEXT("operation.release"), OperationParams());
			if (!RequireOk(TEXT("capture operation release"), Released))
			{
				return false;
			}
			OperationRef.Reset();

			TArray<uint8> FileBytes;
			if (!Test->TestTrue(
					TEXT("T52 artifact remains readable after release"),
					FFileHelper::LoadFileToArray(FileBytes, *Locator))
				|| !Test->TestEqual(
					TEXT("T52 artifact byte count matches its receipt"),
					static_cast<int64>(FileBytes.Num()),
					static_cast<int64>(ByteCount)))
			{
				return false;
			}
			const FString ActualHash = Sha256Bytes(FileBytes);
			if (!Test->TestEqual(
					TEXT("T52 artifact SHA-256 matches its receipt"),
					ActualHash.ToLower(), ContentHash.ToLower()))
			{
				return false;
			}
			if (!VerifyImageContent(Locator))
			{
				return false;
			}

			FString DigestName = ActualHash;
			DigestName.RemoveFromStart(TEXT("sha256:"));
			const FString EvidenceDirectory = FPaths::ProjectDir()
				/ TEXT("runtimeLogs/delivery_workflow/")
				/ TEXT("TROOT-UAI-52-OWNED-RHI-REAL-CONTENT-VISUAL/visual");
			EvidenceArtifact = EvidenceDirectory / (DigestName + TEXT(".png"));
			const bool bEvidenceCopied =
				IFileManager::Get().MakeDirectory(*EvidenceDirectory, true)
				&& IFileManager::Get().Copy(
					*EvidenceArtifact, *Locator, true, true) == COPY_OK;
			if (!Test->TestTrue(
					TEXT("T52 preserves one content-addressed visual artifact"),
					bEvidenceCopied))
			{
				return false;
			}
			Test->AddInfo(TEXT("T52 visual artifact: ") + EvidenceArtifact);
			Test->AddInfo(TEXT("T52 visual SHA-256: ") + ActualHash);
			return true;
		}

		bool VerifyImageContent(const FString& Locator) const
		{
			FImage Image;
			if (!Test->TestTrue(
					TEXT("T52 PNG decodes through Engine image readback"),
					FImageUtils::LoadImage(*Locator, Image)))
			{
				return false;
			}
			Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			const int64 PixelCount = Image.GetNumPixels();
			if (!Test->TestTrue(
					TEXT("T52 PNG has useful dimensions"),
					Image.SizeX >= 256 && Image.SizeY >= 144)
				|| !Test->TestEqual(
					TEXT("T52 PNG pixel storage matches its dimensions"),
					PixelCount,
					static_cast<int64>(Image.SizeX) * Image.SizeY)
				|| Image.RawData.Num()
					< PixelCount * static_cast<int64>(sizeof(FColor)))
			{
				return false;
			}

			const FColor* const Pixels =
				reinterpret_cast<const FColor*>(Image.RawData.GetData());
			double Sum = 0.0;
			double SumSquares = 0.0;
			int64 EdgePixels = 0;
			int64 Quadrants[4] = {};
			int64 CenterBandEdges = 0;
			int64 MinX = Image.SizeX;
			int64 MinY = Image.SizeY;
			int64 MaxX = -1;
			int64 MaxY = -1;
			auto ColorDelta = [](const FColor& A, const FColor& B)
			{
				return FMath::Max3(
					FMath::Abs(static_cast<int32>(A.R) - B.R),
					FMath::Abs(static_cast<int32>(A.G) - B.G),
					FMath::Abs(static_cast<int32>(A.B) - B.B));
			};
			for (int64 Y = 1; Y + 1 < Image.SizeY; ++Y)
			{
				for (int64 X = 1; X + 1 < Image.SizeX; ++X)
				{
					const int64 PixelIndex = Y * Image.SizeX + X;
					const FColor& Pixel = Pixels[PixelIndex];
					const double Luminance =
						0.2126 * Pixel.R + 0.7152 * Pixel.G + 0.0722 * Pixel.B;
					Sum += Luminance;
					SumSquares += Luminance * Luminance;
					const int32 Delta = FMath::Max(
						ColorDelta(Pixel, Pixels[PixelIndex + 1]),
						ColorDelta(Pixel, Pixels[PixelIndex + Image.SizeX]));
					if (Delta < 20)
					{
						continue;
					}
					++EdgePixels;
					MinX = FMath::Min(MinX, X);
					MinY = FMath::Min(MinY, Y);
					MaxX = FMath::Max(MaxX, X);
					MaxY = FMath::Max(MaxY, Y);
					const int32 Quadrant =
						(Y >= Image.SizeY / 2 ? 2 : 0)
						+ (X >= Image.SizeX / 2 ? 1 : 0);
					++Quadrants[Quadrant];
					if (X >= Image.SizeX * 2 / 5 && X <= Image.SizeX * 3 / 5)
					{
						++CenterBandEdges;
					}
				}
			}
			const int64 SampleCount = (Image.SizeX - 2) * (Image.SizeY - 2);
			const double Mean = SampleCount > 0 ? Sum / SampleCount : 0.0;
			const double Variance = SampleCount > 0
				? FMath::Max(0.0, SumSquares / SampleCount - Mean * Mean)
				: 0.0;
			const int64 EdgeWidth = MaxX >= MinX ? MaxX - MinX + 1 : 0;
			const int64 EdgeHeight = MaxY >= MinY ? MaxY - MinY + 1 : 0;
			Test->AddInfo(FString::Printf(
				TEXT("T52 PNG metrics size=%lldx%lld variance=%.3f edges=%lld "
					"bbox=%lldx%lld quadrants=%lld/%lld/%lld/%lld center=%lld"),
				Image.SizeX, Image.SizeY, Variance, EdgePixels,
				EdgeWidth, EdgeHeight,
				Quadrants[0], Quadrants[1], Quadrants[2], Quadrants[3],
				CenterBandEdges));
			return Test->TestTrue(
					TEXT("T52 PNG contains non-flat rendered content"),
					Variance > 20.0
						&& EdgePixels > FMath::Max<int64>(100, PixelCount / 2000))
				&& Test->TestTrue(
					TEXT("T52 framed content spans a useful image region"),
					EdgeWidth > Image.SizeX / 5
						&& EdgeHeight > Image.SizeY / 5)
				&& Test->TestTrue(
					TEXT("T52 symmetric T46 structure reaches every image quadrant"),
					Quadrants[0] > 5 && Quadrants[1] > 5
						&& Quadrants[2] > 5 && Quadrants[3] > 5
						&& CenterBandEdges > 10);
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

			if (OperationRef.IsValid() && Registry.IsValid() && Dispatch.IsValid())
			{
				const TSharedRef<FJsonObject> Cancel = OperationParams();
				Cancel->SetStringField(
					TEXT("reason"), TEXT("t52_vertical_cleanup"));
				Call(TEXT("operation.cancel"), Cancel);
				Call(TEXT("operation.release"), OperationParams());
				OperationRef.Reset();
			}

			if (BaselineViewportState.IsValid()
				&& Registry.IsValid() && Dispatch.IsValid())
			{
				const TSharedRef<FJsonObject> RestoreInput = MakeShared<FJsonObject>();
				RestoreInput->SetObjectField(
					TEXT("snapshot"), BaselineViewportState.ToSharedRef());
				const FUeAgentCommandResult Restored = Call(
					TEXT("viewport.state.restore"),
					ViewportParams(ViewportIdentity, RestoreInput));
				if (!Restored.IsOk() && Test)
				{
					Test->AddError(TEXT("T52 viewport restore failed: ")
						+ Restored.GetErrorJson());
				}
			}

			if (bResizedViewport && GEditor)
			{
				UWorld* const Current = GEditor->GetEditorWorldContext().World();
				int32 Index = INDEX_NONE;
				if (FLevelEditorViewportClient* Client =
						ResolvePerspectiveViewport(Current, Index))
				{
					static_cast<FSceneViewport*>(Client->Viewport)->SetViewportSize(
						OriginalViewportSize.X, OriginalViewportSize.Y);
				}
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
						Test->AddError(TEXT("T52 failed to restore the original worker map"));
					}
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

			if (bOwnsValidationMap)
			{
				if (UPackage* Package = FindPackage(nullptr, ValidationMapPackage))
				{
					Package->SetDirtyFlag(false);
					FText UnloadError;
					UPackageTools::UnloadPackages({Package}, UnloadError, true);
				}
				CollectGarbage(RF_NoFlags, true);
				IFileManager::Get().Delete(
					*ValidationMapFile, false, true, true);
				if (Test)
				{
					Test->TestFalse(
						TEXT("T52 removes the exact owned validation map"),
						IFileManager::Get().FileExists(*ValidationMapFile));
				}
			}

			if (!PublishedArtifact.IsEmpty())
			{
				IFileManager::Get().Delete(*PublishedArtifact, false, true, true);
				IFileManager::Get().Delete(
					*(PublishedArtifact + TEXT(".uai-artifact-owner.json")),
					false, true, true);
			}
			else if (!CaptureOutputPath.IsEmpty())
			{
				IFileManager::Get().Delete(*CaptureOutputPath, false, true, true);
				IFileManager::Get().Delete(
					*(CaptureOutputPath + TEXT(".uai-artifact-owner.json")),
					false, true, true);
			}
		}

		FAutomationTestBase* Test = nullptr;
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		TSharedPtr<FJsonObject> OperationRef;
		TSharedPtr<FJsonObject> BaselineViewportState;
		TArray<FString> ActorPaths;
		FString OriginalWorldPackage;
		FString OriginalWorldFile;
		FString ValidationMapFile;
		FString ViewportIdentity;
		FString CaptureOutputPath;
		FString PublishedArtifact;
		FString EvidenceArtifact;
		FIntPoint OriginalViewportSize = FIntPoint::ZeroValue;
		double StartedAt = 0.0;
		int32 OriginalViewportIndex = INDEX_NONE;
		int32 Stage = 0;
		bool bOriginalScreenshots = false;
		bool bSettingsChanged = false;
		bool bResizedViewport = false;
		bool bOwnsValidationMap = false;
		bool bCleaned = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT52OwnedRhiRealContentVisualTest,
	"UAI.Vertical.T52.OwnedRhiRealContentVisual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT52OwnedRhiRealContentVisualTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::Vertical::T52;
	(void)Parameters;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("T52 active public composition freezes"),
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(
		FOwnedRhiRealContentVisualLatentCommand(this, Registry, Dispatch));
	return true;
}

#endif
