#include "UeAgentHttpServer_Level.h"

#include "UeAgentHttpServer_LevelOps.h"

#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "UObject/Package.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

bool FUeAgentLegacyMechanics::CmdLevelGetActorTransform(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelGetActorTransform req=%s actor=%s"), *Ctx.RequestId, *Id);
	UeAgentLevelOps::FillActorTransformFields(Actor, OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelSetActorTransform(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FVector NewLocation = Actor->GetActorLocation();
	FRotator NewRotation = Actor->GetActorRotation();
	FVector NewScale = Actor->GetActorScale3D();
	const bool bHasLocation = JsonTryGetVector(Ctx.Params, TEXT("location"), NewLocation);
	const bool bHasRotation = JsonTryGetRotator(Ctx.Params, TEXT("rotation"), NewRotation);
	const bool bHasScale = JsonTryGetVector(Ctx.Params, TEXT("scale"), NewScale);
	if (!bHasLocation && !bHasRotation && !bHasScale)
	{
		OutError = TEXT("missing_transform_fields");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelSetActorTransform req=%s actor=%s has_location=%s has_rotation=%s has_scale=%s"),
		*Ctx.RequestId, *Id, bHasLocation ? TEXT("true") : TEXT("false"), bHasRotation ? TEXT("true") : TEXT("false"), bHasScale ? TEXT("true") : TEXT("false"));

	bool bCollisionAware = false;
	JsonTryGetBool(Ctx.Params, TEXT("collision_aware"), bCollisionAware);

	TSharedPtr<FJsonObject> CollisionParams;
	JsonTryGetObject(Ctx.Params, TEXT("collision"), CollisionParams);

	FVector FinalLocation = NewLocation;
	TSharedPtr<FJsonObject> CollisionResult = MakeShared<FJsonObject>();
	if (bCollisionAware && bHasLocation)
	{
		ECollisionChannel TraceChannel = ECC_Visibility;
		if (CollisionParams.IsValid() && !UeAgentLevelOps::ResolveTraceChannel(CollisionParams, TraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		bool bTraceComplex = false;
		if (CollisionParams.IsValid())
		{
			CollisionParams->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);
		}

		double MaxAllowedOverlapsD = 0.0;
		if (CollisionParams.IsValid())
		{
			JsonTryGetNumber(CollisionParams, TEXT("max_allowed_overlaps"), MaxAllowedOverlapsD);
		}
		const int32 MaxAllowedOverlaps = (int32)FMath::Clamp(MaxAllowedOverlapsD, 0.0, 100000.0);

		bool bIncludeOverlaps = false;
		if (CollisionParams.IsValid())
		{
			CollisionParams->TryGetBoolField(TEXT("include_overlaps"), bIncludeOverlaps);
		}

		int32 OverlapLimit = 10;
		if (CollisionParams.IsValid())
		{
			double OverlapLimitD = (double)OverlapLimit;
			JsonTryGetNumber(CollisionParams, TEXT("overlap_limit"), OverlapLimitD);
			OverlapLimit = (int32)FMath::Clamp(OverlapLimitD, 0.0, 5000.0);
		}

		bool bAutoFallback = true;
		if (CollisionParams.IsValid())
		{
			CollisionParams->TryGetBoolField(TEXT("auto_fallback"), bAutoFallback);
		}

		double FallbackStepCmD = 10.0;
		if (CollisionParams.IsValid())
		{
			JsonTryGetNumber(CollisionParams, TEXT("fallback_step_cm"), FallbackStepCmD);
		}
		const double FallbackStepCm = FMath::Clamp(FallbackStepCmD, 0.1, 100000.0);

		TArray<FVector> FallbackOffsets;
		if (CollisionParams.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* OffsetsJson = nullptr;
			if (CollisionParams->TryGetArrayField(TEXT("fallback_offsets_cm"), OffsetsJson) && OffsetsJson)
			{
				auto TryGetVectorFromValue = [](const TSharedPtr<FJsonValue>& Value, FVector& OutVec) -> bool
				{
					if (UeAgentJsonDiagnostics::TryReadVectorValue(Value, OutVec))
					{
						return true;
					}
					if (!Value.IsValid())
					{
						return false;
					}
					const TSharedPtr<FJsonObject> Obj = Value->AsObject();
					if (!Obj.IsValid())
					{
						return false;
					}

					const TSharedPtr<FJsonValue>* OffsetValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, TEXT("offset_cm"));
					if (OffsetValue)
					{
						return UeAgentJsonDiagnostics::TryReadVectorValue(*OffsetValue, OutVec);
					}

					return false;
				};

				for (const TSharedPtr<FJsonValue>& Value : *OffsetsJson)
				{
					FVector Offset = FVector::ZeroVector;
					if (TryGetVectorFromValue(Value, Offset))
					{
						FallbackOffsets.Add(Offset);
					}
				}
			}
		}

		if (FallbackOffsets.Num() == 0)
		{
			FallbackOffsets.Add(FVector::ZeroVector);
			if (bAutoFallback)
			{
				const float Step = (float)FallbackStepCm;
				FallbackOffsets.Add(FVector(Step, 0.0f, 0.0f));
				FallbackOffsets.Add(FVector(-Step, 0.0f, 0.0f));
				FallbackOffsets.Add(FVector(0.0f, Step, 0.0f));
				FallbackOffsets.Add(FVector(0.0f, -Step, 0.0f));
				FallbackOffsets.Add(FVector(0.0f, 0.0f, Step));
				FallbackOffsets.Add(FVector(0.0f, 0.0f, -Step));
			}
		}

		FCollisionShape Shape;
		FQuat RotationQuat = FQuat::Identity;
		bool bShapeDerivedFromBounds = false;

		FString ShapeName;
		if (CollisionParams.IsValid())
		{
			JsonTryGetString(CollisionParams, TEXT("shape"), ShapeName);
		}
		const FString ShapeNormalized = ShapeName.TrimStartAndEnd().ToLower();
		if (ShapeNormalized == TEXT("box"))
		{
			FVector Extent = FVector::ZeroVector;
			if (!CollisionParams.IsValid() || !JsonTryGetVector(CollisionParams, TEXT("box_extent"), Extent))
			{
				OutError = TEXT("collision_missing_box_extent");
				CollisionResult->SetStringField(TEXT("error"), OutError);
				OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
				return false;
			}
			Extent.X = (float)FMath::Max(0.1, (double)Extent.X);
			Extent.Y = (float)FMath::Max(0.1, (double)Extent.Y);
			Extent.Z = (float)FMath::Max(0.1, (double)Extent.Z);
			Shape = FCollisionShape::MakeBox(Extent);

			FRotator CollisionRotation = FRotator::ZeroRotator;
			if (JsonTryGetRotator(CollisionParams, TEXT("rotation"), CollisionRotation))
			{
				RotationQuat = CollisionRotation.Quaternion();
			}
		}
		else if (ShapeNormalized == TEXT("sphere"))
		{
			double RadiusCm = 0.0;
			if (!CollisionParams.IsValid() || !JsonTryGetNumber(CollisionParams, TEXT("radius_cm"), RadiusCm))
			{
				OutError = TEXT("collision_missing_radius_cm");
				CollisionResult->SetStringField(TEXT("error"), OutError);
				OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
				return false;
			}
			RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
			Shape = FCollisionShape::MakeSphere((float)RadiusCm);
		}
		else if (ShapeNormalized == TEXT("capsule"))
		{
			double RadiusCm = 0.0;
			double HalfHeightCm = 0.0;
			if (!CollisionParams.IsValid() || !JsonTryGetNumber(CollisionParams, TEXT("radius_cm"), RadiusCm))
			{
				OutError = TEXT("collision_missing_radius_cm");
				CollisionResult->SetStringField(TEXT("error"), OutError);
				OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
				return false;
			}
			if (!JsonTryGetNumber(CollisionParams, TEXT("half_height_cm"), HalfHeightCm))
			{
				OutError = TEXT("collision_missing_half_height_cm");
				CollisionResult->SetStringField(TEXT("error"), OutError);
				OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
				return false;
			}
			RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
			HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);
			Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

			FRotator CollisionRotation = FRotator::ZeroRotator;
			if (JsonTryGetRotator(CollisionParams, TEXT("rotation"), CollisionRotation))
			{
				RotationQuat = CollisionRotation.Quaternion();
			}
		}
		else if (!ShapeNormalized.IsEmpty())
		{
			OutError = TEXT("collision_unsupported_shape");
			CollisionResult->SetStringField(TEXT("error"), OutError);
			CollisionResult->SetStringField(TEXT("shape"), ShapeNormalized);
			OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
			return false;
		}
		else
		{
			// Derive from actor bounds (world AABB).
			FBox Bounds(EForceInit::ForceInit);
			FString BoundsError;
			if (!UeAgentLevelOps::GetActorBoundsBox(Actor, Bounds, BoundsError))
			{
				OutError = TEXT("collision_actor_bounds_invalid");
				CollisionResult->SetStringField(TEXT("error"), OutError);
				CollisionResult->SetStringField(TEXT("bounds_error"), BoundsError);
				OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
				return false;
			}

			double PaddingCm = 0.0;
			if (CollisionParams.IsValid())
			{
				JsonTryGetNumber(CollisionParams, TEXT("bounds_padding_cm"), PaddingCm);
				if (PaddingCm == 0.0)
				{
					JsonTryGetNumber(CollisionParams, TEXT("padding_cm"), PaddingCm);
				}
			}
			PaddingCm = FMath::Clamp(PaddingCm, -100000.0, 100000.0);

			FVector Extent = Bounds.GetExtent() + FVector((float)PaddingCm, (float)PaddingCm, (float)PaddingCm);
			Extent.X = (float)FMath::Max(0.1, (double)Extent.X);
			Extent.Y = (float)FMath::Max(0.1, (double)Extent.Y);
			Extent.Z = (float)FMath::Max(0.1, (double)Extent.Z);
			Shape = FCollisionShape::MakeBox(Extent);
			RotationQuat = FQuat::Identity;
			bShapeDerivedFromBounds = true;
		}

		TArray<AActor*> IgnoredActors;
		IgnoredActors.Add(Actor);
		if (CollisionParams.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
			if (CollisionParams->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
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
		}
		UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, CollisionParams, IgnoredActors);

		TArray<TSharedPtr<FJsonValue>> AttemptsJson;
		AttemptsJson.Reserve(FallbackOffsets.Num());

		bool bFound = false;
		int32 ChosenIndex = INDEX_NONE;
		FVector ChosenOffset = FVector::ZeroVector;
		int32 ChosenOverlapCount = 0;
		TArray<TSharedPtr<FJsonValue>> ChosenOverlapsJson;

		for (int32 AttemptIndex = 0; AttemptIndex < FallbackOffsets.Num(); ++AttemptIndex)
		{
			const FVector Offset = FallbackOffsets[AttemptIndex];
			const FVector Candidate = NewLocation + Offset;

			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentSetActorTransformCollision), bTraceComplex);
			for (AActor* Ignored : IgnoredActors)
			{
				if (Ignored)
				{
					QueryParams.AddIgnoredActor(Ignored);
				}
			}

			TArray<FOverlapResult> OverlapResults;
			World->OverlapMultiByChannel(OverlapResults, Candidate, RotationQuat, TraceChannel, Shape, QueryParams);

			TSharedPtr<FJsonObject> Attempt = MakeShared<FJsonObject>();
			Attempt->SetNumberField(TEXT("index"), AttemptIndex);
			Attempt->SetObjectField(TEXT("offset_cm"), VecToJson(Offset));
			Attempt->SetObjectField(TEXT("candidate_location"), VecToJson(Candidate));
			Attempt->SetNumberField(TEXT("overlap_count"), OverlapResults.Num());

			TArray<TSharedPtr<FJsonValue>> OverlapsJson;
			if (bIncludeOverlaps && OverlapLimit > 0)
			{
				OverlapsJson.Reserve(FMath::Min(OverlapLimit, OverlapResults.Num()));
				for (const FOverlapResult& Result : OverlapResults)
				{
					if (OverlapsJson.Num() >= OverlapLimit)
					{
						break;
					}
					TSharedPtr<FJsonObject> OverlapItem = MakeShared<FJsonObject>();
					OverlapItem->SetObjectField(TEXT("actor"), UeAgentLevelOps::MakeActorSummaryJson(Result.GetActor()));
					if (Result.GetComponent())
					{
						OverlapItem->SetStringField(TEXT("component_name"), Result.GetComponent()->GetName());
						OverlapItem->SetStringField(TEXT("component_path"), Result.GetComponent()->GetPathName());
					}
					OverlapsJson.Add(MakeShared<FJsonValueObject>(OverlapItem));
				}
			}
			if (bIncludeOverlaps)
			{
				Attempt->SetArrayField(TEXT("overlaps"), OverlapsJson);
			}

			AttemptsJson.Add(MakeShared<FJsonValueObject>(Attempt));

			if ((int32)OverlapResults.Num() <= MaxAllowedOverlaps)
			{
				bFound = true;
				ChosenIndex = AttemptIndex;
				ChosenOffset = Offset;
				ChosenOverlapCount = OverlapResults.Num();
				ChosenOverlapsJson = OverlapsJson;
				FinalLocation = Candidate;
				break;
			}
		}

		CollisionResult->SetBoolField(TEXT("collision_aware"), true);
		CollisionResult->SetObjectField(TEXT("requested_location"), VecToJson(NewLocation));
		CollisionResult->SetObjectField(TEXT("final_location"), VecToJson(FinalLocation));
		CollisionResult->SetStringField(TEXT("shape"), ShapeNormalized.IsEmpty() ? TEXT("actor_bounds_box") : ShapeNormalized);
		CollisionResult->SetBoolField(TEXT("shape_derived_from_bounds"), bShapeDerivedFromBounds);
		CollisionResult->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
		CollisionResult->SetBoolField(TEXT("trace_complex"), bTraceComplex);
		CollisionResult->SetNumberField(TEXT("max_allowed_overlaps"), MaxAllowedOverlaps);
		CollisionResult->SetBoolField(TEXT("auto_fallback"), bAutoFallback);
		CollisionResult->SetNumberField(TEXT("fallback_step_cm"), FallbackStepCm);
		CollisionResult->SetNumberField(TEXT("attempt_count"), AttemptsJson.Num());
		CollisionResult->SetArrayField(TEXT("attempts"), AttemptsJson);
		CollisionResult->SetBoolField(TEXT("fallback_used"), ChosenIndex > 0);
		CollisionResult->SetNumberField(TEXT("fallback_index"), ChosenIndex);
		CollisionResult->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(ChosenOffset));
		CollisionResult->SetNumberField(TEXT("overlap_count"), ChosenOverlapCount);
		if (bIncludeOverlaps)
		{
			CollisionResult->SetArrayField(TEXT("overlaps"), ChosenOverlapsJson);
			CollisionResult->SetNumberField(TEXT("overlap_limit"), OverlapLimit);
			CollisionResult->SetBoolField(TEXT("include_overlaps"), true);
		}
		else
		{
			CollisionResult->SetBoolField(TEXT("include_overlaps"), false);
		}

		if (!bFound)
		{
			OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
			OutError = TEXT("collision_aware_no_valid_location");
			return false;
		}
	}

	Actor->Modify();
	if (bHasRotation)
	{
		Actor->SetActorRotation(NewRotation, ETeleportType::TeleportPhysics);
	}
	if (bHasScale)
	{
		Actor->SetActorScale3D(NewScale);
	}
	if (bHasLocation)
	{
		Actor->SetActorLocation(bCollisionAware ? FinalLocation : NewLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}

	UeAgentLevelOps::FinishActorTransformEdit(Actor);
	UeAgentLevelOps::FillActorTransformFields(Actor, OutData);
	if (bCollisionAware && bHasLocation)
	{
		OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdProbeActorTransformCandidates(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	UeAgentEditorViewportSequenceFrameOptions::FUnifiedSequenceFrameOptionsReport UnifiedOptionsReport;
	UeAgentEditorViewportSequenceFrameOptions::ApplyUnifiedSequenceFrameOptionsToParams(
		Ctx.Params,
		UeAgentEditorViewportSequenceFrameOptions::ESequenceFrameCommandKind::ActorProbe,
		UnifiedOptionsReport);

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* CandidateValues = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("candidates"), CandidateValues) || !CandidateValues || CandidateValues->Num() <= 0)
	{
		OutError = TEXT("missing_candidates");
		return false;
	}
	if (CandidateValues->Num() > 15)
	{
		OutError = TEXT("too_many_candidates");
		return false;
	}

	FString Format = TEXT("png");
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	Format = Format.ToLower();
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_format");
		return false;
	}

	int32 Quality = 90;
	double QualityValue = static_cast<double>(Quality);
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QualityValue))
	{
		Quality = FMath::Clamp(FMath::RoundToInt(QualityValue), 1, 100);
	}

	int32 MaxSize = 720;
	double MaxSizeValue = static_cast<double>(MaxSize);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeValue))
	{
		MaxSize = FMath::Clamp(FMath::RoundToInt(MaxSizeValue), 64, 8192);
	}

	int32 WarmupFrames = 2;
	double WarmupFramesValue = static_cast<double>(WarmupFrames);
	if (JsonTryGetNumber(Ctx.Params, TEXT("warmup_frames"), WarmupFramesValue)
		|| JsonTryGetNumber(Ctx.Params, TEXT("render_warmup_frames"), WarmupFramesValue))
	{
		WarmupFrames = FMath::Clamp(FMath::RoundToInt(WarmupFramesValue), 0, 8);
	}

	bool bTickEditorWorld = false;
	JsonTryGetBool(Ctx.Params, TEXT("tick_editor_world"), bTickEditorWorld);
	JsonTryGetBool(Ctx.Params, TEXT("tick_editor_world_during_warmup"), bTickEditorWorld);

	int32 EditorWorldTickFrames = 0;
	double EditorWorldTickFramesValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("editor_world_tick_frames"), EditorWorldTickFramesValue))
	{
		EditorWorldTickFrames = bTickEditorWorld ? FMath::Clamp(FMath::RoundToInt(EditorWorldTickFramesValue), 0, 240) : 0;
	}

	double EditorWorldTickDeltaValue = 1.0 / 60.0;
	JsonTryGetNumber(Ctx.Params, TEXT("editor_world_tick_delta_seconds"), EditorWorldTickDeltaValue);
	const float EditorWorldTickDeltaSeconds = FMath::Clamp(static_cast<float>(EditorWorldTickDeltaValue), 1.0f / 240.0f, 1.0f / 5.0f);

	bool bRelativeToOriginalDefault = false;
	JsonTryGetBool(Ctx.Params, TEXT("relative_to_original"), bRelativeToOriginalDefault);
	bool bWriteIndividualFrames = true;
	JsonTryGetBool(Ctx.Params, TEXT("write_individual_frames"), bWriteIndividualFrames);
	bool bCreateFrameSheet = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_frame_sheet"), bCreateFrameSheet);
	JsonTryGetBool(Ctx.Params, TEXT("create_sheet"), bCreateFrameSheet);

	const FTransform OriginalTransform = Actor->GetActorTransform();
	UPackage* const ActorPackage = Actor->GetPackage();
	UPackage* const WorldPackage = World->GetPackage();
	const bool bActorPackageDirtyBefore =
		ActorPackage && ActorPackage->IsDirty();
	const bool bWorldPackageDirtyBefore =
		WorldPackage && WorldPackage->IsDirty();
	const bool bActorSelectedBefore = Actor->IsSelected();
	struct FScopedActorTransformRestore
	{
		AActor* Actor = nullptr;
		FTransform OriginalTransform;
		UPackage* ActorPackage = nullptr;
		UPackage* WorldPackage = nullptr;
		bool bActorPackageDirtyBefore = false;
		bool bWorldPackageDirtyBefore = false;
		bool bActorSelectedBefore = false;
		bool bRestored = false;
		bool bTransformRestored = false;
		bool bDirtyStateRestored = false;
		bool bSelectionRestored = false;

		~FScopedActorTransformRestore()
		{
			RestoreNow();
		}

		void RestoreNow()
		{
			if (!Actor || bRestored)
			{
				return;
			}
			Actor->SetActorTransform(OriginalTransform, false, nullptr, ETeleportType::TeleportPhysics);
			Actor->PostEditMove(false);
			if (GEditor && Actor->IsSelected() != bActorSelectedBefore)
			{
				GEditor->SelectActor(
					Actor, bActorSelectedBefore, false, true);
			}
			if (ActorPackage)
			{
				ActorPackage->SetDirtyFlag(bActorPackageDirtyBefore);
			}
			if (WorldPackage && WorldPackage != ActorPackage)
			{
				WorldPackage->SetDirtyFlag(bWorldPackageDirtyBefore);
			}
			bTransformRestored =
				Actor->GetActorTransform().Equals(OriginalTransform);
			bDirtyStateRestored =
				(!ActorPackage
					|| ActorPackage->IsDirty() == bActorPackageDirtyBefore)
				&& (!WorldPackage
					|| WorldPackage->IsDirty() == bWorldPackageDirtyBefore);
			bSelectionRestored =
				Actor->IsSelected() == bActorSelectedBefore;
			bRestored = true;
		}
	};
	FScopedActorTransformRestore RestoreScope{
		Actor,
		OriginalTransform,
		ActorPackage,
		WorldPackage,
		bActorPackageDirtyBefore,
		bWorldPackageDirtyBefore,
		bActorSelectedBefore};

	TArray<TArray<FColor>> FramePixels;
	TArray<FString> FrameLabels;
	FIntPoint FrameSize(0, 0);
	TArray<TSharedPtr<FJsonValue>> FrameResults;
	TArray<TSharedPtr<FJsonValue>> CandidateResults;
	bool bViewportStateRestored = true;

	auto AddArtifact = [&OutArtifacts, &Format](const FString& Kind, const FString& FilePath, const FIntPoint& Size)
	{
		const int64 Bytes = IFileManager::Get().FileSize(*FilePath);
		TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
		Artifact->SetStringField(TEXT("kind"), Kind);
		Artifact->SetStringField(TEXT("file_path"), FilePath);
		Artifact->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
		Artifact->SetStringField(TEXT("mime"), Format == TEXT("png") ? TEXT("image/png") : (Format == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg")));
		Artifact->SetNumberField(TEXT("width"), Size.X);
		Artifact->SetNumberField(TEXT("height"), Size.Y);
		OutArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));
	};

	auto CaptureCurrentFrame = [&](const FString& Kind, const int32 CandidateIndex, const FString& CandidateLabel, const FTransform& AppliedTransform) -> bool
	{
		TArray<FColor> RawPixels;
		FIntPoint RawSize(0, 0);
		TSharedPtr<FJsonObject> CaptureInfo;
		if (!TakeActiveViewportScreenshotWithOptions(WarmupFrames, bTickEditorWorld, EditorWorldTickFrames, EditorWorldTickDeltaSeconds, RawPixels, RawSize, CaptureInfo, OutError))
		{
			return false;
		}

		TArray<FColor> ResizedPixels;
		FIntPoint ResizedSize(0, 0);
		if (!ResizePixelsMaxSize(RawPixels, RawSize, MaxSize, ResizedPixels, ResizedSize, OutError))
		{
			return false;
		}
		if (FrameSize.X == 0 || FrameSize.Y == 0)
		{
			FrameSize = ResizedSize;
		}
		else if (FrameSize != ResizedSize)
		{
			OutError = TEXT("probe_frame_size_mismatch");
			return false;
		}

		const int32 FrameIndex = FramePixels.Num();
		const FString FrameLabel = FString::Printf(TEXT("F%d"), FrameIndex);
		FramePixels.Add(MoveTemp(ResizedPixels));
		FrameLabels.Add(FrameLabel);

		TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
		FrameObj->SetNumberField(TEXT("frame_index"), FrameIndex);
		FrameObj->SetStringField(TEXT("frame_label"), FrameLabel);
		FrameObj->SetStringField(TEXT("kind"), Kind);
		FrameObj->SetNumberField(TEXT("candidate_index"), CandidateIndex);
		FrameObj->SetStringField(TEXT("candidate_label"), CandidateLabel);
		FrameObj->SetObjectField(TEXT("applied_transform"), UeAgentLevelOps::MakeActorTransformJson(AppliedTransform));
		FrameObj->SetNumberField(TEXT("width"), ResizedSize.X);
		FrameObj->SetNumberField(TEXT("height"), ResizedSize.Y);
		if (CaptureInfo.IsValid())
		{
			FrameObj->SetObjectField(TEXT("capture"), CaptureInfo);
			bool bFrameViewportRestored = false;
			bViewportStateRestored = bViewportStateRestored
				&& CaptureInfo->TryGetBoolField(
					TEXT("realtime_state_restored"), bFrameViewportRestored)
				&& bFrameViewportRestored;
		}
		else
		{
			bViewportStateRestored = false;
		}

		if (bWriteIndividualFrames)
		{
			const FString FramePath = UeAgentLevelOps::MakeProbeArtifactPath(Format, Ctx.RequestId, FrameLabel);
			if (!WriteCompressedImage(FramePixels.Last(), ResizedSize, Format, Quality, FramePath, OutError))
			{
				return false;
			}
			const int64 Bytes = IFileManager::Get().FileSize(*FramePath);
			FrameObj->SetStringField(TEXT("file_path"), FramePath);
			FrameObj->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
			AddArtifact(TEXT("probe_frame"), FramePath, ResizedSize);
		}

		FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));
		return true;
	};

	if (!CaptureCurrentFrame(TEXT("original"), INDEX_NONE, TEXT("original"), OriginalTransform))
	{
		return false;
	}

	for (int32 CandidateIndex = 0; CandidateIndex < CandidateValues->Num(); ++CandidateIndex)
	{
		const TSharedPtr<FJsonValue>& CandidateValue = (*CandidateValues)[CandidateIndex];
		const TSharedPtr<FJsonObject>* CandidateObjPtr = nullptr;
		if (!CandidateValue.IsValid() || !CandidateValue->TryGetObject(CandidateObjPtr) || !CandidateObjPtr || !CandidateObjPtr->IsValid())
		{
			OutError = TEXT("invalid_candidate");
			return false;
		}
		TSharedPtr<FJsonObject> CandidateObj = *CandidateObjPtr;
		TSharedPtr<FJsonObject> TransformObj = CandidateObj;
		const TSharedPtr<FJsonObject>* NestedTransformObj = nullptr;
		if (CandidateObj->TryGetObjectField(TEXT("transform"), NestedTransformObj) && NestedTransformObj && NestedTransformObj->IsValid())
		{
			TransformObj = *NestedTransformObj;
		}

		FString CandidateLabel;
		CandidateObj->TryGetStringField(TEXT("label"), CandidateLabel);
		if (CandidateLabel.IsEmpty())
		{
			CandidateLabel = FString::Printf(TEXT("candidate_%d"), CandidateIndex);
		}

		bool bRelativeToOriginal = bRelativeToOriginalDefault;
		CandidateObj->TryGetBoolField(TEXT("relative"), bRelativeToOriginal);
		CandidateObj->TryGetBoolField(TEXT("relative_to_original"), bRelativeToOriginal);

		FVector CandidateLocation = OriginalTransform.GetLocation();
		FRotator CandidateRotation = OriginalTransform.Rotator();
		FVector CandidateScale = OriginalTransform.GetScale3D();
		bool bHasAnyTransformField = false;

		if (bRelativeToOriginal)
		{
			FVector LocationOffset = FVector::ZeroVector;
			if (JsonTryGetVector(TransformObj, TEXT("location"), LocationOffset)
				|| JsonTryGetVector(TransformObj, TEXT("location_offset"), LocationOffset)
				|| JsonTryGetVector(TransformObj, TEXT("location_delta"), LocationOffset)
				|| JsonTryGetVector(TransformObj, TEXT("delta_location"), LocationOffset))
			{
				CandidateLocation = OriginalTransform.GetLocation() + LocationOffset;
				bHasAnyTransformField = true;
			}

			FRotator RotationOffset = FRotator::ZeroRotator;
			if (JsonTryGetRotator(TransformObj, TEXT("rotation"), RotationOffset)
				|| JsonTryGetRotator(TransformObj, TEXT("rotation_offset"), RotationOffset)
				|| JsonTryGetRotator(TransformObj, TEXT("rotation_delta"), RotationOffset)
				|| JsonTryGetRotator(TransformObj, TEXT("delta_rotation"), RotationOffset))
			{
				CandidateRotation = OriginalTransform.Rotator() + RotationOffset;
				bHasAnyTransformField = true;
			}

			FVector ScaleMultiplier = FVector::OneVector;
			if (JsonTryGetVector(TransformObj, TEXT("scale"), ScaleMultiplier)
				|| JsonTryGetVector(TransformObj, TEXT("scale_multiplier"), ScaleMultiplier)
				|| JsonTryGetVector(TransformObj, TEXT("scale_factor"), ScaleMultiplier))
			{
				CandidateScale = OriginalTransform.GetScale3D() * ScaleMultiplier;
				bHasAnyTransformField = true;
			}
		}
		else
		{
			if (JsonTryGetVector(TransformObj, TEXT("location"), CandidateLocation))
			{
				bHasAnyTransformField = true;
			}
			if (JsonTryGetRotator(TransformObj, TEXT("rotation"), CandidateRotation))
			{
				bHasAnyTransformField = true;
			}
			if (JsonTryGetVector(TransformObj, TEXT("scale"), CandidateScale))
			{
				bHasAnyTransformField = true;
			}
		}

		if (!bHasAnyTransformField)
		{
			OutError = TEXT("candidate_missing_transform_fields");
			return false;
		}

		const FTransform CandidateTransform(CandidateRotation, CandidateLocation, CandidateScale);
		Actor->SetActorTransform(CandidateTransform, false, nullptr, ETeleportType::TeleportPhysics);
		Actor->PostEditMove(false);

		if (!CaptureCurrentFrame(TEXT("candidate"), CandidateIndex, CandidateLabel, Actor->GetActorTransform()))
		{
			return false;
		}

		TSharedPtr<FJsonObject> CandidateResult = MakeShared<FJsonObject>();
		CandidateResult->SetNumberField(TEXT("candidate_index"), CandidateIndex);
		CandidateResult->SetStringField(TEXT("label"), CandidateLabel);
		CandidateResult->SetStringField(TEXT("frame_label"), FrameLabels.Last());
		CandidateResult->SetNumberField(TEXT("frame_index"), FrameLabels.Num() - 1);
		CandidateResult->SetBoolField(TEXT("relative_to_original"), bRelativeToOriginal);
		CandidateResult->SetObjectField(TEXT("applied_transform"), UeAgentLevelOps::MakeActorTransformJson(Actor->GetActorTransform()));
		CandidateResults.Add(MakeShared<FJsonValueObject>(CandidateResult));
	}

	RestoreScope.RestoreNow();

	OutData->SetStringField(TEXT("schema"), TEXT("uai.probe.actor_transform_candidates.v1"));
	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetNumberField(TEXT("candidate_count"), CandidateValues->Num());
	OutData->SetNumberField(TEXT("frame_count"), FramePixels.Num());
	OutData->SetObjectField(TEXT("original_transform"), UeAgentLevelOps::MakeActorTransformJson(OriginalTransform));
	OutData->SetObjectField(TEXT("final_transform"), UeAgentLevelOps::MakeActorTransformJson(Actor->GetActorTransform()));
	OutData->SetBoolField(
		TEXT("restored"), RestoreScope.bTransformRestored);
	OutData->SetBoolField(
		TEXT("dirty_state_restored"), RestoreScope.bDirtyStateRestored);
	OutData->SetBoolField(
		TEXT("selection_restored"), RestoreScope.bSelectionRestored);
	OutData->SetBoolField(
		TEXT("viewport_state_restored"), bViewportStateRestored);
	OutData->SetBoolField(TEXT("write_individual_frames"), bWriteIndividualFrames);
	OutData->SetBoolField(TEXT("create_frame_sheet"), bCreateFrameSheet);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("quality"), Quality);
	OutData->SetNumberField(TEXT("max_size"), MaxSize);
	OutData->SetArrayField(TEXT("frames"), FrameResults);
	OutData->SetArrayField(TEXT("candidates"), CandidateResults);

	if (bCreateFrameSheet)
	{
		FString SheetPath;
		TSharedPtr<FJsonObject> SheetData;
		if (!WriteFrameSequenceSheetImage(FramePixels, FrameLabels, FrameSize, Ctx.Params, Format, Quality, SheetPath, SheetData, OutError))
		{
			return false;
		}
		OutData->SetObjectField(TEXT("frame_sheet"), SheetData);
		OutData->SetStringField(TEXT("frame_sheet_file_path"), SheetPath);
		AddArtifact(TEXT("probe_frame_sheet"), SheetPath, FIntPoint(
			SheetData.IsValid() ? FMath::RoundToInt(SheetData->GetNumberField(TEXT("width"))) : 0,
			SheetData.IsValid() ? FMath::RoundToInt(SheetData->GetNumberField(TEXT("height"))) : 0));
	}

	UeAgentEditorViewportSequenceFrameOptions::AttachUnifiedSequenceFrameOptionsReport(OutData, UnifiedOptionsReport);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelSetActorLocation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FVector NewLocation = Actor->GetActorLocation();
	if (!JsonTryGetVector(Ctx.Params, TEXT("location"), NewLocation))
	{
		OutError = TEXT("missing_location");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelSetActorLocation req=%s actor=%s"), *Ctx.RequestId, *Id);
	Actor->Modify();
	Actor->SetActorLocation(NewLocation, false, nullptr, ETeleportType::TeleportPhysics);
	UeAgentLevelOps::FinishActorTransformEdit(Actor);
	UeAgentLevelOps::FillActorTransformFields(Actor, OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelSetActorRotation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FRotator NewRotation = Actor->GetActorRotation();
	if (!JsonTryGetRotator(Ctx.Params, TEXT("rotation"), NewRotation))
	{
		OutError = TEXT("missing_rotation");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelSetActorRotation req=%s actor=%s"), *Ctx.RequestId, *Id);
	Actor->Modify();
	Actor->SetActorRotation(NewRotation, ETeleportType::TeleportPhysics);
	UeAgentLevelOps::FinishActorTransformEdit(Actor);
	UeAgentLevelOps::FillActorTransformFields(Actor, OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelSetActorScale(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FVector NewScale = Actor->GetActorScale3D();
	if (!JsonTryGetVector(Ctx.Params, TEXT("scale"), NewScale))
	{
		OutError = TEXT("missing_scale");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelSetActorScale req=%s actor=%s"), *Ctx.RequestId, *Id);
	Actor->Modify();
	Actor->SetActorScale3D(NewScale);
	UeAgentLevelOps::FinishActorTransformEdit(Actor);
	UeAgentLevelOps::FillActorTransformFields(Actor, OutData);
	return true;
}
