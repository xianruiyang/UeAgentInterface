// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UeAgentLevelAuthoring
{
	inline constexpr const TCHAR* DomainId = TEXT("level");
	inline constexpr const TCHAR* ApplyProviderId = TEXT("level.apply");
	inline constexpr const TCHAR* WorldInputContractId = TEXT("uai.apply_input.level.world");
	inline constexpr const TCHAR* ActorInputContractId = TEXT("uai.apply_input.level.actor");
	inline constexpr const TCHAR* ComponentInputContractId = TEXT("uai.apply_input.level.component");
	inline constexpr const TCHAR* WorkspaceContractId = TEXT("uai.apply_workspace.level.authoring");
	inline constexpr const TCHAR* WorldExportContractId = TEXT("uai.export.level.world.v2");
	inline constexpr const TCHAR* ActorExportContractId = TEXT("uai.export.level.actor.v2");
	inline constexpr const TCHAR* ComponentExportContractId = TEXT("uai.export.level.component.v2");
	inline constexpr const TCHAR* ProjectionId = TEXT("level.snapshot.flat.v1");
	inline constexpr const TCHAR* StateAccessorId = TEXT("level.state");
	inline constexpr const TCHAR* NativeDefaultProviderId = TEXT("level.native");

	inline constexpr const TCHAR* WorldStateSemanticId = TEXT("uai.semantic.level.world");
	inline constexpr const TCHAR* WorldTargetSemanticId = TEXT("uai.semantic.level.world.target");
	inline constexpr const TCHAR* WorldPropertySemanticId = TEXT("uai.semantic.level.world.property");
	inline constexpr const TCHAR* ActorStateSemanticId = TEXT("uai.semantic.level.actor");
	inline constexpr const TCHAR* ActorTargetSemanticId = TEXT("uai.semantic.level.actor.target");
	inline constexpr const TCHAR* ActorPropertySemanticId = TEXT("uai.semantic.level.actor.property");
	inline constexpr const TCHAR* ComponentStateSemanticId = TEXT("uai.semantic.level.component");
	inline constexpr const TCHAR* ComponentTargetSemanticId = TEXT("uai.semantic.level.component.target");
	inline constexpr const TCHAR* ComponentPropertySemanticId = TEXT("uai.semantic.level.component.property");

	inline constexpr const TCHAR* WorldIdentityModelId = TEXT("level_world_identity");
	inline constexpr const TCHAR* WorldPropertyIdentityModelId = TEXT("level_world_property_identity");
	inline constexpr const TCHAR* ActorIdentityModelId = TEXT("level_actor_identity");
	inline constexpr const TCHAR* ComponentIdentityModelId = TEXT("level_component_identity");
	inline constexpr const TCHAR* ActorPropertyIdentityModelId = TEXT("level_actor_property_identity");
	inline constexpr const TCHAR* ComponentPropertyIdentityModelId = TEXT("level_component_property_identity");

	UEAGENTINTERFACE_API bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError);
}
