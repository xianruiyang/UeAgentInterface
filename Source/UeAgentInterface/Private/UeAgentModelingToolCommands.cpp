// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentModelingToolCommands.h"

#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "FUeAgentModelingToolCommands"

FUeAgentModelingToolCommands::FUeAgentModelingToolCommands()
	: TCommands<FUeAgentModelingToolCommands>(
		TEXT("UeAgentModelingToolCommands"),
		NSLOCTEXT("Contexts", "UeAgentModelingToolCommands", "UeAgentInterface Modeling Tools"),
		NAME_None,
		FAppStyle::GetAppStyleSetName())
{
}

void FUeAgentModelingToolCommands::RegisterCommands()
{
	UI_COMMAND(UeAgentPrimitivesTabButton, "UAI", "UeAgentInterface primitive tools", EUserInterfaceActionType::RadioButton, FInputChord());
	UI_COMMAND(BeginAddRampPrimitiveTool, "Ramp", "Create a triangular-prism ramp primitive", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(BeginAddRampCornerPrimitiveTool, "RampCorner", "Create a square-based ramp-corner pyramid primitive", EUserInterfaceActionType::ToggleButton, FInputChord());
}

#undef LOCTEXT_NAMESPACE
