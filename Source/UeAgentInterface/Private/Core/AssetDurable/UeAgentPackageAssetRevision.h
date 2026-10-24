#pragma once

#include "Containers/UnrealString.h"

class UObject;

namespace UE::UAI::Contract { struct FDiagnostic; }

namespace UE::UAI::AssetDurable
{
	// One durable package revision authority is shared by operation producers and
	// consumers that hand revisions across command boundaries.
	UEAGENTINTERFACE_API bool TryReadPackageAssetRevision(
		const FString& TargetRef, FString& OutRevision, Contract::FDiagnostic& OutError);

	// Uses the persisted package header when one exists so revisions remain
	// stable across object reloads and requests. Transient/unsaved fixtures retain
	// an object CRC revision until they acquire a durable package header.
	UEAGENTINTERFACE_API bool TryReadStableAssetRevision(
		UObject* Object, FString& OutRevision, Contract::FDiagnostic& OutError);
}
