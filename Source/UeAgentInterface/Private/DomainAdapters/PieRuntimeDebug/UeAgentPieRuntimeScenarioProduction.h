// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioAdapter.h"
#include "Engine/EngineTypes.h"
#include "InputCoreTypes.h"
#include "InputActionValue.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	class IUeAgentCaptureImageArtifactService;
}

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	class FUeAgentPieRuntimeDebugRegistry;

	namespace CaptureImageOwner = UE::UAI::DomainAdapters::CaptureMediaLongJob;

	struct UEAGENTINTERFACE_API FUeAgentPieInjectedInputCleanup final
	{
		FKey Key;
		EInputEvent Event = IE_Released;
		float Amount = 0.0f;
	};

	class UEAGENTINTERFACE_API FUeAgentPieInjectedInputLedger final
	{
	public:
		void RecordSuccessfulInjection(
			const FKey& Key,
			EInputEvent Event,
			double Amount);
		TArray<FUeAgentPieInjectedInputCleanup> ConsumeCleanupActions();
		bool IsEmpty() const;

	private:
		TSet<FKey> DigitalKeys;
		TSet<FKey> AxisKeys;
	};

	struct UEAGENTINTERFACE_API FUeAgentPieInputActionSample final
	{
		FString ActionRef;
		double AtSeconds = 0.0;
		EInputActionValueType ValueType = EInputActionValueType::Boolean;
		FVector Value = FVector::ZeroVector;

		bool IsNonZero() const { return !Value.IsZero(); }
	};

	UEAGENTINTERFACE_API bool TryParsePieInputActionSample(
		const StateSemantic::FUeAgentCanonicalValueRef& StepValue,
		const TArray<FUeAgentPieInputActionSample>& ExistingSamples,
		FUeAgentPieInputActionSample& OutSample,
		FString& OutErrorCode);

	enum class EUeAgentPieInputActionReleaseStatus : uint8
	{
		Pending,
		Verified,
		Unverified,
	};

	class UEAGENTINTERFACE_API FUeAgentPieInputActionOperationLedger final
	{
	public:
		static constexpr double ReleaseGraceSeconds = 2.0;
		static constexpr uint64 ReleaseGraceFrames = 120;

		void ApplySample(const FUeAgentPieInputActionSample& Sample);
		const TMap<FString, FUeAgentPieInputActionSample>& GetActiveSamples() const;
		void RecordObservedValue(const FString& ActionRef, bool bNonZero);
		void BeginRelease(double WallSeconds, uint64 Frame);
		EUeAgentPieInputActionReleaseStatus EvaluateRelease(
			double WallSeconds,
			uint64 Frame) const;
		bool HasInjectedNonZero() const;
		bool IsReinjectionStopped() const;

	private:
		TMap<FString, FUeAgentPieInputActionSample> ActiveSamples;
		TSet<FString> InjectedNonZero;
		TSet<FString> ObservedNonZero;
		TMap<FString, bool> LatestObservedNonZero;
		double ReleaseStartWallSeconds = 0.0;
		uint64 ReleaseStartFrame = 0;
		bool bReleaseStarted = false;
	};

	enum class EUeAgentPieCaptureSource : uint8
	{
		OutputSurface,
		PlayerCamera,
		SceneCamera,
	};

	struct UEAGENTINTERFACE_API FUeAgentPieCaptureStepSpec final
	{
		EUeAgentPieCaptureSource Source = EUeAgentPieCaptureSource::OutputSurface;
		FString CameraRef;
		TOptional<FVector> OverrideLocation;
		TOptional<FRotator> OverrideRotation;
		TOptional<float> OverrideFovDegrees;
	};

	UEAGENTINTERFACE_API bool TryParsePieCaptureStepSpec(
		const StateSemantic::FUeAgentCanonicalValueRef& StepValue,
		FUeAgentPieCaptureStepSpec& OutSpec,
		FString& OutErrorCode);

	UEAGENTINTERFACE_API FString ResolvePieCaptureFailureCode(
		const FString& ExactErrorCode,
		const TCHAR* FallbackErrorCode);

	class FUeAgentPieRuntimeScenarioPortFactory final
		: public IUeAgentPieRuntimeScenarioPortFactory
	{
	public:
		explicit FUeAgentPieRuntimeScenarioPortFactory(
			TSharedRef<FUeAgentPieRuntimeDebugRegistry,
				ESPMode::ThreadSafe> InRegistry,
			TSharedRef<CaptureImageOwner::IUeAgentCaptureImageArtifactService,
				ESPMode::ThreadSafe> InImageArtifactService);

		bool TryCreateOperationPort(
			const FUeAgentPieRuntimeScenarioTypedRequest& Request,
			const ExecutionMutation::FLongRunningOperationRequest& OperationRequest,
			TSharedPtr<ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) override;

	private:
		TSharedRef<FUeAgentPieRuntimeDebugRegistry,
			ESPMode::ThreadSafe> Registry;
		TSharedRef<CaptureImageOwner::IUeAgentCaptureImageArtifactService,
			ESPMode::ThreadSafe> ImageArtifactService;
	};
}
