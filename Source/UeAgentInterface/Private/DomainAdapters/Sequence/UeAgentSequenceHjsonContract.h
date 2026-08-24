#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::Sequence
{
	struct FSequenceHjsonPatch final
	{
		TSharedPtr<FJsonObject> Structure;
		TArray<FString> ChangedFields;
		bool bSparseSettings = false;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const FString& GetSequenceHjsonSchemaId();
	UEAGENTINTERFACE_API const FString& GetSequenceHjsonRootType();
	UEAGENTINTERFACE_API const FString& GetSequenceHjsonFieldOwner();
	UEAGENTINTERFACE_API const FString& GetSequenceHjsonFieldSetId();

	UEAGENTINTERFACE_API bool TryBuildSequenceHjsonAuthoringSchema(
		const FString& TargetIdentity,
		TSharedPtr<FJsonObject>& OutSchema,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryBuildSequenceHjsonExportDocument(
		const FString& TargetIdentity,
		TSharedPtr<FJsonObject>& OutDocument,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryResolveSequenceHjsonPatch(
		const FString& TargetIdentity,
		bool bTargetPresent,
		const TSharedPtr<FJsonObject>& Root,
		FSequenceHjsonPatch& OutPatch,
		HjsonWorkspace::FWorkspaceError& OutError);
}
