#pragma once

#include "Containers/UnrealString.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class UObject;
class UPackage;

namespace UE::UAI::AssetDurable
{
	enum class EPackageHeaderKind : uint8
	{
		EvidenceRequired,
		Asset,
		Map,
	};

	// Unique owner for the durable header path and package-kind preparation used
	// by package persistence. A loaded UWorld is a map even when legacy package
	// metadata omitted PKG_ContainsMap.
	UEAGENTINTERFACE_API bool TryResolvePackageHeaderFilename(
		const FString& TargetRef,
		EPackageHeaderKind HeaderKind,
		FString& OutFilename,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryPrepareLoadedPackageHeaderForSave(
		UPackage& Package,
		UObject*& InOutTopLevelAsset,
		FString& OutFilename,
		UE::UAI::Contract::FDiagnostic& OutError);
}
