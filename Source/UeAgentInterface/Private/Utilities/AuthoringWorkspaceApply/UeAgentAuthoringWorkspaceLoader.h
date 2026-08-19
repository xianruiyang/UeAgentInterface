// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"

class FJsonObject;

namespace UeAgentAuthoringWorkspaceLoader
{
	struct UEAGENTINTERFACE_API FSourceRequest
	{
		UeAgentApplyWorkspaceContract::ESourceKind Kind =
			UeAgentApplyWorkspaceContract::ESourceKind::Directory;
		FString Path;
		FString InlineHjson;
		bool bHasInlineHjson = false;
		TSharedPtr<const FJsonObject> InlineObject;
		FString SourceLabel;

		static FSourceRequest MakeDirectory(const FString& WorkspaceRoot);
		static FSourceRequest MakeSingleFile(const FString& FilePath);
		static FSourceRequest MakeInlineHjson(const FString& Hjson, const FString& Label = TEXT("inline://request"));
		static FSourceRequest MakeInlineObject(
			const TSharedPtr<const FJsonObject>& Object,
			const FString& Label = TEXT("inline://request"));
	};

	struct UEAGENTINTERFACE_API FLoadLimits
	{
		int32 MaxFiles = 4096;
		int64 MaxFileBytes = 16 * 1024 * 1024;
		int64 MaxTotalBytes = 128 * 1024 * 1024;
	};

	struct UEAGENTINTERFACE_API FLoadResult
	{
		struct FEnumerationReceipt
		{
			bool bPresent = false;
			int32 EntriesVisited = 0;
			int64 BytesVisited = 0;
			bool bStoppedEarly = false;

			TSharedRef<FJsonObject> ToJsonObject() const;
		};

		FString Status;
		FString ErrorCode;
		FString ErrorDetail;
		UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
		UeAgentAuthoringWorkspaceRouter::FWorkspace Workspace;
		FEnumerationReceipt Enumeration;

		bool IsLoaded() const;
		TSharedRef<FJsonObject> ToJsonObject(bool bIncludeDocuments = false) const;
	};

	UEAGENTINTERFACE_API bool LoadWorkspace(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const FSourceRequest& Source,
		FLoadResult& OutResult,
		const FLoadLimits& Limits = FLoadLimits());
}
