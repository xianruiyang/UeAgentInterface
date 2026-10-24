#include "DomainAdapters/CaptureMediaLongJob/UeAgentWorldViewportCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentWorldViewportCaptureMechanics.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelEditorViewport.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	UWorld* FWorldViewportCapture::GetEditorWorld(UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!GEditor)
		{
		OutError = CaptureDiagnostics::Native(TEXT("no_geditor"), TEXT("The Unreal Editor instance is unavailable."), TEXT("viewport_capture"), TEXT("editor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return nullptr;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
		OutError = CaptureDiagnostics::Native(TEXT("no_editor_world"), TEXT("The editor has no active world for capture."), TEXT("viewport_capture"), TEXT("editor_world_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}
		return World;
	}

	AActor* FWorldViewportCapture::FindActorByNameOrLabel(
		UWorld* World,
		const FString& NameOrLabel,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FString Identity = NameOrLabel;
		Identity.RemoveFromStart(TEXT("actor:"));
		TArray<UObject*> Actors;
		for (TActorIterator<AActor> It(World); It; ++It)
			Actors.Add(*It);
		UObject* Resolved = nullptr;
		if (!ContentDiscovery::TryResolveScopedObjectReference(Actors, Identity, Resolved, OutError))
		{
			OutError.Source.JsonPath = TEXT("/id");
			OutError.Source.bCommandInput = true;
			return nullptr;
		}
		return Cast<AActor>(Resolved);
	}

	bool FWorldViewportCapture::JsonTryGetString(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Key,
		FString& OutValue)
	{
		return Object.IsValid() && Object->TryGetStringField(Key, OutValue);
	}

	bool FWorldViewportCapture::JsonTryGetNumber(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Key,
		double& OutValue)
	{
		return UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(
			Object, {Key}, OutValue);
	}

	bool FWorldViewportCapture::JsonTryGetBool(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Key,
		bool& OutValue)
	{
		return Object.IsValid() && Object->TryGetBoolField(Key, OutValue);
	}

	TSharedPtr<FJsonObject> FWorldViewportCapture::VecToJson(
		const FVector& Value)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("x"), Value.X);
		Result->SetNumberField(TEXT("y"), Value.Y);
		Result->SetNumberField(TEXT("z"), Value.Z);
		return Result;
	}

	TSharedPtr<FJsonObject> FWorldViewportCapture::RotToJson(
		const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("pitch"), Value.Pitch);
		Result->SetNumberField(TEXT("yaw"), Value.Yaw);
		Result->SetNumberField(TEXT("roll"), Value.Roll);
		return Result;
	}

bool FWorldViewportCapture::FrameActor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	return FrameActorCore(Ctx, OutData, OutError);
}

bool FWorldViewportCapture::FrameActorCore(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = CaptureDiagnostics::Input(TEXT("missing_id"), TEXT("Actor framing requires a non-empty actor id."), TEXT("input_decode"), TEXT("/id"), Ctx.Params.IsValid() ? Ctx.Params->TryGetField(TEXT("id")) : nullptr, MakeShared<FJsonValueString>(TEXT("non-empty actor name or label")));
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id, OutError);
	if (!Actor)
	{
		return false;
	}

	FString SelectionReason;
	FLevelEditorViewportClient* LVC = UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::GetPreferredLevelViewportClient(&SelectionReason);
	if (!LVC)
	{
		OutError = CaptureDiagnostics::Native(TEXT("no_level_editor_viewport"), TEXT("No usable level editor viewport is available for actor framing."), TEXT("viewport_frame"), TEXT("viewport_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		OutError.TargetPath = Actor->GetPathName();
		OutError.InternalContext = SelectionReason;
		return false;
	}

	FBox Bounds(EForceInit::ForceInit);
	if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::GetActorBoundsBox(Actor, Bounds, OutError))
	{
		return false;
	}

	double Padding = 1.1;
	JsonTryGetNumber(Ctx.Params, TEXT("padding"), Padding);

	const FVector PreviousLocation = LVC->GetViewLocation();
	const FRotator PreviousRotation = LVC->GetViewRotation();
	FVector DesiredLocation = PreviousLocation;
	FVector NewLocation = PreviousLocation;
	if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::PrepareLevelViewportForCapture(LVC, OutError))
	{
		if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Actor->GetPathName();
		return false;
	}
	if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::ComputeViewportLocationToFrameBounds(LVC, Bounds, PreviousRotation, Padding, NewLocation, OutError))
	{
		if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Actor->GetPathName();
		return false;
	}

	DesiredLocation = NewLocation;

	bool bCollisionAware = false;
	JsonTryGetBool(Ctx.Params, TEXT("collision_aware"), bCollisionAware);

	bool bLookAt = false;
	JsonTryGetBool(Ctx.Params, TEXT("look_at"), bLookAt);

	bool bCollisionAdjusted = false;
	bool bFallbackUsed = false;
	int32 FallbackIndex = 0;
	FVector FallbackOffset = FVector::ZeroVector;
	TSharedPtr<FJsonObject> CollisionTrace;
	if (bCollisionAware)
	{
		double SafetyOffsetCm = 15.0;
		JsonTryGetNumber(Ctx.Params, TEXT("safety_offset_cm"), SafetyOffsetCm);
		SafetyOffsetCm = FMath::Max(SafetyOffsetCm, 0.0);

		ECollisionChannel TraceChannel = ECC_Visibility;
		if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::ResolveTraceChannel(Ctx.Params, TraceChannel, OutError))
		{
			return false;
		}

		bool bTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentViewportFrameActorCollision), bTraceComplex);
		QueryParams.AddIgnoredActor(Actor);

		bool bAutoFallback = false;
		JsonTryGetBool(Ctx.Params, TEXT("auto_fallback"), bAutoFallback);

		double FallbackStepCm = 200.0;
		JsonTryGetNumber(Ctx.Params, TEXT("fallback_step_cm"), FallbackStepCm);
		FallbackStepCm = FMath::Max(FallbackStepCm, 0.0);

		TArray<FVector> CandidateOffsets;
		CandidateOffsets.Reserve(8);
		CandidateOffsets.Add(FVector::ZeroVector);

		const TArray<TSharedPtr<FJsonValue>>* ExplicitOffsets = nullptr;
		if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("fallback_offsets_cm")))
		{
			if (!Ctx.Params->TryGetArrayField(TEXT("fallback_offsets_cm"), ExplicitOffsets) || !ExplicitOffsets)
			{
				OutError = CaptureDiagnostics::Input(TEXT("invalid_fallback_offsets"), TEXT("Fallback camera offsets must be an array of vectors in centimeters."),
					TEXT("input_decode"), TEXT("/fallback_offsets_cm"), Ctx.Params->TryGetField(TEXT("fallback_offsets_cm")),
					MakeShared<FJsonValueString>(TEXT("array of vectors in centimeters")));
				return false;
			}
			for (int32 Index = 0; Index < ExplicitOffsets->Num(); ++Index)
			{
				const auto& Value = (*ExplicitOffsets)[Index];
				FVector Offset = FVector::ZeroVector;
				if (!UeAgentJsonDiagnostics::TryReadVectorValue(Value, Offset, OutError))
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, FString::Printf(TEXT("/fallback_offsets_cm/%d"), Index));
					return false;
				}
				if (!Offset.IsNearlyZero())
				{
					CandidateOffsets.Add(Offset);
				}
			}
		}

		const FVector Target = Bounds.GetCenter();
		if (bAutoFallback && CandidateOffsets.Num() <= 1 && FallbackStepCm > KINDA_SMALL_NUMBER)
		{
			FVector BaseDir = (DesiredLocation - Target).GetSafeNormal();
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = (PreviousLocation - Target).GetSafeNormal();
			}
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = FVector::BackwardVector;
			}

			const FVector WorldUp = FVector::UpVector;
			FVector Right = FVector::CrossProduct(WorldUp, BaseDir).GetSafeNormal();
			if (Right.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Right = FVector::CrossProduct(FVector::RightVector, BaseDir).GetSafeNormal();
			}
			FVector Up = FVector::CrossProduct(BaseDir, Right).GetSafeNormal();
			if (Up.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Up = WorldUp;
			}

			CandidateOffsets.Add(Right * (float)FallbackStepCm);
			CandidateOffsets.Add(-Right * (float)FallbackStepCm);
			CandidateOffsets.Add(Up * (float)FallbackStepCm);
			CandidateOffsets.Add(-Up * (float)FallbackStepCm);
			CandidateOffsets.Add(BaseDir * (float)FallbackStepCm);
			CandidateOffsets.Add(-BaseDir * (float)FallbackStepCm);
		}

		struct FCollisionCandidate
		{
			int32 Index = 0;
			FVector Offset = FVector::ZeroVector;
			FVector Desired = FVector::ZeroVector;
			FVector Location = FVector::ZeroVector;
			bool bHit = false;
			bool bAdjusted = false;
			FHitResult Hit;
			double Score = 0.0;
		};

		FCollisionCandidate Best;
		Best.Score = -TNumericLimits<double>::Max();

		for (int32 Index = 0; Index < CandidateOffsets.Num(); ++Index)
		{
			const FVector Offset = CandidateOffsets[Index];
			const FVector CandidateDesired = DesiredLocation + Offset;

			FCollisionCandidate Candidate;
			Candidate.Index = Index;
			Candidate.Offset = Offset;
			Candidate.Desired = CandidateDesired;

			const bool bHit = World->LineTraceSingleByChannel(Candidate.Hit, Target, CandidateDesired, TraceChannel, QueryParams);
			Candidate.bHit = bHit;

			if (bHit && Candidate.Hit.bBlockingHit)
			{
				FVector Dir = (CandidateDesired - Target).GetSafeNormal();
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = (DesiredLocation - Target).GetSafeNormal();
				}
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = FVector::BackwardVector;
				}

				Candidate.Location = Candidate.Hit.ImpactPoint - (Dir * (float)SafetyOffsetCm);
				Candidate.bAdjusted = true;
			}
			else
			{
				Candidate.Location = CandidateDesired;
			}

			const double Distance = FVector::Dist(Target, Candidate.Location);
			Candidate.Score = Distance - (Candidate.bAdjusted ? 0.01 : 0.0);
			if (Candidate.Score > Best.Score)
			{
				Best = Candidate;
			}
		}

		NewLocation = Best.Location;
		bCollisionAdjusted = Best.bAdjusted;
		bFallbackUsed = (Best.Index != 0);
		FallbackIndex = Best.Index;
		FallbackOffset = Best.Offset;

		CollisionTrace = MakeShared<FJsonObject>();
		CollisionTrace->SetObjectField(TEXT("target"), VecToJson(Target));
		CollisionTrace->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
		CollisionTrace->SetBoolField(TEXT("auto_fallback"), bAutoFallback);
		CollisionTrace->SetNumberField(TEXT("fallback_step_cm"), FallbackStepCm);
		CollisionTrace->SetNumberField(TEXT("candidate_count"), CandidateOffsets.Num());
		CollisionTrace->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
		CollisionTrace->SetNumberField(TEXT("fallback_index"), FallbackIndex);
		CollisionTrace->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
		CollisionTrace->SetObjectField(TEXT("candidate_desired_location"), VecToJson(Best.Desired));
		CollisionTrace->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
		CollisionTrace->SetBoolField(TEXT("trace_complex"), bTraceComplex);
		CollisionTrace->SetNumberField(TEXT("safety_offset_cm"), SafetyOffsetCm);
		CollisionTrace->SetBoolField(TEXT("hit"), Best.bHit);
		UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::FillHitResultJson(Best.Hit, CollisionTrace);
		CollisionTrace->SetBoolField(TEXT("adjusted"), Best.bAdjusted);
		CollisionTrace->SetObjectField(TEXT("new_location"), VecToJson(NewLocation));
		if (Best.bAdjusted)
		{
			CollisionTrace->SetStringField(TEXT("adjustment_mode"), TEXT("hit_point_minus_trace_dir"));
		}
	}

	LVC->SetViewLocation(NewLocation);
	if (bLookAt)
	{
		const FVector Target = Bounds.GetCenter();
		const FVector Dir = (Target - NewLocation);
		if (!Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
		{
			FRotator LookAtRot = Dir.Rotation();
			LookAtRot.Roll = 0.0f;
			LVC->SetViewRotation(LookAtRot);
		}
	}
	LVC->Invalidate();

	FUeAgentInterfaceLogger::Log(TEXT("CmdViewportFrameActor req=%s actor=%s padding=%.3f"), *Ctx.RequestId, *Id, Padding);
	OutData->SetObjectField(TEXT("actor"), UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::MakeActorSummaryJson(Actor));
	OutData->SetNumberField(TEXT("padding"), Padding);
	OutData->SetObjectField(TEXT("bounds"), UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::MakeBoundsJson(Bounds));
	OutData->SetObjectField(TEXT("previous_location"), VecToJson(PreviousLocation));
	OutData->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
	OutData->SetObjectField(TEXT("new_location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetBoolField(TEXT("collision_aware"), bCollisionAware);
	OutData->SetBoolField(TEXT("collision_adjusted"), bCollisionAdjusted);
	OutData->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
	OutData->SetNumberField(TEXT("fallback_index"), FallbackIndex);
	OutData->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
	OutData->SetBoolField(TEXT("look_at"), bLookAt);
	if (CollisionTrace.IsValid())
	{
		OutData->SetObjectField(TEXT("collision_trace"), CollisionTrace);
	}
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetBoolField(TEXT("game_view"), LVC->IsInGameView());
	OutData->SetObjectField(TEXT("viewport"), UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::MakeViewportClientJson(LVC, SelectionReason));
	return true;
}
}
