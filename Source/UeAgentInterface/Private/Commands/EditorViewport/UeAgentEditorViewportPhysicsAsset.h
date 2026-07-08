// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Templates/SharedPointer.h"

namespace UeAgentEditorViewport
{
	class IViewportAdapter;

	TSharedRef<IViewportAdapter> MakePhysicsAssetViewportAdapter();
}
