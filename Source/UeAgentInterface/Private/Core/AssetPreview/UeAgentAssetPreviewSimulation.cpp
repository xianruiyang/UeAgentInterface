#include "Core/AssetPreview/UeAgentAssetPreviewState.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"

namespace UE::UAI::AssetPreview
{
	namespace
	{
		TSharedPtr<FUICommandList> Commands(UObject* Asset)
		{
			auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			auto* Editor = Subsystem && IsPhysicsPreview(Asset) ? Subsystem->FindEditorForAsset(Asset, false) : nullptr;
			if (!Editor || Editor->GetEditorName() != TEXT("PhysicsAssetEditor")) return nullptr;
			return static_cast<FAssetEditorToolkit*>(Editor)->GetToolkitCommands();
		}
		TSharedPtr<FUICommandInfo> Command(FName Name)
		{
			return FInputBindingManager::Get().FindCommandInContext(TEXT("PhysicsAssetEditor"), Name);
		}
		bool Read(const TSharedPtr<FUICommandList>& List, FName Name)
		{
			const auto Info = Command(Name);
			return List && Info && List->GetCheckState(Info.ToSharedRef()) == ECheckBoxState::Checked;
		}
		bool Set(const TSharedPtr<FUICommandList>& List, FName Name, bool Value, UObject* Asset, UE::UAI::Contract::FDiagnostic& Error)
		{
			const auto Info = Command(Name);
			if (!List || !Info) { Error = PreviewFailure(TEXT("preview_simulation_command_unavailable"), !List ? TEXT("The physics preview editor has no native simulation command list.") : TEXT("The requested physics simulation command is not registered."), Asset, Name.ToString()); return false; }
			if (Read(List, Name) == Value) return true;
			const bool bExecuted = List->TryExecuteAction(Info.ToSharedRef());
			const bool Actual = Read(List, Name);
			if (!bExecuted || Actual != Value)
			{ Error = PreviewFailure(TEXT("preview_simulation_command_not_applied"), !bExecuted ? TEXT("The native physics preview rejected the simulation command.") : TEXT("The simulation command completed without reaching the requested state."), Asset, Name.ToString()); Error.ExpectedValue = MakeShared<FJsonValueBoolean>(Value); Error.ActualValue = MakeShared<FJsonValueBoolean>(Actual); return false; }
			return true;
		}
	}
	bool IsPhysicsPreview(const UObject* Asset) { return Asset && Asset->IsA<UPhysicsAsset>(); }
	bool ReadSimulationState(UObject* Asset, FUeAgentAssetPreviewSimulation& Out)
	{
		const auto List = Commands(Asset);
		if (!List) return false;
		Out.NoGravity = Read(List, TEXT("SimulationNoGravity"));
		Out.FloorCollision = Read(List, TEXT("SimulationFloorCollision"));
		return true;
	}
	bool ApplySimulationState(UObject* Asset, const FUeAgentAssetPreviewSimulation& Value,
		const TSet<FName>& Fields, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const auto List = Commands(Asset);
		if (Fields.Contains(TEXT("NoGravity")) && !Set(List, TEXT("SimulationNoGravity"), Value.NoGravity, Asset, OutError)) return false;
		if (Fields.Contains(TEXT("FloorCollision")) && !Set(List, TEXT("SimulationFloorCollision"), Value.FloorCollision, Asset, OutError)) return false;
		return true;
	}
	bool ExecuteSimulationAction(UObject* Asset, const FString& Action,
		TSharedPtr<FJsonObject>& OutDetails, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const auto List = Commands(Asset);
		if (!List) { OutError = PreviewFailure(TEXT("preview_simulation_owner_unavailable"), TEXT("The physics asset has no open simulation editor with native commands."), Asset); return false; }
		if (Action == TEXT("play") || Action == TEXT("stop"))
		{
			if (!Set(List, TEXT("RepeatLastSimulation"), Action == TEXT("play"), Asset, OutError)) return false;
		}
		else if (Action != TEXT("status")) { OutError = PreviewValueFailure(TEXT("preview_simulation_action_unsupported"), TEXT("The action is not supported by physics preview."), TEXT("action"), MakeShared<FJsonValueString>(TEXT("play, stop or status")), MakeShared<FJsonValueString>(Action), Asset); OutError.SourceKind = TEXT("command_input"); return false; }
		OutDetails = MakeShared<FJsonObject>();
		OutDetails->SetStringField(TEXT("action"), Action);
		OutDetails->SetStringField(TEXT("source"), TEXT("live"));
		OutDetails->SetBoolField(TEXT("simulating"), Read(List, TEXT("RepeatLastSimulation")));
		OutDetails->SetBoolField(TEXT("all_bodies"), Read(List, TEXT("SimulationAll")));
		OutDetails->SetBoolField(TEXT("selected_bodies"), Read(List, TEXT("SelectedSimulation")));
		OutDetails->SetBoolField(TEXT("no_gravity"), Read(List, TEXT("SimulationNoGravity")));
		OutDetails->SetBoolField(TEXT("floor_collision"), Read(List, TEXT("SimulationFloorCollision")));
		return true;
	}
}
