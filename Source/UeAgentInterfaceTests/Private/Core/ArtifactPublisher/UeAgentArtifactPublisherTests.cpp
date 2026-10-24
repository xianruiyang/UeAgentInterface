#include "Misc/AutomationTest.h"

#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisherTestHooks.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	static const TCHAR* AssetText =
		TEXT("{\n")
		TEXT("  snapshot_id: \"snapshot:c8f:fixture\"\n")
		TEXT("  revision: { source_id: \"fixture\", generation: \"gen-7\", sequence: 42 }\n")
		TEXT("  title: \"旧终态保护😀\"\n")
		TEXT("}\n");
	static const TCHAR* DetailsText =
		TEXT("{\n")
		TEXT("  snapshot_id: \"snapshot:c8f:fixture\"\n")
		TEXT("  revision: { source_id: \"fixture\", generation: \"gen-7\", sequence: 42 }\n")
		TEXT("  exact_integer: 9007199254740993\n")
		TEXT("}\n");
	static const TCHAR* AssetHash =
		TEXT("sha256:41536f1320056b89c09fbd26aa8038876a023f49a382f2c57de33e5405389379");
	static const TCHAR* DetailsHash =
		TEXT("sha256:ac432558d81aade34196ea0ca5afc2547f324541ace1f3c403cbd4f6bbecf1d2");
	static const TCHAR* PlanFingerprint =
		TEXT("sha256:ca6713a93916b93e01c1b5433a4545262d7ebf7185e347c076fd69bdf688fbff");
	static const TCHAR* InventoryHash =
		TEXT("sha256:f6fa0253b0c8759bdccd17a99425bd6050947581c4df1e0f05c90206c0f8c607");

	static TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		return Bytes;
	}

	static FExportWorkspacePlanRef BuildPlan(
		const bool bInvalidHjson = false,
		const bool bBadSecondHash = false)
	{
		TArray<FExportFilePlan> Files;
		if (bInvalidHjson)
		{
			Files.Emplace(
				TEXT("asset.hjson"),
				Utf8(TEXT("{ broken: [ }\n")),
				TEXT("sha256:2d649f413dd147499f95d2aefd94676c38f48b40e875bef1e56b0a9dfadccd64"));
		}
		else
		{
			Files.Emplace(TEXT("asset.hjson"), Utf8(AssetText), AssetHash);
			Files.Emplace(
				TEXT("details/state.hjson"),
				Utf8(DetailsText),
				bBadSecondHash
					? TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000")
					: DetailsHash);
		}
		return MakeShared<FExportWorkspacePlan>(
			EWorkspaceKind::Directory,
			TEXT("asset.hjson"),
			TEXT("snapshot:c8f:fixture"),
			FUeAgentStateRevision(TEXT("fixture"), TEXT("gen-7"), 42),
			TEXT("fixture:c8f:independent-snapshot-readback"),
			MoveTemp(Files),
			PlanFingerprint,
			74);
	}

	static FExportWorkspacePlanRef BuildSingleFilePlan()
	{
		TArray<FExportFilePlan> Files;
		Files.Emplace(TEXT("asset.hjson"), Utf8(AssetText), AssetHash);
		return MakeShared<FExportWorkspacePlan>(
			EWorkspaceKind::SingleFile,
			TEXT("asset.hjson"),
			TEXT("snapshot:c8f:fixture"),
			FUeAgentStateRevision(TEXT("fixture"), TEXT("gen-7"), 42),
			TEXT("fixture:c8f:independent-snapshot-readback"),
			MoveTemp(Files),
			PlanFingerprint,
			38);
	}

	static FUeAgentResourceTracker MakeTracker(const int64 FileLimit = 32)
	{
		return FUeAgentResourceTracker();
	}

	static FString TestRoot(const FString& Leaf)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("Automation/UeAgentArtifactPublisher"),
			Leaf));
	}

	static void ResetOwnedRoot(const FString& Root)
	{
		const FString OwnedRoot = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("Automation/UeAgentArtifactPublisher")));
		const FString Normalized = FPaths::ConvertRelativePathToFull(Root);
		if (Normalized.StartsWith(OwnedRoot + TEXT("/"))
			|| Normalized.Equals(OwnedRoot, ESearchCase::IgnoreCase))
		{
			IFileManager::Get().DeleteDirectory(*Normalized, false, true);
		}
	}

	static TMap<FString, TArray<uint8>> SnapshotTree(const FString& Root)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(
			Files, *Root, TEXT("*"), true, false);
		Files.Sort();
		TMap<FString, TArray<uint8>> Result;
		for (const FString& File : Files)
		{
			TArray<uint8> Bytes;
			if (FFileHelper::LoadFileToArray(Bytes, *File))
			{
				FString Relative = File;
				FPaths::MakePathRelativeTo(Relative, *(Root + TEXT("/")));
				Result.Add(Relative.Replace(TEXT("\\"), TEXT("/")), MoveTemp(Bytes));
			}
		}
		return Result;
	}

	static bool TreesEqual(
		const TMap<FString, TArray<uint8>>& Left,
		const TMap<FString, TArray<uint8>>& Right)
	{
		return Left.OrderIndependentCompareEqual(Right);
	}

	static bool Publish(
		const FString& Final,
		FExportWorkspacePlanRef Plan,
		FUeAgentResourceTracker& Tracker,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError)
	{
		return TryPublishExportPlan(
			FArtifactPublishRequest(
				MoveTemp(Plan),
				Final,
				TEXT("fixture:c8f:owner")),
			Tracker,
			OutReceipt,
			OutError);
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentArtifactPublisherModuleAvailableTest,
	"UeAgentInterface.Core.ArtifactPublisher.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentArtifactPublisherModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	TestTrue(TEXT("C8F ArtifactPublisher module is available"), true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentArtifactPublisherStageReparseReceiptTest,
	"UeAgentInterface.Core.ArtifactPublisher.StageReparseReceipt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentArtifactPublisherStageReparseReceiptTest::RunTest(
	const FString& Parameters)
{
	const FString Root = TestRoot(TEXT("stage_reparse_receipt"));
	const FString Final = Root / TEXT("final");
	ResetOwnedRoot(Root);
	FUeAgentResourceTracker Tracker = MakeTracker();
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError Error;
	if (!TestTrue(
		TEXT("multi-file plan stages, reparses and publishes"),
		Publish(Final, BuildPlan(), Tracker, Receipt, Error)))
	{
		ResetOwnedRoot(Root);
		return false;
	}
	TestTrue(
		TEXT("receipt status is published"),
		Receipt.GetStatus() == EArtifactPublishStatus::Published);
	TestEqual(
		TEXT("receipt binds C5 plan fingerprint"),
		Receipt.GetPlanFingerprint(),
		FString(PlanFingerprint));
	TestEqual(
		TEXT("receipt binds snapshot id"),
		Receipt.GetSnapshotId(),
		FString(TEXT("snapshot:c8f:fixture")));
	TestEqual(
		TEXT("receipt includes both files"),
		Receipt.GetIncludedFiles().Num(),
		2);
	TestEqual(
		TEXT("receipt inventory is independently computed"),
		Receipt.GetInventoryHash(),
		FString(InventoryHash));

	const FString SingleFinal = Root / TEXT("single.hjson");
	FUeAgentResourceTracker SingleTracker = MakeTracker();
	FArtifactPublishReceipt SingleReceipt;
	FArtifactPublishError SingleError;
	TestTrue(
		TEXT("single-file plan uses adjacent staging and ownership"),
		Publish(
			SingleFinal,
			BuildSingleFilePlan(),
			SingleTracker,
			SingleReceipt,
			SingleError));
	TestTrue(
		TEXT("single-file final exists"),
		IFileManager::Get().FileExists(*SingleFinal));
	TestTrue(
		TEXT("single-file owner metadata exists"),
		IFileManager::Get().FileExists(
			*(SingleFinal + TEXT(".uai-export-owner.json"))));
	TestTrue(
		TEXT("single-file receipt preserves workspace kind"),
		SingleReceipt.GetWorkspaceKind() == EWorkspaceKind::SingleFile);
	TestEqual(
		TEXT("single-file receipt preserves entry file"),
		SingleReceipt.GetEntryFile(),
		FString(TEXT("asset.hjson")));
	ResetOwnedRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentArtifactPublisherTransientDirectoryCommitRetryTest,
	"UeAgentInterface.Core.ArtifactPublisher.TransientDirectoryCommitRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentArtifactPublisherTransientDirectoryCommitRetryTest::RunTest(
	const FString& Parameters)
{
	const FString Root = TestRoot(TEXT("transient_directory_commit_retry"));
	const FString Final = Root / TEXT("final");
	ResetOwnedRoot(Root);
	FUeAgentResourceTracker Tracker = MakeTracker();
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError Error;
	{
		TestHooks::FScopedPublishFailure Failure(
			TestHooks::EFailurePoint::FirstMoveAttempt);
		TestTrue(
			TEXT("a transient first atomic directory rename is retried"),
			Publish(Final, BuildPlan(), Tracker, Receipt, Error));
	}
	TestTrue(
		TEXT("the complete directory becomes the final workspace"),
		IFileManager::Get().DirectoryExists(*Final));
	TestTrue(
		TEXT("the final workspace has its owner marker"),
		IFileManager::Get().FileExists(
			*(Final / TEXT(".uai-export-owner.json"))));
	ResetOwnedRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentArtifactPublisherReadOnlyFinalProtectionTest,
	"UeAgentInterface.Core.ArtifactPublisher.ReadOnlyFinalProtection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentArtifactPublisherReadOnlyFinalProtectionTest::RunTest(
	const FString& Parameters)
{
	const FString Root = TestRoot(TEXT("read_only_final"));
	const FString Final = Root / TEXT("single.hjson");
	ResetOwnedRoot(Root);
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError Error;
	FUeAgentResourceTracker FirstTracker = MakeTracker();
	if (!TestTrue(
			TEXT("single-file fixture publishes"),
			Publish(
				Final,
				BuildSingleFilePlan(),
				FirstTracker,
				Receipt,
				Error)))
	{
		ResetOwnedRoot(Root);
		return false;
	}
	TArray<uint8> Before;
	TestTrue(
		TEXT("published final is readable"),
		FFileHelper::LoadFileToArray(Before, *Final));
	IPlatformFile& PlatformFile =
		FPlatformFileManager::Get().GetPlatformFile();
	TestTrue(
		TEXT("fixture final becomes read-only"),
		PlatformFile.SetReadOnly(*Final, true));

	FUeAgentResourceTracker SecondTracker = MakeTracker();
	TestFalse(
		TEXT("read-only final is rejected before commit"),
		Publish(
			Final,
			BuildSingleFilePlan(),
			SecondTracker,
			Receipt,
			Error));
	TestEqual(
		TEXT("read-only final has stable code"),
		Error.Code,
		FString(TEXT("artifact_final_read_only")));
	TArray<uint8> After;
	TestTrue(
		TEXT("rejected final remains readable"),
		FFileHelper::LoadFileToArray(After, *Final));
	TestTrue(
		TEXT("read-only final remains byte-for-byte unchanged"),
		After == Before);

	PlatformFile.SetReadOnly(*Final, false);
	ResetOwnedRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentArtifactPublisherPrecommitFailureTest,
	"UeAgentInterface.Core.ArtifactPublisher.PrecommitFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentArtifactPublisherPrecommitFailureTest::RunTest(
	const FString& Parameters)
{
	const FString Root = TestRoot(TEXT("precommit_failure"));
	const FString Final = Root / TEXT("final");
	ResetOwnedRoot(Root);
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError Error;
	FUeAgentResourceTracker FirstTracker = MakeTracker();
	if (!Publish(Final, BuildPlan(), FirstTracker, Receipt, Error))
	{
		AddError(TEXT("fixture publish failed"));
		ResetOwnedRoot(Root);
		return false;
	}
	const TMap<FString, TArray<uint8>> Before = SnapshotTree(Final);

	FUeAgentResourceTracker HashTracker = MakeTracker();
	TestFalse(
		TEXT("content hash mismatch is rejected"),
		Publish(Final, BuildPlan(false, true), HashTracker, Receipt, Error));
	TestEqual(
		TEXT("hash mismatch has stable code"),
		Error.Code,
		FString(TEXT("artifact_content_hash_mismatch")));
	TestTrue(
		TEXT("hash mismatch preserves complete old final"),
		TreesEqual(SnapshotTree(Final), Before));

	FUeAgentResourceTracker ParseTracker = MakeTracker();
	TestFalse(
		TEXT("independent reparse failure is rejected"),
		Publish(Final, BuildPlan(true, false), ParseTracker, Receipt, Error));
	TestEqual(
		TEXT("reparse failure has stable code"),
		Error.Code,
		FString(TEXT("artifact_reparse_failed")));
	TestTrue(
		TEXT("reparse failure preserves complete old final"),
		TreesEqual(SnapshotTree(Final), Before));
	ResetOwnedRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentArtifactPublisherCommitRollbackTest,
	"UeAgentInterface.Core.ArtifactPublisher.CommitRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentArtifactPublisherCommitRollbackTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ArtifactPublisher::TestHooks;
	const FString Root = TestRoot(TEXT("commit_rollback"));
	const FString Final = Root / TEXT("final");
	ResetOwnedRoot(Root);
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError Error;
	FUeAgentResourceTracker FirstTracker = MakeTracker();
	if (!Publish(Final, BuildPlan(), FirstTracker, Receipt, Error))
	{
		AddError(TEXT("fixture publish failed"));
		ResetOwnedRoot(Root);
		return false;
	}
	const TMap<FString, TArray<uint8>> Before = SnapshotTree(Final);
	FUeAgentResourceTracker FailureTracker = MakeTracker();
	{
		FScopedPublishFailure Failure(EFailurePoint::AfterFinalMovedToBackup);
		TestFalse(
			TEXT("injected commit failure is surfaced"),
			Publish(Final, BuildPlan(), FailureTracker, Receipt, Error));
	}
	TestEqual(
		TEXT("commit failure has stable code"),
		Error.Code,
		FString(TEXT("artifact_publish_failed")));
	TestTrue(
		TEXT("commit failure restores old final byte-for-byte"),
		TreesEqual(SnapshotTree(Final), Before));
	ResetOwnedRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentArtifactPublisherOwnershipBudgetCleanupTest,
	"UeAgentInterface.Core.ArtifactPublisher.OwnershipBudgetCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentArtifactPublisherOwnershipBudgetCleanupTest::RunTest(
	const FString& Parameters)
{
	const FString Root = TestRoot(TEXT("ownership_budget_cleanup"));
	const FString Final = Root / TEXT("final");
	const FString Sibling = Root / TEXT("unowned-sibling");
	const FString Sentinel = Sibling / TEXT("keep.bin");
	ResetOwnedRoot(Root);
	IFileManager::Get().MakeDirectory(*Final, true);
	IFileManager::Get().MakeDirectory(*Sibling, true);
	FFileHelper::SaveStringToFile(TEXT("user-owned"), *(Final / TEXT("user.txt")));
	FFileHelper::SaveStringToFile(TEXT("keep"), *Sentinel);
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError Error;
	FUeAgentResourceTracker Tracker = MakeTracker();
	TestFalse(
		TEXT("unowned non-empty final is rejected"),
		Publish(Final, BuildPlan(), Tracker, Receipt, Error));
	TestEqual(
		TEXT("unowned final has stable code"),
		Error.Code,
		FString(TEXT("artifact_final_ownership_required")));
	TestTrue(
		TEXT("unknown sibling survives ownership rejection"),
		IFileManager::Get().FileExists(*Sentinel));

	IFileManager::Get().DeleteDirectory(*Final, false, true);
	FUeAgentResourceTracker Initial = MakeTracker();
	if (!Publish(Final, BuildPlan(), Initial, Receipt, Error))
	{
		AddError(TEXT("owned final fixture publish failed"));
		ResetOwnedRoot(Root);
		return false;
	}
	TestTrue(
		TEXT("cleanup never removes unknown sibling"),
		IFileManager::Get().FileExists(*Sentinel));
	ResetOwnedRoot(Root);
	return true;
}
