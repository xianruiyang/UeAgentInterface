// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"
#include "Utilities/AuthoringExport/UeAgentSnapshotProjector.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

class FJsonObject;

namespace UeAgentExportWorkspaceWriter
{
	struct UEAGENTINTERFACE_API FTextMetrics
	{
		int64 CharacterCount = 0;
		int64 Utf8Bytes = 0;
		int64 LineCount = 0;
		int64 TokenProxy = 0;

		void Add(const FTextMetrics& Other);
		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	struct UEAGENTINTERFACE_API FPlannedFile
	{
		FString FileId;
		FString RelativePath;
		FString ResolvedPath;
		FString Role;
		TSharedPtr<FJsonObject> Root;
		FUeAgentStructuredHjsonWriteOptions WriteOptions;
		FString HjsonText;
		FTextMetrics Metrics;
		TArray<FString> ReferencedRelativePaths;

		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	struct UEAGENTINTERFACE_API FWorkspaceFragment
	{
		FString FragmentId;
		FString RelativeRoot;
		TArray<FPlannedFile> Files;
	};

	struct UEAGENTINTERFACE_API FRoleCardinality
	{
		FString Role;
		FString PathPattern;
		int32 Minimum = 0;
		int32 Maximum = INDEX_NONE;
	};

	struct UEAGENTINTERFACE_API FCompoundPlanPolicy
	{
		FString ContractId;
		FString ContractVersion;
		FString ProfileId;
		FString EntryFile;
		int32 TokenBudget = 0;
		TArray<FRoleCardinality> RoleCardinalities;
	};

	struct UEAGENTINTERFACE_API FWorkspaceResult
	{
		FString Status;
		FString ErrorCode;
		FString ErrorDetail;
		FString ContractId;
		FString ContractVersion;
		FString ProfileId;
		FString WorkspaceRoot;
		FString EntryFile;
		FString FileId;
		FString FieldId;
		int32 TokenBudget = 0;
		FTextMetrics Metrics;
		TArray<FPlannedFile> Files;
		TArray<FString> WrittenFiles;
		TArray<FString> WrittenRelativeFiles;
		TSharedPtr<FJsonObject> PublisherReceipt;

		bool IsPlanned() const;
		bool IsWritten() const;
		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API bool BuildWorkspacePlan(
		const UeAgentExportContract::FContract& Contract,
		const UeAgentSnapshotProjector::FProjectionResult& Projection,
		const FString& WorkspaceRoot,
		FWorkspaceResult& OutResult);

	UEAGENTINTERFACE_API bool BuildCompoundWorkspacePlan(
		const FString& WorkspaceRoot,
		const TArray<FWorkspaceFragment>& Fragments,
		const FCompoundPlanPolicy& Policy,
		FWorkspaceResult& OutResult);

	UEAGENTINTERFACE_API bool PublishWorkspacePlan(FWorkspaceResult& InOutResult);

	UEAGENTINTERFACE_API bool WriteWorkspace(
		const UeAgentExportContract::FContract& Contract,
		const UeAgentSnapshotProjector::FProjectionResult& Projection,
		const FString& WorkspaceRoot,
		FWorkspaceResult& OutResult);

	UEAGENTINTERFACE_API bool WriteSingleFile(
		const UeAgentExportContract::FContract& Contract,
		const UeAgentSnapshotProjector::FProjectionResult& Projection,
		const FString& OutputFile,
		FWorkspaceResult& OutResult);
}
