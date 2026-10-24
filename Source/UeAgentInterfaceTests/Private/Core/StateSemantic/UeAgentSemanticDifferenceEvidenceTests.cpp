#include "Misc/AutomationTest.h"

// Re-evaluate the complete contract when the production evidence header exists.
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticDifferenceEvidenceModuleAvailableTest,
	"UeAgentInterface.Core.StateSemantic.DifferenceEvidence.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticDifferenceEvidenceModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	return true;
}


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	static FString EvidenceFixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT(
				"UeAgentInterfaceCMD/tests/state_semantic/fixtures/"
				"semantic_difference_evidence_cases.json")));
	}

	static TSharedPtr<FJsonObject> LoadEvidenceFixture(
		FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
			TEXT("load shared C4 difference evidence fixture"),
			FFileHelper::LoadFileToString(Text, *EvidenceFixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
			TEXT("parse shared C4 difference evidence fixture"),
			FJsonSerializer::Deserialize(Reader, Fixture)
				&& Fixture.IsValid()))
		{
			return nullptr;
		}
		Test->TestEqual(
			TEXT("difference evidence fixture schema"),
			Fixture->GetStringField(TEXT("schema")),
			FString(TEXT(
				"uai.c4.semantic_difference_evidence.test_fixture.v1")));
		Test->TestEqual(
			TEXT("difference evidence contract schema"),
			Fixture
				->GetObjectField(TEXT("contract"))
				->GetStringField(TEXT("schema_id")),
			FString(TEXT(
				"uai.c4.semantic_difference_evidence.v1")));
		return Fixture;
	}

	static FUeAgentResourceTracker MakeEvidenceTracker(
		int64 Nodes = 4096)
	{
		return FUeAgentResourceTracker();
	}

	static FUeAgentCanonicalValueRef RequireNull(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& Tracker)
	{
		FUeAgentStateSemanticError Error;
		FUeAgentCanonicalValuePtr Value;
		Test->TestTrue(
			TEXT("create canonical null"),
			FUeAgentCanonicalValue::TryNull(Tracker, Value, Error));
		Test->TestTrue(TEXT("canonical null returned"), Value.IsValid());
		return Value.ToSharedRef();
	}

	static FUeAgentCanonicalValueRef RequireDecimal(
		FAutomationTestBase* Test,
		const FString& Lexeme,
		FUeAgentResourceTracker& Tracker)
	{
		FUeAgentStateSemanticError Error;
		FUeAgentCanonicalValuePtr Value;
		Test->TestTrue(
			TEXT("create canonical decimal"),
			FUeAgentCanonicalValue::TryDecimal(
				Lexeme,
				Tracker,
				Value,
				Error));
		Test->TestTrue(TEXT("canonical decimal returned"), Value.IsValid());
		return Value.ToSharedRef();
	}

	static FUeAgentCanonicalIdentity MakeIdentity()
	{
		return FUeAgentCanonicalIdentity(
			TEXT("core.property"),
			TEXT("property"),
			TEXT("/Game/Fixture/BP_StateFixture.BP_StateFixture_C"),
			TEXT("Defaults.PreciseValue"),
			TEXT("class_default_object"),
			TEXT("fixture-generation-7"));
	}

	static FUeAgentCanonicalType MakeType()
	{
		return FUeAgentCanonicalType(
			TEXT("core.decimal"),
			EUeAgentValueKind::Decimal,
			false,
			TEXT("scalar"));
	}

	static FUeAgentTemporalPoint MakeTemporal()
	{
		return FUeAgentTemporalPoint(
			TEXT("fixture-timeline"),
			TEXT("24000"),
			TEXT("24000"),
			TEXT("1001"),
			TEXT("evaluated"));
	}

	static FUeAgentSpatialFrame MakeSpatialFrame()
	{
		return FUeAgentSpatialFrame(
			TEXT("fixture-local"),
			TEXT("fixture-world"),
			3,
			{
				{TEXT("0"), TEXT("1"), TEXT("0")},
				{TEXT("-2"), TEXT("0"), TEXT("0")},
				{TEXT("0"), TEXT("0"), TEXT("0.5")},
			},
			{TEXT("10"), TEXT("20"), TEXT("30")},
			TEXT("fixture_unit"),
			TEXT("fixture:declared-local-basis"),
			MakeTemporal());
	}

	static FUeAgentSpatialValue MakeSpatialValue()
	{
		return FUeAgentSpatialValue(
			TEXT("fixture-component"),
			EUeAgentSpatialQuantityKind::Point,
			MakeSpatialFrame(),
			{TEXT("1.25"), TEXT("2.5"), TEXT("3.75")},
			MakeTemporal(),
			{TEXT("fixture:direct-observation")});
	}

	static FUeAgentDefaultValue MakeDefault(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& Tracker)
	{
		return FUeAgentDefaultValue(
			EUeAgentDefaultSourceKind::ClassDefaultObject,
			TEXT(
				"/Game/Fixture/BP_StateFixture."
				"Default__BP_StateFixture_C"),
			TEXT("fixture:cdo-readback:Defaults.PreciseValue"),
			RequireDecimal(Test, TEXT("41"), Tracker));
	}

	static FUeAgentStateDetails MakeDetails()
	{
		return FUeAgentStateDetails(
			TEXT("精确值"),
			TEXT("Independent fixture metadata"),
			{TEXT("Fixture"), TEXT("State")},
			true,
			TEXT("available"),
			{
				{TEXT("maximum"), TEXT("100")},
				{TEXT("minimum"), TEXT("-100")},
			});
	}

	static FUeAgentStateRevision MakeRevision()
	{
		return FUeAgentStateRevision(
			TEXT("fixture-reader"),
			TEXT("fixture-generation-7"),
			17);
	}

	static FUeAgentStateEntry MakeEntry(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& Tracker)
	{
		return FUeAgentStateEntry(
			MakeIdentity(),
			MakeType(),
			RequireDecimal(
				Test,
				TEXT("1234567890.12345678901234567890123456789"),
				Tracker),
			MakeDefault(Test, Tracker),
			MakeDetails(),
			MakeRevision());
	}

	static const FUeAgentCanonicalValueRef* FindObjectField(
		const FUeAgentCanonicalValueRef& Object,
		const FString& Key)
	{
		if (Object->GetKind() != EUeAgentValueKind::Object)
		{
			return nullptr;
		}
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Item :
			Object->GetObjectItems())
		{
			if (Item.Key == Key)
			{
				return &Item.Value;
			}
		}
		return nullptr;
	}

	static FString EvidenceTypeTag(
		FAutomationTestBase* Test,
		const FUeAgentCanonicalValuePtr& Value)
	{
		if (!Test->TestTrue(
			TEXT("canonical evidence exists"),
			Value.IsValid()))
		{
			return FString();
		}
		const FUeAgentCanonicalValueRef* Tag =
			FindObjectField(Value.ToSharedRef(), TEXT("$type"));
		if (!Test->TestNotNull(
			TEXT("canonical evidence has $type"),
			Tag))
		{
			return FString();
		}
		Test->TestEqual(
			TEXT("$type is a canonical string"),
			(*Tag)->GetKind(),
			EUeAgentValueKind::String);
		return (*Tag)->GetText();
	}

	static FString CanonicalScalarText(
		const FUeAgentCanonicalValueRef& Value)
	{
		if (
			Value->GetKind() == EUeAgentValueKind::Integer
			|| Value->GetKind() == EUeAgentValueKind::Decimal)
		{
			return Value->GetLexeme();
		}
		return Value->GetText();
	}

	static bool CanonicalMatchesJson(
		FAutomationTestBase* Test,
		const FUeAgentCanonicalValueRef& Actual,
		const TSharedPtr<FJsonValue>& Expected,
		const FString& Path)
	{
		switch (Expected->Type)
		{
		case EJson::Null:
			return Test->TestEqual(
				*FString::Printf(TEXT("%s kind"), *Path),
				Actual->GetKind(),
				EUeAgentValueKind::Null);
		case EJson::Boolean:
			return Test->TestEqual(
				*FString::Printf(TEXT("%s bool"), *Path),
				Actual->GetBoolean(),
				Expected->AsBool())
				&& Test->TestEqual(
					*FString::Printf(TEXT("%s kind"), *Path),
					Actual->GetKind(),
					EUeAgentValueKind::Boolean);
		case EJson::String:
			if (!Test->TestTrue(
				*FString::Printf(TEXT("%s scalar kind"), *Path),
				Actual->GetKind() == EUeAgentValueKind::String
					|| Actual->GetKind() == EUeAgentValueKind::Integer
					|| Actual->GetKind() == EUeAgentValueKind::Decimal))
			{
				return false;
			}
			return Test->TestEqual(
				*FString::Printf(TEXT("%s scalar"), *Path),
				CanonicalScalarText(Actual),
				Expected->AsString());
		case EJson::Array:
		{
			if (!Test->TestEqual(
				*FString::Printf(TEXT("%s kind"), *Path),
				Actual->GetKind(),
				EUeAgentValueKind::Array))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>& ExpectedItems =
				Expected->AsArray();
			const TArray<FUeAgentCanonicalValueRef>& ActualItems =
				Actual->GetArrayItems();
			if (!Test->TestEqual(
				*FString::Printf(TEXT("%s item count"), *Path),
				ActualItems.Num(),
				ExpectedItems.Num()))
			{
				return false;
			}
			for (int32 Index = 0; Index < ExpectedItems.Num(); ++Index)
			{
				if (!CanonicalMatchesJson(
					Test,
					ActualItems[Index],
					ExpectedItems[Index],
					FString::Printf(TEXT("%s/%d"), *Path, Index)))
				{
					return false;
				}
			}
			return true;
		}
		case EJson::Object:
		{
			if (!Test->TestEqual(
				*FString::Printf(TEXT("%s kind"), *Path),
				Actual->GetKind(),
				EUeAgentValueKind::Object))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> ExpectedObject =
				Expected->AsObject();
			if (!Test->TestEqual(
				*FString::Printf(TEXT("%s field count"), *Path),
				Actual->GetObjectItems().Num(),
				ExpectedObject->Values.Num()))
			{
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
				ExpectedObject->Values)
			{
				const FUeAgentCanonicalValueRef* ActualField =
					FindObjectField(Actual, Field.Key);
				if (!Test->TestNotNull(
					*FString::Printf(
						TEXT("%s/%s exists"),
						*Path,
						*Field.Key),
					ActualField))
				{
					return false;
				}
				if (!CanonicalMatchesJson(
					Test,
					*ActualField,
					Field.Value,
					FString::Printf(
						TEXT("%s/%s"),
						*Path,
						*Field.Key)))
				{
					return false;
				}
			}
			return true;
		}
		default:
			Test->AddError(FString::Printf(
				TEXT("unsupported fixture JSON at %s"),
				*Path));
			return false;
		}
	}

	static FString KindName(EUeAgentValueKind Kind)
	{
		switch (Kind)
		{
		case EUeAgentValueKind::Null: return TEXT("null");
		case EUeAgentValueKind::Boolean: return TEXT("boolean");
		case EUeAgentValueKind::String: return TEXT("string");
		case EUeAgentValueKind::Integer: return TEXT("integer");
		case EUeAgentValueKind::Decimal: return TEXT("decimal");
		case EUeAgentValueKind::Array: return TEXT("array");
		case EUeAgentValueKind::Object: return TEXT("object");
		case EUeAgentValueKind::Temporal: return TEXT("temporal");
		case EUeAgentValueKind::Spatial: return TEXT("spatial");
		default: return TEXT("unknown");
		}
	}

	static FUeAgentCanonicalValuePtr CanonicalAt(
		const FUeAgentCanonicalValuePtr& Root,
		const FString& Pointer)
	{
		FUeAgentCanonicalValuePtr Current = Root;
		if (Pointer == TEXT("/"))
		{
			return Current;
		}
		TArray<FString> Tokens;
		Pointer.RightChop(1).ParseIntoArray(Tokens, TEXT("/"), false);
		for (FString Token : Tokens)
		{
			Token.ReplaceInline(TEXT("~1"), TEXT("/"));
			Token.ReplaceInline(TEXT("~0"), TEXT("~"));
			if (Current->GetKind() == EUeAgentValueKind::Object)
			{
				const FUeAgentCanonicalValueRef* Field =
					FindObjectField(Current.ToSharedRef(), Token);
				if (Field == nullptr)
				{
					return nullptr;
				}
				Current = *Field;
			}
			else if (Current->GetKind() == EUeAgentValueKind::Array)
			{
				const int32 Index = FCString::Atoi(*Token);
				if (!Current->GetArrayItems().IsValidIndex(Index))
				{
					return nullptr;
				}
				Current = Current->GetArrayItems()[Index];
			}
			else
			{
				return nullptr;
			}
		}
		return Current;
	}

	static TSharedPtr<FJsonObject> FindEvidenceCase(
		const TSharedPtr<FJsonObject>& Fixture,
		const FString& CaseId)
	{
		for (const TSharedPtr<FJsonValue>& CaseValue :
			Fixture->GetArrayField(TEXT("evidence_cases")))
		{
			const TSharedPtr<FJsonObject> Case = CaseValue->AsObject();
			if (Case->GetStringField(TEXT("case_id")) == CaseId)
			{
				return Case;
			}
		}
		return nullptr;
	}

	static bool MatchesEvidenceCase(
		FAutomationTestBase* Test,
		const FUeAgentCanonicalValuePtr& Actual,
		const TSharedPtr<FJsonObject>& Fixture,
		const FString& CaseId)
	{
		const TSharedPtr<FJsonObject> Case =
			FindEvidenceCase(Fixture, CaseId);
		if (!Test->TestTrue(
			*FString::Printf(TEXT("fixture case %s exists"), *CaseId),
			Case.IsValid()))
		{
			return false;
		}
		if (!CanonicalMatchesJson(
			Test,
			Actual.ToSharedRef(),
			Case->TryGetField(TEXT("expected_data")),
			TEXT("/")))
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& KindRow :
			Case->GetObjectField(TEXT("expected_kinds"))->Values)
		{
			const FUeAgentCanonicalValuePtr At =
				CanonicalAt(Actual, KindRow.Key);
			if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s%s exists"),
					*CaseId,
					*KindRow.Key),
				At.IsValid()))
			{
				return false;
			}
			if (!Test->TestEqual(
				*FString::Printf(
					TEXT("%s%s kind"),
					*CaseId,
					*KindRow.Key),
				KindName(At->GetKind()),
				KindRow.Value->AsString()))
			{
				return false;
			}
		}
		return true;
	}

	template <typename EvidenceType>
	static FUeAgentCanonicalValuePtr RequireEvidence(
		FAutomationTestBase* Test,
		const EvidenceType& Source,
		FUeAgentResourceTracker& Tracker)
	{
		FUeAgentCanonicalValuePtr Result;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("canonicalize typed difference evidence"),
			TryCanonicalizeDifferenceEvidence(
				Source,
				Tracker,
				Result,
				Error));
		Test->TestTrue(
			TEXT("typed difference evidence returned"),
			Result.IsValid());
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticDifferenceEvidenceContractTest,
	"UeAgentInterface.Core.StateSemantic.DifferenceEvidence.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticDifferenceEvidenceContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadEvidenceFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	FUeAgentResourceTracker Tracker = MakeEvidenceTracker();
	const FUeAgentCanonicalValueRef CanonicalNull =
		RequireNull(this, Tracker);
	const FUeAgentSemanticDifference Difference(
		EUeAgentDifferenceKind::ValueMismatch,
		TEXT("/value"),
		CanonicalNull,
		FUeAgentCanonicalValuePtr());

	TestTrue(
		TEXT("canonical null is present evidence"),
		Difference.GetLeft().IsValid());
	TestEqual(
		TEXT("canonical null retains null kind"),
		Difference.GetLeft()->GetKind(),
		EUeAgentValueKind::Null);
	TestFalse(
		TEXT("absent evidence remains absent"),
		Difference.GetRight().IsValid());

	FUeAgentCanonicalValuePtr Absent;
	FUeAgentStateSemanticError Error;
	TestTrue(
		TEXT("canonicalize absent evidence"),
		TryCanonicalizeDifferenceEvidence(
			FUeAgentCanonicalValuePtr(),
			Tracker,
			Absent,
			Error));
	TestFalse(TEXT("absent is not canonical null"), Absent.IsValid());

	FUeAgentCanonicalValuePtr Preserved;
	TestTrue(
		TEXT("canonicalize existing canonical value"),
		TryCanonicalizeDifferenceEvidence(
			FUeAgentCanonicalValuePtr(CanonicalNull),
			Tracker,
			Preserved,
			Error));
	TestTrue(
		TEXT("existing canonical value is not copied"),
		Preserved.Get() == &CanonicalNull.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticDifferenceEvidenceTypedShapeTest,
	"UeAgentInterface.Core.StateSemantic.DifferenceEvidence.TypedShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticDifferenceEvidenceTypedShapeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadEvidenceFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	FUeAgentResourceTracker SourceTracker = MakeEvidenceTracker();
	const FUeAgentDefaultValue Default = MakeDefault(this, SourceTracker);
	const FUeAgentStateEntry Entry = MakeEntry(this, SourceTracker);

	struct FTypedCase
	{
		FString CaseId;
		FString ExpectedType;
		TFunction<FUeAgentCanonicalValuePtr(FUeAgentResourceTracker&)> Build;
	};
	const TArray<FTypedCase> Cases = {
		{
			TEXT("canonical_identity"),
			TEXT("canonical_identity"),
			[this](FUeAgentResourceTracker& Tracker)
			{
				return RequireEvidence(this, MakeIdentity(), Tracker);
			},
		},
		{
			TEXT("canonical_type"),
			TEXT("canonical_type"),
			[this](FUeAgentResourceTracker& Tracker)
			{
				return RequireEvidence(this, MakeType(), Tracker);
			},
		},
		{
			TEXT("default_value"),
			TEXT("default_value"),
			[this, &Default](FUeAgentResourceTracker& Tracker)
			{
				return RequireEvidence(this, Default, Tracker);
			},
		},
		{
			TEXT("state_details"),
			TEXT("state_details"),
			[this](FUeAgentResourceTracker& Tracker)
			{
				return RequireEvidence(this, MakeDetails(), Tracker);
			},
		},
		{
			TEXT("state_revision"),
			TEXT("state_revision"),
			[this](FUeAgentResourceTracker& Tracker)
			{
				return RequireEvidence(this, MakeRevision(), Tracker);
			},
		},
		{
			TEXT("temporal_point"),
			TEXT("temporal_point"),
			[this](FUeAgentResourceTracker& Tracker)
			{
				return RequireEvidence(this, MakeTemporal(), Tracker);
			},
		},
		{
			TEXT("spatial_frame"),
			TEXT("spatial_frame"),
			[this](FUeAgentResourceTracker& Tracker)
			{
				return RequireEvidence(this, MakeSpatialFrame(), Tracker);
			},
		},
		{
			TEXT("spatial_value"),
			TEXT("spatial_value"),
			[this](FUeAgentResourceTracker& Tracker)
			{
				return RequireEvidence(this, MakeSpatialValue(), Tracker);
			},
		},
		{
			TEXT("state_entry"),
			TEXT("state_entry"),
			[this, &Entry](FUeAgentResourceTracker& Tracker)
			{
				return RequireEvidence(this, Entry, Tracker);
			},
		},
		{
			TEXT("state_plane"),
			TEXT("state_plane"),
			[this](FUeAgentResourceTracker& Tracker)
			{
				return RequireEvidence(
					this,
					EUeAgentStatePlane::AuthoringMemory,
					Tracker);
			},
		},
	};

	for (const FTypedCase& Case : Cases)
	{
		FUeAgentResourceTracker Tracker = MakeEvidenceTracker();
		const FUeAgentCanonicalValuePtr Evidence = Case.Build(Tracker);
		TestEqual(
			FString::Printf(
				TEXT("%s has fixed canonical type tag"),
				*Case.ExpectedType),
			EvidenceTypeTag(this, Evidence),
			Case.ExpectedType);
		if (!MatchesEvidenceCase(
			this,
			Evidence,
			Fixture,
			Case.CaseId))
		{
			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticDifferenceEvidenceCompareTest,
	"UeAgentInterface.Core.StateSemantic.DifferenceEvidence.Compare",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticDifferenceEvidenceCompareTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker Tracker = MakeEvidenceTracker();
	const FUeAgentCanonicalValueRef Left = RequireDecimal(
		this,
		TEXT("1234567890.12345678901234567890123456789"),
		Tracker);
	const FUeAgentCanonicalValueRef Right = RequireDecimal(
		this,
		TEXT("1234567890.12345678901234567890123456788"),
		Tracker);

	const FUeAgentSemanticComparison Comparison = CompareValues(
		Left,
		Right,
		TEXT("/value"),
		Tracker);
	TestFalse(TEXT("different values are not equivalent"), Comparison.IsEquivalent());
	TestEqual(
		TEXT("one value difference"),
		Comparison.GetDifferences().Num(),
		1);
	const FUeAgentSemanticDifference& Difference =
		Comparison.GetDifferences()[0];
	TestTrue(
		TEXT("left comparison evidence is original canonical value"),
		Difference.GetLeft().Get() == &Left.Get());
	TestTrue(
		TEXT("right comparison evidence is original canonical value"),
		Difference.GetRight().Get() == &Right.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticDifferenceEvidenceDepthTest,
	"UeAgentInterface.Core.StateSemantic.DifferenceEvidence.Depth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticDifferenceEvidenceDepthTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 Depth = 1536;
	FUeAgentResourceTracker BuildTracker = MakeEvidenceTracker(Depth + 8);
	FUeAgentCanonicalValuePtr Current = RequireNull(this, BuildTracker);
	FUeAgentStateSemanticError Error;
	for (int32 Index = 0; Index < Depth; ++Index)
	{
		FUeAgentCanonicalValuePtr Parent;
		if (!TestTrue(
			TEXT("build deep canonical evidence"),
			FUeAgentCanonicalValue::TryArray(
				{Current.ToSharedRef()},
				BuildTracker,
				Parent,
				Error)))
		{
			return false;
		}
		Current = MoveTemp(Parent);
	}

	FUeAgentResourceTracker NormalizeTracker = MakeEvidenceTracker(1);
	FUeAgentCanonicalValuePtr Normalized;
	TestTrue(
		TEXT("deep canonical evidence has no depth policy"),
		TryCanonicalizeDifferenceEvidence(
			Current,
			NormalizeTracker,
			Normalized,
			Error));
	TestTrue(
		TEXT("deep canonical evidence is preserved without copying"),
		Normalized.Get() == Current.Get());

	int32 ObservedDepth = 0;
	FUeAgentCanonicalValuePtr Cursor = Normalized;
	while (
		Cursor.IsValid()
		&& Cursor->GetKind() == EUeAgentValueKind::Array)
	{
		if (!TestEqual(
			TEXT("deep array node has one child"),
			Cursor->GetArrayItems().Num(),
			1))
		{
			return false;
		}
		Cursor = Cursor->GetArrayItems()[0];
		++ObservedDepth;
	}
	TestEqual(TEXT("all deep nodes preserved"), ObservedDepth, Depth);
	TestTrue(TEXT("deep leaf exists"), Cursor.IsValid());
	TestEqual(
		TEXT("deep leaf remains canonical null"),
		Cursor->GetKind(),
		EUeAgentValueKind::Null);
	return true;
}



IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentCanonicalAuthoringDescriptorTest,
 "UeAgentInterface.Core.StateSemantic.AuthoringDescriptor",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentCanonicalAuthoringDescriptorTest::RunTest(const FString&)
{
 auto Resources = MakeEvidenceTracker(); FUeAgentStateSemanticError Error;
 auto Manual = MakeEntry(this, Resources); FUeAgentCanonicalValuePtr Descriptor, Kind;
 FUeAgentCanonicalValue::TryString(TEXT("enum"), Resources, Kind, Error);
 FUeAgentCanonicalValue::TryObject({{TEXT("kind"), Kind.ToSharedRef()}}, Resources, Descriptor, Error);
 FUeAgentStateEntry Rich(Manual.GetIdentity(), Manual.GetType(), Manual.GetValue(), Manual.GetDefault(),
  Manual.GetDetails(), Manual.GetObservedRevision(), Manual.GetAuthoredPresence(), Manual.GetOverrideState(), Descriptor);
 TestTrue(TEXT("descriptor entry remains valid"), Rich.IsValid());
 TestTrue(TEXT("revision clone preserves immutable descriptor"), Rich.WithObservedRevision(MakeRevision()).GetAuthoringDescriptor() == Descriptor);
 TestFalse(TEXT("manual field does not acquire invented metadata"), Manual.GetAuthoringDescriptor().IsValid());
 FUeAgentCanonicalValuePtr Evidence;
 if (!TestTrue(TEXT("descriptor participates in evidence"), TryCanonicalizeDifferenceEvidence(Rich, Resources, Evidence, Error))) return false;
 TestNotNull(TEXT("state entry evidence contains descriptor"), FindObjectField(Evidence.ToSharedRef(), TEXT("authoring_descriptor")));
 auto Snapshot = [&](const FUeAgentStateEntry& Entry, FUeAgentCanonicalSnapshotPtr& Out) {
  return FUeAgentCanonicalSnapshot::TryCreate(TEXT("descriptor-test"), UE::UAI::Contract::EUeAgentStatePlane::AuthoringMemory,
   MakeRevision(), FUeAgentTemporalPoint(TEXT("capture"), TEXT("0")),
   FUeAgentStateProvenance(TEXT("independent_readback"), TEXT("descriptor-test"), TEXT("evidence")), {Entry}, Resources, Out, Error);
 };
 FUeAgentCanonicalSnapshotPtr Left, Right; FUeAgentSemanticComparison Comparison;
 if (!Snapshot(Manual, Left) || !Snapshot(Rich, Right)
  || !TryCompareSnapshots(Left.ToSharedRef(), Right.ToSharedRef(), Resources, Comparison, Error)) { AddError(Error.Code); return false; }
 TestFalse(TEXT("descriptor change is part of snapshot comparison"), Comparison.IsEquivalent());
 return !HasAnyErrors();
}
