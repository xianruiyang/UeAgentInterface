// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioProduction.h"

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugRegistry.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioCore.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageArtifactService.h"
#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "Async/Async.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Misc/ScopeLock.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;
	using namespace ArtifactPublisher;

	void FUeAgentPieInjectedInputLedger::RecordSuccessfulInjection(
		const FKey& Key,
		const EInputEvent Event,
		const double Amount)
	{
		if (!Key.IsValid()) return;
		if (Event == IE_Pressed || Event == IE_Repeat)
		{
			DigitalKeys.Add(Key);
		}
		else if (Event == IE_Released)
		{
			DigitalKeys.Remove(Key);
		}
		else if (Event == IE_Axis)
		{
			if (FMath::IsNearlyZero(Amount)) AxisKeys.Remove(Key);
			else AxisKeys.Add(Key);
		}
	}

	TArray<FUeAgentPieInjectedInputCleanup>
	FUeAgentPieInjectedInputLedger::ConsumeCleanupActions()
	{
		TArray<FUeAgentPieInjectedInputCleanup> Actions;
		Actions.Reserve(DigitalKeys.Num() + AxisKeys.Num());
		for (const FKey& Key : DigitalKeys)
		{
			Actions.Add({Key, IE_Released, 0.0f});
		}
		for (const FKey& Key : AxisKeys)
		{
			Actions.Add({Key, IE_Axis, 0.0f});
		}
		DigitalKeys.Reset();
		AxisKeys.Reset();
		return Actions;
	}

	bool FUeAgentPieInjectedInputLedger::IsEmpty() const
	{
		return DigitalKeys.IsEmpty() && AxisKeys.IsEmpty();
	}

	bool TryParsePieInputActionSample(
		const FUeAgentCanonicalValueRef& StepValue,
		const TArray<FUeAgentPieInputActionSample>& ExistingSamples,
		FUeAgentPieInputActionSample& OutSample,
		FString& OutErrorCode)
	{
		OutSample = {};
		OutErrorCode.Reset();
		if (StepValue->GetKind() != EUeAgentValueKind::Object)
		{
			OutErrorCode = TEXT("pie_runtime_scenario_input_action_invalid");
			return false;
		}
		const auto Find = [&StepValue](const FString& Name)
			-> const FUeAgentCanonicalValueRef*
		{
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: StepValue->GetObjectItems())
			{
				if (Item.Key == Name) return &Item.Value;
			}
			return nullptr;
		};
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
			: StepValue->GetObjectItems())
		{
			if (Item.Key != TEXT("type") && Item.Key != TEXT("at_seconds")
				&& Item.Key != TEXT("action_ref") && Item.Key != TEXT("value"))
			{
				OutErrorCode = TEXT("pie_runtime_scenario_step_field_forbidden");
				return false;
			}
		}
		const FUeAgentCanonicalValueRef* Type = Find(TEXT("type"));
		const FUeAgentCanonicalValueRef* At = Find(TEXT("at_seconds"));
		const FUeAgentCanonicalValueRef* Action = Find(TEXT("action_ref"));
		const FUeAgentCanonicalValueRef* Value = Find(TEXT("value"));
		if (!Type || (*Type)->GetKind() != EUeAgentValueKind::String
			|| (*Type)->GetText() != TEXT("input_action")
			|| !At || ((*At)->GetKind() != EUeAgentValueKind::Integer
				&& (*At)->GetKind() != EUeAgentValueKind::Decimal)
			|| !LexTryParseString(OutSample.AtSeconds, *(*At)->GetLexeme())
			|| !FMath::IsFinite(OutSample.AtSeconds) || OutSample.AtSeconds < 0.0
			|| !Action || (*Action)->GetKind() != EUeAgentValueKind::String)
		{
			OutErrorCode = TEXT("pie_runtime_scenario_input_action_invalid");
			return false;
		}
		OutSample.ActionRef = (*Action)->GetText();
		if (OutSample.ActionRef.IsEmpty() || !Value)
		{
			OutErrorCode = TEXT("pie_runtime_scenario_input_action_invalid");
			return false;
		}
		if ((*Value)->GetKind() == EUeAgentValueKind::Boolean)
		{
			OutSample.ValueType = EInputActionValueType::Boolean;
			OutSample.Value.X = (*Value)->GetBoolean() ? 1.0 : 0.0;
		}
		else if ((*Value)->GetKind() == EUeAgentValueKind::Integer
			|| (*Value)->GetKind() == EUeAgentValueKind::Decimal)
		{
			double Number = 0.0;
			if (!LexTryParseString(Number, *(*Value)->GetLexeme())
				|| !FMath::IsFinite(Number))
			{
				OutErrorCode = TEXT("pie_runtime_scenario_input_action_invalid");
				return false;
			}
			OutSample.ValueType = EInputActionValueType::Axis1D;
			OutSample.Value.X = Number;
		}
		else if ((*Value)->GetKind() == EUeAgentValueKind::Array
			&& ((*Value)->GetArrayItems().Num() == 2
				|| (*Value)->GetArrayItems().Num() == 3))
		{
			for (int32 Index = 0; Index < (*Value)->GetArrayItems().Num(); ++Index)
			{
				const FUeAgentCanonicalValueRef& Item = (*Value)->GetArrayItems()[Index];
				double Number = 0.0;
				if ((Item->GetKind() != EUeAgentValueKind::Integer
						&& Item->GetKind() != EUeAgentValueKind::Decimal)
					|| !LexTryParseString(Number, *Item->GetLexeme())
					|| !FMath::IsFinite(Number))
				{
					OutErrorCode = TEXT("pie_runtime_scenario_input_action_invalid");
					return false;
				}
				OutSample.Value[Index] = Number;
			}
			OutSample.ValueType = (*Value)->GetArrayItems().Num() == 2
				? EInputActionValueType::Axis2D : EInputActionValueType::Axis3D;
		}
		else
		{
			OutErrorCode = TEXT("pie_runtime_scenario_input_action_invalid");
			return false;
		}
		if (ExistingSamples.ContainsByPredicate([&OutSample](const FUeAgentPieInputActionSample& Existing)
			{
				return Existing.ActionRef == OutSample.ActionRef
					&& Existing.AtSeconds == OutSample.AtSeconds;
			}))
		{
			OutErrorCode = TEXT("pie_runtime_scenario_input_action_duplicate_sample");
			return false;
		}
		return true;
	}

	void FUeAgentPieInputActionOperationLedger::ApplySample(
		const FUeAgentPieInputActionSample& Sample)
	{
		if (bReleaseStarted) return;
		if (Sample.IsNonZero())
		{
			ActiveSamples.Add(Sample.ActionRef, Sample);
			InjectedNonZero.Add(Sample.ActionRef);
		}
		else
		{
			ActiveSamples.Remove(Sample.ActionRef);
		}
	}

	const TMap<FString, FUeAgentPieInputActionSample>&
	FUeAgentPieInputActionOperationLedger::GetActiveSamples() const
	{
		return ActiveSamples;
	}

	void FUeAgentPieInputActionOperationLedger::RecordObservedValue(
		const FString& ActionRef,
		const bool bNonZero)
	{
		if (!InjectedNonZero.Contains(ActionRef)) return;
		LatestObservedNonZero.Add(ActionRef, bNonZero);
		if (bNonZero) ObservedNonZero.Add(ActionRef);
	}

	void FUeAgentPieInputActionOperationLedger::BeginRelease(
		const double WallSeconds,
		const uint64 Frame)
	{
		if (bReleaseStarted) return;
		bReleaseStarted = true;
		ReleaseStartWallSeconds = WallSeconds;
		ReleaseStartFrame = Frame;
		ActiveSamples.Reset();
	}

	EUeAgentPieInputActionReleaseStatus
	FUeAgentPieInputActionOperationLedger::EvaluateRelease(
		const double WallSeconds,
		const uint64 Frame) const
	{
		if (!bReleaseStarted) return EUeAgentPieInputActionReleaseStatus::Pending;
		bool bReleased = true;
		for (const FString& ActionRef : InjectedNonZero)
		{
			const bool* Current = LatestObservedNonZero.Find(ActionRef);
			if (!ObservedNonZero.Contains(ActionRef) || !Current || *Current)
			{
				bReleased = false;
				break;
			}
		}
		if (bReleased) return EUeAgentPieInputActionReleaseStatus::Verified;
		if (WallSeconds - ReleaseStartWallSeconds > ReleaseGraceSeconds
			|| Frame - ReleaseStartFrame > ReleaseGraceFrames)
		{
			return EUeAgentPieInputActionReleaseStatus::Unverified;
		}
		return EUeAgentPieInputActionReleaseStatus::Pending;
	}

	bool FUeAgentPieInputActionOperationLedger::HasInjectedNonZero() const
	{
		return !InjectedNonZero.IsEmpty();
	}

	bool FUeAgentPieInputActionOperationLedger::IsReinjectionStopped() const
	{
		return bReleaseStarted && ActiveSamples.IsEmpty();
	}

	bool TryParsePieCaptureStepSpec(
		const FUeAgentCanonicalValueRef& StepValue,
		FUeAgentPieCaptureStepSpec& OutSpec,
		FString& OutErrorCode)
	{
		OutSpec = {};
		OutErrorCode.Reset();
		if (StepValue->GetKind() != EUeAgentValueKind::Object)
		{
			OutErrorCode = TEXT("pie_runtime_scenario_capture_invalid");
			return false;
		}
		const auto Find = [&StepValue](const FString& Name)
			-> const FUeAgentCanonicalValueRef*
		{
			for (const auto& Item : StepValue->GetObjectItems()) if (Item.Key == Name) return &Item.Value;
			return nullptr;
		};
		for (const auto& Item : StepValue->GetObjectItems())
		{
			if (Item.Key != TEXT("type") && Item.Key != TEXT("at_seconds")
				&& Item.Key != TEXT("source") && Item.Key != TEXT("format")
				&& Item.Key != TEXT("camera_ref") && Item.Key != TEXT("view_transform")
				&& Item.Key != TEXT("fov_degrees"))
			{
				OutErrorCode = TEXT("pie_runtime_scenario_step_field_forbidden");
				return false;
			}
		}
		const FUeAgentCanonicalValueRef* Type = Find(TEXT("type"));
		const FUeAgentCanonicalValueRef* AtSeconds = Find(TEXT("at_seconds"));
		const FUeAgentCanonicalValueRef* Source = Find(TEXT("source"));
		const FUeAgentCanonicalValueRef* Format = Find(TEXT("format"));
		double ParsedAtSeconds = 0.0;
		if (!Type || (*Type)->GetKind() != EUeAgentValueKind::String
			|| (*Type)->GetText() != TEXT("capture")
			|| !AtSeconds
			|| ((*AtSeconds)->GetKind() != EUeAgentValueKind::Integer
				&& (*AtSeconds)->GetKind() != EUeAgentValueKind::Decimal)
			|| !LexTryParseString(ParsedAtSeconds, *(*AtSeconds)->GetLexeme())
			|| !FMath::IsFinite(ParsedAtSeconds) || ParsedAtSeconds < 0.0
			|| !Source || (*Source)->GetKind() != EUeAgentValueKind::String
			|| !Format || (*Format)->GetKind() != EUeAgentValueKind::String
			|| (*Format)->GetText() != TEXT("png"))
		{
			OutErrorCode = TEXT("pie_runtime_scenario_capture_invalid");
			return false;
		}
		const FString SourceText = (*Source)->GetText();
		const FUeAgentCanonicalValueRef* CameraRef = Find(TEXT("camera_ref"));
		const FUeAgentCanonicalValueRef* ViewTransform = Find(TEXT("view_transform"));
		const FUeAgentCanonicalValueRef* FovDegrees = Find(TEXT("fov_degrees"));
		if (SourceText == TEXT("pie_output_surface"))
		{
			if (CameraRef || ViewTransform || FovDegrees)
			{
				OutErrorCode = TEXT("pie_runtime_scenario_capture_fields_forbidden");
				return false;
			}
			OutSpec.Source = EUeAgentPieCaptureSource::OutputSurface;
			return true;
		}
		if (SourceText == TEXT("pie_player_camera"))
		{
			if (CameraRef || ViewTransform || FovDegrees)
			{
				OutErrorCode = TEXT("pie_runtime_scenario_capture_fields_forbidden");
				return false;
			}
			OutSpec.Source = EUeAgentPieCaptureSource::PlayerCamera;
			return true;
		}
		if (SourceText != TEXT("scene_camera")
			|| (CameraRef && ((*CameraRef)->GetKind() != EUeAgentValueKind::String
				|| (*CameraRef)->GetText().IsEmpty())))
		{
			OutErrorCode = TEXT("pie_runtime_scenario_capture_source_invalid");
			return false;
		}
		OutSpec.Source = EUeAgentPieCaptureSource::SceneCamera;
		if (CameraRef) OutSpec.CameraRef = (*CameraRef)->GetText();
		if (FovDegrees)
		{
			double Fov = 0.0;
			if (((*FovDegrees)->GetKind() != EUeAgentValueKind::Integer
					&& (*FovDegrees)->GetKind() != EUeAgentValueKind::Decimal)
				|| !LexTryParseString(Fov, *(*FovDegrees)->GetLexeme())
				|| !FMath::IsFinite(Fov) || Fov < 1.0 || Fov > 179.0)
			{
				OutErrorCode = TEXT("pie_runtime_scenario_capture_fov_invalid");
				return false;
			}
			OutSpec.OverrideFovDegrees = static_cast<float>(Fov);
		}
		if (!ViewTransform)
		{
			if (OutSpec.CameraRef.IsEmpty())
			{
				OutErrorCode = TEXT("pie_runtime_scenario_capture_transient_camera_incomplete");
				return false;
			}
			return true;
		}
		if ((*ViewTransform)->GetKind() != EUeAgentValueKind::Object)
		{
			OutErrorCode = TEXT("pie_runtime_scenario_capture_view_transform_invalid");
			return false;
		}
		const auto FindTransform = [ViewTransform](const FString& Name)
			-> const FUeAgentCanonicalValueRef*
		{
			for (const auto& Item : (*ViewTransform)->GetObjectItems()) if (Item.Key == Name) return &Item.Value;
			return nullptr;
		};
		for (const auto& Item : (*ViewTransform)->GetObjectItems())
		{
			if (Item.Key != TEXT("frame") && Item.Key != TEXT("dimension")
				&& Item.Key != TEXT("unit") && Item.Key != TEXT("rotation_format")
				&& Item.Key != TEXT("location") && Item.Key != TEXT("rotation"))
			{
				OutErrorCode = TEXT("pie_runtime_scenario_capture_view_transform_field_forbidden");
				return false;
			}
		}
		const FUeAgentCanonicalValueRef* Frame = FindTransform(TEXT("frame"));
		const FUeAgentCanonicalValueRef* Dimension = FindTransform(TEXT("dimension"));
		const FUeAgentCanonicalValueRef* Unit = FindTransform(TEXT("unit"));
		const FUeAgentCanonicalValueRef* RotationFormat = FindTransform(TEXT("rotation_format"));
		const FUeAgentCanonicalValueRef* Location = FindTransform(TEXT("location"));
		const FUeAgentCanonicalValueRef* Rotation = FindTransform(TEXT("rotation"));
		int64 DimensionValue = 0;
		if (!Frame || (*Frame)->GetKind() != EUeAgentValueKind::String || (*Frame)->GetText() != TEXT("ue_world")
			|| !Dimension || (*Dimension)->GetKind() != EUeAgentValueKind::Integer
			|| !LexTryParseString(DimensionValue, *(*Dimension)->GetLexeme()) || DimensionValue != 3
			|| !Unit || (*Unit)->GetKind() != EUeAgentValueKind::String || (*Unit)->GetText() != TEXT("centimeter")
			|| !RotationFormat || (*RotationFormat)->GetKind() != EUeAgentValueKind::String
			|| (*RotationFormat)->GetText() != TEXT("pitch_yaw_roll_degrees")
			|| (!Location && !Rotation))
		{
			OutErrorCode = TEXT("pie_runtime_scenario_capture_view_transform_invalid");
			return false;
		}
		const auto ParseVector = [](const FUeAgentCanonicalValueRef* Value, FVector& Out)
		{
			if (!Value || (*Value)->GetKind() != EUeAgentValueKind::Array
				|| (*Value)->GetArrayItems().Num() != 3) return false;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				const auto& Item = (*Value)->GetArrayItems()[Index];
				double Number = 0.0;
				if ((Item->GetKind() != EUeAgentValueKind::Integer && Item->GetKind() != EUeAgentValueKind::Decimal)
					|| !LexTryParseString(Number, *Item->GetLexeme()) || !FMath::IsFinite(Number)) return false;
				Out[Index] = Number;
			}
			return true;
		};
		FVector Vector;
		if (Location)
		{
			if (!ParseVector(Location, Vector)) { OutErrorCode = TEXT("pie_runtime_scenario_capture_view_transform_invalid"); return false; }
			OutSpec.OverrideLocation = Vector;
		}
		if (Rotation)
		{
			if (!ParseVector(Rotation, Vector)) { OutErrorCode = TEXT("pie_runtime_scenario_capture_view_transform_invalid"); return false; }
			OutSpec.OverrideRotation = FRotator(Vector.X, Vector.Y, Vector.Z);
		}
		if (OutSpec.CameraRef.IsEmpty()
			&& (!OutSpec.OverrideLocation.IsSet() || !OutSpec.OverrideRotation.IsSet()
				|| !OutSpec.OverrideFovDegrees.IsSet()))
		{
			OutErrorCode = TEXT("pie_runtime_scenario_capture_transient_camera_incomplete");
			return false;
		}
		return true;
	}

	FString ResolvePieCaptureFailureCode(
		const FString& ExactErrorCode,
		const TCHAR* FallbackErrorCode)
	{
		return ExactErrorCode.IsEmpty() ? FString(FallbackErrorCode) : ExactErrorCode;
	}

	namespace
	{
		enum class ETimelineStepType : uint8
		{
			Wait,
			InputKey,
			ActorTransform,
			Capture,
			InputAction,
		};

		enum class EScenarioDispatchPhase : uint8
		{
			Scheduled,
			Running,
			Terminal,
		};

		struct FTransformPatch
		{
			TOptional<FVector> Location;
			TOptional<FRotator> Rotation;
			TOptional<FVector> Scale;
		};

		struct FTimelineStep
		{
			ETimelineStepType Type = ETimelineStepType::Wait;
			double AtGameSeconds = 0.0;
			FKey Key;
			EInputEvent InputEvent = IE_Pressed;
			double Amount = 1.0;
			FString ActorRef;
			FTransformPatch Transform;
			FString ActionRef;
			FVector ActionValue = FVector::ZeroVector;
			EInputActionValueType ActionValueType = EInputActionValueType::Boolean;
			TWeakObjectPtr<const UInputAction> Action;
			FUeAgentPieCaptureStepSpec CaptureSpec;

			bool IsActionValueNonZero() const
			{
				return !ActionValue.IsNearlyZero();
			}
		};

		struct FObservationField
		{
			FString ObjectRef;
			FString Property;
		};

		struct FScenarioPlan
		{
			FUeAgentPieScenarioRequest CoreRequest;
			TArray<FTimelineStep> Steps;
			TArray<FObservationField> ObservationFields;
		};

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(MoveTemp(Code), MoveTemp(Detail));
			return false;
		}

		const FUeAgentCanonicalValueRef* FindField(
			const FUeAgentCanonicalValueRef& Object,
			const FString& Name)
		{
			if (Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object->GetObjectItems())
			{
				if (Item.Key == Name)
				{
					return &Item.Value;
				}
			}
			return nullptr;
		}

		bool TryNumber(
			const FUeAgentCanonicalValueRef& Object,
			const FString& Name,
			double& OutValue)
		{
			const FUeAgentCanonicalValueRef* Value = FindField(Object, Name);
			if (!Value
				|| ((*Value)->GetKind() != EUeAgentValueKind::Integer
					&& (*Value)->GetKind() != EUeAgentValueKind::Decimal))
			{
				return false;
			}
			return LexTryParseString(OutValue, *(*Value)->GetLexeme())
				&& FMath::IsFinite(OutValue);
		}

		bool TryInteger(
			const FUeAgentCanonicalValueRef& Object,
			const FString& Name,
			int64& OutValue)
		{
			const FUeAgentCanonicalValueRef* Value = FindField(Object, Name);
			return Value
				&& (*Value)->GetKind() == EUeAgentValueKind::Integer
				&& LexTryParseString(OutValue, *(*Value)->GetLexeme());
		}

		bool TryString(
			const FUeAgentCanonicalValueRef& Object,
			const FString& Name,
			FString& OutValue)
		{
			const FUeAgentCanonicalValueRef* Value = FindField(Object, Name);
			if (!Value || (*Value)->GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = (*Value)->GetText();
			return !OutValue.IsEmpty();
		}

		bool HasOnlyFields(
			const FUeAgentCanonicalValueRef& Object,
			const TSet<FString>& Allowed,
			FString& OutUnexpected)
		{
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object->GetObjectItems())
			{
				if (!Allowed.Contains(Item.Key))
				{
					OutUnexpected = Item.Key;
					return false;
				}
			}
			return true;
		}

		bool TryVector(
			const FUeAgentCanonicalValueRef& Object,
			const FString& Name,
			FVector& OutValue)
		{
			const FUeAgentCanonicalValueRef* Value = FindField(Object, Name);
			if (!Value || (*Value)->GetKind() != EUeAgentValueKind::Array
				|| (*Value)->GetArrayItems().Num() != 3)
			{
				return false;
			}
			double Values[3]{};
			for (int32 Index = 0; Index < 3; ++Index)
			{
				const FUeAgentCanonicalValueRef& Item = (*Value)->GetArrayItems()[Index];
				if ((Item->GetKind() != EUeAgentValueKind::Integer
						&& Item->GetKind() != EUeAgentValueKind::Decimal)
					|| !LexTryParseString(Values[Index], *Item->GetLexeme())
					|| !FMath::IsFinite(Values[Index]))
				{
					return false;
				}
			}
			OutValue = FVector(Values[0], Values[1], Values[2]);
			return true;
		}

		bool TryParsePlan(
			const FUeAgentPieRuntimeScenarioTypedRequest& Request,
			FScenarioPlan& OutPlan,
			FExecutionMutationError& OutError)
		{
			const FUeAgentCanonicalValueRef& Params = Request.GetParameters();
			OutPlan.CoreRequest.SessionIdentity = Request.GetSessionRef();
			if (!TryInteger(Params, TEXT("expected_pause_epoch"),
					OutPlan.CoreRequest.ExpectedPauseEpoch)
				|| !TryInteger(Params, TEXT("expected_workspace_revision"),
					OutPlan.CoreRequest.ExpectedWorkspaceRevision))
			{
				return Fail(OutError, TEXT("pie_runtime_scenario_revision_invalid"));
			}
			const FUeAgentCanonicalValueRef* Limits = FindField(Params, TEXT("limits"));
			if (!Limits
				|| !TryNumber(*Limits, TEXT("wall_seconds"),
					OutPlan.CoreRequest.WallTimeLimitSeconds)
				|| !TryNumber(*Limits, TEXT("game_seconds"),
					OutPlan.CoreRequest.GameTimeLimitSeconds)
				|| !TryInteger(*Limits, TEXT("frame_count"),
					OutPlan.CoreRequest.FrameLimit)
				|| !OutPlan.CoreRequest.IsValid())
			{
				return Fail(OutError, TEXT("pie_runtime_scenario_limits_invalid"));
			}

			const FUeAgentCanonicalValueRef* Steps = FindField(Params, TEXT("steps"));
			if (!Steps || (*Steps)->GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, TEXT("pie_runtime_scenario_steps_invalid"));
			}
			double PreviousAt = -1.0;
			for (const FUeAgentCanonicalValueRef& StepValue : (*Steps)->GetArrayItems())
			{
				FString Type;
				FTimelineStep Step;
				if (StepValue->GetKind() != EUeAgentValueKind::Object
					|| !TryString(StepValue, TEXT("type"), Type)
					|| !TryNumber(StepValue, TEXT("at_seconds"), Step.AtGameSeconds)
					|| Step.AtGameSeconds < PreviousAt
					|| Step.AtGameSeconds < 0.0)
				{
					return Fail(OutError, TEXT("pie_runtime_scenario_timeline_invalid"));
				}
				PreviousAt = Step.AtGameSeconds;
				if (Type == TEXT("wait"))
				{
					FString Unexpected;
					if (!HasOnlyFields(
							StepValue,
							{TEXT("type"), TEXT("at_seconds")},
							Unexpected))
						return Fail(OutError, TEXT("pie_runtime_scenario_step_field_forbidden"), Unexpected);
					Step.Type = ETimelineStepType::Wait;
				}
				else if (Type == TEXT("input_key"))
				{
					FString Unexpected;
					if (!HasOnlyFields(
							StepValue,
							{TEXT("type"), TEXT("at_seconds"), TEXT("key"), TEXT("event"), TEXT("amount")},
							Unexpected))
						return Fail(OutError, TEXT("pie_runtime_scenario_step_field_forbidden"), Unexpected);
					FString KeyName;
					FString Event;
					Step.Type = ETimelineStepType::InputKey;
					if (!TryString(StepValue, TEXT("key"), KeyName)
						|| !TryString(StepValue, TEXT("event"), Event))
					{
						return Fail(OutError, TEXT("pie_runtime_scenario_input_key_invalid"));
					}
					Step.Key = FKey(FName(*KeyName));
					if (!Step.Key.IsValid())
					{
						return Fail(OutError, TEXT("pie_runtime_scenario_input_key_unknown"), KeyName);
					}
					if (Event == TEXT("pressed")) Step.InputEvent = IE_Pressed;
					else if (Event == TEXT("released")) Step.InputEvent = IE_Released;
					else if (Event == TEXT("repeat")) Step.InputEvent = IE_Repeat;
					else if (Event == TEXT("axis")) Step.InputEvent = IE_Axis;
					else return Fail(OutError, TEXT("pie_runtime_scenario_input_event_invalid"));
					if (!TryNumber(StepValue, TEXT("amount"), Step.Amount))
					{
						Step.Amount = 1.0;
					}
				}
				else if (Type == TEXT("actor_transform"))
				{
					FString Unexpected;
					if (!HasOnlyFields(
							StepValue,
							{TEXT("type"), TEXT("at_seconds"), TEXT("actor_ref"), TEXT("transform")},
							Unexpected))
						return Fail(OutError, TEXT("pie_runtime_scenario_step_field_forbidden"), Unexpected);
					Step.Type = ETimelineStepType::ActorTransform;
					const FUeAgentCanonicalValueRef* Transform =
						FindField(StepValue, TEXT("transform"));
					if (!TryString(StepValue, TEXT("actor_ref"), Step.ActorRef)
						|| !Transform
						|| (*Transform)->GetKind() != EUeAgentValueKind::Object)
					{
						return Fail(OutError, TEXT("pie_runtime_scenario_actor_transform_invalid"));
					}
					FString Frame;
					FString Unit;
					FString RotationFormat;
					int64 Dimension = 0;
					if (!TryString(*Transform, TEXT("frame"), Frame)
						|| !TryString(*Transform, TEXT("unit"), Unit)
						|| !TryString(*Transform, TEXT("rotation_format"), RotationFormat)
						|| !TryInteger(*Transform, TEXT("dimension"), Dimension)
						|| Frame != TEXT("ue_world")
						|| Unit != TEXT("centimeter")
						|| RotationFormat != TEXT("pitch_yaw_roll_degrees")
						|| Dimension != 3)
					{
						return Fail(OutError, TEXT("pie_runtime_scenario_transform_frame_unsupported"));
					}
					FVector Vector;
					if (FindField(*Transform, TEXT("location")))
					{
						if (!TryVector(*Transform, TEXT("location"), Vector))
							return Fail(OutError, TEXT("pie_runtime_scenario_location_invalid"));
						Step.Transform.Location = Vector;
					}
					if (FindField(*Transform, TEXT("rotation")))
					{
						if (!TryVector(*Transform, TEXT("rotation"), Vector))
							return Fail(OutError, TEXT("pie_runtime_scenario_rotation_invalid"));
						Step.Transform.Rotation = FRotator(Vector.X, Vector.Y, Vector.Z);
					}
					if (FindField(*Transform, TEXT("scale")))
					{
						if (!TryVector(*Transform, TEXT("scale"), Vector))
							return Fail(OutError, TEXT("pie_runtime_scenario_scale_invalid"));
						Step.Transform.Scale = Vector;
					}
					if (!Step.Transform.Location.IsSet()
						&& !Step.Transform.Rotation.IsSet()
						&& !Step.Transform.Scale.IsSet())
					{
						return Fail(OutError, TEXT("pie_runtime_scenario_transform_empty"));
					}
				}
				else if (Type == TEXT("input_action") || Type == TEXT("capture"))
				{
					if (Type == TEXT("input_action"))
					{
						TArray<FUeAgentPieInputActionSample> ExistingSamples;
						for (const FTimelineStep& Existing : OutPlan.Steps)
							{
								if (Existing.Type == ETimelineStepType::InputAction)
								{
									ExistingSamples.Add({Existing.ActionRef, Existing.AtGameSeconds,
										Existing.ActionValueType, Existing.ActionValue});
								}
							}
						FUeAgentPieInputActionSample Sample;
						FString ParseError;
						if (!TryParsePieInputActionSample(
								StepValue, ExistingSamples, Sample, ParseError))
						{
							return Fail(OutError, MoveTemp(ParseError));
						}
						Step.Type = ETimelineStepType::InputAction;
						Step.ActionRef = MoveTemp(Sample.ActionRef);
						Step.AtGameSeconds = Sample.AtSeconds;
						Step.ActionValueType = Sample.ValueType;
						Step.ActionValue = Sample.Value;
						OutPlan.Steps.Add(MoveTemp(Step));
						continue;
					}
					FString CaptureParseError;
					if (!TryParsePieCaptureStepSpec(
							StepValue, Step.CaptureSpec, CaptureParseError))
					{
						return Fail(OutError, MoveTemp(CaptureParseError));
					}
					if (OutPlan.Steps.ContainsByPredicate([](const FTimelineStep& Existing)
						{ return Existing.Type == ETimelineStepType::Capture; }))
					{
						return Fail(OutError, TEXT("pie_runtime_scenario_capture_multiple_forbidden"));
					}
					Step.Type = ETimelineStepType::Capture;
				}
				else
				{
					return Fail(OutError, TEXT("pie_runtime_scenario_step_type_unknown"), Type);
				}
				OutPlan.Steps.Add(MoveTemp(Step));
			}

			if (const FUeAgentCanonicalValueRef* Fields =
				FindField(Params, TEXT("observation_fields")))
			{
				if ((*Fields)->GetKind() != EUeAgentValueKind::Array)
					return Fail(OutError, TEXT("pie_runtime_scenario_observations_invalid"));
				for (const FUeAgentCanonicalValueRef& FieldValue : (*Fields)->GetArrayItems())
				{
					FObservationField Field;
					if (FieldValue->GetKind() != EUeAgentValueKind::Object
						|| !TryString(FieldValue, TEXT("object_ref"), Field.ObjectRef)
						|| !TryString(FieldValue, TEXT("property"), Field.Property))
					{
						return Fail(OutError, TEXT("pie_runtime_scenario_observation_invalid"));
					}
					static const TSet<FString> BuiltInObservationFields = {
						TEXT("actor.transform"),
						TEXT("actor.location"),
						TEXT("actor.rotation"),
						TEXT("actor.scale"),
						TEXT("actor.hidden"),
						TEXT("component.active"),
						TEXT("component.visible"),
					};
					if (!BuiltInObservationFields.Contains(Field.Property))
					{
						return Fail(
							OutError,
							TEXT("pie_runtime_scenario_observation_field_unsupported"),
							Field.Property);
					}
					OutPlan.ObservationFields.Add(MoveTemp(Field));
				}
			}
			return true;
		}

		bool TryMakeStringValue(
			const FString& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
					Value, Resources, OutValue, Error)
				|| !OutValue.IsValid())
			{
				return false;
			}
			return true;
		}

		bool TryMakeIntegerValue(
			int64 Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryInteger(
					FString::Printf(TEXT("%lld"), Value),
					Resources,
					OutValue,
					Error)
				&& OutValue.IsValid();
		}

		bool TryMakeObservationSignal(
			const TArray<TPair<FString, FString>>& Values,
			const FString& TerminalStatus,
			const FString& ErrorCode,
			const FString& SessionRef,
			int64 PauseEpoch,
			int64 WorkspaceRevision,
			const FString& WorkspacePath,
			const TOptional<FFileArtifactReference>& Artifact,
			const FString& CaptureSourceRef,
			const FString& CaptureEvidenceId,
			int64 SignalRevision,
			FUeAgentResourceTracker& Resources,
			TOptional<FLongRunningOperationSignal>& OutSignal)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> ObservationItems;
			for (const TPair<FString, FString>& Value : Values)
			{
				FUeAgentCanonicalValuePtr Canonical;
				if (!TryMakeStringValue(Value.Value, Resources, Canonical))
				{
					return false;
				}
				ObservationItems.Emplace(Value.Key, Canonical.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Observations;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(
					ObservationItems, Resources, Observations, Error)
				|| !Observations.IsValid())
			{
				return false;
			}
			FUeAgentCanonicalValuePtr Schema;
			FUeAgentCanonicalValuePtr Status;
			FUeAgentCanonicalValuePtr ErrorCodeValue;
			FUeAgentCanonicalValuePtr Session;
			FUeAgentCanonicalValuePtr Path;
			FUeAgentCanonicalValuePtr Epoch;
			FUeAgentCanonicalValuePtr Revision;
			if (!TryMakeStringValue(
					TEXT("uai.pie.runtime.observation/1"), Resources, Schema)
				|| !TryMakeStringValue(TerminalStatus, Resources, Status)
				|| !TryMakeStringValue(ErrorCode, Resources, ErrorCodeValue)
				|| !TryMakeStringValue(SessionRef, Resources, Session)
				|| !TryMakeStringValue(WorkspacePath, Resources, Path)
				|| !TryMakeIntegerValue(PauseEpoch, Resources, Epoch)
				|| !TryMakeIntegerValue(WorkspaceRevision, Resources, Revision))
			{
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> RootItems = {
				{TEXT("schema"), Schema.ToSharedRef()},
				{TEXT("terminal_status"), Status.ToSharedRef()},
				{TEXT("error_code"), ErrorCodeValue.ToSharedRef()},
				{TEXT("session_ref"), Session.ToSharedRef()},
				{TEXT("pause_epoch"), Epoch.ToSharedRef()},
				{TEXT("workspace_revision"), Revision.ToSharedRef()},
				{TEXT("workspace_path"), Path.ToSharedRef()},
				{TEXT("fields"), Observations.ToSharedRef()},
			};
			if (Artifact.IsSet())
			{
				FUeAgentCanonicalValuePtr ArtifactValue;
				FUeAgentCanonicalValuePtr SourceValue;
				FUeAgentCanonicalValuePtr EvidenceValue;
				FFileArtifactPublishError ArtifactError;
				if (!TryProjectFileArtifactReference(
						Artifact.GetValue(), Resources, ArtifactValue, ArtifactError)
					|| !ArtifactValue.IsValid()
					|| CaptureSourceRef.IsEmpty()
					|| CaptureEvidenceId.IsEmpty()
					|| !TryMakeStringValue(CaptureSourceRef, Resources, SourceValue)
					|| !TryMakeStringValue(CaptureEvidenceId, Resources, EvidenceValue)) return false;
				RootItems.Emplace(TEXT("artifact_ref"), ArtifactValue.ToSharedRef());
				RootItems.Emplace(TEXT("capture_source_ref"), SourceValue.ToSharedRef());
				RootItems.Emplace(TEXT("capture_evidence_id"), EvidenceValue.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Root;
			if (!FUeAgentCanonicalValue::TryObject(
					RootItems, Resources, Root, Error)
				|| !Root.IsValid())
			{
				return false;
			}
			OutSignal.Emplace(
				TEXT("uai.pie.runtime.observation/1"),
				SignalRevision,
				Root.ToSharedRef());
			return OutSignal.IsSet() && OutSignal->IsValid();
		}

		class FScenarioSharedState final
			: public TSharedFromThis<FScenarioSharedState, ESPMode::ThreadSafe>
			, public IUeAgentPieScenarioPorts
		{
		public:
			FScenarioSharedState(
				FScenarioPlan InPlan,
				TSharedRef<FUeAgentPieRuntimeDebugRegistry,
					ESPMode::ThreadSafe> InRegistry,
				TSharedRef<CaptureMediaLongJob::IUeAgentCaptureImageArtifactService,
					ESPMode::ThreadSafe> InImageArtifactService,
				FString InOperationId,
				FLongRunningWorkIdentity InIdentity,
				FUeAgentCancellationToken InCancellation)
				: Plan(MoveTemp(InPlan))
				, Registry(InRegistry)
				, ImageArtifactService(InImageArtifactService)
				, OperationId(MoveTemp(InOperationId))
				, Identity(MoveTemp(InIdentity))
				, Cancellation(MoveTemp(InCancellation))
			{
				TerminalStatus = TEXT("scheduled");
				ObservedPauseEpoch = Plan.CoreRequest.ExpectedPauseEpoch;
				ObservedWorkspaceRevision =
					Plan.CoreRequest.ExpectedWorkspaceRevision;
			}

			bool IsCurrentPausedSnapshot(
				const FString& SessionIdentity,
				int64 ExpectedPauseEpoch,
				int64 ExpectedWorkspaceRevision) const override
			{
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				FString Error;
				return Registry->TryResolveExact(
					SessionIdentity,
					ExpectedPauseEpoch,
					ExpectedWorkspaceRevision,
					Snapshot,
					Error);
			}

			bool Resume() override
			{
				FString Error;
				return Registry->TryResume(Plan.CoreRequest.SessionIdentity, Error);
			}

			EUeAgentPieScenarioStepResult ExecuteSteps(double, double, int64) override
			{
				return EUeAgentPieScenarioStepResult::Failed;
			}

			bool StopDispatch() override
			{
				TickerHandle.Reset();
				bDispatchStopped = true;
				return true;
			}

			bool ReleaseAllForOperation() override
			{
				const bool bActionReleaseSucceeded = !InputActionLedger.HasInjectedNonZero()
					|| bActionReleaseVerified;
				if (bKeysReleased)
				{
					return bActionReleaseSucceeded;
				}
				if (InjectedInput.IsEmpty())
				{
					bKeysReleased = true;
					return bActionReleaseSucceeded;
				}
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				FString Error;
				if (!Registry->TryResolveExact(
						Plan.CoreRequest.SessionIdentity,
						Plan.CoreRequest.ExpectedPauseEpoch,
						Plan.CoreRequest.ExpectedWorkspaceRevision,
						Snapshot,
						Error))
				{
					return false;
				}
				APlayerController* Controller = Snapshot.PlayerController.Get();
				if (!Controller) return false;
				bKeysReleased = true;
				bool bSucceeded = true;
				for (const FUeAgentPieInjectedInputCleanup& Cleanup
					: InjectedInput.ConsumeCleanupActions())
				{
					bSucceeded = Controller->InputKey(
						FInputKeyEventArgs::CreateSimulated(
							Cleanup.Key,
							Cleanup.Event,
							Cleanup.Amount)) && bSucceeded;
				}
				return bSucceeded && bActionReleaseSucceeded;
			}

			bool RepauseAndVerify(const FString& SessionIdentity, int64& OutPauseEpoch) override
			{
				FString Error;
				return Registry->TryRepauseAndVerify(SessionIdentity, OutPauseEpoch, Error);
			}

			bool RefreshCurrentWorkspace(
				const FString& SessionIdentity,
				int64 PreviousWorkspaceRevision,
				int64& OutWorkspaceRevision) override
			{
				FString Error;
				return Registry->TryRefreshCurrentWorkspace(
					SessionIdentity,
					PreviousWorkspaceRevision,
					OutWorkspaceRevision,
					Error);
			}

			void SetTerminalNoSideEffects(
				const FString& InStatus,
				const FString& InErrorCode)
			{
				FScopeLock Lock(&Mutex);
				Phase = EScenarioDispatchPhase::Terminal;
				++SignalRevision;
				TerminalStatus = InStatus;
				ErrorCode = InErrorCode;
				WorkState = ELongRunningWorkState::ExitedFailure;
			}

			void StoreReceipt(const FUeAgentPieScenarioReceipt& Receipt)
			{
				FScopeLock Lock(&Mutex);
				Phase = EScenarioDispatchPhase::Terminal;
				++SignalRevision;
				TerminalStatus = Receipt.TerminalStatus;
				ErrorCode = Receipt.ErrorCode;
				ObservedPauseEpoch = Receipt.PauseEpoch;
				ObservedWorkspaceRevision = Receipt.WorkspaceRevision;
				WorkState = Receipt.bStateKnown
					? (Receipt.TerminalStatus == TEXT("succeeded")
						? ELongRunningWorkState::ExitedSuccess
						: ELongRunningWorkState::ExitedFailure)
					: ELongRunningWorkState::StateUnknown;
			}

			void BeginOnGameThread()
			{
				check(IsInGameThread());
				bool bCancelled = Cancellation.IsCancelled();
				{
					FScopeLock Lock(&Mutex);
					bCancelled = bCancelled || bStopRequested;
				}
				if (bCancelled)
				{
					SetTerminalNoSideEffects(
						TEXT("cancelled"),
						TEXT("pie_runtime_scenario_cancelled_before_resume"));
					return;
				}
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				FString RegistryError;
				if (!Registry->TryResolveExact(
						Plan.CoreRequest.SessionIdentity,
						Plan.CoreRequest.ExpectedPauseEpoch,
						Plan.CoreRequest.ExpectedWorkspaceRevision,
						Snapshot,
						RegistryError)
					|| !Snapshot.World.IsValid()
					|| !Snapshot.World->IsPaused())
				{
					SetTerminalNoSideEffects(
						TEXT("stale"),
						TEXT("pie_runtime_scenario_snapshot_stale"));
					return;
				}
				{
					FScopeLock Lock(&Mutex);
					WorkspacePath = Snapshot.WorkspacePath;
					ObservedPauseEpoch = Snapshot.PauseEpoch;
					ObservedWorkspaceRevision = Snapshot.WorkspaceRevision;
				}
				if (!ResolveInputActions(Snapshot))
				{
					SetTerminalNoSideEffects(
						TEXT("failed"),
						TEXT("pie_runtime_scenario_input_action_binding_invalid"));
					return;
				}
				bCancelled = Cancellation.IsCancelled();
				{
					FScopeLock Lock(&Mutex);
					bCancelled = bCancelled || bStopRequested;
				}
				if (bCancelled)
				{
					SetTerminalNoSideEffects(
						TEXT("cancelled"),
						TEXT("pie_runtime_scenario_cancelled_before_resume"));
					return;
				}
				if (!Resume())
				{
					if (Snapshot.World->IsPaused())
					{
						SetTerminalNoSideEffects(
							TEXT("failed"),
							TEXT("pie_runtime_scenario_resume_failed"));
					}
					else
					{
						StoreReceipt(FinalizePieRuntimeScenario(
							Plan.CoreRequest,
							EUeAgentPieScenarioStepResult::Failed,
							*this));
					}
					return;
				}
				StartWallSeconds = FPlatformTime::Seconds();
				StartGameSeconds = Snapshot.World->GetTimeSeconds();
				StartFrame = GFrameCounter;
				{
					FScopeLock Lock(&Mutex);
					Phase = EScenarioDispatchPhase::Running;
					++SignalRevision;
					TerminalStatus = TEXT("running");
				}
				const TWeakPtr<FScenarioSharedState, ESPMode::ThreadSafe> WeakSelf = AsShared();
				TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda([WeakSelf](float DeltaSeconds)
					{
						const TSharedPtr<FScenarioSharedState, ESPMode::ThreadSafe> Self = WeakSelf.Pin();
						return Self.IsValid() && Self->Tick(DeltaSeconds);
					}));
				if (!TickerHandle.IsValid())
				{
					StoreReceipt(FinalizePieRuntimeScenario(
						Plan.CoreRequest,
						EUeAgentPieScenarioStepResult::Failed,
						*this));
				}
			}

			void ScheduleFailsafeCleanup()
			{
				{
					FScopeLock Lock(&Mutex);
					if (Phase == EScenarioDispatchPhase::Terminal) return;
					bStopRequested = true;
				}
				const TSharedRef<FScenarioSharedState, ESPMode::ThreadSafe> Self = AsShared();
				AsyncTask(ENamedThreads::GameThread, [Self]()
				{
					EScenarioDispatchPhase Phase;
					{
						FScopeLock Lock(&Self->Mutex);
						Phase = Self->Phase;
					}
					if (Phase == EScenarioDispatchPhase::Terminal) return;
					if (Phase == EScenarioDispatchPhase::Scheduled)
					{
						Self->SetTerminalNoSideEffects(
							TEXT("cancelled"),
							TEXT("pie_runtime_scenario_destroyed_before_resume"));
						return;
					}
					if (Self->TickerHandle.IsValid())
					{
						FTSTicker::GetCoreTicker().RemoveTicker(Self->TickerHandle);
						Self->TickerHandle.Reset();
					}
					FUeAgentPieScenarioReceipt Receipt = FinalizePieRuntimeScenario(
						Self->Plan.CoreRequest,
						EUeAgentPieScenarioStepResult::Cancelled,
						*Self);
					Self->CapturePausedObservations(Receipt);
					Self->StoreReceipt(Receipt);
				});
			}

			bool CapturePausedObservations(
				const FUeAgentPieScenarioReceipt& Receipt)
			{
				if (!Receipt.bStateKnown)
				{
					return false;
				}
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				FString Error;
				return Registry->TryResolveExact(
						Plan.CoreRequest.SessionIdentity,
						Receipt.PauseEpoch,
						Receipt.WorkspaceRevision,
						Snapshot,
						Error)
					&& Snapshot.World.IsValid()
					&& Snapshot.World->IsPaused()
					&& CaptureObservations(Snapshot);
			}

			bool ResolveInputActions(
				const FUeAgentPieRuntimeDebugSnapshot& Snapshot)
			{
				ULocalPlayer* LocalPlayer = Snapshot.LocalPlayer.Get();
				EnhancedInputSubsystem = LocalPlayer
					? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer)
					: nullptr;
				EnhancedPlayerInput = EnhancedInputSubsystem.IsValid()
					? EnhancedInputSubsystem->GetPlayerInput() : nullptr;
				for (FTimelineStep& Step : Plan.Steps)
				{
					if (Step.Type != ETimelineStepType::InputAction) continue;
					const UInputAction* Action = LoadObject<UInputAction>(nullptr, *Step.ActionRef);
					if (!Action || Action->GetPathName() != Step.ActionRef
						|| Action->ValueType != Step.ActionValueType
						|| !EnhancedInputSubsystem.IsValid()
						|| !EnhancedPlayerInput.IsValid()) return false;
					Step.Action = Action;
					ResolvedInputActions.Add(Step.ActionRef, Action);
					if (!InitialActionRefs.Contains(Action))
					{
						if (EnhancedPlayerInput->GetActionValue(Action).IsNonZero()) return false;
						InitialActionRefs.Add(Action);
					}
				}
				return true;
			}

			void ObserveInjectedActionValues()
			{
				if (!EnhancedPlayerInput.IsValid()) return;
				for (const TPair<FString, TWeakObjectPtr<const UInputAction>>& Pair
					: ResolvedInputActions)
				{
					InputActionLedger.RecordObservedValue(
						Pair.Key,
						Pair.Value.IsValid()
							&& EnhancedPlayerInput->GetActionValue(Pair.Value.Get()).IsNonZero());
				}
			}

			bool InjectActiveActionSamples()
			{
				if (!EnhancedInputSubsystem.IsValid())
					return InputActionLedger.GetActiveSamples().IsEmpty();
				for (const TPair<FString, FUeAgentPieInputActionSample>& Pair
					: InputActionLedger.GetActiveSamples())
				{
					const TWeakObjectPtr<const UInputAction>* Action =
						ResolvedInputActions.Find(Pair.Key);
					if (!Action || !Action->IsValid()) return false;
					EnhancedInputSubsystem->InjectInputForAction(
						Action->Get(),
						FInputActionValue(Pair.Value.ValueType, Pair.Value.Value), {}, {});
				}
				return true;
			}

			void BeginActionReleaseBarrier(const EUeAgentPieScenarioStepResult Outcome)
			{
				InputActionLedger.BeginRelease(FPlatformTime::Seconds(), GFrameCounter);
				bActionReleaseBarrier = true;
				PendingOutcome = Outcome;
			}

			bool Tick(float)
			{
				check(IsInGameThread());
				ObserveInjectedActionValues();
				EUeAgentPieScenarioStepResult Outcome = EUeAgentPieScenarioStepResult::Succeeded;
				bool bTerminal = false;
				if (bActionReleaseBarrier)
				{
					const EUeAgentPieInputActionReleaseStatus ReleaseStatus =
						InputActionLedger.EvaluateRelease(FPlatformTime::Seconds(), GFrameCounter);
					if (ReleaseStatus == EUeAgentPieInputActionReleaseStatus::Pending) return true;
					bActionReleaseVerified =
						ReleaseStatus == EUeAgentPieInputActionReleaseStatus::Verified;
					bTerminal = true;
					Outcome = PendingOutcome;
				}
				bool bStopWasRequested = false;
				{
					FScopeLock Lock(&Mutex);
					bStopWasRequested = bStopRequested;
				}
				const bool bCancelled = bStopWasRequested || Cancellation.IsCancelled();
				bTerminal = bTerminal || bCancelled;
				if (bCancelled && !bActionReleaseBarrier)
				{
					Outcome = EUeAgentPieScenarioStepResult::Cancelled;
				}
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				FString Error;
				if (!bTerminal && !Registry->TryResolveExact(
						Plan.CoreRequest.SessionIdentity,
						Plan.CoreRequest.ExpectedPauseEpoch,
						Plan.CoreRequest.ExpectedWorkspaceRevision,
						Snapshot,
						Error))
				{
					bTerminal = true;
					Outcome = EUeAgentPieScenarioStepResult::Failed;
				}
				const double WallElapsed = FPlatformTime::Seconds() - StartWallSeconds;
				const double GameElapsed = Snapshot.World.IsValid()
					? Snapshot.World->GetTimeSeconds() - StartGameSeconds
					: 0.0;
				const int64 FramesElapsed = static_cast<int64>(GFrameCounter - StartFrame);
				if (!bTerminal
					&& (WallElapsed > Plan.CoreRequest.WallTimeLimitSeconds
						|| GameElapsed > Plan.CoreRequest.GameTimeLimitSeconds
						|| FramesElapsed > Plan.CoreRequest.FrameLimit))
				{
					bTerminal = true;
					Outcome = EUeAgentPieScenarioStepResult::TimedOut;
				}
				while (!bTerminal
					&& NextStep < Plan.Steps.Num()
					&& Plan.Steps[NextStep].AtGameSeconds <= GameElapsed)
				{
					if (!ApplyStep(Plan.Steps[NextStep], Snapshot))
					{
						bTerminal = true;
						Outcome = EUeAgentPieScenarioStepResult::Failed;
						break;
					}
					++NextStep;
				}
				if (!bTerminal && !InjectActiveActionSamples())
				{
					bTerminal = true;
					Outcome = EUeAgentPieScenarioStepResult::Failed;
				}
				if (!bTerminal && NextStep == Plan.Steps.Num())
				{
					bTerminal = true;
					Outcome = EUeAgentPieScenarioStepResult::Succeeded;
				}
				if (!bTerminal) return true;
				if (!bActionReleaseBarrier && InputActionLedger.HasInjectedNonZero())
				{
					BeginActionReleaseBarrier(Outcome);
					return true;
				}

				TickerHandle.Reset();
				FUeAgentPieScenarioReceipt Receipt =
					FinalizePieRuntimeScenario(Plan.CoreRequest, Outcome, *this);
				if (Receipt.TerminalStatus == TEXT("failed") && !CaptureStepError.IsEmpty())
				{
					Receipt.ErrorCode = CaptureStepError;
				}
				if (!CapturePausedObservations(Receipt)
					&& Receipt.TerminalStatus == TEXT("succeeded"))
				{
					Receipt.TerminalStatus = TEXT("failed");
					Receipt.ErrorCode = TEXT("scenario_observation_failed");
				}
				if (Receipt.TerminalStatus == TEXT("succeeded") && StagedImage.IsSet())
				{
					FExecutionMutationError ArtifactError;
					FFileArtifactReference Published;
					FString EvidenceId;
					if (!Resources
						|| !ImageArtifactService->TryPublishVerified(
							OperationId,
							TEXT("pie-runtime-scenario:") + OperationId,
							StagedImage.GetValue(),
							*Resources,
							Published,
							EvidenceId,
							ArtifactError))
					{
						Receipt.TerminalStatus = TEXT("failed");
						Receipt.ErrorCode = ArtifactError.GetCode().IsEmpty()
							? TEXT("pie_runtime_capture_publish_failed")
							: ArtifactError.GetCode();
					}
					else
					{
						Artifact = MoveTemp(Published);
						ArtifactEvidenceId = MoveTemp(EvidenceId);
					}
				}
				if (Receipt.TerminalStatus != TEXT("succeeded") && StagedImage.IsSet())
				{
					ImageArtifactService->DiscardStaged(StagedImage.GetValue());
				}
				StoreReceipt(Receipt);
				return false;
			}

			bool ApplyStep(
				const FTimelineStep& Step,
				const FUeAgentPieRuntimeDebugSnapshot& Snapshot)
			{
				if (Step.Type == ETimelineStepType::Wait) return true;
				if (Step.Type == ETimelineStepType::Capture)
				{
					CaptureStepError.Reset();
					TArray<FColor> Pixels;
					FIntPoint Size = FIntPoint::ZeroValue;
					FString SurfaceIdentity;
					FString ReadError;
					if (!Resources)
					{
						CaptureStepError = TEXT("pie_runtime_capture_resources_unavailable");
						return false;
					}
					bool bRead = false;
					if (Step.CaptureSpec.Source == EUeAgentPieCaptureSource::OutputSurface)
					{
						bRead = Registry->TryReadExactOutputSurface(
							Plan.CoreRequest.SessionIdentity,
							Plan.CoreRequest.ExpectedPauseEpoch,
							Plan.CoreRequest.ExpectedWorkspaceRevision,
							Pixels, Size, SurfaceIdentity, ReadError);
					}
					else
					{
						FUeAgentPieCameraReadRequest CameraRequest;
						CameraRequest.Source = Step.CaptureSpec.Source == EUeAgentPieCaptureSource::PlayerCamera
							? EUeAgentPieCameraReadSource::PlayerCamera
							: EUeAgentPieCameraReadSource::SceneCamera;
						CameraRequest.CameraRef = Step.CaptureSpec.CameraRef;
						CameraRequest.TransientIdentity = OperationId;
						CameraRequest.OverrideLocation = Step.CaptureSpec.OverrideLocation;
						CameraRequest.OverrideRotation = Step.CaptureSpec.OverrideRotation;
						CameraRequest.OverrideFovDegrees = Step.CaptureSpec.OverrideFovDegrees;
						bRead = Registry->TryReadExactCamera(
							Plan.CoreRequest.SessionIdentity,
							Plan.CoreRequest.ExpectedPauseEpoch,
							Plan.CoreRequest.ExpectedWorkspaceRevision,
							CameraRequest, Pixels, Size, SurfaceIdentity, ReadError);
					}
					if (!bRead)
					{
						CaptureStepError = ResolvePieCaptureFailureCode(
							ReadError, TEXT("pie_runtime_capture_read_failed"));
						return false;
					}
					FExecutionMutationError StageError;
					CaptureMediaLongJob::FUeAgentStagedCaptureImage Image;
					if (!ImageArtifactService->TryStagePngPixels(
							OperationId, SurfaceIdentity, Pixels, Size,
							*Resources, Image, StageError))
					{
						CaptureStepError = ResolvePieCaptureFailureCode(
							StageError.GetCode(), TEXT("pie_runtime_capture_stage_failed"));
						return false;
					}
					StagedImage = MoveTemp(Image);
					CaptureSourceRef = SurfaceIdentity;
					return true;
				}
				if (Step.Type == ETimelineStepType::InputAction)
				{
					if (!Step.Action.IsValid()) return false;
					InputActionLedger.ApplySample({
						Step.ActionRef,
						Step.AtGameSeconds,
						Step.ActionValueType,
						Step.ActionValue});
					return true;
				}
				if (Step.Type == ETimelineStepType::InputKey)
				{
					APlayerController* Controller = Snapshot.PlayerController.Get();
					if (!Controller || !Controller->InputKey(
							FInputKeyEventArgs::CreateSimulated(
								Step.Key,
								Step.InputEvent,
								static_cast<float>(Step.Amount))))
					{
						return false;
					}
					InjectedInput.RecordSuccessfulInjection(
						Step.Key, Step.InputEvent, Step.Amount);
					return true;
				}
				UWorld* World = Snapshot.World.Get();
				AActor* Actor = FindObject<AActor>(nullptr, *Step.ActorRef);
				if (!World || !IsValid(Actor) || Actor->GetWorld() != World)
				{
					return false;
				}
				FTransform Transform = Actor->GetActorTransform();
				if (Step.Transform.Location.IsSet()) Transform.SetLocation(Step.Transform.Location.GetValue());
				if (Step.Transform.Rotation.IsSet()) Transform.SetRotation(Step.Transform.Rotation.GetValue().Quaternion());
				if (Step.Transform.Scale.IsSet()) Transform.SetScale3D(Step.Transform.Scale.GetValue());
				return Actor->SetActorTransform(
					Transform, false, nullptr, ETeleportType::TeleportPhysics);
			}

			bool CaptureObservations(const FUeAgentPieRuntimeDebugSnapshot& Snapshot)
			{
				FScopeLock Lock(&Mutex);
				ObservedValues.Reset();
				bool bSucceeded = true;
				for (const FObservationField& Field : Plan.ObservationFields)
				{
					UObject* Object = FindObject<UObject>(nullptr, *Field.ObjectRef);
					if (!IsValid(Object) || Object->GetWorld() != Snapshot.World.Get())
					{
						ObservedValues.Emplace(Field.ObjectRef + TEXT(".") + Field.Property, TEXT("<unavailable>"));
						bSucceeded = false;
						continue;
					}
					FString Text;
					if (const AActor* Actor = Cast<AActor>(Object))
					{
						if (Field.Property == TEXT("actor.transform")) Text = Actor->GetActorTransform().ToHumanReadableString();
						else if (Field.Property == TEXT("actor.location")) Text = Actor->GetActorLocation().ToString();
						else if (Field.Property == TEXT("actor.rotation")) Text = Actor->GetActorRotation().ToString();
						else if (Field.Property == TEXT("actor.scale")) Text = Actor->GetActorScale3D().ToString();
						else if (Field.Property == TEXT("actor.hidden")) Text = Actor->IsHidden() ? TEXT("true") : TEXT("false");
						else bSucceeded = false;
					}
					else if (const UActorComponent* Component = Cast<UActorComponent>(Object))
					{
						if (Field.Property == TEXT("component.active")) Text = Component->IsActive() ? TEXT("true") : TEXT("false");
						else if (Field.Property == TEXT("component.visible"))
						{
							const USceneComponent* Scene = Cast<USceneComponent>(Component);
							if (Scene) Text = Scene->IsVisible() ? TEXT("true") : TEXT("false");
							else bSucceeded = false;
						}
						else bSucceeded = false;
					}
					else bSucceeded = false;
					if (!bSucceeded && Text.IsEmpty()) Text = TEXT("<unsupported>");
					ObservedValues.Emplace(Field.ObjectRef + TEXT(".") + Field.Property, MoveTemp(Text));
				}
				return bSucceeded;
			}

			FScenarioPlan Plan;
			TSharedRef<FUeAgentPieRuntimeDebugRegistry,
				ESPMode::ThreadSafe> Registry;
			TSharedRef<CaptureMediaLongJob::IUeAgentCaptureImageArtifactService,
				ESPMode::ThreadSafe> ImageArtifactService;
			FString OperationId;
			FLongRunningWorkIdentity Identity;
			FUeAgentCancellationToken Cancellation;
			FCriticalSection Mutex;
			FTSTicker::FDelegateHandle TickerHandle;
			FUeAgentPieInjectedInputLedger InjectedInput;
			TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> EnhancedInputSubsystem;
			TWeakObjectPtr<UEnhancedPlayerInput> EnhancedPlayerInput;
			TSet<const UInputAction*> InitialActionRefs;
			TMap<FString, TWeakObjectPtr<const UInputAction>> ResolvedInputActions;
			FUeAgentPieInputActionOperationLedger InputActionLedger;
			TArray<TPair<FString, FString>> ObservedValues;
			FUeAgentResourceTracker* Resources = nullptr;
			TOptional<CaptureMediaLongJob::FUeAgentStagedCaptureImage> StagedImage;
			TOptional<FFileArtifactReference> Artifact;
			FString ArtifactEvidenceId;
			FString CaptureSourceRef;
			FString CaptureStepError;
			FString TerminalStatus;
			FString ErrorCode;
			FString WorkspacePath;
			int64 ObservedPauseEpoch = 0;
			int64 ObservedWorkspaceRevision = 0;
			int64 SignalRevision = 1;
			ELongRunningWorkState WorkState = ELongRunningWorkState::Running;
			EScenarioDispatchPhase Phase = EScenarioDispatchPhase::Scheduled;
			double StartWallSeconds = 0.0;
			double StartGameSeconds = 0.0;
			uint64 StartFrame = 0;
			int32 NextStep = 0;
			bool bStopRequested = false;
			bool bDispatchStopped = false;
			bool bKeysReleased = false;
			bool bActionReleaseBarrier = false;
			bool bActionReleaseVerified = false;
			EUeAgentPieScenarioStepResult PendingOutcome =
				EUeAgentPieScenarioStepResult::Failed;
		};

		class FScenarioOperationPort final : public ILongRunningOperationPort
		{
		public:
			FScenarioOperationPort(
				FScenarioPlan InPlan,
				TSharedRef<FUeAgentPieRuntimeDebugRegistry,
					ESPMode::ThreadSafe> InRegistry,
				TSharedRef<CaptureMediaLongJob::IUeAgentCaptureImageArtifactService,
					ESPMode::ThreadSafe> InImageArtifactService)
				: Plan(MoveTemp(InPlan)), Registry(InRegistry),
				  ImageArtifactService(InImageArtifactService)
			{
			}

			~FScenarioOperationPort() override
			{
				FScopeLock PortLock(&PortMutex);
				if (State.IsValid()) State->ScheduleFailsafeCleanup();
			}

			bool Start(
				const FLongRunningOperationRequest& Request,
				const FUeAgentCancellationToken& Cancellation,
				FLongRunningWorkIdentity& OutIdentity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock PortLock(&PortMutex);
				if (State.IsValid()) return Fail(OutError, TEXT("pie_runtime_scenario_already_started"));
				OutIdentity = FLongRunningWorkIdentity(
					TEXT("native:pie-runtime-scenario:") + Request.GetOperationId(), 1);
				State = MakeShared<FScenarioSharedState, ESPMode::ThreadSafe>(
					Plan, Registry, ImageArtifactService,
					Request.GetOperationId(), OutIdentity, Cancellation);
				State->Resources = &Request.GetResources();
				const TSharedRef<FScenarioSharedState, ESPMode::ThreadSafe> Scheduled =
					State.ToSharedRef();
				AsyncTask(ENamedThreads::GameThread, [Scheduled]()
				{
					Scheduled->BeginOnGameThread();
				});
				return true;
			}

			bool Observe(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FUeAgentResourceTracker& Resources,
				FLongRunningWorkObservation& OutObservation,
				FExecutionMutationError& OutError) override
			{
				FScopeLock PortLock(&PortMutex);
				if (!Matches(OperationId, Identity)) return Fail(OutError, TEXT("pie_runtime_scenario_identity_mismatch"));
				FScopeLock Lock(&State->Mutex);
				TOptional<FLongRunningOperationSignal> Signal;
				if (State->SignalRevision > LastEmittedSignalRevision
					&& !TryMakeObservationSignal(
						State->ObservedValues,
						State->TerminalStatus,
						State->ErrorCode,
						State->Plan.CoreRequest.SessionIdentity,
						State->ObservedPauseEpoch,
						State->ObservedWorkspaceRevision,
						State->WorkspacePath,
						State->Artifact,
						State->CaptureSourceRef,
						State->ArtifactEvidenceId,
						State->SignalRevision,
						Resources,
						Signal))
				{
					return Fail(OutError, TEXT("pie_runtime_scenario_observation_signal_failed"));
				}
				if (Signal.IsSet())
				{
					LastEmittedSignalRevision = State->SignalRevision;
				}
				OutObservation = FLongRunningWorkObservation(
					State->Identity.GetGeneration(),
					State->WorkState,
					State->ErrorCode,
					MoveTemp(Signal));
				return true;
			}

			bool RequestStop(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				FScopeLock PortLock(&PortMutex);
				(void)Reason;
				if (!Matches(OperationId, Identity)) return Fail(OutError, TEXT("pie_runtime_scenario_identity_mismatch"));
				FScopeLock Lock(&State->Mutex);
				State->bStopRequested = true;
				return true;
			}

			bool Join(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock PortLock(&PortMutex);
				if (!Matches(OperationId, Identity)) return Fail(OutError, TEXT("pie_runtime_scenario_identity_mismatch"));
				FScopeLock Lock(&State->Mutex);
				return State->WorkState != ELongRunningWorkState::Running
					|| Fail(OutError, TEXT("pie_runtime_scenario_join_pending"));
			}

			bool Release(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock PortLock(&PortMutex);
				if (!Matches(OperationId, Identity)) return Fail(OutError, TEXT("pie_runtime_scenario_identity_mismatch"));
				{
					FScopeLock Lock(&State->Mutex);
					if (State->WorkState == ELongRunningWorkState::Running)
						return Fail(OutError, TEXT("pie_runtime_scenario_release_before_terminal"));
				}
				State.Reset();
				return true;
			}

		private:
			bool Matches(const FString& OperationId, const FLongRunningWorkIdentity& Identity) const
			{
				return State.IsValid()
					&& State->OperationId == OperationId
					&& State->Identity == Identity;
			}

			FScenarioPlan Plan;
			TSharedRef<FUeAgentPieRuntimeDebugRegistry,
				ESPMode::ThreadSafe> Registry;
			TSharedRef<CaptureMediaLongJob::IUeAgentCaptureImageArtifactService,
				ESPMode::ThreadSafe> ImageArtifactService;
			FCriticalSection PortMutex;
			TSharedPtr<FScenarioSharedState, ESPMode::ThreadSafe> State;
			int64 LastEmittedSignalRevision = 0;
		};
	}

	FUeAgentPieRuntimeScenarioPortFactory::FUeAgentPieRuntimeScenarioPortFactory(
		TSharedRef<FUeAgentPieRuntimeDebugRegistry,
			ESPMode::ThreadSafe> InRegistry,
		TSharedRef<CaptureMediaLongJob::IUeAgentCaptureImageArtifactService,
			ESPMode::ThreadSafe> InImageArtifactService)
		: Registry(InRegistry), ImageArtifactService(InImageArtifactService)
	{
	}

	bool FUeAgentPieRuntimeScenarioPortFactory::TryCreateOperationPort(
		const FUeAgentPieRuntimeScenarioTypedRequest& Request,
		const FLongRunningOperationRequest& OperationRequest,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		(void)OperationRequest;
		FScenarioPlan Plan;
		if (!TryParsePlan(Request, Plan, OutError)) return false;
		OutPort = MakeShared<FScenarioOperationPort, ESPMode::ThreadSafe>(
			MoveTemp(Plan), Registry, ImageArtifactService);
		return true;
	}
}
