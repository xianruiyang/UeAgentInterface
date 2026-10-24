#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "SmartObjectDefinition.h"

namespace UeAgentSmartObjectAuthoring
{
	// Borrowed native storage. Consume before mutation/reload; this is not a checkpoint.
	struct FDefinitionCapture
	{
		const USmartObjectDefinition* Definition = nullptr;
		FGuid RootId;
		FGuid ParametersId;
		TConstArrayView<FSmartObjectSlotDefinition> Slots;
		TConstArrayView<TObjectPtr<USmartObjectBehaviorDefinition>> DefaultBehaviors;
		TConstArrayView<FSmartObjectDefinitionDataProxy> DefinitionData;
		const FInstancedPropertyBag* Parameters = nullptr;
		const FSmartObjectBindingCollection* Bindings = nullptr;
		TConstArrayView<FSmartObjectDefinitionBindableStructDescriptor> BindableStructs;
	};

	// Reads the existing definition without initializing GUIDs, bindings or conditions.
	UEAGENTINTERFACE_API bool CaptureDefinition(const USmartObjectDefinition& Definition,
		FDefinitionCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError);
}
