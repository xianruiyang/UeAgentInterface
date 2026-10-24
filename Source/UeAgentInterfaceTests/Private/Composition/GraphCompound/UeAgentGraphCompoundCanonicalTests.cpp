#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundCanonical.h"

namespace
{
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeTracker()
	{
		return FUeAgentResourceTracker();
	}

	FUeAgentCanonicalIdentity MakeIdentity(
		const FString& SemanticId,
		const FString& OwnerId)
	{
		return FUeAgentCanonicalIdentity(
			SemanticId,
			TEXT("graph_element"),
			OwnerId,
			SemanticId,
			TEXT("graph_compound"),
			TEXT("generation:gc014"));
	}

	FUeAgentCanonicalValuePtr MakeString(
		const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(Text, Resources, Value, Error));
		return Value;
	}

	FUeAgentCanonicalSnapshotRef MakeSnapshot(
		FUeAgentResourceTracker& Resources,
		const TArray<FUeAgentCanonicalIdentity>& Identities)
	{
		const FUeAgentStateRevision Revision(
			TEXT("gc014:persisted"),
			TEXT("generation:gc014"),
			14);
		TArray<FUeAgentStateEntry> Entries;
		for (const FUeAgentCanonicalIdentity& Identity : Identities)
		{
			Entries.Emplace(
				Identity,
				FUeAgentCanonicalType(
					TEXT("string"),
					EUeAgentValueKind::String,
					false,
					TEXT("scalar")),
				MakeString(TEXT("before"), Resources).ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::Schema,
					TEXT("gc014:string-default"),
					TEXT("fixture:gc014"),
					MakeString(TEXT(""), Resources).ToSharedRef()),
				FUeAgentStateDetails(
					TEXT("Graph element"),
					TEXT(""),
					{TEXT("GC-014")},
					true,
					TEXT("available"),
					{}),
				Revision,
				EUeAgentAuthoredPresence::Present,
				EUeAgentOverrideState::Overridden);
		}

		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalSnapshot::TryCreate(
			TEXT("snapshot:gc014"),
			EUeAgentStatePlane::Persisted,
			Revision,
			FUeAgentTemporalPoint(TEXT("fixture"), TEXT("14")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("fixture:gc014"),
				TEXT("evidence:gc014:persisted")),
			Entries,
			Resources,
			Snapshot,
			Error));
		return Snapshot.ToSharedRef();
	}

	FUeAgentDescriptorIdentity MakeDescriptor()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.graph_compound.fixture"),
			1,
			TEXT(
				"sha256:1414141414141414141414141414141414141414141414141414141414141414"));
	}

	FGraphCompoundCanonicalEdit MakeCreate(
		const FUeAgentCanonicalIdentity& Identity,
		const TArray<FUeAgentCanonicalIdentity>& Dependencies,
		FUeAgentResourceTracker& Resources)
	{
		return FGraphCompoundCanonicalEdit(
			Identity,
			EGraphCompoundElementKind::Item,
			EChangeIntent::Create,
			MakeString(Identity.GetSemanticId(), Resources),
			Dependencies,
			{TEXT("graph.write")},
			TEXT("evidence:gc014:create"),
			Dependencies);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundCanonicalModuleContractTest,
	"UeAgentInterface.Composition.GraphCompound.Canonical.ModuleContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundCanonicalModuleContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TestEqual(
		TEXT("all graph-like domains share one canonical semantic schema"),
		GetGraphCompoundCanonicalSchemaId(),
		FString(TEXT("uai.graph_compound.canonical.v1")));
	TestEqual(
		TEXT("property has one stable spelling"),
		LexToString(EGraphCompoundElementKind::Property),
		FString(TEXT("property")));
	TestEqual(
		TEXT("relation has one stable spelling"),
		LexToString(EGraphCompoundElementKind::Relation),
		FString(TEXT("relation")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundCanonicalDraftTest,
	"UeAgentInterface.Composition.GraphCompound.Canonical.ChangeSetDraft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundCanonicalDraftTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeTracker();
	const FUeAgentCanonicalIdentity Existing =
		MakeIdentity(TEXT("property:existing"), TEXT("node:existing"));
	const FUeAgentCanonicalIdentity Item =
		MakeIdentity(TEXT("item:new"), TEXT("graph:fixture"));
	const FUeAgentCanonicalIdentity Relation =
		MakeIdentity(TEXT("relation:new"), TEXT("graph:fixture"));
	const FUeAgentCanonicalSnapshotRef Snapshot =
		MakeSnapshot(Resources, {Existing});

	FGraphCompoundCanonicalPayload Payload(
		GetGraphCompoundCanonicalSchemaId(),
		{
			FGraphCompoundCanonicalEdit(
				Existing,
				EGraphCompoundElementKind::Property,
				EChangeIntent::Update,
				MakeString(TEXT("after"), Resources),
				{},
				{TEXT("graph.write")},
				TEXT("evidence:gc014:update"),
				{Existing}),
			MakeCreate(Item, {}, Resources),
			FGraphCompoundCanonicalEdit(
				Relation,
				EGraphCompoundElementKind::Relation,
				EChangeIntent::Create,
				MakeString(TEXT("item:new->property:existing"), Resources),
				{Item, Existing},
				{TEXT("graph.write")},
				TEXT("evidence:gc014:relation"),
				{Item, Existing}),
		});
	TOptional<FChangeSetDraft> Draft;
	FGraphCompoundCanonicalError Error;
	TestTrue(
		TEXT("typed canonical edits build one C6 draft"),
		TryBuildGraphCompoundChangeSetDraft(
			Payload,
			Snapshot,
			MakeDescriptor(),
			TEXT("request:gc014"),
			TEXT("idempotency:gc014"),
			Resources,
			Draft,
			Error));
	TestTrue(TEXT("draft is present"), Draft.IsSet());
	if (Draft.IsSet())
	{
		TestEqual(
			TEXT("one draft change is produced per canonical identity"),
			Draft->GetChanges().Num(),
			3);
		TestEqual(
			TEXT("expected-before comes from the independent C4 snapshot"),
			Draft->GetChanges()[0].GetExpectedBefore()->GetText(),
			FString(TEXT("before")));
		TestEqual(
			TEXT("proposed-after remains the field value, not a whole command payload"),
			Draft->GetChanges()[0].GetProposedAfter()->GetKind(),
			EUeAgentValueKind::String);
		TestEqual(
			TEXT("the exact snapshot revision is retained"),
			Draft->GetSnapshot()->GetRevision().GetSequence(),
			int64(14));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundCanonicalValidationTest,
	"UeAgentInterface.Composition.GraphCompound.Canonical.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundCanonicalValidationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeTracker();
	const FUeAgentCanonicalIdentity Item =
		MakeIdentity(TEXT("item:duplicate"), TEXT("graph:fixture"));
	const FUeAgentCanonicalSnapshotRef Snapshot = MakeSnapshot(Resources, {});

	FGraphCompoundCanonicalError DuplicateError;
	TestFalse(
		TEXT("duplicate canonical identity is rejected"),
		TryValidateGraphCompoundCanonicalPayload(
			FGraphCompoundCanonicalPayload(
				GetGraphCompoundCanonicalSchemaId(),
				{MakeCreate(Item, {}, Resources), MakeCreate(Item, {}, Resources)}),
			Snapshot,
			Resources,
			DuplicateError));
	TestEqual(
		TEXT("duplicate failure is explicit"),
		DuplicateError.Code,
		FString(TEXT("graph_compound_duplicate_identity")));

	const FUeAgentCanonicalIdentity Missing =
		MakeIdentity(TEXT("item:missing"), TEXT("graph:fixture"));
	FGraphCompoundCanonicalError MissingError;
	TestFalse(
		TEXT("dependency absent from both payload and snapshot is rejected"),
		TryValidateGraphCompoundCanonicalPayload(
			FGraphCompoundCanonicalPayload(
				GetGraphCompoundCanonicalSchemaId(),
				{MakeCreate(Item, {Missing}, Resources)}),
			Snapshot,
			Resources,
			MissingError));
	TestEqual(
		TEXT("missing dependency failure is explicit"),
		MissingError.Code,
		FString(TEXT("graph_compound_dependency_missing")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundCanonicalUnboundedHierarchyTest,
	"UeAgentInterface.Composition.GraphCompound.Canonical.UnboundedHierarchy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundCanonicalUnboundedHierarchyTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Snapshot = MakeSnapshot(Resources, {});
	TArray<FGraphCompoundCanonicalEdit> DeepEdits;
	TOptional<FUeAgentCanonicalIdentity> Parent;
	for (int32 Index = 0; Index < 512; ++Index)
	{
		const FUeAgentCanonicalIdentity Identity = MakeIdentity(
			FString::Printf(TEXT("hierarchy:%d"), Index),
			TEXT("graph:deep"));
		DeepEdits.Add(FGraphCompoundCanonicalEdit(
			Identity,
			EGraphCompoundElementKind::Hierarchy,
			EChangeIntent::Create,
			MakeString(FString::FromInt(Index), Resources),
			Parent.IsSet()
				? TArray<FUeAgentCanonicalIdentity>({Parent.GetValue()})
				: TArray<FUeAgentCanonicalIdentity>(),
			{TEXT("graph.write")},
			TEXT("evidence:gc014:deep"),
			{}));
		Parent = Identity;
	}
	FGraphCompoundCanonicalError DeepError;
	TestTrue(
		TEXT("a deep acyclic hierarchy is bounded by resources, not fixed depth"),
		TryValidateGraphCompoundCanonicalPayload(
			FGraphCompoundCanonicalPayload(
				GetGraphCompoundCanonicalSchemaId(),
				MoveTemp(DeepEdits)),
			Snapshot,
			Resources,
			DeepError));

	const FUeAgentCanonicalIdentity A =
		MakeIdentity(TEXT("hierarchy:a"), TEXT("graph:cycle"));
	const FUeAgentCanonicalIdentity B =
		MakeIdentity(TEXT("hierarchy:b"), TEXT("graph:cycle"));
	FGraphCompoundCanonicalError CycleError;
	TestFalse(
		TEXT("a dependency cycle is rejected without recursive traversal"),
		TryValidateGraphCompoundCanonicalPayload(
			FGraphCompoundCanonicalPayload(
				GetGraphCompoundCanonicalSchemaId(),
				{MakeCreate(A, {B}, Resources), MakeCreate(B, {A}, Resources)}),
			Snapshot,
			Resources,
			CycleError));
	TestEqual(
		TEXT("cycle failure is explicit"),
		CycleError.Code,
		FString(TEXT("graph_compound_dependency_cycle")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundExecutionContractTest,
	"UeAgentInterface.Composition.GraphCompound.Canonical.ExecutionContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundExecutionContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FGraphCompoundExecutionContract Query(
		MakeDescriptor(),
		EGraphCompoundExecutionLane::Query,
		TEXT("graph_compound"),
		TEXT("provider:fixture"),
		TEXT("request:fixture.query.v1"),
		TEXT("result:fixture.query.v1"),
		TEXT(""),
		TEXT(""),
		TEXT("readback:fixture"),
		TEXT(""),
		TEXT("canonical_snapshot"),
		false,
		false);
	TestTrue(TEXT("query contract owns only C3Q readback"), Query.IsValid());
	TestEqual(
		TEXT("query lane has one stable spelling"),
		LexToString(Query.GetLane()),
		FString(TEXT("c3q")));

	const FGraphCompoundExecutionContract Operation(
		MakeDescriptor(),
		EGraphCompoundExecutionLane::Operation,
		TEXT("graph_compound"),
		TEXT("binding:fixture"),
		TEXT("request:fixture.operation.v1"),
		TEXT("result:fixture.operation.v1"),
		TEXT("graph_compound.change"),
		TEXT("native:fixture"),
		TEXT("readback:fixture"),
		TEXT("compile_and_settle"),
		TEXT("canonical_state_compare"),
		true,
		true);
	TestTrue(TEXT("operation contract owns the C3O to C6 chain"), Operation.IsValid());

	const FGraphCompoundExecutionContract InvalidQuery(
		MakeDescriptor(),
		EGraphCompoundExecutionLane::Query,
		TEXT("graph_compound"),
		TEXT("provider:fixture"),
		TEXT("request:fixture.query.v1"),
		TEXT("result:fixture.query.v1"),
		TEXT("graph_compound.change"),
		TEXT("native:fixture"),
		TEXT("readback:fixture"),
		TEXT("compile_and_settle"),
		TEXT("canonical_snapshot"),
		true,
		false);
	TestFalse(
		TEXT("query cannot silently claim mutation ownership"),
		InvalidQuery.IsValid());
	return !HasAnyErrors();
}
