#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UClass;
class FProperty;

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	/** The existing DataObject metadata contract, shared by factory and HJSON selection. */
	UEAGENTINTERFACE_API bool IsEligibleDataObjectClass(const UClass* ConcreteClass);
	/** Registration defines scope; the common codec owns value/type qualification. */
	UEAGENTINTERFACE_API bool CollectRegisteredDataObjectFields(
		const UClass* ConcreteClass, TMap<FString, FProperty*>& OutFields, UE::UAI::Contract::FDiagnostic& OutError);
}
