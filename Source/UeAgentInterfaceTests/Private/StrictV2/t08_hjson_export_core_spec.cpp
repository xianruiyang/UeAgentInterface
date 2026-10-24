#include "Misc/AutomationTest.h"

#include "Core/ArtifactPublisher/UeAgentArtifactPublisherTestHooks.h"
#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "HAL/FileManager.h"
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
		TEXT("  snapshot_id: \"snapshot:t08:fixture\"\n")
		TEXT("  revision: { source_id: \"t08-fixture\", generation: \"g1\", sequence: 7 }\n")
		TEXT("  title: \"verified export\"\n")
		TEXT("}\n");
	static const TCHAR* AssetHash =
		TEXT("sha256:4fb13f6cd52d14c75096f9850a27356e8350c1955adda5e3bc9f588abc1488d1");
	static const TCHAR* PlanFingerprint =
		TEXT("sha256:17119ca1227d4676f1f179742cf43c677a381a6052883d1023cc4baa05d7c811");
	static const TCHAR* InventorySemantic =
		TEXT("sha256:41536f1320056b89c09fbd26aa8038876a023f49a382f2c57de33e5405389379");
	static const TCHAR* ReferenceClosureSemantic =
		TEXT("sha256:ac432558d81aade34196ea0ca5afc2547f324541ace1f3c403cbd4f6bbecf1d2");
	static const TCHAR* ContentDigestsSemantic =
		TEXT("sha256:ca6713a93916b93e01c1b5433a4545262d7ebf7185e347c076fd69bdf688fbff");
	static const TCHAR* ContractSemantic =
		TEXT("sha256:f6fa0253b0c8759bdccd17a99425bd6050947581c4df1e0f05c90206c0f8c607");
	static const TCHAR* BusinessSemantic =
		TEXT("sha256:2d649f413dd147499f95d2aefd94676c38f48b40e875bef1e56b0a9dfadccd64");

	static TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		return Bytes;
	}

	static FUeAgentResourceTracker MakeTracker()
	{
		return FUeAgentResourceTracker();
	}

	static FExportSemanticSignature ExpectedSemantic()
	{
		return FExportSemanticSignature(
			InventorySemantic,
			ReferenceClosureSemantic,
			ContentDigestsSemantic,
			ContractSemantic,
			BusinessSemantic);
	}

	static FString TestRoot(const FString& Leaf)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("Automation/StrictV2/T08HjsonExportCore"),
			Leaf));
	}

	static void ResetRoot(const FString& Root)
	{
		const FString OwnedRoot = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("Automation/StrictV2/T08HjsonExportCore")));
		const FString Normalized = FPaths::ConvertRelativePathToFull(Root);
		if (Normalized.StartsWith(OwnedRoot + TEXT("/"))
			|| Normalized.Equals(OwnedRoot, ESearchCase::IgnoreCase))
		{
			IFileManager::Get().DeleteDirectory(*Normalized, false, true);
		}
	}

	static bool HasTransactionDebris(const FString& Root)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*"), true, true);
		for (const FString& Path : Files)
		{
			if (Path.Contains(TEXT(".uai-txn-")))
			{
				return true;
			}
		}
		return false;
	}

	class FFixtureSource final : public IVerifiedExportWorkspaceSource
	{
	public:
		explicit FFixtureSource(const int64 InObservedTokens = 1)
			: ObservedTokens(InObservedTokens)
		{
		}

		virtual bool TryFreezeTarget(
			FUeAgentStateRevision& OutRevision,
			FArtifactPublishError& OutError) override
		{
			Steps.Add(TEXT("freeze"));
			OutRevision = FrozenRevision;
			return true;
		}

		virtual bool TryCaptureWorkspace(
			const FUeAgentStateRevision& Frozen,
			FExportWorkspacePlanPtr& OutPlan,
			FExportSemanticSignature& OutSemantic,
			FArtifactPublishError& OutError) override
		{
			Steps.Add(TEXT("capture"));
			if (Frozen != FrozenRevision)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("fixture_freeze_mismatch"), FString(), TEXT("artifact_publish"), FString(), FString());
				return false;
			}
			TArray<FExportFilePlan> Files;
			Files.Emplace(TEXT("asset.hjson"), Utf8(AssetText), AssetHash);
			OutPlan = MakeShared<FExportWorkspacePlan>(
				EWorkspaceKind::Directory,
				TEXT("asset.hjson"),
				TEXT("snapshot:t08:fixture"),
				FrozenRevision,
				TEXT("strict-v2:t08:real-capture-fixture"),
				MoveTemp(Files),
				PlanFingerprint,
				ObservedTokens);
			OutSemantic = ExpectedSemantic();
			return true;
		}

		virtual bool TryRebuildStagedWorkspace(
			const FString& StagedWorkspacePath,
			const EWorkspaceKind WorkspaceKind,
			const FString& EntryFile,
			FUeAgentResourceTracker& Resources,
			FExportSemanticSignature& OutSemantic,
			FArtifactPublishError& OutError) override
		{
			Steps.Add(TEXT("rebuild"));
			if (WorkspaceKind != EWorkspaceKind::Directory
				|| EntryFile != TEXT("asset.hjson")
				|| !IFileManager::Get().FileExists(
					*(StagedWorkspacePath / EntryFile)))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("fixture_staging_invalid"), FString(), TEXT("artifact_publish"), FString(), StagedWorkspacePath);
				return false;
			}
			OutSemantic = ExpectedSemantic();
			if (bReturnSemanticMismatch)
			{
				OutSemantic = FExportSemanticSignature(
					InventorySemantic,
					ReferenceClosureSemantic,
					ContentDigestsSemantic,
					ContractSemantic,
					TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
			}
			return true;
		}

		virtual bool TryReadCurrentTargetRevision(
			FUeAgentStateRevision& OutRevision,
			FArtifactPublishError& OutError) override
		{
			Steps.Add(TEXT("recheck"));
			OutRevision = bReturnChangedRevision
				? FUeAgentStateRevision(TEXT("t08-fixture"), TEXT("g1"), 8)
				: FrozenRevision;
			return true;
		}

		TArray<FString> Steps;
		bool bReturnSemanticMismatch = false;
		bool bReturnChangedRevision = false;

	private:
		FUeAgentStateRevision FrozenRevision =
			FUeAgentStateRevision(TEXT("t08-fixture"), TEXT("g1"), 7);
		int64 ObservedTokens = 1;
	};

	static bool Export(
		const FString& Final,
		const TSharedRef<FFixtureSource>& Source,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError)
	{
		FUeAgentResourceTracker Resources = MakeTracker();
		return TryExportVerifiedWorkspace(
			FVerifiedExportRequest(
				Source,
				Final,
				TEXT("strict-v2:t08:fixture-owner")),
			Resources,
			OutReceipt,
			OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T08VerifiedPipelineTest,
	"UAI.StrictV2.T08.HJSON.EXPORT.CORE.VerifiedPipeline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T08VerifiedPipelineTest::RunTest(const FString& Parameters)
{
	const FString Root = TestRoot(TEXT("verified_pipeline"));
	const FString Final = Root / TEXT("final");
	ResetRoot(Root);
	const TSharedRef<FFixtureSource> Source = MakeShared<FFixtureSource>();
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError Error;
	TestTrue(TEXT("verified export succeeds"), Export(Final, Source, Receipt, Error));
	TestEqual(
		TEXT("pipeline preserves the mandatory order"),
		FString::Join(Source->Steps, TEXT(",")),
		FString(TEXT("freeze,capture,rebuild,recheck")));
	TestTrue(TEXT("semantic rebuild is recorded"), Receipt.WasSemanticWorkspaceVerified());
	TestTrue(TEXT("target version recheck is recorded"), Receipt.WasTargetVersionRechecked());
	TestEqual(
		TEXT("business semantics are bound to the receipt"),
		Receipt.GetSemanticVerificationFields().FindRef(TEXT("business_semantic")),
		FString(BusinessSemantic));
	TestTrue(
		TEXT("complete workspace is atomically visible"),
		IFileManager::Get().FileExists(*(Final / TEXT("asset.hjson"))));
	TestFalse(TEXT("successful publish leaves no transaction debris"), HasTransactionDebris(Root));
	ResetRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T08SemanticAndVersionFailureTest,
	"UAI.StrictV2.T08.HJSON.EXPORT.CORE.SemanticAndVersionFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T08SemanticAndVersionFailureTest::RunTest(const FString& Parameters)
{
	const FString Root = TestRoot(TEXT("semantic_version_failure"));
	const FString Final = Root / TEXT("final");
	ResetRoot(Root);
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError Error;

	const TSharedRef<FFixtureSource> SemanticMismatch = MakeShared<FFixtureSource>();
	SemanticMismatch->bReturnSemanticMismatch = true;
	TestFalse(
		TEXT("semantic mismatch blocks publication"),
		Export(Final, SemanticMismatch, Receipt, Error));
	TestEqual(
		TEXT("semantic mismatch has a typed error"),
		Error.Code,
		FString(TEXT("artifact_semantic_mismatch")));
	TestFalse(TEXT("semantic mismatch publishes no final"), IFileManager::Get().DirectoryExists(*Final));
	TestFalse(TEXT("semantic mismatch cleans staging"), HasTransactionDebris(Root));

	const TSharedRef<FFixtureSource> VersionConflict = MakeShared<FFixtureSource>();
	VersionConflict->bReturnChangedRevision = true;
	TestFalse(
		TEXT("changed target version blocks publication"),
		Export(Final, VersionConflict, Receipt, Error));
	TestEqual(
		TEXT("version conflict has a typed error"),
		Error.Code,
		FString(TEXT("artifact_target_version_conflict")));
	TestFalse(TEXT("version conflict publishes no final"), IFileManager::Get().DirectoryExists(*Final));
	TestFalse(TEXT("version conflict cleans staging"), HasTransactionDebris(Root));
	ResetRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T08TokenIndependentExportTest,
	"UAI.StrictV2.T08.HJSON.EXPORT.CORE.TokenIndependentExport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T08TokenIndependentExportTest::RunTest(const FString& Parameters)
{
	const FString Root = TestRoot(TEXT("token_independence"));
	ResetRoot(Root);
	FArtifactPublishReceipt SmallReceipt;
	FArtifactPublishReceipt LargeReceipt;
	FArtifactPublishError Error;
	TestTrue(
		TEXT("small observation publishes"),
		Export(Root / TEXT("small"), MakeShared<FFixtureSource>(1), SmallReceipt, Error));
	TestTrue(
		TEXT("large observation publishes without a token gate"),
		Export(Root / TEXT("large"), MakeShared<FFixtureSource>(MAX_int64 / 4), LargeReceipt, Error));
	TestTrue(
		TEXT("token estimates cannot change exported files"),
		SmallReceipt.GetIncludedFiles() == LargeReceipt.GetIncludedFiles());
	TestTrue(
		TEXT("token estimates cannot change exported bytes"),
		SmallReceipt.GetFileHashes().OrderIndependentCompareEqual(
			LargeReceipt.GetFileHashes()));
	TestEqual(
		TEXT("token estimates cannot change semantic verification"),
		SmallReceipt.GetSemanticVerificationFields().FindRef(TEXT("business_semantic")),
		LargeReceipt.GetSemanticVerificationFields().FindRef(TEXT("business_semantic")));
	ResetRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T08AtomicRollbackTest,
	"UAI.StrictV2.T08.HJSON.EXPORT.CORE.AtomicRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T08AtomicRollbackTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::ArtifactPublisher::TestHooks;
	const FString Root = TestRoot(TEXT("atomic_rollback"));
	const FString Final = Root / TEXT("final");
	ResetRoot(Root);
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError Error;
	TestTrue(
		TEXT("old final fixture publishes"),
		Export(Final, MakeShared<FFixtureSource>(), Receipt, Error));
	TArray<uint8> Before;
	TestTrue(
		TEXT("old final is readable"),
		FFileHelper::LoadFileToArray(Before, *(Final / TEXT("asset.hjson"))));
	{
		FScopedPublishFailure Failure(EFailurePoint::AfterFinalMovedToBackup);
		TestFalse(
			TEXT("injected commit failure is surfaced"),
			Export(Final, MakeShared<FFixtureSource>(), Receipt, Error));
	}
	TArray<uint8> After;
	TestTrue(
		TEXT("restored final is readable"),
		FFileHelper::LoadFileToArray(After, *(Final / TEXT("asset.hjson"))));
	TestTrue(TEXT("failed commit restores the old final byte-for-byte"), Before == After);
	TestFalse(TEXT("rollback leaves no transaction debris"), HasTransactionDebris(Root));
	ResetRoot(Root);
	return true;
}
