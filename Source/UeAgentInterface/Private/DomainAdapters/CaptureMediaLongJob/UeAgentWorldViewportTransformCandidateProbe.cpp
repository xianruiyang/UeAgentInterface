#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentWorldViewportCapture.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentWorldViewportCaptureMechanics.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureSequenceFrameOptions.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "UObject/Package.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Misc/ScopeExit.h"





bool UE::UAI::DomainAdapters::CaptureMediaLongJob::FWorldViewportCapture::ProbeActorTransformCandidates(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, UE::UAI::Contract::FDiagnostic& OutError)
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	UeAgentCaptureSequenceFrameOptions::FUnifiedSequenceFrameOptionsReport UnifiedOptionsReport;
	UeAgentCaptureSequenceFrameOptions::ApplyUnifiedSequenceFrameOptionsToParams(
		Ctx.Params,
		UeAgentCaptureSequenceFrameOptions::ESequenceFrameCommandKind::ActorProbe,
		UnifiedOptionsReport);
	ON_SCOPE_EXIT { UnifiedOptionsReport.LocateDiagnosticSource(OutError); };

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = CaptureDiagnostics::Input(TEXT("missing_id"), TEXT("Transform probing requires a non-empty actor id."), TEXT("input_decode"), TEXT("/id"), Ctx.Params.IsValid() ? Ctx.Params->TryGetField(TEXT("id")) : nullptr, MakeShared<FJsonValueString>(TEXT("non-empty actor name or label")));
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id, OutError);
	if (!Actor)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* CandidateValues = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("candidates"), CandidateValues) || !CandidateValues || CandidateValues->Num() <= 0)
	{
		OutError = CaptureDiagnostics::Input(TEXT("missing_candidates"), TEXT("Transform probing requires a non-empty candidates array."), TEXT("input_decode"), TEXT("/candidates"), Ctx.Params.IsValid() ? Ctx.Params->TryGetField(TEXT("candidates")) : nullptr, MakeShared<FJsonValueString>(TEXT("non-empty array of transform candidates")));
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
		OutError = CaptureDiagnostics::Input(TEXT("unsupported_format"), TEXT("The requested probe image format is not supported."), TEXT("input_decode"), TEXT("/format"), Ctx.Params->TryGetField(TEXT("format")), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueString>(TEXT("png")), MakeShared<FJsonValueString>(TEXT("jpg")), MakeShared<FJsonValueString>(TEXT("jpeg")), MakeShared<FJsonValueString>(TEXT("webp")) }));
		return false;
	}

	int32 Quality = 90;
	double QualityValue = static_cast<double>(Quality);
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QualityValue))
	{
		Quality = FMath::Clamp(FMath::RoundToInt(QualityValue), 1, 100);
	}

	int32 MaxSize = 0;
	double MaxSizeValue = static_cast<double>(MaxSize);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeValue))
	{
		MaxSize = FMath::Max(FMath::RoundToInt(MaxSizeValue), 1);
	}

	int32 WarmupFrames = 2;
	double WarmupFramesValue = static_cast<double>(WarmupFrames);
	if (JsonTryGetNumber(Ctx.Params, TEXT("warmup_frames"), WarmupFramesValue)
		|| JsonTryGetNumber(Ctx.Params, TEXT("render_warmup_frames"), WarmupFramesValue))
	{
		WarmupFrames = FMath::Max(FMath::RoundToInt(WarmupFramesValue), 0);
	}

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
		if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FViewportCapture::TakeActiveViewportScreenshotWithWarmup(
				WarmupFrames, RawPixels, RawSize, CaptureInfo, OutError))
		{
			return false;
		}

		TArray<FColor> ResizedPixels;
		FIntPoint ResizedSize(0, 0);
		if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::ResizePixelsMaxSize(RawPixels, RawSize, MaxSize, ResizedPixels, ResizedSize, OutError))
		{
			return false;
		}
		if (FrameSize.X == 0 || FrameSize.Y == 0)
		{
			FrameSize = ResizedSize;
		}
		else if (FrameSize != ResizedSize)
		{
			OutError = CaptureDiagnostics::Native(TEXT("probe_frame_size_mismatch"),
				TEXT("The candidate capture dimensions differ from the original probe frame."), TEXT("transform_probe"),
				TEXT("frames/") + ::LexToString(FramePixels.Num()) + TEXT("/size"),
				CaptureDiagnostics::Size(ResizedSize), CaptureDiagnostics::Size(FrameSize));
			OutError.TargetPath = Actor->GetPathName();
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
		FrameObj->SetObjectField(TEXT("applied_transform"), UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::MakeActorTransformJson(AppliedTransform));
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
			const FString FramePath = UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::MakeProbeArtifactPath(Format, Ctx.RequestId, FrameLabel);
			if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::WriteCompressedImage(FramePixels.Last(), ResizedSize, Format, Quality, FramePath, OutError))
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
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_candidate"), TEXT("The transform candidate must be an object."), TEXT("transform_probe"));
			OutError.Source.JsonPath = FString::Printf(TEXT("/candidates/%d"), CandidateIndex);
			OutError.ActualValue = CandidateValue;
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
		const FString TransformPointer = FString::Printf(TEXT("/candidates/%d"), CandidateIndex)
			+ (TransformObj != CandidateObj ? TEXT("/transform") : TEXT(""));
		auto ReadTransform = [&](auto& Value, auto Reader, const TArray<FString>& Names)
		{
			for (const FString& Name : Names)
			{
				const auto* Input = UeAgentJsonDiagnostics::FindFieldValue(TransformObj, Name);
				if (!Input) continue;
				if (!Reader(*Input, Value, OutError))
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TransformPointer + TEXT("/") + Name);
					OutError.CandidateName = CandidateLabel;
					return false;
				}
				bHasAnyTransformField = true;
				break;
			}
			return true;
		};

		if (bRelativeToOriginal)
		{
			FVector LocationOffset = FVector::ZeroVector;
			if (!ReadTransform(LocationOffset, UeAgentJsonDiagnostics::TryReadVectorValue,
				{TEXT("location"), TEXT("location_offset"), TEXT("location_delta"), TEXT("delta_location")})) return false;
			CandidateLocation = OriginalTransform.GetLocation() + LocationOffset;

			FRotator RotationOffset = FRotator::ZeroRotator;
			if (!ReadTransform(RotationOffset, UeAgentJsonDiagnostics::TryReadRotatorValue,
				{TEXT("rotation"), TEXT("rotation_offset"), TEXT("rotation_delta"), TEXT("delta_rotation")})) return false;
			CandidateRotation = OriginalTransform.Rotator() + RotationOffset;

			FVector ScaleMultiplier = FVector::OneVector;
			if (!ReadTransform(ScaleMultiplier, UeAgentJsonDiagnostics::TryReadVectorValue,
				{TEXT("scale"), TEXT("scale_multiplier"), TEXT("scale_factor")})) return false;
			CandidateScale = OriginalTransform.GetScale3D() * ScaleMultiplier;
		}
		else
		{
			if (!ReadTransform(CandidateLocation, UeAgentJsonDiagnostics::TryReadVectorValue, {TEXT("location")})
				|| !ReadTransform(CandidateRotation, UeAgentJsonDiagnostics::TryReadRotatorValue, {TEXT("rotation")})
				|| !ReadTransform(CandidateScale, UeAgentJsonDiagnostics::TryReadVectorValue, {TEXT("scale")})) return false;
		}

		if (!bHasAnyTransformField)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_missing_transform_fields"), TEXT("The candidate must provide a location, rotation or scale."), TEXT("transform_probe"));
			OutError.CandidateName = CandidateLabel;
			OutError.Source.JsonPath = TransformPointer;
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
		CandidateResult->SetObjectField(TEXT("applied_transform"), UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::MakeActorTransformJson(Actor->GetActorTransform()));
		CandidateResults.Add(MakeShared<FJsonValueObject>(CandidateResult));
	}

	RestoreScope.RestoreNow();

	OutData->SetStringField(TEXT("schema"), TEXT("uai.probe.actor_transform_candidates.v1"));
	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetNumberField(TEXT("candidate_count"), CandidateValues->Num());
	OutData->SetNumberField(TEXT("frame_count"), FramePixels.Num());
	OutData->SetObjectField(TEXT("original_transform"), UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::MakeActorTransformJson(OriginalTransform));
	OutData->SetObjectField(TEXT("final_transform"), UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::MakeActorTransformJson(Actor->GetActorTransform()));
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
		if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::WriteFrameSequenceSheetImage(FramePixels, FrameLabels, FrameSize, Ctx.Params, Format, Quality, SheetPath, SheetData, OutError))
		{
			return false;
		}
		OutData->SetObjectField(TEXT("frame_sheet"), SheetData);
		OutData->SetStringField(TEXT("frame_sheet_file_path"), SheetPath);
		AddArtifact(TEXT("probe_frame_sheet"), SheetPath, FIntPoint(
			SheetData.IsValid() ? FMath::RoundToInt(SheetData->GetNumberField(TEXT("width"))) : 0,
			SheetData.IsValid() ? FMath::RoundToInt(SheetData->GetNumberField(TEXT("height"))) : 0));
	}

	UeAgentCaptureSequenceFrameOptions::AttachUnifiedSequenceFrameOptionsReport(OutData, UnifiedOptionsReport);
	return true;
}
