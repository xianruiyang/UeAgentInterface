// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/ModelingGeometry/UeAgentModelingSession.h"
#include "Dom/JsonValue.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "DomainAdapters/ModelingGeometry/UeAgentModelingMechanics.h"

#include "Module/UeAgentInterfaceLogger.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "InteractiveToolManager.h"
#include "ModelingToolsEditorMode.h"
#include "Selection/GeometrySelectionManager.h"
#include "Selection/GeometrySelector.h"
#include "Tools/EdModeInteractiveToolsContext.h"

namespace
{
	TSharedPtr<FJsonObject> MakeModelingSessionVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("x"), Value.X);
		Result->SetNumberField(TEXT("y"), Value.Y);
		Result->SetNumberField(TEXT("z"), Value.Z);
		return Result;
	}
}

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::Activate(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	FlushToolsContext(Context, 3);
	OutData->SetBoolField(TEXT("mode_active"), true);
	OutData->SetStringField(TEXT("mode_id"), UModelingToolsEditorMode::EM_ModelingToolsEditorModeId.ToString());
	FUeAgentInterfaceLogger::Log(TEXT("ModelingActivateMode req=%s"), *Ctx.RequestId);
	return true;
}

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::ObserveSelection(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ActorsJson;
	if (GEditor)
	{
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				ActorsJson.Add(MakeShared<FJsonValueObject>(MakeActorSummaryJson(Actor)));
			}
		}
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	UGeometrySelectionManager::EGeometryTopologyType TopologyType = UGeometrySelectionManager::EGeometryTopologyType::Triangle;
	UGeometrySelectionManager::EGeometryElementType ElementType = UGeometrySelectionManager::EGeometryElementType::Face;
	int32 NumTargets = 0;
	bool bIsEmpty = true;
	if (SelectionManager)
	{
		SelectionManager->GetActiveSelectionInfo(TopologyType, ElementType, NumTargets, bIsEmpty);
	}

	OutData->SetArrayField(TEXT("selected_actors"), ActorsJson);
	OutData->SetStringField(TEXT("mesh_selection_element_type"), SelectionManager ? ElementTypeToString(ElementType) : TEXT("face"));
	OutData->SetStringField(TEXT("mesh_topology_mode"), SelectionManager ? TopologyModeToString(SelectionManager->GetMeshTopologyMode()) : TEXT("triangle"));
	OutData->SetNumberField(TEXT("mesh_target_count"), NumTargets);
	OutData->SetBoolField(TEXT("mesh_selection_empty"), bIsEmpty);
	return true;
}

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::SetActorSelection(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	UWorld* World = ResolveEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("actor_ids"), ActorIds) || !ActorIds)
	{
		OutError = ModelingInputFailure(TEXT("actor_ids_required"), TEXT("Setting actor selection requires an actor_ids array."), Ctx.Params, TEXT("actor_ids"), MakeShared<FJsonValueString>(TEXT("array of actor references")));
		return false;
	}

	const bool bSyncGeometryTargets = !Ctx.Params->HasField(TEXT("sync_geometry_targets")) || Ctx.Params->GetBoolField(TEXT("sync_geometry_targets"));
	const TArray<AActor*> Actors = ResolveActorsByIds(World, *ActorIds);
	SelectActors(Actors);
	if (bSyncGeometryTargets)
	{
		SyncGeometryTargets(Actors, Context);
	}

	OutData->SetNumberField(TEXT("selected_actor_count"), Actors.Num());
	if (UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr)
	{
		OutData->SetBoolField(TEXT("has_active_targets"), SelectionManager->HasActiveTargets());
		OutData->SetBoolField(TEXT("has_selection"), SelectionManager->HasSelection());
	}
	FUeAgentInterfaceLogger::Log(TEXT("ModelingSetSelection req=%s count=%d sync_targets=%s has_active_targets=%s"),
		*Ctx.RequestId,
		Actors.Num(),
		bSyncGeometryTargets ? TEXT("true") : TEXT("false"),
		(Context.Mode && Context.Mode->GetSelectionManager() && Context.Mode->GetSelectionManager()->HasActiveTargets()) ? TEXT("true") : TEXT("false"));
	return true;
}

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::SetSelectionMode(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		OutError = ModelingCondition(TEXT("modeling_selection_manager_unavailable"), TEXT("The modeling mode has no geometry selection manager."), TEXT("/selection_manager_available"), false, true);
		return false;
	}

	FString ElementValue = TEXT("face");
	FString TopologyValue = TEXT("triangle");
	bool bConvertExisting = true;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("element_type"), ElementValue);
		Ctx.Params->TryGetStringField(TEXT("topology_mode"), TopologyValue);
		Ctx.Params->TryGetBoolField(TEXT("convert_existing_selection"), bConvertExisting);
	}

	const UGeometrySelectionManager::EGeometryElementType ElementType = ParseElementType(ElementValue);
	const UGeometrySelectionManager::EMeshTopologyMode TopologyMode = ParseTopologyMode(TopologyValue);
	SelectionManager->SetMeshSelectionTypeAndMode(ElementType, TopologyMode, bConvertExisting);

	OutData->SetStringField(TEXT("element_type"), ElementTypeToString(ElementType));
	OutData->SetStringField(TEXT("topology_mode"), TopologyModeToString(TopologyMode));
	OutData->SetBoolField(TEXT("convert_existing_selection"), bConvertExisting);
	return true;
}

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::ObserveMeshSelectionInfo(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		OutError = ModelingCondition(TEXT("modeling_selection_manager_unavailable"), TEXT("The modeling mode has no geometry selection manager."), TEXT("/selection_manager_available"), false, true);
		return false;
	}

	UGeometrySelectionManager::EGeometryTopologyType TopologyType = UGeometrySelectionManager::EGeometryTopologyType::Triangle;
	UGeometrySelectionManager::EGeometryElementType ElementType = UGeometrySelectionManager::EGeometryElementType::Face;
	int32 NumTargets = 0;
	bool bIsEmpty = true;
	SelectionManager->GetActiveSelectionInfo(TopologyType, ElementType, NumTargets, bIsEmpty);

	OutData->SetStringField(TEXT("element_type"), ElementTypeToString(ElementType));
	OutData->SetStringField(TEXT("topology_mode"), TopologyModeToString(SelectionManager->GetMeshTopologyMode()));
	OutData->SetNumberField(TEXT("target_count"), NumTargets);
	OutData->SetBoolField(TEXT("is_empty"), bIsEmpty);
	OutData->SetBoolField(TEXT("has_active_targets"), SelectionManager->HasActiveTargets());
	return true;
}

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::ClearMeshSelection(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		OutError = ModelingCondition(TEXT("modeling_selection_manager_unavailable"), TEXT("The modeling mode has no geometry selection manager."), TEXT("/selection_manager_available"), false, true);
		return false;
	}

	const bool bSaveSelectionBeforeClear = [&Ctx]()
	{
		bool bValue = false;
		if (Ctx.Params.IsValid())
		{
			Ctx.Params->TryGetBoolField(TEXT("save_selection_before_clear"), bValue);
		}
		return bValue;
	}();

	SelectionManager->ClearSelection(bSaveSelectionBeforeClear);
	FlushToolsContext(Context, 3);

	UGeometrySelectionManager::EGeometryTopologyType TopologyType = UGeometrySelectionManager::EGeometryTopologyType::Triangle;
	UGeometrySelectionManager::EGeometryElementType ElementType = UGeometrySelectionManager::EGeometryElementType::Face;
	int32 NumTargets = 0;
	bool bIsEmpty = true;
	SelectionManager->GetActiveSelectionInfo(TopologyType, ElementType, NumTargets, bIsEmpty);

	OutData->SetBoolField(TEXT("selection_cleared"), true);
	OutData->SetBoolField(TEXT("save_selection_before_clear"), bSaveSelectionBeforeClear);
	OutData->SetStringField(TEXT("element_type"), ElementTypeToString(ElementType));
	OutData->SetStringField(TEXT("topology_mode"), TopologyModeToString(SelectionManager->GetMeshTopologyMode()));
	OutData->SetNumberField(TEXT("target_count"), NumTargets);
	OutData->SetBoolField(TEXT("is_empty"), bIsEmpty);
	OutData->SetBoolField(TEXT("has_active_targets"), SelectionManager->HasActiveTargets());
	return true;
}

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::SelectViaScreen(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		OutError = ModelingCondition(TEXT("modeling_selection_manager_unavailable"), TEXT("The modeling mode has no geometry selection manager."), TEXT("/selection_manager_available"), false, true);
		return false;
	}
	if (!SelectionManager->HasActiveTargets())
	{
		OutError = ModelingCondition(TEXT("modeling_selection_targets_empty"), TEXT("The geometry selection manager has no active targets for the selection query."), TEXT("/active_targets_present"), false, true);
		return false;
	}

	FVector2D ScreenPoint(0.0f, 0.0f);
	if (!ResolveViewportScreenPoint(Context.ViewportClient, Ctx.Params, ScreenPoint, OutError))
	{
		return false;
	}

	FRay3d WorldRay;
	if (!DeprojectViewportScreenPoint(Context, ScreenPoint, WorldRay, OutError))
	{
		return false;
	}

	FGeometrySelectionUpdateConfig UpdateConfig;
	FString ChangeTypeText = TEXT("replace");
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("change_type"), ChangeTypeText);
	}
	UpdateConfig.ChangeType = ParseSelectionChangeType(ChangeTypeText);

	const bool bClearOnMiss = [&Ctx, &UpdateConfig]()
	{
		bool bValue = (UpdateConfig.ChangeType == EGeometrySelectionChangeType::Replace);
		if (Ctx.Params.IsValid())
		{
			Ctx.Params->TryGetBoolField(TEXT("clear_on_miss"), bValue);
		}
		return bValue;
	}();

	FGeometrySelectionUpdateResult Result;
	SelectionManager->UpdateSelectionViaRaycast(WorldRay, UpdateConfig, Result);
	if (Result.bSelectionMissed && bClearOnMiss)
	{
		SelectionManager->ClearSelection();
	}
	FlushToolsContext(Context, 3);

	OutData->SetObjectField(TEXT("screen_point"), MakeModelingSessionVectorJson(FVector(ScreenPoint.X, ScreenPoint.Y, 0.0)));
	OutData->SetObjectField(TEXT("world_origin"), MakeModelingSessionVectorJson((FVector)WorldRay.Origin));
	OutData->SetObjectField(TEXT("world_direction"), MakeModelingSessionVectorJson((FVector)WorldRay.Direction));
	OutData->SetBoolField(TEXT("clear_on_miss"), bClearOnMiss);
	FillSelectionUpdateJson(SelectionManager, UpdateConfig, Result, OutData);
	return true;
}

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::SelectViaWorldRay(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		OutError = ModelingCondition(TEXT("modeling_selection_manager_unavailable"), TEXT("The modeling mode has no geometry selection manager."), TEXT("/selection_manager_available"), false, true);
		return false;
	}
	if (!SelectionManager->HasActiveTargets())
	{
		OutError = ModelingCondition(TEXT("modeling_selection_targets_empty"), TEXT("The geometry selection manager has no active targets for the selection query."), TEXT("/active_targets_present"), false, true);
		return false;
	}

	FRay3d WorldRay;
	if (!ResolveWorldRayFromParams(Ctx.Params, WorldRay, OutError))
	{
		return false;
	}

	FGeometrySelectionUpdateConfig UpdateConfig;
	FString ChangeTypeText = TEXT("replace");
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("change_type"), ChangeTypeText);
	}
	UpdateConfig.ChangeType = ParseSelectionChangeType(ChangeTypeText);

	const bool bClearOnMiss = [&Ctx, &UpdateConfig]()
	{
		bool bValue = (UpdateConfig.ChangeType == EGeometrySelectionChangeType::Replace);
		if (Ctx.Params.IsValid())
		{
			Ctx.Params->TryGetBoolField(TEXT("clear_on_miss"), bValue);
		}
		return bValue;
	}();

	FGeometrySelectionUpdateResult Result;
	SelectionManager->UpdateSelectionViaRaycast(WorldRay, UpdateConfig, Result);
	if (Result.bSelectionMissed && bClearOnMiss)
	{
		SelectionManager->ClearSelection();
	}
	FlushToolsContext(Context, 3);

	OutData->SetObjectField(TEXT("world_origin"), MakeModelingSessionVectorJson((FVector)WorldRay.Origin));
	OutData->SetObjectField(TEXT("world_direction"), MakeModelingSessionVectorJson((FVector)WorldRay.Direction));
	OutData->SetBoolField(TEXT("clear_on_miss"), bClearOnMiss);
	FillSelectionUpdateJson(SelectionManager, UpdateConfig, Result, OutData);
	return true;
}
