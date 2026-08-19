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
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	static FString OutcomeViewFixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT(
				"UeAgentInterfaceCMD/tests/verification_service/fixtures/"
				"canonical_outcome_view_cases.json")));
	}

	static TSharedPtr<FJsonObject> LoadOutcomeViewFixture(
		FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
			TEXT("load shared C9 outcome-view fixture"),
			FFileHelper::LoadFileToString(
				Text,
				*OutcomeViewFixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
			TEXT("parse shared C9 outcome-view fixture"),
			FJsonSerializer::Deserialize(Reader, Fixture)
				&& Fixture.IsValid()))
		{
			return nullptr;
		}
		Test->TestEqual(
			TEXT("fixture schema"),
			Fixture->GetStringField(TEXT("schema")),
			FString(TEXT(
				"uai.c9.canonical_outcome_view.test_fixture.v1")));
		Test->TestEqual(
			TEXT("outcome-view schema"),
			Fixture
				->GetObjectField(TEXT("contract"))
				->GetStringField(TEXT("schema_id")),
			FString(TEXT(
				"uai.c9.canonical_outcome_view.v1")));
		return Fixture;
	}

	static FUeAgentResourceTracker MakeViewTracker(
		const int64 Nodes = 4096,
		const int64 StringBytes = 65536)
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 1048576},
			{EUeAgentResourceDimension::OutputBytes, 1048576},
			{EUeAgentResourceDimension::StringBytes, StringBytes},
			{EUeAgentResourceDimension::Nodes, Nodes},
			{EUeAgentResourceDimension::Items, 4096},
			{EUeAgentResourceDimension::Files, 128},
			{EUeAgentResourceDimension::ReferenceEdges, 4096},
			{EUeAgentResourceDimension::WorkUnits, 16384},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 1048576},
		}));
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
							TEXT("/entries/0/value"),
							Left,
							nullptr)
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
	const TSharedPtr<FJsonObject> Fixture = LoadOutcomeViewFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

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

	TArray<FString> ExpectedKeys;
	for (const TSharedPtr<FJsonValue>& Field :
		Fixture
			->GetObjectField(TEXT("contract"))
			->GetObjectField(TEXT("type_fields"))
			->GetObjectField(TEXT("verification_outcome"))
			->GetArrayField(TEXT("required")))
	{
		ExpectedKeys.Add(Field->AsString());
	}
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
	const FUeAgentCanonicalValuePtr FirstIdentity =
		FindObjectField(
			Targets->GetArrayItems()[0],
			TEXT("identity"));
	const FUeAgentCanonicalValuePtr FirstSelector =
		FirstIdentity.IsValid()
			? FindObjectField(
				FirstIdentity.ToSharedRef(),
				TEXT("selector"))
			: nullptr;
	TestTrue(
		TEXT("first selector present"),
		FirstSelector.IsValid());
	if (FirstSelector.IsValid())
	{
		TestEqual(
			TEXT("targets sorted by canonical identity"),
			FirstSelector->GetText(),
			FString(TEXT("title")));
	}
	const FUeAgentCanonicalValuePtr SecondIdentity =
		FindObjectField(
			Targets->GetArrayItems()[1],
			TEXT("identity"));
	const FUeAgentCanonicalValuePtr SecondSelector =
		SecondIdentity.IsValid()
			? FindObjectField(
				SecondIdentity.ToSharedRef(),
				TEXT("selector"))
			: nullptr;
	TestTrue(
		TEXT("second selector present"),
		SecondSelector.IsValid());
	if (SecondSelector.IsValid())
	{
		TestEqual(
			TEXT("canonical identity order is complete"),
			SecondSelector->GetText(),
			FString(TEXT("references")));
	}

	const FUeAgentCanonicalValuePtr Acknowledgement =
		FindObjectField(
			View.ToSharedRef(),
			TEXT("acknowledgement"));
	TestTrue(
		TEXT("acknowledgement present"),
		Acknowledgement.IsValid());
	const FUeAgentCanonicalValuePtr FencingToken =
		Acknowledgement.IsValid()
			? FindObjectField(
				Acknowledgement.ToSharedRef(),
				TEXT("fencing_token"))
			: nullptr;
	TestTrue(
		TEXT("fencing token present"),
		FencingToken.IsValid());
	if (FencingToken.IsValid())
	{
		TestEqual(
			TEXT("fencing token is exact integer"),
			FencingToken->GetKind(),
			EUeAgentValueKind::Integer);
		TestEqual(
			TEXT("fencing token lexeme"),
			FencingToken->GetLexeme(),
			FString(TEXT("47")));
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
	FUeAgentVerificationOutcomeViewResourceDepthTest,
	"UeAgentInterface.Core.Verification.OutcomeView.ResourceDepth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentVerificationOutcomeViewResourceDepthTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker BuildTracker = MakeViewTracker(8192);
	FUeAgentCanonicalValuePtr Deep =
		RequireNull(this, BuildTracker);
	FUeAgentStateSemanticError StateError;
	for (int32 Index = 0; Index < 1536; ++Index)
	{
		FUeAgentCanonicalValuePtr Next;
		if (!TestTrue(
			TEXT("build deep canonical evidence"),
			FUeAgentCanonicalValue::TryArray(
				{Deep.ToSharedRef()},
				BuildTracker,
				Next,
				StateError)))
		{
			return false;
		}
		Deep = MoveTemp(Next);
	}

	FUeAgentResourceTracker ViewTracker = MakeViewTracker(128);
	FUeAgentCanonicalValuePtr View;
	FVerificationError Error;
	if (!TestTrue(
		TEXT("deep evidence does not impose a depth policy"),
		BuildOutcomeView(
			MakePartialOutcome(Deep),
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
	const FUeAgentCanonicalValuePtr Observed =
		FindObjectField(
			Differences->GetArrayItems()[0],
			TEXT("left"));
	TestTrue(
		TEXT("deep canonical evidence reference preserved"),
		Observed.Get() == Deep.Get());

	FUeAgentResourceTracker TinyTracker = MakeViewTracker(1);
	FUeAgentCanonicalValuePtr Rejected;
	FVerificationError BudgetError;
	TestFalse(
		TEXT("node budget rejects whole view"),
		BuildOutcomeView(
			MakeCompleteOutcome(),
			TinyTracker,
			Rejected,
			BudgetError));
	TestEqual(
		TEXT("budget error code"),
		BudgetError.GetCode(),
		FString(TEXT("resource_budget_exceeded")));
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

