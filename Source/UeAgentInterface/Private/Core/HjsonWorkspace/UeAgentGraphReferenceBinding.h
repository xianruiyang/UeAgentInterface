#pragma once

#include "CoreMinimal.h"

class FJsonObject;
namespace UeAgentAuthoringState { struct FReadRequest; struct FStateEntry; class FStateSnapshot; }
namespace UE::UAI::Contract { struct FDiagnostic; }

namespace UE::UAI::HjsonWorkspace
{
	/** A reference resolved by the import owner before native mutation. Final
	 * readback may translate only this exact authored field, never infer an
	 * expected identity from the object that happened to be read back. */
	struct FGraphNodeReferenceBinding
	{
		FString GraphUnitId;
		FString NativeNodeId;
		FString FieldId;
		FString AuthoredReference;
		FString NativeReference;
	};
	/** Project authored node fields backed by separate entries in this final capture. */
	using FProjectObservedGraphNodeFields = bool (*)(
		FJsonObject&, UeAgentAuthoringState::FStateEntry&, const UeAgentAuthoringState::FStateSnapshot&,
		const TArray<UeAgentAuthoringState::FReadRequest>*, Contract::FDiagnostic&);
}
