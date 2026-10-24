#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentMechanicsRequestContext.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	/** Owns Blueprint editor, graph, viewport and Slate-window image capture. */
	class UEAGENTINTERFACE_API FBlueprintEditorCapture final
	{
	public:
		static bool Capture(
			const FUeAgentRequestContext& Context,
			TSharedPtr<FJsonObject>& OutData,
			UE::UAI::Contract::FDiagnostic& OutError);

		/** Reuses Blueprint capture and only projects Control Rig request/result aliases. */
		static bool CaptureControlRig(
			const FUeAgentRequestContext& Context,
			TSharedPtr<FJsonObject>& OutData,
			UE::UAI::Contract::FDiagnostic& OutError);
	};
}
