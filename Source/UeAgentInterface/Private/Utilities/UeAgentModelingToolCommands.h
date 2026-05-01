// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"

class FUeAgentModelingToolCommands final : public TCommands<FUeAgentModelingToolCommands>
{
public:
	FUeAgentModelingToolCommands();

	virtual void RegisterCommands() override;

	TSharedPtr<FUICommandInfo> UeAgentPrimitivesTabButton;
	TSharedPtr<FUICommandInfo> BeginAddRampPrimitiveTool;
	TSharedPtr<FUICommandInfo> BeginAddRampCornerPrimitiveTool;
};
