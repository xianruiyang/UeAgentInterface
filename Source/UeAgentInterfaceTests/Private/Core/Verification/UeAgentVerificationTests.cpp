#include "Misc/AutomationTest.h"

#include <type_traits>

#include "Core/Verification/UeAgentVerification.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"


namespace
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	static FUeAgentResourceTracker MakeTracker(
		const int64 WorkLimit = 262144)
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 1048576},
			{EUeAgentResourceDimension::OutputBytes, 1048576},
			{EUeAgentResourceDimension::StringBytes, 1048576},
			{EUeAgentResourceDimension::Nodes, 65536},
			{EUeAgentResourceDimension::Items, 65536},
			{EUeAgentResourceDimension::Files, 4096},
			{EUeAgentResourceDimension::ReferenceEdges, 65536},
			{EUeAgentResourceDimension::WorkUnits, WorkLimit},
			{
				EUeAgentResourceDimension::EstimatedMemoryBytes,
				8388608
			},
		}));
	}

	static FUeAgentCanonicalIdentity MakeIdentity(
		const FString& Selector = TEXT("title"))
	{
		return FUeAgentCanonicalIdentity(
			TEXT("uai.reference.asset.") + Selector,
			TEXT("reference_asset"),
			TEXT("/Game/UAI/P4_011/SaveAsset"),
			Selector,
			Selector == TEXT("references")
				? TEXT("asset_references")
				: TEXT("asset_property"),
			TEXT("editor-session-29"));
	}

	static FUeAgentDescriptorIdentity MakeDescriptor()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.reference.asset.update"),
			1,
			TEXT(
				"sha256:24b70c881cdd8dc01b925ba4f7d15b630b7e5f178f3623fb54cb38195a3dff11"));
	}

	static FUeAgentCandidateBinding MakeBinding(
		const FUeAgentCanonicalIdentity& Identity)
	{
		return FUeAgentCandidateBinding(
			TEXT("binding:p4-011:save"),
			FUeAgentCandidateSummary(
				Identity,
				MakeDescriptor(),
				TEXT("reference_asset"),
				TEXT("persisted"),
				TEXT("checked-in C9 verification target"),
				{TEXT("P4-011")}),
			FUeAgentCatalogRevision(
				TEXT("fixture:c9"),
				TEXT("editor-session-29"),
				91),
			TEXT("/Game/UAI/P4_011/SaveAsset.title"),
			TEXT("read-port:reference-asset"),
			{EUeAgentCandidateOperation::Read},
			TEXT("fixture:c9:persisted:binding"));
	}

	static FUeAgentCanonicalValueRef MakeString(
		const FString& Value,
		FUeAgentResourceTracker& Tracker)
	{
		FUeAgentCanonicalValuePtr Result;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Value,
			Tracker,
			Result,
			Error));
		return Result.ToSharedRef();
	}

	static FUeAgentCanonicalSnapshotRef MakeSnapshot(
		const FString& SnapshotId,
		const FString& EvidenceId,
		const FString& Value,
		const int64 Sequence,
		FUeAgentResourceTracker& Tracker,
		const bool bExpected = false)
	{
		const FUeAgentStateRevision Revision(
			TEXT("fixture:c9:read-port"),
			TEXT("editor-session-29"),
			Sequence);
		const FUeAgentCanonicalIdentity Identity = MakeIdentity();
		TArray<FUeAgentStateEntry> Entries;
		Entries.Emplace(
			Identity,
			FUeAgentCanonicalType(
				TEXT("string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			MakeString(Value, Tracker),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("fixture:c9:string-default"),
				TEXT("checked-in C9 fixture"),
				MakeString(TEXT(""), Tracker)),
			FUeAgentStateDetails(
				TEXT("persisted"),
				TEXT(""),
				{TEXT("P4-011")},
				true,
				TEXT("available"),
				{}),
			Revision);
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		const FUeAgentStateProvenance Provenance(
			bExpected
				? TEXT("operation_expected_state")
				: TEXT("independent_readback"),
			bExpected
				? TEXT("fixture:c9:operation-plan")
				: TEXT("fixture:c9:read-port"),
			EvidenceId);
		const bool bCreated = bExpected
			? FUeAgentCanonicalSnapshot::TryCreateExpected(
				SnapshotId,
				EUeAgentStatePlane::Persisted,
				Revision,
				FUeAgentTemporalPoint(
					TEXT("fixture-clock"),
					FString::Printf(TEXT("%lld"), Sequence)),
				Provenance,
				Entries,
				Tracker,
				Snapshot,
				Error)
			: FUeAgentCanonicalSnapshot::TryCreate(
				SnapshotId,
				EUeAgentStatePlane::Persisted,
				Revision,
				FUeAgentTemporalPoint(
					TEXT("fixture-clock"),
					FString::Printf(TEXT("%lld"), Sequence)),
				Provenance,
				Entries,
				Tracker,
				Snapshot,
				Error);
		check(bCreated);
		return Snapshot.ToSharedRef();
	}

	static FDurableInventory MakeInventory()
	{
		return FDurableInventory(
			{
				FDurableArtifact(
					TEXT("artifact:p4-011:save:header"),
					MakeIdentity(),
					TEXT("/Game/UAI/P4_011/SaveAsset"),
					TEXT("D:/owned/save.uasset"),
					EAssetDurableIntent::Save,
					TEXT("package_header"))
			},
			TEXT(
				"sha256:f576bc7d6f1eeeda3a36cd16a081a24c2b91aa02a980d4992fb50f6672f17db1"));
	}

	static FDurableReceipt MakeReceipt(
		const EDurableStatus Status =
			EDurableStatus::PersistedUnverified,
		const ERecoveryAction RecoveryAction =
			ERecoveryAction::None)
	{
		return FDurableReceipt(
			TEXT("journal:p4-011:001"),
			Status,
			TEXT(
				"sha256:4042ab8bce9c9b56fcb511f09f63a74f70350ab2a42428cf727ae9c0c2b18db5"),
			TEXT(
				"sha256:f576bc7d6f1eeeda3a36cd16a081a24c2b91aa02a980d4992fb50f6672f17db1"),
			{
				TEXT("prepared"),
				TEXT("published"),
				TEXT("persisted_unverified")
			},
			RecoveryAction,
			true,
			false);
	}

	class FRecordingReadPort final : public IUeAgentStateReadPort
	{
	public:
		explicit FRecordingReadPort(
			FUeAgentCanonicalSnapshotRef InSnapshot)
			: Snapshot(MoveTemp(InSnapshot))
		{
		}

		virtual const FString& GetPortId() const override
		{
			static const FString Id =
				TEXT("read-port:reference-asset");
			return Id;
		}

		virtual bool TryRead(
			const FUeAgentStateReadRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentDiscoveryError& OutError) override
		{
			++CallCount;
			LastPlane = Request.GetStatePlane();
			OutSnapshot = Snapshot;
			return true;
		}

		int32 CallCount = 0;
		EUeAgentStatePlane LastPlane =
			EUeAgentStatePlane::AuthoringMemory;

	private:
		FUeAgentCanonicalSnapshotRef Snapshot;
	};

	class FRecordingAbsenceOracle final : public IAbsenceOracle
	{
	public:
		virtual bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			const TArray<FDurableArtifact>& Artifacts,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FAbsenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			++CallCount;
			OutObservation = FAbsenceObservation(
				Identity,
				true,
				false,
				TEXT("evidence:c9:absence:independent"));
			return true;
		}

		int32 CallCount = 0;
	};

	class FRecordingReferenceOracle final : public IReferenceOracle
	{
	public:
		virtual bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FReferenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			++CallCount;
			OutObservation = FReferenceObservation(
				Identity,
				{TEXT("/Game/UAI/Shared/RequiredAsset")},
				false,
				TEXT("evidence:c9:references:independent"));
			return true;
		}

		int32 CallCount = 0;
	};

	class FRecordingAcknowledger final
		: public IDurableVerificationAcknowledger
	{
	public:
		virtual bool TryAcknowledge(
			const FVerificationAcknowledgement& Acknowledgement,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			++CallCount;
			Last = Acknowledgement;
			return bAccept;
		}

		bool bAccept = true;
		int32 CallCount = 0;
		FVerificationAcknowledgement Last;
	};

	static FVerificationRequest MakeRequest(
		const FUeAgentCanonicalSnapshotRef& Expected,
		const EDurableStatus Status =
			EDurableStatus::PersistedUnverified,
		const ERecoveryAction RecoveryAction =
			ERecoveryAction::None)
	{
		const FUeAgentCanonicalIdentity Identity = MakeIdentity();
		return FVerificationRequest(
			TEXT("operation:p4-011:001"),
			MakeDescriptor(),
			MakeReceipt(Status, RecoveryAction),
			MakeInventory(),
			TEXT("owned-worker:p4-011"),
			47,
			{
				FVerificationTarget(
					Identity,
					MakeBinding(Identity),
					EVerificationOracleKind::Persisted,
					Expected,
					{TEXT("artifact:p4-011:save:header")},
					{})
			});
	}

	static bool RunPersistedVerification(
		FVerificationRequest Request,
		FRecordingReadPort& ReadPort,
		FRecordingAcknowledger& Acknowledger,
		FUeAgentResourceTracker& Tracker,
		FVerificationOutcome& OutOutcome,
		FVerificationError& OutError)
	{
		FRecordingAbsenceOracle Absence;
		FRecordingReferenceOracle References;
		double Now = 100.0;
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[&Now]() { return Now; });
		FUeAgentCancellationSource Cancellation(
			[&Now]() { return Now; });
		FVerificationService Service(
			ReadPort,
			Absence,
			References,
			Acknowledger);
		return Service.TryVerify(
			Request,
			Tracker,
			Deadline,
			Cancellation.GetToken(),
			OutOutcome,
			OutError);
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationModuleAvailableTest,
	"UeAgentInterface.Core.Verification.StateVerifier.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	TestTrue(TEXT("C9 Verification module is available"), true);
	static_assert(std::is_same_v<
		FVerificationAcknowledgement,
		FDurableVerificationAcknowledgement>);
	static_assert(std::is_same_v<
		IDurableVerificationAcknowledger,
		IDurableVerificationAcknowledgerPort>);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationIndependentReadbackTest,
	"UeAgentInterface.Core.Verification.StateVerifier.IndependentPersistedReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationIndependentReadbackTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker Tracker = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Expected = MakeSnapshot(
		TEXT("snapshot:c9:expected"),
		TEXT("evidence:c9:expected"),
		TEXT("new-title"),
		91,
		Tracker,
		true);
	FRecordingReadPort ReadPort(MakeSnapshot(
		TEXT("snapshot:c9:actual"),
		TEXT("evidence:c9:actual:independent"),
		TEXT("new-title"),
		92,
		Tracker));
	FRecordingAcknowledger Acknowledger;
	FVerificationOutcome Outcome;
	FVerificationError Error;
	TestTrue(
		TEXT("independent persisted verification succeeds"),
		RunPersistedVerification(
			MakeRequest(Expected),
			ReadPort,
			Acknowledger,
			Tracker,
			Outcome,
			Error));
	TestEqual(
		TEXT("C2 read port is called exactly once"),
		ReadPort.CallCount,
		1);
	TestEqual(
		TEXT("readback explicitly requests persisted state"),
		ReadPort.LastPlane,
		EUeAgentStatePlane::Persisted);
	TestEqual(
		TEXT("semantic equality produces complete"),
		Outcome.GetEffectOutcome(),
		EEffectOutcome::Complete);
	TestEqual(
		TEXT("complete is acknowledged once"),
		Acknowledger.CallCount,
		1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationMismatchTest,
	"UeAgentInterface.Core.Verification.StateVerifier.TypedMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationMismatchTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker Tracker = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Expected = MakeSnapshot(
		TEXT("snapshot:c9:expected"),
		TEXT("evidence:c9:expected"),
		TEXT("new-title"),
		91,
		Tracker,
		true);
	FRecordingReadPort ReadPort(MakeSnapshot(
		TEXT("snapshot:c9:actual"),
		TEXT("evidence:c9:actual:independent"),
		TEXT("wrong-title"),
		92,
		Tracker));
	FRecordingAcknowledger Acknowledger;
	FVerificationOutcome Outcome;
	FVerificationError Error;
	TestTrue(
		TEXT("known mismatch returns typed outcome"),
		RunPersistedVerification(
			MakeRequest(Expected),
			ReadPort,
			Acknowledger,
			Tracker,
			Outcome,
			Error));
	TestEqual(
		TEXT("known persisted mismatch is partial"),
		Outcome.GetEffectOutcome(),
		EEffectOutcome::Partial);
	TestEqual(
		TEXT("mismatch is not acknowledged"),
		Acknowledger.CallCount,
		0);
	TestFalse(
		TEXT("C4 semantic difference is preserved"),
		Outcome.GetTargets()[0].GetComparison().IsEquivalent());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationRecoveryTest,
	"UeAgentInterface.Core.Verification.StateVerifier.RecoveryUsesSameOracle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationRecoveryTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker Tracker = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Expected = MakeSnapshot(
		TEXT("snapshot:c9:expected"),
		TEXT("evidence:c9:expected"),
		TEXT("new-title"),
		91,
		Tracker,
		true);
	FRecordingReadPort ReadPort(MakeSnapshot(
		TEXT("snapshot:c9:actual"),
		TEXT("evidence:c9:actual:independent"),
		TEXT("new-title"),
		92,
		Tracker));
	FRecordingAcknowledger Acknowledger;
	FVerificationOutcome Outcome;
	FVerificationError Error;
	TestTrue(
		TEXT("recovery-required verify-persisted reuses C9"),
		RunPersistedVerification(
			MakeRequest(
				Expected,
				EDurableStatus::RecoveryRequired,
				ERecoveryAction::VerifyPersisted),
			ReadPort,
			Acknowledger,
			Tracker,
			Outcome,
			Error));
	TestEqual(
		TEXT("recovery path performs one independent read"),
		ReadPort.CallCount,
		1);
	TestEqual(
		TEXT("verified recovery can complete"),
		Outcome.GetEffectOutcome(),
		EEffectOutcome::Complete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationAcknowledgementTest,
	"UeAgentInterface.Core.Verification.StateVerifier.FencedAcknowledgement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationAcknowledgementTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker Tracker = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Expected = MakeSnapshot(
		TEXT("snapshot:c9:expected"),
		TEXT("evidence:c9:expected"),
		TEXT("new-title"),
		91,
		Tracker,
		true);
	FRecordingReadPort ReadPort(MakeSnapshot(
		TEXT("snapshot:c9:actual"),
		TEXT("evidence:c9:actual:independent"),
		TEXT("new-title"),
		92,
		Tracker));
	FRecordingAcknowledger Acknowledger;
	FVerificationOutcome Outcome;
	FVerificationError Error;
	RunPersistedVerification(
		MakeRequest(Expected),
		ReadPort,
		Acknowledger,
		Tracker,
		Outcome,
		Error);
	TestEqual(
		TEXT("acknowledgement binds journal"),
		Acknowledger.Last.GetJournalId(),
		FString(TEXT("journal:p4-011:001")));
	TestEqual(
		TEXT("acknowledgement binds journal owner"),
		Acknowledger.Last.GetJournalOwnerId(),
		FString(TEXT("owned-worker:p4-011")));
	TestEqual(
		TEXT("acknowledgement binds fence"),
		Acknowledger.Last.GetFencingToken(),
		static_cast<int64>(47));

	Acknowledger.bAccept = false;
	FVerificationOutcome RejectedOutcome;
	TestTrue(
		TEXT("stale acknowledgement remains a typed outcome"),
		RunPersistedVerification(
			MakeRequest(Expected),
			ReadPort,
			Acknowledger,
			Tracker,
			RejectedOutcome,
			Error));
	TestEqual(
		TEXT("rejected acknowledgement cannot claim complete"),
		RejectedOutcome.GetEffectOutcome(),
		EEffectOutcome::StateUnknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationStateUnknownTest,
	"UeAgentInterface.Core.Verification.StateVerifier.StateUnknownNoReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationStateUnknownTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker Tracker = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Expected = MakeSnapshot(
		TEXT("snapshot:c9:expected"),
		TEXT("evidence:c9:expected"),
		TEXT("new-title"),
		91,
		Tracker,
		true);
	FRecordingReadPort ReadPort(MakeSnapshot(
		TEXT("snapshot:c9:actual"),
		TEXT("evidence:c9:actual:independent"),
		TEXT("new-title"),
		92,
		Tracker));
	FRecordingAcknowledger Acknowledger;
	FVerificationOutcome Outcome;
	FVerificationError Error;
	TestTrue(
		TEXT("state_unknown is returned without replay"),
		RunPersistedVerification(
			MakeRequest(
				Expected,
				EDurableStatus::StateUnknown,
				ERecoveryAction::ManualIntervention),
			ReadPort,
			Acknowledger,
			Tracker,
			Outcome,
			Error));
	TestEqual(
		TEXT("state_unknown receipt is not self-proved"),
		Outcome.GetEffectOutcome(),
		EEffectOutcome::StateUnknown);
	TestEqual(
		TEXT("state_unknown receipt performs no read/replay"),
		ReadPort.CallCount,
		0);
	TestEqual(
		TEXT("state_unknown receipt is not acknowledged"),
		Acknowledger.CallCount,
		0);
	return true;
}
