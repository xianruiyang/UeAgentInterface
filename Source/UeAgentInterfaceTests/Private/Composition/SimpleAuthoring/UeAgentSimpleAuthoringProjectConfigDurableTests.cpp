#include "Misc/AutomationTest.h"

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"



namespace
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeTracker()
	{
		return FUeAgentResourceTracker(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1048576},
				{EUeAgentResourceDimension::OutputBytes, 1048576},
				{EUeAgentResourceDimension::StringBytes, 1048576},
				{EUeAgentResourceDimension::Nodes, 8192},
				{EUeAgentResourceDimension::Items, 8192},
				{EUeAgentResourceDimension::Files, 256},
				{EUeAgentResourceDimension::ReferenceEdges, 8192},
				{EUeAgentResourceDimension::WorkUnits, 32768},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					4194304
				},
			}));
	}

	FUeAgentCanonicalIdentity MakeIdentity()
	{
		return FUeAgentCanonicalIdentity(
			TEXT("sa008.project_config.value"),
			TEXT("project_config"),
			TEXT("GptProjectTest"),
			TEXT("/Script/UeAgentInterfaceTests.SA008"),
			TEXT("config_section"),
			TEXT("revision-1"));
	}

	FUeAgentCanonicalValueRef MakeString(
		const FString& Value,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Result;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Value,
			Resources,
			Result,
			Error));
		return Result.ToSharedRef();
	}

	FPlanningPreflightPlanRef MakePlan(
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentStateRevision Revision(
			TEXT("sa008-project-config"),
			TEXT("editor-session"),
			1);
		TArray<FUeAgentStateEntry> Entries;
		Entries.Emplace(
			MakeIdentity(),
			FUeAgentCanonicalType(
				TEXT("core.string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			MakeString(TEXT("before"), Resources),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("sa008.project_config.default"),
				TEXT("SA-008 config fixture"),
				MakeString(FString(), Resources)),
			FUeAgentStateDetails(
				TEXT("SA008"),
				TEXT("owned project config fixture"),
				{TEXT("SA-008")},
				true,
				TEXT("available"),
				{}),
			Revision);
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError StateError;
		check(FUeAgentCanonicalSnapshot::TryCreate(
			TEXT("sa008-project-config-pre-state"),
			EUeAgentStatePlane::AuthoringMemory,
			Revision,
			FUeAgentTemporalPoint(
				TEXT("sa008-test-clock"),
				TEXT("1")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("sa008-project-config-fixture"),
				TEXT("pre-state")),
			MoveTemp(Entries),
			Resources,
			Snapshot,
			StateError));
		TArray<FChangeDraft> Changes;
		Changes.Emplace(
			MakeIdentity(),
			EChangeIntent::Update,
			MakeString(TEXT("before"), Resources),
			MakeString(TEXT("after"), Resources),
			TArray<FString>{TEXT("uai:project_config:write")},
			TEXT("editor_project:config_section"),
			TArray<FUeAgentCanonicalIdentity>{});
		const FPlanningRequest Request(
			FChangeSetDraft(
				FUeAgentDescriptorIdentity(
					TEXT("config.section.apply"),
					1,
					TEXT(
						"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")),
				TEXT("sa008-project-config-request"),
				TEXT("sa008-project-config-idempotency"),
				Snapshot.ToSharedRef(),
				MoveTemp(Changes)),
			FUeAgentCallerContext(
				TEXT("agent:sa008"),
				{TEXT("uai:project_config:write")},
				TEXT("sa008-project-config-correlation")),
			false);
		FPlanningResult Result;
		FPlanningPreflightError Error;
		check(TryPlanChanges(
			Request,
			Resources,
			Result,
			Error));
		return Result.GetPlan().ToSharedRef();
	}

	class FRecordingProjectConfigStageSource final
		: public IProjectConfigDurableStageSource
	{
	public:
		bool TryWriteStagedArtifact(
			const FAssetDurableTarget& Target,
			const FString& StagedPath,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			++WriteCount;
			LastKind = Target.GetArtifactKind();
			LastLocation = Target.GetDurableLocation();
			if (bFailWrite)
			{
				OutError = FAssetDurableError(
					TEXT("sa008_config_stage_write_failed"),
					StagedPath);
				return false;
			}
			if (!FFileHelper::SaveStringToFile(
				TEXT("[SA008]\nValue=after\n"),
				*StagedPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = FAssetDurableError(
					TEXT("sa008_config_stage_write_failed"),
					StagedPath);
				return false;
			}
			return true;
		}

		int32 WriteCount = 0;
		EDurableArtifactKind LastKind =
			EDurableArtifactKind::Package;
		FString LastLocation;
		bool bFailWrite = false;
	};

	struct FOwnedProjectConfigFixture
	{
		FOwnedProjectConfigFixture()
		{
			Root = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectIntermediateDir()
					/ TEXT("UAI/SA008/ProjectConfig")
					/ FGuid::NewGuid().ToString(
						EGuidFormats::Digits));
			ConfigRoot = Root / TEXT("Config");
			JournalRoot = Root / TEXT("Journal");
			ConfigPath = ConfigRoot / TEXT("SA008.ini");
			IFileManager::Get().MakeDirectory(
				*ConfigRoot,
				true);
			check(FFileHelper::SaveStringToFile(
				TEXT("[SA008]\nValue=before\n"),
				*ConfigPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		}

		~FOwnedProjectConfigFixture()
		{
			IFileManager::Get().DeleteDirectory(
				*Root,
				false,
				true);
		}

		FString Root;
		FString ConfigRoot;
		FString JournalRoot;
		FString ConfigPath;
	};
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringProjectConfigDurableBackendTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ProjectConfigDurableBackend",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringProjectConfigDurableBackendTest::RunTest(
	const FString& Parameters)
{
	FOwnedProjectConfigFixture Fixture;
	TSharedRef<
		FRecordingProjectConfigStageSource,
		ESPMode::ThreadSafe> StageSource =
			MakeShared<
				FRecordingProjectConfigStageSource,
				ESPMode::ThreadSafe>();
	FProjectConfigAssetDurableBackend Backend(
		{Fixture.ConfigRoot},
		StageSource);
	FUeAgentResourceTracker Resources = MakeTracker();
	const FAssetDurableRequest Request(
		MakePlan(Resources),
		{
			FAssetDurableTarget(
				MakeIdentity(),
				EDurableArtifactKind::ProjectConfig,
				Fixture.ConfigPath,
				EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup)
		},
		7,
		TEXT("sa008-project-config-owner"),
		EAtomicityMode::CompensatedSaga);
	FAssetDurableSessionPtr Session;
	FAssetDurableError Error;
	TestTrue(
		TEXT("ProjectConfig target prepares through C8D"),
		TryPrepareAssetDurability(
			Request,
			Backend,
			Fixture.JournalRoot,
			Resources,
			Session,
			Error));
	if (!Session.IsValid())
	{
		AddError(
			TEXT("sa008_project_config_session_missing:")
				+ Error.GetCode());
		return false;
	}
	TestEqual(
		TEXT("prepare does not serialize or publish config"),
		StageSource->WriteCount,
		0);
	TestEqual(
		TEXT("one typed config artifact is inventoried"),
		Session->GetInventory().GetArtifacts().Num(),
		1);
	const FDurableArtifact& Artifact =
		Session->GetInventory().GetArtifacts()[0];
	TestEqual(
		TEXT("artifact kind remains ProjectConfig"),
		Artifact.GetArtifactKind(),
		EDurableArtifactKind::ProjectConfig);
	TestEqual(
		TEXT("artifact durable location remains the config path"),
		Artifact.GetDurableLocation(),
		Fixture.ConfigPath);
	TestTrue(
		TEXT("config artifact is not disguised as a package"),
		Artifact.GetPackageName().IsEmpty());

	FDurableReceipt Receipt;
	TestTrue(
		TEXT("C8D stages and publishes the config document"),
		TryPersistAssetDurability(
			Session.ToSharedRef(),
			Backend,
			7,
			Resources,
			Receipt,
			Error));
	TestEqual(
		TEXT("config serialization happens exactly once in staging"),
		StageSource->WriteCount,
		1);
	TestEqual(
		TEXT("stage source receives typed ProjectConfig target"),
		StageSource->LastKind,
		EDurableArtifactKind::ProjectConfig);
	TestEqual(
		TEXT("stage source receives the frozen durable location"),
		StageSource->LastLocation,
		Fixture.ConfigPath);
	TestEqual(
		TEXT("publish remains unverified"),
		Receipt.GetStatus(),
		EDurableStatus::PersistedUnverified);
	TestTrue(
		TEXT("publish requires independent C9 verification"),
		Receipt.RequiresIndependentVerification());
	FString Persisted;
	TestTrue(
		TEXT("published config reads from disk"),
		FFileHelper::LoadFileToString(
			Persisted,
			*Fixture.ConfigPath));
	TestTrue(
		TEXT("published config contains staged state"),
		Persisted.Contains(TEXT("Value=after")));

	TArray<FDurableReceipt> RecoveryReceipts;
	TestTrue(
		TEXT("typed ProjectConfig journal can be independently recovered"),
		TryRecoverAssetDurability(
			Fixture.JournalRoot,
			Backend,
			7,
			TEXT("sa008-project-config-owner"),
			Resources,
			RecoveryReceipts,
			Error));
	TestEqual(
		TEXT("one persisted ProjectConfig journal is recovered"),
		RecoveryReceipts.Num(),
		1);
	if (RecoveryReceipts.Num() == 1)
	{
		TestEqual(
			TEXT("recovery requires C9 instead of claiming complete"),
			RecoveryReceipts[0].GetStatus(),
			EDurableStatus::RecoveryRequired);
		TestEqual(
			TEXT("recovery action is independent verification"),
			RecoveryReceipts[0].GetRecoveryAction(),
			ERecoveryAction::VerifyPersisted);
		TestTrue(
			TEXT("recovered receipt still requires independent C9"),
			RecoveryReceipts[0]
				.RequiresIndependentVerification());
	}

	FOwnedProjectConfigFixture EscapeFixture;
	TSharedRef<
		FRecordingProjectConfigStageSource,
		ESPMode::ThreadSafe> EscapeStageSource =
			MakeShared<
				FRecordingProjectConfigStageSource,
				ESPMode::ThreadSafe>();
	FProjectConfigAssetDurableBackend EscapeBackend(
		{EscapeFixture.ConfigRoot},
		EscapeStageSource);
	FUeAgentResourceTracker EscapeResources = MakeTracker();
	const FString EscapedPath =
		EscapeFixture.Root / TEXT("outside.ini");
	const FAssetDurableRequest EscapeRequest(
		MakePlan(EscapeResources),
		{
			FAssetDurableTarget(
				MakeIdentity(),
				EDurableArtifactKind::ProjectConfig,
				EscapedPath,
				EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup)
		},
		8,
		TEXT("sa008-project-config-escape-owner"),
		EAtomicityMode::CompensatedSaga);
	FAssetDurableSessionPtr EscapeSession;
	FAssetDurableError EscapeError;
	TestFalse(
		TEXT("ProjectConfig target cannot escape its explicit root"),
		TryPrepareAssetDurability(
			EscapeRequest,
			EscapeBackend,
			EscapeFixture.JournalRoot,
			EscapeResources,
			EscapeSession,
			EscapeError));
	TestEqual(
		TEXT("path escape has a stable typed error"),
		EscapeError.GetCode(),
		FString(TEXT("durable_project_config_path_invalid")));
	TestEqual(
		TEXT("path escape performs no staging"),
		EscapeStageSource->WriteCount,
		0);

	const FDurableArtifact PackageHashArtifact(
		TEXT("sa008:hash-kind"),
		MakeIdentity(),
		EDurableArtifactKind::Package,
		TEXT("/Game/UAI/SA008/HashKind"),
		Fixture.ConfigPath,
		EAssetDurableIntent::Save,
		TEXT("same_role"));
	const FDurableArtifact ConfigHashArtifact(
		TEXT("sa008:hash-kind"),
		MakeIdentity(),
		EDurableArtifactKind::ProjectConfig,
		TEXT("/Game/UAI/SA008/HashKind"),
		Fixture.ConfigPath,
		EAssetDurableIntent::Save,
		TEXT("same_role"));
	FString PackageHash;
	FString ConfigHash;
	FAssetDurableError HashError;
	TestTrue(
		TEXT("package artifact inventory hash computes"),
		TryComputeDurableInventoryHash(
			{PackageHashArtifact},
			Resources,
			PackageHash,
			HashError));
	TestTrue(
		TEXT("config artifact inventory hash computes"),
		TryComputeDurableInventoryHash(
			{ConfigHashArtifact},
			Resources,
			ConfigHash,
			HashError));
	TestNotEqual(
		TEXT("inventory hash binds typed artifact kind"),
		PackageHash,
		ConfigHash);
	return true;
}
