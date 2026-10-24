#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentMechanicsRequestContext.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	/**
	 * Captures one frame per real Editor engine-frame interval. The owning
	 * long-running operation drives Tick; this class never recursively ticks a
	 * UWorld or creates a second operation lifecycle.
	 */
	class UEAGENTINTERFACE_API FLevelViewportSequenceCapture final
		: public TSharedFromThis<FLevelViewportSequenceCapture>
	{
	public:
		static bool TryCreate(
			const FUeAgentRequestContext& Context,
			TSharedPtr<FLevelViewportSequenceCapture>& OutCapture,
			UE::UAI::Contract::FDiagnostic& OutError);

		~FLevelViewportSequenceCapture();

		bool Tick(
			TSharedPtr<FJsonObject>& OutData,
			bool& bOutComplete,
			UE::UAI::Contract::FDiagnostic& OutError);

		void Cancel();

	private:
		class FImpl;
		explicit FLevelViewportSequenceCapture(TUniquePtr<FImpl> InImpl);
		TUniquePtr<FImpl> Impl;
	};
}
