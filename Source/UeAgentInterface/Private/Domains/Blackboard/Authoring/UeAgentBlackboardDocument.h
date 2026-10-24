#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UBlackboardData;
namespace UE::UAI::HjsonWorkspace { class FAuthoringModel; class FWorkspaceError; }
namespace UE::UAI::Composition::HjsonImport
{
	class FPreparedAuthoringImport;
	struct FSemanticWorkspaceChangeProjection;
}

namespace UeAgentBlackboardAuthoring
{
	UEAGENTINTERFACE_API bool ValidateDocument(
		const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ReadDocument(
		UBlackboardData& Blackboard, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
	// Called inside the Reflection transaction, which owns checkpoint, persistence and reload.
	UEAGENTINTERFACE_API bool ApplyDocument(
		UBlackboardData& Blackboard, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
	bool BuildDocumentFromSemanticModel(
		UBlackboardData& Blackboard,
		const TSharedRef<const UE::UAI::HjsonWorkspace::FAuthoringModel>& Model,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		TSharedPtr<FJsonObject>& OutDocument,
		UE::UAI::HjsonWorkspace::FWorkspaceError& Error);
}
