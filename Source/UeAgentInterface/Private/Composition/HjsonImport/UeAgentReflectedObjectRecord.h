#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

namespace UE::UAI::Composition::HjsonImport
{
	/** Native owners supply qualification policy; the public property pipeline owns representation and decoding. */
	UEAGENTINTERFACE_API bool CaptureReflectedObjectRecord(const UObject& Object,
		const UeAgentDetailsAuthoringService::FCapturePolicy& Policy,
		TSharedPtr<FJsonObject>& OutProperties, TSharedPtr<FJsonObject>& OutDefinitions, UE::UAI::Contract::FDiagnostic& OutError);

	/** Prepare against this owner's current state (or a native new-object default), without writing it. */
	UEAGENTINTERFACE_API bool PrepareReflectedObjectRecord(UObject& Object,
		const UeAgentDetailsAuthoringService::FCapturePolicy& Policy,
		const TSharedPtr<FJsonObject>& Properties,
		UeAgentAuthoringObjectApply::FPreparedApply& OutPrepared, UE::UAI::Contract::FDiagnostic& OutError);

	/** A native owner selects the authored members; struct identities remain outside the record. */
	UEAGENTINTERFACE_API bool CaptureReflectedStructRecord(UStruct& Type, const void* Data, UObject& Owner,
		const TArray<FString>& PropertyPaths, const UeAgentDetailsAuthoringService::FCapturePolicy& Policy,
		TSharedPtr<FJsonObject>& OutProperties, TSharedPtr<FJsonObject>& OutDefinitions, UE::UAI::Contract::FDiagnostic& OutError);

	/** Storage and its notification owner must outlive the returned prepared operation. */
	UEAGENTINTERFACE_API bool PrepareReflectedStructRecord(UStruct& Type, void* Data, UObject& Owner,
		const TArray<FString>& PropertyPaths, const UeAgentDetailsAuthoringService::FCapturePolicy& Policy,
		const TSharedPtr<FJsonObject>& Properties, const UeAgentAuthoringObjectApply::FOptions& Options,
		UeAgentAuthoringObjectApply::FPreparedApply& OutPrepared, UE::UAI::Contract::FDiagnostic& OutError);
}
