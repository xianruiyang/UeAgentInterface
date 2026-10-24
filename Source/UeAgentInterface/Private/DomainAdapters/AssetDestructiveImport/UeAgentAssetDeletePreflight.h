#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteSelection.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	struct UEAGENTINTERFACE_API FUeAgentAssetDeletePreparedTarget final
	{
		FString PackageName;
		FString ObjectPath;
		TArray<FUeAgentAssetDeleteSelectionSource> Sources;
		TArray<FString> OwnedExternalPackages;
		TArray<FString> Referencers;
		UE::UAI::Contract::FDiagnostic Diagnostic;

		bool IsExecutable() const
		{
			return Diagnostic.IsEmpty();
		}
	};

	struct UEAGENTINTERFACE_API FUeAgentAssetDeletePreflightResult final
	{
		TArray<FUeAgentAssetDeletePreparedTarget> Targets;
		UE::UAI::Contract::FDiagnostic Diagnostic;

		bool IsSuccess() const
		{
			return Diagnostic.IsEmpty();
		}
	};

	UEAGENTINTERFACE_API FUeAgentAssetDeletePreflightResult
		PrepareAssetDeleteSelection(
			const FUeAgentAssetDeleteSelectionRequest& Request);
}
