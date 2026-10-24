#include "DomainAdapters/ModelingGeometry/UeAgentModelingSession.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingMechanics.h"
#include "Dom/JsonValue.h"

#include "Editor.h"
#include "Engine/World.h"

namespace UE::UAI::DomainAdapters::ModelingGeometry
{
	bool FModelingSession::ExecuteAction(
		const FString& Action,
		const FModelingSessionRequest& Request,
		TSharedPtr<FJsonObject>& OutObservation,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (Action == TEXT("activate")) return Activate(Request, OutObservation, OutError);
		if (Action == TEXT("observe_tool")) return ObserveTool(Request, OutObservation, OutError);
		if (Action == TEXT("start_tool")) return StartTool(Request, OutObservation, OutError);
		if (Action == TEXT("set_tool_property")) return SetToolProperty(Request, OutObservation, OutError);
		if (Action == TEXT("invoke_tool_action")) return InvokeToolAction(Request, OutObservation, OutError);
		if (Action == TEXT("accept_tool")) return AcceptTool(Request, OutObservation, OutError);
		if (Action == TEXT("cancel_tool")) return CancelTool(Request, OutObservation, OutError);
		if (Action == TEXT("set_actor_selection")) return SetActorSelection(Request, OutObservation, OutError);
		if (Action == TEXT("set_selection_mode")) return SetSelectionMode(Request, OutObservation, OutError);
		if (Action == TEXT("clear_mesh_selection")) return ClearMeshSelection(Request, OutObservation, OutError);
		if (Action == TEXT("select_via_screen")) return SelectViaScreen(Request, OutObservation, OutError);
		if (Action == TEXT("save_mesh_asset")) return SaveMeshAsset(Request, OutObservation, OutError);

		OutError = UeAgentModelingOps::ModelingFact(TEXT("modeling_session_action_unknown"), TEXT("The modeling session has no implementation for this action."), TEXT("/action"), MakeShared<FJsonValueString>(Action), MakeShared<FJsonValueString>(TEXT("supported modeling session action")));
		return false;
	}

	UWorld* FModelingSession::ResolveEditorWorld(UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!GEditor)
		{
			OutError = UeAgentModelingOps::ModelingCondition(TEXT("no_geditor"), TEXT("The modeling operation requires an available Unreal Editor."), TEXT("/editor_available"), false, true);
			return nullptr;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			OutError = UeAgentModelingOps::ModelingCondition(TEXT("no_editor_world"), TEXT("The Unreal Editor has no editor world for the modeling operation."), TEXT("/editor_world_available"), false, true);
			return nullptr;
		}
		return World;
	}
}
