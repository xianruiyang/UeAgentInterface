// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationProduction.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugRegistry.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	UEAGENTINTERFACE_API bool TryCreatePieRuntimeDebugEditorSession(
		const EditorSessionOperation::FUeAgentEditorSessionOperationTypedRequest&
			Request,
		TSharedRef<FUeAgentPieRuntimeDebugRegistry, ESPMode::ThreadSafe> Registry,
		TSharedPtr<
			EditorSessionOperation::IUeAgentEditorSessionNativeSession,
			ESPMode::ThreadSafe>& OutSession,
		ExecutionMutation::FExecutionMutationError& OutError);
}
