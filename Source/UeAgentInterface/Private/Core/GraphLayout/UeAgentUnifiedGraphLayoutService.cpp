#include "Core/GraphLayout/UeAgentUnifiedGraphLayoutService.h"

#include "Animation/AnimBlueprint.h"
#include "Commands/Blueprint/UeAgentBlueprintGraphLayoutOps.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/GraphLayout/BlueprintAutoLayout/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "DomainAdapters/AnimationBlueprint/UeAgentAnimationBlueprintDomainMechanics.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintSurfaceMechanics.h"
#include "DomainAdapters/UmgWidget/UeAgentUmgWidgetMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "PackageTools.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::Core::GraphLayout
{
	using namespace CommandEdge;

	namespace
	{
		const FString CommandId(TEXT("graph.layout.apply"));
		const FString Owner(TEXT("unified-graph-layout-owner"));
		const FString StrategyId(TEXT("hierarchical_left_to_right"));

		struct FTarget final
		{
			FString Identity;
			FString ExpectedRevision;
			FString ProviderId;
			UEdGraph* Graph = nullptr;
			UBlueprint* Blueprint = nullptr;
			UPackage* Package = nullptr;
			UeAgentBlueprintOps::FGraphLayoutModel Model;
			UeAgentBlueprintAutoLayout::FRunResult Run;
			UeAgentBlueprintOps::FGraphLayoutGeometryStats Geometry;
			int32 AppliedMoveCount = 0;
			TMap<FString, FIntPoint> BeforePositions;
			TMap<FString, FIntPoint> ExpectedAfterPositions;
		};

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& Subject,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Subject);
			Error->SetStringField(TEXT("detail"), Detail);
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Text);
		}

		bool Canonical(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutText)
		{
			FString Error;
			return CommandContract::Json::TryCanonicalize(Object, OutText, Error);
		}

		FString NodeKey(const UEdGraphNode& Node)
		{
			return Node.NodeGuid.IsValid()
				? Node.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
				: Node.GetPathName();
		}

		void CapturePositions(
			const UEdGraph& Graph,
			TMap<FString, FIntPoint>& OutPositions)
		{
			OutPositions.Reset();
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (Node)
				{
					OutPositions.Add(NodeKey(*Node),
						FIntPoint(Node->NodePosX, Node->NodePosY));
				}
			}
		}

		bool TryResolveProvider(
			UEdGraph& Graph,
			UBlueprint*& OutBlueprint,
			FString& OutProviderId)
		{
			OutBlueprint = FBlueprintEditorUtils::FindBlueprintForGraph(&Graph);
			OutProviderId.Reset();
			if (!OutBlueprint)
			{
				return false;
			}
			if (OutBlueprint->IsA<UAnimBlueprint>())
			{
				OutProviderId = TEXT("animation-blueprint-graph-layout-provider");
			}
			else if (OutBlueprint->IsA<UWidgetBlueprint>())
			{
				OutProviderId = TEXT("umg-blueprint-graph-layout-provider");
			}
			else if (OutBlueprint->IsA<UBlueprint>())
			{
				OutProviderId = TEXT("blueprint-graph-layout-provider");
			}
			return !OutProviderId.IsEmpty();
		}

		bool TryReadPublicRevision(
			UBlueprint& Blueprint,
			const FString& ProviderId,
			FString& OutRevision,
			FString& OutDetail)
		{
			OutRevision.Reset();
			OutDetail.Reset();
			const FString Target = Blueprint.GetPathName();
			const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("target"), Target);
			FString ParamsJson;
			if (!Canonical(Params, ParamsJson))
			{
				OutDetail = TEXT("could not canonicalize public read params");
				return false;
			}

			FString PayloadJson;
			bool bReadOnly = false;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			bool bRead = false;
			if (ProviderId == TEXT("animation-blueprint-graph-layout-provider"))
			{
				bRead = DomainAdapters::AnimationBlueprint::TryExecuteContentGraph(
					TEXT("content_graph.summary"), Target, ParamsJson,
					PayloadJson, bReadOnly, WorkspaceError);
			}
			else if (ProviderId == TEXT("umg-blueprint-graph-layout-provider"))
			{
				bRead = DomainAdapters::UmgWidget::TryExecuteContentGraph(
					TEXT("content_graph.summary"), Target, ParamsJson,
					PayloadJson, bReadOnly, WorkspaceError);
			}
			else if (ProviderId == TEXT("blueprint-graph-layout-provider"))
			{
				bRead = DomainAdapters::Blueprint::Surfaces::TryExecuteContentGraph(
					TEXT("content_graph.summary"), Target, ParamsJson,
					PayloadJson, bReadOnly, WorkspaceError);
			}
			if (!bRead || !bReadOnly)
			{
				OutDetail = WorkspaceError.GetCode().IsEmpty()
					? FString(TEXT("public content graph read failed"))
					: WorkspaceError.GetCode();
				return false;
			}

			TSharedPtr<FJsonObject> Payload;
			FString JsonError;
			if (!CommandContract::Json::TryParseObject(
					PayloadJson, Payload, JsonError)
				|| !Payload.IsValid()
				|| !Payload->TryGetStringField(TEXT("revision"), OutRevision)
				|| OutRevision.IsEmpty())
			{
				OutDetail = JsonError.IsEmpty()
					? FString(TEXT("public content graph revision missing"))
					: JsonError;
				return false;
			}
			return true;
		}

		bool ParseOptions(
			const TSharedPtr<FJsonObject>& Params,
			UeAgentBlueprintAutoLayout::FOptions& OutOptions,
			FString& OutCode,
			FString& OutDetail)
		{
			const TSharedPtr<FJsonObject>* Parameters = nullptr;
			if (!Params.IsValid()
				|| !Params->TryGetObjectField(TEXT("parameters"), Parameters)
				|| !Parameters || !Parameters->IsValid())
			{
				OutCode = TEXT("unified_graph_layout_contract_invalid");
				OutDetail = TEXT("parameters must be an object");
				return false;
			}
			const TSet<FString> Supported = {
				TEXT("horizontal_spacing"), TEXT("vertical_spacing"),
				TEXT("node_padding"), TEXT("origin_x"), TEXT("origin_y")};
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
				: (*Parameters)->Values)
			{
				if (!Supported.Contains(Pair.Key))
				{
					OutCode = TEXT("unified_graph_layout_parameter_unsupported");
					OutDetail = Pair.Key;
					return false;
				}
			}
			(*Parameters)->TryGetNumberField(
				TEXT("horizontal_spacing"), OutOptions.HorizontalSpacing);
			(*Parameters)->TryGetNumberField(
				TEXT("vertical_spacing"), OutOptions.VerticalSpacing);
			(*Parameters)->TryGetNumberField(
				TEXT("node_padding"), OutOptions.NodePadding);
			(*Parameters)->TryGetNumberField(TEXT("origin_x"), OutOptions.OriginX);
			(*Parameters)->TryGetNumberField(TEXT("origin_y"), OutOptions.OriginY);
			if (!FMath::IsFinite(OutOptions.HorizontalSpacing)
				|| !FMath::IsFinite(OutOptions.VerticalSpacing)
				|| !FMath::IsFinite(OutOptions.NodePadding)
				|| !FMath::IsFinite(OutOptions.OriginX)
				|| !FMath::IsFinite(OutOptions.OriginY)
				|| OutOptions.HorizontalSpacing <= 0.0
				|| OutOptions.VerticalSpacing <= 0.0
				|| OutOptions.NodePadding < 0.0)
			{
				OutCode = TEXT("unified_graph_layout_spacing_invalid");
				OutDetail = TEXT("spacing must be finite and positive; padding must be non-negative");
				return false;
			}
			double AccessOffsetX = 0.0;
			if (!UeAgentBlueprintOps::TryComputeGraphEditorAccessOffsetX(
					AccessOffsetX))
			{
				OutCode = TEXT("unified_graph_layout_geometry_unavailable");
				OutDetail = TEXT("graph editor access offset is unavailable");
				return false;
			}
			OutOptions.OutputAccessOffsetX = AccessOffsetX;
			OutOptions.InputAccessOffsetX = AccessOffsetX;
			OutOptions.AccessOffsetSource =
				TEXT("graph_editor_settings_compute_spline_tangent");
			return true;
		}

		bool ParseTargets(
			const TSharedPtr<FJsonObject>& Params,
			TArray<FTarget>& OutTargets,
			UBlueprint*& OutBlueprint,
			UPackage*& OutPackage,
			FString& OutCode,
			FString& OutSubject,
			FString& OutDetail)
		{
			OutTargets.Reset();
			OutBlueprint = nullptr;
			OutPackage = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Params.IsValid()
				|| !Params->TryGetArrayField(TEXT("targets"), Values)
				|| !Values || Values->IsEmpty())
			{
				OutCode = TEXT("unified_graph_layout_targets_required");
				return false;
			}
			TSet<FString> Identities;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> Object = Value.IsValid()
					? Value->AsObject() : nullptr;
				FTarget Target;
				if (!Object.IsValid()
					|| !Object->TryGetStringField(
						TEXT("graph_identity"), Target.Identity)
					|| Target.Identity.IsEmpty()
					|| !Object->TryGetStringField(
						TEXT("expected_revision"), Target.ExpectedRevision)
					|| Target.ExpectedRevision.IsEmpty())
				{
					OutCode = TEXT("unified_graph_layout_target_invalid");
					return false;
				}
				Target.Graph = Cast<UEdGraph>(StaticLoadObject(
					UEdGraph::StaticClass(), nullptr, *Target.Identity));
				if (!Target.Graph)
				{
					OutCode = TEXT("unified_graph_layout_target_unsupported");
					OutSubject = Target.Identity;
					return false;
				}
				Target.Identity = Target.Graph->GetPathName();
				if (Identities.Contains(Target.Identity))
				{
					OutCode = TEXT("unified_graph_layout_target_duplicate");
					OutSubject = Target.Identity;
					return false;
				}
				Identities.Add(Target.Identity);
				if (!TryResolveProvider(
						*Target.Graph, Target.Blueprint, Target.ProviderId))
				{
					OutCode = TEXT("unified_graph_layout_provider_unsupported");
					OutSubject = Target.Identity;
					return false;
				}
				Target.Package = Target.Blueprint->GetOutermost();
				if (!Target.Package)
				{
					OutCode = TEXT("unified_graph_layout_package_missing");
					OutSubject = Target.Identity;
					return false;
				}
				if (!OutBlueprint)
				{
					OutBlueprint = Target.Blueprint;
					OutPackage = Target.Package;
				}
				else if (Target.Blueprint != OutBlueprint
					|| Target.Package != OutPackage)
				{
					OutCode = TEXT(
						"unified_graph_layout_cross_package_atomicity_unsupported");
					OutSubject = Target.Identity;
					OutDetail = TEXT(
						"all targets must belong to the same owning Blueprint asset and package");
					return false;
				}
				OutTargets.Add(MoveTemp(Target));
			}
			return true;
		}

		void Rollback(TArray<FTarget>& Targets, UBlueprint* Blueprint)
		{
			for (FTarget& Target : Targets)
			{
				if (!Target.Graph)
				{
					continue;
				}
				for (UEdGraphNode* Node : Target.Graph->Nodes)
				{
					if (!Node)
					{
						continue;
					}
					if (const FIntPoint* Before =
						Target.BeforePositions.Find(NodeKey(*Node)))
					{
						Node->NodePosX = Before->X;
						Node->NodePosY = Before->Y;
					}
				}
				Target.Graph->NotifyGraphChanged();
			}
			if (Blueprint)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				FKismetEditorUtilities::CompileBlueprint(Blueprint);
			}
		}

		TSharedPtr<FJsonObject> CaptureReadback(
			const FTarget& Target,
			const FString& Revision,
			FString& OutMismatch)
		{
			OutMismatch.Reset();
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("graph_identity"), Target.Identity);
			Object->SetStringField(TEXT("owner_identity"),
				Target.Blueprint ? Target.Blueprint->GetPathName() : FString());
			Object->SetStringField(TEXT("provider_id"), Target.ProviderId);
			Object->SetStringField(TEXT("revision"), Revision);
			Object->SetNumberField(TEXT("moved_node_count"),
				Target.Run.MovedNodeCount);
			Object->SetNumberField(TEXT("applied_move_count"),
				Target.AppliedMoveCount);
			Object->SetBoolField(TEXT("layout_design_complete"),
				Target.Run.bDesignComplete);
			Object->SetNumberField(TEXT("reroute_plan_count"),
				Target.Run.ReroutePlanCount);
			Object->SetStringField(TEXT("geometry_source"),
				Target.Geometry.GeometrySource);
			Object->SetBoolField(TEXT("geometry_used_offscreen_graph_editor"),
				Target.Geometry.bUsedOffscreenGraphEditor);
			Object->SetObjectField(TEXT("metrics_before"),
				UeAgentBlueprintAutoLayout::MetricsToJson(Target.Run.MetricsBefore));
			Object->SetObjectField(TEXT("metrics_after"),
				UeAgentBlueprintAutoLayout::MetricsToJson(Target.Run.MetricsAfter));

			TArray<UEdGraphNode*> SortedNodes;
			if (Target.Graph)
			{
				for (UEdGraphNode* Node : Target.Graph->Nodes)
				{
					if (Node)
					{
						SortedNodes.Add(Node);
					}
				}
			}
			SortedNodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
			{
				return NodeKey(Left) < NodeKey(Right);
			});
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const UEdGraphNode* Node : SortedNodes)
			{
				const FString Key = NodeKey(*Node);
				const FIntPoint* Expected = Target.ExpectedAfterPositions.Find(Key);
				if (!Expected
					|| Expected->X != Node->NodePosX
					|| Expected->Y != Node->NodePosY)
				{
					OutMismatch = Key;
					return nullptr;
				}
				const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("node_identity"), Node->GetPathName());
				Item->SetStringField(TEXT("node_guid"), Key);
				Item->SetNumberField(TEXT("x"), Node->NodePosX);
				Item->SetNumberField(TEXT("y"), Node->NodePosY);
				Nodes.Add(MakeShared<FJsonValueObject>(Item));
			}
			Object->SetNumberField(TEXT("node_count"), Nodes.Num());
			Object->SetArrayField(TEXT("nodes"), MoveTemp(Nodes));
			return Object;
		}
	}

	FUeAgentCommandResult Execute(const FUeAgentCommandRequest& Request)
	{
		if (!IsInGameThread())
		{
			return Failure(Request,
				TEXT("unified_graph_layout_game_thread_required"),
				CommandId, TEXT(""));
		}
		const TSharedPtr<FJsonObject>& Params = Request.GetParams();
		FString Strategy;
		FString Authorization;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("strategy"), Strategy)
			|| Strategy != StrategyId
			|| !Params->TryGetStringField(
				TEXT("authorization_id"), Authorization)
			|| Authorization.IsEmpty())
		{
			return Failure(Request,
				TEXT("unified_graph_layout_contract_invalid"),
				CommandId, Strategy);
		}

		UeAgentBlueprintAutoLayout::FOptions Options;
		FString Code;
		FString Subject;
		FString Detail;
		if (!ParseOptions(Params, Options, Code, Detail))
		{
			return Failure(Request, Code, CommandId, Detail);
		}

		TArray<FTarget> Targets;
		UBlueprint* Blueprint = nullptr;
		UPackage* Package = nullptr;
		if (!ParseTargets(Params, Targets, Blueprint, Package,
				Code, Subject, Detail))
		{
			return Failure(Request, Code, Subject, Detail);
		}

		FString CurrentRevision;
		if (!TryReadPublicRevision(
				*Blueprint, Targets[0].ProviderId, CurrentRevision, Detail))
		{
			return Failure(Request,
				TEXT("unified_graph_layout_public_revision_unavailable"),
				Blueprint->GetPathName(), Detail);
		}
		for (const FTarget& Target : Targets)
		{
			if (Target.ExpectedRevision != CurrentRevision)
			{
				return Failure(Request,
					TEXT("unified_graph_layout_stale_revision"),
					Target.Identity, CurrentRevision);
			}
		}

		for (FTarget& Target : Targets)
		{
			Target.Model = UeAgentBlueprintOps::CollectGraphLayoutModel(Target.Graph);
			FString GeometryError;
			if (!UeAgentBlueprintOps::CaptureGraphLayoutRealGeometry(
					Blueprint, Target.Graph, Target.Model, Target.Geometry,
					false, false, GeometryError))
			{
				return Failure(Request,
					TEXT("unified_graph_layout_geometry_unavailable"),
					Target.Identity, GeometryError);
			}
			UeAgentBlueprintAutoLayout::FOptions TargetOptions = Options;
			TargetOptions.bUseRealGeometry = Target.Geometry.bUsedRealGeometry;
			TargetOptions.MissingPinAnchorCount =
				Target.Geometry.MissingPinAnchorCount;
			TargetOptions.MissingMeasuredNodeGeometryCount = FMath::Max(
				0, Target.Geometry.NodeCount - Target.Geometry.MeasuredNodeCount);
			if (!UeAgentBlueprintAutoLayout::RunLayout(
					Target.Model.Nodes,
					Target.Model.Edges,
					TargetOptions,
					Target.Run))
			{
				return Failure(Request,
					TEXT("unified_graph_layout_algorithm_failed"),
					Target.Identity,
					TEXT("real-geometry topology layout rejected the graph"));
			}
			if (Target.Run.DesignDiagnostics.IslandLocalMembershipMutationCount > 0
				|| Target.Run.DesignDiagnostics.IslandLocalMutationCount > 0)
			{
				return Failure(Request,
					TEXT("unified_graph_layout_algorithm_invariant_failed"),
					Target.Identity,
					TEXT("island-local membership changed during layout"));
			}
			CapturePositions(*Target.Graph, Target.BeforePositions);
		}

		TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(
			NSLOCTEXT("UeAgentInterface", "UnifiedGraphLayoutTransaction",
				"UeAgentInterface Unified Graph Layout"));
		Blueprint->Modify();
		for (FTarget& Target : Targets)
		{
			Target.Graph->Modify();
			for (UEdGraphNode* Node : Target.Graph->Nodes)
			{
				if (Node)
				{
					Node->Modify();
				}
			}
			Target.AppliedMoveCount =
				UeAgentBlueprintOps::ApplyNodePositions(Target.Model);
			Target.Graph->NotifyGraphChanged();
			CapturePositions(*Target.Graph, Target.ExpectedAfterPositions);
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (Blueprint->Status == BS_Error)
		{
			Rollback(Targets, Blueprint);
			Transaction->Cancel();
			return Failure(Request,
				TEXT("unified_graph_layout_compile_failed"),
				Blueprint->GetPathName(), TEXT("Blueprint compile status is Error"));
		}
		Package->MarkPackageDirty();
		const TArray<UPackage*> Packages = {Package};
		if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, true))
		{
			Rollback(Targets, Blueprint);
			Transaction->Cancel();
			return Failure(Request,
				TEXT("unified_graph_layout_save_failed"),
				Package->GetName(), TEXT("package save returned false"));
		}
		Transaction.Reset();

		const FString BlueprintIdentity = Blueprint->GetPathName();
		const FString PackageIdentity = Package->GetName();
		for (FTarget& Target : Targets)
		{
			Target.Model = UeAgentBlueprintOps::FGraphLayoutModel();
			Target.Graph = nullptr;
			Target.Blueprint = nullptr;
			Target.Package = nullptr;
		}
		Blueprint = nullptr;
		FText ReloadError;
		if (!UPackageTools::ReloadPackages(
				Packages, ReloadError,
				EReloadPackagesInteractionMode::AssumeNegative))
		{
			return Failure(Request,
				TEXT("unified_graph_layout_reload_failed_state_unknown"),
				PackageIdentity, ReloadError.ToString());
		}

		UBlueprint* ReloadedBlueprint = nullptr;
		for (FTarget& Target : Targets)
		{
			Target.Graph = Cast<UEdGraph>(StaticLoadObject(
				UEdGraph::StaticClass(), nullptr, *Target.Identity));
			FString ReadbackProvider;
			if (!Target.Graph
				|| !TryResolveProvider(
					*Target.Graph, Target.Blueprint, ReadbackProvider)
				|| !Target.Blueprint
				|| Target.Blueprint->GetPathName() != BlueprintIdentity
				|| ReadbackProvider != Target.ProviderId)
			{
				return Failure(Request,
					TEXT("unified_graph_layout_independent_readback_failed"),
					Target.Identity,
					TEXT("graph owner/provider missing after package reload"));
			}
			if (!ReloadedBlueprint)
			{
				ReloadedBlueprint = Target.Blueprint;
			}
		}
		if (!ReloadedBlueprint || ReloadedBlueprint->Status == BS_Error)
		{
			return Failure(Request,
				TEXT("unified_graph_layout_independent_readback_failed"),
				BlueprintIdentity,
				TEXT("reloaded Blueprint compile status is Error"));
		}

		FString ReadbackRevision;
		if (!TryReadPublicRevision(*ReloadedBlueprint,
				Targets[0].ProviderId, ReadbackRevision, Detail))
		{
			return Failure(Request,
				TEXT("unified_graph_layout_independent_readback_failed"),
				BlueprintIdentity, Detail);
		}

		TArray<TSharedPtr<FJsonValue>> Results;
		TArray<TSharedPtr<FJsonValue>> ImpactTargets;
		for (const FTarget& Target : Targets)
		{
			FString Mismatch;
			const TSharedPtr<FJsonObject> Captured =
				CaptureReadback(Target, ReadbackRevision, Mismatch);
			if (!Captured.IsValid())
			{
				return Failure(Request,
					TEXT("unified_graph_layout_independent_readback_failed"),
					Target.Identity, Mismatch);
			}
			Results.Add(MakeShared<FJsonValueObject>(Captured));
			ImpactTargets.Add(MakeShared<FJsonValueString>(Target.Identity));
		}

		const TSharedPtr<FJsonObject> Impact = MakeShared<FJsonObject>();
		Impact->SetArrayField(TEXT("targets"), MoveTemp(ImpactTargets));
		Impact->SetStringField(TEXT("owner_identity"), BlueprintIdentity);
		Impact->SetStringField(TEXT("package_identity"), PackageIdentity);
		Impact->SetStringField(TEXT("authorization_id"), Authorization);
		Impact->SetStringField(TEXT("atomicity"),
			TEXT("single_package_preflight_with_pre_save_rollback"));
		Impact->SetBoolField(TEXT("cross_package_supported"), false);

		const TSharedPtr<FJsonObject> Persistence = MakeShared<FJsonObject>();
		Persistence->SetStringField(TEXT("receipt_id"),
			TEXT("reload:") + BlueprintIdentity + TEXT(":") + ReadbackRevision);
		Persistence->SetStringField(TEXT("package_identity"), PackageIdentity);
		Persistence->SetStringField(TEXT("revision"), ReadbackRevision);
		Persistence->SetBoolField(TEXT("saved"), true);
		Persistence->SetBoolField(TEXT("reloaded"), true);

		const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("command_id"), CommandId);
		Payload->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
		Payload->SetStringField(TEXT("owner"), Owner);
		Payload->SetStringField(TEXT("strategy"), Strategy);
		Payload->SetNumberField(TEXT("target_count"), Targets.Num());
		Payload->SetBoolField(TEXT("saved"), true);
		Payload->SetBoolField(TEXT("reloaded"), true);
		Payload->SetBoolField(TEXT("compile_settled"), true);
		Payload->SetBoolField(TEXT("settled"), true);
		Payload->SetObjectField(TEXT("persistence_receipt"), Persistence);
		Payload->SetObjectField(TEXT("impact_plan"), Impact);
		Payload->SetArrayField(TEXT("independent_readback"), MoveTemp(Results));
		FString PayloadJson;
		if (!Canonical(Payload, PayloadJson))
		{
			return Failure(Request,
				TEXT("unified_graph_layout_result_invalid"),
				CommandId, TEXT("could not canonicalize result"));
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson));
	}
}
