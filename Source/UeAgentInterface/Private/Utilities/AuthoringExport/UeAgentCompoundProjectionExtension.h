// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentCompoundProjection
{
	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered,
	};

	struct UEAGENTINTERFACE_API FExtensionDescriptor
	{
		FString ProjectionId;
		FString ImplementationId;
		TArray<UeAgentExportContract::EFieldSourceKind> SourceKinds;

		bool SupportsSourceKind(UeAgentExportContract::EFieldSourceKind Kind) const;
		bool operator==(const FExtensionDescriptor& Other) const;
		bool operator!=(const FExtensionDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FRequest
	{
		const UeAgentExportContract::FContract* Contract = nullptr;
		const UeAgentExportContract::FProfileDescriptor* Profile = nullptr;
		const UeAgentExportContract::FFileDescriptor* File = nullptr;
		const UeAgentExportContract::FFieldDescriptor* Field = nullptr;
		FString JsonPointerPattern;
		int32 WildcardCount = 0;
		TArray<UeAgentAuthoringState::FStateEntry> Entries;

		bool IsValid() const
		{
			return Contract && Profile && File && Field && WildcardCount > 0;
		}
	};

	// Extensions select structure only. Canonical values are projected from these
	// snapshot entry indices by the shared projector after the callback returns.
	struct UEAGENTINTERFACE_API FExpansionSelection
	{
		TArray<FString> WildcardValues;
		TArray<int32> EntryIndices;
	};

	using FExtension = TFunction<bool(
		const FRequest& Request,
		TArray<FExpansionSelection>& OutSelections,
		UE::UAI::Contract::FDiagnostic& OutError)>;

	class UEAGENTINTERFACE_API FRegistry
	{
	public:
		FRegistry() = default;
		FRegistry(const FRegistry&) = delete;
		FRegistry& operator=(const FRegistry&) = delete;

		static FRegistry& Get();

		bool RegisterExtension(
			const FExtensionDescriptor& Descriptor,
			FExtension Extension,
			ERegistrationResult& OutResult,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool FindExtension(const FString& ProjectionId, FExtensionDescriptor& OutDescriptor) const;
		bool CopyExtension(const FString& ProjectionId, FExtension& OutExtension) const;
		TArray<FExtensionDescriptor> ListExtensions() const;
		int32 GetExtensionCount() const;

	private:
		struct FEntry
		{
			FExtensionDescriptor Descriptor;
			FExtension Extension;
		};

		mutable FRWLock RegistryLock;
		TMap<FString, FEntry> Extensions;
	};

	struct UEAGENTINTERFACE_API FJsonPointerPatternInfo
	{
		int32 WildcardCount = 0;
		FString EmptyContainerPointer;
	};

	UEAGENTINTERFACE_API bool InspectJsonPointerPattern(
		const FString& JsonPointerPattern,
		FJsonPointerPatternInfo& OutInfo,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ExpandJsonPointerPattern(
		const FString& JsonPointerPattern,
		const TArray<FString>& WildcardValues,
		FString& OutJsonPointer,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool MatchesExpandedJsonPointer(
		const FString& JsonPointerPattern,
		const FString& JsonPointer);
}
