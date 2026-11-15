#include "Core/HjsonWorkspace/UeAgentWorkspaceRefresh.h"

#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentWorkspacePublication.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceExport.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::HjsonWorkspace
{
	void ConfigureWorkspacePublication(const FPreparedWorkspaceExport* Prepared, ArtifactPublisher::FArtifactPublishRequest& Request)
	{
		if (!Prepared) return;
		// ConsumerState is sealed. Receipt observations are not part of the
		// publisher's source/W+R proof and must not occupy its kernel budget.
		UeAgentAuthoringWorkspaceTransaction::ReleaseDurableConsumerHistoryReads();
		Request.SetVerification(Prepared->Verification);
		Request.SetConsumerState(Prepared->ConsumerState);
		Request.SetManagedSourcePaths(Prepared->ManagedSourcePaths);
		if (Prepared->SourceCheckpoint) Request.SetSourceCheckpoint(
			[Checkpoint = Prepared->SourceCheckpoint](ArtifactPublisher::FArtifactPublishError& Error) {
				FWorkspaceError SourceError;
				if (Checkpoint(TEXT("before_workspace_publish"), SourceError)) return true;
				Error = SourceError.GetDiagnostic(); return false;
			});
	}
	namespace {
		bool MakeRefreshCheckpoint(const FString& Root, FHjsonOperationCheckpoint& Out, FWorkspaceError& Error)
		{
			FHjsonOperationCheckpoint Source;
			if (!TryMakeSemanticWorkspaceSourceCheckpoint(Root, Source, Error)) return false;
			FString AcceptedConsumer;
			ArtifactPublisher::FArtifactPublishError PublishError;
			if (!ArtifactPublisher::TryReadWorkspaceConsumerState(Root, FString(), AcceptedConsumer, PublishError)) {
				Error.SetFailure(MoveTemp(PublishError)); return false;
			}
			Out = [Root, AcceptedConsumer = MoveTemp(AcceptedConsumer), Source = MoveTemp(Source)]
				(const FString& Stage, FWorkspaceError& CheckError) {
				if (!Source(Stage, CheckError)) return false;
				FString Current;
				ArtifactPublisher::FArtifactPublishError ReadError;
				if (!ArtifactPublisher::TryReadWorkspaceConsumerState(Root, FString(), Current, ReadError)) {
					CheckError.SetFailure(MoveTemp(ReadError)); return false;
				}
				if (!Current.Equals(AcceptedConsumer, ESearchCase::CaseSensitive)) {
					CheckError.SetSourceFailure(TEXT("workspace_committed_watermark_changed"),
						TEXT("The published consumer watermark changed after the source was accepted."), Root);
					return false;
				}
				return true;
			};
			return true;
		}
	}
	bool TryPrepareIncrementalWorkspaceExport(const FString& WorkspaceRoot, const FString& Target,
		const FString& ProfileId, ContextResource::FUeAgentResourceTracker& Resources,
		TArray<FString>& OutManagedPaths, FHjsonOperationCheckpoint& OutSourceCheckpoint, FWorkspaceError& OutError,
		bool bAllowProfileChange, bool bAllowAppliedCommittedInput)
	{
		// Both explicit full and incremental preparation consume the formal
		// directory-state Source owner. A formatter memo or an optional lease
		// does not select a second reader or qualify a valid full request.
		const auto SourceObject = MakeShared<FJsonObject>();
		SourceObject->SetStringField(TEXT("kind"), TEXT("directory"));
		SourceObject->SetStringField(TEXT("path"), WorkspaceRoot);
		FResolvedContentGraphImportSourcePtr Source;
		FResolvedSemanticWorkspacePtr Workspace;
		const double LoadStarted = FPlatformTime::Seconds();
		if (!TryLoadContentGraphImportWorkspace(SourceObject, Target, Resources, Source, Workspace, OutError)) return false;
		const double LoadedAt = FPlatformTime::Seconds();
		if (!Source->IsSemanticWorkspaceDirectory()) {
			OutError.SetSourceFailure(TEXT("workspace_incremental_baseline_unsupported"),
				TEXT("This legacy workspace has no semantic incremental baseline; use explicit full export or a new workspace."), WorkspaceRoot);
			return false;
		}
		if (!bAllowProfileChange && Workspace->GetModel()->GetProfileId() != ProfileId) {
			OutError.SetSourceFailure(TEXT("workspace_incremental_profile_mismatch"),
				TEXT("Incremental export requires the existing workspace profile; choose a new workspace or explicit full export."), WorkspaceRoot);
			return false;
		}
		if (!Workspace->GetEditedUnitIds().IsSet() || !Workspace->GetEditedUnitIds()->IsEmpty()
			|| !Workspace->GetStructuralEdits().IsEmpty()) {
			if (bAllowAppliedCommittedInput) {
				if (!TryPrepareCommittedAuthoringWriteback(WorkspaceRoot, Workspace.ToSharedRef(), OutError)) return false;
			} else {
			OutError.SetSourceFailure(TEXT("workspace_incremental_unapplied_edits"),
				TEXT("The workspace contains unapplied authored edits or lacks an exact edit baseline; import them or export to a new workspace."), WorkspaceRoot);
			return false;
			}
		}
		OutManagedPaths = Workspace->GetManagedUnitPaths();
		OutManagedPaths.AddUnique(TEXT(".uai/workspace.hjson"));
		const double CheckpointStarted = FPlatformTime::Seconds();
		const bool bPrepared = MakeRefreshCheckpoint(WorkspaceRoot, OutSourceCheckpoint, OutError);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=workspace_source_preparation load_s=%.6f paths_and_intent_s=%.6f checkpoint_prepare_s=%.6f managed_paths=%d success=%s"),
			LoadedAt - LoadStarted, CheckpointStarted - LoadedAt, FPlatformTime::Seconds() - CheckpointStarted,
			OutManagedPaths.Num(), bPrepared ? TEXT("true") : TEXT("false"));
		return bPrepared;
	}

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

	bool TryRefreshWorkspace(
		const FString& WorkspaceRoot, const TArray<FString>& ManagedSourcePaths,
		TFunctionRef<bool(const FString&, FPreparedWorkspaceExport&, FWorkspaceError&)> ExportCurrent,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& OutError,
		TArray<Contract::FDiagnostic>& OutWarnings, FHjsonOperationCheckpoint SourceCheckpoint)
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
		PreparedExport.AcceptedWorkspaceRoot = Root;
		if (!TrySelectWorkspacePublication(Root, FString(), false, PreparedExport.Publication, OutError)) return false;
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
			// Any subsequent history read starts fresh; durable receipts are untouched.
			UeAgentAuthoringWorkspaceTransaction::ReleaseDurableConsumerHistoryReads();
			ArtifactPublisher::FArtifactPublishReceipt Receipt;
			ArtifactPublisher::FArtifactPublishError PublishError;
			if (!PreparedExport.Plan.IsValid()) {
				OutError.SetSourceFailure(TEXT("workspace_incremental_export_unavailable"),
					TEXT("workspace_refresh: the exporter did not provide its prepared artifact plan; the selected exporter must produce the plan before publication. No legacy exporter was selected."), WorkspaceRoot);
			} else {
			TFunction<bool(ArtifactPublisher::FArtifactPublishError&)> BeforeCommit;
			if (SourceCheckpoint) BeforeCommit = [SourceCheckpoint = MoveTemp(SourceCheckpoint)](ArtifactPublisher::FArtifactPublishError& Error) {
				FWorkspaceError SourceError;
				if (SourceCheckpoint(TEXT("before_workspace_publish"), SourceError)) return true;
				Error = SourceError.GetDiagnostic(); return false;
			};
			bPublished = ArtifactPublisher::TryRefreshPlannedWorkspace(PreparedExport.Plan.ToSharedRef(),
				PreparedExport.PublisherOwnerId, Root, ManagedSourcePaths, Resources, Receipt, PublishError, MoveTemp(BeforeCommit), PreparedExport.ConsumerState, PreparedExport.Verification);
			if (!bPublished)
			{
				OutError.SetFailure(PublishError);
			}
			else
				OutWarnings.Append(Receipt.GetCleanupWarnings());
			}
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

	bool TryRefreshImportedWorkspace(const FString& WorkspaceRoot, const TArray<FString>& ManagedSourcePaths,
		TFunctionRef<bool(const FString&, FPreparedWorkspaceExport&, FWorkspaceError&)> ExportCurrent,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& OutError,
		TArray<Contract::FDiagnostic>& OutWarnings)
	{
		FHjsonOperationCheckpoint SourceCheckpoint;
		if (!MakeRefreshCheckpoint(WorkspaceRoot, SourceCheckpoint, OutError)) return false;
		return TryRefreshWorkspace(WorkspaceRoot, ManagedSourcePaths, ExportCurrent, Resources, OutError, OutWarnings, MoveTemp(SourceCheckpoint));
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
