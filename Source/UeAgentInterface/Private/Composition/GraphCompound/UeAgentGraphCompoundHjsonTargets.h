#pragma once

#include "Composition/HjsonImport/UeAgentHjsonImportTargetRegistry.h"

namespace UE::UAI::Composition::GraphCompound
{
	/**
	 * Active Graph/Compound HJSON targets owned by the common composition
	 * layer. Domain hooks receive typed native values only; they never parse a
	 * workspace or own planning, persistence, verification, or outcome.
	 */
	UEAGENTINTERFACE_API const TArray<FString>&
	GetGraphCompoundHjsonCompositionKinds();

	UEAGENTINTERFACE_API bool TryCreateBlueprintHjsonImportOperation(
		HjsonImport::FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<HjsonImport::IUeAgentHjsonImportOperation>& OutOperation,
		HjsonImport::FUeAgentHjsonImportTargetError& OutError);

	UEAGENTINTERFACE_API bool TryCreateWidgetBlueprintHjsonImportOperation(
		HjsonImport::FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<HjsonImport::IUeAgentHjsonImportOperation>& OutOperation,
		HjsonImport::FUeAgentHjsonImportTargetError& OutError);

	UEAGENTINTERFACE_API bool TryCreateNiagaraSystemHjsonImportOperation(
		HjsonImport::FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<HjsonImport::IUeAgentHjsonImportOperation>& OutOperation,
		HjsonImport::FUeAgentHjsonImportTargetError& OutError);

	UEAGENTINTERFACE_API bool TryCreateNiagaraEmitterHjsonImportOperation(
		HjsonImport::FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<HjsonImport::IUeAgentHjsonImportOperation>& OutOperation,
		HjsonImport::FUeAgentHjsonImportTargetError& OutError);

	UEAGENTINTERFACE_API bool TryCreateNiagaraScriptHjsonImportOperation(
		HjsonImport::FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<HjsonImport::IUeAgentHjsonImportOperation>& OutOperation,
		HjsonImport::FUeAgentHjsonImportTargetError& OutError);
}
