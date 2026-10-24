// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UeAgentNiagaraAuthoring
{
	inline constexpr const TCHAR* DomainId = TEXT("niagara");
	inline constexpr const TCHAR* ApplyProviderId = TEXT("niagara.apply");
	inline constexpr const TCHAR* AssetInputContractId = TEXT("uai.apply_input.niagara.asset");
	inline constexpr const TCHAR* AssetWorkspaceContractId = TEXT("uai.apply_workspace.niagara.asset");
	inline constexpr const TCHAR* ModuleInputContractId = TEXT("uai.apply_input.niagara.module");
	inline constexpr const TCHAR* ModuleWorkspaceContractId = TEXT("uai.apply_workspace.niagara.module");
	inline constexpr const TCHAR* DynamicInputInputContractId = TEXT("uai.apply_input.niagara.dynamic_input");
	inline constexpr const TCHAR* DynamicInputWorkspaceContractId = TEXT("uai.apply_workspace.niagara.dynamic_input");
	inline constexpr const TCHAR* StateAccessorId = TEXT("niagara.state");
	inline constexpr const TCHAR* FactoryDefaultProviderId = TEXT("niagara.factory");
	inline constexpr const TCHAR* SchemaDefaultProviderId = TEXT("niagara.schema");
	inline constexpr const TCHAR* DomainNativeDefaultProviderId = TEXT("niagara.domain_native");

	inline constexpr const TCHAR* AssetSemanticId = TEXT("uai.semantic.niagara.asset");
	inline constexpr const TCHAR* SystemSemanticId = TEXT("uai.semantic.niagara.system");
	inline constexpr const TCHAR* EmitterSemanticId = TEXT("uai.semantic.niagara.emitter");
	inline constexpr const TCHAR* ScriptSemanticId = TEXT("uai.semantic.niagara.script");
	inline constexpr const TCHAR* StackSemanticId = TEXT("uai.semantic.niagara.stack_entry");
	inline constexpr const TCHAR* ModuleSemanticId = TEXT("uai.semantic.niagara.module");
	inline constexpr const TCHAR* DynamicInputSemanticId = TEXT("uai.semantic.niagara.dynamic_input");
	inline constexpr const TCHAR* EmitterOwnerSemanticId = TEXT("uai.semantic.niagara.emitter_owner");
	inline constexpr const TCHAR* EmitterPropertySemanticId = TEXT("uai.semantic.niagara.emitter_property");
	inline constexpr const TCHAR* RendererSemanticId = TEXT("uai.semantic.niagara.renderer");
	inline constexpr const TCHAR* EventHandlerSemanticId = TEXT("uai.semantic.niagara.event_handler");
	inline constexpr const TCHAR* UserParameterSemanticId = TEXT("uai.semantic.niagara.user_parameter");

	UEAGENTINTERFACE_API bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API FString GetAssetKind(const UObject* Object);
	UEAGENTINTERFACE_API FString GetAssetSemanticId(const UObject* Object);
	UEAGENTINTERFACE_API FString GetAssetIdentityModelId(const UObject* Object);
}
