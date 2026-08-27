#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::TestSupport
{
	bool TryRemoveEmptyFixturePackageDirectory(
		const FString& AllowedOwnerPackage,
		const FString& ExactFixturePackageDirectory,
		FString& OutError);
}
