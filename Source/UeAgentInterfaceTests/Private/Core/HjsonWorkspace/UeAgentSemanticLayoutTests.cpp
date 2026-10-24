#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticLayoutSiblingRelationsTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticLayout.SiblingRelationsRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticLayoutSiblingRelationsTest::RunTest(const FString&)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;
	for (const int32 RelationCount : {1, 2})
	{
		auto Resources = MakeSemanticWorkspaceResourceTracker();
		FUeAgentCanonicalValuePtr FirstValue, SecondValue;
		FUeAgentStateSemanticError ValueError;
		if (!FUeAgentCanonicalValue::TryString(TEXT("first-endpoint"), Resources, FirstValue, ValueError)
			|| !FUeAgentCanonicalValue::TryString(TEXT("second-endpoint"), Resources, SecondValue, ValueError)) return false;
		const FString TargetId = TEXT("target:relation-owner");
		const TArray<FString> RelationIds = RelationCount == 1
			? TArray<FString>({TEXT("relation:primary")})
			: TArray<FString>({TEXT("relation:primary"), TEXT("relation:secondary")} );
		TArray<FSemanticUnit> Units;
		Units.Emplace(TargetId, ESemanticUnitKind::TargetState, TEXT(""),
			TEXT("ue.fixture.asset"), TEXT("authoring_target"), TEXT(""),
			TMap<FString, FUeAgentCanonicalValueRef>(), TArray<FString>(), RelationIds,
			TEXT(""), TEXT(""), TArray<FString>(), TArray<FString>({TEXT("state")}), TEXT("fixture:relations"));
		for (int32 Index = 0; Index < RelationCount; ++Index)
		{
			Units.Emplace(RelationIds[Index], ESemanticUnitKind::RelationSet, TargetId,
				Index == 0 ? TEXT("uai.fixture.primary") : TEXT("uai.fixture.secondary"),
				Index == 0 ? TEXT("primary") : TEXT("secondary"), TEXT(""),
				TMap<FString, FUeAgentCanonicalValueRef>({{TEXT("source"),
					Index == 0 ? FirstValue.ToSharedRef() : SecondValue.ToSharedRef()}}),
				TArray<FString>(), TArray<FString>(), TEXT(""), TEXT(""),
				TArray<FString>(), TArray<FString>({TEXT("state")}), TEXT("fixture:relations"));
		}
		FAuthoringModelPtr Model;
		FWorkspaceError Error;
		if (!TestTrue(TEXT("legal sibling relations compile"), TryCompileAuthoringModel(
			TEXT("uai.fixture.relations"), 1, TEXT("llm_authoring"), TEXT("relations:r1"),
			MoveTemp(Units), {TargetId}, Resources, Model, Error))) return false;
		FWorkspaceLayoutPlanPtr Layout;
		if (!TestTrue(TEXT("legal sibling relations receive distinct paths"), TryPlanWorkspaceLayout(
			Model.ToSharedRef(), 1, EWorkspaceSourceMode::Directory, {}, Resources, Layout, Error)))
		{
			AddError(Error.GetCode());
			return false;
		}
		TSet<FString> Paths;
		for (const FString& RelationId : RelationIds)
		{
			const FString* Path = Layout->GetLogicalPlacements().Find(RelationId);
			if (!TestNotNull(TEXT("each relation has a logical placement"), Path)) return false;
			TestTrue(TEXT("single relation keeps its existing file; siblings use readable handles"),
				RelationCount == 1 ? Path->EndsWith(TEXT("/relations.hjson"))
					: Path->Contains(TEXT("/relations/")) && Path->EndsWith(TEXT(".hjson")));
			Paths.Add(*Path);
		}
		TestEqual(TEXT("no relation overwrites a sibling"), Paths.Num(), RelationCount);
		FExportWorkspacePlanPtr Plan;
		if (!TestTrue(TEXT("sibling relations export through the common plan"),
			TryBuildCompositionExportPlan(Model.ToSharedRef(), {}, Resources, Plan, Error))) return false;
		const FString Root = FPaths::ProjectIntermediateDir() / TEXT("UAI/Tests/RelationLayout")
			/ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
		for (const FExportFilePlan& File : Plan->GetFiles())
		{
			const FString Path = Root / File.GetRelativePath();
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
				|| !FFileHelper::SaveArrayToFile(File.GetCanonicalUtf8(), *Path)) return false;
		}
		FResolvedSemanticWorkspacePtr Resolved;
		if (!TestTrue(TEXT("persisted relations resolve through the ordinary workspace"),
			TryResolveSemanticWorkspace(FWorkspaceSource::Directory(Root),
				ESemanticWorkspaceMode::State, Resources, Resolved, Error)))
		{
			AddError(Error.GetCode() + TEXT(":") + Error.GetJsonPointer());
			return false;
		}
		TestEqual(TEXT("all relation identities and endpoint values survive readback"),
			Resolved->GetModel()->GetSemanticFingerprint(), Model->GetSemanticFingerprint());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticLayoutModuleContractTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticLayout.ModuleContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticLayoutModuleContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ContextResource;

	const FString TargetAlias = MakeReadableSemanticAlias(
		TEXT("target:/Game/Fixture/BP_HeavyNode"));
	const FString SameTargetAlias = MakeReadableSemanticAlias(
		TEXT("target:/Game/Fixture/BP_HeavyNode"));
	const FString NodeAlias = MakeReadableSemanticAlias(
		TEXT("item:blueprint:EventGraph:HeavyCall"));
	const FString PeerNodeAlias = MakeReadableSemanticAlias(
		TEXT("item:blueprint:EventGraph:PeerCall"));

	TestEqual(
		TEXT("the same semantic name produces the same readable alias"),
		TargetAlias,
		SameTargetAlias);
	TestNotEqual(
		TEXT("different semantic names do not share a readable alias"),
		TargetAlias,
		NodeAlias);
	TestNotEqual(
		TEXT("different peer names do not share a readable alias"),
		NodeAlias,
		PeerNodeAlias);
	TestFalse(
		TEXT("readable alias is not a source path"),
		TargetAlias.Contains(TEXT("/"))
			|| TargetAlias.Contains(TEXT("\\"))
			|| TargetAlias.Contains(TEXT("..")));
	TestTrue(
		TEXT("machine digest material is rejected instead of becoming an alias"),
		MakeReadableSemanticAlias(
			TEXT("4f1a9c6d2b8e44d0a6c7e1f39570ab12")).IsEmpty());
	TestTrue(
		TEXT("workspace schema is the versioned common schema"),
		GetSemanticWorkspaceSchemaId()
			== TEXT("uai.authoring.workspace/2"));

	const FString TargetId =
		TEXT("target:/Game/Fixture/BP_HeavyNode");
	const FString StructureId =
		TEXT("structure:blueprint:EventGraph");
	const FString NodeId =
		TEXT("item:blueprint:EventGraph:HeavyCall");
	const FString PropertyId =
		TEXT("property:item:HeavyCall:node.advanced");
	const TMap<FString,
		UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>
		EmptyValues;
	TArray<FSemanticUnit> Units;
	Units.Emplace(
		PropertyId,
		ESemanticUnitKind::PropertySet,
		NodeId,
		TEXT("uai.authoring.property_set/1"),
		TEXT("editable_properties"),
		TEXT("node.advanced"),
		EmptyValues,
		TArray<FString>{},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state"), TEXT("edit_bundle")},
		TEXT("fixture:hx004:native"));
	Units.Emplace(
		NodeId,
		ESemanticUnitKind::ItemState,
		StructureId,
		TEXT("ue.blueprint.node.call_function"),
		TEXT("call_function"),
		FString(),
		EmptyValues,
		TArray<FString>{},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state"), TEXT("edit_bundle")},
		TEXT("fixture:hx004:native"));
	Units.Emplace(
		StructureId,
		ESemanticUnitKind::StructureState,
		TargetId,
		TEXT("ue.structure.graph"),
		TEXT("event_graph"),
		FString(),
		EmptyValues,
		TArray<FString>{NodeId},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state"), TEXT("edit_bundle")},
		TEXT("fixture:hx004:native"));
	Units.Emplace(
		TargetId,
		ESemanticUnitKind::TargetState,
		FString(),
		TEXT("ue.blueprint.asset"),
		TEXT("authoring_target"),
		FString(),
		EmptyValues,
		TArray<FString>{StructureId},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state"), TEXT("edit_bundle")},
		TEXT("fixture:hx004:native"));

	const TMap<EUeAgentResourceDimension, int64> Limits{
		{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::OutputBytes, 8 * 1024 * 1024},
		{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::Nodes, 32768},
		{EUeAgentResourceDimension::Items, 32768},
		{EUeAgentResourceDimension::Files, 4096},
		{EUeAgentResourceDimension::ReferenceEdges, 32768},
		{EUeAgentResourceDimension::WorkUnits, 262144},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 32 * 1024 * 1024},
	};
	FUeAgentResourceTracker CompileResources;
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	TestTrue(
		TEXT("unordered semantic units compile into one common model"),
		TryCompileAuthoringModel(
			TEXT("uai.fixture.blueprint.authoring"),
			2,
			TEXT("state"),
			TEXT("fixture-hx004-r1"),
			MoveTemp(Units),
			TArray<FString>{TargetId},
			CompileResources,
			Model,
			Error));
	TestTrue(TEXT("compiled model is available"), Model.IsValid());
	if (!Model.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("model keeps all semantic units"),
		Model->GetUnits().Num(),
		4);
	TestTrue(
		TEXT("semantic fingerprint is content addressed"),
		Model->GetSemanticFingerprint().StartsWith(TEXT("sha256:")));

	const FSemanticUnit SparseUnit(
		TEXT("target:sparse"),
		ESemanticUnitKind::TargetState,
		FString(),
		TEXT("ue.test.asset"),
		TEXT("authoring_target"),
		FString(),
		{},
		{},
		{},
		FString(),
		FString(),
		{},
		{TEXT("state")},
		TEXT("fixture:diagnostic-only"));
	const FSemanticUnit SameSparseSemantic(
		TEXT("target:sparse"),
		ESemanticUnitKind::TargetState,
		FString(),
		TEXT("ue.test.asset"),
		TEXT("authoring_target"),
		FString(),
		{},
		{},
		{},
		FString(),
		FString(),
		{},
		{TEXT("state")},
		TEXT("fixture:different-diagnostic"));
	TArray<uint8> SparseBytes;
	FWorkspaceError SerializationError;
	TestTrue(
		TEXT("sparse semantic unit serializes"),
		TrySerializeSemanticUnitDocument(SparseUnit, SparseBytes, SerializationError));
	const FUTF8ToTCHAR SparseText(
		reinterpret_cast<const ANSICHAR*>(SparseBytes.GetData()),
		SparseBytes.Num());
	const FString SparseDocument(SparseText.Length(), SparseText.Get());
	TestTrue(
		TEXT("small semantic document keeps a complete HJSON root object"),
		SparseDocument.StartsWith(TEXT("{"))
			&& SparseDocument.EndsWith(TEXT("}\n")));
	TSharedPtr<FJsonObject> ParsedSparseDocument;
	FString SparseParseError;
	TestTrue(
		TEXT("structured semantic document parses through the common HJSON formatter"),
		FUeAgentStructuredHjsonParser::ParseObject(
			SparseDocument,
			TEXT("semantic-layout-test.hjson"),
			ParsedSparseDocument,
			SparseParseError));
	TestTrue(
		TEXT("parsed semantic document remains an object"),
		ParsedSparseDocument.IsValid());
	TestFalse(
		TEXT("diagnostic provenance is not model-visible"),
		SparseDocument.Contains(TEXT("source_provenance")));
	for (const FString& MachineOnlyField : {
		TEXT("unit_id"), TEXT("unit_kind"), TEXT("owner_ref"), TEXT("semantic_type_ref"),
		TEXT("role"), TEXT("profile_visibility"), TEXT("target_identity_key")})
	{
		TestFalse(
			FString::Printf(
				TEXT("machine-only field %s is not model-visible"),
				*MachineOnlyField),
			SparseDocument.Contains(MachineOnlyField));
	}
	const TArray<FString> EmptyOptionalFields{
		TEXT("item_refs"),
		TEXT("relation_refs"),
		TEXT("ordering_ref"),
		TEXT("layout_ref"),
		TEXT("dependency_refs")};
	for (const FString& EmptyOptionalField : EmptyOptionalFields)
	{
		TestFalse(
			FString::Printf(
				TEXT("empty optional field %s is omitted"),
				*EmptyOptionalField),
			SparseDocument.Contains(EmptyOptionalField));
	}
	TestEqual(
		TEXT("diagnostic provenance does not affect semantic content hash"),
		SemanticUnitContentHash(SparseUnit),
		SemanticUnitContentHash(SameSparseSemantic));

	FUeAgentResourceTracker LowResources;
	FWorkspaceLayoutPlanPtr LowPlan;
	TestTrue(
		TEXT("directory layout plans without a token gate"),
		TryPlanWorkspaceLayout(
			Model.ToSharedRef(),
			1,
			EWorkspaceSourceMode::Directory,
			TMap<FString, int64>{
				{TEXT(" estimated_tokens "), 64},
				{TEXT("estimated_bytes"), 1024}},
			LowResources,
			LowPlan,
			Error));
	FUeAgentResourceTracker HighResources;
	FWorkspaceLayoutPlanPtr HighPlan;
	TestTrue(
		TEXT("large token telemetry remains non-blocking"),
		TryPlanWorkspaceLayout(
			Model.ToSharedRef(),
			1,
			EWorkspaceSourceMode::Directory,
			TMap<FString, int64>{
				{TEXT("estimated_tokens"), 1000000},
				{TEXT("estimated_bytes"), 8388608}},
			HighResources,
			HighPlan,
			Error));
	TestTrue(
		TEXT("both layout plans are available"),
		LowPlan.IsValid() && HighPlan.IsValid());
	if (!LowPlan.IsValid() || !HighPlan.IsValid())
	{
		return false;
	}
	TestTrue(
		TEXT("telemetry does not alter logical placements"),
		LowPlan->GetLogicalPlacements().OrderIndependentCompareEqual(
			HighPlan->GetLogicalPlacements()));
	TestTrue(
		TEXT("telemetry does not alter physical files"),
		LowPlan->GetPhysicalFiles().OrderIndependentCompareEqual(
			HighPlan->GetPhysicalFiles()));
	TestEqual(
		TEXT("manifest uses the same semantic fingerprint"),
		LowPlan->GetMachineManifest().SemanticFingerprint,
		Model->GetSemanticFingerprint());
	TestEqual(
		TEXT("manifest inventory covers every unit"),
		LowPlan->GetMachineManifest().UnitInventory.Num(),
		Model->GetUnits().Num());
	TestEqual(
		TEXT("LLM index is navigation-only"),
		LowPlan->GetLlmIndex().SurfaceUnitIds.Num(),
		3);
	TestTrue(
		TEXT("telemetry keys are normalized without corrupting lookup"),
		LowPlan->GetTelemetry().Contains(TEXT("estimated_tokens")));

	int32 NodeFileCount = 0;
	bool bGraphUsesReadablePath = false;
	bool bGraphChildIsMachineOnly = false;
	for (const TPair<FString, TArray<FString>>& File
		: LowPlan->GetPhysicalFiles())
	{
		NodeFileCount += File.Value.Contains(NodeId) ? 1 : 0;
		bGraphUsesReadablePath |= File.Key == TEXT("graphs/event_graph.hjson")
			&& File.Value.Contains(StructureId);
		bGraphChildIsMachineOnly |= File.Key.StartsWith(TEXT(".uai/graph-state/"))
			&& File.Value.Contains(NodeId);
	}
	TestEqual(
		TEXT("one semantic unit is not token-split across files"),
		NodeFileCount,
		1);
	TestTrue(
		TEXT("graph document uses a readable scoped path without a hash handle"),
		bGraphUsesReadablePath);
	TestTrue(
		TEXT("graph child identity rows stay in the machine-only workspace"),
		bGraphChildIsMachineOnly);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticReadablePhysicalPathContractTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticLayout.ReadablePhysicalPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticReadablePhysicalPathContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ContextResource;

	const FString TargetId = TEXT("target:/Game/UAI/ReadableTarget");
	const FString StructureId = TEXT("structure:blueprint:ReadableGraph");
	const FString RepeatedItemA =
		TEXT("item:blueprint:ReadableGraph:RepeatedItemA");
	const FString RepeatedItemB =
		TEXT("item:blueprint:ReadableGraph:RepeatedItemB");
	const FString UniqueItem =
		TEXT("item:blueprint:ReadableGraph:UniqueItem");
	const FString UnsafeItem =
		TEXT("item:blueprint:ReadableGraph:UnsafeItem");
	const FString MissingNameItem =
		TEXT("item:blueprint:ReadableGraph:MissingNameItem");
	const TMap<FString, FUeAgentCanonicalValueRef> EmptyValues;

	TArray<FSemanticUnit> Units;
	Units.Emplace(
		RepeatedItemA,
		ESemanticUnitKind::ItemState,
		StructureId,
		TEXT("ue.blueprint.node.call_function"),
		TEXT("RepeatedItem"),
		FString(),
		EmptyValues,
		TArray<FString>{},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state")},
		TEXT("fixture:readable-paths"));
	Units.Emplace(
		RepeatedItemB,
		ESemanticUnitKind::ItemState,
		StructureId,
		TEXT("ue.blueprint.node.call_function"),
		TEXT("RepeatedItem"),
		FString(),
		EmptyValues,
		TArray<FString>{},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state")},
		TEXT("fixture:readable-paths"));
	Units.Emplace(
		UniqueItem,
		ESemanticUnitKind::ItemState,
		StructureId,
		TEXT("ue.blueprint.node.call_function"),
		TEXT("UniqueItem"),
		FString(),
		EmptyValues,
		TArray<FString>{},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state")},
		TEXT("fixture:readable-paths"));
	Units.Emplace(
		UnsafeItem,
		ESemanticUnitKind::ItemState,
		StructureId,
		TEXT("ue.blueprint.node.call_function"),
		TEXT(" ../Unsafe\\Name "),
		FString(),
		EmptyValues,
		TArray<FString>{},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state")},
		TEXT("fixture:readable-paths"));
	Units.Emplace(
		MissingNameItem,
		ESemanticUnitKind::ItemState,
		StructureId,
		TEXT("ue.blueprint.node.missing"),
		TEXT("missing"),
		FString(),
		EmptyValues,
		TArray<FString>{},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state")},
		TEXT("fixture:readable-paths"));
	Units.Emplace(
		StructureId,
		ESemanticUnitKind::StructureState,
		TargetId,
		TEXT("ue.blueprint.structure"),
		TEXT("ReadableGraph"),
		FString(),
		EmptyValues,
		TArray<FString>{
			RepeatedItemA, RepeatedItemB, UniqueItem,
			UnsafeItem, MissingNameItem},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state")},
		TEXT("fixture:readable-paths"));
	Units.Emplace(
		TargetId,
		ESemanticUnitKind::TargetState,
		FString(),
		TEXT("ue.blueprint.asset"),
		TEXT("ReadableTarget"),
		FString(),
		EmptyValues,
		TArray<FString>{StructureId},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state")},
		TEXT("fixture:readable-paths"));

	const TMap<EUeAgentResourceDimension, int64> Limits{
		{EUeAgentResourceDimension::InputBytes, 2 * 1024 * 1024},
		{EUeAgentResourceDimension::OutputBytes, 2 * 1024 * 1024},
		{EUeAgentResourceDimension::StringBytes, 2 * 1024 * 1024},
		{EUeAgentResourceDimension::Nodes, 4096},
		{EUeAgentResourceDimension::Items, 4096},
		{EUeAgentResourceDimension::Files, 4096},
		{EUeAgentResourceDimension::ReferenceEdges, 4096},
		{EUeAgentResourceDimension::WorkUnits, 16384},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024}};
	FUeAgentResourceTracker Resources;
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	TestTrue(
		TEXT("readable path fixture compiles"),
		TryCompileAuthoringModel(
			TEXT("uai.fixture.blueprint.authoring"),
			2,
			TEXT("state"),
			TEXT("fixture-readable-paths"),
			MoveTemp(Units),
			TArray<FString>{TargetId},
			Resources,
			Model,
			Error));
	if (!Model.IsValid())
	{
		return false;
	}

	FWorkspaceLayoutPlanPtr Plan;
	TestTrue(
		TEXT("readable path layout is planned"),
		TryPlanWorkspaceLayout(
			Model.ToSharedRef(),
			1,
			EWorkspaceSourceMode::Directory,
			{},
			Resources,
			Plan,
			Error));
	if (!Plan.IsValid())
	{
		return false;
	}

	const TMap<FString, FString>& Physical = Plan->GetPhysicalPlacements();
	const FString* TargetPath = Physical.Find(TargetId);
	const FString* StructurePath = Physical.Find(StructureId);
	const FString* RepeatedPathA = Physical.Find(RepeatedItemA);
	const FString* RepeatedPathB = Physical.Find(RepeatedItemB);
	const FString* UniquePath = Physical.Find(UniqueItem);
	const FString* UnsafePath = Physical.Find(UnsafeItem);
	const FString* MissingNamePath = Physical.Find(MissingNameItem);
	TestNotNull(TEXT("target has a physical path"), TargetPath);
	TestNotNull(TEXT("structure has a physical path"), StructurePath);
	TestNotNull(TEXT("first repeated item has a physical path"), RepeatedPathA);
	TestNotNull(TEXT("second repeated item has a physical path"), RepeatedPathB);
	TestNotNull(TEXT("unique item has a physical path"), UniquePath);
	TestNotNull(TEXT("unsafe-name item has a physical path"), UnsafePath);
	TestNotNull(TEXT("missing-name item has a physical path"), MissingNamePath);
	if (TargetPath == nullptr || StructurePath == nullptr || RepeatedPathA == nullptr
		|| RepeatedPathB == nullptr || UniquePath == nullptr
		|| UnsafePath == nullptr || MissingNamePath == nullptr)
	{
		return false;
	}

	TestTrue(
		TEXT("target physical path uses its readable semantic name"),
		TargetPath->Contains(TEXT("targets/readabletarget/")));
	TestTrue(
		TEXT("structure physical path uses its readable semantic name"),
		StructurePath->Contains(
			TEXT("structures/readablegraph/")));
	TestTrue(
		TEXT("first same-owner item keeps the base alias"),
		RepeatedPathA->Contains(TEXT("/items/repeateditem/")));
	TestTrue(
		TEXT("second same-owner item receives occurrence two"),
		RepeatedPathB->Contains(TEXT("/items/repeateditem#2/")));
	TestTrue(
		TEXT("unique item does not receive an unnecessary occurrence"),
		UniquePath->Contains(TEXT("/items/uniqueitem/"))
			&& !UniquePath->Contains(TEXT("uniqueitem1")));
	TestTrue(
		TEXT("unsafe semantic name is reduced to a safe readable segment"),
		UnsafePath->Contains(TEXT("unsafe"))
			&& UnsafePath->Contains(TEXT("name")));
	TestTrue(
		TEXT("missing semantic name falls back to readable semantic role"),
		MissingNamePath->Contains(
			TEXT("/items/missing/")));

	for (const TPair<FString, FString>& Placement : Physical)
	{
		const FString& Path = Placement.Value;
		if (Path.StartsWith(TEXT(".uai/")))
		{
			continue;
		}
		TestFalse(
			TEXT("public physical path has no traversal or backslash"),
			Path.Contains(TEXT("..")) || Path.Contains(TEXT("\\")));
		TestFalse(
			TEXT("public physical path has no digest marker"),
			Path.Contains(TEXT("sha256:"))
				|| Path.Contains(TEXT("sha1:"))
				|| Path.Contains(TEXT("md5:"))
				|| Path.Contains(TEXT("guid:"))
				|| Path.Contains(TEXT("hash:")));
		TArray<FString> PathSegments;
		Path.ParseIntoArray(PathSegments, TEXT("/"), true);
		for (const FString& Segment : PathSegments)
		{
			bool bHexOnly = !Segment.IsEmpty();
			for (const TCHAR Character : Segment)
			{
				bHexOnly &=
					(Character >= TEXT('0') && Character <= TEXT('9'))
					|| (Character >= TEXT('a') && Character <= TEXT('f'))
					|| (Character >= TEXT('A') && Character <= TEXT('F'));
			}
			TestFalse(
				TEXT("public physical path has no standalone digest fragment"),
				bHexOnly && (Segment.Len() == 32 || Segment.Len() == 64));
		}
	}

	TestEqual(
		TEXT("hidden local mapping retains every semantic unit"),
		Plan->GetMachineManifest().LocalHandleMapping.Num(),
		Model->GetUnits().Num());
	for (const TPair<FString, FString>& Mapping
		: Plan->GetMachineManifest().LocalHandleMapping)
	{
		const FSemanticUnit* Unit = Model->FindUnit(Mapping.Key);
		TestNotNull(TEXT("hidden local mapping can resolve a unit"), Unit);
		if (Unit != nullptr)
		{
			const FWorkspaceUnitInventoryEntry* Inventory =
				Plan->GetMachineManifest().UnitInventory.FindByPredicate(
					[&Mapping](const FWorkspaceUnitInventoryEntry& Entry)
					{
						return Entry.UnitId == Mapping.Key
							&& Entry.LocalHandle == Mapping.Value;
					});
			TestNotNull(TEXT("hidden mapping is reflected in unit inventory"), Inventory);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticLayoutNestedOwnerChainTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticLayout.NestedOwnerChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticLayoutNestedOwnerChainTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	const FString TargetId = TEXT("target:nested-owner");
	const FString StructureId = TEXT("structure:nested-owner-graph");
	const FString ParentAId = TEXT("item:parent-a");
	const FString ParentBId = TEXT("item:parent-b");
	const FString ChildAId = TEXT("item:child-a");
	const FString ChildBId = TEXT("item:child-b");
	const FString PropertyAId = TEXT("property:child-a");
	const FString PropertyBId = TEXT("property:child-b");
	const TMap<FString, FUeAgentCanonicalValueRef> EmptyValues;
	const auto MakeUnit =
		[&](const FString& Id,
			ESemanticUnitKind Kind,
			const FString& Owner,
			const FString& Name,
			const TArray<FString>& Children,
			const FString& FieldSet = FString())
		{
			return FSemanticUnit(
				Id,
				Kind,
				Owner,
				Kind == ESemanticUnitKind::PropertySet
					? TEXT("uai.authoring.property_set/1")
					: TEXT("ue.blueprint.node"),
				Name,
				FieldSet,
				EmptyValues,
				Children,
				{},
				FString(),
				FString(),
				{},
				{TEXT("state")},
				TEXT("fixture:nested-owner-chain"));
		};

	TArray<FSemanticUnit> Units;
	Units.Add(MakeUnit(
		PropertyAId, ESemanticUnitKind::PropertySet, ChildAId,
		TEXT("child_property"), {}, TEXT("editable_properties")));
	Units.Add(MakeUnit(
		PropertyBId, ESemanticUnitKind::PropertySet, ChildBId,
		TEXT("child_property"), {}, TEXT("editable_properties")));
	Units.Add(MakeUnit(
		ChildAId, ESemanticUnitKind::ItemState, ParentAId,
		TEXT("Child"), {}));
	Units.Add(MakeUnit(
		ChildBId, ESemanticUnitKind::ItemState, ParentBId,
		TEXT("Child"), {}));
	Units.Add(MakeUnit(
		ParentAId, ESemanticUnitKind::ItemState, StructureId,
		TEXT("Parent"), {ChildAId}));
	Units.Add(MakeUnit(
		ParentBId, ESemanticUnitKind::ItemState, StructureId,
		TEXT("Parent"), {ChildBId}));
	Units.Add(MakeUnit(
		StructureId, ESemanticUnitKind::StructureState, TargetId,
		TEXT("NestedGraph"), {ParentAId, ParentBId}));
	Units.Add(MakeUnit(
		TargetId, ESemanticUnitKind::TargetState, FString(),
		TEXT("NestedTarget"), {StructureId}));
	// A collection and item may share a readable base in different kind scopes.
	// Both still contribute an /items/<owner>/ segment to descendant paths.
	const FString CollectionId = TEXT("collection:shared");
	const FString SharedItemId = TEXT("item:shared");
	const FString CollectionChildId = TEXT("item:collection-point");
	const FString ItemChildId = TEXT("item:item-point");
	Units.Add(MakeUnit(CollectionId, ESemanticUnitKind::CollectionState,
		StructureId, TEXT("Shared"), {CollectionChildId}));
	Units.Add(MakeUnit(SharedItemId, ESemanticUnitKind::ItemState,
		StructureId, TEXT("Shared"), {ItemChildId}));
	Units.Add(MakeUnit(CollectionChildId, ESemanticUnitKind::ItemState,
		CollectionId, TEXT("Point"), {}));
	Units.Add(MakeUnit(ItemChildId, ESemanticUnitKind::ItemState,
		SharedItemId, TEXT("Point"), {}));

	const TMap<EUeAgentResourceDimension, int64> Limits{
		{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
		{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
		{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
		{EUeAgentResourceDimension::Nodes, 1024},
		{EUeAgentResourceDimension::Items, 1024},
		{EUeAgentResourceDimension::Files, 1024},
		{EUeAgentResourceDimension::ReferenceEdges, 1024},
		{EUeAgentResourceDimension::WorkUnits, 8192},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 8 * 1024 * 1024}};
	FUeAgentResourceTracker Resources;
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	TestTrue(
		TEXT("nested owner fixture compiles"),
		TryCompileAuthoringModel(
			TEXT("uai.fixture.nested-owner"), 2, TEXT("state"),
			TEXT("fixture-nested-owner"), MoveTemp(Units), {TargetId},
			Resources, Model, Error));
	if (!Model.IsValid())
	{
		return false;
	}
	FWorkspaceLayoutPlanPtr Plan;
	TestTrue(
		TEXT("nested owner layout is planned"),
		TryPlanWorkspaceLayout(
			Model.ToSharedRef(), 1, EWorkspaceSourceMode::Directory, {},
			Resources, Plan, Error));
	if (!Plan.IsValid())
	{
		return false;
	}
	const FString* PropertyAPath =
		Plan->GetLogicalPlacements().Find(PropertyAId);
	const FString* PropertyBPath =
		Plan->GetLogicalPlacements().Find(PropertyBId);
	const FString* PropertyAPhysicalPath =
		Plan->GetPhysicalPlacements().Find(PropertyAId);
	const FString* PropertyBPhysicalPath =
		Plan->GetPhysicalPlacements().Find(PropertyBId);
	TestNotNull(TEXT("first nested property has a logical path"), PropertyAPath);
	TestNotNull(TEXT("second nested property has a logical path"), PropertyBPath);
	TestNotNull(
		TEXT("first nested property has a physical path"),
		PropertyAPhysicalPath);
	TestNotNull(
		TEXT("second nested property has a physical path"),
		PropertyBPhysicalPath);
	if (PropertyAPath == nullptr || PropertyBPath == nullptr
		|| PropertyAPhysicalPath == nullptr
		|| PropertyBPhysicalPath == nullptr)
	{
		return false;
	}
	const auto& PublicRefs = Plan->GetMachineManifest().PublicRefMapping;
	const auto& Physical = Plan->GetPhysicalPlacements();
	auto PublicAlias = [&](const FString& Id) {
		FString Kind, Alias;
		PublicRefs.FindChecked(Id).Split(TEXT(":"), &Kind, &Alias);
		return Alias;
	};
	for (const FString& Child : {ChildAId, ChildBId, CollectionChildId, ItemChildId})
	{
		const FString Owner = Model->FindUnit(Child)->GetOwnerRef();
		const FString ExpectedOwnerDirectory = FPaths::GetPath(Physical.FindChecked(StructureId))
			/ TEXT("items") / PublicAlias(Owner);
		TestEqual(TEXT("public child reference resolves beneath its complete public owner chain"),
			Physical.FindChecked(Child), ExpectedOwnerDirectory / TEXT("items") / PublicAlias(Child) / TEXT("state.hjson"));
		if (Model->FindUnit(Owner)->GetUnitKind() == ESemanticUnitKind::ItemState)
			TestEqual(TEXT("child directory uses the actual parent placement, including collision suffix"),
				FPaths::GetPath(Physical.FindChecked(Owner)), ExpectedOwnerDirectory);
	}
	TestNotEqual(TEXT("cross-kind colliding owners have distinct navigation identities"),
		PublicAlias(CollectionId), PublicAlias(SharedItemId));
	TestTrue(TEXT("property remains below the actual child state directory"),
		PropertyAPath->StartsWith(FPaths::GetPath(Physical.FindChecked(ChildAId)) + TEXT("/"))
			&& PropertyBPath->StartsWith(FPaths::GetPath(Physical.FindChecked(ChildBId)) + TEXT("/")));
	TestNotEqual(
		TEXT("same-named nested properties have distinct scopes"),
		*PropertyAPath, *PropertyBPath);
	TestNotEqual(
		TEXT("same-named nested properties have distinct physical scopes"),
		*PropertyAPhysicalPath, *PropertyBPhysicalPath);

	TArray<FSemanticUnit> DuplicateUnits;
	DuplicateUnits.Add(MakeUnit(
		TEXT("property:child-a-duplicate"), ESemanticUnitKind::PropertySet,
		ChildAId, TEXT("child_property"), {}, TEXT("editable_properties")));
	for (const FSemanticUnit& Unit : Model->GetUnits())
	{
		DuplicateUnits.Add(Unit);
	}
	FAuthoringModelPtr DuplicateModel;
	FWorkspaceError DuplicateError;
	FUeAgentResourceTracker DuplicateResources;
	TestTrue(
		TEXT("duplicate-owner fixture compiles"),
		TryCompileAuthoringModel(
			TEXT("uai.fixture.nested-owner"), 2, TEXT("state"),
			TEXT("fixture-nested-owner-duplicate"), MoveTemp(DuplicateUnits),
			{TargetId}, DuplicateResources, DuplicateModel, DuplicateError));
	if (DuplicateModel.IsValid())
	{
		FWorkspaceLayoutPlanPtr DuplicatePlan;
		TestFalse(
			TEXT("same-owner same-property path is rejected"),
			TryPlanWorkspaceLayout(
				DuplicateModel.ToSharedRef(), 1,
				EWorkspaceSourceMode::Directory, {}, DuplicateResources,
				DuplicatePlan, DuplicateError));
	}
	return !HasAnyErrors();
}
