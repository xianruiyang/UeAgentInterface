// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/ModelingGeometry/UeAgentModelingSession.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "DomainAdapters/ModelingGeometry/UeAgentModelingMechanics.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingMetricMaterial.h"
#include "Components/MeshComponent.h"
#include "Materials/Material.h"

#include "Module/UeAgentInterfaceLogger.h"

#include "GameFramework/Actor.h"
#include "InteractiveTool.h"
#include "InteractiveToolManager.h"
#include "Tools/EdModeInteractiveToolsContext.h"

namespace
{
	bool ApplyCreatedMetricMaterial(AActor* Actor, const TSharedPtr<FJsonObject>& Params, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Actor) return true;
		TOptional<FLinearColor> Color;
		if (!UE::UAI::ModelingMetricMaterial::TryReadColor(Params, Color))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_color_invalid"), TEXT("color must be three linear RGB numbers in [0,1]."), TEXT("modeling"));
			return false;
		}
		FString ExplicitMaterial;
		if (Params.IsValid()) Params->TryGetStringField(TEXT("material_ref"), ExplicitMaterial);
		TInlineComponentArray<UMeshComponent*> Components(Actor);
		for (UMeshComponent* Component : Components)
		{
			UMaterialInterface* Current = Component->GetMaterial(0);
			const bool bEngineDefault = !Current || Current == UMaterial::GetDefaultMaterial(MD_Surface)
				|| Current->GetPathName() == TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
			if (ExplicitMaterial.IsEmpty() && !bEngineDefault && !UE::UAI::ModelingMetricMaterial::IsMetric(Current))
			{
				if (!Color.IsSet()) continue;
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_color_requires_metric_material"), TEXT("The tool selected a custom material; its color must be edited through that material's parameters."), TEXT("modeling"));
				return false;
			}
			UMaterialInterface* Material = ExplicitMaterial.IsEmpty() ? UE::UAI::ModelingMetricMaterial::Load() : LoadObject<UMaterialInterface>(nullptr, *ExplicitMaterial);
			if (!Material || (Color.IsSet() && !UE::UAI::ModelingMetricMaterial::IsMetric(Material)))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_material_invalid"), TEXT("A loadable material is required; color applies only to the bundled metric material."), TEXT("modeling"));
				return false;
			}
			Component->Modify();
			Component->SetMaterial(0, Material);
			UE::UAI::ModelingMetricMaterial::Initialize(*Component, Component->CalcBounds(FTransform::Identity).GetBox(), Color);
		}
		Actor->MarkPackageDirty();
		return true;
	}

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

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::StartTool(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	const FString RequestCommand = Ctx.CommandId.TrimStartAndEnd().ToLower();
	const FModelingToolWrapperDefinition* WrapperDefinition = GetModelingToolWrapperRegistry().Find(RequestCommand);
	const bool bInternalPivotTool = RequestCommand == TEXT("modeling_set_pivot");
	const FString WrapperCommand = (WrapperDefinition || bInternalPivotTool) ? RequestCommand : FString();
	FString ToolIdentifier;
	FString PostAction;
	EModelingOutputTypePolicy OutputTypePolicy = EModelingOutputTypePolicy::DynamicMeshActorDefault;
	FString RejectedField;
	if (WrapperDefinition)
	{
		static const TArray<const TCHAR*> WrapperInternalFields = {
			TEXT("__uai_wrapper_command"), TEXT("tool_identifier"), TEXT("post_action")
		};
		if (HasAnyField(Ctx.Params, WrapperInternalFields, &RejectedField))
		{
			OutError = ModelingInputFailure(TEXT("internal_tool_wrapper_parameters_forbidden"), TEXT("The wrapper owns this tool-control field; omit it from the input."), Ctx.Params, RejectedField, MakeShared<FJsonValueString>(TEXT("field omitted")));
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		ToolIdentifier = WrapperDefinition->ToolIdentifier;
		PostAction = WrapperDefinition->PostAction;
		OutputTypePolicy = WrapperDefinition->OutputTypePolicy;
		if (!PostAction.IsEmpty() && !WrapperDefinition->bPostActionSupported)
		{
			OutError = ModelingFact(WrapperDefinition->UnsupportedActionError.IsEmpty()
				? TEXT("tool_action_not_supported")
				: WrapperDefinition->UnsupportedActionError, TEXT("The wrapper declares a post action that this native tool does not support."), TEXT("/post_action"), MakeShared<FJsonValueString>(PostAction), MakeShared<FJsonValueString>(TEXT("supported tool action")));
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
		if (HasAnyField(Ctx.Params, InternalFields, &RejectedField))
		{
			OutError = ModelingInputFailure(TEXT("internal_tool_wrapper_parameters_forbidden"), TEXT("This tool-control field is reserved for the command wrapper."), Ctx.Params, RejectedField, MakeShared<FJsonValueString>(TEXT("field omitted")));
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		FString RequestedToolIdentifier;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("tool_identifier"), RequestedToolIdentifier) || RequestedToolIdentifier.TrimStartAndEnd().IsEmpty())
		{
			OutError = ModelingInputFailure(TEXT("tool_identifier_required"), TEXT("Starting a tool requires a non-empty string tool_identifier."), Ctx.Params, TEXT("tool_identifier"), MakeShared<FJsonValueString>(TEXT("non-empty supported tool identifier")));
			return false;
		}
		if (!ResolveAllowedModelingToolIdentifier(RequestedToolIdentifier, ToolIdentifier))
		{
			OutError = ModelingInputFailure(TEXT("tool_identifier_not_allowed"), TEXT("The requested tool identifier is not in the supported modeling tool registry."), Ctx.Params, TEXT("tool_identifier"), MakeShared<FJsonValueString>(TEXT("registered modeling tool identifier")));
			return false;
		}
	}

	static const TArray<const TCHAR*> SingletonPropertyFields = {
		TEXT("property_set"), TEXT("property_path"), TEXT("kind"), TEXT("value")
	};
	if (HasAnyField(Ctx.Params, SingletonPropertyFields, &RejectedField))
	{
		OutError = ModelingInputFailure(TEXT("tool_properties_array_required"), TEXT("Tool property edits must be entries of tool_properties, rather than singleton input fields."), Ctx.Params, RejectedField, MakeShared<FJsonValueString>(TEXT("field inside a tool_properties array entry")));
		return false;
	}

	FString PrimitiveOnlyField;
	const bool bHasPrimitiveOnlyParameters = HasPrimitiveOnlyParameters(Ctx.Params, PrimitiveOnlyField);
	if (bHasPrimitiveOnlyParameters && !bInternalPivotTool && (!WrapperDefinition || !WrapperDefinition->bPrimitiveCreation))
	{
		OutData->SetStringField(TEXT("rejected_parameter"), PrimitiveOnlyField);
		OutError = ModelingInputFailure(TEXT("primitive_parameters_require_create_wrapper"), TEXT("This primitive-specific parameter requires a primitive creation wrapper."), Ctx.Params, PrimitiveOnlyField, MakeShared<FJsonValueString>(TEXT("primitive creation command")));
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
		OutError = ModelingInputFailure(HasPrimitiveBoundsParameters(EffectiveParams)
			? TEXT("primitive_bounds_incomplete")
			: TEXT("primitive_options_require_bounds"), TEXT("Primitive creation options require complete center/extent or min/max bounds."), Ctx.Params, PrimitiveOnlyField, MakeShared<FJsonValueString>(TEXT("complete primitive bounds")));
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
		FUeAgentInterfaceLogger::Log(TEXT("ModelingStartToolFailed req=%s tool=%s error=%s"), *Ctx.RequestId, *ToolIdentifier, *OutError.ToPublicDetail());
		return false;
	}
	const auto CancelStartedTool = [&Context, &OutError]()
	{
		UE::UAI::Contract::FDiagnostic CleanupError;
		if (!FinishTool(Context, EToolShutdownType::Cancel, CleanupError)) OutError.Append(CleanupError);
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
		if (!UeAgentModelingOps::InvokeToolAction(Tool, PostAction))
		{
			OutError = ModelingFact(WrapperDefinition && !WrapperDefinition->UnsupportedActionError.IsEmpty()
				? WrapperDefinition->UnsupportedActionError
				: TEXT("tool_action_not_found"), TEXT("The started tool has no matching post action."), TEXT("/post_action"), MakeShared<FJsonValueString>(PostAction), MakeShared<FJsonValueString>(TEXT("action on the active tool or its property sets")));
			OutError.SubjectId = Tool ? Tool->GetClass()->GetPathName() : FString();
			CancelStartedTool();
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
		if (AcceptedOutputActor && !SelectionBeforeAccept.Contains(AcceptedOutputActor)
			&& !ApplyCreatedMetricMaterial(AcceptedOutputActor, EffectiveParams, OutError)) return false;

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
				OutError = ModelingFact(TEXT("accepted_static_mesh_output_resolution_failed"), TEXT("The accepted tool output has no resolvable StaticMesh asset."), TEXT("/accepted_output_class"), MakeShared<FJsonValueString>(AcceptedOutputActor ? AcceptedOutputActor->GetClass()->GetPathName() : TEXT("no output actor")), MakeShared<FJsonValueString>(TEXT("actor with a StaticMesh asset")), AcceptedOutputActor ? AcceptedOutputActor->GetPathName() : FString());
				return false;
			}
			OutData->SetObjectField(TEXT("created_actor"), MakeActorSummaryJson(AcceptedOutputActor));
			OutData->SetStringField(TEXT("created_asset_path"), CreatedAssetPath);
		}
		else if (OutputTypePolicy == EModelingOutputTypePolicy::DynamicMeshActorRequired)
		{
			if (!IsAcceptedDynamicMeshOutput(AcceptedOutputActor))
			{
				OutError = ModelingFact(TEXT("accepted_dynamic_mesh_output_resolution_failed"), TEXT("The accepted tool output does not satisfy the required DynamicMesh actor policy."), TEXT("/accepted_output_class"), MakeShared<FJsonValueString>(AcceptedOutputActor ? AcceptedOutputActor->GetClass()->GetPathName() : TEXT("no output actor")), MakeShared<FJsonValueString>(TEXT("DynamicMesh actor output")), AcceptedOutputActor ? AcceptedOutputActor->GetPathName() : FString());
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

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::ObserveTool(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
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

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::SetToolProperty(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
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
		OutError = ModelingCondition(TEXT("no_active_tool"), TEXT("No modeling tool is active on the left tool side."), TEXT("/active_tool_present"), false, true);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ToolProperties = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("tool_properties"), ToolProperties) || !ToolProperties || ToolProperties->Num() != 1)
	{
		OutError = ModelingInputFailure(TEXT("exactly_one_tool_property_required"), TEXT("Setting one tool property requires tool_properties to contain exactly one entry."), Ctx.Params, TEXT("tool_properties"), MakeShared<FJsonValueString>(TEXT("array containing exactly one property edit")));
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

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::InvokeToolAction(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
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
		OutError = ModelingCondition(TEXT("no_active_tool"), TEXT("No modeling tool is active on the left tool side."), TEXT("/active_tool_present"), false, true);
		return false;
	}

	FString ActionName;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("action_name"), ActionName) || ActionName.IsEmpty())
	{
		OutError = ModelingInputFailure(TEXT("action_name_required"), TEXT("Invoking a tool action requires a non-empty string action_name."), Ctx.Params, TEXT("action_name"), MakeShared<FJsonValueString>(TEXT("non-empty tool action name")));
		return false;
	}

	if (!UeAgentModelingOps::InvokeToolAction(Tool, ActionName))
	{
		OutError = ModelingInputFailure(TEXT("tool_action_not_found"), TEXT("The active tool and its property sets have no action matching action_name."), Ctx.Params, TEXT("action_name"), MakeShared<FJsonValueString>(TEXT("action exposed by the active tool or a property set")));
		OutError.SubjectId = Tool->GetClass()->GetPathName();
		return false;
	}

	FlushToolsContext(Context, 3);
	FillActiveToolData(Context, OutData);
	OutData->SetStringField(TEXT("invoked_action"), ActionName);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingInvokeAction req=%s tool=%s action=%s"), *Ctx.RequestId, *Context.ToolsContext->GetActiveToolName(), *ActionName);
	return true;
}

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::AcceptTool(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	TArray<AActor*> BeforeAccept;
	GetSelectedActors(BeforeAccept);
	if (!FinishTool(Context, EToolShutdownType::Accept, OutError))
	{
		return false;
	}

	TArray<AActor*> AfterAccept;
	GetSelectedActors(AfterAccept);
	AActor* Created = ResolveCreatedActorFromSelectionDelta(BeforeAccept, AfterAccept);
	if (Created && !BeforeAccept.Contains(Created) && !ApplyCreatedMetricMaterial(Created, Ctx.Params, OutError)) return false;
	FillActiveToolData(Context, OutData);
	OutData->SetBoolField(TEXT("accepted"), true);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingAcceptTool req=%s"), *Ctx.RequestId);
	return true;
}

bool UE::UAI::DomainAdapters::ModelingGeometry::FModelingSession::CancelTool(const FModelingSessionRequest& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
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
