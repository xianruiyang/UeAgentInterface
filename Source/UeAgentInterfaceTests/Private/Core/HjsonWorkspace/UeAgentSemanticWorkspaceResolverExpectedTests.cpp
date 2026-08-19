#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;

	FUeAgentResourceTracker MakeResolverTracker()
	{
		using UE::UAI::ContextResource::EUeAgentResourceDimension;
		const TMap<EUeAgentResourceDimension, int64> Limits{
			{EUeAgentResourceDimension::InputBytes, 32 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 32 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 32 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 262144},
			{EUeAgentResourceDimension::Items, 262144},
			{EUeAgentResourceDimension::Files, 32768},
			{EUeAgentResourceDimension::ReferenceEdges, 262144},
			{EUeAgentResourceDimension::WorkUnits, 2097152},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 256 * 1024 * 1024},
		};
		return FUeAgentResourceTracker(FUeAgentResourceBudget(Limits));
	}

	UeAgentAuthoringState::FStateEntry MakeResolverEntry(
		const FString& CanonicalKey,
		const FString& NodeGuid,
		const FString& Title)
	{
		TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("node_guid"), NodeGuid);
		Value->SetStringField(TEXT("title"), Title);
		UeAgentAuthoringState::FStateEntry Entry;
		Entry.SemanticId = TEXT("uai.semantic.blueprint.node");
		Entry.TargetIdentity.TargetKind = TEXT("blueprint_node");
		Entry.TargetIdentity.CanonicalKey = CanonicalKey;
		Entry.TargetIdentity.Selectors = {
			{TEXT("asset_path"), TEXT("/Game/HX013/BP_Resolver")},
			{TEXT("graph_guid"), TEXT("graph-guid")},
			{TEXT("node_guid"), NodeGuid},
		};
		Entry.Value = MakeShared<FJsonValueObject>(Value);
		Entry.bAvailable = true;
		Entry.bEditable = true;
		Entry.Provenance = TEXT("fixture:hx013:native-resolver");
		return Entry;
	}

	UeAgentAuthoringState::FStateEntry MakeResolverRelation(
		const FString& CanonicalKey,
		const FString& FromNode,
		const FString& ToNode)
	{
		TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("from_node"), FromNode);
		Value->SetStringField(TEXT("from_pin"), TEXT("then"));
		Value->SetStringField(TEXT("to_node"), ToNode);
		Value->SetStringField(TEXT("to_pin"), TEXT("execute"));
		UeAgentAuthoringState::FStateEntry Entry;
		Entry.SemanticId = TEXT("uai.semantic.blueprint.edge");
		Entry.TargetIdentity.TargetKind = TEXT("blueprint_edge");
		Entry.TargetIdentity.CanonicalKey = CanonicalKey;
		Entry.TargetIdentity.Selectors = {
			{TEXT("asset_path"), TEXT("/Game/HX013/BP_Resolver")},
			{TEXT("graph_guid"), TEXT("graph-guid")},
			{TEXT("edge_key"), CanonicalKey},
		};
		Entry.Value = MakeShared<FJsonValueObject>(Value);
		Entry.bAvailable = true;
		Entry.bEditable = true;
		Entry.Provenance = TEXT("fixture:hx013:native-resolver");
		return Entry;
	}

	bool WritePlan(
		const FExportWorkspacePlan& Plan,
		const FString& Root)
	{
		for (const FExportFilePlan& File : Plan.GetFiles())
		{
			const FString FullPath =
				FPaths::Combine(Root, File.GetRelativePath());
			if (!IFileManager::Get().MakeDirectory(
					*FPaths::GetPath(FullPath),
					true)
				|| !FFileHelper::SaveArrayToFile(
					File.GetCanonicalUtf8(),
					*FullPath))
			{
				return false;
			}
		}
		return true;
	}

	class FScopedResolverWorkspace final
	{
	public:
		FScopedResolverWorkspace()
			: Root(FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("UAI/Tests/HX013"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FScopedResolverWorkspace()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString Root;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticWorkspaceResolverRoundTripTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticWorkspaceResolver.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceResolverRoundTripTest::RunTest(
	const FString& Parameters)
{
	const FString FirstKey =
		TEXT("blueprint_node|blueprint_node|node_guid|node_guid=node-a");
	const FString SecondKey =
		TEXT("blueprint_node|blueprint_node|node_guid|node_guid=node-b");
	FCompositionRequest Request;
	Request.TargetKind = TEXT("ue.blueprint.asset");
	Request.ProfileId = TEXT("llm_authoring");
	Request.BaseRevision = TEXT("fixture-hx013-resolver-r1");
	Request.SourceProvenance = TEXT("fixture:hx013:native-resolver");
	Request.Entries = {
		MakeResolverEntry(FirstKey, TEXT("node-a"), TEXT("First")),
		MakeResolverEntry(SecondKey, TEXT("node-b"), TEXT("Second")),
		MakeResolverRelation(
			TEXT("edge:node-a:node-b"),
			TEXT("node-a"),
			TEXT("node-b")),
		MakeResolverRelation(
			TEXT("edge:node-b:node-a"),
			TEXT("node-b"),
			TEXT("node-a")),
	};

	FUeAgentResourceTracker CompositionResources = MakeResolverTracker();
	FAuthoringModelPtr SourceModel;
	FWorkspaceError Error;
	TestTrue(
		TEXT("same-semantic-type Blueprint nodes compose"),
		TryComposeAuthoringModel(
			Request,
			CompositionResources,
			SourceModel,
			Error));
	if (!SourceModel.IsValid())
	{
		return false;
	}

	FUeAgentResourceTracker PlanResources = MakeResolverTracker();
	FExportWorkspacePlanPtr Plan;
	TestTrue(
		TEXT("semantic directory plan builds"),
		TryBuildCompositionExportPlan(
			SourceModel.ToSharedRef(),
			{{TEXT("estimated_tokens"), 1000000}},
			PlanResources,
			Plan,
			Error));
	if (!Plan.IsValid())
	{
		return false;
	}

	FScopedResolverWorkspace Workspace;
	TestTrue(
		TEXT("semantic directory plan is persisted"),
		WritePlan(*Plan, Workspace.Root));

	FUeAgentResourceTracker ResolveResources = MakeResolverTracker();
	FResolvedSemanticWorkspacePtr Resolved;
	TestTrue(
		TEXT("native resolver rebuilds the persisted semantic workspace"),
		TryResolveSemanticWorkspace(
			FWorkspaceSource::Directory(Workspace.Root),
			ESemanticWorkspaceMode::State,
			ResolveResources,
			Resolved,
			Error));
	TestTrue(TEXT("resolved workspace is available"), Resolved.IsValid());
	if (Resolved.IsValid())
	{
		TestEqual(
			TEXT("semantic fingerprint survives persisted round-trip"),
			Resolved->GetModel()->GetSemanticFingerprint(),
			SourceModel->GetSemanticFingerprint());
		TSet<FString> IdentityKeys;
		int32 RelationSetCount = 0;
		for (const FSemanticUnit& Unit :
			Resolved->GetModel()->GetUnits())
		{
			RelationSetCount += Unit.GetUnitKind()
				== ESemanticUnitKind::RelationSet;
			if (Unit.GetSemanticTypeRef()
				== TEXT("uai.semantic.blueprint.node"))
			{
				IdentityKeys.Add(Unit.GetTargetIdentityKey());
			}
		}
		TestEqual(
			TEXT("same semantic type retains two exact target bindings"),
			IdentityKeys.Num(),
			2);
		TestTrue(TEXT("first target binding survives"), IdentityKeys.Contains(FirstKey));
		TestTrue(TEXT("second target binding survives"), IdentityKeys.Contains(SecondKey));
		TestEqual(
			TEXT("one owner keeps one persisted relation set"),
			RelationSetCount,
			1);
	}

	const FExportFilePlan* UnitFile = Plan->GetFiles().FindByPredicate(
		[](const FExportFilePlan& File)
		{
			if (File.GetRelativePath()
				== TEXT(".uai/workspace.hjson"))
			{
				return false;
			}
			const TArray<uint8>& Bytes = File.GetCanonicalUtf8();
			FUTF8ToTCHAR Text(
				reinterpret_cast<const ANSICHAR*>(
					Bytes.GetData()),
				Bytes.Num());
			return FString(Text.Length(), Text.Get())
				.Contains(TEXT("\"First\""));
		});
	TestNotNull(TEXT("plan contains a semantic unit file"), UnitFile);
	if (UnitFile)
	{
		const FString FullPath =
			FPaths::Combine(Workspace.Root, UnitFile->GetRelativePath());
		FString Text;
		TestTrue(
			TEXT("semantic unit file can be read for tamper fixture"),
			FFileHelper::LoadFileToString(Text, *FullPath));
		Text.ReplaceInline(TEXT("\"First\""), TEXT("\"Tampered\""));
		TestTrue(
			TEXT("tampered semantic unit is written"),
			FFileHelper::SaveStringToFile(
				Text,
				*FullPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

		FUeAgentResourceTracker TamperResources = MakeResolverTracker();
		FResolvedSemanticWorkspacePtr Rejected;
		FWorkspaceError TamperError;
		TestFalse(
			TEXT("stale inventory hash fails before any Apply"),
			TryResolveSemanticWorkspace(
				FWorkspaceSource::Directory(Workspace.Root),
				ESemanticWorkspaceMode::State,
				TamperResources,
				Rejected,
				TamperError));
		TestEqual(
			TEXT("tamper failure is explicit"),
			TamperError.GetCode(),
			FString(TEXT("semantic_workspace_unit_hash_mismatch")));
		TestFalse(
			TEXT("rejected workspace is not exposed"),
			Rejected.IsValid());
	}
	return !HasAnyErrors();
}
