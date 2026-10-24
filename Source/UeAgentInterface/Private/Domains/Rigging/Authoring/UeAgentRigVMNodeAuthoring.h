#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UBlueprintNodeSpawner;
class UControlRigBlueprint;
class URigVMController;
class URigVMNode;
class URigVMPin;

namespace UeAgentRigVMNodeAuthoring
{
	// Discovery and persistence use the same native node-family registration.
	UEAGENTINTERFACE_API FString GetSupportedNodeType(const UBlueprintNodeSpawner& Spawner);
	// Returns whether a supported creation family was captured. Unsupported native
	// nodes still expose their class identity and may be retained/validated in place.
	UEAGENTINTERFACE_API bool CaptureDefinition(const UControlRigBlueprint& Blueprint, const URigVMNode& Node, FJsonObject& OutDefinition);

	UEAGENTINTERFACE_API bool IsAuthorableDefaultPin(const URigVMPin& Pin);
	UEAGENTINTERFACE_API bool ApplyPinDefaults(URigVMController& Controller, URigVMNode& Node,
		const FJsonObject& Definition, bool bNewNode, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	UEAGENTINTERFACE_API URigVMNode* CreateNode(UControlRigBlueprint& Blueprint, URigVMController& Controller,
		const FJsonObject& Definition, const FString& Name, const FVector2D& Position, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ValidateExistingDefinition(const UControlRigBlueprint& Blueprint, const URigVMNode& Node,
		const FJsonObject& Definition, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ApplyFunctionVariableMap(URigVMController& Controller, URigVMNode& Node,
		const FJsonObject& Definition, UE::UAI::Contract::FDiagnostic& OutError);
}
