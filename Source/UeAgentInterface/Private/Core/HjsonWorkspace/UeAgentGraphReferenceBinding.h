#pragma once

#include "CoreMinimal.h"

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
}
