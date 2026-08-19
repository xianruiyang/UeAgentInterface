#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNative.h"

class UWorld;
class UBlueprint;

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	struct UEAGENTINTERFACE_API FUeAgentWorldSpatialViewportBinding
	{
		FString Identity;
		FEditorViewportClient* Client = nullptr;
		FViewport* Viewport = nullptr;

		bool IsValidForWorld(const UWorld& ExplicitWorld) const;
	};

	struct UEAGENTINTERFACE_API FUeAgentWorldSpatialMeshSelectionBinding
	{
		FString ViewportIdentity;
		UGeometrySelectionManager* Manager = nullptr;
		TArray<FUeAgentWorldSpatialMeshSelectionTarget> Targets;

		bool IsValid() const;
	};

	/**
	 * Exact transient identity for the Components preview viewport owned by one
	 * Actor Blueprint asset. The identity contains no implicit current-editor
	 * fallback and is safe to use as the public viewport target.
	 */
	UEAGENTINTERFACE_API FString MakeBlueprintComponentsViewportIdentity(
		const UBlueprint& Blueprint);

	/**
	 * Registers a live, externally owned Blueprint Components preview session.
	 * The registry only resolves the viewport mechanics; it does not own or
	 * cache camera state. The owner must unregister before destroying the client
	 * or viewport.
	 */
	UEAGENTINTERFACE_API bool TryRegisterBlueprintComponentsViewport(
		UBlueprint& Blueprint,
		FEditorViewportClient& Client,
		FViewport& Viewport,
		FString& OutViewportIdentity,
		FString& OutError);

	UEAGENTINTERFACE_API void UnregisterBlueprintComponentsViewport(
		const FString& ViewportIdentity,
		FEditorViewportClient& Client);

	UEAGENTINTERFACE_API bool TryCreateWorldSpatialProductionDependencies(
		UWorld& ExplicitWorld,
		const FString& RuntimeGeneration,
		const FString& ServerSessionId,
		FUeAgentWorldSpatialNativeDependencies& OutDependencies,
		FUeAgentWorldSpatialExecutionError& OutError,
		const TArray<FUeAgentWorldSpatialViewportBinding>& ViewportBindings = {},
		const TArray<FUeAgentWorldSpatialMeshSelectionBinding>&
			MeshSelectionBindings = {});
}
