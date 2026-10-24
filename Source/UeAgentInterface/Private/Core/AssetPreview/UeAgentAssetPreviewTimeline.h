#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Misc/FrameRate.h"

class ISequencer;
class FJsonObject;
class UWorld;

namespace UE::UAI::AssetPreview
{
	struct FContext;
	TOptional<FFrameRate> PreviewFrameRate(const FContext& Context);
	void ReadTimelineStatus(const FContext& Context, FJsonObject& Out);

	// Owns only temporary native playback settings, never an alternate simulation.
	class FContinuousCapture
	{
	public:
		bool Begin(const FContext& Context, double StartSeconds, double EndSeconds, UE::UAI::Contract::FDiagnostic& OutError);
		bool Advance(const FContext& Context, double TargetSeconds, bool& bReady, UE::UAI::Contract::FDiagnostic& OutError);
		void Restore();
	private:
		TSharedPtr<ISequencer> Sequencer;
		double LastSeconds = 0;
		float PreviousSpeed = 1;
		bool bWasPlaying = false;
		bool bExpectedPlaying = false;
		bool bActive = false;
		bool bSettling = false;
		TWeakObjectPtr<UWorld> SimulationWorld;
		FDelegateHandle WorldTickHandle;
		uint64 CompletedWorldTicks = 0;
		uint64 ReadyAfterWorldTick = 0;
		bool bSimulationCaughtUp = false;
	};
}
