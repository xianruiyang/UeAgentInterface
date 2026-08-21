// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Modeling.h"
#include "UeAgentHttpServer_ModelingOps.h"

#include "Module/UeAgentInterfaceLogger.h"

#include "GameFramework/Actor.h"
#include "InteractiveTool.h"
#include "InteractiveToolManager.h"
#include "Tools/EdModeInteractiveToolsContext.h"

namespace
{
	bool HasAnyField(const TSharedPtr<FJsonObject>& Params, const TArray<const TCHAR*>& FieldNames, FString* OutFieldName = nullptr)
	{
		if (!Params.IsValid())
		{
			return false;
		}

		for (const TCHAR* FieldName : FieldNames)
		{
			if (Params->HasField(FieldName))
			{
				if (OutFieldName)
				{
					*OutFieldName = FieldName;
				}
				return true;
			}
		}
		return false;
	}

	bool HasPrimitiveOnlyParameters(const TSharedPtr<FJsonObject>& Params, FString& OutFieldName)
	{
		static const TArray<const TCHAR*> PrimitiveOnlyFields = {
			TEXT("bounds"), TEXT("bounds_center"), TEXT("bounds_extent"), TEXT("bounds_min"), TEXT("bounds_max"),
			TEXT("rotation"), TEXT("orientation"), TEXT("label"), TEXT("folder_path"),
			TEXT("auto_generate_collision"), TEXT("collision")
		};
		return HasAnyField(Params, PrimitiveOnlyFields, &OutFieldName);
	}

	bool HasPrimitiveBoundsParameters(const TSharedPtr<FJsonObject>& Params)
	{
		static const TArray<const TCHAR*> BoundsFields = {
			TEXT("bounds"), TEXT("bounds_center"), TEXT("bounds_extent"), TEXT("bounds_min"), TEXT("bounds_max")
		};
		return HasAnyField(Params, BoundsFields);
	}
}

bool FUeAgentLegacyMechanics::CmdModelingStartTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	const FString RequestCommand = Ctx.Command.TrimStartAndEnd().ToLower();
	const FModelingToolWrapperDefinition* WrapperDefinition = GetModelingToolWrapperRegistry().Find(RequestCommand);
	const bool bInternalPivotTool = RequestCommand == TEXT("modeling_set_pivot");
	const FString WrapperCommand = (WrapperDefinition || bInternalPivotTool) ? RequestCommand : FString();
	FString ToolIdentifier;
	FString PostAction;
	EModelingOutputTypePolicy OutputTypePolicy = EModelingOutputTypePolicy::DynamicMeshActorDefault;
	if (WrapperDefinition)
	{
		static const TArray<const TCHAR*> WrapperInternalFields = {
			TEXT("__uai_wrapper_command"), TEXT("tool_identifier"), TEXT("post_action")
		};
		if (HasAnyField(Ctx.Params, WrapperInternalFields))
		{
			OutError = TEXT("internal_tool_wrapper_parameters_forbidden");
			return false;
		}
		ToolIdentifier = WrapperDefinition->ToolIdentifier;
		PostAction = WrapperDefinition->PostAction;
		OutputTypePolicy = WrapperDefinition->OutputTypePolicy;
		if (!PostAction.IsEmpty() && !WrapperDefinition->bPostActionSupported)
		{
			OutError = WrapperDefinition->UnsupportedActionError.IsEmpty()
				? TEXT("tool_action_not_supported")
				: WrapperDefinition->UnsupportedActionError;
			return false;
		}
	}
	else if (bInternalPivotTool)
	{
		ToolIdentifier = TEXT("BeginEditPivotTool");
		if (Ctx.Params.IsValid())
		{
			Ctx.Params->TryGetStringField(TEXT("post_action"), PostAction);
		}
	}
	else
	{
		static const TArray<const TCHAR*> InternalFields = {
			TEXT("__uai_wrapper_command"), TEXT("post_action")
		};
		if (HasAnyField(Ctx.Params, InternalFields))
		{
			OutError = TEXT("internal_tool_wrapper_parameters_forbidden");
			return false;
		}
		FString RequestedToolIdentifier;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("tool_identifier"), RequestedToolIdentifier) || RequestedToolIdentifier.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("tool_identifier_required");
			return false;
		}
		if (!ResolveAllowedModelingToolIdentifier(RequestedToolIdentifier, ToolIdentifier))
		{
			OutError = TEXT("tool_identifier_not_allowed");
			return false;
		}
	}

	static const TArray<const TCHAR*> SingletonPropertyFields = {
		TEXT("property_set"), TEXT("property_path"), TEXT("value_kind"), TEXT("value")
	};
	if (HasAnyField(Ctx.Params, SingletonPropertyFields))
	{
		OutError = TEXT("tool_properties_array_required");
		return false;
	}

	FString PrimitiveOnlyField;
	const bool bHasPrimitiveOnlyParameters = HasPrimitiveOnlyParameters(Ctx.Params, PrimitiveOnlyField);
	if (bHasPrimitiveOnlyParameters && !bInternalPivotTool && (!WrapperDefinition || !WrapperDefinition->bPrimitiveCreation))
	{
		OutData->SetStringField(TEXT("rejected_parameter"), PrimitiveOnlyField);
		OutError = TEXT("primitive_parameters_require_create_wrapper");
		return false;
	}

	TSharedPtr<FJsonObject> EffectiveParams = Ctx.Params.IsValid() ? MakeShared<FJsonObject>(*Ctx.Params) : MakeShared<FJsonObject>();
	FPrimitiveBoundsPlacement PrimitivePlacement;
	TSharedPtr<FJsonObject> DerivedPrimitiveData;
	if (!PreparePrimitivePlacementParameters(WrapperCommand, ToolIdentifier, EffectiveParams, EffectiveParams, PrimitivePlacement, DerivedPrimitiveData, OutError))
	{
		return false;
	}
	if (WrapperDefinition && WrapperDefinition->bPrimitiveCreation && bHasPrimitiveOnlyParameters && !PrimitivePlacement.bEnabled)
	{
		OutError = HasPrimitiveBoundsParameters(EffectiveParams)
			? TEXT("primitive_bounds_incomplete")
			: TEXT("primitive_options_require_bounds");
		return false;
	}

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	TArray<AActor*> SelectionBeforeAccept;
	GetSelectedActors(SelectionBeforeAccept);

	ConfigureDefaultModelingOutputType(OutputTypePolicy);
	if (!StartToolByIdentifier(ToolIdentifier, Context, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("ModelingStartToolFailed req=%s tool=%s error=%s"), *Ctx.RequestId, *ToolIdentifier, *OutError);
		return false;
	}
	const auto CancelStartedTool = [&Context]()
	{
		FString CleanupError;
		FinishTool(Context, EToolShutdownType::Cancel, CleanupError);
	};

	UInteractiveTool* Tool = Context.ToolsContext->ToolManager ? Context.ToolsContext->ToolManager->GetActiveTool(EToolSide::Left) : nullptr;
	TArray<TSharedPtr<FJsonValue>> PropertyImportResults;
	if (Tool && !AutoPrepareModelingOutputType(EffectiveParams, Tool, OutputTypePolicy, OutError, OutData))
	{
		CancelStartedTool();
		return false;
	}
	if (Tool && !ApplyToolProperties(EffectiveParams, Tool, OutError, &PropertyImportResults))
	{
		OutData->SetArrayField(TEXT("property_import_results"), PropertyImportResults);
		CancelStartedTool();
		return false;
	}
	if (Tool && !AutoPreparePrimitiveCreationTool(EffectiveParams, Tool, ToolIdentifier, OutError))
	{
		CancelStartedTool();
		return false;
	}

	if (!PostAction.IsEmpty())
	{
		if (!InvokeToolAction(Tool, PostAction))
		{
			CancelStartedTool();
			OutError = WrapperDefinition && !WrapperDefinition->UnsupportedActionError.IsEmpty()
				? WrapperDefinition->UnsupportedActionError
				: TEXT("tool_action_not_found");
			return false;
		}
		OutData->SetStringField(TEXT("invoked_action"), PostAction);
	}

	FlushToolsContext(Context, 3);

	bool bAutoAccept = false;
	if (EffectiveParams.IsValid())
	{
		EffectiveParams->TryGetBoolField(TEXT("accept"), bAutoAccept);
	}
	if (bAutoAccept)
	{
		if (!FinishTool(Context, EToolShutdownType::Accept, OutError))
		{
			return false;
		}

		TArray<AActor*> SelectionAfterAccept;
		GetSelectedActors(SelectionAfterAccept);
		AActor* AcceptedOutputActor = ResolveCreatedActorFromSelectionDelta(SelectionBeforeAccept, SelectionAfterAccept);

		if (PrimitivePlacement.bEnabled)
		{
			if (!ApplyPrimitivePlacementToActor(AcceptedOutputActor, PrimitivePlacement, OutError))
			{
				return false;
			}
			ApplyOptionalCreatedActorMetadata(AcceptedOutputActor, EffectiveParams);

			if (!AutoGeneratePrimitiveCollisionIfMissing(AcceptedOutputActor, PrimitivePlacement, DerivedPrimitiveData, EffectiveParams, OutData, OutError))
			{
				return false;
			}

			OutData->SetBoolField(TEXT("used_bounds_placement"), true);
			OutData->SetObjectField(TEXT("created_actor"), MakeActorSummaryJson(AcceptedOutputActor));
			OutData->SetObjectField(TEXT("created_actor_location"), MakeVectorJson(AcceptedOutputActor->GetActorLocation()));
			OutData->SetObjectField(TEXT("created_actor_rotation"), MakeRotatorJson(AcceptedOutputActor->GetActorRotation()));
			if (DerivedPrimitiveData.IsValid())
			{
				OutData->SetObjectField(TEXT("derived_primitive"), DerivedPrimitiveData);
			}
		}
		else if (OutputTypePolicy == EModelingOutputTypePolicy::StaticMeshRequired)
		{
			const FString CreatedAssetPath = ResolveAcceptedOutputAssetPath(AcceptedOutputActor);
			if (CreatedAssetPath.IsEmpty())
			{
				OutError = TEXT("accepted_static_mesh_output_resolution_failed");
				return false;
			}
			OutData->SetObjectField(TEXT("created_actor"), MakeActorSummaryJson(AcceptedOutputActor));
			OutData->SetStringField(TEXT("created_asset_path"), CreatedAssetPath);
		}
		else if (OutputTypePolicy == EModelingOutputTypePolicy::DynamicMeshActorRequired)
		{
			if (!IsAcceptedDynamicMeshOutput(AcceptedOutputActor))
			{
				OutError = TEXT("accepted_dynamic_mesh_output_resolution_failed");
				return false;
			}
			OutData->SetObjectField(TEXT("created_actor"), MakeActorSummaryJson(AcceptedOutputActor));
		}
	}

	FillActiveToolData(Context, OutData);
	if (PropertyImportResults.Num() > 0)
	{
		OutData->SetArrayField(TEXT("property_import_results"), PropertyImportResults);
	}
	OutData->SetStringField(TEXT("requested_tool_identifier"), ToolIdentifier);
	OutData->SetBoolField(TEXT("accepted"), bAutoAccept);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingStartTool req=%s tool=%s accepted=%s"), *Ctx.RequestId, *ToolIdentifier, bAutoAccept ? TEXT("true") : TEXT("false"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdModelingGetActiveTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	FillActiveToolData(Context, OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdModelingGetActiveToolProperties(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdModelingGetActiveTool(Ctx, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdModelingSetActiveToolProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UInteractiveTool* Tool = Context.ToolsContext && Context.ToolsContext->ToolManager ? Context.ToolsContext->ToolManager->GetActiveTool(EToolSide::Left) : nullptr;
	if (!Tool)
	{
		OutError = TEXT("no_active_tool");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ToolProperties = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("tool_properties"), ToolProperties) || !ToolProperties || ToolProperties->Num() != 1)
	{
		OutError = TEXT("exactly_one_tool_property_required");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	if (!ApplyToolProperties(Ctx.Params, Tool, OutError, &PropertyResults))
	{
		OutData->SetArrayField(TEXT("property_results"), PropertyResults);
		return false;
	}

	FlushToolsContext(Context, 3);
	FillActiveToolData(Context, OutData);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	return true;
}

bool FUeAgentLegacyMechanics::CmdModelingInvokeActiveToolAction(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UInteractiveTool* Tool = Context.ToolsContext && Context.ToolsContext->ToolManager ? Context.ToolsContext->ToolManager->GetActiveTool(EToolSide::Left) : nullptr;
	if (!Tool)
	{
		OutError = TEXT("no_active_tool");
		return false;
	}

	FString ActionName;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("action_name"), ActionName) || ActionName.IsEmpty())
	{
		OutError = TEXT("action_name_required");
		return false;
	}

	if (!InvokeToolAction(Tool, ActionName))
	{
		OutError = TEXT("tool_action_not_found");
		return false;
	}

	FlushToolsContext(Context, 3);
	FillActiveToolData(Context, OutData);
	OutData->SetStringField(TEXT("invoked_action"), ActionName);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingInvokeAction req=%s tool=%s action=%s"), *Ctx.RequestId, *Context.ToolsContext->GetActiveToolName(), *ActionName);
	return true;
}

bool FUeAgentLegacyMechanics::CmdModelingAcceptTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	if (!FinishTool(Context, EToolShutdownType::Accept, OutError))
	{
		return false;
	}

	FillActiveToolData(Context, OutData);
	OutData->SetBoolField(TEXT("accepted"), true);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingAcceptTool req=%s"), *Ctx.RequestId);
	return true;
}

bool FUeAgentLegacyMechanics::CmdModelingCancelTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	if (!FinishTool(Context, EToolShutdownType::Cancel, OutError))
	{
		return false;
	}

	FillActiveToolData(Context, OutData);
	OutData->SetBoolField(TEXT("cancelled"), true);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingCancelTool req=%s"), *Ctx.RequestId);
	return true;
}
