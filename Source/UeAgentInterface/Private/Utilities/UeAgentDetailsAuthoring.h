#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"

class FJsonObject;

namespace UeAgentDetailsAuthoring
{
	struct FOwnerInfo
	{
		FString OwnerId;
		FString OwnerKind;
		FString OwnerName;
		FString OwnerPath;
		FString OwnerClassPath;
		FString SourceCommand;
	};

	struct FOwnerPropertiesInput
	{
		bool bPresent = false;
		bool bInline = false;
		FString PropertiesFieldName;
		FString RefFieldName;
		FString DetailsRoot;
		FString IndexFile;
		TSharedPtr<FJsonObject> PropertiesJson;
	};

	struct UEAGENTINTERFACE_API FOwnerDetailsExportRequest
	{
		FOwnerDetailsExportRequest();

		UObject* Object;
		FOwnerInfo OwnerInfo;
		FString DetailsRoot;
		FString IndexFile;
		FString RefFieldName;
		UeAgentDetailsAuthoringService::FCapturePolicy PropertyOptions;
	};

	struct UEAGENTINTERFACE_API FOwnerDetailsApplyRequest
	{
		FOwnerDetailsApplyRequest();

		UObject* Object;
		FOwnerInfo OwnerInfo;
		FString DetailsRoot;
		FString IndexFile;
		FString RefFieldName;
		UeAgentAuthoringObjectApply::FOptions ApplyOptions;
	};

	struct UEAGENTINTERFACE_API FOwnerDetailsValidateRequest
	{
		FOwnerDetailsValidateRequest();

		FOwnerInfo OwnerInfo;
		FString DetailsRoot;
		FString FolderRoot;
		FString IndexFile;
		FString RefFieldName;
		TSharedPtr<FJsonObject> EntryObject;
		TArray<FString> CoverageGaps;
	};

	struct UEAGENTINTERFACE_API FOwnerDetailsDiffRequest
	{
		FOwnerDetailsDiffRequest();

		FOwnerInfo OwnerInfo;
		FString OldDetailsRoot;
		FString NewDetailsRoot;
		FString OldFolderRoot;
		FString NewFolderRoot;
		FString OldIndexFile;
		FString NewIndexFile;
		FString RefFieldName;
		TSharedPtr<FJsonObject> OldEntryObject;
		TSharedPtr<FJsonObject> NewEntryObject;
		TArray<FString> CoverageGaps;
	};

	struct UEAGENTINTERFACE_API FDiagnosticsIsolationRequest
	{
		FDiagnosticsIsolationRequest();

		FOwnerInfo OwnerInfo;
		FString FolderRoot;
		FString AllowedTarget;
		TSharedPtr<FJsonObject> EntryObject;
	};

	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildOwnerRef(const FOwnerInfo& OwnerInfo);

	UEAGENTINTERFACE_API FOwnerInfo MakeOwnerInfoForObject(
		UObject* Object,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& SourceCommand,
		const FString& OwnerName = FString());

	UEAGENTINTERFACE_API FOwnerDetailsExportRequest MakeOwnerDetailsExportRequest(
		UObject* Object,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& DetailsRoot,
		const FString& SourceCommand,
		const FString& IndexFile = FString(),
		const FString& RefFieldName = FString());

	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> CloneObjectTree(const TSharedPtr<FJsonObject>& Object);

	UEAGENTINTERFACE_API bool ExportOwnerDetails(
		const FOwnerDetailsExportRequest& Request,
		TSharedPtr<FJsonObject>& OutDetailsSummary,
		FString& OutError);

	UEAGENTINTERFACE_API bool ApplyOwnerDetails(
		const FOwnerDetailsApplyRequest& Request,
		TSharedPtr<FJsonObject>& OutApplySummary,
		FString& OutError);

	UEAGENTINTERFACE_API bool ApplyOwnerDetailsFromEntryIfPresent(
		UObject* Object,
		const FString& EntryDirectory,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FOwnerInfo& OwnerInfo,
		const FString& RefFieldName,
		bool bApply,
		TSharedPtr<FJsonObject>& OutApplySummary,
		int32& InOutAppliedCount,
		FString& OutError);

	UEAGENTINTERFACE_API bool ResolveOwnerPropertiesFromEntry(
		const FString& EntryDirectory,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& RefFieldName,
		FOwnerPropertiesInput& OutInput,
		FString& OutError);

	UEAGENTINTERFACE_API bool ValidateOwnerDetails(
		const FOwnerDetailsValidateRequest& Request,
		TSharedPtr<FJsonObject>& OutValidateSummary,
		FString& OutError);

	UEAGENTINTERFACE_API bool DiffOwnerDetails(
		const FOwnerDetailsDiffRequest& Request,
		TSharedPtr<FJsonObject>& OutDiffSummary,
		FString& OutError);

	UEAGENTINTERFACE_API FString GetDiagnosticsOnlyPath(const FString& Surface);

	UEAGENTINTERFACE_API bool IsDiagnosticsOnlyFieldName(const FString& FieldName);

	UEAGENTINTERFACE_API bool IsDiagnosticsOnlyRelativePath(const FString& RelativePath);

	UEAGENTINTERFACE_API bool BuildDiagnosticsIsolationReport(
		const FDiagnosticsIsolationRequest& Request,
		TSharedPtr<FJsonObject>& OutReport,
		FString& OutError);

	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> MakeAuthoringPropertiesRejection(
		const FString& ErrorCode,
		const FString& Surface,
		const FOwnerInfo& OwnerInfo,
		const FString& JsonPath,
		const FString& ResolutionHint,
		const FString& AllowedTarget);

	UEAGENTINTERFACE_API bool FindAuthoringPropertiesIssue(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutSurface,
		FString& OutJsonPath,
		FString& OutErrorCode);

	UEAGENTINTERFACE_API bool FindAuthoringPropertiesFileIssue(
		const FString& FolderRoot,
		FString& OutSurface,
		FString& OutJsonPath,
		FString& OutErrorCode);

	UEAGENTINTERFACE_API bool BuildAuthoringPropertiesRejectionIfPresent(
		const TSharedPtr<FJsonObject>& Object,
		const FOwnerInfo& OwnerInfo,
		const FString& AllowedTarget,
		TSharedPtr<FJsonObject>& OutRejection);

	UEAGENTINTERFACE_API bool BuildAuthoringPropertiesFileRejectionIfPresent(
		const FString& FolderRoot,
		const FOwnerInfo& OwnerInfo,
		const FString& AllowedTarget,
		TSharedPtr<FJsonObject>& OutRejection);

	UEAGENTINTERFACE_API bool BuildAuthoringPropertiesFolderRejectionIfPresent(
		const FString& FolderRoot,
		const FOwnerInfo& OwnerInfo,
		const FString& AllowedTarget,
		TSharedPtr<FJsonObject>& OutRejection,
		FString& OutError);

	UEAGENTINTERFACE_API bool ResolveDetailsRefRoot(
		const FString& EntryDirectory,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& RefFieldName,
		bool& bOutHasDetails,
		FString& OutDetailsRoot,
		FString& OutIndexFile,
		FString& OutError);

	UEAGENTINTERFACE_API bool WriteGroupedDetails(
		const FString& DetailsRoot,
		const FOwnerInfo& OwnerInfo,
		const TSharedPtr<FJsonObject>& PropertiesJson,
		TSharedPtr<FJsonObject>& OutDetailsSummary,
		FString& OutError,
		const FString& IndexFile = FString());

	UEAGENTINTERFACE_API bool LoadGroupedDetails(
		const FString& DetailsRoot,
		TSharedPtr<FJsonObject>& OutPropertiesJson,
		FString& OutError,
		const FString& IndexFile = FString());

	UEAGENTINTERFACE_API bool HasGroupedDetails(const FString& DetailsRoot, const FString& IndexFile = FString());

	UEAGENTINTERFACE_API bool ExternalizeInlineDetailsInObjectTree(
		const FString& EntryFilePath,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& SourceCommand,
		FString& OutError,
		bool bScopeToEntryFile = false);

	UEAGENTINTERFACE_API void RemoveDiagnosticAuthoringFieldsInObjectTree(
		const TSharedPtr<FJsonObject>& EntryObject);

	UEAGENTINTERFACE_API bool RehydrateGroupedDetailsInObjectTree(
		const FString& EntryFilePath,
		const TSharedPtr<FJsonObject>& EntryObject,
		FString& OutError);
}
