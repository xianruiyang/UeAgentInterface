#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UWorld;

namespace UE::UAI::EditorWorld
{
enum class ECurrentWorldDirtyPolicy : uint8
{
	RequireClean,
	SaveDirtyMapPackages,
};

	using FWorldActivationError = UE::UAI::Contract::FDiagnostic;

struct FWorldActivationResult
{
	UWorld* World = nullptr;
	FString OriginalPackage;
	bool bOriginalHasPersistentPackage = false;
	bool bSwitched = false;
};

bool TryActivateExistingWorld(const FString& TargetPackage, ECurrentWorldDirtyPolicy DirtyPolicy,
	FWorldActivationResult& OutResult, FWorldActivationError& OutError);
} // namespace UE::UAI::EditorWorld
