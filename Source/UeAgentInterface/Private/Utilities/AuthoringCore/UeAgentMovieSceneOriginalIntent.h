#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonValue;

namespace UE::UAI::Authoring::Timeline
{
	// The MovieScene owner, shared by sequences and widget animations, owns
	// integer-clock retiming and removal of empty native sections.
	bool ProjectMovieSceneOriginalIntent(TSharedPtr<FJsonValue>& Before,
		TSharedPtr<FJsonValue>& Desired, TSharedPtr<FJsonValue>& Actual,
		TMap<FString, FString>& SourcePaths, UE::UAI::Contract::FDiagnostic& Error);
}
