// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class APlayerController;
class ULocalPlayer;
class UWorld;

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	struct FUeAgentPieRuntimeDebugRegistryState;

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
			FString& OutError);
		bool TryResolveExact(
			const FString& SessionRef,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision,
			FUeAgentPieRuntimeDebugSnapshot& OutSnapshot,
			FString& OutError) const;
		bool TryReadExactOutputSurface(
			const FString& SessionRef,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision,
			TArray<FColor>& OutPixels,
			FIntPoint& OutSize,
			FString& OutSurfaceIdentity,
			FString& OutError) const;
		bool TryReadExactCamera(
			const FString& SessionRef,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision,
			const FUeAgentPieCameraReadRequest& Request,
			TArray<FColor>& OutPixels,
			FIntPoint& OutSize,
			FString& OutSourceIdentity,
			FString& OutError) const;
		bool TryGetCachedSnapshot(
			const FString& SessionRef,
			FUeAgentPieRuntimeDebugCachedSnapshot& OutSnapshot,
			FString& OutError) const;
		bool TryResume(const FString& SessionRef, FString& OutError);
		bool TryRepauseAndVerify(
			const FString& SessionRef,
			int64& OutPauseEpoch,
			FString& OutError);
		bool TryRefreshCurrentWorkspace(
			const FString& SessionRef,
			int64 ExpectedWorkspaceRevision,
			int64& OutWorkspaceRevision,
			FString& OutError);
		bool TryEndSession(const FString& SessionRef, FString& OutError);

	private:
		mutable FCriticalSection Mutex;
		TUniquePtr<FUeAgentPieRuntimeDebugRegistryState> State;
	};
}
