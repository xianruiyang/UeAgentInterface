#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Templates/SharedPointer.h"

class UObject;

namespace UE::UAI::DomainAdapters::ContentDiscovery
{
	/** Resolve an exact path, native name, actor label or scoped discovery name
	 * within the caller's actual loaded-object scope. Never choose the first match. */
	UEAGENTINTERFACE_API bool TryResolveScopedObjectReference(
		TConstArrayView<UObject*> Objects, const FString& Reference,
		UObject*& OutObject, UE::UAI::Contract::FDiagnostic& OutError);

	/** Resolve a current search name server-side; existing exact paths remain unchanged. */
	UEAGENTINTERFACE_API bool TryResolveEditorContentReference(
		FString& InOutReference, const FString& RequiredKind,
		UE::UAI::ContentDiscovery::FContentDiscoveryError& OutError);

	UEAGENTINTERFACE_API bool TryCreateEditorAssetContentDiscoveryProvider(
		TSharedPtr<UE::UAI::ContentDiscovery::
			IUeAgentContentDiscoveryProvider>& OutProvider,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateEditorWorldContentDiscoveryProvider(
		TSharedPtr<UE::UAI::ContentDiscovery::
			IUeAgentContentDiscoveryProvider>& OutProvider,
		UE::UAI::Contract::FDiagnostic& OutError);
}
