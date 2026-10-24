#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	struct UEAGENTINTERFACE_API FUeAgentAssetDeleteSelectionRequest final
	{
		TArray<FString> Targets;
		bool bRecursive = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentAssetDeleteSelectionSource final
	{
		FString InputRef;
		bool bFromFolder = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentAssetDeleteSelectionItem final
	{
		FString PackageName;
		FString ObjectPath;
		TArray<FUeAgentAssetDeleteSelectionSource> Sources;
	};

	struct UEAGENTINTERFACE_API FUeAgentAssetDeleteSelectionResult final
	{
		TArray<FUeAgentAssetDeleteSelectionItem> Items;
		UE::UAI::Contract::FDiagnostic Diagnostic;

		bool IsSuccess() const
		{
			return Diagnostic.IsEmpty();
		}
	};

	UEAGENTINTERFACE_API FUeAgentAssetDeleteSelectionResult
		ResolveAssetDeleteSelection(
			const FUeAgentAssetDeleteSelectionRequest& Request);
}
