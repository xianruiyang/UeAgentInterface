#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "CoreMinimal.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"

class UWorld;
class FJsonObject;

namespace UE::UAI::HjsonWorkspace { struct FPreparedWorkspaceExport; }
namespace UeAgentLevelAuthoring { struct FLevelAuthoringCapture; }

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	// Read-only preparation shared by import and proposed scene queries. It neither
	// executes construction scripts nor applies the projected native bundle.
	bool TryPrepareLevelWorkspace(UWorld& World, const FString& WorkspaceRoot,
		const FString& ProfileId,
		Composition::HjsonImport::FPreparedAuthoringImportPtr& OutPrepared,
		TSharedRef<FJsonObject>& OutBundle, HjsonWorkspace::FWorkspaceError& OutError,
		TFunction<Contract::FSourceLocation(const FString&, const FString&)>* OutSourceResolver = nullptr,
		UeAgentLevelAuthoring::FLevelAuthoringCapture* OutCapture = nullptr);
	struct UEAGENTINTERFACE_API FUeAgentWorldActorCandidateQuery final
	{
		FString Target;
		FString Category;
		FString Query;
	};

	UEAGENTINTERFACE_API bool TryQueryWorldActorCandidates(
		const FUeAgentWorldActorCandidateQuery& Query,
		FString& OutPayloadJson,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		HjsonWorkspace::FWorkspaceError& OutError,
		const FString& RequestId = FString(),
		HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport = nullptr,
		const FString& WorkspaceProfileId = FString());
}
