// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

struct FRequestPlaySessionParams;

namespace UE::UAI::DomainAdapters::EditorSessionOperation
{
	// Request-local native compilation policy, retained across queued PIE startup.
	// Both runtime-preview consumers use the same cancellation and diagnostics.
	class FPieLaunchPolicy
	{
	public:
		using FCompletion = TFunction<void(bool, const Contract::FDiagnostic&)>;
		FPieLaunchPolicy();
		~FPieLaunchPolicy();
		bool RequestPlaySession(FRequestPlaySessionParams& Params, FCompletion Completion, Contract::FDiagnostic& Error);
		void Disarm();
	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};
}
