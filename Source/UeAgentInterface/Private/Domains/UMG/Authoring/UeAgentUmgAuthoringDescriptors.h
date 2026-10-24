// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UeAgentUmgAuthoring
{
	inline constexpr const TCHAR* DomainId = TEXT("umg");
	inline constexpr const TCHAR* ApplyProviderId = TEXT("umg.apply");
	inline constexpr const TCHAR* AssetInputContractId = TEXT("uai.apply_input.umg.asset");
	inline constexpr const TCHAR* AssetWorkspaceContractId = TEXT("uai.apply_workspace.umg.asset");
	inline constexpr const TCHAR* ExportContractId = TEXT("uai.export.umg.asset.v2");
	inline constexpr const TCHAR* ProjectionId = TEXT("umg.snapshot.compound");
	inline constexpr const TCHAR* StateAccessorId = TEXT("umg.state");
	inline constexpr const TCHAR* NativeDefaultProviderId = TEXT("umg.native");

	inline constexpr const TCHAR* AssetSemanticId = TEXT("uai.semantic.umg.asset");
	inline constexpr const TCHAR* ExportTargetSemanticId = TEXT("uai.semantic.umg.export.target");
	inline constexpr const TCHAR* ParentClassSemanticId = TEXT("uai.semantic.umg.parent_class");
	inline constexpr const TCHAR* WidgetSemanticId = TEXT("uai.semantic.umg.widget");
	inline constexpr const TCHAR* WidgetPropertySemanticId = TEXT("uai.semantic.umg.widget.property");
	inline constexpr const TCHAR* SlotSemanticId = TEXT("uai.semantic.umg.slot");
	inline constexpr const TCHAR* SlotPropertySemanticId = TEXT("uai.semantic.umg.slot.property");
	inline constexpr const TCHAR* BindingSemanticId = TEXT("uai.semantic.umg.binding");
	inline constexpr const TCHAR* AnimationSemanticId = TEXT("uai.semantic.umg.animation");

	inline constexpr const TCHAR* AssetIdentityModelId = TEXT("umg_asset_identity");
	inline constexpr const TCHAR* WidgetIdentityModelId = TEXT("umg_widget_identity");
	inline constexpr const TCHAR* SlotIdentityModelId = TEXT("umg_slot_identity");
	inline constexpr const TCHAR* BindingIdentityModelId = TEXT("umg_binding_identity");
	inline constexpr const TCHAR* AnimationIdentityModelId = TEXT("umg_animation_identity");
	inline constexpr const TCHAR* WidgetPropertyIdentityModelId = TEXT("umg_widget_property_identity");
	inline constexpr const TCHAR* SlotPropertyIdentityModelId = TEXT("umg_slot_property_identity");

	UEAGENTINTERFACE_API bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError);
}
