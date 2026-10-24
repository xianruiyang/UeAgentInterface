#include "DomainAdapters/CaptureMediaLongJob/UeAgentWorldViewportCaptureMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/EditorCapture/UeAgentSlateWindowCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "Core/EditorViewport/UeAgentViewportViewMode.h"
#include "Engine/World.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "SEditorViewport.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/CollisionProfile.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "UeAgentInterfaceSettings.h"
#include "Dom/JsonObject.h"
#include "Engine/HitResult.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics
{
	bool PrepareLevelViewportForCapture(FLevelEditorViewportClient* ViewportClient, UE::UAI::Contract::FDiagnostic& OutError)
	{
		// Keep the selected client's owner; another active window may host a
		// different viewport, and FViewport itself need not be a FSceneViewport.
		const auto Widget = ViewportClient ? ViewportClient->GetEditorViewportWidget() : nullptr;
		return EditorCapture::SlateWindow::PrepareViewport(
			Widget ? Widget->GetSceneViewport() : nullptr, OutError, true, true);
	}

	static bool TryGetString(const TSharedPtr<FJsonObject>& Object, const FString& Key, FString& OutValue)
	{
		return Object.IsValid() && Object->TryGetStringField(Key, OutValue);
	}

	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}


	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}


	static FString ViewportTypeToString(ELevelViewportType ViewportType)
	{
		switch (ViewportType)
		{
		case LVT_Perspective: return TEXT("Perspective");
		case LVT_OrthoXY: return TEXT("OrthoXY");
		case LVT_OrthoXZ: return TEXT("OrthoXZ");
		case LVT_OrthoYZ: return TEXT("OrthoYZ");
		default: return FString::Printf(TEXT("Unknown(%d)"), (int32)ViewportType);
		}
	}


	static FString WidgetModeToString(UE::Widget::EWidgetMode WidgetMode)
	{
		switch (WidgetMode)
		{
		case UE::Widget::WM_None: return TEXT("None");
		case UE::Widget::WM_Translate: return TEXT("Translate");
		case UE::Widget::WM_Rotate: return TEXT("Rotate");
		case UE::Widget::WM_Scale: return TEXT("Scale");
		default: return FString::Printf(TEXT("Unknown(%d)"), (int32)WidgetMode);
		}
	}


static int32 GetLevelViewportClientIndex(FLevelEditorViewportClient* TargetClient)
	{
		if (!GEditor || !TargetClient)
		{
			return -1;
		}

		int32 Index = 0;
		for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
		{
			if (ViewportClient == TargetClient)
			{
				return Index;
			}
			Index++;
		}
		return -1;
	}


	TSharedPtr<FJsonObject> MakeViewportClientJson(FLevelEditorViewportClient* ViewportClient, const FString& SelectionReason)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("selection_reason"), SelectionReason);
		Obj->SetNumberField(TEXT("viewport_client_index"), GetLevelViewportClientIndex(ViewportClient));

		if (!ViewportClient)
		{
			Obj->SetBoolField(TEXT("valid"), false);
			return Obj;
		}

		FViewport* ActiveViewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
		Obj->SetBoolField(TEXT("valid"), true);
		Obj->SetBoolField(TEXT("is_active_viewport"), ActiveViewport && ViewportClient->Viewport == ActiveViewport);
		Obj->SetBoolField(TEXT("is_current_level_editing_viewport"), ViewportClient == GCurrentLevelEditingViewportClient);
		Obj->SetStringField(TEXT("viewport_type"), ViewportTypeToString(ViewportClient->ViewportType));
		Obj->SetNumberField(TEXT("viewport_type_value"), (int32)ViewportClient->ViewportType);
		Obj->SetStringField(TEXT("view_mode"), FString::Printf(TEXT("%d"), (int32)ViewportClient->GetViewMode()));
		Obj->SetNumberField(TEXT("view_mode_value"), (int32)ViewportClient->GetViewMode());
		Obj->SetStringField(TEXT("view_mode_name"), UeAgentViewportViewMode::ViewModeIndexToStableName(ViewportClient->GetViewMode()));
		Obj->SetStringField(TEXT("view_mode_display_name"), UeAgentViewportViewMode::ViewModeIndexToDisplayName(ViewportClient->GetViewMode()));
		Obj->SetObjectField(TEXT("view_mode_state"), UeAgentViewportViewMode::MakeCurrentViewportViewModeJson(ViewportClient));
		Obj->SetObjectField(TEXT("location"), MakeVectorJson(ViewportClient->GetViewLocation()));
		Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(ViewportClient->GetViewRotation()));
		Obj->SetNumberField(TEXT("fov"), ViewportClient->ViewFOV);
		Obj->SetBoolField(TEXT("realtime"), ViewportClient->IsRealtime());
		Obj->SetBoolField(TEXT("game_view"), ViewportClient->IsInGameView());
		Obj->SetStringField(TEXT("widget_mode"), WidgetModeToString(ViewportClient->GetWidgetMode()));
		Obj->SetNumberField(TEXT("widget_mode_value"), (int32)ViewportClient->GetWidgetMode());

		TSharedPtr<FJsonObject> ShowFlagsObj = MakeShared<FJsonObject>();
		ShowFlagsObj->SetBoolField(TEXT("mode_widgets"), ViewportClient->EngineShowFlags.ModeWidgets != 0);
		ShowFlagsObj->SetBoolField(TEXT("selection"), ViewportClient->EngineShowFlags.Selection != 0);
		ShowFlagsObj->SetBoolField(TEXT("pivot"), ViewportClient->EngineShowFlags.Pivot != 0);
		ShowFlagsObj->SetBoolField(TEXT("editor"), ViewportClient->EngineShowFlags.Editor != 0);
		ShowFlagsObj->SetBoolField(TEXT("game"), ViewportClient->EngineShowFlags.Game != 0);
		Obj->SetObjectField(TEXT("show_flags"), ShowFlagsObj);

		TSharedPtr<FJsonObject> SizeObj = MakeShared<FJsonObject>();
		if (ViewportClient->Viewport)
		{
			const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
			SizeObj->SetNumberField(TEXT("width"), Size.X);
			SizeObj->SetNumberField(TEXT("height"), Size.Y);
		}
		else
		{
			SizeObj->SetNumberField(TEXT("width"), 0);
			SizeObj->SetNumberField(TEXT("height"), 0);
		}
		Obj->SetObjectField(TEXT("size"), SizeObj);
		return Obj;
	}


	FLevelEditorViewportClient* GetPreferredLevelViewportClient()
	{
		return GetPreferredLevelViewportClient(nullptr);
	}


	FLevelEditorViewportClient* GetPreferredLevelViewportClient(FString* OutSelectionReason)
	{
		auto SetReason = [OutSelectionReason](const TCHAR* Reason)
		{
			if (OutSelectionReason)
			{
				*OutSelectionReason = Reason;
			}
		};

		if (GEditor)
		{
			if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
			{
				for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
				{
					if (ViewportClient && ViewportClient->Viewport == ActiveViewport)
					{
						SetReason(TEXT("active_viewport"));
						return ViewportClient;
					}
				}
			}
		}

		if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
		{
			SetReason(TEXT("current_level_editing_viewport"));
			return GCurrentLevelEditingViewportClient;
		}

		if (GEditor)
		{
			FLevelEditorViewportClient* FirstValid = nullptr;

			FLevelEditorViewportClient* BestPerspectiveByArea = nullptr;
			int64 BestPerspectiveArea = -1;

			FLevelEditorViewportClient* BestAnyByArea = nullptr;
			int64 BestAnyArea = -1;
			for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
			{
				if (!ViewportClient || !ViewportClient->Viewport)
				{
					continue;
				}
				if (!FirstValid)
				{
					FirstValid = ViewportClient;
				}

				const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
				const int64 Area = (int64)Size.X * (int64)Size.Y;
				if (Area > BestAnyArea)
				{
					BestAnyArea = Area;
					BestAnyByArea = ViewportClient;
				}
				if (ViewportClient->ViewportType == LVT_Perspective && Area > BestPerspectiveArea)
				{
					BestPerspectiveArea = Area;
					BestPerspectiveByArea = ViewportClient;
				}
			}
			if (BestPerspectiveByArea)
			{
				SetReason(TEXT("largest_perspective_viewport"));
				return BestPerspectiveByArea;
			}
			if (BestAnyByArea)
			{
				SetReason(TEXT("largest_any_viewport"));
				return BestAnyByArea;
			}
			if (FirstValid)
			{
				SetReason(TEXT("first_valid_viewport"));
				return FirstValid;
			}
		}

		SetReason(TEXT("fallback_current_level_editing_viewport"));
		return GCurrentLevelEditingViewportClient;
	}


	TSharedPtr<FJsonObject> MakeActorSummaryJson(AActor* Actor)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Actor ? Actor->GetName() : TEXT(""));
		Item->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : TEXT(""));
		Item->SetStringField(TEXT("class"), (Actor && Actor->GetClass()) ? Actor->GetClass()->GetPathName() : TEXT(""));
		return Item;
	}


	TSharedPtr<FJsonObject> MakeActorTransformJson(const FTransform& Transform)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("location"), MakeVectorJson(Transform.GetLocation()));
		Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Transform.Rotator()));
		Obj->SetObjectField(TEXT("scale"), MakeVectorJson(Transform.GetScale3D()));
		return Obj;
	}


	static FString SanitizeArtifactSegment(const FString& Value)
	{
		FString Out;
		Out.Reserve(Value.Len());
		for (const TCHAR Ch : Value)
		{
			if (FChar::IsAlnum(Ch) || Ch == TCHAR('-') || Ch == TCHAR('_'))
			{
				Out.AppendChar(Ch);
			}
			else
			{
				Out.AppendChar(TCHAR('_'));
			}
		}
		Out.TrimStartAndEndInline();
		return Out.IsEmpty() ? TEXT("probe") : Out;
	}


	FString MakeProbeArtifactPath(const FString& Ext, const FString& RequestId, const FString& Suffix)
	{
		const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
		const FString RootRel = Settings ? Settings->ArtifactsDirRelative : TEXT("Saved/RemoteArtifacts");
		const FString ShotsDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RootRel / TEXT("Shots"));
		const FString Hostname = SanitizeArtifactSegment(FPlatformProcess::ComputerName());
		const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		const FString SafeRequestId = SanitizeArtifactSegment(RequestId);
		const FString SafeSuffix = SanitizeArtifactSegment(Suffix);
		const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		return FPaths::Combine(
			ShotsDir,
			FString::Printf(TEXT("probe_%s_%s_%s_%s_%s.%s"), *Hostname, *Timestamp, *SafeRequestId, *SafeSuffix, *Guid, *Ext));
	}


	bool GetActorBoundsBox(AActor* Actor, FBox& OutBox, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Actor)
		{
			OutError = CaptureDiagnostics::Native(TEXT("actor_not_found"), TEXT("No actor object is available for capture bounds."), TEXT("viewport_frame"), TEXT("actor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}

		OutBox = Actor->GetComponentsBoundingBox(true);
		if (!OutBox.IsValid)
		{
			OutError = CaptureDiagnostics::Native(TEXT("actor_has_invalid_bounds"), TEXT("The actor components do not provide valid capture bounds."), TEXT("viewport_frame"), TEXT("bounds_valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = Actor->GetPathName();
			return false;
		}
		return true;
	}


	TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
		Obj->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
		Obj->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
		Obj->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
		return Obj;
	}


	void GetBoxCorners(const FBox& Box, TArray<FVector>& OutCorners)
	{
		OutCorners.Reset(8);
		const FVector& Min = Box.Min;
		const FVector& Max = Box.Max;
		OutCorners.Add(FVector(Min.X, Min.Y, Min.Z));
		OutCorners.Add(FVector(Min.X, Min.Y, Max.Z));
		OutCorners.Add(FVector(Min.X, Max.Y, Min.Z));
		OutCorners.Add(FVector(Min.X, Max.Y, Max.Z));
		OutCorners.Add(FVector(Max.X, Min.Y, Min.Z));
		OutCorners.Add(FVector(Max.X, Min.Y, Max.Z));
		OutCorners.Add(FVector(Max.X, Max.Y, Min.Z));
		OutCorners.Add(FVector(Max.X, Max.Y, Max.Z));
	}


	bool ComputeViewportLocationToFrameBounds(
		FLevelEditorViewportClient* ViewportClient,
		const FBox& Bounds,
		const FRotator& ViewRotation,
		double Padding,
		FVector& OutLocation,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!ViewportClient || !ViewportClient->Viewport)
		{
			OutError = CaptureDiagnostics::Native(TEXT("no_level_editor_viewport"),
				ViewportClient ? TEXT("The level viewport client has no viewport surface.") : TEXT("No level viewport client is available."),
				TEXT("viewport_frame"), ViewportClient ? TEXT("viewport_surface_available") : TEXT("viewport_client_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}

		const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			OutError = CaptureDiagnostics::Native(TEXT("invalid_viewport_size"), TEXT("The level viewport has a non-positive pixel dimension."), TEXT("viewport_frame"), TEXT("viewport_size"), CaptureDiagnostics::Size(Size), MakeShared<FJsonValueString>(TEXT("positive width and height")));
			return false;
		}

		if (ViewportClient->ViewportType != LVT_Perspective)
		{
			OutError = CaptureDiagnostics::Native(TEXT("viewport_not_perspective"), TEXT("Actor framing requires a perspective level viewport."), TEXT("viewport_frame"), TEXT("viewport_type"), MakeShared<FJsonValueString>(ViewportTypeToString(ViewportClient->ViewportType)), MakeShared<FJsonValueString>(ViewportTypeToString(LVT_Perspective)));
			return false;
		}

		const double AspectRatio = static_cast<double>(Size.X) / static_cast<double>(Size.Y);
		if (AspectRatio <= KINDA_SMALL_NUMBER)
		{
			OutError = CaptureDiagnostics::Native(TEXT("invalid_viewport_aspect_ratio"), TEXT("The viewport aspect ratio is too small for actor framing."), TEXT("viewport_frame"), TEXT("aspect_ratio"), CaptureDiagnostics::Number(AspectRatio), CaptureDiagnostics::Facts({ { TEXT("minimum_exclusive"), CaptureDiagnostics::Number(KINDA_SMALL_NUMBER) } }));
			return false;
		}

		Padding = FMath::Clamp(Padding, 1.0, 4.0);
		const double HorizontalHalfFovRadians = FMath::DegreesToRadians(FMath::Clamp(static_cast<double>(ViewportClient->ViewFOV), 5.0, 170.0) * 0.5);
		const double TanHalfHorizontalFov = FMath::Tan(HorizontalHalfFovRadians);
		const double TanHalfVerticalFov = TanHalfHorizontalFov / AspectRatio;
		if (TanHalfHorizontalFov <= KINDA_SMALL_NUMBER || TanHalfVerticalFov <= KINDA_SMALL_NUMBER)
		{
			OutError = CaptureDiagnostics::Native(TEXT("invalid_viewport_fov"), TEXT("The viewport projection scale is too small for actor framing."), TEXT("viewport_frame"), TEXT("projection"), CaptureDiagnostics::Facts({ { TEXT("view_fov_degrees"), CaptureDiagnostics::Number(ViewportClient->ViewFOV) }, { TEXT("aspect_ratio"), CaptureDiagnostics::Number(AspectRatio) },
					{ TanHalfHorizontalFov <= KINDA_SMALL_NUMBER ? TEXT("tan_half_horizontal_fov") : TEXT("tan_half_vertical_fov"),
						CaptureDiagnostics::Number(TanHalfHorizontalFov <= KINDA_SMALL_NUMBER ? TanHalfHorizontalFov : TanHalfVerticalFov) } }), CaptureDiagnostics::Facts({ { TEXT("minimum_exclusive"), CaptureDiagnostics::Number(KINDA_SMALL_NUMBER) } }));
			return false;
		}

		const FVector BoundsCenter = Bounds.GetCenter();
		const FRotationMatrix RotationMatrix(ViewRotation);
		const FVector Forward = RotationMatrix.GetUnitAxis(EAxis::X);
		const FVector Right = RotationMatrix.GetUnitAxis(EAxis::Y);
		const FVector Up = RotationMatrix.GetUnitAxis(EAxis::Z);

		TArray<FVector> Corners;
		GetBoxCorners(Bounds, Corners);

		double RequiredDistance = 1.0;
		for (const FVector& Corner : Corners)
		{
			const FVector Relative = Corner - BoundsCenter;
			const double ForwardOffset = FVector::DotProduct(Relative, Forward);
			const double RightOffset = FMath::Abs(FVector::DotProduct(Relative, Right));
			const double UpOffset = FMath::Abs(FVector::DotProduct(Relative, Up));

			RequiredDistance = FMath::Max(RequiredDistance, (RightOffset / TanHalfHorizontalFov) - ForwardOffset);
			RequiredDistance = FMath::Max(RequiredDistance, (UpOffset / TanHalfVerticalFov) - ForwardOffset);
		}

		OutLocation = BoundsCenter - (Forward * FMath::Max(RequiredDistance * Padding, 1.0));
		return true;
	}


	bool ResolveTraceChannel(const TSharedPtr<FJsonObject>& Params, ECollisionChannel& OutChannel, Contract::FDiagnostic& OutError)
	{
		OutChannel = ECC_Visibility;

		FString ChannelName;
		if (!TryGetString(Params, TEXT("trace_channel"), ChannelName) || ChannelName.IsEmpty())
		{
			return true;
		}

		const FString Normalized = ChannelName.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("visibility") || Normalized == TEXT("ecc_visibility"))
		{
			OutChannel = ECC_Visibility;
			return true;
		}
		if (Normalized == TEXT("camera") || Normalized == TEXT("ecc_camera"))
		{
			OutChannel = ECC_Camera;
			return true;
		}
		if (Normalized == TEXT("worldstatic") || Normalized == TEXT("world_static") || Normalized == TEXT("ecc_worldstatic"))
		{
			OutChannel = ECC_WorldStatic;
			return true;
		}
		if (Normalized == TEXT("worlddynamic") || Normalized == TEXT("world_dynamic") || Normalized == TEXT("ecc_worlddynamic"))
		{
			OutChannel = ECC_WorldDynamic;
			return true;
		}
		if (Normalized == TEXT("pawn") || Normalized == TEXT("ecc_pawn"))
		{
			OutChannel = ECC_Pawn;
			return true;
		}
		if (Normalized == TEXT("physicsbody") || Normalized == TEXT("physics_body") || Normalized == TEXT("ecc_physicsbody"))
		{
			OutChannel = ECC_PhysicsBody;
			return true;
		}

		OutError = CaptureDiagnostics::Input(TEXT("unsupported_trace_channel"), TEXT("The requested collision trace channel is not supported."), TEXT("input_decode"), TEXT("/trace_channel"), MakeShared<FJsonValueString>(ChannelName), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueString>(TEXT("visibility")), MakeShared<FJsonValueString>(TEXT("camera")), MakeShared<FJsonValueString>(TEXT("worldstatic")), MakeShared<FJsonValueString>(TEXT("worlddynamic")), MakeShared<FJsonValueString>(TEXT("pawn")), MakeShared<FJsonValueString>(TEXT("physicsbody")) }));
		return false;
	}


	void FillHitResultJson(const FHitResult& Hit, TSharedPtr<FJsonObject>& OutData)
	{
		OutData->SetBoolField(TEXT("hit"), Hit.bBlockingHit);
		if (!Hit.bBlockingHit)
		{
			return;
		}

		OutData->SetObjectField(TEXT("location"), MakeVectorJson(Hit.ImpactPoint));
		OutData->SetObjectField(TEXT("normal"), MakeVectorJson(Hit.ImpactNormal));
		OutData->SetNumberField(TEXT("distance"), Hit.Distance);
		OutData->SetStringField(TEXT("bone_name"), Hit.BoneName.ToString());
		OutData->SetStringField(TEXT("face_index"), FString::FromInt(Hit.FaceIndex));
		if (Hit.GetActor())
		{
			OutData->SetStringField(TEXT("actor_id"), Hit.GetActor()->GetActorLabel());
			OutData->SetStringField(TEXT("actor_name"), Hit.GetActor()->GetName());
		}
		if (Hit.GetComponent())
		{
			OutData->SetStringField(TEXT("component_name"), Hit.GetComponent()->GetName());
			OutData->SetStringField(TEXT("component_path"), Hit.GetComponent()->GetPathName());
		}
	}


}
