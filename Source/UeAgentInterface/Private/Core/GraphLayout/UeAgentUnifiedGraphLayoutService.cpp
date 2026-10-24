#include "Core/GraphLayout/UeAgentUnifiedGraphLayoutService.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"

#include "Animation/AnimBlueprint.h"
#include "Core/GraphLayout/BlueprintAutoLayout/UeAgentBlueprintGraphLayoutOps.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/GraphLayout/BlueprintAutoLayout/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
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
			// Frozen by the service after target resolution; never supplied by or
			// projected to the model.
			FString FrozenRevision;
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
			TMap<FString, FString> ExpectedNodeNames;
		};

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request,
            UE::UAI::Contract::FDiagnostic Diagnostic, const FString& EnvelopeCode = FString(), const FString& Subject = FString())
        {
            if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("graph_layout");
            if (Diagnostic.TargetPath.IsEmpty() && Subject.StartsWith(TEXT("/"))) Diagnostic.TargetPath = Subject;
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Diagnostic, EnvelopeCode);
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

		bool TryReadOwnerRevision(
			UBlueprint& Blueprint,
			FString& OutRevision,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutRevision.Reset();
			OutError.Reset();
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadError CaptureError;
			if (!DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadProductionEnvironment::
				TryCaptureBlueprintRevision(Blueprint, OutRevision, CaptureError))
			{
				auto Diagnostic = CaptureError;
				if (Diagnostic.TargetPath.IsEmpty())
				{
					Diagnostic.TargetPath = CaptureError.SubjectId.StartsWith(TEXT("/"))
						? CaptureError.SubjectId : Blueprint.GetPathName();
				}
				OutError = Diagnostic;
				return false;
			}
			if (OutRevision.IsEmpty())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_revision_missing"), TEXT("The graph owner did not provide a state revision."), TEXT("revision_capture"));
				OutError.TargetPath = Blueprint.GetPathName(); OutError.SourceKind = TEXT("native_result");
				return false;
			}
			return true;
		}

		UE::UAI::Contract::FDiagnostic ParameterFailure(const FString& Code, const FString& Field,
			const FString& Message)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Message, TEXT("parameters"));
			Diagnostic.PropertyPath = Field;
			Diagnostic.SourceKind = TEXT("command_input");
			return Diagnostic;
		}

		bool ParseOptions(
			const TSharedPtr<FJsonObject>& Params,
			UeAgentBlueprintAutoLayout::FOptions& OutOptions,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TSharedPtr<FJsonObject>* Parameters = nullptr;
			if (!Params.IsValid()
				|| !Params->TryGetObjectField(TEXT("parameters"), Parameters)
				|| !Parameters || !Parameters->IsValid())
			{
				OutError = ParameterFailure(TEXT("unified_graph_layout_contract_invalid"), TEXT("parameters"),
					TEXT("parameters must be an object."));
				OutError.Source.JsonPath = TEXT("/parameters"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				OutError.ActualValue = Params ? Params->TryGetField(TEXT("parameters")) : nullptr; OutError.ActualValuePresent = Params && Params->HasField(TEXT("parameters"));
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
					OutError = ParameterFailure(TEXT("unified_graph_layout_parameter_unsupported"), TEXT("parameters.") + Pair.Key,
						TEXT("Unknown layout parameter. Use horizontal_spacing, vertical_spacing, node_padding, origin_x or origin_y."));
					OutError.ActualValue = Pair.Value;
					return false;
				}
			}
			const auto ReadNumber = [&](const TCHAR* Field, double& Value) -> bool
			{
				if (!(*Parameters)->HasField(Field)) return true;
				if (!(*Parameters)->TryGetNumberField(Field, Value))
				{
					OutError = ParameterFailure(TEXT("unified_graph_layout_parameter_invalid"), FString(TEXT("parameters.")) + Field,
						TEXT("The supplied layout parameter must be a number."));
					OutError.Source.JsonPath = FString(TEXT("/parameters/")) + Field; OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("number")); OutError.ActualValue = (*Parameters)->TryGetField(Field);
					return false;
				}
				const bool bSpacing = FCString::Strcmp(Field, TEXT("horizontal_spacing")) == 0
					|| FCString::Strcmp(Field, TEXT("vertical_spacing")) == 0;
				const bool bPadding = FCString::Strcmp(Field, TEXT("node_padding")) == 0;
				if (!FMath::IsFinite(Value) || (bSpacing && Value <= 0.0)
					|| (bPadding && Value < 0.0))
				{
					OutError = ParameterFailure(TEXT("unified_graph_layout_spacing_invalid"), FString(TEXT("parameters.")) + Field,
						!FMath::IsFinite(Value) ? TEXT("The layout parameter must be finite.") : bSpacing ? TEXT("Layout spacing must be positive.") : TEXT("Layout node padding must be non-negative."));
					OutError.Source.JsonPath = FString(TEXT("/parameters/")) + Field;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(bSpacing ? TEXT("finite number > 0") : bPadding ? TEXT("finite number >= 0") : TEXT("finite number"));
					OutError.ActualValue = FMath::IsFinite(Value) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value)));
					return false;
				}
				return true;
			};
			if (!ReadNumber(TEXT("horizontal_spacing"), OutOptions.HorizontalSpacing)
				|| !ReadNumber(TEXT("vertical_spacing"), OutOptions.VerticalSpacing)
				|| !ReadNumber(TEXT("node_padding"), OutOptions.NodePadding)
				|| !ReadNumber(TEXT("origin_x"), OutOptions.OriginX)
				|| !ReadNumber(TEXT("origin_y"), OutOptions.OriginY))
			{
				return false;
			}
			double AccessOffsetX = 0.0;
			if (!UeAgentBlueprintOps::TryComputeGraphEditorAccessOffsetX(
					AccessOffsetX))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_geometry_unavailable"), TEXT("The graph editor settings could not produce a usable spline access offset for layout."), TEXT("parameters")); OutError.SourceKind = TEXT("native_result");
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutTargets.Reset();
			OutBlueprint = nullptr;
			OutPackage = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Params.IsValid()
				|| !Params->TryGetArrayField(TEXT("targets"), Values)
				|| !Values || Values->IsEmpty())
			{
				OutError = ParameterFailure(TEXT("unified_graph_layout_targets_required"), TEXT("targets"),
					TEXT("targets must be a non-empty array of graph selections."));
				OutError.Source.JsonPath = TEXT("/targets"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty array")); OutError.ActualValue = Params ? Params->TryGetField(TEXT("targets")) : nullptr; OutError.ActualValuePresent = Params && Params->HasField(TEXT("targets"));
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
					)
				{
					OutError = ParameterFailure(TEXT("unified_graph_layout_target_invalid"),
						FString::Printf(TEXT("targets[%d].graph_identity"), OutTargets.Num()),
						TEXT("Each target must contain a non-empty graph_identity string."));
					OutError.Source.JsonPath = Object ? FString::Printf(TEXT("/targets/%d/graph_identity"), OutTargets.Num()) : FString::Printf(TEXT("/targets/%d"), OutTargets.Num());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Object ? TEXT("non-empty graph identity") : TEXT("graph selection object"));
					OutError.ActualEvidence = Object ? Object->TryGetField(TEXT("graph_identity")) : Value; OutError.ActualValuePresent = Object ? Object->HasField(TEXT("graph_identity")) : Value.IsValid();
					return false;
				}
				Target.Graph = Cast<UEdGraph>(StaticLoadObject(
					UEdGraph::StaticClass(), nullptr, *Target.Identity));
				if (!Target.Graph)
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_target_unsupported"), TEXT("The requested layout target could not be loaded as an Unreal graph."), TEXT("parameters"));
					OutError.Source.JsonPath = FString::Printf(TEXT("/targets/%d/graph_identity"), OutTargets.Num()); OutError.PropertyPath = TEXT("graph_identity");
					OutError.TargetPath = Target.Identity;
					return false;
				}
				Target.Identity = Target.Graph->GetPathName();
				if (Identities.Contains(Target.Identity))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_target_duplicate"), TEXT("The layout request selects the same graph more than once."), TEXT("parameters")); OutError.Source.JsonPath = FString::Printf(TEXT("/targets/%d/graph_identity"), OutTargets.Num());
					OutError.TargetPath = Target.Identity;
					return false;
				}
				Identities.Add(Target.Identity);
				if (!TryResolveProvider(
						*Target.Graph, Target.Blueprint, Target.ProviderId))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_provider_unsupported"), TEXT("No supported Blueprint layout provider owns the selected graph."), TEXT("parameters")); OutError.Source.JsonPath = FString::Printf(TEXT("/targets/%d/graph_identity"), OutTargets.Num()); OutError.ActualValue = MakeShared<FJsonValueString>(Target.Graph->GetClass()->GetName());
					OutError.TargetPath = Target.Identity;
					return false;
				}
				Target.Package = Target.Blueprint->GetOutermost();
				if (!Target.Package)
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_package_missing"), TEXT("The selected graph's owning Blueprint has no package for the layout transaction."), TEXT("parameters")); OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Target.Identity;
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
					OutError = ParameterFailure(TEXT("unified_graph_layout_cross_package_atomicity_unsupported"), TEXT("targets"),
						TEXT("All targets must belong to the same owning Blueprint asset and package."));
					OutError.TargetPath = Target.Identity;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(OutBlueprint->GetPathName()); OutError.ActualValue = MakeShared<FJsonValueString>(Target.Blueprint->GetPathName());
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
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
			TSet<FString> ObservedKeys;
			for (const UEdGraphNode* Node : SortedNodes)
			{
				const FString Key = NodeKey(*Node);
				ObservedKeys.Add(Key);
				const FIntPoint* Expected = Target.ExpectedAfterPositions.Find(Key);
				if (!Expected
					|| Expected->X != Node->NodePosX
					|| Expected->Y != Node->NodePosY)
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_independent_readback_failed"), Expected ? TEXT("The persisted node position differs from the applied layout.") : TEXT("The persisted graph contains a node absent from the applied layout."), TEXT("graph_layout"), Key);
					OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target.Identity; OutError.CandidateName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
					OutError.PropertyPath = TEXT("node_position"); OutError.ExpectedValuePresent = Expected != nullptr;
					if (Expected) OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected->ToString());
					OutError.ActualValue = MakeShared<FJsonValueString>(FIntPoint(Node->NodePosX, Node->NodePosY).ToString());
					return nullptr;
				}
			}
			for (const TPair<FString, FIntPoint>& Expected : Target.ExpectedAfterPositions)
			{
				if (!ObservedKeys.Contains(Expected.Key))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_independent_readback_failed"), TEXT("A node from the applied layout is missing after package reload."), TEXT("graph_layout"), Expected.Key);
					OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target.Identity; OutError.PropertyPath = TEXT("node_position");
					OutError.CandidateName = Target.ExpectedNodeNames.FindRef(Expected.Key);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected.Value.ToString()); OutError.ActualValuePresent = false;
					return nullptr;
				}
			}
			Object->SetNumberField(TEXT("node_count"), SortedNodes.Num());
			return Object;
		}
	}

	FUeAgentCommandResult Execute(const FUeAgentCommandRequest& Request)
	{
        bool bMutationStarted = false;
        bool bPersisted = false;
        bool bRollbackAttempted = false;
        auto Fail = [&](const FUeAgentCommandRequest& CommandRequest, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& EnvelopeCode = FString(), const FString& Subject = FString())
        {
            Diagnostic.MutationStarted = bMutationStarted;
            if (bMutationStarted) Diagnostic.bRetrySafe = false;
            if (bPersisted) Diagnostic.TransactionStatus = TEXT("committed");
            if (bRollbackAttempted) Diagnostic.RollbackAttempted = true;
            return Failure(CommandRequest, MoveTemp(Diagnostic), EnvelopeCode, Subject);
        };

		if (!IsInGameThread())
		{
			return Fail(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_game_thread_required"), TEXT("Graph layout must run on the editor game thread."), TEXT("graph_layout")), TEXT("unified_graph_layout_game_thread_required"), CommandId);
		}
		const TSharedPtr<FJsonObject>& Params = Request.GetParams();
		FString Strategy;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("strategy"), Strategy)
			|| Strategy != StrategyId)
		{
			auto Error = ParameterFailure(TEXT("unified_graph_layout_contract_invalid"), TEXT("strategy"), TEXT("strategy must be hierarchical_left_to_right."));
			Error.Source.JsonPath = TEXT("/strategy"); Error.ExpectedValue = MakeShared<FJsonValueString>(StrategyId); Error.ActualValue = Params ? Params->TryGetField(TEXT("strategy")) : nullptr; Error.ActualValuePresent = Params && Params->HasField(TEXT("strategy"));
			return Fail(Request, MoveTemp(Error), TEXT("unified_graph_layout_contract_invalid"), CommandId);
		}

		UeAgentBlueprintAutoLayout::FOptions Options;
		UE::UAI::Contract::FDiagnostic Detail;
		if (!ParseOptions(Params, Options, Detail))
		{
			return Fail(Request, Detail, FString(), CommandId);
		}

		TArray<FTarget> Targets;
		UBlueprint* Blueprint = nullptr;
		UPackage* Package = nullptr;
		if (!ParseTargets(Params, Targets, Blueprint, Package,
				Detail))
		{
			return Fail(Request, Detail);
		}

		FString CurrentRevision;
		if (!TryReadOwnerRevision(*Blueprint, CurrentRevision, Detail))
		{
			return Fail(Request, Detail, TEXT("unified_graph_layout_public_revision_unavailable"), Blueprint->GetPathName());
		}
		for (FTarget& Target : Targets)
		{
			Target.FrozenRevision = CurrentRevision;
		}

		for (FTarget& Target : Targets)
		{
			Target.Model = UeAgentBlueprintOps::CollectGraphLayoutModel(Target.Graph);
			UE::UAI::Contract::FDiagnostic GeometryError;
			if (!UeAgentBlueprintOps::CaptureGraphLayoutRealGeometry(
					Blueprint, Target.Graph, Target.Model, Target.Geometry,
					false, false, GeometryError))
			{
				return Fail(Request, GeometryError, TEXT("unified_graph_layout_geometry_unavailable"), Target.Identity);
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
				return Fail(Request, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("unified_graph_layout_algorithm_failed"), Target.Run.Error), TEXT("unified_graph_layout_algorithm_failed"), Target.Identity);
			}
			if (Target.Run.DesignDiagnostics.IslandLocalMembershipMutationCount > 0
				|| Target.Run.DesignDiagnostics.IslandLocalMutationCount > 0)
			{
				auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_algorithm_invariant_failed"), FString::Printf(TEXT("Layout computation changed graph island membership %d times and island state %d times; both counts must be zero."), Target.Run.DesignDiagnostics.IslandLocalMembershipMutationCount, Target.Run.DesignDiagnostics.IslandLocalMutationCount), TEXT("graph_layout"));
				Error.SourceKind = TEXT("native_result");
				return Fail(Request, MoveTemp(Error), TEXT("unified_graph_layout_algorithm_invariant_failed"), Target.Identity);
			}
			CapturePositions(*Target.Graph, Target.BeforePositions);
		}

		// Freeze the target revision during resolution and re-read it at the
		// last safe point before any graph mutation.
		FString PreWriteRevision;
		if (!TryReadOwnerRevision(*Blueprint, PreWriteRevision, Detail))
		{
			return Fail(Request, Detail, TEXT("unified_graph_layout_public_revision_unavailable"), Blueprint->GetPathName());
		}
		for (const FTarget& Target : Targets)
		{
			if (Target.FrozenRevision != PreWriteRevision)
			{
				return Fail(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_stale_revision"), TEXT("Target state changed before the first write; no layout changes were applied."), TEXT("graph_layout"), TEXT("")), TEXT("unified_graph_layout_stale_revision"), Target.Identity);
			}
		}

		TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(
			NSLOCTEXT("UeAgentInterface", "UnifiedGraphLayoutTransaction",
				"UeAgentInterface Unified Graph Layout"));
		bMutationStarted = true;
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
			for (const UEdGraphNode* Node : Target.Graph->Nodes)
			{
				if (Node) Target.ExpectedNodeNames.Add(NodeKey(*Node), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		FCompilerResultsLog CompilerLog;
		CompilerLog.bLogInfoOnly = false;
		FKismetEditorUtilities::CompileBlueprint(
			Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
		if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error)
		{
			auto CompileDetail = UeAgentAuthoringDiagnostics::MakeCompileFailure(
				TEXT("unified_graph_layout_compile_failed"), Blueprint->GetPathName(),
				CompilerLog, Blueprint);
			Rollback(Targets, Blueprint);
            bRollbackAttempted = true;
			Transaction->Cancel();
			return Fail(Request, CompileDetail, TEXT("unified_graph_layout_compile_failed"), Blueprint->GetPathName());
		}
		Package->MarkPackageDirty();
		const TArray<UPackage*> Packages = {Package};
		UE::UAI::Contract::FDiagnostic SaveError;
		if (!UE::UAI::EditorResources::SavePackages(Packages, true, SaveError))
		{
			Rollback(Targets, Blueprint);
            bRollbackAttempted = true;
			Transaction->Cancel();
			return Fail(Request, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("unified_graph_layout_save_failed"), SaveError, TEXT("graph_layout"), TEXT("The native package save failed; node positions were restored in memory.")), TEXT("unified_graph_layout_save_failed"), Package->GetName());
		}
		bPersisted = true;
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
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_reload_failed_state_unknown"), TEXT("The package was saved but could not be reloaded; inspect the target before retrying."), TEXT("graph_layout"), ReloadError.ToString());
			Error.SourceKind = TEXT("native_result");
			return Fail(Request, MoveTemp(Error), TEXT("unified_graph_layout_reload_failed_state_unknown"), PackageIdentity);
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
				const FString Reason = !Target.Graph ? TEXT("The saved graph could not be loaded after package reload.")
					: !Target.Blueprint ? TEXT("The reloaded graph has no supported Blueprint owner.")
					: Target.Blueprint->GetPathName() != BlueprintIdentity ? TEXT("The reloaded graph belongs to a different Blueprint.") : TEXT("The reloaded graph uses a different layout provider.");
				auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_independent_readback_failed"), Reason, TEXT("graph_layout"));
				Error.SourceKind = TEXT("native_result");
				if (Target.Blueprint && Target.Blueprint->GetPathName() != BlueprintIdentity)
				{
					Error.PropertyPath = TEXT("graph_owner"); Error.ExpectedValue = MakeShared<FJsonValueString>(BlueprintIdentity); Error.ActualValue = MakeShared<FJsonValueString>(Target.Blueprint->GetPathName());
				}
				else if (Target.Blueprint)
				{
					Error.PropertyPath = TEXT("layout_provider"); Error.ExpectedValue = MakeShared<FJsonValueString>(Target.ProviderId); Error.ActualValue = MakeShared<FJsonValueString>(ReadbackProvider);
				}
				return Fail(Request, MoveTemp(Error), TEXT("unified_graph_layout_independent_readback_failed"), Target.Identity);
			}
			if (!ReloadedBlueprint)
			{
				ReloadedBlueprint = Target.Blueprint;
			}
		}
		if (!ReloadedBlueprint || ReloadedBlueprint->Status == BS_Error)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unified_graph_layout_independent_readback_failed"), !ReloadedBlueprint ? TEXT("No Blueprint was recovered after package reload.") : TEXT("The reloaded Blueprint reports a compile error."), TEXT("graph_layout"));
			Error.SourceKind = TEXT("native_result");
			return Fail(Request, MoveTemp(Error), TEXT("unified_graph_layout_independent_readback_failed"), BlueprintIdentity);
		}

		FString ReadbackRevision;
		if (!TryReadOwnerRevision(*ReloadedBlueprint, ReadbackRevision, Detail))
		{
			return Fail(Request, Detail, TEXT("unified_graph_layout_independent_readback_failed"), BlueprintIdentity);
		}

		TArray<TSharedPtr<FJsonValue>> Results;
		for (const FTarget& Target : Targets)
		{
			UE::UAI::Contract::FDiagnostic ReadbackError;
			const TSharedPtr<FJsonObject> Captured =
				CaptureReadback(Target, ReadbackError);
			if (!Captured.IsValid())
			{
				return Fail(Request, MoveTemp(ReadbackError), TEXT("unified_graph_layout_independent_readback_failed"), Target.Identity);
			}
			Results.Add(MakeShared<FJsonValueObject>(Captured));
		}

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
		Payload->SetArrayField(TEXT("layout_summary"), MoveTemp(Results));
		FString PayloadJson;
		if (!CommandContract::Json::TryCanonicalize(Payload, PayloadJson, Detail))
		{
			return Fail(Request, Detail, TEXT("unified_graph_layout_result_invalid"), CommandId);
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson));
	}
}
