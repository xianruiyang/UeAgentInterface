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
		// Transformed documents retain their original source through this typed sidecar.
		TFunction<UeAgentAuthoringDiagnostics::FSourceLocation(const FString&, const FString&)> ResolveSource;

		static FSourceRequest MakeDirectory(const FString& WorkspaceRoot);
		static FSourceRequest MakeSingleFile(const FString& FilePath);
		static FSourceRequest MakeInlineHjson(const FString& Hjson, const FString& Label = TEXT("inline://request"));
		static FSourceRequest MakeInlineObject(
			const TSharedPtr<const FJsonObject>& Object,
			const FString& Label = TEXT("inline://request"));
	};

	struct UEAGENTINTERFACE_API FLoadResult
	{
		struct FEnumerationReceipt
		{
			bool bPresent = false;
			int32 EntriesVisited = 0;
			int64 BytesVisited = 0;

			TSharedRef<FJsonObject> ToJsonObject() const;
		};

		FString Status;
		UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
		UeAgentAuthoringWorkspaceRouter::FWorkspace Workspace;
		FEnumerationReceipt Enumeration;

		bool IsLoaded() const;
		TSharedRef<FJsonObject> ToJsonObject(bool bIncludeDocuments = false) const;
	};

	UEAGENTINTERFACE_API bool LoadWorkspace(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const FSourceRequest& Source,
		FLoadResult& OutResult);
}
