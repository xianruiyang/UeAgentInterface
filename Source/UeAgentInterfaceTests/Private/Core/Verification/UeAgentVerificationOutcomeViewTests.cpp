#include "Misc/AutomationTest.h"

// P4-017 test-first contract: production is intentionally absent in this task.
#include "Core/Verification/UeAgentVerificationOutcomeView.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationOutcomeViewModuleAvailableTest,
	"UeAgentInterface.Core.Verification.OutcomeView.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationOutcomeViewModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	return true;
}


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Projection/UeAgentProjection.h"
namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	static FUeAgentResourceTracker MakeViewTracker(
		const int64 Nodes = 4096,
		const int64 StringBytes = 65536)
	{
		return FUeAgentResourceTracker();
	}

	static FUeAgentCanonicalIdentity MakeIdentity(
		const bool bReference)
	{
		return FUeAgentCanonicalIdentity(
			bReference
				? TEXT("uai.reference.asset.references")
				: TEXT("uai.reference.asset.title"),
			TEXT("reference_asset"),
			TEXT("/Game/UAI/P4_017/SaveAsset"),
			bReference ? TEXT("references") : TEXT("title"),
			bReference
				? TEXT("asset_references")
				: TEXT("asset_property"),
			TEXT("editor-session-31"));
	}

	static FUeAgentCanonicalValuePtr FindObjectField(
		const FUeAgentCanonicalValueRef& Object,
		const FString& Field)
	{
		if (Object->GetKind() != EUeAgentValueKind::Object)
		{
			return nullptr;
		}
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Item :
			Object->GetObjectItems())
		{
			if (Item.Key == Field)
			{
				return Item.Value;
			}
		}
		return nullptr;
	}

	static TArray<FString> ObjectKeys(
		const FUeAgentCanonicalValueRef& Object)
	{
		TArray<FString> Keys;
		if (Object->GetKind() != EUeAgentValueKind::Object)
		{
			return Keys;
		}
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Item :
			Object->GetObjectItems())
		{
			Keys.Add(Item.Key);
		}
		Keys.Sort();
		return Keys;
	}

	static FUeAgentCanonicalValuePtr RequireNull(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& Tracker)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create canonical null evidence"),
			FUeAgentCanonicalValue::TryNull(
				Tracker,
				Value,
				Error));
		return Value;
	}

	static FVerificationAcknowledgement MakeAcknowledgement()
	{
		return FVerificationAcknowledgement(
			TEXT("journal:p4-017:001"),
			TEXT(
				"sha256:4042ab8bce9c9b56fcb511f09f63a74f70350ab2a42428cf727ae9c0c2b18db5"),
			TEXT(
				"sha256:f576bc7d6f1eeeda3a36cd16a081a24c2b91aa02a980d4992fb50f6672f17db1"),
			TEXT("verification:p4-017:complete"),
			TEXT("owned-worker:p4-017"),
			47);
	}

	static FTargetVerification MakeEquivalentTarget(
		const bool bReference)
	{
		return FTargetVerification(
			MakeIdentity(bReference),
			bReference
				? EVerificationOracleKind::Reference
				: EVerificationOracleKind::Persisted,
			EVerificationStatus::Verified,
			bReference
				? FString()
				: FString(TEXT("snapshot:p4-017:expected")),
			bReference
				? FString()
				: FString(TEXT("snapshot:p4-017:actual")),
			FUeAgentSemanticComparison(),
			!bReference,
			{
				bReference
					? TEXT("evidence:p4-017:reference")
					: TEXT("evidence:p4-017:persisted")
			});
	}

	static FVerificationOutcome MakeCompleteOutcome()
	{
		return FVerificationOutcome(
			TEXT("verification:p4-017:complete"),
			EVerificationStatus::Verified,
			EEffectOutcome::Complete,
			TEXT("verification_complete"),
			false,
			{
				MakeEquivalentTarget(false),
				MakeEquivalentTarget(true),
			},
			{
				TEXT("evidence:p4-017:persisted"),
				TEXT("evidence:p4-017:reference"),
				TEXT("evidence:p4-017:verification"),
			},
			MakeAcknowledgement());
	}

	static FVerificationOutcome MakePartialOutcome(
		const FUeAgentCanonicalValuePtr& Left)
	{
		const FString Entry = TEXT("/entries/") + MakeIdentity(false).GetCanonicalKey()
			.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		const FString OtherEntry = TEXT("/entries/") + MakeIdentity(true).GetCanonicalKey()
			.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		return FVerificationOutcome(
			TEXT("verification:p4-017:partial"),
			EVerificationStatus::Mismatch,
			EEffectOutcome::Partial,
			TEXT("verification_semantic_mismatch"),
			false,
			{
				FTargetVerification(
					MakeIdentity(false),
					EVerificationOracleKind::Persisted,
					EVerificationStatus::Mismatch,
					TEXT("snapshot:p4-017:expected"),
					TEXT("snapshot:p4-017:actual"),
					FUeAgentSemanticComparison({
						FUeAgentSemanticDifference(
							EUeAgentDifferenceKind::MissingRight,
							Entry + TEXT("/value"),
							Left,
							nullptr),
						FUeAgentSemanticDifference(
							EUeAgentDifferenceKind::MissingRight,
							Entry + TEXT("/value/nested~1field"), Left, nullptr),
						FUeAgentSemanticDifference(
							EUeAgentDifferenceKind::MissingLeft,
							OtherEntry, nullptr, Left)
					}),
					true,
					{TEXT("evidence:p4-017:mismatch")})
			},
			{TEXT("evidence:p4-017:mismatch")},
			{});
	}

	static bool BuildOutcomeView(
		const FVerificationOutcome& Outcome,
		FUeAgentResourceTracker& Tracker,
		FUeAgentCanonicalValuePtr& OutView,
		FVerificationError& OutError)
	{
		return TryCanonicalizeVerificationOutcome(
			Outcome,
			Tracker,
			OutView,
			OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationOutcomeViewContractTest,
	"UeAgentInterface.Core.Verification.OutcomeView.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationOutcomeViewContractTest::RunTest(
	const FString& Parameters)
{
	const FVerificationOutcome Outcome(
		TEXT("verification:p4-017:rolled-back"),
		EVerificationStatus::Unavailable,
		EEffectOutcome::RolledBack,
		TEXT("verification_durable_rolled_back"),
		false,
		{},
		{TEXT("evidence:p4-017:rolled-back")},
		{});
	FUeAgentResourceTracker Tracker = MakeViewTracker();
	FUeAgentCanonicalValuePtr View;
	FVerificationError Error;
	if (!TestTrue(
		TEXT("canonicalize rolled-back outcome"),
		BuildOutcomeView(Outcome, Tracker, View, Error)))
	{
		return false;
	}
	TestTrue(TEXT("view returned"), View.IsValid());
	TestEqual(
		TEXT("root kind"),
		View->GetKind(),
		EUeAgentValueKind::Object);

	const FUeAgentCanonicalValuePtr Schema =
		FindObjectField(View.ToSharedRef(), TEXT("$schema"));
	const FUeAgentCanonicalValuePtr Type =
		FindObjectField(View.ToSharedRef(), TEXT("$type"));
	TestTrue(TEXT("schema field present"), Schema.IsValid());
	TestTrue(TEXT("type field present"), Type.IsValid());
	if (Schema.IsValid())
	{
		TestEqual(
			TEXT("schema id"),
			Schema->GetText(),
			FString(TEXT("uai.c9.canonical_outcome_view.v1")));
	}
	if (Type.IsValid())
	{
		TestEqual(
			TEXT("root type"),
			Type->GetText(),
			FString(TEXT("verification_outcome")));
	}
	TestFalse(
		TEXT("acknowledgement is absent when not produced"),
		FindObjectField(
			View.ToSharedRef(),
			TEXT("acknowledgement")).IsValid());

	TArray<FString> ExpectedKeys = {
		TEXT("$schema"),
		TEXT("$type"),
		TEXT("status"),
		TEXT("effect_outcome"),
		TEXT("diagnostic_code"),
		TEXT("diagnostic"),
		TEXT("retry_safe"),
		TEXT("targets"),
	};
	ExpectedKeys.Sort();
	TestEqual(
		TEXT("root fixed field count"),
		ObjectKeys(View.ToSharedRef()).Num(),
		ExpectedKeys.Num());
	TestTrue(
		TEXT("root fixed fields"),
		ObjectKeys(View.ToSharedRef()) == ExpectedKeys);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationOutcomeViewFullFieldTest,
	"UeAgentInterface.Core.Verification.OutcomeView.FullField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationOutcomeViewFullFieldTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker Tracker = MakeViewTracker();
	FUeAgentCanonicalValuePtr View;
	FVerificationError Error;
	if (!TestTrue(
		TEXT("canonicalize complete outcome"),
		BuildOutcomeView(
			MakeCompleteOutcome(),
			Tracker,
			View,
			Error)))
	{
		return false;
	}

	const FUeAgentCanonicalValuePtr Targets =
		FindObjectField(View.ToSharedRef(), TEXT("targets"));
	TestTrue(TEXT("targets present"), Targets.IsValid());
	if (!Targets.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("two targets preserved"),
		Targets->GetArrayItems().Num(),
		2);
	for (const TCHAR* Field : {
		TEXT("identity"),
		TEXT("expected_snapshot_id"),
		TEXT("actual_snapshot_id"),
		TEXT("evidence_ids")})
	{
		for (const FUeAgentCanonicalValueRef& Target :
			Targets->GetArrayItems())
		{
			TestFalse(
				FString::Printf(TEXT("target field hidden: %s"), Field),
				FindObjectField(Target, Field).IsValid());
		}
	}
	TestFalse(
		TEXT("acknowledgement is machine-only"),
		FindObjectField(
			View.ToSharedRef(),
			TEXT("acknowledgement")).IsValid());
	TestFalse(
		TEXT("default verified outcome has no public result"),
		FindObjectField(
			View.ToSharedRef(),
			TEXT("result")).IsValid());
	for (const TCHAR* Field : {
		TEXT("verification_id"),
		TEXT("evidence_ids")})
	{
		TestFalse(
			FString::Printf(TEXT("outcome field hidden: %s"), Field),
			FindObjectField(View.ToSharedRef(), Field).IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationOutcomeViewComparisonTest,
	"UeAgentInterface.Core.Verification.OutcomeView.Comparison",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationOutcomeViewComparisonTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker BuildTracker = MakeViewTracker();
	const FUeAgentCanonicalValuePtr Left =
		RequireNull(this, BuildTracker);
	FUeAgentResourceTracker ViewTracker = MakeViewTracker();
	FUeAgentCanonicalValuePtr View;
	FVerificationError Error;
	if (!TestTrue(
		TEXT("canonicalize partial outcome"),
		BuildOutcomeView(
			MakePartialOutcome(Left),
			ViewTracker,
			View,
			Error)))
	{
		return false;
	}

	const FUeAgentCanonicalValuePtr Targets =
		FindObjectField(View.ToSharedRef(), TEXT("targets"));
	const FUeAgentCanonicalValuePtr Comparison =
		FindObjectField(
			Targets->GetArrayItems()[0],
			TEXT("comparison"));
	const FUeAgentCanonicalValuePtr Differences =
		FindObjectField(
			Comparison.ToSharedRef(),
			TEXT("differences"));
	const FUeAgentCanonicalValueRef Difference =
		Differences->GetArrayItems()[0];
	TestEqual(TEXT("all differences remain visible"), Differences->GetArrayItems().Num(), 3);
	TestEqual(TEXT("canonical entry identity becomes a local index"),
		FindObjectField(Difference, TEXT("path"))->GetText(), FString(TEXT("/entries/0/value")));
	TestEqual(TEXT("same entry reuses index and preserves field escaping"),
		FindObjectField(Differences->GetArrayItems()[1], TEXT("path"))->GetText(),
		FString(TEXT("/entries/0/value/nested~1field")));
	TestEqual(TEXT("missing second entry retains distinct index"),
		FindObjectField(Differences->GetArrayItems()[2], TEXT("path"))->GetText(),
		FString(TEXT("/entries/1")));
	const FUeAgentCanonicalValuePtr Target =
		FindObjectField(Targets->GetArrayItems()[0], TEXT("target"));
	TestTrue(TEXT("business target remains locatable"), Target.IsValid());
	if (Target.IsValid())
	{
		TestTrue(TEXT("business selectors remain structured"),
			FindObjectField(Target.ToSharedRef(), TEXT("selectors")).IsValid());
		TestFalse(TEXT("target omits generation custody"),
			FindObjectField(Target.ToSharedRef(), TEXT("generation")).IsValid());
	}
	const FUeAgentCanonicalValuePtr ObservedLeft =
		FindObjectField(Difference, TEXT("left"));
	const FUeAgentCanonicalValuePtr ObservedRight =
		FindObjectField(Difference, TEXT("right"));

	TestTrue(TEXT("left evidence present"), ObservedLeft.IsValid());
	TestTrue(
		TEXT("canonical left reference preserved"),
		ObservedLeft.Get() == Left.Get());
	TestEqual(
		TEXT("canonical null remains null"),
		ObservedLeft->GetKind(),
		EUeAgentValueKind::Null);
	TestFalse(
		TEXT("absent right field remains absent"),
		ObservedRight.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationOutcomeViewProjectionOpaqueTest,
	"UeAgentInterface.Core.Verification.OutcomeView.ProjectionOpaque",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationOutcomeViewProjectionOpaqueTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker Tracker = MakeViewTracker();
	FUeAgentCanonicalValuePtr View;
	FVerificationError Error;
	if (!TestTrue(
		TEXT("canonicalize outcome for projection"),
		BuildOutcomeView(
			MakeCompleteOutcome(),
			Tracker,
			View,
			Error)))
	{
		return false;
	}

	const FUeAgentProjectionSourceValue Source(View.ToSharedRef());
	const FUeAgentProjectedPayload Payload(View.ToSharedRef());
	TestTrue(
		TEXT("C10 source keeps canonical view opaque"),
		Source.GetValue().Get() == View.Get());
	TestTrue(
		TEXT("C10 payload keeps canonical view opaque"),
		Payload.GetValue().Get() == View.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentVerificationOutcomeViewOptionalPublicResultTest,
	"UeAgentInterface.Core.Verification.OutcomeView.OptionalPublicResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationOutcomeViewOptionalPublicResultTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker BuildTracker = MakeViewTracker();
	FUeAgentCanonicalValuePtr TargetRef;
	FUeAgentCanonicalValuePtr PublicResult;
	FUeAgentStateSemanticError StateError;
	if (!TestTrue(
			TEXT("build public target ref"),
			FUeAgentCanonicalValue::TryString(
				TEXT("/Game/UAI/Public.Target"),
				BuildTracker,
				TargetRef,
				StateError))
		|| !TestTrue(
			TEXT("build public result"),
			FUeAgentCanonicalValue::TryObject(
				{{TEXT("target_ref"), TargetRef.ToSharedRef()}},
				BuildTracker,
				PublicResult,
				StateError)))
	{
		return false;
	}

	const FVerificationOutcome Outcome(
		TEXT("verification:p4-017:public-result"),
		EVerificationStatus::Verified,
		EEffectOutcome::Complete,
		FString(),
		false,
		{},
		{TEXT("evidence:p4-017:public-result")},
		{},
		FString(),
		PublicResult);
	FUeAgentResourceTracker ViewTracker = MakeViewTracker();
	FUeAgentCanonicalValuePtr View;
	FVerificationError Error;
	if (!TestTrue(
			TEXT("canonicalize outcome with public result"),
			BuildOutcomeView(Outcome, ViewTracker, View, Error)))
	{
		return false;
	}
	const FUeAgentCanonicalValuePtr Observed =
		FindObjectField(View.ToSharedRef(), TEXT("result"));
	TestTrue(TEXT("optional result is exposed"), Observed.IsValid());
	TestTrue(
		TEXT("immutable canonical result reference is preserved"),
		Observed.Get() == PublicResult.Get());

	const FVerificationOutcome RejectedOutcome(
		TEXT("verification:p4-017:unverified-public-result"),
		EVerificationStatus::Mismatch,
		EEffectOutcome::Partial,
		TEXT("verification_semantic_mismatch"),
		false,
		{},
		{},
		{},
		FString(),
		PublicResult);
	FUeAgentResourceTracker RejectedTracker = MakeViewTracker();
	FUeAgentCanonicalValuePtr RejectedView;
	if (!TestTrue(
			TEXT("canonicalize unverified outcome"),
			BuildOutcomeView(
				RejectedOutcome,
				RejectedTracker,
				RejectedView,
				Error)))
	{
		return false;
	}
	TestFalse(
		TEXT("unverified public result is discarded"),
		FindObjectField(
			RejectedView.ToSharedRef(),
			TEXT("result")).IsValid());
	return true;
}
