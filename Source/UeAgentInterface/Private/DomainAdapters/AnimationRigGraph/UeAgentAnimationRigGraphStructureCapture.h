#pragma once

#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphAdapter.h"
#include "Dom/JsonObject.h"

class UObject;

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	struct FAnimationRigGraphStructureValue final
	{
		FString SemanticId;
		FString ItemKind;
		FString ClassPath;
		TSharedRef<FJsonObject> Value;
	};

	UEAGENTINTERFACE_API bool TryCaptureAnimationRigGraphStructure(
		UObject& Target,
		TArray<FAnimationRigGraphStructureValue>& OutValues,
		FAnimationRigGraphAdapterError& OutError);
}
