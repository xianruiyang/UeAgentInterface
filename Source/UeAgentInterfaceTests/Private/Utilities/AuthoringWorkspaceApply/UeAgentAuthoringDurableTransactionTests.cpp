#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringApplyProvider.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	using namespace UeAgentAuthoringWorkspaceTransaction;

	class FDurableFixtureMutation final : public IPreparedMutation
	{
	public:
		virtual FString GetBackendId() const override { return TEXT("fixture"); }
		virtual FString GetUnitKey() const override { return TEXT("fixture:unit"); }
		virtual FString GetTargetKey() const override { return TEXT("fixture:target"); }
	};

	class FDurableFixtureBackend final : public ITransactionBackend
	{
	public:
		explicit FDurableFixtureBackend(FString InFinalPath)
			: FinalPath(MoveTemp(InFinalPath))
		{
		}

		virtual bool BeginWorkspace(
			const UeAgentAuthoringWorkspaceRouter::FWorkspace&,
			const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph&,
			FOperationFailure&) override { return true; }
		virtual bool CaptureRollback(IPreparedMutation&, FOperationFailure&) override { return true; }
		virtual bool ApplyUnit(IPreparedMutation&, FOperationFailure&) override { return true; }
		virtual bool Compile(FOperationFailure&) override { return true; }
		virtual bool Save(FOperationFailure&) override { return true; }
		virtual bool Verify(FOperationFailure&) override { return true; }
		virtual bool RollbackUnit(IPreparedMutation&, FOperationFailure&) override { return true; }
		virtual bool EndWorkspace(bool, FOperationFailure&) override { return true; }
		virtual bool SupportsDurableTransactions() const override { return true; }
		virtual bool BuildDurableArtifactInventory(
			const TArray<IPreparedMutation*>&,
			TArray<FDurableArtifact>& OutArtifacts,
			FOperationFailure&) override
		{
			FDurableArtifact& Artifact = OutArtifacts.AddDefaulted_GetRef();
			Artifact.ArtifactId = TEXT("fixture:artifact");
			Artifact.FinalPath = FinalPath;
			Artifact.Kind = Kind;
			return true;
		}
		virtual bool StageDurableArtifacts(
			TArray<FDurableArtifact>& Artifacts,
			FOperationFailure&) override {
			for (auto& Artifact : Artifacts) {
				if (Artifact.Kind == EDurableArtifactKind::DirectoryTree) { Artifact.bStagedExists = false; continue; }
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Artifact.StagingPath), true);
				Artifact.bStagedExists = FFileHelper::SaveStringToFile(*StageContent, *Artifact.StagingPath);
				if (!Artifact.bStagedExists) return false;
			}
			return true;
		}
		FString StageContent = TEXT("dirty-memory-before");
		EDurableArtifactKind Kind = EDurableArtifactKind::File;

	private:
		FString FinalPath;
	};

	class FNoOpMutation final : public IPreparedMutation {
	public:
		explicit FNoOpMutation(FString InKey) : Key(MoveTemp(InKey)) {}
		virtual FString GetUnitKey() const override { return Key; }
		virtual FString GetTargetKey() const override { return TEXT("no-op"); }
		virtual bool IsNoOp() const override { return true; }
	private:
		FString Key;
	};
	class FNoOpProvider final : public IPreflightProvider {
	public:
		virtual bool PreflightUnit(const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit& Unit,
			TUniquePtr<IPreparedMutation>& OutMutation, FOperationFailure&) const override {
			OutMutation = MakeUnique<FNoOpMutation>(Unit.CanonicalUnitKey);
			return true;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentAuthoringDurableReplayIdentityTest,
	"UeAgentInterface.Utilities.AuthoringWorkspaceApply.DurableReplayIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAuthoringDurableReplayIdentityTest::RunTest(const FString&)
{
	using namespace UeAgentAuthoringWorkspaceTransaction;
	const FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(),
		TEXT("UAI_ReplayIdentity"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const FString FinalPath = Root / TEXT("artifact.bin");
	if (!FFileHelper::SaveStringToFile(TEXT("original"), *FinalPath)) return false;
	FExecutionOptions Options;
	Options.RequestId = TEXT("replay-request-one"); Options.JournalRoot = Root / TEXT("journals");
	FDurableFixtureBackend Backend(FinalPath); Backend.StageContent = TEXT("published");
	FDurableFixtureMutation Mutation;
	FDurableTransactionCoordinator Writer;
	FDurabilityResult Result;
	FOperationFailure Failure;
	if (!TestTrue(TEXT("real durable transaction produces a completed receipt"),
		Writer.Prepare(Options, TEXT("fingerprint-one"), Backend, {&Mutation}, Result, Failure)
		&& Writer.Stage(Backend, Result, Failure) && Writer.Publish(Result, Failure)
		&& Writer.Commit(Result, Failure))) { AddError(Failure.Diagnostic.Code); return false; }
	TArray<uint8> Published;
	if (!FFileHelper::LoadFileToArray(Published, *FinalPath)) return false;
	FDurableTransactionCoordinator Reader;
	bool bReplayed = false;
	FDurabilityResult Replay;
	TestTrue(TEXT("same public identity and fingerprint replay committed result"),
		Reader.TryReplay(Options, TEXT("fingerprint-one"), bReplayed, Replay, Failure)
		&& bReplayed && Replay.bReplayed);
	bReplayed = false; Replay = FDurabilityResult();
	TestFalse(TEXT("same identity cannot apply different request content"),
		Reader.TryReplay(Options, TEXT("fingerprint-two"), bReplayed, Replay, Failure));
	TestEqual(TEXT("identity conflict remains typed"), Failure.Diagnostic.Code, FString(TEXT("durable_replay_identity_mismatch")));
	TestFalse(TEXT("conflicting content never replays"), bReplayed);
	Options.RequestId = TEXT("replay-request-two"); Replay = FDurabilityResult();
	TestTrue(TEXT("distinct public request is not mistaken for replay"),
		Reader.TryReplay(Options, TEXT("fingerprint-two"), bReplayed, Replay, Failure) && !bReplayed);
	TArray<uint8> Actual;
	TestTrue(TEXT("all replay decisions leave published artifact unchanged"),
		FFileHelper::LoadFileToArray(Actual, *FinalPath) && Actual == Published);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAuthoringDurableIdempotentRecoveryTest,
	"UeAgentInterface.Utilities.AuthoringWorkspaceApply.DurableIdempotentRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAuthoringDurableIdempotentRecoveryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UeAgentAuthoringWorkspaceTransaction;
	const FString FixtureRoot = FPaths::Combine(
		FPlatformProcess::UserTempDir(),
		TEXT("UAI_DurableIdempotentRecovery_"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString FinalPath = FPaths::Combine(FixtureRoot, TEXT("artifact.bin"));
	const FString JournalRoot = FPaths::Combine(FixtureRoot, TEXT("journals"));
	IFileManager::Get().MakeDirectory(*FixtureRoot, true);
	ON_SCOPE_EXIT
	{
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(
			*FinalPath, false);
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
	};
	if (!TestTrue(
		TEXT("the fixture original artifact is written"),
		FFileHelper::SaveStringToFile(TEXT("original"), *FinalPath)))
	{
		return false;
	}

	FExecutionOptions Options;
	Options.RequestId = TEXT("durable-idempotent-recovery-fixture");
	Options.JournalRoot = JournalRoot;
	FDurableFixtureMutation Mutation;
	TArray<IPreparedMutation*> Mutations{&Mutation};
	FDurableFixtureBackend Backend(FinalPath);
	FDurabilityResult Durability;
	FOperationFailure Failure;
	FDurableTransactionCoordinator Coordinator;
	if (!TestTrue(
		TEXT("the legacy durable transaction prepares an original backup"),
		Coordinator.Prepare(
			Options,
			TEXT("fixture-fingerprint"),
			Backend,
			Mutations,
			Durability,
			Failure)))
	{
		AddError(Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.PublicMessage);
		return false;
	}
	TestFalse(
		TEXT("an unconfirmed memory rollback leaves recovery pending"),
		Coordinator.Restore(false, Durability, Failure));
	TestEqual(
		TEXT("the pending journal records the unconfirmed restore"),
		Durability.JournalPhase,
		FString(TEXT("restore_unconfirmed")));
	TestTrue(
		TEXT("the already-restored artifact can be made non-writable"),
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(
			*FinalPath, true));

	FDurableTransactionCoordinator RecoveryCoordinator;
	FDurabilityResult RecoveryResult;
	FString JournalText;
	TSharedPtr<FJsonObject> LegacyJournal;
	if (!FFileHelper::LoadFileToString(JournalText, *Durability.JournalPath)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JournalText), LegacyJournal)) return false;
	for (const auto& Value : LegacyJournal->GetArrayField(TEXT("artifacts"))) Value->AsObject()->RemoveField(TEXT("kind"));
	JournalText.Reset();
	if (!FJsonSerializer::Serialize(LegacyJournal.ToSharedRef(), TJsonWriterFactory<>::Create(&JournalText))
		|| !FFileHelper::SaveStringToFile(JournalText, *Durability.JournalPath)) return false;
	Failure = FOperationFailure();
	TestTrue(
		TEXT("recovery accepts an exact already-restored artifact without rewriting it"),
		RecoveryCoordinator.RecoverPending(
			Options, RecoveryResult, Failure));
	if (!Failure.Diagnostic.Code.IsEmpty())
	{
		AddError(Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.PublicMessage);
	}
	TestEqual(
		TEXT("the pending transaction reaches restored recovery status"),
		RecoveryResult.RecoveryStatus,
		FString(TEXT("restored")));
	TestFalse(
		TEXT("the terminal recovery removes the active transaction directory"),
		IFileManager::Get().DirectoryExists(*FPaths::GetPath(Durability.JournalPath)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAuthoringDurableMemoryCheckpointTest,
	"UeAgentInterface.Utilities.AuthoringWorkspaceApply.DurableMemoryCheckpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAuthoringDurableMemoryCheckpointTest::RunTest(const FString& Parameters)
{
	using namespace UeAgentAuthoringWorkspaceTransaction;
	const FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(),
		TEXT("UAI_MemoryCheckpoint_"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const FString FinalPath = Root / TEXT("asset.bin");
	FFileHelper::SaveStringToFile(TEXT("disk-before"), *FinalPath);
	FExecutionOptions Options;
	Options.RequestId = TEXT("memory-checkpoint-fixture");
	Options.JournalRoot = Root / TEXT("journals");
	FDurableFixtureBackend Backend(FinalPath);
	FDurableFixtureMutation Mutation;
	FDurableTransactionCoordinator Coordinator;
	FDurabilityResult Result;
	FOperationFailure Failure;
	if (!TestTrue(TEXT("the transaction captures original disk artifacts"),
		Coordinator.Prepare(Options, TEXT("memory-fixture"), Backend, {&Mutation}, Result, Failure)) ||
		!TestTrue(TEXT("the same stager captures a different dirty-memory image"),
		Coordinator.CaptureMemoryCheckpoint(Backend, Result, Failure))) return false;
	const FString TransactionRoot = FPaths::GetPath(Result.JournalPath);
	FFileHelper::SaveStringToFile(TEXT("published-new"), *FinalPath);
	TestTrue(TEXT("memory recovery publishes the dirty before-image"),
		Coordinator.PublishMemoryCheckpoint(Result, Failure));
	FString Content;
	FFileHelper::LoadFileToString(Content, *FinalPath);
	TestEqual(TEXT("the memory checkpoint differs from original disk"), Content, FString(TEXT("dirty-memory-before")));
	TestTrue(TEXT("the disk restore preserves its separate before-image"),
		Coordinator.RestoreDiskArtifacts(Result, Failure));
	FFileHelper::LoadFileToString(Content, *FinalPath);
	TestEqual(TEXT("disk returns to its original value"), Content, FString(TEXT("disk-before")));
	TestFalse(TEXT("unconfirmed memory cannot finalize recovery"),
		Coordinator.FinalizeRestore(false, Result, Failure));
	TestEqual(TEXT("the receipt reports the unknown recovery state"),
		Result.JournalPhase, FString(TEXT("restore_unconfirmed")));
	TestFalse(TEXT("the receipt never reports successful restoration"), Result.bRestoreSucceeded);
	FString DiskBackup;
	FString MemoryBackup;
	TestTrue(TEXT("original disk backup remains available"),
		FFileHelper::LoadFileToString(DiskBackup, *(TransactionRoot / TEXT("backup/0000.bin"))) &&
		DiskBackup == TEXT("disk-before"));
	TestTrue(TEXT("dirty-memory checkpoint remains available"),
		FFileHelper::LoadFileToString(MemoryBackup, *(TransactionRoot / TEXT("memory_before/0000.bin"))) &&
		MemoryBackup == TEXT("dirty-memory-before"));
	FDurableTransactionCoordinator Recovery;
	FDurabilityResult RecoveryResult;
	TestTrue(TEXT("disk recovery retains unconfirmed memory evidence without blocking future work"),
		Recovery.RecoverPending(Options, RecoveryResult, Failure));
	TestTrue(TEXT("the unknown recovery retains the complete journal directory"),
		IFileManager::Get().DirectoryExists(*TransactionRoot));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentAuthoringDurableDirectoryJournalRecoveryTest,
	"UeAgentInterface.Utilities.AuthoringWorkspaceApply.DurableDirectoryJournalRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAuthoringDurableDirectoryJournalRecoveryTest::RunTest(const FString&)
{
	using namespace UeAgentAuthoringWorkspaceTransaction;
	const FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("UAI_DirectoryJournal"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Final = Root / TEXT("tree");
	ON_SCOPE_EXIT
	{
		if (!IFileManager::Get().DeleteDirectory(*Root, false, true) || FPaths::DirectoryExists(Root)) AddError(TEXT("directory journal fixture cleanup failed"));
	};
	if (!IFileManager::Get().MakeDirectory(*(Final / TEXT("nested/empty")), true)
		|| !IFileManager::Get().MakeDirectory(*(Final / TEXT("empty")), true)) return false;
	const TArray<uint8> Bytes{0, 255, 1, 128, 0};
	if (!FFileHelper::SaveArrayToFile(Bytes, *(Final / TEXT("nested/data.bin")))) return false;
	FExecutionOptions Options;
	Options.RequestId = TEXT("directory-journal-recovery");
	Options.JournalRoot = Root / TEXT("journals");
	FDurableFixtureBackend Backend(Final);
	Backend.Kind = EDurableArtifactKind::DirectoryTree;
	FDurableFixtureMutation Mutation;
	FDurabilityResult Published;
	FOperationFailure Failure;
	{
		FDurableTransactionCoordinator Original;
		if (!Original.Prepare(Options, TEXT("directory-journal"), Backend, {&Mutation}, Published, Failure)
			|| !Original.Stage(Backend, Published, Failure) || !Original.Publish(Published, Failure))
		{ AddError(Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.PublicMessage); return false; }
	}
	if (!TestFalse(TEXT("published directory is absent before separate recovery"), FPaths::DirectoryExists(Final))) return false;
	FString JournalText;
	TSharedPtr<FJsonObject> Journal;
	if (!FFileHelper::LoadFileToString(JournalText, *Published.JournalPath)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JournalText), Journal)) return false;
	const auto Artifact = Journal->GetArrayField(TEXT("artifacts"))[0]->AsObject();
	if (!TestEqual(TEXT("disk journal declares directory kind"), Artifact->GetStringField(TEXT("kind")), FString(TEXT("directory_tree")))) return false;
	Artifact->SetStringField(TEXT("kind"), TEXT("unsupported_kind"));
	FString InvalidJournal;
	if (!FJsonSerializer::Serialize(Journal.ToSharedRef(), TJsonWriterFactory<>::Create(&InvalidJournal))
		|| !FFileHelper::SaveStringToFile(InvalidJournal, *Published.JournalPath)) return false;
	FDurableTransactionCoordinator InvalidRecovery;
	FDurabilityResult Recovered;
	TestFalse(TEXT("unknown journal artifact kind is rejected"), InvalidRecovery.RecoverPending(Options, Recovered, Failure));
	TestEqual(TEXT("unknown kind reports invalid journal"), Failure.Diagnostic.Code, FString(TEXT("durable_recovery_journal_invalid")));
	TestFalse(TEXT("invalid journal cannot mutate final directory"), FPaths::DirectoryExists(Final));
	if (!FFileHelper::SaveStringToFile(JournalText, *Published.JournalPath)) return false;
	FDurableTransactionCoordinator Recovery;
	if (!TestTrue(TEXT("new coordinator recovers from serialized directory journal"), Recovery.RecoverPending(Options, Recovered, Failure)))
	{ AddError(Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.PublicMessage); return false; }
	TArray<uint8> Restored;
	TestTrue(TEXT("journal recovery restores binary bytes"), FFileHelper::LoadFileToArray(Restored, *(Final / TEXT("nested/data.bin"))) && Restored == Bytes);
	TestTrue(TEXT("journal recovery restores empty directories"), FPaths::DirectoryExists(Final / TEXT("empty")) && FPaths::DirectoryExists(Final / TEXT("nested/empty")));
	TArray<FString> Files, Directories;
	IFileManager::Get().FindFilesRecursive(Files, *Final, TEXT("*"), true, false);
	IFileManager::Get().FindFilesRecursive(Directories, *Final, TEXT("*"), false, true);
	TestTrue(TEXT("journal recovery restores exact tree membership"), Files.Num() == 1 && Directories.Num() == 3);
	TestFalse(TEXT("successful recovery removes active journal tree"), FPaths::DirectoryExists(FPaths::GetPath(Published.JournalPath)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAuthoringNoOpFinalReadbackTest,
	"UeAgentInterface.Utilities.AuthoringWorkspaceApply.NoOpFinalReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAuthoringNoOpFinalReadbackTest::RunTest(const FString& Parameters)
{
	using namespace UeAgentAuthoringWorkspaceTransaction;
	FString Error;
	if (!UeAgentAudioAuthoring::RegisterApplyContracts(Error)) { AddError(Error); return false; }
	const auto* Profile = UeAgentAudioAuthoring::FindProfile(UeAgentAudioAuthoring::EAssetKind::MetaSound);
	UeAgentApplyWorkspaceContract::FContract Contract;
	if (!Profile || !UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(Profile->WorkspaceContractId, Contract))
		return false;
	const FString Document =
		TEXT("operation: update\n")
		TEXT("target: { asset_path: \"/Game/UAI/Transient/NoOpFixture\" }\n");
	UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
	const bool bLoaded = UeAgentAuthoringWorkspaceLoader::LoadWorkspace(Contract,
		UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineHjson(
			Document, TEXT("inline://noop")), Load);
	if (!TestTrue(TEXT("a no-op fixture uses the formal workspace loader"), bLoaded))
	{
		AddError(FString::Printf(TEXT("workspace_loader_error:%s:%s"),
			*Load.ErrorCode, *Load.ErrorDetail));
		return false;
	}
	UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
	UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
	if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(Contract, Load.Workspace,
		UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(), Graph, GraphFailure)) return false;
	for (const bool bAccept : {true, false}) {
		FPreflightResult Prepared;
		FNoOpProvider Provider;
		if (!PreflightWorkspace(Contract, Load.Workspace, Graph, Provider, Prepared)) return false;
		bool bReadbackCalled = false;
		UeAgentAuthoringOperationApply::FBackendOptions BackendOptions;
		BackendOptions.bUseEditorTransaction = false;
		BackendOptions.FinalReadback = [&](FOperationFailure& Failure) {
			bReadbackCalled = true;
			if (!bAccept) Failure.Diagnostic.Code = TEXT("fixture_noop_readback_rejected");
			return bAccept;
		};
		UeAgentAuthoringOperationApply::FOperationTransactionBackend Backend(BackendOptions);
		FExecutionOptions Options;
		Options.bSaveWhenCallerControlled = true;
		FExecutionResult Result;
		TestEqual(TEXT("no-op final acceptance determines success without durable context"),
			ExecuteAtomic(Contract, Load.Workspace, Graph, Prepared, Backend, Options, Result), bAccept);
		TestTrue(TEXT("no-op performs the requested final readback"), bReadbackCalled);
		TestEqual(TEXT("no-op never mutates"), Result.MutationAttempts, 0);
		TestFalse(TEXT("no-op never releases native state or requires a checkpoint"), Backend.HasReleasedNativeState());
		TestFalse(TEXT("no-op does not create a durable journal"), Result.Durability.IsPresent());
		if (!bAccept) TestTrue(TEXT("no-op rejection confirms unchanged state instead of unknown"),
			Result.bRollbackSucceeded && Result.Status != TEXT("state_unknown"));
	}
	return !HasAnyErrors();
}
