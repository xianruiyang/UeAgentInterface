#pragma once

#include "Core/Contract/UeAgentMechanicsRequestContext.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportFrameCapture.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	/** One image; only a cold viewport needs preparation across real engine frames. */
	class FLevelViewportImageCapture final : public IViewportFrameCapture
	{
	public:
		static bool TryCreate(const FUeAgentRequestContext& Context,
			TSharedPtr<IViewportFrameCapture>& OutCapture, UE::UAI::Contract::FDiagnostic& OutError);
		~FLevelViewportImageCapture() override;
		bool CanExecuteImmediately() const override;
		bool Tick(TSharedPtr<FJsonObject>& OutData, bool& bOutComplete,
			UE::UAI::Contract::FDiagnostic& OutError) override;
		void Cancel() override;
	private:
		class FImpl;
		explicit FLevelViewportImageCapture(TUniquePtr<FImpl> InImpl);
		TUniquePtr<FImpl> Impl;
	};
}
