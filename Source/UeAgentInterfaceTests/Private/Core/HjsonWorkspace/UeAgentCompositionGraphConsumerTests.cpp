#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/PCG/Authoring/UeAgentPCGGraphAuthoring.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCompositionPcgGraphConsumerTest,
	"UeAgentInterface.Core.HjsonWorkspace.Composition.PcgGraphConsumer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCompositionPcgGraphConsumerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;

	FString RegistrationError;
	if (!TestTrue(TEXT("graph target compositions register"),
		RegisterGraphTreeTargetCompositionContracts(RegistrationError)))
	{
		AddError(RegistrationError);
		return false;
	}

	auto MakeEntry = [](
		const FString& SemanticId,
		const FString& CanonicalKey,
		const TMap<FString, FString>& Selectors,
		const TSharedRef<FJsonObject>& Value)
	{
		UeAgentAuthoringState::FStateEntry Entry;
		Entry.SemanticId = SemanticId;
		Entry.TargetIdentity.CanonicalKey = CanonicalKey;
		Entry.TargetIdentity.Selectors = Selectors;
		Entry.Type.Kind = TEXT("object");
		Entry.Value = MakeShared<FJsonValueObject>(Value);
		Entry.bAvailable = true;
		Entry.bEditable = true;
		Entry.Provenance = TEXT("test");
		return Entry;
	};

	const FString AssetPath = TEXT("/Game/Test/PCG_CommonGraph");
	TSharedRef<FJsonObject> GraphMetadata = MakeShared<FJsonObject>();
	GraphMetadata->SetStringField(TEXT("name"), TEXT("PCG Common Graph"));
	GraphMetadata->SetStringField(TEXT("graph_kind"), TEXT("pcg"));
	TSharedRef<FJsonObject> GraphValue = MakeShared<FJsonObject>();
	GraphValue->SetObjectField(TEXT("graph"), GraphMetadata);

	auto MakePin = [](const TCHAR* Name, const TCHAR* Direction)
	{
		TSharedRef<FJsonObject> Pin = MakeShared<FJsonObject>();
		Pin->SetStringField(TEXT("name"), Name);
		Pin->SetStringField(TEXT("direction"), Direction);
		Pin->SetNumberField(TEXT("occurrence"), 0);
		Pin->SetStringField(TEXT("allowed_types"), TEXT("Point"));
		return MakeShared<FJsonValueObject>(Pin);
	};
	auto MakeNode = [&MakePin](
		const TCHAR* NodeId, const TCHAR* Title, const TCHAR* ClassPath)
	{
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("id"), NodeId);
		Node->SetStringField(TEXT("authored_title"), Title);
		Node->SetStringField(TEXT("settings_class"), ClassPath);
		TSharedRef<FJsonObject> Interface = MakeShared<FJsonObject>();
		Interface->SetArrayField(TEXT("pins"), {
			MakePin(TEXT("Out"), TEXT("output")),
			MakePin(TEXT("In"), TEXT("input"))});
		Node->SetObjectField(TEXT("pin_interface"), Interface);
		return Node;
	};

	TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("node"), TEXT("native-node-a"));
	From->SetStringField(TEXT("pin"), TEXT("Out"));
	TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("node"), TEXT("native-node-b"));
	To->SetStringField(TEXT("pin"), TEXT("In"));
	TSharedRef<FJsonObject> EdgeValue = MakeShared<FJsonObject>();
	EdgeValue->SetObjectField(TEXT("from"), From);
	EdgeValue->SetObjectField(TEXT("to"), To);

	FCompositionRequest Request;
	Request.TargetKind = TEXT("ue.pcg_graph.asset");
	Request.ProfileId = TEXT("llm_authoring");
	Request.BaseRevision = TEXT("pcg-common-graph:r1");
	Request.SourceProvenance = TEXT("test");
	Request.Entries = {
		MakeEntry(TEXT("uai.semantic.pcg.graph"), TEXT("pcg-graph"),
			{{TEXT("asset_path"), AssetPath}}, GraphValue),
		MakeEntry(TEXT("uai.semantic.pcg.export.node"), TEXT("pcg-node-a"),
			{{TEXT("asset_path"), AssetPath}, {TEXT("node_id"), TEXT("native-node-a")}},
			MakeNode(TEXT("native-node-a"), TEXT("Create Points Grid"), TEXT("/Script/PCG.PCGCreatePointsGridSettings"))),
		MakeEntry(TEXT("uai.semantic.pcg.export.node"), TEXT("pcg-node-b"),
			{{TEXT("asset_path"), AssetPath}, {TEXT("node_id"), TEXT("native-node-b")}},
			MakeNode(TEXT("native-node-b"), TEXT("Transform Points"), TEXT("/Script/PCG.PCGTransformPointsSettings"))),
		MakeEntry(TEXT("uai.semantic.pcg.export.edge"), TEXT("pcg-edge"),
			{{TEXT("asset_path"), AssetPath}, {TEXT("edge_key"), TEXT("opaque-edge-key")}},
			EdgeValue)};

	const TMap<EUeAgentResourceDimension, int64> Limits{
		{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::Nodes, 4096},
		{EUeAgentResourceDimension::Items, 4096},
		{EUeAgentResourceDimension::Files, 4096},
		{EUeAgentResourceDimension::ReferenceEdges, 4096},
		{EUeAgentResourceDimension::WorkUnits, 32768},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 32 * 1024 * 1024}};
	FUeAgentResourceTracker Resources;
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	if (!TestTrue(TEXT("PCG graph composes through the common graph owner"),
		TryComposeAuthoringModel(Request, Resources, Model, Error))
		|| !Model.IsValid())
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetSourceUnit());
		return false;
	}

	const FSemanticUnit* GraphUnit = nullptr;
	for (const FSemanticUnit& Unit : Model->GetUnits())
	{
		if (Unit.GetSemanticTypeRef() == TEXT("uai.semantic.pcg.graph"))
		{
			GraphUnit = &Unit;
			break;
		}
	}
	if (!TestNotNull(TEXT("PCG has an explicit common graph structure unit"), GraphUnit))
	{
		return false;
	}
	TestEqual(TEXT("PCG graph uses StructureState"),
		GraphUnit->GetUnitKind(), ESemanticUnitKind::StructureState);
	TestTrue(TEXT("common graph projection owns nodes"),
		GraphUnit->GetValues().Contains(TEXT("nodes")));
	TestTrue(TEXT("common graph projection owns edges"),
		GraphUnit->GetValues().Contains(TEXT("edges")));

	FExportWorkspacePlanPtr Plan;
	if (!TestTrue(TEXT("PCG common graph export plan builds"),
		TryBuildCompositionExportPlan(
			Model.ToSharedRef(),
			FCompositionSnapshotBinding(
				TEXT("snapshot:pcg-common"),
				UE::UAI::StateSemantic::FUeAgentStateRevision(TEXT("test"), TEXT("r1"), 1),
				TEXT("evidence:pcg-common")),
			{}, Resources, Plan, Error))
		|| !Plan.IsValid())
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetSourceUnit());
		return false;
	}

	FString VisibleText;
	int64 VisibleBytes = 0;
	bool bMachineGraphStateCaptured = false;
	for (const FExportFilePlan& File : Plan->GetFiles())
	{
		if (!IsModelVisibleSemanticWorkspacePath(File.GetRelativePath()))
		{
			bMachineGraphStateCaptured = bMachineGraphStateCaptured
				|| File.GetRelativePath().StartsWith(TEXT(".uai/graph-state/"));
			continue;
		}
		VisibleBytes += File.GetCanonicalUtf8().Num();
		const FUTF8ToTCHAR Text(
			reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()),
			File.GetCanonicalUtf8().Num());
		VisibleText += FString(Text.Length(), Text.Get());
	}
	TestTrue(TEXT("public PCG graph uses common connections"),
		VisibleText.Contains(TEXT("connections")));
	TestTrue(TEXT("public PCG graph uses readable node aliases"),
		VisibleText.Contains(TEXT("CreatePointsGrid"))
			&& VisibleText.Contains(TEXT("TransformPoints")));
	TestTrue(TEXT("public PCG graph exposes a separate Pin interface companion"),
		VisibleText.Contains(TEXT("allowed_types")));
	TestFalse(TEXT("PCG settings class stays machine-side"),
		VisibleText.Contains(TEXT("settings_class"))
			|| VisibleText.Contains(TEXT("PCGCreatePointsGridSettings")));
	TestFalse(TEXT("opaque edge identity stays machine-side"),
		VisibleText.Contains(TEXT("opaque-edge-key")));
	TestTrue(TEXT("graph child recovery state remains in the machine workspace"),
		bMachineGraphStateCaptured);
	TestEqual(TEXT("observed tokens count only model-visible graph files"),
		Plan->GetObservedTokens(), (VisibleBytes + 3) / 4);

	TSharedPtr<FJsonObject> NativeDocument;
	FString AdapterError;
	if (!TestTrue(TEXT("PCG consumes the common semantic graph through its adapter"),
		UeAgentPCGAuthoring::TryBuildNativeGraphDocumentFromSemanticModel(
			Model.ToSharedRef(), NativeDocument, AdapterError))
		|| !NativeDocument.IsValid())
	{
		AddError(AdapterError);
		return false;
	}
	const TSharedPtr<FJsonObject>* NativeNodes = nullptr;
	const TSharedPtr<FJsonObject>* NativeEdges = nullptr;
	TestTrue(TEXT("native PCG document restores nodes by machine identity"),
		NativeDocument->TryGetObjectField(TEXT("nodes"), NativeNodes)
			&& NativeNodes != nullptr && NativeNodes->IsValid()
			&& (*NativeNodes)->HasField(TEXT("native-node-a"))
			&& (*NativeNodes)->HasField(TEXT("native-node-b")));
	TestTrue(TEXT("native PCG document restores one edge"),
		NativeDocument->TryGetObjectField(TEXT("edges"), NativeEdges)
			&& NativeEdges != nullptr && NativeEdges->IsValid()
			&& (*NativeEdges)->Values.Num() == 1);
	if (NativeNodes != nullptr && NativeNodes->IsValid())
	{
		const TSharedPtr<FJsonObject>* NativeNode = nullptr;
		TestTrue(TEXT("native PCG node keeps the class required by the backend"),
			(*NativeNodes)->TryGetObjectField(TEXT("native-node-a"), NativeNode)
				&& NativeNode != nullptr && NativeNode->IsValid()
				&& (*NativeNode)->HasField(TEXT("settings_class")));
		if (NativeNode != nullptr && NativeNode->IsValid())
		{
			TestFalse(TEXT("native PCG adapter removes common-only Pin interface"),
				(*NativeNode)->HasField(TEXT("pin_interface")));
			TestFalse(TEXT("native PCG adapter removes duplicate node identity"),
				(*NativeNode)->HasField(TEXT("id")));
		}
	}
	UeAgentPCGAuthoring::FGraphApplyResult PreflightResult;
	if (!TestTrue(TEXT("resolved common graph reaches real PCG graph mechanics"),
		UeAgentPCGAuthoring::PreflightGraphDocument(
			nullptr, NativeDocument, true, PreflightResult, AdapterError)))
	{
		AddError(AdapterError);
		return false;
	}
	TestEqual(TEXT("PCG preflight creates both resolved nodes"),
		PreflightResult.NodeCount, 2);
	TestEqual(TEXT("PCG preflight creates the resolved connection"),
		PreflightResult.EdgeCount, 1);
	return !HasAnyErrors();
}
