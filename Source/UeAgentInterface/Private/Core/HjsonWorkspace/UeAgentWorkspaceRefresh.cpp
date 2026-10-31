#include "Core/HjsonWorkspace/UeAgentWorkspaceRefresh.h"

#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::HjsonWorkspace
{
	Contract::FDiagnostic MakeAppliedWorkspaceFailure(const FString& Code, const FWorkspaceError& Cause)
	{
		auto Diagnostic = Contract::FDiagnostic::Failure(Code,
            TEXT("The asset changes were applied, but the workspace or its result could not be refreshed."),
            TEXT("workspace_refresh"));
        Diagnostic.PublicCause = MakeShared<Contract::FDiagnostic>(Cause.GetDiagnostic());
		Diagnostic.CauseCode = Cause.GetCode();
		Diagnostic.TransactionStatus = TEXT("committed");
		Diagnostic.AssetApplied = true;
		Diagnostic.RetryImport = false;
		Diagnostic.bRetrySafe = false;
		Diagnostic.RecoveryAction = TEXT("export_current_asset_to_new_workspace");
		return Diagnostic;
	}

	bool TryRefreshImportedWorkspace(
		const FString& WorkspaceRoot, const TArray<FString>& ManagedSourcePaths,
		TFunctionRef<bool(const FString&, FPreparedWorkspaceExport&, FWorkspaceError&)> ExportCurrent,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& OutError,
		TArray<Contract::FDiagnostic>& OutWarnings)
	{
		OutWarnings.Reset();
		FString Root = FPaths::ConvertRelativePathToFull(WorkspaceRoot);
		FPaths::NormalizeDirectoryName(Root);
		const FString Fresh = FPaths::GetPath(Root) / (TEXT(".uai-refresh-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
		if (IFileManager::Get().DirectoryExists(*Fresh))
		{
			auto Error = Contract::FDiagnostic::Failure(TEXT("workspace_refresh_destination_exists"),
				TEXT("The generated workspace refresh destination already exists."), TEXT("workspace_refresh"), Fresh);
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = WorkspaceRoot;
			OutError.SetFailure(Error);
			return false;
		}
		FPreparedWorkspaceExport PreparedExport;
		const bool bExported = ExportCurrent(Fresh, PreparedExport, OutError);
		if (!bExported && !OutError.HasErrors())
		{
			auto Error = Contract::FDiagnostic::Failure(TEXT("workspace_refresh_export_failed"),
				TEXT("The current asset could not be exported for workspace refresh."), TEXT("workspace_refresh"), Fresh);
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = WorkspaceRoot;
			OutError.SetFailure(Error);
		}
		bool bPublished = false;
		if (bExported)
		{
			ArtifactPublisher::FArtifactPublishReceipt Receipt;
			ArtifactPublisher::FArtifactPublishError PublishError;
			bPublished = PreparedExport.Plan.IsValid()
				? ArtifactPublisher::TryRefreshPlannedWorkspace(PreparedExport.Plan.ToSharedRef(),
					PreparedExport.PublisherOwnerId, Root, ManagedSourcePaths, Resources, Receipt, PublishError)
				: ArtifactPublisher::TryRefreshPublishedWorkspace(
					Fresh, Root, ManagedSourcePaths, Resources, Receipt, PublishError);
			if (!bPublished)
			{
				OutError.SetFailure(PublishError);
			}
			else
				OutWarnings.Append(Receipt.GetCleanupWarnings());
		}
		// Only this invocation's generated export directory is disposable.
		if (IFileManager::Get().DirectoryExists(*Fresh)
			&& !IFileManager::Get().DeleteDirectory(*Fresh, false, true))
		{
			auto Warning = Contract::FDiagnostic::Failure(TEXT("workspace_refresh_temporary_cleanup_failed"),
				TEXT("The temporary export directory could not be removed after workspace refresh."), TEXT("workspace_refresh"), Fresh);
			Warning.Severity = Contract::ESeverity::Warning;
			Warning.SourceKind = TEXT("native_result");
			Warning.TargetPath = WorkspaceRoot;
			if (bPublished) OutWarnings.Add(Warning);
			else
			{
				auto Failure = OutError.GetDiagnostic();
				Failure.Append(Warning);
				OutError.SetFailure(Failure);
			}
			UE_LOG(LogTemp, Warning, TEXT("UAI workspace refresh temporary cleanup failed: %s"), *Fresh);
		}
		return bPublished;
	}

	bool TryRefreshImportedWorkspace(
		const FString& WorkspaceRoot, const TArray<FString>& ManagedSourcePaths,
		TFunctionRef<bool(const FString&, FWorkspaceError&)> ExportCurrent,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& OutError,
		TArray<Contract::FDiagnostic>& OutWarnings)
	{
		return TryRefreshImportedWorkspace(WorkspaceRoot, ManagedSourcePaths,
			[&](const FString& Destination, FPreparedWorkspaceExport&, FWorkspaceError& Error)
			{ return ExportCurrent(Destination, Error); }, Resources, OutError, OutWarnings);
	}
}
