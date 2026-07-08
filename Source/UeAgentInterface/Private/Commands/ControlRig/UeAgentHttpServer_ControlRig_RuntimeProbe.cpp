#include "UeAgentHttpServer_ControlRig.h"

#include "UeAgentHttpServer_ControlRigOps.h"

#include "Utilities/UeAgentJsonDiagnostics.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Rigs/RigHierarchy.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"
#include "UObject/Package.h"

static void ReadStringArrayParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, TArray<FString>& OutItems)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text) && !Text.TrimStartAndEnd().IsEmpty())
		{
			OutItems.Add(Text);
		}
	}
}

static TSharedPtr<FJsonObject> MakeRuntimeElementSample(URigHierarchy* Hierarchy, const FRigElementKey& Key)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Key.Name.ToString());
	Obj->SetStringField(TEXT("type"), UeAgentControlRigOps::ElementTypeToString(Key.Type));
	if (!Hierarchy || !Hierarchy->Contains(Key))
	{
		Obj->SetBoolField(TEXT("found"), false);
		return Obj;
	}
	Obj->SetBoolField(TEXT("found"), true);
	Obj->SetObjectField(TEXT("global_transform"), UeAgentControlRigOps::MakeTransformJson(Hierarchy->GetGlobalTransform(Key, false)));
	Obj->SetObjectField(TEXT("local_transform"), UeAgentControlRigOps::MakeTransformJson(Hierarchy->GetLocalTransform(Key, false)));
	Obj->SetObjectField(TEXT("initial_global_transform"), UeAgentControlRigOps::MakeTransformJson(Hierarchy->GetGlobalTransform(Key, true)));
	return Obj;
}

bool FUeAgentHttpServer::CmdControlRigRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}

	UControlRigBlueprint* Blueprint = UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath);
	if (!Blueprint)
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}

	FCompilerResultsLog CompilerLog;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
	if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("asset_path"), UeAgentControlRigOps::NormalizeAssetPath(AssetPath));
		OutData->SetObjectField(TEXT("compile_report"), UeAgentControlRigOps::MakeCompileReportJson(Blueprint, &CompilerLog));
		OutError = TEXT("control_rig_compile_failed");
		return false;
	}

	UClass* RigClass = Blueprint->GetControlRigClass();
	if (!RigClass || !RigClass->IsChildOf(UControlRig::StaticClass()))
	{
		OutError = TEXT("control_rig_class_missing");
		return false;
	}

	UControlRig* RuntimeRig = NewObject<UControlRig>(GetTransientPackage(), RigClass, NAME_None, RF_Transient);
	if (!RuntimeRig)
	{
		OutError = TEXT("control_rig_runtime_instance_failed");
		return false;
	}

	RuntimeRig->Initialize(true);
	RuntimeRig->RequestInit();

	TArray<TSharedPtr<FJsonValue>> ProbeIssues;
	TArray<FString> VariableNamesToSample;
	auto ApplyVariableInput = [&](const FString& VariableName, const TSharedPtr<FJsonValue>& Value, const FString& JsonPath)
	{
		if (VariableName.TrimStartAndEnd().IsEmpty())
		{
			UeAgentJsonDiagnostics::AddIssue(ProbeIssues, TEXT("error"), TEXT("control_rig_probe_variable_name_required"), JsonPath, TEXT("Runtime probe variable input requires a non-empty variable name."));
			return;
		}
		FString ValueText;
		if (!UeAgentControlRigOps::ReadJsonValueAsDefaultText(Value, ValueText))
		{
			UeAgentJsonDiagnostics::AddIssue(ProbeIssues, TEXT("error"), TEXT("control_rig_probe_variable_value_invalid"), JsonPath, TEXT("Runtime probe variable value must be a string, number, bool, vector object, or color object."));
			return;
		}
		FProperty* Property = FindFProperty<FProperty>(RigClass, FName(*VariableName));
		if (!Property)
		{
			UeAgentJsonDiagnostics::AddIssue(ProbeIssues, TEXT("error"), TEXT("control_rig_probe_variable_not_found"), JsonPath, FString::Printf(TEXT("Variable '%s' does not exist on the generated Control Rig class."), *VariableName));
			return;
		}
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RuntimeRig);
		if (!ValuePtr || Property->ImportText_Direct(*ValueText, ValuePtr, RuntimeRig, PPF_None) == nullptr)
		{
			UeAgentJsonDiagnostics::AddIssue(ProbeIssues, TEXT("error"), TEXT("control_rig_probe_variable_import_failed"), JsonPath, FString::Printf(TEXT("Failed to import runtime value for variable '%s'."), *VariableName));
			return;
		}
		VariableNamesToSample.AddUnique(VariableName);
	};

	const TSharedPtr<FJsonObject>* VariableInputsObj = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("variables"), VariableInputsObj) && VariableInputsObj && VariableInputsObj->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*VariableInputsObj)->Values)
		{
			ApplyVariableInput(Pair.Key, Pair.Value, FString::Printf(TEXT("variables.%s"), *Pair.Key));
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* VariableInputsArray = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("variable_inputs"), VariableInputsArray) && VariableInputsArray)
	{
		for (int32 VariableInputIndex = 0; VariableInputIndex < VariableInputsArray->Num(); ++VariableInputIndex)
		{
			TSharedPtr<FJsonObject> VariableInputObj;
			const FString VariableInputPath = FString::Printf(TEXT("variable_inputs[%d]"), VariableInputIndex);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*VariableInputsArray)[VariableInputIndex], VariableInputPath, VariableInputObj, ProbeIssues))
			{
				continue;
			}
			FString Name;
			UeAgentJsonDiagnostics::ReadStringField(VariableInputObj, TEXT("name"), VariableInputPath + TEXT(".name"), Name, ProbeIssues, true);
			const TSharedPtr<FJsonValue>* Value = UeAgentJsonDiagnostics::FindFieldValue(VariableInputObj, TEXT("value"));
			if (!Value)
			{
				Value = UeAgentJsonDiagnostics::FindFieldValue(VariableInputObj, TEXT("default_value"));
			}
			if (!Value)
			{
				UeAgentJsonDiagnostics::AddIssue(ProbeIssues, TEXT("error"), TEXT("control_rig_probe_variable_value_required"), VariableInputPath, TEXT("variable_inputs item requires value or default_value."));
				continue;
			}
			ApplyVariableInput(Name, *Value, VariableInputPath + TEXT(".value"));
		}
	}

	TArray<TSharedPtr<FJsonValue>> SupportedEventValues;
	for (const FName& SupportedEvent : RuntimeRig->GetSupportedEvents())
	{
		SupportedEventValues.Add(MakeShared<FJsonValueString>(SupportedEvent.ToString()));
	}

	bool bExecuteConstruction = true;
	JsonTryGetBool(Ctx.Params, TEXT("execute_construction"), bExecuteConstruction);
	bool bConstructionOk = true;
	bool bConstructionSkipped = false;
	if (bExecuteConstruction)
	{
		if (RuntimeRig->SupportsEvent(FRigUnit_PrepareForExecution::EventName))
		{
			bConstructionOk = RuntimeRig->Execute(FRigUnit_PrepareForExecution::EventName);
		}
		else
		{
			bConstructionSkipped = true;
		}
	}

	FString EventName = TEXT("Forwards Solve");
	JsonTryGetString(Ctx.Params, TEXT("event_name"), EventName);
	int32 Frames = 1;
	double FramesNum = static_cast<double>(Frames);
	if (JsonTryGetNumber(Ctx.Params, TEXT("frames"), FramesNum))
	{
		Frames = FMath::Clamp(FMath::RoundToInt(FramesNum), 0, 1000);
	}

	bool bExecutionOk = true;
	bool bExecutionSkipped = false;
	const FName EventFName(*EventName);
	if (Frames > 0 && RuntimeRig->SupportsEvent(EventFName))
	{
		for (int32 FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
		{
			bExecutionOk = RuntimeRig->Execute(EventFName) && bExecutionOk;
		}
	}
	else if (Frames > 0)
	{
		bExecutionSkipped = true;
	}

	URigHierarchy* Hierarchy = RuntimeRig->GetHierarchy();
	TArray<FString> SampleBones;
	TArray<FString> SampleControls;
	TArray<FString> SampleVariables;
	ReadStringArrayParam(Ctx.Params, TEXT("sample_bones"), SampleBones);
	ReadStringArrayParam(Ctx.Params, TEXT("sample_controls"), SampleControls);
	ReadStringArrayParam(Ctx.Params, TEXT("sample_variables"), SampleVariables);

	int32 MaxSamples = 64;
	double MaxSamplesNum = static_cast<double>(MaxSamples);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_samples"), MaxSamplesNum))
	{
		MaxSamples = FMath::Clamp(FMath::RoundToInt(MaxSamplesNum), 1, 512);
	}

	if (Hierarchy)
	{
		if (SampleBones.Num() == 0)
		{
			for (const FRigElementKey& Key : Hierarchy->GetBoneKeys(true))
			{
				if (SampleBones.Num() >= MaxSamples)
				{
					break;
				}
				SampleBones.Add(Key.Name.ToString());
			}
		}
		if (SampleControls.Num() == 0)
		{
			for (const FRigElementKey& Key : Hierarchy->GetControlKeys(true))
			{
				if (SampleControls.Num() >= MaxSamples)
				{
					break;
				}
				SampleControls.Add(Key.Name.ToString());
			}
		}
	}
	if (SampleVariables.Num() == 0)
	{
		SampleVariables = VariableNamesToSample;
		if (SampleVariables.Num() == 0)
		{
			for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
			{
				if (SampleVariables.Num() >= MaxSamples)
				{
					break;
				}
				SampleVariables.Add(VarDesc.VarName.ToString());
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> BoneValues;
	for (const FString& BoneName : SampleBones)
	{
		BoneValues.Add(MakeShared<FJsonValueObject>(MakeRuntimeElementSample(Hierarchy, FRigElementKey(FName(*BoneName), ERigElementType::Bone))));
	}
	TArray<TSharedPtr<FJsonValue>> ControlValues;
	for (const FString& ControlName : SampleControls)
	{
		ControlValues.Add(MakeShared<FJsonValueObject>(MakeRuntimeElementSample(Hierarchy, FRigElementKey(FName(*ControlName), ERigElementType::Control))));
	}
	TArray<TSharedPtr<FJsonValue>> VariableValues;
	for (const FString& VariableName : SampleVariables)
	{
		TSharedPtr<FJsonObject> VariableObj = MakeShared<FJsonObject>();
		VariableObj->SetStringField(TEXT("name"), VariableName);
		FProperty* Property = FindFProperty<FProperty>(RigClass, FName(*VariableName));
		if (!Property)
		{
			VariableObj->SetBoolField(TEXT("found"), false);
		}
		else
		{
			VariableObj->SetBoolField(TEXT("found"), true);
			FString ExtendedType;
			VariableObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType(&ExtendedType) + ExtendedType);
			FString ValueText;
			if (void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RuntimeRig))
			{
				Property->ExportTextItem_Direct(ValueText, ValuePtr, ValuePtr, RuntimeRig, PPF_None);
			}
			VariableObj->SetStringField(TEXT("value_text"), ValueText);
		}
		VariableValues.Add(MakeShared<FJsonValueObject>(VariableObj));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentControlRigOps::NormalizeAssetPath(AssetPath));
	OutData->SetStringField(TEXT("probe_mode"), TEXT("transient_control_rig_instance"));
	OutData->SetArrayField(TEXT("supported_events"), SupportedEventValues);
	OutData->SetBoolField(TEXT("construction_executed"), bExecuteConstruction);
	OutData->SetBoolField(TEXT("construction_skipped"), bConstructionSkipped);
	OutData->SetBoolField(TEXT("construction_ok"), bConstructionOk);
	OutData->SetStringField(TEXT("event_name"), EventName);
	OutData->SetNumberField(TEXT("frames"), Frames);
	OutData->SetBoolField(TEXT("execution_skipped"), bExecutionSkipped);
	OutData->SetBoolField(TEXT("execution_ok"), bExecutionOk);
	OutData->SetObjectField(TEXT("compile_report"), UeAgentControlRigOps::MakeCompileReportJson(Blueprint, &CompilerLog));
	OutData->SetArrayField(TEXT("issues"), ProbeIssues);
	OutData->SetNumberField(TEXT("error_count"), UeAgentControlRigOps::CountIssuesBySeverity(ProbeIssues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentControlRigOps::CountIssuesBySeverity(ProbeIssues, TEXT("warning")));
	OutData->SetArrayField(TEXT("bones"), BoneValues);
	OutData->SetArrayField(TEXT("controls"), ControlValues);
	OutData->SetArrayField(TEXT("variables"), VariableValues);
	return bConstructionOk && bExecutionOk && !UeAgentControlRigOps::HasBlockingIssue(ProbeIssues);
}
