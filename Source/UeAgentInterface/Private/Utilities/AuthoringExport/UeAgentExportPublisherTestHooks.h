// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UeAgentExportPublisherTestHooks
{
	enum class EFailurePoint : uint8
	{
		NthStagedFile,
		AfterFinalMovedToBackup,
		LevelFragmentAssembly,
	};

	class UEAGENTINTERFACE_API FScopedFailureInjection
	{
	public:
		explicit FScopedFailureInjection(EFailurePoint FailurePoint, int32 Ordinal = 1);
		~FScopedFailureInjection();

		FScopedFailureInjection(const FScopedFailureInjection&) = delete;
		FScopedFailureInjection& operator=(const FScopedFailureInjection&) = delete;

	private:
		EFailurePoint PreviousFailurePoint;
		int32 PreviousOrdinal = 0;
		int32 PreviousHitCount = 0;
		bool bPreviousEnabled = false;
	};

	UEAGENTINTERFACE_API bool ShouldFail(EFailurePoint FailurePoint);
}
