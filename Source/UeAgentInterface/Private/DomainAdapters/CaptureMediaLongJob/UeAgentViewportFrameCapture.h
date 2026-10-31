#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	/** GT-driven viewport work; the native session owns scheduling and cleanup. */
	class IViewportFrameCapture
	{
	public:
		virtual ~IViewportFrameCapture() = default;
		virtual bool CanExecuteImmediately() const { return false; }
		virtual bool Tick(TSharedPtr<FJsonObject>& OutData, bool& bOutComplete,
			UE::UAI::Contract::FDiagnostic& OutError) = 0;
		virtual void Cancel() = 0;
	};
}
