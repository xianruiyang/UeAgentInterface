// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentExportPublisherTestHooks.h"

namespace
{
	thread_local bool GFailureEnabled = false;
	thread_local UeAgentExportPublisherTestHooks::EFailurePoint GFailurePoint =
		UeAgentExportPublisherTestHooks::EFailurePoint::NthStagedFile;
	thread_local int32 GFailureOrdinal = 0;
	thread_local int32 GFailureHitCount = 0;
}

namespace UeAgentExportPublisherTestHooks
{
	FScopedFailureInjection::FScopedFailureInjection(
		const EFailurePoint FailurePoint,
		const int32 Ordinal)
		: PreviousFailurePoint(GFailurePoint)
		, PreviousOrdinal(GFailureOrdinal)
		, PreviousHitCount(GFailureHitCount)
		, bPreviousEnabled(GFailureEnabled)
	{
		GFailureEnabled = true;
		GFailurePoint = FailurePoint;
		GFailureOrdinal = FMath::Max(1, Ordinal);
		GFailureHitCount = 0;
	}

	FScopedFailureInjection::~FScopedFailureInjection()
	{
		GFailureEnabled = bPreviousEnabled;
		GFailurePoint = PreviousFailurePoint;
		GFailureOrdinal = PreviousOrdinal;
		GFailureHitCount = PreviousHitCount;
	}

	bool ShouldFail(const EFailurePoint FailurePoint)
	{
		if (!GFailureEnabled || GFailurePoint != FailurePoint)
		{
			return false;
		}
		++GFailureHitCount;
		return GFailureHitCount == GFailureOrdinal;
	}
}
