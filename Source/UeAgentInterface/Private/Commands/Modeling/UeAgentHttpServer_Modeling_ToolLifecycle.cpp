// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Modeling.h"
#include "UeAgentHttpServer_ModelingOps.h"

#include "Module/UeAgentInterfaceLogger.h"

#include "GameFramework/Actor.h"
#include "InteractiveTool.h"
#include "InteractiveToolManager.h"
#include "Tools/EdModeInteractiveToolsContext.h"

bool FUeAgentHttpServer::CmdModelingStartTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	FString ToolIdentifier;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("tool_identifier"), ToolIdentifier))
	{
		OutError = TEXT("tool_identifier_required");
		return false;
	}
	ToolIdentifier = ResolveToolIdentifierAlias(ToolIdentifier);
	if (ToolIdentifier.IsEmpty())
	{
		OutError = TEXT("tool_identifier_required");
		return false;
	}

	FString WrapperCommand;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("__uai_wrapper_command"), WrapperCommand);
	}
	TSharedPtr<FJsonObject> EffectiveParams = Ctx.Params.IsValid() ? MakeShared<FJsonObject>(*Ctx.Params) : MakeShared<FJsonObject>();
	FPrimitiveBoundsPlacement PrimitivePlacement;
	TSharedPtr<FJsonObject> DerivedPrimitiveData;
	if (!PreparePrimitivePlacementParameters(WrapperCommand, ToolIdentifier, EffectiveParams, EffectiveParams, PrimitivePlacement, DerivedPrimitiveData, OutError))
	{
		return false;
	}

	TArray<AActor*> SelectionBeforeCreate;
	if (PrimitivePlacement.bEnabled)
	{
		GetSelectedActors(SelectionBeforeCreate);
	}

	ConfigureDefaultModelingOutputType();
	if (!StartToolByIdentifier(ToolIdentifier, Context, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("ModelingStartToolFailed req=%s tool=%s error=%s"), *Ctx.RequestId, *ToolIdentifier, *OutError);
		return false;
	}

	UInteractiveTool* Tool = Context.ToolsContext->ToolManager ? Context.ToolsContext->ToolManager->GetActiveTool(EToolSide::Left) : nullptr;
	TArray<TSharedPtr<FJsonValue>> PropertyImportResults;
	if (Tool && !AutoPrepareModelingOutputType(EffectiveParams, Tool, OutError, OutData))
	{
		return false;
	}
	if (Tool && !ApplyToolProperties(EffectiveParams, Tool, OutError, &PropertyImportResults))
	{
		OutData->SetArrayField(TEXT("property_import_results"), PropertyImportResults);
		return false;
	}
	if (Tool && !AutoPreparePrimitiveCreationTool(EffectiveParams, Tool, ToolIdentifier, OutError))
	{
		return false;
	}

	FString PostAction;
	if (EffectiveParams.IsValid() && EffectiveParams->TryGetStringField(TEXT("post_action"), PostAction) && !PostAction.IsEmpty())
	{
		if (!InvokeToolAction(Tool, PostAction))
		{
			OutError = TEXT("tool_action_not_found");
			return false;
		}
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

		if (PrimitivePlacement.bEnabled)
		{
			TArray<AActor*> SelectionAfterCreate;
			GetSelectedActors(SelectionAfterCreate);
			AActor* CreatedActor = ResolveCreatedActorFromSelectionDelta(SelectionBeforeCreate, SelectionAfterCreate);
			if (!ApplyPrimitivePlacementToActor(CreatedActor, PrimitivePlacement, OutError))
			{
				return false;
			}
			ApplyOptionalCreatedActorMetadata(CreatedActor, EffectiveParams);

			if (!AutoGeneratePrimitiveCollisionIfMissing(CreatedActor, PrimitivePlacement, DerivedPrimitiveData, EffectiveParams, OutData, OutError))
			{
				return false;
			}

			OutData->SetBoolField(TEXT("used_bounds_placement"), true);
			OutData->SetObjectField(TEXT("created_actor"), MakeActorSummaryJson(CreatedActor));
			OutData->SetObjectField(TEXT("created_actor_location"), MakeVectorJson(CreatedActor->GetActorLocation()));
			OutData->SetObjectField(TEXT("created_actor_rotation"), MakeRotatorJson(CreatedActor->GetActorRotation()));
			if (DerivedPrimitiveData.IsValid())
			{
				OutData->SetObjectField(TEXT("derived_primitive"), DerivedPrimitiveData);
			}
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

bool FUeAgentHttpServer::CmdModelingGetActiveTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdModelingGetActiveToolProperties(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdModelingGetActiveTool(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdModelingSetActiveToolProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FString Selector;
	FString PropertyName;
	FString ValueText;
	bool bHasValueText = false;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("property_set"), Selector);
		Ctx.Params->TryGetStringField(TEXT("property_name"), PropertyName);
		bHasValueText = Ctx.Params->TryGetStringField(TEXT("value_text"), ValueText);
	}
	if (PropertyName.IsEmpty() || !bHasValueText)
	{
		OutError = TEXT("tool_property_name_or_value_missing");
		return false;
	}

	UObject* PropertySet = FindToolPropertySet(Tool, Selector);
	if (!PropertySet)
	{
		OutError = TEXT("tool_property_set_not_found");
		return false;
	}
	FString AppliedValueText;
	FString CppType;
	if (!ImportResolvedProperty(PropertySet, PropertyName, ValueText, OutError, &AppliedValueText, &CppType))
	{
		const FString ImportStatus = OutError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetStringField(TEXT("property_set"), PropertySet->GetName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, OutError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		return false;
	}
	NotifyToolPropertySetUpdated(Tool, PropertySet);

	FlushToolsContext(Context, 3);
	FillActiveToolData(Context, OutData);
	OutData->SetStringField(TEXT("property_set"), PropertySet->GetName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingSetActiveToolProperty req=%s tool=%s set=%s property=%s requested=%s applied=%s cpp_type=%s exact=%s"),
		*Ctx.RequestId,
		*Context.ToolsContext->GetActiveToolName(),
		*PropertySet->GetName(),
		*PropertyName,
		*ValueText,
		*AppliedValueText,
		*CppType,
		AppliedValueText.Equals(ValueText, ESearchCase::CaseSensitive) ? TEXT("true") : TEXT("false"));
	return true;
}

bool FUeAgentHttpServer::CmdModelingInvokeActiveToolAction(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdModelingAcceptTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdModelingCancelTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
