#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentMechanicsRequestContext.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	/** Level-sequence evaluation and camera capture with full transient-state
	 * cleanup. */
	class UEAGENTINTERFACE_API FSequenceCapture final
	{
	public:
		static bool Capture(
			const FUeAgentRequestContext& Context,
			TSharedPtr<FJsonObject>& OutData,
			UE::UAI::Contract::FDiagnostic& OutError);
	};
}
