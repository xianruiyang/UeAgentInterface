#pragma once

#include "CoreMinimal.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"

class UObject;
class UClass;
class FProperty;
class FObjectPropertyBase;

namespace UeAgentAuthoringObjectApply
{
	using FDiagnostic = UE::UAI::Contract::FDiagnostic;
	struct FOwnedClassCandidate
	{
		UE::UAI::ContentDiscovery::FNameSelectionEntry Selection;
		UClass* Class = nullptr;
	};

	/** Native mechanics for an already authorized owner/property context. No domain policy or name parsing. */
	class UEAGENTINTERFACE_API FNativeOwnedCreationContext
	{
	public:
		FNativeOwnedCreationContext();
		~FNativeOwnedCreationContext();
		/** Prospective paths may address new struct-array elements; an omitted terminal array index appends on scratch only. */
		bool Initialize(UObject* Owner, FProperty* Property, bool bProspective, FDiagnostic& Error, int32 ArrayIndex = INDEX_NONE);
		bool Initialize(UObject* Owner, const FString& PropertyPath, bool bProspective, FDiagnostic& Error, int32 ArrayIndex = INDEX_NONE);
		bool InitializeScopedMember(UObject* Owner, const FString& RootMember, FObjectPropertyBase* Property,
			bool bProspective, FDiagnostic& Error);
		bool Collect(TArray<FOwnedClassCandidate>& Candidates, FDiagnostic& Error) const;
		bool Allows(UClass* Class) const;
		UObject* Create(UClass* Class, FDiagnostic& Error);
	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};
}
