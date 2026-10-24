#pragma once

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	using FLocalizationSettingsStage = TFunction<bool(
		TFunctionRef<bool(UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)>,
		UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)>;

	UEAGENTINTERFACE_API bool ApplyLocalizationSettings(const FString& RequestId,
		const TSharedRef<FJsonObject>& Desired,
		HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome& Outcome,
		const FLocalizationSettingsStage& Stage = {});
}
