// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioProduction.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Core/EditorCapture/UeAgentAsyncCapture.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDiagnostics.h"
#include "Utilities/UeAgentEditorCPUThrottleScope.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugRegistry.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeInputState.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioCore.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageArtifactService.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Async/Async.h"
#include "Camera/CameraTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
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
	using namespace PieDiagnostics;

	namespace
	{
		FString PointerToken(const FString& Value)
		{
			return Value.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		Contract::FDiagnostic CanonicalInput(const FString& Code, const FString& Reason,
			const FString& Pointer, const FUeAgentCanonicalValueRef* Value, const FString& Expected)
		{
			return Input(Code, Reason, TEXT("pie_scenario"), Pointer,
				CanonicalValueFact(Value), MakeShared<FJsonValueString>(Expected));
		}

		FString StepPointer(int32 Index)
		{
			return FString::Printf(TEXT("/parameters/steps/%d"), Index);
		}

		Contract::FDiagnostic StepInput(const FString& Code, const FString& Reason, int32 Index,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = Input(Code, Reason, TEXT("pie_scenario"), StepPointer(Index) + Field, Actual, Expected);
			Error.OperationIndex = Index;
			return Error;
		}
		void PrefixInputFailure(Contract::FDiagnostic& Error, const FString& Prefix)
		{
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Prefix, false, true);
			const auto MarkInput = [](auto&& Self, Contract::FDiagnostic& Item) -> void
			{
				if (Item.SourceKind.IsEmpty() && Item.Source.SourceFile.IsEmpty())
				{
					Item.SourceKind = TEXT("command_input");
					Item.Source.bCommandInput = true;
					if (Item.SemanticPointer.IsEmpty()) Item.SemanticPointer = Item.Source.JsonPath;
				}
				for (auto& Child : Item.Diagnostics) Self(Self, Child);
				if (Item.PublicCause)
				{
					auto Cause = MakeShared<Contract::FDiagnostic>(*Item.PublicCause);
					Self(Self, *Cause);
					Item.PublicCause = MoveTemp(Cause);
				}
			};
			MarkInput(MarkInput, Error);
		}

		FString InputActionTypeName(EInputActionValueType Type)
		{
			switch (Type)
			{
			case EInputActionValueType::Boolean: return TEXT("boolean");
			case EInputActionValueType::Axis1D: return TEXT("scalar");
			case EInputActionValueType::Axis2D: return TEXT("vector2");
			case EInputActionValueType::Axis3D: return TEXT("vector3");
			default: return FString::Printf(TEXT("unknown(%d)"), static_cast<int32>(Type));
			}
		}

		TSharedPtr<FJsonValue> VectorFact(const FVector& Value)
		{
			return Facts({ { TEXT("x"), Number(Value.X) }, { TEXT("y"), Number(Value.Y) },
				{ TEXT("z"), Number(Value.Z) } });
		}

		TSharedPtr<FJsonValue> QuaternionFact(const FQuat& Value)
		{
			return Facts({ { TEXT("x"), Number(Value.X) }, { TEXT("y"), Number(Value.Y) },
				{ TEXT("z"), Number(Value.Z) }, { TEXT("w"), Number(Value.W) } });
		}

		bool TryVerifyActorTransform(const FTransform& Expected, const FTransform& Actual,
			bool bCheckLocation, bool bCheckRotation, bool bCheckScale, Contract::FDiagnostic& OutError)
		{
			const TCHAR* Field = nullptr;
			TSharedPtr<FJsonValue> ExpectedValue;
			TSharedPtr<FJsonValue> ActualValue;
			if (bCheckLocation && !Expected.GetLocation().Equals(Actual.GetLocation(), KINDA_SMALL_NUMBER))
			{
				Field = TEXT("transform/location_cm");
				ExpectedValue = VectorFact(Expected.GetLocation());
				ActualValue = VectorFact(Actual.GetLocation());
			}
			else if (bCheckRotation && !Expected.GetRotation().Equals(Actual.GetRotation(), KINDA_SMALL_NUMBER))
			{
				Field = TEXT("transform/rotation_quaternion");
				ExpectedValue = QuaternionFact(Expected.GetRotation());
				ActualValue = QuaternionFact(Actual.GetRotation());
			}
			else if (bCheckScale && !Expected.GetScale3D().Equals(Actual.GetScale3D(), KINDA_SMALL_NUMBER))
			{
				Field = TEXT("transform/scale");
				ExpectedValue = VectorFact(Expected.GetScale3D());
				ActualValue = VectorFact(Actual.GetScale3D());
			}
			if (!Field) return true;
			OutError = Native(TEXT("pie_runtime_actor_transform_readback_mismatch"),
				TEXT("The actor transform readback differs from the requested transform."),
				TEXT("pie_transform_observation"), Field, ActualValue, ExpectedValue);
			return false;
		}

	}

	bool DoesPieActorTransformReadbackMatch(
		const FTransform& Expected,
		const FTransform& Actual,
		const bool bCheckLocation,
		const bool bCheckRotation,
		const bool bCheckScale)
	{
		Contract::FDiagnostic Error;
		return TryVerifyActorTransform(Expected, Actual, bCheckLocation, bCheckRotation, bCheckScale, Error);
	}

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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutSample = {};
		OutError.Reset();
		const auto Reject = [&OutError](const FString& Code, const FString& Reason, const FString& Pointer,
			const FUeAgentCanonicalValueRef* Value, const FString& Expected)
		{
			OutError = CanonicalInput(Code, Reason, Pointer, Value, Expected);
			OutError.Stage = TEXT("input_decode");
			return false;
		};
		if (StepValue->GetKind() != EUeAgentValueKind::Object)
		{
			return Reject(TEXT("pie_runtime_scenario_input_action_invalid"), TEXT("An input action sample must be an object."),
				TEXT(""), &StepValue, TEXT("object"));
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
				OutError = Input(TEXT("pie_runtime_scenario_step_field_forbidden"), TEXT("This field is not supported by an input action sample."),
					TEXT("input_decode"), TEXT("/") + PointerToken(Item.Key), MakeShared<FJsonValueString>(Item.Key),
					MakeShared<FJsonValueString>(TEXT("type, at_seconds, action_ref, or value")));
				return false;
			}
		}
		const FUeAgentCanonicalValueRef* Type = Find(TEXT("type"));
		const FUeAgentCanonicalValueRef* At = Find(TEXT("at_seconds"));
		const FUeAgentCanonicalValueRef* Action = Find(TEXT("action_ref"));
		const FUeAgentCanonicalValueRef* Value = Find(TEXT("value"));
		if (!Type || (*Type)->GetKind() != EUeAgentValueKind::String || (*Type)->GetText() != TEXT("input_action"))
			return Reject(TEXT("pie_runtime_scenario_input_action_invalid"), TEXT("The input action sample type must be input_action."),
				TEXT("/type"), Type, TEXT("input_action"));
		if ((At && (((*At)->GetKind() != EUeAgentValueKind::Integer && (*At)->GetKind() != EUeAgentValueKind::Decimal)
				|| !LexTryParseString(OutSample.AtSeconds, *(*At)->GetLexeme())))
			|| !FMath::IsFinite(OutSample.AtSeconds) || OutSample.AtSeconds < 0.0)
			return Reject(TEXT("pie_runtime_scenario_input_action_invalid"), TEXT("The input action time must be finite and nonnegative."),
				TEXT("/at_seconds"), At, TEXT("finite number >= 0"));
		if (!Action || (*Action)->GetKind() != EUeAgentValueKind::String)
			return Reject(TEXT("pie_runtime_scenario_input_action_invalid"), TEXT("The input action reference must be a string."),
				TEXT("/action_ref"), Action, TEXT("nonempty input action asset reference"));
		OutSample.ActionRef = (*Action)->GetText();
		if (OutSample.ActionRef.IsEmpty())
			return Reject(TEXT("pie_runtime_scenario_input_action_invalid"), TEXT("The input action reference cannot be empty."),
				TEXT("/action_ref"), Action, TEXT("nonempty input action asset reference"));
		if (!Value)
			return Reject(TEXT("pie_runtime_scenario_input_action_invalid"), TEXT("The input action sample requires a value."),
				TEXT("/value"), nullptr, TEXT("boolean, finite scalar, or two/three finite components"));
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
				return Reject(TEXT("pie_runtime_scenario_input_action_invalid"), TEXT("The input action scalar value must be finite."),
					TEXT("/value"), Value, TEXT("finite number"));
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
					return Reject(TEXT("pie_runtime_scenario_input_action_invalid"), TEXT("The input action vector component must be a finite number."),
						FString::Printf(TEXT("/value/%d"), Index), &Item, TEXT("finite number"));
				}
				OutSample.Value[Index] = Number;
			}
			OutSample.ValueType = (*Value)->GetArrayItems().Num() == 2
				? EInputActionValueType::Axis2D : EInputActionValueType::Axis3D;
		}
		else
		{
			return Reject(TEXT("pie_runtime_scenario_input_action_invalid"), TEXT("The input action value has an unsupported type or vector length."),
				TEXT("/value"), Value, TEXT("boolean, finite scalar, or array of two/three finite numbers"));
		}
		(void)ExistingSamples;
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
		if (Frame <= ReleaseStartFrame) return EUeAgentPieInputActionReleaseStatus::Pending;
		bool bReleased = true;
		for (const FString& ActionRef : InjectedNonZero)
		{
			const bool* Current = LatestObservedNonZero.Find(ActionRef);
			if (!Current || *Current)
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
		UE::UAI::Contract::FDiagnostic& OutError,
		int32 OperationIndex)
	{
		OutSpec = {};
		OutError.Reset();
		const auto Reject = [&OutError, OperationIndex](const TCHAR* Code, const FString& FieldPath,
			const TCHAR* Detail, const FUeAgentCanonicalValueRef* Actual, const TCHAR* Expected)
		{
			const FString OperationPath = OperationIndex == INDEX_NONE
				? FString() : FString::Printf(TEXT("/steps/%d"), OperationIndex);
			OutError = CanonicalInput(Code, Detail, OperationPath + FieldPath, Actual, Expected);
			OutError.Keyword = TEXT("format");
			if (OperationIndex != INDEX_NONE) OutError.OperationIndex = OperationIndex;
			return false;
		};
		if (StepValue->GetKind() != EUeAgentValueKind::Object)
			return Reject(TEXT("pie_runtime_scenario_capture_invalid"), TEXT(""), TEXT("A capture operation must be an object."),
				&StepValue, TEXT("object"));
		const auto Find = [&StepValue](const FString& Name) -> const FUeAgentCanonicalValueRef*
		{
			for (const auto& Item : StepValue->GetObjectItems()) if (Item.Key == Name) return &Item.Value;
			return nullptr;
		};
		for (const auto& Item : StepValue->GetObjectItems())
		{
			if (Item.Key != TEXT("type") && Item.Key != TEXT("at_seconds")
				&& Item.Key != TEXT("source") && Item.Key != TEXT("format")
				&& Item.Key != TEXT("camera") && Item.Key != TEXT("view_transform")
				&& Item.Key != TEXT("fov_degrees") && Item.Key != TEXT("name") && Item.Key != TEXT("interval"))
				return Reject(TEXT("pie_runtime_scenario_step_field_forbidden"), TEXT("/") + PointerToken(Item.Key),
					TEXT("This field is not supported by a capture operation."), &Item.Value,
					TEXT("type, at_seconds, source, format, camera, view_transform, fov_degrees, name, or interval"));
		}
		const FUeAgentCanonicalValueRef* Type = Find(TEXT("type"));
		const FUeAgentCanonicalValueRef* AtSeconds = Find(TEXT("at_seconds"));
		const FUeAgentCanonicalValueRef* Source = Find(TEXT("source"));
		const FUeAgentCanonicalValueRef* Format = Find(TEXT("format"));
		double ParsedAtSeconds = 0.0;
		if (!Type || (*Type)->GetKind() != EUeAgentValueKind::String
			|| ((*Type)->GetText() != TEXT("capture") && (*Type)->GetText() != TEXT("capture_start")))
			return Reject(TEXT("pie_runtime_scenario_capture_invalid"), TEXT("/type"), TEXT("The capture operation type is unsupported."),
				Type, TEXT("capture or capture_start"));
		if ((AtSeconds && (((*AtSeconds)->GetKind() != EUeAgentValueKind::Integer
				&& (*AtSeconds)->GetKind() != EUeAgentValueKind::Decimal)
				|| !LexTryParseString(ParsedAtSeconds, *(*AtSeconds)->GetLexeme())))
			|| !FMath::IsFinite(ParsedAtSeconds) || ParsedAtSeconds < 0.0)
			return Reject(TEXT("pie_runtime_scenario_capture_invalid"), TEXT("/at_seconds"), TEXT("The capture time must be finite and nonnegative."),
				AtSeconds, TEXT("finite number >= 0"));
		if (!Source || (*Source)->GetKind() != EUeAgentValueKind::String)
			return Reject(TEXT("pie_runtime_scenario_capture_invalid"), TEXT("/source"), TEXT("The capture source must be a string."),
				Source, TEXT("pie_output_surface, pie_player_camera, or scene_camera"));
		if (!Format || (*Format)->GetKind() != EUeAgentValueKind::String || (*Format)->GetText() != TEXT("png"))
			return Reject(TEXT("pie_runtime_scenario_capture_invalid"), TEXT("/format"), TEXT("PIE captures require PNG format."),
				Format, TEXT("png"));
		const FString SourceText = (*Source)->GetText();
		const FUeAgentCanonicalValueRef* CameraRef = Find(TEXT("camera"));
		const FUeAgentCanonicalValueRef* ViewTransform = Find(TEXT("view_transform"));
		const FUeAgentCanonicalValueRef* FovDegrees = Find(TEXT("fov_degrees"));
		if (SourceText == TEXT("pie_output_surface"))
		{
			if (CameraRef || ViewTransform || FovDegrees)
				return Reject(TEXT("pie_runtime_scenario_capture_fields_forbidden"),
					CameraRef ? TEXT("/camera") : ViewTransform ? TEXT("/view_transform") : TEXT("/fov_degrees"),
					TEXT("The PIE output surface does not accept camera or view overrides."),
					CameraRef ? CameraRef : ViewTransform ? ViewTransform : FovDegrees, TEXT("field omitted"));
			OutSpec.Source = EUeAgentPieCaptureSource::OutputSurface;
			return true;
		}
		if (SourceText == TEXT("pie_player_camera"))
		{
			if (CameraRef || ViewTransform || FovDegrees)
				return Reject(TEXT("pie_runtime_scenario_capture_fields_forbidden"),
					CameraRef ? TEXT("/camera") : ViewTransform ? TEXT("/view_transform") : TEXT("/fov_degrees"),
					TEXT("The evaluated player camera does not accept camera or view overrides."),
					CameraRef ? CameraRef : ViewTransform ? ViewTransform : FovDegrees, TEXT("field omitted"));
			OutSpec.Source = EUeAgentPieCaptureSource::PlayerCamera;
			return true;
		}
		if (SourceText != TEXT("scene_camera"))
			return Reject(TEXT("pie_runtime_scenario_capture_source_invalid"), TEXT("/source"), TEXT("The capture source is unsupported."),
				Source, TEXT("pie_output_surface, pie_player_camera, or scene_camera"));
		if (CameraRef && ((*CameraRef)->GetKind() != EUeAgentValueKind::String || (*CameraRef)->GetText().IsEmpty()))
			return Reject(TEXT("pie_runtime_scenario_capture_source_invalid"), TEXT("/camera"), TEXT("A scene camera alias must be a nonempty string."),
				CameraRef, TEXT("nonempty camera alias"));
		OutSpec.Source = EUeAgentPieCaptureSource::SceneCamera;
		if (CameraRef) OutSpec.CameraRef = (*CameraRef)->GetText();
		if (FovDegrees)
		{
			double Fov = 0.0;
			if (((*FovDegrees)->GetKind() != EUeAgentValueKind::Integer && (*FovDegrees)->GetKind() != EUeAgentValueKind::Decimal)
				|| !LexTryParseString(Fov, *(*FovDegrees)->GetLexeme()) || !FMath::IsFinite(Fov) || Fov < 1.0 || Fov > 179.0)
				return Reject(TEXT("pie_runtime_scenario_capture_fov_invalid"), TEXT("/fov_degrees"), TEXT("The camera field of view must be between 1 and 179 degrees."),
					FovDegrees, TEXT("finite number in [1,179]"));
			OutSpec.OverrideFovDegrees = static_cast<float>(Fov);
		}
		if (!ViewTransform)
		{
			if (OutSpec.CameraRef.IsEmpty())
				return Reject(TEXT("pie_runtime_scenario_capture_transient_camera_incomplete"), TEXT("/view_transform"),
					TEXT("A scene camera without an alias requires a view transform and field of view."), nullptr,
					TEXT("view_transform with location and rotation, plus fov_degrees"));
			return true;
		}
		if ((*ViewTransform)->GetKind() != EUeAgentValueKind::Object)
			return Reject(TEXT("pie_runtime_scenario_capture_view_transform_invalid"), TEXT("/view_transform"), TEXT("The capture view transform must be an object."),
				ViewTransform, TEXT("object"));
		const auto FindTransform = [ViewTransform](const FString& Name) -> const FUeAgentCanonicalValueRef*
		{
			for (const auto& Item : (*ViewTransform)->GetObjectItems()) if (Item.Key == Name) return &Item.Value;
			return nullptr;
		};
		for (const auto& Item : (*ViewTransform)->GetObjectItems())
		{
			if (Item.Key != TEXT("frame") && Item.Key != TEXT("dimension")
				&& Item.Key != TEXT("unit") && Item.Key != TEXT("rotation_format")
				&& Item.Key != TEXT("location") && Item.Key != TEXT("rotation"))
				return Reject(TEXT("pie_runtime_scenario_capture_view_transform_field_forbidden"),
					TEXT("/view_transform/") + PointerToken(Item.Key), TEXT("This field is not supported by the capture view transform."),
					&Item.Value, TEXT("frame, dimension, unit, rotation_format, location, or rotation"));
		}
		const FUeAgentCanonicalValueRef* Frame = FindTransform(TEXT("frame"));
		const FUeAgentCanonicalValueRef* Dimension = FindTransform(TEXT("dimension"));
		const FUeAgentCanonicalValueRef* Unit = FindTransform(TEXT("unit"));
		const FUeAgentCanonicalValueRef* RotationFormat = FindTransform(TEXT("rotation_format"));
		const FUeAgentCanonicalValueRef* Location = FindTransform(TEXT("location"));
		const FUeAgentCanonicalValueRef* Rotation = FindTransform(TEXT("rotation"));
		int64 DimensionValue = 0;
		if (!Frame || (*Frame)->GetKind() != EUeAgentValueKind::String || (*Frame)->GetText() != TEXT("ue_world"))
			return Reject(TEXT("pie_runtime_scenario_capture_view_transform_invalid"), TEXT("/view_transform/frame"), TEXT("Capture view transforms require the UE world frame."),
				Frame, TEXT("ue_world"));
		if (!Dimension || (*Dimension)->GetKind() != EUeAgentValueKind::Integer
			|| !LexTryParseString(DimensionValue, *(*Dimension)->GetLexeme()) || DimensionValue != 3)
			return Reject(TEXT("pie_runtime_scenario_capture_view_transform_invalid"), TEXT("/view_transform/dimension"), TEXT("Capture view transforms require integer dimension 3."),
				Dimension, TEXT("integer 3"));
		if (!Unit || (*Unit)->GetKind() != EUeAgentValueKind::String || (*Unit)->GetText() != TEXT("centimeter"))
			return Reject(TEXT("pie_runtime_scenario_capture_view_transform_invalid"), TEXT("/view_transform/unit"), TEXT("Capture view locations use centimeters."),
				Unit, TEXT("centimeter"));
		if (!RotationFormat || (*RotationFormat)->GetKind() != EUeAgentValueKind::String || (*RotationFormat)->GetText() != TEXT("pitch_yaw_roll_degrees"))
			return Reject(TEXT("pie_runtime_scenario_capture_view_transform_invalid"), TEXT("/view_transform/rotation_format"), TEXT("Capture view rotations use pitch, yaw, and roll in degrees."),
				RotationFormat, TEXT("pitch_yaw_roll_degrees"));
		if (!Location && !Rotation)
			return Reject(TEXT("pie_runtime_scenario_capture_view_transform_invalid"), TEXT("/view_transform"),
				TEXT("The capture view transform must specify location or rotation."), ViewTransform, TEXT("location or rotation"));
		const auto ParseVector = [&Reject](const FUeAgentCanonicalValueRef* Value, const FString& Field, FVector& Out)
		{
			if (!Value || (*Value)->GetKind() != EUeAgentValueKind::Array || (*Value)->GetArrayItems().Num() != 3)
				return Reject(TEXT("pie_runtime_scenario_capture_view_transform_invalid"), Field,
					TEXT("The capture view component must contain exactly three finite numbers."), Value, TEXT("array of three finite numbers"));
			for (int32 Index = 0; Index < 3; ++Index)
			{
				const auto& Item = (*Value)->GetArrayItems()[Index];
				double Parsed = 0.0;
				if ((Item->GetKind() != EUeAgentValueKind::Integer && Item->GetKind() != EUeAgentValueKind::Decimal)
					|| !LexTryParseString(Parsed, *Item->GetLexeme()) || !FMath::IsFinite(Parsed))
					return Reject(TEXT("pie_runtime_scenario_capture_view_transform_invalid"), Field + FString::Printf(TEXT("/%d"), Index),
						TEXT("The capture view coordinate must be a finite number."), &Item, TEXT("finite number"));
				Out[Index] = Parsed;
			}
			return true;
		};
		FVector Vector;
		if (Location)
		{
			if (!ParseVector(Location, TEXT("/view_transform/location"), Vector)) return false;
			OutSpec.OverrideLocation = Vector;
		}
		if (Rotation)
		{
			if (!ParseVector(Rotation, TEXT("/view_transform/rotation"), Vector)) return false;
			OutSpec.OverrideRotation = FRotator(Vector.X, Vector.Y, Vector.Z);
		}
		if (OutSpec.CameraRef.IsEmpty()
			&& (!OutSpec.OverrideLocation.IsSet() || !OutSpec.OverrideRotation.IsSet() || !OutSpec.OverrideFovDegrees.IsSet()))
			return Reject(TEXT("pie_runtime_scenario_capture_transient_camera_incomplete"),
				!OutSpec.OverrideLocation.IsSet() ? TEXT("/view_transform/location") : !OutSpec.OverrideRotation.IsSet() ? TEXT("/view_transform/rotation") : TEXT("/fov_degrees"),
				TEXT("A scene camera without an alias requires location, rotation, and field of view."), nullptr, TEXT("required camera component"));
		return true;
	}

	

	namespace
	{
		enum class ETimelineStepType : uint8
		{
			Wait,
			InputKey,
			InputPointer,
			InputScroll,
			InputText,
			ActorTransform,
			Capture,
			InputAction,
			PropertyWrite,
			PropertyGet,
			CaptureStart,
			CaptureStop,
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
			int32 Index = 0;
			double AtGameSeconds = 0.0;
			int32 AfterWait = INDEX_NONE;
			double WaitSeconds = 0.0;
			FKey Key;
			EInputEvent InputEvent = IE_Pressed;
			double Amount = 1.0;
			FVector2D PointerValue = FVector2D::ZeroVector;
			bool bRelativePointer = false;
			FString TextValue;
			FString ActorRef;
			FString Property;
			TSharedPtr<FJsonValue> PropertyValue;
			TArray<TSharedPtr<FJsonValue>> PropertyPath;
			FString PropertyAddressPointer;
			bool bRawPropertyValue = false;
			FTransformPatch Transform;
			FString ActionRef;
			FVector ActionValue = FVector::ZeroVector;
			EInputActionValueType ActionValueType = EInputActionValueType::Boolean;
			TWeakObjectPtr<const UInputAction> Action;
			FUeAgentPieCaptureStepSpec CaptureSpec;
			FString CollectionName;
			double CaptureInterval = 0.0;

			bool IsActionValueNonZero() const
			{
				return !ActionValue.IsNearlyZero();
			}
		};

		void BindPropertyStepInput(Contract::FDiagnostic& Error, const FTimelineStep& Step)
		{
			const FString Address = Step.PropertyAddressPointer;
			RemapArgumentInput(Error, {
				{ TEXT("/value"), StepPointer(Step.Index) + TEXT("/value") },
				{ TEXT("/path"), Address + TEXT("/path") },
				{ TEXT("/alias"), Address + TEXT("/alias") },
				{ TEXT("/property"), Address + TEXT("/property") }
			});
			if (!Error.OperationIndex.IsSet()) Error.OperationIndex = Step.Index;
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Step.ActorRef;
		}

		void AddCaptureContext(const FUeAgentPieCaptureStepSpec& Spec, const TSharedRef<FJsonObject>& Item)
		{
			Item->SetStringField(TEXT("source"), Spec.Source == EUeAgentPieCaptureSource::OutputSurface
				? TEXT("pie_output_surface") : Spec.Source == EUeAgentPieCaptureSource::PlayerCamera
				? TEXT("pie_player_camera") : TEXT("scene_camera"));
			if (!Spec.CameraRef.IsEmpty()) Item->SetStringField(TEXT("camera"), Spec.CameraRef);
		}

		void AddCaptureView(const FMinimalViewInfo& View, const TSharedRef<FJsonObject>& Item)
		{
			const auto Transform = MakeShared<FJsonObject>();
			Transform->SetStringField(TEXT("frame"), TEXT("ue_world"));
			Transform->SetNumberField(TEXT("dimension"), 3);
			Transform->SetStringField(TEXT("unit"), TEXT("centimeter"));
			Transform->SetStringField(TEXT("rotation_format"), TEXT("pitch_yaw_roll_degrees"));
			Transform->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(View.Location.X),
				MakeShared<FJsonValueNumber>(View.Location.Y), MakeShared<FJsonValueNumber>(View.Location.Z)});
			Transform->SetArrayField(TEXT("rotation"), {MakeShared<FJsonValueNumber>(View.Rotation.Pitch),
				MakeShared<FJsonValueNumber>(View.Rotation.Yaw), MakeShared<FJsonValueNumber>(View.Rotation.Roll)});
			Item->SetObjectField(TEXT("view_transform"), Transform);
			Item->SetNumberField(TEXT("fov_degrees"), View.FOV);
			Item->SetStringField(TEXT("projection"), View.ProjectionMode == ECameraProjectionMode::Orthographic ? TEXT("orthographic") : TEXT("perspective"));
			if (View.ProjectionMode == ECameraProjectionMode::Orthographic) Item->SetNumberField(TEXT("ortho_width"), View.OrthoWidth);
		}

		struct FCaptureCollection
		{
			TSharedPtr<EditorCapture::FCaptureSurface> Surface;
			TSharedPtr<FJsonObject> ClosedResult;
			FString CloseReason;
			double ClosedSeconds = 0.0;
			int32 CloseOperation = INDEX_NONE;
			FTimelineStep CaptureStep;
			double StartedAt = 0.0;
			double NextAt = 0.0;
			int64 NextSampleIndex = 0;
			int64 MissedFrames = 0;
			int32 FailedFrames = 0;
			bool bOpen = true;
			TArray<TSharedPtr<FJsonValue>> Frames;
			TArray<TSharedPtr<FJsonValue>> Gaps;
			TArray<TArray<FColor>> PreviewFrames;
			TArray<FString> PreviewLabels;
			TArray<int32> PreviewSampleIndices;
			FIntPoint PreviewSize = FIntPoint::ZeroValue;
			UE::UAI::Contract::FDiagnostic PreviewError;
		};

		struct FPendingCapture
		{
            UE::UAI::Contract::FDiagnostic Diagnostic;
			TSharedPtr<EditorCapture::FCaptureSurface> Surface;
			TSharedPtr<EditorCapture::FAsyncCaptureFrame, ESPMode::ThreadSafe> Readback;
			TFuture<CaptureMediaLongJob::FPublishedCaptureImage> Publication;
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			FString SourceIdentity;
			FString SurfaceKey;
			int32 Collection = INDEX_NONE;
			double Started = 0.0;
		};

		struct FObservationField
		{
			FString ObjectRef;
			FString Property;
		};

		struct FScenarioPlan
		{
			FUeAgentPieScenarioRequest CoreRequest;
			double EndGameSeconds = 0.0;
			TArray<FTimelineStep> Steps;
			TArray<FObservationField> ObservationFields;
			TArray<TSharedPtr<FJsonValue>> WatchRefs;
			TArray<TSharedPtr<FJsonValue>> ForgetRefs;
		};

		// Only ready events enter this min heap. Implicit cursor steps become ready
		// when their preceding wait completes; explicit at_seconds stays absolute.
		struct FScheduledStep
		{
			double AtSeconds = 0.0;
			int32 PlanIndex = 0;
			int32 DeclarationIndex = 0;
			bool bFinishWait = false;

			bool operator<(const FScheduledStep& Other) const
			{
				return AtSeconds != Other.AtSeconds ? AtSeconds < Other.AtSeconds
					: DeclarationIndex < Other.DeclarationIndex;
			}
		};

		bool Fail(FExecutionMutationError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
        { OutError = MoveTemp(Diagnostic); return false; }

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
			FVector& OutValue, Contract::FDiagnostic& OutError, const FString& Code, const FString& Prefix)
		{
			const FUeAgentCanonicalValueRef* Value = FindField(Object, Name);
			if (!Value || (*Value)->GetKind() != EUeAgentValueKind::Array
				|| (*Value)->GetArrayItems().Num() != 3)
			{
				return Fail(OutError, CanonicalInput(Code, TEXT("The transform component must contain exactly three finite numbers."),
					Prefix + TEXT("/") + Name, Value, TEXT("array of three finite numbers")));
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
					return Fail(OutError, CanonicalInput(Code, TEXT("The transform coordinate must be a finite number."),
						Prefix + TEXT("/") + Name + FString::Printf(TEXT("/%d"), Index), &Item, TEXT("finite number")));
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
			const auto Reject = [&OutError](const FString& Code, const FString& Reason, const FString& Pointer,
				const FUeAgentCanonicalValueRef* Value, const FString& Expected)
			{
				return Fail(OutError, CanonicalInput(Code, Reason, Pointer, Value, Expected));
			};
			for (const FString& Field : {FString(TEXT("watch_refs")), FString(TEXT("unwatch_refs"))})
			{
				if (const FUeAgentCanonicalValueRef* Value = FindField(Params, Field))
				{
					TSharedPtr<FJsonValue> Json;
					UE::UAI::Contract::FDiagnostic Error;
					if ((*Value)->GetKind() != EUeAgentValueKind::Array)
						return Reject(TEXT("pie_runtime_watch_refs_invalid"), TEXT("Reference tracking entries must be an array."),
							TEXT("/parameters/") + Field, Value, TEXT("array"));
					if (!CanonicalJson::TryToJsonValue(Value->Get(), Json, Error))
					{
						PrefixInputFailure(Error, TEXT("/parameters/") + Field);
						return Fail(OutError, MoveTemp(Error));
					}
					(Field == TEXT("watch_refs") ? OutPlan.WatchRefs : OutPlan.ForgetRefs) = Json->AsArray();
				}
			}
			OutPlan.CoreRequest.SessionIdentity = Request.GetSessionRef();
			if (!TryInteger(Params, TEXT("expected_pause_epoch"), OutPlan.CoreRequest.ExpectedPauseEpoch))
				return Reject(TEXT("pie_runtime_scenario_revision_invalid"), TEXT("The expected pause epoch must be an int64."),
					TEXT("/parameters/expected_pause_epoch"), FindField(Params, TEXT("expected_pause_epoch")), TEXT("int64"));
			if (!TryInteger(Params, TEXT("expected_workspace_revision"), OutPlan.CoreRequest.ExpectedWorkspaceRevision))
				return Reject(TEXT("pie_runtime_scenario_revision_invalid"), TEXT("The expected workspace revision must be an int64."),
					TEXT("/parameters/expected_workspace_revision"), FindField(Params, TEXT("expected_workspace_revision")), TEXT("int64"));
			const FUeAgentCanonicalValueRef* Limits = FindField(Params, TEXT("limits"));
			if (Limits)
			{
				if ((*Limits)->GetKind() != EUeAgentValueKind::Object)
					return Reject(TEXT("pie_runtime_scenario_limits_invalid"), TEXT("Scenario limits must be an object."),
						TEXT("/parameters/limits"), Limits, TEXT("object"));
				if (!TryNumber(*Limits, TEXT("wall_seconds"), OutPlan.CoreRequest.WallTimeLimitSeconds))
					return Reject(TEXT("pie_runtime_scenario_limits_invalid"), TEXT("The wall-time limit must be a finite number."),
						TEXT("/parameters/limits/wall_seconds"), FindField(*Limits, TEXT("wall_seconds")), TEXT("finite number"));
				if (!TryNumber(*Limits, TEXT("game_seconds"), OutPlan.CoreRequest.GameTimeLimitSeconds))
					return Reject(TEXT("pie_runtime_scenario_limits_invalid"), TEXT("The game-time limit must be a finite number."),
						TEXT("/parameters/limits/game_seconds"), FindField(*Limits, TEXT("game_seconds")), TEXT("finite number"));
				if (!TryInteger(*Limits, TEXT("frame_count"), OutPlan.CoreRequest.FrameLimit))
					return Reject(TEXT("pie_runtime_scenario_limits_invalid"), TEXT("The frame limit must be an int64."),
						TEXT("/parameters/limits/frame_count"), FindField(*Limits, TEXT("frame_count")), TEXT("int64"));
			}

			const FUeAgentCanonicalValueRef* Steps = FindField(Params, TEXT("steps"));
			if (!Steps || (*Steps)->GetKind() != EUeAgentValueKind::Array)
			{
				return Reject(TEXT("pie_runtime_scenario_steps_invalid"), TEXT("Scenario steps must be an array."),
					TEXT("/parameters/steps"), Steps, TEXT("array"));
			}
			double Cursor = 0.0;
			int32 CursorWait = INDEX_NONE;
			for (const FUeAgentCanonicalValueRef& StepValue : (*Steps)->GetArrayItems())
			{
				FString Type;
				FTimelineStep Step;
				Step.Index = OutPlan.Steps.Num();
				Step.AtGameSeconds = Cursor;
				Step.AfterWait = FindField(StepValue, TEXT("at_seconds")) ? INDEX_NONE : CursorWait;
				const auto RejectStep = [&OutError, &Step](const FString& Code, const FString& Reason,
					const FString& Field, const FUeAgentCanonicalValueRef* Value, const FString& Expected)
				{
					return Fail(OutError, StepInput(Code, Reason, Step.Index, Field, CanonicalValueFact(Value), MakeShared<FJsonValueString>(Expected)));
				};
				const auto RejectUnexpected = [&OutError, &Step, &Type](const FString& Field)
				{
					return Fail(OutError, StepInput(TEXT("pie_runtime_scenario_step_field_forbidden"),
						TEXT("This field is not supported by the scenario step."), Step.Index, TEXT("/") + PointerToken(Field),
						MakeShared<FJsonValueString>(Field), MakeShared<FJsonValueString>(TEXT("supported field for ") + Type)));
				};
				if (StepValue->GetKind() != EUeAgentValueKind::Object)
					return RejectStep(TEXT("pie_runtime_scenario_timeline_invalid"), TEXT("A scenario step must be an object."),
						TEXT(""), &StepValue, TEXT("object"));
				if (!TryString(StepValue, TEXT("type"), Type))
					return RejectStep(TEXT("pie_runtime_scenario_timeline_invalid"), TEXT("A scenario step needs a nonempty string type."),
						TEXT("/type"), FindField(StepValue, TEXT("type")), TEXT("nonempty string"));
				if ((FindField(StepValue, TEXT("at_seconds")) && !TryNumber(StepValue, TEXT("at_seconds"), Step.AtGameSeconds))
					|| Step.AtGameSeconds < 0.0)
					return RejectStep(TEXT("pie_runtime_scenario_timeline_invalid"), TEXT("The step time must be finite and nonnegative."),
						TEXT("/at_seconds"), FindField(StepValue, TEXT("at_seconds")), TEXT("finite number >= 0"));
				OutPlan.EndGameSeconds = FMath::Max(OutPlan.EndGameSeconds, Step.AtGameSeconds);
				if (Type == TEXT("wait"))
				{
					FString Unexpected;
					if (!HasOnlyFields(
							StepValue,
						{TEXT("type"), TEXT("at_seconds"), TEXT("duration_ms"), TEXT("duration")},
							Unexpected))
						return RejectUnexpected(Unexpected);
					Step.Type = ETimelineStepType::Wait;
					const bool bMilliseconds = FindField(StepValue, TEXT("duration_ms")) != nullptr;
					if (bMilliseconds && FindField(StepValue, TEXT("duration")))
						return Fail(OutError, StepInput(TEXT("pie_runtime_scenario_wait_invalid"),
							TEXT("Use duration_ms or the legacy duration seconds field, without combining them."), Step.Index, TEXT("/duration_ms"),
							Facts({ { TEXT("duration_ms_present"), MakeShared<FJsonValueBoolean>(true) }, { TEXT("duration_present"), MakeShared<FJsonValueBoolean>(true) } }),
							MakeShared<FJsonValueString>(TEXT("exactly one duration field"))));
					const TCHAR* DurationField = bMilliseconds ? TEXT("duration_ms") : TEXT("duration");
					double Duration = 0.0;
					if (FindField(StepValue, DurationField)
						&& (!TryNumber(StepValue, DurationField, Duration) || Duration < 0.0))
						return RejectStep(TEXT("pie_runtime_scenario_wait_invalid"), TEXT("The wait duration must be a finite nonnegative number."),
							TEXT("/") + FString(DurationField), FindField(StepValue, DurationField), TEXT("finite number >= 0"));
					Step.WaitSeconds = bMilliseconds ? Duration / 1000.0 : Duration;
					Cursor = Step.AtGameSeconds + Step.WaitSeconds;
					CursorWait = Step.Index;
					if (!FMath::IsFinite(Cursor)) return Fail(OutError, StepInput(TEXT("pie_runtime_scenario_wait_invalid"),
						TEXT("Adding the wait duration to the step time overflows the scenario timeline."), Step.Index, TEXT("/") + FString(DurationField),
						Facts({ { TEXT("at_seconds"), Number(Step.AtGameSeconds) }, { TEXT("wait_seconds"), Number(Step.WaitSeconds) }, { TEXT("end_seconds"), Number(Cursor) } }),
						MakeShared<FJsonValueString>(TEXT("finite end time"))));
					OutPlan.EndGameSeconds = FMath::Max(OutPlan.EndGameSeconds, Cursor);
				}
				else if (Type == TEXT("input_key"))
				{
					FString Unexpected;
					if (!HasOnlyFields(
							StepValue,
							{TEXT("type"), TEXT("at_seconds"), TEXT("key"), TEXT("event"), TEXT("amount")},
							Unexpected))
						return RejectUnexpected(Unexpected);
					FString KeyName;
					FString Event;
					Step.Type = ETimelineStepType::InputKey;
					if (!TryString(StepValue, TEXT("key"), KeyName))
						return RejectStep(TEXT("pie_runtime_scenario_input_key_invalid"), TEXT("The input key must be a nonempty string."),
							TEXT("/key"), FindField(StepValue, TEXT("key")), TEXT("registered key name"));
					if (!TryString(StepValue, TEXT("event"), Event))
						return RejectStep(TEXT("pie_runtime_scenario_input_key_invalid"), TEXT("The input event must be a nonempty string."),
							TEXT("/event"), FindField(StepValue, TEXT("event")), TEXT("pressed, released, repeat, axis, or mouse double_click"));
					Step.Key = FKey(FName(*KeyName));
					if (!Step.Key.IsValid())
					{
						return RejectStep(TEXT("pie_runtime_scenario_input_key_unknown"), TEXT("No registered input key has this name."),
							TEXT("/key"), FindField(StepValue, TEXT("key")), TEXT("registered key name"));
					}
					if (Event == TEXT("pressed")) Step.InputEvent = IE_Pressed;
					else if (Event == TEXT("released")) Step.InputEvent = IE_Released;
					else if (Event == TEXT("repeat")) Step.InputEvent = IE_Repeat;
					else if (Event == TEXT("double_click") && Step.Key.IsMouseButton()) Step.InputEvent = IE_DoubleClick;
					else if (Event == TEXT("axis")) Step.InputEvent = IE_Axis;
					else return Fail(OutError, StepInput(TEXT("pie_runtime_scenario_input_event_invalid"), TEXT("The event is unsupported for this input key."),
						Step.Index, TEXT("/event"), Facts({ { TEXT("event"), MakeShared<FJsonValueString>(Event) }, { TEXT("key"), MakeShared<FJsonValueString>(KeyName) }, { TEXT("mouse_button"), MakeShared<FJsonValueBoolean>(Step.Key.IsMouseButton()) } }),
						MakeShared<FJsonValueString>(TEXT("pressed, released, repeat, axis, or double_click for a mouse button"))));
					if (Step.InputEvent == IE_Axis && (Step.Key.IsAxis2D() || Step.Key.IsAxis3D()))
						return Fail(OutError, StepInput(TEXT("pie_input_device_vector_not_implemented"), TEXT("Device axis events support scalar keys only."),
							Step.Index, TEXT("/key"), Facts({ { TEXT("key"), MakeShared<FJsonValueString>(KeyName) }, { TEXT("dimensions"), Number(Step.Key.IsAxis3D() ? 3 : 2) } }),
							MakeShared<FJsonValueString>(TEXT("scalar input key"))));
					if (!TryNumber(StepValue, TEXT("amount"), Step.Amount))
					{
						Step.Amount = 1.0;
					}
				}
				else if (Type == TEXT("input_pointer") || Type == TEXT("input_scroll") || Type == TEXT("input_text"))
				{
					FString Unexpected;
					if (!HasOnlyFields(StepValue, {TEXT("type"), TEXT("at_seconds"), TEXT("position"), TEXT("delta"), TEXT("text")}, Unexpected))
						return RejectUnexpected(Unexpected);
					if (Type == TEXT("input_text"))
					{
						Step.Type = ETimelineStepType::InputText;
						if (!TryString(StepValue, TEXT("text"), Step.TextValue)) return RejectStep(TEXT("pie_input_text_invalid"), TEXT("Text input must be a nonempty string."),
							TEXT("/text"), FindField(StepValue, TEXT("text")), TEXT("nonempty string"));
					}
					else if (Type == TEXT("input_scroll"))
					{
						Step.Type = ETimelineStepType::InputScroll;
						if (!TryNumber(StepValue, TEXT("delta"), Step.Amount)) return RejectStep(TEXT("pie_input_scroll_invalid"), TEXT("The scroll delta must be a finite number."),
							TEXT("/delta"), FindField(StepValue, TEXT("delta")), TEXT("finite number"));
					}
					else
					{
						Step.Type = ETimelineStepType::InputPointer;
						const auto* Position = FindField(StepValue, TEXT("position"));
						const auto* Delta = FindField(StepValue, TEXT("delta"));
						if ((Position != nullptr) == (Delta != nullptr)) return Fail(OutError, StepInput(TEXT("pie_input_pointer_one_coordinate_required"), TEXT("Pointer input requires exactly one of position or delta."),
							Step.Index, TEXT(""), Facts({ { TEXT("position_present"), MakeShared<FJsonValueBoolean>(Position != nullptr) }, { TEXT("delta_present"), MakeShared<FJsonValueBoolean>(Delta != nullptr) } }),
							MakeShared<FJsonValueString>(TEXT("exactly one coordinate field"))));
						Step.bRelativePointer = Delta != nullptr;
						const auto& Value = *(Position ? Position : Delta);
						TSharedPtr<FJsonValue> Json;
						UE::UAI::Contract::FDiagnostic Error;
						const FString Field = Position ? TEXT("/position") : TEXT("/delta");
						if (!CanonicalJson::TryToJsonValue(*Value, Json, Error))
						{
							PrefixInputFailure(Error, StepPointer(Step.Index) + Field);
							return Fail(OutError, MoveTemp(Error));
						}
						if (Json->Type != EJson::Array || Json->AsArray().Num() != 2)
							return RejectStep(TEXT("pie_input_pointer_invalid"), TEXT("Pointer coordinates must contain exactly two numbers."),
								Field, &Value, TEXT("array of two finite numbers"));
						const bool bXParsed = Json->AsArray()[0]->TryGetNumber(Step.PointerValue.X);
						const bool bYParsed = bXParsed && Json->AsArray()[1]->TryGetNumber(Step.PointerValue.Y);
						if (!bXParsed || !bYParsed || !FMath::IsFinite(Step.PointerValue.X) || !FMath::IsFinite(Step.PointerValue.Y))
						{
							const int32 InvalidIndex = !bXParsed ? 0 : !bYParsed ? 1 : !FMath::IsFinite(Step.PointerValue.X) ? 0 : 1;
							return Fail(OutError, StepInput(TEXT("pie_input_pointer_invalid"), TEXT("The pointer coordinate must be a finite number."),
								Step.Index, Field + FString::Printf(TEXT("/%d"), InvalidIndex), Json->AsArray()[InvalidIndex], MakeShared<FJsonValueString>(TEXT("finite number"))));
						}
					}
				}
				else if (Type == TEXT("actor_transform"))
				{
					FString Unexpected;
					if (!HasOnlyFields(
							StepValue,
							{TEXT("type"), TEXT("at_seconds"), TEXT("alias"), TEXT("transform")},
							Unexpected))
						return RejectUnexpected(Unexpected);
					Step.Type = ETimelineStepType::ActorTransform;
					const FUeAgentCanonicalValueRef* Transform =
						FindField(StepValue, TEXT("transform"));
					if (!TryString(StepValue, TEXT("alias"), Step.ActorRef))
						return RejectStep(TEXT("pie_runtime_scenario_actor_transform_invalid"), TEXT("The actor alias must be a nonempty string."),
							TEXT("/alias"), FindField(StepValue, TEXT("alias")), TEXT("nonempty actor alias"));
					if (!Transform || (*Transform)->GetKind() != EUeAgentValueKind::Object)
						return RejectStep(TEXT("pie_runtime_scenario_actor_transform_invalid"), TEXT("An actor transform must be an object."),
							TEXT("/transform"), Transform, TEXT("object"));
					FString Frame;
					FString Unit;
					FString RotationFormat;
					int64 Dimension = 0;
					if (!TryString(*Transform, TEXT("frame"), Frame))
						return RejectStep(TEXT("pie_runtime_scenario_transform_frame_unsupported"), TEXT("The transform frame must be a nonempty string."),
							TEXT("/transform/frame"), FindField(*Transform, TEXT("frame")), TEXT("ue_world"));
					if (!TryString(*Transform, TEXT("unit"), Unit))
						return RejectStep(TEXT("pie_runtime_scenario_transform_frame_unsupported"), TEXT("The transform unit must be a nonempty string."),
							TEXT("/transform/unit"), FindField(*Transform, TEXT("unit")), TEXT("centimeter"));
					if (!TryString(*Transform, TEXT("rotation_format"), RotationFormat))
						return RejectStep(TEXT("pie_runtime_scenario_transform_frame_unsupported"), TEXT("The transform rotation format must be a nonempty string."),
							TEXT("/transform/rotation_format"), FindField(*Transform, TEXT("rotation_format")), TEXT("pitch_yaw_roll_degrees"));
					if (!TryInteger(*Transform, TEXT("dimension"), Dimension))
						return RejectStep(TEXT("pie_runtime_scenario_transform_frame_unsupported"), TEXT("The transform dimension must be an integer."),
							TEXT("/transform/dimension"), FindField(*Transform, TEXT("dimension")), TEXT("integer 3"));
					if (Frame != TEXT("ue_world"))
						return RejectStep(TEXT("pie_runtime_scenario_transform_frame_unsupported"), TEXT("Actor transforms require the UE world frame."),
							TEXT("/transform/frame"), FindField(*Transform, TEXT("frame")), TEXT("ue_world"));
					if (Unit != TEXT("centimeter"))
						return RejectStep(TEXT("pie_runtime_scenario_transform_frame_unsupported"), TEXT("Actor transform locations use centimeters."),
							TEXT("/transform/unit"), FindField(*Transform, TEXT("unit")), TEXT("centimeter"));
					if (RotationFormat != TEXT("pitch_yaw_roll_degrees"))
						return RejectStep(TEXT("pie_runtime_scenario_transform_frame_unsupported"), TEXT("Actor transform rotations use pitch, yaw, and roll in degrees."),
							TEXT("/transform/rotation_format"), FindField(*Transform, TEXT("rotation_format")), TEXT("pitch_yaw_roll_degrees"));
					if (Dimension != 3)
						return RejectStep(TEXT("pie_runtime_scenario_transform_frame_unsupported"), TEXT("Actor transforms require three dimensions."),
							TEXT("/transform/dimension"), FindField(*Transform, TEXT("dimension")), TEXT("integer 3"));
					FVector Vector;
					if (FindField(*Transform, TEXT("location")))
					{
						if (!TryVector(*Transform, TEXT("location"), Vector, OutError,
								TEXT("pie_runtime_scenario_location_invalid"), StepPointer(Step.Index) + TEXT("/transform"))) return false;
						Step.Transform.Location = Vector;
					}
					if (FindField(*Transform, TEXT("rotation")))
					{
						if (!TryVector(*Transform, TEXT("rotation"), Vector, OutError,
								TEXT("pie_runtime_scenario_rotation_invalid"), StepPointer(Step.Index) + TEXT("/transform"))) return false;
						Step.Transform.Rotation = FRotator(Vector.X, Vector.Y, Vector.Z);
					}
					if (FindField(*Transform, TEXT("scale")))
					{
						if (!TryVector(*Transform, TEXT("scale"), Vector, OutError,
								TEXT("pie_runtime_scenario_scale_invalid"), StepPointer(Step.Index) + TEXT("/transform"))) return false;
						Step.Transform.Scale = Vector;
					}
					if (!Step.Transform.Location.IsSet()
						&& !Step.Transform.Rotation.IsSet()
						&& !Step.Transform.Scale.IsSet())
					{
						return Fail(OutError, StepInput(TEXT("pie_runtime_scenario_transform_empty"), TEXT("The actor transform must specify location, rotation, or scale."),
							Step.Index, TEXT("/transform"), Facts({ { TEXT("location_present"), MakeShared<FJsonValueBoolean>(false) }, { TEXT("rotation_present"), MakeShared<FJsonValueBoolean>(false) }, { TEXT("scale_present"), MakeShared<FJsonValueBoolean>(false) } }),
							MakeShared<FJsonValueString>(TEXT("at least one transform component"))));
					}
				}
				else if (Type == TEXT("property_write") || Type == TEXT("property_get") || Type == TEXT("property_set"))
				{
					FString Unexpected;
					const FUeAgentCanonicalValueRef* Ref = FindField(StepValue, TEXT("ref"));
					const FUeAgentCanonicalValueRef& Address = Ref ? *Ref : StepValue;
					if (!HasOnlyFields(StepValue,
							{TEXT("type"), TEXT("at_seconds"), TEXT("alias"), TEXT("property"), TEXT("ref"), TEXT("value")}, Unexpected))
						return Fail(OutError, StepInput(TEXT("pie_runtime_scenario_property_write_invalid"), TEXT("This field is not supported by a property operation."),
							Step.Index, TEXT("/") + PointerToken(Unexpected), MakeShared<FJsonValueString>(Unexpected),
							MakeShared<FJsonValueString>(TEXT("type, at_seconds, alias, property, ref, or value"))));
					Step.PropertyAddressPointer = StepPointer(Step.Index) + (Ref ? TEXT("/ref") : TEXT(""));
					const FString AddressField = Ref ? TEXT("/ref") : TEXT("");
					if (Address->GetKind() != EUeAgentValueKind::Object)
						return RejectStep(TEXT("pie_runtime_scenario_property_write_invalid"), TEXT("A property reference must be an object."),
							AddressField, &Address, TEXT("object containing alias and property"));
					if (!TryString(Address, TEXT("alias"), Step.ActorRef))
						return RejectStep(TEXT("pie_runtime_scenario_property_write_invalid"), TEXT("A property reference needs a nonempty alias."),
							AddressField + TEXT("/alias"), FindField(Address, TEXT("alias")), TEXT("nonempty alias"));
					if (!TryString(Address, TEXT("property"), Step.Property))
						return RejectStep(TEXT("pie_runtime_scenario_property_write_invalid"), TEXT("A property reference needs a nonempty property name."),
							AddressField + TEXT("/property"), FindField(Address, TEXT("property")), TEXT("nonempty property name"));
					Step.bRawPropertyValue = Type == TEXT("property_set");
					if (const FUeAgentCanonicalValueRef* Path = FindField(Address, TEXT("path")))
					{
						TSharedPtr<FJsonValue> PathJson;
						UE::UAI::Contract::FDiagnostic PathError;
						if ((*Path)->GetKind() != EUeAgentValueKind::Array)
							return RejectStep(TEXT("pie_runtime_property_path_invalid"), TEXT("A property path must be an array."),
								AddressField + TEXT("/path"), Path, TEXT("array"));
						if (!CanonicalJson::TryToJsonValue(Path->Get(), PathJson, PathError))
						{
							PrefixInputFailure(PathError, Step.PropertyAddressPointer + TEXT("/path"));
							return Fail(OutError, MoveTemp(PathError));
						}
						Step.PropertyPath = PathJson->AsArray();
					}
					if (Type == TEXT("property_get"))
					{
						Step.Type = ETimelineStepType::PropertyGet;
						OutPlan.Steps.Add(MoveTemp(Step));
						continue;
					}
					const FUeAgentCanonicalValueRef* Value = FindField(StepValue, TEXT("value"));
					UE::UAI::Contract::FDiagnostic JsonError;
					if (!Value)
						return RejectStep(TEXT("pie_runtime_property_value_missing"), TEXT("The property write requires a value."),
							TEXT("/value"), nullptr, TEXT("present value, including explicit null"));
					if (!CanonicalJson::TryToJsonValue(Value->Get(), Step.PropertyValue, JsonError))
					{
						PrefixInputFailure(JsonError, StepPointer(Step.Index) + TEXT("/value"));
						return Fail(OutError, MoveTemp(JsonError));
					}
					Step.Type = ETimelineStepType::PropertyWrite;
				}
				else if (Type == TEXT("capture_stop"))
				{
					Step.Type = ETimelineStepType::CaptureStop;
					if (!TryString(StepValue, TEXT("name"), Step.CollectionName))
						return RejectStep(TEXT("pie_runtime_capture_collection_name_missing"), TEXT("The capture collection name must be a nonempty string."),
							TEXT("/name"), FindField(StepValue, TEXT("name")), TEXT("nonempty collection name"));
				}
				else if (Type == TEXT("input_action") || Type == TEXT("capture") || Type == TEXT("capture_start"))
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
						UE::UAI::Contract::FDiagnostic ParseError;
						if (!TryParsePieInputActionSample(
								StepValue, ExistingSamples, Sample, ParseError))
						{
							PrefixInputFailure(ParseError, StepPointer(Step.Index));
							ParseError.OperationIndex = Step.Index;
							return Fail(OutError, MoveTemp(ParseError));
						}
						Step.Type = ETimelineStepType::InputAction;
						Step.ActionRef = MoveTemp(Sample.ActionRef);
						Step.ActionValueType = Sample.ValueType;
						Step.ActionValue = Sample.Value;
						OutPlan.Steps.Add(MoveTemp(Step));
						continue;
					}
					UE::UAI::Contract::FDiagnostic CaptureParseError;
					if (!TryParsePieCaptureStepSpec(
							StepValue, Step.CaptureSpec, CaptureParseError, Step.Index))
					{
						PrefixInputFailure(CaptureParseError, TEXT("/parameters"));
						return Fail(OutError, MoveTemp(CaptureParseError));
					}
					Step.Type = ETimelineStepType::Capture;
					if (Type == TEXT("capture_start"))
					{
						Step.Type = ETimelineStepType::CaptureStart;
						if (!TryString(StepValue, TEXT("name"), Step.CollectionName))
							return RejectStep(TEXT("pie_runtime_capture_collection_invalid"), TEXT("The capture collection name must be a nonempty string."),
								TEXT("/name"), FindField(StepValue, TEXT("name")), TEXT("nonempty collection name"));
						if (!TryNumber(StepValue, TEXT("interval"), Step.CaptureInterval) || Step.CaptureInterval <= 0.0)
							return RejectStep(TEXT("pie_runtime_capture_collection_invalid"), TEXT("The capture interval must be finite and positive."),
								TEXT("/interval"), FindField(StepValue, TEXT("interval")), TEXT("finite number > 0"));
					}
				}
				else
				{
					return RejectStep(TEXT("pie_runtime_scenario_step_type_unknown"), TEXT("The scenario step type is unsupported."),
						TEXT("/type"), FindField(StepValue, TEXT("type")),
						TEXT("wait, input_key, input_pointer, input_scroll, input_text, actor_transform, property_write, property_get, property_set, input_action, capture, capture_start, or capture_stop"));
				}
				OutPlan.Steps.Add(MoveTemp(Step));
			}
			OutPlan.Steps.StableSort([](const FTimelineStep& A, const FTimelineStep& B)
			{
				return A.AtGameSeconds < B.AtGameSeconds;
			});
			TSet<FString> OpenCollections;
			for (const FTimelineStep& Step : OutPlan.Steps)
			{
				if (Step.Type == ETimelineStepType::CaptureStart)
				{
					if (OpenCollections.Contains(Step.CollectionName)) return Fail(OutError, StepInput(TEXT("pie_runtime_capture_collection_already_open"),
						TEXT("A capture collection with this name is already open at this step."), Step.Index, TEXT("/name"),
						MakeShared<FJsonValueString>(Step.CollectionName), MakeShared<FJsonValueString>(TEXT("name of a closed collection"))));
					OpenCollections.Add(Step.CollectionName);
				}
				else if (Step.Type == ETimelineStepType::CaptureStop && OpenCollections.Remove(Step.CollectionName) == 0)
					return Fail(OutError, StepInput(TEXT("pie_runtime_capture_collection_not_open"),
						TEXT("No capture collection with this name is open at this step."), Step.Index, TEXT("/name"),
						MakeShared<FJsonValueString>(Step.CollectionName), MakeShared<FJsonValueString>(TEXT("name of an open collection"))));
			}
			TMap<FString, Contract::FSourceLocation> CoreInputSources;
			const auto RegisterInput = [&CoreInputSources](const FString& Field)
			{
				Contract::FSourceLocation Source;
				Source.JsonPath = TEXT("/parameters/") + Field;
				Source.bCommandInput = true;
				CoreInputSources.Add(Field, MoveTemp(Source));
			};
			RegisterInput(TEXT("expected_pause_epoch"));
			RegisterInput(TEXT("expected_workspace_revision"));
			if (Limits)
			{
				RegisterInput(TEXT("limits/wall_seconds"));
				RegisterInput(TEXT("limits/game_seconds"));
				RegisterInput(TEXT("limits/frame_count"));
			}
			Contract::FDiagnostic ValidationError;
			if (!OutPlan.CoreRequest.TryValidate(ValidationError, CoreInputSources))
				return Fail(OutError, Contract::FDiagnostic::WrapFailure(TEXT("pie_runtime_scenario_request_invalid"), ValidationError,
					TEXT("pie_scenario"), TEXT("The scenario request contains an invalid session, revision, or limit.")));

			if (const FUeAgentCanonicalValueRef* Fields =
				FindField(Params, TEXT("observation_fields")))
			{
				if ((*Fields)->GetKind() != EUeAgentValueKind::Array)
					return Reject(TEXT("pie_runtime_scenario_observations_invalid"), TEXT("Observation fields must be an array."),
						TEXT("/parameters/observation_fields"), Fields, TEXT("array"));
				for (const FUeAgentCanonicalValueRef& FieldValue : (*Fields)->GetArrayItems())
				{
					FObservationField Field;
					const FString Pointer = FString::Printf(TEXT("/parameters/observation_fields/%d"), OutPlan.ObservationFields.Num());
					if (FieldValue->GetKind() != EUeAgentValueKind::Object)
						return Reject(TEXT("pie_runtime_scenario_observation_invalid"), TEXT("An observation field must be an object."),
							Pointer, &FieldValue, TEXT("object containing alias and property"));
					if (!TryString(FieldValue, TEXT("alias"), Field.ObjectRef))
						return Reject(TEXT("pie_runtime_scenario_observation_invalid"), TEXT("An observation field needs a nonempty alias."),
							Pointer + TEXT("/alias"), FindField(FieldValue, TEXT("alias")), TEXT("nonempty alias"));
					if (!TryString(FieldValue, TEXT("property"), Field.Property))
						return Reject(TEXT("pie_runtime_scenario_observation_invalid"), TEXT("An observation field needs a nonempty property name."),
							Pointer + TEXT("/property"), FindField(FieldValue, TEXT("property")), TEXT("nonempty property name"));
					OutPlan.ObservationFields.Add(MoveTemp(Field));
				}
			}
			return true;
		}

		bool CheckObservationValue(
			const bool bSucceeded, const FUeAgentCanonicalValuePtr& Value,
			UE::UAI::Contract::FDiagnostic& OutError, const FString& Path)
		{
			if (bSucceeded && Value.IsValid()) return true;
			if (OutError.IsEmpty())
			{
				OutError = Native(TEXT("pie_runtime_scenario_observation_signal_failed"),
					bSucceeded ? TEXT("Observation value construction reported success without returning a value.")
						: TEXT("Observation value construction failed without a diagnostic."), TEXT("pie_scenario"), Path,
					Facts({ { TEXT("operation_succeeded"), MakeShared<FJsonValueBoolean>(bSucceeded) },
						{ TEXT("value_available"), MakeShared<FJsonValueBoolean>(Value.IsValid()) } }),
					Facts({ { TEXT("operation_succeeded"), MakeShared<FJsonValueBoolean>(true) },
						{ TEXT("value_available"), MakeShared<FJsonValueBoolean>(true) } }));
			}
			if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Path;
			return false;
		}

		bool TryMakeStringValue(
			const FString& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& Path)
		{
			const bool bSucceeded = FUeAgentCanonicalValue::TryString(Value, Resources, OutValue, OutError);
			return CheckObservationValue(bSucceeded, OutValue, OutError, Path);
		}

		bool TryMakeIntegerValue(
			int64 Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& Path)
		{
			const bool bSucceeded = FUeAgentCanonicalValue::TryInteger(
				FString::Printf(TEXT("%lld"), Value), Resources, OutValue, OutError);
			return CheckObservationValue(bSucceeded, OutValue, OutError, Path);
		}

		bool TryMakeObservationSignal(
			const TArray<TPair<FString, TSharedPtr<FJsonValue>>>& Values,
			const TSharedRef<FJsonObject>& Flow,
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
			TOptional<FLongRunningOperationSignal>& OutSignal,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutSignal.Reset();
			OutError.Reset();
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> ObservationItems;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Value : Values)
			{
				FUeAgentCanonicalValuePtr Canonical;
				const bool bConverted = CanonicalJson::TryFromJsonValue(Value.Value, Resources, Canonical, OutError);
				if (!CheckObservationValue(bConverted, Canonical, OutError, TEXT("fields.") + Value.Key))
				{
					return false;
				}
				ObservationItems.Emplace(Value.Key, Canonical.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Observations;
			const bool bObservationsBuilt = FUeAgentCanonicalValue::TryObject(ObservationItems, Resources, Observations, OutError);
			if (!CheckObservationValue(bObservationsBuilt, Observations, OutError, TEXT("fields")))
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
					TEXT("uai.pie.runtime.observation/2"), Resources, Schema, OutError, TEXT("schema"))
				|| !TryMakeStringValue(TerminalStatus, Resources, Status, OutError, TEXT("terminal_status"))
				|| !TryMakeStringValue(ErrorCode, Resources, ErrorCodeValue, OutError, TEXT("error_code"))
				|| !TryMakeStringValue(SessionRef, Resources, Session, OutError, TEXT("session_ref"))
				|| !TryMakeStringValue(WorkspacePath, Resources, Path, OutError, TEXT("workspace_path"))
				|| !TryMakeIntegerValue(PauseEpoch, Resources, Epoch, OutError, TEXT("pause_epoch"))
				|| !TryMakeIntegerValue(WorkspaceRevision, Resources, Revision, OutError, TEXT("workspace_revision")))
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
			FUeAgentCanonicalValuePtr FlowValue;
			const bool bFlowConverted = CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Flow), Resources, FlowValue, OutError);
			if (!CheckObservationValue(bFlowConverted, FlowValue, OutError, TEXT("flow"))) return false;
			RootItems.Emplace(TEXT("flow"), FlowValue.ToSharedRef());
			if (Artifact.IsSet())
			{
				FUeAgentCanonicalValuePtr ArtifactValue;
				FUeAgentCanonicalValuePtr SourceValue;
				FUeAgentCanonicalValuePtr EvidenceValue;
				const bool bArtifactProjected = TryProjectFileArtifactReference(Artifact.GetValue(), Resources, ArtifactValue, OutError);
				if (!CheckObservationValue(bArtifactProjected, ArtifactValue, OutError, TEXT("artifact_ref"))) return false;
				if (CaptureSourceRef.IsEmpty() || CaptureEvidenceId.IsEmpty())
				{
					OutError = Native(TEXT("pie_runtime_scenario_observation_signal_failed"),
						CaptureSourceRef.IsEmpty() ? TEXT("The captured artifact observation has no capture source reference.")
							: TEXT("The captured artifact observation has no capture evidence identifier."), TEXT("pie_scenario"),
						CaptureSourceRef.IsEmpty() ? TEXT("capture_source_ref_present") : TEXT("capture_evidence_id_present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (!TryMakeStringValue(CaptureSourceRef, Resources, SourceValue, OutError, TEXT("capture_source_ref"))
					|| !TryMakeStringValue(CaptureEvidenceId, Resources, EvidenceValue, OutError, TEXT("capture_evidence_id"))) return false;
				RootItems.Emplace(TEXT("artifact_ref"), ArtifactValue.ToSharedRef());
				RootItems.Emplace(TEXT("capture_source_ref"), SourceValue.ToSharedRef());
				RootItems.Emplace(TEXT("capture_evidence_id"), EvidenceValue.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Root;
			const bool bRootBuilt = FUeAgentCanonicalValue::TryObject(RootItems, Resources, Root, OutError);
			if (!CheckObservationValue(bRootBuilt, Root, OutError, TEXT("observation")))
			{
				return false;
			}
			OutSignal.Emplace(
				TEXT("uai.pie.runtime.observation/2"),
				SignalRevision,
				Root.ToSharedRef());
			if (!OutSignal->IsValid())
			{
				// The type identifier above is a non-empty literal; only revision can invalidate this signal.
				OutError = Native(TEXT("pie_runtime_scenario_observation_signal_failed"), TEXT("The observation signal revision must be positive."), TEXT("pie_scenario"),
					TEXT("signal_revision"), Number(static_cast<double>(SignalRevision)), MakeShared<FJsonValueString>(TEXT("positive integer")));
				OutSignal.Reset();
				return false;
			}
			return true;
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
				RuntimeEndGameSeconds = Plan.EndGameSeconds;
				CompletedSteps.Init(false, Plan.Steps.Num());
				TimingByOperation.SetNum(Plan.Steps.Num());
				for (int32 I = 0; I < Plan.Steps.Num(); ++I)
				{
					const FTimelineStep& Step = Plan.Steps[I];
					if (Step.AfterWait == INDEX_NONE)
						ScheduledSteps.HeapPush({Step.AtGameSeconds, I, Step.Index, false});
					else StepsAfterWait.FindOrAdd(Step.AfterWait).Add(I);
				}
			}

			bool IsCurrentPausedSnapshot(
				const FString& SessionIdentity,
				int64 ExpectedPauseEpoch,
				int64 ExpectedWorkspaceRevision, UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				
				return Registry->TryResolveExact(
					SessionIdentity,
					ExpectedPauseEpoch,
					ExpectedWorkspaceRevision,
					Snapshot,
                    OutError);
			}

			bool Resume(UE::UAI::Contract::FDiagnostic& OutError) override
			{
				
				return Registry->TryResume(Plan.CoreRequest.SessionIdentity, OutError);
			}

			EUeAgentPieScenarioStepResult ExecuteSteps(double, double, int64, UE::UAI::Contract::FDiagnostic& OutError) override
            {
				OutError = Native(TEXT("pie_runtime_scenario_async_dispatch_required"), TEXT("This scenario requires asynchronous dispatch."), TEXT("pie_scenario"),
					TEXT("dispatch_mode"), MakeShared<FJsonValueString>(TEXT("synchronous")), MakeShared<FJsonValueString>(TEXT("asynchronous")));
                return EUeAgentPieScenarioStepResult::Failed;
            }

			bool StopDispatch(UE::UAI::Contract::FDiagnostic& OutError) override
			{
				TickerHandle.Reset();
				bDispatchStopped = true;
				return true;
			}

			bool ReleaseAllForOperation(const FUeAgentPieScenarioReceipt& Receipt, UE::UAI::Contract::FDiagnostic& OutError) override
			{
				UE::UAI::Contract::FDiagnostic ReleaseError;
				const bool bSessionReleased = Registry->ReleaseInputs(Plan.CoreRequest.SessionIdentity, ReleaseError);
				const bool bActionReleaseSucceeded = !InputActionLedger.HasInjectedNonZero() || bActionReleaseVerified;
				if (!bSessionReleased) OutError.Append(UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("pie_runtime_input_release_failed"),
					ReleaseError, TEXT("cleanup"), TEXT("Held session input could not be released.")));
				if (!bActionReleaseSucceeded) OutError.Append(Native(TEXT("pie_runtime_action_release_unverified"), TEXT("Previously injected nonzero input action values could not be verified as released."), TEXT("cleanup"),
					TEXT("action_release_verified"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				if (bKeysReleased)
				{
					return bActionReleaseSucceeded && bSessionReleased;
				}
				if (InjectedInput.IsEmpty())
				{
					bKeysReleased = true;
					return bActionReleaseSucceeded && bSessionReleased;
				}
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				UE::UAI::Contract::FDiagnostic ResolveError;
				if (!Registry->TryResolveExact(
						Plan.CoreRequest.SessionIdentity,
						Receipt.PauseEpoch,
						Receipt.WorkspaceRevision,
						Snapshot,
                        ResolveError))
				{
					OutError.Append(ResolveError);
					return false;
				}
				APlayerController* Controller = Snapshot.PlayerController.Get();
				if (!Controller)
				{
					OutError.Append(Native(TEXT("pie_runtime_input_controller_unavailable"), TEXT("The player controller needed for input cleanup is unavailable."), TEXT("cleanup"),
					TEXT("player_controller_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					return false;
				}
				bKeysReleased = true;
				bool bSucceeded = true;
				for (const FUeAgentPieInjectedInputCleanup& Cleanup
					: InjectedInput.ConsumeCleanupActions())
				{
					Controller->InputKey(
						FInputKeyEventArgs::CreateSimulated(
							Cleanup.Key,
							Cleanup.Event,
							Cleanup.Amount));
				}
				return bSucceeded && bActionReleaseSucceeded && bSessionReleased;
			}

			bool RepauseAndVerify(const FString& SessionIdentity, int64& OutPauseEpoch, UE::UAI::Contract::FDiagnostic& OutError) override
			{
				
				return Registry->TryRepauseAndVerify(SessionIdentity, OutPauseEpoch, OutError);
			}

			bool RefreshCurrentWorkspace(
				const FString& SessionIdentity,
				int64 PreviousWorkspaceRevision,
				int64& OutWorkspaceRevision, UE::UAI::Contract::FDiagnostic& OutError) override
			{
				
				return Registry->TryRefreshCurrentWorkspace(
					SessionIdentity,
					PreviousWorkspaceRevision,
					OutWorkspaceRevision,
                    OutError);
			}

			void SetTerminalNoSideEffects(
				const FString& InStatus,
				const UE::UAI::Contract::FDiagnostic& InDiagnostic)
			{
				CPUThrottleExemption.Reset();
				FUeAgentPieRuntimeDebugSnapshot Current;
				UE::UAI::Contract::FDiagnostic ResolveError;
				const bool bResolved = Registry->TryResolveExact(Plan.CoreRequest.SessionIdentity,
					Plan.CoreRequest.ExpectedPauseEpoch, Plan.CoreRequest.ExpectedWorkspaceRevision, Current, ResolveError);
				FScopeLock Lock(&Mutex);
				Phase = EScenarioDispatchPhase::Terminal;
				++SignalRevision;
				TerminalStatus = InStatus;
				Diagnostic = InDiagnostic;
				WorkState = ELongRunningWorkState::ExitedFailure;
				const auto Execution = MakeShared<FJsonObject>();
				Execution->SetStringField(TEXT("status"), InStatus);
				Execution->SetNumberField(TEXT("game_seconds"), 0.0);
				Execution->SetArrayField(TEXT("completed_operations"), {});
				TArray<TSharedPtr<FJsonValue>> Remaining;
				for (const auto& Step : Plan.Steps) Remaining.Add(MakeShared<FJsonValueNumber>(Step.Index));
				Execution->SetArrayField(TEXT("remaining_operations"), Remaining);
				if (FailedOperation != INDEX_NONE) Execution->SetNumberField(TEXT("failed_operation"), FailedOperation);
				Flow->SetObjectField(TEXT("execution"), Execution);
				Flow->SetArrayField(TEXT("results"), {});
				const auto Snapshot = MakeShared<FJsonObject>();
				Snapshot->SetBoolField(TEXT("paused"), bResolved && Current.World.IsValid() && Current.World->IsPaused());
				Snapshot->SetBoolField(TEXT("updated"), false);
				if (bResolved) Snapshot->SetStringField(TEXT("path"), Current.WorkspacePath);
				Flow->SetObjectField(TEXT("snapshot"), Snapshot);
				Flow->SetArrayField(TEXT("diagnostics"), UE::UAI::Contract::ToPublicDiagnostics({InDiagnostic}));
			}

			void StoreReceipt(const FUeAgentPieScenarioReceipt& Receipt)
			{
				CPUThrottleExemption.Reset();
				FScopeLock Lock(&Mutex);
				Phase = EScenarioDispatchPhase::Terminal;
				++SignalRevision;
				TerminalStatus = Receipt.TerminalStatus;
				Diagnostic = Receipt.Diagnostic;
				ObservedPauseEpoch = Receipt.PauseEpoch;
				ObservedWorkspaceRevision = Receipt.WorkspaceRevision;
				const TSharedRef<FJsonObject> Execution = MakeShared<FJsonObject>();
				Execution->SetStringField(TEXT("status"), Receipt.TerminalStatus);
				Execution->SetNumberField(TEXT("game_seconds"), LastGameSeconds);
				TArray<TSharedPtr<FJsonValue>> Completed;
				for (int32 Index : CompletedOperations) Completed.Add(MakeShared<FJsonValueNumber>(Index));
				Execution->SetArrayField(TEXT("completed_operations"), Completed);
				TArray<TSharedPtr<FJsonValue>> Remaining;
				for (int32 I = 0; I < Plan.Steps.Num(); ++I)
					if (!CompletedSteps[I]) Remaining.Add(MakeShared<FJsonValueNumber>(Plan.Steps[I].Index));
				Execution->SetArrayField(TEXT("remaining_operations"), Remaining);
				Execution->SetArrayField(TEXT("operation_times_ms"), OperationTimes);
				if (FailedOperation != INDEX_NONE)
					Execution->SetNumberField(TEXT("failed_operation"), FailedOperation);
				if (!FailedCollection.IsEmpty()) Execution->SetStringField(TEXT("failed_collection"), FailedCollection);
				Flow->SetObjectField(TEXT("execution"), Execution);
				Flow->SetArrayField(TEXT("results"), Results);
				const TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
				Snapshot->SetBoolField(TEXT("paused"), Receipt.bPaused);
				Snapshot->SetBoolField(TEXT("updated"), Receipt.bWorkspaceRefreshed);
				if (Receipt.bWorkspaceRefreshed) Snapshot->SetObjectField(TEXT("important_references"), Registry->ReferenceUpdates(Receipt.SessionIdentity));
				if (Receipt.bWorkspaceRefreshed) Snapshot->SetStringField(TEXT("path"), WorkspacePath);
				Flow->SetObjectField(TEXT("snapshot"), Snapshot);
                TArray<UE::UAI::Contract::FDiagnostic> Diagnostics;
                if (!Receipt.Diagnostic.IsEmpty()) Diagnostics.Add(Receipt.Diagnostic);
                Diagnostics.Append(Receipt.CleanupDiagnostics);
                Flow->SetArrayField(TEXT("diagnostics"), UE::UAI::Contract::ToPublicDiagnostics(Diagnostics));
				if (!ConsistencyWarnings.IsEmpty()) Flow->SetArrayField(TEXT("warnings"), UE::UAI::Contract::ToPublicDiagnostics(ConsistencyWarnings, true));
				WorkState = Receipt.bStateKnown
					? (Receipt.TerminalStatus == TEXT("succeeded")
						? ELongRunningWorkState::ExitedSuccess
						: ELongRunningWorkState::ExitedFailure)
					: ELongRunningWorkState::StateUnknown;
			}

			void ReleaseSessionInputs(FUeAgentPieScenarioReceipt& Receipt, const TCHAR* FailureCode)
			{
				UE::UAI::Contract::FDiagnostic ReleaseError;
				if (!Registry->ReleaseInputs(Plan.CoreRequest.SessionIdentity, ReleaseError))
				{
					Receipt.bStateKnown = false;
					Receipt.CleanupDiagnostics.Add(UE::UAI::Contract::FDiagnostic::WrapFailure(FailureCode, ReleaseError, TEXT("cleanup"), TEXT("Session input cleanup failed.")));
				}
			}

			void CancelBeforeResume(const TCHAR* Reason = TEXT("pie_runtime_scenario_cancelled_before_resume"))
			{
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				UE::UAI::Contract::FDiagnostic Error;
				if (!Registry->TryResolveExact(Plan.CoreRequest.SessionIdentity,
					Plan.CoreRequest.ExpectedPauseEpoch, Plan.CoreRequest.ExpectedWorkspaceRevision, Snapshot, Error)
					|| !Snapshot.World.IsValid() || !Snapshot.World->IsPaused())
				{
					if (Error.IsEmpty()) Error = Native(TEXT("pie_runtime_scenario_snapshot_stale"),
						Snapshot.World.IsValid() ? TEXT("The scenario world is no longer paused.") : TEXT("The scenario world is unavailable."),
						TEXT("pie_scenario"), Snapshot.World.IsValid() ? TEXT("world_paused") : TEXT("world_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					SetTerminalNoSideEffects(TEXT("cancelled"), Contract::FDiagnostic::WrapFailure(
						Reason, Error, TEXT("pie_scenario"), TEXT("The scenario was cancelled before resuming; its paused snapshot could not be resolved.")));
					return;
				}
				// Cancel releases the session's held state without advancing paused gameplay.
				// Invalid flow parameters instead leave the existing session untouched.
				FUeAgentPieScenarioReceipt Receipt;
				Receipt.TerminalStatus = TEXT("cancelled");
				Receipt.Diagnostic = Native(Reason, TEXT("The scenario was cancelled before resuming."),
					TEXT("pie_scenario"), TEXT("cancelled"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				Receipt.SessionIdentity = Snapshot.SessionRef;
				Receipt.PauseEpoch = Snapshot.PauseEpoch;
				Receipt.WorkspaceRevision = Snapshot.WorkspaceRevision;
				Receipt.bPaused = true;
				ReleaseSessionInputs(Receipt, TEXT("pie_runtime_input_release_failed"));
				Receipt.bWorkspaceRefreshed = Registry->TryRefreshCurrentWorkspace(Snapshot.SessionRef,
					Snapshot.WorkspaceRevision, Receipt.WorkspaceRevision, Error);
				if (!Receipt.bWorkspaceRefreshed)
				{
					Receipt.bStateKnown = false;
					Receipt.CleanupDiagnostics.Add(Error);
				}
				{
					FScopeLock Lock(&Mutex);
					WorkspacePath = Snapshot.WorkspacePath;
				}
				StoreReceipt(Receipt);
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
					CancelBeforeResume();
					return;
				}
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				UE::UAI::Contract::FDiagnostic RegistryError;
				if (!Registry->TryResolveExact(
						Plan.CoreRequest.SessionIdentity,
						Plan.CoreRequest.ExpectedPauseEpoch,
						Plan.CoreRequest.ExpectedWorkspaceRevision,
						Snapshot,
						RegistryError)
					|| !Snapshot.World.IsValid()
					|| !Snapshot.World->IsPaused())
				{
					SetTerminalNoSideEffects(TEXT("stale"), RegistryError.IsEmpty() ? Native(TEXT("pie_runtime_scenario_snapshot_stale"),
						Snapshot.World.IsValid() ? TEXT("The scenario world is no longer paused.") : TEXT("The scenario world is unavailable."),
						TEXT("pie_scenario"), Snapshot.World.IsValid() ? TEXT("world_paused") : TEXT("world_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)) : RegistryError);
					return;
				}
				{
					FScopeLock Lock(&Mutex);
					WorkspacePath = Snapshot.WorkspacePath;
					ObservedPauseEpoch = Snapshot.PauseEpoch;
					ObservedWorkspaceRevision = Snapshot.WorkspaceRevision;
				}
				TArray<TSharedPtr<FJsonValue>> Refs = Plan.WatchRefs;
				TMap<FString, FString> ReferenceSources;
				ReferenceSources.Add(TEXT("/forget"), TEXT("/parameters/unwatch_refs"));
				for (int32 Index = 0; Index < Plan.WatchRefs.Num(); ++Index)
					ReferenceSources.Add(FString::Printf(TEXT("/refs/%d"), Index),
						FString::Printf(TEXT("/parameters/watch_refs/%d"), Index));
				for (const FTimelineStep& Step : Plan.Steps)
				{
					const auto Ref = MakeShared<FJsonObject>();
					if (Step.Type == ETimelineStepType::PropertyGet || Step.Type == ETimelineStepType::PropertyWrite)
					{
						if (!Registry->ValidatePropertyOperation(Snapshot.SessionRef,
							Step.ActorRef, Step.Property, Step.PropertyPath,
							Step.Type == ETimelineStepType::PropertyWrite ? Step.PropertyValue : TSharedPtr<FJsonValue>{},
							Step.bRawPropertyValue, RegistryError))
						{
						FailedOperation = Step.Index;
						BindPropertyStepInput(RegistryError, Step);
						SetTerminalNoSideEffects(TEXT("failed"), RegistryError);
						return;
						}
						Ref->SetStringField(TEXT("alias"), Step.ActorRef);
						Ref->SetStringField(TEXT("property"), Step.Property);
						Ref->SetArrayField(TEXT("path"), Step.PropertyPath);
						const FString RefPointer = FString::Printf(TEXT("/refs/%d"), Refs.Num());
						for (const FString& Field : { FString(TEXT("alias")), FString(TEXT("property")), FString(TEXT("path")) })
							ReferenceSources.Add(RefPointer + TEXT("/") + Field, Step.PropertyAddressPointer + TEXT("/") + Field);
					}
					else if (!Step.CaptureSpec.CameraRef.IsEmpty())
					{
						Ref->SetStringField(TEXT("alias"), Step.CaptureSpec.CameraRef);
						ReferenceSources.Add(FString::Printf(TEXT("/refs/%d/alias"), Refs.Num()), StepPointer(Step.Index) + TEXT("/camera"));
					}
					if (!Ref->Values.IsEmpty()) Refs.Add(MakeShared<FJsonValueObject>(Ref));
				}
				if (!Registry->TrackReferences(Snapshot.SessionRef, Refs, Plan.ForgetRefs, RegistryError))
				{
					RemapArgumentInput(RegistryError, ReferenceSources);
					SetTerminalNoSideEffects(TEXT("failed"), RegistryError);
					return;
				}
				if (!ResolveInputActions(Snapshot, RegistryError))
				{
					SetTerminalNoSideEffects(TEXT("failed"), RegistryError);
					return;
				}
				bCancelled = Cancellation.IsCancelled();
				{
					FScopeLock Lock(&Mutex);
					bCancelled = bCancelled || bStopRequested;
				}
				if (bCancelled)
				{
					CancelBeforeResume();
					return;
				}
				CPUThrottleExemption = MakeUnique<EditorRuntime::FScopedCPUThrottleExemption>();
				CPUThrottleStartFrame = GFrameCounter;
				const TWeakPtr<FScenarioSharedState, ESPMode::ThreadSafe> WeakSelf = AsShared();
				TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda([WeakSelf](float DeltaSeconds)
					{
						const TSharedPtr<FScenarioSharedState, ESPMode::ThreadSafe> Self = WeakSelf.Pin();
						return Self.IsValid() && Self->Tick(DeltaSeconds);
					}));
				if (!TickerHandle.IsValid())
				{
					SetTerminalNoSideEffects(TEXT("failed"), Native(TEXT("pie_runtime_scenario_dispatch_failed"), TEXT("The scenario dispatch ticker could not be registered."), TEXT("pie_scenario"),
					TEXT("ticker_registered"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				}
			}

			bool StartRunningOnGameThread()
			{
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				UE::UAI::Contract::FDiagnostic RegistryError;
				if (!Registry->TryResolveExact(
						Plan.CoreRequest.SessionIdentity,
						Plan.CoreRequest.ExpectedPauseEpoch,
						Plan.CoreRequest.ExpectedWorkspaceRevision,
						Snapshot,
						RegistryError)
					|| !Snapshot.World.IsValid() || !Snapshot.World->IsPaused())
				{
					SetTerminalNoSideEffects(TEXT("stale"), RegistryError.IsEmpty() ? Native(TEXT("pie_runtime_scenario_snapshot_stale"),
						Snapshot.World.IsValid() ? TEXT("The scenario world is no longer paused.") : TEXT("The scenario world is unavailable."),
						TEXT("pie_scenario"), Snapshot.World.IsValid() ? TEXT("world_paused") : TEXT("world_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)) : RegistryError);
					return false;
				}
				if (!Resume(RegistryError))
				{
					if (Snapshot.World->IsPaused())
					{
						SetTerminalNoSideEffects(TEXT("failed"), RegistryError.IsEmpty()
							? Native(TEXT("pie_runtime_scenario_resume_failed"), TEXT("The resume operation failed without a diagnostic."), TEXT("pie_scenario"),
					TEXT("diagnostic_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)) : RegistryError);
					}
					else
					{
						StoreReceipt(FinalizePieRuntimeScenario(
							Plan.CoreRequest,
							EUeAgentPieScenarioStepResult::Failed,
                            *this, RegistryError));
					}
					return false;
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
				return true;
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
						if (Self->TickerHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(Self->TickerHandle);
						Self->TickerHandle.Reset();
						Self->CancelBeforeResume(TEXT("pie_runtime_scenario_destroyed_before_resume"));
						return;
					}
					if (!Self->PendingReceipt.IsSet())
					{
						Self->ReleaseCaptureSurfaces();
						Self->CloseCollections(TEXT("interrupted"));
						Self->PendingReceipt = FinalizePieRuntimeScenario(Self->Plan.CoreRequest, EUeAgentPieScenarioStepResult::Cancelled, *Self, Self->StepError);
					}
					if (Self->TickerHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(Self->TickerHandle);
					// The port can disappear before GPU/encoding work finishes. Keep the drain alive.
					Self->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
						[Self](float) { Self->PollCaptures(); return Self->FinishPendingReceipt(); }));
				});
			}

			bool CapturePausedObservations(const FUeAgentPieScenarioReceipt& Receipt, UE::UAI::Contract::FDiagnostic& OutError) override
            {
				if (!Receipt.bStateKnown)
				{
					OutError = Native(TEXT("scenario_observation_state_unknown"), TEXT("Paused observations require a known scenario state."), TEXT("pie_observation"),
					TEXT("state_known"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
                FUeAgentPieRuntimeDebugSnapshot Snapshot;
                if (!Registry->TryResolveExact(Plan.CoreRequest.SessionIdentity, Receipt.PauseEpoch, Receipt.WorkspaceRevision, Snapshot, OutError)) return false;
				if (!Snapshot.World.IsValid() || !Snapshot.World->IsPaused())
				{
					OutError = Native(TEXT("scenario_observation_not_paused"),
						Snapshot.World.IsValid() ? TEXT("The observation world is not paused.") : TEXT("The observation world is unavailable."),
						TEXT("pie_observation"), Snapshot.World.IsValid() ? TEXT("world_paused") : TEXT("world_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
                return CaptureObservations(Snapshot, OutError);
            }

			bool ResolveInputActions(
				const FUeAgentPieRuntimeDebugSnapshot& Snapshot, Contract::FDiagnostic& OutError)
			{
				SessionInput = Registry->GetInputState(Snapshot.SessionRef);
				if (!SessionInput)
				{
					OutError = Native(TEXT("pie_runtime_scenario_input_action_binding_invalid"),
						TEXT("The scenario session input state is unavailable."), TEXT("pie_scenario"),
						TEXT("session_input_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				ULocalPlayer* LocalPlayer = Snapshot.LocalPlayer.Get();
				EnhancedInputSubsystem = LocalPlayer
					? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer)
					: nullptr;
				EnhancedPlayerInput = EnhancedInputSubsystem.IsValid()
					? EnhancedInputSubsystem->GetPlayerInput() : nullptr;
				for (const auto& Existing : SessionInput->Actions)
				{
					if (!Existing.Key.IsValid())
					{
						OutError = Native(TEXT("pie_runtime_scenario_input_action_binding_invalid"),
							TEXT("A previously held input action is no longer available."), TEXT("pie_scenario"),
							TEXT("held_action_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						return false;
					}
					const FString Ref = Existing.Key->GetPathName();
					ResolvedInputActions.Add(Ref, Existing.Key);
					InputActionLedger.ApplySample({Ref, 0.0, Existing.Value.GetValueType(), Existing.Value.Get<FVector>()});
				}
				for (FTimelineStep& Step : Plan.Steps)
				{
					if (Step.Type != ETimelineStepType::InputAction) continue;
					const UInputAction* Action = LoadObject<UInputAction>(nullptr, *Step.ActionRef);
					if (!Action)
						OutError = StepInput(TEXT("pie_runtime_scenario_input_action_binding_invalid"),
							TEXT("The input action reference could not be loaded as an InputAction asset."), Step.Index,
							TEXT("/action_ref"), MakeShared<FJsonValueString>(Step.ActionRef),
							MakeShared<FJsonValueString>(TEXT("loadable InputAction asset reference")));
					else if (Action->GetPathName() != Step.ActionRef)
						OutError = StepInput(TEXT("pie_runtime_scenario_input_action_binding_invalid"),
							TEXT("The input action reference does not match the loaded asset path."), Step.Index,
							TEXT("/action_ref"), MakeShared<FJsonValueString>(Step.ActionRef),
							MakeShared<FJsonValueString>(Action->GetPathName()));
					else if (Action->ValueType != Step.ActionValueType)
						OutError = StepInput(TEXT("pie_runtime_scenario_input_action_binding_invalid"),
							TEXT("The input sample type does not match the input action value type."), Step.Index,
							TEXT("/value"), MakeShared<FJsonValueString>(InputActionTypeName(Step.ActionValueType)),
							MakeShared<FJsonValueString>(InputActionTypeName(Action->ValueType)));
					else if (!EnhancedInputSubsystem.IsValid())
						OutError = Native(TEXT("pie_runtime_scenario_input_action_binding_invalid"),
							TEXT("The local player's Enhanced Input subsystem is unavailable."), TEXT("pie_scenario"),
							TEXT("enhanced_input_subsystem_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					else if (!EnhancedPlayerInput.IsValid())
						OutError = Native(TEXT("pie_runtime_scenario_input_action_binding_invalid"),
							TEXT("The Enhanced Input player input instance is unavailable."), TEXT("pie_scenario"),
							TEXT("enhanced_player_input_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					else
					{
						Step.Action = Action;
						ResolvedInputActions.Add(Step.ActionRef, Action);
						if (!InitialActionRefs.Contains(Action)) InitialActionRefs.Add(Action);
						continue;
					}
					OutError.OperationIndex = Step.Index;
					OutError.TargetPath = Step.ActionRef;
					return false;
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

			bool InjectActiveActionSamples(UE::UAI::Contract::FDiagnostic& OutError)
			{
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				
				if (!Registry->TryResolveExact(Plan.CoreRequest.SessionIdentity, Plan.CoreRequest.ExpectedPauseEpoch,
					Plan.CoreRequest.ExpectedWorkspaceRevision, Snapshot, OutError)) return false;
				if (APlayerController* Controller = Snapshot.PlayerController.Get())
					for (const auto& Axis : SessionInput->Axes)
						if (!SessionInput->Key(Snapshot, Axis.Key, IE_Axis, Axis.Value, OutError)) return false;
				if (!EnhancedInputSubsystem.IsValid())
				{
					if (InputActionLedger.GetActiveSamples().IsEmpty()) return true;
					OutError = Native(TEXT("pie_runtime_input_subsystem_unavailable"),
						TEXT("The input subsystem is unavailable while action samples are active."), TEXT("pie_scenario"),
						TEXT("enhanced_input_subsystem_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				for (const TPair<FString, FUeAgentPieInputActionSample>& Pair
					: InputActionLedger.GetActiveSamples())
				{
					const TWeakObjectPtr<const UInputAction>* Action =
						ResolvedInputActions.Find(Pair.Key);
					if (!Action || !Action->IsValid())
					{
						OutError = Native(TEXT("pie_runtime_input_action_unavailable"),
							TEXT("The active input action is no longer available."), TEXT("pie_scenario"),
							TEXT("input_action_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.TargetPath = Pair.Key;
						return false;
					}
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

			void ReleaseCaptureSurfaces()
			{
				bCaptureSurfacesReleased = true;
				for (auto& Pool : IdleCaptureSurfaces)
					for (auto& Surface : Pool.Value) Surface->Stop();
				IdleCaptureSurfaces.Reset();
				for (auto& Pending : PendingCaptures)
					if (Pending.Surface) Pending.Surface->Stop();
				for (auto& Collection : Collections)
				{
					if (Collection.Surface) Collection.Surface->Stop();
					Collection.Surface.Reset();
				}
			}

			void PollCaptures()
			{
				for (int32 Index = PendingCaptures.Num() - 1; Index >= 0; --Index)
				{
					auto& Pending = PendingCaptures[Index];
					if (Pending.Readback)
					{
						Pending.Readback->Poll();
						TArray<FColor> Pixels;
						FIntPoint Size;
						UE::UAI::Contract::FDiagnostic Error;
						if (!Pending.Readback->Take(Pixels, Size, Error)) continue;
						Pending.Readback.Reset();
						if (Pending.Collection == INDEX_NONE && Pending.Surface && !bCaptureSurfacesReleased)
							IdleCaptureSurfaces.FindOrAdd(Pending.SurfaceKey).Add(MoveTemp(Pending.Surface));
						else Pending.Surface.Reset();
						Pending.Item->SetNumberField(TEXT("readback_seconds"), FPlatformTime::Seconds() - Pending.Started);
						if (Error.IsEmpty())
							Pending.Publication = CaptureMediaLongJob::PublishCaptureImageAsync(ImageArtifactService,
								OperationId, Pending.SourceIdentity, TEXT("pie-runtime-scenario:") + OperationId,
								MoveTemp(Pixels), Size, Pending.Collection != INDEX_NONE);
						else Pending.Diagnostic = MoveTemp(Error);
					}
					if (Pending.Publication.IsValid() && !Pending.Publication.IsReady()) continue;
					UE::UAI::Contract::FDiagnostic Error = Pending.Diagnostic;
					if (Pending.Publication.IsValid())
					{
						auto Published = Pending.Publication.Get();
						Error.Append(Published.Error);
						if (!Published.CleanupError.IsEmpty())
						{
							CaptureCleanupDiagnostics.Add(Published.CleanupError);
							Pending.Item->SetObjectField(TEXT("cleanup_error"), Published.CleanupError.ToPublicJson());
						}
						FUeAgentResourceChargeError ChargeError;
						CaptureResources.TryChargeMany(Published.Charges, TEXT("capture_async_publication"), ChargeError);
						Pending.Item->SetNumberField(TEXT("processing_seconds"), Published.ProcessingSeconds);
						if (Error.IsEmpty()) Pending.Item->SetField(TEXT("artifact_ref"), Published.Artifact);
						if (Pending.Collection != INDEX_NONE && Error.IsEmpty())
						{
							auto& Collection = Collections[Pending.Collection];
							if (Published.PreviewError.IsEmpty() && (Collection.PreviewFrames.IsEmpty()
								|| Published.PreviewSize == Collection.PreviewSize))
							{
								Collection.PreviewSize = Published.PreviewSize;
								const int32 Sample = Pending.Item->GetIntegerField(TEXT("sample_index"));
								int32 Position = 0;
								while (Collection.PreviewSampleIndices.IsValidIndex(Position)
									&& Collection.PreviewSampleIndices[Position] < Sample) ++Position;
								Collection.PreviewSampleIndices.Insert(Sample, Position);
								Collection.PreviewFrames.Insert(MoveTemp(Published.Preview), Position);
								Collection.PreviewLabels.Insert(FString::Printf(TEXT("F%d %.3fs"),
									Sample, Pending.Item->GetNumberField(TEXT("game_seconds"))), Position);
							}
							else
							{
								Collection.PreviewError = Published.PreviewError.IsEmpty()
									? Native(TEXT("pie_frame_sheet_dimensions_changed"),
										TEXT("Capture preview frame dimensions changed within the collection."), TEXT("capture_preview"),
										TEXT("preview_size"), PieDiagnostics::Size(Published.PreviewSize), PieDiagnostics::Size(Collection.PreviewSize))
									: Published.PreviewError;
								if (Collection.PreviewError.TargetPath.IsEmpty())
									Collection.PreviewError.TargetPath = Collection.CaptureStep.CollectionName;
							}
						}
					}
					Pending.Item->SetStringField(TEXT("status"), Error.IsEmpty() ? TEXT("succeeded") : TEXT("failed"));
					if (!Error.IsEmpty())
					{
						Pending.Item->SetObjectField(TEXT("error"), Error.ToPublicJson());
						bCaptureFailed = true;
						if (PendingReceipt.IsSet()) PendingReceipt->Diagnostic.Append(Error);
                        else StepError.Append(Error);
						FailedOperation = Pending.Item->GetIntegerField(TEXT("operation"));
						if (Pending.Collection != INDEX_NONE)
						{
							auto& Collection = Collections[Pending.Collection];
							++Collection.FailedFrames;
							FailedCollection = Collection.CaptureStep.CollectionName;
							const auto Gap = MakeShared<FJsonObject>();
							Gap->Values = Pending.Item->Values;
							Gap->SetStringField(TEXT("reason"), TEXT("capture_failed"));
							Collection.Gaps.Add(MakeShared<FJsonValueObject>(Gap));
							Collection.Frames.RemoveAll([&Pending](const TSharedPtr<FJsonValue>& Value)
								{ return Value->AsObject() == Pending.Item; });
						}
					}
					PendingCaptures.RemoveAt(Index);
				}
			}

			bool FinishPendingReceipt()
			{
				bool bCancelled;
				{
					FScopeLock Lock(&Mutex);
					bCancelled = bStopRequested || Cancellation.IsCancelled();
				}
				if (bCancelled && PendingReceipt->TerminalStatus == TEXT("succeeded"))
				{
					PendingReceipt->TerminalStatus = TEXT("cancelled");
					PendingReceipt->Diagnostic = Native(TEXT("pie_runtime_scenario_cancelled"),
						TEXT("The scenario was cancelled while awaiting capture completion."), TEXT("pie_scenario"),
						TEXT("cancelled"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					ReleaseSessionInputs(PendingReceipt.GetValue(), TEXT("pie_capture_cancel_input_release_failed"));
				}
				if (!PendingCaptures.IsEmpty()) return true;
				for (auto& Collection : Collections)
					if (Collection.ClosedResult) FinishCollection(Collection);
				auto Receipt = PendingReceipt.GetValue();
				if (!CaptureCleanupDiagnostics.IsEmpty())
				{
					Receipt.bStateKnown = false;
					Receipt.CleanupDiagnostics.Append(CaptureCleanupDiagnostics);
				}
				if (bCaptureFailed && Receipt.TerminalStatus == TEXT("succeeded"))
				{
					Receipt.TerminalStatus = TEXT("failed");
					if (Receipt.Diagnostic.IsEmpty()) Receipt.Diagnostic = StepError;
					ReleaseSessionInputs(Receipt, TEXT("pie_capture_failure_input_release_failed"));
				}
				StoreReceipt(Receipt);
				TickerHandle.Reset();
				return false;
			}

			bool Tick(float)
			{
				check(IsInGameThread());
				EScenarioDispatchPhase CurrentPhase;
				bool bStopAtStartup;
				{
					FScopeLock Lock(&Mutex);
					CurrentPhase = Phase;
					bStopAtStartup = bStopRequested;
				}
				if (CurrentPhase == EScenarioDispatchPhase::Terminal)
				{
					TickerHandle.Reset();
					return false;
				}
				if (CurrentPhase == EScenarioDispatchPhase::Scheduled)
				{
					if (bStopAtStartup || Cancellation.IsCancelled())
					{
						CancelBeforeResume();
						TickerHandle.Reset();
						return false;
					}
					// CoreTicker runs after World Tick. Keep PIE paused until a fresh
					// unthrottled engine frame, then dispatch time-zero steps here before
					// the first resumed World Tick can consume a stale/background delta.
					if (GFrameCounter <= CPUThrottleStartFrame) return true;
					if (!StartRunningOnGameThread())
					{
						TickerHandle.Reset();
						return false;
					}
				}
				PollCaptures();
				if (PendingReceipt.IsSet()) return FinishPendingReceipt();
				ObserveInjectedActionValues();
				EUeAgentPieScenarioStepResult Outcome = EUeAgentPieScenarioStepResult::Succeeded;
				bool bTerminal = bCaptureFailed;
				if (bCaptureFailed) Outcome = EUeAgentPieScenarioStepResult::Failed;
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
					if (StepError.IsEmpty()) StepError = Native(TEXT("pie_runtime_scenario_cancelled"),
						TEXT("The running scenario was cancelled."), TEXT("pie_scenario"),
						TEXT("cancelled"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				}
				FUeAgentPieRuntimeDebugSnapshot Snapshot;
				UE::UAI::Contract::FDiagnostic Error;
				if (!bTerminal && !Registry->TryResolveExact(
						Plan.CoreRequest.SessionIdentity,
						Plan.CoreRequest.ExpectedPauseEpoch,
						Plan.CoreRequest.ExpectedWorkspaceRevision,
						Snapshot,
						Error))
				{
					StepError = Error;
					bTerminal = true;
					Outcome = EUeAgentPieScenarioStepResult::Failed;
				}
				const double WallElapsed = FPlatformTime::Seconds() - StartWallSeconds;
				const double GameElapsed = Snapshot.World.IsValid()
					? Snapshot.World->GetTimeSeconds() - StartGameSeconds
					: LastGameSeconds;
				const int64 FramesElapsed = static_cast<int64>(GFrameCounter - StartFrame);
				LastGameSeconds = GameElapsed;
				if (!bTerminal
					&& ((Plan.CoreRequest.WallTimeLimitSeconds > 0.0 && WallElapsed > Plan.CoreRequest.WallTimeLimitSeconds)
						|| (Plan.CoreRequest.GameTimeLimitSeconds > 0.0 && GameElapsed > Plan.CoreRequest.GameTimeLimitSeconds)
						|| (Plan.CoreRequest.FrameLimit > 0 && FramesElapsed > Plan.CoreRequest.FrameLimit)))
				{
					bTerminal = true;
					Outcome = EUeAgentPieScenarioStepResult::TimedOut;
					if (Plan.CoreRequest.WallTimeLimitSeconds > 0.0 && WallElapsed > Plan.CoreRequest.WallTimeLimitSeconds)
						StepError = Native(TEXT("pie_runtime_scenario_timed_out"),
							TEXT("The scenario exceeded its wall time limit."), TEXT("pie_scenario"),
							TEXT("wall_seconds"), Number(WallElapsed), Number(Plan.CoreRequest.WallTimeLimitSeconds));
					else if (Plan.CoreRequest.GameTimeLimitSeconds > 0.0 && GameElapsed > Plan.CoreRequest.GameTimeLimitSeconds)
						StepError = Native(TEXT("pie_runtime_scenario_timed_out"),
							TEXT("The scenario exceeded its game time limit."), TEXT("pie_scenario"),
							TEXT("game_seconds"), Number(GameElapsed), Number(Plan.CoreRequest.GameTimeLimitSeconds));
					else
						StepError = Native(TEXT("pie_runtime_scenario_timed_out"),
							TEXT("The scenario exceeded its frame limit."), TEXT("pie_scenario"),
							TEXT("frames"), Number(static_cast<double>(FramesElapsed)), Number(static_cast<double>(Plan.CoreRequest.FrameLimit)));
				}
				while (!bTerminal)
				{
					FCaptureCollection* Due = nullptr;
					for (FCaptureCollection& Collection : Collections)
					{
						if (Collection.bOpen && Collection.NextAt <= GameElapsed
							&& Collection.NextAt <= RuntimeEndGameSeconds
							&& (!Due || Collection.NextAt < Due->NextAt)) Due = &Collection;
					}
					const bool bStepDue = !ScheduledSteps.IsEmpty() && ScheduledSteps[0].AtSeconds <= GameElapsed;
					if (Due && (!bStepDue || Due->NextAt < ScheduledSteps[0].AtSeconds))
					{
						if (!SampleCollection(*Due, Snapshot))
						{
							FailedOperation = Due->CaptureStep.Index;
							FailedCollection = Due->CaptureStep.CollectionName;
							bTerminal = true;
							Outcome = EUeAgentPieScenarioStepResult::Failed;
							break;
						}
						continue;
					}
					if (!bStepDue) break;
					FScheduledStep Scheduled;
					ScheduledSteps.HeapPop(Scheduled, EAllowShrinking::No);
					const FTimelineStep& Step = Plan.Steps[Scheduled.PlanIndex];
					if (Scheduled.bFinishWait)
					{
						TimingByOperation[Step.Index]->SetNumberField(TEXT("completed_ms"), GameElapsed * 1000.0);
						CompleteStep(Scheduled.PlanIndex, GameElapsed);
						continue;
					}
					UeAgentAuthoringDiagnostics::FScopedWarnings StepWarnings(false);
					const bool bStepApplied = ApplyStep(Step, Snapshot);
					for (auto Warning : StepWarnings.GetWarnings()) {
						Warning.OperationIndex = Step.Index;
						ConsistencyWarnings.Add(MoveTemp(Warning));
					}
					if (!bStepApplied)
					{
						const FTimelineStep& Failed = Step;
						FailedOperation = Failed.Index;
						if (!StepError.OperationIndex.IsSet()) StepError.OperationIndex = Failed.Index;
						if (Failed.Type == ETimelineStepType::Capture || Failed.Type == ETimelineStepType::PropertyGet)
						{
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("type"), Failed.Type == ETimelineStepType::Capture ? TEXT("capture") : TEXT("property_get"));
							Item->SetStringField(TEXT("status"), TEXT("failed"));
							Item->SetNumberField(TEXT("operation"), Failed.Index);
							Item->SetNumberField(TEXT("game_seconds"), GameElapsed);
							Item->SetObjectField(TEXT("error"), StepError.ToPublicJson());
							if (Failed.Type == ETimelineStepType::Capture) AddCaptureContext(Failed.CaptureSpec, Item);
							FScopeLock Lock(&Mutex);
							Results.Add(MakeShared<FJsonValueObject>(Item));
						}
						bTerminal = true;
						Outcome = EUeAgentPieScenarioStepResult::Failed;
						break;
					}
					const auto Timing = MakeShared<FJsonObject>();
					Timing->SetNumberField(TEXT("operation"), Step.Index);
					Timing->SetNumberField(TEXT("game_ms"), GameElapsed * 1000.0);
					TimingByOperation[Step.Index] = Timing;
					OperationTimes.Add(MakeShared<FJsonValueObject>(Timing));
					if (Step.Type == ETimelineStepType::Wait)
					{
						Timing->SetNumberField(TEXT("wait_ms"), Step.WaitSeconds * 1000.0);
						const double Deadline = GameElapsed + Step.WaitSeconds;
						RuntimeEndGameSeconds = FMath::Max(RuntimeEndGameSeconds, Deadline);
						ScheduledSteps.HeapPush({Deadline, Scheduled.PlanIndex, Step.Index, true});
					}
					else CompleteStep(Scheduled.PlanIndex, GameElapsed);
				}
				if (!bTerminal && !InjectActiveActionSamples(StepError))
				{
					if (StepError.IsEmpty()) StepError = Native(TEXT("pie_input_continuous_sample_failed"),
						TEXT("Continuous input sampling failed without a diagnostic."), TEXT("pie_scenario"),
						TEXT("diagnostic_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					bTerminal = true;
					Outcome = EUeAgentPieScenarioStepResult::Failed;
				}
				if (!bTerminal && CompletedOperations.Num() == Plan.Steps.Num() && GameElapsed >= RuntimeEndGameSeconds)
				{
					bTerminal = true;
					Outcome = EUeAgentPieScenarioStepResult::Succeeded;
				}
				if (!bTerminal) return true;
				if (Outcome != EUeAgentPieScenarioStepResult::Succeeded && !bActionReleaseBarrier && InputActionLedger.HasInjectedNonZero())
				{
					BeginActionReleaseBarrier(Outcome);
					return true;
				}

				ReleaseCaptureSurfaces();
				CloseCollections(Outcome == EUeAgentPieScenarioStepResult::Succeeded ? TEXT("flow_end") : TEXT("interrupted"));
				FUeAgentPieScenarioReceipt Receipt =
					FinalizePieRuntimeScenario(Plan.CoreRequest, Outcome, *this, StepError);
				PendingReceipt = Receipt;
				return FinishPendingReceipt();
			}

			void CompleteStep(int32 PlanIndex, double GameElapsed)
			{
				const FTimelineStep& Step = Plan.Steps[PlanIndex];
				CompletedSteps[PlanIndex] = true;
				CompletedOperations.Add(Step.Index);
				if (const auto* Following = StepsAfterWait.Find(Step.Index))
				{
					for (int32 Index : *Following)
						ScheduledSteps.HeapPush({GameElapsed, Index, Plan.Steps[Index].Index, false});
				}
			}

			bool SampleCollection(FCaptureCollection& Collection, const FUeAgentPieRuntimeDebugSnapshot& Snapshot)
			{
				const double Interval = Collection.CaptureStep.CaptureInterval;
				int64 Missed = FMath::FloorToInt64(FMath::Max(0.0, FMath::Min(LastGameSeconds, RuntimeEndGameSeconds) - Collection.NextAt) / Interval);
				for (const FScheduledStep& Scheduled : ScheduledSteps)
				{
					const auto& Pending = Plan.Steps[Scheduled.PlanIndex];
					if (Pending.Type == ETimelineStepType::CaptureStop && Pending.CollectionName == Collection.CaptureStep.CollectionName)
					{
						Missed = FMath::Min(Missed, FMath::Max<int64>(0, FMath::CeilToInt64((Scheduled.AtSeconds - Collection.NextAt) / Interval) - 1));
					}
				}
				if (Missed > 0)
				{
					const auto Gap = MakeShared<FJsonObject>();
					Gap->SetStringField(TEXT("reason"), TEXT("sampling_delayed"));
					Gap->SetNumberField(TEXT("first_sample_index"), Collection.NextSampleIndex);
					Gap->SetNumberField(TEXT("count"), Missed);
					Gap->SetNumberField(TEXT("from_seconds"), Collection.NextAt);
					Gap->SetNumberField(TEXT("through_seconds"), Collection.NextAt + (Missed - 1) * Interval);
					Collection.Gaps.Add(MakeShared<FJsonValueObject>(Gap));
					Collection.MissedFrames += Missed;
					Collection.NextSampleIndex += Missed;
				}
				const int32 CollectionIndex = static_cast<int32>(&Collection - Collections.GetData());
				int32 InFlight = 0;
				for (const auto& Pending : PendingCaptures)
					if (Pending.Collection == CollectionIndex)
						++InFlight;
				// Each submission copies into its own readback before this surface's next draw.
				// Either bounded slot may await readback or publication without serializing samples.
				// Backpressure reports a gap instead of growing an unbounded full-resolution queue.
				if (InFlight >= 2)
				{
					const auto Gap = MakeShared<FJsonObject>();
					Gap->SetStringField(TEXT("reason"), TEXT("capture_in_flight"));
					Gap->SetNumberField(TEXT("first_sample_index"), Collection.NextSampleIndex);
					Gap->SetNumberField(TEXT("count"), 1);
					Gap->SetNumberField(TEXT("from_seconds"), Collection.StartedAt + Collection.NextSampleIndex * Interval);
					Gap->SetNumberField(TEXT("through_seconds"), Collection.StartedAt + Collection.NextSampleIndex * Interval);
					Collection.Gaps.Add(MakeShared<FJsonValueObject>(Gap));
					++Collection.MissedFrames;
					++Collection.NextSampleIndex;
					Collection.NextAt = Collection.StartedAt + Collection.NextSampleIndex * Interval;
					return true;
				}
				const double Scheduled = Collection.StartedAt + Collection.NextSampleIndex * Interval;
				const bool bCaptured = ApplyStep(Collection.CaptureStep, Snapshot, &Collection.Frames);
				if (bCaptured)
				{
					const auto Frame = Collection.Frames.Last()->AsObject();
					Frame->SetNumberField(TEXT("sample_index"), Collection.NextSampleIndex);
					Frame->SetNumberField(TEXT("scheduled_seconds"), Scheduled);
				}
				else
				{
					++Collection.FailedFrames;
					const auto Gap = MakeShared<FJsonObject>();
					Gap->SetStringField(TEXT("reason"), TEXT("capture_failed"));
					Gap->SetObjectField(TEXT("error"), StepError.ToPublicJson());
					Gap->SetNumberField(TEXT("first_sample_index"), Collection.NextSampleIndex);
					Gap->SetNumberField(TEXT("count"), 1);
					Gap->SetNumberField(TEXT("scheduled_seconds"), Scheduled);
					Gap->SetNumberField(TEXT("game_seconds"), LastGameSeconds);
					Collection.Gaps.Add(MakeShared<FJsonValueObject>(Gap));
					FailedCollection = Collection.CaptureStep.CollectionName;
				}
				++Collection.NextSampleIndex;
				Collection.NextAt = Collection.StartedAt + Collection.NextSampleIndex * Interval;
				return bCaptured;
			}

			void CloseCollection(FCaptureCollection& Collection, const FString& Reason, int32 OperationIndex)
			{
				Collection.bOpen = false;
				Collection.CloseReason = Reason;
				Collection.CloseOperation = OperationIndex;
				Collection.ClosedSeconds = LastGameSeconds;
				Collection.ClosedResult = MakeShared<FJsonObject>();
				Results.Add(MakeShared<FJsonValueObject>(Collection.ClosedResult));
			}

			void FinishCollection(FCaptureCollection& Collection)
			{
				const auto Item = Collection.ClosedResult.ToSharedRef();
				const FString& Reason = Collection.CloseReason;
				const int32 OperationIndex = Collection.CloseOperation;
				Item->SetStringField(TEXT("type"), TEXT("capture_collection"));
				AddCaptureContext(Collection.CaptureStep.CaptureSpec, Item);
				Item->SetStringField(TEXT("name"), Collection.CaptureStep.CollectionName);
				Item->SetStringField(TEXT("status"), Reason == TEXT("interrupted") || Collection.MissedFrames > 0 || Collection.FailedFrames > 0 ? TEXT("partial") : TEXT("succeeded"));
				Item->SetStringField(TEXT("close_reason"), Reason);
				Item->SetNumberField(TEXT("operation"), OperationIndex);
				Item->SetNumberField(TEXT("started_seconds"), Collection.StartedAt);
				Item->SetNumberField(TEXT("game_seconds"), Collection.ClosedSeconds);
				Item->SetNumberField(TEXT("interval"), Collection.CaptureStep.CaptureInterval);
				Item->SetArrayField(TEXT("frames"), Collection.Frames);
				Item->SetNumberField(TEXT("frame_count"), Collection.Frames.Num());
				Item->SetNumberField(TEXT("missed_frames"), Collection.MissedFrames);
				Item->SetNumberField(TEXT("failed_frames"), Collection.FailedFrames);
				Item->SetArrayField(TEXT("gaps"), Collection.Gaps);
				if (!Collection.PreviewError.IsEmpty()) Item->SetObjectField(TEXT("overview_error"), Collection.PreviewError.ToPublicJson());
				if (!Collection.PreviewFrames.IsEmpty() && Collection.PreviewError.IsEmpty())
				{
					FString Path;
                    UE::UAI::Contract::FDiagnostic Error;
					TSharedPtr<FJsonObject> Sheet;
					const auto Params = MakeShared<FJsonObject>();
					if (CaptureMediaLongJob::FCaptureImageProcessing::WriteFrameSequenceSheetImage(
						Collection.PreviewFrames, Collection.PreviewLabels, Collection.PreviewSize, Params, TEXT("png"), -1, Path, Sheet, Error))
					{
						CaptureMediaLongJob::FUeAgentStagedCaptureImage Image;
						FExecutionMutationError PublishError;
						FFileArtifactReference Published;
						FString Evidence;
						FUeAgentCanonicalValuePtr Value;
						FFileArtifactPublishError ProjectionError;
						TSharedPtr<FJsonValue> Json;
						if (ImageArtifactService->TryAdoptStagedImage(OperationId, TEXT("pie-frame-sheet"), Path, FString(), TEXT("png"), Image, PublishError)
							&& ImageArtifactService->TryPublishVerified(OperationId, TEXT("pie-runtime-scenario:") + OperationId, Image, CaptureResources, Published, Evidence, PublishError)
							&& TryProjectFileArtifactReference(Published, CaptureResources, Value, ProjectionError)
							&& CanonicalJson::TryToJsonValue(*Value, Json, Error))
						{
							Sheet->RemoveField(TEXT("file_path"));
							Sheet->SetField(TEXT("artifact_ref"), Json);
							Item->SetObjectField(TEXT("overview"), Sheet);
						}
						else {
                            if (!PublishError.IsEmpty()) Error = PublishError;
                            else if (!ProjectionError.IsEmpty()) Error = ProjectionError;
                            else if (Error.IsEmpty()) Error = Native(TEXT("pie_frame_sheet_publish_failed"),
								TEXT("A frame overview publication step failed without a diagnostic."), TEXT("capture_publish"),
								TEXT("diagnostic_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                        }
					}
					// This path is produced by this collection, including a partial write
					// or an image that failed adoption before a staged handle existed.
					if (!CaptureMediaLongJob::DiscardProducedCaptureFile(Path))
					{
						auto CleanupError = Native(TEXT("capture_staged_cleanup_failed"),
							TEXT("The frame overview staging file could not be removed."), TEXT("cleanup"),
							TEXT("staged_file_removed"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						CleanupError.InternalContext = Path;
						CleanupError.TargetPath = Collection.CaptureStep.CollectionName;
						CaptureCleanupDiagnostics.Add(CleanupError);
						Item->SetObjectField(TEXT("cleanup_error"), CleanupError.ToPublicJson());
					}
					if (!Error.IsEmpty()) Item->SetObjectField(TEXT("overview_error"), Error.ToPublicJson());
					Collection.PreviewFrames.Reset();
				}
			}

			void CloseCollections(const FString& Reason)
			{
				for (FCaptureCollection& Collection : Collections)
					if (Collection.bOpen) CloseCollection(Collection, Reason, Collection.CaptureStep.Index);
			}

			bool ApplyStep(
				const FTimelineStep& Step,
				const FUeAgentPieRuntimeDebugSnapshot& Snapshot,
				TArray<TSharedPtr<FJsonValue>>* CaptureFrames = nullptr)
			{
				StepError.Reset();
				if (Step.Type == ETimelineStepType::Wait) return true;
				if (Step.Type == ETimelineStepType::CaptureStart)
				{
					if (Collections.ContainsByPredicate([&Step](const FCaptureCollection& Collection) {
						return Collection.bOpen && Collection.CaptureStep.CollectionName == Step.CollectionName;
					}))
					{
					StepError = Native(TEXT("pie_runtime_capture_collection_already_open"), TEXT("A capture collection with this name is already open."), TEXT("pie_scenario"),
						TEXT("collection_open"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					StepError.TargetPath = Step.CollectionName;
						return false;
					}
					FCaptureCollection& Collection = Collections.AddDefaulted_GetRef();
					Collection.CaptureStep = Step;
					Collection.CaptureStep.Type = ETimelineStepType::Capture;
					Collection.StartedAt = LastGameSeconds;
					Collection.NextAt = LastGameSeconds;
					return SampleCollection(Collection, Snapshot);
				}
				if (Step.Type == ETimelineStepType::CaptureStop)
				{
					for (FCaptureCollection& Collection : Collections)
					{
						if (Collection.bOpen && Collection.CaptureStep.CollectionName == Step.CollectionName)
						{
							CloseCollection(Collection, TEXT("explicit"), Step.Index);
							return true;
						}
					}
					StepError = Native(TEXT("pie_runtime_capture_collection_not_open"), TEXT("No open capture collection has this name."), TEXT("pie_scenario"),
						TEXT("collection_open"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					StepError.TargetPath = Step.CollectionName;
					return false;
				}
				if (Step.Type == ETimelineStepType::Capture)
				{
					FPendingCapture Pending;
					Pending.Started = FPlatformTime::Seconds();
					for (int32 Index = 0; Index < Collections.Num(); ++Index)
						if (&Collections[Index].Frames == CaptureFrames) Pending.Collection = Index;
					if (Pending.Collection != INDEX_NONE)
					{
						auto& Collection = Collections[Pending.Collection];
						if (!Collection.Surface) Collection.Surface = MakeShared<EditorCapture::FCaptureSurface>();
						Pending.Surface = Collection.Surface;
					}
					else
					{
						// Camera history and Slate render targets belong to their own source.
						Pending.SurfaceKey = FString::FromInt(static_cast<int32>(Step.CaptureSpec.Source))
							+ TEXT(":") + Step.CaptureSpec.CameraRef;
						auto& Pool = IdleCaptureSurfaces.FindOrAdd(Pending.SurfaceKey);
						Pending.Surface = Pool.IsEmpty() ? MakeShared<EditorCapture::FCaptureSurface>()
							: Pool.Pop(EAllowShrinking::No);
					}
					TArray<FColor> Unused;
					FIntPoint Size;
					FMinimalViewInfo View;
					bool bRead = false;
					if (Step.CaptureSpec.Source == EUeAgentPieCaptureSource::OutputSurface)
						bRead = Registry->TryReadExactOutputSurface(Plan.CoreRequest.SessionIdentity,
							Plan.CoreRequest.ExpectedPauseEpoch, Plan.CoreRequest.ExpectedWorkspaceRevision,
							Unused, Size, Pending.SourceIdentity, StepError, Pending.Surface.Get(), &Pending.Readback);
					else
					{
						FUeAgentPieCameraReadRequest Request;
						Request.Source = Step.CaptureSpec.Source == EUeAgentPieCaptureSource::PlayerCamera
							? EUeAgentPieCameraReadSource::PlayerCamera : EUeAgentPieCameraReadSource::SceneCamera;
						Request.CameraRef = Step.CaptureSpec.CameraRef;
						Request.TransientIdentity = OperationId;
						Request.OverrideLocation = Step.CaptureSpec.OverrideLocation;
						Request.OverrideRotation = Step.CaptureSpec.OverrideRotation;
						Request.OverrideFovDegrees = Step.CaptureSpec.OverrideFovDegrees;
						bRead = Registry->TryReadExactCamera(Plan.CoreRequest.SessionIdentity,
							Plan.CoreRequest.ExpectedPauseEpoch, Plan.CoreRequest.ExpectedWorkspaceRevision,
							Request, Unused, Size, Pending.SourceIdentity, StepError, &View,
							Pending.Surface.Get(), &Pending.Readback);
					}
					if (!bRead)
					{
						RemapArgumentInput(StepError, {
							{ TEXT("/camera"), StepPointer(Step.Index) + TEXT("/camera") },
							{ TEXT("/view_transform"), StepPointer(Step.Index) + TEXT("/view_transform") },
							{ TEXT("/fov_degrees"), StepPointer(Step.Index) + TEXT("/fov_degrees") }
						});
						if (!StepError.OperationIndex.IsSet()) StepError.OperationIndex = Step.Index;
						return false;
					}
					auto Item = Pending.Item;
					Item->SetStringField(TEXT("type"), TEXT("capture"));
					AddCaptureContext(Step.CaptureSpec, Item);
					if (Step.CaptureSpec.Source != EUeAgentPieCaptureSource::OutputSurface) AddCaptureView(View, Item);
					Item->SetNumberField(TEXT("operation"), Step.Index);
					// This is the world state submitted to rendering, not later file completion.
					Item->SetNumberField(TEXT("game_seconds"), LastGameSeconds);
					Item->SetNumberField(TEXT("width"), Size.X);
					Item->SetNumberField(TEXT("height"), Size.Y);
					Item->SetNumberField(TEXT("submit_seconds"), FPlatformTime::Seconds() - Pending.Started);
					(CaptureFrames ? *CaptureFrames : Results).Add(MakeShared<FJsonValueObject>(Item));
					PendingCaptures.Add(MoveTemp(Pending));
					return true;
				}
				if (Step.Type == ETimelineStepType::InputAction)
				{
					if (!Step.Action.IsValid())
					{
						StepError = Native(TEXT("pie_runtime_input_action_unavailable"),
							TEXT("The bound input action is no longer available."), TEXT("pie_scenario"),
							TEXT("input_action_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						StepError.TargetPath = Step.ActionRef;
						return false;
					}
					InputActionLedger.ApplySample({
						Step.ActionRef,
						LastGameSeconds,
						Step.ActionValueType,
						Step.ActionValue});
					if (Step.ActionValue.IsZero()) SessionInput->Actions.Remove(Step.Action);
					else SessionInput->Actions.Add(Step.Action, FInputActionValue(Step.ActionValueType, Step.ActionValue));
					return true;
				}
				if (Step.Type == ETimelineStepType::InputKey)
				{
					APlayerController* Controller = Snapshot.PlayerController.Get();
					if (!Controller)
					{
						StepError = Native(TEXT("pie_runtime_input_key_failed"),
							TEXT("The player controller needed to inject the key is unavailable."), TEXT("pie_scenario"),
							TEXT("player_controller_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						StepError.TargetPath = Step.Key.ToString();
						return false;
					}
					if (!SessionInput->Key(Snapshot, Step.Key, Step.InputEvent, Step.Amount, StepError)) return false;
					if (Step.InputEvent == IE_Pressed || Step.InputEvent == IE_Repeat) SessionInput->Keys.Add(Step.Key);
					else if (Step.InputEvent == IE_Released) SessionInput->Keys.Remove(Step.Key);
					else if (Step.InputEvent == IE_Axis && Step.Key != EKeys::MouseX && Step.Key != EKeys::MouseY && Step.Key != EKeys::MouseWheelAxis)
					{
						if (FMath::IsNearlyZero(Step.Amount)) SessionInput->Axes.Remove(Step.Key);
						else SessionInput->Axes.Add(Step.Key, Step.Amount);
					}
					InjectedInput.RecordSuccessfulInjection(
						Step.Key, Step.InputEvent, Step.Amount);
					return true;
				}
				if (Step.Type == ETimelineStepType::InputPointer) return SessionInput->MovePointer(Snapshot, Step.PointerValue, Step.bRelativePointer, StepError);
				if (Step.Type == ETimelineStepType::InputScroll) return SessionInput->Scroll(Snapshot, Step.Amount, StepError);
				if (Step.Type == ETimelineStepType::InputText) return SessionInput->Text(Snapshot, Step.TextValue, StepError);
				if (Step.Type == ETimelineStepType::PropertyWrite)
				{
					UE::UAI::Contract::FDiagnostic WriteError;
					if (!Registry->TryWriteProperty(
						Snapshot.SessionRef, Snapshot.PauseEpoch, Snapshot.WorkspaceRevision,
						Step.ActorRef, Step.Property, Step.PropertyValue, WriteError, Step.PropertyPath, Step.bRawPropertyValue))
					{
						StepError = WriteError.IsEmpty() ? Native(TEXT("pie_runtime_property_write_failed"),
							TEXT("The property write failed without a diagnostic."), TEXT("pie_scenario"),
							TEXT("diagnostic_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)) : WriteError;
						BindPropertyStepInput(StepError, Step);
						return false;
					}
					return true;
				}
				if (Step.Type == ETimelineStepType::PropertyGet)
				{
					TSharedPtr<FJsonValue> Value;
					if (!Registry->TryReadProperty(Snapshot.SessionRef, Snapshot.PauseEpoch, Snapshot.WorkspaceRevision,
						Step.ActorRef, Step.Property, Value, StepError, Step.PropertyPath))
					{
						BindPropertyStepInput(StepError, Step);
						return false;
					}
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("type"), TEXT("property_get"));
					Item->SetStringField(TEXT("status"), TEXT("succeeded"));
					Item->SetNumberField(TEXT("operation"), Step.Index);
					Item->SetNumberField(TEXT("game_seconds"), Snapshot.World->GetTimeSeconds() - StartGameSeconds);
					Item->SetField(TEXT("value"), Value);
					FScopeLock Lock(&Mutex);
					Results.Add(MakeShared<FJsonValueObject>(Item));
					return true;
				}
				UWorld* World = Snapshot.World.Get();
				UObject* ResolvedObject = nullptr;
				UE::UAI::Contract::FDiagnostic ResolveError;
				AActor* Actor = Registry->TryResolveAlias(
					Snapshot.SessionRef, Snapshot.PauseEpoch, Snapshot.WorkspaceRevision,
					Step.ActorRef, ResolvedObject, ResolveError)
					? Cast<AActor>(ResolvedObject) : nullptr;
				if (!World || !IsValid(Actor) || Actor->GetWorld() != World)
				{
					if (!ResolveError.IsEmpty()) StepError = ResolveError;
					else
					{
						StepError = Native(TEXT("pie_runtime_actor_transform_target_invalid"),
							!World ? TEXT("The scenario world is unavailable for the actor transform.")
								: !IsValid(Actor) ? TEXT("The resolved target is not a valid Actor.")
								: TEXT("The target Actor belongs to a different world."),
							TEXT("pie_scenario"), !World ? TEXT("world_available")
								: !IsValid(Actor) ? TEXT("actor_available") : TEXT("actor_in_scenario_world"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						StepError.TargetPath = Step.ActorRef;
					}
					return false;
				}
				FTransform Transform = Actor->GetActorTransform();
				if (Step.Transform.Location.IsSet()) Transform.SetLocation(Step.Transform.Location.GetValue());
				if (Step.Transform.Rotation.IsSet()) Transform.SetRotation(Step.Transform.Rotation.GetValue().Quaternion());
				if (Step.Transform.Scale.IsSet()) Transform.SetScale3D(Step.Transform.Scale.GetValue());
				if (!Actor->SetActorTransform(
						Transform, false, nullptr, ETeleportType::TeleportPhysics))
				{
					StepError = Native(TEXT("pie_runtime_actor_transform_apply_failed"),
						TEXT("The target Actor rejected the transform update."), TEXT("pie_scenario"),
						TEXT("transform_applied"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					StepError.TargetPath = Step.ActorRef;
					return false;
				}
				Contract::FDiagnostic ReadbackError;
				if (!TryVerifyActorTransform(
						Transform,
						Actor->GetActorTransform(),
						Step.Transform.Location.IsSet(),
						Step.Transform.Rotation.IsSet(),
						Step.Transform.Scale.IsSet(), ReadbackError))
				{
					ReadbackError.TargetPath = Step.ActorRef;
					ReadbackError.OperationIndex = Step.Index;
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(ReadbackError);
				}
				return true;
			}

			bool CaptureObservations(const FUeAgentPieRuntimeDebugSnapshot& Snapshot, UE::UAI::Contract::FDiagnostic& OutError)
			{
				FScopeLock Lock(&Mutex);
				ObservedValues.Reset();
				bool bSucceeded = true;
				for (const FObservationField& Field : Plan.ObservationFields)
				{
					TSharedPtr<FJsonValue> Value;
					UE::UAI::Contract::FDiagnostic ReadError;
					if (!Registry->TryReadProperty(
							Snapshot.SessionRef, Snapshot.PauseEpoch, Snapshot.WorkspaceRevision,
							Field.ObjectRef, Field.Property, Value, ReadError))
					{
						bSucceeded = false;
                        OutError.Append(ReadError);
						continue;
					}
					ObservedValues.Emplace(Field.ObjectRef + TEXT(".") + Field.Property, MoveTemp(Value));
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
			TUniquePtr<EditorRuntime::FScopedCPUThrottleExemption> CPUThrottleExemption;
			FUeAgentPieInjectedInputLedger InjectedInput;
			TSharedPtr<FUeAgentPieRuntimeInputState> SessionInput;
			TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> EnhancedInputSubsystem;
			TWeakObjectPtr<UEnhancedPlayerInput> EnhancedPlayerInput;
			TSet<const UInputAction*> InitialActionRefs;
			TMap<FString, TWeakObjectPtr<const UInputAction>> ResolvedInputActions;
			FUeAgentPieInputActionOperationLedger InputActionLedger;
			TArray<TPair<FString, TSharedPtr<FJsonValue>>> ObservedValues;
			FUeAgentResourceTracker CaptureResources;
			TMap<EUeAgentResourceDimension, int64> ReportedCaptureCharges;
			TArray<TSharedPtr<FJsonValue>> Results;
			TArray<UeAgentAuthoringDiagnostics::FDiagnostic> ConsistencyWarnings;
			TArray<FCaptureCollection> Collections;
			TArray<FPendingCapture> PendingCaptures;
			TArray<UE::UAI::Contract::FDiagnostic> CaptureCleanupDiagnostics;
			TMap<FString, TArray<TSharedPtr<EditorCapture::FCaptureSurface>>> IdleCaptureSurfaces;
			bool bCaptureSurfacesReleased = false;
			TOptional<FUeAgentPieScenarioReceipt> PendingReceipt;
			bool bCaptureFailed = false;
			TSharedRef<FJsonObject> Flow = MakeShared<FJsonObject>();
			TOptional<FFileArtifactReference> Artifact;
			FString ArtifactEvidenceId;
			FString CaptureSourceRef;
			UE::UAI::Contract::FDiagnostic StepError;
			FString FailedCollection;
			FString TerminalStatus;
			UE::UAI::Contract::FDiagnostic Diagnostic;
			FString WorkspacePath;
			int64 ObservedPauseEpoch = 0;
			int64 ObservedWorkspaceRevision = 0;
			int64 SignalRevision = 1;
			ELongRunningWorkState WorkState = ELongRunningWorkState::Running;
			EScenarioDispatchPhase Phase = EScenarioDispatchPhase::Scheduled;
			double StartWallSeconds = 0.0;
			double StartGameSeconds = 0.0;
			double LastGameSeconds = 0.0;
			uint64 StartFrame = 0;
			uint64 CPUThrottleStartFrame = 0;
			TArray<FScheduledStep> ScheduledSteps;
			TMap<int32, TArray<int32>> StepsAfterWait;
			TBitArray<> CompletedSteps;
			TArray<int32> CompletedOperations;
			TArray<TSharedPtr<FJsonObject>> TimingByOperation;
			TArray<TSharedPtr<FJsonValue>> OperationTimes;
			double RuntimeEndGameSeconds = 0.0;
			int32 FailedOperation = INDEX_NONE;
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
				if (State.IsValid()) return Fail(OutError, Native(TEXT("pie_runtime_scenario_already_started"),
					TEXT("This scenario operation port has already been started."), TEXT("pie_scenario"),
					TEXT("started"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
				OutIdentity = FLongRunningWorkIdentity(
					TEXT("native:pie-runtime-scenario:") + Request.GetOperationId(), 1);
				State = MakeShared<FScenarioSharedState, ESPMode::ThreadSafe>(
					Plan, Registry, ImageArtifactService,
					Request.GetOperationId(), OutIdentity, Cancellation);
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
				if (!TryMatch(OperationId, Identity, OutError)) return false;
				FScopeLock Lock(&State->Mutex);
				const auto CaptureUsage = State->CaptureResources.Snapshot();
				TMap<EUeAgentResourceDimension, int64> Charges;
				for (uint8 Index = 0; Index <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++Index)
				{
					const auto Dimension = static_cast<EUeAgentResourceDimension>(Index);
					const int64 Total = CaptureUsage.Consumed(Dimension);
					Charges.Add(Dimension, Total - State->ReportedCaptureCharges.FindRef(Dimension));
					State->ReportedCaptureCharges.Add(Dimension, Total);
				}
				FUeAgentResourceChargeError ChargeError;
				Resources.TryChargeMany(Charges, TEXT("pie_capture_work"), ChargeError);
				TOptional<FLongRunningOperationSignal> Signal;
				UE::UAI::Contract::FDiagnostic SignalError;
				if (State->SignalRevision > LastEmittedSignalRevision
					&& !TryMakeObservationSignal(
						State->ObservedValues,
						State->Flow,
						State->TerminalStatus,
						State->Diagnostic.Code,
						State->Plan.CoreRequest.SessionIdentity,
						State->ObservedPauseEpoch,
						State->ObservedWorkspaceRevision,
						State->WorkspacePath,
						State->Artifact,
						State->CaptureSourceRef,
						State->ArtifactEvidenceId,
						State->SignalRevision,
						Resources,
						Signal, SignalError))
				{
					if (!State->Diagnostic.IsEmpty()) SignalError.Append(State->Diagnostic);
					return Fail(OutError, MoveTemp(SignalError));
				}
				if (Signal.IsSet())
				{
					LastEmittedSignalRevision = State->SignalRevision;
				}
				OutObservation = FLongRunningWorkObservation(
					State->Identity.GetGeneration(),
					State->WorkState,
					State->Diagnostic,
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
				if (!TryMatch(OperationId, Identity, OutError)) return false;
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
				if (!TryMatch(OperationId, Identity, OutError)) return false;
				FScopeLock Lock(&State->Mutex);
				return State->WorkState != ELongRunningWorkState::Running
					|| Fail(OutError, Native(TEXT("pie_runtime_scenario_join_pending"),
						TEXT("The scenario is still running and cannot be joined yet."), TEXT("pie_scenario"),
						TEXT("work_state"), MakeShared<FJsonValueString>(TEXT("running")), MakeShared<FJsonValueString>(TEXT("terminal"))));
			}

			bool Release(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock PortLock(&PortMutex);
				if (!TryMatch(OperationId, Identity, OutError)) return false;
				{
					FScopeLock Lock(&State->Mutex);
					if (State->WorkState == ELongRunningWorkState::Running)
						return Fail(OutError, Native(TEXT("pie_runtime_scenario_release_before_terminal"),
							TEXT("The scenario work must reach a terminal state before release."), TEXT("pie_scenario"),
							TEXT("work_state"), MakeShared<FJsonValueString>(TEXT("running")), MakeShared<FJsonValueString>(TEXT("terminal"))));
				}
				State.Reset();
				return true;
			}

		private:
			bool TryMatch(const FString& OperationId, const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) const
			{
				if (State.IsValid() && State->OperationId == OperationId && State->Identity == Identity) return true;
				OutError = Native(TEXT("pie_runtime_scenario_identity_mismatch"),
					!State.IsValid() ? TEXT("The scenario has no active operation state.")
						: State->OperationId != OperationId ? TEXT("The requested operation does not match this scenario.")
						: TEXT("The supplied work identity does not match the active scenario work."),
					TEXT("pie_scenario"), !State.IsValid() ? TEXT("operation_state_available")
						: State->OperationId != OperationId ? TEXT("operation_matches") : TEXT("work_identity_matches"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.SubjectId = OperationId;
				return false;
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
