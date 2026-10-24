#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class UWorld;

namespace UE::UAI::DomainAdapters::WorldSpatial::Private
{
	bool BuildViewportWarnings(
		UWorld* World,
		bool bIncludeSuppressed,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError);
	FString NormalizeFolderPathForMatch(const FString& InFolderPath);
	bool DoesActorMatchFolderPath(AActor* Actor, const FString& NormalizedFolderPath, bool bIncludeChildFolders);
	bool GetFolderActorBounds(UWorld* World, const FString& FolderPath, bool bIncludeChildFolders, FBox& OutCombinedBounds, TArray<AActor*>& OutActors, UE::UAI::Contract::FDiagnostic& OutError);
	bool GetActorsInFolder(UWorld* World, const FString& FolderPath, bool bIncludeChildFolders, TArray<AActor*>& OutActors, UE::UAI::Contract::FDiagnostic& OutError);
}
