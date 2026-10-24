// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentBlueprintAuthoring
{
	inline constexpr const TCHAR* BlueprintExportContractId = TEXT("uai.export.blueprint.asset");
	inline constexpr const TCHAR* BlueprintProjectionId = TEXT("uai.export.blueprint.compound");

	UEAGENTINTERFACE_API bool RegisterExportAuthoring(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool GetExportContract(
		UeAgentExportContract::FContract& OutContract,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool GetReusableProjectionKeys(
		const FString& FieldId,
		const UeAgentAuthoringState::FStateEntry& Entry,
		TArray<FString>& OutKeys, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool GetReusableProjectionKeys(
		const FString& FieldId,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity,
		TArray<FString>& OutKeys, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API FString GetReusableComponentKey(
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity);
	UEAGENTINTERFACE_API FString GetReusableGraphKey(
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity);
	UEAGENTINTERFACE_API FString GetReusableNodeKey(
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity);
}
