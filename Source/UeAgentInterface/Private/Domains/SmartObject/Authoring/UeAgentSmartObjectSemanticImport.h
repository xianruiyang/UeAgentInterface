#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class USmartObjectDefinition;

namespace UE::UAI::Composition::HjsonImport
{
	class FPreparedAuthoringImport;
	struct FSemanticWorkspaceChangeProjection;
}
namespace UE::UAI::HjsonWorkspace
{
	class FAuthoringModel;
	class FWorkspaceError;
}

namespace UeAgentSmartObjectAuthoring
{
	bool BuildDefinitionDocumentFromSemanticModel(USmartObjectDefinition& Definition,
		const TSharedRef<const UE::UAI::HjsonWorkspace::FAuthoringModel>& Model,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		TSharedPtr<FJsonObject>& OutDocument,
		UE::UAI::HjsonWorkspace::FWorkspaceError& Error);
}
