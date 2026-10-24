#pragma once

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

namespace UeAgentReflectionAuthoring { struct FProfile; }

namespace UE::UAI::Composition::HjsonImport
{
	UEAGENTINTERFACE_API bool TryExportReflectionRootWorkspace(
		const UeAgentReflectionAuthoring::FProfile& Profile, const FString& Target,
		const FString& WorkspacePath, FString& OutPayload, HjsonWorkspace::FWorkspaceError& OutError);
	/** Reflection capture/apply bridge. Domain-specific property ordering stays with the caller. */
	UEAGENTINTERFACE_API bool TryImportReflectionWorkspace(
		const UeAgentReflectionAuthoring::FProfile& Profile,
		const HjsonWorkspace::FResolvedSemanticWorkspaceRef& Workspace,
		const HjsonWorkspace::DomainAuthoring::FRootPropertyImportContract& Contract,
		TFunctionRef<void(TArray<TSharedPtr<FJsonValue>>&)> PrepareProperties,
		FString& OutPayload, HjsonWorkspace::FWorkspaceError& OutError);
}
