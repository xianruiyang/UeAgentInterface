// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class APlayerController;
struct FMinimalViewInfo;
class FJsonValue;
class FJsonObject;
class ULocalPlayer;
class UObject;
class UWorld;
namespace UE::UAI::EditorCapture { class FCaptureSurface; class FAsyncCaptureFrame; }

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	struct FUeAgentPieRuntimeDebugRegistryState;
	struct FUeAgentPieRuntimeInputState;

	struct FUeAgentPieRuntimeDebugSnapshot
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ULocalPlayer> LocalPlayer;
		TWeakObjectPtr<APlayerController> PlayerController;
		FString SessionRef;
		int32 PieInstance = INDEX_NONE;
		int32 LocalPlayerIndex = INDEX_NONE;
		int64 PauseEpoch = 0;
		int64 WorkspaceRevision = 0;
		FString WorkspacePath;
	};

	struct FUeAgentPieRuntimeDebugCachedSnapshot
	{
		FString SessionRef;
		int32 PieInstance = INDEX_NONE;
		int32 LocalPlayerIndex = INDEX_NONE;
		int64 PauseEpoch = 0;
		int64 WorkspaceRevision = 0;
		FString WorkspacePath;
	};

	enum class EUeAgentPieCameraReadSource : uint8
	{
		PlayerCamera,
		SceneCamera,
	};

	struct FUeAgentPieCameraReadRequest
	{
		EUeAgentPieCameraReadSource Source = EUeAgentPieCameraReadSource::PlayerCamera;
		FString CameraRef;
		FString TransientIdentity;
		TOptional<FVector> OverrideLocation;
		TOptional<FRotator> OverrideRotation;
		TOptional<float> OverrideFovDegrees;
	};

	UEAGENTINTERFACE_API FString MakePieCameraSourceIdentity(
		EUeAgentPieCameraReadSource Source,
		const FString& ExactObjectIdentity);
	UEAGENTINTERFACE_API FString MakePieTransientCameraSourceIdentity(
		const FString& OperationId);

	class FUeAgentPieRuntimeDebugRegistry final
	{
	public:
		FUeAgentPieRuntimeDebugRegistry();
		~FUeAgentPieRuntimeDebugRegistry();

		bool TryActivate(
			int32 RequestedPieInstance,
			int32 RequestedLocalPlayerIndex,
			FUeAgentPieRuntimeDebugSnapshot& OutSnapshot,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool TryResolveExact(
			const FString& SessionRef,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision,
			FUeAgentPieRuntimeDebugSnapshot& OutSnapshot,
			UE::UAI::Contract::FDiagnostic& OutError) const;
		bool TryResolveAlias(
			const FString& SessionRef,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision,
			const FString& Alias,
			UObject*& OutObject,
			UE::UAI::Contract::FDiagnostic& OutError) const;
		bool TryReadProperty(
			const FString& SessionRef,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision,
			const FString& Alias,
			const FString& PropertyName,
			TSharedPtr<FJsonValue>& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError,
			const TArray<TSharedPtr<FJsonValue>>& Path = {}) const;
		bool TryWriteProperty(
			const FString& SessionRef,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision,
			const FString& Alias,
			const FString& PropertyName,
			const TSharedPtr<FJsonValue>& Value,
			UE::UAI::Contract::FDiagnostic& OutError,
			const TArray<TSharedPtr<FJsonValue>>& Path = {}, bool bRawValue = false);
		bool ValidatePropertyOperation(const FString& SessionRef, const FString& Alias,
			const FString& PropertyName, const TArray<TSharedPtr<FJsonValue>>& Path,
			const TSharedPtr<FJsonValue>& WriteValue, bool bRawValue, UE::UAI::Contract::FDiagnostic& OutError) const;
		bool TryReadExactOutputSurface(
			const FString& SessionRef,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision,
			TArray<FColor>& OutPixels,
			FIntPoint& OutSize,
			FString& OutSurfaceIdentity,
			UE::UAI::Contract::FDiagnostic& OutError,
			EditorCapture::FCaptureSurface* AsyncSurface = nullptr,
			TSharedPtr<EditorCapture::FAsyncCaptureFrame, ESPMode::ThreadSafe>* OutFrame = nullptr) const;
		bool TryReadExactCamera(
			const FString& SessionRef,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision,
			const FUeAgentPieCameraReadRequest& Request,
			TArray<FColor>& OutPixels,
			FIntPoint& OutSize,
			FString& OutSourceIdentity,
			UE::UAI::Contract::FDiagnostic& OutError,
			FMinimalViewInfo* OutView = nullptr,
			EditorCapture::FCaptureSurface* AsyncSurface = nullptr,
			TSharedPtr<EditorCapture::FAsyncCaptureFrame, ESPMode::ThreadSafe>* OutFrame = nullptr) const;
		bool TryGetCachedSnapshot(
			const FString& SessionRef,
			FUeAgentPieRuntimeDebugCachedSnapshot& OutSnapshot,
			UE::UAI::Contract::FDiagnostic& OutError) const;
		bool TryResume(const FString& SessionRef, UE::UAI::Contract::FDiagnostic& OutError);
		bool TryRepauseAndVerify(
			const FString& SessionRef,
			int64& OutPauseEpoch,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool TryRefreshCurrentWorkspace(
			const FString& SessionRef,
			int64 ExpectedWorkspaceRevision,
			int64& OutWorkspaceRevision,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool TryEndSession(const FString& SessionRef, UE::UAI::Contract::FDiagnostic& OutError);
		TSharedPtr<FUeAgentPieRuntimeInputState> GetInputState(const FString& SessionRef) const;
		bool ReleaseInputs(const FString& SessionRef, UE::UAI::Contract::FDiagnostic& OutError);
		bool TrackReferences(const FString& SessionRef, const TArray<TSharedPtr<FJsonValue>>& Refs,
			const TArray<TSharedPtr<FJsonValue>>& Forget, UE::UAI::Contract::FDiagnostic& OutError);
		TSharedRef<FJsonObject> ReferenceUpdates(const FString& SessionRef) const;

	private:
		mutable FCriticalSection Mutex;
		TUniquePtr<FUeAgentPieRuntimeDebugRegistryState> State;
	};
}
