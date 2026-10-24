#pragma once

#include "Containers/UnrealString.h"
#include "Templates/Function.h"

namespace UE::UAI
{
	// A display name is not an identity. Callers first reuse an existing identity's
	// name, then allocate only for a distinct entry in their own namespace.
	inline FString MakeNumberedReadableName(const FString& Base, int32 Ordinal)
	{
		return Ordinal <= 1 ? Base : Base + TEXT("#") + FString::FromInt(Ordinal);
	}

	// Keep native names intact: neither "X2" nor "X#2" is parsed as a suffix of X.
	// The caller defines case sensitivity, reserved names and file extensions.
	inline FString AllocateReadableName(const FString& Base,
		TFunctionRef<bool(const FString&)> IsUnavailable, int32 FirstOrdinal = 1)
	{
		int32 Ordinal = FirstOrdinal;
		FString Name = MakeNumberedReadableName(Base, Ordinal);
		while (IsUnavailable(Name)) Name = MakeNumberedReadableName(Base, ++Ordinal);
		return Name;
	}
}
