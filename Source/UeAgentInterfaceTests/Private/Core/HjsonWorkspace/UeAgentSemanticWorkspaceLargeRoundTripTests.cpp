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

	constexpr int64 HistoricalCompleteExportCeiling = 65536;

	FUeAgentResourceTracker MakeLargeRoundTripTracker()
	{
		using UE::UAI::ContextResource::EUeAgentResourceDimension;
		return FUeAgentResourceTracker();
	}

	UeAgentAuthoringState::FStateEntry MakeEntry(
		const FString& SemanticId,
		const FString& TargetKind,
		const FString& CanonicalKey,
		TMap<FString, FString> Selectors,
		const TSharedPtr<FJsonValue>& Value)
	{
		UeAgentAuthoringState::FStateEntry Entry;
		Entry.SemanticId = SemanticId;
		Entry.TargetIdentity.TargetKind = TargetKind;
		Entry.TargetIdentity.CanonicalKey = CanonicalKey;
		Entry.TargetIdentity.Selectors = MoveTemp(Selectors);
		Entry.Value = Value;
		Entry.bAvailable = true;
		Entry.bEditable = true;
		Entry.Provenance = TEXT("fixture:hx013:large-round-trip");
		return Entry;
	}

	UeAgentAuthoringState::FStateEntry MakeTargetEntry(
		const FString& SemanticId,
		const FString& TargetKind,
		const FString& AssetPath)
	{
		TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("asset_path"), AssetPath);
		Value->SetStringField(TEXT("asset_kind"), TargetKind);
		return MakeEntry(
			SemanticId,
			TargetKind,
			TEXT("asset:") + AssetPath,
			{{TEXT("asset_path"), AssetPath}},
			MakeShared<FJsonValueObject>(Value));
	}

	TArray<UeAgentAuthoringState::FStateEntry> MakeLargeBlueprintEntries()
	{
		const FString AssetPath = TEXT("/Game/HX013/BP_LargeRoundTrip");
		const FString GraphGuid = TEXT("11111111111111111111111111111111");
		TArray<UeAgentAuthoringState::FStateEntry> Entries{
			MakeTargetEntry(
				TEXT("uai.semantic.blueprint.export.target"),
				TEXT("blueprint_asset"),
				AssetPath),
		};
		TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
		Graph->SetStringField(TEXT("graph_guid"), GraphGuid);
		Graph->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		Entries.Add(MakeEntry(
			TEXT("uai.semantic.blueprint.graph"),
			TEXT("blueprint_graph"),
			TEXT("graph:") + GraphGuid,
			{
				{TEXT("asset_path"), AssetPath},
				{TEXT("graph_guid"), GraphGuid},
			},
			MakeShared<FJsonValueObject>(Graph)));

		const FString LargePayload = FString::ChrN(1024, TEXT('x'));
		for (int32 Index = 0; Index < 512; ++Index)
		{
			const FString NodeGuid = FString::Printf(
				TEXT("%032x"),
				Index + 1);
			TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("node_guid"), NodeGuid);
			Node->SetStringField(
				TEXT("node_class"),
				TEXT("/Script/BlueprintGraph.K2Node_CallFunction"));
			TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), Index * 160.0);
			Position->SetNumberField(TEXT("y"), (Index % 8) * 96.0);
			Node->SetObjectField(TEXT("pos"), Position);
			Entries.Add(MakeEntry(
				TEXT("uai.semantic.blueprint.node"),
				TEXT("blueprint_node"),
				TEXT("node:") + NodeGuid,
				{
					{TEXT("asset_path"), AssetPath},
					{TEXT("graph_guid"), GraphGuid},
					{TEXT("node_guid"), NodeGuid},
				},
				MakeShared<FJsonValueObject>(Node)));

			const FString FieldId = FString::Printf(
				TEXT("node.large_payload.%04d"),
				Index);
			TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(
				TEXT("value"),
				FString::Printf(TEXT("%04d:"), Index) + LargePayload);
			Entries.Add(MakeEntry(
				TEXT("uai.semantic.blueprint.node.property"),
				TEXT("blueprint_node_property"),
				TEXT("node-property:") + NodeGuid + TEXT(":payload"),
				{
					{TEXT("asset_path"), AssetPath},
					{TEXT("graph_guid"), GraphGuid},
					{TEXT("node_guid"), NodeGuid},
					{TEXT("property_path"), FieldId},
				},
				MakeShared<FJsonValueObject>(Property)));
		}
		return Entries;
	}

	TArray<UeAgentAuthoringState::FStateEntry> MakeLargeNiagaraEntries()
	{
		const FString AssetPath = TEXT("/Game/HX013/NS_LargeRoundTrip");
		TArray<UeAgentAuthoringState::FStateEntry> Entries{
			MakeTargetEntry(
				TEXT("uai.semantic.niagara.asset"),
				TEXT("niagara_system"),
				AssetPath),
		};
		const FString Payload = FString::ChrN(384, TEXT('n'));
		for (int32 Index = 0; Index < 384; ++Index)
		{
			const FString UsageId = FString::Printf(
				TEXT("usage-%04d"),
				Index / 16);
			const FString ModuleGuid = FString::Printf(
				TEXT("%032x"),
				Index + 1);
			TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>();
			Module->SetStringField(TEXT("module_guid"), ModuleGuid);
			Module->SetStringField(TEXT("usage_id"), UsageId);
			Module->SetStringField(TEXT("authored_value"), Payload);
			Entries.Add(MakeEntry(
				TEXT("uai.semantic.niagara.module"),
				TEXT("niagara_module"),
				TEXT("module:") + ModuleGuid,
				{
					{TEXT("asset_path"), AssetPath},
					{TEXT("usage_id"), UsageId},
					{TEXT("module_guid"), ModuleGuid},
				},
				MakeShared<FJsonValueObject>(Module)));
		}
		return Entries;
	}

	TArray<UeAgentAuthoringState::FStateEntry> MakeLargeLevelEntries()
	{
		const FString AssetPath = TEXT("/Game/HX013/L_LargeRoundTrip");
		TArray<UeAgentAuthoringState::FStateEntry> Entries{
			MakeTargetEntry(
				TEXT("uai.semantic.level.world.target"),
				TEXT("level_world"),
				AssetPath),
		};
		const FString Payload = FString::ChrN(256, TEXT('l'));
		for (int32 Index = 0; Index < 384; ++Index)
		{
			const FString ActorGuid = FString::Printf(
				TEXT("%032x"),
				Index + 1);
			TSharedRef<FJsonObject> Actor = MakeShared<FJsonObject>();
			Actor->SetStringField(TEXT("actor_guid"), ActorGuid);
			Actor->SetStringField(
				TEXT("actor_class"),
				TEXT("/Script/Engine.StaticMeshActor"));
			Entries.Add(MakeEntry(
				TEXT("uai.semantic.level.actor.target"),
				TEXT("level_actor"),
				TEXT("actor:") + ActorGuid,
				{
					{TEXT("asset_path"), AssetPath},
					{TEXT("actor_guid"), ActorGuid},
				},
				MakeShared<FJsonValueObject>(Actor)));

			TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("label"), Payload);
			Entries.Add(MakeEntry(
				TEXT("uai.semantic.level.actor.property"),
				TEXT("level_actor_property"),
				TEXT("actor-property:") + ActorGuid,
				{
					{TEXT("asset_path"), AssetPath},
					{TEXT("actor_guid"), ActorGuid},
					{TEXT("property_path"), TEXT("ActorLabel")},
				},
				MakeShared<FJsonValueObject>(Property)));
		}
		return Entries;
	}

	TArray<UeAgentAuthoringState::FStateEntry> MakeDeepSequenceEntries()
	{
		const FString AssetPath = TEXT("/Game/HX013/LS_DeepRoundTrip");
		TSharedRef<FJsonObject> Current = MakeShared<FJsonObject>();
		Current->SetNumberField(TEXT("frame"), 240);
		Current->SetNumberField(TEXT("value"), 0.75);
		for (int32 Depth = 0; Depth < 768; ++Depth)
		{
			TSharedRef<FJsonObject> Parent = MakeShared<FJsonObject>();
			Parent->SetStringField(
				TEXT("id"),
				FString::Printf(TEXT("timeline-%04d"), Depth));
			Parent->SetObjectField(TEXT("children"), Current);
			Current = Parent;
		}
		TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetNumberField(TEXT("display_rate"), 24);
		Document->SetObjectField(TEXT("movie_scene"), Current);
		return {
			MakeTargetEntry(
				TEXT("uai.semantic.reflection.sequence.export.target"),
				TEXT("sequence"),
				AssetPath),
			MakeEntry(
				TEXT("uai.semantic.reflection.sequence.document"),
				TEXT("sequence"),
				TEXT("sequence:") + AssetPath,
				{{TEXT("asset_path"), AssetPath}},
				MakeShared<FJsonValueObject>(Document)),
		};
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

	class FScopedLargeWorkspace final
	{
	public:
		explicit FScopedLargeWorkspace(const FString& Label)
			: Root(FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("UAI/Tests/HX013/LargeRoundTrip"),
				Label + TEXT("-")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FScopedLargeWorkspace()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString Root;
	};

	bool RunPersistedSemanticRoundTrip(
		FAutomationTestBase& Test,
		const FString& Label,
		const FString& TargetKind,
		TArray<UeAgentAuthoringState::FStateEntry> Entries,
		const bool bMustExceedHistoricalCeiling,
		FAuthoringModelPtr* OutSourceModel = nullptr)
	{
		FCompositionRequest Request;
		Request.TargetKind = TargetKind;
		Request.ProfileId = TEXT("llm_authoring");
		Request.BaseRevision = TEXT("fixture:hx013:") + Label;
		Request.SourceProvenance = TEXT("fixture:hx013:large-round-trip");
		Request.Entries = MoveTemp(Entries);

		FUeAgentResourceTracker ComposeResources =
			MakeLargeRoundTripTracker();
		FAuthoringModelPtr SourceModel;
		FWorkspaceError Error;
		if (!Test.TestTrue(
				Label + TEXT(" composes"),
				TryComposeAuthoringModel(
					Request,
					ComposeResources,
					SourceModel,
					Error))
			|| !Test.TestTrue(
				Label + TEXT(" has a source model"),
				SourceModel.IsValid()))
		{
			return false;
		}

		FUeAgentResourceTracker PlanResources =
			MakeLargeRoundTripTracker();
		FExportWorkspacePlanPtr Plan;
		if (!Test.TestTrue(
				Label + TEXT(" plans without a token hard gate"),
				TryBuildCompositionExportPlan(
					SourceModel.ToSharedRef(),
					{{TEXT("estimated_tokens"), 16777216}},
					PlanResources,
					Plan,
					Error))
			|| !Test.TestTrue(
				Label + TEXT(" has a complete plan"),
				Plan.IsValid()))
		{
			return false;
		}
		if (bMustExceedHistoricalCeiling)
		{
			Test.TestTrue(
				Label + TEXT(" actually exceeds the historical complete-export ceiling"),
				Plan->GetObservedTokens() > HistoricalCompleteExportCeiling);
		}
		for (const FExportFilePlan& File : Plan->GetFiles())
		{
			Test.TestFalse(
				Label + TEXT(" has no legacy provider-extension path"),
				File.GetRelativePath().Contains(TEXT("provider_extension")));
			Test.TestFalse(
				Label + TEXT(" has no active legacy index file"),
				File.GetRelativePath() == TEXT("index.hjson"));
		}

		FScopedLargeWorkspace Workspace(Label);
		if (!Test.TestTrue(
				Label + TEXT(" persists every planned unit"),
				WritePlan(*Plan, Workspace.Root)))
		{
			return false;
		}
		FUeAgentResourceTracker ResolveResources =
			MakeLargeRoundTripTracker();
		FResolvedSemanticWorkspacePtr Resolved;
		if (!Test.TestTrue(
				Label + TEXT(" independently reparses from persisted files"),
				TryResolveSemanticWorkspace(
					FWorkspaceSource::Directory(Workspace.Root),
					ESemanticWorkspaceMode::State,
					ResolveResources,
					Resolved,
					Error))
			|| !Test.TestTrue(
				Label + TEXT(" returns a resolved workspace"),
				Resolved.IsValid()))
		{
			return false;
		}
		Test.TestEqual(
			Label + TEXT(" preserves the semantic fingerprint"),
			Resolved->GetModel()->GetSemanticFingerprint(),
			SourceModel->GetSemanticFingerprint());
		Test.TestEqual(
			Label + TEXT(" preserves every semantic unit"),
			Resolved->GetModel()->GetUnits().Num(),
			SourceModel->GetUnits().Num());

		TSet<FString> UnitIds;
		for (const FSemanticUnit& Unit : Resolved->GetModel()->GetUnits())
		{
			const bool bAlreadyPresent =
				UnitIds.Contains(Unit.GetUnitId());
			UnitIds.Add(Unit.GetUnitId());
			Test.TestFalse(
				Label + TEXT(" has no duplicate semantic unit id"),
				bAlreadyPresent);
			Test.TestTrue(
				Label + TEXT(" does not reconstruct an opaque extension"),
				Unit.GetUnitKind() != ESemanticUnitKind::OpaqueExtension);
		}
		for (const FSemanticUnit& Unit : Resolved->GetModel()->GetUnits())
		{
			if (!Unit.GetOwnerRef().IsEmpty())
			{
				Test.TestTrue(
					Label + TEXT(" owner resolves inside the same model"),
					UnitIds.Contains(Unit.GetOwnerRef()));
			}
		}
		if (OutSourceModel)
		{
			*OutSourceModel = SourceModel;
		}
		return !Test.HasAnyErrors();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticWorkspaceLargeCrossDomainRoundTripTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticWorkspace.LargeCrossDomainRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceLargeCrossDomainRoundTripTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	RunPersistedSemanticRoundTrip(
		*this,
		TEXT("blueprint"),
		TEXT("ue.blueprint.asset"),
		MakeLargeBlueprintEntries(),
		true);
	RunPersistedSemanticRoundTrip(
		*this,
		TEXT("niagara"),
		TEXT("ue.niagara_system.asset"),
		MakeLargeNiagaraEntries(),
		false);
	RunPersistedSemanticRoundTrip(
		*this,
		TEXT("level"),
		TEXT("ue.level.asset"),
		MakeLargeLevelEntries(),
		false);
	RunPersistedSemanticRoundTrip(
		*this,
		TEXT("sequence"),
		TEXT("ue.level_sequence.asset"),
		MakeDeepSequenceEntries(),
		false);
	return !HasAnyErrors();
}
