// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"

#include "Commands/Blueprint/Layout/Preprocess/UeAgentBlueprintGraphLayoutPreprocess.inl"
#include "Commands/Blueprint/Layout/Postprocess/UeAgentBlueprintGraphLayoutPostprocess.inl"

bool FUeAgentHttpServer::CmdBlueprintLayoutGraphSelfTest(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	(void)Ctx;
	UeAgentBlueprintAutoLayout::FSelfTestResult Result = UeAgentBlueprintAutoLayout::RunSelfTest();
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("layout_self_test_version"), TEXT("exec_region_deterministic_inter_island_bus_bundle_20260519"));
	OutData->SetNumberField(TEXT("case_count"), Result.CaseCount);
	OutData->SetNumberField(TEXT("passed_count"), Result.PassedCount);
	OutData->SetNumberField(TEXT("failed_count"), Result.FailedCount);
	OutData->SetBoolField(TEXT("all_passed"), Result.bAllPassed);
	OutData->SetArrayField(TEXT("cases"), Result.Cases);
	if (!Result.bAllPassed)
	{
		OutError = FString::Printf(TEXT("blueprint_layout_graph_self_test_failed failed=%d passed=%d"), Result.FailedCount, Result.PassedCount);
		return false;
	}
	OutError.Reset();
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintLayoutGraph(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("dry_run")))
	{
		OutError = TEXT("dry_run_removed_blueprint_layout_graph_always_writes_use_undo_to_revert");
		return false;
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString GraphName = TEXT("EventGraph");
	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);

	UeAgentBlueprintAutoLayout::FOptions Options;
	JsonTryGetNumber(Ctx.Params, TEXT("horizontal_spacing"), Options.HorizontalSpacing);
	JsonTryGetNumber(Ctx.Params, TEXT("vertical_spacing"), Options.VerticalSpacing);
	JsonTryGetNumber(Ctx.Params, TEXT("node_padding"), Options.NodePadding);
	JsonTryGetNumber(Ctx.Params, TEXT("origin_x"), Options.OriginX);
	JsonTryGetNumber(Ctx.Params, TEXT("origin_y"), Options.OriginY);
	double GraphEditorAccessOffsetX = 0.0;
	if (!UeAgentBlueprintOps::TryComputeGraphEditorAccessOffsetX(GraphEditorAccessOffsetX))
	{
		OutError = TEXT("graph_editor_access_offset_unavailable");
		return false;
	}
	Options.OutputAccessOffsetX = GraphEditorAccessOffsetX;
	Options.InputAccessOffsetX = GraphEditorAccessOffsetX;
	Options.AccessOffsetSource = TEXT("graph_editor_settings_compute_spline_tangent");

	FString LayoutReportFile;
	JsonTryGetString(Ctx.Params, TEXT("layout_report_file"), LayoutReportFile);

	bool bCompileAfterLayout = false;
	bool bSaveAfterLayout = false;
	bool bInsertRerouteNodes = false;
	bool bReplaceExistingRerouteNodes = false;
	bool bRequireRealGeometry = true;
	bool bUseRealGeometry = true;
	bool bAllowGeometryViewCapture = false;
	bool bAllowFocusedGeometryCapture = false;
	bool bAllowRemoveAllRerouteNodes = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_layout"), bCompileAfterLayout);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_layout"), bSaveAfterLayout);
	JsonTryGetBool(Ctx.Params, TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
	JsonTryGetBool(Ctx.Params, TEXT("require_real_geometry"), bRequireRealGeometry);
	JsonTryGetBool(Ctx.Params, TEXT("use_real_geometry"), bUseRealGeometry);
	JsonTryGetBool(Ctx.Params, TEXT("allow_geometry_view_capture"), bAllowGeometryViewCapture);
	JsonTryGetBool(Ctx.Params, TEXT("allow_focused_geometry_capture"), bAllowFocusedGeometryCapture);
	JsonTryGetBool(Ctx.Params, TEXT("allow_remove_all_reroute_nodes"), bAllowRemoveAllRerouteNodes);
	if (bAllowFocusedGeometryCapture)
	{
		bAllowGeometryViewCapture = true;
	}
	const bool bEffectiveReplaceExistingRerouteNodes = bReplaceExistingRerouteNodes || bInsertRerouteNodes;
	const bool bUseLayoutReport = !LayoutReportFile.IsEmpty();
	if (!bUseLayoutReport)
	{
		bUseRealGeometry = true;
		bRequireRealGeometry = true;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}
	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);
	if (!Graph)
	{
		OutError = TEXT("graph_not_found");
		return false;
	}

	const FString CompileStatusBefore = UeAgentBlueprintOps::BlueprintStatusToString(Blueprint->Status);
	FString CompileStatusAfter = CompileStatusBefore;
	bool bPrecompiledBeforeLayout = false;
	if (bCompileAfterLayout)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		CompileStatusAfter = UeAgentBlueprintOps::BlueprintStatusToString(Blueprint->Status);
		bPrecompiledBeforeLayout = true;
	}

	auto Model = UeAgentBlueprintOps::CollectGraphLayoutModel(Graph);
	UeAgentBlueprintOps::FGraphLayoutGeometryStats GeometryStats;
	FString GeometryError;
	GeometryStats.NodeCount = Model.Nodes.Num();
	GeometryStats.GeometrySource = bUseLayoutReport ? TEXT("layout_report_file") : TEXT("real_geometry_required");
	const bool bCapturedRealGeometry = !bUseLayoutReport
		? UeAgentBlueprintOps::CaptureGraphLayoutRealGeometry(
			Blueprint,
			Graph,
			Model,
			GeometryStats,
			bAllowGeometryViewCapture,
			bAllowFocusedGeometryCapture,
			GeometryError)
		: false;
	if (!bUseLayoutReport && !bCapturedRealGeometry)
	{
		OutError = GeometryError.IsEmpty() ? TEXT("real_geometry_required_but_unavailable") : GeometryError;
		return false;
	}
	Options.bUseRealGeometry = GeometryStats.bUsedRealGeometry;
	Options.MissingPinAnchorCount = GeometryStats.MissingPinAnchorCount;
	UeAgentBlueprintAutoLayout::FRunResult RunResult;
	if (bUseLayoutReport)
	{
		GeometryStats.GeometrySource = TEXT("layout_report_file");
		if (!UeAgentBlueprintOps::ApplyLayoutReportToModel(LayoutReportFile, Model, RunResult, OutError))
		{
			return false;
		}
	}
	else
	{
		const double LayoutStartSeconds = FPlatformTime::Seconds();
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutAlgorithm start nodes=%d edges=%d"), Model.Nodes.Num(), Model.Edges.Num());
		if (!UeAgentBlueprintAutoLayout::RunLayout(Model.Nodes, Model.Edges, Options, RunResult))
		{
			OutError = TEXT("layout_algorithm_failed");
			return false;
		}
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutAlgorithm done elapsed_ms=%.1f reroutes=%d inter_island_buses=%d route_through=%d hard=%d design_complete=%s"),
				(FPlatformTime::Seconds() - LayoutStartSeconds) * 1000.0,
				RunResult.ReroutePlans.Num(),
				RunResult.DesignDiagnostics.InterIslandBusCount,
				RunResult.DesignDiagnostics.RouteThroughIslandCount,
				RunResult.DesignDiagnostics.RouteThroughHardOccupancyCount,
				RunResult.bDesignComplete ? TEXT("true") : TEXT("false"));
	}
	if (!bUseLayoutReport
		&& (RunResult.DesignDiagnostics.IslandLocalMembershipMutationCount > 0
			|| RunResult.DesignDiagnostics.IslandLocalMutationCount > 0))
	{
		OutError = TEXT("island_local_layout_mutation_blocked");
		return false;
	}

	const int32 ExistingRerouteNodeCount = UeAgentBlueprintOps::CountExistingRerouteNodes(Graph);
	const int32 PlannedRerouteKnotNodeCount = UeAgentBlueprintOps::CountPlannedRerouteKnotNodes(RunResult.ReroutePlans);
	if (!bUseLayoutReport
		&& bEffectiveReplaceExistingRerouteNodes
		&& ExistingRerouteNodeCount > 0
		&& PlannedRerouteKnotNodeCount == 0
		&& !bAllowRemoveAllRerouteNodes)
	{
		OutError = FString::Printf(
			TEXT("reroute_delete_all_guard_blocked existing_reroute_nodes=%d planned_reroute_knots=0; pass allow_remove_all_reroute_nodes=true only when intentionally flattening every existing reroute"),
			ExistingRerouteNodeCount);
		return false;
	}

	int32 RemovedRerouteNodeCount = 0;
	int32 CreatedRerouteNodeCount = 0;
	int32 AppliedMoveCount = 0;
	bool bChanged = false;
	bool bSaved = false;
	FString SaveError;

	bool bExternalLayoutTransaction = false;
	JsonTryGetBool(Ctx.Params, TEXT("_layout_transaction_scope_active"), bExternalLayoutTransaction);
	TUniquePtr<FScopedTransaction> LayoutTransaction;
	if (!bExternalLayoutTransaction)
	{
		LayoutTransaction = MakeUnique<FScopedTransaction>(
			NSLOCTEXT("UeAgentInterface", "BlueprintLayoutGraphTransaction", "UeAgentInterface Blueprint Layout Graph"));
	}
	Graph->Modify();
	Blueprint->Modify();
	if (bEffectiveReplaceExistingRerouteNodes)
	{
		RemovedRerouteNodeCount = UeAgentBlueprintOps::RemoveExistingRerouteNodes(Graph);
	}

	AppliedMoveCount = UeAgentBlueprintOps::ApplyNodePositions(Model);
	CreatedRerouteNodeCount = UeAgentBlueprintOps::ApplyReroutePlans(
		Graph,
		Model,
		RunResult.ReroutePlans,
		bInsertRerouteNodes,
		bEffectiveReplaceExistingRerouteNodes);

	bChanged = AppliedMoveCount > 0 || RemovedRerouteNodeCount > 0 || CreatedRerouteNodeCount > 0;
	if (bChanged)
	{
		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	if (bCompileAfterLayout)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		CompileStatusAfter = UeAgentBlueprintOps::BlueprintStatusToString(Blueprint->Status);
	}

	if (bSaveAfterLayout)
	{
		bSaved = UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, SaveError);
		if (!bSaved)
		{
			if (LayoutTransaction)
			{
				LayoutTransaction->Cancel();
			}
			OutError = SaveError.IsEmpty() ? TEXT("save_failed") : SaveError;
			return false;
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), AssetPathInput);
	OutData->SetStringField(TEXT("graph_name"), GraphName);
	OutData->SetStringField(TEXT("layout_strategy"), RunResult.Strategy);
	OutData->SetStringField(TEXT("selected_candidate"), TEXT("clean_rewrite"));
	OutData->SetStringField(TEXT("layout_framework"), TEXT("clean_semantic_island_multiport_layout"));
	OutData->SetStringField(TEXT("layout_report_file"), LayoutReportFile);
	OutData->SetBoolField(TEXT("layout_report_restore"), bUseLayoutReport);
	OutData->SetNumberField(TEXT("output_access_offset_x"), Options.OutputAccessOffsetX);
	OutData->SetNumberField(TEXT("input_access_offset_x"), Options.InputAccessOffsetX);
	OutData->SetStringField(TEXT("access_offset_source"), Options.AccessOffsetSource);
	OutData->SetBoolField(TEXT("require_real_geometry"), bRequireRealGeometry);
	OutData->SetBoolField(TEXT("use_real_geometry"), bUseRealGeometry);
	OutData->SetBoolField(TEXT("allow_geometry_view_capture"), bAllowGeometryViewCapture);
	OutData->SetBoolField(TEXT("allow_focused_geometry_capture"), bAllowFocusedGeometryCapture);
	OutData->SetBoolField(TEXT("real_geometry_capture_requested"), GeometryStats.bRealGeometryCaptureRequested);
	OutData->SetStringField(TEXT("geometry_source"), GeometryStats.GeometrySource);
	OutData->SetBoolField(TEXT("used_real_geometry"), GeometryStats.bUsedRealGeometry);
	OutData->SetBoolField(TEXT("geometry_window_prepared"), GeometryStats.bWindowPrepared);
	OutData->SetBoolField(TEXT("geometry_used_offscreen_graph_editor"), GeometryStats.bUsedOffscreenGraphEditor);
	OutData->SetNumberField(TEXT("measured_node_count"), GeometryStats.MeasuredNodeCount);
	OutData->SetNumberField(TEXT("missing_measured_node_geometry_count"), FMath::Max(0, GeometryStats.NodeCount - GeometryStats.MeasuredNodeCount));
	OutData->SetNumberField(TEXT("pin_widget_count"), GeometryStats.PinWidgetCount);
	OutData->SetNumberField(TEXT("pin_geometry_anchor_count"), GeometryStats.PinGeometryAnchorCount);
	OutData->SetNumberField(TEXT("missing_visible_pin_anchor_count"), GeometryStats.VisibleMissingPinAnchorCount);
		OutData->SetNumberField(TEXT("missing_pin_anchor_count"), GeometryStats.MissingPinAnchorCount);
		OutData->SetNumberField(TEXT("focused_geometry_node_count"), GeometryStats.FocusedGeometryNodeCount);
		OutData->SetBoolField(TEXT("focused_geometry_budget_exhausted"), GeometryStats.bFocusedGeometryBudgetExhausted);
		OutData->SetBoolField(TEXT("geometry_restore_view_state_from_document"), GeometryStats.bRestoreViewStateFromDocument);
		OutData->SetBoolField(TEXT("geometry_restored_graph_view_after_capture"), GeometryStats.bRestoredGraphViewAfterCapture);
		OutData->SetNumberField(TEXT("geometry_restored_view_x"), GeometryStats.RestoredViewLocation.X);
		OutData->SetNumberField(TEXT("geometry_restored_view_y"), GeometryStats.RestoredViewLocation.Y);
		OutData->SetNumberField(TEXT("geometry_restored_zoom"), GeometryStats.RestoredZoomAmount);
		OutData->SetNumberField(TEXT("geometry_measurement_zoom"), GeometryStats.GeometryMeasurementZoomAmount);
		if (!GeometryStats.Error.IsEmpty())
		{
			OutData->SetStringField(TEXT("geometry_error"), GeometryStats.Error);
		}
	OutData->SetBoolField(TEXT("layout_design_complete"), RunResult.bDesignComplete);
	OutData->SetNumberField(TEXT("layout_design_feature_count"), RunResult.DesignFeatureCount);
	OutData->SetNumberField(TEXT("layout_design_feature_complete_count"), RunResult.DesignFeatureCompleteCount);
	OutData->SetNumberField(TEXT("layout_design_feature_partial_count"), RunResult.DesignFeaturePartialCount);
	OutData->SetNumberField(TEXT("layout_design_feature_missing_count"), RunResult.DesignFeatureMissingCount);
	OutData->SetArrayField(TEXT("layout_design_feature_coverage"), RunResult.DesignFeatureCoverage);
	OutData->SetBoolField(TEXT("changed"), bChanged);
	OutData->SetBoolField(TEXT("saved"), bSaved);
	OutData->SetBoolField(TEXT("compile_after_layout"), bCompileAfterLayout);
	OutData->SetBoolField(TEXT("precompiled_before_layout"), bPrecompiledBeforeLayout);
	OutData->SetStringField(TEXT("compile_status_before"), CompileStatusBefore);
	OutData->SetStringField(TEXT("compile_status_after"), CompileStatusAfter);
	OutData->SetBoolField(TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
	OutData->SetBoolField(TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
	OutData->SetBoolField(TEXT("effective_replace_existing_reroute_nodes"), bEffectiveReplaceExistingRerouteNodes);
	OutData->SetBoolField(TEXT("allow_remove_all_reroute_nodes"), bAllowRemoveAllRerouteNodes);
	OutData->SetNumberField(TEXT("existing_reroute_node_count"), ExistingRerouteNodeCount);
	OutData->SetNumberField(TEXT("planned_reroute_knot_node_count"), PlannedRerouteKnotNodeCount);
	const FVector2D RerouteKnotSpacer = UeAgentBlueprintOps::GetLayoutKnotNodeSpacerSize();
	OutData->SetNumberField(TEXT("reroute_knot_node_spacer_x"), RerouteKnotSpacer.X);
	OutData->SetNumberField(TEXT("reroute_knot_node_spacer_y"), RerouteKnotSpacer.Y);
	OutData->SetNumberField(TEXT("reroute_knot_control_point_to_node_offset_x"), RerouteKnotSpacer.X * 0.5);
	OutData->SetNumberField(TEXT("reroute_knot_control_point_to_node_offset_y"), RerouteKnotSpacer.Y * 0.5);
	OutData->SetNumberField(TEXT("node_count"), RunResult.NodeCount);
	OutData->SetNumberField(TEXT("edge_count"), RunResult.EdgeCount);
	OutData->SetNumberField(TEXT("moved_node_count"), RunResult.MovedNodeCount);
	OutData->SetNumberField(TEXT("applied_move_count"), AppliedMoveCount);
	OutData->SetNumberField(TEXT("removed_reroute_node_count"), RemovedRerouteNodeCount);
	OutData->SetNumberField(TEXT("created_reroute_node_count"), CreatedRerouteNodeCount);
	OutData->SetNumberField(TEXT("reroute_plan_count"), RunResult.ReroutePlanCount);
	OutData->SetNumberField(TEXT("local_bus_edge_count"), RunResult.LocalBusEdgeCount);
	OutData->SetNumberField(TEXT("external_bus_edge_count"), RunResult.ExternalBusEdgeCount);
	OutData->SetNumberField(TEXT("data_island_count"), RunResult.IslandCount);
	OutData->SetNumberField(TEXT("island_edge_count"), RunResult.IslandEdgeCount);
	OutData->SetNumberField(TEXT("rank_count"), RunResult.RankCount);
	OutData->SetNumberField(TEXT("alignment_failed_unit_count"), RunResult.AlignmentFailedUnitCount);
	OutData->SetNumberField(TEXT("invalid_island_edge_count"), RunResult.InvalidIslandEdgeCount);
	OutData->SetNumberField(TEXT("scc_count_before_cut"), RunResult.DesignDiagnostics.SccCountBeforeCut);
	OutData->SetNumberField(TEXT("scc_feedback_edge_count"), RunResult.DesignDiagnostics.SccFeedbackEdgeCount);
	OutData->SetNumberField(TEXT("fanout_node_count"), RunResult.DesignDiagnostics.FanoutNodeCount);
	OutData->SetNumberField(TEXT("fanout_implicit_edge_count"), RunResult.DesignDiagnostics.FanoutImplicitEdgeCount);
	OutData->SetNumberField(TEXT("comment_island_count"), RunResult.DesignDiagnostics.CommentIslandCount);
	OutData->SetNumberField(TEXT("comment_island_node_count"), RunResult.DesignDiagnostics.CommentIslandNodeCount);
	OutData->SetNumberField(TEXT("comment_island_boundary_edge_count"), RunResult.DesignDiagnostics.CommentIslandBoundaryEdgeCount);
	OutData->SetNumberField(TEXT("rank_ignored_edge_count"), RunResult.DesignDiagnostics.RankIgnoredEdgeCount);
	OutData->SetNumberField(TEXT("island_cut_edge_count"), RunResult.DesignDiagnostics.IslandCutEdgeCount);
		OutData->SetNumberField(TEXT("implicit_edge_count"), RunResult.DesignDiagnostics.ImplicitEdgeCount);
		OutData->SetNumberField(TEXT("node_island_count"), RunResult.DesignDiagnostics.NodeIslandCount);
		OutData->SetNumberField(TEXT("multi_sink_island_count"), RunResult.DesignDiagnostics.MultiSinkIslandCount);
		OutData->SetNumberField(TEXT("input_chain_count"), RunResult.DesignDiagnostics.InputChainCount);
		OutData->SetNumberField(TEXT("input_chain_node_count"), RunResult.DesignDiagnostics.InputChainNodeCount);
		OutData->SetNumberField(TEXT("input_chain_forbidden_zone_count"), RunResult.DesignDiagnostics.InputChainForbiddenZoneCount);
		OutData->SetNumberField(TEXT("input_chain_forbidden_avoidance_count"), RunResult.DesignDiagnostics.InputChainForbiddenAvoidanceCount);
		OutData->SetNumberField(TEXT("input_chain_collision_reroute_count"), RunResult.DesignDiagnostics.InputChainCollisionRerouteCount);
		OutData->SetNumberField(TEXT("input_chain_collision_reroute_rejected_count"), RunResult.DesignDiagnostics.InputChainCollisionRerouteRejectedCount);
		OutData->SetNumberField(TEXT("implicit_edge_inside_island_count"), RunResult.DesignDiagnostics.ImplicitEdgeInsideIslandCount);
	OutData->SetNumberField(TEXT("source_param_count"), RunResult.DesignDiagnostics.SourceParamCount);
	OutData->SetNumberField(TEXT("internal_bus_group_count"), RunResult.DesignDiagnostics.InternalBusGroupCount);
	OutData->SetNumberField(TEXT("boundary_entry_count"), RunResult.DesignDiagnostics.BoundaryEntryCount);
	OutData->SetNumberField(TEXT("left_boundary_entry_count"), RunResult.DesignDiagnostics.LeftBoundaryEntryCount);
	OutData->SetNumberField(TEXT("top_boundary_entry_count"), RunResult.DesignDiagnostics.TopBoundaryEntryCount);
	OutData->SetNumberField(TEXT("bottom_boundary_entry_count"), RunResult.DesignDiagnostics.BottomBoundaryEntryCount);
	OutData->SetNumberField(TEXT("boundary_bus_required_count"), RunResult.DesignDiagnostics.BoundaryBusRequiredCount);
	OutData->SetNumberField(TEXT("boundary_bus_count"), RunResult.DesignDiagnostics.BoundaryBusCount);
	OutData->SetNumberField(TEXT("internal_attach_merge_group_count"), RunResult.DesignDiagnostics.InternalAttachMergeGroupCount);
	OutData->SetNumberField(TEXT("boundary_attach_merge_group_count"), RunResult.DesignDiagnostics.BoundaryAttachMergeGroupCount);
	OutData->SetNumberField(TEXT("boundary_entry_rejected_candidate_count"), RunResult.DesignDiagnostics.BoundaryEntryRejectedCandidateCount);
	OutData->SetNumberField(TEXT("anchored_column_placement_count"), RunResult.DesignDiagnostics.AnchoredColumnPlacementCount);
	OutData->SetNumberField(TEXT("anchored_column_unconnected_item_count"), RunResult.DesignDiagnostics.AnchoredColumnUnconnectedItemCount);
	OutData->SetNumberField(TEXT("anchored_column_total_pin_residual"), RunResult.DesignDiagnostics.AnchoredColumnTotalPinResidual);
	OutData->SetNumberField(TEXT("anchored_column_max_pin_residual"), RunResult.DesignDiagnostics.AnchoredColumnMaxPinResidual);
	OutData->SetNumberField(TEXT("island_internal_placement_count"), RunResult.DesignDiagnostics.IslandInternalPlacementCount);
	OutData->SetNumberField(TEXT("island_internal_left_boundary_entry_count"), RunResult.DesignDiagnostics.IslandInternalLeftBoundaryEntryCount);
	OutData->SetNumberField(TEXT("island_internal_multi_column_bus_count"), RunResult.DesignDiagnostics.IslandInternalMultiColumnBusCount);
	OutData->SetNumberField(TEXT("island_internal_same_column_collision_bus_count"), RunResult.DesignDiagnostics.IslandInternalSameColumnCollisionBusCount);
	OutData->SetNumberField(TEXT("island_internal_total_pin_residual"), RunResult.DesignDiagnostics.IslandInternalTotalPinResidual);
	OutData->SetNumberField(TEXT("island_internal_max_pin_residual"), RunResult.DesignDiagnostics.IslandInternalMaxPinResidual);
	OutData->SetNumberField(TEXT("island_local_frozen_node_count"), RunResult.DesignDiagnostics.IslandLocalFrozenNodeCount);
	OutData->SetNumberField(TEXT("island_local_membership_mutation_count"), RunResult.DesignDiagnostics.IslandLocalMembershipMutationCount);
	OutData->SetNumberField(TEXT("island_local_mutation_count"), RunResult.DesignDiagnostics.IslandLocalMutationCount);
	OutData->SetNumberField(TEXT("island_local_max_mutation"), RunResult.DesignDiagnostics.IslandLocalMaxMutation);
	OutData->SetNumberField(TEXT("endpoint_candidate_count"), RunResult.DesignDiagnostics.EndpointCandidateCount);
	OutData->SetNumberField(TEXT("placement_graph_edge_count"), RunResult.DesignDiagnostics.PlacementGraphEdgeCount);
	OutData->SetNumberField(TEXT("route_only_edge_count"), RunResult.DesignDiagnostics.RouteOnlyEdgeCount);
	OutData->SetNumberField(TEXT("placement_owner_edge_count"), RunResult.DesignDiagnostics.PlacementOwnerEdgeCount);
	OutData->SetNumberField(TEXT("placement_non_owner_edge_count"), RunResult.DesignDiagnostics.PlacementNonOwnerEdgeCount);
	OutData->SetNumberField(TEXT("source_order_group_count"), RunResult.DesignDiagnostics.SourceOrderGroupCount);
	OutData->SetNumberField(TEXT("fanout_intent_group_count"), RunResult.DesignDiagnostics.FanoutIntentGroupCount);
	OutData->SetNumberField(TEXT("placement_proposal_count"), RunResult.DesignDiagnostics.PlacementProposalCount);
	OutData->SetNumberField(TEXT("rejected_proposal_count"), RunResult.DesignDiagnostics.RejectedProposalCount);
	OutData->SetNumberField(TEXT("source_port_order_violation_count"), RunResult.DesignDiagnostics.SourcePortOrderViolationCount);
	OutData->SetNumberField(TEXT("invalid_forward_connection_count"), RunResult.DesignDiagnostics.InvalidForwardConnectionCount);
	OutData->SetNumberField(TEXT("selected_endpoint_candidate_count"), RunResult.DesignDiagnostics.SelectedEndpointCandidateCount);
	OutData->SetNumberField(TEXT("connection_group_count"), RunResult.DesignDiagnostics.ConnectionGroupCount);
	OutData->SetNumberField(TEXT("route_group_count"), RunResult.DesignDiagnostics.RouteGroupCount);
	OutData->SetNumberField(TEXT("route_kind_upgrade_count"), RunResult.DesignDiagnostics.RouteKindUpgradeCount);
	OutData->SetNumberField(TEXT("branch_merge_group_count"), RunResult.DesignDiagnostics.BranchMergeGroupCount);
	OutData->SetNumberField(TEXT("cross_bus_trunk_lane_count"), RunResult.DesignDiagnostics.CrossBusTrunkLaneCount);
	OutData->SetNumberField(TEXT("cross_bus_lane_count"), RunResult.DesignDiagnostics.CrossBusLaneCount);
	OutData->SetNumberField(TEXT("route_through_hard_occupancy_count"), RunResult.DesignDiagnostics.RouteThroughHardOccupancyCount);
	OutData->SetNumberField(TEXT("island_connection_total_length"), RunResult.DesignDiagnostics.IslandConnectionTotalLength);
	OutData->SetNumberField(TEXT("total_route_length"), RunResult.DesignDiagnostics.TotalRouteLength);
	OutData->SetNumberField(TEXT("bus_y_variance_total"), RunResult.DesignDiagnostics.BusYVarianceTotal);
	OutData->SetNumberField(TEXT("island_connection_external_upgrade_count"), RunResult.DesignDiagnostics.IslandConnectionExternalUpgradeCount);
	OutData->SetNumberField(TEXT("island_connection_top_bottom_x_residual"), RunResult.DesignDiagnostics.IslandConnectionTopBottomXResidual);
	OutData->SetNumberField(TEXT("island_connection_left_y_residual"), RunResult.DesignDiagnostics.IslandConnectionLeftYResidual);
	OutData->SetNumberField(TEXT("placement_component_count"), RunResult.DesignDiagnostics.PlacementComponentCount);
	OutData->SetNumberField(TEXT("lane_coloring_lane_count"), RunResult.DesignDiagnostics.LaneColoringLaneCount);
	OutData->SetNumberField(TEXT("route_through_island_count"), RunResult.DesignDiagnostics.RouteThroughIslandCount);
	OutData->SetNumberField(TEXT("island_topological_order_count"), RunResult.DesignDiagnostics.IslandTopologicalOrderCount);
	OutData->SetNumberField(TEXT("island_topological_residual_cycle_edge_count"), RunResult.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount);
	OutData->SetNumberField(TEXT("root_island_count"), RunResult.DesignDiagnostics.RootIslandCount);
	OutData->SetNumberField(TEXT("root_scoring_target_count"), RunResult.DesignDiagnostics.RootScoringTargetCount);
	OutData->SetNumberField(TEXT("root_share_component_count"), RunResult.DesignDiagnostics.RootShareComponentCount);
	OutData->SetNumberField(TEXT("root_equivalent_group_count"), RunResult.DesignDiagnostics.RootEquivalentGroupCount);
	OutData->SetNumberField(TEXT("root_continuous_order_score"), RunResult.DesignDiagnostics.RootContinuousOrderScore);
	OutData->SetNumberField(TEXT("root_continuous_order_inversion_score"), RunResult.DesignDiagnostics.RootContinuousOrderInversionScore);
	OutData->SetNumberField(TEXT("root_continuous_order_pruned_state_count"), RunResult.DesignDiagnostics.RootContinuousOrderPrunedStateCount);
	OutData->SetNumberField(TEXT("root_continuous_order_memo_hit_count"), RunResult.DesignDiagnostics.RootContinuousOrderMemoHitCount);
	OutData->SetNumberField(TEXT("root_continuous_order_exact_limit_exceeded"), RunResult.DesignDiagnostics.RootContinuousOrderExactLimitExceeded);
	OutData->SetNumberField(TEXT("root_compact_vertical_gap"), RunResult.DesignDiagnostics.RootCompactVerticalGap);
	OutData->SetNumberField(TEXT("root_compact_total_height"), RunResult.DesignDiagnostics.RootCompactTotalHeight);
	OutData->SetNumberField(TEXT("root_island_top_y_count"), RunResult.DesignDiagnostics.RootIslandTopYCount);
	OutData->SetNumberField(TEXT("island_placement_order_count"), RunResult.DesignDiagnostics.IslandPlacementOrderCount);
	OutData->SetNumberField(TEXT("island_leftward_reanchor_base_column"), RunResult.DesignDiagnostics.IslandLeftwardReanchorBaseColumn);
	OutData->SetNumberField(TEXT("island_leftward_reanchor_column_count"), RunResult.DesignDiagnostics.IslandLeftwardReanchorColumnCount);
	OutData->SetNumberField(TEXT("direct_island_connection_count"), RunResult.DesignDiagnostics.DirectIslandConnectionCount);
	OutData->SetNumberField(TEXT("bus_island_connection_count"), RunResult.DesignDiagnostics.BusIslandConnectionCount);
	OutData->SetNumberField(TEXT("inter_island_feedback_bus_connection_count"), RunResult.DesignDiagnostics.InterIslandFeedbackBusConnectionCount);
	OutData->SetNumberField(TEXT("inter_island_bus_count"), RunResult.DesignDiagnostics.InterIslandBusCount);
	OutData->SetNumberField(TEXT("inter_island_bus_duplicate_merge_count"), RunResult.DesignDiagnostics.InterIslandBusDuplicateMergeCount);
	OutData->SetNumberField(TEXT("inter_island_bus_bundle_count"), RunResult.DesignDiagnostics.InterIslandBusBundleCount);
	OutData->SetNumberField(TEXT("inter_island_bus_slot_assignment_count"), RunResult.DesignDiagnostics.InterIslandBusSlotAssignmentCount);
	OutData->SetNumberField(TEXT("inter_island_bus_max_slot_count"), RunResult.DesignDiagnostics.InterIslandBusMaxSlotCount);
	OutData->SetNumberField(TEXT("inter_island_bus_bundle_collision_box_count"), RunResult.DesignDiagnostics.InterIslandBusBundleCollisionBoxCount);
	OutData->SetNumberField(TEXT("island_bus_collision_box_count"), RunResult.DesignDiagnostics.IslandBusCollisionBoxCount);
	OutData->SetNumberField(TEXT("island_band_group_count"), RunResult.DesignDiagnostics.IslandBandGroupCount);
	OutData->SetNumberField(TEXT("topology_column_horizontal_push_count"), RunResult.DesignDiagnostics.TopologyColumnHorizontalPushCount);
	OutData->SetNumberField(TEXT("topology_column_bus_pull_count"), RunResult.DesignDiagnostics.TopologyColumnBusPullCount);
	OutData->SetNumberField(TEXT("incoming_bus_branch_count"), RunResult.DesignDiagnostics.IncomingBusBranchCount);
	OutData->SetNumberField(TEXT("inter_island_bus_branch_x_slot_count"), RunResult.DesignDiagnostics.InterIslandBusBranchXSlotCount);
	OutData->SetNumberField(TEXT("island_bus_branch_right_shift_count"), RunResult.DesignDiagnostics.IslandBusBranchRightShiftCount);
	OutData->SetNumberField(TEXT("inter_island_bus_branch_knot_count"), RunResult.DesignDiagnostics.InterIslandBusBranchKnotCount);
	OutData->SetNumberField(TEXT("inter_island_bus_branch_knot_omitted_count"), RunResult.DesignDiagnostics.InterIslandBusBranchKnotOmittedCount);
	OutData->SetObjectField(TEXT("metrics_before"), UeAgentBlueprintAutoLayout::MetricsToJson(RunResult.MetricsBefore));
	OutData->SetObjectField(TEXT("metrics_after"), UeAgentBlueprintAutoLayout::MetricsToJson(RunResult.MetricsAfter));
	OutData->SetObjectField(TEXT("metrics_after_reroute_plan"), UeAgentBlueprintAutoLayout::MetricsToJson(RunResult.MetricsAfterReroute));
	OutData->SetNumberField(TEXT("overlap_count_after"), RunResult.MetricsAfter.OverlapCount);
	OutData->SetNumberField(TEXT("edge_crossing_count_after"), RunResult.MetricsAfter.EdgeCrossingCount);
	OutData->SetNumberField(TEXT("edge_node_intersection_count_after"), RunResult.MetricsAfter.EdgeNodeIntersectionCount);
	OutData->SetArrayField(TEXT("nodes"), UeAgentBlueprintOps::NodesToJson(Model));
	OutData->SetArrayField(TEXT("reroute_plan"), UeAgentBlueprintOps::ReroutePlansToJson(RunResult.ReroutePlans));
	OutData->SetArrayField(TEXT("island_diagnostics"), RunResult.IslandDiagnostics);
	OutData->SetArrayField(TEXT("island_edge_diagnostics"), RunResult.IslandEdgeDiagnostics);
	OutData->SetArrayField(TEXT("island_partition_stages"), RunResult.IslandPartitionStageDiagnostics);
	OutData->SetArrayField(TEXT("column_diagnostics"), RunResult.ColumnDiagnostics);
	OutData->SetArrayField(TEXT("input_chains"), RunResult.InputChainDiagnostics);
	OutData->SetArrayField(TEXT("internal_bus_groups"), RunResult.InternalBusGroupDiagnostics);
	OutData->SetArrayField(TEXT("boundary_entries"), RunResult.BoundaryEntryDiagnostics);
	OutData->SetArrayField(TEXT("boundary_buses"), RunResult.BoundaryBusDiagnostics);
	OutData->SetArrayField(TEXT("island_local_mutations"), RunResult.IslandLocalMutationDiagnostics);
	OutData->SetArrayField(TEXT("island_port_candidates"), RunResult.IslandPortCandidateDiagnostics);
	OutData->SetArrayField(TEXT("placement_proposals"), RunResult.PlacementProposalDiagnostics);
	OutData->SetArrayField(TEXT("source_order_groups"), RunResult.SourceOrderGroupDiagnostics);
	OutData->SetArrayField(TEXT("fanout_intent_groups"), RunResult.FanoutIntentGroupDiagnostics);
	OutData->SetArrayField(TEXT("selected_endpoint_candidates"), RunResult.SelectedEndpointCandidateDiagnostics);
	OutData->SetArrayField(TEXT("inter_island_bus_build_stages"), RunResult.InterIslandBusBuildDiagnostics);
	OutData->SetArrayField(TEXT("inter_island_buses"), RunResult.InterIslandBusDiagnostics);
	OutData->SetArrayField(TEXT("inter_island_bus_branch_visible_targets"), RunResult.InterIslandBusBranchVisibleTargetDiagnostics);
	OutData->SetArrayField(TEXT("inter_island_bus_branch_target_modes"), RunResult.InterIslandBusBranchTargetModeDiagnostics);
	OutData->SetArrayField(TEXT("branch_merge_groups"), RunResult.BranchMergeDiagnostics);
	OutData->SetArrayField(TEXT("cross_bus_lanes"), RunResult.CrossBusLaneDiagnostics);
	OutData->SetArrayField(TEXT("route_diagnostics"), RunResult.RouteDiagnostics);
	OutData->SetArrayField(TEXT("lane_assignments"), RunResult.LaneAssignmentDiagnostics);
	OutData->SetArrayField(TEXT("rejection_diagnostics"), RunResult.RejectionDiagnostics);
	return true;
}

namespace UeAgentBlueprintOps
{
	namespace
	{
		static bool ExecuteBlueprintLayoutAllGraphsCommand(
			const FUeAgentRequestContext& Ctx,
			TSharedPtr<FJsonObject>& OutData,
			FString& OutError,
			TFunctionRef<bool(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&)> RunGraphLayout)
		{
	FString AssetPathInput;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	if (Ctx.Params->HasField(TEXT("dry_run")))
	{
		OutError = TEXT("dry_run_removed_blueprint_layout_all_graphs_always_writes_use_undo_to_revert");
		return false;
	}

	UBlueprint* Blueprint = LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	TArray<FBlueprintLayoutGraphTarget> Targets;
	FString TargetError;
	if (!CollectBlueprintLayoutTargets(Blueprint, Ctx.Params, Targets, TargetError))
	{
		OutError = TargetError.IsEmpty() ? TEXT("collect_layout_graph_targets_failed") : TargetError;
		return false;
	}

	bool bSkipEmptyGraphs = true;
	bool bStopOnError = true;
	bool bIncludeGraphResultDetails = false;
	bool bSaveAfterLayout = false;
	Ctx.Params->TryGetBoolField(TEXT("skip_empty_graphs"), bSkipEmptyGraphs);
	Ctx.Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);
	Ctx.Params->TryGetBoolField(TEXT("include_graph_result_details"), bIncludeGraphResultDetails);
	Ctx.Params->TryGetBoolField(TEXT("save_after_layout"), bSaveAfterLayout);

	FString LayoutReportFile;
	Ctx.Params->TryGetStringField(TEXT("layout_report_file"), LayoutReportFile);
	if (!LayoutReportFile.IsEmpty() && Targets.Num() > 1)
	{
		OutError = TEXT("layout_report_file_requires_single_graph");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> GraphResults;
	int32 ProcessedGraphCount = 0;
	int32 SkippedGraphCount = 0;
	int32 FailedGraphCount = 0;
	int32 ChangedGraphCount = 0;
	int32 SavedGraphCount = 0;
	int32 DesignIncompleteGraphCount = 0;
	double TotalNodeCount = 0.0;
	double TotalEdgeCount = 0.0;
	double TotalMovedNodeCount = 0.0;
	double TotalReroutePlanCount = 0.0;
	double TotalCreatedRerouteNodeCount = 0.0;
	double TotalRouteThroughIslandCount = 0.0;
	double TotalRouteThroughHardOccupancyCount = 0.0;
	TUniquePtr<FScopedTransaction> LayoutAllTransaction = MakeUnique<FScopedTransaction>(
		NSLOCTEXT("UeAgentInterface", "BlueprintLayoutAllGraphsTransaction", "UeAgentInterface Blueprint Layout All Graphs"));

	auto TryCopyBoolField = [](const TSharedPtr<FJsonObject>& Source, const FString& Field, TSharedPtr<FJsonObject>& Target)
	{
		bool Value = false;
		if (Source.IsValid() && Source->TryGetBoolField(Field, Value))
		{
			Target->SetBoolField(Field, Value);
		}
	};
	auto TryCopyNumberField = [](const TSharedPtr<FJsonObject>& Source, const FString& Field, TSharedPtr<FJsonObject>& Target, double* OutValue = nullptr)
	{
		double Value = 0.0;
		if (Source.IsValid() && Source->TryGetNumberField(Field, Value))
		{
			Target->SetNumberField(Field, Value);
			if (OutValue)
			{
				*OutValue = Value;
			}
		}
	};
	auto TryCopyStringField = [](const TSharedPtr<FJsonObject>& Source, const FString& Field, TSharedPtr<FJsonObject>& Target)
	{
		FString Value;
		if (Source.IsValid() && Source->TryGetStringField(Field, Value))
		{
			Target->SetStringField(Field, Value);
		}
	};

	for (const FBlueprintLayoutGraphTarget& Target : Targets)
	{
		TSharedPtr<FJsonObject> ResultItem = MakeShared<FJsonObject>();
		ResultItem->SetStringField(TEXT("graph_name"), Target.Graph ? Target.Graph->GetName() : TEXT(""));
		ResultItem->SetStringField(TEXT("graph_path"), Target.Graph ? Target.Graph->GetPathName() : TEXT(""));
		ResultItem->SetStringField(TEXT("graph_type"), Target.GraphType);

		if (!Target.Graph)
		{
			ResultItem->SetBoolField(TEXT("ok"), false);
			ResultItem->SetStringField(TEXT("error"), TEXT("graph_missing"));
			GraphResults.Add(MakeShared<FJsonValueObject>(ResultItem));
			++FailedGraphCount;
			if (bStopOnError)
			{
				OutData = MakeShared<FJsonObject>();
				OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
				OutData->SetArrayField(TEXT("graphs"), GraphResults);
				if (LayoutAllTransaction)
				{
					LayoutAllTransaction->Cancel();
				}
				OutError = TEXT("graph_missing");
				return false;
			}
			continue;
		}

		const int32 SemanticNodeCount = CountLayoutSemanticNodes(Target.Graph);
		ResultItem->SetNumberField(TEXT("semantic_node_count"), SemanticNodeCount);
		if (bSkipEmptyGraphs && SemanticNodeCount == 0)
		{
			ResultItem->SetBoolField(TEXT("ok"), true);
			ResultItem->SetBoolField(TEXT("skipped"), true);
			ResultItem->SetStringField(TEXT("skip_reason"), TEXT("empty_graph"));
			GraphResults.Add(MakeShared<FJsonValueObject>(ResultItem));
			++SkippedGraphCount;
			continue;
		}

		TSharedPtr<FJsonObject> GraphParams = MakeShared<FJsonObject>();
		if (Ctx.Params.IsValid())
		{
			GraphParams->Values = Ctx.Params->Values;
		}
		GraphParams->SetStringField(TEXT("graph_name"), Target.Graph->GetPathName());
		GraphParams->SetBoolField(TEXT("_layout_transaction_scope_active"), true);
		GraphParams->SetBoolField(TEXT("save_after_layout"), false);

		FUeAgentRequestContext GraphCtx;
		GraphCtx.RequestId = FString::Printf(TEXT("%s:%s"), *Ctx.RequestId, *Target.Graph->GetName());
		GraphCtx.Command = TEXT("blueprint_layout_graph");
		GraphCtx.Params = GraphParams;

		TSharedPtr<FJsonObject> GraphData;
		FString GraphError;
		const bool bGraphOk = RunGraphLayout(GraphCtx, GraphData, GraphError);
		ResultItem->SetBoolField(TEXT("ok"), bGraphOk);
		ResultItem->SetBoolField(TEXT("skipped"), false);
		if (!bGraphOk)
		{
			ResultItem->SetStringField(TEXT("error"), GraphError);
			GraphResults.Add(MakeShared<FJsonValueObject>(ResultItem));
			++FailedGraphCount;
			if (bStopOnError)
			{
				OutData = MakeShared<FJsonObject>();
				OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
				OutData->SetStringField(TEXT("failed_graph_name"), Target.Graph->GetName());
				OutData->SetStringField(TEXT("failed_graph_path"), Target.Graph->GetPathName());
				OutData->SetArrayField(TEXT("graphs"), GraphResults);
				if (LayoutAllTransaction)
				{
					LayoutAllTransaction->Cancel();
				}
				OutError = FString::Printf(TEXT("blueprint_layout_all_graphs_failed graph=%s error=%s"), *Target.Graph->GetName(), *GraphError);
				return false;
			}
			continue;
		}

		++ProcessedGraphCount;
		bool bGraphChanged = false;
		bool bGraphSaved = false;
		bool bGraphDesignComplete = true;
		if (GraphData.IsValid())
		{
			GraphData->TryGetBoolField(TEXT("changed"), bGraphChanged);
			GraphData->TryGetBoolField(TEXT("saved"), bGraphSaved);
			GraphData->TryGetBoolField(TEXT("layout_design_complete"), bGraphDesignComplete);
			TryCopyStringField(GraphData, TEXT("layout_strategy"), ResultItem);
			TryCopyStringField(GraphData, TEXT("layout_framework"), ResultItem);
			TryCopyStringField(GraphData, TEXT("geometry_source"), ResultItem);
			TryCopyBoolField(GraphData, TEXT("layout_design_complete"), ResultItem);
			TryCopyBoolField(GraphData, TEXT("changed"), ResultItem);
			TryCopyBoolField(GraphData, TEXT("saved"), ResultItem);
			TryCopyBoolField(GraphData, TEXT("used_real_geometry"), ResultItem);
			double NodeCount = 0.0;
			double EdgeCount = 0.0;
			double MovedNodeCount = 0.0;
			double ReroutePlanCount = 0.0;
			double CreatedRerouteNodeCount = 0.0;
			double RouteThroughIslandCount = 0.0;
			double RouteThroughHardOccupancyCount = 0.0;
			TryCopyNumberField(GraphData, TEXT("node_count"), ResultItem, &NodeCount);
			TryCopyNumberField(GraphData, TEXT("edge_count"), ResultItem, &EdgeCount);
			TryCopyNumberField(GraphData, TEXT("moved_node_count"), ResultItem, &MovedNodeCount);
			TryCopyNumberField(GraphData, TEXT("reroute_plan_count"), ResultItem, &ReroutePlanCount);
			TryCopyNumberField(GraphData, TEXT("created_reroute_node_count"), ResultItem, &CreatedRerouteNodeCount);
			TryCopyNumberField(GraphData, TEXT("overlap_count_after"), ResultItem);
			TryCopyNumberField(GraphData, TEXT("route_through_island_count"), ResultItem, &RouteThroughIslandCount);
			TryCopyNumberField(GraphData, TEXT("route_through_hard_occupancy_count"), ResultItem, &RouteThroughHardOccupancyCount);
			TryCopyNumberField(GraphData, TEXT("missing_pin_anchor_count"), ResultItem);
			TryCopyNumberField(GraphData, TEXT("input_chain_count"), ResultItem);
			TryCopyNumberField(GraphData, TEXT("local_bus_edge_count"), ResultItem);
			TryCopyNumberField(GraphData, TEXT("external_bus_edge_count"), ResultItem);
			TotalNodeCount += NodeCount;
			TotalEdgeCount += EdgeCount;
			TotalMovedNodeCount += MovedNodeCount;
			TotalReroutePlanCount += ReroutePlanCount;
			TotalCreatedRerouteNodeCount += CreatedRerouteNodeCount;
			TotalRouteThroughIslandCount += RouteThroughIslandCount;
			TotalRouteThroughHardOccupancyCount += RouteThroughHardOccupancyCount;
			if (bIncludeGraphResultDetails)
			{
				ResultItem->SetObjectField(TEXT("layout_result"), GraphData);
			}
		}
		if (bGraphChanged)
		{
			++ChangedGraphCount;
		}
		if (bGraphSaved)
		{
			++SavedGraphCount;
		}
		if (!bGraphDesignComplete)
		{
			++DesignIncompleteGraphCount;
		}
		GraphResults.Add(MakeShared<FJsonValueObject>(ResultItem));
	}

	bool bSaved = false;
	FString SaveError;
	if (bSaveAfterLayout)
	{
		bSaved = SaveBlueprintPackage(Blueprint, SaveError);
		if (!bSaved)
		{
			if (LayoutAllTransaction)
			{
				LayoutAllTransaction->Cancel();
			}
			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
			OutData->SetArrayField(TEXT("graphs"), GraphResults);
			OutError = SaveError.IsEmpty() ? TEXT("save_failed") : SaveError;
			return false;
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Blueprint->GetPathName());
	OutData->SetBoolField(TEXT("skip_empty_graphs"), bSkipEmptyGraphs);
	OutData->SetBoolField(TEXT("stop_on_error"), bStopOnError);
	OutData->SetBoolField(TEXT("include_graph_result_details"), bIncludeGraphResultDetails);
	OutData->SetNumberField(TEXT("graph_count"), Targets.Num());
	OutData->SetNumberField(TEXT("processed_graph_count"), ProcessedGraphCount);
	OutData->SetNumberField(TEXT("skipped_graph_count"), SkippedGraphCount);
	OutData->SetNumberField(TEXT("failed_graph_count"), FailedGraphCount);
	OutData->SetNumberField(TEXT("changed_graph_count"), ChangedGraphCount);
	OutData->SetNumberField(TEXT("saved_graph_count"), SavedGraphCount);
	OutData->SetBoolField(TEXT("saved"), bSaved);
	OutData->SetNumberField(TEXT("design_incomplete_graph_count"), DesignIncompleteGraphCount);
	OutData->SetBoolField(TEXT("all_succeeded"), FailedGraphCount == 0);
	OutData->SetBoolField(TEXT("layout_design_complete"), FailedGraphCount == 0 && DesignIncompleteGraphCount == 0);
	OutData->SetNumberField(TEXT("total_node_count"), TotalNodeCount);
	OutData->SetNumberField(TEXT("total_edge_count"), TotalEdgeCount);
	OutData->SetNumberField(TEXT("total_moved_node_count"), TotalMovedNodeCount);
	OutData->SetNumberField(TEXT("total_reroute_plan_count"), TotalReroutePlanCount);
	OutData->SetNumberField(TEXT("total_created_reroute_node_count"), TotalCreatedRerouteNodeCount);
	OutData->SetNumberField(TEXT("total_route_through_island_count"), TotalRouteThroughIslandCount);
	OutData->SetNumberField(TEXT("total_route_through_hard_occupancy_count"), TotalRouteThroughHardOccupancyCount);
	OutData->SetArrayField(TEXT("graphs"), GraphResults);
	return FailedGraphCount == 0 || !bStopOnError;
}
	}
}
