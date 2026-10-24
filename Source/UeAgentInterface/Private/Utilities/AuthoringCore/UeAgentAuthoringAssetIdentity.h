// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UeAgentAuthoringAssetIdentity
{
	/**
	 * Converts package and object-path spellings to a trimmed package path.
	 * This function intentionally does not validate mounts, existence, or policy.
	 */
	UEAGENTINTERFACE_API FString NormalizePackagePath(const FString& InPath);

	/**
	 * Converts a package or object path to the conventional Package.Asset form.
	 * Empty input remains empty; validity and existence remain caller concerns.
	 */
	UEAGENTINTERFACE_API FString ToObjectPath(const FString& InPath);
}
