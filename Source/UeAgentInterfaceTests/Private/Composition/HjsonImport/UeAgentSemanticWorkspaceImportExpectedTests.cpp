#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"

namespace
{
	using namespace UE::UAI::Composition::HjsonImport;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeImportTracker()
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 32768},
			{EUeAgentResourceDimension::Items, 32768},
			{EUeAgentResourceDimension::Files, 128},
			{EUeAgentResourceDimension::ReferenceEdges, 32768},
			{EUeAgentResourceDimension::WorkUnits, 262144},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 8 * 1024 * 1024},
		}));
	}

	FUeAgentCanonicalValueRef StringValue(
		const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Text,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	FAuthoringModelRef MakeImportModel(
		FUeAgentResourceTracker& Resources,
		const FString& ValueA,
		const FString& ValueB)
	{
		const FString FieldId = TEXT("shared.message");
		TArray<FSemanticUnit> Units;
		Units.Emplace(
			TEXT("target:blueprint"),
			ESemanticUnitKind::TargetState,
			TEXT(""),
			TEXT("ue.blueprint.asset"),
			TEXT("authoring_target"),
			TEXT(""),
			TMap<FString, FUeAgentCanonicalValueRef>(),
			TArray<FString>({TEXT("node:a"), TEXT("node:b")}),
			TArray<FString>(),
			TEXT(""),
			TEXT(""),
			TArray<FString>(),
			TArray<FString>({TEXT("state")}),
			TEXT("fixture:hx013"),
			TEXT(
				"blueprint_asset|ue.blueprint.asset|asset_path|"
				"asset_path=%2FGame%2FHX013%2FBP_Import"));
		Units.Emplace(
			TEXT("node:a"),
			ESemanticUnitKind::ItemState,
			TEXT("target:blueprint"),
			TEXT("uai.semantic.blueprint.node"),
			TEXT("graph_collection"),
			TEXT(""),
			TMap<FString, FUeAgentCanonicalValueRef>(),
			TArray<FString>({TEXT("properties:a")}),
			TArray<FString>(),
			TEXT(""),
			TEXT(""),
			TArray<FString>(),
			TArray<FString>({TEXT("state")}),
			TEXT("fixture:hx013"),
			TEXT(
				"blueprint_node|blueprint_node|node_guid|"
				"node_guid=00000000000000000000000000000001"));
		Units.Emplace(
			TEXT("node:b"),
			ESemanticUnitKind::ItemState,
			TEXT("target:blueprint"),
			TEXT("uai.semantic.blueprint.node"),
			TEXT("graph_collection"),
			TEXT(""),
			TMap<FString, FUeAgentCanonicalValueRef>(),
			TArray<FString>({TEXT("properties:b")}),
			TArray<FString>(),
			TEXT(""),
			TEXT(""),
			TArray<FString>(),
			TArray<FString>({TEXT("state")}),
			TEXT("fixture:hx013"),
			TEXT(
				"blueprint_node|blueprint_node|node_guid|"
				"node_guid=00000000000000000000000000000002"));
		Units.Emplace(
			TEXT("properties:a"),
			ESemanticUnitKind::PropertySet,
			TEXT("node:a"),
			TEXT("uai.semantic.blueprint.node.property"),
			TEXT("node_properties"),
			FieldId,
			TMap<FString, FUeAgentCanonicalValueRef>({
				{FieldId, StringValue(ValueA, Resources)}}),
			TArray<FString>(),
			TArray<FString>(),
			TEXT(""),
			TEXT(""),
			TArray<FString>(),
			TArray<FString>({TEXT("state")}),
			TEXT("fixture:hx013"));
		Units.Emplace(
			TEXT("properties:b"),
			ESemanticUnitKind::PropertySet,
			TEXT("node:b"),
			TEXT("uai.semantic.blueprint.node.property"),
			TEXT("node_properties"),
			FieldId,
			TMap<FString, FUeAgentCanonicalValueRef>({
				{FieldId, StringValue(ValueB, Resources)}}),
			TArray<FString>(),
			TArray<FString>(),
			TEXT(""),
			TEXT(""),
			TArray<FString>(),
			TArray<FString>({TEXT("state")}),
			TEXT("fixture:hx013"));
		FAuthoringModelPtr Model;
		FWorkspaceError Error;
		check(TryCompileAuthoringModel(
			TEXT("uai.authoring.composition/2"),
			2,
			TEXT("llm_authoring"),
			TEXT("generation:hx013"),
			MoveTemp(Units),
			{TEXT("target:blueprint")},
			Resources,
			Model,
			Error));
		return Model.ToSharedRef();
	}

	FUeAgentCanonicalSnapshotRef MakeImportSnapshot(
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentStateRevision Revision(
			TEXT("hx013:persisted"),
			TEXT("generation:hx013"),
			13);
		TArray<FUeAgentStateEntry> Entries;
		for (const TPair<FString, FString>& Row : {
			TPair<FString, FString>(TEXT("node:a"), TEXT("before-a")),
			TPair<FString, FString>(TEXT("node:b"), TEXT("before-b"))})
		{
			const FUeAgentCanonicalValueRef Value =
				StringValue(Row.Value, Resources);
			Entries.Emplace(
				FUeAgentCanonicalIdentity(
					TEXT("shared.message"),
					TEXT("authoring_field"),
					Row.Key,
					TEXT("shared.message"),
					TEXT("blueprint_node"),
					TEXT("generation:hx013")),
				FUeAgentCanonicalType(
					TEXT("string"),
					EUeAgentValueKind::String,
					false,
					TEXT("scalar")),
				Value,
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::Schema,
					TEXT("hx013:string-default"),
					TEXT("fixture:hx013"),
					StringValue(TEXT(""), Resources)),
				FUeAgentStateDetails(
					TEXT("Shared message"),
					TEXT(""),
					{TEXT("HX-013")},
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
			TEXT("snapshot:hx013:import"),
			EUeAgentStatePlane::Persisted,
			Revision,
			FUeAgentTemporalPoint(TEXT("fixture"), TEXT("13")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("fixture:hx013"),
				TEXT("evidence:hx013:persisted")),
			Entries,
			Resources,
			Snapshot,
			Error));
		return Snapshot.ToSharedRef();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticWorkspaceImportModuleContractTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.ModuleContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportModuleContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::Composition::HjsonImport;

	TestEqual(
		TEXT("state, seed and edit bundle share one workspace schema"),
		GetSemanticWorkspaceSchemaId(),
		FString(TEXT("uai.authoring.workspace/2")));
	TestEqual(
		TEXT("state mode has one stable spelling"),
		LexToString(ESemanticWorkspaceMode::State),
		FString(TEXT("state")));
	TestEqual(
		TEXT("seed mode has one stable spelling"),
		LexToString(ESemanticWorkspaceMode::Seed),
		FString(TEXT("seed")));
	TestEqual(
		TEXT("edit bundle mode has one stable spelling"),
		LexToString(ESemanticWorkspaceMode::EditBundle),
		FString(TEXT("edit_bundle")));
	TestTrue(
		TEXT("resolved semantic workspace is a native C5 contract"),
		TIsClass<FResolvedSemanticWorkspace>::Value);
	TestTrue(
		TEXT("structural edit is explicit and typed"),
		TIsClass<FStructuralEdit>::Value);
	TestTrue(
		TEXT("prepared import is a composition contract"),
		TIsClass<FPreparedAuthoringImport>::Value);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticWorkspaceImportChangeSetReassemblyTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.ChangeSetReassembly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportChangeSetReassemblyTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::Composition::HjsonImport;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FAuthoringModelRef CurrentModel =
		MakeImportModel(Resources, TEXT("before-a"), TEXT("before-b"));
	const FAuthoringModelRef EditedModel =
		MakeImportModel(Resources, TEXT("after-a"), TEXT("before-b"));
	const FUeAgentCanonicalSnapshotRef Snapshot =
		MakeImportSnapshot(Resources);
	const FResolvedSemanticWorkspaceRef Workspace =
		MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory,
			ESemanticWorkspaceMode::State,
			EditedModel,
			Snapshot->GetRevision(),
			TArray<FStructuralEdit>(),
			TArray<FString>({TEXT("units/property/properties-a.hjson")}),
			TEXT(""),
			TEXT(""),
			TArray<FString>(),
			EditedModel->GetSemanticFingerprint());
	TOptional<FChangeSetDraft> Draft;
	FSemanticWorkspaceImportError Error;
	TestTrue(
		TEXT("semantic property units reassemble into one typed ChangeSet"),
		TryBuildSemanticChangeSetDraft(
			Workspace,
			Snapshot,
			FUeAgentDescriptorIdentity(
				TEXT("uai.hjson.semantic.import"),
				1,
				TEXT(
					"sha256:1111111111111111111111111111111111111111111111111111111111111111")),
			TEXT("hx013:change-set"),
			TEXT("hx013:change-set:1"),
			Resources,
			Draft,
			Error));
	TestTrue(TEXT("ChangeSet is present"), Draft.IsSet());
	if (Draft.IsSet())
	{
		TestEqual(
			TEXT("unchanged sibling with the same field id is omitted"),
			Draft->GetChanges().Num(),
			1);
		if (Draft->GetChanges().Num() == 1)
		{
			const FChangeDraft& Change = Draft->GetChanges()[0];
			TestEqual(
				TEXT("owner disambiguates duplicate field id"),
				Change.GetIdentity().GetOwnerId(),
				FString(TEXT("node:a")));
			TestEqual(
				TEXT("edited value is preserved"),
				Change.GetProposedAfter()->GetText(),
				FString(TEXT("after-a")));
		}
	}

	TOptional<FChangeSetDraft> NoOpDraft;
	FSemanticWorkspaceImportError NoOpError;
	const FResolvedSemanticWorkspaceRef NoOpWorkspace =
		MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory,
			ESemanticWorkspaceMode::State,
			CurrentModel,
			Snapshot->GetRevision(),
			TArray<FStructuralEdit>(),
			TArray<FString>({TEXT("units/property/properties-a.hjson")}),
			TEXT(""),
			TEXT(""),
			TArray<FString>(),
			CurrentModel->GetSemanticFingerprint());
	TestFalse(
		TEXT("omission and unchanged values are not mutations"),
		TryBuildSemanticChangeSetDraft(
			NoOpWorkspace,
			Snapshot,
			FUeAgentDescriptorIdentity(
				TEXT("uai.hjson.semantic.import"),
				1,
				TEXT(
					"sha256:1111111111111111111111111111111111111111111111111111111111111111")),
			TEXT("hx013:no-op"),
			TEXT("hx013:no-op:1"),
			Resources,
			NoOpDraft,
			NoOpError));
	TestEqual(
		TEXT("no-op failure is explicit"),
		NoOpError.Code,
		FString(TEXT("hjson_import_no_explicit_changes")));
	TestFalse(TEXT("failed no-op has no draft"), NoOpDraft.IsSet());
	return !HasAnyErrors();
}
