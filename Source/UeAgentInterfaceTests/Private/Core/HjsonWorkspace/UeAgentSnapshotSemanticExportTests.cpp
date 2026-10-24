#include "Misc/AutomationTest.h"

#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentSnapshotComposition.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Containers/StringConv.h"

namespace
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeSnapshotExportTracker()
	{
		return FUeAgentResourceTracker();
	}

	FUeAgentStateEntry MakeSnapshotField(
		const FString& SemanticId,
		const FString& OwnerId,
		TMap<FString, FString> Selectors,
		FUeAgentCanonicalValueRef Value,
		FUeAgentCanonicalValueRef Default,
		const FUeAgentStateRevision& Revision)
	{
		return FUeAgentStateEntry(
			FUeAgentCanonicalIdentity(
				SemanticId,
				TEXT("ue.blueprint.asset"),
				OwnerId,
				MoveTemp(Selectors),
				TEXT("state"),
				Revision.GetGeneration()),
			FUeAgentCanonicalType(
				TEXT("fixture.structured_value.v1"),
				Value->GetKind(),
				false,
				Value->GetKind() == EUeAgentValueKind::Object ? TEXT("object")
					: Value->GetKind() == EUeAgentValueKind::Array ? TEXT("array") : TEXT("scalar")),
			Value,
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::DomainNative,
				TEXT("fixture-native-default"),
				TEXT("fixture-independent-readback"),
				Default),
			FUeAgentStateDetails(
				SemanticId,
				TEXT("RE-008 semantic export fixture"),
				{TEXT("Fixture")},
				true,
				TEXT("available"),
				{}),
			Revision,
			EUeAgentAuthoredPresence::Present,
			EUeAgentOverrideState::Overridden);
	}

	bool ContainsText(
		const FExportWorkspacePlan& Plan,
		const FString& Needle)
	{
		for (const FExportFilePlan& File : Plan.GetFiles())
		{
			const TArray<uint8>& Bytes = File.GetCanonicalUtf8();
			const FUTF8ToTCHAR Converted(
				reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
				Bytes.Num());
			const FString Text(
				Converted.Length(),
				Converted.Get());
			if (Text.Contains(Needle))
			{
				return true;
			}
		}
		return false;
	}

	bool ContainsRole(
		const FAuthoringModel& Model,
		const FString& Role)
	{
		return Model.GetUnits().ContainsByPredicate(
			[&Role](const FSemanticUnit& Unit)
			{
				return Unit.GetRole() == Role;
			});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSnapshotSemanticExportTest,
	"UeAgentInterface.Core.HjsonWorkspace.SnapshotComposition."
		"C4ToExportFieldToC8F",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSnapshotSemanticExportTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker Resources = MakeSnapshotExportTracker();
	FUeAgentStateSemanticError StateError;
	FUeAgentCanonicalValuePtr BoolValue;
	FUeAgentCanonicalValuePtr BoolDefault;
	TestTrue(
		TEXT("create authored boolean"),
		FUeAgentCanonicalValue::TryBoolean(
			true,
			Resources,
			BoolValue,
			StateError));
	TestTrue(
		TEXT("create boolean default"),
		FUeAgentCanonicalValue::TryBoolean(
			false,
			Resources,
			BoolDefault,
			StateError));

	FUeAgentCanonicalValuePtr ExactInteger;
	FUeAgentCanonicalValuePtr ExactIntegerDefault;
	FUeAgentCanonicalValuePtr WholeDecimal;
	TestTrue(
		TEXT("create exact integer"),
		FUeAgentCanonicalValue::TryInteger(
			TEXT("9007199254740993"),
			Resources,
			ExactInteger,
			StateError));
	TestTrue(
		TEXT("create exact integer default"),
		FUeAgentCanonicalValue::TryInteger(
			TEXT("0"),
			Resources,
			ExactIntegerDefault,
			StateError));
	TestTrue(
		TEXT("create whole-valued decimal without losing its kind"),
		FUeAgentCanonicalValue::TryDecimal(
			TEXT("320"),
			Resources,
			WholeDecimal,
			StateError));
	const FString RootOwner(TEXT("/Game/RE008/BP_SemanticExport"));
	const FString GraphOwner(
		TEXT("/Game/RE008/BP_SemanticExport:EventGraph"));
	const FString NodeOwner(GraphOwner + TEXT(":K2Node_1"));
	FUeAgentCanonicalValuePtr NodeId;
	FUeAgentCanonicalValuePtr NodeName;
	FUeAgentCanonicalValuePtr NodeValue;
	FUeAgentCanonicalValuePtr LayoutX, LayoutY, LayoutPosition;
	if (!FUeAgentCanonicalValue::TryInteger(TEXT("320"), Resources, LayoutX, StateError)
		|| !FUeAgentCanonicalValue::TryInteger(TEXT("96"), Resources, LayoutY, StateError)
		|| !FUeAgentCanonicalValue::TryArray({LayoutX.ToSharedRef(), LayoutY.ToSharedRef()},
			Resources, LayoutPosition, StateError)) return false;
	TestTrue(TEXT("captured node carries its native identity"),
		FUeAgentCanonicalValue::TryString(TEXT("K2Node_1"), Resources, NodeId, StateError)
		&& FUeAgentCanonicalValue::TryString(TEXT("Fixture Node"), Resources, NodeName, StateError)
		&& FUeAgentCanonicalValue::TryObject({
			{TEXT("id"), NodeId.ToSharedRef()},
			{TEXT("name"), NodeName.ToSharedRef()},
			{TEXT("position"), LayoutPosition.ToSharedRef()}}, Resources, NodeValue, StateError));
	if (!NodeValue.IsValid()) return false;

	const FUeAgentTemporalPoint AuthoredTime(
		TEXT("fixture-timeline"),
		TEXT("24000"),
		TEXT("24000"),
		TEXT("1001"),
		TEXT("authored"));
	FUeAgentCanonicalValuePtr TemporalValue;
	TestTrue(
		TEXT("create typed temporal value"),
		FUeAgentCanonicalValue::TryTemporal(
			AuthoredTime,
			Resources,
			TemporalValue,
			StateError));
	const FUeAgentSpatialFrame AuthoredFrame(
		TEXT("fixture-local"),
		TEXT("fixture-world"),
		3,
		{
			{TEXT("1"), TEXT("0"), TEXT("0")},
			{TEXT("0"), TEXT("1"), TEXT("0")},
			{TEXT("0"), TEXT("0"), TEXT("1")},
		},
		{TEXT("0"), TEXT("0"), TEXT("0")},
		TEXT("cm"),
		TEXT("fixture:declared-frame"),
		AuthoredTime);
	const FUeAgentSpatialValue AuthoredPosition(
		NodeOwner,
		EUeAgentSpatialQuantityKind::Point,
		AuthoredFrame,
		{TEXT("320"), TEXT("-96"), TEXT("0")},
		AuthoredTime,
		{TEXT("fixture:direct")});
	FUeAgentCanonicalValuePtr SpatialValue;
	TestTrue(
		TEXT("create typed spatial value"),
		FUeAgentCanonicalValue::TrySpatial(
			AuthoredPosition,
			Resources,
			SpatialValue,
			StateError));
	if (!BoolValue.IsValid() || !BoolDefault.IsValid()
		|| !ExactInteger.IsValid() || !ExactIntegerDefault.IsValid()
		|| !WholeDecimal.IsValid() || !TemporalValue.IsValid() || !SpatialValue.IsValid())
	{
		AddError(StateError.Code + TEXT(":") + StateError.Path);
		return false;
	}

	const FUeAgentStateRevision Revision(
		TEXT("fixture:content-graph"),
		TEXT("re008"),
		8);
	const TArray<FUeAgentStateEntry> Entries = {
		MakeSnapshotField(
			TEXT("uai.semantic.blueprint.export.target"),
			RootOwner,
			{{TEXT("asset_path"), RootOwner}},
			BoolValue.ToSharedRef(),
			BoolDefault.ToSharedRef(),
			Revision),
		MakeSnapshotField(
			TEXT("uai.semantic.blueprint.graph"),
			GraphOwner,
			{
				{TEXT("asset_path"), RootOwner},
				{TEXT("graph_name"), TEXT("EventGraph")},
			},
			BoolValue.ToSharedRef(),
			BoolDefault.ToSharedRef(),
			Revision),
		MakeSnapshotField(
			TEXT("uai.semantic.blueprint.node"),
			NodeOwner,
			{
				{TEXT("asset_path"), RootOwner},
				{TEXT("graph_name"), TEXT("EventGraph")},
				{TEXT("node_id"), TEXT("K2Node_1")},
			},
			NodeValue.ToSharedRef(),
			NodeValue.ToSharedRef(),
			Revision),
		MakeSnapshotField(
			TEXT("uai.semantic.blueprint.node.property"),
			NodeOwner,
			{
				{TEXT("asset_path"), RootOwner},
				{TEXT("graph_name"), TEXT("EventGraph")},
				{TEXT("node_id"), TEXT("K2Node_1")},
				{TEXT("property_path"), TEXT("InputPriority")},
			},
			ExactInteger.ToSharedRef(),
			ExactIntegerDefault.ToSharedRef(),
			Revision),
		MakeSnapshotField(
			TEXT("uai.semantic.blueprint.node.property"),
			NodeOwner,
			{
				{TEXT("asset_path"), RootOwner},
				{TEXT("graph_name"), TEXT("EventGraph")},
				{TEXT("node_id"), TEXT("K2Node_1")},
				{TEXT("property_path"), TEXT("AuthoredTime")},
			},
			TemporalValue.ToSharedRef(),
			TemporalValue.ToSharedRef(),
			Revision),
		MakeSnapshotField(
			TEXT("uai.semantic.blueprint.node.property"),
			NodeOwner,
			{
				{TEXT("asset_path"), RootOwner},
				{TEXT("graph_name"), TEXT("EventGraph")},
				{TEXT("node_id"), TEXT("K2Node_1")},
				{TEXT("property_path"), TEXT("AuthoredPosition")},
			},
			SpatialValue.ToSharedRef(),
			SpatialValue.ToSharedRef(),
			Revision),
		MakeSnapshotField(
			TEXT("uai.semantic.blueprint.node.property"),
			NodeOwner,
			{
				{TEXT("asset_path"), RootOwner},
				{TEXT("graph_name"), TEXT("EventGraph")},
				{TEXT("node_id"), TEXT("K2Node_1")},
				{TEXT("property_path"), TEXT("WholeDecimal")},
			},
			WholeDecimal.ToSharedRef(),
			WholeDecimal.ToSharedRef(),
			Revision),
	};
	TestTrue(
		TEXT("named-selector identity is valid"),
		Entries[2].GetIdentity().IsValid());
	TestEqual(
		TEXT("C4 exposes named selectors without a second selector parser"),
		Entries[2].GetIdentity().GetSelector(TEXT("node_id")),
		FString(TEXT("K2Node_1")));
	FUeAgentCanonicalSnapshotPtr Snapshot;
	TestTrue(
		TEXT("create independent C4 snapshot"),
		FUeAgentCanonicalSnapshot::TryCreate(
			TEXT("content_graph.read.compound:re008"),
			EUeAgentStatePlane::AuthoringMemory,
			Revision,
			FUeAgentTemporalPoint(TEXT("fixture"), TEXT("8")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("content_graph.read.compound"),
				RootOwner),
			Entries,
			Resources,
			Snapshot,
			StateError));
	if (!Snapshot.IsValid())
	{
		AddError(StateError.Code + TEXT(":") + StateError.Path);
		return false;
	}

	FWorkspaceError CompositionError;
	FAuthoringModelPtr Model;
	const bool bComposed = TryComposeSnapshotAuthoringModel(
		FSnapshotCompositionRequest(
			TEXT("ue.blueprint.asset"),
			TEXT("llm_authoring"),
			Snapshot.ToSharedRef()),
		Resources,
		Model,
		CompositionError);
	TestTrue(
		TEXT("C5 consumes the C4 snapshot through the shared target composition contract"),
		bComposed);
	if (!bComposed)
	{
		AddError(FString::Printf(
			TEXT("snapshot_composition_error:%s:%s:%s"),
			*CompositionError.GetCode(),
			*CompositionError.GetSourceUnit(),
			*CompositionError.GetJsonPointer()));
	}
	TestTrue(TEXT("shared composition model exists"), Model.IsValid());
	if (!Model.IsValid())
	{
		return false;
	}
	TestTrue(
		TEXT("Blueprint target contract contributes graph_collection"),
		ContainsRole(*Model, TEXT("graph_collection")));
	bool bCanonicalLayout = false;
	for (const auto& Unit : Model->GetUnits())
		if (Unit.GetUnitKind() == ESemanticUnitKind::LayoutState)
		{
			const auto* X = Unit.GetValues().Find(TEXT("x"));
			const auto* Y = Unit.GetValues().Find(TEXT("y"));
			bCanonicalLayout |= X && Y && Unit.GetValues().Num() == 2
				&& (*X)->GetLexeme() == TEXT("320") && (*Y)->GetLexeme() == TEXT("96");
		}
	TestTrue(TEXT("graph array layout normalizes internal x/y once"), bCanonicalLayout);
	TestTrue(
		TEXT("Blueprint target contract contributes node_properties"),
		ContainsRole(*Model, TEXT("node_properties")));
	TestFalse(
		TEXT("snapshot composition does not create the obsolete generic owner role"),
		ContainsRole(*Model, TEXT("owner")));
	TestFalse(
		TEXT("snapshot composition does not create the obsolete generic state role"),
		ContainsRole(*Model, TEXT("state")));

	FWorkspaceError WorkspaceError;
	FExportWorkspacePlanPtr Plan;
	const bool bBuiltPlan =
		TryBuildSnapshotExportPlan(
			FSnapshotCompositionRequest(
				TEXT("ue.blueprint.asset"),
				TEXT("llm_authoring"),
				Snapshot.ToSharedRef()),
			{
				{TEXT("estimated_tokens"), 1000000},
			},
			Resources,
			Plan,
			WorkspaceError);
	TestTrue(
		TEXT("C5 consumes the C4 snapshot through field contracts"),
		bBuiltPlan);
	if (!bBuiltPlan)
	{
		AddError(FString::Printf(
			TEXT("snapshot_export_error:%s:%s:%s"),
			*WorkspaceError.GetCode(),
			*WorkspaceError.GetSourceUnit(),
			*WorkspaceError.GetJsonPointer()));
	}
	TestTrue(TEXT("semantic export plan exists"), Plan.IsValid());
	if (!Plan.IsValid())
	{
		return false;
	}
	TestTrue(
		TEXT("plan preserves the exact integer"),
		ContainsText(*Plan, TEXT("9007199254740993")));
	TestTrue(
		TEXT("plan contains compact external field names"),
		ContainsText(*Plan, TEXT("InputPriority")));
	TestFalse(
		TEXT("plan does not serialize the raw C4 envelope"),
		ContainsText(*Plan, TEXT("\"captured_at\"")));
	TestFalse(
		TEXT("plan does not serialize the legacy authoring state"),
		ContainsText(*Plan, TEXT("\"state_entries\"")));

	const FString OutputRoot =
		FPaths::ProjectSavedDir()
		/ TEXT("Automation/RE008/SemanticExport");
	IFileManager::Get().DeleteDirectory(
		*OutputRoot,
		false,
		true);
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError PublishError;
	TestTrue(
		TEXT("C8F publishes the complete semantic plan"),
		TryPublishExportPlan(
			FArtifactPublishRequest(
				Plan.ToSharedRef(),
				OutputRoot,
				TEXT("re008-semantic-export-test")),
			Resources,
			Receipt,
			PublishError));
	TestEqual(
		TEXT("receipt is bound to the same C4 snapshot"),
		Receipt.GetSnapshotId(),
		Snapshot->GetSnapshotId());
	TestTrue(
		TEXT("machine manifest is published"),
		FPaths::FileExists(
			OutputRoot / TEXT(".uai/workspace.hjson")));
	TestTrue(
		TEXT("semantic entry file is published"),
		FPaths::FileExists(
			OutputRoot / Receipt.GetEntryFile()));
	FUeAgentResourceTracker ResolveResources = MakeSnapshotExportTracker();
	FResolvedSemanticWorkspacePtr Resolved;
	FWorkspaceError ResolveError;
	const bool bResolved = TryResolveSemanticWorkspace(
		FWorkspaceSource::Directory(OutputRoot),
		ESemanticWorkspaceMode::State,
		ResolveResources,
		Resolved,
		ResolveError);
	TestTrue(
		TEXT("published C4 snapshot workspace reparses with the same fingerprint"),
		bResolved);
	if (!bResolved)
	{
		AddError(FString::Printf(
			TEXT("snapshot_resolve_error:%s:%s:%s"),
			*ResolveError.GetCode(),
			*ResolveError.GetSourceUnit(),
			*ResolveError.GetJsonPointer()));
	}
	if (Resolved.IsValid())
	{
		bool bTemporalPreserved = false;
		bool bSpatialPreserved = false;
		bool bWholeDecimalPreserved = false;
		bool bExactIntegerPreserved = false;
		for (const FSemanticUnit& Unit : Resolved->GetModel()->GetUnits())
		{
			const FUeAgentCanonicalValueRef* Value =
				Unit.GetValues().Find(TEXT("value"));
			if (!Value)
			{
				continue;
			}
			bTemporalPreserved |= Unit.GetFieldSetRef()
					== TEXT("AuthoredTime")
				&& (*Value)->GetKind() == EUeAgentValueKind::Temporal;
			bSpatialPreserved |= Unit.GetFieldSetRef()
					== TEXT("AuthoredPosition")
				&& (*Value)->GetKind() == EUeAgentValueKind::Spatial;
			bWholeDecimalPreserved |= Unit.GetFieldSetRef()
					== TEXT("WholeDecimal")
				&& (*Value)->GetKind() == EUeAgentValueKind::Decimal
				&& (*Value)->GetLexeme() == TEXT("320");
			bExactIntegerPreserved |= Unit.GetFieldSetRef() == TEXT("InputPriority")
				&& (*Value)->GetKind() == EUeAgentValueKind::Integer
				&& (*Value)->GetLexeme() == TEXT("9007199254740993");
		}
		TestTrue(
			TEXT("typed temporal semantics survive HJSON round-trip"),
			bTemporalPreserved);
		TestTrue(
			TEXT("typed spatial semantics survive HJSON round-trip"),
			bSpatialPreserved);
		TestTrue(
			TEXT("whole-valued decimals survive HJSON round-trip"),
			bWholeDecimalPreserved);
		TestTrue(TEXT("exact editable integer survives snapshot export and normal resolve"), bExactIntegerPreserved);
	}
	FString GraphPath, GraphText, ParseError;
	TSharedPtr<FJsonObject> Ordinary;
	for (const auto& File : Plan->GetFiles())
	{
		if (File.GetRelativePath().StartsWith(TEXT(".uai/")) || File.GetRelativePath().EndsWith(TEXT(".interfaces.hjson"))) continue;
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()), File.GetCanonicalUtf8().Num());
		TSharedPtr<FJsonObject> Candidate;
		if (FUeAgentStructuredHjsonParser::ParseObject(FString(Converted.Length(), Converted.Get()), File.GetRelativePath(), Candidate, ParseError)
			&& Candidate->HasTypedField<EJson::Object>(TEXT("nodes")))
		{ GraphPath = OutputRoot / File.GetRelativePath(); Ordinary = Candidate; break; }
	}
	if (!TestTrue(TEXT("split captured layout is present in the ordinary graph"), Ordinary.IsValid())) return false;
	const auto Nodes = Ordinary->GetObjectField(TEXT("nodes"));
	if (!TestEqual(TEXT("layout fixture has one ordinary node"), Nodes->Values.Num(), 1)) return false;
	TSharedPtr<FJsonObject> NodeSpec = Nodes->Values.CreateConstIterator().Value()->AsObject();
	const TSharedPtr<FJsonObject>* Position = nullptr;
	if (!TestTrue(TEXT("ordinary node receives separated LayoutState coordinates"),
		NodeSpec->TryGetObjectField(TEXT("pos"), Position) && Position
		&& (*Position)->GetNumberField(TEXT("x")) == 320 && (*Position)->GetNumberField(TEXT("y")) == 96)) return false;
	(*Position)->SetNumberField(TEXT("x"), 640);
	if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Ordinary.ToSharedRef(), GraphText, ParseError)
		|| !FFileHelper::SaveStringToFile(GraphText, *GraphPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	FResolvedSemanticWorkspacePtr Edited;
	auto EditResources = MakeSnapshotExportTracker();
	if (!TestTrue(TEXT("ordinary position edit resolves"), TryResolveEditedSemanticWorkspace(FWorkspaceSource::Directory(OutputRoot),
		ESemanticWorkspaceMode::State, EditResources, Edited, ResolveError)))
	{ AddError(ResolveError.GetCode()); return false; }
	bool bDerivedLayout = false;
	for (const auto& Unit : Edited->GetModel()->GetUnits())
		if (Unit.GetUnitKind() == ESemanticUnitKind::LayoutState)
		{
			const auto* X = Unit.GetValues().Find(TEXT("x"));
			bDerivedLayout |= X && ((*X)->GetKind() == EUeAgentValueKind::Integer || (*X)->GetKind() == EUeAgentValueKind::Decimal)
				&& FCString::Atod(*(*X)->GetLexeme()) == 640;
			if (Edited->GetEditedUnitIds().IsSet())
				TestFalse(TEXT("derived layout is not a second independently edited unit"), Edited->GetEditedUnitIds().GetValue().Contains(Unit.GetUnitId()));
		}
	TestTrue(TEXT("resolved layout derives its changed value from ordinary graph pos"), bDerivedLayout);
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);
	return !HasAnyErrors();
}
