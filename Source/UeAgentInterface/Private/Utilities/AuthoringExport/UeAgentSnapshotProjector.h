// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

class FJsonObject;
class FJsonValue;

namespace UeAgentCompoundProjection
{
	class FRegistry;
}

namespace UeAgentSnapshotProjector
{
	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered,
	};

	struct UEAGENTINTERFACE_API FProjectorDescriptor
	{
		FString ProjectorId;
		FString ImplementationId;
		TArray<UeAgentExportContract::EFieldSourceKind> SourceKinds;
		bool bListOutput = false;

		bool SupportsSourceKind(UeAgentExportContract::EFieldSourceKind Kind) const;
		bool operator==(const FProjectorDescriptor& Other) const;
		bool operator!=(const FProjectorDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FProjectorRequest
	{
		const UeAgentExportContract::FContract* Contract = nullptr;
		const UeAgentExportContract::FProfileDescriptor* Profile = nullptr;
		const UeAgentExportContract::FFieldDescriptor* Field = nullptr;
		TArray<UeAgentAuthoringState::FStateEntry> Entries;

		bool IsValid() const { return Contract && Profile && Field; }
	};

	struct UEAGENTINTERFACE_API FProjectedValue
	{
		bool bHasValue = false;
		TSharedPtr<FJsonValue> Value;
	};

	using FProjector = TFunction<bool(
		const FProjectorRequest& Request,
		FProjectedValue& OutValue,
		FString& OutError)>;

	class UEAGENTINTERFACE_API FProjectorRegistry
	{
	public:
		FProjectorRegistry() = default;
		FProjectorRegistry(const FProjectorRegistry&) = delete;
		FProjectorRegistry& operator=(const FProjectorRegistry&) = delete;

		static FProjectorRegistry& Get();

		bool RegisterProjector(
			const FProjectorDescriptor& Descriptor,
			FProjector Projector,
			ERegistrationResult& OutResult,
			FString& OutError);
		bool FindProjector(const FString& ProjectorId, FProjectorDescriptor& OutDescriptor) const;
		bool CopyProjector(const FString& ProjectorId, FProjector& OutProjector) const;
		TArray<FProjectorDescriptor> ListProjectors() const;
		int32 GetProjectorCount() const;

	private:
		struct FEntry
		{
			FProjectorDescriptor Descriptor;
			FProjector Projector;
		};

		mutable FRWLock RegistryLock;
		TMap<FString, FEntry> Projectors;
	};

	struct UEAGENTINTERFACE_API FProjectedField
	{
		FString FieldId;
		FString ContractFieldId;
		FString JsonPointer;
		FString Role;
		bool bEditable = false;
		bool bCompoundContainerFallback = false;
		TSharedPtr<FJsonValue> Value;
	};

	struct UEAGENTINTERFACE_API FProjectedFile
	{
		FString FileId;
		FString Path;
		FString Role;
		bool bRequired = false;
		TArray<FProjectedField> Fields;
		TSharedPtr<FJsonObject> Root;
	};

	struct UEAGENTINTERFACE_API FProjectionResult
	{
		FString Status;
		FString ErrorCode;
		FString ErrorDetail;
		FString ContractId;
		FString ContractVersion;
		FString ProfileId;
		FString FileId;
		FString FieldId;
		TArray<FProjectedFile> Files;

		bool IsProjected() const;
		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API bool BuildProjectedFileRoot(
		const FProjectedFile& File,
		TSharedPtr<FJsonObject>& OutRoot,
		FString& OutError);

	UEAGENTINTERFACE_API bool RegisterBuiltInProjectors(
		FProjectorRegistry& Registry,
		FString& OutError);
	UEAGENTINTERFACE_API bool ProjectSnapshot(
		const UeAgentExportContract::FContract& Contract,
		const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		FProjectionResult& OutResult,
		const FProjectorRegistry* ProjectorRegistry = nullptr,
		const UeAgentCompoundProjection::FRegistry* CompoundProjectionRegistry = nullptr);
}
