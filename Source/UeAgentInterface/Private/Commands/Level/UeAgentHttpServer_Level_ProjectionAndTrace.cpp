#include "UeAgentHttpServer_Level.h"

#include "UeAgentHttpServer_LevelOps.h"

#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LevelEditorViewport.h"

bool FUeAgentLegacyMechanics::CmdViewportDeprojectScreenToWorld(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FLevelEditorViewportClient* ViewportClient = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!ViewportClient || !ViewportClient->Viewport)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FVector2D ScreenPoint(0.0f, 0.0f);
	if (!UeAgentLevelOps::ResolveViewportScreenPoint(ViewportClient, Ctx.Params, ScreenPoint, OutError))
	{
		return false;
	}

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		ViewportClient->Viewport,
		ViewportClient->GetWorld()->Scene,
		ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
	FSceneView* SceneView = ViewportClient->CalcSceneView(&ViewFamily);
	if (!SceneView)
	{
		OutError = TEXT("calc_scene_view_failed");
		return false;
	}

	FVector WorldOrigin = FVector::ZeroVector;
	FVector WorldDirection = FVector::ForwardVector;
	SceneView->DeprojectFVector2D(ScreenPoint, WorldOrigin, WorldDirection);

	OutData->SetObjectField(TEXT("screen_point"), VecToJson(FVector(ScreenPoint.X, ScreenPoint.Y, 0.0)));
	OutData->SetObjectField(TEXT("world_origin"), VecToJson(WorldOrigin));
	OutData->SetObjectField(TEXT("world_direction"), VecToJson(WorldDirection));
	return true;
}

bool FUeAgentLegacyMechanics::CmdProbeCameraPixelScale(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString SelectionReason;
	FLevelEditorViewportClient* ViewportClient = UeAgentLevelOps::GetPreferredLevelViewportClient(&SelectionReason);
	if (!ViewportClient || !ViewportClient->Viewport)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	const FIntPoint ViewportSize = ViewportClient->Viewport->GetSizeXY();
	if (ViewportSize.X <= 1 || ViewportSize.Y <= 1)
	{
		OutError = TEXT("invalid_viewport_size");
		return false;
	}
	if (ViewportClient->ViewportType != LVT_Perspective)
	{
		OutError = TEXT("viewport_not_perspective");
		return false;
	}

	const double AspectRatio = static_cast<double>(ViewportSize.X) / static_cast<double>(ViewportSize.Y);
	if (AspectRatio <= KINDA_SMALL_NUMBER)
	{
		OutError = TEXT("invalid_viewport_aspect_ratio");
		return false;
	}

	FVector TargetWorld = FVector::ZeroVector;
	FString TargetSource = TEXT("world_location");
	FString TargetPointMode = TEXT("world_location");
	AActor* TargetActor = nullptr;
	FBox TargetBounds(ForceInit);

	FString ActorId;
	if (JsonTryGetString(Ctx.Params, TEXT("id"), ActorId)
		|| JsonTryGetString(Ctx.Params, TEXT("actor_id"), ActorId)
		|| JsonTryGetString(Ctx.Params, TEXT("actor_name"), ActorId)
		|| JsonTryGetString(Ctx.Params, TEXT("actor_label"), ActorId))
	{
		TargetActor = FindActorByNameOrLabel(World, ActorId);
		if (!TargetActor)
		{
			OutError = TEXT("actor_not_found");
			return false;
		}

		TargetPointMode = TEXT("bounds_center");
		FString RequestedTargetPoint;
		if (JsonTryGetString(Ctx.Params, TEXT("target_point"), RequestedTargetPoint)
			|| JsonTryGetString(Ctx.Params, TEXT("point"), RequestedTargetPoint))
		{
			const FString NormalizedTargetPoint = RequestedTargetPoint.TrimStartAndEnd().ToLower();
			if (NormalizedTargetPoint == TEXT("actor_location") || NormalizedTargetPoint == TEXT("origin") || NormalizedTargetPoint == TEXT("pivot"))
			{
				TargetPointMode = TEXT("actor_location");
			}
			else if (NormalizedTargetPoint == TEXT("bounds_center") || NormalizedTargetPoint == TEXT("bounds") || NormalizedTargetPoint == TEXT("center"))
			{
				TargetPointMode = TEXT("bounds_center");
			}
			else
			{
				OutError = TEXT("unsupported_target_point");
				return false;
			}
		}

		TargetSource = TEXT("actor");
		TargetBounds = TargetActor->GetComponentsBoundingBox(true);
		if (TargetPointMode == TEXT("actor_location") || !TargetBounds.IsValid)
		{
			TargetWorld = TargetActor->GetActorLocation();
		}
		else
		{
			TargetWorld = TargetBounds.GetCenter();
		}
	}
	else if (JsonTryGetVector(Ctx.Params, TEXT("world_location"), TargetWorld)
		|| JsonTryGetVector(Ctx.Params, TEXT("target_location"), TargetWorld)
		|| JsonTryGetVector(Ctx.Params, TEXT("location"), TargetWorld))
	{
		TargetSource = TEXT("world_location");
		TargetPointMode = TEXT("world_location");
	}
	else
	{
		OutError = TEXT("missing_target");
		return false;
	}

	const FVector CameraLocation = ViewportClient->GetViewLocation();
	const FRotator CameraRotation = ViewportClient->GetViewRotation();
	const FRotationMatrix CameraRotationMatrix(CameraRotation);
	const FVector CameraForward = CameraRotationMatrix.GetUnitAxis(EAxis::X);
	const FVector CameraRight = CameraRotationMatrix.GetUnitAxis(EAxis::Y);
	const FVector CameraUp = CameraRotationMatrix.GetUnitAxis(EAxis::Z);
	const FVector RelativeWorld = TargetWorld - CameraLocation;
	const FVector CameraSpaceLocation(
		FVector::DotProduct(RelativeWorld, CameraForward),
		FVector::DotProduct(RelativeWorld, CameraRight),
		FVector::DotProduct(RelativeWorld, CameraUp));

	const double DepthCm = CameraSpaceLocation.X;
	const bool bInFrontOfCamera = DepthCm > KINDA_SMALL_NUMBER;
	const double HorizontalHalfFovRadians = FMath::DegreesToRadians(FMath::Clamp(static_cast<double>(ViewportClient->ViewFOV), 5.0, 170.0) * 0.5);
	const double TanHalfHorizontalFov = FMath::Tan(HorizontalHalfFovRadians);
	const double TanHalfVerticalFov = TanHalfHorizontalFov / AspectRatio;
	if (TanHalfHorizontalFov <= KINDA_SMALL_NUMBER || TanHalfVerticalFov <= KINDA_SMALL_NUMBER)
	{
		OutError = TEXT("invalid_viewport_fov");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	auto AddWarning = [&Warnings](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
		Warning->SetStringField(TEXT("code"), Code);
		Warning->SetStringField(TEXT("message"), Message);
		Warnings.Add(MakeShared<FJsonValueObject>(Warning));
	};

	double ProjectedScreenX = ViewportSize.X * 0.5;
	double ProjectedScreenY = ViewportSize.Y * 0.5;
	bool bProjectionValid = false;
	bool bInViewport = false;
	if (bInFrontOfCamera)
	{
		const double NormalizedX = CameraSpaceLocation.Y / (DepthCm * TanHalfHorizontalFov);
		const double NormalizedY = CameraSpaceLocation.Z / (DepthCm * TanHalfVerticalFov);
		ProjectedScreenX = (NormalizedX * 0.5 + 0.5) * static_cast<double>(ViewportSize.X);
		ProjectedScreenY = (0.5 - NormalizedY * 0.5) * static_cast<double>(ViewportSize.Y);
		bProjectionValid = FMath::IsFinite(ProjectedScreenX) && FMath::IsFinite(ProjectedScreenY);
		bInViewport = bProjectionValid
			&& ProjectedScreenX >= 0.0 && ProjectedScreenX <= static_cast<double>(ViewportSize.X)
			&& ProjectedScreenY >= 0.0 && ProjectedScreenY <= static_cast<double>(ViewportSize.Y);
	}
	else
	{
		AddWarning(TEXT("target_not_in_front_of_camera"), TEXT("Target depth is not positive; pixel scale is not valid."));
	}

	double SampleScreenX = bProjectionValid ? ProjectedScreenX : ViewportSize.X * 0.5;
	double SampleScreenY = bProjectionValid ? ProjectedScreenY : ViewportSize.Y * 0.5;
	bool bSampleOverridden = false;
	double SampleOverride = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("sample_screen_x"), SampleOverride))
	{
		SampleScreenX = SampleOverride;
		bSampleOverridden = true;
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("sample_screen_y"), SampleOverride))
	{
		SampleScreenY = SampleOverride;
		bSampleOverridden = true;
	}

	double PixelStep = 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("pixel_step"), PixelStep);
	PixelStep = FMath::Clamp(PixelStep, 0.001, 128.0);

	bool bPixelScaleValid = false;
	FVector WorldDeltaPerPixelX = FVector::ZeroVector;
	FVector WorldDeltaPerPixelY = FVector::ZeroVector;
	FVector CameraDeltaPerPixelX = FVector::ZeroVector;
	FVector CameraDeltaPerPixelY = FVector::ZeroVector;
	double UnitsPerPixelX = 0.0;
	double UnitsPerPixelY = 0.0;
	double WorldUnitsPerPixelX = 0.0;
	double WorldUnitsPerPixelY = 0.0;

	if (bInFrontOfCamera)
	{
		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			ViewportClient->Viewport,
			World->Scene,
			ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
		FSceneView* SceneView = ViewportClient->CalcSceneView(&ViewFamily);
		if (!SceneView)
		{
			OutError = TEXT("calc_scene_view_failed");
			return false;
		}

		const FVector TargetDepthPlanePoint = CameraLocation + CameraForward * DepthCm;
		auto IntersectScreenRayWithTargetDepthPlane = [&](const FVector2D& ScreenPoint, FVector& OutPoint) -> bool
		{
			FVector WorldOrigin = FVector::ZeroVector;
			FVector WorldDirection = FVector::ForwardVector;
			SceneView->DeprojectFVector2D(ScreenPoint, WorldOrigin, WorldDirection);
			const double Denom = FVector::DotProduct(WorldDirection, CameraForward);
			if (FMath::Abs(Denom) <= KINDA_SMALL_NUMBER)
			{
				return false;
			}
			const double T = FVector::DotProduct(TargetDepthPlanePoint - WorldOrigin, CameraForward) / Denom;
			OutPoint = WorldOrigin + WorldDirection * T;
			return FMath::IsFinite(OutPoint.X) && FMath::IsFinite(OutPoint.Y) && FMath::IsFinite(OutPoint.Z);
		};

		FVector PlanePoint = FVector::ZeroVector;
		FVector PlanePointX = FVector::ZeroVector;
		FVector PlanePointY = FVector::ZeroVector;
		const bool bBaseOk = IntersectScreenRayWithTargetDepthPlane(FVector2D(SampleScreenX, SampleScreenY), PlanePoint);
		const bool bXOk = IntersectScreenRayWithTargetDepthPlane(FVector2D(SampleScreenX + PixelStep, SampleScreenY), PlanePointX);
		const bool bYOk = IntersectScreenRayWithTargetDepthPlane(FVector2D(SampleScreenX, SampleScreenY + PixelStep), PlanePointY);
		if (bBaseOk && bXOk && bYOk)
		{
			WorldDeltaPerPixelX = (PlanePointX - PlanePoint) / PixelStep;
			WorldDeltaPerPixelY = (PlanePointY - PlanePoint) / PixelStep;
			CameraDeltaPerPixelX = FVector(
				FVector::DotProduct(WorldDeltaPerPixelX, CameraForward),
				FVector::DotProduct(WorldDeltaPerPixelX, CameraRight),
				FVector::DotProduct(WorldDeltaPerPixelX, CameraUp));
			CameraDeltaPerPixelY = FVector(
				FVector::DotProduct(WorldDeltaPerPixelY, CameraForward),
				FVector::DotProduct(WorldDeltaPerPixelY, CameraRight),
				FVector::DotProduct(WorldDeltaPerPixelY, CameraUp));

			WorldUnitsPerPixelX = WorldDeltaPerPixelX.Size();
			WorldUnitsPerPixelY = WorldDeltaPerPixelY.Size();
			UnitsPerPixelX = FMath::Abs(CameraDeltaPerPixelX.Y);
			UnitsPerPixelY = FMath::Abs(CameraDeltaPerPixelY.Z);
			bPixelScaleValid = UnitsPerPixelX > KINDA_SMALL_NUMBER && UnitsPerPixelY > KINDA_SMALL_NUMBER;
		}
		else
		{
			AddWarning(TEXT("pixel_ray_plane_intersection_failed"), TEXT("Could not intersect sample pixel rays with the target depth plane."));
		}
	}

	const double AnalyticUnitsPerPixelX = bInFrontOfCamera
		? (2.0 * DepthCm * TanHalfHorizontalFov) / static_cast<double>(ViewportSize.X)
		: 0.0;
	const double AnalyticUnitsPerPixelY = bInFrontOfCamera
		? (2.0 * DepthCm * TanHalfVerticalFov) / static_cast<double>(ViewportSize.Y)
		: 0.0;

	TSharedPtr<FJsonObject> ScreenPointObj = MakeShared<FJsonObject>();
	ScreenPointObj->SetNumberField(TEXT("x"), ProjectedScreenX);
	ScreenPointObj->SetNumberField(TEXT("y"), ProjectedScreenY);
	ScreenPointObj->SetBoolField(TEXT("valid"), bProjectionValid);
	ScreenPointObj->SetBoolField(TEXT("in_viewport"), bInViewport);

	TSharedPtr<FJsonObject> SamplePointObj = MakeShared<FJsonObject>();
	SamplePointObj->SetNumberField(TEXT("x"), SampleScreenX);
	SamplePointObj->SetNumberField(TEXT("y"), SampleScreenY);
	SamplePointObj->SetBoolField(TEXT("overridden"), bSampleOverridden);
	SamplePointObj->SetNumberField(TEXT("pixel_step"), PixelStep);

	TSharedPtr<FJsonObject> CameraSpaceUnitsObj = MakeShared<FJsonObject>();
	CameraSpaceUnitsObj->SetNumberField(TEXT("x"), UnitsPerPixelX);
	CameraSpaceUnitsObj->SetNumberField(TEXT("y"), UnitsPerPixelY);

	TSharedPtr<FJsonObject> AnalyticUnitsObj = MakeShared<FJsonObject>();
	AnalyticUnitsObj->SetNumberField(TEXT("x"), AnalyticUnitsPerPixelX);
	AnalyticUnitsObj->SetNumberField(TEXT("y"), AnalyticUnitsPerPixelY);

	TSharedPtr<FJsonObject> WorldUnitsObj = MakeShared<FJsonObject>();
	WorldUnitsObj->SetNumberField(TEXT("x"), WorldUnitsPerPixelX);
	WorldUnitsObj->SetNumberField(TEXT("y"), WorldUnitsPerPixelY);

	TSharedPtr<FJsonObject> CameraObj = MakeShared<FJsonObject>();
	CameraObj->SetObjectField(TEXT("location"), VecToJson(CameraLocation));
	CameraObj->SetObjectField(TEXT("rotation"), RotToJson(CameraRotation));
	CameraObj->SetNumberField(TEXT("fov"), ViewportClient->ViewFOV);
	CameraObj->SetStringField(TEXT("fov_axis"), TEXT("horizontal"));
	CameraObj->SetObjectField(TEXT("forward"), VecToJson(CameraForward));
	CameraObj->SetObjectField(TEXT("right"), VecToJson(CameraRight));
	CameraObj->SetObjectField(TEXT("up"), VecToJson(CameraUp));

	OutData->SetStringField(TEXT("schema"), TEXT("uai.probe.camera_pixel_scale.v1"));
	OutData->SetStringField(TEXT("projection_model"), TEXT("perspective_editor_viewport"));
	OutData->SetObjectField(TEXT("camera"), CameraObj);
	OutData->SetObjectField(TEXT("viewport"), UeAgentLevelOps::MakeViewportClientJson(ViewportClient, SelectionReason));
	OutData->SetObjectField(TEXT("viewport_size"), VecToJson(FVector(ViewportSize.X, ViewportSize.Y, 0.0)));
	OutData->SetStringField(TEXT("target_source"), TargetSource);
	OutData->SetStringField(TEXT("target_point"), TargetPointMode);
	OutData->SetObjectField(TEXT("target_world_location"), VecToJson(TargetWorld));
	OutData->SetObjectField(TEXT("relative_world_location"), VecToJson(RelativeWorld));
	OutData->SetObjectField(TEXT("camera_space_location"), VecToJson(CameraSpaceLocation));
	OutData->SetNumberField(TEXT("distance_to_camera"), RelativeWorld.Size());
	OutData->SetNumberField(TEXT("depth_cm"), DepthCm);
	OutData->SetBoolField(TEXT("in_front"), bInFrontOfCamera);
	OutData->SetBoolField(TEXT("in_front_of_camera"), bInFrontOfCamera);
	OutData->SetObjectField(TEXT("projected_screen_point"), ScreenPointObj);
	OutData->SetObjectField(TEXT("sample_screen_point"), SamplePointObj);
	OutData->SetBoolField(TEXT("pixel_scale_valid"), bPixelScaleValid);
	OutData->SetObjectField(TEXT("camera_space_units_per_pixel"), CameraSpaceUnitsObj);
	OutData->SetObjectField(TEXT("analytic_camera_space_units_per_pixel"), AnalyticUnitsObj);
	OutData->SetObjectField(TEXT("world_units_per_pixel"), WorldUnitsObj);
	OutData->SetObjectField(TEXT("world_delta_per_pixel_x"), VecToJson(WorldDeltaPerPixelX));
	OutData->SetObjectField(TEXT("world_delta_per_pixel_y"), VecToJson(WorldDeltaPerPixelY));
	OutData->SetObjectField(TEXT("camera_space_delta_per_pixel_x"), VecToJson(CameraDeltaPerPixelX));
	OutData->SetObjectField(TEXT("camera_space_delta_per_pixel_y"), VecToJson(CameraDeltaPerPixelY));
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);

	if (TargetActor)
	{
		OutData->SetStringField(TEXT("actor_name"), TargetActor->GetName());
		OutData->SetStringField(TEXT("actor_label"), TargetActor->GetActorLabel());
		if (TargetBounds.IsValid)
		{
			TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			BoundsObj->SetObjectField(TEXT("min"), VecToJson(TargetBounds.Min));
			BoundsObj->SetObjectField(TEXT("max"), VecToJson(TargetBounds.Max));
			BoundsObj->SetObjectField(TEXT("center"), VecToJson(TargetBounds.GetCenter()));
			BoundsObj->SetObjectField(TEXT("extent"), VecToJson(TargetBounds.GetExtent()));
			OutData->SetObjectField(TEXT("actor_bounds"), BoundsObj);
		}
	}

	return true;
}

bool FUeAgentLegacyMechanics::CmdViewportTraceScreenPoint(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FLevelEditorViewportClient* ViewportClient = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!ViewportClient || !ViewportClient->Viewport)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FVector2D ScreenPoint(0.0f, 0.0f);
	if (!UeAgentLevelOps::ResolveViewportScreenPoint(ViewportClient, Ctx.Params, ScreenPoint, OutError))
	{
		return false;
	}

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		ViewportClient->Viewport,
		World->Scene,
		ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
	FSceneView* SceneView = ViewportClient->CalcSceneView(&ViewFamily);
	if (!SceneView)
	{
		OutError = TEXT("calc_scene_view_failed");
		return false;
	}

	FVector WorldOrigin = FVector::ZeroVector;
	FVector WorldDirection = FVector::ForwardVector;
	SceneView->DeprojectFVector2D(ScreenPoint, WorldOrigin, WorldDirection);

	double TraceDistance = HALF_WORLD_MAX;
	JsonTryGetNumber(Ctx.Params, TEXT("trace_distance"), TraceDistance);
	TraceDistance = FMath::Clamp(TraceDistance, 1.0, (double)HALF_WORLD_MAX);

	ECollisionChannel TraceChannel = ECC_Visibility;
	if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
	{
		OutError = TEXT("unsupported_trace_channel");
		return false;
	}

	bool bTraceComplex = true;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentTraceScreenPoint), bTraceComplex);
	FHitResult Hit;
	const FVector TraceEnd = WorldOrigin + (WorldDirection * TraceDistance);
	const bool bHit = World->LineTraceSingleByChannel(Hit, WorldOrigin, TraceEnd, TraceChannel, QueryParams);

	OutData->SetObjectField(TEXT("screen_point"), VecToJson(FVector(ScreenPoint.X, ScreenPoint.Y, 0.0)));
	OutData->SetObjectField(TEXT("world_origin"), VecToJson(WorldOrigin));
	OutData->SetObjectField(TEXT("world_direction"), VecToJson(WorldDirection));
	OutData->SetNumberField(TEXT("trace_distance"), TraceDistance);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetBoolField(TEXT("hit"), bHit);
	if (bHit)
	{
		UeAgentLevelOps::FillHitResultJson(Hit, OutData);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelTraceWorldRay(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* RaysJson = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("rays"), RaysJson) && RaysJson && RaysJson->Num() > 0)
	{
		bool bContinueOnError = false;
		JsonTryGetBool(Ctx.Params, TEXT("continue_on_error"), bContinueOnError);

		double DefaultTraceDistance = HALF_WORLD_MAX;
		if (!JsonTryGetNumber(Ctx.Params, TEXT("trace_distance"), DefaultTraceDistance))
		{
			JsonTryGetNumber(Ctx.Params, TEXT("distance"), DefaultTraceDistance);
		}
		DefaultTraceDistance = FMath::Clamp(DefaultTraceDistance, 1.0, (double)HALF_WORLD_MAX);

		ECollisionChannel DefaultTraceChannel = ECC_Visibility;
		FString DefaultChannelName;
		if (Ctx.Params->TryGetStringField(TEXT("trace_channel"), DefaultChannelName) && !DefaultChannelName.TrimStartAndEnd().IsEmpty())
		{
			if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, DefaultTraceChannel))
			{
				OutError = TEXT("unsupported_trace_channel");
				return false;
			}
		}

		bool bDefaultTraceComplex = true;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bDefaultTraceComplex);

		bool bDefaultIncludeAllHits = false;
		JsonTryGetBool(Ctx.Params, TEXT("include_all_hits"), bDefaultIncludeAllHits);

		int32 DefaultMaxHits = 32;
		{
			double MaxHitsNumber = (double)DefaultMaxHits;
			JsonTryGetNumber(Ctx.Params, TEXT("max_hits"), MaxHitsNumber);
			DefaultMaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
		}

		bool bDefaultIncludeActorFolderTags = false;
		JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bDefaultIncludeActorFolderTags);

		int32 MaxItems = 4096;
		{
			double MaxItemsNumber = (double)MaxItems;
			JsonTryGetNumber(Ctx.Params, TEXT("max_items"), MaxItemsNumber);
			MaxItems = (int32)FMath::Clamp(MaxItemsNumber, 1.0, 20000.0);
		}

		TArray<AActor*> GlobalIgnoredActors;
		const TArray<TSharedPtr<FJsonValue>>* GlobalIgnoreIds = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), GlobalIgnoreIds) && GlobalIgnoreIds)
		{
			for (const TSharedPtr<FJsonValue>& IgnoreValue : *GlobalIgnoreIds)
			{
				const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
				if (IgnoreId.IsEmpty())
				{
					continue;
				}

				if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
				{
					GlobalIgnoredActors.Add(IgnoreActor);
				}
			}
		}
		UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, GlobalIgnoredActors);

		const int32 InputCount = RaysJson->Num();
		const int32 ExecuteCount = FMath::Min(InputCount, MaxItems);
		const bool bTruncated = InputCount > ExecuteCount;

		int32 ErrorCount = 0;
		int32 HitCount = 0;
		int32 FirstErrorIndex = INDEX_NONE;
		FString FirstError;

		TArray<TSharedPtr<FJsonValue>> Results;
		Results.Reserve(ExecuteCount);

		for (int32 Index = 0; Index < ExecuteCount; ++Index)
		{
			const TSharedPtr<FJsonValue>& RayValue = (*RaysJson)[Index];
			const TSharedPtr<FJsonObject> RayObj = RayValue.IsValid() ? RayValue->AsObject() : nullptr;
			if (!RayObj.IsValid())
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("rays_invalid_item");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("rays_invalid_item");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("rays_invalid_item"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			FVector Start = FVector::ZeroVector;
			if (!JsonTryGetVector(RayObj, TEXT("start"), Start))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_start");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_start");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_start"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			FVector Direction = FVector::ForwardVector;
			if (!JsonTryGetVector(RayObj, TEXT("direction"), Direction))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_direction");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_direction");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_direction"));
				Item->SetObjectField(TEXT("start"), VecToJson(Start));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			const FVector DirectionNormalized = Direction.GetSafeNormal();
			if (DirectionNormalized.IsNearlyZero())
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("invalid_direction");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("invalid_direction");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("invalid_direction"));
				Item->SetObjectField(TEXT("start"), VecToJson(Start));
				Item->SetObjectField(TEXT("direction"), VecToJson(Direction));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			double TraceDistance = DefaultTraceDistance;
			if (!JsonTryGetNumber(RayObj, TEXT("trace_distance"), TraceDistance))
			{
				JsonTryGetNumber(RayObj, TEXT("distance"), TraceDistance);
			}
			TraceDistance = FMath::Clamp(TraceDistance, 1.0, (double)HALF_WORLD_MAX);

			ECollisionChannel TraceChannel = DefaultTraceChannel;
			FString ChannelName;
			if (RayObj->TryGetStringField(TEXT("trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
			{
				if (!UeAgentLevelOps::ResolveTraceChannel(RayObj, TraceChannel))
				{
					++ErrorCount;
					if (FirstErrorIndex == INDEX_NONE)
					{
						FirstErrorIndex = Index;
						FirstError = TEXT("unsupported_trace_channel");
					}
					if (!bContinueOnError)
					{
						OutError = TEXT("unsupported_trace_channel");
						return false;
					}

					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetBoolField(TEXT("ok"), false);
					Item->SetStringField(TEXT("error"), TEXT("unsupported_trace_channel"));
					Item->SetObjectField(TEXT("start"), VecToJson(Start));
					Item->SetObjectField(TEXT("direction"), VecToJson(DirectionNormalized));
					Results.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}
			}

			bool bTraceComplex = bDefaultTraceComplex;
			RayObj->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);

			bool bIncludeAllHits = bDefaultIncludeAllHits;
			RayObj->TryGetBoolField(TEXT("include_all_hits"), bIncludeAllHits);

			int32 MaxHits = DefaultMaxHits;
			{
				double MaxHitsNumber = (double)MaxHits;
				JsonTryGetNumber(RayObj, TEXT("max_hits"), MaxHitsNumber);
				MaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
			}

			bool bIncludeActorFolderTags = bDefaultIncludeActorFolderTags;
			RayObj->TryGetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentTraceWorldRayBatch), bTraceComplex);
			for (AActor* Ignored : GlobalIgnoredActors)
			{
				if (Ignored)
				{
					QueryParams.AddIgnoredActor(Ignored);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
			if (RayObj->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
			{
				for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
				{
					const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
					if (IgnoreId.IsEmpty())
					{
						continue;
					}

					if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
					{
						QueryParams.AddIgnoredActor(IgnoreActor);
					}
				}
			}

			const FVector TraceEnd = Start + (DirectionNormalized * TraceDistance);

			FHitResult PrimaryHit;
			TArray<FHitResult> Hits;
			bool bBlockingHit = false;
			if (bIncludeAllHits)
			{
				World->LineTraceMultiByChannel(Hits, Start, TraceEnd, TraceChannel, QueryParams);
				Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });
				for (const FHitResult& H : Hits)
				{
					if (H.bBlockingHit)
					{
						PrimaryHit = H;
						bBlockingHit = true;
						break;
					}
				}
			}
			else
			{
				bBlockingHit = World->LineTraceSingleByChannel(PrimaryHit, Start, TraceEnd, TraceChannel, QueryParams);
			}
			if (bBlockingHit)
			{
				++HitCount;
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetBoolField(TEXT("ok"), true);
			Item->SetObjectField(TEXT("start"), VecToJson(Start));
			Item->SetObjectField(TEXT("direction"), VecToJson(DirectionNormalized));
			Item->SetNumberField(TEXT("trace_distance"), TraceDistance);
			Item->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
			Item->SetBoolField(TEXT("trace_complex"), bTraceComplex);
			Item->SetBoolField(TEXT("include_all_hits"), bIncludeAllHits);
			Item->SetNumberField(TEXT("max_hits"), MaxHits);
			Item->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
			Item->SetBoolField(TEXT("hit"), bBlockingHit);
			if (bBlockingHit)
			{
				if (bIncludeActorFolderTags)
				{
					if (AActor* HitActor = PrimaryHit.GetActor())
					{
						Item->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

						TArray<TSharedPtr<FJsonValue>> TagValues;
						TagValues.Reserve(HitActor->Tags.Num());
						for (const FName& Tag : HitActor->Tags)
						{
							TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
						}
						Item->SetArrayField(TEXT("hit_actor_tags"), TagValues);
					}
				}
				UeAgentLevelOps::FillHitResultJson(PrimaryHit, Item);
			}
			if (bIncludeAllHits)
			{
				Item->SetNumberField(TEXT("hit_count"), Hits.Num());
				Item->SetBoolField(TEXT("hits_truncated"), Hits.Num() > MaxHits);

				TArray<TSharedPtr<FJsonValue>> HitItems;
				HitItems.Reserve(FMath::Min(MaxHits, Hits.Num()));
				for (int32 HitIndex = 0; HitIndex < Hits.Num() && HitItems.Num() < MaxHits; ++HitIndex)
				{
					const FHitResult& H = Hits[HitIndex];
					TSharedPtr<FJsonObject> HitObj = UeAgentLevelOps::MakeHitResultSummaryJson(H, bIncludeActorFolderTags, false);
					HitObj->SetNumberField(TEXT("hit_index"), HitIndex);
					HitItems.Add(MakeShared<FJsonValueObject>(HitObj));
				}
				Item->SetArrayField(TEXT("hits"), HitItems);
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));
		}

		OutData->SetStringField(TEXT("mode"), TEXT("batch"));
		OutData->SetNumberField(TEXT("input_ray_count"), InputCount);
		OutData->SetNumberField(TEXT("ray_count"), Results.Num());
		OutData->SetNumberField(TEXT("max_items"), MaxItems);
		OutData->SetBoolField(TEXT("truncated"), bTruncated);
		OutData->SetBoolField(TEXT("continue_on_error"), bContinueOnError);
		OutData->SetNumberField(TEXT("hit_count"), HitCount);
		OutData->SetNumberField(TEXT("error_count"), ErrorCount);
		OutData->SetNumberField(TEXT("first_error_index"), FirstErrorIndex);
		if (FirstErrorIndex != INDEX_NONE)
		{
			OutData->SetStringField(TEXT("first_error"), FirstError);
		}
		OutData->SetArrayField(TEXT("rays"), Results);
		return true;
	}

	FVector Start = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("start"), Start))
	{
		OutError = TEXT("missing_start");
		return false;
	}
	if (!JsonTryGetVector(Ctx.Params, TEXT("direction"), Direction))
	{
		OutError = TEXT("missing_direction");
		return false;
	}

	const FVector DirectionNormalized = Direction.GetSafeNormal();
	if (DirectionNormalized.IsNearlyZero())
	{
		OutError = TEXT("invalid_direction");
		return false;
	}

	double TraceDistance = HALF_WORLD_MAX;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("trace_distance"), TraceDistance))
	{
		JsonTryGetNumber(Ctx.Params, TEXT("distance"), TraceDistance);
	}
	TraceDistance = FMath::Clamp(TraceDistance, 1.0, (double)HALF_WORLD_MAX);

	ECollisionChannel TraceChannel = ECC_Visibility;
	if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
	{
		OutError = TEXT("unsupported_trace_channel");
		return false;
	}

	bool bTraceComplex = true;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	bool bIncludeAllHits = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_all_hits"), bIncludeAllHits);

	int32 MaxHits = 32;
	{
		double MaxHitsNumber = (double)MaxHits;
		JsonTryGetNumber(Ctx.Params, TEXT("max_hits"), MaxHitsNumber);
		MaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
	}

	bool bIncludeActorFolderTags = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentTraceWorldRay), bTraceComplex);
	TArray<AActor*> IgnoredActors;
	const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
	{
		for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
		{
			const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
			if (IgnoreId.IsEmpty())
			{
				continue;
			}

			if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
			{
				IgnoredActors.Add(IgnoreActor);
			}
		}
	}
	UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, IgnoredActors);
	for (AActor* Ignored : IgnoredActors)
	{
		if (Ignored)
		{
			QueryParams.AddIgnoredActor(Ignored);
		}
	}

	const FVector TraceEnd = Start + (DirectionNormalized * TraceDistance);

	FHitResult PrimaryHit;
	TArray<FHitResult> Hits;
	bool bBlockingHit = false;
	if (bIncludeAllHits)
	{
		World->LineTraceMultiByChannel(Hits, Start, TraceEnd, TraceChannel, QueryParams);
		Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });
		for (const FHitResult& H : Hits)
		{
			if (H.bBlockingHit)
			{
				PrimaryHit = H;
				bBlockingHit = true;
				break;
			}
		}
	}
	else
	{
		bBlockingHit = World->LineTraceSingleByChannel(PrimaryHit, Start, TraceEnd, TraceChannel, QueryParams);
	}

	OutData->SetObjectField(TEXT("start"), VecToJson(Start));
	OutData->SetObjectField(TEXT("direction"), VecToJson(DirectionNormalized));
	OutData->SetNumberField(TEXT("trace_distance"), TraceDistance);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetBoolField(TEXT("include_all_hits"), bIncludeAllHits);
	OutData->SetNumberField(TEXT("max_hits"), MaxHits);
	OutData->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
	OutData->SetBoolField(TEXT("hit"), bBlockingHit);
	if (bBlockingHit)
	{
		if (bIncludeActorFolderTags)
		{
			if (AActor* HitActor = PrimaryHit.GetActor())
			{
				OutData->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

				TArray<TSharedPtr<FJsonValue>> TagValues;
				TagValues.Reserve(HitActor->Tags.Num());
				for (const FName& Tag : HitActor->Tags)
				{
					TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
				}
				OutData->SetArrayField(TEXT("hit_actor_tags"), TagValues);
			}
		}
		UeAgentLevelOps::FillHitResultJson(PrimaryHit, OutData);
	}
	if (bIncludeAllHits)
	{
		OutData->SetNumberField(TEXT("hit_count"), Hits.Num());
		OutData->SetBoolField(TEXT("hits_truncated"), Hits.Num() > MaxHits);

		TArray<TSharedPtr<FJsonValue>> HitItems;
		HitItems.Reserve(FMath::Min(MaxHits, Hits.Num()));
		for (int32 HitIndex = 0; HitIndex < Hits.Num() && HitItems.Num() < MaxHits; ++HitIndex)
		{
			const FHitResult& H = Hits[HitIndex];
			TSharedPtr<FJsonObject> HitObj = UeAgentLevelOps::MakeHitResultSummaryJson(H, bIncludeActorFolderTags, false);
			HitObj->SetNumberField(TEXT("hit_index"), HitIndex);
			HitItems.Add(MakeShared<FJsonValueObject>(HitObj));
		}
		OutData->SetArrayField(TEXT("hits"), HitItems);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelSnapToSurface(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}
	Id = Id.TrimStartAndEnd();

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FVector Start = Actor->GetActorLocation();
	JsonTryGetVector(Ctx.Params, TEXT("start"), Start);

	FVector Direction = FVector(0.0f, 0.0f, -1.0f);
	JsonTryGetVector(Ctx.Params, TEXT("direction"), Direction);
	const FVector DirectionNormalized = Direction.GetSafeNormal();
	if (DirectionNormalized.IsNearlyZero())
	{
		OutError = TEXT("invalid_direction");
		return false;
	}

	double TraceDistance = 100000.0;
	JsonTryGetNumber(Ctx.Params, TEXT("trace_distance"), TraceDistance);
	TraceDistance = FMath::Clamp(TraceDistance, 1.0, (double)HALF_WORLD_MAX);

	ECollisionChannel TraceChannel = ECC_Visibility;
	if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
	{
		OutError = TEXT("unsupported_trace_channel");
		return false;
	}

	bool bTraceComplex = true;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	double OffsetCm = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("offset_cm"), OffsetCm);
	OffsetCm = FMath::Clamp(OffsetCm, -100000.0, 100000.0);

	FString OffsetMode = TEXT("normal");
	JsonTryGetString(Ctx.Params, TEXT("offset_mode"), OffsetMode);
	OffsetMode = OffsetMode.TrimStartAndEnd().ToLower();
	if (OffsetMode.IsEmpty())
	{
		OffsetMode = TEXT("normal");
	}
	if (OffsetMode != TEXT("normal") && OffsetMode != TEXT("direction"))
	{
		OutError = TEXT("unsupported_offset_mode");
		return false;
	}

	bool bAlignRotation = false;
	JsonTryGetBool(Ctx.Params, TEXT("align_rotation"), bAlignRotation);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentSnapToSurface), bTraceComplex);
	QueryParams.AddIgnoredActor(Actor);

	const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
	{
		for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
		{
			const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
			if (IgnoreId.IsEmpty())
			{
				continue;
			}

			if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
			{
				QueryParams.AddIgnoredActor(IgnoreActor);
			}
		}
	}

	const FVector TraceEnd = Start + (DirectionNormalized * TraceDistance);
	FHitResult Hit;
	const bool bHit = World->LineTraceSingleByChannel(Hit, Start, TraceEnd, TraceChannel, QueryParams);

	const FVector PreviousLocation = Actor->GetActorLocation();
	const FRotator PreviousRotation = Actor->GetActorRotation();

	OutData->SetObjectField(TEXT("actor"), UeAgentLevelOps::MakeActorInfoJson(Actor));
	OutData->SetObjectField(TEXT("start"), VecToJson(Start));
	OutData->SetObjectField(TEXT("direction"), VecToJson(DirectionNormalized));
	OutData->SetNumberField(TEXT("trace_distance"), TraceDistance);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetNumberField(TEXT("offset_cm"), OffsetCm);
	OutData->SetStringField(TEXT("offset_mode"), OffsetMode);
	OutData->SetBoolField(TEXT("align_rotation"), bAlignRotation);
	OutData->SetObjectField(TEXT("previous_location"), VecToJson(PreviousLocation));
	OutData->SetObjectField(TEXT("previous_rotation"), RotToJson(PreviousRotation));

	if (!bHit || !Hit.bBlockingHit)
	{
		OutData->SetBoolField(TEXT("snapped"), false);
		UeAgentLevelOps::FillHitResultJson(Hit, OutData);
		return true;
	}

	const FVector OffsetDir = (OffsetMode == TEXT("direction")) ? DirectionNormalized : Hit.ImpactNormal.GetSafeNormal();
	const FVector NewLocation = Hit.ImpactPoint + (OffsetDir * (float)OffsetCm);

	FRotator NewRotation = PreviousRotation;
	if (bAlignRotation)
	{
		const FVector Up = Hit.ImpactNormal.GetSafeNormal();
		FVector Forward = Actor->GetActorForwardVector();
		Forward = (Forward - (Up * FVector::DotProduct(Forward, Up))).GetSafeNormal();
		if (Forward.IsNearlyZero())
		{
			Forward = FVector::CrossProduct(FVector::RightVector, Up).GetSafeNormal();
			if (Forward.IsNearlyZero())
			{
				Forward = FVector::ForwardVector;
			}
		}

		NewRotation = FRotationMatrix::MakeFromXZ(Forward, Up).Rotator();
		NewRotation.Roll = 0.0f;
	}

	Actor->Modify();
	Actor->SetActorLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
	UeAgentLevelOps::FinishActorTransformEdit(Actor);

	OutData->SetBoolField(TEXT("snapped"), true);
	OutData->SetObjectField(TEXT("new_location"), VecToJson(Actor->GetActorLocation()));
	OutData->SetObjectField(TEXT("new_rotation"), RotToJson(Actor->GetActorRotation()));
	UeAgentLevelOps::FillHitResultJson(Hit, OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelSweepCapsule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* SweepsJson = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("sweeps"), SweepsJson) && SweepsJson && SweepsJson->Num() > 0)
	{
		bool bContinueOnError = false;
		JsonTryGetBool(Ctx.Params, TEXT("continue_on_error"), bContinueOnError);

		bool bStopOnBlockingHit = false;
		JsonTryGetBool(Ctx.Params, TEXT("stop_on_blocking_hit"), bStopOnBlockingHit);

		int32 MaxItems = 4096;
		{
			double MaxItemsNumber = (double)MaxItems;
			JsonTryGetNumber(Ctx.Params, TEXT("max_items"), MaxItemsNumber);
			MaxItems = (int32)FMath::Clamp(MaxItemsNumber, 1.0, 20000.0);
		}

		double DefaultRadiusCm = 0.0;
		double DefaultHalfHeightCm = 0.0;
		JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), DefaultRadiusCm);
		JsonTryGetNumber(Ctx.Params, TEXT("half_height_cm"), DefaultHalfHeightCm);
		if (DefaultRadiusCm > 0.0)
		{
			DefaultRadiusCm = FMath::Clamp(DefaultRadiusCm, 0.1, 100000.0);
		}
		if (DefaultHalfHeightCm > 0.0)
		{
			DefaultHalfHeightCm = FMath::Clamp(DefaultHalfHeightCm, 0.1, 100000.0);
		}

		ECollisionChannel DefaultTraceChannel = ECC_Pawn;
		{
			FString ChannelName;
			if (Ctx.Params->TryGetStringField(TEXT("trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
			{
				if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, DefaultTraceChannel))
				{
					OutError = TEXT("unsupported_trace_channel");
					return false;
				}
			}
		}

		bool bDefaultTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bDefaultTraceComplex);

		bool bDefaultFindInitialOverlaps = true;
		JsonTryGetBool(Ctx.Params, TEXT("find_initial_overlaps"), bDefaultFindInitialOverlaps);

		bool bDefaultIncludeAllHits = false;
		JsonTryGetBool(Ctx.Params, TEXT("include_all_hits"), bDefaultIncludeAllHits);

		int32 DefaultMaxHits = 32;
		{
			double MaxHitsNumber = (double)DefaultMaxHits;
			JsonTryGetNumber(Ctx.Params, TEXT("max_hits"), MaxHitsNumber);
			DefaultMaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
		}

		bool bDefaultIncludeActorFolderTags = false;
		JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bDefaultIncludeActorFolderTags);

		bool bDefaultReturnPenetrationDepth = false;
		JsonTryGetBool(Ctx.Params, TEXT("return_penetration_depth"), bDefaultReturnPenetrationDepth);

		TArray<AActor*> GlobalIgnoredActors;
		const TArray<TSharedPtr<FJsonValue>>* GlobalIgnoreIds = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), GlobalIgnoreIds) && GlobalIgnoreIds)
		{
			for (const TSharedPtr<FJsonValue>& IgnoreValue : *GlobalIgnoreIds)
			{
				const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
				if (IgnoreId.IsEmpty())
				{
					continue;
				}

				if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
				{
					GlobalIgnoredActors.Add(IgnoreActor);
				}
			}
		}
		UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, GlobalIgnoredActors);

		const int32 InputCount = SweepsJson->Num();
		const int32 ExecuteCount = FMath::Min(InputCount, MaxItems);
		const bool bTruncatedByMaxItems = InputCount > ExecuteCount;

		int32 ErrorCount = 0;
		int32 BlockingHitCount = 0;
		int32 FirstErrorIndex = INDEX_NONE;
		FString FirstError;
		int32 FirstBlockingIndex = INDEX_NONE;

		TArray<TSharedPtr<FJsonValue>> Results;
		Results.Reserve(ExecuteCount);

		for (int32 Index = 0; Index < ExecuteCount; ++Index)
		{
			const TSharedPtr<FJsonValue>& SweepValue = (*SweepsJson)[Index];
			const TSharedPtr<FJsonObject> SweepObj = SweepValue.IsValid() ? SweepValue->AsObject() : nullptr;
			if (!SweepObj.IsValid())
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("sweeps_invalid_item");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("sweeps_invalid_item");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("sweeps_invalid_item"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			FVector Start = FVector::ZeroVector;
			FVector End = FVector::ZeroVector;
			if (!JsonTryGetVector(SweepObj, TEXT("start"), Start))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_start");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_start");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_start"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}
			if (!JsonTryGetVector(SweepObj, TEXT("end"), End))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_end");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_end");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_end"));
				Item->SetObjectField(TEXT("start"), VecToJson(Start));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			double RadiusCm = DefaultRadiusCm;
			double HalfHeightCm = DefaultHalfHeightCm;
			JsonTryGetNumber(SweepObj, TEXT("radius_cm"), RadiusCm);
			JsonTryGetNumber(SweepObj, TEXT("half_height_cm"), HalfHeightCm);
			if (RadiusCm <= 0.0)
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_radius_cm");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_radius_cm");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_radius_cm"));
				Item->SetObjectField(TEXT("start"), VecToJson(Start));
				Item->SetObjectField(TEXT("end"), VecToJson(End));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}
			if (HalfHeightCm <= 0.0)
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_half_height_cm");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_half_height_cm");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_half_height_cm"));
				Item->SetObjectField(TEXT("start"), VecToJson(Start));
				Item->SetObjectField(TEXT("end"), VecToJson(End));
				Item->SetNumberField(TEXT("radius_cm"), RadiusCm);
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
			HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);

			ECollisionChannel TraceChannel = DefaultTraceChannel;
			{
				FString ChannelName;
				if (SweepObj->TryGetStringField(TEXT("trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
				{
					if (!UeAgentLevelOps::ResolveTraceChannel(SweepObj, TraceChannel))
					{
						++ErrorCount;
						if (FirstErrorIndex == INDEX_NONE)
						{
							FirstErrorIndex = Index;
							FirstError = TEXT("unsupported_trace_channel");
						}
						if (!bContinueOnError)
						{
							OutError = TEXT("unsupported_trace_channel");
							return false;
						}

						TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetNumberField(TEXT("index"), Index);
						Item->SetBoolField(TEXT("ok"), false);
						Item->SetStringField(TEXT("error"), TEXT("unsupported_trace_channel"));
						Item->SetObjectField(TEXT("start"), VecToJson(Start));
						Item->SetObjectField(TEXT("end"), VecToJson(End));
						Item->SetNumberField(TEXT("radius_cm"), RadiusCm);
						Item->SetNumberField(TEXT("half_height_cm"), HalfHeightCm);
						Results.Add(MakeShared<FJsonValueObject>(Item));
						continue;
					}
				}
			}

			bool bTraceComplex = bDefaultTraceComplex;
			SweepObj->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);

			bool bFindInitialOverlaps = bDefaultFindInitialOverlaps;
			SweepObj->TryGetBoolField(TEXT("find_initial_overlaps"), bFindInitialOverlaps);

			bool bIncludeAllHits = bDefaultIncludeAllHits;
			SweepObj->TryGetBoolField(TEXT("include_all_hits"), bIncludeAllHits);

			int32 MaxHits = DefaultMaxHits;
			{
				double MaxHitsNumber = (double)MaxHits;
				JsonTryGetNumber(SweepObj, TEXT("max_hits"), MaxHitsNumber);
				MaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
			}

			bool bIncludeActorFolderTags = bDefaultIncludeActorFolderTags;
			SweepObj->TryGetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

			bool bReturnPenetrationDepth = bDefaultReturnPenetrationDepth;
			SweepObj->TryGetBoolField(TEXT("return_penetration_depth"), bReturnPenetrationDepth);

			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentSweepCapsuleBatch), bTraceComplex);
			QueryParams.bFindInitialOverlaps = bFindInitialOverlaps;
			for (AActor* Ignored : GlobalIgnoredActors)
			{
				if (Ignored)
				{
					QueryParams.AddIgnoredActor(Ignored);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
			if (SweepObj->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
			{
				for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
				{
					const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
					if (IgnoreId.IsEmpty())
					{
						continue;
					}

					if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
					{
						QueryParams.AddIgnoredActor(IgnoreActor);
					}
				}
			}

			const FCollisionShape Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

			FHitResult PrimaryHit;
			TArray<FHitResult> Hits;
			if (bIncludeAllHits)
			{
				World->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, TraceChannel, Shape, QueryParams);
				Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });
				for (const FHitResult& H : Hits)
				{
					if (H.bBlockingHit)
					{
						PrimaryHit = H;
						break;
					}
				}
			}
			else
			{
				World->SweepSingleByChannel(PrimaryHit, Start, End, FQuat::Identity, TraceChannel, Shape, QueryParams);
			}

			const bool bBlockingHit = PrimaryHit.bBlockingHit;
			if (bBlockingHit)
			{
				++BlockingHitCount;
				if (FirstBlockingIndex == INDEX_NONE)
				{
					FirstBlockingIndex = Index;
				}
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetBoolField(TEXT("ok"), true);
			Item->SetObjectField(TEXT("start"), VecToJson(Start));
			Item->SetObjectField(TEXT("end"), VecToJson(End));
			Item->SetNumberField(TEXT("radius_cm"), RadiusCm);
			Item->SetNumberField(TEXT("half_height_cm"), HalfHeightCm);
			Item->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
			Item->SetBoolField(TEXT("trace_complex"), bTraceComplex);
			Item->SetBoolField(TEXT("find_initial_overlaps"), bFindInitialOverlaps);
			Item->SetBoolField(TEXT("include_all_hits"), bIncludeAllHits);
			Item->SetNumberField(TEXT("max_hits"), MaxHits);
			Item->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
			Item->SetBoolField(TEXT("return_penetration_depth"), bReturnPenetrationDepth);
			Item->SetBoolField(TEXT("blocking_hit"), bBlockingHit);
			Item->SetBoolField(TEXT("start_penetrating"), PrimaryHit.bStartPenetrating);
			if (bReturnPenetrationDepth && PrimaryHit.bStartPenetrating)
			{
				Item->SetNumberField(TEXT("penetration_depth_cm"), PrimaryHit.PenetrationDepth);
			}
			UeAgentLevelOps::MaybeAttachStartPenetratingAdvice(
				TEXT("level_sweep_capsule"),
				PrimaryHit.bStartPenetrating,
				bFindInitialOverlaps,
				false,
				0.0,
				Item);
			UeAgentLevelOps::FillHitResultJson(PrimaryHit, Item);
			if (bIncludeActorFolderTags)
			{
				if (AActor* HitActor = PrimaryHit.GetActor())
				{
					Item->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

					TArray<TSharedPtr<FJsonValue>> TagValues;
					TagValues.Reserve(HitActor->Tags.Num());
					for (const FName& Tag : HitActor->Tags)
					{
						TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
					}
					Item->SetArrayField(TEXT("hit_actor_tags"), TagValues);
				}
			}
			if (bIncludeAllHits)
			{
				Item->SetNumberField(TEXT("hit_count"), Hits.Num());
				Item->SetBoolField(TEXT("hits_truncated"), Hits.Num() > MaxHits);

				TArray<TSharedPtr<FJsonValue>> HitItems;
				HitItems.Reserve(FMath::Min(MaxHits, Hits.Num()));
				for (int32 HitIndex = 0; HitIndex < Hits.Num() && HitItems.Num() < MaxHits; ++HitIndex)
				{
					const FHitResult& H = Hits[HitIndex];
					TSharedPtr<FJsonObject> HitObj = UeAgentLevelOps::MakeHitResultSummaryJson(H, bIncludeActorFolderTags, bReturnPenetrationDepth);
					HitObj->SetNumberField(TEXT("hit_index"), HitIndex);
					HitItems.Add(MakeShared<FJsonValueObject>(HitObj));
				}
				Item->SetArrayField(TEXT("hits"), HitItems);
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));

			if (bBlockingHit && bStopOnBlockingHit)
			{
				break;
			}
		}

		const bool bStoppedEarly = bStopOnBlockingHit && FirstBlockingIndex != INDEX_NONE && (FirstBlockingIndex < ExecuteCount - 1);

		OutData->SetStringField(TEXT("mode"), TEXT("batch"));
		OutData->SetNumberField(TEXT("input_sweep_count"), InputCount);
		OutData->SetNumberField(TEXT("sweep_count"), Results.Num());
		OutData->SetNumberField(TEXT("max_items"), MaxItems);
		OutData->SetBoolField(TEXT("truncated"), bTruncatedByMaxItems || bStoppedEarly);
		OutData->SetBoolField(TEXT("continue_on_error"), bContinueOnError);
		OutData->SetBoolField(TEXT("stop_on_blocking_hit"), bStopOnBlockingHit);
		OutData->SetNumberField(TEXT("blocking_hit_count"), BlockingHitCount);
		OutData->SetNumberField(TEXT("first_blocking_index"), FirstBlockingIndex);
		OutData->SetNumberField(TEXT("error_count"), ErrorCount);
		OutData->SetNumberField(TEXT("first_error_index"), FirstErrorIndex);
		if (FirstErrorIndex != INDEX_NONE)
		{
			OutData->SetStringField(TEXT("first_error"), FirstError);
		}
		OutData->SetArrayField(TEXT("sweeps"), Results);
		return true;
	}

	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("start"), Start))
	{
		OutError = TEXT("missing_start");
		return false;
	}
	if (!JsonTryGetVector(Ctx.Params, TEXT("end"), End))
	{
		OutError = TEXT("missing_end");
		return false;
	}

	double RadiusCm = 0.0;
	double HalfHeightCm = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), RadiusCm))
	{
		OutError = TEXT("missing_radius_cm");
		return false;
	}
	if (!JsonTryGetNumber(Ctx.Params, TEXT("half_height_cm"), HalfHeightCm))
	{
		OutError = TEXT("missing_half_height_cm");
		return false;
	}
	RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
	HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);

	ECollisionChannel TraceChannel = ECC_Pawn;
	{
		FString ChannelName;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
		{
			if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
			{
				OutError = TEXT("unsupported_trace_channel");
				return false;
			}
		}
	}

	bool bTraceComplex = false;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	bool bFindInitialOverlaps = true;
	JsonTryGetBool(Ctx.Params, TEXT("find_initial_overlaps"), bFindInitialOverlaps);

	bool bReturnPenetrationDepth = false;
	JsonTryGetBool(Ctx.Params, TEXT("return_penetration_depth"), bReturnPenetrationDepth);

	bool bIncludeActorFolderTags = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

	bool bIncludeAllHits = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_all_hits"), bIncludeAllHits);

	int32 MaxHits = 32;
	{
		double MaxHitsNumber = (double)MaxHits;
		JsonTryGetNumber(Ctx.Params, TEXT("max_hits"), MaxHitsNumber);
		MaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentSweepCapsule), bTraceComplex);
	QueryParams.bFindInitialOverlaps = bFindInitialOverlaps;

	TArray<AActor*> IgnoredActors;
	const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
	{
		for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
		{
			const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
			if (IgnoreId.IsEmpty())
			{
				continue;
			}

			if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
			{
				IgnoredActors.Add(IgnoreActor);
			}
		}
	}
	UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, IgnoredActors);
	for (AActor* Ignored : IgnoredActors)
	{
		if (Ignored)
		{
			QueryParams.AddIgnoredActor(Ignored);
		}
	}

	const FCollisionShape Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

	FHitResult PrimaryHit;
	TArray<FHitResult> Hits;
	if (bIncludeAllHits)
	{
		World->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, TraceChannel, Shape, QueryParams);
		Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });
		for (const FHitResult& H : Hits)
		{
			if (H.bBlockingHit)
			{
				PrimaryHit = H;
				break;
			}
		}
	}
	else
	{
		World->SweepSingleByChannel(PrimaryHit, Start, End, FQuat::Identity, TraceChannel, Shape, QueryParams);
	}

	OutData->SetObjectField(TEXT("start"), VecToJson(Start));
	OutData->SetObjectField(TEXT("end"), VecToJson(End));
	OutData->SetNumberField(TEXT("radius_cm"), RadiusCm);
	OutData->SetNumberField(TEXT("half_height_cm"), HalfHeightCm);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetBoolField(TEXT("find_initial_overlaps"), bFindInitialOverlaps);
	OutData->SetBoolField(TEXT("include_all_hits"), bIncludeAllHits);
	OutData->SetNumberField(TEXT("max_hits"), MaxHits);
	OutData->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
	OutData->SetBoolField(TEXT("return_penetration_depth"), bReturnPenetrationDepth);
	OutData->SetBoolField(TEXT("blocking_hit"), PrimaryHit.bBlockingHit);
	OutData->SetBoolField(TEXT("start_penetrating"), PrimaryHit.bStartPenetrating);
	if (bReturnPenetrationDepth && PrimaryHit.bStartPenetrating)
	{
		OutData->SetNumberField(TEXT("penetration_depth_cm"), PrimaryHit.PenetrationDepth);
	}
	UeAgentLevelOps::MaybeAttachStartPenetratingAdvice(
		TEXT("level_sweep_capsule"),
		PrimaryHit.bStartPenetrating,
		bFindInitialOverlaps,
		false,
		0.0,
		OutData);
	UeAgentLevelOps::FillHitResultJson(PrimaryHit, OutData);
	if (bIncludeActorFolderTags)
	{
		if (AActor* HitActor = PrimaryHit.GetActor())
		{
			OutData->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

			TArray<TSharedPtr<FJsonValue>> TagValues;
			TagValues.Reserve(HitActor->Tags.Num());
			for (const FName& Tag : HitActor->Tags)
			{
				TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			OutData->SetArrayField(TEXT("hit_actor_tags"), TagValues);
		}
	}
	if (bIncludeAllHits)
	{
		OutData->SetNumberField(TEXT("hit_count"), Hits.Num());
		OutData->SetBoolField(TEXT("hits_truncated"), Hits.Num() > MaxHits);

		TArray<TSharedPtr<FJsonValue>> HitItems;
		HitItems.Reserve(FMath::Min(MaxHits, Hits.Num()));
		for (int32 HitIndex = 0; HitIndex < Hits.Num() && HitItems.Num() < MaxHits; ++HitIndex)
		{
			const FHitResult& H = Hits[HitIndex];
			TSharedPtr<FJsonObject> HitObj = UeAgentLevelOps::MakeHitResultSummaryJson(H, bIncludeActorFolderTags, bReturnPenetrationDepth);
			HitObj->SetNumberField(TEXT("hit_index"), HitIndex);
			HitItems.Add(MakeShared<FJsonValueObject>(HitObj));
		}
		OutData->SetArrayField(TEXT("hits"), HitItems);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelSweepCapsulePath(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PointsJson = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("points"), PointsJson) || !PointsJson || PointsJson->Num() < 2)
	{
		OutError = TEXT("points_required");
		return false;
	}

	double RadiusCm = 0.0;
	double HalfHeightCm = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), RadiusCm))
	{
		OutError = TEXT("missing_radius_cm");
		return false;
	}
	if (!JsonTryGetNumber(Ctx.Params, TEXT("half_height_cm"), HalfHeightCm))
	{
		OutError = TEXT("missing_half_height_cm");
		return false;
	}
	RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
	HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);

	double StepCm = 50.0;
	JsonTryGetNumber(Ctx.Params, TEXT("step_cm"), StepCm);
	StepCm = FMath::Clamp(StepCm, 0.1, 100000.0);

	int32 MaxSamples = 2048;
	{
		double MaxSamplesNumber = (double)MaxSamples;
		JsonTryGetNumber(Ctx.Params, TEXT("max_samples"), MaxSamplesNumber);
		MaxSamples = (int32)FMath::Clamp(MaxSamplesNumber, 2.0, 20000.0);
	}

	FString PointsMode = TEXT("feet");
	JsonTryGetString(Ctx.Params, TEXT("points_mode"), PointsMode);
	PointsMode = PointsMode.TrimStartAndEnd().ToLower();
	const bool bPointsAreFeet = PointsMode.IsEmpty() || PointsMode == TEXT("feet");
	const bool bPointsAreCenter = PointsMode == TEXT("center");
	if (!bPointsAreFeet && !bPointsAreCenter)
	{
		OutError = TEXT("unsupported_points_mode");
		return false;
	}

	bool bSnapToFloor = bPointsAreFeet;
	JsonTryGetBool(Ctx.Params, TEXT("snap_to_floor"), bSnapToFloor);

	bool bRequireFloor = bPointsAreFeet && bSnapToFloor;
	JsonTryGetBool(Ctx.Params, TEXT("require_floor"), bRequireFloor);

	double FloorClearanceCm = 2.0;
	JsonTryGetNumber(Ctx.Params, TEXT("floor_clearance_cm"), FloorClearanceCm);
	FloorClearanceCm = FMath::Clamp(FloorClearanceCm, 0.0, 1000.0);

	double FloorTraceUpCm = 50.0;
	double FloorTraceDownCm = 200.0;
	JsonTryGetNumber(Ctx.Params, TEXT("floor_trace_up_cm"), FloorTraceUpCm);
	JsonTryGetNumber(Ctx.Params, TEXT("floor_trace_down_cm"), FloorTraceDownCm);
	FloorTraceUpCm = FMath::Clamp(FloorTraceUpCm, 0.0, 100000.0);
	FloorTraceDownCm = FMath::Clamp(FloorTraceDownCm, 0.0, 100000.0);

	bool bIgnoreWalkableFloorHits = false;
	JsonTryGetBool(Ctx.Params, TEXT("ignore_walkable_floor_hits"), bIgnoreWalkableFloorHits);

	double MaxWalkableSlopeDeg = 45.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_walkable_slope_deg"), MaxWalkableSlopeDeg);
	MaxWalkableSlopeDeg = FMath::Clamp(MaxWalkableSlopeDeg, 0.0, 89.0);
	const double WalkableCos = FMath::Cos(FMath::DegreesToRadians(MaxWalkableSlopeDeg));

	bool bStopOnBlockingHit = true;
	JsonTryGetBool(Ctx.Params, TEXT("stop_on_blocking_hit"), bStopOnBlockingHit);

	bool bIncludeSamples = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_samples"), bIncludeSamples);
	bool bIncludeSegments = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_segments"), bIncludeSegments);

	ECollisionChannel TraceChannel = ECC_Pawn;
	{
		FString ChannelName;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
		{
			if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
			{
				OutError = TEXT("unsupported_trace_channel");
				return false;
			}
		}
	}

	bool bTraceComplex = false;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	bool bFindInitialOverlaps = true;
	JsonTryGetBool(Ctx.Params, TEXT("find_initial_overlaps"), bFindInitialOverlaps);

	bool bReturnPenetrationDepth = false;
	JsonTryGetBool(Ctx.Params, TEXT("return_penetration_depth"), bReturnPenetrationDepth);

	bool bIncludeActorFolderTags = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

	// Optional floor trace settings.
	ECollisionChannel FloorTraceChannel = ECC_Visibility;
	{
		FString ChannelName;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("floor_trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
		{
			const FString Normalized = ChannelName.TrimStartAndEnd().ToLower();
			if (Normalized == TEXT("visibility") || Normalized == TEXT("ecc_visibility"))
			{
				FloorTraceChannel = ECC_Visibility;
			}
			else if (Normalized == TEXT("camera") || Normalized == TEXT("ecc_camera"))
			{
				FloorTraceChannel = ECC_Camera;
			}
			else if (Normalized == TEXT("worldstatic") || Normalized == TEXT("world_static") || Normalized == TEXT("ecc_worldstatic"))
			{
				FloorTraceChannel = ECC_WorldStatic;
			}
			else if (Normalized == TEXT("worlddynamic") || Normalized == TEXT("world_dynamic") || Normalized == TEXT("ecc_worlddynamic"))
			{
				FloorTraceChannel = ECC_WorldDynamic;
			}
			else if (Normalized == TEXT("pawn") || Normalized == TEXT("ecc_pawn"))
			{
				FloorTraceChannel = ECC_Pawn;
			}
			else if (Normalized == TEXT("physicsbody") || Normalized == TEXT("physics_body") || Normalized == TEXT("ecc_physicsbody"))
			{
				FloorTraceChannel = ECC_PhysicsBody;
			}
			else
			{
				OutError = TEXT("unsupported_floor_trace_channel");
				return false;
			}
		}
	}

	bool bFloorTraceComplex = true;
	JsonTryGetBool(Ctx.Params, TEXT("floor_trace_complex"), bFloorTraceComplex);

	const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
	TArray<AActor*> IgnoredActors;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
	{
		for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
		{
			const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
			if (IgnoreId.IsEmpty())
			{
				continue;
			}

			if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
			{
				IgnoredActors.Add(IgnoreActor);
			}
		}
	}
	UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, IgnoredActors);

	auto TryGetVectorFromValue = [](const TSharedPtr<FJsonValue>& Value, FVector& OutVec) -> bool
	{
		return UeAgentJsonDiagnostics::TryReadVectorValue(Value, OutVec);
	};

	TArray<FVector> InputPoints;
	InputPoints.Reserve(PointsJson->Num());
	for (const TSharedPtr<FJsonValue>& Value : *PointsJson)
	{
		FVector Point;
		if (!TryGetVectorFromValue(Value, Point))
		{
			OutError = TEXT("points_invalid_vector");
			return false;
		}
		InputPoints.Add(Point);
	}

	TArray<FVector> SamplePoints;
	SamplePoints.Reserve(FMath::Min(MaxSamples, (int32)(InputPoints.Num() * 8)));
	bool bTruncated = false;
	for (int32 i = 0; i < InputPoints.Num() - 1; ++i)
	{
		const FVector A = InputPoints[i];
		const FVector B = InputPoints[i + 1];
		const double Dist = FVector::Distance(A, B);
		const int32 Steps = FMath::Max(1, (int32)FMath::CeilToDouble(Dist / StepCm));

		for (int32 s = (i == 0 ? 0 : 1); s <= Steps; ++s)
		{
			const double Alpha = Steps > 0 ? ((double)s / (double)Steps) : 1.0;
			const FVector P = FMath::Lerp(A, B, (float)Alpha);
			SamplePoints.Add(P);
			if (SamplePoints.Num() >= MaxSamples)
			{
				bTruncated = true;
				break;
			}
		}
		if (bTruncated)
		{
			break;
		}
	}

	struct FSample
	{
		FVector Input = FVector::ZeroVector;
		FVector Feet = FVector::ZeroVector;
		FVector Center = FVector::ZeroVector;
		bool bFloorHit = false;
		FVector FloorNormal = FVector::UpVector;
	};

	TArray<FSample> Samples;
	Samples.Reserve(SamplePoints.Num());

	bool bFloorMissing = false;
	int32 FirstMissingFloorSampleIndex = INDEX_NONE;

	FCollisionQueryParams FloorQueryParams(SCENE_QUERY_STAT(UeAgentSweepCapsulePathFloor), bFloorTraceComplex);
	for (AActor* Ignored : IgnoredActors)
	{
		if (Ignored)
		{
			FloorQueryParams.AddIgnoredActor(Ignored);
		}
	}

	for (int32 Index = 0; Index < SamplePoints.Num(); ++Index)
	{
		FSample Sample;
		Sample.Input = SamplePoints[Index];

		if (bPointsAreCenter)
		{
			Sample.Center = Sample.Input;
			Sample.Feet = Sample.Input - FVector(0.0f, 0.0f, (float)(HalfHeightCm + FloorClearanceCm));
			Samples.Add(Sample);
			continue;
		}

		Sample.Feet = Sample.Input;
		if (bSnapToFloor)
		{
			const FVector TraceStart = Sample.Feet + FVector(0.0f, 0.0f, (float)FloorTraceUpCm);
			const FVector TraceEnd = Sample.Feet - FVector(0.0f, 0.0f, (float)FloorTraceDownCm);
			FHitResult FloorHit;
			const bool bHit = World->LineTraceSingleByChannel(FloorHit, TraceStart, TraceEnd, FloorTraceChannel, FloorQueryParams);
			if (bHit && FloorHit.bBlockingHit)
			{
				Sample.bFloorHit = true;
				Sample.Feet = FloorHit.ImpactPoint;
				Sample.FloorNormal = FloorHit.ImpactNormal.GetSafeNormal();
			}
		}

		if (bRequireFloor && !Sample.bFloorHit)
		{
			bFloorMissing = true;
			FirstMissingFloorSampleIndex = Index;
			break;
		}

		Sample.Center = Sample.Feet + FVector(0.0f, 0.0f, (float)(HalfHeightCm + FloorClearanceCm));
		Samples.Add(Sample);
	}

	OutData->SetStringField(TEXT("points_mode"), bPointsAreCenter ? TEXT("center") : TEXT("feet"));
	OutData->SetBoolField(TEXT("snap_to_floor"), bSnapToFloor);
	OutData->SetBoolField(TEXT("require_floor"), bRequireFloor);
	OutData->SetNumberField(TEXT("radius_cm"), RadiusCm);
	OutData->SetNumberField(TEXT("half_height_cm"), HalfHeightCm);
	OutData->SetNumberField(TEXT("step_cm"), StepCm);
	OutData->SetNumberField(TEXT("max_samples"), MaxSamples);
	OutData->SetBoolField(TEXT("truncated"), bTruncated);
	OutData->SetNumberField(TEXT("floor_clearance_cm"), FloorClearanceCm);
	OutData->SetNumberField(TEXT("floor_trace_up_cm"), FloorTraceUpCm);
	OutData->SetNumberField(TEXT("floor_trace_down_cm"), FloorTraceDownCm);
	OutData->SetStringField(TEXT("floor_trace_channel"), FString::Printf(TEXT("%d"), (int32)FloorTraceChannel));
	OutData->SetBoolField(TEXT("floor_trace_complex"), bFloorTraceComplex);
	OutData->SetBoolField(TEXT("ignore_walkable_floor_hits"), bIgnoreWalkableFloorHits);
	OutData->SetNumberField(TEXT("max_walkable_slope_deg"), MaxWalkableSlopeDeg);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetBoolField(TEXT("find_initial_overlaps"), bFindInitialOverlaps);
	OutData->SetBoolField(TEXT("return_penetration_depth"), bReturnPenetrationDepth);
	OutData->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
	OutData->SetBoolField(TEXT("stop_on_blocking_hit"), bStopOnBlockingHit);
	OutData->SetBoolField(TEXT("include_samples"), bIncludeSamples);
	OutData->SetBoolField(TEXT("include_segments"), bIncludeSegments);

	OutData->SetNumberField(TEXT("input_point_count"), InputPoints.Num());
	OutData->SetNumberField(TEXT("sample_count"), Samples.Num());
	OutData->SetBoolField(TEXT("floor_missing"), bFloorMissing);
	if (bFloorMissing)
	{
		OutData->SetNumberField(TEXT("first_missing_floor_sample_index"), FirstMissingFloorSampleIndex);
		OutData->SetBoolField(TEXT("blocking_hit"), false);
		OutData->SetNumberField(TEXT("segment_count"), FMath::Max(0, Samples.Num() - 1));
		OutData->SetBoolField(TEXT("path_valid"), false);
		OutData->SetStringField(TEXT("path_invalid_reason"), TEXT("floor_missing"));

		if (bIncludeSamples)
		{
			TArray<TSharedPtr<FJsonValue>> SamplesArr;
			SamplesArr.Reserve(Samples.Num());
			for (const FSample& S : Samples)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetObjectField(TEXT("input"), VecToJson(S.Input));
				Obj->SetObjectField(TEXT("feet"), VecToJson(S.Feet));
				Obj->SetObjectField(TEXT("center"), VecToJson(S.Center));
				Obj->SetBoolField(TEXT("floor_hit"), S.bFloorHit);
				Obj->SetObjectField(TEXT("floor_normal"), VecToJson(S.FloorNormal));
				SamplesArr.Add(MakeShared<FJsonValueObject>(Obj));
			}
			OutData->SetArrayField(TEXT("samples"), SamplesArr);
		}
		return true;
	}

	FCollisionQueryParams SweepParams(SCENE_QUERY_STAT(UeAgentSweepCapsulePath), bTraceComplex);
	SweepParams.bFindInitialOverlaps = bFindInitialOverlaps;
	for (AActor* Ignored : IgnoredActors)
	{
		if (Ignored)
		{
			SweepParams.AddIgnoredActor(Ignored);
		}
	}

	const FCollisionShape Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

	bool bBlockingHitFound = false;
	int32 FirstBlockingSegmentIndex = INDEX_NONE;
	FHitResult FirstBlockingHit;

	TArray<TSharedPtr<FJsonValue>> SegmentArr;
	if (bIncludeSegments)
	{
		SegmentArr.Reserve(FMath::Max(0, Samples.Num() - 1));
	}

	const double FloorZTolerance = FloorClearanceCm + 5.0;
	for (int32 SegIndex = 0; SegIndex < Samples.Num() - 1; ++SegIndex)
	{
		const FVector StartCenter = Samples[SegIndex].Center;
		const FVector EndCenter = Samples[SegIndex + 1].Center;

		TArray<FHitResult> Hits;
		World->SweepMultiByChannel(Hits, StartCenter, EndCenter, FQuat::Identity, TraceChannel, Shape, SweepParams);
		Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });

		int32 IgnoredFloorHitCount = 0;
		FHitResult BlockingHit;
		for (const FHitResult& Hit : Hits)
		{
			if (!Hit.bBlockingHit)
			{
				continue;
			}

			bool bIgnored = false;
			if (bIgnoreWalkableFloorHits && bPointsAreFeet && bSnapToFloor && Samples[SegIndex].bFloorHit && Samples[SegIndex + 1].bFloorHit)
			{
				const FVector N = Hit.ImpactNormal.GetSafeNormal();
				const double DotUp = FVector::DotProduct(N, FVector::UpVector);
				if (DotUp >= WalkableCos)
				{
					const double MinFloorZ = FMath::Min((double)Samples[SegIndex].Feet.Z, (double)Samples[SegIndex + 1].Feet.Z) - FloorZTolerance;
					const double MaxFloorZ = FMath::Max((double)Samples[SegIndex].Feet.Z, (double)Samples[SegIndex + 1].Feet.Z) + FloorZTolerance;
					if ((double)Hit.ImpactPoint.Z >= MinFloorZ && (double)Hit.ImpactPoint.Z <= MaxFloorZ)
					{
						bIgnored = true;
					}
				}
			}

			if (bIgnored)
			{
				++IgnoredFloorHitCount;
				continue;
			}

			BlockingHit = Hit;
			break;
		}

		const bool bSegmentBlocked = BlockingHit.bBlockingHit;
		if (bSegmentBlocked && !bBlockingHitFound)
		{
			bBlockingHitFound = true;
			FirstBlockingSegmentIndex = SegIndex;
			FirstBlockingHit = BlockingHit;
		}

		if (bIncludeSegments)
		{
			TSharedPtr<FJsonObject> SegmentObj = MakeShared<FJsonObject>();
			SegmentObj->SetNumberField(TEXT("segment_index"), SegIndex);
			SegmentObj->SetObjectField(TEXT("start_center"), VecToJson(StartCenter));
			SegmentObj->SetObjectField(TEXT("end_center"), VecToJson(EndCenter));
			SegmentObj->SetNumberField(TEXT("hit_count"), Hits.Num());
			SegmentObj->SetNumberField(TEXT("ignored_floor_hit_count"), IgnoredFloorHitCount);
			SegmentObj->SetBoolField(TEXT("blocking_hit"), bSegmentBlocked);
			if (bSegmentBlocked)
			{
				SegmentObj->SetNumberField(TEXT("hit_time"), BlockingHit.Time);
				SegmentObj->SetBoolField(TEXT("start_penetrating"), BlockingHit.bStartPenetrating);
				if (bReturnPenetrationDepth && BlockingHit.bStartPenetrating)
				{
					SegmentObj->SetNumberField(TEXT("penetration_depth_cm"), BlockingHit.PenetrationDepth);
				}
				UeAgentLevelOps::MaybeAttachStartPenetratingAdvice(
					TEXT("level_sweep_capsule_path.segment"),
					BlockingHit.bStartPenetrating,
					bFindInitialOverlaps,
					true,
					FloorClearanceCm,
					SegmentObj);
				if (bIncludeActorFolderTags)
				{
					if (AActor* HitActor = BlockingHit.GetActor())
					{
						SegmentObj->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

						TArray<TSharedPtr<FJsonValue>> TagValues;
						TagValues.Reserve(HitActor->Tags.Num());
						for (const FName& Tag : HitActor->Tags)
						{
							TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
						}
						SegmentObj->SetArrayField(TEXT("hit_actor_tags"), TagValues);
					}
				}
				UeAgentLevelOps::FillHitResultJson(BlockingHit, SegmentObj);
			}
			SegmentArr.Add(MakeShared<FJsonValueObject>(SegmentObj));
		}

		if (bSegmentBlocked && bStopOnBlockingHit)
		{
			break;
		}
	}

	OutData->SetBoolField(TEXT("path_valid"), true);
	OutData->SetNumberField(TEXT("segment_count"), FMath::Max(0, Samples.Num() - 1));
	OutData->SetBoolField(TEXT("blocking_hit"), bBlockingHitFound);
	OutData->SetNumberField(TEXT("first_blocking_segment_index"), FirstBlockingSegmentIndex);
	if (bBlockingHitFound)
	{
		OutData->SetNumberField(TEXT("first_hit_time"), FirstBlockingHit.Time);
		OutData->SetBoolField(TEXT("first_start_penetrating"), FirstBlockingHit.bStartPenetrating);
		if (bReturnPenetrationDepth && FirstBlockingHit.bStartPenetrating)
		{
			OutData->SetNumberField(TEXT("first_penetration_depth_cm"), FirstBlockingHit.PenetrationDepth);
		}
		UeAgentLevelOps::MaybeAttachStartPenetratingAdvice(
			TEXT("level_sweep_capsule_path"),
			FirstBlockingHit.bStartPenetrating,
			bFindInitialOverlaps,
			true,
			FloorClearanceCm,
			OutData);
		if (bIncludeActorFolderTags)
		{
			if (AActor* HitActor = FirstBlockingHit.GetActor())
			{
				OutData->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

				TArray<TSharedPtr<FJsonValue>> TagValues;
				TagValues.Reserve(HitActor->Tags.Num());
				for (const FName& Tag : HitActor->Tags)
				{
					TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
				}
				OutData->SetArrayField(TEXT("hit_actor_tags"), TagValues);
			}
		}
		UeAgentLevelOps::FillHitResultJson(FirstBlockingHit, OutData);
	}

	if (bIncludeSamples)
	{
		TArray<TSharedPtr<FJsonValue>> SamplesArr;
		SamplesArr.Reserve(Samples.Num());
		for (const FSample& S : Samples)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetObjectField(TEXT("input"), VecToJson(S.Input));
			Obj->SetObjectField(TEXT("feet"), VecToJson(S.Feet));
			Obj->SetObjectField(TEXT("center"), VecToJson(S.Center));
			Obj->SetBoolField(TEXT("floor_hit"), S.bFloorHit);
			Obj->SetObjectField(TEXT("floor_normal"), VecToJson(S.FloorNormal));
			SamplesArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		OutData->SetArrayField(TEXT("samples"), SamplesArr);
	}
	if (bIncludeSegments)
	{
		OutData->SetArrayField(TEXT("segments"), SegmentArr);
	}

	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelCheckOverlaps(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ChecksJson = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("checks"), ChecksJson) && ChecksJson && ChecksJson->Num() > 0)
	{
		bool bContinueOnError = false;
		JsonTryGetBool(Ctx.Params, TEXT("continue_on_error"), bContinueOnError);

		bool bStopOnOverlap = false;
		JsonTryGetBool(Ctx.Params, TEXT("stop_on_overlap"), bStopOnOverlap);

		int32 MaxItems = 4096;
		{
			double MaxItemsNumber = (double)MaxItems;
			JsonTryGetNumber(Ctx.Params, TEXT("max_items"), MaxItemsNumber);
			MaxItems = (int32)FMath::Clamp(MaxItemsNumber, 1.0, 20000.0);
		}

		ECollisionChannel DefaultTraceChannel = ECC_Visibility;
		if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, DefaultTraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		bool bDefaultTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bDefaultTraceComplex);

		double DefaultLimitD = 100.0;
		JsonTryGetNumber(Ctx.Params, TEXT("limit"), DefaultLimitD);
		const int32 DefaultLimit = static_cast<int32>(FMath::Clamp(DefaultLimitD, 0.0, 5000.0));

		bool bDefaultIncludeOverlaps = true;
		JsonTryGetBool(Ctx.Params, TEXT("include_overlaps"), bDefaultIncludeOverlaps);

		bool bDefaultIncludeActorFolderTags = false;
		JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bDefaultIncludeActorFolderTags);

		TArray<AActor*> GlobalIgnoredActors;
		const TArray<TSharedPtr<FJsonValue>>* GlobalIgnoreIds = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), GlobalIgnoreIds) && GlobalIgnoreIds)
		{
			for (const TSharedPtr<FJsonValue>& IgnoreValue : *GlobalIgnoreIds)
			{
				const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
				if (IgnoreId.IsEmpty())
				{
					continue;
				}

				if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
				{
					GlobalIgnoredActors.Add(IgnoreActor);
				}
			}
		}
		UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, GlobalIgnoredActors);

		const int32 InputCount = ChecksJson->Num();
		const int32 ExecuteCount = FMath::Min(InputCount, MaxItems);
		const bool bTruncatedByMaxItems = InputCount > ExecuteCount;

		int32 ErrorCount = 0;
		int32 FirstErrorIndex = INDEX_NONE;
		FString FirstError;
		int32 TotalOverlapCount = 0;
		int32 FirstOverlappingIndex = INDEX_NONE;

		TArray<TSharedPtr<FJsonValue>> Results;
		Results.Reserve(ExecuteCount);

		for (int32 Index = 0; Index < ExecuteCount; ++Index)
		{
			const TSharedPtr<FJsonValue>& CheckValue = (*ChecksJson)[Index];
			const TSharedPtr<FJsonObject> CheckObj = CheckValue.IsValid() ? CheckValue->AsObject() : nullptr;
			if (!CheckObj.IsValid())
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("checks_invalid_item");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("checks_invalid_item");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("checks_invalid_item"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			FString ShapeName;
			if (!JsonTryGetString(CheckObj, TEXT("shape"), ShapeName) || ShapeName.TrimStartAndEnd().IsEmpty())
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_shape");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_shape");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_shape"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			FVector Center = FVector::ZeroVector;
			if (!JsonTryGetVector(CheckObj, TEXT("center"), Center))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_center");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_center");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_center"));
				Item->SetStringField(TEXT("shape"), ShapeName.TrimStartAndEnd().ToLower());
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			const FString ShapeNormalized = ShapeName.TrimStartAndEnd().ToLower();
			FCollisionShape Shape;
			FQuat RotationQuat = FQuat::Identity;

			if (ShapeNormalized == TEXT("box"))
			{
				FVector Extent = FVector::ZeroVector;
				if (!JsonTryGetVector(CheckObj, TEXT("box_extent"), Extent))
				{
					++ErrorCount;
					if (FirstErrorIndex == INDEX_NONE)
					{
						FirstErrorIndex = Index;
						FirstError = TEXT("missing_box_extent");
					}
					if (!bContinueOnError)
					{
						OutError = TEXT("missing_box_extent");
						return false;
					}

					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetBoolField(TEXT("ok"), false);
					Item->SetStringField(TEXT("error"), TEXT("missing_box_extent"));
					Item->SetStringField(TEXT("shape"), ShapeNormalized);
					Item->SetObjectField(TEXT("center"), VecToJson(Center));
					Results.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}
				Extent.X = (float)FMath::Max(0.1, (double)Extent.X);
				Extent.Y = (float)FMath::Max(0.1, (double)Extent.Y);
				Extent.Z = (float)FMath::Max(0.1, (double)Extent.Z);
				Shape = FCollisionShape::MakeBox(Extent);

				FRotator Rotation = FRotator::ZeroRotator;
				if (JsonTryGetRotator(CheckObj, TEXT("rotation"), Rotation))
				{
					RotationQuat = Rotation.Quaternion();
				}
			}
			else if (ShapeNormalized == TEXT("sphere"))
			{
				double RadiusCm = 0.0;
				if (!JsonTryGetNumber(CheckObj, TEXT("radius_cm"), RadiusCm))
				{
					++ErrorCount;
					if (FirstErrorIndex == INDEX_NONE)
					{
						FirstErrorIndex = Index;
						FirstError = TEXT("missing_radius_cm");
					}
					if (!bContinueOnError)
					{
						OutError = TEXT("missing_radius_cm");
						return false;
					}

					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetBoolField(TEXT("ok"), false);
					Item->SetStringField(TEXT("error"), TEXT("missing_radius_cm"));
					Item->SetStringField(TEXT("shape"), ShapeNormalized);
					Item->SetObjectField(TEXT("center"), VecToJson(Center));
					Results.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}
				RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
				Shape = FCollisionShape::MakeSphere((float)RadiusCm);
			}
			else if (ShapeNormalized == TEXT("capsule"))
			{
				double RadiusCm = 0.0;
				double HalfHeightCm = 0.0;
				if (!JsonTryGetNumber(CheckObj, TEXT("radius_cm"), RadiusCm))
				{
					++ErrorCount;
					if (FirstErrorIndex == INDEX_NONE)
					{
						FirstErrorIndex = Index;
						FirstError = TEXT("missing_radius_cm");
					}
					if (!bContinueOnError)
					{
						OutError = TEXT("missing_radius_cm");
						return false;
					}

					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetBoolField(TEXT("ok"), false);
					Item->SetStringField(TEXT("error"), TEXT("missing_radius_cm"));
					Item->SetStringField(TEXT("shape"), ShapeNormalized);
					Item->SetObjectField(TEXT("center"), VecToJson(Center));
					Results.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}
				if (!JsonTryGetNumber(CheckObj, TEXT("half_height_cm"), HalfHeightCm))
				{
					++ErrorCount;
					if (FirstErrorIndex == INDEX_NONE)
					{
						FirstErrorIndex = Index;
						FirstError = TEXT("missing_half_height_cm");
					}
					if (!bContinueOnError)
					{
						OutError = TEXT("missing_half_height_cm");
						return false;
					}

					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetBoolField(TEXT("ok"), false);
					Item->SetStringField(TEXT("error"), TEXT("missing_half_height_cm"));
					Item->SetStringField(TEXT("shape"), ShapeNormalized);
					Item->SetObjectField(TEXT("center"), VecToJson(Center));
					Item->SetNumberField(TEXT("radius_cm"), RadiusCm);
					Results.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}
				RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
				HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);
				Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

				FRotator Rotation = FRotator::ZeroRotator;
				if (JsonTryGetRotator(CheckObj, TEXT("rotation"), Rotation))
				{
					RotationQuat = Rotation.Quaternion();
				}
			}
			else
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("unsupported_shape");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("unsupported_shape");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("unsupported_shape"));
				Item->SetStringField(TEXT("shape"), ShapeNormalized);
				Item->SetObjectField(TEXT("center"), VecToJson(Center));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			ECollisionChannel TraceChannel = DefaultTraceChannel;
			if (!UeAgentLevelOps::ResolveTraceChannel(CheckObj, TraceChannel))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("unsupported_trace_channel");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("unsupported_trace_channel");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("unsupported_trace_channel"));
				Item->SetStringField(TEXT("shape"), ShapeNormalized);
				Item->SetObjectField(TEXT("center"), VecToJson(Center));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			bool bTraceComplex = bDefaultTraceComplex;
			CheckObj->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);

			double LimitD = (double)DefaultLimit;
			JsonTryGetNumber(CheckObj, TEXT("limit"), LimitD);
			const int32 Limit = static_cast<int32>(FMath::Clamp(LimitD, 0.0, 5000.0));

			bool bIncludeOverlaps = bDefaultIncludeOverlaps;
			CheckObj->TryGetBoolField(TEXT("include_overlaps"), bIncludeOverlaps);

			bool bIncludeActorFolderTags = bDefaultIncludeActorFolderTags;
			CheckObj->TryGetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentCheckOverlapsBatch), bTraceComplex);
			for (AActor* Ignored : GlobalIgnoredActors)
			{
				if (Ignored)
				{
					QueryParams.AddIgnoredActor(Ignored);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
			if (CheckObj->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
			{
				for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
				{
					const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
					if (IgnoreId.IsEmpty())
					{
						continue;
					}

					if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
					{
						QueryParams.AddIgnoredActor(IgnoreActor);
					}
				}
			}

			TArray<FOverlapResult> OverlapResults;
			World->OverlapMultiByChannel(OverlapResults, Center, RotationQuat, TraceChannel, Shape, QueryParams);

			TotalOverlapCount += OverlapResults.Num();
			if (FirstOverlappingIndex == INDEX_NONE && OverlapResults.Num() > 0)
			{
				FirstOverlappingIndex = Index;
			}

			const bool bReturnOverlaps = bIncludeOverlaps && Limit > 0;
			TArray<TSharedPtr<FJsonValue>> Overlaps;
			if (bReturnOverlaps)
			{
				Overlaps.Reserve(FMath::Min(Limit, OverlapResults.Num()));
				for (const FOverlapResult& Result : OverlapResults)
				{
					if (Overlaps.Num() >= Limit)
					{
						break;
					}

					TSharedPtr<FJsonObject> OverlapItem = MakeShared<FJsonObject>();
					TSharedPtr<FJsonObject> ActorSummary = UeAgentLevelOps::MakeActorSummaryJson(Result.GetActor());
					if (bIncludeActorFolderTags && Result.GetActor())
					{
						UeAgentLevelOps::AugmentActorSummaryWithFolderTags(Result.GetActor(), ActorSummary);
					}
					OverlapItem->SetObjectField(TEXT("actor"), ActorSummary);
					if (Result.GetComponent())
					{
						OverlapItem->SetStringField(TEXT("component_name"), Result.GetComponent()->GetName());
						OverlapItem->SetStringField(TEXT("component_path"), Result.GetComponent()->GetPathName());
					}
					Overlaps.Add(MakeShared<FJsonValueObject>(OverlapItem));
				}
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetBoolField(TEXT("ok"), true);
			Item->SetStringField(TEXT("shape"), ShapeNormalized);
			Item->SetObjectField(TEXT("center"), VecToJson(Center));
			Item->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
			Item->SetBoolField(TEXT("trace_complex"), bTraceComplex);
			Item->SetNumberField(TEXT("limit"), Limit);
			Item->SetBoolField(TEXT("include_overlaps"), bIncludeOverlaps);
			Item->SetBoolField(TEXT("overlaps_included"), bReturnOverlaps);
			Item->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
			Item->SetNumberField(TEXT("overlap_count"), OverlapResults.Num());
			if (bReturnOverlaps)
			{
				Item->SetArrayField(TEXT("overlaps"), Overlaps);
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));

			if (bStopOnOverlap && OverlapResults.Num() > 0)
			{
				break;
			}
		}

		const bool bStoppedEarly = bStopOnOverlap && FirstOverlappingIndex != INDEX_NONE && (FirstOverlappingIndex < ExecuteCount - 1);

		OutData->SetStringField(TEXT("mode"), TEXT("batch"));
		OutData->SetNumberField(TEXT("input_check_count"), InputCount);
		OutData->SetNumberField(TEXT("check_count"), Results.Num());
		OutData->SetNumberField(TEXT("max_items"), MaxItems);
		OutData->SetBoolField(TEXT("truncated"), bTruncatedByMaxItems || bStoppedEarly);
		OutData->SetBoolField(TEXT("continue_on_error"), bContinueOnError);
		OutData->SetBoolField(TEXT("stop_on_overlap"), bStopOnOverlap);
		OutData->SetNumberField(TEXT("total_overlap_count"), TotalOverlapCount);
		OutData->SetNumberField(TEXT("first_overlapping_index"), FirstOverlappingIndex);
		OutData->SetNumberField(TEXT("error_count"), ErrorCount);
		OutData->SetNumberField(TEXT("first_error_index"), FirstErrorIndex);
		if (FirstErrorIndex != INDEX_NONE)
		{
			OutData->SetStringField(TEXT("first_error"), FirstError);
		}
		OutData->SetArrayField(TEXT("checks"), Results);
		return true;
	}

	FString ShapeName;
	if (!JsonTryGetString(Ctx.Params, TEXT("shape"), ShapeName) || ShapeName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_shape");
		return false;
	}

	FVector Center = FVector::ZeroVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("center"), Center))
	{
		OutError = TEXT("missing_center");
		return false;
	}

	const FString ShapeNormalized = ShapeName.TrimStartAndEnd().ToLower();
	FCollisionShape Shape;
	FQuat RotationQuat = FQuat::Identity;

	if (ShapeNormalized == TEXT("box"))
	{
		FVector Extent = FVector::ZeroVector;
		if (!JsonTryGetVector(Ctx.Params, TEXT("box_extent"), Extent))
		{
			OutError = TEXT("missing_box_extent");
			return false;
		}
		Extent.X = (float)FMath::Max(0.1, (double)Extent.X);
		Extent.Y = (float)FMath::Max(0.1, (double)Extent.Y);
		Extent.Z = (float)FMath::Max(0.1, (double)Extent.Z);
		Shape = FCollisionShape::MakeBox(Extent);

		FRotator Rotation = FRotator::ZeroRotator;
		if (JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation))
		{
			RotationQuat = Rotation.Quaternion();
		}
	}
	else if (ShapeNormalized == TEXT("sphere"))
	{
		double RadiusCm = 0.0;
		if (!JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), RadiusCm))
		{
			OutError = TEXT("missing_radius_cm");
			return false;
		}
		RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
		Shape = FCollisionShape::MakeSphere((float)RadiusCm);
	}
	else if (ShapeNormalized == TEXT("capsule"))
	{
		double RadiusCm = 0.0;
		double HalfHeightCm = 0.0;
		if (!JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), RadiusCm))
		{
			OutError = TEXT("missing_radius_cm");
			return false;
		}
		if (!JsonTryGetNumber(Ctx.Params, TEXT("half_height_cm"), HalfHeightCm))
		{
			OutError = TEXT("missing_half_height_cm");
			return false;
		}
		RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
		HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);
		Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

		FRotator Rotation = FRotator::ZeroRotator;
		if (JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation))
		{
			RotationQuat = Rotation.Quaternion();
		}
	}
	else
	{
		OutError = TEXT("unsupported_shape");
		return false;
	}

	ECollisionChannel TraceChannel = ECC_Visibility;
	if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
	{
		OutError = TEXT("unsupported_trace_channel");
		return false;
	}

	bool bTraceComplex = false;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	double LimitD = 100.0;
	JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitD);
	const int32 Limit = static_cast<int32>(FMath::Clamp(LimitD, 0.0, 5000.0));

	bool bIncludeOverlaps = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_overlaps"), bIncludeOverlaps);

	bool bIncludeActorFolderTags = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentCheckOverlaps), bTraceComplex);

	TArray<AActor*> IgnoredActors;
	const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
	{
		for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
		{
			const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
			if (IgnoreId.IsEmpty())
			{
				continue;
			}

			if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
			{
				IgnoredActors.Add(IgnoreActor);
			}
		}
	}
	UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, IgnoredActors);
	for (AActor* Ignored : IgnoredActors)
	{
		if (Ignored)
		{
			QueryParams.AddIgnoredActor(Ignored);
		}
	}

	TArray<FOverlapResult> OverlapResults;
	World->OverlapMultiByChannel(OverlapResults, Center, RotationQuat, TraceChannel, Shape, QueryParams);

	const bool bReturnOverlaps = bIncludeOverlaps && Limit > 0;
	TArray<TSharedPtr<FJsonValue>> Overlaps;
	if (bReturnOverlaps)
	{
		Overlaps.Reserve(FMath::Min(Limit, OverlapResults.Num()));
		for (const FOverlapResult& Result : OverlapResults)
		{
			if (Overlaps.Num() >= Limit)
			{
				break;
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			TSharedPtr<FJsonObject> ActorSummary = UeAgentLevelOps::MakeActorSummaryJson(Result.GetActor());
			if (bIncludeActorFolderTags && Result.GetActor())
			{
				UeAgentLevelOps::AugmentActorSummaryWithFolderTags(Result.GetActor(), ActorSummary);
			}
			Item->SetObjectField(TEXT("actor"), ActorSummary);
			if (Result.GetComponent())
			{
				Item->SetStringField(TEXT("component_name"), Result.GetComponent()->GetName());
				Item->SetStringField(TEXT("component_path"), Result.GetComponent()->GetPathName());
			}
			Overlaps.Add(MakeShared<FJsonValueObject>(Item));
		}
	}

	OutData->SetStringField(TEXT("shape"), ShapeNormalized);
	OutData->SetObjectField(TEXT("center"), VecToJson(Center));
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetNumberField(TEXT("limit"), Limit);
	OutData->SetBoolField(TEXT("include_overlaps"), bIncludeOverlaps);
	OutData->SetBoolField(TEXT("overlaps_included"), bReturnOverlaps);
	OutData->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
	OutData->SetNumberField(TEXT("overlap_count"), OverlapResults.Num());
	if (bReturnOverlaps)
	{
		OutData->SetArrayField(TEXT("overlaps"), Overlaps);
	}
	return true;
}
