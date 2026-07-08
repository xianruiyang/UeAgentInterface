// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Niagara.h"
#include "UeAgentHttpServer_NiagaraOps.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Module/UeAgentInterfaceLogger.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetCompilingManager.h"
#include "Components/SceneCaptureComponent2D.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "EditorSupportDelegates.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorModule.h"
#include "NiagaraGraph.h"
#include "NiagaraMessageStore.h"
#include "NiagaraMessages.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptExecutionContext.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraTypes.h"
#include "NiagaraWorldManager.h"
#include "NiagaraParameterMapHistory.h"
#include "ObjectTools.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Slate/WidgetRenderer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/GarbageCollection.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraEmitterViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackObject.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"
#include "UObject/UnrealType.h"
#include "UeAgentInterfaceSettings.h"
#include "Materials/MaterialInterface.h"
#include "ShaderCompiler.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

#include "EdGraphSchema_Niagara.h"

namespace UeAgentNiagaraOps
{
#include "UeAgentHttpServer_Niagara_UtilityCore.inl"
	static TSharedPtr<FJsonObject> CollectNiagaraComponentStats(UNiagaraSystem* NiagaraSystem, UNiagaraComponent* PreviewComponent, FString& OutError)
	{
		OutError.Reset();
		if (!NiagaraSystem)
		{
			OutError = TEXT("preview_stats_only_supported_for_system_assets");
			return nullptr;
		}
		if (!PreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_not_found");
			return nullptr;
		}

		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetBoolField(TEXT("system_ready_to_run"), NiagaraSystem->IsReadyToRun());
		Stats->SetBoolField(TEXT("system_has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
		Stats->SetObjectField(TEXT("system_spawn_script"), BuildNiagaraScriptRuntimeStats(NiagaraSystem->GetSystemSpawnScript(), ENiagaraSimTarget::CPUSim));
		Stats->SetObjectField(TEXT("system_update_script"), BuildNiagaraScriptRuntimeStats(NiagaraSystem->GetSystemUpdateScript(), ENiagaraSimTarget::CPUSim));
		Stats->SetBoolField(TEXT("component_registered"), PreviewComponent->IsRegistered());
		Stats->SetBoolField(TEXT("component_active"), PreviewComponent->IsActive());
		Stats->SetBoolField(TEXT("component_paused"), PreviewComponent->IsPaused());
		Stats->SetStringField(TEXT("component_name"), PreviewComponent->GetName());
		Stats->SetStringField(TEXT("component_path"), PreviewComponent->GetPathName());
		Stats->SetStringField(TEXT("component_world"), PreviewComponent->GetWorld() ? PreviewComponent->GetWorld()->GetPathName() : FString());
		Stats->SetBoolField(TEXT("component_has_attach_parent"), PreviewComponent->GetAttachParent() != nullptr);
		Stats->SetStringField(TEXT("component_requested_execution_state"), NiagaraEnumToString(PreviewComponent->GetRequestedExecutionState()));
		Stats->SetStringField(TEXT("component_execution_state"), NiagaraEnumToString(PreviewComponent->GetExecutionState()));
		Stats->SetBoolField(TEXT("component_complete"), PreviewComponent->IsComplete());
		Stats->SetStringField(TEXT("age_update_mode"), NiagaraEnumToString(PreviewComponent->GetAgeUpdateMode()));
		Stats->SetNumberField(TEXT("desired_age"), PreviewComponent->GetDesiredAge());

PRAGMA_DISABLE_DEPRECATION_WARNINGS
		FNiagaraSystemInstance* SystemInstance = PreviewComponent->GetSystemInstance();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		if (!SystemInstance)
		{
			Stats->SetBoolField(TEXT("has_system_instance"), false);
			return Stats;
		}

		Stats->SetBoolField(TEXT("has_system_instance"), true);
		Stats->SetStringField(TEXT("system_instance_state"), NiagaraEnumToString(SystemInstance->SystemInstanceState));
		Stats->SetBoolField(TEXT("system_instance_pending_spawn"), SystemInstance->IsPendingSpawn());
		Stats->SetBoolField(TEXT("system_instance_disabled"), SystemInstance->IsDisabled());
		Stats->SetStringField(TEXT("system_attach_component"), SystemInstance->GetAttachComponent() ? SystemInstance->GetAttachComponent()->GetPathName() : FString());
		Stats->SetNumberField(TEXT("system_age"), SystemInstance->GetAge());
		Stats->SetNumberField(TEXT("system_tick_count"), SystemInstance->GetTickCount());
		Stats->SetBoolField(TEXT("system_instance_ready_to_run"), SystemInstance->IsReadyToRun());
		Stats->SetBoolField(TEXT("system_complete"), SystemInstance->IsComplete());
		Stats->SetStringField(TEXT("system_execution_state"), NiagaraEnumToString(SystemInstance->GetActualExecutionState()));
		Stats->SetStringField(TEXT("system_requested_execution_state"), NiagaraEnumToString(SystemInstance->GetRequestedExecutionState()));

		int32 TotalParticles = 0;
		int32 TotalSpawnedParticles = 0;
		TArray<TSharedPtr<FJsonValue>> EmitterStats;
		for (const FNiagaraEmitterInstanceRef& EmitterRef : SystemInstance->GetEmitters())
		{
			FNiagaraEmitterInstance& EmitterInstance = EmitterRef.Get();
			const int32 NumParticles = EmitterInstance.GetNumParticles();
			const int32 SpawnedParticles = EmitterInstance.GetTotalSpawnedParticles();
			TotalParticles += NumParticles;
			TotalSpawnedParticles += SpawnedParticles;

			TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
			EmitterObj->SetStringField(TEXT("name"), EmitterInstance.GetEmitterHandle().GetName().ToString());
			EmitterObj->SetStringField(TEXT("execution_state"), NiagaraEnumToString(EmitterInstance.GetExecutionState()));
			EmitterObj->SetBoolField(TEXT("active"), EmitterInstance.IsActive());
			EmitterObj->SetBoolField(TEXT("disabled"), EmitterInstance.IsDisabled());
			EmitterObj->SetBoolField(TEXT("inactive"), EmitterInstance.IsInactive());
			EmitterObj->SetBoolField(TEXT("complete"), EmitterInstance.IsComplete());
			EmitterObj->SetBoolField(TEXT("should_tick"), EmitterInstance.ShouldTick());
			EmitterObj->SetStringField(TEXT("sim_target"), NiagaraEnumToString(EmitterInstance.GetSimTarget()));
			EmitterObj->SetBoolField(TEXT("is_stateful"), EmitterInstance.AsStateful() != nullptr);
			EmitterObj->SetBoolField(TEXT("is_stateless"), EmitterInstance.AsStateless() != nullptr);
#if WITH_EDITORONLY_DATA
			EmitterObj->SetBoolField(TEXT("disabled_from_isolation"), EmitterInstance.IsDisabledFromIsolation());
#else
			EmitterObj->SetBoolField(TEXT("disabled_from_isolation"), false);
#endif
			EmitterObj->SetNumberField(TEXT("particle_count"), NumParticles);
			EmitterObj->SetNumberField(TEXT("total_spawned_particles"), SpawnedParticles);
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			EmitterObj->SetBoolField(TEXT("ready_to_run"), EmitterInstance.IsReadyToRun());
			EmitterObj->SetBoolField(TEXT("has_ticked"), EmitterInstance.HasTicked());
			if (EmitterInstance.AsStateful() != nullptr)
			{
				int32 SpawnInfoTotalCount = 0;
				TArray<TSharedPtr<FJsonValue>> SpawnInfoJson = BuildSpawnInfoArray(EmitterInstance.GetSpawnInfo(), SpawnInfoTotalCount);
				EmitterObj->SetNumberField(TEXT("spawn_info_count"), SpawnInfoJson.Num());
				EmitterObj->SetNumberField(TEXT("spawn_info_total_count"), SpawnInfoTotalCount);
				EmitterObj->SetArrayField(TEXT("spawn_infos"), SpawnInfoJson);
				EmitterObj->SetObjectField(TEXT("spawn_execution_context"), BuildScriptExecutionContextStats(EmitterInstance.GetSpawnExecutionContext(), EmitterInstance.GetSimTarget()));
				EmitterObj->SetObjectField(TEXT("update_execution_context"), BuildScriptExecutionContextStats(EmitterInstance.GetUpdateExecutionContext(), EmitterInstance.GetSimTarget()));
			}
PRAGMA_ENABLE_DEPRECATION_WARNINGS
			EmitterStats.Add(MakeShared<FJsonValueObject>(EmitterObj));
		}

		Stats->SetNumberField(TEXT("emitter_instance_count"), EmitterStats.Num());
		Stats->SetNumberField(TEXT("total_particle_count"), TotalParticles);
		Stats->SetNumberField(TEXT("total_spawned_particles"), TotalSpawnedParticles);
		Stats->SetArrayField(TEXT("emitters"), EmitterStats);
		return Stats;
	}

	static TSharedPtr<FJsonObject> CollectNiagaraPreviewStats(UObject* Asset, FString& OutError)
	{
		OutError.Reset();

		UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
		if (!NiagaraSystem)
		{
			OutError = TEXT("preview_stats_only_supported_for_system_assets");
			return nullptr;
		}

		FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
		if (!NiagaraEditorModule)
		{
			OutError = TEXT("niagara_editor_module_not_loaded");
			return nullptr;
		}

		const TSharedPtr<FNiagaraSystemViewModel> SystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
		if (!SystemViewModel.IsValid())
		{
			OutError = TEXT("niagara_system_view_model_not_found");
			return nullptr;
		}

		UNiagaraComponent* PreviewComponent = SystemViewModel->GetPreviewComponent();
		if (!PreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_not_found");
			return nullptr;
		}

		return CollectNiagaraComponentStats(NiagaraSystem, PreviewComponent, OutError);
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("asset_not_found");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.IsEmpty())
		{
			return nullptr;
		}

		const FString Normalized = Segment.TrimStartAndEnd();
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}

		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			const FString PropertyNameWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}
		return nullptr;
	}

	static bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				return false;
			}
			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}
		return false;
	}

	static bool ResolveStructPropertyPath(UStruct* RootStruct, void* RootContainer, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!RootStruct || !RootContainer || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = RootStruct;
		void* CurrentContainer = RootContainer;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				return false;
			}

			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}

		return false;
	}

	static bool SetStructPropertyText(UStruct* RootStruct, void* RootContainer, UObject* OwnerForImport, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText = nullptr, FString* OutCppType = nullptr, FString* OutImportError = nullptr)
	{
		if (OutImportError)
		{
			OutImportError->Reset();
		}

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolveStructPropertyPath(RootStruct, RootContainer, PropertyPath, Property, ValuePtr))
		{
			if (OutImportError)
			{
				*OutImportError = TEXT("property_not_found");
			}
			return false;
		}

		if (OutCppType)
		{
			*OutCppType = Property ? Property->GetCPPType() : TEXT("");
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FGuid>::Get())
			{
				FString GuidText = ValueText.TrimStartAndEnd();
				if (GuidText.StartsWith(TEXT("\"")) && GuidText.EndsWith(TEXT("\"")) && GuidText.Len() >= 2)
				{
					GuidText = GuidText.Mid(1, GuidText.Len() - 2);
				}

				FGuid ParsedGuid;
				if (FGuid::Parse(GuidText, ParsedGuid))
				{
					*reinterpret_cast<FGuid*>(ValuePtr) = ParsedGuid;
					if (OutAppliedValueText)
					{
						Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, OwnerForImport, PPF_None);
					}
					return true;
				}
			}
		}

		if (!Property || Property->ImportText_Direct(*ValueText, ValuePtr, OwnerForImport, PPF_None, GLog) == nullptr)
		{
			if (OutImportError)
			{
				*OutImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyPath, *ValueText);
			}
			return false;
		}
		if (OutAppliedValueText)
		{
			Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, OwnerForImport, PPF_None);
		}
		return true;
	}

	static bool GetStructPropertyText(UStruct* RootStruct, void* RootContainer, UObject* OwnerForExport, const FString& PropertyPath, FString& OutValueText, FString& OutCppType)
	{
		OutValueText.Reset();
		OutCppType.Reset();

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolveStructPropertyPath(RootStruct, RootContainer, PropertyPath, Property, ValuePtr))
		{
			return false;
		}

		if (!Property)
		{
			return false;
		}

		OutCppType = Property->GetCPPType();
		Property->ExportTextItem_Direct(OutValueText, ValuePtr, nullptr, OwnerForExport, PPF_None);
		return true;
	}

	static FString NormalizeEventHandlerPropertyName(const FString& InPropertyName)
	{
		const FString Property = InPropertyName.TrimStartAndEnd();
		if (Property.IsEmpty())
		{
			return Property;
		}

		if (Property.Equals(TEXT("source_event_name"), ESearchCase::IgnoreCase)) return TEXT("SourceEventName");
		if (Property.Equals(TEXT("source_emitter_id"), ESearchCase::IgnoreCase)) return TEXT("SourceEmitterID");
		if (Property.Equals(TEXT("execution_mode"), ESearchCase::IgnoreCase)) return TEXT("ExecutionMode");
		if (Property.Equals(TEXT("spawn_number"), ESearchCase::IgnoreCase)) return TEXT("SpawnNumber");
		if (Property.Equals(TEXT("min_spawn_number"), ESearchCase::IgnoreCase)) return TEXT("MinSpawnNumber");
		if (Property.Equals(TEXT("random_spawn_number"), ESearchCase::IgnoreCase)) return TEXT("bRandomSpawnNumber");
		if (Property.Equals(TEXT("max_events_per_frame"), ESearchCase::IgnoreCase)) return TEXT("MaxEventsPerFrame");
		if (Property.Equals(TEXT("update_attribute_initial_values"), ESearchCase::IgnoreCase)) return TEXT("bUpdateAttributeInitialValues");
		return Property;
	}

	struct FEmitterPropertySideEffectResult
	{
		bool bHandled = false;
		bool bUpdatedSpawnScriptUsage = false;
		bool bMarkedGraphSourceUnsynchronized = false;
		ENiagaraScriptUsage OldSpawnScriptUsage = ENiagaraScriptUsage::ParticleSpawnScript;
		ENiagaraScriptUsage NewSpawnScriptUsage = ENiagaraScriptUsage::ParticleSpawnScript;
	};

	static FEmitterPropertySideEffectResult ApplyEmitterPropertySideEffects(FVersionedNiagaraEmitterData* EmitterData, const FString& PropertyName)
	{
		FEmitterPropertySideEffectResult Result;
		if (!EmitterData)
		{
			return Result;
		}

		if (PropertyName.Equals(TEXT("InterpolatedSpawnMode"), ESearchCase::IgnoreCase))
		{
			Result.bHandled = true;
			const ENiagaraScriptUsage DesiredUsage = EmitterData->UsesInterpolatedSpawning()
				? ENiagaraScriptUsage::ParticleSpawnScriptInterpolated
				: ENiagaraScriptUsage::ParticleSpawnScript;

			if (UNiagaraScript* SpawnScript = EmitterData->SpawnScriptProps.Script)
			{
				Result.OldSpawnScriptUsage = SpawnScript->GetUsage();
				Result.NewSpawnScriptUsage = DesiredUsage;
				if (Result.OldSpawnScriptUsage != DesiredUsage)
				{
					SpawnScript->Modify();
					SpawnScript->SetUsage(DesiredUsage);
					Result.bUpdatedSpawnScriptUsage = true;
				}
			}

			if (EmitterData->GraphSource)
			{
				EmitterData->GraphSource->MarkNotSynchronized(TEXT("Emitter interpolated spawn changed"));
				Result.bMarkedGraphSourceUnsynchronized = true;
			}
		}

		return Result;
	}

	static bool SetObjectPropertyText(UObject* Target, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText = nullptr, FString* OutCppType = nullptr, FString* OutImportError = nullptr)
	{
		if (OutImportError)
		{
			OutImportError->Reset();
		}

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr))
		{
			if (OutImportError)
			{
				*OutImportError = TEXT("property_not_found");
			}
			return false;
		}

		Target->Modify();
		if (OutCppType)
		{
			*OutCppType = Property ? Property->GetCPPType() : TEXT("");
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FGuid>::Get())
			{
				FString GuidText = ValueText.TrimStartAndEnd();
				if (GuidText.StartsWith(TEXT("\"")) && GuidText.EndsWith(TEXT("\"")) && GuidText.Len() >= 2)
				{
					GuidText = GuidText.Mid(1, GuidText.Len() - 2);
				}

				FGuid ParsedGuid;
				if (FGuid::Parse(GuidText, ParsedGuid))
				{
					*reinterpret_cast<FGuid*>(ValuePtr) = ParsedGuid;
					if (OutAppliedValueText)
					{
						Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, Target, PPF_None);
					}
					return true;
				}
			}
		}
		if (!Property || Property->ImportText_Direct(*ValueText, ValuePtr, Target, PPF_None, GLog) == nullptr)
		{
			if (OutImportError)
			{
				*OutImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyPath, *ValueText);
			}
			return false;
		}
		if (OutAppliedValueText)
		{
			Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, Target, PPF_None);
		}
		return true;
	}

	static bool GetObjectPropertyText(UObject* Target, const FString& PropertyPath, FString& OutValueText)
	{
		OutValueText.Reset();

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr))
		{
			return false;
		}

		Property->ExportTextItem_Direct(OutValueText, ValuePtr, nullptr, Target, PPF_None);
		return true;
	}

	static TSharedPtr<FJsonObject> BuildPropertyValueJson(UObject& Target, FProperty& Property, void* ValuePtr)
	{
		TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
		PropertyObj->SetStringField(TEXT("property_name"), Property.GetName());
		PropertyObj->SetStringField(TEXT("authored_name"), Property.GetAuthoredName());
		PropertyObj->SetStringField(TEXT("cpp_type"), Property.GetCPPType());

		FString ValueText;
		Property.ExportTextItem_Direct(ValueText, ValuePtr, nullptr, &Target, PPF_None);
		PropertyObj->SetStringField(TEXT("value_text"), ValueText);
		UeAgentAuthoringJson::ApplyPropertyMetadata(
			PropertyObj,
			Property.GetName(),
			Property.GetDisplayNameText().ToString(),
			Property.GetMetaData(TEXT("Category")),
			TEXT("text"));
		TSharedPtr<FJsonObject> CurveJson;
		if (UeAgentCurveJson::TryBuildPropertyCurveJson(&Property, ValuePtr, CurveJson) && CurveJson.IsValid())
		{
			PropertyObj->SetObjectField(TEXT("value_json"), CurveJson);
			PropertyObj->SetObjectField(TEXT("curve_json"), CurveJson);
			PropertyObj->SetStringField(TEXT("value_json_schema"), UeAgentCurveJson::SchemaName);
		}
		return PropertyObj;
	}

	static FString StripUserNamespace(const FString& InName)
	{
		if (InName.StartsWith(TEXT("User."), ESearchCase::IgnoreCase))
		{
			return InName.Mid(5);
		}
		return InName;
	}

	static FString GetVariableNamespace(const FString& InName)
	{
		int32 DotIndex = INDEX_NONE;
		if (InName.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			return InName.Left(DotIndex);
		}
		return FString();
	}

	static FString GetVariableLeafName(const FString& InName)
	{
		int32 DotIndex = INDEX_NONE;
		if (InName.FindLastChar(TEXT('.'), DotIndex) && DotIndex >= 0 && DotIndex + 1 < InName.Len())
		{
			return InName.Mid(DotIndex + 1);
		}
		return InName;
	}

	static bool ParseBoolText(const FString& InText, bool& OutValue)
	{
		const FString Normalized = InText.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("1") || Normalized == TEXT("true") || Normalized == TEXT("yes") || Normalized == TEXT("on"))
		{
			OutValue = true;
			return true;
		}
		if (Normalized == TEXT("0") || Normalized == TEXT("false") || Normalized == TEXT("no") || Normalized == TEXT("off"))
		{
			OutValue = false;
			return true;
		}
		return false;
	}

	static bool ParseFloatComponents(const FString& InText, const int32 ExpectedNum, TArray<float>& OutValues)
	{
		OutValues.Reset();
		if (ExpectedNum <= 0)
		{
			return false;
		}

		FString Clean = InText;
		Clean.ReplaceInline(TEXT("("), TEXT(" "));
		Clean.ReplaceInline(TEXT(")"), TEXT(" "));
		Clean.ReplaceInline(TEXT("{"), TEXT(" "));
		Clean.ReplaceInline(TEXT("}"), TEXT(" "));
		Clean.ReplaceInline(TEXT("["), TEXT(" "));
		Clean.ReplaceInline(TEXT("]"), TEXT(" "));
		Clean.ReplaceInline(TEXT(","), TEXT(" "));
		Clean.ReplaceInline(TEXT(";"), TEXT(" "));

		const TCHAR* PrefixTokens[] =
		{
			TEXT("X="), TEXT("Y="), TEXT("Z="), TEXT("W="),
			TEXT("R="), TEXT("G="), TEXT("B="), TEXT("A="),
			TEXT("x="), TEXT("y="), TEXT("z="), TEXT("w="),
			TEXT("r="), TEXT("g="), TEXT("b="), TEXT("a=")
		};
		for (const TCHAR* Token : PrefixTokens)
		{
			Clean.ReplaceInline(Token, TEXT(" "));
		}

		TArray<FString> Parts;
		Clean.ParseIntoArrayWS(Parts);
		if (Parts.Num() != ExpectedNum)
		{
			return false;
		}

		OutValues.Reserve(ExpectedNum);
		for (const FString& Part : Parts)
		{
			float Value = 0.0f;
			if (!LexTryParseString(Value, *Part) || !FMath::IsFinite(Value))
			{
				return false;
			}
			OutValues.Add(Value);
		}
		return true;
	}

	static bool TryResolveNiagaraEnumValueFromText(const UEnum& EnumType, const FString& InValueText, int64& OutEnumValue)
	{
		FString ValueText = InValueText.TrimStartAndEnd();
		if (ValueText.IsEmpty())
		{
			return false;
		}

		if (ValueText.StartsWith(TEXT("\"")) && ValueText.EndsWith(TEXT("\"")) && ValueText.Len() > 1)
		{
			ValueText = ValueText.Mid(1, ValueText.Len() - 2);
		}

		int64 ParsedIntValue = 0;
		if (LexTryParseString(ParsedIntValue, *ValueText))
		{
			if (IsUserSettableNiagaraEnumValue(EnumType, ParsedIntValue))
			{
				OutEnumValue = ParsedIntValue;
				return true;
			}
		}

		int64 ParsedEnumValue = EnumType.GetValueByNameString(ValueText, EGetByNameFlags::CheckAuthoredName);
		if (ParsedEnumValue == INDEX_NONE)
		{
			ParsedEnumValue = EnumType.GetValueByNameString(ValueText, EGetByNameFlags::None);
		}
		if (ParsedEnumValue == INDEX_NONE && ValueText.StartsWith(TEXT("NewEnumerator"), ESearchCase::IgnoreCase))
		{
			const FString EnumIndexText = ValueText.Mid(13).TrimStartAndEnd();
			int32 EnumIndex = INDEX_NONE;
			if (LexTryParseString(EnumIndex, *EnumIndexText))
			{
				if (EnumIndex >= 0 && EnumIndex < EnumType.NumEnums())
				{
					ParsedEnumValue = EnumType.GetValueByIndex(EnumIndex);
				}
				else if (EnumType.IsValidEnumValue(EnumIndex))
				{
					ParsedEnumValue = EnumIndex;
				}
			}
		}
		if (ParsedEnumValue == INDEX_NONE)
		{
			int32 ScopePos = INDEX_NONE;
			if (ValueText.FindLastChar(TEXT(':'), ScopePos) && ScopePos + 1 < ValueText.Len())
			{
				const FString ShortName = ValueText.Mid(ScopePos + 1).TrimStartAndEnd();
				if (!ShortName.IsEmpty())
				{
					ParsedEnumValue = EnumType.GetValueByNameString(ShortName, EGetByNameFlags::CheckAuthoredName);
					if (ParsedEnumValue == INDEX_NONE)
					{
						ParsedEnumValue = EnumType.GetValueByNameString(ShortName, EGetByNameFlags::None);
					}
				}
			}
		}
		if (ParsedEnumValue == INDEX_NONE || !EnumType.IsValidEnumValue(ParsedEnumValue))
		{
			return false;
		}
		if (!IsUserSettableNiagaraEnumValue(EnumType, ParsedEnumValue))
		{
			return false;
		}

		OutEnumValue = ParsedEnumValue;
		return true;
	}

	static bool ResolveNiagaraTypeDefinition(const FString& InTypeText, FNiagaraTypeDefinition& OutTypeDefinition)
	{
		FString TypeText = InTypeText;
		TypeText.TrimStartAndEndInline();
		if (TypeText.IsEmpty())
		{
			return false;
		}

		FString TypeToken = TypeText;
		TypeToken.ReplaceInline(TEXT(" "), TEXT(""));
		TypeToken.ReplaceInline(TEXT("_"), TEXT(""));
		TypeToken.ReplaceInline(TEXT("-"), TEXT(""));
		TypeToken.ToLowerInline();

		if (TypeToken == TEXT("bool") || TypeToken == TEXT("布尔") || TypeToken.EndsWith(TEXT("niagarabool")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetBoolDef();
			return true;
		}
		if (TypeToken == TEXT("int") || TypeToken == TEXT("int32") || TypeToken == TEXT("整数") || TypeToken.EndsWith(TEXT("niagaraint")) || TypeToken.EndsWith(TEXT("niagaraint32")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetIntDef();
			return true;
		}
		if (TypeToken == TEXT("float") || TypeToken == TEXT("float32") || TypeToken == TEXT("浮点") || TypeToken.EndsWith(TEXT("niagarafloat")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetFloatDef();
			return true;
		}
		if (TypeToken == TEXT("double") || TypeToken == TEXT("float64") || TypeToken.EndsWith(TEXT("niagaradouble")))
		{
			OutTypeDefinition = FNiagaraTypeHelper::GetDoubleDef();
			return true;
		}
		if (TypeToken == TEXT("vec2") || TypeToken == TEXT("vector2") || TypeToken == TEXT("vector2d") || TypeToken == TEXT("fvector2f") || TypeToken.EndsWith(TEXT("niagaravector2")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetVec2Def();
			return true;
		}
		if (TypeToken == TEXT("vec3") || TypeToken == TEXT("vector") || TypeToken == TEXT("vector3") || TypeToken == TEXT("fvector3f") || TypeToken == TEXT("fvector") || TypeToken == TEXT("向量") || TypeToken == TEXT("矢量") || TypeToken.EndsWith(TEXT("niagaravector3")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetVec3Def();
			return true;
		}
		if (TypeToken == TEXT("position") || TypeToken == TEXT("niagaraposition"))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetPositionDef();
			return true;
		}
		if (TypeToken == TEXT("vec4") || TypeToken == TEXT("vector4") || TypeToken == TEXT("fvector4f") || TypeToken.EndsWith(TEXT("niagaravector4")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetVec4Def();
			return true;
		}
		if (TypeToken == TEXT("quat") || TypeToken == TEXT("quaternion") || TypeToken == TEXT("fquat") || TypeToken == TEXT("fquat4f"))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetQuatDef();
			return true;
		}
		if (TypeToken == TEXT("color") || TypeToken == TEXT("linearcolor") || TypeToken == TEXT("flinearcolor") || TypeToken == TEXT("颜色") || TypeToken.EndsWith(TEXT("niagaracolor")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetColorDef();
			return true;
		}

		if (UClass* TypeClass = ResolveClassByPath(TypeText))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(TypeClass);
			return true;
		}
		if (UScriptStruct* TypeStruct = FindObject<UScriptStruct>(nullptr, *TypeText))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(TypeStruct);
			return true;
		}
		if (UScriptStruct* LoadedTypeStruct = LoadObject<UScriptStruct>(nullptr, *TypeText))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(LoadedTypeStruct);
			return true;
		}
		if (UEnum* TypeEnum = FindObject<UEnum>(nullptr, *TypeText))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(TypeEnum);
			return true;
		}
		if (UEnum* LoadedTypeEnum = LoadObject<UEnum>(nullptr, *TypeText))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(LoadedTypeEnum);
			return true;
		}

		return false;
	}

	static bool BuildNiagaraValueBytesFromText(const FNiagaraTypeDefinition& TypeDefinition, const FString& ValueText, TArray<uint8>& OutBytes, FString& OutError)
	{
		OutBytes.Reset();
		if (TypeDefinition == FNiagaraTypeDefinition::GetBoolDef())
		{
			bool bValue = false;
			if (!ParseBoolText(ValueText, bValue))
			{
				OutError = TEXT("invalid_bool_value_text");
				return false;
			}
			FNiagaraBool NiagaraBool;
			NiagaraBool.SetValue(bValue);
			OutBytes.SetNumUninitialized(sizeof(FNiagaraBool));
			FMemory::Memcpy(OutBytes.GetData(), &NiagaraBool, sizeof(FNiagaraBool));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetIntDef())
		{
			int32 IntValue = 0;
			if (!LexTryParseString(IntValue, *ValueText))
			{
				OutError = TEXT("invalid_int_value_text");
				return false;
			}
			OutBytes.SetNumUninitialized(sizeof(int32));
			FMemory::Memcpy(OutBytes.GetData(), &IntValue, sizeof(int32));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetFloatDef())
		{
			float FloatValue = 0.0f;
			if (!LexTryParseString(FloatValue, *ValueText))
			{
				OutError = TEXT("invalid_float_value_text");
				return false;
			}
			OutBytes.SetNumUninitialized(sizeof(float));
			FMemory::Memcpy(OutBytes.GetData(), &FloatValue, sizeof(float));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeHelper::GetDoubleDef())
		{
			double DoubleValue = 0.0;
			if (!LexTryParseString(DoubleValue, *ValueText))
			{
				OutError = TEXT("invalid_double_value_text");
				return false;
			}
			OutBytes.SetNumUninitialized(sizeof(double));
			FMemory::Memcpy(OutBytes.GetData(), &DoubleValue, sizeof(double));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec2Def())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 2, Components))
			{
				OutError = TEXT("invalid_vector2_value_text");
				return false;
			}
			const FVector2f VecValue(Components[0], Components[1]);
			OutBytes.SetNumUninitialized(sizeof(FVector2f));
			FMemory::Memcpy(OutBytes.GetData(), &VecValue, sizeof(FVector2f));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec3Def())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 3, Components))
			{
				OutError = TEXT("invalid_vector3_value_text");
				return false;
			}
			const FVector3f VecValue(Components[0], Components[1], Components[2]);
			OutBytes.SetNumUninitialized(sizeof(FVector3f));
			FMemory::Memcpy(OutBytes.GetData(), &VecValue, sizeof(FVector3f));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetPositionDef())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 3, Components))
			{
				OutError = TEXT("invalid_position_value_text");
				return false;
			}
			const FNiagaraPosition PositionValue(Components[0], Components[1], Components[2]);
			OutBytes.SetNumUninitialized(sizeof(FNiagaraPosition));
			FMemory::Memcpy(OutBytes.GetData(), &PositionValue, sizeof(FNiagaraPosition));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec4Def())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 4, Components))
			{
				OutError = TEXT("invalid_vector4_value_text");
				return false;
			}
			const FVector4f VecValue(Components[0], Components[1], Components[2], Components[3]);
			OutBytes.SetNumUninitialized(sizeof(FVector4f));
			FMemory::Memcpy(OutBytes.GetData(), &VecValue, sizeof(FVector4f));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetQuatDef())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 4, Components))
			{
				OutError = TEXT("invalid_quat_value_text");
				return false;
			}
			const FQuat4f QuatValue(Components[0], Components[1], Components[2], Components[3]);
			OutBytes.SetNumUninitialized(sizeof(FQuat4f));
			FMemory::Memcpy(OutBytes.GetData(), &QuatValue, sizeof(FQuat4f));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetColorDef())
		{
			FLinearColor ColorValue;
			const FString Trimmed = ValueText.TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("#")))
			{
				ColorValue = FLinearColor(FColor::FromHex(Trimmed));
			}
			else
			{
				TArray<float> Components;
				if (ParseFloatComponents(Trimmed, 3, Components))
				{
					ColorValue = FLinearColor(Components[0], Components[1], Components[2], 1.0f);
				}
				else if (ParseFloatComponents(Trimmed, 4, Components))
				{
					ColorValue = FLinearColor(Components[0], Components[1], Components[2], Components[3]);
				}
				else if (!ColorValue.InitFromString(Trimmed))
				{
					OutError = TEXT("invalid_color_value_text");
					return false;
				}
			}

			OutBytes.SetNumUninitialized(sizeof(FLinearColor));
			FMemory::Memcpy(OutBytes.GetData(), &ColorValue, sizeof(FLinearColor));
			return true;
		}
		if (TypeDefinition.IsEnum())
		{
			UEnum* EnumType = TypeDefinition.GetEnum();
			if (!EnumType)
			{
				OutError = TEXT("enum_type_not_found");
				return false;
			}

			int64 EnumValue = 0;
			if (!TryResolveNiagaraEnumValueFromText(*EnumType, ValueText, EnumValue))
			{
				OutError = TEXT("invalid_enum_value_text");
				return false;
			}

			const int32 EnumIntValue = static_cast<int32>(EnumValue);
			OutBytes.SetNumUninitialized(sizeof(int32));
			FMemory::Memcpy(OutBytes.GetData(), &EnumIntValue, sizeof(int32));
			return true;
		}

		OutError = TEXT("unsupported_parameter_type_for_value_text");
		return false;
	}

	static FString FormatNiagaraFloatDefaultValue(const float Value)
	{
		return FString::SanitizeFloat(Value);
	}

	static FString FormatNiagaraVectorDefaultValue(const TArray<float>& Components, const TArray<const TCHAR*>& ComponentNames)
	{
		TArray<FString> Parts;
		const int32 ComponentCount = FMath::Min(Components.Num(), ComponentNames.Num());
		Parts.Reserve(ComponentCount);
		for (int32 Index = 0; Index < ComponentCount; ++Index)
		{
			Parts.Add(FString::Printf(
				TEXT("%s=%s"),
				ComponentNames[Index],
				*FormatNiagaraFloatDefaultValue(Components[Index])));
		}
		return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
	}

	static bool NormalizeNiagaraValueTextForPinDefault(const FNiagaraTypeDefinition& TypeDefinition, const FString& ValueText, FString& OutValueText, FString& OutError)
	{
		OutValueText = ValueText;
		OutError.Empty();

		if (TypeDefinition == FNiagaraTypeDefinition::GetBoolDef())
		{
			bool bValue = false;
			if (!ParseBoolText(ValueText, bValue))
			{
				OutError = TEXT("invalid_bool_value_text");
				return false;
			}
			OutValueText = bValue ? TEXT("true") : TEXT("false");
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetIntDef())
		{
			int32 IntValue = 0;
			if (!LexTryParseString(IntValue, *ValueText))
			{
				OutError = TEXT("invalid_int_value_text");
				return false;
			}
			OutValueText = LexToString(IntValue);
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetFloatDef())
		{
			float FloatValue = 0.0f;
			if (!LexTryParseString(FloatValue, *ValueText) || !FMath::IsFinite(FloatValue))
			{
				OutError = TEXT("invalid_float_value_text");
				return false;
			}
			OutValueText = FormatNiagaraFloatDefaultValue(FloatValue);
			return true;
		}
		if (TypeDefinition == FNiagaraTypeHelper::GetDoubleDef())
		{
			double DoubleValue = 0.0;
			if (!LexTryParseString(DoubleValue, *ValueText) || !FMath::IsFinite(DoubleValue))
			{
				OutError = TEXT("invalid_double_value_text");
				return false;
			}
			OutValueText = FString::SanitizeFloat(DoubleValue);
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec2Def())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 2, Components))
			{
				OutError = TEXT("invalid_vector2_value_text");
				return false;
			}
			OutValueText = FormatNiagaraVectorDefaultValue(Components, { TEXT("X"), TEXT("Y") });
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec3Def() || TypeDefinition == FNiagaraTypeDefinition::GetPositionDef())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 3, Components))
			{
				OutError = TypeDefinition == FNiagaraTypeDefinition::GetPositionDef() ? TEXT("invalid_position_value_text") : TEXT("invalid_vector3_value_text");
				return false;
			}
			OutValueText = FormatNiagaraVectorDefaultValue(Components, { TEXT("X"), TEXT("Y"), TEXT("Z") });
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec4Def())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 4, Components))
			{
				OutError = TEXT("invalid_vector4_value_text");
				return false;
			}
			OutValueText = FormatNiagaraVectorDefaultValue(Components, { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") });
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetQuatDef())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 4, Components))
			{
				OutError = TEXT("invalid_quat_value_text");
				return false;
			}
			OutValueText = FormatNiagaraVectorDefaultValue(Components, { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") });
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetColorDef())
		{
			FLinearColor ColorValue;
			const FString Trimmed = ValueText.TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("#")))
			{
				ColorValue = FLinearColor(FColor::FromHex(Trimmed));
			}
			else
			{
				TArray<float> Components;
				if (ParseFloatComponents(Trimmed, 4, Components))
				{
					ColorValue = FLinearColor(Components[0], Components[1], Components[2], Components[3]);
				}
				else if (ParseFloatComponents(Trimmed, 3, Components))
				{
					ColorValue = FLinearColor(Components[0], Components[1], Components[2], 1.0f);
				}
				else if (!ColorValue.InitFromString(Trimmed))
				{
					OutError = TEXT("invalid_color_value_text");
					return false;
				}
			}

			if (!FMath::IsFinite(ColorValue.R) || !FMath::IsFinite(ColorValue.G) || !FMath::IsFinite(ColorValue.B) || !FMath::IsFinite(ColorValue.A))
			{
				OutError = TEXT("invalid_color_value_text");
				return false;
			}
			TArray<float> Components = { ColorValue.R, ColorValue.G, ColorValue.B, ColorValue.A };
			OutValueText = FormatNiagaraVectorDefaultValue(Components, { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") });
			return true;
		}
		if (TypeDefinition.IsEnum())
		{
			UEnum* EnumType = TypeDefinition.GetEnum();
			if (!EnumType)
			{
				OutError = TEXT("enum_type_not_found");
				return false;
			}

			int64 EnumValue = 0;
			if (!TryResolveNiagaraEnumValueFromText(*EnumType, ValueText, EnumValue))
			{
				OutError = TEXT("invalid_enum_value_text");
				return false;
			}

			OutValueText = EnumType->GetNameStringByValue(EnumValue);
			if (OutValueText.IsEmpty())
			{
				OutValueText = LexToString(static_cast<int32>(EnumValue));
			}
			return true;
		}

		return true;
	}

	static bool IsNiagaraStrictStructPinDefaultType(const FNiagaraTypeDefinition& TypeDefinition)
	{
		return TypeDefinition == FNiagaraTypeDefinition::GetVec2Def() ||
			TypeDefinition == FNiagaraTypeDefinition::GetVec3Def() ||
			TypeDefinition == FNiagaraTypeDefinition::GetVec4Def() ||
			TypeDefinition == FNiagaraTypeDefinition::GetPositionDef() ||
			TypeDefinition == FNiagaraTypeDefinition::GetQuatDef() ||
			TypeDefinition == FNiagaraTypeDefinition::GetColorDef();
	}

	static bool IsNiagaraValidatedPinDefaultType(const FNiagaraTypeDefinition& TypeDefinition)
	{
		return TypeDefinition == FNiagaraTypeDefinition::GetBoolDef() ||
			TypeDefinition == FNiagaraTypeDefinition::GetIntDef() ||
			TypeDefinition == FNiagaraTypeDefinition::GetFloatDef() ||
			TypeDefinition == FNiagaraTypeHelper::GetDoubleDef() ||
			IsNiagaraStrictStructPinDefaultType(TypeDefinition) ||
			TypeDefinition.IsEnum();
	}

	static bool ValidateNiagaraPinDefaultValueAfterSet(
		const FNiagaraTypeDefinition& TypeDefinition,
		const FString& AppliedValueText,
		const FString& StoredDefaultValue,
		FString& OutStatus,
		FString& OutError)
	{
		OutStatus.Empty();
		OutError.Empty();

		if (!IsNiagaraValidatedPinDefaultType(TypeDefinition))
		{
			OutStatus = TEXT("not_validated_unsupported_type");
			return true;
		}

		const FString AppliedTrimmed = AppliedValueText.TrimStartAndEnd();
		const FString StoredTrimmed = StoredDefaultValue.TrimStartAndEnd();
		if (StoredTrimmed.Equals(AppliedTrimmed, ESearchCase::CaseSensitive))
		{
			OutStatus = TEXT("stored_default_exact_match");
			return true;
		}

		FString NormalizedStoredValue;
		FString NormalizeStoredError;
		if (!NormalizeNiagaraValueTextForPinDefault(TypeDefinition, StoredTrimmed, NormalizedStoredValue, NormalizeStoredError))
		{
			OutStatus = TEXT("stored_default_parse_failed");
			OutError = NormalizeStoredError;
			return false;
		}

		// UE Niagara struct defaults are safest when the stored text is the native import
		// form, e.g. (X=...,Y=...,Z=...) or (R=...,G=...,B=...,A=...).
		// A bare vector like "0 0 1" can normalize semantically here but still appear
		// as a zero vector or black color in the Niagara stack UI.
		if (IsNiagaraStrictStructPinDefaultType(TypeDefinition))
		{
			OutStatus = TEXT("stored_struct_default_text_mismatch");
			OutError = FString::Printf(
				TEXT("stored_default_value_does_not_match_applied_import_text; stored=%s; applied=%s; normalized_stored=%s"),
				*StoredTrimmed,
				*AppliedTrimmed,
				*NormalizedStoredValue);
			return false;
		}

		if (NormalizedStoredValue.Equals(AppliedTrimmed, ESearchCase::CaseSensitive))
		{
			OutStatus = TEXT("stored_default_semantic_match");
			return true;
		}

		OutStatus = TEXT("stored_default_value_mismatch");
		OutError = FString::Printf(
			TEXT("stored_default_value_does_not_match_applied_value; stored=%s; applied=%s; normalized_stored=%s"),
			*StoredTrimmed,
			*AppliedTrimmed,
			*NormalizedStoredValue);
		return false;
	}

	static bool FindUserParameterByName(const FNiagaraUserRedirectionParameterStore& Store, const FString& InParameterName, FNiagaraVariable& OutParameter)
	{
		const FString SearchName = InParameterName.TrimStartAndEnd();
		if (SearchName.IsEmpty())
		{
			return false;
		}

		const FString SearchNameWithoutUser = StripUserNamespace(SearchName);
		TArray<FNiagaraVariable> Parameters;
		Store.GetUserParameters(Parameters);
		for (const FNiagaraVariable& Parameter : Parameters)
		{
			const FString FullName = Parameter.GetName().ToString();
			const FString ShortName = StripUserNamespace(FullName);
			if (FullName.Equals(SearchName, ESearchCase::IgnoreCase) ||
				ShortName.Equals(SearchName, ESearchCase::IgnoreCase) ||
				ShortName.Equals(SearchNameWithoutUser, ESearchCase::IgnoreCase))
			{
				OutParameter = Parameter;
				return true;
			}
		}
		return false;
	}

	static bool SetUserParameterValueFromText(FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Parameter, const FString& ValueText, FString& OutError)
	{
		const FNiagaraTypeDefinition& TypeDefinition = Parameter.GetType();
		if (TypeDefinition.IsDataInterface())
		{
			UObject* ObjectValue = nullptr;
			const FString ObjectPath = ValueText.TrimStartAndEnd();
			if (!ObjectPath.IsEmpty() && !ObjectPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				ObjectValue = LoadAssetObject(ObjectPath);
				if (!ObjectValue)
				{
					OutError = TEXT("data_interface_object_not_found");
					return false;
				}
			}

			UNiagaraDataInterface* DataInterface = Cast<UNiagaraDataInterface>(ObjectValue);
			if (ObjectValue && !DataInterface)
			{
				OutError = TEXT("object_is_not_data_interface");
				return false;
			}
			Store.SetDataInterface(DataInterface, Parameter);
			return true;
		}

		if (TypeDefinition.IsUObject())
		{
			UObject* ObjectValue = nullptr;
			const FString ObjectPath = ValueText.TrimStartAndEnd();
			if (!ObjectPath.IsEmpty() && !ObjectPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				ObjectValue = LoadAssetObject(ObjectPath);
				if (!ObjectValue)
				{
					OutError = TEXT("uobject_parameter_value_not_found");
					return false;
				}
			}

			if (ObjectValue)
			{
				if (UClass* ExpectedClass = TypeDefinition.GetClass())
				{
					if (!ObjectValue->IsA(ExpectedClass))
					{
						OutError = TEXT("uobject_parameter_value_type_mismatch");
						return false;
					}
				}
			}

			Store.SetUObject(ObjectValue, Parameter);
			return true;
		}

		TArray<uint8> ValueBytes;
		if (!BuildNiagaraValueBytesFromText(TypeDefinition, ValueText, ValueBytes, OutError))
		{
			return false;
		}
		if (!Store.SetParameterData(ValueBytes.GetData(), Parameter, false))
		{
			OutError = TEXT("set_parameter_data_failed");
			return false;
		}
		return true;
	}

	static bool GetUserParameterValueAsText(const FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Parameter, FString& OutValueText, FString& OutObjectPath)
	{
		OutValueText.Reset();
		OutObjectPath.Reset();
		const FNiagaraTypeDefinition& TypeDefinition = Parameter.GetType();

		if (TypeDefinition.IsDataInterface())
		{
			if (UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Parameter))
			{
				OutObjectPath = DataInterface->GetPathName();
				return true;
			}
			return false;
		}
		if (TypeDefinition.IsUObject())
		{
			if (UObject* ObjectValue = Store.GetUObject(Parameter))
			{
				OutObjectPath = ObjectValue->GetPathName();
				return true;
			}
			return false;
		}

		const uint8* DataPtr = Store.GetParameterData(Parameter);
		if (!DataPtr)
		{
			return false;
		}

		OutValueText = TypeDefinition.ToString(DataPtr);
		return true;
	}

	static TSharedPtr<FJsonObject> BuildUserParameterJson(const FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Parameter, const bool bIncludeValue)
	{
		TSharedPtr<FJsonObject> ParameterObj = MakeShared<FJsonObject>();
		const FString FullName = Parameter.GetName().ToString();
		const FString ShortName = StripUserNamespace(FullName);
		const FNiagaraTypeDefinition& TypeDefinition = Parameter.GetType();

		ParameterObj->SetStringField(TEXT("name"), FullName);
		ParameterObj->SetStringField(TEXT("short_name"), ShortName);
		ParameterObj->SetStringField(TEXT("namespace"), GetVariableNamespace(FullName));
		ParameterObj->SetStringField(TEXT("leaf_name"), GetVariableLeafName(FullName));
		ParameterObj->SetStringField(TEXT("type"), TypeDefinition.GetNameText().ToString());
		ParameterObj->SetStringField(TEXT("type_internal"), TypeDefinition.GetName());
		ParameterObj->SetStringField(TEXT("type_path"), TypeDefinition.GetStruct() ? TypeDefinition.GetStruct()->GetPathName() : TEXT(""));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			ParameterObj,
			TEXT("parameter_name"),
			FullName,
			ShortName,
			TEXT("Niagara/UserParameters"),
			TEXT("niagara_parameter"),
			TEXT("typed_value"),
			{ TEXT("set"), TEXT("bind"), TEXT("clear") });

		if (bIncludeValue)
		{
			FString ValueText;
			FString ObjectPath;
			const bool bHasValue = GetUserParameterValueAsText(Store, Parameter, ValueText, ObjectPath);
			ParameterObj->SetBoolField(TEXT("has_value"), bHasValue);
			ParameterObj->SetStringField(TEXT("value_text"), ValueText);
			ParameterObj->SetStringField(TEXT("value_object_path"), ObjectPath);
		}
		return ParameterObj;
	}

	static bool FindGraphParameterByName(UNiagaraGraph& Graph, const FString& InParameterName, FNiagaraVariable& OutParameter, UNiagaraScriptVariable*& OutScriptVariable)
	{
		OutScriptVariable = nullptr;
		const FString SearchName = InParameterName.TrimStartAndEnd();
		if (SearchName.IsEmpty())
		{
			return false;
		}

		const UNiagaraGraph::FScriptVariableMap& ScriptVariableMap = Graph.GetAllMetaData();

		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : ScriptVariableMap)
		{
			if (Pair.Key.GetName().ToString().Equals(SearchName, ESearchCase::IgnoreCase))
			{
				OutParameter = Pair.Key;
				OutScriptVariable = Pair.Value;
				return true;
			}
		}

		const FString SearchLeaf = GetVariableLeafName(SearchName);
		FNiagaraVariable Candidate;
		UNiagaraScriptVariable* CandidateVar = nullptr;
		int32 CandidateCount = 0;
		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : ScriptVariableMap)
		{
			const FString Leaf = GetVariableLeafName(Pair.Key.GetName().ToString());
			if (Leaf.Equals(SearchLeaf, ESearchCase::IgnoreCase))
			{
				++CandidateCount;
				Candidate = Pair.Key;
				CandidateVar = Pair.Value;
			}
		}

		if (CandidateCount == 1 && CandidateVar)
		{
			OutParameter = Candidate;
			OutScriptVariable = CandidateVar;
			return true;
		}

		return false;
	}

	static UNiagaraScriptVariable* AddGraphParameterMetadata(UNiagaraGraph& Graph, const FNiagaraVariable& Parameter, const bool bIsStaticSwitch)
	{
		UNiagaraGraph::FScriptVariableMap& ScriptVariableMap = Graph.GetAllMetaData();
		if (TObjectPtr<UNiagaraScriptVariable>* ExistingScriptVariable = ScriptVariableMap.Find(Parameter))
		{
			return ExistingScriptVariable->Get();
		}

		UNiagaraScriptVariable* NewScriptVariable = NewObject<UNiagaraScriptVariable>(&Graph, NAME_None, RF_Transactional);
		if (!NewScriptVariable)
		{
			return nullptr;
		}

		NewScriptVariable->Init(Parameter, FNiagaraVariableMetaData());
		NewScriptVariable->SetIsStaticSwitch(bIsStaticSwitch);
		NewScriptVariable->DefaultMode = ENiagaraDefaultMode::FailIfPreviouslyNotSet;

		const int32 ValueSizeBytes = NewScriptVariable->Variable.GetSizeInBytes();
		if (ValueSizeBytes > 0)
		{
			TArray<uint8> ZeroValueBytes;
			ZeroValueBytes.SetNumZeroed(ValueSizeBytes);
			NewScriptVariable->SetDefaultValueData(ZeroValueBytes.GetData());
		}

		ScriptVariableMap.Add(Parameter, NewScriptVariable);
		return NewScriptVariable;
	}

	static bool RemoveGraphParameterMetadata(UNiagaraGraph& Graph, const FNiagaraVariable& Parameter)
	{
		UNiagaraGraph::FScriptVariableMap& ScriptVariableMap = Graph.GetAllMetaData();
		return ScriptVariableMap.Remove(Parameter) > 0;
	}

	static bool SetScriptVariableDefaultValueFromText(UNiagaraScriptVariable& ScriptVariable, const FString& ValueText, FString& OutError)
	{
		const FNiagaraTypeDefinition& TypeDefinition = ScriptVariable.Variable.GetType();
		if (TypeDefinition.IsUObject() || TypeDefinition.IsDataInterface())
		{
			OutError = TEXT("script_variable_object_default_not_supported");
			return false;
		}

		TArray<uint8> ValueBytes;
		if (!BuildNiagaraValueBytesFromText(TypeDefinition, ValueText, ValueBytes, OutError))
		{
			return false;
		}

		ScriptVariable.Modify();
		ScriptVariable.SetDefaultValueData(ValueBytes.GetData());
		ScriptVariable.UpdateChangeId();
		return true;
	}

	static TSharedPtr<FJsonObject> BuildGraphParameterJson(const FNiagaraVariable& Parameter, const UNiagaraScriptVariable* ScriptVariable, const bool bIncludeValue)
	{
		TSharedPtr<FJsonObject> ParameterObj = MakeShared<FJsonObject>();
		const FString FullName = Parameter.GetName().ToString();
		const FNiagaraTypeDefinition& TypeDefinition = Parameter.GetType();

		ParameterObj->SetStringField(TEXT("name"), FullName);
		ParameterObj->SetStringField(TEXT("namespace"), GetVariableNamespace(FullName));
		ParameterObj->SetStringField(TEXT("leaf_name"), GetVariableLeafName(FullName));
		ParameterObj->SetStringField(TEXT("type"), TypeDefinition.GetNameText().ToString());
		ParameterObj->SetStringField(TEXT("type_internal"), TypeDefinition.GetName());
		ParameterObj->SetStringField(TEXT("type_path"), TypeDefinition.GetStruct() ? TypeDefinition.GetStruct()->GetPathName() : TEXT(""));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			ParameterObj,
			TEXT("parameter_name"),
			FullName,
			GetVariableLeafName(FullName),
			TEXT("Niagara/GraphParameters"),
			TEXT("niagara_graph_parameter"),
			TEXT("typed_value"),
			{ TEXT("set_default"), TEXT("set_static_switch"), TEXT("bind") });

		if (ScriptVariable)
		{
			ParameterObj->SetBoolField(TEXT("is_static_switch"), ScriptVariable->GetIsStaticSwitch());
			ParameterObj->SetNumberField(TEXT("default_mode"), static_cast<int32>(ScriptVariable->DefaultMode));
		}

		if (bIncludeValue && ScriptVariable)
		{
			const uint8* DefaultValueData = ScriptVariable->GetDefaultValueData();
			const bool bHasDefaultValue = DefaultValueData != nullptr;
			ParameterObj->SetBoolField(TEXT("has_default_value"), bHasDefaultValue);
			if (bHasDefaultValue && !TypeDefinition.IsUObject() && !TypeDefinition.IsDataInterface())
			{
				ParameterObj->SetStringField(TEXT("default_value_text"), TypeDefinition.ToString(DefaultValueData));
			}
		}
		return ParameterObj;
	}

	static FString GetNiagaraAssetKind(UObject* Asset)
	{
		if (Cast<UNiagaraSystem>(Asset))
		{
			return TEXT("system");
		}
		if (Cast<UNiagaraEmitter>(Asset))
		{
			return TEXT("emitter");
		}
		if (Cast<UNiagaraScript>(Asset))
		{
			return TEXT("script");
		}
		return TEXT("unknown");
	}

	enum class ENiagaraCompileLogSeverityFilter : uint8
	{
		All,
		ErrorOnly,
		WarningOnly,
		WarningOrError,
		InfoOnly,
	};

	static FString ScriptCompileStatusToString(const ENiagaraScriptCompileStatus Status)
	{
		if (const UEnum* StatusEnum = StaticEnum<ENiagaraScriptCompileStatus>())
		{
			return StatusEnum->GetNameStringByValue(static_cast<int64>(Status));
		}
		return TEXT("NCS_Unknown");
	}

	static FString CompileEventSeverityToString(const FNiagaraCompileEventSeverity Severity)
	{
		switch (Severity)
		{
		case FNiagaraCompileEventSeverity::Log:
			return TEXT("Log");
		case FNiagaraCompileEventSeverity::Display:
			return TEXT("Display");
		case FNiagaraCompileEventSeverity::Warning:
			return TEXT("Warning");
		case FNiagaraCompileEventSeverity::Error:
			return TEXT("Error");
		default:
			return TEXT("Display");
		}
	}

	static FString CompileEventSourceToString(const FNiagaraCompileEventSource Source)
	{
		switch (Source)
		{
		case FNiagaraCompileEventSource::Unset:
			return TEXT("Unset");
		case FNiagaraCompileEventSource::ScriptDependency:
			return TEXT("ScriptDependency");
		default:
			return TEXT("Unset");
		}
	}

	static bool IsCompileStatusWarning(const ENiagaraScriptCompileStatus Status)
	{
		return Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
			|| Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings;
	}

	static bool ParseCompileLogSeverityFilter(const FString& SeverityFilterText, ENiagaraCompileLogSeverityFilter& OutFilter)
	{
		FString Normalized = SeverityFilterText.TrimStartAndEnd().ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("all"))
		{
			OutFilter = ENiagaraCompileLogSeverityFilter::All;
			return true;
		}
		if (Normalized == TEXT("error") || Normalized == TEXT("errors"))
		{
			OutFilter = ENiagaraCompileLogSeverityFilter::ErrorOnly;
			return true;
		}
		if (Normalized == TEXT("warning") || Normalized == TEXT("warnings") || Normalized == TEXT("warn"))
		{
			OutFilter = ENiagaraCompileLogSeverityFilter::WarningOnly;
			return true;
		}
		if (Normalized == TEXT("warning_or_error") || Normalized == TEXT("warn_or_error") || Normalized == TEXT("errors_and_warnings"))
		{
			OutFilter = ENiagaraCompileLogSeverityFilter::WarningOrError;
			return true;
		}
		if (Normalized == TEXT("info") || Normalized == TEXT("display") || Normalized == TEXT("log"))
		{
			OutFilter = ENiagaraCompileLogSeverityFilter::InfoOnly;
			return true;
		}
		return false;
	}

	static bool ShouldIncludeCompileEvent(const FNiagaraCompileEvent& CompileEvent, const ENiagaraCompileLogSeverityFilter SeverityFilter)
	{
		switch (SeverityFilter)
		{
		case ENiagaraCompileLogSeverityFilter::All:
			return true;
		case ENiagaraCompileLogSeverityFilter::ErrorOnly:
			return CompileEvent.Severity == FNiagaraCompileEventSeverity::Error;
		case ENiagaraCompileLogSeverityFilter::WarningOnly:
			return CompileEvent.Severity == FNiagaraCompileEventSeverity::Warning;
		case ENiagaraCompileLogSeverityFilter::WarningOrError:
			return CompileEvent.Severity == FNiagaraCompileEventSeverity::Warning || CompileEvent.Severity == FNiagaraCompileEventSeverity::Error;
		case ENiagaraCompileLogSeverityFilter::InfoOnly:
			return CompileEvent.Severity == FNiagaraCompileEventSeverity::Log || CompileEvent.Severity == FNiagaraCompileEventSeverity::Display;
		default:
			return true;
		}
	}

	static TSharedPtr<FJsonObject> BuildCompileEventJson(const FNiagaraCompileEvent& CompileEvent, const bool bIncludeStackGuids)
	{
		TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
		EventObj->SetStringField(TEXT("severity"), CompileEventSeverityToString(CompileEvent.Severity));
		EventObj->SetStringField(TEXT("message"), CompileEvent.Message);
		EventObj->SetStringField(TEXT("short_description"), CompileEvent.ShortDescription);
		EventObj->SetStringField(TEXT("node_guid"), CompileEvent.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		EventObj->SetStringField(TEXT("pin_guid"), CompileEvent.PinGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		EventObj->SetStringField(TEXT("source"), CompileEventSourceToString(CompileEvent.Source));

		if (bIncludeStackGuids)
		{
			TArray<TSharedPtr<FJsonValue>> StackGuidValues;
			StackGuidValues.Reserve(CompileEvent.StackGuids.Num());
			for (const FGuid& StackGuid : CompileEvent.StackGuids)
			{
				StackGuidValues.Add(MakeShared<FJsonValueString>(StackGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
			}
			EventObj->SetArrayField(TEXT("stack_guids"), StackGuidValues);
		}

		return EventObj;
	}

	struct FCollectedNiagaraScriptInfo
	{
		UNiagaraScript* Script = nullptr;
		FString OwnerType;
		FString OwnerName;
		FString OwnerPath;
		FString EmitterHandleId;
		FString EmitterVersion;
		bool bEmitterEnabled = true;
	};

	static void AppendUniqueCollectedScript(TArray<FCollectedNiagaraScriptInfo>& OutScripts, TSet<const UNiagaraScript*>& SeenScripts, UNiagaraScript* Script, const FString& OwnerType, const FString& OwnerName, const FString& OwnerPath, const FString& EmitterHandleId, const FString& EmitterVersion, const bool bEmitterEnabled)
	{
		if (!Script || SeenScripts.Contains(Script))
		{
			return;
		}

		SeenScripts.Add(Script);

		FCollectedNiagaraScriptInfo& Item = OutScripts.AddDefaulted_GetRef();
		Item.Script = Script;
		Item.OwnerType = OwnerType;
		Item.OwnerName = OwnerName;
		Item.OwnerPath = OwnerPath;
		Item.EmitterHandleId = EmitterHandleId;
		Item.EmitterVersion = EmitterVersion;
		Item.bEmitterEnabled = bEmitterEnabled;
	}

	static void CollectSystemScriptsForCompileLog(UNiagaraSystem& NiagaraSystem, const bool bIncludeDisabledEmitters, TArray<FCollectedNiagaraScriptInfo>& OutScripts)
	{
		OutScripts.Reset();

		TSet<const UNiagaraScript*> SeenScripts;
		AppendUniqueCollectedScript(
			OutScripts,
			SeenScripts,
			NiagaraSystem.GetSystemSpawnScript(),
			TEXT("system_script"),
			TEXT("SystemSpawn"),
			NiagaraSystem.GetPathName(),
			FString(),
			FString(),
			true);

		AppendUniqueCollectedScript(
			OutScripts,
			SeenScripts,
			NiagaraSystem.GetSystemUpdateScript(),
			TEXT("system_script"),
			TEXT("SystemUpdate"),
			NiagaraSystem.GetPathName(),
			FString(),
			FString(),
			true);

		const TArray<FNiagaraEmitterHandle>& EmitterHandles = NiagaraSystem.GetEmitterHandles();
		for (const FNiagaraEmitterHandle& Handle : EmitterHandles)
		{
			if (!bIncludeDisabledEmitters && !Handle.GetIsEnabled())
			{
				continue;
			}

			FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
			const FVersionedNiagaraEmitterData* EmitterData = VersionedEmitter.GetEmitterData();
			if (!EmitterData && VersionedEmitter.Emitter)
			{
				EmitterData = VersionedEmitter.Emitter->GetLatestEmitterData();
			}
			if (!EmitterData)
			{
				continue;
			}

			TArray<UNiagaraScript*> EmitterScripts;
			EmitterData->GetScripts(EmitterScripts, false, false);
			const FString OwnerName = Handle.GetUniqueInstanceName();
			const FString OwnerPath = VersionedEmitter.Emitter ? VersionedEmitter.Emitter->GetPathName() : FString();
			const FString EmitterVersion = VersionedEmitter.Version.ToString(EGuidFormats::DigitsWithHyphensLower);
			const FString HandleId = Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
			for (UNiagaraScript* Script : EmitterScripts)
			{
				AppendUniqueCollectedScript(
					OutScripts,
					SeenScripts,
					Script,
					TEXT("emitter_in_system"),
					OwnerName,
					OwnerPath,
					HandleId,
					EmitterVersion,
					Handle.GetIsEnabled());
			}
		}
	}

	struct FNiagaraEmitterEditContext
	{
		UNiagaraEmitter* Emitter = nullptr;
		FGuid VersionGuid;
		FVersionedNiagaraEmitterData* EmitterData = nullptr;
		UNiagaraScriptSource* ScriptSource = nullptr;
		UNiagaraGraph* Graph = nullptr;
	};

	struct FNiagaraSystemEditContext
	{
		UNiagaraSystem* System = nullptr;
		UNiagaraScriptSource* ScriptSource = nullptr;
		UNiagaraGraph* Graph = nullptr;
	};

	struct FResolvedNiagaraStage
	{
		UNiagaraNodeOutput* OutputNode = nullptr;
		ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
		FGuid UsageId;
		FString StageKey;
	};

	static FString ScriptUsageToString(const ENiagaraScriptUsage Usage)
	{
		if (const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>())
		{
			return UsageEnum->GetNameStringByValue((int64)Usage);
		}
		return TEXT("Unknown");
	}

	static bool TryParseScriptUsage(const FString& InUsageText, ENiagaraScriptUsage& OutUsage)
	{
		const FString UsageText = InUsageText.TrimStartAndEnd();
		if (UsageText.IsEmpty())
		{
			return false;
		}

		if (const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>())
		{
			int64 ParsedValue = UsageEnum->GetValueByNameString(UsageText, EGetByNameFlags::CheckAuthoredName);
			if (ParsedValue == INDEX_NONE)
			{
				const FString QualifiedName = FString::Printf(TEXT("ENiagaraScriptUsage::%s"), *UsageText);
				ParsedValue = UsageEnum->GetValueByNameString(QualifiedName, EGetByNameFlags::CheckAuthoredName);
			}
			if (ParsedValue == INDEX_NONE)
			{
				ParsedValue = UsageEnum->GetValueByNameString(UsageText, EGetByNameFlags::None);
			}
			if (ParsedValue != INDEX_NONE)
			{
				OutUsage = (ENiagaraScriptUsage)ParsedValue;
				return true;
			}
		}
		return false;
	}

	static FString ScriptLibraryVisibilityToString(const ENiagaraScriptLibraryVisibility Visibility)
	{
		if (const UEnum* VisibilityEnum = StaticEnum<ENiagaraScriptLibraryVisibility>())
		{
			return VisibilityEnum->GetNameStringByValue((int64)Visibility);
		}
		return TEXT("Unknown");
	}

	static TArray<TSharedPtr<FJsonValue>> BuildScriptUsageJsonArray(const TArray<ENiagaraScriptUsage>& Usages)
	{
		TArray<TSharedPtr<FJsonValue>> UsageJson;
		UsageJson.Reserve(Usages.Num());
		for (const ENiagaraScriptUsage Usage : Usages)
		{
			UsageJson.Add(MakeShared<FJsonValueString>(ScriptUsageToString(Usage)));
		}
		return UsageJson;
	}

	static FString MakeRelativePackagePath(const FString& PackagePath, const FString& RootPath)
	{
		FString RelativePath = PackagePath;
		if (RelativePath.RemoveFromStart(RootPath))
		{
			RelativePath.RemoveFromStart(TEXT("/"));
		}
		return RelativePath;
	}

	static FString MakeModuleCategory(const FString& RelativePackagePath)
	{
		if (RelativePackagePath.IsEmpty())
		{
			return TEXT("Root");
		}

		FString Category;
		FString Remainder;
		if (RelativePackagePath.Split(TEXT("/"), &Category, &Remainder))
		{
			return Category;
		}
		return RelativePackagePath;
	}

	static FString MakeStageKey(const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		return FString::Printf(TEXT("%s|%s"), *ScriptUsageToString(Usage), *UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	}

	static bool TryParseStageKey(const FString& StageKey, ENiagaraScriptUsage& OutUsage, FGuid& OutUsageId)
	{
		FString StageKeyText = StageKey.TrimStartAndEnd();
		if (StageKeyText.IsEmpty())
		{
			return false;
		}

		FString UsageText;
		FString UsageIdText;
		if (!StageKeyText.Split(TEXT("|"), &UsageText, &UsageIdText))
		{
			return false;
		}

		if (!TryParseScriptUsage(UsageText, OutUsage))
		{
			return false;
		}
		return FGuid::Parse(UsageIdText, OutUsageId);
	}

	static bool GetSystemEditContext(const TSharedPtr<FJsonObject>& Params, FNiagaraSystemEditContext& OutCtx, FString& OutError, const bool bRequireGraph)
	{
		FString SystemAssetPathInput;
		if (!Params.IsValid() ||
			(!Params->TryGetStringField(TEXT("system_asset_path"), SystemAssetPathInput) &&
				!Params->TryGetStringField(TEXT("asset_path"), SystemAssetPathInput)) ||
			SystemAssetPathInput.IsEmpty())
		{
			OutError = TEXT("missing_system_asset_path");
			return false;
		}

		UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadAssetObject(SystemAssetPathInput));
		if (!System)
		{
			OutError = TEXT("system_asset_not_found");
			return false;
		}

		UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
		UNiagaraScriptSource* ScriptSource = SpawnScript ? Cast<UNiagaraScriptSource>(SpawnScript->GetLatestSource()) : nullptr;
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (bRequireGraph && (!ScriptSource || !Graph))
		{
			OutError = TEXT("missing_system_graph_source");
			return false;
		}

		OutCtx.System = System;
		OutCtx.ScriptSource = ScriptSource;
		OutCtx.Graph = Graph;
		return true;
	}

	static bool IsSystemStageUsage(const ENiagaraScriptUsage Usage)
	{
		return Usage == ENiagaraScriptUsage::SystemSpawnScript ||
			Usage == ENiagaraScriptUsage::SystemUpdateScript;
	}

	static bool GetEmitterEditContext(const TSharedPtr<FJsonObject>& Params, FNiagaraEmitterEditContext& OutCtx, FString& OutError, const bool bRequireGraph)
	{
		FString EmitterAssetPathInput;
		if (!Params.IsValid() ||
			(!Params->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPathInput) &&
				!Params->TryGetStringField(TEXT("asset_path"), EmitterAssetPathInput)) ||
			EmitterAssetPathInput.IsEmpty())
		{
			OutError = TEXT("missing_emitter_asset_path");
			return false;
		}

		UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(LoadAssetObject(EmitterAssetPathInput));
		if (!Emitter)
		{
			OutError = TEXT("emitter_asset_not_found");
			return false;
		}

		FGuid VersionGuid = Emitter->GetExposedVersion().VersionGuid;
		FString VersionText;
		if (Params->TryGetStringField(TEXT("emitter_version"), VersionText) && !VersionText.IsEmpty())
		{
			FGuid ParsedVersionGuid;
			if (!FGuid::Parse(VersionText, ParsedVersionGuid))
			{
				OutError = TEXT("invalid_emitter_version");
				return false;
			}
			VersionGuid = ParsedVersionGuid;
		}

		FVersionedNiagaraEmitterData* EmitterData = Emitter->GetEmitterData(VersionGuid);
		if (!EmitterData)
		{
			EmitterData = Emitter->GetLatestEmitterData();
			if (EmitterData)
			{
				VersionGuid = EmitterData->Version.VersionGuid;
			}
		}
		if (!EmitterData)
		{
			OutError = TEXT("missing_emitter_version_data");
			return false;
		}

		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (bRequireGraph && (!ScriptSource || !Graph))
		{
			OutError = TEXT("missing_emitter_graph_source");
			return false;
		}

		OutCtx.Emitter = Emitter;
		OutCtx.VersionGuid = VersionGuid;
		OutCtx.EmitterData = EmitterData;
		OutCtx.ScriptSource = ScriptSource;
		OutCtx.Graph = Graph;
		return true;
	}

	static UNiagaraNodeFunctionCall* FindModuleNodeByGuid(UNiagaraGraph* Graph, const FGuid& ModuleNodeGuid)
	{
		if (!Graph || !ModuleNodeGuid.IsValid())
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(Node);
			if (ModuleNode && ModuleNode->NodeGuid == ModuleNodeGuid)
			{
				return ModuleNode;
			}
		}
		return nullptr;
	}

	static FString GetModuleScriptPath(UNiagaraNodeFunctionCall& ModuleNode)
	{
		if (ModuleNode.FunctionScript)
		{
			return ModuleNode.FunctionScript->GetPathName();
		}
		return ModuleNode.FunctionScriptAssetObjectPath.ToString();
	}

	static void GetOutputNodesFromGraph(UNiagaraGraph& Graph, TArray<UNiagaraNodeOutput*>& OutOutputNodes)
	{
		OutOutputNodes.Reset();
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
			{
				OutOutputNodes.Add(OutputNode);
			}
		}
	}

	static UEdGraphPin* FindFirstInputPin(UEdGraphNode& Node)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UEdGraphPin* FindFirstOutputPin(UEdGraphNode& Node)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	struct FLocalNiagaraStackNodeGroup
	{
		TArray<UNiagaraNode*> StartNodes;
		UNiagaraNode* EndNode = nullptr;
	};

	static bool IsParameterMapPin(const UEdGraphPin* Pin)
	{
		return Pin != nullptr && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef();
	}

	static UEdGraphPin* FindParameterMapInputPin(UEdGraphNode& Node)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && IsParameterMapPin(Pin))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UEdGraphPin* FindParameterMapOutputPin(UEdGraphNode& Node)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && IsParameterMapPin(Pin))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static bool TryGetOrCreateStackFunctionInputOverridePin(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FNiagaraParameterHandle& AliasedInputHandle,
		const FNiagaraTypeDefinition& InputType,
		const FGuid& InputScriptVariableGuid,
		UEdGraphPin*& OutOverridePin,
		FString& OutError)
	{
		OutOverridePin = FindOverridePinByAliasedInputName(
			Cast<UNiagaraGraph>(ModuleNode.GetGraph()),
			AliasedInputHandle.GetParameterHandleString());
		if (OutOverridePin)
		{
			return true;
		}

		UNiagaraGraph* Graph = Cast<UNiagaraGraph>(ModuleNode.GetGraph());
		if (!Graph)
		{
			OutError = TEXT("module_graph_missing");
			return false;
		}

		bool bModuleIsInOutputStack = false;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node);
			if (!OutputNode)
			{
				continue;
			}

			TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
			GetStageModuleNodes(*OutputNode, ModuleNodes);
			if (ModuleNodes.Contains(&ModuleNode))
			{
				bModuleIsInOutputStack = true;
				break;
			}
		}
		if (!bModuleIsInOutputStack)
		{
			OutError = TEXT("module_not_in_output_stack");
			return false;
		}

		UEdGraphPin* ParameterMapInputPin = FindParameterMapInputPin(ModuleNode);
		if (!ParameterMapInputPin)
		{
			OutError = TEXT("module_parameter_map_input_missing");
			return false;
		}
		if (ParameterMapInputPin->LinkedTo.Num() != 1)
		{
			OutError = FString::Printf(TEXT("module_parameter_map_input_link_count:%d"), ParameterMapInputPin->LinkedTo.Num());
			return false;
		}

		UEdGraphPin* PreviousStackPin = ParameterMapInputPin->LinkedTo[0];
		if (!PreviousStackPin || !IsParameterMapPin(PreviousStackPin) || PreviousStackPin->Direction != EGPD_Output)
		{
			OutError = TEXT("module_parameter_map_input_invalid_link");
			return false;
		}

		OutOverridePin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
			ModuleNode,
			AliasedInputHandle,
			InputType,
			InputScriptVariableGuid,
			FGuid());
		return OutOverridePin != nullptr;
	}


	static UNiagaraNodeInput* FindStageParameterMapInputNode(UNiagaraNodeOutput& OutputNode)
	{
		UEdGraphNode* CurrentNode = &OutputNode;
		TSet<UEdGraphNode*> VisitedNodes;

		while (CurrentNode && !VisitedNodes.Contains(CurrentNode))
		{
			VisitedNodes.Add(CurrentNode);

			UEdGraphPin* InputPin = FindParameterMapInputPin(*CurrentNode);
			if (!InputPin || InputPin->LinkedTo.Num() == 0)
			{
				return nullptr;
			}

			UEdGraphPin* LinkedPin = nullptr;
			for (UEdGraphPin* CandidatePin : InputPin->LinkedTo)
			{
				if (IsParameterMapPin(CandidatePin))
				{
					LinkedPin = CandidatePin;
					break;
				}
			}
			if (!LinkedPin)
			{
				return nullptr;
			}

			UEdGraphNode* PreviousNode = LinkedPin->GetOwningNode();
			if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(PreviousNode))
			{
				return InputNode;
			}

			CurrentNode = PreviousNode;
		}

		return nullptr;
	}

	static void GetLocalStackNodeGroups(UNiagaraNodeOutput& OutputNode, TArray<FLocalNiagaraStackNodeGroup>& OutStackNodeGroups)
	{
		OutStackNodeGroups.Reset();

		UNiagaraNodeInput* InputNode = FindStageParameterMapInputNode(OutputNode);
		if (!InputNode)
		{
			return;
		}

		FLocalNiagaraStackNodeGroup InputGroup;
		InputGroup.StartNodes.Add(InputNode);
		InputGroup.EndNode = InputNode;
		OutStackNodeGroups.Add(InputGroup);

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		GetStageModuleNodes(OutputNode, ModuleNodes);
		for (UNiagaraNodeFunctionCall* ModuleNode : ModuleNodes)
		{
			if (!ModuleNode)
			{
				continue;
			}

			FLocalNiagaraStackNodeGroup ModuleGroup;
			UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*OutStackNodeGroups.Last().EndNode);
			if (!PreviousOutputPin)
			{
				return;
			}
			for (UEdGraphPin* LinkedPin : PreviousOutputPin->LinkedTo)
			{
				if (LinkedPin)
				{
					if (UNiagaraNode* LinkedNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
					{
						ModuleGroup.StartNodes.Add(LinkedNode);
					}
				}
			}
			ModuleGroup.EndNode = ModuleNode;
			OutStackNodeGroups.Add(ModuleGroup);
		}

		FLocalNiagaraStackNodeGroup OutputGroup;
		UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*OutStackNodeGroups.Last().EndNode);
		if (!PreviousOutputPin)
		{
			return;
		}
		for (UEdGraphPin* LinkedPin : PreviousOutputPin->LinkedTo)
		{
			if (LinkedPin)
			{
				if (UNiagaraNode* LinkedNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
				{
					OutputGroup.StartNodes.Add(LinkedNode);
				}
			}
		}
		OutputGroup.EndNode = &OutputNode;
		OutStackNodeGroups.Add(OutputGroup);
	}

	static void DisconnectLocalStackNodeGroup(const FLocalNiagaraStackNodeGroup& DisconnectGroup, const FLocalNiagaraStackNodeGroup& PreviousGroup, const FLocalNiagaraStackNodeGroup& NextGroup)
	{
		UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*PreviousGroup.EndNode);
		UEdGraphPin* DisconnectOutputPin = FindParameterMapOutputPin(*DisconnectGroup.EndNode);
		if (!PreviousOutputPin || !DisconnectOutputPin)
		{
			return;
		}

		PreviousOutputPin->BreakAllPinLinks(true);
		DisconnectOutputPin->BreakAllPinLinks(true);

		for (UNiagaraNode* NextStartNode : NextGroup.StartNodes)
		{
			if (!NextStartNode)
			{
				continue;
			}
			if (UEdGraphPin* NextInputPin = FindParameterMapInputPin(*NextStartNode))
			{
				PreviousOutputPin->MakeLinkTo(NextInputPin);
			}
		}
	}

	static void ConnectLocalStackNodeGroup(const FLocalNiagaraStackNodeGroup& ConnectGroup, const FLocalNiagaraStackNodeGroup& NewPreviousGroup, const FLocalNiagaraStackNodeGroup& NewNextGroup)
	{
		UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*NewPreviousGroup.EndNode);
		UEdGraphPin* ConnectOutputPin = FindParameterMapOutputPin(*ConnectGroup.EndNode);
		if (!PreviousOutputPin || !ConnectOutputPin)
		{
			return;
		}

		PreviousOutputPin->BreakAllPinLinks(true);
		for (UNiagaraNode* ConnectStartNode : ConnectGroup.StartNodes)
		{
			if (!ConnectStartNode)
			{
				continue;
			}
			if (UEdGraphPin* ConnectInputPin = FindParameterMapInputPin(*ConnectStartNode))
			{
				PreviousOutputPin->MakeLinkTo(ConnectInputPin);
			}
		}

		for (UNiagaraNode* NextStartNode : NewNextGroup.StartNodes)
		{
			if (!NextStartNode)
			{
				continue;
			}
			if (UEdGraphPin* NextInputPin = FindParameterMapInputPin(*NextStartNode))
			{
				ConnectOutputPin->MakeLinkTo(NextInputPin);
			}
		}
	}

	static UNiagaraNodeOutput* EnsureStageInputOutputBridge(UNiagaraGraph& Graph, const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		Graph.Modify();
		NormalizeGraphNodeGuids(&Graph);

		UNiagaraNodeOutput* OutputNode = nullptr;
		TArray<UNiagaraNodeOutput*> ExistingOutputs;
		GetOutputNodesFromGraph(Graph, ExistingOutputs);
		for (UNiagaraNodeOutput* ExistingOutput : ExistingOutputs)
		{
			if (ExistingOutput && ExistingOutput->GetUsage() == Usage && ExistingOutput->GetUsageId() == UsageId)
			{
				OutputNode = ExistingOutput;
				break;
			}
		}

		UEdGraphPin* OutputNodeInputPin = OutputNode ? FindFirstInputPin(*OutputNode) : nullptr;
		if (OutputNode && !OutputNodeInputPin)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: remove invalid output node usage=%s usage_id=%s reason=no_input_pin"),
				*ScriptUsageToString(Usage),
				*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			Graph.RemoveNode(OutputNode);
			OutputNode = nullptr;
		}

		if (!OutputNode)
		{
			OutputNode = NewObject<UNiagaraNodeOutput>(&Graph, NAME_None, RF_Transactional);
			if (!OutputNode)
			{
				return nullptr;
			}
			OutputNode->CreateNewGuid();
			OutputNode->SetUsage(Usage);
			OutputNode->SetUsageId(UsageId);
			OutputNode->Outputs.Empty();
			OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
			OutputNode->AllocateDefaultPins();
			Graph.AddNode(OutputNode, true, false);
			OutputNodeInputPin = FindFirstInputPin(*OutputNode);
		}
		else
		{
			OutputNode->Modify();
			OutputNodeInputPin = FindFirstInputPin(*OutputNode);
		}

		if (!OutputNodeInputPin)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=output_input_pin_missing"),
				*ScriptUsageToString(Usage),
				*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			return nullptr;
		}

		if (OutputNodeInputPin->LinkedTo.Num() > 0 && FindStageParameterMapInputNode(*OutputNode) == nullptr)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: reset invalid stack usage=%s usage_id=%s reason=input_node_unreachable"),
				*ScriptUsageToString(Usage),
				*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			OutputNodeInputPin->BreakAllPinLinks();
		}

		if (OutputNodeInputPin->LinkedTo.Num() == 0)
		{
			UNiagaraNodeInput* InputNode = NewObject<UNiagaraNodeInput>(&Graph, NAME_None, RF_Transactional);
			if (!InputNode)
			{
				FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=create_input_node_failed"),
					*ScriptUsageToString(Usage),
					*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
				return nullptr;
			}

			InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
			InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
			InputNode->CreateNewGuid();
			InputNode->AllocateDefaultPins();
			Graph.AddNode(InputNode, true, false);

			UEdGraphPin* InputNodeOutputPin = FindFirstOutputPin(*InputNode);
			if (!InputNodeOutputPin)
			{
				FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=input_output_pin_missing"),
					*ScriptUsageToString(Usage),
					*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
				return nullptr;
			}

			bool bLinked = false;
			if (const UEdGraphSchema* Schema = Graph.GetSchema())
			{
				bLinked = Schema->TryCreateConnection(OutputNodeInputPin, InputNodeOutputPin);
			}
			if (!bLinked)
			{
				OutputNodeInputPin->MakeLinkTo(InputNodeOutputPin);
				bLinked = OutputNodeInputPin->LinkedTo.Contains(InputNodeOutputPin) || InputNodeOutputPin->LinkedTo.Contains(OutputNodeInputPin);
			}
			if (!bLinked)
			{
				FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=link_failed"),
					*ScriptUsageToString(Usage),
					*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
				return nullptr;
			}
		}

		return OutputNode;
	}

	static void GatherUpstreamNodesRecursive(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes, TArray<UEdGraphNode*>& OutNodes)
	{
		if (!Node || VisitedNodes.Contains(Node))
		{
			return;
		}

		VisitedNodes.Add(Node);
		OutNodes.Add(Node);

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin)
				{
					GatherUpstreamNodesRecursive(LinkedPin->GetOwningNode(), VisitedNodes, OutNodes);
				}
			}
		}
	}

	static void GetStageModuleNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules)
	{
		OutModules.Reset();

		TSet<UEdGraphNode*> VisitedNodes;
		TArray<UEdGraphNode*> StageNodes;
		GatherUpstreamNodesRecursive(&OutputNode, VisitedNodes, StageNodes);

		TArray<UNiagaraNodeFunctionCall*> ModulesFromOutputToInput;
		for (UEdGraphNode* StageNode : StageNodes)
		{
			UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(StageNode);
			if (!ModuleNode)
			{
				continue;
			}

			const ENiagaraScriptUsage CalledUsage = ModuleNode->GetCalledUsage();
			if (CalledUsage == ENiagaraScriptUsage::Module || CalledUsage == ENiagaraScriptUsage::Function)
			{
				ModulesFromOutputToInput.Add(ModuleNode);
			}
		}

		for (int32 Index = ModulesFromOutputToInput.Num() - 1; Index >= 0; --Index)
		{
			OutModules.Add(ModulesFromOutputToInput[Index]);
		}
	}

	static bool ResolveStageFromParams(UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& Params, FResolvedNiagaraStage& OutStage, FString& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = TEXT("missing_params");
			return false;
		}

		ENiagaraScriptUsage TargetUsage = ENiagaraScriptUsage::Function;
		FGuid TargetUsageId;
		bool bHasUsage = false;
		bool bHasUsageId = false;

		FString StageKey;
		if (Params->TryGetStringField(TEXT("stage_key"), StageKey) && !StageKey.IsEmpty())
		{
			if (!TryParseStageKey(StageKey, TargetUsage, TargetUsageId))
			{
				OutError = TEXT("invalid_stage_key");
				return false;
			}
			bHasUsage = true;
			bHasUsageId = true;
		}
		else
		{
			FString UsageText;
			if (Params->TryGetStringField(TEXT("script_usage"), UsageText) && !UsageText.IsEmpty())
			{
				if (!TryParseScriptUsage(UsageText, TargetUsage))
				{
					OutError = TEXT("invalid_script_usage");
					return false;
				}
				bHasUsage = true;
			}

			FString UsageIdText;
			if (Params->TryGetStringField(TEXT("script_usage_id"), UsageIdText) && !UsageIdText.IsEmpty())
			{
				if (!FGuid::Parse(UsageIdText, TargetUsageId))
				{
					OutError = TEXT("invalid_script_usage_id");
					return false;
				}
				bHasUsageId = true;
			}
		}

		TArray<UNiagaraNodeOutput*> OutputNodes;
		GetOutputNodesFromGraph(Graph, OutputNodes);
		if (OutputNodes.Num() == 0)
		{
			OutError = TEXT("emitter_has_no_output_nodes");
			return false;
		}

		TArray<UNiagaraNodeOutput*> Matches;
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (!OutputNode)
			{
				continue;
			}
			if (bHasUsage && OutputNode->GetUsage() != TargetUsage)
			{
				continue;
			}
			if (bHasUsageId && OutputNode->GetUsageId() != TargetUsageId)
			{
				continue;
			}
			Matches.Add(OutputNode);
		}

		if (Matches.Num() == 0)
		{
			OutError = TEXT("stage_not_found");
			return false;
		}
		if (Matches.Num() > 1 && !bHasUsageId)
		{
			OutError = TEXT("stage_is_ambiguous_require_script_usage_id");
			return false;
		}

		OutStage.OutputNode = Matches[0];
		OutStage.Usage = Matches[0]->GetUsage();
		OutStage.UsageId = Matches[0]->GetUsageId();
		OutStage.StageKey = MakeStageKey(OutStage.Usage, OutStage.UsageId);
		return true;
	}

	static void GetModuleInputPins(UNiagaraNodeFunctionCall& ModuleNode, TArray<UEdGraphPin*>& OutInputPins)
	{
		OutInputPins.Reset();
		for (UEdGraphPin* Pin : ModuleNode.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->ParentPin != nullptr)
			{
				continue;
			}

			const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (PinType == FNiagaraTypeDefinition::GetParameterMapDef())
			{
				continue;
			}

			OutInputPins.Add(Pin);
		}
	}

	struct FResolvedNiagaraModuleInput
	{
		FNiagaraVariable InputVariable;
		FString FullInputName;
		FString ShortInputName;
		FGuid InputScriptVariableGuid;
		UEdGraphPin* VisiblePin = nullptr;
	};

	static FString MakeLowerTrimmed(const FString& InText)
	{
		FString OutText = InText.TrimStartAndEnd();
		OutText.ToLowerInline();
		return OutText;
	}

	static bool IsNiagaraParameterMapSetNode(const UEdGraphNode* Node)
	{
		for (const UClass* Class = Node ? Node->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetName().Equals(TEXT("NiagaraNodeParameterMapSet"), ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	static bool IsNiagaraParameterMapGetNode(const UEdGraphNode* Node)
	{
		for (const UClass* Class = Node ? Node->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetName().Equals(TEXT("NiagaraNodeParameterMapGet"), ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	static UEdGraphPin* FindOverridePinByAliasedInputName(UNiagaraGraph* Graph, const FName AliasedInputName)
	{
		if (!Graph || AliasedInputName.IsNone())
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				if (Pin->Direction != EGPD_Input)
				{
					continue;
				}
				if (!IsNiagaraParameterMapSetNode(Pin->GetOwningNode()))
				{
					continue;
				}
				if (Pin->PinName == AliasedInputName)
				{
					return Pin;
				}
			}
		}

		return nullptr;
	}

	static void BuildResolvedModuleInputs(UNiagaraNodeFunctionCall& ModuleNode, TArray<FResolvedNiagaraModuleInput>& OutInputs)
	{
		OutInputs.Reset();

		TMap<FString, int32> IndexByLowerFullName;

		TArray<UEdGraphPin*> VisiblePins;
		GetModuleInputPins(ModuleNode, VisiblePins);
		for (UEdGraphPin* InputPin : VisiblePins)
		{
			if (!InputPin)
			{
				continue;
			}

			FResolvedNiagaraModuleInput InputData;
			InputData.FullInputName = InputPin->PinName.ToString();
			InputData.ShortInputName = InputData.FullInputName;
			int32 DotIndex = INDEX_NONE;
			if (InputData.ShortInputName.FindLastChar(TEXT('.'), DotIndex))
			{
				InputData.ShortInputName = InputData.ShortInputName.Mid(DotIndex + 1);
			}
			InputData.InputVariable = UEdGraphSchema_Niagara::PinToNiagaraVariable(InputPin);
			if (!InputData.InputVariable.IsValid())
			{
				InputData.InputVariable = FNiagaraVariable(UEdGraphSchema_Niagara::PinToTypeDefinition(InputPin), InputPin->PinName);
			}
			InputData.VisiblePin = InputPin;

			const int32 NewIndex = OutInputs.Add(MoveTemp(InputData));
			IndexByLowerFullName.Add(MakeLowerTrimmed(OutInputs[NewIndex].FullInputName), NewIndex);
		}

		TArray<FNiagaraVariable> StackInputVariables;
		TSet<FNiagaraVariable> HiddenInputVariables;
		FCompileConstantResolver ConstantResolver;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(
			ModuleNode,
			StackInputVariables,
			HiddenInputVariables,
			ConstantResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
			false);

		for (const FNiagaraVariable& InputVariable : StackInputVariables)
		{
			if (!InputVariable.IsValid())
			{
				continue;
			}

			const FString FullInputName = InputVariable.GetName().ToString();
			const FString LowerFullInputName = MakeLowerTrimmed(FullInputName);
			int32* ExistingIndex = IndexByLowerFullName.Find(LowerFullInputName);
			if (ExistingIndex)
			{
				FResolvedNiagaraModuleInput& ExistingInput = OutInputs[*ExistingIndex];
				if (!ExistingInput.InputVariable.IsValid())
				{
					ExistingInput.InputVariable = InputVariable;
				}
				continue;
			}

			FResolvedNiagaraModuleInput InputData;
			InputData.InputVariable = InputVariable;
			InputData.FullInputName = FullInputName;
			InputData.ShortInputName = FullInputName;
			int32 DotIndex = INDEX_NONE;
			if (InputData.ShortInputName.FindLastChar(TEXT('.'), DotIndex))
			{
				InputData.ShortInputName = InputData.ShortInputName.Mid(DotIndex + 1);
			}

			const int32 NewIndex = OutInputs.Add(MoveTemp(InputData));
			IndexByLowerFullName.Add(LowerFullInputName, NewIndex);
		}
	}

	static const FResolvedNiagaraModuleInput* ResolveModuleInputByName(const TArray<FResolvedNiagaraModuleInput>& ModuleInputs, const FString& InputName, FString& OutResolvedInputName)
	{
		const FString InputNameNormalized = InputName.TrimStartAndEnd();
		if (InputNameNormalized.IsEmpty())
		{
			return nullptr;
		}

		for (const FResolvedNiagaraModuleInput& InputData : ModuleInputs)
		{
			if (InputData.FullInputName.Equals(InputNameNormalized, ESearchCase::IgnoreCase))
			{
				OutResolvedInputName = InputData.FullInputName;
				return &InputData;
			}
		}

		const FResolvedNiagaraModuleInput* ShortMatch = nullptr;
		for (const FResolvedNiagaraModuleInput& InputData : ModuleInputs)
		{
			if (!InputData.ShortInputName.Equals(InputNameNormalized, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (ShortMatch != nullptr)
			{
				return nullptr;
			}
			ShortMatch = &InputData;
		}

		if (ShortMatch)
		{
			OutResolvedInputName = ShortMatch->FullInputName;
		}
		return ShortMatch;
	}

	static bool InitializeDynamicInputStackInputs(
		UNiagaraEmitter* Emitter,
		const FGuid& VersionGuid,
		UNiagaraGraph* Graph,
		UNiagaraNodeFunctionCall& ModuleNode,
		UNiagaraNodeFunctionCall& DynamicInputNode,
		FString& OutInitializationSource)
	{
		OutInitializationSource.Reset();
		if (!Graph)
		{
			OutInitializationSource = TEXT("invalid_graph_context");
			return false;
		}

		TArray<FResolvedNiagaraModuleInput> DynamicInputInputs;
		BuildResolvedModuleInputs(DynamicInputNode, DynamicInputInputs);

		int32 DataInterfaceInputCount = 0;
		int32 InitializedInputCount = 0;
		TArray<FString> Errors;
		for (const FResolvedNiagaraModuleInput& DynamicInput : DynamicInputInputs)
		{
			if (!DynamicInput.InputVariable.IsValid())
			{
				continue;
			}

			const FNiagaraTypeDefinition InputType = DynamicInput.InputVariable.GetType();
			UClass* InputClass = InputType.GetClass();
			if (!InputType.IsDataInterface() || !InputClass || !InputClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
			{
				continue;
			}

			++DataInterfaceInputCount;
			const FNiagaraParameterHandle InputHandle(DynamicInput.InputVariable.GetName());
			const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, &DynamicInputNode);

			UEdGraphPin* DynamicInputParameterMapPin = FindParameterMapInputPin(DynamicInputNode);
			if (!DynamicInputParameterMapPin || DynamicInputParameterMapPin->LinkedTo.Num() != 1)
			{
				Errors.Add(FString::Printf(
					TEXT("%s:dynamic_input_parameter_map_input_link_count:%d"),
					*DynamicInput.FullInputName,
					DynamicInputParameterMapPin ? DynamicInputParameterMapPin->LinkedTo.Num() : 0));
				continue;
			}

			UEdGraphPin* OverridePin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				DynamicInputNode,
				AliasedInputHandle,
				InputType,
				DynamicInput.InputScriptVariableGuid,
				FGuid());
			if (!IsNiagaraParameterMapSetNode(OverridePin->GetOwningNode()))
			{
				Errors.Add(FString::Printf(TEXT("%s:override_pin_owner_not_parameter_map_set"), *DynamicInput.FullInputName));
				continue;
			}

			if (OverridePin->LinkedTo.Num() > 0)
			{
				++InitializedInputCount;
				continue;
			}

			const FString InputNodeInputName = FString::Printf(TEXT("%s.%s"), *DynamicInputNode.GetFunctionName(), *DynamicInput.ShortInputName);
			UNiagaraDataInterface* DataInterface = nullptr;
			FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(
				*OverridePin,
				InputClass,
				InputNodeInputName,
				DataInterface);
			if (DataInterface)
			{
				++InitializedInputCount;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("%s:data_interface_create_failed"), *DynamicInput.FullInputName));
			}
		}

		if (Graph)
		{
			Graph->NotifyGraphChanged();
		}

		OutInitializationSource = FString::Printf(
			TEXT("data_interface_inputs:%d;initialized:%d"),
			DataInterfaceInputCount,
			InitializedInputCount);
		if (Errors.Num() > 0)
		{
			OutInitializationSource += FString::Printf(TEXT(";errors:%s"), *FString::Join(Errors, TEXT("|")));
		}
		return DataInterfaceInputCount == InitializedInputCount;
	}

	static UNiagaraNodeFunctionCall* FindLinkedDynamicInputNode(const UEdGraphPin* OverridePin)
	{
		if (!OverridePin)
		{
			return nullptr;
		}

		for (UEdGraphPin* LinkedPin : OverridePin->LinkedTo)
		{
			if (!LinkedPin)
			{
				continue;
			}
			if (UNiagaraNodeFunctionCall* LinkedFunctionCall = Cast<UNiagaraNodeFunctionCall>(LinkedPin->GetOwningNode()))
			{
				return LinkedFunctionCall;
			}
		}
		return nullptr;
	}

	static bool TryResolveLinkedInputParameterName(const UEdGraphPin* OverridePin, FString& OutParameterName)
	{
		OutParameterName.Reset();
		if (!OverridePin)
		{
			return false;
		}

		for (UEdGraphPin* LinkedPin : OverridePin->LinkedTo)
		{
			if (!LinkedPin)
			{
				continue;
			}
			if (UNiagaraNodeInput* LinkedInput = Cast<UNiagaraNodeInput>(LinkedPin->GetOwningNode()))
			{
				OutParameterName = LinkedInput->Input.GetName().ToString();
				return !OutParameterName.IsEmpty();
			}
			if (IsNiagaraParameterMapGetNode(LinkedPin->GetOwningNode()))
			{
				const FNiagaraVariable LinkedVariable = UEdGraphSchema_Niagara::PinToNiagaraVariable(LinkedPin);
				OutParameterName = LinkedVariable.IsValid()
					? LinkedVariable.GetName().ToString()
					: LinkedPin->PinName.ToString();
				return !OutParameterName.IsEmpty();
			}
		}
		return false;
	}

	static bool ResolveDataInterfaceFromInputNode(UNiagaraNodeInput* InputNode, UNiagaraDataInterface*& OutDataInterface)
	{
		OutDataInterface = nullptr;
		if (!InputNode)
		{
			return false;
		}

		FProperty* DataInterfaceProperty = nullptr;
		void* DataInterfaceValuePtr = nullptr;
		if (!ResolvePropertyPath(InputNode, TEXT("DataInterface"), DataInterfaceProperty, DataInterfaceValuePtr))
		{
			return false;
		}

		FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(DataInterfaceProperty);
		OutDataInterface = ObjectProperty
			? Cast<UNiagaraDataInterface>(ObjectProperty->GetObjectPropertyValue(DataInterfaceValuePtr))
			: nullptr;
		return OutDataInterface != nullptr;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildObjectPropertiesJsonForUObject(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		if (!Object)
		{
			return PropertiesJson;
		}

		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildPropertyValueJson(*Object, *Property, ValuePtr)));
		}
		return PropertiesJson;
	}

	static TSharedPtr<FJsonObject> BuildDataInterfaceNodeJson(UNiagaraNodeInput& InputNode)
	{
		UNiagaraDataInterface* DataInterface = nullptr;
		if (!ResolveDataInterfaceFromInputNode(&InputNode, DataInterface) || !DataInterface)
		{
			return nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> RawProperties = BuildObjectPropertiesJsonForUObject(DataInterface);
		TSharedPtr<FJsonObject> DataInterfaceObj = MakeShared<FJsonObject>();
		DataInterfaceObj->SetStringField(TEXT("node_guid"), InputNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		DataInterfaceObj->SetStringField(TEXT("input_name"), InputNode.Input.GetName().ToString());
		DataInterfaceObj->SetStringField(TEXT("input_type"), InputNode.Input.GetType().GetNameText().ToString());
		DataInterfaceObj->SetStringField(TEXT("data_interface_class"), DataInterface->GetClass()->GetPathName());
		DataInterfaceObj->SetStringField(TEXT("data_interface_object_path"), DataInterface->GetPathName());
		DataInterfaceObj->SetArrayField(TEXT("raw_properties"), RawProperties);
		DataInterfaceObj->SetNumberField(TEXT("raw_properties_count"), RawProperties.Num());
		return DataInterfaceObj;
	}

	static void CollectLinkedDataInterfaceInputNodesForDynamicInput(UNiagaraNodeFunctionCall& DynamicInputNode, TArray<UNiagaraNodeInput*>& OutInputNodes)
	{
		OutInputNodes.Reset();
		TSet<UNiagaraNodeInput*> SeenInputNodes;

		auto AddInputNodeIfDataInterface = [&SeenInputNodes, &OutInputNodes](UNiagaraNodeInput* InputNode)
		{
			UNiagaraDataInterface* DataInterface = nullptr;
			if (!InputNode || SeenInputNodes.Contains(InputNode) || !ResolveDataInterfaceFromInputNode(InputNode, DataInterface))
			{
				return;
			}
			SeenInputNodes.Add(InputNode);
			OutInputNodes.Add(InputNode);
		};

		for (UEdGraphPin* Pin : DynamicInputNode.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin))
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UNiagaraNodeInput* InputNode = LinkedPin ? Cast<UNiagaraNodeInput>(LinkedPin->GetOwningNode()) : nullptr;
				AddInputNodeIfDataInterface(InputNode);
			}
		}

		UNiagaraGraph* Graph = Cast<UNiagaraGraph>(DynamicInputNode.GetGraph());
		const FString DynamicInputFunctionName = DynamicInputNode.GetFunctionName();
		if (!Graph || DynamicInputFunctionName.IsEmpty())
		{
			return;
		}

		const FString DynamicInputPrefix = DynamicInputFunctionName + TEXT(".");
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
			if (!InputNode)
			{
				continue;
			}

			const FString InputName = InputNode->Input.GetName().ToString();
			if (InputName.StartsWith(DynamicInputPrefix, ESearchCase::CaseSensitive))
			{
				AddInputNodeIfDataInterface(InputNode);
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildModuleInputJson(UNiagaraNodeFunctionCall& ModuleNode, const FResolvedNiagaraModuleInput& InputData, const int32 RecursionDepth = 0)
	{
		TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
		const FNiagaraTypeDefinition InputType = InputData.InputVariable.GetType();
		bool bHasLinks = false;
		bool bHasDefaultOverride = false;
		FString OverrideDefaultValue;
		FString AutogeneratedDefaultValue;
		UEdGraphPin* EffectiveInputPin = nullptr;

		if (InputData.VisiblePin)
		{
			EffectiveInputPin = InputData.VisiblePin;
			bHasLinks = InputData.VisiblePin->LinkedTo.Num() > 0;
			bHasDefaultOverride = InputData.VisiblePin->DefaultValue != InputData.VisiblePin->AutogeneratedDefaultValue;
			OverrideDefaultValue = InputData.VisiblePin->DefaultValue;
			AutogeneratedDefaultValue = InputData.VisiblePin->AutogeneratedDefaultValue;
		}
		else if (InputData.InputVariable.IsValid())
		{
			const FNiagaraParameterHandle InputHandle(InputData.InputVariable.GetName());
			const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, &ModuleNode);
			UNiagaraGraph* Graph = Cast<UNiagaraGraph>(ModuleNode.GetGraph());
			if (UEdGraphPin* OverridePin = FindOverridePinByAliasedInputName(Graph, AliasedHandle.GetParameterHandleString()))
			{
				EffectiveInputPin = OverridePin;
				bHasLinks = OverridePin->LinkedTo.Num() > 0;
				bHasDefaultOverride = !OverridePin->DefaultValue.IsEmpty() ||
					OverridePin->DefaultValue != OverridePin->AutogeneratedDefaultValue;
				OverrideDefaultValue = OverridePin->DefaultValue;
				AutogeneratedDefaultValue = OverridePin->AutogeneratedDefaultValue;
			}
		}

		InputObj->SetStringField(TEXT("input_name"), InputData.FullInputName);
		InputObj->SetStringField(TEXT("input_short_name"), InputData.ShortInputName);
		InputObj->SetStringField(TEXT("input_type"), InputType.GetNameText().ToString());
		InputObj->SetObjectField(TEXT("input_type_descriptor"), BuildNiagaraInputTypeDescriptorJson(InputType.GetNameText().ToString(), InputType));
		InputObj->SetBoolField(TEXT("has_override"), bHasLinks || bHasDefaultOverride);
		InputObj->SetBoolField(TEXT("has_links"), bHasLinks);
		InputObj->SetStringField(TEXT("override_default_value"), OverrideDefaultValue);
		InputObj->SetStringField(TEXT("autogenerated_default_value"), AutogeneratedDefaultValue);
		InputObj->SetBoolField(TEXT("has_visible_pin"), InputData.VisiblePin != nullptr);
		if (InputType.IsEnum())
		{
			if (UEnum* EnumType = InputType.GetEnum())
			{
				InputObj->SetStringField(TEXT("enum_type"), EnumType->GetPathName());

				auto AppendEnumValueFields = [EnumType](const TSharedPtr<FJsonObject>& TargetObj, const FString& Prefix, const FString& ValueText) -> bool
				{
					int64 EnumValue = 0;
					if (ValueText.TrimStartAndEnd().IsEmpty() || !TryResolveNiagaraEnumValueFromText(*EnumType, ValueText, EnumValue))
					{
						return false;
					}

					TargetObj->SetStringField(Prefix + TEXT("enum_value_name"), EnumType->GetNameStringByValue(EnumValue));
					TargetObj->SetStringField(Prefix + TEXT("enum_value_display_name"), EnumType->GetDisplayNameTextByValue(EnumValue).ToString());
					TargetObj->SetNumberField(Prefix + TEXT("enum_value_int"), static_cast<double>(EnumValue));
					return true;
				};

				const bool bWroteOverrideEnumFields = AppendEnumValueFields(InputObj, TEXT("override_"), OverrideDefaultValue);
				AppendEnumValueFields(InputObj, TEXT("autogenerated_"), AutogeneratedDefaultValue);
				if (bWroteOverrideEnumFields)
				{
					int64 EnumValue = 0;
					if (TryResolveNiagaraEnumValueFromText(*EnumType, OverrideDefaultValue, EnumValue))
					{
						InputObj->SetStringField(TEXT("enum_value_name"), EnumType->GetNameStringByValue(EnumValue));
						InputObj->SetStringField(TEXT("enum_value_display_name"), EnumType->GetDisplayNameTextByValue(EnumValue).ToString());
						InputObj->SetNumberField(TEXT("enum_value_int"), static_cast<double>(EnumValue));
					}
				}
				else
				{
					int64 EnumValue = 0;
					if (TryResolveNiagaraEnumValueFromText(*EnumType, AutogeneratedDefaultValue, EnumValue))
					{
						InputObj->SetStringField(TEXT("enum_value_name"), EnumType->GetNameStringByValue(EnumValue));
						InputObj->SetStringField(TEXT("enum_value_display_name"), EnumType->GetDisplayNameTextByValue(EnumValue).ToString());
						InputObj->SetNumberField(TEXT("enum_value_int"), static_cast<double>(EnumValue));
					}
				}

				TArray<TSharedPtr<FJsonValue>> OptionsJson;
				for (int32 EnumIndex = 0; EnumIndex < EnumType->NumEnums(); ++EnumIndex)
				{
					const int64 EnumValue = EnumType->GetValueByIndex(EnumIndex);
					if (!IsUserSettableNiagaraEnumValue(*EnumType, EnumValue))
					{
						continue;
					}

					TSharedPtr<FJsonObject> OptionObj = MakeShared<FJsonObject>();
					OptionObj->SetNumberField(TEXT("index"), EnumIndex);
					OptionObj->SetNumberField(TEXT("value"), static_cast<double>(EnumValue));
					OptionObj->SetStringField(TEXT("name"), EnumType->GetNameStringByIndex(EnumIndex));
					OptionObj->SetStringField(TEXT("display_name"), EnumType->GetDisplayNameTextByIndex(EnumIndex).ToString());
					OptionsJson.Add(MakeShared<FJsonValueObject>(OptionObj));
				}
				InputObj->SetArrayField(TEXT("candidates"), OptionsJson);
			}
		}

		if (bHasLinks && EffectiveInputPin && RecursionDepth < 4)
		{
			if (UNiagaraNodeFunctionCall* DynamicInputNode = FindLinkedDynamicInputNode(EffectiveInputPin))
			{
				TSharedPtr<FJsonObject> DynamicInputObj = MakeShared<FJsonObject>();
				DynamicInputObj->SetStringField(TEXT("node_guid"), DynamicInputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
				DynamicInputObj->SetStringField(TEXT("node_name"), DynamicInputNode->GetFunctionName());
				DynamicInputObj->SetStringField(TEXT("script_asset_path"), GetModuleScriptPath(*DynamicInputNode));

				TArray<FResolvedNiagaraModuleInput> DynamicInputs;
				BuildResolvedModuleInputs(*DynamicInputNode, DynamicInputs);
				TArray<TSharedPtr<FJsonValue>> DynamicInputsJson;
				DynamicInputsJson.Reserve(DynamicInputs.Num());
				for (const FResolvedNiagaraModuleInput& DynamicInput : DynamicInputs)
				{
					DynamicInputsJson.Add(MakeShared<FJsonValueObject>(BuildModuleInputJson(*DynamicInputNode, DynamicInput, RecursionDepth + 1)));
				}
				DynamicInputObj->SetArrayField(TEXT("inputs"), DynamicInputsJson);
				DynamicInputObj->SetNumberField(TEXT("input_count"), DynamicInputsJson.Num());

				TArray<UNiagaraNodeInput*> DataInterfaceInputNodes;
				CollectLinkedDataInterfaceInputNodesForDynamicInput(*DynamicInputNode, DataInterfaceInputNodes);
				TArray<TSharedPtr<FJsonValue>> DataInterfacesJson;
				DataInterfacesJson.Reserve(DataInterfaceInputNodes.Num());
				for (UNiagaraNodeInput* DataInterfaceInputNode : DataInterfaceInputNodes)
				{
					TSharedPtr<FJsonObject> DataInterfaceObj = DataInterfaceInputNode ? BuildDataInterfaceNodeJson(*DataInterfaceInputNode) : nullptr;
					if (DataInterfaceObj.IsValid())
					{
						DataInterfacesJson.Add(MakeShared<FJsonValueObject>(DataInterfaceObj));
					}
				}
				DynamicInputObj->SetArrayField(TEXT("data_interfaces"), DataInterfacesJson);
				DynamicInputObj->SetNumberField(TEXT("data_interface_count"), DataInterfacesJson.Num());

				InputObj->SetStringField(TEXT("link_kind"), TEXT("dynamic_input"));
				InputObj->SetObjectField(TEXT("dynamic_input"), DynamicInputObj);
			}
			else
			{
				UNiagaraNodeInput* LinkedInputNode = EffectiveInputPin->LinkedTo.Num() == 1
					? Cast<UNiagaraNodeInput>(EffectiveInputPin->LinkedTo[0]->GetOwningNode())
					: nullptr;
				TSharedPtr<FJsonObject> DataInterfaceObj = LinkedInputNode ? BuildDataInterfaceNodeJson(*LinkedInputNode) : nullptr;
				if (DataInterfaceObj.IsValid())
				{
					InputObj->SetStringField(TEXT("link_kind"), TEXT("data_interface"));
					InputObj->SetObjectField(TEXT("data_interface"), DataInterfaceObj);
				}
				else
				{
					FString LinkedParameterName;
					if (TryResolveLinkedInputParameterName(EffectiveInputPin, LinkedParameterName))
					{
						InputObj->SetStringField(TEXT("link_kind"), TEXT("linked_input"));
						InputObj->SetStringField(TEXT("linked_parameter_name"), LinkedParameterName);
					}
				}
			}
		}
		return InputObj;
	}

	static TSharedPtr<FJsonObject> BuildModuleNodeJson(UNiagaraNodeFunctionCall& ModuleNode, const int32 ModuleIndex, const bool bIncludeInputs)
	{
		TSharedPtr<FJsonObject> ModuleObj = MakeShared<FJsonObject>();
		ModuleObj->SetNumberField(TEXT("module_index"), ModuleIndex);
		ModuleObj->SetStringField(TEXT("module_node_guid"), ModuleNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		ModuleObj->SetStringField(TEXT("module_name"), ModuleNode.GetFunctionName());
		ModuleObj->SetStringField(TEXT("module_script_path"), GetModuleScriptPath(ModuleNode));
		ModuleObj->SetStringField(TEXT("module_kind"), Cast<UNiagaraNodeAssignment>(&ModuleNode) ? TEXT("assignment") : TEXT("script_module"));
		ModuleObj->SetBoolField(TEXT("module_has_enabled_state"), true);
		ModuleObj->SetBoolField(TEXT("module_enabled"), ModuleNode.IsNodeEnabled());

		if (bIncludeInputs)
		{
			TArray<FResolvedNiagaraModuleInput> Inputs;
			BuildResolvedModuleInputs(ModuleNode, Inputs);
			TArray<TSharedPtr<FJsonValue>> InputsJson;
			InputsJson.Reserve(Inputs.Num());
			for (const FResolvedNiagaraModuleInput& InputData : Inputs)
			{
				InputsJson.Add(MakeShared<FJsonValueObject>(BuildModuleInputJson(ModuleNode, InputData)));
			}
			ModuleObj->SetArrayField(TEXT("inputs"), InputsJson);
			ModuleObj->SetNumberField(TEXT("input_count"), InputsJson.Num());
		}

		return ModuleObj;
	}

	static TSharedPtr<FJsonObject> BuildStageNodeJson(UEdGraphNode& StageNode, const int32 StageNodeIndex, const bool bIncludeProperties, const bool bIncludeModuleInputs)
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetNumberField(TEXT("stage_node_index"), StageNodeIndex);
		NodeObj->SetStringField(TEXT("node_guid"), StageNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		NodeObj->SetStringField(TEXT("node_class"), StageNode.GetClass()->GetPathName());
		NodeObj->SetStringField(TEXT("node_title"), StageNode.GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeObj->SetNumberField(TEXT("node_pos_x"), StageNode.NodePosX);
		NodeObj->SetNumberField(TEXT("node_pos_y"), StageNode.NodePosY);
		NodeObj->SetBoolField(TEXT("is_output_node"), Cast<UNiagaraNodeOutput>(&StageNode) != nullptr);
		NodeObj->SetBoolField(TEXT("is_module_node"), Cast<UNiagaraNodeFunctionCall>(&StageNode) != nullptr);

		if (UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(&StageNode))
		{
			NodeObj->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
			NodeObj->SetStringField(TEXT("module_script_path"), GetModuleScriptPath(*ModuleNode));
			NodeObj->SetStringField(TEXT("module_kind"), Cast<UNiagaraNodeAssignment>(ModuleNode) ? TEXT("assignment") : TEXT("script_module"));
			NodeObj->SetBoolField(TEXT("module_has_enabled_state"), true);
			NodeObj->SetBoolField(TEXT("module_enabled"), ModuleNode->IsNodeEnabled());

			if (bIncludeModuleInputs)
			{
				TArray<FResolvedNiagaraModuleInput> Inputs;
				BuildResolvedModuleInputs(*ModuleNode, Inputs);

				TArray<TSharedPtr<FJsonValue>> InputsJson;
				InputsJson.Reserve(Inputs.Num());
				for (const FResolvedNiagaraModuleInput& InputData : Inputs)
				{
					InputsJson.Add(MakeShared<FJsonValueObject>(BuildModuleInputJson(*ModuleNode, InputData)));
				}
				NodeObj->SetArrayField(TEXT("inputs"), InputsJson);
				NodeObj->SetNumberField(TEXT("input_count"), InputsJson.Num());
			}
		}

		if (bIncludeProperties)
		{
			TArray<TSharedPtr<FJsonValue>> PropertiesJson;
			for (TFieldIterator<FProperty> It(StageNode.GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property)
				{
					continue;
				}

				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(&StageNode);
				PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildPropertyValueJson(StageNode, *Property, ValuePtr)));
			}
			NodeObj->SetArrayField(TEXT("properties"), PropertiesJson);
			NodeObj->SetNumberField(TEXT("property_count"), PropertiesJson.Num());
		}

		return NodeObj;
	}

	static TSharedPtr<FJsonObject> BuildRendererJson(UNiagaraRendererProperties& Renderer, const int32 RendererIndex)
	{
		TSharedPtr<FJsonObject> RendererObj = MakeShared<FJsonObject>();
		RendererObj->SetNumberField(TEXT("renderer_index"), RendererIndex);
		RendererObj->SetStringField(TEXT("renderer_name"), Renderer.GetFName().ToString());
		RendererObj->SetStringField(TEXT("renderer_class"), Renderer.GetClass()->GetPathName());
		RendererObj->SetStringField(TEXT("renderer_object_path"), Renderer.GetPathName());
		return RendererObj;
	}

	static UClass* ResolveRendererClass(const FString& RendererClassOrAlias)
	{
		FString ClassText = RendererClassOrAlias.TrimStartAndEnd();
		if (ClassText.IsEmpty())
		{
			return nullptr;
		}

		const FString AliasLower = ClassText.ToLower();
		if (AliasLower == TEXT("sprite"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraSpriteRendererProperties");
		}
		else if (AliasLower == TEXT("ribbon"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraRibbonRendererProperties");
		}
		else if (AliasLower == TEXT("mesh"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraMeshRendererProperties");
		}
		else if (AliasLower == TEXT("light"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraLightRendererProperties");
		}
		else if (AliasLower == TEXT("decal"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraDecalRendererProperties");
		}

		UClass* RendererClass = ResolveClassByPath(ClassText);
		if (!RendererClass || !RendererClass->IsChildOf(UNiagaraRendererProperties::StaticClass()) || RendererClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return nullptr;
		}

		return RendererClass;
	}

	static bool ResolveRendererFromParams(FNiagaraEmitterEditContext& EditContext, const TSharedPtr<FJsonObject>& Params, int32& OutRendererIndex, UNiagaraRendererProperties*& OutRenderer, FString& OutError)
	{
		OutRendererIndex = INDEX_NONE;
		OutRenderer = nullptr;

		if (!EditContext.EmitterData)
		{
			OutError = TEXT("missing_emitter_version_data");
			return false;
		}

		const TArray<UNiagaraRendererProperties*>& Renderers = EditContext.EmitterData->GetRenderers();
		if (Renderers.Num() == 0)
		{
			OutError = TEXT("emitter_has_no_renderers");
			return false;
		}

		double RendererIndexNumber = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("renderer_index"), RendererIndexNumber))
		{
			const int32 Index = FMath::RoundToInt(RendererIndexNumber);
			if (!Renderers.IsValidIndex(Index))
			{
				OutError = TEXT("invalid_renderer_index");
				return false;
			}

			OutRendererIndex = Index;
			OutRenderer = Renderers[Index];
			return OutRenderer != nullptr;
		}

		FString RendererName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("renderer_name"), RendererName) && !RendererName.IsEmpty())
		{
			for (int32 Index = 0; Index < Renderers.Num(); ++Index)
			{
				UNiagaraRendererProperties* Renderer = Renderers[Index];
				if (Renderer && Renderer->GetFName().ToString().Equals(RendererName, ESearchCase::IgnoreCase))
				{
					OutRendererIndex = Index;
					OutRenderer = Renderer;
					return true;
				}
			}
		}

		FString RendererClass;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("renderer_class"), RendererClass) && !RendererClass.IsEmpty())
		{
			UClass* ResolvedClass = ResolveRendererClass(RendererClass);
			for (int32 Index = 0; Index < Renderers.Num(); ++Index)
			{
				UNiagaraRendererProperties* Renderer = Renderers[Index];
				if (!Renderer)
				{
					continue;
				}
				if (ResolvedClass)
				{
					if (Renderer->GetClass() == ResolvedClass)
					{
						OutRendererIndex = Index;
						OutRenderer = Renderer;
						return true;
					}
				}
				else if (Renderer->GetClass()->GetPathName().Equals(RendererClass, ESearchCase::IgnoreCase))
				{
					OutRendererIndex = Index;
					OutRenderer = Renderer;
					return true;
				}
			}
		}

		OutError = TEXT("renderer_not_found");
		return false;
	}

	static FString ExecutionModeToString(const EScriptExecutionMode ExecutionMode)
	{
		if (const UEnum* Enum = StaticEnum<EScriptExecutionMode>())
		{
			return Enum->GetNameStringByValue((int64)ExecutionMode);
		}
		return TEXT("");
	}

	static bool ParseExecutionMode(const FString& ExecutionModeText, EScriptExecutionMode& OutExecutionMode)
	{
		const FString Input = ExecutionModeText.TrimStartAndEnd();
		if (Input.IsEmpty())
		{
			return false;
		}

		if (const UEnum* Enum = StaticEnum<EScriptExecutionMode>())
		{
			int64 Value = Enum->GetValueByNameString(Input, EGetByNameFlags::CheckAuthoredName);
			if (Value == INDEX_NONE)
			{
				const FString QualifiedName = FString::Printf(TEXT("EScriptExecutionMode::%s"), *Input);
				Value = Enum->GetValueByNameString(QualifiedName, EGetByNameFlags::CheckAuthoredName);
			}
			if (Value == INDEX_NONE)
			{
				Value = Enum->GetValueByNameString(Input, EGetByNameFlags::None);
			}
			if (Value != INDEX_NONE)
			{
				OutExecutionMode = static_cast<EScriptExecutionMode>(Value);
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildEventHandlerJson(FNiagaraEventScriptProperties& EventHandler, const int32 EventIndex)
	{
		TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
		const FGuid UsageId = EventHandler.Script ? EventHandler.Script->GetUsageId() : FGuid();
		EventObj->SetNumberField(TEXT("event_index"), EventIndex);
		EventObj->SetStringField(TEXT("script_usage_id"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		EventObj->SetStringField(TEXT("stage_key"), MakeStageKey(ENiagaraScriptUsage::ParticleEventScript, UsageId));
		EventObj->SetStringField(TEXT("execution_mode"), ExecutionModeToString(EventHandler.ExecutionMode));
		EventObj->SetNumberField(TEXT("spawn_number"), EventHandler.SpawnNumber);
		EventObj->SetNumberField(TEXT("min_spawn_number"), EventHandler.MinSpawnNumber);
		EventObj->SetBoolField(TEXT("random_spawn_number"), EventHandler.bRandomSpawnNumber);
		EventObj->SetNumberField(TEXT("max_events_per_frame"), EventHandler.MaxEventsPerFrame);
		EventObj->SetBoolField(TEXT("update_attribute_initial_values"), EventHandler.UpdateAttributeInitialValues);
		EventObj->SetStringField(TEXT("source_event_name"), EventHandler.SourceEventName.ToString());
		EventObj->SetStringField(TEXT("source_emitter_id"), EventHandler.SourceEmitterID.ToString(EGuidFormats::DigitsWithHyphensLower));
		if (EventHandler.Script)
		{
			EventObj->SetStringField(TEXT("script_object_path"), EventHandler.Script->GetPathName());
		}
		return EventObj;
	}

	static bool ResolveEventHandlerFromParams(FNiagaraEmitterEditContext& EditContext, const TSharedPtr<FJsonObject>& Params, int32& OutEventIndex, FNiagaraEventScriptProperties*& OutEventHandler, FString& OutError)
	{
		OutEventIndex = INDEX_NONE;
		OutEventHandler = nullptr;

		if (!EditContext.EmitterData)
		{
			OutError = TEXT("missing_emitter_version_data");
			return false;
		}

		TArray<FNiagaraEventScriptProperties>& EventHandlers = EditContext.EmitterData->EventHandlerScriptProps;
		if (EventHandlers.Num() == 0)
		{
			OutError = TEXT("emitter_has_no_event_handlers");
			return false;
		}

		double EventIndexNumber = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("event_index"), EventIndexNumber))
		{
			const int32 Index = FMath::RoundToInt(EventIndexNumber);
			if (!EventHandlers.IsValidIndex(Index))
			{
				OutError = TEXT("invalid_event_index");
				return false;
			}

			OutEventIndex = Index;
			OutEventHandler = &EventHandlers[Index];
			return true;
		}

		FString ScriptUsageIdText;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("script_usage_id"), ScriptUsageIdText) && !ScriptUsageIdText.IsEmpty())
		{
			FGuid ScriptUsageId;
			if (!FGuid::Parse(ScriptUsageIdText, ScriptUsageId))
			{
				OutError = TEXT("invalid_script_usage_id");
				return false;
			}

			for (int32 Index = 0; Index < EventHandlers.Num(); ++Index)
			{
				FNiagaraEventScriptProperties& EventHandler = EventHandlers[Index];
				if (EventHandler.Script && EventHandler.Script->GetUsageId() == ScriptUsageId)
				{
					OutEventIndex = Index;
					OutEventHandler = &EventHandler;
					return true;
				}
			}

			OutError = TEXT("event_handler_not_found");
			return false;
		}

		FString SourceEventName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("source_event_name"), SourceEventName) && !SourceEventName.IsEmpty())
		{
			for (int32 Index = 0; Index < EventHandlers.Num(); ++Index)
			{
				FNiagaraEventScriptProperties& EventHandler = EventHandlers[Index];
				if (EventHandler.SourceEventName.ToString().Equals(SourceEventName, ESearchCase::IgnoreCase))
				{
					OutEventIndex = Index;
					OutEventHandler = &EventHandler;
					return true;
				}
			}
		}

		OutError = TEXT("event_handler_not_found");
		return false;
	}

	static UEdGraphNode* FindGraphNodeByGuid(UNiagaraGraph& Graph, const FGuid& NodeGuid)
	{
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	static bool EnsureGraphNodeGuid(UEdGraphNode* Node, TSet<FGuid>* ExistingGuids)
	{
		if (!Node)
		{
			return false;
		}

		if (Node->NodeGuid.IsValid() && (!ExistingGuids || !ExistingGuids->Contains(Node->NodeGuid)))
		{
			if (ExistingGuids)
			{
				ExistingGuids->Add(Node->NodeGuid);
			}
			return false;
		}

		Node->Modify();
		for (int32 Attempt = 0; Attempt < 16; ++Attempt)
		{
			Node->CreateNewGuid();
			if (Node->NodeGuid.IsValid() && (!ExistingGuids || !ExistingGuids->Contains(Node->NodeGuid)))
			{
				if (ExistingGuids)
				{
					ExistingGuids->Add(Node->NodeGuid);
				}
				return true;
			}
		}
		return false;
	}

	static int32 NormalizeGraphNodeGuids(UEdGraph* Graph)
	{
		if (!Graph)
		{
			return 0;
		}

		TSet<FGuid> ExistingGuids;
		int32 FixedCount = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (EnsureGraphNodeGuid(Node, &ExistingGuids))
			{
				++FixedCount;
			}
		}
		return FixedCount;
	}

	static UEdGraphPin* ResolvePinByName(UEdGraphNode& Node, const FString& PinName)
	{
		const FString Wanted = PinName.TrimStartAndEnd();
		if (Wanted.IsEmpty())
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (!Pin)
			{
				continue;
			}
			if (Pin->PinName.ToString().Equals(Wanted, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static bool ResolveStageNodeFromParams(UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& Params, FResolvedNiagaraStage& OutStage, UEdGraphNode*& OutStageNode, FString& OutError)
	{
		OutStageNode = nullptr;

		if (!ResolveStageFromParams(Graph, Params, OutStage, OutError))
		{
			return false;
		}

		FString NodeGuidText;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("node_guid"), NodeGuidText) || NodeGuidText.IsEmpty())
		{
			OutError = TEXT("missing_node_guid");
			return false;
		}

		FGuid NodeGuid;
		if (!FGuid::Parse(NodeGuidText, NodeGuid))
		{
			OutError = TEXT("invalid_node_guid");
			return false;
		}

		TSet<UEdGraphNode*> VisitedNodes;
		TArray<UEdGraphNode*> StageNodes;
		GatherUpstreamNodesRecursive(OutStage.OutputNode, VisitedNodes, StageNodes);

		for (UEdGraphNode* StageNode : StageNodes)
		{
			if (StageNode && StageNode->NodeGuid == NodeGuid)
			{
				OutStageNode = StageNode;
				break;
			}
		}

		if (!OutStageNode)
		{
			OutError = TEXT("node_not_found_in_stage");
			return false;
		}

		return true;
	}

	static bool FindOwningOutputNodeForModule(UNiagaraGraph& Graph, UNiagaraNodeFunctionCall& ModuleNode, UNiagaraNodeOutput*& OutOutputNode)
	{
		OutOutputNode = nullptr;
		TArray<UNiagaraNodeOutput*> OutputNodes;
		GetOutputNodesFromGraph(Graph, OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (!OutputNode)
			{
				continue;
			}

			TArray<UNiagaraNodeFunctionCall*> Modules;
			GetStageModuleNodes(*OutputNode, Modules);
			for (UNiagaraNodeFunctionCall* CandidateModule : Modules)
			{
				if (CandidateModule == &ModuleNode)
				{
					OutOutputNode = OutputNode;
					return true;
				}
			}
		}
		return false;
	}

	static UClass* ResolveClassByPath(const FString& InClassPath)
	{
		FString ClassPath = InClassPath;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* Existing = FindObject<UClass>(nullptr, *ClassPath))
		{
			return Existing;
		}
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return Loaded;
		}
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			const FString GeneratedClassPath = ClassPath + TEXT("_C");
			if (UClass* GeneratedClass = LoadObject<UClass>(nullptr, *GeneratedClassPath))
			{
				return GeneratedClass;
			}
		}
		return nullptr;
	}

	static void DestroyGraphNodesForStage(UNiagaraGraph& Graph, const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		TArray<UNiagaraNodeOutput*> OutputNodes;
		GetOutputNodesFromGraph(Graph, OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (!OutputNode || OutputNode->GetUsage() != Usage || OutputNode->GetUsageId() != UsageId)
			{
				continue;
			}

			TSet<UEdGraphNode*> VisitedNodes;
			TArray<UEdGraphNode*> StageNodes;
			GatherUpstreamNodesRecursive(OutputNode, VisitedNodes, StageNodes);
			for (UEdGraphNode* StageNode : StageNodes)
			{
				if (!StageNode)
				{
					continue;
				}
				StageNode->Modify();
				StageNode->DestroyNode();
			}
			return;
		}
	}

	static UNiagaraScript* ResolveOwningScriptForStage(FVersionedNiagaraEmitterData& EmitterData, const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		if (Usage == ENiagaraScriptUsage::EmitterSpawnScript)
		{
			return EmitterData.EmitterSpawnScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::EmitterUpdateScript)
		{
			return EmitterData.EmitterUpdateScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::ParticleSpawnScript)
		{
			return EmitterData.SpawnScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::ParticleUpdateScript)
		{
			return EmitterData.UpdateScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			for (FNiagaraEventScriptProperties& EventHandler : EmitterData.EventHandlerScriptProps)
			{
				if (EventHandler.Script && EventHandler.Script->GetUsageId() == UsageId)
				{
					return EventHandler.Script;
				}
			}
			return nullptr;
		}
		if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			if (UNiagaraSimulationStageBase* SimulationStage = EmitterData.GetSimulationStageById(UsageId))
			{
				return SimulationStage->Script;
			}
			return nullptr;
		}
		return nullptr;
	}

	static FNiagaraSystemRefreshResult MarkEmitterGraphEdited(FNiagaraEmitterEditContext& Context)
	{
		FNiagaraSystemRefreshResult RefreshResult;
		if (Context.Graph)
		{
			Context.Graph->NotifyGraphChanged();
		}
		if (Context.Emitter)
		{
			Context.Emitter->MarkPackageDirty();

			if (UNiagaraSystem* OwningSystem = Context.Emitter->GetTypedOuter<UNiagaraSystem>())
			{
				RefreshResult = RefreshNiagaraSystemAfterStructuralEdit(*OwningSystem, true, true);
				FUeAgentInterfaceLogger::Log(
					TEXT("Niagara MarkEmitterGraphEdited: refreshed system=%s emitter=%s"),
					*OwningSystem->GetPathName(),
					*Context.Emitter->GetPathName());
			}
		}
		return RefreshResult;
	}

	static FNiagaraSystemRefreshResult MarkSystemGraphEdited(FNiagaraSystemEditContext& Context)
	{
		FNiagaraSystemRefreshResult RefreshResult;
		if (Context.Graph)
		{
			Context.Graph->NotifyGraphChanged();
		}
		if (Context.System)
		{
			Context.System->MarkPackageDirty();
			RefreshResult = RefreshNiagaraSystemAfterStructuralEdit(*Context.System, true, true);
			FUeAgentInterfaceLogger::Log(
				TEXT("Niagara MarkSystemGraphEdited: refreshed system=%s"),
				*Context.System->GetPathName());
		}
		return RefreshResult;
	}

	static int32 RepairSystemInvalidEmitterVersions(UNiagaraSystem& NiagaraSystem)
	{
		int32 RepairedCount = 0;
		TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem.GetEmitterHandles();
		for (FNiagaraEmitterHandle& Handle : Handles)
		{
			FVersionedNiagaraEmitter Instance = Handle.GetInstance();
			if (!Instance.Emitter)
			{
				continue;
			}

			const bool bVersionGuidInvalid = !Instance.Version.IsValid();
			const bool bMissingVersionData = Instance.GetEmitterData() == nullptr;
			if (!bVersionGuidInvalid && !bMissingVersionData)
			{
				continue;
			}

			const FGuid ExposedVersionGuid = Instance.Emitter->GetExposedVersion().VersionGuid;
			if (!ExposedVersionGuid.IsValid())
			{
				continue;
			}

			const bool bChangedByApi = NiagaraSystem.ChangeEmitterVersion(Instance, ExposedVersionGuid);
			if (!bChangedByApi)
			{
				FVersionedNiagaraEmitter& MutableInstance = Handle.GetInstance();
				MutableInstance.Version = ExposedVersionGuid;
			}

			++RepairedCount;
		}
		return RepairedCount;
	}

	static bool ResolveEmitterHandleFromParams(UNiagaraSystem& NiagaraSystem, const TSharedPtr<FJsonObject>& Params, int32& OutEmitterIndex, FNiagaraEmitterHandle*& OutEmitterHandle, FString& OutError)
	{
		OutEmitterIndex = INDEX_NONE;
		OutEmitterHandle = nullptr;

		TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem.GetEmitterHandles();
		if (Handles.Num() == 0)
		{
			OutError = TEXT("system_has_no_emitters");
			return false;
		}

		double HandleIndexNumber = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("emitter_index"), HandleIndexNumber))
		{
			const int32 Index = FMath::RoundToInt(HandleIndexNumber);
			if (!Handles.IsValidIndex(Index))
			{
				OutError = TEXT("invalid_emitter_index");
				return false;
			}

			OutEmitterIndex = Index;
			OutEmitterHandle = &Handles[Index];
			return true;
		}

		FString EmitterIdString;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("emitter_id"), EmitterIdString) && !EmitterIdString.IsEmpty())
		{
			FGuid EmitterGuid;
			if (!FGuid::Parse(EmitterIdString, EmitterGuid))
			{
				OutError = TEXT("invalid_emitter_id");
				return false;
			}

			for (int32 Index = 0; Index < Handles.Num(); ++Index)
			{
				if (Handles[Index].GetId() == EmitterGuid)
				{
					OutEmitterIndex = Index;
					OutEmitterHandle = &Handles[Index];
					return true;
				}
			}
			OutError = TEXT("emitter_not_found");
			return false;
		}

		FString EmitterName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("emitter_name"), EmitterName) && !EmitterName.IsEmpty())
		{
			for (int32 Index = 0; Index < Handles.Num(); ++Index)
			{
				if (Handles[Index].GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
				{
					OutEmitterIndex = Index;
					OutEmitterHandle = &Handles[Index];
					return true;
				}
			}

			OutError = TEXT("emitter_not_found");
			return false;
		}

		OutError = TEXT("missing_emitter_identifier");
		return false;
	}

}



#include "Commands/EditorViewport/UeAgentEditorViewportLegacy.h"
#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"

bool FUeAgentHttpServer::CmdNiagaraListAssets(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString RootPath = TEXT("/Game");
	JsonTryGetString(Ctx.Params, TEXT("root_path"), RootPath);
	RootPath.TrimStartAndEndInline();
	if (RootPath.IsEmpty())
	{
		RootPath = TEXT("/Game");
	}
	if (!RootPath.StartsWith(TEXT("/")))
	{
		RootPath = TEXT("/") + RootPath;
	}
	if (!FPackageName::IsValidPath(RootPath))
	{
		OutError = TEXT("invalid_root_path");
		return false;
	}

	int32 Limit = 200;
	double LimitNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber))
	{
		Limit = FMath::Clamp(FMath::RoundToInt(LimitNumber), 1, 5000);
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
	Filter.ClassPaths.Add(UNiagaraEmitter::StaticClass()->GetClassPathName());
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(*RootPath);
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(FMath::Min(AssetList.Num(), Limit));
	for (const FAssetData& AssetData : AssetList)
	{
		if (Items.Num() >= Limit)
		{
			break;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Item->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Item->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Item->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Item->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("root_path"), RootPath);
	OutData->SetNumberField(TEXT("total_count"), AssetList.Num());
	OutData->SetNumberField(TEXT("returned_count"), Items.Num());
	OutData->SetArrayField(TEXT("items"), Items);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraListModuleLibrary(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString RootPath = TEXT("/Niagara/Modules");
	JsonTryGetString(Ctx.Params, TEXT("root_path"), RootPath);
	RootPath.TrimStartAndEndInline();
	if (RootPath.IsEmpty())
	{
		RootPath = TEXT("/Niagara/Modules");
	}
	if (!RootPath.StartsWith(TEXT("/")))
	{
		RootPath = TEXT("/") + RootPath;
	}
	if (!FPackageName::IsValidPath(RootPath))
	{
		OutError = TEXT("invalid_root_path");
		return false;
	}

	FString CategoryFilter;
	JsonTryGetString(Ctx.Params, TEXT("category"), CategoryFilter);
	CategoryFilter.TrimStartAndEndInline();

	FString NameContains;
	JsonTryGetString(Ctx.Params, TEXT("name_contains"), NameContains);
	if (NameContains.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("search"), NameContains);
	}
	NameContains.TrimStartAndEndInline();

	int32 Limit = 5000;
	double LimitNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber))
	{
		JsonTryGetNumber(Ctx.Params, TEXT("max_results"), LimitNumber);
	}
	if (LimitNumber > 0.0)
	{
		Limit = FMath::Clamp(FMath::RoundToInt(LimitNumber), 1, 20000);
	}

	bool bIncludeNonModuleScripts = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_non_module_scripts"), bIncludeNonModuleScripts);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(*RootPath);
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);
	AssetList.Sort([](const FAssetData& A, const FAssetData& B)
	{
		const int32 PackagePathCompare = A.PackagePath.ToString().Compare(B.PackagePath.ToString(), ESearchCase::IgnoreCase);
		if (PackagePathCompare != 0)
		{
			return PackagePathCompare < 0;
		}
		return A.AssetName.ToString().Compare(B.AssetName.ToString(), ESearchCase::IgnoreCase) < 0;
	});

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(FMath::Min(AssetList.Num(), Limit));
	int32 MatchedCount = 0;
	for (const FAssetData& AssetData : AssetList)
	{
		if (!NameContains.IsEmpty() && !AssetData.AssetName.ToString().Contains(NameContains, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(AssetData.GetAsset());
		if (!NiagaraScript)
		{
			continue;
		}
		if (!bIncludeNonModuleScripts && !NiagaraScript->IsModuleScript())
		{
			continue;
		}

		const FString PackagePath = AssetData.PackagePath.ToString();
		const FString RelativePackagePath = UeAgentNiagaraOps::MakeRelativePackagePath(PackagePath, RootPath);
		const FString Category = UeAgentNiagaraOps::MakeModuleCategory(RelativePackagePath);
		if (!CategoryFilter.IsEmpty() && !Category.Equals(CategoryFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		++MatchedCount;
		if (Items.Num() >= Limit)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Item->SetStringField(TEXT("asset_path"), AssetData.PackageName.ToString());
		Item->SetStringField(TEXT("package_path"), PackagePath);
		Item->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Item->SetStringField(TEXT("category"), Category);
		Item->SetStringField(TEXT("relative_package_path"), RelativePackagePath);
		Item->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(NiagaraScript->GetUsage()));
		Item->SetBoolField(TEXT("is_module_script"), NiagaraScript->IsModuleScript());

		if (const FVersionedNiagaraScriptData* ScriptData = NiagaraScript->GetLatestScriptData())
		{
			Item->SetArrayField(TEXT("supported_usages"), UeAgentNiagaraOps::BuildScriptUsageJsonArray(ScriptData->GetSupportedUsageContexts()));
			Item->SetStringField(TEXT("library_visibility"), UeAgentNiagaraOps::ScriptLibraryVisibilityToString(ScriptData->LibraryVisibility));
			Item->SetNumberField(TEXT("module_usage_bitmask"), ScriptData->ModuleUsageBitmask);
		}
		else
		{
			Item->SetArrayField(TEXT("supported_usages"), TArray<TSharedPtr<FJsonValue>>());
		}

		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("root_path"), RootPath);
	OutData->SetStringField(TEXT("category_filter"), CategoryFilter);
	OutData->SetStringField(TEXT("name_contains"), NameContains);
	OutData->SetBoolField(TEXT("include_non_module_scripts"), bIncludeNonModuleScripts);
	OutData->SetNumberField(TEXT("total_count"), AssetList.Num());
	OutData->SetNumberField(TEXT("matched_count"), MatchedCount);
	OutData->SetNumberField(TEXT("returned_count"), Items.Num());
	OutData->SetArrayField(TEXT("items"), Items);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraCreateSystem(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentNiagaraOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	bool bFoundStaleObjectBeforeCreate = false;
	const bool bDetachedStaleObjectBeforeCreate = UeAgentNiagaraOps::DetachStaleObjectAtPath(ObjectPath, &bFoundStaleObjectBeforeCreate);
	if (UeAgentNiagaraOps::LoadAssetObject(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = NewObject<UNiagaraSystem>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!NiagaraSystem)
	{
		OutError = TEXT("create_niagara_system_failed");
		return false;
	}

	bool bCreateDefaultNodes = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_default_nodes"), bCreateDefaultNodes);
	UNiagaraSystemFactoryNew::InitializeSystem(NiagaraSystem, bCreateDefaultNodes);

	FAssetRegistryModule::AssetCreated(NiagaraSystem);
	NiagaraSystem->MarkPackageDirty();
	Package->MarkPackageDirty();

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (!UeAgentNiagaraOps::CanSafelyOpenNiagaraSystemEditor(*NiagaraSystem, OutError))
		{
			return false;
		}
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(NiagaraSystem);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("asset_name"), AssetName);
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetBoolField(TEXT("created_default_nodes"), bCreateDefaultNodes);
	OutData->SetBoolField(TEXT("found_stale_object_before_create"), bFoundStaleObjectBeforeCreate);
	OutData->SetBoolField(TEXT("detached_stale_object_before_create"), bDetachedStaleObjectBeforeCreate);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraCreateEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentNiagaraOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	bool bFoundStaleObjectBeforeCreate = false;
	const bool bDetachedStaleObjectBeforeCreate = UeAgentNiagaraOps::DetachStaleObjectAtPath(ObjectPath, &bFoundStaleObjectBeforeCreate);
	if (UeAgentNiagaraOps::LoadAssetObject(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	UNiagaraEmitter* NiagaraEmitter = NewObject<UNiagaraEmitter>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!NiagaraEmitter)
	{
		OutError = TEXT("create_niagara_emitter_failed");
		return false;
	}

	bool bAddDefaultModulesAndRenderers = true;
	JsonTryGetBool(Ctx.Params, TEXT("add_default_modules_and_renderers"), bAddDefaultModulesAndRenderers);
	UNiagaraEmitterFactoryNew::InitializeEmitter(NiagaraEmitter, bAddDefaultModulesAndRenderers);

	FAssetRegistryModule::AssetCreated(NiagaraEmitter);
	NiagaraEmitter->MarkPackageDirty();
	Package->MarkPackageDirty();

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(NiagaraEmitter);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraEmitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("asset_name"), AssetName);
	OutData->SetStringField(TEXT("object_path"), NiagaraEmitter->GetPathName());
	OutData->SetBoolField(TEXT("added_default_modules_and_renderers"), bAddDefaultModulesAndRenderers);
	OutData->SetBoolField(TEXT("found_stale_object_before_create"), bFoundStaleObjectBeforeCreate);
	OutData->SetBoolField(TEXT("detached_stale_object_before_create"), bDetachedStaleObjectBeforeCreate);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraDeleteAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(Asset) && !Cast<UNiagaraEmitter>(Asset) && !Cast<UNiagaraScript>(Asset))
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	bool bForceDelete = false;
	JsonTryGetBool(Ctx.Params, TEXT("force_delete"), bForceDelete);
	bool bUseUncheckedDelete = false;
	JsonTryGetBool(Ctx.Params, TEXT("use_unchecked_delete"), bUseUncheckedDelete);

	const FString DeletedAssetPath = Asset->GetOutermost()->GetName();
	const FString DeletedObjectPath = Asset->GetPathName();
	const FString AssetKind = UeAgentNiagaraOps::GetNiagaraAssetKind(Asset);
	const bool bPackageExistsOnDisk = FPackageName::DoesPackageExist(DeletedAssetPath);
	const bool bUseUncheckedDeletePath = bForceDelete && (bUseUncheckedDelete || !bPackageExistsOnDisk);

	FUeAgentInterfaceLogger::Log(
		TEXT("Niagara DeleteAsset start asset=%s kind=%s force=%s unchecked=%s package_exists=%s"),
		*DeletedObjectPath,
		*AssetKind,
		bForceDelete ? TEXT("true") : TEXT("false"),
		bUseUncheckedDeletePath ? TEXT("true") : TEXT("false"),
		bPackageExistsOnDisk ? TEXT("true") : TEXT("false"));

	bool bDeleted = false;
	bool bFoundResidualObjectAfterDelete = false;
	bool bDetachedResidualObjectAfterDelete = false;
	{
		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
		if (bForceDelete)
		{
			TArray<UObject*> ObjectsToDelete;
			ObjectsToDelete.Add(Asset);
			bDeleted = bUseUncheckedDeletePath
				? ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete) > 0
				: ObjectTools::ForceDeleteObjects(ObjectsToDelete, false) > 0;
		}
		else
		{
			bDeleted = ObjectTools::DeleteSingleObject(Asset, true);
		}
	}

	if (bDeleted)
	{
		bDetachedResidualObjectAfterDelete = UeAgentNiagaraOps::DetachStaleObjectAtPath(DeletedObjectPath, &bFoundResidualObjectAfterDelete);
	}

	FUeAgentInterfaceLogger::Log(
		TEXT("Niagara DeleteAsset done asset=%s kind=%s deleted=%s strategy=%s residual_found=%s residual_detached=%s"),
		*DeletedObjectPath,
		*AssetKind,
		bDeleted ? TEXT("true") : TEXT("false"),
		bUseUncheckedDeletePath ? TEXT("delete_objects_unchecked") : (bForceDelete ? TEXT("force_delete_objects") : TEXT("delete_single_object")),
		bFoundResidualObjectAfterDelete ? TEXT("true") : TEXT("false"),
		bDetachedResidualObjectAfterDelete ? TEXT("true") : TEXT("false"));

	if (!bDeleted)
	{
		OutError = bForceDelete ? TEXT("force_delete_asset_failed") : TEXT("delete_asset_failed_or_blocked_by_references");
		return false;
	}

	OutData->SetStringField(TEXT("deleted_asset_path"), DeletedAssetPath);
	OutData->SetStringField(TEXT("deleted_object_path"), DeletedObjectPath);
	OutData->SetStringField(TEXT("asset_kind"), AssetKind);
	OutData->SetBoolField(TEXT("force_delete"), bForceDelete);
	OutData->SetBoolField(TEXT("package_exists_on_disk"), bPackageExistsOnDisk);
	OutData->SetBoolField(TEXT("use_unchecked_delete"), bUseUncheckedDeletePath);
	OutData->SetBoolField(TEXT("found_residual_object_after_delete"), bFoundResidualObjectAfterDelete);
	OutData->SetBoolField(TEXT("detached_residual_object_after_delete"), bDetachedResidualObjectAfterDelete);
	OutData->SetStringField(TEXT("delete_strategy"), bUseUncheckedDeletePath ? TEXT("delete_objects_unchecked") : (bForceDelete ? TEXT("force_delete_objects") : TEXT("delete_single_object")));
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraDuplicateAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_asset_path"), SourceAssetPathInput) || SourceAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_source_asset_path");
		return false;
	}

	FString TargetAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("target_asset_path"), TargetAssetPathInput) || TargetAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_target_asset_path");
		return false;
	}

	UObject* SourceAsset = UeAgentNiagaraOps::LoadAssetObject(SourceAssetPathInput);
	if (!SourceAsset)
	{
		OutError = TEXT("source_asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(SourceAsset) && !Cast<UNiagaraEmitter>(SourceAsset) && !Cast<UNiagaraScript>(SourceAsset))
	{
		OutError = TEXT("source_asset_is_not_niagara");
		return false;
	}

	const FString TargetPackageName = UeAgentNiagaraOps::NormalizeAssetPath(TargetAssetPathInput);
	if (!FPackageName::IsValidLongPackageName(TargetPackageName))
	{
		OutError = TEXT("invalid_target_asset_path");
		return false;
	}
	const FString TargetAssetName = FPackageName::GetLongPackageAssetName(TargetPackageName);
	if (TargetAssetName.IsEmpty())
	{
		OutError = TEXT("invalid_target_asset_name");
		return false;
	}

	const FString TargetObjectPath = FString::Printf(TEXT("%s.%s"), *TargetPackageName, *TargetAssetName);
	bool bFoundStaleTargetObjectBeforeDuplicate = false;
	const bool bDetachedStaleTargetObjectBeforeDuplicate = UeAgentNiagaraOps::DetachStaleObjectAtPath(TargetObjectPath, &bFoundStaleTargetObjectBeforeDuplicate);
	if (UeAgentNiagaraOps::LoadAssetObject(TargetObjectPath))
	{
		OutError = TEXT("target_asset_already_exists");
		return false;
	}

	UPackage* TargetPackage = CreatePackage(*TargetPackageName);
	if (!TargetPackage)
	{
		OutError = TEXT("create_target_package_failed");
		return false;
	}

	UObject* DuplicatedAsset = StaticDuplicateObject(SourceAsset, TargetPackage, *TargetAssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!DuplicatedAsset)
	{
		OutError = TEXT("duplicate_asset_failed");
		return false;
	}

	int32 RepairedEmitterVersionCount = 0;
	if (UNiagaraSystem* DuplicatedSystem = Cast<UNiagaraSystem>(DuplicatedAsset))
	{
		RepairedEmitterVersionCount = UeAgentNiagaraOps::RepairSystemInvalidEmitterVersions(*DuplicatedSystem);
		if (RepairedEmitterVersionCount > 0)
		{
			DuplicatedSystem->Modify();
			DuplicatedSystem->MarkPackageDirty();
		}
	}

	FAssetRegistryModule::AssetCreated(DuplicatedAsset);
	DuplicatedAsset->MarkPackageDirty();
	TargetPackage->MarkPackageDirty();

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(DuplicatedAsset);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(DuplicatedAsset, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("source_asset_path"), SourceAsset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("target_asset_path"), TargetPackageName);
	OutData->SetStringField(TEXT("target_object_path"), DuplicatedAsset->GetPathName());
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(DuplicatedAsset));
	OutData->SetNumberField(TEXT("repaired_emitter_version_count"), RepairedEmitterVersionCount);
	OutData->SetBoolField(TEXT("found_stale_target_object_before_duplicate"), bFoundStaleTargetObjectBeforeDuplicate);
	OutData->SetBoolField(TEXT("detached_stale_target_object_before_duplicate"), bDetachedStaleTargetObjectBeforeDuplicate);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(Asset) && !Cast<UNiagaraEmitter>(Asset) && !Cast<UNiagaraScript>(Asset))
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!AssetEditorSubsystem)
	{
		OutError = TEXT("missing_asset_editor_subsystem");
		return false;
	}
	AssetEditorSubsystem->OpenEditorForAsset(Asset);

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraPreviewAdvance(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
	if (!NiagaraSystem)
	{
		OutError = TEXT("asset_is_not_niagara_system");
		return false;
	}

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	if (NiagaraSystem->HasOutstandingCompilationRequests(true))
	{
		NiagaraSystem->WaitForCompilationComplete(true, false);
		NiagaraSystem->PollForCompilationComplete(true);
	}
	NiagaraSystem->CacheFromCompiledData();

	UPackage* NiagaraSystemPackage = NiagaraSystem->GetOutermost();
	const bool bWasPackageDirtyBeforeAdvance = NiagaraSystemPackage ? NiagaraSystemPackage->IsDirty() : false;

	TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
	UNiagaraComponent* PreviewComponent = nullptr;
	if (!UeAgentNiagaraOps::ResolveNiagaraSystemPreviewComponent(NiagaraSystem, bOpenEditorIfNeeded, PreviewComponent, SystemViewModel, OutError))
	{
		return false;
	}
	if (NiagaraSystem->HasOutstandingCompilationRequests(true))
	{
		NiagaraSystem->WaitForCompilationComplete(true, false);
		NiagaraSystem->PollForCompilationComplete(true);
	}
	NiagaraSystem->CacheFromCompiledData();

	bool bResetPreview = true;
	JsonTryGetBool(Ctx.Params, TEXT("reset_preview"), bResetPreview);

	bool bDeactivateBeforeReset = true;
	JsonTryGetBool(Ctx.Params, TEXT("deactivate_before_reset"), bDeactivateBeforeReset);

	bool bPauseAfterAdvance = true;
	JsonTryGetBool(Ctx.Params, TEXT("pause_after_advance"), bPauseAfterAdvance);
	JsonTryGetBool(Ctx.Params, TEXT("pause_preview_after_advance"), bPauseAfterAdvance);
	bool bRestorePreviewAfterAdvance = false;
	UeAgentNiagaraOps::TryGetBoolAnyField(
		Ctx.Params,
		{
			TEXT("restore_preview_after_advance"),
			TEXT("resume_preview_after_advance"),
			TEXT("restore_interactive_preview"),
			TEXT("resume_interactive_preview")
		},
		bRestorePreviewAfterAdvance);

	double TickDeltaNumber = 1.0 / 60.0;
	JsonTryGetNumber(Ctx.Params, TEXT("tick_delta_seconds"), TickDeltaNumber);
	JsonTryGetNumber(Ctx.Params, TEXT("preview_tick_delta_seconds"), TickDeltaNumber);
	const float TickDeltaSeconds = FMath::Clamp(static_cast<float>(TickDeltaNumber), 1.0f / 240.0f, 1.0f / 5.0f);

	double TargetFrameNumber = -1.0;
	const bool bHasTargetFrame =
		JsonTryGetNumber(Ctx.Params, TEXT("target_frame"), TargetFrameNumber) ||
		JsonTryGetNumber(Ctx.Params, TEXT("frame"), TargetFrameNumber) ||
		JsonTryGetNumber(Ctx.Params, TEXT("advance_frames"), TargetFrameNumber) ||
		JsonTryGetNumber(Ctx.Params, TEXT("preview_advance_frames"), TargetFrameNumber);

	if (!bHasTargetFrame)
	{
		double TargetTimeSeconds = -1.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("target_time_seconds"), TargetTimeSeconds) ||
			JsonTryGetNumber(Ctx.Params, TEXT("preview_advance_seconds"), TargetTimeSeconds))
		{
			TargetFrameNumber = TargetTimeSeconds / TickDeltaSeconds;
		}
	}

	if (TargetFrameNumber < -KINDA_SMALL_NUMBER)
	{
		OutError = TEXT("missing_target_frame");
		return false;
	}

	const int32 TargetFrame = FMath::Clamp(FMath::RoundToInt(TargetFrameNumber), 0, 14400);

	double StartFrameNumber = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("start_frame"), StartFrameNumber);
	const int32 StartFrame = FMath::Max(0, FMath::RoundToInt(StartFrameNumber));
	if (bResetPreview && StartFrame != 0)
	{
		OutError = TEXT("reset_preview_requires_start_frame_zero");
		OutData->SetNumberField(TEXT("start_frame"), StartFrame);
		return false;
	}
	if (StartFrame > TargetFrame)
	{
		OutError = TEXT("start_frame_after_target_frame");
		OutData->SetNumberField(TEXT("start_frame"), StartFrame);
		OutData->SetNumberField(TEXT("target_frame"), TargetFrame);
		return false;
	}
	const int32 FramesToAdvance = bResetPreview ? TargetFrame : (TargetFrame - StartFrame);

	FString AdvanceMode = TEXT("tick_component");
	JsonTryGetString(Ctx.Params, TEXT("advance_mode"), AdvanceMode);
	JsonTryGetString(Ctx.Params, TEXT("preview_advance_mode"), AdvanceMode);
	AdvanceMode.TrimStartAndEndInline();
	if (AdvanceMode.IsEmpty())
	{
		AdvanceMode = TEXT("tick_component");
	}
	const FString NormalizedAdvanceMode = AdvanceMode.ToLower();
	const bool bUseAdvanceSimulation =
		NormalizedAdvanceMode == TEXT("advance") ||
		NormalizedAdvanceMode == TEXT("advance_simulation") ||
		NormalizedAdvanceMode == TEXT("advance_simulation_by_time");
	const bool bUseTickComponent =
		NormalizedAdvanceMode == TEXT("tick") ||
		NormalizedAdvanceMode == TEXT("tick_component") ||
		NormalizedAdvanceMode == TEXT("tick_delta") ||
		NormalizedAdvanceMode == TEXT("tick_delta_time");
	if (!bUseAdvanceSimulation && !bUseTickComponent)
	{
		OutError = TEXT("unsupported_continuous_advance_mode");
		OutData->SetStringField(TEXT("advance_mode"), AdvanceMode);
		return false;
	}

	auto ConfigurePreviewComponent = [TickDeltaSeconds, NiagaraSystem](UNiagaraComponent* Component)
	{
		if (!Component)
		{
			return;
		}
		Component->SetAsset(NiagaraSystem, false);
		Component->SetForceSolo(true);
		Component->SetAgeUpdateMode(ENiagaraAgeUpdateMode::TickDeltaTime);
		Component->SetSeekDelta(TickDeltaSeconds);
		Component->SetLockDesiredAgeDeltaTimeToSeekDelta(true);
		Component->SetCanRenderWhileSeeking(true);
		Component->SetAllowScalability(false);
		Component->SetRenderingEnabled(true);
	};

	UeAgentNiagaraOps::SetNiagaraPreviewStateToken(PreviewComponent, FString());
	ConfigurePreviewComponent(PreviewComponent);

	if (bResetPreview)
	{
		if (bDeactivateBeforeReset)
		{
			PreviewComponent->DeactivateImmediate();
			UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		}

		PreviewComponent->SetDesiredAge(0.0f);
		PreviewComponent->ReinitializeSystem();
		PreviewComponent = SystemViewModel->GetPreviewComponent();
		if (!PreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_missing_after_reset");
			return false;
		}
		ConfigurePreviewComponent(PreviewComponent);
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
	}

	const bool bWasActiveBeforeActivation = PreviewComponent->IsActive();
	PreviewComponent->SetPaused(false);
	PreviewComponent->Activate(false);
	UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);

	for (int32 FrameIndex = 0; FrameIndex < FramesToAdvance; ++FrameIndex)
	{
		if (bUseAdvanceSimulation)
		{
			PreviewComponent->AdvanceSimulation(1, TickDeltaSeconds);
		}
		else
		{
			PreviewComponent->TickComponent(TickDeltaSeconds, ELevelTick::LEVELTICK_All, nullptr);
		}
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
	}

	if (bPauseAfterAdvance)
	{
		PreviewComponent->SetPaused(true);
	}
	PreviewComponent->MarkRenderStateDirty();

	double SystemAge = -1.0;
	int32 SystemTickCount = INDEX_NONE;
	int32 FrameEstimate = INDEX_NONE;
	const bool bHasRuntimePosition = UeAgentNiagaraOps::GetNiagaraComponentRuntimePosition(PreviewComponent, TickDeltaSeconds, SystemAge, SystemTickCount, FrameEstimate);

	const FString EffectiveAdvanceMode = bUseAdvanceSimulation ? TEXT("advance_simulation") : TEXT("tick_component");
	const FString AdvanceSemantics = bResetPreview ? TEXT("continuous_from_zero") : TEXT("continuous_from_current");
	const FString PreviewStateToken = UeAgentNiagaraOps::MakeNiagaraPreviewStateToken(NiagaraSystem, PreviewComponent, TargetFrame, TickDeltaSeconds, EffectiveAdvanceMode);
	UeAgentNiagaraOps::SetNiagaraPreviewStateToken(PreviewComponent, PreviewStateToken);
	TSharedPtr<FJsonObject> PreviewRestoreReport;
	if (bRestorePreviewAfterAdvance)
	{
		PreviewRestoreReport = UeAgentNiagaraOps::RestoreNiagaraPreviewForInteractiveEditing(NiagaraSystem, SystemViewModel);
	}

	bool bRestoredDirtyState = false;
	if (NiagaraSystemPackage && !bWasPackageDirtyBeforeAdvance && NiagaraSystemPackage->IsDirty())
	{
		NiagaraSystemPackage->SetDirtyFlag(false);
		bRestoredDirtyState = true;
	}

	FString StatsError;
	TSharedPtr<FJsonObject> Stats = UeAgentNiagaraOps::CollectNiagaraComponentStats(NiagaraSystem, PreviewComponent, StatsError);

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetStringField(TEXT("component_source"), TEXT("editor_preview_component"));
	OutData->SetStringField(TEXT("component_name"), PreviewComponent->GetName());
	OutData->SetStringField(TEXT("component_path"), PreviewComponent->GetPathName());
	OutData->SetStringField(TEXT("component_world"), PreviewComponent->GetWorld() ? PreviewComponent->GetWorld()->GetPathName() : FString());
	OutData->SetNumberField(TEXT("start_frame"), StartFrame);
	OutData->SetNumberField(TEXT("target_frame"), TargetFrame);
	OutData->SetNumberField(TEXT("advanced_frame_count"), FramesToAdvance);
	OutData->SetNumberField(TEXT("tick_delta_seconds"), TickDeltaSeconds);
	OutData->SetStringField(TEXT("advance_mode"), EffectiveAdvanceMode);
	OutData->SetStringField(TEXT("advance_semantics"), AdvanceSemantics);
	OutData->SetBoolField(TEXT("used_seek"), false);
	OutData->SetBoolField(TEXT("reset_preview"), bResetPreview);
	OutData->SetBoolField(TEXT("deactivate_before_reset"), bDeactivateBeforeReset);
	OutData->SetBoolField(TEXT("pause_after_advance"), bPauseAfterAdvance);
	OutData->SetBoolField(TEXT("restore_preview_after_advance"), bRestorePreviewAfterAdvance);
	OutData->SetBoolField(TEXT("component_active_before_activation"), bWasActiveBeforeActivation);
	OutData->SetBoolField(TEXT("component_paused"), PreviewComponent->IsPaused());
	OutData->SetNumberField(TEXT("system_age"), SystemAge);
	OutData->SetNumberField(TEXT("system_tick_count"), SystemTickCount);
	OutData->SetNumberField(TEXT("current_frame_estimate"), FrameEstimate);
	OutData->SetBoolField(TEXT("has_runtime_position"), bHasRuntimePosition);
	OutData->SetStringField(TEXT("preview_state_token"), PreviewStateToken);
	OutData->SetBoolField(TEXT("preview_state_token_invalidated_by_restore"), bRestorePreviewAfterAdvance);
	OutData->SetBoolField(TEXT("mutated_preview_component"), true);
	OutData->SetBoolField(TEXT("was_package_dirty_before_advance"), bWasPackageDirtyBeforeAdvance);
	OutData->SetBoolField(TEXT("restored_dirty_state"), bRestoredDirtyState);
	OutData->SetStringField(TEXT("stats_error"), StatsError);
	if (Stats.IsValid())
	{
		OutData->SetObjectField(TEXT("stats"), Stats);
	}
	if (PreviewRestoreReport.IsValid())
	{
		OutData->SetObjectField(TEXT("preview_restore"), PreviewRestoreReport);
	}
	if (!bHasRuntimePosition)
	{
		OutError = TEXT("preview_advance_no_system_instance");
		return false;
	}
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings || !Settings->bEnableScreenshots)
	{
		OutError = TEXT("screenshots_disabled");
		return false;
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(Asset) && !Cast<UNiagaraEmitter>(Asset) && !Cast<UNiagaraScript>(Asset))
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	bool bSynchronizedOverviewGraph = false;
	int32 OverviewNodeCountBefore = INDEX_NONE;
	int32 OverviewNodeCountAfter = INDEX_NONE;
	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
	{
		bSynchronizedOverviewGraph = UeAgentNiagaraOps::SynchronizeSystemEditorData(*NiagaraSystem, OverviewNodeCountBefore, OverviewNodeCountAfter);
	}

	UeAgentEditorViewportSequenceFrameOptions::FUnifiedSequenceFrameOptionsReport UnifiedOptionsReport;
	UeAgentEditorViewportSequenceFrameOptions::ApplyUnifiedSequenceFrameOptionsToParams(
		Ctx.Params,
		UeAgentEditorViewportSequenceFrameOptions::ESequenceFrameCommandKind::Niagara,
		UnifiedOptionsReport);

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	IAssetEditorInstance* AssetEditor = UeAgentNiagaraOps::GetNiagaraAssetEditor(Asset, bOpenEditorIfNeeded, OutError);
	if (!AssetEditor)
	{
		return false;
	}

	FString Target = TEXT("window");
	JsonTryGetString(Ctx.Params, TEXT("target"), Target);
	Target = Target.TrimStartAndEnd().ToLower();

	FString RequestedCaptureMode;
	JsonTryGetString(Ctx.Params, TEXT("capture_mode"), RequestedCaptureMode);
	RequestedCaptureMode.TrimStartAndEndInline();
	const FString NormalizedRequestedCaptureMode = RequestedCaptureMode.ToLower();
	const bool bCaptureCurrentPreview =
		NormalizedRequestedCaptureMode == TEXT("current_preview") ||
		NormalizedRequestedCaptureMode == TEXT("current_preview_readonly") ||
		NormalizedRequestedCaptureMode == TEXT("readonly_current_preview");

	TArray<UeAgentNiagaraOps::FNiagaraScreenshotFrameRequest> FrameRequests;
	bool bFrameSequenceRequested = false;
	if (!bCaptureCurrentPreview)
	{
		if (!UeAgentNiagaraOps::ResolveNiagaraScreenshotFrameSequence(Ctx.Params, FrameRequests, bFrameSequenceRequested, OutError))
		{
			return false;
		}
	}

	FString Format = TEXT("png");
	int32 Quality = 95;
	int32 MaxSize = 2048;
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);

	double QualityNum = static_cast<double>(Quality);
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QualityNum))
	{
		Quality = FMath::Clamp(static_cast<int32>(QualityNum), 1, 100);
	}

	double MaxSizeNum = static_cast<double>(MaxSize);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeNum))
	{
		MaxSize = FMath::Clamp(static_cast<int32>(MaxSizeNum), 64, 8192);
	}

	const bool bRequestsUnifiedPreviewTarget =
		Target == TEXT("preview") ||
		Target == TEXT("viewport") ||
		Target == TEXT("preview_viewport") ||
		Target == TEXT("current_preview") ||
		bCaptureCurrentPreview;
	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
		NiagaraSystem && bRequestsUnifiedPreviewTarget && !bFrameSequenceRequested)
	{
		TSharedPtr<FJsonObject> ProxyParams = UeAgentEditorViewportLegacy::MakeParamsWithTarget(
			Ctx.Params,
			UeAgentEditorViewportLegacy::MakeAssetTarget(NiagaraSystem->GetPathName(), TEXT("niagara_system"), TEXT("preview"), bOpenEditorIfNeeded));
		ProxyParams->SetNumberField(TEXT("quality"), Quality);
		ProxyParams->SetNumberField(TEXT("max_size"), MaxSize);
		if (!ProxyParams->HasField(TEXT("format")))
		{
			ProxyParams->SetStringField(TEXT("format"), Format);
		}
		if (bCaptureCurrentPreview && !ProxyParams->HasField(TEXT("preview_state")))
		{
			TSharedPtr<FJsonObject> PreviewState = MakeShared<FJsonObject>();
			PreviewState->SetStringField(TEXT("mode"), TEXT("current_preview_readonly"));
			ProxyParams->SetObjectField(TEXT("preview_state"), PreviewState);
		}

		FUeAgentRequestContext ProxyCtx = Ctx;
		ProxyCtx.Command = TEXT("editor_viewport_screenshot");
		ProxyCtx.Params = ProxyParams;
		if (!ExecuteEditorViewportCommand(TEXT("editor_viewport_screenshot"), ProxyCtx, OutData, OutError))
		{
			return false;
		}
		UeAgentEditorViewportLegacy::AddCompatMetadata(OutData, TEXT("niagara_screenshot"), TEXT("editor_viewport_screenshot"));
		OutData->SetBoolField(TEXT("deprecated_for_new_authoring"), false);
		OutData->SetBoolField(TEXT("partial_migration"), true);
		OutData->SetStringField(TEXT("compat_subpath"), TEXT("niagara_system_single_preview_frame"));
		OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("target"), Target);
		OutData->SetBoolField(TEXT("synchronized_overview_graph"), bSynchronizedOverviewGraph);
		OutData->SetNumberField(TEXT("overview_node_count_before"), OverviewNodeCountBefore);
		OutData->SetNumberField(TEXT("overview_node_count_after"), OverviewNodeCountAfter);
		return true;
	}

	double PreviewAdvanceSeconds = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("preview_advance_seconds"), PreviewAdvanceSeconds);
	PreviewAdvanceSeconds = FMath::Clamp(PreviewAdvanceSeconds, 0.0, 60.0);

	double PreviewTickDeltaSeconds = 1.0 / 60.0;
	JsonTryGetNumber(Ctx.Params, TEXT("preview_tick_delta_seconds"), PreviewTickDeltaSeconds);
	PreviewTickDeltaSeconds = FMath::Clamp(PreviewTickDeltaSeconds, 1.0 / 240.0, 1.0 / 5.0);

	FString PreviewAdvanceMode = TEXT("desired_age");
	JsonTryGetString(Ctx.Params, TEXT("preview_advance_mode"), PreviewAdvanceMode);
	PreviewAdvanceMode.TrimStartAndEndInline();
	if (PreviewAdvanceMode.IsEmpty())
	{
		PreviewAdvanceMode = TEXT("desired_age");
	}

	bool bResetPreview = PreviewAdvanceSeconds > 0.0;
	JsonTryGetBool(Ctx.Params, TEXT("reset_preview"), bResetPreview);
	if (bCaptureCurrentPreview)
	{
		PreviewAdvanceSeconds = 0.0;
		bResetPreview = false;
	}
	if (bFrameSequenceRequested)
	{
		PreviewAdvanceSeconds = 0.0;
		bResetPreview = false;
	}

	bool bPausePreviewAfterAdvance = true;
	JsonTryGetBool(Ctx.Params, TEXT("pause_preview_after_advance"), bPausePreviewAfterAdvance);
	bool bRestorePreviewAfterCapture = true;
	UeAgentNiagaraOps::TryGetBoolAnyField(
		Ctx.Params,
		{
			TEXT("restore_preview_after_capture"),
			TEXT("resume_preview_after_capture"),
			TEXT("restore_interactive_preview"),
			TEXT("resume_interactive_preview")
		},
		bRestorePreviewAfterCapture);

	const bool bPreviewPrepareRequested = !bFrameSequenceRequested && !bCaptureCurrentPreview && (PreviewAdvanceSeconds > 0.0 || bResetPreview);
	bool bRequestedOffscreenRenderer = true;
	const bool bUseOffscreenRendererSpecified =
		JsonTryGetBool(Ctx.Params, TEXT("offscreen"), bRequestedOffscreenRenderer) ||
		JsonTryGetBool(Ctx.Params, TEXT("use_offscreen_renderer"), bRequestedOffscreenRenderer);
	const bool bUseOffscreenRenderer = true;

	Format = Format.ToLower();
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_format");
		return false;
	}

	const TSharedPtr<SWindow> NiagaraWindow = UeAgentNiagaraOps::ResolveNiagaraEditorWindow(AssetEditor, OutError);
	if (!NiagaraWindow.IsValid())
	{
		return false;
	}

	bool bPreviewPrepared = false;
	FString PreviewPrepareError;
	bool bPrePreviewRedrawPerformed = false;
	FString PrePreviewRedrawSkipReason;

	TSharedPtr<SWidget> WidgetToShot;
	FString EffectiveTarget = Target;
	FString CaptureMode = TEXT("slate_widget");
	if (Target == TEXT("window"))
	{
		WidgetToShot = NiagaraWindow;
	}
	else if (Target == TEXT("viewport") || Target == TEXT("preview") || Target == TEXT("preview_viewport"))
	{
		const TSharedPtr<SWidget> ViewportWidget = UeAgentNiagaraOps::ResolveNiagaraViewportWidget(NiagaraWindow.ToSharedRef(), OutError);
		if (!ViewportWidget.IsValid())
		{
			return false;
		}

		const TSharedPtr<SWidget> SceneWidget = UeAgentNiagaraOps::ResolveNiagaraViewportSceneWidget(ViewportWidget.ToSharedRef());
		WidgetToShot = SceneWidget.IsValid() ? SceneWidget : ViewportWidget;
		EffectiveTarget = TEXT("viewport");
	}
	else
	{
		OutError = TEXT("invalid_target");
		return false;
	}

	if (!WidgetToShot.IsValid())
	{
		OutError = TEXT("target_widget_not_found");
		return false;
	}

	FString WindowBackbufferUnsafeReason;
	const bool bWindowBackbufferSafe = UeAgentNiagaraOps::IsSlateWindowSafeForBackbufferCapture(NiagaraWindow, WindowBackbufferUnsafeReason);

	if (bFrameSequenceRequested)
	{
		UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
		if (!NiagaraSystem)
		{
			OutError = TEXT("frame_sequence_capture_requires_niagara_system");
			return false;
		}
		if (FrameRequests.Num() <= 0)
		{
			OutError = TEXT("preview_frame_sequence_empty");
			return false;
		}

		TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
		UNiagaraComponent* PreviewComponent = nullptr;
		if (!UeAgentNiagaraOps::ResolveNiagaraSystemPreviewComponent(NiagaraSystem, bOpenEditorIfNeeded, PreviewComponent, SystemViewModel, OutError))
		{
			return false;
		}
		if (NiagaraSystem->HasOutstandingCompilationRequests(true))
		{
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
		}
		NiagaraSystem->CacheFromCompiledData();

		UPackage* NiagaraSystemPackage = NiagaraSystem->GetOutermost();
		const bool bWasPackageDirtyBeforeFrameSequence = NiagaraSystemPackage ? NiagaraSystemPackage->IsDirty() : false;

		FString FrameAdvanceMode = PreviewAdvanceMode;
		JsonTryGetString(Ctx.Params, TEXT("advance_mode"), FrameAdvanceMode);
		JsonTryGetString(Ctx.Params, TEXT("preview_advance_mode"), FrameAdvanceMode);
		FrameAdvanceMode.TrimStartAndEndInline();
		if (FrameAdvanceMode.IsEmpty() || FrameAdvanceMode.Equals(TEXT("desired_age"), ESearchCase::IgnoreCase))
		{
			FrameAdvanceMode = TEXT("tick_component");
		}
		const FString NormalizedFrameAdvanceMode = FrameAdvanceMode.ToLower();
		const bool bUseAdvanceSimulation =
			NormalizedFrameAdvanceMode == TEXT("advance") ||
			NormalizedFrameAdvanceMode == TEXT("advance_simulation") ||
			NormalizedFrameAdvanceMode == TEXT("advance_simulation_by_time");
		const bool bUseTickComponent =
			NormalizedFrameAdvanceMode == TEXT("tick") ||
			NormalizedFrameAdvanceMode == TEXT("tick_component") ||
			NormalizedFrameAdvanceMode == TEXT("tick_delta") ||
			NormalizedFrameAdvanceMode == TEXT("tick_delta_time");
		if (!bUseAdvanceSimulation && !bUseTickComponent)
		{
			OutError = TEXT("unsupported_continuous_advance_mode");
			OutData->SetStringField(TEXT("advance_mode"), FrameAdvanceMode);
			return false;
		}
		const FString EffectiveFrameAdvanceMode = bUseAdvanceSimulation ? TEXT("advance_simulation") : TEXT("tick_component");

		auto ConfigurePreviewComponent = [PreviewTickDeltaSeconds, NiagaraSystem](UNiagaraComponent* Component)
		{
			if (!Component)
			{
				return;
			}
			const float SafeTickDelta = static_cast<float>(PreviewTickDeltaSeconds);
			Component->SetAsset(NiagaraSystem, false);
			Component->SetForceSolo(true);
			Component->SetAgeUpdateMode(ENiagaraAgeUpdateMode::TickDeltaTime);
			Component->SetSeekDelta(SafeTickDelta);
			Component->SetLockDesiredAgeDeltaTimeToSeekDelta(true);
			Component->SetCanRenderWhileSeeking(true);
			Component->SetAllowScalability(false);
			Component->SetRenderingEnabled(true);
		};

		UeAgentNiagaraOps::SetNiagaraPreviewStateToken(PreviewComponent, FString());
		ConfigurePreviewComponent(PreviewComponent);
		PreviewComponent->DeactivateImmediate();
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		PreviewComponent->SetDesiredAge(0.0f);
		PreviewComponent->ReinitializeSystem();
		PreviewComponent = SystemViewModel->GetPreviewComponent();
		if (!PreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_missing_after_reset");
			return false;
		}
		ConfigurePreviewComponent(PreviewComponent);
		PreviewComponent->SetPaused(false);
		PreviewComponent->Activate(false);
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);

		const FVector2D FrameOffscreenDrawSize = UeAgentNiagaraOps::ResolveOffscreenDrawSize(WidgetToShot.ToSharedRef(), EffectiveTarget, Ctx.Params, MaxSize);
		FString FrameCaptureRenderer = TEXT("preview_scene_capture");
		JsonTryGetString(Ctx.Params, TEXT("frame_capture_renderer"), FrameCaptureRenderer) ||
			JsonTryGetString(Ctx.Params, TEXT("capture_renderer"), FrameCaptureRenderer) ||
			JsonTryGetString(Ctx.Params, TEXT("renderer"), FrameCaptureRenderer);
		FrameCaptureRenderer.TrimStartAndEndInline();
		FrameCaptureRenderer = FrameCaptureRenderer.ToLower();
		if (FrameCaptureRenderer.IsEmpty())
		{
			FrameCaptureRenderer = TEXT("preview_scene_capture");
		}
		const bool bUsePreviewSceneCapture =
			FrameCaptureRenderer == TEXT("preview_scene_capture") ||
			FrameCaptureRenderer == TEXT("scene_capture") ||
			FrameCaptureRenderer == TEXT("render_target");
		const bool bUseSlateFrameCapture =
			FrameCaptureRenderer == TEXT("slate") ||
			FrameCaptureRenderer == TEXT("slate_widget") ||
			FrameCaptureRenderer == TEXT("slate_widget_offscreen");

		auto RestoreFrameSequencePreviewAfterFailure = [&]() -> void
		{
			if (!PreviewComponent)
			{
				return;
			}
			if (bRestorePreviewAfterCapture)
			{
				UeAgentNiagaraOps::RestoreNiagaraPreviewForInteractiveEditing(NiagaraSystem, SystemViewModel);
				return;
			}
			PreviewComponent->SetPaused(false);
			PreviewComponent->MarkRenderStateDirty();
			UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		};

		if (!bUsePreviewSceneCapture && !bUseSlateFrameCapture)
		{
			OutError = TEXT("unsupported_frame_capture_renderer");
			OutData->SetStringField(TEXT("frame_capture_renderer"), FrameCaptureRenderer);
			RestoreFrameSequencePreviewAfterFailure();
			return false;
		}

		const FIntPoint FrameCaptureSize = UeAgentNiagaraOps::ResolveNiagaraPreviewSceneCaptureSize(FrameOffscreenDrawSize, Ctx.Params, MaxSize);
		UeAgentNiagaraOps::FNiagaraPreviewSceneCaptureContext SceneCaptureContext;
		TSharedPtr<FJsonObject> SceneCaptureCameraInfo;
		if (bUsePreviewSceneCapture)
		{
			if (!UeAgentNiagaraOps::InitializeNiagaraPreviewSceneCapture(PreviewComponent, FrameCaptureSize, Ctx.Params, SceneCaptureContext, SceneCaptureCameraInfo, OutError))
			{
				RestoreFrameSequencePreviewAfterFailure();
				return false;
			}
		}

		TArray<TSharedPtr<FJsonValue>> CapturedFrames;
		TSharedPtr<FJsonObject> FirstFrameObject;
		TArray<TArray<FColor>> CapturedFramePixelsForSheet;
		TArray<FString> CapturedFrameLabelsForSheet;
		FIntPoint FrameSheetTileSize(0, 0);
		TArray<FColor> PreviousResizedPixels;
		FString PreviousPixelSignature;
		TSharedPtr<FJsonObject> FirstFrameRenderDependencyWait;
		int32 CurrentFrame = 0;

		bool bCreateFrameSheet = FrameRequests.Num() > 1;
		JsonTryGetBool(Ctx.Params, TEXT("create_frame_sheet"), bCreateFrameSheet) ||
			JsonTryGetBool(Ctx.Params, TEXT("frame_sheet"), bCreateFrameSheet) ||
			JsonTryGetBool(Ctx.Params, TEXT("sequence_frame_image"), bCreateFrameSheet);
		bool bWriteIndividualFrames = FrameRequests.Num() == 1;
		JsonTryGetBool(Ctx.Params, TEXT("write_individual_frames"), bWriteIndividualFrames) ||
			JsonTryGetBool(Ctx.Params, TEXT("keep_individual_frames"), bWriteIndividualFrames) ||
			JsonTryGetBool(Ctx.Params, TEXT("output_individual_frames"), bWriteIndividualFrames);
		if (FrameRequests.Num() > 1 && !bCreateFrameSheet && !bWriteIndividualFrames)
		{
			OutError = TEXT("multi_frame_output_disabled");
			RestoreFrameSequencePreviewAfterFailure();
			return false;
		}

		auto TickPreviewOneFrame = [&]() -> void
		{
			if (bUseAdvanceSimulation)
			{
				PreviewComponent->AdvanceSimulation(1, static_cast<float>(PreviewTickDeltaSeconds));
			}
			else
			{
				PreviewComponent->TickComponent(static_cast<float>(PreviewTickDeltaSeconds), ELevelTick::LEVELTICK_All, nullptr);
			}
			++CurrentFrame;
			UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		};

		auto CaptureCurrentPreviewFrame = [&](const UeAgentNiagaraOps::FNiagaraScreenshotFrameRequest& Request, const int32 SequenceIndex, TSharedPtr<FJsonObject>& OutFrameObject) -> bool
		{
			while (CurrentFrame < Request.FrameIndex)
			{
				TickPreviewOneFrame();
			}

			PreviewComponent->SetPaused(true);
			PreviewComponent->MarkRenderStateDirty();
			UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);

			double SystemAge = -1.0;
			int32 SystemTickCount = INDEX_NONE;
			int32 FrameEstimate = INDEX_NONE;
			const bool bHasRuntimePosition = UeAgentNiagaraOps::GetNiagaraComponentRuntimePosition(PreviewComponent, static_cast<float>(PreviewTickDeltaSeconds), SystemAge, SystemTickCount, FrameEstimate);
			const FString PreviewStateToken = UeAgentNiagaraOps::MakeNiagaraPreviewStateToken(NiagaraSystem, PreviewComponent, Request.FrameIndex, static_cast<float>(PreviewTickDeltaSeconds), EffectiveFrameAdvanceMode);
			UeAgentNiagaraOps::SetNiagaraPreviewStateToken(PreviewComponent, PreviewStateToken);

			TArray<FColor> Pixels;
			FIntPoint ShotSize(0, 0);
			if (bUsePreviewSceneCapture)
			{
				if (SequenceIndex == 0)
				{
					if (!UeAgentNiagaraOps::WarmupNiagaraPreviewSceneCapture(SceneCaptureContext, PreviewComponent, Ctx.Params, FirstFrameRenderDependencyWait, OutError))
					{
						RestoreFrameSequencePreviewAfterFailure();
						return false;
					}
				}
				if (!UeAgentNiagaraOps::CaptureNiagaraPreviewSceneFrame(SceneCaptureContext, PreviewComponent, Pixels, ShotSize, OutError))
				{
					RestoreFrameSequencePreviewAfterFailure();
					return false;
				}
			}
			else if (bUseOffscreenRenderer)
			{
				if (!UeAgentNiagaraOps::ScreenshotSlateWidgetOffscreen(WidgetToShot.ToSharedRef(), FrameOffscreenDrawSize, Pixels, ShotSize, OutError))
				{
					RestoreFrameSequencePreviewAfterFailure();
					return false;
				}
			}
			else if (!UeAgentNiagaraOps::ScreenshotSlateWidget(WidgetToShot.ToSharedRef(), Pixels, ShotSize, OutError))
			{
				RestoreFrameSequencePreviewAfterFailure();
				return false;
			}

			TArray<FColor> ResizedPixels;
			FIntPoint ResizedSize(0, 0);
			if (!ResizePixelsMaxSize(Pixels, ShotSize, MaxSize, ResizedPixels, ResizedSize, OutError))
			{
				RestoreFrameSequencePreviewAfterFailure();
				return false;
			}

			const TSharedPtr<FJsonObject> PixelSignal = UeAgentNiagaraOps::BuildPixelSignalStats(ResizedPixels);
			FString PixelSignature;
			if (PixelSignal.IsValid())
			{
				PixelSignal->TryGetStringField(TEXT("pixel_signature"), PixelSignature);
			}
			const int32 ChangedPixelCountFromPrevious = PreviousResizedPixels.Num() > 0
				? UeAgentNiagaraOps::CountPixelDifferences(PreviousResizedPixels, ResizedPixels)
				: INDEX_NONE;
			const bool bVisualDuplicateOfPrevious = SequenceIndex > 0 && !PixelSignature.IsEmpty() && PixelSignature == PreviousPixelSignature;

			FString OutPath;
			int64 Bytes = 0;
			if (bWriteIndividualFrames)
			{
				OutPath = FrameRequests.Num() == 1 ? UeAgentNiagaraOps::ResolveOutputScreenshotPath(Ctx.Params, Format) : MakeShotPath(Format);
				if (!WriteCompressedImage(ResizedPixels, ResizedSize, Format, Quality, OutPath, OutError))
				{
					RestoreFrameSequencePreviewAfterFailure();
					return false;
				}
				Bytes = IFileManager::Get().FileSize(*OutPath);
			}

			if (FrameSheetTileSize.X <= 0 || FrameSheetTileSize.Y <= 0)
			{
				FrameSheetTileSize = ResizedSize;
			}
			if (ResizedSize == FrameSheetTileSize)
			{
				CapturedFramePixelsForSheet.Add(ResizedPixels);
				CapturedFrameLabelsForSheet.Add(FString::Printf(TEXT("F%d"), Request.FrameIndex));
			}

			OutFrameObject = MakeShared<FJsonObject>();
			OutFrameObject->SetNumberField(TEXT("sequence_index"), SequenceIndex);
			if (!OutPath.IsEmpty())
			{
				OutFrameObject->SetStringField(TEXT("file_path"), OutPath);
				OutFrameObject->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
			}
			OutFrameObject->SetNumberField(TEXT("width"), ResizedSize.X);
			OutFrameObject->SetNumberField(TEXT("height"), ResizedSize.Y);
			OutFrameObject->SetNumberField(TEXT("frame_index"), Request.FrameIndex);
			OutFrameObject->SetNumberField(TEXT("target_frame"), Request.FrameIndex);
			OutFrameObject->SetStringField(TEXT("frame_label"), FString::Printf(TEXT("F%d"), Request.FrameIndex));
			OutFrameObject->SetNumberField(TEXT("current_frame_estimate"), FrameEstimate);
			OutFrameObject->SetBoolField(TEXT("has_runtime_position"), bHasRuntimePosition);
			OutFrameObject->SetNumberField(TEXT("system_age"), SystemAge);
			OutFrameObject->SetNumberField(TEXT("system_tick_count"), SystemTickCount);
			OutFrameObject->SetStringField(TEXT("preview_state_token"), PreviewStateToken);
			OutFrameObject->SetBoolField(TEXT("frame_estimate_matched"), FrameEstimate == Request.FrameIndex);
			OutFrameObject->SetStringField(TEXT("frame_capture_renderer"), bUsePreviewSceneCapture ? TEXT("preview_scene_capture") : (bUseOffscreenRenderer ? TEXT("slate_widget_offscreen") : TEXT("slate_widget")));
			OutFrameObject->SetStringField(TEXT("pixel_signature"), PixelSignature);
			if (PixelSignal.IsValid())
			{
				OutFrameObject->SetObjectField(TEXT("pixel_signal"), PixelSignal);
			}
			if (SequenceIndex == 0 && FirstFrameRenderDependencyWait.IsValid())
			{
				OutFrameObject->SetObjectField(TEXT("render_dependency_wait"), FirstFrameRenderDependencyWait);
			}
			if (ChangedPixelCountFromPrevious != INDEX_NONE)
			{
				OutFrameObject->SetNumberField(TEXT("changed_pixel_count_from_previous"), ChangedPixelCountFromPrevious);
				OutFrameObject->SetNumberField(TEXT("changed_pixel_fraction_from_previous"), ResizedPixels.Num() > 0 ? static_cast<double>(ChangedPixelCountFromPrevious) / static_cast<double>(ResizedPixels.Num()) : 0.0);
			}
			OutFrameObject->SetBoolField(TEXT("visual_duplicate_of_previous"), bVisualDuplicateOfPrevious);

			PreviousResizedPixels = ResizedPixels;
			PreviousPixelSignature = PixelSignature;
			PreviewComponent->SetPaused(false);
			return true;
		};

		for (int32 RequestIndex = 0; RequestIndex < FrameRequests.Num(); ++RequestIndex)
		{
			TSharedPtr<FJsonObject> FrameObject;
			if (!CaptureCurrentPreviewFrame(FrameRequests[RequestIndex], RequestIndex, FrameObject))
			{
				return false;
			}
			if (RequestIndex == 0)
			{
				FirstFrameObject = FrameObject;
			}
			CapturedFrames.Add(MakeShared<FJsonValueObject>(FrameObject));
		}

		TSharedPtr<FJsonObject> PreviewRestoreReport;
		if (bRestorePreviewAfterCapture)
		{
			PreviewRestoreReport = UeAgentNiagaraOps::RestoreNiagaraPreviewForInteractiveEditing(NiagaraSystem, SystemViewModel);
		}
		else
		{
			PreviewComponent->SetPaused(true);
			PreviewComponent->MarkRenderStateDirty();
		}

		bool bRestoredDirtyState = false;
		if (NiagaraSystemPackage && !bWasPackageDirtyBeforeFrameSequence && NiagaraSystemPackage->IsDirty())
		{
			NiagaraSystemPackage->SetDirtyFlag(false);
			bRestoredDirtyState = true;
		}

		if (!FirstFrameObject.IsValid())
		{
			OutError = TEXT("preview_frame_capture_empty");
			return false;
		}

		FString FrameSheetPath;
		TSharedPtr<FJsonObject> FrameSheetData;
		if (bCreateFrameSheet && FrameRequests.Num() > 1)
		{
			if (CapturedFramePixelsForSheet.Num() != FrameRequests.Num())
			{
				OutError = TEXT("frame_sheet_capture_size_mismatch");
				return false;
			}
			if (!WriteFrameSequenceSheetImage(CapturedFramePixelsForSheet, CapturedFrameLabelsForSheet, FrameSheetTileSize, Ctx.Params, Format, Quality, FrameSheetPath, FrameSheetData, OutError))
			{
				return false;
			}
		}

		FString PreviewStatsError;
		TSharedPtr<FJsonObject> PreviewStats = UeAgentNiagaraOps::CollectNiagaraPreviewStats(Asset, PreviewStatsError);

		const FString FrameSequenceCaptureMode = bUsePreviewSceneCapture
			? TEXT("continuous_from_zero_preview_scene_capture")
			: (bUseOffscreenRenderer ? TEXT("continuous_from_zero_slate_widget_offscreen") : TEXT("continuous_from_zero_slate_widget"));
		OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
		OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));
		if (FrameSheetData.IsValid())
		{
			OutData->SetStringField(TEXT("file_path"), FrameSheetPath);
			OutData->SetNumberField(TEXT("width"), FrameSheetData->GetNumberField(TEXT("width")));
			OutData->SetNumberField(TEXT("height"), FrameSheetData->GetNumberField(TEXT("height")));
			OutData->SetNumberField(TEXT("bytes"), FrameSheetData->GetNumberField(TEXT("bytes")));
		}
		else
		{
			FString FirstFramePath;
			FirstFrameObject->TryGetStringField(TEXT("file_path"), FirstFramePath);
			OutData->SetStringField(TEXT("file_path"), FirstFramePath);
			OutData->SetNumberField(TEXT("width"), FirstFrameObject->GetNumberField(TEXT("width")));
			OutData->SetNumberField(TEXT("height"), FirstFrameObject->GetNumberField(TEXT("height")));
			double FirstFrameBytes = 0.0;
			FirstFrameObject->TryGetNumberField(TEXT("bytes"), FirstFrameBytes);
			OutData->SetNumberField(TEXT("bytes"), FirstFrameBytes);
		}
		OutData->SetStringField(TEXT("target"), EffectiveTarget);
		OutData->SetStringField(TEXT("capture_mode"), FrameSequenceCaptureMode);
		OutData->SetStringField(TEXT("legacy_backbuffer_capture"), TEXT("disabled"));
		OutData->SetStringField(TEXT("requested_capture_mode"), RequestedCaptureMode);
		OutData->SetStringField(TEXT("format"), Format);
		OutData->SetNumberField(TEXT("frame_index"), FirstFrameObject->GetNumberField(TEXT("frame_index")));
		OutData->SetNumberField(TEXT("target_frame"), FirstFrameObject->GetNumberField(TEXT("target_frame")));
		OutData->SetNumberField(TEXT("frame_count"), FrameRequests.Num());
		OutData->SetArrayField(TEXT("frames"), CapturedFrames);
		OutData->SetBoolField(TEXT("frame_sheet_created"), FrameSheetData.IsValid());
		OutData->SetBoolField(TEXT("individual_frame_files_created"), bWriteIndividualFrames);
		if (FrameSheetData.IsValid())
		{
			OutData->SetStringField(TEXT("frame_sheet_file_path"), FrameSheetPath);
			OutData->SetStringField(TEXT("sequence_frame_image_path"), FrameSheetPath);
			OutData->SetObjectField(TEXT("frame_sheet"), FrameSheetData);
		}
		OutData->SetStringField(TEXT("pixel_signature"), FirstFrameObject->GetStringField(TEXT("pixel_signature")));
		OutData->SetStringField(TEXT("frame_capture_renderer"), bUsePreviewSceneCapture ? TEXT("preview_scene_capture") : (bUseOffscreenRenderer ? TEXT("slate_widget_offscreen") : TEXT("slate_widget")));
		OutData->SetBoolField(TEXT("opened_editor_if_needed"), bOpenEditorIfNeeded);
		OutData->SetBoolField(TEXT("synchronized_overview_graph"), bSynchronizedOverviewGraph);
		OutData->SetNumberField(TEXT("overview_node_count_before"), OverviewNodeCountBefore);
		OutData->SetNumberField(TEXT("overview_node_count_after"), OverviewNodeCountAfter);
		OutData->SetBoolField(TEXT("offscreen_renderer"), bUseSlateFrameCapture && bUseOffscreenRenderer);
		OutData->SetBoolField(TEXT("scene_capture_renderer"), bUsePreviewSceneCapture);
		OutData->SetBoolField(TEXT("requested_offscreen_renderer"), bRequestedOffscreenRenderer);
		OutData->SetBoolField(TEXT("requested_offscreen_renderer_specified"), bUseOffscreenRendererSpecified);
		OutData->SetNumberField(TEXT("offscreen_draw_width"), FrameOffscreenDrawSize.X);
		OutData->SetNumberField(TEXT("offscreen_draw_height"), FrameOffscreenDrawSize.Y);
		OutData->SetNumberField(TEXT("capture_width"), FrameCaptureSize.X);
		OutData->SetNumberField(TEXT("capture_height"), FrameCaptureSize.Y);
		OutData->SetBoolField(TEXT("window_backbuffer_safe"), bWindowBackbufferSafe);
		OutData->SetStringField(TEXT("window_backbuffer_unsafe_reason"), WindowBackbufferUnsafeReason);
		OutData->SetBoolField(TEXT("preview_prepare_requested"), true);
		OutData->SetBoolField(TEXT("preview_prepared"), true);
		OutData->SetStringField(TEXT("sample_mode"), TEXT("frame_sequence"));
		OutData->SetStringField(TEXT("advance_semantics"), TEXT("continuous_from_zero"));
		OutData->SetStringField(TEXT("frame_sequence_capture_mode"), TEXT("continuous_from_zero_once"));
		OutData->SetBoolField(TEXT("mutated_preview_component"), true);
		OutData->SetBoolField(TEXT("preview_reset"), true);
		OutData->SetNumberField(TEXT("preview_tick_delta_seconds"), PreviewTickDeltaSeconds);
		OutData->SetStringField(TEXT("preview_advance_mode"), EffectiveFrameAdvanceMode);
		OutData->SetBoolField(TEXT("restore_preview_after_capture"), bRestorePreviewAfterCapture);
		OutData->SetBoolField(TEXT("preview_paused_after_advance"), !bRestorePreviewAfterCapture);
		if (PreviewRestoreReport.IsValid())
		{
			OutData->SetObjectField(TEXT("preview_restore"), PreviewRestoreReport);
		}
		OutData->SetBoolField(TEXT("was_package_dirty_before_capture"), bWasPackageDirtyBeforeFrameSequence);
		OutData->SetBoolField(TEXT("restored_dirty_state"), bRestoredDirtyState);
		if (SceneCaptureCameraInfo.IsValid())
		{
			OutData->SetObjectField(TEXT("scene_capture_camera"), SceneCaptureCameraInfo);
		}
		if (FirstFrameRenderDependencyWait.IsValid())
		{
			OutData->SetObjectField(TEXT("render_dependency_wait"), FirstFrameRenderDependencyWait);
		}
		OutData->SetStringField(TEXT("preview_stats_error"), PreviewStatsError);
		if (PreviewStats.IsValid())
		{
			OutData->SetObjectField(TEXT("preview_stats"), PreviewStats);
		}
		UeAgentEditorViewportSequenceFrameOptions::AttachUnifiedSequenceFrameOptionsReport(OutData, UnifiedOptionsReport);
		return true;
	}

	if (bPreviewPrepareRequested)
	{
		if (bUseOffscreenRenderer)
		{
			PrePreviewRedrawSkipReason = TEXT("offscreen_renderer");
		}
		else
		{
			bPrePreviewRedrawPerformed = UeAgentNiagaraOps::PrepareSlateWindowForCapture(NiagaraWindow, PrePreviewRedrawSkipReason);
		}
		bPreviewPrepared = UeAgentNiagaraOps::PrepareNiagaraPreviewForCapture(Asset, bResetPreview, PreviewAdvanceSeconds, PreviewTickDeltaSeconds, PreviewAdvanceMode, bPausePreviewAfterAdvance, PreviewPrepareError);
	}

	TSharedPtr<FJsonObject> CurrentPreviewValidation;
	bool bCurrentPreviewValidated = false;
	if (bCaptureCurrentPreview)
	{
		UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
		if (!NiagaraSystem)
		{
			OutError = TEXT("current_preview_capture_requires_niagara_system");
			return false;
		}

		TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
		UNiagaraComponent* PreviewComponent = nullptr;
		if (!UeAgentNiagaraOps::ResolveNiagaraSystemPreviewComponent(NiagaraSystem, bOpenEditorIfNeeded, PreviewComponent, SystemViewModel, OutError))
		{
			return false;
		}
		if (NiagaraSystem->HasOutstandingCompilationRequests(true))
		{
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
		}
		NiagaraSystem->CacheFromCompiledData();

		CurrentPreviewValidation = MakeShared<FJsonObject>();
		if (!UeAgentNiagaraOps::ValidateNiagaraPreviewStateRequest(Ctx.Params, PreviewComponent, static_cast<float>(PreviewTickDeltaSeconds), CurrentPreviewValidation, OutError))
		{
			OutData->SetStringField(TEXT("sample_mode"), TEXT("current_preview"));
			OutData->SetBoolField(TEXT("mutated_preview_component"), false);
			OutData->SetObjectField(TEXT("current_preview_validation"), CurrentPreviewValidation);
			return false;
		}
		bCurrentPreviewValidated = true;
	}

	auto RestorePreparedPreviewAfterFailure = [&]() -> void
	{
		if ((bPreviewPrepareRequested || bCaptureCurrentPreview) && bRestorePreviewAfterCapture)
		{
			if (UNiagaraSystem* NiagaraSystemForRestore = Cast<UNiagaraSystem>(Asset))
			{
				UeAgentNiagaraOps::RestoreNiagaraPreviewForInteractiveEditing(NiagaraSystemForRestore, TSharedPtr<FNiagaraSystemViewModel>());
			}
		}
	};

	bool bCaptureRedrawPerformed = false;
	FString CaptureRedrawSkipReason;
	if (!bUseOffscreenRenderer)
	{
		bCaptureRedrawPerformed = UeAgentNiagaraOps::PrepareSlateWindowForCapture(NiagaraWindow, CaptureRedrawSkipReason);
	}

	FString PreviewStatsError;
	TSharedPtr<FJsonObject> PreviewStats = UeAgentNiagaraOps::CollectNiagaraPreviewStats(Asset, PreviewStatsError);

	TArray<FColor> Pixels;
	FIntPoint ShotSize(0, 0);
	FVector2D OffscreenDrawSize(0.0, 0.0);
	if (bUseOffscreenRenderer)
	{
		CaptureMode = bCaptureCurrentPreview ? TEXT("current_preview_slate_widget_offscreen") : TEXT("slate_widget_offscreen");
		OffscreenDrawSize = UeAgentNiagaraOps::ResolveOffscreenDrawSize(WidgetToShot.ToSharedRef(), EffectiveTarget, Ctx.Params, MaxSize);
		if (!UeAgentNiagaraOps::ScreenshotSlateWidgetOffscreen(WidgetToShot.ToSharedRef(), OffscreenDrawSize, Pixels, ShotSize, OutError))
		{
			FUeAgentInterfaceLogger::Log(TEXT("CmdNiagaraScreenshot offscreen failed target=%s asset=%s error=%s"), *EffectiveTarget, *Asset->GetOutermost()->GetName(), *OutError);
			RestorePreparedPreviewAfterFailure();
			return false;
		}
	}
	else if (!UeAgentNiagaraOps::ScreenshotSlateWidget(WidgetToShot.ToSharedRef(), Pixels, ShotSize, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdNiagaraScreenshot failed target=%s asset=%s error=%s"), *EffectiveTarget, *Asset->GetOutermost()->GetName(), *OutError);
		RestorePreparedPreviewAfterFailure();
		return false;
	}
	else if (bCaptureCurrentPreview)
	{
		CaptureMode = TEXT("current_preview_slate_widget");
	}

	TArray<FColor> ResizedPixels;
	FIntPoint ResizedSize(0, 0);
	if (!ResizePixelsMaxSize(Pixels, ShotSize, MaxSize, ResizedPixels, ResizedSize, OutError))
	{
		RestorePreparedPreviewAfterFailure();
		return false;
	}

	const FString OutPath = UeAgentNiagaraOps::ResolveOutputScreenshotPath(Ctx.Params, Format);
	if (!WriteCompressedImage(ResizedPixels, ResizedSize, Format, Quality, OutPath, OutError))
	{
		RestorePreparedPreviewAfterFailure();
		return false;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*OutPath);
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetStringField(TEXT("target"), EffectiveTarget);
	OutData->SetStringField(TEXT("capture_mode"), CaptureMode);
	OutData->SetStringField(TEXT("legacy_backbuffer_capture"), TEXT("disabled"));
	OutData->SetStringField(TEXT("requested_capture_mode"), bCaptureCurrentPreview ? TEXT("current_preview") : RequestedCaptureMode);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
	OutData->SetBoolField(TEXT("opened_editor_if_needed"), bOpenEditorIfNeeded);
	OutData->SetBoolField(TEXT("synchronized_overview_graph"), bSynchronizedOverviewGraph);
	OutData->SetNumberField(TEXT("overview_node_count_before"), OverviewNodeCountBefore);
	OutData->SetNumberField(TEXT("overview_node_count_after"), OverviewNodeCountAfter);
	OutData->SetBoolField(TEXT("offscreen_renderer"), bUseOffscreenRenderer);
	OutData->SetBoolField(TEXT("requested_offscreen_renderer"), bRequestedOffscreenRenderer);
	OutData->SetBoolField(TEXT("requested_offscreen_renderer_specified"), bUseOffscreenRendererSpecified);
	OutData->SetNumberField(TEXT("offscreen_draw_width"), OffscreenDrawSize.X);
	OutData->SetNumberField(TEXT("offscreen_draw_height"), OffscreenDrawSize.Y);
	OutData->SetBoolField(TEXT("window_backbuffer_safe"), bWindowBackbufferSafe);
	OutData->SetStringField(TEXT("window_backbuffer_unsafe_reason"), WindowBackbufferUnsafeReason);
	OutData->SetBoolField(TEXT("pre_preview_redraw_performed"), bPrePreviewRedrawPerformed);
	OutData->SetStringField(TEXT("pre_preview_redraw_skip_reason"), PrePreviewRedrawSkipReason);
	OutData->SetBoolField(TEXT("capture_redraw_performed"), bCaptureRedrawPerformed);
	OutData->SetStringField(TEXT("capture_redraw_skip_reason"), CaptureRedrawSkipReason);
	OutData->SetBoolField(TEXT("preview_prepare_requested"), bPreviewPrepareRequested);
	OutData->SetBoolField(TEXT("preview_prepared"), bPreviewPrepared);
	OutData->SetStringField(TEXT("preview_prepare_error"), PreviewPrepareError);
	OutData->SetStringField(TEXT("sample_mode"), bCaptureCurrentPreview ? TEXT("current_preview") : TEXT("capture_after_optional_prepare"));
	OutData->SetBoolField(TEXT("current_preview_validated"), bCurrentPreviewValidated);
	OutData->SetBoolField(TEXT("mutated_preview_component"), bPreviewPrepareRequested);
	OutData->SetBoolField(TEXT("preview_reset"), bResetPreview);
	OutData->SetNumberField(TEXT("preview_advance_seconds"), PreviewAdvanceSeconds);
	OutData->SetNumberField(TEXT("preview_tick_delta_seconds"), PreviewTickDeltaSeconds);
	OutData->SetStringField(TEXT("preview_advance_mode"), PreviewAdvanceMode);
	OutData->SetBoolField(TEXT("preview_paused_after_advance"), bPausePreviewAfterAdvance);
	OutData->SetStringField(TEXT("preview_stats_error"), PreviewStatsError);
	if (CurrentPreviewValidation.IsValid())
	{
		OutData->SetObjectField(TEXT("current_preview_validation"), CurrentPreviewValidation);
	}
	if (PreviewStats.IsValid())
	{
		OutData->SetObjectField(TEXT("preview_stats"), PreviewStats);
	}
	if ((bPreviewPrepareRequested || bCaptureCurrentPreview) && bRestorePreviewAfterCapture)
	{
		if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
		{
			TSharedPtr<FJsonObject> PreviewRestoreReport = UeAgentNiagaraOps::RestoreNiagaraPreviewForInteractiveEditing(NiagaraSystem, TSharedPtr<FNiagaraSystemViewModel>());
			OutData->SetObjectField(TEXT("preview_restore"), PreviewRestoreReport);
		}
	}
	OutData->SetBoolField(TEXT("restore_preview_after_capture"), bRestorePreviewAfterCapture);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
	if (!NiagaraSystem)
	{
		OutError = TEXT("asset_is_not_niagara_system");
		return false;
	}

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	if (NiagaraSystem->HasOutstandingCompilationRequests(true))
	{
		NiagaraSystem->WaitForCompilationComplete(true, false);
		NiagaraSystem->PollForCompilationComplete(true);
	}
	NiagaraSystem->CacheFromCompiledData();
	UPackage* NiagaraSystemPackage = NiagaraSystem->GetOutermost();
	const bool bWasPackageDirtyBeforeProbe = NiagaraSystemPackage ? NiagaraSystemPackage->IsDirty() : false;

	FString SampleMode = TEXT("simulate");
	JsonTryGetString(Ctx.Params, TEXT("sample_mode"), SampleMode);
	JsonTryGetString(Ctx.Params, TEXT("probe_mode"), SampleMode);
	SampleMode.TrimStartAndEndInline();
	if (SampleMode.IsEmpty())
	{
		SampleMode = TEXT("simulate");
	}
	const FString NormalizedSampleMode = SampleMode.ToLower();
	const bool bSampleCurrentPreview =
		NormalizedSampleMode == TEXT("current_preview") ||
		NormalizedSampleMode == TEXT("current_preview_readonly") ||
		NormalizedSampleMode == TEXT("readonly_current_preview");

	bool bUseTransientComponent = false;
	JsonTryGetBool(Ctx.Params, TEXT("use_transient_component"), bUseTransientComponent);
	if (!bUseTransientComponent)
	{
		JsonTryGetBool(Ctx.Params, TEXT("use_transient_preview_component"), bUseTransientComponent);
	}
	if (bSampleCurrentPreview && bUseTransientComponent)
	{
		OutError = TEXT("current_preview_sample_does_not_support_transient_component");
		return false;
	}

	TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
	UNiagaraComponent* PreviewComponent = nullptr;
	if (bUseTransientComponent)
	{
		if (!GEditor)
		{
			OutError = TEXT("editor_unavailable");
			return false;
		}
		UWorld* PreviewWorld = GEditor->GetEditorWorldContext().World();
		if (!PreviewWorld)
		{
			OutError = TEXT("editor_world_unavailable");
			return false;
		}

		PreviewComponent = NewObject<UNiagaraComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!PreviewComponent)
		{
			OutError = TEXT("transient_niagara_component_create_failed");
			return false;
		}
		PreviewComponent->AddToRoot();
		PreviewComponent->SetAutoActivate(false);
		PreviewComponent->SetAsset(NiagaraSystem, false);
		PreviewComponent->RegisterComponentWithWorld(PreviewWorld);
	}
	else
	{
		IAssetEditorInstance* AssetEditor = UeAgentNiagaraOps::GetNiagaraAssetEditor(Asset, bOpenEditorIfNeeded, OutError);
		if (!AssetEditor)
		{
			return false;
		}

		FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
		if (!NiagaraEditorModule)
		{
			OutError = TEXT("niagara_editor_module_not_loaded");
			return false;
		}

		SystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
		if (!SystemViewModel.IsValid())
		{
			OutError = TEXT("niagara_system_view_model_not_found");
			return false;
		}

		PreviewComponent = SystemViewModel->GetPreviewComponent();
		if (!PreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_not_found");
			return false;
		}
	}
	if (NiagaraSystem->HasOutstandingCompilationRequests(true))
	{
		NiagaraSystem->WaitForCompilationComplete(true, false);
		NiagaraSystem->PollForCompilationComplete(true);
	}
	NiagaraSystem->CacheFromCompiledData();

	bool bResetPreview = true;
	JsonTryGetBool(Ctx.Params, TEXT("reset_preview"), bResetPreview);
	if (bSampleCurrentPreview)
	{
		bResetPreview = false;
	}

	bool bDeactivateBeforeReset = true;
	JsonTryGetBool(Ctx.Params, TEXT("deactivate_before_reset"), bDeactivateBeforeReset);

	bool bIncludeEachTick = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_each_tick"), bIncludeEachTick);

	bool bIncludeSnapshots = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_snapshots"), bIncludeSnapshots);

	bool bPauseAfterProbe = false;
	JsonTryGetBool(Ctx.Params, TEXT("pause_after_probe"), bPauseAfterProbe);
	bool bRestorePreviewAfterProbe = !bSampleCurrentPreview && !bPauseAfterProbe;
	UeAgentNiagaraOps::TryGetBoolAnyField(
		Ctx.Params,
		{
			TEXT("restore_preview_after_probe"),
			TEXT("resume_preview_after_probe"),
			TEXT("restore_interactive_preview"),
			TEXT("resume_interactive_preview")
		},
		bRestorePreviewAfterProbe);

	FString AdvanceMode = TEXT("advance_simulation");
	JsonTryGetString(Ctx.Params, TEXT("advance_mode"), AdvanceMode);
	AdvanceMode.TrimStartAndEndInline();
	if (AdvanceMode.IsEmpty())
	{
		AdvanceMode = TEXT("advance_simulation");
	}
	const FString NormalizedAdvanceMode = AdvanceMode.ToLower();
	const bool bUseAdvanceSimulation =
		NormalizedAdvanceMode == TEXT("advance") ||
		NormalizedAdvanceMode == TEXT("advance_simulation") ||
		NormalizedAdvanceMode == TEXT("advance_simulation_by_time");
	const bool bUseTickComponent =
		NormalizedAdvanceMode == TEXT("tick") ||
		NormalizedAdvanceMode == TEXT("tick_component") ||
		NormalizedAdvanceMode == TEXT("tick_delta") ||
		NormalizedAdvanceMode == TEXT("tick_delta_time");

	double TickDeltaNumber = 1.0 / 60.0;
	JsonTryGetNumber(Ctx.Params, TEXT("tick_delta_seconds"), TickDeltaNumber);
	const float TickDeltaSeconds = FMath::Clamp(static_cast<float>(TickDeltaNumber), 1.0f / 240.0f, 1.0f / 5.0f);

	double TickCountNumber = 3.0;
	JsonTryGetNumber(Ctx.Params, TEXT("tick_count"), TickCountNumber);
	const int32 TickCount = FMath::Clamp(FMath::RoundToInt(TickCountNumber), 0, 240);

	auto ConfigurePreviewComponent = [TickDeltaSeconds, NiagaraSystem](UNiagaraComponent* Component)
	{
		if (!Component)
		{
			return;
		}
		Component->SetAsset(NiagaraSystem, false);
		Component->SetForceSolo(true);
		Component->SetAgeUpdateMode(ENiagaraAgeUpdateMode::TickDeltaTime);
		Component->SetSeekDelta(TickDeltaSeconds);
		Component->SetLockDesiredAgeDeltaTimeToSeekDelta(true);
		Component->SetCanRenderWhileSeeking(true);
		Component->SetAllowScalability(false);
		Component->SetRenderingEnabled(true);
	};

	struct FEmitterProbePeak
	{
		int32 MaxParticleCount = 0;
		int32 MaxTotalSpawnedParticles = 0;
		int32 MaxSpawnInfoTotalCount = 0;
		FString MaxParticleCountLabel;
		FString MaxTotalSpawnedParticlesLabel;
		FString MaxSpawnInfoTotalCountLabel;
	};

	TArray<TSharedPtr<FJsonValue>> Snapshots;
	TMap<FString, FEmitterProbePeak> EmitterPeaks;
	int32 CollectedSnapshotCount = 0;
	FString FirstSnapshotLabel;
	FString FinalSnapshotLabel;
	TSharedPtr<FJsonObject> InitialStats;
	TSharedPtr<FJsonObject> FinalStats;

	auto UpdateEmitterPeaks = [&EmitterPeaks](const FString& Label, const TSharedPtr<FJsonObject>& Stats)
	{
		if (!Stats.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
		if (!Stats->TryGetArrayField(TEXT("emitters"), Emitters) || !Emitters)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& EmitterValue : *Emitters)
		{
			const TSharedPtr<FJsonObject> EmitterObj = EmitterValue.IsValid() ? EmitterValue->AsObject() : nullptr;
			if (!EmitterObj.IsValid())
			{
				continue;
			}

			FString EmitterName;
			if (!EmitterObj->TryGetStringField(TEXT("name"), EmitterName) || EmitterName.IsEmpty())
			{
				continue;
			}

			FEmitterProbePeak& Peak = EmitterPeaks.FindOrAdd(EmitterName);
			double NumberValue = 0.0;
			if (EmitterObj->TryGetNumberField(TEXT("particle_count"), NumberValue))
			{
				const int32 ParticleCount = FMath::RoundToInt(NumberValue);
				if (ParticleCount > Peak.MaxParticleCount)
				{
					Peak.MaxParticleCount = ParticleCount;
					Peak.MaxParticleCountLabel = Label;
				}
			}
			if (EmitterObj->TryGetNumberField(TEXT("total_spawned_particles"), NumberValue))
			{
				const int32 TotalSpawnedParticles = FMath::RoundToInt(NumberValue);
				if (TotalSpawnedParticles > Peak.MaxTotalSpawnedParticles)
				{
					Peak.MaxTotalSpawnedParticles = TotalSpawnedParticles;
					Peak.MaxTotalSpawnedParticlesLabel = Label;
				}
			}
			if (EmitterObj->TryGetNumberField(TEXT("spawn_info_total_count"), NumberValue))
			{
				const int32 SpawnInfoTotalCount = FMath::RoundToInt(NumberValue);
				if (SpawnInfoTotalCount > Peak.MaxSpawnInfoTotalCount)
				{
					Peak.MaxSpawnInfoTotalCount = SpawnInfoTotalCount;
					Peak.MaxSpawnInfoTotalCountLabel = Label;
				}
			}
		}
	};

	auto AddSnapshot = [&Snapshots, &EmitterPeaks, &CollectedSnapshotCount, &FirstSnapshotLabel, &FinalSnapshotLabel, &InitialStats, &FinalStats, &UpdateEmitterPeaks, bIncludeSnapshots, NiagaraSystem, &PreviewComponent](const FString& Label)
	{
		FString StatsError;
		TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetStringField(TEXT("label"), Label);
		TSharedPtr<FJsonObject> Stats = UeAgentNiagaraOps::CollectNiagaraComponentStats(NiagaraSystem, PreviewComponent, StatsError);
		Snapshot->SetStringField(TEXT("stats_error"), StatsError);
		if (Stats.IsValid())
		{
			Snapshot->SetObjectField(TEXT("stats"), Stats);
			UpdateEmitterPeaks(Label, Stats);
			if (!InitialStats.IsValid())
			{
				InitialStats = Stats;
			}
			FinalStats = Stats;
		}
		++CollectedSnapshotCount;
		if (FirstSnapshotLabel.IsEmpty())
		{
			FirstSnapshotLabel = Label;
		}
		FinalSnapshotLabel = Label;
		if (bIncludeSnapshots)
		{
			Snapshots.Add(MakeShared<FJsonValueObject>(Snapshot));
		}
	};

	if (bSampleCurrentPreview)
	{
		TSharedPtr<FJsonObject> CurrentPreviewValidation = MakeShared<FJsonObject>();
		if (!UeAgentNiagaraOps::ValidateNiagaraPreviewStateRequest(Ctx.Params, PreviewComponent, TickDeltaSeconds, CurrentPreviewValidation, OutError))
		{
			OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
			OutData->SetStringField(TEXT("sample_mode"), TEXT("current_preview"));
			OutData->SetStringField(TEXT("component_source"), TEXT("editor_preview_component"));
			OutData->SetBoolField(TEXT("mutated_preview_component"), false);
			OutData->SetBoolField(TEXT("used_seek"), false);
			OutData->SetObjectField(TEXT("current_preview_validation"), CurrentPreviewValidation);
			return false;
		}

		FString StatsError;
		TSharedPtr<FJsonObject> Stats = UeAgentNiagaraOps::CollectNiagaraComponentStats(NiagaraSystem, PreviewComponent, StatsError);
		if (Stats.IsValid())
		{
			UpdateEmitterPeaks(TEXT("current_preview"), Stats);
			InitialStats = Stats;
			FinalStats = Stats;
			++CollectedSnapshotCount;
			FirstSnapshotLabel = TEXT("current_preview");
			FinalSnapshotLabel = TEXT("current_preview");
			if (bIncludeSnapshots)
			{
				TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
				Snapshot->SetStringField(TEXT("label"), TEXT("current_preview"));
				Snapshot->SetStringField(TEXT("stats_error"), StatsError);
				Snapshot->SetObjectField(TEXT("stats"), Stats);
				Snapshots.Add(MakeShared<FJsonValueObject>(Snapshot));
			}
		}

		TArray<TSharedPtr<FJsonValue>> EmitterPeakJson;
		for (const TPair<FString, FEmitterProbePeak>& Pair : EmitterPeaks)
		{
			TSharedPtr<FJsonObject> PeakObj = MakeShared<FJsonObject>();
			PeakObj->SetStringField(TEXT("name"), Pair.Key);
			PeakObj->SetNumberField(TEXT("max_particle_count"), Pair.Value.MaxParticleCount);
			PeakObj->SetStringField(TEXT("max_particle_count_label"), Pair.Value.MaxParticleCountLabel);
			PeakObj->SetNumberField(TEXT("max_total_spawned_particles"), Pair.Value.MaxTotalSpawnedParticles);
			PeakObj->SetStringField(TEXT("max_total_spawned_particles_label"), Pair.Value.MaxTotalSpawnedParticlesLabel);
			PeakObj->SetNumberField(TEXT("max_spawn_info_total_count"), Pair.Value.MaxSpawnInfoTotalCount);
			PeakObj->SetStringField(TEXT("max_spawn_info_total_count_label"), Pair.Value.MaxSpawnInfoTotalCountLabel);
			EmitterPeakJson.Add(MakeShared<FJsonValueObject>(PeakObj));
		}

		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("collected_snapshot_count"), CollectedSnapshotCount);
		Summary->SetNumberField(TEXT("emitted_snapshot_count"), Snapshots.Num());
		Summary->SetStringField(TEXT("first_snapshot_label"), FirstSnapshotLabel);
		Summary->SetStringField(TEXT("final_snapshot_label"), FinalSnapshotLabel);
		Summary->SetArrayField(TEXT("emitter_peaks"), EmitterPeakJson);

		OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
		OutData->SetStringField(TEXT("sample_mode"), TEXT("current_preview"));
		OutData->SetStringField(TEXT("component_source"), TEXT("editor_preview_component"));
		OutData->SetStringField(TEXT("component_name"), PreviewComponent->GetName());
		OutData->SetStringField(TEXT("component_path"), PreviewComponent->GetPathName());
		OutData->SetStringField(TEXT("component_world"), PreviewComponent->GetWorld() ? PreviewComponent->GetWorld()->GetPathName() : FString());
		OutData->SetStringField(TEXT("advance_mode"), TEXT("none"));
		OutData->SetStringField(TEXT("advance_semantics"), TEXT("readonly_current_preview"));
		OutData->SetNumberField(TEXT("tick_count"), 0);
		OutData->SetNumberField(TEXT("tick_delta_seconds"), TickDeltaSeconds);
		OutData->SetBoolField(TEXT("reset_preview"), false);
		OutData->SetBoolField(TEXT("include_snapshots"), bIncludeSnapshots);
		OutData->SetBoolField(TEXT("mutated_preview_component"), false);
		OutData->SetBoolField(TEXT("used_seek"), false);
		OutData->SetStringField(TEXT("stats_error"), StatsError);
		OutData->SetObjectField(TEXT("current_preview_validation"), CurrentPreviewValidation);
		OutData->SetBoolField(TEXT("restore_preview_after_probe"), bRestorePreviewAfterProbe);
		if (bRestorePreviewAfterProbe)
		{
			TSharedPtr<FJsonObject> PreviewRestoreReport = UeAgentNiagaraOps::RestoreNiagaraPreviewForInteractiveEditing(NiagaraSystem, SystemViewModel);
			OutData->SetObjectField(TEXT("preview_restore"), PreviewRestoreReport);
		}
		if (InitialStats.IsValid())
		{
			OutData->SetObjectField(TEXT("initial_stats"), InitialStats);
		}
		if (FinalStats.IsValid())
		{
			OutData->SetObjectField(TEXT("final_stats"), FinalStats);
		}
		OutData->SetObjectField(TEXT("summary"), Summary);
		OutData->SetArrayField(TEXT("snapshots"), Snapshots);
		OutData->SetNumberField(TEXT("snapshot_count"), CollectedSnapshotCount);
		OutData->SetNumberField(TEXT("snapshots_emitted_count"), Snapshots.Num());
		if (!Stats.IsValid())
		{
			OutError = StatsError.IsEmpty() ? TEXT("collect_current_preview_stats_failed") : StatsError;
		}
		return Stats.IsValid();
	}

	UeAgentNiagaraOps::SetNiagaraPreviewStateToken(PreviewComponent, FString());
	ConfigurePreviewComponent(PreviewComponent);
	AddSnapshot(TEXT("after_configure_before_reset"));

	if (bResetPreview)
	{
		if (bDeactivateBeforeReset)
		{
			PreviewComponent->DeactivateImmediate();
			UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
			AddSnapshot(TEXT("after_deactivate_immediate"));
		}

		PreviewComponent->SetDesiredAge(0.0f);
		PreviewComponent->ReinitializeSystem();
		if (!bUseTransientComponent)
		{
			PreviewComponent = SystemViewModel->GetPreviewComponent();
			if (!PreviewComponent)
			{
				OutError = TEXT("niagara_preview_component_missing_after_reset");
				return false;
			}
		}
		ConfigurePreviewComponent(PreviewComponent);
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		AddSnapshot(TEXT("after_reinitialize"));
	}

	const bool bWasActiveBeforeActivation = PreviewComponent->IsActive();
	PreviewComponent->SetPaused(false);
	PreviewComponent->Activate(false);
	UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
	AddSnapshot(TEXT("after_activate_before_tick"));

	for (int32 TickIndex = 0; TickIndex < TickCount; ++TickIndex)
	{
		if (bUseAdvanceSimulation)
		{
			PreviewComponent->AdvanceSimulation(1, TickDeltaSeconds);
		}
		else
		{
			PreviewComponent->TickComponent(TickDeltaSeconds, ELevelTick::LEVELTICK_All, nullptr);
		}
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		if (bIncludeEachTick || TickIndex == TickCount - 1)
		{
			AddSnapshot(FString::Printf(TEXT("after_%s_%d"), bUseAdvanceSimulation ? TEXT("advance") : TEXT("tick"), TickIndex + 1));
		}
	}

	if (bPauseAfterProbe)
	{
		PreviewComponent->SetPaused(true);
	}
	PreviewComponent->MarkRenderStateDirty();
	TSharedPtr<FJsonObject> PreviewRestoreReport;
	if (!bUseTransientComponent && bRestorePreviewAfterProbe)
	{
		PreviewRestoreReport = UeAgentNiagaraOps::RestoreNiagaraPreviewForInteractiveEditing(NiagaraSystem, SystemViewModel);
	}
	bool bRestoredDirtyState = false;
	if (NiagaraSystemPackage && !bWasPackageDirtyBeforeProbe && NiagaraSystemPackage->IsDirty())
	{
		NiagaraSystemPackage->SetDirtyFlag(false);
		bRestoredDirtyState = true;
	}

	TArray<TSharedPtr<FJsonValue>> EmitterPeakJson;
	for (const TPair<FString, FEmitterProbePeak>& Pair : EmitterPeaks)
	{
		TSharedPtr<FJsonObject> PeakObj = MakeShared<FJsonObject>();
		PeakObj->SetStringField(TEXT("name"), Pair.Key);
		PeakObj->SetNumberField(TEXT("max_particle_count"), Pair.Value.MaxParticleCount);
		PeakObj->SetStringField(TEXT("max_particle_count_label"), Pair.Value.MaxParticleCountLabel);
		PeakObj->SetNumberField(TEXT("max_total_spawned_particles"), Pair.Value.MaxTotalSpawnedParticles);
		PeakObj->SetStringField(TEXT("max_total_spawned_particles_label"), Pair.Value.MaxTotalSpawnedParticlesLabel);
		PeakObj->SetNumberField(TEXT("max_spawn_info_total_count"), Pair.Value.MaxSpawnInfoTotalCount);
		PeakObj->SetStringField(TEXT("max_spawn_info_total_count_label"), Pair.Value.MaxSpawnInfoTotalCountLabel);
		EmitterPeakJson.Add(MakeShared<FJsonValueObject>(PeakObj));
	}

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("collected_snapshot_count"), CollectedSnapshotCount);
	Summary->SetNumberField(TEXT("emitted_snapshot_count"), Snapshots.Num());
	Summary->SetStringField(TEXT("first_snapshot_label"), FirstSnapshotLabel);
	Summary->SetStringField(TEXT("final_snapshot_label"), FinalSnapshotLabel);
	Summary->SetArrayField(TEXT("emitter_peaks"), EmitterPeakJson);

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetStringField(TEXT("sample_mode"), TEXT("simulate"));
	OutData->SetStringField(TEXT("component_source"), bUseTransientComponent ? TEXT("transient_component") : TEXT("editor_preview_component"));
	OutData->SetNumberField(TEXT("tick_count"), TickCount);
	OutData->SetNumberField(TEXT("tick_delta_seconds"), TickDeltaSeconds);
	OutData->SetStringField(TEXT("advance_mode"), bUseAdvanceSimulation ? TEXT("advance_simulation") : (bUseTickComponent ? TEXT("tick_component") : NormalizedAdvanceMode));
	OutData->SetStringField(TEXT("advance_semantics"), bResetPreview ? TEXT("continuous_from_zero") : TEXT("continuous_from_current"));
	OutData->SetBoolField(TEXT("mutated_preview_component"), true);
	OutData->SetBoolField(TEXT("used_seek"), false);
	OutData->SetBoolField(TEXT("reset_preview"), bResetPreview);
	OutData->SetBoolField(TEXT("deactivate_before_reset"), bDeactivateBeforeReset);
	OutData->SetBoolField(TEXT("include_snapshots"), bIncludeSnapshots);
	OutData->SetBoolField(TEXT("pause_after_probe"), bPauseAfterProbe);
	OutData->SetBoolField(TEXT("restore_preview_after_probe"), bRestorePreviewAfterProbe);
	OutData->SetBoolField(TEXT("component_active_before_activation"), bWasActiveBeforeActivation);
	OutData->SetBoolField(TEXT("activate_reset"), false);
	OutData->SetBoolField(TEXT("was_package_dirty_before_probe"), bWasPackageDirtyBeforeProbe);
	OutData->SetBoolField(TEXT("restored_dirty_state"), bRestoredDirtyState);
	if (PreviewRestoreReport.IsValid())
	{
		OutData->SetObjectField(TEXT("preview_restore"), PreviewRestoreReport);
	}
	if (InitialStats.IsValid())
	{
		OutData->SetObjectField(TEXT("initial_stats"), InitialStats);
	}
	if (FinalStats.IsValid())
	{
		OutData->SetObjectField(TEXT("final_stats"), FinalStats);
	}
	OutData->SetObjectField(TEXT("summary"), Summary);
	OutData->SetArrayField(TEXT("snapshots"), Snapshots);
	OutData->SetNumberField(TEXT("snapshot_count"), CollectedSnapshotCount);
	OutData->SetNumberField(TEXT("snapshots_emitted_count"), Snapshots.Num());
	if (bUseTransientComponent && PreviewComponent)
	{
		PreviewComponent->DeactivateImmediate();
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		PreviewComponent->UnregisterComponent();
		PreviewComponent->RemoveFromRoot();
		PreviewComponent->DestroyComponent();
	}
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraGetStackIssues(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	FString SeverityFilter = TEXT("warning_or_error");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilter);

	bool bCompileBeforeRead = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_before_read"), bCompileBeforeRead);
	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);

	bool bPreferExistingViewModel = true;
	JsonTryGetBool(Ctx.Params, TEXT("prefer_existing_view_model"), bPreferExistingViewModel);

	bool bOpenEditorIfNeeded = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	bool bIncludeSystemScope = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_system_scope"), bIncludeSystemScope);

	bool bIncludeEmitters = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_emitters"), bIncludeEmitters);

	FString EmitterNameFilter;
	const bool bHasEmitterNameFilter = JsonTryGetString(Ctx.Params, TEXT("emitter_name"), EmitterNameFilter) && !EmitterNameFilter.IsEmpty();

	FGuid EmitterIdFilter;
	FString EmitterIdFilterText;
	const bool bHasEmitterIdFilter =
		JsonTryGetString(Ctx.Params, TEXT("emitter_id"), EmitterIdFilterText) &&
		!EmitterIdFilterText.IsEmpty() &&
		FGuid::Parse(EmitterIdFilterText, EmitterIdFilter);

	int32 EmitterIndexFilter = INDEX_NONE;
	double EmitterIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("emitter_index"), EmitterIndexNumber))
	{
		EmitterIndexFilter = FMath::RoundToInt(EmitterIndexNumber);
	}

	TArray<TSharedPtr<FJsonValue>> ScopeJson;
	TArray<TSharedPtr<FJsonValue>> IssuesJson;
	UeAgentNiagaraOps::FNiagaraStackIssueCounts TotalIssueCounts;
	UeAgentNiagaraOps::FNiagaraStackIssueCounts ReturnedIssueCounts;
	FString AssetKind;
	FString ViewModelSource;
	UeAgentNiagaraOps::FNiagaraCompileAfterSetResult StackCompileResult;

	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
	{
		AssetKind = TEXT("system");
		TSharedPtr<FNiagaraSystemViewModel> TemporarySystemViewModel;
		TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
		if (bPreferExistingViewModel)
		{
			if (bOpenEditorIfNeeded)
			{
				if (!UeAgentNiagaraOps::GetNiagaraAssetEditor(Asset, true, OutError))
				{
					return false;
				}
			}

			if (FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor")))
			{
				SystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
				if (SystemViewModel.IsValid())
				{
					ViewModelSource = TEXT("existing_view_model");
				}
			}
		}
		if (!SystemViewModel.IsValid())
		{
			TemporarySystemViewModel = UeAgentNiagaraOps::CreateDataProcessingSystemViewModel(*NiagaraSystem);
			SystemViewModel = TemporarySystemViewModel;
			ViewModelSource = TEXT("data_processing_view_model");
		}

		if (!SystemViewModel.IsValid())
		{
			OutError = TEXT("niagara_system_view_model_create_failed");
			return false;
		}

		if (bCompileBeforeRead)
		{
			TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
			CompileParams->SetBoolField(TEXT("compile_after_set"), true);
			CompileParams->SetBoolField(TEXT("force_compile"), bForceCompile);
			CompileParams->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
			StackCompileResult = UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, CompileParams, true);
		}

		if (bIncludeSystemScope)
		{
			TArray<FNiagaraMessageSourceAndStore> SystemMessageStores;
			SystemMessageStores.Add(FNiagaraMessageSourceAndStore(*NiagaraSystem, NiagaraSystem->GetMessageStore()));
			TSharedPtr<FJsonObject> ScopeObj = UeAgentNiagaraOps::CollectNiagaraStackIssuesFromViewModel(
				SystemViewModel->GetSystemStackViewModel(),
				TEXT("system"),
				SystemViewModel->GetDisplayName().ToString(),
				FGuid(),
				SeverityFilter,
				&SystemMessageStores,
				IssuesJson,
				TotalIssueCounts,
				ReturnedIssueCounts);
			UeAgentNiagaraOps::AppendNiagaraSystemCompileStatus(ScopeObj, *NiagaraSystem);
			ScopeJson.Add(MakeShared<FJsonValueObject>(ScopeObj));
		}

		if (bIncludeEmitters)
		{
			const TArray<TSharedRef<FNiagaraEmitterHandleViewModel>>& EmitterHandleViewModels = SystemViewModel->GetEmitterHandleViewModels();
			for (int32 EmitterIndex = 0; EmitterIndex < EmitterHandleViewModels.Num(); ++EmitterIndex)
			{
				const TSharedRef<FNiagaraEmitterHandleViewModel>& EmitterHandleViewModel = EmitterHandleViewModels[EmitterIndex];
				const FString EmitterName = EmitterHandleViewModel->GetNameText().ToString();
				const FGuid EmitterHandleId = EmitterHandleViewModel->GetId();

				if (EmitterIndexFilter != INDEX_NONE && EmitterIndex != EmitterIndexFilter)
				{
					continue;
				}
				if (bHasEmitterNameFilter && EmitterName != EmitterNameFilter)
				{
					continue;
				}
				if (bHasEmitterIdFilter && EmitterHandleId != EmitterIdFilter)
				{
					continue;
				}

				TArray<FNiagaraMessageSourceAndStore> EmitterMessageStores;
				UNiagaraStackViewModel::FTopLevelViewModel EmitterTopLevelViewModel(EmitterHandleViewModel);
				EmitterTopLevelViewModel.GetMessageStores(EmitterMessageStores);
				TSharedPtr<FJsonObject> ScopeObj = UeAgentNiagaraOps::CollectNiagaraStackIssuesFromViewModel(
					EmitterHandleViewModel->GetEmitterStackViewModel(),
					TEXT("emitter"),
					EmitterName,
					EmitterHandleId,
					SeverityFilter,
					&EmitterMessageStores,
					IssuesJson,
					TotalIssueCounts,
					ReturnedIssueCounts);
				UeAgentNiagaraOps::AppendNiagaraEmitterHandleStatus(ScopeObj, EmitterHandleViewModel, EmitterIndex);
				ScopeJson.Add(MakeShared<FJsonValueObject>(ScopeObj));
			}
		}
	}
	else if (UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(Asset))
	{
		AssetKind = TEXT("emitter");
		UNiagaraSystem* TransientSystem = nullptr;
		TSharedPtr<FNiagaraSystemViewModel> SystemViewModel = UeAgentNiagaraOps::CreateDataProcessingEmitterViewModel(*NiagaraEmitter, TransientSystem);
		ViewModelSource = TEXT("data_processing_transient_system");
		if (!SystemViewModel.IsValid())
		{
			OutError = TEXT("niagara_emitter_view_model_create_failed");
			return false;
		}

		const TArray<TSharedRef<FNiagaraEmitterHandleViewModel>>& EmitterHandleViewModels = SystemViewModel->GetEmitterHandleViewModels();
		if (EmitterHandleViewModels.Num() == 0)
		{
			OutError = TEXT("niagara_emitter_view_model_missing_emitter_handle");
			return false;
		}

		const TSharedRef<FNiagaraEmitterHandleViewModel>& EmitterHandleViewModel = EmitterHandleViewModels[0];
		TArray<FNiagaraMessageSourceAndStore> EmitterMessageStores;
		UNiagaraStackViewModel::FTopLevelViewModel EmitterTopLevelViewModel(EmitterHandleViewModel);
		EmitterTopLevelViewModel.GetMessageStores(EmitterMessageStores);
		TSharedPtr<FJsonObject> ScopeObj = UeAgentNiagaraOps::CollectNiagaraStackIssuesFromViewModel(
			EmitterHandleViewModel->GetEmitterStackViewModel(),
			TEXT("emitter"),
			EmitterHandleViewModel->GetNameText().ToString(),
			EmitterHandleViewModel->GetId(),
			SeverityFilter,
			&EmitterMessageStores,
			IssuesJson,
			TotalIssueCounts,
			ReturnedIssueCounts);
		UeAgentNiagaraOps::AppendNiagaraEmitterHandleStatus(ScopeObj, EmitterHandleViewModel, 0);
		ScopeJson.Add(MakeShared<FJsonValueObject>(ScopeObj));

		OutData->SetStringField(TEXT("transient_system_path"), TransientSystem ? TransientSystem->GetPathName() : FString());
	}
	else
	{
		OutError = TEXT("asset_is_not_niagara_system_or_emitter");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_kind"), AssetKind);
	OutData->SetStringField(TEXT("view_model_source"), ViewModelSource);
	OutData->SetStringField(TEXT("severity_filter"), SeverityFilter);
	OutData->SetBoolField(TEXT("compile_before_read"), bCompileBeforeRead);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_requested"), StackCompileResult.bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), StackCompileResult.bCompilationComplete);
	OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), StackCompileResult.bHasOutstandingCompilationRequests);
	OutData->SetBoolField(TEXT("prefer_existing_view_model"), bPreferExistingViewModel);
	OutData->SetBoolField(TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	OutData->SetArrayField(TEXT("scopes"), ScopeJson);
	OutData->SetNumberField(TEXT("scope_count"), ScopeJson.Num());
	OutData->SetArrayField(TEXT("issues"), IssuesJson);
	OutData->SetNumberField(TEXT("returned_issue_count"), IssuesJson.Num());
	TotalIssueCounts.AppendToJson(OutData, TEXT("total_"));
	ReturnedIssueCounts.AppendToJson(OutData, TEXT("returned_"));
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraApplyStackIssueFix(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
	if (!NiagaraSystem)
	{
		OutError = TEXT("asset_is_not_niagara_system");
		return false;
	}

	FString IssueUniqueIdentifier;
	const bool bHasIssueUniqueIdentifier =
		JsonTryGetString(Ctx.Params, TEXT("issue_unique_identifier"), IssueUniqueIdentifier) &&
		!IssueUniqueIdentifier.IsEmpty();

	FString ModuleNodeGuidText;
	FGuid ModuleNodeGuid;
	const bool bHasModuleNodeGuid =
		JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) &&
		!ModuleNodeGuidText.IsEmpty() &&
		FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid);
	if (!ModuleNodeGuidText.IsEmpty() && !bHasModuleNodeGuid)
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	FString ShortDescriptionContains;
	const bool bHasShortDescriptionContains =
		JsonTryGetString(Ctx.Params, TEXT("short_description_contains"), ShortDescriptionContains) &&
		!ShortDescriptionContains.IsEmpty();

	FString LongDescriptionContains;
	const bool bHasLongDescriptionContains =
		JsonTryGetString(Ctx.Params, TEXT("long_description_contains"), LongDescriptionContains) &&
		!LongDescriptionContains.IsEmpty();

	FString EntryPathContains;
	const bool bHasEntryPathContains =
		JsonTryGetString(Ctx.Params, TEXT("entry_path_contains"), EntryPathContains) &&
		!EntryPathContains.IsEmpty();

	bool bApplyFirstMatch = false;
	JsonTryGetBool(Ctx.Params, TEXT("apply_first_match"), bApplyFirstMatch);
	if (!bHasIssueUniqueIdentifier && !bHasModuleNodeGuid && !bHasShortDescriptionContains && !bHasLongDescriptionContains && !bHasEntryPathContains && !bApplyFirstMatch)
	{
		OutError = TEXT("missing_stack_issue_selector");
		return false;
	}

	FString FixUniqueIdentifier;
	const bool bHasFixUniqueIdentifier =
		JsonTryGetString(Ctx.Params, TEXT("fix_unique_identifier"), FixUniqueIdentifier) &&
		!FixUniqueIdentifier.IsEmpty();

	FString FixDescriptionContains;
	const bool bHasFixDescriptionContains =
		JsonTryGetString(Ctx.Params, TEXT("fix_description_contains"), FixDescriptionContains) &&
		!FixDescriptionContains.IsEmpty();

	int32 RequestedFixIndex = 0;
	double FixIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("fix_index"), FixIndexNumber))
	{
		RequestedFixIndex = FMath::RoundToInt(FixIndexNumber);
	}

	bool bAllowLinkFix = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_link_fix"), bAllowLinkFix);

	FString SeverityFilter = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilter);

	bool bPreferExistingViewModel = true;
	JsonTryGetBool(Ctx.Params, TEXT("prefer_existing_view_model"), bPreferExistingViewModel);

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	bool bIncludeSystemScope = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_system_scope"), bIncludeSystemScope);

	bool bIncludeEmitters = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_emitters"), bIncludeEmitters);

	FString ScopeTypeFilter;
	const bool bHasScopeTypeFilter =
		JsonTryGetString(Ctx.Params, TEXT("scope_type"), ScopeTypeFilter) &&
		!ScopeTypeFilter.IsEmpty();

	FString EmitterNameFilter;
	const bool bHasEmitterNameFilter =
		JsonTryGetString(Ctx.Params, TEXT("emitter_name"), EmitterNameFilter) &&
		!EmitterNameFilter.IsEmpty();

	FGuid EmitterIdFilter;
	FString EmitterIdFilterText;
	const bool bHasEmitterIdFilter =
		JsonTryGetString(Ctx.Params, TEXT("emitter_id"), EmitterIdFilterText) &&
		!EmitterIdFilterText.IsEmpty() &&
		FGuid::Parse(EmitterIdFilterText, EmitterIdFilter);
	if (!EmitterIdFilterText.IsEmpty() && !bHasEmitterIdFilter)
	{
		OutError = TEXT("invalid_emitter_id");
		return false;
	}

	int32 EmitterIndexFilter = INDEX_NONE;
	double EmitterIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("emitter_index"), EmitterIndexNumber))
	{
		EmitterIndexFilter = FMath::RoundToInt(EmitterIndexNumber);
	}

	TSharedPtr<FNiagaraSystemViewModel> TemporarySystemViewModel;
	TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
	FString ViewModelSource;
	if (bPreferExistingViewModel)
	{
		if (bOpenEditorIfNeeded)
		{
			if (!UeAgentNiagaraOps::GetNiagaraAssetEditor(Asset, true, OutError))
			{
				return false;
			}
		}

		if (FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor")))
		{
			SystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
			if (SystemViewModel.IsValid())
			{
				ViewModelSource = TEXT("existing_view_model");
			}
		}
	}
	if (!SystemViewModel.IsValid())
	{
		TemporarySystemViewModel = UeAgentNiagaraOps::CreateDataProcessingSystemViewModel(*NiagaraSystem);
		SystemViewModel = TemporarySystemViewModel;
		ViewModelSource = TEXT("data_processing_view_model");
	}
	if (!SystemViewModel.IsValid())
	{
		OutError = TEXT("niagara_system_view_model_create_failed");
		return false;
	}

	struct FMatchedStackIssueFix
	{
		UNiagaraStackViewModel* StackViewModel = nullptr;
		UNiagaraStackEntry* Entry = nullptr;
		UNiagaraStackEntry::FStackIssue Issue;
		UNiagaraStackEntry::FStackIssueFix Fix;
		FString ScopeType;
		FString ScopeName;
		FGuid EmitterHandleId;
		FString EntryPath;
		int32 IssueIndex = INDEX_NONE;
		int32 FixIndex = INDEX_NONE;
	};

	TArray<FMatchedStackIssueFix> Matches;
	TArray<TSharedPtr<FJsonValue>> MatchSummaries;
	int32 VisitedScopeCount = 0;
	int32 VisitedIssueCount = 0;

	auto FindFixIndex = [&](const UNiagaraStackEntry::FStackIssue& Issue, int32& OutFixIndex) -> bool
	{
		OutFixIndex = INDEX_NONE;
		const TArray<UNiagaraStackEntry::FStackIssueFix>& Fixes = Issue.GetFixes();
		if (bHasFixUniqueIdentifier)
		{
			for (int32 FixIndex = 0; FixIndex < Fixes.Num(); ++FixIndex)
			{
				if (Fixes[FixIndex].GetUniqueIdentifier() == FixUniqueIdentifier)
				{
					OutFixIndex = FixIndex;
					return true;
				}
			}
			return false;
		}
		if (bHasFixDescriptionContains)
		{
			for (int32 FixIndex = 0; FixIndex < Fixes.Num(); ++FixIndex)
			{
				if (Fixes[FixIndex].GetDescription().ToString().Contains(FixDescriptionContains, ESearchCase::IgnoreCase))
				{
					OutFixIndex = FixIndex;
					return true;
				}
			}
			return false;
		}
		if (!Fixes.IsValidIndex(RequestedFixIndex))
		{
			return false;
		}
		OutFixIndex = RequestedFixIndex;
		return true;
	};

	auto CollectMatchesForScope = [&](UNiagaraStackViewModel* StackViewModel, const FString& ScopeType, const FString& ScopeName, const FGuid& EmitterHandleId)
	{
		if (!StackViewModel)
		{
			return;
		}
		if (bHasScopeTypeFilter && !ScopeType.Equals(ScopeTypeFilter, ESearchCase::IgnoreCase))
		{
			return;
		}

		UNiagaraStackEntry* RootEntry = StackViewModel->GetRootEntry();
		if (!RootEntry)
		{
			return;
		}

		++VisitedScopeCount;
		RootEntry->RefreshChildren();
		RootEntry->RefreshFilteredChildren();

		TArray<UNiagaraStackEntry*> EntriesToCheck;
		EntriesToCheck.Add(RootEntry);
		EntriesToCheck.Append(RootEntry->GetAllChildrenWithIssues());

		TSet<UNiagaraStackEntry*> SeenEntries;
		for (UNiagaraStackEntry* EntryToCheck : EntriesToCheck)
		{
			if (!EntryToCheck || SeenEntries.Contains(EntryToCheck))
			{
				continue;
			}
			SeenEntries.Add(EntryToCheck);

			const FString EntryPath = UeAgentNiagaraOps::BuildStackIssueEntryPath(StackViewModel, *EntryToCheck);
			const TArray<UNiagaraStackEntry::FStackIssue>& Issues = EntryToCheck->GetIssues();
			for (int32 IssueIndex = 0; IssueIndex < Issues.Num(); ++IssueIndex)
			{
				const UNiagaraStackEntry::FStackIssue& Issue = Issues[IssueIndex];
				++VisitedIssueCount;
				if (!UeAgentNiagaraOps::ShouldIncludeStackIssueSeverity(Issue.GetSeverity(), SeverityFilter))
				{
					continue;
				}
				if (bHasIssueUniqueIdentifier && Issue.GetUniqueIdentifier() != IssueUniqueIdentifier)
				{
					continue;
				}
				if (bHasShortDescriptionContains && !Issue.GetShortDescription().ToString().Contains(ShortDescriptionContains, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (bHasLongDescriptionContains && !Issue.GetLongDescription().ToString().Contains(LongDescriptionContains, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (bHasEntryPathContains && !EntryPath.Contains(EntryPathContains, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (bHasModuleNodeGuid)
				{
					UNiagaraStackModuleItem* ModuleItem = Cast<UNiagaraStackModuleItem>(EntryToCheck);
					if (!ModuleItem || ModuleItem->GetModuleNode().NodeGuid != ModuleNodeGuid)
					{
						continue;
					}
				}

				int32 FixIndex = INDEX_NONE;
				if (!FindFixIndex(Issue, FixIndex))
				{
					continue;
				}

				const TArray<UNiagaraStackEntry::FStackIssueFix>& Fixes = Issue.GetFixes();
				const UNiagaraStackEntry::FStackIssueFix& Fix = Fixes[FixIndex];
				if (!bAllowLinkFix && Fix.GetStyle() == UNiagaraStackEntry::EStackIssueFixStyle::Link)
				{
					continue;
				}

				FMatchedStackIssueFix Match;
				Match.StackViewModel = StackViewModel;
				Match.Entry = EntryToCheck;
				Match.Issue = Issue;
				Match.Fix = Fix;
				Match.ScopeType = ScopeType;
				Match.ScopeName = ScopeName;
				Match.EmitterHandleId = EmitterHandleId;
				Match.EntryPath = EntryPath;
				Match.IssueIndex = IssueIndex;
				Match.FixIndex = FixIndex;
				Matches.Add(Match);

				TSharedPtr<FJsonObject> SummaryObj = UeAgentNiagaraOps::BuildStackIssueJson(*EntryToCheck, Issue, ScopeType, ScopeName, EmitterHandleId, EntryPath);
				SummaryObj->SetNumberField(TEXT("selected_fix_index"), FixIndex);
				SummaryObj->SetStringField(TEXT("selected_fix_description"), Fix.GetDescription().ToString());
				SummaryObj->SetStringField(TEXT("selected_fix_unique_identifier"), Fix.GetUniqueIdentifier());
				MatchSummaries.Add(MakeShared<FJsonValueObject>(SummaryObj));
			}
		}
	};

	if (bIncludeSystemScope)
	{
		CollectMatchesForScope(SystemViewModel->GetSystemStackViewModel(), TEXT("system"), SystemViewModel->GetDisplayName().ToString(), FGuid());
	}

	if (bIncludeEmitters)
	{
		const TArray<TSharedRef<FNiagaraEmitterHandleViewModel>>& EmitterHandleViewModels = SystemViewModel->GetEmitterHandleViewModels();
		for (int32 EmitterIndex = 0; EmitterIndex < EmitterHandleViewModels.Num(); ++EmitterIndex)
		{
			const TSharedRef<FNiagaraEmitterHandleViewModel>& EmitterHandleViewModel = EmitterHandleViewModels[EmitterIndex];
			const FString EmitterName = EmitterHandleViewModel->GetNameText().ToString();
			const FGuid EmitterHandleId = EmitterHandleViewModel->GetId();

			if (EmitterIndexFilter != INDEX_NONE && EmitterIndex != EmitterIndexFilter)
			{
				continue;
			}
			if (bHasEmitterNameFilter && EmitterName != EmitterNameFilter)
			{
				continue;
			}
			if (bHasEmitterIdFilter && EmitterHandleId != EmitterIdFilter)
			{
				continue;
			}

			CollectMatchesForScope(EmitterHandleViewModel->GetEmitterStackViewModel(), TEXT("emitter"), EmitterName, EmitterHandleId);
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetStringField(TEXT("view_model_source"), ViewModelSource);
	OutData->SetNumberField(TEXT("visited_scope_count"), VisitedScopeCount);
	OutData->SetNumberField(TEXT("visited_issue_count"), VisitedIssueCount);
	OutData->SetNumberField(TEXT("matching_issue_count"), Matches.Num());
	OutData->SetArrayField(TEXT("matches"), MatchSummaries);

	if (Matches.Num() == 0)
	{
		OutError = TEXT("no_matching_stack_issue_fix");
		return false;
	}
	if (Matches.Num() > 1 && !bApplyFirstMatch)
	{
		OutError = TEXT("multiple_matching_stack_issue_fixes");
		return false;
	}

	FMatchedStackIssueFix Match = Matches[0];
	if (!Match.Fix.IsValid() || !Match.Fix.GetFixDelegate().IsBound())
	{
		OutError = TEXT("selected_stack_issue_fix_has_no_delegate");
		return false;
	}

	TSharedPtr<FJsonObject> AppliedObj = UeAgentNiagaraOps::BuildStackIssueJson(*Match.Entry, Match.Issue, Match.ScopeType, Match.ScopeName, Match.EmitterHandleId, Match.EntryPath);
	AppliedObj->SetNumberField(TEXT("selected_fix_index"), Match.FixIndex);
	AppliedObj->SetStringField(TEXT("selected_fix_description"), Match.Fix.GetDescription().ToString());
	AppliedObj->SetStringField(TEXT("selected_fix_unique_identifier"), Match.Fix.GetUniqueIdentifier());

	NiagaraSystem->Modify();
	UNiagaraStackEntry::FStackIssueFixDelegate FixDelegate = Match.Fix.GetFixDelegate();
	FixDelegate.Execute();
	const UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult =
		UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, true);

	if (Match.StackViewModel)
	{
		if (UNiagaraStackEntry* RootEntry = Match.StackViewModel->GetRootEntry())
		{
			RootEntry->RefreshChildren();
			RootEntry->RefreshFilteredChildren();
		}
	}

	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);

	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);

	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);

	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	bool bCompileHasError = false;
	TSharedPtr<FJsonObject> CompileLogData;
	if (bCompileAfterApply)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(bForceCompile);
		if (bWaitForComplete)
		{
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
			NiagaraSystem->CacheFromCompiledData();
			bCompilationComplete = !NiagaraSystem->HasOutstandingCompilationRequests(true);
		}

		FUeAgentRequestContext CompileLogCtx;
		CompileLogCtx.RequestId = TEXT("niagara_apply_stack_issue_fix_compile_log");
		CompileLogCtx.Command = TEXT("niagara_get_compile_log");
		CompileLogCtx.Params = MakeShared<FJsonObject>();
		CompileLogCtx.Params->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
		CompileLogCtx.Params->SetBoolField(TEXT("compile_before_read"), false);
		CompileLogCtx.Params->SetBoolField(TEXT("include_events"), true);
		CompileLogCtx.Params->SetStringField(TEXT("severity_filter"), TEXT("warning_or_error"));
		CompileLogCtx.Params->SetNumberField(TEXT("max_events_per_script"), 64);
		CompileLogData = MakeShared<FJsonObject>();
		if (!CmdNiagaraGetCompileLog(CompileLogCtx, CompileLogData, OutError))
		{
			return false;
		}
		CompileLogData->TryGetBoolField(TEXT("has_error"), bCompileHasError);
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bSaved = false;

	OutData->SetObjectField(TEXT("applied_issue"), AppliedObj);
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("applied"), true);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
	OutData->SetBoolField(TEXT("compile_has_error"), bCompileHasError);
	if (CompileLogData.IsValid())
	{
		OutData->SetObjectField(TEXT("compile_log"), CompileLogData);
	}
	OutData->SetBoolField(TEXT("saved"), bSaved);
	if (bCompileAfterApply && bWaitForComplete && NiagaraSystem->HasOutstandingCompilationRequests(true))
	{
		OutError = TEXT("compile_incomplete_after_apply");
		return false;
	}
	if (bCompileAfterApply && bCompileHasError)
	{
		OutError = TEXT("compile_failed_after_apply");
		return false;
	}
	if (bSaveAfterApply)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
		bSaved = true;
		OutData->SetBoolField(TEXT("saved"), bSaved);
	}
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterClearParent(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	bool bClearMergeMessage = true;
	JsonTryGetBool(Ctx.Params, TEXT("clear_merge_message"), bClearMergeMessage);

	bool bForce = false;
	JsonTryGetBool(Ctx.Params, TEXT("force"), bForce);

	auto BuildVersionedEmitterJson = [](const FVersionedNiagaraEmitter& VersionedEmitter) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("has_emitter"), VersionedEmitter.Emitter != nullptr);
		Obj->SetStringField(TEXT("emitter_path"), VersionedEmitter.Emitter ? VersionedEmitter.Emitter->GetPathName() : FString());
		Obj->SetStringField(TEXT("version"), VersionedEmitter.Version.IsValid() ? VersionedEmitter.Version.ToString(EGuidFormats::DigitsWithHyphensLower) : FString());
		return Obj;
	};

	const FVersionedNiagaraEmitter ParentBefore = EditContext.EmitterData->GetParent();
	const FVersionedNiagaraEmitter ParentAtLastMergeBefore = EditContext.EmitterData->GetParentAtLastMerge();
	const bool bHadParent = ParentBefore.Emitter != nullptr || ParentAtLastMergeBefore.Emitter != nullptr;
	const FGuid EmitterMergeMessageId(0xDAF26E73, 0x4D1B416B, 0x815FA6C2, 0x6D5D0A75);
	const bool bHadMergeMessage = EditContext.Emitter->GetMessageStore().GetMessages().Contains(EmitterMergeMessageId);

	if (!bHadParent && !bHadMergeMessage && !bForce)
	{
		OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("object_path"), EditContext.Emitter->GetPathName());
		OutData->SetObjectField(TEXT("parent_before"), BuildVersionedEmitterJson(ParentBefore));
		OutData->SetObjectField(TEXT("parent_at_last_merge_before"), BuildVersionedEmitterJson(ParentAtLastMergeBefore));
		OutData->SetBoolField(TEXT("had_parent"), false);
		OutData->SetBoolField(TEXT("had_merge_message"), false);
		OutData->SetBoolField(TEXT("changed"), false);
		return true;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("Clear Niagara emitter parent")));
	EditContext.Emitter->Modify();
	if (UNiagaraSystem* OwningSystem = EditContext.Emitter->GetTypedOuter<UNiagaraSystem>())
	{
		OwningSystem->Modify();
	}

	EditContext.EmitterData->RemoveParent();
	if (bClearMergeMessage && bHadMergeMessage)
	{
		EditContext.Emitter->GetMessageStore().RemoveMessage(EmitterMergeMessageId);
	}
	EditContext.EmitterData->InvalidateCompileResults();
	const UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult =
		UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	UNiagaraSystem* OwningSystem = EditContext.Emitter->GetTypedOuter<UNiagaraSystem>();

	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);

	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);

	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);

	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	bool bCompileHasError = false;
	TSharedPtr<FJsonObject> CompileLogData;
	if (bCompileAfterApply && OwningSystem)
	{
		bCompilationRequested = OwningSystem->RequestCompile(bForceCompile);
		if (bWaitForComplete)
		{
			OwningSystem->WaitForCompilationComplete(true, false);
			OwningSystem->PollForCompilationComplete(true);
			OwningSystem->CacheFromCompiledData();
			bCompilationComplete = !OwningSystem->HasOutstandingCompilationRequests(true);
		}
	}
	if (bCompileAfterApply)
	{
		FUeAgentRequestContext CompileLogCtx;
		CompileLogCtx.RequestId = TEXT("niagara_emitter_clear_parent_compile_log");
		CompileLogCtx.Command = TEXT("niagara_get_compile_log");
		CompileLogCtx.Params = MakeShared<FJsonObject>();
		CompileLogCtx.Params->SetStringField(TEXT("asset_path"), OwningSystem ? OwningSystem->GetOutermost()->GetName() : EditContext.Emitter->GetOutermost()->GetName());
		if (!OwningSystem)
		{
			CompileLogCtx.Params->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			CompileLogCtx.Params->SetBoolField(TEXT("compile_before_read"), true);
		}
		else
		{
			CompileLogCtx.Params->SetBoolField(TEXT("compile_before_read"), false);
		}
		CompileLogCtx.Params->SetBoolField(TEXT("include_events"), true);
		CompileLogCtx.Params->SetStringField(TEXT("severity_filter"), TEXT("warning_or_error"));
		CompileLogCtx.Params->SetNumberField(TEXT("max_events_per_script"), 64);
		CompileLogData = MakeShared<FJsonObject>();
		if (!CmdNiagaraGetCompileLog(CompileLogCtx, CompileLogData, OutError))
		{
			return false;
		}
		CompileLogData->TryGetBoolField(TEXT("has_error"), bCompileHasError);
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bSaved = false;

	const FVersionedNiagaraEmitter ParentAfter = EditContext.EmitterData->GetParent();
	const FVersionedNiagaraEmitter ParentAtLastMergeAfter = EditContext.EmitterData->GetParentAtLastMerge();
	const bool bHasMergeMessageAfter = EditContext.Emitter->GetMessageStore().GetMessages().Contains(EmitterMergeMessageId);

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), EditContext.Emitter->GetPathName());
	OutData->SetStringField(TEXT("owning_system_path"), OwningSystem ? OwningSystem->GetPathName() : FString());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("parent_before"), BuildVersionedEmitterJson(ParentBefore));
	OutData->SetObjectField(TEXT("parent_at_last_merge_before"), BuildVersionedEmitterJson(ParentAtLastMergeBefore));
	OutData->SetObjectField(TEXT("parent_after"), BuildVersionedEmitterJson(ParentAfter));
	OutData->SetObjectField(TEXT("parent_at_last_merge_after"), BuildVersionedEmitterJson(ParentAtLastMergeAfter));
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("had_parent"), bHadParent);
	OutData->SetBoolField(TEXT("had_merge_message"), bHadMergeMessage);
	OutData->SetBoolField(TEXT("has_merge_message_after"), bHasMergeMessageAfter);
	OutData->SetBoolField(TEXT("cleared_merge_message"), bClearMergeMessage && bHadMergeMessage);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("compile_has_error"), bCompileHasError);
	if (OwningSystem)
	{
		OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), OwningSystem->HasOutstandingCompilationRequests(true));
	}
	if (CompileLogData.IsValid())
	{
		OutData->SetObjectField(TEXT("compile_log"), CompileLogData);
	}
	OutData->SetBoolField(TEXT("saved"), bSaved);
	if (bCompileAfterApply && bWaitForComplete && OwningSystem && OwningSystem->HasOutstandingCompilationRequests(true))
	{
		OutError = TEXT("compile_incomplete_after_apply");
		return false;
	}
	if (bCompileAfterApply && bCompileHasError)
	{
		OutError = TEXT("compile_failed_after_apply");
		return false;
	}
	if (bSaveAfterApply)
	{
		UObject* SaveTarget = OwningSystem ? static_cast<UObject*>(OwningSystem) : static_cast<UObject*>(EditContext.Emitter);
		if (!UeAgentNiagaraOps::SaveAssetPackage(SaveTarget, OutError))
		{
			return false;
		}
		bSaved = true;
		OutData->SetBoolField(TEXT("saved"), bSaved);
	}
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(Asset) && !Cast<UNiagaraEmitter>(Asset) && !Cast<UNiagaraScript>(Asset))
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));

	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
	{
		OutData->SetNumberField(TEXT("emitter_handle_count"), NiagaraSystem->GetEmitterHandles().Num());
		OutData->SetBoolField(TEXT("has_fixed_bounds"), NiagaraSystem->GetFixedBounds().IsValid != 0);
		if (UNiagaraSystemEditorData* SystemEditorData = Cast<UNiagaraSystemEditorData>(NiagaraSystem->GetEditorData()))
		{
			UEdGraph* OverviewGraph = SystemEditorData->GetSystemOverviewGraph();
			OutData->SetNumberField(TEXT("overview_node_count"), OverviewGraph ? OverviewGraph->Nodes.Num() : 0);
		}

		TArray<FNiagaraVariable> UserParameters;
		NiagaraSystem->GetExposedParameters().GetUserParameters(UserParameters);

		TArray<TSharedPtr<FJsonValue>> ParametersJson;
		ParametersJson.Reserve(UserParameters.Num());
		for (const FNiagaraVariable& Parameter : UserParameters)
		{
			TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
			ParamObj->SetStringField(TEXT("name"), Parameter.GetName().ToString());
			ParamObj->SetStringField(TEXT("type"), Parameter.GetType().GetNameText().ToString());
			ParametersJson.Add(MakeShared<FJsonValueObject>(ParamObj));
		}
		OutData->SetArrayField(TEXT("user_parameters"), ParametersJson);
		OutData->SetNumberField(TEXT("user_parameter_count"), ParametersJson.Num());
	}
	else if (UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(Asset))
	{
		const FNiagaraAssetVersion ExposedVersion = NiagaraEmitter->GetExposedVersion();
		OutData->SetStringField(TEXT("exposed_version"), ExposedVersion.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetNumberField(TEXT("major_version"), ExposedVersion.MajorVersion);
		OutData->SetNumberField(TEXT("minor_version"), ExposedVersion.MinorVersion);
	}
	else if (UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(Asset))
	{
		const FNiagaraAssetVersion ExposedVersion = NiagaraScript->GetExposedVersion();
		OutData->SetStringField(TEXT("exposed_version"), ExposedVersion.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetNumberField(TEXT("major_version"), ExposedVersion.MajorVersion);
		OutData->SetNumberField(TEXT("minor_version"), ExposedVersion.MinorVersion);
		if (const UEnum* ScriptUsageEnum = StaticEnum<ENiagaraScriptUsage>())
		{
			OutData->SetStringField(TEXT("usage"), ScriptUsageEnum->GetNameStringByValue((int64)NiagaraScript->GetUsage()));
		}
		else
		{
			OutData->SetStringField(TEXT("usage"), TEXT(""));
		}
	}

	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemGetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyName = UeAgentNiagaraOps::NormalizeEventHandlerPropertyName(PropertyName);

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FString ValueText;
	if (!UeAgentNiagaraOps::GetObjectPropertyText(NiagaraSystem, PropertyName, ValueText))
	{
		OutError = TEXT("get_property_failed");
		return false;
	}

	FProperty* ResolvedProperty = nullptr;
	void* ValuePtr = nullptr;
	UeAgentNiagaraOps::ResolvePropertyPath(NiagaraSystem, PropertyName, ResolvedProperty, ValuePtr);

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("cpp_type"), ResolvedProperty ? ResolvedProperty->GetCPPType() : TEXT(""));
	OutData->SetBoolField(TEXT("property_found"), ResolvedProperty != nullptr);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}

	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(Asset) && !Cast<UNiagaraEmitter>(Asset) && !Cast<UNiagaraScript>(Asset))
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetObjectPropertyText(Asset, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		ResultObj->SetStringField(TEXT("cpp_type"), CppType);
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutData->SetObjectField(TEXT("property_import_result"), ResultObj);
		OutError = ImportError.IsEmpty() ? FString::Printf(TEXT("set_property_failed:%s:%s"), *PropertyName, *ValueText) : ImportError;
		return false;
	}

	Asset->MarkPackageDirty();
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(Asset, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraRefreshSystem(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("niagara_system_not_found");
		return false;
	}
	UPackage* NiagaraSystemPackage = NiagaraSystem->GetOutermost();
	const bool bWasPackageDirtyBeforeRefresh = NiagaraSystemPackage ? NiagaraSystemPackage->IsDirty() : false;

	bool bBroadcastPostEditChange = true;
	JsonTryGetBool(Ctx.Params, TEXT("broadcast_post_edit_change"), bBroadcastPostEditChange);
	bool bMarkDirty = true;
	JsonTryGetBool(Ctx.Params, TEXT("mark_dirty"), bMarkDirty);
	bool bCompileAfterRefresh = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_refresh"), bCompileAfterRefresh);
	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);

	const UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult =
		UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, bBroadcastPostEditChange, bMarkDirty);

	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterRefresh)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(bForceCompile);
		if (bWaitForComplete)
		{
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
			NiagaraSystem->CacheFromCompiledData();
			bCompilationComplete = !NiagaraSystem->HasOutstandingCompilationRequests(true);
		}
	}

	bool bSaveAfterRefresh = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_refresh"), bSaveAfterRefresh);
	bool bRestoredDirtyState = false;
	if (!bMarkDirty && !bSaveAfterRefresh && NiagaraSystemPackage && !bWasPackageDirtyBeforeRefresh && NiagaraSystemPackage->IsDirty())
	{
		NiagaraSystemPackage->SetDirtyFlag(false);
		bRestoredDirtyState = true;
	}
	if (bSaveAfterRefresh)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("broadcast_post_edit_change"), bBroadcastPostEditChange);
	OutData->SetBoolField(TEXT("mark_dirty"), bMarkDirty);
	OutData->SetBoolField(TEXT("compile_after_refresh"), bCompileAfterRefresh);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
	OutData->SetBoolField(TEXT("is_ready_to_run"), NiagaraSystem->IsReadyToRun());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRefresh);
	OutData->SetBoolField(TEXT("restored_dirty_state"), bRestoredDirtyState);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraCompileSystem(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	UPackage* AssetPackage = Asset->GetOutermost();
	const bool bWasPackageDirtyBeforeCompile = AssetPackage ? AssetPackage->IsDirty() : false;

	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bRefreshBeforeCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("refresh_before_compile"), bRefreshBeforeCompile);
	bool bSaveAfterCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_compile"), bSaveAfterCompile);
	bool bMarkDirtyAfterCompile = bSaveAfterCompile;
	JsonTryGetBool(Ctx.Params, TEXT("mark_dirty_after_compile"), bMarkDirtyAfterCompile);

	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	FString CompileTarget;
	UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult;

	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
	{
		if (bRefreshBeforeCompile)
		{
			RefreshResult = UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, bMarkDirtyAfterCompile);
		}
		bCompilationRequested = NiagaraSystem->RequestCompile(bForceCompile);
		if (bWaitForComplete)
		{
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
			NiagaraSystem->CacheFromCompiledData();
			bCompilationComplete = !NiagaraSystem->HasOutstandingCompilationRequests(true);
		}
		CompileTarget = TEXT("system");
	}
	else if (UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(Asset))
	{
		NiagaraScript->RequestCompile(NiagaraScript->GetExposedVersion().VersionGuid, bForceCompile);
		bCompilationRequested = true;
		bCompilationComplete = true;
		CompileTarget = TEXT("script");
	}
	else
	{
		OutError = TEXT("niagara_compile_requires_system_or_script_asset");
		return false;
	}

	if (bMarkDirtyAfterCompile)
	{
		Asset->MarkPackageDirty();
	}
	bool bRestoredDirtyState = false;
	if (!bMarkDirtyAfterCompile && AssetPackage && !bWasPackageDirtyBeforeCompile && AssetPackage->IsDirty())
	{
		AssetPackage->SetDirtyFlag(false);
		bRestoredDirtyState = true;
	}
	if (bSaveAfterCompile)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(Asset, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("compile_target"), CompileTarget);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("refresh_before_compile"), bRefreshBeforeCompile);
	OutData->SetBoolField(TEXT("mark_dirty_after_compile"), bMarkDirtyAfterCompile);
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
	{
		OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
		OutData->SetBoolField(TEXT("is_ready_to_run"), NiagaraSystem->IsReadyToRun());
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCompile);
	OutData->SetBoolField(TEXT("restored_dirty_state"), bRestoredDirtyState);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraGetCompileLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	UPackage* AssetPackage = Asset->GetOutermost();
	const bool bWasPackageDirtyBeforeRead = AssetPackage ? AssetPackage->IsDirty() : false;

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
	UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(Asset);
	UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(Asset);
	if (!NiagaraSystem && !NiagaraEmitter && !NiagaraScript)
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	bool bCompileBeforeRead = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_before_read"), bCompileBeforeRead);

	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);

	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bRefreshBeforeCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("refresh_before_compile"), bRefreshBeforeCompile);

	bool bIncludeEvents = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_events"), bIncludeEvents);

	bool bIncludeStackGuids = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_stack_guids"), bIncludeStackGuids);

	bool bIncludeDisabledEmitters = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_disabled_emitters"), bIncludeDisabledEmitters);

	int32 MaxEventsPerScript = 200;
	double MaxEventsPerScriptNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_events_per_script"), MaxEventsPerScriptNumber))
	{
		MaxEventsPerScript = FMath::Clamp(FMath::FloorToInt(MaxEventsPerScriptNumber), 0, 5000);
	}

	FString SeverityFilterText = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilterText);

	UeAgentNiagaraOps::ENiagaraCompileLogSeverityFilter SeverityFilter = UeAgentNiagaraOps::ENiagaraCompileLogSeverityFilter::All;
	if (!UeAgentNiagaraOps::ParseCompileLogSeverityFilter(SeverityFilterText, SeverityFilter))
	{
		OutError = TEXT("invalid_severity_filter");
		return false;
	}

	UeAgentNiagaraOps::FNiagaraEmitterEditContext EmitterEditContext;
	if (NiagaraEmitter)
	{
		if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EmitterEditContext, OutError, false))
		{
			return false;
		}
	}

	TArray<UeAgentNiagaraOps::FCollectedNiagaraScriptInfo> CollectedScripts;
	if (NiagaraSystem)
	{
		UeAgentNiagaraOps::CollectSystemScriptsForCompileLog(*NiagaraSystem, bIncludeDisabledEmitters, CollectedScripts);
	}
	else if (NiagaraEmitter)
	{
		TSet<const UNiagaraScript*> SeenScripts;
		TArray<UNiagaraScript*> EmitterScripts;
		EmitterEditContext.EmitterData->GetScripts(EmitterScripts, false, false);
		const FString EmitterVersion = EmitterEditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		for (UNiagaraScript* Script : EmitterScripts)
		{
			UeAgentNiagaraOps::AppendUniqueCollectedScript(
				CollectedScripts,
				SeenScripts,
				Script,
				TEXT("emitter_asset"),
				EmitterEditContext.Emitter->GetName(),
				EmitterEditContext.Emitter->GetPathName(),
				FString(),
				EmitterVersion,
				true);
		}
	}
	else if (NiagaraScript)
	{
		TSet<const UNiagaraScript*> SeenScripts;
		UeAgentNiagaraOps::AppendUniqueCollectedScript(
			CollectedScripts,
			SeenScripts,
			NiagaraScript,
			TEXT("script_asset"),
			NiagaraScript->GetName(),
			NiagaraScript->GetPathName(),
			FString(),
			FString(),
			true);
	}

	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	bool bWaitForCompleteSupported = false;
	UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult;

	if (bCompileBeforeRead)
	{
		if (NiagaraSystem)
		{
			bWaitForCompleteSupported = true;
			if (bRefreshBeforeCompile)
			{
				RefreshResult = UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, false);
			}
			bCompilationRequested = NiagaraSystem->RequestCompile(bForceCompile);
			if (bWaitForComplete)
			{
				NiagaraSystem->WaitForCompilationComplete(true, false);
				NiagaraSystem->PollForCompilationComplete(true);
				NiagaraSystem->CacheFromCompiledData();
				bCompilationComplete = !NiagaraSystem->HasOutstandingCompilationRequests(true);
			}
		}
		else if (NiagaraScript)
		{
			NiagaraScript->RequestCompile(NiagaraScript->GetExposedVersion().VersionGuid, bForceCompile);
			bCompilationRequested = true;
			bCompilationComplete = true;
		}
		else if (NiagaraEmitter)
		{
			for (const UeAgentNiagaraOps::FCollectedNiagaraScriptInfo& ScriptInfo : CollectedScripts)
			{
				if (!ScriptInfo.Script)
				{
					continue;
				}

				ScriptInfo.Script->RequestCompile(ScriptInfo.Script->GetExposedVersion().VersionGuid, bForceCompile);
				bCompilationRequested = true;
			}
			bCompilationComplete = true;
		}
	}
	bool bRestoredDirtyState = false;
	if (AssetPackage && !bWasPackageDirtyBeforeRead && AssetPackage->IsDirty())
	{
		AssetPackage->SetDirtyFlag(false);
		bRestoredDirtyState = true;
	}

	TArray<TSharedPtr<FJsonValue>> ScriptsJson;
	ScriptsJson.Reserve(CollectedScripts.Num());

	int32 TotalCompileEventCount = 0;
	int32 TotalFilteredCompileEventCount = 0;
	int32 ErrorScriptCount = 0;
	int32 WarningScriptCount = 0;
	bool bHasError = false;
	bool bHasWarning = false;

	for (const UeAgentNiagaraOps::FCollectedNiagaraScriptInfo& ScriptInfo : CollectedScripts)
	{
		if (!ScriptInfo.Script)
		{
			continue;
		}

		UNiagaraScript& Script = *ScriptInfo.Script;
		const FNiagaraVMExecutableData& VMExecutableData = Script.GetVMExecutableData();
		const ENiagaraScriptCompileStatus CompileStatus = Script.GetLastCompileStatus();

		const int32 RawCompileEventCount = VMExecutableData.LastCompileEvents.Num();
		TotalCompileEventCount += RawCompileEventCount;

		int32 ErrorEventCount = 0;
		int32 WarningEventCount = 0;
		int32 InfoEventCount = 0;
		int32 FilteredEventCount = 0;

		TArray<TSharedPtr<FJsonValue>> EventsJson;
		if (bIncludeEvents)
		{
			EventsJson.Reserve(FMath::Min(RawCompileEventCount, MaxEventsPerScript));
		}

		for (const FNiagaraCompileEvent& CompileEvent : VMExecutableData.LastCompileEvents)
		{
			if (!UeAgentNiagaraOps::ShouldIncludeCompileEvent(CompileEvent, SeverityFilter))
			{
				continue;
			}

			++FilteredEventCount;
			if (CompileEvent.Severity == FNiagaraCompileEventSeverity::Error)
			{
				++ErrorEventCount;
			}
			else if (CompileEvent.Severity == FNiagaraCompileEventSeverity::Warning)
			{
				++WarningEventCount;
			}
			else
			{
				++InfoEventCount;
			}

			if (bIncludeEvents && EventsJson.Num() < MaxEventsPerScript)
			{
				EventsJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildCompileEventJson(CompileEvent, bIncludeStackGuids)));
			}
		}

		TotalFilteredCompileEventCount += FilteredEventCount;

		const bool bScriptHasError = CompileStatus == ENiagaraScriptCompileStatus::NCS_Error || !VMExecutableData.ErrorMsg.IsEmpty() || ErrorEventCount > 0;
		const bool bScriptHasWarning = UeAgentNiagaraOps::IsCompileStatusWarning(CompileStatus) || WarningEventCount > 0;
		if (bScriptHasError)
		{
			++ErrorScriptCount;
			bHasError = true;
		}
		if (bScriptHasWarning)
		{
			++WarningScriptCount;
			bHasWarning = true;
		}

		TSharedPtr<FJsonObject> ScriptObj = MakeShared<FJsonObject>();
		ScriptObj->SetStringField(TEXT("script_object_path"), Script.GetPathName());
		ScriptObj->SetStringField(TEXT("script_asset_path"), Script.GetOutermost()->GetName());
		ScriptObj->SetStringField(TEXT("script_name"), Script.GetName());
		ScriptObj->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Script.GetUsage()));
		ScriptObj->SetStringField(TEXT("script_usage_id"), Script.GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
		ScriptObj->SetStringField(TEXT("compile_status"), UeAgentNiagaraOps::ScriptCompileStatusToString(CompileStatus));
		ScriptObj->SetStringField(TEXT("error_msg"), VMExecutableData.ErrorMsg);
		ScriptObj->SetBoolField(TEXT("has_error"), bScriptHasError);
		ScriptObj->SetBoolField(TEXT("has_warning"), bScriptHasWarning);
		ScriptObj->SetNumberField(TEXT("compile_event_total_count"), RawCompileEventCount);
		ScriptObj->SetNumberField(TEXT("compile_event_filtered_count"), FilteredEventCount);
		ScriptObj->SetNumberField(TEXT("compile_event_emitted_count"), EventsJson.Num());
		ScriptObj->SetNumberField(TEXT("error_event_count"), ErrorEventCount);
		ScriptObj->SetNumberField(TEXT("warning_event_count"), WarningEventCount);
		ScriptObj->SetNumberField(TEXT("info_event_count"), InfoEventCount);
		ScriptObj->SetBoolField(TEXT("events_truncated"), bIncludeEvents && EventsJson.Num() < FilteredEventCount);
		ScriptObj->SetStringField(TEXT("owner_type"), ScriptInfo.OwnerType);
		ScriptObj->SetStringField(TEXT("owner_name"), ScriptInfo.OwnerName);
		ScriptObj->SetStringField(TEXT("owner_path"), ScriptInfo.OwnerPath);
		ScriptObj->SetStringField(TEXT("emitter_handle_id"), ScriptInfo.EmitterHandleId);
		ScriptObj->SetStringField(TEXT("emitter_version"), ScriptInfo.EmitterVersion);
		ScriptObj->SetBoolField(TEXT("emitter_enabled"), ScriptInfo.bEmitterEnabled);

		if (bIncludeEvents)
		{
			ScriptObj->SetArrayField(TEXT("events"), EventsJson);
		}

		ScriptsJson.Add(MakeShared<FJsonValueObject>(ScriptObj));
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));
	OutData->SetBoolField(TEXT("compile_before_read"), bCompileBeforeRead);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("refresh_before_compile"), bRefreshBeforeCompile);
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("wait_for_complete_supported"), bWaitForCompleteSupported);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("restored_dirty_state"), bRestoredDirtyState);
	if (NiagaraSystem)
	{
		OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
		OutData->SetBoolField(TEXT("is_ready_to_run"), NiagaraSystem->IsReadyToRun());
	}
	OutData->SetStringField(TEXT("severity_filter"), SeverityFilterText);
	OutData->SetBoolField(TEXT("include_events"), bIncludeEvents);
	OutData->SetBoolField(TEXT("include_stack_guids"), bIncludeStackGuids);
	OutData->SetNumberField(TEXT("max_events_per_script"), MaxEventsPerScript);
	OutData->SetNumberField(TEXT("script_count"), ScriptsJson.Num());
	OutData->SetNumberField(TEXT("total_compile_event_count"), TotalCompileEventCount);
	OutData->SetNumberField(TEXT("total_compile_event_filtered_count"), TotalFilteredCompileEventCount);
	OutData->SetNumberField(TEXT("error_script_count"), ErrorScriptCount);
	OutData->SetNumberField(TEXT("warning_script_count"), WarningScriptCount);
	OutData->SetBoolField(TEXT("has_error"), bHasError);
	OutData->SetBoolField(TEXT("has_warning"), bHasWarning);
	OutData->SetArrayField(TEXT("scripts"), ScriptsJson);

	if (NiagaraEmitter)
	{
		OutData->SetStringField(TEXT("emitter_version"), EmitterEditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	}

	return true;
}



bool FUeAgentHttpServer::CmdNiagaraUserParameterList(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	bool bIncludeValues = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_values"), bIncludeValues);

	TArray<FNiagaraVariable> UserParameters;
	NiagaraSystem->GetExposedParameters().GetUserParameters(UserParameters);
	UserParameters.Sort([](const FNiagaraVariable& A, const FNiagaraVariable& B)
	{
		return A.GetName().LexicalLess(B.GetName());
	});

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(UserParameters.Num());
	for (const FNiagaraVariable& UserParameter : UserParameters)
	{
		Items.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildUserParameterJson(NiagaraSystem->GetExposedParameters(), UserParameter, bIncludeValues)));
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetArrayField(TEXT("parameters"), Items);
	OutData->SetNumberField(TEXT("parameter_count"), Items.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraUserParameterAdd(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	FString ParameterTypeText;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_type"), ParameterTypeText) || ParameterTypeText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_type");
		return false;
	}
	ParameterTypeText = ParameterTypeText.TrimStartAndEnd();

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FNiagaraTypeDefinition TypeDefinition;
	if (!UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(ParameterTypeText, TypeDefinition))
	{
		OutError = TEXT("invalid_parameter_type");
		return false;
	}

	FNiagaraVariable ExistingParameter;
	if (UeAgentNiagaraOps::FindUserParameterByName(NiagaraSystem->GetExposedParameters(), ParameterName, ExistingParameter))
	{
		OutError = TEXT("user_parameter_already_exists");
		return false;
	}

	FNiagaraVariable NewParameter(TypeDefinition, *ParameterName);
	FNiagaraUserRedirectionParameterStore::MakeUserVariable(NewParameter);

	const bool bHasDefaultValueText = Ctx.Params.IsValid() && (Ctx.Params->HasField(TEXT("default_value_text")) || Ctx.Params->HasField(TEXT("default_value")));
	FString DefaultValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("default_value_text"), DefaultValueText))
	{
		JsonTryGetString(Ctx.Params, TEXT("default_value"), DefaultValueText);
	}

	NiagaraSystem->Modify();
	FNiagaraUserRedirectionParameterStore& ExposedParameters = NiagaraSystem->GetExposedParameters();
	if (!ExposedParameters.AddParameter(NewParameter, true, true))
	{
		OutError = TEXT("add_user_parameter_failed");
		return false;
	}

	if (bHasDefaultValueText)
	{
		if (!UeAgentNiagaraOps::SetUserParameterValueFromText(ExposedParameters, NewParameter, DefaultValueText, OutError))
		{
			return false;
		}
	}

	NiagaraSystem->MarkPackageDirty();

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, Ctx.Params, false);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	FNiagaraVariable AddedParameter;
	if (!UeAgentNiagaraOps::FindUserParameterByName(ExposedParameters, NewParameter.GetName().ToString(), AddedParameter))
	{
		OutError = TEXT("added_user_parameter_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildUserParameterJson(ExposedParameters, AddedParameter, true));
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraUserParameterRemove(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FNiagaraVariable ParameterToRemove;
	if (!UeAgentNiagaraOps::FindUserParameterByName(NiagaraSystem->GetExposedParameters(), ParameterName, ParameterToRemove))
	{
		OutError = TEXT("user_parameter_not_found");
		return false;
	}

	const FString RemovedName = ParameterToRemove.GetName().ToString();
	const FString RemovedType = ParameterToRemove.GetType().GetNameText().ToString();

	NiagaraSystem->Modify();
	FNiagaraUserRedirectionParameterStore& ExposedParameters = NiagaraSystem->GetExposedParameters();
	if (!ExposedParameters.RemoveParameter(ParameterToRemove))
	{
		OutError = TEXT("remove_user_parameter_failed");
		return false;
	}
	NiagaraSystem->MarkPackageDirty();

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, Ctx.Params, false);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("removed_parameter_name"), RemovedName);
	OutData->SetStringField(TEXT("removed_parameter_type"), RemovedType);
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraUserParameterGet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FNiagaraVariable UserParameter;
	if (!UeAgentNiagaraOps::FindUserParameterByName(NiagaraSystem->GetExposedParameters(), ParameterName, UserParameter))
	{
		OutError = TEXT("user_parameter_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildUserParameterJson(NiagaraSystem->GetExposedParameters(), UserParameter, true));
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraUserParameterSet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		if (!JsonTryGetString(Ctx.Params, TEXT("default_value_text"), ValueText))
		{
			OutError = TEXT("missing_value_text");
			return false;
		}
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FNiagaraVariable UserParameter;
	if (!UeAgentNiagaraOps::FindUserParameterByName(NiagaraSystem->GetExposedParameters(), ParameterName, UserParameter))
	{
		OutError = TEXT("user_parameter_not_found");
		return false;
	}

	NiagaraSystem->Modify();
	FNiagaraUserRedirectionParameterStore& ExposedParameters = NiagaraSystem->GetExposedParameters();
	if (!UeAgentNiagaraOps::SetUserParameterValueFromText(ExposedParameters, UserParameter, ValueText, OutError))
	{
		return false;
	}
	NiagaraSystem->MarkPackageDirty();

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, Ctx.Params, false);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildUserParameterJson(ExposedParameters, UserParameter, true));
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterParameterList(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	bool bIncludeDefaultValues = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_default_values"), bIncludeDefaultValues);

	FString NamespaceFilter;
	JsonTryGetString(Ctx.Params, TEXT("namespace"), NamespaceFilter);
	NamespaceFilter = NamespaceFilter.TrimStartAndEnd();

	const UNiagaraGraph::FScriptVariableMap& ScriptVariableMap = EditContext.Graph->GetAllMetaData();
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(ScriptVariableMap.Num());
	for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : ScriptVariableMap)
	{
		const FString FullName = Pair.Key.GetName().ToString();
		const FString Namespace = UeAgentNiagaraOps::GetVariableNamespace(FullName);
		if (!NamespaceFilter.IsEmpty() && !Namespace.Equals(NamespaceFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		Items.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildGraphParameterJson(Pair.Key, Pair.Value, bIncludeDefaultValues)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("parameters"), Items);
	OutData->SetNumberField(TEXT("parameter_count"), Items.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterParameterAdd(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	FString ParameterTypeText;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_type"), ParameterTypeText) || ParameterTypeText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_type");
		return false;
	}
	ParameterTypeText = ParameterTypeText.TrimStartAndEnd();

	FNiagaraVariable ExistingParameter;
	UNiagaraScriptVariable* ExistingScriptVariable = nullptr;
	if (UeAgentNiagaraOps::FindGraphParameterByName(*EditContext.Graph, ParameterName, ExistingParameter, ExistingScriptVariable))
	{
		OutError = TEXT("emitter_parameter_already_exists");
		return false;
	}

	FNiagaraTypeDefinition TypeDefinition;
	if (!UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(ParameterTypeText, TypeDefinition))
	{
		OutError = TEXT("invalid_parameter_type");
		return false;
	}

	bool bIsStaticSwitch = false;
	JsonTryGetBool(Ctx.Params, TEXT("is_static_switch"), bIsStaticSwitch);

	const bool bHasDefaultValueText = Ctx.Params.IsValid() && (Ctx.Params->HasField(TEXT("default_value_text")) || Ctx.Params->HasField(TEXT("default_value")));
	FString DefaultValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("default_value_text"), DefaultValueText))
	{
		JsonTryGetString(Ctx.Params, TEXT("default_value"), DefaultValueText);
	}

	FNiagaraVariable NewParameter(TypeDefinition, *ParameterName);
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraEmitterParameter", "UeAgentInterface Add Niagara Emitter Parameter"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	UNiagaraScriptVariable* AddedScriptVariable = UeAgentNiagaraOps::AddGraphParameterMetadata(*EditContext.Graph, NewParameter, bIsStaticSwitch);
	if (!AddedScriptVariable)
	{
		OutError = TEXT("add_emitter_parameter_failed");
		return false;
	}

	if (bHasDefaultValueText)
	{
		if (!UeAgentNiagaraOps::SetScriptVariableDefaultValueFromText(*AddedScriptVariable, DefaultValueText, OutError))
		{
			return false;
		}
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildGraphParameterJson(NewParameter, AddedScriptVariable, true));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterParameterRemove(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	FNiagaraVariable ParameterToRemove;
	UNiagaraScriptVariable* ScriptVariable = nullptr;
	if (!UeAgentNiagaraOps::FindGraphParameterByName(*EditContext.Graph, ParameterName, ParameterToRemove, ScriptVariable))
	{
		OutError = TEXT("emitter_parameter_not_found");
		return false;
	}

	const FString RemovedParameterName = ParameterToRemove.GetName().ToString();
	const FString RemovedParameterType = ParameterToRemove.GetType().GetNameText().ToString();

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "RemoveNiagaraEmitterParameter", "UeAgentInterface Remove Niagara Emitter Parameter"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	if (!UeAgentNiagaraOps::RemoveGraphParameterMetadata(*EditContext.Graph, ParameterToRemove))
	{
		OutError = TEXT("remove_emitter_parameter_failed");
		return false;
	}
	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("removed_parameter_name"), RemovedParameterName);
	OutData->SetStringField(TEXT("removed_parameter_type"), RemovedParameterType);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterParameterGet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	FNiagaraVariable Parameter;
	UNiagaraScriptVariable* ScriptVariable = nullptr;
	if (!UeAgentNiagaraOps::FindGraphParameterByName(*EditContext.Graph, ParameterName, Parameter, ScriptVariable))
	{
		OutError = TEXT("emitter_parameter_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildGraphParameterJson(Parameter, ScriptVariable, true));
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterParameterSet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	const bool bHasValueText = Ctx.Params.IsValid() && (Ctx.Params->HasField(TEXT("value_text")) || Ctx.Params->HasField(TEXT("default_value_text")));
	const bool bHasNewName = Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("new_parameter_name"));
	const bool bHasType = Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("parameter_type"));
	if (!bHasValueText && !bHasNewName && !bHasType)
	{
		OutError = TEXT("missing_set_operation");
		return false;
	}

	FNiagaraVariable ExistingParameter;
	UNiagaraScriptVariable* ExistingScriptVariable = nullptr;
	if (!UeAgentNiagaraOps::FindGraphParameterByName(*EditContext.Graph, ParameterName, ExistingParameter, ExistingScriptVariable))
	{
		OutError = TEXT("emitter_parameter_not_found");
		return false;
	}

	FNiagaraVariable WorkingParameter = ExistingParameter;
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraEmitterParameter", "UeAgentInterface Set Niagara Emitter Parameter"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();

	if (bHasType)
	{
		FString NewTypeText;
		if (!JsonTryGetString(Ctx.Params, TEXT("parameter_type"), NewTypeText) || NewTypeText.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("invalid_parameter_type");
			return false;
		}

		FNiagaraTypeDefinition NewType;
		if (!UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(NewTypeText, NewType))
		{
			OutError = TEXT("invalid_parameter_type");
			return false;
		}

		if (!(WorkingParameter.GetType() == NewType))
		{
			TArray<FNiagaraVariable> ParametersToChange;
			ParametersToChange.Add(WorkingParameter);
			EditContext.Graph->ChangeParameterType(ParametersToChange, NewType, false);
			WorkingParameter.SetType(NewType);
		}
	}

	if (bHasNewName)
	{
		FString NewParameterName;
		if (!JsonTryGetString(Ctx.Params, TEXT("new_parameter_name"), NewParameterName) || NewParameterName.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("invalid_new_parameter_name");
			return false;
		}
		NewParameterName = NewParameterName.TrimStartAndEnd();

		const bool bRenamed = EditContext.Graph->RenameParameter(WorkingParameter, *NewParameterName, ExistingScriptVariable ? ExistingScriptVariable->GetIsStaticSwitch() : false, nullptr, false);
		if (!bRenamed)
		{
			OutError = TEXT("rename_parameter_failed");
			return false;
		}
		WorkingParameter.SetName(*NewParameterName);
	}

	UNiagaraScriptVariable* TargetScriptVariable = EditContext.Graph->GetScriptVariable(WorkingParameter.GetName());
	if (!TargetScriptVariable)
	{
		OutError = TEXT("updated_parameter_not_found");
		return false;
	}

	if (bHasValueText)
	{
		FString ValueText;
		if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
		{
			JsonTryGetString(Ctx.Params, TEXT("default_value_text"), ValueText);
		}
		if (!UeAgentNiagaraOps::SetScriptVariableDefaultValueFromText(*TargetScriptVariable, ValueText, OutError))
		{
			return false;
		}
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildGraphParameterJson(WorkingParameter, TargetScriptVariable, true));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemListEmitters(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	const TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
	const int32 EmitterCompiledDataCount = NiagaraSystem->GetEmitterCompiledData().Num();
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Handles.Num());
	for (int32 Index = 0; Index < Handles.Num(); ++Index)
	{
		const FNiagaraEmitterHandle& Handle = Handles[Index];
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetStringField(TEXT("id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		Item->SetStringField(TEXT("name"), Handle.GetName().ToString());
		Item->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		if (const UNiagaraEmitter* EmitterAsset = Handle.GetInstance().Emitter)
		{
			Item->SetStringField(TEXT("instance_object_path"), EmitterAsset->GetPathName());
		}
		Item->SetStringField(TEXT("instance_version"), Handle.GetInstance().Version.ToString(EGuidFormats::DigitsWithHyphensLower));

		FVersionedNiagaraEmitterData* EmitterData = Handle.GetInstance().GetEmitterData();
		Item->SetBoolField(TEXT("has_emitter_data"), EmitterData != nullptr);
		Item->SetBoolField(TEXT("compiled_data_index_valid"), Index < EmitterCompiledDataCount);
		Item->SetBoolField(TEXT("runtime_allowed_by_handle"), Handle.IsAllowedByScalability());
		if (EmitterData)
		{
			Item->SetBoolField(TEXT("emitter_data_valid"), EmitterData->IsValid());
			Item->SetBoolField(TEXT("emitter_ready_to_run"), EmitterData->IsReadyToRun());
			Item->SetBoolField(TEXT("emitter_allowed_by_scalability"), EmitterData->IsAllowedByScalability());
			Item->SetBoolField(TEXT("emitter_allowed_to_execute"), EmitterData->IsAllowedToExecute());
			Item->SetStringField(TEXT("sim_target"), StaticEnum<ENiagaraSimTarget>() ? StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(EmitterData->SimTarget)) : TEXT(""));
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("compiled_emitter_data_count"), EmitterCompiledDataCount);
	OutData->SetArrayField(TEXT("emitters"), Items);
	OutData->SetNumberField(TEXT("emitter_count"), Items.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemAddEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SystemAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("system_asset_path"), SystemAssetPathInput) || SystemAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_system_asset_path");
		return false;
	}

	FString EmitterAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("emitter_asset_path"), EmitterAssetPathInput) || EmitterAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_emitter_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(SystemAssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	UNiagaraEmitter* EmitterAsset = Cast<UNiagaraEmitter>(UeAgentNiagaraOps::LoadAssetObject(EmitterAssetPathInput));
	if (!EmitterAsset)
	{
		OutError = TEXT("emitter_asset_not_found");
		return false;
	}

	FString EmitterNameString;
	JsonTryGetString(Ctx.Params, TEXT("emitter_name"), EmitterNameString);
	if (EmitterNameString.IsEmpty())
	{
		EmitterNameString = EmitterAsset->GetName();
	}

	FGuid EmitterVersionGuid = EmitterAsset->GetExposedVersion().VersionGuid;
	FString EmitterVersionString;
	if (JsonTryGetString(Ctx.Params, TEXT("emitter_version"), EmitterVersionString) && !EmitterVersionString.IsEmpty())
	{
		FGuid ParsedGuid;
		if (FGuid::Parse(EmitterVersionString, ParsedGuid))
		{
			EmitterVersionGuid = ParsedGuid;
		}
	}

	bool bCloseOpenEditorBeforeStructuralEdit = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	const int32 ClosedEditorCount = bCloseOpenEditorBeforeStructuralEdit
		? UeAgentNiagaraOps::CloseAssetEditorsForStructuralNiagaraEdit(*NiagaraSystem)
		: 0;

	NiagaraSystem->Modify();
	const FNiagaraEmitterHandle NewHandle = NiagaraSystem->AddEmitterHandle(*EmitterAsset, *EmitterNameString, EmitterVersionGuid);
	const UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult =
		UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, true);

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, Ctx.Params, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	const TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
	int32 HandleIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Handles.Num(); ++Index)
	{
		if (Handles[Index].GetId() == NewHandle.GetId())
		{
			HandleIndex = Index;
			break;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("added_emitter_id"), NewHandle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("added_emitter_name"), NewHandle.GetName().ToString());
	OutData->SetNumberField(TEXT("added_emitter_index"), HandleIndex);
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("synchronized_overview_graph"), RefreshResult.bSynchronizedOverviewGraph);
	OutData->SetNumberField(TEXT("overview_node_count_before"), RefreshResult.OverviewNodeCountBefore);
	OutData->SetNumberField(TEXT("overview_node_count_after"), RefreshResult.OverviewNodeCountAfter);
	OutData->SetBoolField(TEXT("closed_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	OutData->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemRemoveEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SystemAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("system_asset_path"), SystemAssetPathInput) || SystemAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_system_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(SystemAssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	const TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
	if (Handles.Num() == 0)
	{
		OutError = TEXT("system_has_no_emitters");
		return false;
	}

	const FNiagaraEmitterHandle* HandleToRemove = nullptr;
	int32 HandleIndex = INDEX_NONE;

	double HandleIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("emitter_index"), HandleIndexNumber))
	{
		const int32 Index = FMath::RoundToInt(HandleIndexNumber);
		if (Handles.IsValidIndex(Index))
		{
			HandleToRemove = &Handles[Index];
			HandleIndex = Index;
		}
	}

	if (!HandleToRemove)
	{
		FString EmitterIdString;
		if (JsonTryGetString(Ctx.Params, TEXT("emitter_id"), EmitterIdString) && !EmitterIdString.IsEmpty())
		{
			FGuid EmitterGuid;
			if (FGuid::Parse(EmitterIdString, EmitterGuid))
			{
				for (int32 Index = 0; Index < Handles.Num(); ++Index)
				{
					if (Handles[Index].GetId() == EmitterGuid)
					{
						HandleToRemove = &Handles[Index];
						HandleIndex = Index;
						break;
					}
				}
			}
		}
	}

	if (!HandleToRemove)
	{
		FString EmitterName;
		if (JsonTryGetString(Ctx.Params, TEXT("emitter_name"), EmitterName) && !EmitterName.IsEmpty())
		{
			for (int32 Index = 0; Index < Handles.Num(); ++Index)
			{
				if (Handles[Index].GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
				{
					HandleToRemove = &Handles[Index];
					HandleIndex = Index;
					break;
				}
			}
		}
	}

	if (!HandleToRemove)
	{
		OutError = TEXT("emitter_not_found");
		return false;
	}

	const FString RemovedId = HandleToRemove->GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString RemovedName = HandleToRemove->GetName().ToString();

	bool bCloseOpenEditorBeforeStructuralEdit = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	const int32 ClosedEditorCount = bCloseOpenEditorBeforeStructuralEdit
		? UeAgentNiagaraOps::CloseAssetEditorsForStructuralNiagaraEdit(*NiagaraSystem)
		: 0;

	NiagaraSystem->Modify();
	NiagaraSystem->RemoveEmitterHandle(*HandleToRemove);
	int32 OverviewNodeCountBefore = INDEX_NONE;
	int32 OverviewNodeCountAfter = INDEX_NONE;
	const bool bSynchronizedOverviewGraph = UeAgentNiagaraOps::SynchronizeSystemEditorData(*NiagaraSystem, OverviewNodeCountBefore, OverviewNodeCountAfter);
	NiagaraSystem->MarkPackageDirty();

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, Ctx.Params, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("removed_emitter_id"), RemovedId);
	OutData->SetStringField(TEXT("removed_emitter_name"), RemovedName);
	OutData->SetNumberField(TEXT("removed_emitter_index"), HandleIndex);
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("synchronized_overview_graph"), bSynchronizedOverviewGraph);
	OutData->SetNumberField(TEXT("overview_node_count_before"), OverviewNodeCountBefore);
	OutData->SetNumberField(TEXT("overview_node_count_after"), OverviewNodeCountAfter);
	OutData->SetBoolField(TEXT("closed_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	OutData->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemMoveEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SystemAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("system_asset_path"), SystemAssetPathInput) || SystemAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_system_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(SystemAssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	double TargetIndexNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		OutError = TEXT("missing_target_index");
		return false;
	}

	int32 EmitterIndex = INDEX_NONE;
	FNiagaraEmitterHandle* Handle = nullptr;
	if (!UeAgentNiagaraOps::ResolveEmitterHandleFromParams(*NiagaraSystem, Ctx.Params, EmitterIndex, Handle, OutError))
	{
		return false;
	}

	TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
	const int32 TargetIndex = FMath::Clamp(FMath::RoundToInt(TargetIndexNumber), 0, Handles.Num() - 1);

	const FString EmitterId = Handle->GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString EmitterName = Handle->GetName().ToString();
	const int32 SourceIndex = EmitterIndex;
	const bool bMoved = SourceIndex != TargetIndex;
	bool bCloseOpenEditorBeforeStructuralEdit = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	const int32 ClosedEditorCount = (bMoved && bCloseOpenEditorBeforeStructuralEdit)
		? UeAgentNiagaraOps::CloseAssetEditorsForStructuralNiagaraEdit(*NiagaraSystem)
		: 0;

	if (bMoved)
	{
		NiagaraSystem->Modify();
		const FNiagaraEmitterHandle MovedHandle = Handles[SourceIndex];
		Handles.RemoveAt(SourceIndex);
		Handles.Insert(MovedHandle, TargetIndex);
		int32 OverviewNodeCountBefore = INDEX_NONE;
		int32 OverviewNodeCountAfter = INDEX_NONE;
		UeAgentNiagaraOps::SynchronizeSystemEditorData(*NiagaraSystem, OverviewNodeCountBefore, OverviewNodeCountAfter);
		NiagaraSystem->MarkPackageDirty();
	}

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, Ctx.Params, true, bMoved);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_id"), EmitterId);
	OutData->SetStringField(TEXT("emitter_name"), EmitterName);
	OutData->SetNumberField(TEXT("source_index"), SourceIndex);
	OutData->SetNumberField(TEXT("target_index"), TargetIndex);
	OutData->SetBoolField(TEXT("moved"), bMoved);
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("closed_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	OutData->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemSetEmitterEnabled(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SystemAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("system_asset_path"), SystemAssetPathInput) || SystemAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_system_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(SystemAssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	bool bEnabled = false;
	if (!JsonTryGetBool(Ctx.Params, TEXT("enabled"), bEnabled))
	{
		OutError = TEXT("missing_enabled");
		return false;
	}

	int32 EmitterIndex = INDEX_NONE;
	FNiagaraEmitterHandle* Handle = nullptr;
	if (!UeAgentNiagaraOps::ResolveEmitterHandleFromParams(*NiagaraSystem, Ctx.Params, EmitterIndex, Handle, OutError))
	{
		return false;
	}

	const FString EmitterId = Handle->GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString EmitterName = Handle->GetName().ToString();
	const bool bOldEnabled = Handle->GetIsEnabled();
	bool bCloseOpenEditorBeforeStructuralEdit = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	const int32 ClosedEditorCount = bCloseOpenEditorBeforeStructuralEdit
		? UeAgentNiagaraOps::CloseAssetEditorsForStructuralNiagaraEdit(*NiagaraSystem)
		: 0;

	NiagaraSystem->Modify();
	const bool bChanged = Handle->SetIsEnabled(bEnabled, *NiagaraSystem, false);
	if (bChanged)
	{
		int32 OverviewNodeCountBefore = INDEX_NONE;
		int32 OverviewNodeCountAfter = INDEX_NONE;
		UeAgentNiagaraOps::SynchronizeSystemEditorData(*NiagaraSystem, OverviewNodeCountBefore, OverviewNodeCountAfter);
		NiagaraSystem->MarkPackageDirty();
	}

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, Ctx.Params, true, bChanged);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_id"), EmitterId);
	OutData->SetStringField(TEXT("emitter_name"), EmitterName);
	OutData->SetNumberField(TEXT("emitter_index"), EmitterIndex);
	OutData->SetBoolField(TEXT("enabled_before"), bOldEnabled);
	OutData->SetBoolField(TEXT("enabled_after"), Handle->GetIsEnabled());
	OutData->SetBoolField(TEXT("changed"), bChanged);
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("closed_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	OutData->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemSetEmitterVersion(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SystemAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("system_asset_path"), SystemAssetPathInput) || SystemAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_system_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(SystemAssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FString TargetVersionText;
	if (!JsonTryGetString(Ctx.Params, TEXT("emitter_version"), TargetVersionText) || TargetVersionText.IsEmpty())
	{
		OutError = TEXT("missing_emitter_version");
		return false;
	}

	FGuid TargetVersionGuid;
	if (!FGuid::Parse(TargetVersionText, TargetVersionGuid))
	{
		OutError = TEXT("invalid_emitter_version");
		return false;
	}

	int32 EmitterIndex = INDEX_NONE;
	FNiagaraEmitterHandle* Handle = nullptr;
	if (!UeAgentNiagaraOps::ResolveEmitterHandleFromParams(*NiagaraSystem, Ctx.Params, EmitterIndex, Handle, OutError))
	{
		return false;
	}

	FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();
	if (!VersionedEmitter.Emitter)
	{
		OutError = TEXT("emitter_instance_missing");
		return false;
	}
	if (!VersionedEmitter.Emitter->GetEmitterData(TargetVersionGuid))
	{
		OutError = TEXT("target_emitter_version_not_found");
		return false;
	}

	const FString EmitterId = Handle->GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString EmitterName = Handle->GetName().ToString();
	const FString PreviousVersion = VersionedEmitter.Version.ToString(EGuidFormats::DigitsWithHyphensLower);
	const bool bVersionChanged = VersionedEmitter.Version != TargetVersionGuid;
	bool bCloseOpenEditorBeforeStructuralEdit = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	const int32 ClosedEditorCount = (bVersionChanged && bCloseOpenEditorBeforeStructuralEdit)
		? UeAgentNiagaraOps::CloseAssetEditorsForStructuralNiagaraEdit(*NiagaraSystem)
		: 0;

	bool bChanged = false;
	if (bVersionChanged)
	{
		NiagaraSystem->Modify();
		bChanged = NiagaraSystem->ChangeEmitterVersion(VersionedEmitter, TargetVersionGuid);
		if (!bChanged)
		{
			OutError = TEXT("change_emitter_version_failed");
			return false;
		}
		int32 OverviewNodeCountBefore = INDEX_NONE;
		int32 OverviewNodeCountAfter = INDEX_NONE;
		UeAgentNiagaraOps::SynchronizeSystemEditorData(*NiagaraSystem, OverviewNodeCountBefore, OverviewNodeCountAfter);
		NiagaraSystem->MarkPackageDirty();
	}

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, Ctx.Params, true, bVersionChanged);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	FString CurrentVersion = PreviousVersion;
	if (NiagaraSystem->GetEmitterHandles().IsValidIndex(EmitterIndex))
	{
		CurrentVersion = NiagaraSystem->GetEmitterHandles()[EmitterIndex].GetInstance().Version.ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_id"), EmitterId);
	OutData->SetStringField(TEXT("emitter_name"), EmitterName);
	OutData->SetNumberField(TEXT("emitter_index"), EmitterIndex);
	OutData->SetStringField(TEXT("previous_version"), PreviousVersion);
	OutData->SetStringField(TEXT("current_version"), CurrentVersion);
	OutData->SetBoolField(TEXT("changed"), bVersionChanged && bChanged);
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("closed_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	OutData->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterGetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyName = UeAgentNiagaraOps::NormalizeEventHandlerPropertyName(PropertyName);

	FString ValueText;
	FString CppType;
	if (!UeAgentNiagaraOps::GetStructPropertyText(FVersionedNiagaraEmitterData::StaticStruct(), EditContext.EmitterData, EditContext.Emitter, PropertyName, ValueText, CppType))
	{
		OutError = TEXT("get_emitter_property_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), EditContext.Emitter->GetPathName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("property_found"), true);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}

	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraEmitterProperty", "UeAgentInterface Set Niagara Emitter Property"));
	EditContext.Emitter->Modify();
	if (EditContext.ScriptSource)
	{
		EditContext.ScriptSource->Modify();
	}
	if (EditContext.Graph)
	{
		EditContext.Graph->Modify();
	}

	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetStructPropertyText(FVersionedNiagaraEmitterData::StaticStruct(), EditContext.EmitterData, EditContext.Emitter, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("object_path"), EditContext.Emitter->GetPathName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutError = ImportError.IsEmpty() ? TEXT("set_emitter_property_failed") : ImportError;
		return false;
	}
	const UeAgentNiagaraOps::FEmitterPropertySideEffectResult SideEffects = UeAgentNiagaraOps::ApplyEmitterPropertySideEffects(EditContext.EmitterData, PropertyName);

	EditContext.Emitter->MarkPackageDirty();
	if (EditContext.Graph)
	{
		EditContext.Graph->NotifyGraphChanged();
	}

	UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult;
	if (UNiagaraSystem* OwnerSystem = Cast<UNiagaraSystem>(EditContext.Emitter->GetOuter()))
	{
		CompileResult = UeAgentNiagaraOps::CompileSystemAfterSet(OwnerSystem, Ctx.Params, true);
	}
	else
	{
		CompileResult = UeAgentNiagaraOps::CompileEmitterScriptsAfterSet(EditContext.EmitterData, Ctx.Params, true);
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), EditContext.Emitter->GetPathName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("side_effect_handled"), SideEffects.bHandled);
	OutData->SetBoolField(TEXT("updated_spawn_script_usage"), SideEffects.bUpdatedSpawnScriptUsage);
	OutData->SetBoolField(TEXT("marked_graph_source_unsynchronized"), SideEffects.bMarkedGraphSourceUnsynchronized);
	if (SideEffects.bHandled)
	{
		OutData->SetStringField(TEXT("old_spawn_script_usage"), UeAgentNiagaraOps::ScriptUsageToString(SideEffects.OldSpawnScriptUsage));
		OutData->SetStringField(TEXT("new_spawn_script_usage"), UeAgentNiagaraOps::ScriptUsageToString(SideEffects.NewSpawnScriptUsage));
	}
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}


bool FUeAgentHttpServer::CmdNiagaraEmitterListRenderers(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	bool bIncludeProperties = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_properties"), bIncludeProperties);

	const TArray<UNiagaraRendererProperties*>& Renderers = EditContext.EmitterData->GetRenderers();
	TArray<TSharedPtr<FJsonValue>> RenderersJson;
	RenderersJson.Reserve(Renderers.Num());
	for (int32 RendererIndex = 0; RendererIndex < Renderers.Num(); ++RendererIndex)
	{
		UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
		if (!Renderer)
		{
			continue;
		}

		TSharedPtr<FJsonObject> RendererObj = UeAgentNiagaraOps::BuildRendererJson(*Renderer, RendererIndex);
		if (bIncludeProperties)
		{
			TArray<TSharedPtr<FJsonValue>> PropertiesJson;
			for (TFieldIterator<FProperty> It(Renderer->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property)
				{
					continue;
				}

				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Renderer);
				PropertiesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildPropertyValueJson(*Renderer, *Property, ValuePtr)));
			}
			RendererObj->SetArrayField(TEXT("properties"), PropertiesJson);
			RendererObj->SetNumberField(TEXT("property_count"), PropertiesJson.Num());
		}

		RenderersJson.Add(MakeShared<FJsonValueObject>(RendererObj));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("renderers"), RenderersJson);
	OutData->SetNumberField(TEXT("renderer_count"), RenderersJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterAddRenderer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	FString RendererClassText;
	if (!JsonTryGetString(Ctx.Params, TEXT("renderer_class"), RendererClassText))
	{
		JsonTryGetString(Ctx.Params, TEXT("renderer_type"), RendererClassText);
	}
	if (RendererClassText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_renderer_class");
		return false;
	}

	UClass* RendererClass = UeAgentNiagaraOps::ResolveRendererClass(RendererClassText);
	if (!RendererClass)
	{
		OutError = TEXT("invalid_renderer_class");
		return false;
	}

	FString RendererName;
	JsonTryGetString(Ctx.Params, TEXT("renderer_name"), RendererName);
	RendererName.TrimStartAndEndInline();

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraRenderer", "UeAgentInterface Add Niagara Renderer"));
	EditContext.Emitter->Modify();

	UNiagaraRendererProperties* NewRenderer = NewObject<UNiagaraRendererProperties>(
		EditContext.Emitter,
		RendererClass,
		RendererName.IsEmpty() ? NAME_None : FName(*RendererName),
		RF_Transactional);
	if (!NewRenderer)
	{
		OutError = TEXT("add_renderer_failed");
		return false;
	}

	EditContext.Emitter->AddRenderer(NewRenderer, EditContext.VersionGuid);

	FString PropertyName;
	FString ValueText;
	TSharedPtr<FJsonObject> InitialPropertyResult;
	if (JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) && JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		FString AppliedValueText;
		FString CppType;
		FString ImportError;
		if (!UeAgentNiagaraOps::SetObjectPropertyText(NewRenderer, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
		{
			const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
			EditContext.Emitter->RemoveRenderer(NewRenderer, EditContext.VersionGuid);
			SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
			OutData->SetStringField(TEXT("property_name"), PropertyName);
			OutData->SetStringField(TEXT("value_text"), ValueText);
			OutData->SetStringField(TEXT("cpp_type"), CppType);
			OutData->SetBoolField(TEXT("renderer_removed_after_property_failure"), true);
			OutError = ImportError.IsEmpty() ? TEXT("set_renderer_property_failed") : ImportError;
			return false;
		}

		InitialPropertyResult = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
		InitialPropertyResult->SetStringField(TEXT("cpp_type"), CppType);
	}

	EditContext.Emitter->MarkPackageDirty();

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterScriptsAfterSet(EditContext.EmitterData, Ctx.Params, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	int32 AddedRendererIndex = INDEX_NONE;
	const TArray<UNiagaraRendererProperties*>& Renderers = EditContext.EmitterData->GetRenderers();
	for (int32 Index = 0; Index < Renderers.Num(); ++Index)
	{
		if (Renderers[Index] == NewRenderer)
		{
			AddedRendererIndex = Index;
			break;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("renderer"), UeAgentNiagaraOps::BuildRendererJson(*NewRenderer, AddedRendererIndex));
	if (InitialPropertyResult.IsValid())
	{
		OutData->SetObjectField(TEXT("initial_property_import_result"), InitialPropertyResult);
	}
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterRemoveRenderer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	int32 RendererIndex = INDEX_NONE;
	UNiagaraRendererProperties* Renderer = nullptr;
	if (!UeAgentNiagaraOps::ResolveRendererFromParams(EditContext, Ctx.Params, RendererIndex, Renderer, OutError))
	{
		return false;
	}

	const FString RemovedRendererName = Renderer->GetFName().ToString();
	const FString RemovedRendererClass = Renderer->GetClass()->GetPathName();

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "RemoveNiagaraRenderer", "UeAgentInterface Remove Niagara Renderer"));
	EditContext.Emitter->Modify();
	EditContext.Emitter->RemoveRenderer(Renderer, EditContext.VersionGuid);
	EditContext.Emitter->MarkPackageDirty();

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterScriptsAfterSet(EditContext.EmitterData, Ctx.Params, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("removed_renderer_name"), RemovedRendererName);
	OutData->SetStringField(TEXT("removed_renderer_class"), RemovedRendererClass);
	OutData->SetNumberField(TEXT("removed_renderer_index"), RendererIndex);
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterMoveRenderer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	int32 RendererIndex = INDEX_NONE;
	UNiagaraRendererProperties* Renderer = nullptr;
	if (!UeAgentNiagaraOps::ResolveRendererFromParams(EditContext, Ctx.Params, RendererIndex, Renderer, OutError))
	{
		return false;
	}

	double TargetIndexNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		OutError = TEXT("missing_target_index");
		return false;
	}

	const TArray<UNiagaraRendererProperties*>& ExistingRenderers = EditContext.EmitterData->GetRenderers();
	const int32 TargetIndex = FMath::RoundToInt(TargetIndexNumber);
	if (!ExistingRenderers.IsValidIndex(TargetIndex))
	{
		OutError = TEXT("invalid_target_index");
		return false;
	}

	if (RendererIndex == TargetIndex)
	{
		OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("renderer_name"), Renderer->GetFName().ToString());
		OutData->SetNumberField(TEXT("old_index"), RendererIndex);
		OutData->SetNumberField(TEXT("target_index"), TargetIndex);
		OutData->SetNumberField(TEXT("new_index"), RendererIndex);
		OutData->SetBoolField(TEXT("moved"), false);
		return true;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "MoveNiagaraRenderer", "UeAgentInterface Move Niagara Renderer"));
	EditContext.Emitter->Modify();
	Renderer->Modify();
	EditContext.Emitter->MoveRenderer(Renderer, TargetIndex, EditContext.VersionGuid);
	EditContext.Emitter->MarkPackageDirty();

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterScriptsAfterSet(EditContext.EmitterData, Ctx.Params, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	int32 NewRendererIndex = INDEX_NONE;
	const TArray<UNiagaraRendererProperties*>& UpdatedRenderers = EditContext.EmitterData->GetRenderers();
	for (int32 Index = 0; Index < UpdatedRenderers.Num(); ++Index)
	{
		if (UpdatedRenderers[Index] == Renderer)
		{
			NewRendererIndex = Index;
			break;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("renderer_name"), Renderer->GetFName().ToString());
	OutData->SetNumberField(TEXT("old_index"), RendererIndex);
	OutData->SetNumberField(TEXT("target_index"), TargetIndex);
	OutData->SetNumberField(TEXT("new_index"), NewRendererIndex);
	OutData->SetObjectField(TEXT("renderer"), UeAgentNiagaraOps::BuildRendererJson(*Renderer, NewRendererIndex));
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	OutData->SetBoolField(TEXT("moved"), true);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterGetRendererProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	int32 RendererIndex = INDEX_NONE;
	UNiagaraRendererProperties* Renderer = nullptr;
	if (!UeAgentNiagaraOps::ResolveRendererFromParams(EditContext, Ctx.Params, RendererIndex, Renderer, OutError))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyName = UeAgentNiagaraOps::NormalizeEventHandlerPropertyName(PropertyName);

	FString ValueText;
	if (!UeAgentNiagaraOps::GetObjectPropertyText(Renderer, PropertyName, ValueText))
	{
		OutError = TEXT("get_renderer_property_failed");
		return false;
	}

	FProperty* ResolvedProperty = nullptr;
	void* ValuePtr = nullptr;
	UeAgentNiagaraOps::ResolvePropertyPath(Renderer, PropertyName, ResolvedProperty, ValuePtr);

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetNumberField(TEXT("renderer_index"), RendererIndex);
	OutData->SetStringField(TEXT("renderer_name"), Renderer->GetFName().ToString());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("cpp_type"), ResolvedProperty ? ResolvedProperty->GetCPPType() : TEXT(""));
	OutData->SetBoolField(TEXT("property_found"), ResolvedProperty != nullptr);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetRendererProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	int32 RendererIndex = INDEX_NONE;
	UNiagaraRendererProperties* Renderer = nullptr;
	if (!UeAgentNiagaraOps::ResolveRendererFromParams(EditContext, Ctx.Params, RendererIndex, Renderer, OutError))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}

	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraRendererProperty", "UeAgentInterface Set Niagara Renderer Property"));
	EditContext.Emitter->Modify();
	Renderer->Modify();
	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetObjectPropertyText(Renderer, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetStringField(TEXT("renderer_name"), Renderer->GetFName().ToString());
		OutData->SetStringField(TEXT("property_name"), PropertyName.TrimStartAndEnd());
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutError = ImportError.IsEmpty() ? TEXT("set_renderer_property_failed") : ImportError;
		return false;
	}
	EditContext.Emitter->MarkPackageDirty();

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterScriptsAfterSet(EditContext.EmitterData, Ctx.Params, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetNumberField(TEXT("renderer_index"), RendererIndex);
	OutData->SetStringField(TEXT("renderer_name"), Renderer->GetFName().ToString());
	OutData->SetStringField(TEXT("property_name"), PropertyName.TrimStartAndEnd());
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterListEventHandlers(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	const TArray<FNiagaraEventScriptProperties>& EventHandlers = EditContext.EmitterData->GetEventHandlers();
	TArray<TSharedPtr<FJsonValue>> EventsJson;
	EventsJson.Reserve(EventHandlers.Num());
	for (int32 EventIndex = 0; EventIndex < EventHandlers.Num(); ++EventIndex)
	{
		FNiagaraEventScriptProperties& EventHandler = const_cast<FNiagaraEventScriptProperties&>(EventHandlers[EventIndex]);
		EventsJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildEventHandlerJson(EventHandler, EventIndex)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("event_handlers"), EventsJson);
	OutData->SetNumberField(TEXT("event_handler_count"), EventsJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterAddEventHandler(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString SourceEventName = TEXT("Event");
	JsonTryGetString(Ctx.Params, TEXT("source_event_name"), SourceEventName);
	SourceEventName.TrimStartAndEndInline();
	if (SourceEventName.IsEmpty())
	{
		SourceEventName = TEXT("Event");
	}

	FGuid ScriptUsageId = FGuid::NewGuid();
	FString ScriptUsageIdText;
	if (JsonTryGetString(Ctx.Params, TEXT("script_usage_id"), ScriptUsageIdText) && !ScriptUsageIdText.IsEmpty())
	{
		if (!FGuid::Parse(ScriptUsageIdText, ScriptUsageId))
		{
			OutError = TEXT("invalid_script_usage_id");
			return false;
		}
	}

	FGuid SourceEmitterId;
	FString SourceEmitterIdText;
	if (JsonTryGetString(Ctx.Params, TEXT("source_emitter_id"), SourceEmitterIdText) && !SourceEmitterIdText.IsEmpty())
	{
		if (!FGuid::Parse(SourceEmitterIdText, SourceEmitterId))
		{
			OutError = TEXT("invalid_source_emitter_id");
			return false;
		}
	}

	FNiagaraEventScriptProperties NewEventHandler;
	NewEventHandler.SourceEventName = FName(*SourceEventName);
	NewEventHandler.SourceEmitterID = SourceEmitterId;

	FString ExecutionModeText;
	if (JsonTryGetString(Ctx.Params, TEXT("execution_mode"), ExecutionModeText) && !ExecutionModeText.IsEmpty())
	{
		EScriptExecutionMode ParsedExecutionMode = EScriptExecutionMode::EveryParticle;
		if (!UeAgentNiagaraOps::ParseExecutionMode(ExecutionModeText, ParsedExecutionMode))
		{
			OutError = TEXT("invalid_execution_mode");
			return false;
		}
		NewEventHandler.ExecutionMode = ParsedExecutionMode;
	}

	double NumberValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("spawn_number"), NumberValue))
	{
		NewEventHandler.SpawnNumber = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_events_per_frame"), NumberValue))
	{
		NewEventHandler.MaxEventsPerFrame = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("min_spawn_number"), NumberValue))
	{
		NewEventHandler.MinSpawnNumber = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
	}
	JsonTryGetBool(Ctx.Params, TEXT("random_spawn_number"), NewEventHandler.bRandomSpawnNumber);
	JsonTryGetBool(Ctx.Params, TEXT("update_attribute_initial_values"), NewEventHandler.UpdateAttributeInitialValues);

	NewEventHandler.Script = NewObject<UNiagaraScript>(
		EditContext.Emitter,
		MakeUniqueObjectName(EditContext.Emitter, UNiagaraScript::StaticClass(), TEXT("EventHandler")),
		RF_Transactional);
	if (!NewEventHandler.Script)
	{
		OutError = TEXT("create_event_handler_script_failed");
		return false;
	}
	NewEventHandler.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
	NewEventHandler.Script->SetUsageId(ScriptUsageId);
	NewEventHandler.Script->SetLatestSource(EditContext.ScriptSource);

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraEventHandler", "UeAgentInterface Add Niagara Event Handler"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	EditContext.Emitter->AddEventHandler(NewEventHandler, EditContext.VersionGuid);

	UNiagaraNodeOutput* EventOutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(
		*EditContext.Graph,
		ENiagaraScriptUsage::ParticleEventScript,
		ScriptUsageId);
	if (!EventOutputNode)
	{
		OutError = TEXT("create_event_handler_output_failed");
		return false;
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterScriptsAfterSet(EditContext.EmitterData, Ctx.Params, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	int32 AddedEventIndex = INDEX_NONE;
	FNiagaraEventScriptProperties* AddedEventHandler = nullptr;
	if (!UeAgentNiagaraOps::ResolveEventHandlerFromParams(EditContext, Ctx.Params, AddedEventIndex, AddedEventHandler, OutError))
	{
		OutError.Reset();
		for (int32 Index = 0; Index < EditContext.EmitterData->EventHandlerScriptProps.Num(); ++Index)
		{
			FNiagaraEventScriptProperties& EventHandler = EditContext.EmitterData->EventHandlerScriptProps[Index];
			if (EventHandler.Script && EventHandler.Script->GetUsageId() == ScriptUsageId)
			{
				AddedEventIndex = Index;
				AddedEventHandler = &EventHandler;
				break;
			}
		}
	}
	if (!AddedEventHandler)
	{
		OutError = TEXT("event_handler_added_but_not_resolved");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("event_handler"), UeAgentNiagaraOps::BuildEventHandlerJson(*AddedEventHandler, AddedEventIndex));
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterRemoveEventHandler(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	int32 EventIndex = INDEX_NONE;
	FNiagaraEventScriptProperties* EventHandler = nullptr;
	if (!UeAgentNiagaraOps::ResolveEventHandlerFromParams(EditContext, Ctx.Params, EventIndex, EventHandler, OutError))
	{
		return false;
	}

	const FString RemovedSourceEventName = EventHandler->SourceEventName.ToString();
	const FGuid RemovedUsageId = EventHandler->Script ? EventHandler->Script->GetUsageId() : FGuid();

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "RemoveNiagaraEventHandler", "UeAgentInterface Remove Niagara Event Handler"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	EditContext.Emitter->RemoveEventHandlerByUsageId(RemovedUsageId, EditContext.VersionGuid);
	if (RemovedUsageId.IsValid())
	{
		UeAgentNiagaraOps::DestroyGraphNodesForStage(*EditContext.Graph, ENiagaraScriptUsage::ParticleEventScript, RemovedUsageId);
	}
	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterScriptsAfterSet(EditContext.EmitterData, Ctx.Params, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetNumberField(TEXT("removed_event_index"), EventIndex);
	OutData->SetStringField(TEXT("removed_script_usage_id"), RemovedUsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("removed_source_event_name"), RemovedSourceEventName);
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterGetEventHandlerProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	int32 EventIndex = INDEX_NONE;
	FNiagaraEventScriptProperties* EventHandler = nullptr;
	if (!UeAgentNiagaraOps::ResolveEventHandlerFromParams(EditContext, Ctx.Params, EventIndex, EventHandler, OutError))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyName = UeAgentNiagaraOps::NormalizeEventHandlerPropertyName(PropertyName);

	FString ValueText;
	FString CppType;
	if (!UeAgentNiagaraOps::GetStructPropertyText(FNiagaraEventScriptProperties::StaticStruct(), EventHandler, EditContext.Emitter, PropertyName, ValueText, CppType))
	{
		OutError = TEXT("get_event_handler_property_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetNumberField(TEXT("event_index"), EventIndex);
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("property_found"), true);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetEventHandlerProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	int32 EventIndex = INDEX_NONE;
	FNiagaraEventScriptProperties* EventHandler = nullptr;
	if (!UeAgentNiagaraOps::ResolveEventHandlerFromParams(EditContext, Ctx.Params, EventIndex, EventHandler, OutError))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyName = UeAgentNiagaraOps::NormalizeEventHandlerPropertyName(PropertyName);

	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraEventHandlerProperty", "UeAgentInterface Set Niagara Event Handler Property"));
	EditContext.Emitter->Modify();
	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetStructPropertyText(FNiagaraEventScriptProperties::StaticStruct(), EventHandler, EditContext.Emitter, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara SetEventHandlerProperty FAILED: emitter=%s event_index=%d property=%s value=%s"),
			*EditContext.Emitter->GetPathName(),
			EventIndex,
			*PropertyName,
			*ValueText);
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetNumberField(TEXT("event_index"), EventIndex);
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutError = ImportError.IsEmpty() ? TEXT("set_event_handler_property_failed") : ImportError;
		return false;
	}
	FUeAgentInterfaceLogger::Log(
		TEXT("Niagara SetEventHandlerProperty: emitter=%s event_index=%d property=%s value=%s"),
		*EditContext.Emitter->GetPathName(),
		EventIndex,
		*PropertyName,
		*ValueText);
	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterScriptsAfterSet(EditContext.EmitterData, Ctx.Params, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetNumberField(TEXT("event_index"), EventIndex);
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	UeAgentNiagaraOps::AppendCompileAfterSetFields(OutData, CompileResult);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}


bool FUeAgentHttpServer::CmdNiagaraEmitterListStages(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	bool bIncludeModules = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_modules"), bIncludeModules);
	bool bIncludeModuleInputs = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_module_inputs"), bIncludeModuleInputs);

	TArray<UNiagaraNodeOutput*> OutputNodes;
	UeAgentNiagaraOps::GetOutputNodesFromGraph(*EditContext.Graph, OutputNodes);

	TArray<TSharedPtr<FJsonValue>> StagesJson;
	StagesJson.Reserve(OutputNodes.Num());
	for (UNiagaraNodeOutput* OutputNode : OutputNodes)
	{
		if (!OutputNode)
		{
			continue;
		}

		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		const ENiagaraScriptUsage Usage = OutputNode->GetUsage();
		const FGuid UsageId = OutputNode->GetUsageId();

		StageObj->SetStringField(TEXT("stage_key"), UeAgentNiagaraOps::MakeStageKey(Usage, UsageId));
		StageObj->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Usage));
		StageObj->SetStringField(TEXT("script_usage_id"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		StageObj->SetStringField(TEXT("output_node_guid"), OutputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

		FString StageType = TEXT("builtin");
		if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			StageType = TEXT("simulation_stage");
		}
		else if (Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			StageType = TEXT("event_stage");
		}
		StageObj->SetStringField(TEXT("stage_type"), StageType);

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		UeAgentNiagaraOps::GetStageModuleNodes(*OutputNode, ModuleNodes);
		StageObj->SetNumberField(TEXT("module_count"), ModuleNodes.Num());

		if (bIncludeModules)
		{
			TArray<TSharedPtr<FJsonValue>> ModulesJson;
			ModulesJson.Reserve(ModuleNodes.Num());
			for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
			{
				UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
				if (!ModuleNode)
				{
					continue;
				}
				ModulesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleNodeJson(*ModuleNode, ModuleIndex, bIncludeModuleInputs)));
			}
			StageObj->SetArrayField(TEXT("modules"), ModulesJson);
		}

		StagesJson.Add(MakeShared<FJsonValueObject>(StageObj));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("stages"), StagesJson);
	OutData->SetNumberField(TEXT("stage_count"), StagesJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterAddSimulationStage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString StageClassPath = TEXT("/Script/Niagara.NiagaraSimulationStageGeneric");
	JsonTryGetString(Ctx.Params, TEXT("stage_class"), StageClassPath);
	UClass* StageClass = UeAgentNiagaraOps::ResolveClassByPath(StageClassPath);
	if (!StageClass || !StageClass->IsChildOf(UNiagaraSimulationStageBase::StaticClass()))
	{
		OutError = TEXT("invalid_stage_class");
		return false;
	}
	if (StageClass->HasAnyClassFlags(CLASS_Abstract))
	{
		OutError = TEXT("stage_class_is_abstract");
		return false;
	}

	FString StageName = TEXT("SimulationStage");
	JsonTryGetString(Ctx.Params, TEXT("stage_name"), StageName);
	StageName.TrimStartAndEndInline();
	if (StageName.IsEmpty())
	{
		StageName = TEXT("SimulationStage");
	}

	int32 TargetIndex = INDEX_NONE;
	double TargetIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		TargetIndex = FMath::Max(0, FMath::RoundToInt(TargetIndexNumber));
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraSimulationStage", "UeAgentInterface Add Niagara Simulation Stage"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();

	UNiagaraSimulationStageBase* AddedStage = NewObject<UNiagaraSimulationStageBase>(EditContext.Emitter, StageClass, NAME_None, RF_Transactional);
	if (!AddedStage)
	{
		OutError = TEXT("create_simulation_stage_failed");
		return false;
	}

	AddedStage->Script = NewObject<UNiagaraScript>(
		AddedStage,
		MakeUniqueObjectName(AddedStage, UNiagaraScript::StaticClass(), TEXT("SimulationStage")),
		RF_Transactional);
	if (!AddedStage->Script)
	{
		OutError = TEXT("create_simulation_stage_script_failed");
		return false;
	}

	AddedStage->SimulationStageName = FName(*StageName);
	AddedStage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
	AddedStage->Script->SetUsageId(FGuid::NewGuid());
	AddedStage->Script->SetLatestSource(EditContext.ScriptSource);
	EditContext.Emitter->AddSimulationStage(AddedStage, EditContext.VersionGuid);
	if (TargetIndex != INDEX_NONE)
	{
		EditContext.Emitter->MoveSimulationStageToIndex(AddedStage, TargetIndex, EditContext.VersionGuid);
	}

	TArray<UNiagaraNodeOutput*> ExistingOutputs;
	UeAgentNiagaraOps::GetOutputNodesFromGraph(*EditContext.Graph, ExistingOutputs);
	bool bFoundOutputNode = false;
	for (UNiagaraNodeOutput* ExistingOutput : ExistingOutputs)
	{
		if (ExistingOutput &&
			ExistingOutput->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript &&
			ExistingOutput->GetUsageId() == AddedStage->Script->GetUsageId())
		{
			bFoundOutputNode = true;
			break;
		}
	}

	if (!bFoundOutputNode)
	{
		UNiagaraNodeOutput* OutputNode = NewObject<UNiagaraNodeOutput>(EditContext.Graph, NAME_None, RF_Transactional);
		if (OutputNode)
		{
			OutputNode->CreateNewGuid();
			OutputNode->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
			OutputNode->SetUsageId(AddedStage->Script->GetUsageId());
			OutputNode->AllocateDefaultPins();
			EditContext.Graph->AddNode(OutputNode, true, false);
		}
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("stage_key"), UeAgentNiagaraOps::MakeStageKey(ENiagaraScriptUsage::ParticleSimulationStageScript, AddedStage->Script->GetUsageId()));
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(ENiagaraScriptUsage::ParticleSimulationStageScript));
	OutData->SetStringField(TEXT("script_usage_id"), AddedStage->Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("stage_class"), StageClass->GetPathName());
	OutData->SetStringField(TEXT("stage_name"), AddedStage->SimulationStageName.ToString());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterRemoveStage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	if (!Stage.UsageId.IsValid())
	{
		OutError = TEXT("remove_stage_requires_valid_script_usage_id");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "RemoveNiagaraStage", "UeAgentInterface Remove Niagara Stage"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();

	bool bRemoved = false;
	if (Stage.Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
	{
		if (UNiagaraSimulationStageBase* SimulationStage = EditContext.EmitterData->GetSimulationStageById(Stage.UsageId))
		{
			EditContext.Emitter->RemoveSimulationStage(SimulationStage, EditContext.VersionGuid);
			UeAgentNiagaraOps::DestroyGraphNodesForStage(*EditContext.Graph, Stage.Usage, Stage.UsageId);
			bRemoved = true;
		}
	}
	else if (Stage.Usage == ENiagaraScriptUsage::ParticleEventScript)
	{
		EditContext.Emitter->RemoveEventHandlerByUsageId(Stage.UsageId, EditContext.VersionGuid);
		UeAgentNiagaraOps::DestroyGraphNodesForStage(*EditContext.Graph, Stage.Usage, Stage.UsageId);
		bRemoved = true;
	}
	else
	{
		OutError = TEXT("built_in_stage_cannot_remove");
		return false;
	}

	if (!bRemoved)
	{
		OutError = TEXT("stage_not_found_or_not_removed");
		return false;
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("removed_stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetStageProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}

	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	UObject* TargetObject = Stage.OutputNode;
	FString TargetObjectType = TEXT("output_node");
	if (Stage.Usage == ENiagaraScriptUsage::ParticleSimulationStageScript && Stage.UsageId.IsValid())
	{
		if (UNiagaraSimulationStageBase* SimulationStage = EditContext.EmitterData->GetSimulationStageById(Stage.UsageId))
		{
			TargetObject = SimulationStage;
			TargetObjectType = TEXT("simulation_stage");
		}
	}

	if (!TargetObject)
	{
		OutError = TEXT("stage_property_target_not_found");
		return false;
	}

	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetObjectPropertyText(TargetObject, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
		OutData->SetStringField(TEXT("target_object_type"), TargetObjectType);
		OutData->SetStringField(TEXT("target_object_path"), TargetObject->GetPathName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutError = ImportError.IsEmpty() ? TEXT("set_stage_property_failed") : ImportError;
		return false;
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("target_object_type"), TargetObjectType);
	OutData->SetStringField(TEXT("target_object_path"), TargetObject->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterListStageModules(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	bool bIncludeInputs = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_inputs"), bIncludeInputs);

	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ModuleNodes);

	TArray<TSharedPtr<FJsonValue>> ModulesJson;
	ModulesJson.Reserve(ModuleNodes.Num());
	for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
	{
		UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
		if (!ModuleNode)
		{
			continue;
		}
		ModulesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleNodeJson(*ModuleNode, ModuleIndex, bIncludeInputs)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("modules"), ModulesJson);
	OutData->SetNumberField(TEXT("module_count"), ModulesJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemListStageModules(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraSystemEditContext EditContext;
	if (!UeAgentNiagaraOps::GetSystemEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}
	if (!UeAgentNiagaraOps::IsSystemStageUsage(Stage.Usage))
	{
		OutError = TEXT("stage_is_not_system_stage");
		return false;
	}

	bool bIncludeInputs = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_inputs"), bIncludeInputs);

	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ModuleNodes);

	TArray<TSharedPtr<FJsonValue>> ModulesJson;
	ModulesJson.Reserve(ModuleNodes.Num());
	for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
	{
		UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
		if (!ModuleNode)
		{
			continue;
		}
		ModulesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleNodeJson(*ModuleNode, ModuleIndex, bIncludeInputs)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.System->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("modules"), ModulesJson);
	OutData->SetNumberField(TEXT("module_count"), ModulesJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterAddStageModule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	FString ModuleScriptPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_script_asset_path"), ModuleScriptPath) || ModuleScriptPath.IsEmpty())
	{
		FString CatalogName;
		if (!JsonTryGetString(Ctx.Params, TEXT("catalog_name"), CatalogName))
		{
			JsonTryGetString(Ctx.Params, TEXT("name"), CatalogName);
		}
		if (!CatalogName.TrimStartAndEnd().IsEmpty())
		{
			UeAgentPaletteSearch::FCandidate CatalogCandidate;
			FString ResolveError;
			if (!UeAgentPaletteSearch::ResolveCatalogNameToCandidate(TEXT("niagara_module"), CatalogName, CatalogCandidate, ResolveError))
			{
				OutError = ResolveError;
				return false;
			}
			ModuleScriptPath = CatalogCandidate.FullName;
		}
		if (ModuleScriptPath.IsEmpty())
		{
			OutError = TEXT("missing_module_script_asset_path");
			return false;
		}
	}

	UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(ModuleScriptPath));
	if (!ModuleScript)
	{
		OutError = TEXT("module_script_not_found");
		return false;
	}

	int32 TargetIndex = INDEX_NONE;
	double TargetIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		TargetIndex = FMath::RoundToInt(TargetIndexNumber);
	}

	FString SuggestedName;
	JsonTryGetString(Ctx.Params, TEXT("module_name"), SuggestedName);

	TArray<UNiagaraNodeFunctionCall*> ExistingModules;
	UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ExistingModules);
	TArray<UeAgentNiagaraOps::FLocalNiagaraStackNodeGroup> StackGroups;
	UeAgentNiagaraOps::GetLocalStackNodeGroups(*Stage.OutputNode, StackGroups);
	if (StackGroups.Num() < 2)
	{
		UNiagaraNodeOutput* ResetOutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(
			*EditContext.Graph,
			Stage.Usage,
			Stage.UsageId);
		if (!ResetOutputNode)
		{
			OutError = TEXT("stage_stack_invalid_add_module_repair_failed");
			return false;
		}
		Stage.OutputNode = ResetOutputNode;
		UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ExistingModules);
		FUeAgentInterfaceLogger::Log(TEXT("Niagara AddStageModule repaired invalid stack: emitter=%s stage_key=%s usage=%s module=%s"),
			*EditContext.Emitter->GetPathName(),
			*Stage.StageKey,
			*UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage),
			*ModuleScriptPath);
	}
	if (ExistingModules.Num() == 0)
	{
		UNiagaraNodeOutput* ResetOutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(
			*EditContext.Graph,
			Stage.Usage,
			Stage.UsageId);
		if (!ResetOutputNode)
		{
			OutError = TEXT("stage_stack_invalid_or_empty_add_module_blocked");
			return false;
		}
		Stage.OutputNode = ResetOutputNode;
		UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ExistingModules);
		FUeAgentInterfaceLogger::Log(TEXT("Niagara AddStageModule bootstrap: emitter=%s stage_key=%s usage=%s module=%s"),
			*EditContext.Emitter->GetPathName(),
			*Stage.StageKey,
			*UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage),
			*ModuleScriptPath);
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraStageModule", "UeAgentInterface Add Niagara Stage Module"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	Stage.OutputNode->Modify();

	UNiagaraNodeFunctionCall* AddedModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript,
		*Stage.OutputNode,
		TargetIndex,
		SuggestedName,
		ModuleScript->GetExposedVersion().VersionGuid);
	if (!AddedModule)
	{
		OutError = TEXT("add_stage_module_failed");
		return false;
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetObjectField(TEXT("module"), UeAgentNiagaraOps::BuildModuleNodeJson(*AddedModule, TargetIndex, false));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterRemoveStageModule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = nullptr;
	int32 ModuleIndex = INDEX_NONE;
	FString StageKey;

	FString ModuleNodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) && !ModuleNodeGuidText.IsEmpty())
	{
		FGuid ModuleNodeGuid;
		if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
		{
			OutError = TEXT("invalid_module_node_guid");
			return false;
		}
		ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
		if (!ModuleNode)
		{
			OutError = TEXT("module_node_not_found");
			return false;
		}
	}
	else
	{
		UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
		if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
		{
			return false;
		}
		StageKey = Stage.StageKey;

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ModuleNodes);
		if (ModuleNodes.Num() == 0)
		{
			OutError = TEXT("stage_has_no_modules");
			return false;
		}

		double ModuleIndexNumber = 0.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("module_index"), ModuleIndexNumber))
		{
			ModuleIndex = FMath::RoundToInt(ModuleIndexNumber);
			if (!ModuleNodes.IsValidIndex(ModuleIndex))
			{
				OutError = TEXT("invalid_module_index");
				return false;
			}
			ModuleNode = ModuleNodes[ModuleIndex];
		}
		else
		{
			FString ModuleName;
			if (!JsonTryGetString(Ctx.Params, TEXT("module_name"), ModuleName) || ModuleName.IsEmpty())
			{
				OutError = TEXT("missing_module_identifier");
				return false;
			}
			for (int32 Index = 0; Index < ModuleNodes.Num(); ++Index)
			{
				if (ModuleNodes[Index] && ModuleNodes[Index]->GetFunctionName().Equals(ModuleName, ESearchCase::IgnoreCase))
				{
					ModuleNode = ModuleNodes[Index];
					ModuleIndex = Index;
					break;
				}
			}
			if (!ModuleNode)
			{
				OutError = TEXT("module_name_not_found");
				return false;
			}
		}
	}

	const FString RemovedModuleGuid = ModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString RemovedModuleName = ModuleNode->GetFunctionName();

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "DisableNiagaraStageModule", "UeAgentInterface Disable Niagara Stage Module"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();

	// NiagaraEditor 的 RemoveModuleFromStack 未对外导出（无法稳定链接），
	// 直接 DestroyNode 会破坏 Stack 图，后续 AddScriptModuleToStack 可能触发断言崩溃。
	// 这里改为“软删除”：禁用模块，保持图结构稳定。
	FNiagaraStackGraphUtilities::SetModuleIsEnabled(*ModuleNode, false);

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), StageKey);
	OutData->SetStringField(TEXT("removed_module_node_guid"), RemovedModuleGuid);
	OutData->SetStringField(TEXT("removed_module_name"), RemovedModuleName);
	OutData->SetNumberField(TEXT("removed_module_index"), ModuleIndex);
	OutData->SetBoolField(TEXT("soft_removed"), true);
	OutData->SetBoolField(TEXT("module_enabled"), false);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterMoveStageModule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	double TargetIndexNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		OutError = TEXT("missing_target_index");
		return false;
	}

	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ModuleNodes);
	if (ModuleNodes.Num() == 0)
	{
		OutError = TEXT("stage_has_no_modules");
		return false;
	}

	const int32 TargetIndex = FMath::RoundToInt(TargetIndexNumber);
	if (!ModuleNodes.IsValidIndex(TargetIndex))
	{
		OutError = TEXT("invalid_target_index");
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = nullptr;
	int32 ModuleIndex = INDEX_NONE;

	FString ModuleNodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) && !ModuleNodeGuidText.IsEmpty())
	{
		FGuid ModuleNodeGuid;
		if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
		{
			OutError = TEXT("invalid_module_node_guid");
			return false;
		}

		for (int32 Index = 0; Index < ModuleNodes.Num(); ++Index)
		{
			if (ModuleNodes[Index] && ModuleNodes[Index]->NodeGuid == ModuleNodeGuid)
			{
				ModuleNode = ModuleNodes[Index];
				ModuleIndex = Index;
				break;
			}
		}
		if (!ModuleNode)
		{
			OutError = TEXT("module_node_not_found_in_stage");
			return false;
		}
	}
	else
	{
		double ModuleIndexNumber = 0.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("module_index"), ModuleIndexNumber))
		{
			ModuleIndex = FMath::RoundToInt(ModuleIndexNumber);
			if (!ModuleNodes.IsValidIndex(ModuleIndex))
			{
				OutError = TEXT("invalid_module_index");
				return false;
			}
			ModuleNode = ModuleNodes[ModuleIndex];
		}
		else
		{
			FString ModuleName;
			if (!JsonTryGetString(Ctx.Params, TEXT("module_name"), ModuleName) || ModuleName.IsEmpty())
			{
				OutError = TEXT("missing_module_identifier");
				return false;
			}
			for (int32 Index = 0; Index < ModuleNodes.Num(); ++Index)
			{
				if (ModuleNodes[Index] && ModuleNodes[Index]->GetFunctionName().Equals(ModuleName, ESearchCase::IgnoreCase))
				{
					ModuleNode = ModuleNodes[Index];
					ModuleIndex = Index;
					break;
				}
			}
			if (!ModuleNode)
			{
				OutError = TEXT("module_name_not_found");
				return false;
			}
		}
	}

	if (ModuleIndex == TargetIndex)
	{
		OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
		OutData->SetStringField(TEXT("module_node_guid"), ModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
		OutData->SetNumberField(TEXT("old_index"), ModuleIndex);
		OutData->SetNumberField(TEXT("target_index"), TargetIndex);
		OutData->SetNumberField(TEXT("new_index"), ModuleIndex);
		OutData->SetBoolField(TEXT("moved"), false);
		return true;
	}

	TArray<UeAgentNiagaraOps::FLocalNiagaraStackNodeGroup> StackGroups;
	UeAgentNiagaraOps::GetLocalStackNodeGroups(*Stage.OutputNode, StackGroups);
	const int32 SourceGroupIndex = StackGroups.IndexOfByPredicate([ModuleNode](const UeAgentNiagaraOps::FLocalNiagaraStackNodeGroup& StackNodeGroup)
	{
		return StackNodeGroup.EndNode == ModuleNode;
	});
	if (SourceGroupIndex == INDEX_NONE || SourceGroupIndex <= 0 || SourceGroupIndex >= StackGroups.Num() - 1)
	{
		OutError = TEXT("stage_module_group_not_found");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "MoveNiagaraStageModule", "UeAgentInterface Move Niagara Stage Module"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();

	const UeAgentNiagaraOps::FLocalNiagaraStackNodeGroup ModuleGroup = StackGroups[SourceGroupIndex];
	UeAgentNiagaraOps::DisconnectLocalStackNodeGroup(ModuleGroup, StackGroups[SourceGroupIndex - 1], StackGroups[SourceGroupIndex + 1]);

	TArray<UeAgentNiagaraOps::FLocalNiagaraStackNodeGroup> RebuiltGroups;
	UeAgentNiagaraOps::GetLocalStackNodeGroups(*Stage.OutputNode, RebuiltGroups);
	if (RebuiltGroups.Num() < 2)
	{
		UeAgentNiagaraOps::ConnectLocalStackNodeGroup(ModuleGroup, StackGroups[SourceGroupIndex - 1], StackGroups[SourceGroupIndex + 1]);
		Transaction.Cancel();
		OutError = TEXT("stage_stack_invalid_after_disconnect");
		return false;
	}

	const int32 InsertIndex = FMath::Clamp(TargetIndex + 1, 1, RebuiltGroups.Num() - 1);
	UeAgentNiagaraOps::ConnectLocalStackNodeGroup(ModuleGroup, RebuiltGroups[InsertIndex - 1], RebuiltGroups[InsertIndex]);
	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	TArray<UNiagaraNodeFunctionCall*> UpdatedModuleNodes;
	UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, UpdatedModuleNodes);
	int32 NewIndex = INDEX_NONE;
	for (int32 Index = 0; Index < UpdatedModuleNodes.Num(); ++Index)
	{
		if (UpdatedModuleNodes[Index] == ModuleNode)
		{
			NewIndex = Index;
			break;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetNumberField(TEXT("old_index"), ModuleIndex);
	OutData->SetNumberField(TEXT("target_index"), TargetIndex);
	OutData->SetNumberField(TEXT("new_index"), NewIndex);
	OutData->SetObjectField(TEXT("module"), UeAgentNiagaraOps::BuildModuleNodeJson(*ModuleNode, NewIndex, false));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	OutData->SetBoolField(TEXT("moved"), true);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterListStageNodes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	bool bIncludeProperties = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_properties"), bIncludeProperties);
	bool bIncludeModuleInputs = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_module_inputs"), bIncludeModuleInputs);

	TSet<UEdGraphNode*> VisitedNodes;
	TArray<UEdGraphNode*> StageNodes;
	UeAgentNiagaraOps::GatherUpstreamNodesRecursive(Stage.OutputNode, VisitedNodes, StageNodes);
	StageNodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
	{
		if (A.NodePosX != B.NodePosX)
		{
			return A.NodePosX < B.NodePosX;
		}
		return A.NodePosY < B.NodePosY;
	});

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	NodesJson.Reserve(StageNodes.Num());
	for (int32 NodeIndex = 0; NodeIndex < StageNodes.Num(); ++NodeIndex)
	{
		UEdGraphNode* StageNode = StageNodes[NodeIndex];
		if (!StageNode)
		{
			continue;
		}
		NodesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildStageNodeJson(*StageNode, NodeIndex, bIncludeProperties, bIncludeModuleInputs)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("nodes"), NodesJson);
	OutData->SetNumberField(TEXT("node_count"), NodesJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterGetStageNodeProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	UEdGraphNode* StageNode = nullptr;
	if (!UeAgentNiagaraOps::ResolveStageNodeFromParams(*EditContext.Graph, Ctx.Params, Stage, StageNode, OutError))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}

	FString ValueText;
	if (!UeAgentNiagaraOps::GetObjectPropertyText(StageNode, PropertyName, ValueText))
	{
		OutError = TEXT("get_stage_node_property_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("node_guid"), StageNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("node_class"), StageNode->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetStageNodeProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	UEdGraphNode* StageNode = nullptr;
	if (!UeAgentNiagaraOps::ResolveStageNodeFromParams(*EditContext.Graph, Ctx.Params, Stage, StageNode, OutError))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}

	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetObjectPropertyText(StageNode, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
		OutData->SetStringField(TEXT("node_guid"), StageNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("node_class"), StageNode->GetClass()->GetPathName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutError = ImportError.IsEmpty() ? TEXT("set_stage_node_property_failed") : ImportError;
		return false;
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("node_guid"), StageNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("node_class"), StageNode->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterRemoveStageNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	UEdGraphNode* StageNode = nullptr;
	if (!UeAgentNiagaraOps::ResolveStageNodeFromParams(*EditContext.Graph, Ctx.Params, Stage, StageNode, OutError))
	{
		return false;
	}

	if (StageNode == Stage.OutputNode)
	{
		OutError = TEXT("cannot_remove_stage_output_node_use_remove_stage");
		return false;
	}

	const FString RemovedNodeGuid = StageNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString RemovedNodeClass = StageNode->GetClass()->GetPathName();

	if (UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(StageNode))
	{
		FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "DisableNiagaraStageNodeModule", "UeAgentInterface Disable Niagara Stage Node Module"));
		EditContext.Emitter->Modify();
		EditContext.Graph->Modify();
		ModuleNode->Modify();
		FNiagaraStackGraphUtilities::SetModuleIsEnabled(*ModuleNode, false);
		UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet)
		{
			if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
			{
				return false;
			}
		}

		OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
		OutData->SetStringField(TEXT("removed_node_guid"), RemovedNodeGuid);
		OutData->SetStringField(TEXT("removed_node_class"), RemovedNodeClass);
		OutData->SetBoolField(TEXT("soft_removed"), true);
		OutData->SetBoolField(TEXT("module_enabled"), false);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "RemoveNiagaraStageNode", "UeAgentInterface Remove Niagara Stage Node"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	StageNode->Modify();
	for (UEdGraphPin* Pin : StageNode->Pins)
	{
		if (Pin && Pin->LinkedTo.Num() > 0)
		{
			Pin->BreakAllPinLinks(true);
		}
	}
	StageNode->DestroyNode();

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("removed_node_guid"), RemovedNodeGuid);
	OutData->SetStringField(TEXT("removed_node_class"), RemovedNodeClass);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetStageModuleEnabled(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	bool bEnabled = true;
	if (!JsonTryGetBool(Ctx.Params, TEXT("enabled"), bEnabled))
	{
		OutError = TEXT("missing_enabled");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraStageModuleEnabled", "UeAgentInterface Set Niagara Stage Module Enabled"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();

	FNiagaraStackGraphUtilities::SetModuleIsEnabled(*ModuleNode, bEnabled);
	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetBoolField(TEXT("enabled"), bEnabled);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterListModuleInputs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
	UeAgentNiagaraOps::BuildResolvedModuleInputs(*ModuleNode, ModuleInputs);
	TArray<TSharedPtr<FJsonValue>> InputsJson;
	InputsJson.Reserve(ModuleInputs.Num());
	for (const UeAgentNiagaraOps::FResolvedNiagaraModuleInput& InputData : ModuleInputs)
	{
		InputsJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleInputJson(*ModuleNode, InputData)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetArrayField(TEXT("inputs"), InputsJson);
	OutData->SetNumberField(TEXT("input_count"), InputsJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemListModuleInputs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraSystemEditContext EditContext;
	if (!UeAgentNiagaraOps::GetSystemEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
	UeAgentNiagaraOps::BuildResolvedModuleInputs(*ModuleNode, ModuleInputs);
	TArray<TSharedPtr<FJsonValue>> InputsJson;
	InputsJson.Reserve(ModuleInputs.Num());
	for (const UeAgentNiagaraOps::FResolvedNiagaraModuleInput& InputData : ModuleInputs)
	{
		InputsJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleInputJson(*ModuleNode, InputData)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.System->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetArrayField(TEXT("inputs"), InputsJson);
	OutData->SetNumberField(TEXT("input_count"), InputsJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetModuleInput(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	FString InputName;
	if (!JsonTryGetString(Ctx.Params, TEXT("input_name"), InputName) || InputName.IsEmpty())
	{
		OutError = TEXT("missing_input_name");
		return false;
	}

	FString ValueText;
	const bool bHasValueText = JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText);
	FString DynamicInputScriptPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("dynamic_input_script_asset_path"), DynamicInputScriptPath))
	{
		if (!JsonTryGetString(Ctx.Params, TEXT("dynamic_input_script_path"), DynamicInputScriptPath))
		{
			JsonTryGetString(Ctx.Params, TEXT("dynamic_input"), DynamicInputScriptPath);
		}
	}
	DynamicInputScriptPath.TrimStartAndEndInline();
	if (!DynamicInputScriptPath.StartsWith(TEXT("/")))
	{
		FString CatalogName;
		if (!JsonTryGetString(Ctx.Params, TEXT("catalog_name"), CatalogName))
		{
			JsonTryGetString(Ctx.Params, TEXT("name"), CatalogName);
		}
		if (!CatalogName.TrimStartAndEnd().IsEmpty())
		{
			UeAgentPaletteSearch::FCandidate CatalogCandidate;
			FString ResolveError;
			if (!UeAgentPaletteSearch::ResolveCatalogNameToCandidate(TEXT("niagara_dynamic_input"), CatalogName, CatalogCandidate, ResolveError))
			{
				OutError = ResolveError;
				return false;
			}
			DynamicInputScriptPath = CatalogCandidate.FullName;
		}
	}

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
	UeAgentNiagaraOps::BuildResolvedModuleInputs(*ModuleNode, ModuleInputs);
	FString ResolvedInputName;
	const UeAgentNiagaraOps::FResolvedNiagaraModuleInput* ResolvedInput =
		UeAgentNiagaraOps::ResolveModuleInputByName(ModuleInputs, InputName, ResolvedInputName);
	if (!ResolvedInput || !ResolvedInput->InputVariable.IsValid())
	{
		OutError = TEXT("input_name_not_found");
		return false;
	}

	FString ValueTextForApply = ValueText;

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraModuleInput", "UeAgentInterface Set Niagara Module Input"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();
	auto CancelAndFail = [&Transaction, &OutError](const FString& ErrorText) -> bool
	{
		Transaction.Cancel();
		OutError = ErrorText;
		return false;
	};

	const FNiagaraParameterHandle ModuleInputHandle(ResolvedInput->InputVariable.GetName());
	const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleInputHandle, ModuleNode);
	UEdGraphPin* ExistingOverridePin =
		UeAgentNiagaraOps::FindOverridePinByAliasedInputName(EditContext.Graph, AliasedInputHandle.GetParameterHandleString());
	bool bBreakInputLinks = true;
	JsonTryGetBool(Ctx.Params, TEXT("break_input_links"), bBreakInputLinks);
	UEdGraphPin* OverridePinPtr = ResolvedInput->VisiblePin;
	FString WriteTarget = OverridePinPtr ? TEXT("visible_pin") : TEXT("stack_override_pin");
	if (OverridePinPtr)
	{
		if (UEdGraphNode* OverrideOwnerNode = OverridePinPtr->GetOwningNode())
		{
			OverrideOwnerNode->Modify();
		}
		if (bBreakInputLinks && OverridePinPtr->LinkedTo.Num() > 0)
		{
			OverridePinPtr->BreakAllPinLinks();
		}

	}
	else
	{
		if (bBreakInputLinks && ExistingOverridePin && ExistingOverridePin->LinkedTo.Num() > 0)
		{
			ExistingOverridePin->BreakAllPinLinks();
		}

		FString OverridePinError;
		if (!UeAgentNiagaraOps::TryGetOrCreateStackFunctionInputOverridePin(
			*ModuleNode,
			AliasedInputHandle,
			ResolvedInput->InputVariable.GetType(),
			ResolvedInput->InputScriptVariableGuid,
			OverridePinPtr,
			OverridePinError) || !OverridePinPtr)
		{
			return CancelAndFail(FString::Printf(TEXT("module_input_override_unavailable:%s"), *OverridePinError));
		}
	}
	UEdGraphPin& OverridePin = *OverridePinPtr;

	if (!DynamicInputScriptPath.IsEmpty())
	{
		if (OverridePin.LinkedTo.Num() > 0 && bBreakInputLinks)
		{
			OverridePin.BreakAllPinLinks();
		}

		UNiagaraScript* DynamicInputScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(DynamicInputScriptPath));
		if (!DynamicInputScript)
		{
			return CancelAndFail(TEXT("dynamic_input_script_not_found"));
		}

		FString DynamicInputSuggestedName;
		if (!JsonTryGetString(Ctx.Params, TEXT("dynamic_input_name"), DynamicInputSuggestedName))
		{
			JsonTryGetString(Ctx.Params, TEXT("suggested_dynamic_input_name"), DynamicInputSuggestedName);
		}

		UNiagaraNodeFunctionCall* DynamicInputNode = nullptr;
		FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(
			OverridePin,
			DynamicInputScript,
			DynamicInputNode,
			FGuid(),
			DynamicInputSuggestedName,
			DynamicInputScript->GetExposedVersion().VersionGuid);
		if (!DynamicInputNode)
		{
			return CancelAndFail(TEXT("set_dynamic_input_failed"));
		}

		FString DynamicInputInitializationSource;
		const bool bDynamicInputInputsInitialized = UeAgentNiagaraOps::InitializeDynamicInputStackInputs(
			EditContext.Emitter,
			EditContext.VersionGuid,
			EditContext.Graph,
			*ModuleNode,
			*DynamicInputNode,
			DynamicInputInitializationSource);

		bool bCurveJsonApplied = false;
		FString CurveInputNodeGuidText;
		FString CurveDataInterfaceObjectPath;
		TArray<TSharedPtr<FJsonValue>> CurveJsonIssues;
		const TSharedPtr<FJsonObject>* CurveJsonPtr = nullptr;
		const bool bHasCurveJson =
			(Ctx.Params->TryGetObjectField(TEXT("curve_json"), CurveJsonPtr) && CurveJsonPtr && CurveJsonPtr->IsValid()) ||
			(Ctx.Params->TryGetObjectField(TEXT("value_json"), CurveJsonPtr) && CurveJsonPtr && CurveJsonPtr->IsValid());
		if (bHasCurveJson)
		{
			FString CurveInputNameFilter;
			if (!JsonTryGetString(Ctx.Params, TEXT("dynamic_input_curve_input_name"), CurveInputNameFilter))
			{
				CurveInputNameFilter = TEXT("FloatCurve");
			}
			CurveInputNameFilter.TrimStartAndEndInline();

			UNiagaraNodeInput* CurveInputNode = nullptr;
			UNiagaraDataInterface* CurveDataInterface = nullptr;
			auto TryUseInputNode = [&CurveInputNode, &CurveDataInterface](UNiagaraNodeInput* InputNode) -> bool
			{
				if (!InputNode)
				{
					return false;
				}
				FProperty* DataInterfaceProperty = nullptr;
				void* DataInterfaceValuePtr = nullptr;
				if (!UeAgentNiagaraOps::ResolvePropertyPath(InputNode, TEXT("DataInterface"), DataInterfaceProperty, DataInterfaceValuePtr))
				{
					return false;
				}
				FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(DataInterfaceProperty);
				if (!ObjectProperty)
				{
					return false;
				}
				UNiagaraDataInterface* CandidateDataInterface = Cast<UNiagaraDataInterface>(ObjectProperty->GetObjectPropertyValue(DataInterfaceValuePtr));
				if (!CandidateDataInterface)
				{
					return false;
				}
				FProperty* CurveProperty = nullptr;
				void* CurveValuePtr = nullptr;
				if (!UeAgentNiagaraOps::ResolvePropertyPath(CandidateDataInterface, TEXT("Curve"), CurveProperty, CurveValuePtr))
				{
					return false;
				}
				CurveInputNode = InputNode;
				CurveDataInterface = CandidateDataInterface;
				return true;
			};

			const FString CurveFilterLower = UeAgentNiagaraOps::MakeLowerTrimmed(CurveInputNameFilter);
			for (UEdGraphPin* Pin : DynamicInputNode->Pins)
			{
				if (CurveDataInterface || !Pin || Pin->Direction != EGPD_Input)
				{
					continue;
				}
				const FString PinNameLower = UeAgentNiagaraOps::MakeLowerTrimmed(Pin->PinName.ToString());
				if (!CurveFilterLower.IsEmpty() && !PinNameLower.Contains(CurveFilterLower))
				{
					continue;
				}
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UNiagaraNodeInput* InputNode = LinkedPin ? Cast<UNiagaraNodeInput>(LinkedPin->GetOwningNode()) : nullptr;
					if (TryUseInputNode(InputNode))
					{
						break;
					}
				}
			}

			if (!CurveDataInterface && EditContext.Graph)
			{
				const FString DynamicInputNameLower = UeAgentNiagaraOps::MakeLowerTrimmed(DynamicInputNode->GetFunctionName());
				for (UEdGraphNode* Node : EditContext.Graph->Nodes)
				{
					UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
					if (!InputNode)
					{
						continue;
					}
					const FString InputNameLower = UeAgentNiagaraOps::MakeLowerTrimmed(InputNode->Input.GetName().ToString());
					if (!DynamicInputNameLower.IsEmpty() && !InputNameLower.Contains(DynamicInputNameLower))
					{
						continue;
					}
					if (!CurveFilterLower.IsEmpty() && !InputNameLower.Contains(CurveFilterLower))
					{
						continue;
					}
					if (TryUseInputNode(InputNode))
					{
						break;
					}
				}
			}

			if (!CurveDataInterface && EditContext.Graph)
			{
				for (UEdGraphNode* Node : EditContext.Graph->Nodes)
				{
					UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
					if (!InputNode)
					{
						continue;
					}
					const FString InputNameLower = UeAgentNiagaraOps::MakeLowerTrimmed(InputNode->Input.GetName().ToString());
					if (!CurveFilterLower.IsEmpty() && !InputNameLower.Contains(CurveFilterLower))
					{
						continue;
					}
					if (TryUseInputNode(InputNode))
					{
						break;
					}
				}
			}

			if (!CurveDataInterface)
			{
				return CancelAndFail(TEXT("dynamic_input_curve_data_interface_not_found"));
			}

			FProperty* CurveProperty = nullptr;
			void* CurveValuePtr = nullptr;
			if (!UeAgentNiagaraOps::ResolvePropertyPath(CurveDataInterface, TEXT("Curve"), CurveProperty, CurveValuePtr))
			{
				return CancelAndFail(TEXT("dynamic_input_curve_property_not_found"));
			}

			CurveDataInterface->Modify();
			if (!UeAgentCurveJson::TryApplyPropertyCurveJson(CurveProperty, CurveValuePtr, *CurveJsonPtr, TEXT("curve_json"), CurveJsonIssues) ||
				UeAgentCurveJson::HasError(CurveJsonIssues))
			{
				OutData->SetArrayField(TEXT("json_issues"), CurveJsonIssues);
				return CancelAndFail(TEXT("dynamic_input_curve_json_apply_failed"));
			}
			FPropertyChangedEvent CurveChangedEvent(CurveProperty);
			CurveDataInterface->PostEditChangeProperty(CurveChangedEvent);
			CurveDataInterface->MarkPackageDirty();
			bCurveJsonApplied = true;
			if (CurveInputNode)
			{
				CurveInputNodeGuidText = CurveInputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
			}
			CurveDataInterfaceObjectPath = CurveDataInterface->GetPathName();
		}

		UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet)
		{
			if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
			{
				return false;
			}
		}

		OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
		OutData->SetStringField(TEXT("input_name"), ResolvedInputName);
		OutData->SetStringField(TEXT("input_type"), ResolvedInput->InputVariable.GetType().GetNameText().ToString());
		OutData->SetStringField(TEXT("requested_dynamic_input_script_asset_path"), DynamicInputScriptPath);
		OutData->SetStringField(TEXT("dynamic_input_node_guid"), DynamicInputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("dynamic_input_node_name"), DynamicInputNode->GetFunctionName());
		OutData->SetStringField(TEXT("dynamic_input_script_asset_path"), UeAgentNiagaraOps::GetModuleScriptPath(*DynamicInputNode));
		OutData->SetBoolField(TEXT("is_dynamic_input"), true);
		OutData->SetBoolField(TEXT("dynamic_input_extension_deprecated"), true);
		OutData->SetStringField(TEXT("dynamic_input_extension_replacement"), TEXT("Use niagara_export_folder / niagara_apply_folder and edit inputs[].dynamic_input plus dynamic_input.data_interfaces[].raw_properties[].curve_json."));
		OutData->SetBoolField(TEXT("dynamic_input_inputs_initialized"), bDynamicInputInputsInitialized);
		if (!DynamicInputInitializationSource.IsEmpty())
		{
			OutData->SetStringField(TEXT("dynamic_input_initialization_source"), DynamicInputInitializationSource);
		}
		OutData->SetBoolField(TEXT("curve_json_applied"), bCurveJsonApplied);
		if (!CurveInputNodeGuidText.IsEmpty())
		{
			OutData->SetStringField(TEXT("curve_input_node_guid"), CurveInputNodeGuidText);
		}
		if (!CurveDataInterfaceObjectPath.IsEmpty())
		{
			OutData->SetStringField(TEXT("curve_data_interface_object_path"), CurveDataInterfaceObjectPath);
		}
		if (CurveJsonIssues.Num() > 0)
		{
			OutData->SetArrayField(TEXT("json_issues"), CurveJsonIssues);
		}
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	FString LinkedParameterName;
	FString ApplyVerificationStatus = TEXT("not_run");
	FString ApplyVerificationError;
	if (!JsonTryGetString(Ctx.Params, TEXT("link_parameter_name"), LinkedParameterName))
	{
		JsonTryGetString(Ctx.Params, TEXT("link_parameter"), LinkedParameterName);
	}
	LinkedParameterName.TrimStartAndEndInline();
	if (LinkedParameterName.IsEmpty() && bHasValueText)
	{
		const FString ValueTextTrimmed = ValueTextForApply.TrimStartAndEnd();
		if (ValueTextTrimmed.StartsWith(TEXT("link:"), ESearchCase::IgnoreCase))
		{
			LinkedParameterName = ValueTextTrimmed.Mid(5).TrimStartAndEnd();
		}
	}

	if (!bHasValueText && LinkedParameterName.IsEmpty())
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	if (LinkedParameterName.IsEmpty())
	{
		const FNiagaraTypeDefinition InputType = ResolvedInput->InputVariable.GetType();
		FString NormalizeError;
		if (!UeAgentNiagaraOps::NormalizeNiagaraValueTextForPinDefault(InputType, ValueText, ValueTextForApply, NormalizeError))
		{
			return CancelAndFail(FString::Printf(TEXT("invalid_value_text_for_input:%s:%s"), *ResolvedInputName, *NormalizeError));
		}
	}

	const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
	if (!LinkedParameterName.IsEmpty())
	{
		if (Cast<UNiagaraNodeFunctionCall>(OverridePin.GetOwningNode()) != nullptr)
		{
			return CancelAndFail(TEXT("input_link_not_supported_for_static_switch"));
		}

		if (OverridePin.LinkedTo.Num() > 0)
		{
			OverridePin.BreakAllPinLinks();
		}
		if (NiagaraSchema)
		{
			NiagaraSchema->TrySetDefaultValue(OverridePin, FString(), true);
		}
		else
		{
			OverridePin.DefaultValue.Empty();
		}

		TSet<FNiagaraVariableBase> KnownParameters;
		const FNiagaraVariableBase LinkedParameter(ResolvedInput->InputVariable.GetType(), *LinkedParameterName);
		FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(OverridePin, LinkedParameter, KnownParameters);
		FString LinkedParameterReadback;
		if (!UeAgentNiagaraOps::TryResolveLinkedInputParameterName(&OverridePin, LinkedParameterReadback)
			|| !LinkedParameterReadback.Equals(LinkedParameterName, ESearchCase::IgnoreCase))
		{
			return CancelAndFail(FString::Printf(
				TEXT("linked_parameter_apply_verification_failed:%s:readback=%s"),
				*LinkedParameterName,
				*LinkedParameterReadback));
		}
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara SetModuleInput linked: emitter=%s module=%s input=%s link=%s"),
			*EditContext.Emitter->GetPathName(),
			*ModuleNode->GetFunctionName(),
			*ResolvedInputName,
			*LinkedParameterName);
	}
	else
	{
		if (OverridePin.LinkedTo.Num() > 0 && !bBreakInputLinks)
		{
			return CancelAndFail(TEXT("input_has_linked_override_set_break_input_links_true"));
		}
		if (NiagaraSchema)
		{
			NiagaraSchema->TrySetDefaultValue(OverridePin, ValueTextForApply, true);
		}
		else
		{
			OverridePin.DefaultValue = ValueTextForApply;
		}
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara SetModuleInput value: emitter=%s module=%s input=%s requested=%s applied=%s"),
			*EditContext.Emitter->GetPathName(),
			*ModuleNode->GetFunctionName(),
			*ResolvedInputName,
			*ValueText,
			*ValueTextForApply);

		const FNiagaraTypeDefinition InputType = ResolvedInput->InputVariable.GetType();
		if (InputType.IsEnum())
		{
			UEnum* EnumType = InputType.GetEnum();
			if (!EnumType)
			{
				return CancelAndFail(TEXT("input_enum_type_not_found"));
			}

			int64 AppliedEnumValue = 0;
			if (!UeAgentNiagaraOps::TryResolveNiagaraEnumValueFromText(*EnumType, OverridePin.DefaultValue, AppliedEnumValue))
			{
				FUeAgentInterfaceLogger::Log(
					TEXT("Niagara SetModuleInput REJECTED enum apply: emitter=%s module=%s input=%s requested=%s applied_default=%s"),
					*EditContext.Emitter->GetPathName(),
					*ModuleNode->GetFunctionName(),
					*ResolvedInputName,
					*ValueText,
					*OverridePin.DefaultValue);
				return CancelAndFail(FString::Printf(TEXT("invalid_applied_enum_value_for_input:%s"), *ResolvedInputName));
			}
		}

		if (!UeAgentNiagaraOps::ValidateNiagaraPinDefaultValueAfterSet(InputType, ValueTextForApply, OverridePin.DefaultValue, ApplyVerificationStatus, ApplyVerificationError))
		{
			FUeAgentInterfaceLogger::Log(
				TEXT("Niagara SetModuleInput REJECTED default apply: emitter=%s module=%s input=%s requested=%s applied=%s stored=%s status=%s error=%s"),
				*EditContext.Emitter->GetPathName(),
				*ModuleNode->GetFunctionName(),
				*ResolvedInputName,
				*ValueText,
				*ValueTextForApply,
				*OverridePin.DefaultValue,
				*ApplyVerificationStatus,
				*ApplyVerificationError);
			return CancelAndFail(FString::Printf(TEXT("pin_default_apply_verification_failed:%s:%s"), *ResolvedInputName, *ApplyVerificationError));
		}
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetStringField(TEXT("input_name"), ResolvedInputName);
	OutData->SetStringField(TEXT("input_type"), ResolvedInput->InputVariable.GetType().GetNameText().ToString());
	OutData->SetStringField(TEXT("override_default_value"), OverridePin.DefaultValue);
	OutData->SetStringField(TEXT("requested_value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ValueTextForApply);
	OutData->SetBoolField(TEXT("value_text_normalized"), !LinkedParameterName.IsEmpty() ? false : !ValueTextForApply.Equals(ValueText, ESearchCase::CaseSensitive));
	OutData->SetBoolField(TEXT("default_value_apply_verified"), !LinkedParameterName.IsEmpty() ? false : ApplyVerificationError.IsEmpty());
	OutData->SetStringField(TEXT("default_value_apply_status"), !LinkedParameterName.IsEmpty() ? TEXT("linked_parameter") : ApplyVerificationStatus);
	if (!ApplyVerificationError.IsEmpty())
	{
		OutData->SetStringField(TEXT("default_value_apply_error"), ApplyVerificationError);
	}
	OutData->SetStringField(TEXT("write_target"), WriteTarget);
	OutData->SetBoolField(TEXT("is_linked_value"), !LinkedParameterName.IsEmpty());
	if (!LinkedParameterName.IsEmpty())
	{
		OutData->SetStringField(TEXT("linked_parameter_name"), LinkedParameterName);
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemSetModuleInput(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraSystemEditContext EditContext;
	if (!UeAgentNiagaraOps::GetSystemEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	FString InputName;
	if (!JsonTryGetString(Ctx.Params, TEXT("input_name"), InputName) || InputName.IsEmpty())
	{
		OutError = TEXT("missing_input_name");
		return false;
	}

	FString ValueText;
	const bool bHasValueText = JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText);

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
	UeAgentNiagaraOps::BuildResolvedModuleInputs(*ModuleNode, ModuleInputs);
	FString ResolvedInputName;
	const UeAgentNiagaraOps::FResolvedNiagaraModuleInput* ResolvedInput =
		UeAgentNiagaraOps::ResolveModuleInputByName(ModuleInputs, InputName, ResolvedInputName);
	if (!ResolvedInput || !ResolvedInput->InputVariable.IsValid())
	{
		OutError = TEXT("input_name_not_found");
		return false;
	}

	FString ValueTextForApply = ValueText;
	FString LinkedParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("link_parameter_name"), LinkedParameterName))
	{
		JsonTryGetString(Ctx.Params, TEXT("link_parameter"), LinkedParameterName);
	}
	LinkedParameterName.TrimStartAndEndInline();
	if (LinkedParameterName.IsEmpty() && bHasValueText)
	{
		const FString ValueTextTrimmed = ValueTextForApply.TrimStartAndEnd();
		if (ValueTextTrimmed.StartsWith(TEXT("link:"), ESearchCase::IgnoreCase))
		{
			LinkedParameterName = ValueTextTrimmed.Mid(5).TrimStartAndEnd();
		}
	}

	if (!bHasValueText && LinkedParameterName.IsEmpty())
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraSystemModuleInput", "UeAgentInterface Set Niagara System Module Input"));
	EditContext.System->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();
	auto CancelAndFail = [&Transaction, &OutError](const FString& ErrorText) -> bool
	{
		Transaction.Cancel();
		OutError = ErrorText;
		return false;
	};

	const FNiagaraParameterHandle ModuleInputHandle(ResolvedInput->InputVariable.GetName());
	const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleInputHandle, ModuleNode);
	UEdGraphPin* ExistingOverridePin =
		UeAgentNiagaraOps::FindOverridePinByAliasedInputName(EditContext.Graph, AliasedInputHandle.GetParameterHandleString());
	bool bBreakInputLinks = true;
	JsonTryGetBool(Ctx.Params, TEXT("break_input_links"), bBreakInputLinks);
	UEdGraphPin* OverridePinPtr = ResolvedInput->VisiblePin;
	FString WriteTarget = OverridePinPtr ? TEXT("visible_pin") : TEXT("stack_override_pin");
	if (OverridePinPtr)
	{
		if (UEdGraphNode* OverrideOwnerNode = OverridePinPtr->GetOwningNode())
		{
			OverrideOwnerNode->Modify();
		}
		if (bBreakInputLinks && OverridePinPtr->LinkedTo.Num() > 0)
		{
			OverridePinPtr->BreakAllPinLinks();
		}
	}
	else
	{
		if (bBreakInputLinks && ExistingOverridePin && ExistingOverridePin->LinkedTo.Num() > 0)
		{
			ExistingOverridePin->BreakAllPinLinks();
		}

		FString OverridePinError;
		if (!UeAgentNiagaraOps::TryGetOrCreateStackFunctionInputOverridePin(
			*ModuleNode,
			AliasedInputHandle,
			ResolvedInput->InputVariable.GetType(),
			ResolvedInput->InputScriptVariableGuid,
			OverridePinPtr,
			OverridePinError) || !OverridePinPtr)
		{
			return CancelAndFail(FString::Printf(TEXT("module_input_override_unavailable:%s"), *OverridePinError));
		}
	}
	UEdGraphPin& OverridePin = *OverridePinPtr;
	FString ApplyVerificationStatus = TEXT("not_run");
	FString ApplyVerificationError;

	if (LinkedParameterName.IsEmpty())
	{
		const FNiagaraTypeDefinition InputType = ResolvedInput->InputVariable.GetType();
		FString NormalizeError;
		if (!UeAgentNiagaraOps::NormalizeNiagaraValueTextForPinDefault(InputType, ValueText, ValueTextForApply, NormalizeError))
		{
			return CancelAndFail(FString::Printf(TEXT("invalid_value_text_for_input:%s:%s"), *ResolvedInputName, *NormalizeError));
		}
	}

	const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
	if (!LinkedParameterName.IsEmpty())
	{
		if (Cast<UNiagaraNodeFunctionCall>(OverridePin.GetOwningNode()) != nullptr)
		{
			return CancelAndFail(TEXT("input_link_not_supported_for_static_switch"));
		}

		if (OverridePin.LinkedTo.Num() > 0)
		{
			OverridePin.BreakAllPinLinks();
		}
		if (NiagaraSchema)
		{
			NiagaraSchema->TrySetDefaultValue(OverridePin, FString(), true);
		}
		else
		{
			OverridePin.DefaultValue.Empty();
		}

		TSet<FNiagaraVariableBase> KnownParameters;
		const FNiagaraVariableBase LinkedParameter(ResolvedInput->InputVariable.GetType(), *LinkedParameterName);
		FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(OverridePin, LinkedParameter, KnownParameters);
		FString LinkedParameterReadback;
		if (!UeAgentNiagaraOps::TryResolveLinkedInputParameterName(&OverridePin, LinkedParameterReadback)
			|| !LinkedParameterReadback.Equals(LinkedParameterName, ESearchCase::IgnoreCase))
		{
			return CancelAndFail(FString::Printf(
				TEXT("linked_parameter_apply_verification_failed:%s:readback=%s"),
				*LinkedParameterName,
				*LinkedParameterReadback));
		}
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara System SetModuleInput linked: system=%s module=%s input=%s link=%s"),
			*EditContext.System->GetPathName(),
			*ModuleNode->GetFunctionName(),
			*ResolvedInputName,
			*LinkedParameterName);
	}
	else
	{
		if (OverridePin.LinkedTo.Num() > 0 && !bBreakInputLinks)
		{
			return CancelAndFail(TEXT("input_has_linked_override_set_break_input_links_true"));
		}
		if (NiagaraSchema)
		{
			NiagaraSchema->TrySetDefaultValue(OverridePin, ValueTextForApply, true);
		}
		else
		{
			OverridePin.DefaultValue = ValueTextForApply;
		}
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara System SetModuleInput value: system=%s module=%s input=%s requested=%s applied=%s"),
			*EditContext.System->GetPathName(),
			*ModuleNode->GetFunctionName(),
			*ResolvedInputName,
			*ValueText,
			*ValueTextForApply);

		const FNiagaraTypeDefinition InputType = ResolvedInput->InputVariable.GetType();
		if (InputType.IsEnum())
		{
			UEnum* EnumType = InputType.GetEnum();
			if (!EnumType)
			{
				return CancelAndFail(TEXT("input_enum_type_not_found"));
			}

			int64 AppliedEnumValue = 0;
			if (!UeAgentNiagaraOps::TryResolveNiagaraEnumValueFromText(*EnumType, OverridePin.DefaultValue, AppliedEnumValue))
			{
				FUeAgentInterfaceLogger::Log(
					TEXT("Niagara System SetModuleInput REJECTED enum apply: system=%s module=%s input=%s requested=%s applied_default=%s"),
					*EditContext.System->GetPathName(),
					*ModuleNode->GetFunctionName(),
					*ResolvedInputName,
					*ValueText,
					*OverridePin.DefaultValue);
				return CancelAndFail(FString::Printf(TEXT("invalid_applied_enum_value_for_input:%s"), *ResolvedInputName));
			}
		}

		if (!UeAgentNiagaraOps::ValidateNiagaraPinDefaultValueAfterSet(InputType, ValueTextForApply, OverridePin.DefaultValue, ApplyVerificationStatus, ApplyVerificationError))
		{
			FUeAgentInterfaceLogger::Log(
				TEXT("Niagara System SetModuleInput REJECTED default apply: system=%s module=%s input=%s requested=%s applied=%s stored=%s status=%s error=%s"),
				*EditContext.System->GetPathName(),
				*ModuleNode->GetFunctionName(),
				*ResolvedInputName,
				*ValueText,
				*ValueTextForApply,
				*OverridePin.DefaultValue,
				*ApplyVerificationStatus,
				*ApplyVerificationError);
			return CancelAndFail(FString::Printf(TEXT("pin_default_apply_verification_failed:%s:%s"), *ResolvedInputName, *ApplyVerificationError));
		}
	}

	const UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult = UeAgentNiagaraOps::MarkSystemGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.System, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.System->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetStringField(TEXT("input_name"), ResolvedInputName);
	OutData->SetStringField(TEXT("input_type"), ResolvedInput->InputVariable.GetType().GetNameText().ToString());
	OutData->SetStringField(TEXT("override_default_value"), OverridePin.DefaultValue);
	OutData->SetStringField(TEXT("requested_value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ValueTextForApply);
	OutData->SetBoolField(TEXT("value_text_normalized"), !LinkedParameterName.IsEmpty() ? false : !ValueTextForApply.Equals(ValueText, ESearchCase::CaseSensitive));
	OutData->SetBoolField(TEXT("default_value_apply_verified"), !LinkedParameterName.IsEmpty() ? false : ApplyVerificationError.IsEmpty());
	OutData->SetStringField(TEXT("default_value_apply_status"), !LinkedParameterName.IsEmpty() ? TEXT("linked_parameter") : ApplyVerificationStatus);
	if (!ApplyVerificationError.IsEmpty())
	{
		OutData->SetStringField(TEXT("default_value_apply_error"), ApplyVerificationError);
	}
	OutData->SetStringField(TEXT("write_target"), WriteTarget);
	OutData->SetBoolField(TEXT("is_linked_value"), !LinkedParameterName.IsEmpty());
	if (!LinkedParameterName.IsEmpty())
	{
		OutData->SetStringField(TEXT("linked_parameter_name"), LinkedParameterName);
	}
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterClearModuleInput(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	FString InputName;
	if (!JsonTryGetString(Ctx.Params, TEXT("input_name"), InputName) || InputName.IsEmpty())
	{
		OutError = TEXT("missing_input_name");
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
	UeAgentNiagaraOps::BuildResolvedModuleInputs(*ModuleNode, ModuleInputs);
	FString ResolvedInputName;
	const UeAgentNiagaraOps::FResolvedNiagaraModuleInput* ResolvedInput =
		UeAgentNiagaraOps::ResolveModuleInputByName(ModuleInputs, InputName, ResolvedInputName);
	if (!ResolvedInput || !ResolvedInput->InputVariable.IsValid())
	{
		OutError = TEXT("input_name_not_found");
		return false;
	}

	const FNiagaraParameterHandle ModuleInputHandle(ResolvedInput->InputVariable.GetName());
	const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleInputHandle, ModuleNode);
	UEdGraphPin* OverridePin =
		UeAgentNiagaraOps::FindOverridePinByAliasedInputName(EditContext.Graph, AliasedInputHandle.GetParameterHandleString());
	const bool bHasOverride = OverridePin != nullptr &&
		(OverridePin->LinkedTo.Num() > 0 || OverridePin->DefaultValue != OverridePin->AutogeneratedDefaultValue);
	if (!bHasOverride)
	{
		OutError = TEXT("module_input_has_no_override");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "ClearNiagaraModuleInput", "UeAgentInterface Clear Niagara Module Input"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();

	OverridePin->BreakAllPinLinks();

	if (UEdGraphNode* OverrideOwnerNode = OverridePin->GetOwningNode())
	{
		const bool bIsStaticSwitchInput = Cast<UNiagaraNodeFunctionCall>(OverrideOwnerNode) != nullptr;
		if (bIsStaticSwitchInput)
		{
			const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
			if (NiagaraSchema)
			{
				NiagaraSchema->TrySetDefaultValue(*OverridePin, OverridePin->AutogeneratedDefaultValue, true);
			}
			else
			{
				OverridePin->DefaultValue = OverridePin->AutogeneratedDefaultValue;
			}
		}
		else
		{
			OverrideOwnerNode->Modify();
			OverrideOwnerNode->RemovePin(OverridePin);
		}
	}
	else
	{
		const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
		if (NiagaraSchema)
		{
			NiagaraSchema->TrySetDefaultValue(*OverridePin, OverridePin->AutogeneratedDefaultValue, true);
		}
		else
		{
			OverridePin->DefaultValue = OverridePin->AutogeneratedDefaultValue;
		}
	}
	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);
	FUeAgentInterfaceLogger::Log(
		TEXT("Niagara ClearModuleInput: emitter=%s module=%s input=%s"),
		*EditContext.Emitter->GetPathName(),
		*ModuleNode->GetFunctionName(),
		*ResolvedInputName);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetStringField(TEXT("cleared_input_name"), ResolvedInputName);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterAddStageNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	double NodePosX = 0.0;
	double NodePosY = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("node_pos_x"), NodePosX);
	JsonTryGetNumber(Ctx.Params, TEXT("node_pos_y"), NodePosY);

	UEdGraphNode* AddedNode = nullptr;
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraStageNode", "UeAgentInterface Add Niagara Stage Node"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();

	FString ModuleScriptPath;
	const bool bHasModuleScriptPath = JsonTryGetString(Ctx.Params, TEXT("module_script_asset_path"), ModuleScriptPath) && !ModuleScriptPath.IsEmpty();
	if (bHasModuleScriptPath)
	{
		if (Stage.Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara AddStageNode blocked: emitter=%s stage_key=%s usage=%s module=%s reason=particle_event_script_crash_guard"),
				*EditContext.Emitter->GetPathName(),
				*Stage.StageKey,
				*UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage),
				*ModuleScriptPath);
			OutError = TEXT("stage_add_stage_node_particle_event_script_blocked");
			return false;
		}

		TArray<UNiagaraNodeFunctionCall*> ExistingModules;
		UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ExistingModules);
		if (ExistingModules.Num() == 0)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara AddStageNode blocked: emitter=%s stage_key=%s usage=%s module=%s reason=stage_stack_invalid_or_empty"),
				*EditContext.Emitter->GetPathName(),
				*Stage.StageKey,
				*UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage),
				*ModuleScriptPath);
			OutError = TEXT("stage_stack_invalid_or_empty_add_node_blocked");
			return false;
		}
	}
	if (bHasModuleScriptPath)
	{
		UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(ModuleScriptPath));
		if (!ModuleScript)
		{
			OutError = TEXT("module_script_not_found");
			return false;
		}

		int32 TargetIndex = INDEX_NONE;
		double TargetIndexNumber = 0.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
		{
			TargetIndex = FMath::RoundToInt(TargetIndexNumber);
		}

		FString SuggestedName;
		JsonTryGetString(Ctx.Params, TEXT("module_name"), SuggestedName);

		UNiagaraNodeFunctionCall* AddedModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
			ModuleScript,
			*Stage.OutputNode,
			TargetIndex,
			SuggestedName,
			ModuleScript->GetExposedVersion().VersionGuid);
		if (!AddedModule)
		{
			OutError = TEXT("add_stage_node_failed");
			return false;
		}

		AddedModule->NodePosX = FMath::RoundToInt(NodePosX);
		AddedModule->NodePosY = FMath::RoundToInt(NodePosY);
		AddedNode = AddedModule;
	}
	else
	{
		FString NodeClassPath;
		if (!JsonTryGetString(Ctx.Params, TEXT("node_class"), NodeClassPath) || NodeClassPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_node_class_or_module_script_asset_path");
			return false;
		}

		UClass* NodeClass = UeAgentNiagaraOps::ResolveClassByPath(NodeClassPath);
		if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("invalid_node_class");
			return false;
		}

		AddedNode = NewObject<UEdGraphNode>(EditContext.Graph, NodeClass, NAME_None, RF_Transactional);
		if (!AddedNode)
		{
			OutError = TEXT("create_stage_node_failed");
			return false;
		}

		AddedNode->CreateNewGuid();
		AddedNode->NodePosX = FMath::RoundToInt(NodePosX);
		AddedNode->NodePosY = FMath::RoundToInt(NodePosY);
		AddedNode->AllocateDefaultPins();
		EditContext.Graph->AddNode(AddedNode, true, false);
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetObjectField(TEXT("node"), UeAgentNiagaraOps::BuildStageNodeJson(*AddedNode, INDEX_NONE, true, true));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterConnectStageNodes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	FString FromNodeGuidText;
	FString ToNodeGuidText;
	FString FromPinName;
	FString ToPinName;
	if (!JsonTryGetString(Ctx.Params, TEXT("from_node_guid"), FromNodeGuidText) || FromNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_from_node_guid");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("to_node_guid"), ToNodeGuidText) || ToNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_to_node_guid");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("from_pin"), FromPinName) || FromPinName.IsEmpty())
	{
		OutError = TEXT("missing_from_pin");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("to_pin"), ToPinName) || ToPinName.IsEmpty())
	{
		OutError = TEXT("missing_to_pin");
		return false;
	}

	FGuid FromNodeGuid;
	FGuid ToNodeGuid;
	if (!FGuid::Parse(FromNodeGuidText, FromNodeGuid))
	{
		OutError = TEXT("invalid_from_node_guid");
		return false;
	}
	if (!FGuid::Parse(ToNodeGuidText, ToNodeGuid))
	{
		OutError = TEXT("invalid_to_node_guid");
		return false;
	}

	UEdGraphNode* FromNode = UeAgentNiagaraOps::FindGraphNodeByGuid(*EditContext.Graph, FromNodeGuid);
	UEdGraphNode* ToNode = UeAgentNiagaraOps::FindGraphNodeByGuid(*EditContext.Graph, ToNodeGuid);
	if (!FromNode || !ToNode)
	{
		OutError = TEXT("connect_nodes_not_found");
		return false;
	}

	UEdGraphPin* FromPin = UeAgentNiagaraOps::ResolvePinByName(*FromNode, FromPinName);
	UEdGraphPin* ToPin = UeAgentNiagaraOps::ResolvePinByName(*ToNode, ToPinName);
	if (!FromPin || !ToPin)
	{
		OutError = TEXT("connect_pin_not_found");
		return false;
	}

	bool bBreakInputLinks = false;
	JsonTryGetBool(Ctx.Params, TEXT("break_input_links"), bBreakInputLinks);

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "ConnectNiagaraStageNodes", "UeAgentInterface Connect Niagara Stage Nodes"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	FromNode->Modify();
	ToNode->Modify();

	if (bBreakInputLinks && ToPin->Direction == EGPD_Input && ToPin->LinkedTo.Num() > 0)
	{
		ToPin->BreakAllPinLinks(true);
	}

	bool bConnected = false;
	if (const UEdGraphSchema* Schema = EditContext.Graph->GetSchema())
	{
		bConnected = Schema->TryCreateConnection(FromPin, ToPin);
	}
	if (!bConnected)
	{
		FromPin->MakeLinkTo(ToPin);
		bConnected = FromPin->LinkedTo.Contains(ToPin) || ToPin->LinkedTo.Contains(FromPin);
	}
	if (!bConnected)
	{
		OutError = TEXT("connect_stage_nodes_failed");
		return false;
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("from_node_guid"), FromNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("from_pin"), FromPin->PinName.ToString());
	OutData->SetStringField(TEXT("to_node_guid"), ToNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("to_pin"), ToPin->PinName.ToString());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterDisconnectStageNodes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	FString FromNodeGuidText;
	FString FromPinName;
	if (!JsonTryGetString(Ctx.Params, TEXT("from_node_guid"), FromNodeGuidText) || FromNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_from_node_guid");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("from_pin"), FromPinName) || FromPinName.IsEmpty())
	{
		OutError = TEXT("missing_from_pin");
		return false;
	}

	FGuid FromNodeGuid;
	if (!FGuid::Parse(FromNodeGuidText, FromNodeGuid))
	{
		OutError = TEXT("invalid_from_node_guid");
		return false;
	}

	UEdGraphNode* FromNode = UeAgentNiagaraOps::FindGraphNodeByGuid(*EditContext.Graph, FromNodeGuid);
	if (!FromNode)
	{
		OutError = TEXT("disconnect_from_node_not_found");
		return false;
	}

	UEdGraphPin* FromPin = UeAgentNiagaraOps::ResolvePinByName(*FromNode, FromPinName);
	if (!FromPin)
	{
		OutError = TEXT("disconnect_from_pin_not_found");
		return false;
	}

	int32 RemovedLinkCount = 0;
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "DisconnectNiagaraStageNodes", "UeAgentInterface Disconnect Niagara Stage Nodes"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	FromNode->Modify();

	FString ToNodeGuidText;
	FString ToPinName;
	if (JsonTryGetString(Ctx.Params, TEXT("to_node_guid"), ToNodeGuidText) && !ToNodeGuidText.IsEmpty() &&
		JsonTryGetString(Ctx.Params, TEXT("to_pin"), ToPinName) && !ToPinName.IsEmpty())
	{
		FGuid ToNodeGuid;
		if (!FGuid::Parse(ToNodeGuidText, ToNodeGuid))
		{
			OutError = TEXT("invalid_to_node_guid");
			return false;
		}

		UEdGraphNode* ToNode = UeAgentNiagaraOps::FindGraphNodeByGuid(*EditContext.Graph, ToNodeGuid);
		if (!ToNode)
		{
			OutError = TEXT("disconnect_to_node_not_found");
			return false;
		}
		UEdGraphPin* ToPin = UeAgentNiagaraOps::ResolvePinByName(*ToNode, ToPinName);
		if (!ToPin)
		{
			OutError = TEXT("disconnect_to_pin_not_found");
			return false;
		}

		const int32 OldLinkCount = FromPin->LinkedTo.Num();
		FromPin->BreakLinkTo(ToPin);
		RemovedLinkCount = FMath::Max(0, OldLinkCount - FromPin->LinkedTo.Num());
	}
	else
	{
		RemovedLinkCount = FromPin->LinkedTo.Num();
		FromPin->BreakAllPinLinks(true);
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("from_node_guid"), FromNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("from_pin"), FromPin->PinName.ToString());
	OutData->SetNumberField(TEXT("removed_link_count"), RemovedLinkCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

namespace UeAgentNiagaraFolderOps
{
	static constexpr int32 NiagaraFolderFormatVersion = 1;
	static thread_local bool GIncludeRawPropertiesInFolderExport = false;

	struct FScopedIncludeRawPropertiesInFolderExport
	{
		explicit FScopedIncludeRawPropertiesInFolderExport(const bool bInIncludeRawProperties)
			: bPreviousValue(GIncludeRawPropertiesInFolderExport)
		{
			GIncludeRawPropertiesInFolderExport = bInIncludeRawProperties;
		}

		~FScopedIncludeRawPropertiesInFolderExport()
		{
			GIncludeRawPropertiesInFolderExport = bPreviousValue;
		}

	private:
		bool bPreviousValue = false;
	};

	static int32 GetJsonIntField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
	{
		double Value = 0.0;
		return Obj.IsValid() && Obj->TryGetNumberField(FieldName, Value) ? FMath::RoundToInt(Value) : 0;
	}

	static bool IsRawAuthoringFieldName(const FString& FieldName)
	{
		return FieldName.Equals(TEXT("raw_properties"), ESearchCase::CaseSensitive)
			|| FieldName.Equals(TEXT("raw_properties_count"), ESearchCase::CaseSensitive)
			|| FieldName.EndsWith(TEXT("_raw_properties"), ESearchCase::CaseSensitive)
			|| FieldName.EndsWith(TEXT("_raw_properties_count"), ESearchCase::CaseSensitive)
			|| FieldName.Equals(TEXT("readonly_properties"), ESearchCase::CaseSensitive)
			|| FieldName.Equals(TEXT("readback_diff"), ESearchCase::CaseSensitive)
			|| FieldName.Equals(TEXT("diagnostics"), ESearchCase::CaseSensitive)
			|| FieldName.Equals(TEXT("debug_diagnostics"), ESearchCase::CaseSensitive)
			|| FieldName.Equals(TEXT("compile_report"), ESearchCase::CaseSensitive)
			|| FieldName.Equals(TEXT("runtime_probe_report"), ESearchCase::CaseSensitive);
	}

	static void StripRawAuthoringFieldsFromValue(const TSharedPtr<FJsonValue>& Value);

	static void StripRawAuthoringFieldsFromObject(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		TArray<FString> KeysToRemove;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			if (IsRawAuthoringFieldName(Pair.Key))
			{
				KeysToRemove.Add(Pair.Key);
				continue;
			}
			StripRawAuthoringFieldsFromValue(Pair.Value);
		}

		for (const FString& Key : KeysToRemove)
		{
			Obj->RemoveField(Key);
		}
	}

	static void StripRawAuthoringFieldsFromValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return;
		}
		if (Value->Type == EJson::Object)
		{
			StripRawAuthoringFieldsFromObject(Value->AsObject());
			return;
		}
		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& ArrayValue : Value->AsArray())
			{
				StripRawAuthoringFieldsFromValue(ArrayValue);
			}
		}
	}

	struct FPostApplyStackIssueReport
	{
		bool bCollectAfterApply = true;
		bool bFailOnStackErrors = false;
		bool bCollectionOk = false;
		FString CollectionError;
		FString SeverityFilter = TEXT("warning_or_error");
		bool bPreferExistingViewModel = true;
		bool bOpenEditorIfNeeded = true;
		bool bCompileBeforeRead = true;
		bool bForceCompileBeforeRead = true;
		bool bWaitForCompileBeforeRead = true;
		TSharedPtr<FJsonObject> Report;
		int32 TotalIssueCount = 0;
		int32 TotalErrorCount = 0;
		int32 TotalWarningCount = 0;
		int32 TotalInfoCount = 0;
		int32 ReturnedIssueCount = 0;
	};

	static bool CollectPostApplyStackIssues(
		const FUeAgentHttpServer* Server,
		const FUeAgentRequestContext& SourceCtx,
		const FString& AssetPath,
		const bool bDefaultFailOnStackErrors,
		const FString& RequestIdSuffix,
		FPostApplyStackIssueReport& OutReport)
	{
		if (!Server)
		{
			OutReport.CollectionError = TEXT("missing_server");
			return false;
		}

		OutReport.bFailOnStackErrors = bDefaultFailOnStackErrors;
		if (SourceCtx.Params.IsValid())
		{
			SourceCtx.Params->TryGetBoolField(TEXT("collect_stack_issues_after_apply"), OutReport.bCollectAfterApply);
			SourceCtx.Params->TryGetBoolField(TEXT("fail_on_stack_errors"), OutReport.bFailOnStackErrors);
			SourceCtx.Params->TryGetStringField(TEXT("stack_issue_severity_filter"), OutReport.SeverityFilter);
			SourceCtx.Params->TryGetBoolField(TEXT("prefer_existing_stack_view_model"), OutReport.bPreferExistingViewModel);
			SourceCtx.Params->TryGetBoolField(TEXT("open_editor_for_stack_issues"), OutReport.bOpenEditorIfNeeded);
			SourceCtx.Params->TryGetBoolField(TEXT("compile_before_stack_issue_read"), OutReport.bCompileBeforeRead);
			SourceCtx.Params->TryGetBoolField(TEXT("force_compile"), OutReport.bForceCompileBeforeRead);
			SourceCtx.Params->TryGetBoolField(TEXT("wait_for_complete"), OutReport.bWaitForCompileBeforeRead);
		}
		OutReport.SeverityFilter.TrimStartAndEndInline();
		if (OutReport.SeverityFilter.IsEmpty())
		{
			OutReport.SeverityFilter = TEXT("warning_or_error");
		}

		if (!OutReport.bCollectAfterApply)
		{
			OutReport.bCollectionOk = false;
			return true;
		}

		FUeAgentRequestContext StackCtx;
		StackCtx.RequestId = SourceCtx.RequestId.IsEmpty()
			? FString::Printf(TEXT("post_apply_stack_issues_%s"), *RequestIdSuffix)
			: FString::Printf(TEXT("%s_%s"), *SourceCtx.RequestId, *RequestIdSuffix);
		StackCtx.Command = TEXT("niagara_get_stack_issues");
		StackCtx.Params = MakeShared<FJsonObject>();
		StackCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
		StackCtx.Params->SetStringField(TEXT("severity_filter"), OutReport.SeverityFilter);
		StackCtx.Params->SetBoolField(TEXT("prefer_existing_view_model"), OutReport.bPreferExistingViewModel);
		StackCtx.Params->SetBoolField(TEXT("open_editor_if_needed"), OutReport.bOpenEditorIfNeeded);
		StackCtx.Params->SetBoolField(TEXT("compile_before_read"), OutReport.bCompileBeforeRead);
		StackCtx.Params->SetBoolField(TEXT("force_compile"), OutReport.bForceCompileBeforeRead);
		StackCtx.Params->SetBoolField(TEXT("wait_for_complete"), OutReport.bWaitForCompileBeforeRead);

		OutReport.Report = MakeShared<FJsonObject>();
		OutReport.bCollectionOk = Server->CmdNiagaraGetStackIssues(StackCtx, OutReport.Report, OutReport.CollectionError);
		if (!OutReport.bCollectionOk)
		{
			return false;
		}

		OutReport.TotalIssueCount = GetJsonIntField(OutReport.Report, TEXT("total_issue_count"));
		OutReport.TotalErrorCount = GetJsonIntField(OutReport.Report, TEXT("total_error_count"));
		OutReport.TotalWarningCount = GetJsonIntField(OutReport.Report, TEXT("total_warning_count"));
		OutReport.TotalInfoCount = GetJsonIntField(OutReport.Report, TEXT("total_info_count"));
		OutReport.ReturnedIssueCount = GetJsonIntField(OutReport.Report, TEXT("returned_issue_count"));
		return true;
	}

	static void AppendPostApplyStackIssueFields(TSharedPtr<FJsonObject> OutData, const FPostApplyStackIssueReport& Report)
	{
		if (!OutData.IsValid())
		{
			return;
		}

		OutData->SetBoolField(TEXT("collect_stack_issues_after_apply"), Report.bCollectAfterApply);
		OutData->SetBoolField(TEXT("fail_on_stack_errors"), Report.bFailOnStackErrors);
		OutData->SetBoolField(TEXT("stack_issue_collection_ok"), Report.bCollectionOk);
		OutData->SetStringField(TEXT("stack_issue_collection_error"), Report.CollectionError);
		OutData->SetStringField(TEXT("stack_issue_severity_filter"), Report.SeverityFilter);
		OutData->SetBoolField(TEXT("prefer_existing_stack_view_model"), Report.bPreferExistingViewModel);
		OutData->SetBoolField(TEXT("open_editor_for_stack_issues"), Report.bOpenEditorIfNeeded);
		OutData->SetBoolField(TEXT("compile_before_stack_issue_read"), Report.bCompileBeforeRead);
		OutData->SetBoolField(TEXT("force_compile_before_stack_issue_read"), Report.bForceCompileBeforeRead);
		OutData->SetBoolField(TEXT("wait_for_stack_issue_compile"), Report.bWaitForCompileBeforeRead);
		OutData->SetNumberField(TEXT("stack_total_issue_count"), Report.TotalIssueCount);
		OutData->SetNumberField(TEXT("stack_error_count"), Report.TotalErrorCount);
		OutData->SetNumberField(TEXT("stack_warning_count"), Report.TotalWarningCount);
		OutData->SetNumberField(TEXT("stack_info_count"), Report.TotalInfoCount);
		OutData->SetNumberField(TEXT("stack_returned_issue_count"), Report.ReturnedIssueCount);
		OutData->SetBoolField(TEXT("has_stack_errors"), Report.TotalErrorCount > 0);
		OutData->SetBoolField(TEXT("has_stack_warnings"), Report.TotalWarningCount > 0);

		if (Report.Report.IsValid())
		{
			OutData->SetObjectField(TEXT("stack_issue_report"), Report.Report);

			const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
			if (Report.Report->TryGetArrayField(TEXT("issues"), Issues) && Issues)
			{
				OutData->SetArrayField(TEXT("stack_issues"), *Issues);
			}

			const TArray<TSharedPtr<FJsonValue>>* Scopes = nullptr;
			if (Report.Report->TryGetArrayField(TEXT("scopes"), Scopes) && Scopes)
			{
				OutData->SetArrayField(TEXT("stack_scopes"), *Scopes);
			}

			FString ViewModelSource;
			if (Report.Report->TryGetStringField(TEXT("view_model_source"), ViewModelSource))
			{
				OutData->SetStringField(TEXT("stack_issue_view_model_source"), ViewModelSource);
			}

			FString AssetKind;
			if (Report.Report->TryGetStringField(TEXT("asset_kind"), AssetKind))
			{
				OutData->SetStringField(TEXT("stack_issue_asset_kind"), AssetKind);
			}
		}
	}

	static FString GetSystemFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraSystem")));
	}

	static FString GetEmitterFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraEmitter")));
	}

	static FString GetScriptFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraScript")));
	}

	static FString NormalizeAssetRelativeFolder(const FString& InAssetPath)
	{
		FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(InAssetPath);
		AssetPath.TrimStartAndEndInline();
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		return AssetPath;
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetSystemFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static bool ResolveEmitterFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetEmitterFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static bool ResolveScriptFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetScriptFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static FString MakeSafeFileName(const FString& InName)
	{
		const FString SafeName = FPaths::MakeValidFileName(InName, TEXT('_'));
		return SafeName.IsEmpty() ? TEXT("Item") : SafeName;
	}

	static FString MakeEmitterFolderName(const FString& DisplayName, const FGuid& HandleId, const int32 Index)
	{
		FString Prefix = FString::Printf(TEXT("%02d_%s"), Index, *MakeSafeFileName(DisplayName));
		if (HandleId.IsValid())
		{
			Prefix += TEXT("_") + HandleId.ToString(EGuidFormats::Digits).Left(8);
		}
		return Prefix;
	}

	static FString MakeStageFileName(const int32 Index, const FString& UsageText, const FGuid& UsageId)
	{
		const FString IdText = UsageId.IsValid() ? UsageId.ToString(EGuidFormats::Digits).Left(8) : TEXT("default");
		return FString::Printf(TEXT("%02d_%s_%s.json"), Index, *MakeSafeFileName(UsageText), *IdText);
	}

	static FString MakeJsonString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString JsonText;
		UeAgentJsonDiagnostics::SerializeJsonObjectCompact(Obj, JsonText);
		return JsonText;
	}

	static bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj, TArray<FString>* OutWrittenFiles = nullptr)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		if (!GIncludeRawPropertiesInFolderExport)
		{
			StripRawAuthoringFieldsFromObject(Obj);
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		TSharedPtr<FJsonObject> SaveObj = UeAgentDetailsAuthoring::CloneObjectTree(Obj);
		FString DetailsError;
		if (!UeAgentDetailsAuthoring::ExternalizeInlineDetailsInObjectTree(FilePath, SaveObj, TEXT("niagara_export_folder"), DetailsError))
		{
			return false;
		}
		const bool bSaved = FFileHelper::SaveStringToFile(
			MakeJsonString(SaveObj),
			*FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (bSaved && OutWrittenFiles)
		{
			OutWrittenFiles->Add(FilePath);
		}
		return bSaved;
	}

	static FString MakeDetailsStem(const FString& Category, const FString& OwnerId)
	{
		FString SafeOwnerId = FPaths::MakeValidFileName(OwnerId, TEXT('_'));
		if (SafeOwnerId.IsEmpty())
		{
			SafeOwnerId = TEXT("owner");
		}
		return FPaths::Combine(TEXT("details"), Category, SafeOwnerId);
	}

	static TSharedPtr<FJsonObject> BuildPropertiesContainer(const TArray<TSharedPtr<FJsonValue>>& Properties)
	{
		TSharedPtr<FJsonObject> PropertiesJson = MakeShared<FJsonObject>();
		PropertiesJson->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v1"));
		PropertiesJson->SetStringField(TEXT("profile"), TEXT("authoring"));
		PropertiesJson->SetNumberField(TEXT("property_count"), Properties.Num());
		PropertiesJson->SetBoolField(TEXT("truncated"), false);
		PropertiesJson->SetArrayField(TEXT("properties"), Properties);
		return PropertiesJson;
	}

	static bool ExportFolderOwnerDetails(
		UObject* Object,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& DetailsCategory,
		const TSharedPtr<FJsonObject>& EntryObject,
		FString& OutError)
	{
		if (!Object || !EntryObject.IsValid())
		{
			return true;
		}

		const FString ResolvedOwnerId = OwnerId.IsEmpty() ? Object->GetName() : OwnerId;
		const FString DetailsStem = MakeDetailsStem(DetailsCategory, ResolvedOwnerId);
		UeAgentDetailsAuthoring::FOwnerDetailsExportRequest Request =
			UeAgentDetailsAuthoring::MakeOwnerDetailsExportRequest(
				Object,
				OwnerKind,
				ResolvedOwnerId,
				FPaths::Combine(EntryDirectory, DetailsStem),
				SourceCommand,
				FPaths::Combine(DetailsStem, TEXT("index.json")),
				TEXT("details"));

		TSharedPtr<FJsonObject> DetailsSummary;
		if (!UeAgentDetailsAuthoring::ExportOwnerDetails(Request, DetailsSummary, OutError))
		{
			OutError = FString::Printf(TEXT("%s_details_export_failed:%s:%s"), *SourceCommand, *ResolvedOwnerId, *OutError);
			return false;
		}

		EntryObject->RemoveField(TEXT("raw_properties"));
		EntryObject->RemoveField(TEXT("raw_properties_count"));
		EntryObject->SetObjectField(TEXT("details"), DetailsSummary);
		return true;
	}

	static bool ExportFolderPropertyArrayDetails(
		const TArray<TSharedPtr<FJsonValue>>& Properties,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& OwnerName,
		const FString& OwnerPath,
		const FString& OwnerClassPath,
		const FString& DetailsCategory,
		const FString& PropertiesFieldName,
		const FString& CountFieldName,
		const FString& RefFieldName,
		const TSharedPtr<FJsonObject>& EntryObject,
		FString& OutError)
	{
		if (!EntryObject.IsValid())
		{
			return true;
		}

		UeAgentDetailsAuthoring::FOwnerInfo OwnerInfo;
		OwnerInfo.OwnerKind = OwnerKind;
		OwnerInfo.OwnerId = OwnerId.IsEmpty() ? FString(TEXT("owner")) : OwnerId;
		OwnerInfo.OwnerName = OwnerName.IsEmpty() ? OwnerInfo.OwnerId : OwnerName;
		OwnerInfo.OwnerPath = OwnerPath;
		OwnerInfo.OwnerClassPath = OwnerClassPath;
		OwnerInfo.SourceCommand = SourceCommand;

		const FString DetailsStem = MakeDetailsStem(DetailsCategory, OwnerInfo.OwnerId);
		TSharedPtr<FJsonObject> DetailsSummary;
		if (!UeAgentDetailsAuthoring::WriteGroupedDetails(
			FPaths::Combine(EntryDirectory, DetailsStem),
			OwnerInfo,
			BuildPropertiesContainer(Properties),
			DetailsSummary,
			OutError))
		{
			OutError = FString::Printf(TEXT("%s_details_export_failed:%s:%s"), *SourceCommand, *OwnerInfo.OwnerId, *OutError);
			return false;
		}

		FString IndexFile = FPaths::Combine(DetailsStem, TEXT("index.json"));
		IndexFile.ReplaceInline(TEXT("\\"), TEXT("/"));
		DetailsSummary->SetStringField(TEXT("index_file"), IndexFile);
		DetailsSummary->SetStringField(TEXT("ref_field"), RefFieldName);
		DetailsSummary->SetStringField(TEXT("owner_kind"), OwnerInfo.OwnerKind);
		DetailsSummary->SetStringField(TEXT("owner_id"), OwnerInfo.OwnerId);
		EntryObject->RemoveField(PropertiesFieldName);
		if (!CountFieldName.IsEmpty())
		{
			EntryObject->RemoveField(CountFieldName);
		}
		EntryObject->SetObjectField(RefFieldName, DetailsSummary);
		return true;
	}

	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		if (!UeAgentJsonDiagnostics::LoadJsonObjectFile(FilePath, OutObj, nullptr, OutError, false))
		{
			return false;
		}
		return true;
	}

	static bool LoadJsonFileOptional(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		if (!UeAgentJsonDiagnostics::LoadJsonObjectFile(FilePath, OutObj, nullptr, OutError, true))
		{
			return false;
		}
		return true;
	}

	static void AddWarning(TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Code, const FString& Path, const FString& Message)
	{
		UeAgentJsonDiagnostics::AddIssue(Warnings, TEXT("warning"), Code, Path, Message);
	}

	static void AddCoverageArea(
		TArray<TSharedPtr<FJsonValue>>& Areas,
		const FString& Area,
		const FString& ExportSupport,
		const FString& ApplySupport,
		const FString& Notes)
	{
		TSharedPtr<FJsonObject> AreaObj = MakeShared<FJsonObject>();
		AreaObj->SetStringField(TEXT("area"), Area);
		AreaObj->SetStringField(TEXT("export_support"), ExportSupport);
		AreaObj->SetStringField(TEXT("apply_support"), ApplySupport);
		AreaObj->SetStringField(TEXT("notes"), Notes);
		Areas.Add(MakeShared<FJsonValueObject>(AreaObj));
	}

	static TSharedPtr<FJsonObject> BuildCoverageReport(
		const FString& AssetPath,
		const FString& Mode,
		const TArray<TSharedPtr<FJsonValue>>& Warnings,
		const TMap<FString, int32>& Stats)
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetNumberField(TEXT("format_version"), NiagaraFolderFormatVersion);
		Coverage->SetStringField(TEXT("asset_kind"), TEXT("niagara_system"));
		Coverage->SetStringField(TEXT("asset_path"), AssetPath);
		Coverage->SetStringField(TEXT("mode"), Mode);
		Coverage->SetStringField(TEXT("target_engine"), TEXT("UE_5.6"));
		Coverage->SetBoolField(TEXT("uses_system_emitter_handle_model"), true);
		Coverage->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.niagara.system.folder"));
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetBoolField(TEXT("is_lossless_roundtrip"), Warnings.Num() == 0);
		Coverage->SetStringField(TEXT("completion_note"), TEXT("NiagaraSystem folder export/apply covers system details, user parameters, emitter handles, standalone emitters, script assets, event handlers, simulation stages, module stacks, module input defaults, module input dynamic inputs, renderers, event generators and reflected UObject/UStruct details."));

		TArray<TSharedPtr<FJsonValue>> ImplementedProfiles;
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_system")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_emitter")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_script")));
		Coverage->SetArrayField(TEXT("implemented_profiles"), ImplementedProfiles);

		TArray<TSharedPtr<FJsonValue>> PendingProfiles;
		Coverage->SetArrayField(TEXT("pending_profiles"), PendingProfiles);

		TArray<TSharedPtr<FJsonValue>> BlockingGaps;
		Coverage->SetArrayField(TEXT("blocking_gaps"), BlockingGaps);

		TSharedPtr<FJsonObject> StatsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : Stats)
		{
			StatsObj->SetNumberField(Pair.Key, Pair.Value);
		}
		Coverage->SetObjectField(TEXT("stats"), StatsObj);

		TArray<TSharedPtr<FJsonValue>> Areas;
		AddCoverageArea(Areas, TEXT("UNiagaraSystem asset metadata"), TEXT("structured"), TEXT("structured"), TEXT("asset.json and settings/system.json"));
		AddCoverageArea(Areas, TEXT("UNiagaraSystem UObject properties"), TEXT("grouped_details"), TEXT("legacy_property_import_until_P4"), TEXT("New UE fields are exported through grouped details."));
		AddCoverageArea(Areas, TEXT("System Spawn / System Update"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Script references, grouped script metadata and standalone NiagaraScript folder profiles are supported."));
		AddCoverageArea(Areas, TEXT("User parameters"), TEXT("structured"), TEXT("structured"), TEXT("User parameter add/set is supported for Niagara value types accepted by existing atomic commands."));
		AddCoverageArea(Areas, TEXT("Emitter handles"), TEXT("structured"), TEXT("structured"), TEXT("System -> FNiagaraEmitterHandle -> FVersionedNiagaraEmitter is preserved; handle GUIDs are exported and stable logical matching is name/folder based on apply."));
		AddCoverageArea(Areas, TEXT("UNiagaraEmitter version data"), TEXT("grouped_details"), TEXT("legacy_struct_import_until_P4"), TEXT("FVersionedNiagaraEmitterData fields are exported as grouped details."));
		AddCoverageArea(Areas, TEXT("Stages and modules"), TEXT("structured"), TEXT("structured"), TEXT("Stage/module stack order, module script references, module enabled state, module input defaults and module input dynamic inputs are exported and applied."));
		AddCoverageArea(Areas, TEXT("Renderers"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Renderer class/index and UObject details are exported; missing renderers can be added when class is resolvable."));
		AddCoverageArea(Areas, TEXT("Event handlers"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Event handler scripts, output stages and FNiagaraEventScriptProperties details are exported."));
		AddCoverageArea(Areas, TEXT("Event generators"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Spawn/update script event generator arrays are exported explicitly and retained as grouped details."));
		AddCoverageArea(Areas, TEXT("Data interfaces"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Data interface references are exported through graph nodes, module inputs, dynamic inputs and grouped details."));
		AddCoverageArea(Areas, TEXT("Custom Niagara Script / Scratch Pad / Custom HLSL"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Standalone NiagaraScript assets are supported; CustomHlsl nodes are exported with HLSL text where present."));
		AddCoverageArea(Areas, TEXT("External assets"), TEXT("reference"), TEXT("reference"), TEXT("External assets remain references and are not copied into the Niagara folder."));
		Coverage->SetArrayField(TEXT("coverage_areas"), Areas);
		Coverage->SetArrayField(TEXT("warnings"), Warnings);
		Coverage->SetNumberField(TEXT("warning_count"), Warnings.Num());
		return Coverage;
	}

	static TSharedPtr<FJsonObject> BuildEmitterCoverageReport(
		const FString& AssetPath,
		const FString& Mode,
		const TArray<TSharedPtr<FJsonValue>>& Warnings,
		const TMap<FString, int32>& Stats)
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetNumberField(TEXT("format_version"), NiagaraFolderFormatVersion);
		Coverage->SetStringField(TEXT("asset_kind"), TEXT("niagara_emitter"));
		Coverage->SetStringField(TEXT("asset_path"), AssetPath);
		Coverage->SetStringField(TEXT("mode"), Mode);
		Coverage->SetStringField(TEXT("target_engine"), TEXT("UE_5.6"));
		Coverage->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.niagara.emitter.folder"));
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetBoolField(TEXT("is_lossless_roundtrip"), Warnings.Num() == 0);
		Coverage->SetStringField(TEXT("completion_note"), TEXT("Standalone NiagaraEmitter folder export/apply covers emitter metadata, version data, parameters, renderers, event handlers, event generators, simulation stages, module stacks, module input defaults, module input dynamic inputs, script references and grouped reflected details."));

		TArray<TSharedPtr<FJsonValue>> ImplementedProfiles;
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_system")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_emitter")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_script")));
		Coverage->SetArrayField(TEXT("implemented_profiles"), ImplementedProfiles);

		TArray<TSharedPtr<FJsonValue>> PendingProfiles;
		Coverage->SetArrayField(TEXT("pending_profiles"), PendingProfiles);

		TArray<TSharedPtr<FJsonValue>> BlockingGaps;
		Coverage->SetArrayField(TEXT("blocking_gaps"), BlockingGaps);

		TSharedPtr<FJsonObject> StatsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : Stats)
		{
			StatsObj->SetNumberField(Pair.Key, Pair.Value);
		}
		Coverage->SetObjectField(TEXT("stats"), StatsObj);

		TArray<TSharedPtr<FJsonValue>> Areas;
		AddCoverageArea(Areas, TEXT("UNiagaraEmitter asset metadata"), TEXT("structured"), TEXT("structured"), TEXT("asset.json and emitter.json"));
		AddCoverageArea(Areas, TEXT("UNiagaraEmitter version data"), TEXT("grouped_details"), TEXT("legacy_struct_import_until_P4"), TEXT("FVersionedNiagaraEmitterData fields are exported as grouped details."));
		AddCoverageArea(Areas, TEXT("Emitter parameters"), TEXT("structured"), TEXT("structured"), TEXT("Graph parameters and scalar default values are exported and applied."));
		AddCoverageArea(Areas, TEXT("Stages and modules"), TEXT("structured"), TEXT("structured"), TEXT("Stage/module stack order, module script references, module enabled state, module input defaults and module input dynamic inputs are exported and applied."));
		AddCoverageArea(Areas, TEXT("Renderers"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Renderer class/index and UObject details are exported; missing renderers can be added when class is resolvable."));
		AddCoverageArea(Areas, TEXT("Event handlers"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Event handler scripts, output stages and FNiagaraEventScriptProperties details are exported."));
		AddCoverageArea(Areas, TEXT("Event generators"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Spawn/update script event generator arrays are exported explicitly and retained as grouped details."));
		AddCoverageArea(Areas, TEXT("Data interfaces"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Data interface references are exported through graph nodes, module inputs, dynamic inputs and grouped details."));
		AddCoverageArea(Areas, TEXT("Custom Niagara Script / Scratch Pad / Custom HLSL"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Standalone NiagaraScript assets are supported; CustomHlsl nodes are exported with HLSL text where present."));
		Coverage->SetArrayField(TEXT("coverage_areas"), Areas);
		Coverage->SetArrayField(TEXT("warnings"), Warnings);
		Coverage->SetNumberField(TEXT("warning_count"), Warnings.Num());
		return Coverage;
	}

	static TSharedPtr<FJsonObject> BuildScriptCoverageReport(
		const FString& AssetPath,
		const FString& Mode,
		const TArray<TSharedPtr<FJsonValue>>& Warnings,
		const TMap<FString, int32>& Stats)
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetNumberField(TEXT("format_version"), NiagaraFolderFormatVersion);
		Coverage->SetStringField(TEXT("asset_kind"), TEXT("niagara_script"));
		Coverage->SetStringField(TEXT("asset_path"), AssetPath);
		Coverage->SetStringField(TEXT("mode"), Mode);
		Coverage->SetStringField(TEXT("target_engine"), TEXT("UE_5.6"));
		Coverage->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.niagara.script.folder"));
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetBoolField(TEXT("is_lossless_roundtrip"), Warnings.Num() == 0);
		Coverage->SetStringField(TEXT("completion_note"), TEXT("Standalone NiagaraScript folder export/apply covers script metadata, grouped reflected details, graph nodes, graph links and custom HLSL node text."));

		TArray<TSharedPtr<FJsonValue>> ImplementedProfiles;
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_system")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_emitter")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_script")));
		Coverage->SetArrayField(TEXT("implemented_profiles"), ImplementedProfiles);
		Coverage->SetArrayField(TEXT("pending_profiles"), TArray<TSharedPtr<FJsonValue>>());
		Coverage->SetArrayField(TEXT("blocking_gaps"), TArray<TSharedPtr<FJsonValue>>());

		TSharedPtr<FJsonObject> StatsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : Stats)
		{
			StatsObj->SetNumberField(Pair.Key, Pair.Value);
		}
		Coverage->SetObjectField(TEXT("stats"), StatsObj);

		TArray<TSharedPtr<FJsonValue>> Areas;
		AddCoverageArea(Areas, TEXT("UNiagaraScript asset metadata"), TEXT("structured"), TEXT("structured"), TEXT("asset.json and script.json"));
		AddCoverageArea(Areas, TEXT("UNiagaraScript UObject properties"), TEXT("grouped_details"), TEXT("legacy_property_import_until_P4"), TEXT("Reflected fields are exported as grouped details."));
		AddCoverageArea(Areas, TEXT("Script graph"), TEXT("structured_details"), TEXT("structured_legacy_until_P4"), TEXT("Graph nodes, links and node positions are exported; apply initializes a NiagaraScript graph and imports supported reflected state."));
		AddCoverageArea(Areas, TEXT("Custom HLSL"), TEXT("structured"), TEXT("structured"), TEXT("UNiagaraNodeCustomHlsl nodes expose their HLSL text in custom_hlsl_nodes."));
		Coverage->SetArrayField(TEXT("coverage_areas"), Areas);
		Coverage->SetArrayField(TEXT("warnings"), Warnings);
		Coverage->SetNumberField(TEXT("warning_count"), Warnings.Num());
		return Coverage;
	}

	static bool ShouldApplyProperty(const FProperty* Property)
	{
		return Property &&
			Property->HasAnyPropertyFlags(CPF_Edit) &&
			!Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst);
	}

	static TSharedPtr<FJsonObject> BuildPropertyJson(FProperty* Property, void* ValuePtr, UObject* OwnerForExport)
	{
		TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
		if (!Property || !ValuePtr)
		{
			PropertyObj->SetBoolField(TEXT("valid"), false);
			return PropertyObj;
		}

		FString ValueText;
		Property->ExportTextItem_Direct(ValueText, ValuePtr, nullptr, OwnerForExport, PPF_None);
		PropertyObj->SetBoolField(TEXT("valid"), true);
		PropertyObj->SetStringField(TEXT("property_name"), Property->GetName());
		PropertyObj->SetStringField(TEXT("authored_name"), Property->GetAuthoredName());
		PropertyObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		PropertyObj->SetStringField(TEXT("value_text"), ValueText);
		TSharedPtr<FJsonObject> CurveJson;
		if (UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr, CurveJson) && CurveJson.IsValid())
		{
			PropertyObj->SetObjectField(TEXT("value_json"), CurveJson);
			PropertyObj->SetObjectField(TEXT("curve_json"), CurveJson);
			PropertyObj->SetStringField(TEXT("value_json_schema"), UeAgentCurveJson::SchemaName);
		}
		const bool bCanApplyGeneric = ShouldApplyProperty(Property) && !Property->GetName().Equals(TEXT("NodeGuid"), ESearchCase::CaseSensitive);
		PropertyObj->SetBoolField(TEXT("can_apply_generic"), bCanApplyGeneric);
		PropertyObj->SetStringField(TEXT("property_flags"), FString::Printf(TEXT("0x%016llx"), static_cast<unsigned long long>(Property->PropertyFlags)));
		return PropertyObj;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildObjectPropertiesJson(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		if (!Object)
		{
			return PropertiesJson;
		}

		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildPropertyJson(Property, ValuePtr, Object)));
		}
		return PropertiesJson;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildStructPropertiesJson(UStruct* StructType, void* StructData, UObject* OwnerForExport)
	{
		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		if (!StructType || !StructData)
		{
			return PropertiesJson;
		}

		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructData);
			PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildPropertyJson(Property, ValuePtr, OwnerForExport)));
		}
		return PropertiesJson;
	}

	static bool TryGetArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const TArray<TSharedPtr<FJsonValue>>*& OutArray)
	{
		OutArray = nullptr;
		return Obj.IsValid() && Obj->TryGetArrayField(Key, OutArray) && OutArray != nullptr;
	}

	static FString GetStringFieldAny(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, const FString& DefaultValue = FString())
	{
		if (!Obj.IsValid())
		{
			return DefaultValue;
		}

		for (const FString& Key : Keys)
		{
			FString Value;
			if (Obj->TryGetStringField(Key, Value))
			{
				return Value;
			}
		}
		return DefaultValue;
	}

	static bool GetObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedPtr<FJsonObject>* FoundObject = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(Key, FoundObject) || !FoundObject || !FoundObject->IsValid())
		{
			return false;
		}
		OutObject = *FoundObject;
		return true;
	}

	static void AppendPropertyArray(TSharedPtr<FJsonObject> TargetObj, const FString& FieldName, const TArray<TSharedPtr<FJsonValue>>& Properties)
	{
		TargetObj->SetArrayField(FieldName, Properties);
		TargetObj->SetNumberField(FieldName + TEXT("_count"), Properties.Num());
	}

	static TSharedPtr<FJsonObject> BuildScriptReferenceJson(UNiagaraScript* Script, const FString& OwnerKind, const FString& OwnerPath)
	{
		TSharedPtr<FJsonObject> ScriptObj = MakeShared<FJsonObject>();
		ScriptObj->SetStringField(TEXT("owner_kind"), OwnerKind);
		ScriptObj->SetStringField(TEXT("owner_path"), OwnerPath);
		if (!Script)
		{
			ScriptObj->SetBoolField(TEXT("valid"), false);
			return ScriptObj;
		}

		ScriptObj->SetBoolField(TEXT("valid"), true);
		ScriptObj->SetStringField(TEXT("script_name"), Script->GetName());
		ScriptObj->SetStringField(TEXT("script_object_path"), Script->GetPathName());
		ScriptObj->SetStringField(TEXT("script_asset_path"), Script->GetOutermost()->GetName());
		ScriptObj->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Script->GetUsage()));
		ScriptObj->SetStringField(TEXT("script_usage_id"), Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
		ScriptObj->SetStringField(TEXT("compile_status"), UeAgentNiagaraOps::ScriptCompileStatusToString(Script->GetLastCompileStatus()));
		AppendPropertyArray(ScriptObj, TEXT("raw_properties"), BuildObjectPropertiesJson(Script));
		return ScriptObj;
	}

	static void AddUniqueScriptReference(TMap<FString, TSharedPtr<FJsonObject>>& ScriptRefs, UNiagaraScript* Script, const FString& OwnerKind, const FString& OwnerPath)
	{
		if (!Script)
		{
			return;
		}

		const FString Key = Script->GetPathName();
		if (!ScriptRefs.Contains(Key))
		{
			ScriptRefs.Add(Key, BuildScriptReferenceJson(Script, OwnerKind, OwnerPath));
		}
	}

	static void AddUniqueScriptReferenceByPath(TMap<FString, TSharedPtr<FJsonObject>>& ScriptRefs, const FString& ScriptPath, const FString& OwnerKind, const FString& OwnerPath)
	{
		if (ScriptPath.IsEmpty() || ScriptRefs.Contains(ScriptPath))
		{
			return;
		}

		TSharedPtr<FJsonObject> ScriptObj = MakeShared<FJsonObject>();
		ScriptObj->SetBoolField(TEXT("valid"), false);
		ScriptObj->SetStringField(TEXT("script_object_path"), ScriptPath);
		ScriptObj->SetStringField(TEXT("owner_kind"), OwnerKind);
		ScriptObj->SetStringField(TEXT("owner_path"), OwnerPath);
		ScriptObj->SetStringField(TEXT("reference_only_reason"), TEXT("asset_not_loaded_or_non_script_reference"));
		ScriptRefs.Add(ScriptPath, ScriptObj);
	}

	static void ExportGraphLinksForNodes(const TSet<UEdGraphNode*>& Nodes, TArray<TSharedPtr<FJsonValue>>& OutLinks)
	{
		for (UEdGraphNode* Node : Nodes)
		{
			if (!Node)
			{
				continue;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode() || !Nodes.Contains(LinkedPin->GetOwningNode()))
					{
						continue;
					}
					TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
					LinkObj->SetStringField(TEXT("from_node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
					LinkObj->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
					LinkObj->SetStringField(TEXT("to_node_guid"), LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
					LinkObj->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
					OutLinks.Add(MakeShared<FJsonValueObject>(LinkObj));
				}
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildStageFolderJson(
		UNiagaraNodeOutput& OutputNode,
		const int32 StageIndex,
		FVersionedNiagaraEmitterData* EmitterData,
		TMap<FString, TSharedPtr<FJsonObject>>& ScriptRefs,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		const ENiagaraScriptUsage Usage = OutputNode.GetUsage();
		const FGuid UsageId = OutputNode.GetUsageId();
		const FString UsageText = UeAgentNiagaraOps::ScriptUsageToString(Usage);
		const FString StageKey = UeAgentNiagaraOps::MakeStageKey(Usage, UsageId);

		StageObj->SetNumberField(TEXT("stage_index"), StageIndex);
		StageObj->SetStringField(TEXT("stage_key"), StageKey);
		StageObj->SetStringField(TEXT("script_usage"), UsageText);
		StageObj->SetStringField(TEXT("script_usage_id"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		StageObj->SetStringField(TEXT("output_node_guid"), OutputNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

		FString StageType = TEXT("standard");
		if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			StageType = TEXT("simulation_stage");
			if (EmitterData)
			{
				if (UNiagaraSimulationStageBase* SimulationStage = EmitterData->GetSimulationStageById(UsageId))
				{
					StageObj->SetStringField(TEXT("simulation_stage_class"), SimulationStage->GetClass()->GetPathName());
					StageObj->SetStringField(TEXT("simulation_stage_name"), SimulationStage->GetFName().ToString());
					if (!ExportFolderOwnerDetails(
						SimulationStage,
						EntryDirectory,
						SourceCommand,
						TEXT("niagara_simulation_stage"),
						StageKey,
						TEXT("simulation_stages"),
						StageObj,
						OutError))
					{
						return nullptr;
					}
				}
			}
		}
		else if (Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			StageType = TEXT("event_stage");
		}
		StageObj->SetStringField(TEXT("stage_type"), StageType);

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		UeAgentNiagaraOps::GetStageModuleNodes(OutputNode, ModuleNodes);
		TArray<TSharedPtr<FJsonValue>> ModulesJson;
		TSet<UEdGraphNode*> StageNodes;
		TArray<UEdGraphNode*> UpstreamNodes;
		UeAgentNiagaraOps::GatherUpstreamNodesRecursive(&OutputNode, StageNodes, UpstreamNodes);
		StageNodes.Add(&OutputNode);
		ModulesJson.Reserve(ModuleNodes.Num());
		for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
		{
			UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
			if (!ModuleNode)
			{
				continue;
			}

			StageNodes.Add(ModuleNode);
			TSharedPtr<FJsonObject> ModuleObj = UeAgentNiagaraOps::BuildModuleNodeJson(*ModuleNode, ModuleIndex, true);
			if (!ExportFolderOwnerDetails(
				ModuleNode,
				EntryDirectory,
				SourceCommand,
				TEXT("niagara_module"),
				FString::Printf(TEXT("%s.module_%d"), *StageKey, ModuleIndex),
				TEXT("modules"),
				ModuleObj,
				OutError))
			{
				return nullptr;
			}
			ModulesJson.Add(MakeShared<FJsonValueObject>(ModuleObj));
			AddUniqueScriptReference(ScriptRefs, ModuleNode->FunctionScript, TEXT("stage_module"), StageKey);
			AddUniqueScriptReferenceByPath(ScriptRefs, UeAgentNiagaraOps::GetModuleScriptPath(*ModuleNode), TEXT("stage_module"), StageKey);
		}
		StageObj->SetArrayField(TEXT("modules"), ModulesJson);
		StageObj->SetNumberField(TEXT("module_count"), ModulesJson.Num());

		TArray<TSharedPtr<FJsonValue>> NodesJson;
		TArray<TSharedPtr<FJsonValue>> CustomHlslNodesJson;
		for (UEdGraphNode* StageNode : StageNodes)
		{
			if (!StageNode)
			{
				continue;
			}
			TSharedPtr<FJsonObject> NodeJson = UeAgentNiagaraOps::BuildStageNodeJson(*StageNode, NodesJson.Num(), true, true);
			if (UNiagaraNodeCustomHlsl* CustomHlslNode = Cast<UNiagaraNodeCustomHlsl>(StageNode))
			{
				FString CustomHlslText;
				if (UeAgentNiagaraOps::GetObjectPropertyText(CustomHlslNode, TEXT("CustomHlsl"), CustomHlslText))
				{
					NodeJson->SetStringField(TEXT("custom_hlsl"), CustomHlslText);
				}
			}
			NodesJson.Add(MakeShared<FJsonValueObject>(NodeJson));
			if (StageNode->GetClass() && StageNode->GetClass()->GetName().Contains(TEXT("CustomHlsl")))
			{
				CustomHlslNodesJson.Add(MakeShared<FJsonValueObject>(NodeJson));
			}
		}
		StageObj->SetArrayField(TEXT("graph_nodes"), NodesJson);
		StageObj->SetNumberField(TEXT("graph_node_count"), NodesJson.Num());
		StageObj->SetArrayField(TEXT("custom_hlsl_nodes"), CustomHlslNodesJson);
		StageObj->SetNumberField(TEXT("custom_hlsl_node_count"), CustomHlslNodesJson.Num());

		TArray<TSharedPtr<FJsonValue>> LinksJson;
		ExportGraphLinksForNodes(StageNodes, LinksJson);
		StageObj->SetArrayField(TEXT("graph_links"), LinksJson);
		StageObj->SetNumberField(TEXT("graph_link_count"), LinksJson.Num());
		return StageObj;
	}

	static bool ExportEmitterFolder(
		const FString& EmitterFolder,
		const FString& EmitterFolderRelative,
		const int32 EmitterIndex,
		const FNiagaraEmitterHandle& Handle,
		const FString& SourceCommand,
		TArray<FString>& WrittenFiles,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		TMap<FString, TSharedPtr<FJsonObject>>& ScriptRefs,
		TSharedPtr<FJsonObject>& OutEmitterIndexObj,
		FString& OutError)
	{
		FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
		UNiagaraEmitter* Emitter = VersionedEmitter.Emitter;
		const FVersionedNiagaraEmitterData* ConstEmitterData = VersionedEmitter.GetEmitterData();
		if (!ConstEmitterData && Emitter)
		{
			ConstEmitterData = Emitter->GetLatestEmitterData();
		}
		FVersionedNiagaraEmitterData* EmitterData = const_cast<FVersionedNiagaraEmitterData*>(ConstEmitterData);

		OutEmitterIndexObj = MakeShared<FJsonObject>();
		OutEmitterIndexObj->SetNumberField(TEXT("index"), EmitterIndex);
		OutEmitterIndexObj->SetStringField(TEXT("logical_id"), EmitterFolderRelative);
		OutEmitterIndexObj->SetStringField(TEXT("folder"), EmitterFolderRelative);
		OutEmitterIndexObj->SetStringField(TEXT("handle_id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		OutEmitterIndexObj->SetStringField(TEXT("emitter_name"), Handle.GetName().ToString());
		OutEmitterIndexObj->SetStringField(TEXT("unique_instance_name"), Handle.GetUniqueInstanceName());
		OutEmitterIndexObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		OutEmitterIndexObj->SetStringField(TEXT("emitter_version"), VersionedEmitter.Version.ToString(EGuidFormats::DigitsWithHyphensLower));

		TSharedPtr<FJsonObject> SourceObj = MakeShared<FJsonObject>();
		if (Emitter)
		{
			const bool bSystemInstance = Emitter->GetTypedOuter<UNiagaraSystem>() != nullptr;
			SourceObj->SetStringField(TEXT("ownership"), bSystemInstance ? TEXT("system_instance") : TEXT("standalone_asset"));
			SourceObj->SetBoolField(TEXT("is_embedded_instance"), bSystemInstance);
			if (bSystemInstance)
			{
				SourceObj->SetStringField(TEXT("source_system_asset_path"), Emitter->GetOutermost()->GetName());
			}
			else
			{
				SourceObj->SetStringField(TEXT("emitter_asset_path"), Emitter->GetOutermost()->GetName());
			}
			SourceObj->SetStringField(TEXT("emitter_object_path"), Emitter->GetPathName());
			SourceObj->SetStringField(TEXT("emitter_class"), Emitter->GetClass()->GetPathName());
		}
		OutEmitterIndexObj->SetObjectField(TEXT("source"), SourceObj);

		TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
		EmitterObj->SetNumberField(TEXT("format_version"), NiagaraFolderFormatVersion);
		EmitterObj->SetStringField(TEXT("asset_kind"), TEXT("niagara_emitter_in_system"));
		EmitterObj->SetStringField(TEXT("logical_id"), EmitterFolderRelative);
		EmitterObj->SetStringField(TEXT("handle_id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		EmitterObj->SetStringField(TEXT("emitter_name"), Handle.GetName().ToString());
		EmitterObj->SetStringField(TEXT("unique_instance_name"), Handle.GetUniqueInstanceName());
		EmitterObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		EmitterObj->SetObjectField(TEXT("source"), SourceObj);
		EmitterObj->SetStringField(TEXT("emitter_version"), VersionedEmitter.Version.ToString(EGuidFormats::DigitsWithHyphensLower));
		if (EmitterData)
		{
			EmitterObj->SetStringField(TEXT("version_guid"), EmitterData->Version.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			EmitterObj->SetNumberField(TEXT("major_version"), EmitterData->Version.MajorVersion);
			EmitterObj->SetNumberField(TEXT("minor_version"), EmitterData->Version.MinorVersion);
		}
		TArray<TSharedPtr<FJsonValue>> EmitterProperties;
		if (EmitterData)
		{
			EmitterProperties = BuildStructPropertiesJson(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, Emitter);
		}
		else
		{
			AddWarning(Warnings, TEXT("emitter_missing_version_data"), EmitterFolderRelative, TEXT("Emitter has no readable FVersionedNiagaraEmitterData."));
		}
		if (!ExportFolderPropertyArrayDetails(
			EmitterProperties,
			EmitterFolder,
			SourceCommand,
			TEXT("niagara_emitter_settings"),
			EmitterFolderRelative,
			EmitterFolderRelative,
			Emitter ? Emitter->GetPathName() : FString(),
			FVersionedNiagaraEmitterData::StaticStruct()->GetPathName(),
			TEXT("emitter"),
			TEXT("raw_properties"),
			TEXT("raw_properties_count"),
			TEXT("details"),
			EmitterObj,
			OutError))
		{
			return false;
		}
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("emitter.json")), EmitterObj, &WrittenFiles))
		{
			return false;
		}
		if (GIncludeRawPropertiesInFolderExport)
		{
			TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
			PropertiesObj->SetStringField(TEXT("logical_id"), EmitterFolderRelative);
			AppendPropertyArray(PropertiesObj, TEXT("raw_properties"), EmitterProperties);
			if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("properties.json")), PropertiesObj, &WrittenFiles))
			{
				return false;
			}
		}

		TSharedPtr<FJsonObject> ParametersObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ParametersJson;
		if (EmitterData)
		{
			UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
			UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
			if (Graph)
			{
				const UNiagaraGraph::FScriptVariableMap& ScriptVariableMap = Graph->GetAllMetaData();
				for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : ScriptVariableMap)
				{
					ParametersJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildGraphParameterJson(Pair.Key, Pair.Value, true)));
				}
			}
		}
		ParametersObj->SetArrayField(TEXT("parameters"), ParametersJson);
		ParametersObj->SetNumberField(TEXT("parameter_count"), ParametersJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("parameters.json")), ParametersObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> RapidIterationObj = MakeShared<FJsonObject>();
		RapidIterationObj->SetArrayField(TEXT("parameters"), TArray<TSharedPtr<FJsonValue>>());
		RapidIterationObj->SetStringField(TEXT("note"), TEXT("Rapid iteration parameters are compiler/runtime generated and are reported here as an explicit non-lossless area."));
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("rapid_iteration_parameters.json")), RapidIterationObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> RenderersObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> RenderersJson;
		if (EmitterData)
		{
			const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
			RenderersJson.Reserve(Renderers.Num());
			for (int32 RendererIndex = 0; RendererIndex < Renderers.Num(); ++RendererIndex)
			{
				UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
				if (!Renderer)
				{
					continue;
				}
				TSharedPtr<FJsonObject> RendererObj = UeAgentNiagaraOps::BuildRendererJson(*Renderer, RendererIndex);
				if (!ExportFolderOwnerDetails(
					Renderer,
					EmitterFolder,
					SourceCommand,
					TEXT("niagara_renderer"),
					FString::Printf(TEXT("%s.renderer_%d"), *EmitterFolderRelative, RendererIndex),
					TEXT("renderers"),
					RendererObj,
					OutError))
				{
					return false;
				}
				RenderersJson.Add(MakeShared<FJsonValueObject>(RendererObj));
			}
		}
		RenderersObj->SetArrayField(TEXT("renderers"), RenderersJson);
		RenderersObj->SetNumberField(TEXT("renderer_count"), RenderersJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("renderers.json")), RenderersObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> EventHandlersObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EventHandlersJson;
		if (EmitterData)
		{
			TArray<FNiagaraEventScriptProperties>& EventHandlers = EmitterData->EventHandlerScriptProps;
			EventHandlersJson.Reserve(EventHandlers.Num());
			for (int32 EventIndex = 0; EventIndex < EventHandlers.Num(); ++EventIndex)
			{
				FNiagaraEventScriptProperties& EventHandler = EventHandlers[EventIndex];
				TSharedPtr<FJsonObject> EventObj = UeAgentNiagaraOps::BuildEventHandlerJson(EventHandler, EventIndex);
				if (!ExportFolderPropertyArrayDetails(
					BuildStructPropertiesJson(FNiagaraEventScriptProperties::StaticStruct(), &EventHandler, Emitter),
					EmitterFolder,
					SourceCommand,
					TEXT("niagara_event_handler"),
					FString::Printf(TEXT("%s.event_handler_%d"), *EmitterFolderRelative, EventIndex),
					FString::Printf(TEXT("event_handler_%d"), EventIndex),
					Emitter ? Emitter->GetPathName() : FString(),
					FNiagaraEventScriptProperties::StaticStruct()->GetPathName(),
					TEXT("event_handlers"),
					TEXT("raw_properties"),
					TEXT("raw_properties_count"),
					TEXT("details"),
					EventObj,
					OutError))
				{
					return false;
				}
				AddUniqueScriptReference(ScriptRefs, EventHandler.Script, TEXT("event_handler"), EmitterFolderRelative);
				EventHandlersJson.Add(MakeShared<FJsonValueObject>(EventObj));
			}
		}
		EventHandlersObj->SetArrayField(TEXT("event_handlers"), EventHandlersJson);
		EventHandlersObj->SetNumberField(TEXT("event_handler_count"), EventHandlersJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("event_handlers.json")), EventHandlersObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> EventGeneratorsObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EventGeneratorsJson;
		if (EmitterData)
		{
			auto ResolveOwnerUsage = [](const FString& OwnerScript, ENiagaraScriptUsage& OutUsage) -> bool
			{
				if (OwnerScript.Equals(TEXT("emitter_spawn"), ESearchCase::IgnoreCase))
				{
					OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;
					return true;
				}
				if (OwnerScript.Equals(TEXT("emitter_update"), ESearchCase::IgnoreCase))
				{
					OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;
					return true;
				}
				if (OwnerScript.Equals(TEXT("particle_spawn"), ESearchCase::IgnoreCase))
				{
					OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;
					return true;
				}
				if (OwnerScript.Equals(TEXT("particle_update"), ESearchCase::IgnoreCase))
				{
					OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
					return true;
				}
				return false;
			};

			auto StageContainsModule = [EmitterData, &ResolveOwnerUsage](const FString& OwnerScript, const FString& RequiredModuleName) -> bool
			{
				if (!EmitterData || RequiredModuleName.IsEmpty())
				{
					return true;
				}

				ENiagaraScriptUsage RequiredUsage = ENiagaraScriptUsage::Function;
				if (!ResolveOwnerUsage(OwnerScript, RequiredUsage))
				{
					return true;
				}

				UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
				UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
				if (!Graph)
				{
					return true;
				}

				TArray<UNiagaraNodeOutput*> OutputNodes;
				UeAgentNiagaraOps::GetOutputNodesFromGraph(*Graph, OutputNodes);
				for (UNiagaraNodeOutput* OutputNode : OutputNodes)
				{
					if (!OutputNode || OutputNode->GetUsage() != RequiredUsage)
					{
						continue;
					}

					TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
					UeAgentNiagaraOps::GetStageModuleNodes(*OutputNode, ModuleNodes);
					for (UNiagaraNodeFunctionCall* ModuleNode : ModuleNodes)
					{
						if (!ModuleNode)
						{
							continue;
						}

						const FString ModuleName = ModuleNode->GetFunctionName();
						const FString ModuleScriptPath = UeAgentNiagaraOps::GetModuleScriptPath(*ModuleNode);
						if (ModuleName.Equals(RequiredModuleName, ESearchCase::IgnoreCase) || ModuleScriptPath.Contains(RequiredModuleName, ESearchCase::IgnoreCase))
						{
							return true;
						}
					}
					return false;
				}

				return true;
			};

			auto ShouldSkipOrphanBuiltInGenerator = [&StageContainsModule](const FString& OwnerScript, const FString& GeneratorId) -> bool
			{
				FString RequiredModuleName;
				if (GeneratorId.Equals(TEXT("CollisionEvent"), ESearchCase::IgnoreCase))
				{
					RequiredModuleName = TEXT("GenerateCollisionEvent");
				}
				else if (GeneratorId.Equals(TEXT("DeathEvent"), ESearchCase::IgnoreCase))
				{
					RequiredModuleName = TEXT("GenerateDeathEvent");
				}

				return !RequiredModuleName.IsEmpty() && !StageContainsModule(OwnerScript, RequiredModuleName);
			};

			auto AppendEventGenerators = [
				&EventGeneratorsJson,
				Emitter,
				&ShouldSkipOrphanBuiltInGenerator,
				&EmitterFolder,
				&EmitterFolderRelative,
				&SourceCommand,
				&OutError](const FString& OwnerScript, const TArray<FNiagaraEventGeneratorProperties>& EventGenerators) -> bool
			{
				for (int32 GeneratorIndex = 0; GeneratorIndex < EventGenerators.Num(); ++GeneratorIndex)
				{
					FNiagaraEventGeneratorProperties& Generator = const_cast<FNiagaraEventGeneratorProperties&>(EventGenerators[GeneratorIndex]);
					if (ShouldSkipOrphanBuiltInGenerator(OwnerScript, Generator.ID.ToString()))
					{
						continue;
					}

					TSharedPtr<FJsonObject> GeneratorObj = MakeShared<FJsonObject>();
					GeneratorObj->SetStringField(TEXT("owner_script"), OwnerScript);
					GeneratorObj->SetNumberField(TEXT("generator_index"), GeneratorIndex);
					GeneratorObj->SetStringField(TEXT("id"), Generator.ID.ToString());
					GeneratorObj->SetNumberField(TEXT("max_events_per_frame"), Generator.MaxEventsPerFrame);
					if (!ExportFolderPropertyArrayDetails(
						BuildStructPropertiesJson(FNiagaraEventGeneratorProperties::StaticStruct(), &Generator, Emitter),
						EmitterFolder,
						SourceCommand,
						TEXT("niagara_event_generator"),
						FString::Printf(TEXT("%s.%s.generator_%d"), *EmitterFolderRelative, *OwnerScript, GeneratorIndex),
						Generator.ID.ToString(),
						Emitter ? Emitter->GetPathName() : FString(),
						FNiagaraEventGeneratorProperties::StaticStruct()->GetPathName(),
						TEXT("event_generators"),
						TEXT("raw_properties"),
						TEXT("raw_properties_count"),
						TEXT("details"),
						GeneratorObj,
						OutError))
					{
						return false;
					}
					EventGeneratorsJson.Add(MakeShared<FJsonValueObject>(GeneratorObj));
				}
				return true;
			};

			if (!AppendEventGenerators(TEXT("emitter_spawn"), EmitterData->EmitterSpawnScriptProps.EventGenerators)
				|| !AppendEventGenerators(TEXT("emitter_update"), EmitterData->EmitterUpdateScriptProps.EventGenerators)
				|| !AppendEventGenerators(TEXT("particle_spawn"), EmitterData->SpawnScriptProps.EventGenerators)
				|| !AppendEventGenerators(TEXT("particle_update"), EmitterData->UpdateScriptProps.EventGenerators))
			{
				return false;
			}
		}
		EventGeneratorsObj->SetArrayField(TEXT("event_generators"), EventGeneratorsJson);
		EventGeneratorsObj->SetNumberField(TEXT("event_generator_count"), EventGeneratorsJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("event_generators.json")), EventGeneratorsObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> DataInterfacesObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> DataInterfacesJson;
		if (EmitterData)
		{
			if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource))
			{
				if (UNiagaraGraph* Graph = ScriptSource->NodeGraph)
				{
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
						if (!InputNode)
						{
							continue;
						}
						FProperty* DataInterfaceProperty = nullptr;
						void* DataInterfaceValuePtr = nullptr;
						if (!UeAgentNiagaraOps::ResolvePropertyPath(InputNode, TEXT("DataInterface"), DataInterfaceProperty, DataInterfaceValuePtr))
						{
							continue;
						}
						FObjectPropertyBase* DataInterfaceObjectProperty = CastField<FObjectPropertyBase>(DataInterfaceProperty);
						UNiagaraDataInterface* DataInterface = DataInterfaceObjectProperty ? Cast<UNiagaraDataInterface>(DataInterfaceObjectProperty->GetObjectPropertyValue(DataInterfaceValuePtr)) : nullptr;
						if (!DataInterface)
						{
							continue;
						}

						TSharedPtr<FJsonObject> DataInterfaceObj = MakeShared<FJsonObject>();
						DataInterfaceObj->SetStringField(TEXT("node_guid"), InputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
						DataInterfaceObj->SetStringField(TEXT("input_name"), InputNode->Input.GetName().ToString());
						DataInterfaceObj->SetStringField(TEXT("input_type"), InputNode->Input.GetType().GetNameText().ToString());
						DataInterfaceObj->SetStringField(TEXT("data_interface_class"), DataInterface->GetClass()->GetPathName());
						DataInterfaceObj->SetStringField(TEXT("data_interface_object_path"), DataInterface->GetPathName());
						if (!ExportFolderOwnerDetails(
							DataInterface,
							EmitterFolder,
							SourceCommand,
							TEXT("niagara_data_interface"),
							FString::Printf(TEXT("%s.%s"), *EmitterFolderRelative, *InputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)),
							TEXT("data_interfaces"),
							DataInterfaceObj,
							OutError))
						{
							return false;
						}
						DataInterfacesJson.Add(MakeShared<FJsonValueObject>(DataInterfaceObj));
					}
				}
			}
		}
		DataInterfacesObj->SetArrayField(TEXT("data_interfaces"), DataInterfacesJson);
		DataInterfacesObj->SetNumberField(TEXT("data_interface_count"), DataInterfacesJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("data_interfaces.json")), DataInterfacesObj, &WrittenFiles))
		{
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> StagesIndexJson;
		if (EmitterData)
		{
			UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
			UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
			if (Graph)
			{
				TArray<UNiagaraNodeOutput*> OutputNodes;
				UeAgentNiagaraOps::GetOutputNodesFromGraph(*Graph, OutputNodes);
				for (int32 StageIndex = 0; StageIndex < OutputNodes.Num(); ++StageIndex)
				{
					UNiagaraNodeOutput* OutputNode = OutputNodes[StageIndex];
					if (!OutputNode)
					{
						continue;
					}

					const FString UsageText = UeAgentNiagaraOps::ScriptUsageToString(OutputNode->GetUsage());
					const FString FileName = MakeStageFileName(StageIndex, UsageText, OutputNode->GetUsageId());
					TSharedPtr<FJsonObject> StageObj = BuildStageFolderJson(
						*OutputNode,
						StageIndex,
						EmitterData,
						ScriptRefs,
						FPaths::Combine(EmitterFolder, TEXT("stages")),
						SourceCommand,
						OutError);
					if (!StageObj.IsValid())
					{
						return false;
					}
					StageObj->SetStringField(TEXT("file"), FileName);
					if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("stages"), FileName), StageObj, &WrittenFiles))
					{
						return false;
					}

					TSharedPtr<FJsonObject> StageIndexObj = MakeShared<FJsonObject>();
					StageIndexObj->SetNumberField(TEXT("stage_index"), StageIndex);
					StageIndexObj->SetStringField(TEXT("stage_key"), StageObj->GetStringField(TEXT("stage_key")));
					StageIndexObj->SetStringField(TEXT("script_usage"), UsageText);
					StageIndexObj->SetStringField(TEXT("script_usage_id"), OutputNode->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
					StageIndexObj->SetStringField(TEXT("file"), FileName);
					StagesIndexJson.Add(MakeShared<FJsonValueObject>(StageIndexObj));
				}
			}
			else
			{
				AddWarning(Warnings, TEXT("emitter_missing_graph_source"), EmitterFolderRelative, TEXT("Emitter graph source is missing; stages cannot be exported structurally."));
			}
		}

		TSharedPtr<FJsonObject> StagesIndexObj = MakeShared<FJsonObject>();
		StagesIndexObj->SetArrayField(TEXT("stages"), StagesIndexJson);
		StagesIndexObj->SetNumberField(TEXT("stage_count"), StagesIndexJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("stages"), TEXT("index.json")), StagesIndexObj, &WrittenFiles))
		{
			return false;
		}

		if (EmitterData)
		{
			TArray<UNiagaraScript*> EmitterScripts;
			EmitterData->GetScripts(EmitterScripts, false, false);
			for (UNiagaraScript* Script : EmitterScripts)
			{
				AddUniqueScriptReference(ScriptRefs, Script, TEXT("emitter"), EmitterFolderRelative);
			}
		}

		return true;
	}

	static TSharedPtr<FJsonObject> BuildSystemStageJson(
		UNiagaraScript* Script,
		const FString& StageName,
		TMap<FString, TSharedPtr<FJsonObject>>& ScriptRefs,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		StageObj->SetStringField(TEXT("stage_name"), StageName);
		StageObj->SetStringField(TEXT("stage_type"), TEXT("system_stage"));
		if (Script)
		{
			StageObj->SetObjectField(TEXT("script"), BuildScriptReferenceJson(Script, TEXT("system_stage"), StageName));
			UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
			UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
			if (Graph)
			{
				TArray<UNiagaraNodeOutput*> OutputNodes;
				UeAgentNiagaraOps::GetOutputNodesFromGraph(*Graph, OutputNodes);
				for (int32 OutputIndex = 0; OutputIndex < OutputNodes.Num(); ++OutputIndex)
				{
					UNiagaraNodeOutput* OutputNode = OutputNodes[OutputIndex];
					if (OutputNode && OutputNode->GetUsage() == Script->GetUsage())
					{
						StageObj = BuildStageFolderJson(*OutputNode, OutputIndex, nullptr, ScriptRefs, EntryDirectory, SourceCommand, OutError);
						if (!StageObj.IsValid())
						{
							return nullptr;
						}
						StageObj->SetStringField(TEXT("stage_name"), StageName);
						StageObj->SetStringField(TEXT("stage_type"), TEXT("system_stage"));
						StageObj->SetObjectField(TEXT("script"), BuildScriptReferenceJson(Script, TEXT("system_stage"), StageName));
						break;
					}
				}
			}
		}
		else
		{
			StageObj->SetObjectField(TEXT("script"), MakeShared<FJsonObject>());
		}
		return StageObj;
	}

	static int32 ApplyObjectPropertyArray(UObject* Target, const TSharedPtr<FJsonObject>& SourceObj, const FString& FieldName, TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& ContextPath)
	{
		if (!Target || !SourceObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!TryGetArrayField(SourceObj, *FieldName, Properties))
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& PropertyObj = *PropertyObjPtr;
			bool bCanApply = true;
			PropertyObj->TryGetBoolField(TEXT("can_apply_generic"), bCanApply);
			if (!bCanApply)
			{
				continue;
			}

			const FString PropertyName = GetStringFieldAny(PropertyObj, { TEXT("property_name"), TEXT("path") });
			const FString ValueText = GetStringFieldAny(PropertyObj, { TEXT("value_text") });
			if (PropertyName.IsEmpty())
			{
				AddWarning(Warnings, TEXT("object_property_missing_name"), ContextPath, TEXT("Skipped UObject property entry with no property_name/path."));
				continue;
			}
			if (Target->IsA<UEdGraphNode>() && PropertyName.Equals(TEXT("NodeGuid"), ESearchCase::CaseSensitive))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* CurveJsonObjPtr = nullptr;
			FString CurveJsonFieldName;
			if (PropertyObj->TryGetObjectField(TEXT("curve_json"), CurveJsonObjPtr) && CurveJsonObjPtr && CurveJsonObjPtr->IsValid())
			{
				CurveJsonFieldName = TEXT("curve_json");
			}
			else if (PropertyObj->TryGetObjectField(TEXT("value_json"), CurveJsonObjPtr) && CurveJsonObjPtr && CurveJsonObjPtr->IsValid())
			{
				CurveJsonFieldName = TEXT("value_json");
			}
			if (!CurveJsonFieldName.IsEmpty())
			{
				FProperty* Property = nullptr;
				void* ValuePtr = nullptr;
				if (!UeAgentNiagaraOps::ResolvePropertyPath(Target, PropertyName, Property, ValuePtr))
				{
					UeAgentCurveJson::AddCurveIssue(
						Warnings,
						TEXT("error"),
						TEXT("object_property_curve_path_not_found"),
						ContextPath + TEXT(".") + PropertyName,
						TEXT("Skipped curve_json because the target UObject property could not be resolved."));
					continue;
				}

				if (UeAgentCurveJson::TryApplyPropertyCurveJson(Property, ValuePtr, *CurveJsonObjPtr, ContextPath + TEXT(".") + PropertyName + TEXT(".") + CurveJsonFieldName, Warnings))
				{
					++Applied;
				}
				continue;
			}

			if (!PropertyObj->HasField(TEXT("value_text")))
			{
				AddWarning(Warnings, TEXT("object_property_missing_value_text"), ContextPath + TEXT(".") + PropertyName, TEXT("Skipped UObject property entry with no value_text."));
				continue;
			}

			FString AppliedValueText;
			FString CppType;
			FString ImportError;
			if (UeAgentNiagaraOps::SetObjectPropertyText(Target, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
			{
				++Applied;
				if (!AppliedValueText.Equals(ValueText, ESearchCase::CaseSensitive))
				{
					AddWarning(
						Warnings,
						TEXT("object_property_value_changed_after_import"),
						ContextPath + TEXT(".") + PropertyName,
						FString::Printf(TEXT("UObject property import read back a different value. requested='%s' applied='%s' cpp_type='%s'."), *ValueText, *AppliedValueText, *CppType));
				}
			}
			else
			{
				AddWarning(
					Warnings,
					TEXT("object_property_apply_failed"),
					ContextPath + TEXT(".") + PropertyName,
					FString::Printf(TEXT("ImportText failed for UObject property. requested='%s' cpp_type='%s' error='%s'."), *ValueText, *CppType, *ImportError));
			}
		}
		return Applied;
	}

	static bool ShouldApplyStructFolderProperty(const UStruct* StructType, const FString& FieldName, const FString& PropertyName)
	{
		const bool bIsFolderPropertiesField =
			FieldName.Equals(TEXT("raw_properties"), ESearchCase::CaseSensitive)
			|| FieldName.Equals(TEXT("properties"), ESearchCase::CaseSensitive);
		if (StructType != FVersionedNiagaraEmitterData::StaticStruct() || !bIsFolderPropertiesField)
		{
			return true;
		}

		static const TSet<FString> RebuiltEmitterDataProperties = {
			TEXT("EventHandlerScriptProps"),
			TEXT("UpdateScriptProps"),
			TEXT("SpawnScriptProps"),
			TEXT("EmitterSpawnScriptProps"),
			TEXT("EmitterUpdateScriptProps"),
			TEXT("GraphSource"),
			TEXT("ScratchPads"),
			TEXT("ParentScratchPads"),
			TEXT("RendererProperties"),
			TEXT("SimulationStages"),
			TEXT("SimStageExecutionLoops"),
			TEXT("SimStageExecutionLoopEditorData"),
			TEXT("GPUComputeScript"),
			TEXT("ResolvedDIBindings"),
			TEXT("SharedEventGeneratorIds"),
			TEXT("EditorData"),
			TEXT("EditorParameters")
		};

		return !RebuiltEmitterDataProperties.Contains(PropertyName);
	}

	static int32 ApplyStructPropertyArray(UStruct* StructType, void* StructData, UObject* OwnerForImport, const TSharedPtr<FJsonObject>& SourceObj, const FString& FieldName, TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& ContextPath)
	{
		if (!StructType || !StructData || !SourceObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!TryGetArrayField(SourceObj, *FieldName, Properties))
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& PropertyObj = *PropertyObjPtr;
			bool bCanApply = true;
			PropertyObj->TryGetBoolField(TEXT("can_apply_generic"), bCanApply);
			if (!bCanApply)
			{
				continue;
			}

			const FString PropertyName = GetStringFieldAny(PropertyObj, { TEXT("property_name"), TEXT("path") });
			const FString ValueText = GetStringFieldAny(PropertyObj, { TEXT("value_text") });
			if (PropertyName.IsEmpty())
			{
				AddWarning(Warnings, TEXT("struct_property_missing_name"), ContextPath, TEXT("Skipped UStruct property entry with no property_name/path."));
				continue;
			}
			if (!ShouldApplyStructFolderProperty(StructType, FieldName, PropertyName))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* CurveJsonObjPtr = nullptr;
			FString CurveJsonFieldName;
			if (PropertyObj->TryGetObjectField(TEXT("curve_json"), CurveJsonObjPtr) && CurveJsonObjPtr && CurveJsonObjPtr->IsValid())
			{
				CurveJsonFieldName = TEXT("curve_json");
			}
			else if (PropertyObj->TryGetObjectField(TEXT("value_json"), CurveJsonObjPtr) && CurveJsonObjPtr && CurveJsonObjPtr->IsValid())
			{
				CurveJsonFieldName = TEXT("value_json");
			}
			if (!CurveJsonFieldName.IsEmpty())
			{
				FProperty* Property = nullptr;
				void* ValuePtr = nullptr;
				if (!UeAgentNiagaraOps::ResolveStructPropertyPath(StructType, StructData, PropertyName, Property, ValuePtr))
				{
					UeAgentCurveJson::AddCurveIssue(
						Warnings,
						TEXT("error"),
						TEXT("struct_property_curve_path_not_found"),
						ContextPath + TEXT(".") + PropertyName,
						TEXT("Skipped curve_json because the target UStruct property could not be resolved."));
					continue;
				}

				if (UeAgentCurveJson::TryApplyPropertyCurveJson(Property, ValuePtr, *CurveJsonObjPtr, ContextPath + TEXT(".") + PropertyName + TEXT(".") + CurveJsonFieldName, Warnings))
				{
					++Applied;
				}
				continue;
			}

			if (!PropertyObj->HasField(TEXT("value_text")))
			{
				AddWarning(Warnings, TEXT("struct_property_missing_value_text"), ContextPath + TEXT(".") + PropertyName, TEXT("Skipped UStruct property entry with no value_text."));
				continue;
			}

			FString AppliedValueText;
			FString CppType;
			FString ImportError;
			if (UeAgentNiagaraOps::SetStructPropertyText(StructType, StructData, OwnerForImport, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
			{
				++Applied;
				if (!AppliedValueText.Equals(ValueText, ESearchCase::CaseSensitive))
				{
					AddWarning(
						Warnings,
						TEXT("struct_property_value_changed_after_import"),
						ContextPath + TEXT(".") + PropertyName,
						FString::Printf(TEXT("UStruct property import read back a different value. requested='%s' applied='%s' cpp_type='%s'."), *ValueText, *AppliedValueText, *CppType));
				}
			}
			else
			{
				AddWarning(
					Warnings,
					TEXT("struct_property_apply_failed"),
					ContextPath + TEXT(".") + PropertyName,
					FString::Printf(TEXT("ImportText failed for UStruct property. requested='%s' cpp_type='%s' error='%s'."), *ValueText, *CppType, *ImportError));
			}
		}
		return Applied;
	}

	static UeAgentDetailsAuthoring::FOwnerInfo MakeApplyOwnerInfo(
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& SourceCommand,
		UObject* Object = nullptr)
	{
		return UeAgentDetailsAuthoring::MakeOwnerInfoForObject(
			Object,
			OwnerKind,
			OwnerId,
			SourceCommand);
	}

	static int32 GetGroupedDetailsAppliedCount(const TSharedPtr<FJsonObject>& ApplySummary)
	{
		if (!ApplySummary.IsValid())
		{
			return 0;
		}
		double Applied = 0.0;
		return ApplySummary->TryGetNumberField(TEXT("properties_applied"), Applied)
			? static_cast<int32>(Applied)
			: 0;
	}

	static bool LoadGroupedDetailsForRef(
		const FString& EntryDirectory,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& RefFieldName,
		bool& bOutHasDetails,
		TSharedPtr<FJsonObject>& OutPropertiesJson,
		FString& OutError)
	{
		bOutHasDetails = false;
		OutPropertiesJson.Reset();
		FString DetailsRoot;
		FString IndexFile;
		if (!UeAgentDetailsAuthoring::ResolveDetailsRefRoot(
			EntryDirectory,
			EntryObject,
			RefFieldName,
			bOutHasDetails,
			DetailsRoot,
			IndexFile,
			OutError))
		{
			return false;
		}
		if (!bOutHasDetails)
		{
			return true;
		}
		return UeAgentDetailsAuthoring::LoadGroupedDetails(DetailsRoot, OutPropertiesJson, OutError);
	}

	static bool ApplyFolderOwnerDetailsIfPresent(
		UObject* Object,
		const FString& EntryDirectory,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& SourceCommand,
		FString& OutError,
		int32& InOutAppliedCount)
	{
		bool bHasDetails = false;
		FString DetailsRoot;
		FString IndexFile;
		if (!UeAgentDetailsAuthoring::ResolveDetailsRefRoot(
			EntryDirectory,
			EntryObject,
			TEXT("details"),
			bHasDetails,
			DetailsRoot,
			IndexFile,
			OutError))
		{
			return false;
		}
		if (!bHasDetails)
		{
			return true;
		}
		if (!Object)
		{
			OutError = TEXT("details_owner_missing");
			return false;
		}

		UeAgentDetailsAuthoring::FOwnerDetailsApplyRequest Request;
		Request.Object = Object;
		Request.OwnerInfo = MakeApplyOwnerInfo(OwnerKind, OwnerId, SourceCommand, Object);
		Request.DetailsRoot = DetailsRoot;
		Request.IndexFile = IndexFile;
		Request.RefFieldName = TEXT("details");

		TSharedPtr<FJsonObject> ApplySummary;
		if (!UeAgentDetailsAuthoring::ApplyOwnerDetails(Request, ApplySummary, OutError))
		{
			return false;
		}
		bool bOk = true;
		if (ApplySummary.IsValid() && ApplySummary->HasField(TEXT("ok")))
		{
			bOk = ApplySummary->GetBoolField(TEXT("ok"));
		}
		if (!bOk)
		{
			OutError = TEXT("grouped_details_apply_failed");
			return false;
		}

		InOutAppliedCount += GetGroupedDetailsAppliedCount(ApplySummary);
		return true;
	}

	static int32 ApplyStructDetailsIfPresent(
		UStruct* StructType,
		void* StructData,
		UObject* OwnerForImport,
		const FString& EntryDirectory,
		const TSharedPtr<FJsonObject>& EntryObject,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		const FString& ContextPath,
		FString& OutError)
	{
		bool bHasDetails = false;
		TSharedPtr<FJsonObject> PropertiesJson;
		if (!LoadGroupedDetailsForRef(EntryDirectory, EntryObject, TEXT("details"), bHasDetails, PropertiesJson, OutError))
		{
			return INDEX_NONE;
		}
		if (!bHasDetails || !PropertiesJson.IsValid())
		{
			return 0;
		}
		return ApplyStructPropertyArray(StructType, StructData, OwnerForImport, PropertiesJson, TEXT("properties"), Warnings, ContextPath);
	}

	static bool JsonPropertyArrayContainsProperty(const TSharedPtr<FJsonObject>& SourceObj, const FString& FieldName, const FString& PropertyName)
	{
		if (!SourceObj.IsValid() || PropertyName.IsEmpty())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!TryGetArrayField(SourceObj, *FieldName, Properties))
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
			{
				continue;
			}

			bool bCanApply = true;
			(*PropertyObjPtr)->TryGetBoolField(TEXT("can_apply_generic"), bCanApply);
			if (!bCanApply)
			{
				continue;
			}

			const FString CandidateName = GetStringFieldAny(*PropertyObjPtr, { TEXT("property_name"), TEXT("path") });
			if (CandidateName.Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	static FString OwnerScriptToStageScriptUsage(const FString& OwnerScript)
	{
		if (OwnerScript.Equals(TEXT("emitter_spawn"), ESearchCase::IgnoreCase))
		{
			return TEXT("EmitterSpawnScript");
		}
		if (OwnerScript.Equals(TEXT("emitter_update"), ESearchCase::IgnoreCase))
		{
			return TEXT("EmitterUpdateScript");
		}
		if (OwnerScript.Equals(TEXT("particle_spawn"), ESearchCase::IgnoreCase))
		{
			return TEXT("ParticleSpawnScript");
		}
		if (OwnerScript.Equals(TEXT("particle_update"), ESearchCase::IgnoreCase))
		{
			return TEXT("ParticleUpdateScript");
		}
		return FString();
	}

	static bool FolderStageContainsModule(const FString& EmitterFolder, const FString& OwnerScript, const FString& ModuleName)
	{
		const FString StageUsage = OwnerScriptToStageScriptUsage(OwnerScript);
		if (StageUsage.IsEmpty() || ModuleName.IsEmpty())
		{
			return true;
		}

		TSharedPtr<FJsonObject> StagesIndexObj;
		FString LoadError;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("stages"), TEXT("index.json")), StagesIndexObj, LoadError) || !StagesIndexObj.IsValid())
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
		if (!TryGetArrayField(StagesIndexObj, TEXT("stages"), Stages))
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& StageValue : *Stages)
		{
			const TSharedPtr<FJsonObject>* StageObjPtr = nullptr;
			if (!StageValue.IsValid() || !StageValue->TryGetObject(StageObjPtr) || !StageObjPtr || !StageObjPtr->IsValid())
			{
				continue;
			}

			FString ScriptUsage;
			(*StageObjPtr)->TryGetStringField(TEXT("script_usage"), ScriptUsage);
			if (!ScriptUsage.Equals(StageUsage, ESearchCase::IgnoreCase))
			{
				continue;
			}

			FString StageFile;
			(*StageObjPtr)->TryGetStringField(TEXT("file"), StageFile);
			if (StageFile.IsEmpty())
			{
				return true;
			}

			TSharedPtr<FJsonObject> StageObj;
			if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("stages"), StageFile), StageObj, LoadError) || !StageObj.IsValid())
			{
				return true;
			}

			const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
			if (!TryGetArrayField(StageObj, TEXT("modules"), Modules))
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
			{
				const TSharedPtr<FJsonObject>* ModuleObjPtr = nullptr;
				if (!ModuleValue.IsValid() || !ModuleValue->TryGetObject(ModuleObjPtr) || !ModuleObjPtr || !ModuleObjPtr->IsValid())
				{
					continue;
				}

				FString ModuleNameText;
				FString ModuleScriptPath;
				(*ModuleObjPtr)->TryGetStringField(TEXT("module_name"), ModuleNameText);
				(*ModuleObjPtr)->TryGetStringField(TEXT("module_script_path"), ModuleScriptPath);
				if (ModuleNameText.Equals(ModuleName, ESearchCase::IgnoreCase) || ModuleScriptPath.Contains(ModuleName, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}

			return false;
		}

		return true;
	}

	static bool ShouldSkipBuiltInEventGeneratorWithoutStageModule(const FString& EmitterFolder, const FString& OwnerScript, const FString& GeneratorId, TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		FString RequiredModuleName;
		if (GeneratorId.Equals(TEXT("CollisionEvent"), ESearchCase::IgnoreCase))
		{
			RequiredModuleName = TEXT("GenerateCollisionEvent");
		}
		else if (GeneratorId.Equals(TEXT("DeathEvent"), ESearchCase::IgnoreCase))
		{
			RequiredModuleName = TEXT("GenerateDeathEvent");
		}
		else
		{
			return false;
		}

		if (FolderStageContainsModule(EmitterFolder, OwnerScript, RequiredModuleName))
		{
			return false;
		}

		AddWarning(
			Warnings,
			TEXT("event_generator_skipped_missing_stage_module"),
			EmitterFolder + TEXT(":") + OwnerScript + TEXT(":") + GeneratorId,
			FString::Printf(TEXT("Skipped built-in event generator because the %s stage has no %s module."), *OwnerScript, *RequiredModuleName));
		return true;
	}

	static UNiagaraSystem* CreateNiagaraSystemAsset(const FString& AssetPath, FString& OutError)
	{
		const FString PackageName = UeAgentNiagaraOps::NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_name");
			return nullptr;
		}

		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
		UeAgentNiagaraOps::DetachStaleObjectAtPath(ObjectPath);

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UNiagaraSystem* NiagaraSystem = NewObject<UNiagaraSystem>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!NiagaraSystem)
		{
			OutError = TEXT("create_niagara_system_failed");
			return nullptr;
		}

		UNiagaraSystemFactoryNew::InitializeSystem(NiagaraSystem, false);
		FAssetRegistryModule::AssetCreated(NiagaraSystem);
		NiagaraSystem->MarkPackageDirty();
		Package->MarkPackageDirty();
		return NiagaraSystem;
	}

	static UNiagaraEmitter* CreateNiagaraEmitterAsset(const FString& AssetPath, const bool bAddDefaultModulesAndRenderers, FString& OutError)
	{
		const FString PackageName = UeAgentNiagaraOps::NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_name");
			return nullptr;
		}

		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
		UeAgentNiagaraOps::DetachStaleObjectAtPath(ObjectPath);

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UNiagaraEmitter* NiagaraEmitter = NewObject<UNiagaraEmitter>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!NiagaraEmitter)
		{
			OutError = TEXT("create_niagara_emitter_failed");
			return nullptr;
		}

		UNiagaraEmitterFactoryNew::InitializeEmitter(NiagaraEmitter, bAddDefaultModulesAndRenderers);
		FAssetRegistryModule::AssetCreated(NiagaraEmitter);
		NiagaraEmitter->MarkPackageDirty();
		Package->MarkPackageDirty();
		return NiagaraEmitter;
	}

	static UNiagaraScript* CreateNiagaraScriptAsset(const FString& AssetPath, const ENiagaraScriptUsage Usage, FString& OutError)
	{
		const FString PackageName = UeAgentNiagaraOps::NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_name");
			return nullptr;
		}

		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
		UeAgentNiagaraOps::DetachStaleObjectAtPath(ObjectPath);

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UNiagaraScript* NiagaraScript = NewObject<UNiagaraScript>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!NiagaraScript)
		{
			OutError = TEXT("create_niagara_script_failed");
			return nullptr;
		}

		NiagaraScript->SetUsage(Usage);

		UNiagaraScriptSource* ScriptSource = NewObject<UNiagaraScriptSource>(NiagaraScript, NAME_None, RF_Transactional);
		UNiagaraGraph* Graph = ScriptSource ? NewObject<UNiagaraGraph>(ScriptSource, NAME_None, RF_Transactional) : nullptr;
		if (!ScriptSource || !Graph)
		{
			OutError = TEXT("create_niagara_script_graph_failed");
			return nullptr;
		}
		ScriptSource->NodeGraph = Graph;

		UNiagaraNodeInput* InputNode = NewObject<UNiagaraNodeInput>(Graph, NAME_None, RF_Transactional);
		UNiagaraNodeOutput* OutputNode = NewObject<UNiagaraNodeOutput>(Graph, NAME_None, RF_Transactional);
		if (!InputNode || !OutputNode)
		{
			OutError = TEXT("create_niagara_script_bridge_failed");
			return nullptr;
		}
		InputNode->CreateNewGuid();
		OutputNode->CreateNewGuid();

		if (Usage == ENiagaraScriptUsage::Module)
		{
			InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("MapIn"));
			OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Output")));
		}
		else
		{
			InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Input"));
			OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Output")));
		}
		InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
		OutputNode->SetUsage(Usage);

		InputNode->AllocateDefaultPins();
		OutputNode->AllocateDefaultPins();
		Graph->AddNode(InputNode, true, false);
		Graph->AddNode(OutputNode, true, false);
		if (UEdGraphPin* InputOutputPin = UeAgentNiagaraOps::FindFirstOutputPin(*InputNode))
		{
			if (UEdGraphPin* OutputInputPin = UeAgentNiagaraOps::FindFirstInputPin(*OutputNode))
			{
				if (const UEdGraphSchema* Schema = Graph->GetSchema())
				{
					Schema->TryCreateConnection(InputOutputPin, OutputInputPin);
				}
				if (!InputOutputPin->LinkedTo.Contains(OutputInputPin))
				{
					InputOutputPin->MakeLinkTo(OutputInputPin);
				}
			}
		}
		NiagaraScript->SetLatestSource(ScriptSource);
		NiagaraScript->RequestCompile(FGuid());
		FAssetRegistryModule::AssetCreated(NiagaraScript);
		NiagaraScript->MarkPackageDirty();
		Package->MarkPackageDirty();
		return NiagaraScript;
	}

	static bool FindUserParameterByName(const FNiagaraUserRedirectionParameterStore& Store, const FString& ParameterName, FNiagaraVariable& OutParameter)
	{
		return UeAgentNiagaraOps::FindUserParameterByName(Store, ParameterName, OutParameter);
	}

	static int32 ApplyUserParameters(UNiagaraSystem& NiagaraSystem, const FString& FolderPath, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		TSharedPtr<FJsonObject> UserParamsObj;
		if (!LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("parameters"), TEXT("user_parameters.json")), UserParamsObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!UserParamsObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
		if (!TryGetArrayField(UserParamsObj, TEXT("parameters"), Parameters))
		{
			return 0;
		}

		int32 Applied = 0;
		FNiagaraUserRedirectionParameterStore& Store = NiagaraSystem.GetExposedParameters();
		for (const TSharedPtr<FJsonValue>& ParameterValue : *Parameters)
		{
			const TSharedPtr<FJsonObject>* ParameterObjPtr = nullptr;
			if (!ParameterValue.IsValid() || !ParameterValue->TryGetObject(ParameterObjPtr) || !ParameterObjPtr || !ParameterObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& ParameterObj = *ParameterObjPtr;
			const FString ParameterName = GetStringFieldAny(ParameterObj, { TEXT("name"), TEXT("parameter_name") });
			TArray<FString> TypeCandidates;
			auto AddTypeCandidate = [&TypeCandidates](FString TypeCandidate)
			{
				TypeCandidate.TrimStartAndEndInline();
				if (!TypeCandidate.IsEmpty())
				{
					TypeCandidates.AddUnique(TypeCandidate);
				}
			};
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("parameter_type") }));
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type_path") }));
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type_internal") }));
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type") }));
			if (ParameterName.IsEmpty() || TypeCandidates.IsEmpty())
			{
				AddWarning(Warnings, TEXT("user_parameter_missing_name_or_type"), TEXT("parameters/user_parameters.json"), TEXT("Parameter entry has no name or type."));
				continue;
			}

			FNiagaraVariable Parameter;
			if (!FindUserParameterByName(Store, ParameterName, Parameter))
			{
				FNiagaraTypeDefinition TypeDefinition;
				FString ResolvedFromTypeText;
				for (const FString& TypeCandidate : TypeCandidates)
				{
					if (UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(TypeCandidate, TypeDefinition))
					{
						ResolvedFromTypeText = TypeCandidate;
						break;
					}
				}
				if (ResolvedFromTypeText.IsEmpty())
				{
					AddWarning(Warnings, TEXT("user_parameter_type_unresolved"), ParameterName, FString::Join(TypeCandidates, TEXT(" | ")));
					continue;
				}

				FNiagaraVariable NewParameter(TypeDefinition, *ParameterName);
				FNiagaraUserRedirectionParameterStore::MakeUserVariable(NewParameter);
				if (!Store.AddParameter(NewParameter, true, true))
				{
					AddWarning(Warnings, TEXT("user_parameter_add_failed"), ParameterName, TEXT("Store.AddParameter returned false."));
					continue;
				}
				Parameter = NewParameter;
			}

			bool bHasValue = ParameterObj->HasField(TEXT("value_text"));
			ParameterObj->TryGetBoolField(TEXT("has_value"), bHasValue);
			if (bHasValue && ParameterObj->HasField(TEXT("value_text")))
			{
				const FString ValueText = ParameterObj->GetStringField(TEXT("value_text"));
				if (!UeAgentNiagaraOps::SetUserParameterValueFromText(Store, Parameter, ValueText, OutError))
				{
					AddWarning(Warnings, TEXT("user_parameter_value_apply_failed"), ParameterName, OutError);
					OutError.Reset();
					continue;
				}
			}
			++Applied;
		}

		return Applied;
	}

	static UNiagaraEmitter* CreateTemplateEmitterForFolderApply(UNiagaraSystem& NiagaraSystem, const FString& EmitterName)
	{
		const FName TemplateName = MakeUniqueObjectName(&NiagaraSystem, UNiagaraEmitter::StaticClass(), *FString::Printf(TEXT("%s_Template"), *MakeSafeFileName(EmitterName)));
		UNiagaraEmitter* TemplateEmitter = NewObject<UNiagaraEmitter>(&NiagaraSystem, TemplateName, RF_Transactional);
		if (TemplateEmitter)
		{
			UNiagaraEmitterFactoryNew::InitializeEmitter(TemplateEmitter, true);
		}
		return TemplateEmitter;
	}

	static int32 ApplyEmitterGraphParameters(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("emitter_parameters_missing_graph"), EmitterFolder, TEXT("Emitter graph source is missing; parameters cannot be applied."));
			return 0;
		}

		TSharedPtr<FJsonObject> ParametersObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("parameters.json")), ParametersObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!ParametersObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
		if (!TryGetArrayField(ParametersObj, TEXT("parameters"), Parameters))
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& ParameterValue : *Parameters)
		{
			const TSharedPtr<FJsonObject>* ParameterObjPtr = nullptr;
			if (!ParameterValue.IsValid() || !ParameterValue->TryGetObject(ParameterObjPtr) || !ParameterObjPtr || !ParameterObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& ParameterObj = *ParameterObjPtr;
			const FString ParameterName = GetStringFieldAny(ParameterObj, { TEXT("name"), TEXT("parameter_name") });
			TArray<FString> TypeCandidates;
			auto AddTypeCandidate = [&TypeCandidates](const FString& TypeCandidate)
			{
				const FString Trimmed = TypeCandidate.TrimStartAndEnd();
				if (!Trimmed.IsEmpty())
				{
					TypeCandidates.AddUnique(Trimmed);
				}
			};
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type_path") }));
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type_internal") }));
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type") }));
			if (ParameterName.IsEmpty() || TypeCandidates.IsEmpty())
			{
				continue;
			}

			FNiagaraVariable ExistingParameter;
			UNiagaraScriptVariable* ScriptVariable = nullptr;
			if (!UeAgentNiagaraOps::FindGraphParameterByName(*Graph, ParameterName, ExistingParameter, ScriptVariable))
			{
				FNiagaraTypeDefinition TypeDefinition;
				for (const FString& TypeCandidate : TypeCandidates)
				{
					if (UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(TypeCandidate, TypeDefinition))
					{
						break;
					}
				}
				if (!TypeDefinition.IsValid())
				{
					AddWarning(Warnings, TEXT("emitter_parameter_type_unresolved"), ParameterName, FString::Join(TypeCandidates, TEXT(" | ")));
					continue;
				}

				bool bIsStaticSwitch = false;
				ParameterObj->TryGetBoolField(TEXT("is_static_switch"), bIsStaticSwitch);
				ExistingParameter = FNiagaraVariable(TypeDefinition, *ParameterName);
				ScriptVariable = UeAgentNiagaraOps::AddGraphParameterMetadata(*Graph, ExistingParameter, bIsStaticSwitch);
			}

			if (!ScriptVariable)
			{
				AddWarning(Warnings, TEXT("emitter_parameter_apply_failed"), ParameterName, TEXT("Could not resolve or create UNiagaraScriptVariable metadata."));
				continue;
			}

			if (ParameterObj->HasField(TEXT("default_value_text")))
			{
				const FString DefaultValueText = ParameterObj->GetStringField(TEXT("default_value_text"));
				if (!UeAgentNiagaraOps::SetScriptVariableDefaultValueFromText(*ScriptVariable, DefaultValueText, OutError))
				{
					AddWarning(Warnings, TEXT("emitter_parameter_default_apply_failed"), ParameterName, OutError);
					OutError.Reset();
					continue;
				}
			}
			++Applied;
		}

		return Applied;
	}

	static bool EnsureEventHandlerForFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& EventObj, FNiagaraEventScriptProperties*& OutEventHandler, int32& OutEventIndex, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		OutEventHandler = nullptr;
		OutEventIndex = INDEX_NONE;

		FString UsageIdText;
		EventObj->TryGetStringField(TEXT("script_usage_id"), UsageIdText);
		FGuid UsageId;
		FGuid::Parse(UsageIdText, UsageId);
		if (!UsageId.IsValid())
		{
			UsageId = FGuid::NewGuid();
		}

		for (int32 Index = 0; Index < EmitterData.EventHandlerScriptProps.Num(); ++Index)
		{
			FNiagaraEventScriptProperties& Existing = EmitterData.EventHandlerScriptProps[Index];
			if (Existing.Script && Existing.Script->GetUsageId() == UsageId)
			{
				OutEventHandler = &Existing;
				OutEventIndex = Index;
				return true;
			}
		}

		FNiagaraEventScriptProperties NewEventHandler;
		FString SourceEventName = TEXT("Event");
		EventObj->TryGetStringField(TEXT("source_event_name"), SourceEventName);
		SourceEventName.TrimStartAndEndInline();
		if (SourceEventName.IsEmpty())
		{
			SourceEventName = TEXT("Event");
		}
		NewEventHandler.SourceEventName = FName(*SourceEventName);

		FString SourceEmitterIdText;
		if (EventObj->TryGetStringField(TEXT("source_emitter_id"), SourceEmitterIdText))
		{
			FGuid::Parse(SourceEmitterIdText, NewEventHandler.SourceEmitterID);
		}

		FString ExecutionModeText;
		if (EventObj->TryGetStringField(TEXT("execution_mode"), ExecutionModeText) && !ExecutionModeText.IsEmpty())
		{
			EScriptExecutionMode ParsedExecutionMode = EScriptExecutionMode::EveryParticle;
			if (UeAgentNiagaraOps::ParseExecutionMode(ExecutionModeText, ParsedExecutionMode))
			{
				NewEventHandler.ExecutionMode = ParsedExecutionMode;
			}
		}

		double NumberValue = 0.0;
		if (EventObj->TryGetNumberField(TEXT("spawn_number"), NumberValue))
		{
			NewEventHandler.SpawnNumber = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
		}
		if (EventObj->TryGetNumberField(TEXT("max_events_per_frame"), NumberValue))
		{
			NewEventHandler.MaxEventsPerFrame = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
		}
		if (EventObj->TryGetNumberField(TEXT("min_spawn_number"), NumberValue))
		{
			NewEventHandler.MinSpawnNumber = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
		}
		EventObj->TryGetBoolField(TEXT("random_spawn_number"), NewEventHandler.bRandomSpawnNumber);
		EventObj->TryGetBoolField(TEXT("update_attribute_initial_values"), NewEventHandler.UpdateAttributeInitialValues);

		NewEventHandler.Script = NewObject<UNiagaraScript>(&Emitter, MakeUniqueObjectName(&Emitter, UNiagaraScript::StaticClass(), TEXT("EventHandler")), RF_Transactional);
		if (!NewEventHandler.Script)
		{
			OutError = TEXT("create_event_handler_script_failed");
			return false;
		}
		NewEventHandler.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
		NewEventHandler.Script->SetUsageId(UsageId);
		if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource))
		{
			NewEventHandler.Script->SetLatestSource(ScriptSource);
		}

		Emitter.AddEventHandler(NewEventHandler, EmitterData.Version.VersionGuid);
		UNiagaraNodeOutput* OutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(Graph, ENiagaraScriptUsage::ParticleEventScript, UsageId);
		if (!OutputNode)
		{
			AddWarning(Warnings, TEXT("event_handler_output_bridge_failed"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower), TEXT("Event handler was created but output bridge could not be initialized."));
		}

		for (int32 Index = 0; Index < EmitterData.EventHandlerScriptProps.Num(); ++Index)
		{
			FNiagaraEventScriptProperties& Existing = EmitterData.EventHandlerScriptProps[Index];
			if (Existing.Script && Existing.Script->GetUsageId() == UsageId)
			{
				OutEventHandler = &Existing;
				OutEventIndex = Index;
				return true;
			}
		}

		OutError = TEXT("event_handler_added_but_not_resolved");
		return false;
	}

	static bool LoadDynamicStageUsageIdsFromFolder(const FString& EmitterFolder, bool& bOutHasStageIndex, TSet<FGuid>& OutEventUsageIds, TSet<FGuid>& OutSimulationStageUsageIds, FString& OutError)
	{
		bOutHasStageIndex = false;
		OutEventUsageIds.Reset();
		OutSimulationStageUsageIds.Reset();

		TSharedPtr<FJsonObject> StagesIndexObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("stages"), TEXT("index.json")), StagesIndexObj, OutError))
		{
			return false;
		}
		if (!StagesIndexObj.IsValid())
		{
			return true;
		}

		bOutHasStageIndex = true;
		const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
		if (!TryGetArrayField(StagesIndexObj, TEXT("stages"), Stages))
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& StageValue : *Stages)
		{
			const TSharedPtr<FJsonObject>* StageObjPtr = nullptr;
			if (!StageValue.IsValid() || !StageValue->TryGetObject(StageObjPtr) || !StageObjPtr || !StageObjPtr->IsValid())
			{
				continue;
			}

			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
			FGuid UsageId;
			FString StageKey;
			if ((*StageObjPtr)->TryGetStringField(TEXT("stage_key"), StageKey) && !StageKey.IsEmpty())
			{
				UeAgentNiagaraOps::TryParseStageKey(StageKey, Usage, UsageId);
			}
			if (Usage == ENiagaraScriptUsage::Function)
			{
				FString UsageText;
				(*StageObjPtr)->TryGetStringField(TEXT("script_usage"), UsageText);
				UeAgentNiagaraOps::TryParseScriptUsage(UsageText, Usage);
				FString UsageIdText;
				(*StageObjPtr)->TryGetStringField(TEXT("script_usage_id"), UsageIdText);
				FGuid::Parse(UsageIdText, UsageId);
			}

			if (!UsageId.IsValid())
			{
				continue;
			}
			if (Usage == ENiagaraScriptUsage::ParticleEventScript)
			{
				OutEventUsageIds.Add(UsageId);
			}
			else if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
			{
				OutSimulationStageUsageIds.Add(UsageId);
			}
		}

		return true;
	}

	static int32 PruneDynamicStagesMissingFromFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		bool bHasStageIndex = false;
		TSet<FGuid> ExpectedEventUsageIds;
		TSet<FGuid> ExpectedSimulationStageUsageIds;
		if (!LoadDynamicStageUsageIdsFromFolder(EmitterFolder, bHasStageIndex, ExpectedEventUsageIds, ExpectedSimulationStageUsageIds, OutError))
		{
			return INDEX_NONE;
		}
		if (!bHasStageIndex)
		{
			return 0;
		}

		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("dynamic_stage_prune_missing_graph"), EmitterFolder, TEXT("Emitter graph source is missing; obsolete dynamic stages cannot be pruned."));
			return 0;
		}

		int32 Pruned = 0;
		TArray<int32> EventHandlerIndicesWithoutUsageId;
		TArray<FGuid> EventUsageIdsToRemove;
		for (int32 Index = 0; Index < EmitterData.EventHandlerScriptProps.Num(); ++Index)
		{
			const FNiagaraEventScriptProperties& EventHandler = EmitterData.EventHandlerScriptProps[Index];
			const FGuid UsageId = EventHandler.Script ? EventHandler.Script->GetUsageId() : FGuid();
			if (!UsageId.IsValid())
			{
				EventHandlerIndicesWithoutUsageId.Add(Index);
			}
			else if (!ExpectedEventUsageIds.Contains(UsageId))
			{
				EventUsageIdsToRemove.Add(UsageId);
			}
		}

		for (int32 RemoveIndex = EventHandlerIndicesWithoutUsageId.Num() - 1; RemoveIndex >= 0; --RemoveIndex)
		{
			const int32 EventHandlerIndex = EventHandlerIndicesWithoutUsageId[RemoveIndex];
			if (EmitterData.EventHandlerScriptProps.IsValidIndex(EventHandlerIndex))
			{
				EmitterData.EventHandlerScriptProps.RemoveAt(EventHandlerIndex);
				++Pruned;
			}
		}
		for (const FGuid& UsageId : EventUsageIdsToRemove)
		{
			Emitter.RemoveEventHandlerByUsageId(UsageId, EmitterData.Version.VersionGuid);
			UeAgentNiagaraOps::DestroyGraphNodesForStage(*Graph, ENiagaraScriptUsage::ParticleEventScript, UsageId);
			++Pruned;
		}

		TArray<UNiagaraSimulationStageBase*> SimulationStagesToRemove;
		for (UNiagaraSimulationStageBase* SimulationStage : EmitterData.GetSimulationStages())
		{
			if (!SimulationStage)
			{
				continue;
			}
			const FGuid UsageId = (SimulationStage && SimulationStage->Script) ? SimulationStage->Script->GetUsageId() : FGuid();
			if (!UsageId.IsValid() || !ExpectedSimulationStageUsageIds.Contains(UsageId))
			{
				SimulationStagesToRemove.Add(SimulationStage);
			}
		}
		for (UNiagaraSimulationStageBase* SimulationStage : SimulationStagesToRemove)
		{
			if (!SimulationStage)
			{
				continue;
			}
			const FGuid UsageId = (SimulationStage && SimulationStage->Script) ? SimulationStage->Script->GetUsageId() : FGuid();
			Emitter.RemoveSimulationStage(SimulationStage, EmitterData.Version.VersionGuid);
			if (UsageId.IsValid())
			{
				UeAgentNiagaraOps::DestroyGraphNodesForStage(*Graph, ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId);
			}
			++Pruned;
		}

		if (Pruned > 0)
		{
			Graph->NotifyGraphChanged();
			Emitter.MarkPackageDirty();
		}
		return Pruned;
	}

	static TArray<FNiagaraEventGeneratorProperties>* ResolveEventGeneratorArray(FVersionedNiagaraEmitterData& EmitterData, const FString& OwnerScript)
	{
		if (OwnerScript.Equals(TEXT("emitter_spawn"), ESearchCase::IgnoreCase))
		{
			return &EmitterData.EmitterSpawnScriptProps.EventGenerators;
		}
		if (OwnerScript.Equals(TEXT("emitter_update"), ESearchCase::IgnoreCase))
		{
			return &EmitterData.EmitterUpdateScriptProps.EventGenerators;
		}
		if (OwnerScript.Equals(TEXT("particle_spawn"), ESearchCase::IgnoreCase))
		{
			return &EmitterData.SpawnScriptProps.EventGenerators;
		}
		if (OwnerScript.Equals(TEXT("particle_update"), ESearchCase::IgnoreCase))
		{
			return &EmitterData.UpdateScriptProps.EventGenerators;
		}
		return nullptr;
	}

	static int32 ApplyEventGeneratorsFromFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		TSharedPtr<FJsonObject> EventGeneratorsObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("event_generators.json")), EventGeneratorsObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!EventGeneratorsObj.IsValid())
		{
			return 0;
		}

		EmitterData.EmitterSpawnScriptProps.EventGenerators.Reset();
		EmitterData.EmitterUpdateScriptProps.EventGenerators.Reset();
		EmitterData.SpawnScriptProps.EventGenerators.Reset();
		EmitterData.UpdateScriptProps.EventGenerators.Reset();

		const TArray<TSharedPtr<FJsonValue>>* EventGenerators = nullptr;
		if (!TryGetArrayField(EventGeneratorsObj, TEXT("event_generators"), EventGenerators))
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& GeneratorValue : *EventGenerators)
		{
			const TSharedPtr<FJsonObject>* GeneratorObjPtr = nullptr;
			if (!GeneratorValue.IsValid() || !GeneratorValue->TryGetObject(GeneratorObjPtr) || !GeneratorObjPtr || !GeneratorObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& GeneratorObj = *GeneratorObjPtr;
			FString OwnerScript;
			GeneratorObj->TryGetStringField(TEXT("owner_script"), OwnerScript);
			TArray<FNiagaraEventGeneratorProperties>* TargetGenerators = ResolveEventGeneratorArray(EmitterData, OwnerScript);
			if (!TargetGenerators)
			{
				AddWarning(Warnings, TEXT("event_generator_owner_unresolved"), OwnerScript, TEXT("Event generator owner_script must be emitter_spawn, emitter_update, particle_spawn or particle_update."));
				continue;
			}

			FNiagaraEventGeneratorProperties NewGenerator;
			FString GeneratorId;
			if (GeneratorObj->TryGetStringField(TEXT("id"), GeneratorId) && !GeneratorId.IsEmpty())
			{
				NewGenerator.ID = FName(*GeneratorId);
			}
			if (ShouldSkipBuiltInEventGeneratorWithoutStageModule(EmitterFolder, OwnerScript, GeneratorId, Warnings))
			{
				continue;
			}

			double MaxEventsPerFrame = 0.0;
			if (GeneratorObj->TryGetNumberField(TEXT("max_events_per_frame"), MaxEventsPerFrame))
			{
				NewGenerator.MaxEventsPerFrame = FMath::Max(0, FMath::RoundToInt(MaxEventsPerFrame));
			}

			const int32 NewIndex = TargetGenerators->Add(NewGenerator);
			FNiagaraEventGeneratorProperties& AddedGenerator = (*TargetGenerators)[NewIndex];
			const int32 DetailsApplied = ApplyStructDetailsIfPresent(
				FNiagaraEventGeneratorProperties::StaticStruct(),
				&AddedGenerator,
				&Emitter,
				EmitterFolder,
				GeneratorObj,
				Warnings,
				Emitter.GetPathName(),
				OutError);
			if (DetailsApplied == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			Applied += DetailsApplied;
			++Applied;
		}

		return Applied;
	}

	static int32 ApplyEventHandlersFromFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("event_handlers_missing_graph"), EmitterFolder, TEXT("Emitter graph source is missing; event handler output stages cannot be created."));
			return 0;
		}

		TSharedPtr<FJsonObject> EventHandlersObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("event_handlers.json")), EventHandlersObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!EventHandlersObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* EventHandlers = nullptr;
		if (!TryGetArrayField(EventHandlersObj, TEXT("event_handlers"), EventHandlers))
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& EventValue : *EventHandlers)
		{
			const TSharedPtr<FJsonObject>* EventObjPtr = nullptr;
			if (!EventValue.IsValid() || !EventValue->TryGetObject(EventObjPtr) || !EventObjPtr || !EventObjPtr->IsValid())
			{
				continue;
			}

			FNiagaraEventScriptProperties* EventHandler = nullptr;
			int32 EventIndex = INDEX_NONE;
			if (!EnsureEventHandlerForFolder(Emitter, EmitterData, *Graph, *EventObjPtr, EventHandler, EventIndex, Warnings, OutError))
			{
				return INDEX_NONE;
			}
			if (!EventHandler)
			{
				continue;
			}

			const int32 DetailsApplied = ApplyStructDetailsIfPresent(
				FNiagaraEventScriptProperties::StaticStruct(),
				EventHandler,
				&Emitter,
				EmitterFolder,
				*EventObjPtr,
				Warnings,
				Emitter.GetPathName(),
				OutError);
			if (DetailsApplied == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			Applied += DetailsApplied;
			++Applied;
		}
		return Applied;
	}

	static int32 ApplyDataInterfacesFromFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("data_interfaces_missing_graph"), EmitterFolder, TEXT("Emitter graph source is missing; data interfaces cannot be applied."));
			return 0;
		}

		TSharedPtr<FJsonObject> DataInterfacesObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("data_interfaces.json")), DataInterfacesObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!DataInterfacesObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* DataInterfaces = nullptr;
		if (!TryGetArrayField(DataInterfacesObj, TEXT("data_interfaces"), DataInterfaces))
		{
			return 0;
		}

		int32 Applied = 0;
		for (int32 DataInterfaceIndex = 0; DataInterfaceIndex < DataInterfaces->Num(); ++DataInterfaceIndex)
		{
			const FString DataInterfacePath = FString::Printf(TEXT("%s/data_interfaces.json.data_interfaces[%d]"), *EmitterFolder, DataInterfaceIndex);
			TSharedPtr<FJsonObject> DataInterfaceObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*DataInterfaces)[DataInterfaceIndex], DataInterfacePath, DataInterfaceObj, Warnings))
			{
				continue;
			}

			UeAgentJsonDiagnostics::WarnUnknownFields(
				DataInterfaceObj,
				DataInterfacePath,
				{
					TEXT("node_guid"),
					TEXT("input_name"),
					TEXT("input_type"),
					TEXT("data_interface_class"),
					TEXT("data_interface_object_path"),
					TEXT("details")
				},
				Warnings);

			FString NodeGuidText;
			FString InputNameText;
			FString DataInterfaceClassText;
			DataInterfaceObj->TryGetStringField(TEXT("node_guid"), NodeGuidText);
			DataInterfaceObj->TryGetStringField(TEXT("input_name"), InputNameText);
			DataInterfaceObj->TryGetStringField(TEXT("data_interface_class"), DataInterfaceClassText);

			FGuid NodeGuid;
			FGuid::Parse(NodeGuidText, NodeGuid);
			UNiagaraNodeInput* TargetInputNode = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
				if (!InputNode)
				{
					continue;
				}
				if ((NodeGuid.IsValid() && InputNode->NodeGuid == NodeGuid)
					|| (!InputNameText.IsEmpty() && InputNode->Input.GetName().ToString().Equals(InputNameText, ESearchCase::IgnoreCase)))
				{
					TargetInputNode = InputNode;
					break;
				}
			}

			if (!TargetInputNode && !DataInterfaceClassText.IsEmpty())
			{
				TArray<UNiagaraNodeInput*> ClassMatchedInputNodes;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
					if (!InputNode)
					{
						continue;
					}

					FProperty* CandidateDataInterfaceProperty = nullptr;
					void* CandidateDataInterfaceValuePtr = nullptr;
					if (!UeAgentNiagaraOps::ResolvePropertyPath(InputNode, TEXT("DataInterface"), CandidateDataInterfaceProperty, CandidateDataInterfaceValuePtr))
					{
						continue;
					}

					FObjectPropertyBase* CandidateObjectProperty = CastField<FObjectPropertyBase>(CandidateDataInterfaceProperty);
					UNiagaraDataInterface* CandidateDataInterface = CandidateObjectProperty
						? Cast<UNiagaraDataInterface>(CandidateObjectProperty->GetObjectPropertyValue(CandidateDataInterfaceValuePtr))
						: nullptr;
					if (CandidateDataInterface
						&& CandidateDataInterface->GetClass()
						&& CandidateDataInterface->GetClass()->GetPathName().Equals(DataInterfaceClassText, ESearchCase::IgnoreCase))
					{
						ClassMatchedInputNodes.Add(InputNode);
					}
				}

				if (ClassMatchedInputNodes.Num() == 1)
				{
					TargetInputNode = ClassMatchedInputNodes[0];
				}
				else if (ClassMatchedInputNodes.Num() > 1)
				{
					AddWarning(
						Warnings,
						TEXT("data_interface_class_match_ambiguous"),
						DataInterfacePath,
						FString::Printf(TEXT("Found %d data interface input nodes for class '%s'; node_guid/input_name did not disambiguate."), ClassMatchedInputNodes.Num(), *DataInterfaceClassText));
				}
			}

			if (!TargetInputNode)
			{
				AddWarning(Warnings, TEXT("data_interface_input_node_not_found"), DataInterfacePath, FString::Printf(TEXT("No UNiagaraNodeInput matched node_guid='%s' input_name='%s'."), *NodeGuidText, *InputNameText));
				continue;
			}

			FProperty* DataInterfaceProperty = nullptr;
			void* DataInterfaceValuePtr = nullptr;
			if (!UeAgentNiagaraOps::ResolvePropertyPath(TargetInputNode, TEXT("DataInterface"), DataInterfaceProperty, DataInterfaceValuePtr))
			{
				AddWarning(Warnings, TEXT("data_interface_property_not_found"), DataInterfacePath, TEXT("UNiagaraNodeInput.DataInterface property could not be resolved."));
				continue;
			}

			FObjectPropertyBase* DataInterfaceObjectProperty = CastField<FObjectPropertyBase>(DataInterfaceProperty);
			if (!DataInterfaceObjectProperty)
			{
				AddWarning(Warnings, TEXT("data_interface_property_not_object"), DataInterfacePath, TEXT("UNiagaraNodeInput.DataInterface is not an object property."));
				continue;
			}

			UNiagaraDataInterface* DataInterface = Cast<UNiagaraDataInterface>(DataInterfaceObjectProperty->GetObjectPropertyValue(DataInterfaceValuePtr));
			if (!DataInterface && !DataInterfaceClassText.IsEmpty())
			{
				UClass* DataInterfaceClass = LoadObject<UClass>(nullptr, *DataInterfaceClassText);
				if (DataInterfaceClass && DataInterfaceClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
				{
					TargetInputNode->Modify();
					DataInterface = NewObject<UNiagaraDataInterface>(TargetInputNode, DataInterfaceClass, NAME_None, RF_Transactional);
					DataInterfaceObjectProperty->SetObjectPropertyValue(DataInterfaceValuePtr, DataInterface);
				}
			}

			if (!DataInterface)
			{
				AddWarning(Warnings, TEXT("data_interface_instance_missing"), DataInterfacePath, FString::Printf(TEXT("Data interface instance could not be resolved or created for class '%s'."), *DataInterfaceClassText));
				continue;
			}

			DataInterface->Modify();
			if (!ApplyFolderOwnerDetailsIfPresent(
				DataInterface,
				EmitterFolder,
				DataInterfaceObj,
				TEXT("niagara_data_interface"),
				FString::Printf(TEXT("%s.%s"), *FPaths::GetCleanFilename(EmitterFolder), *NodeGuidText),
				TEXT("niagara_emitter_apply_folder"),
				OutError,
				Applied))
			{
				return INDEX_NONE;
			}
			++Applied;
		}

		return Applied;
	}

	static UNiagaraSimulationStageBase* EnsureSimulationStageForFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& StageObj, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		FString UsageIdText;
		StageObj->TryGetStringField(TEXT("script_usage_id"), UsageIdText);
		FGuid UsageId;
		FGuid::Parse(UsageIdText, UsageId);

		if (UsageId.IsValid())
		{
			if (UNiagaraSimulationStageBase* ExistingStage = EmitterData.GetSimulationStageById(UsageId))
			{
				return ExistingStage;
			}
		}
		else
		{
			UsageId = FGuid::NewGuid();
		}

		FString StageClassPath = TEXT("/Script/Niagara.NiagaraSimulationStageGeneric");
		StageObj->TryGetStringField(TEXT("simulation_stage_class"), StageClassPath);
		UClass* StageClass = UeAgentNiagaraOps::ResolveClassByPath(StageClassPath);
		if (!StageClass || !StageClass->IsChildOf(UNiagaraSimulationStageBase::StaticClass()) || StageClass->HasAnyClassFlags(CLASS_Abstract))
		{
			StageClass = UeAgentNiagaraOps::ResolveClassByPath(TEXT("/Script/Niagara.NiagaraSimulationStageGeneric"));
		}
		if (!StageClass)
		{
			OutError = TEXT("invalid_simulation_stage_class");
			return nullptr;
		}

		UNiagaraSimulationStageBase* AddedStage = NewObject<UNiagaraSimulationStageBase>(&Emitter, StageClass, NAME_None, RF_Transactional);
		if (!AddedStage)
		{
			OutError = TEXT("create_simulation_stage_failed");
			return nullptr;
		}

		AddedStage->Script = NewObject<UNiagaraScript>(AddedStage, MakeUniqueObjectName(AddedStage, UNiagaraScript::StaticClass(), TEXT("SimulationStage")), RF_Transactional);
		if (!AddedStage->Script)
		{
			OutError = TEXT("create_simulation_stage_script_failed");
			return nullptr;
		}

		FString StageName = TEXT("SimulationStage");
		StageObj->TryGetStringField(TEXT("simulation_stage_name"), StageName);
		StageName.TrimStartAndEndInline();
		if (StageName.IsEmpty())
		{
			StageName = TEXT("SimulationStage");
		}
		AddedStage->SimulationStageName = FName(*StageName);
		AddedStage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
		AddedStage->Script->SetUsageId(UsageId);
		if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource))
		{
			AddedStage->Script->SetLatestSource(ScriptSource);
		}

		Emitter.AddSimulationStage(AddedStage, EmitterData.Version.VersionGuid);
		if (!UeAgentNiagaraOps::EnsureStageInputOutputBridge(Graph, ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId))
		{
			AddWarning(Warnings, TEXT("simulation_stage_output_bridge_failed"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower), TEXT("Simulation stage was created but output bridge could not be initialized."));
		}
		return AddedStage;
	}

	static bool IsAssignmentModuleFolderObject(const TSharedPtr<FJsonObject>& ModuleObj)
	{
		const FString ModuleKind = GetStringFieldAny(ModuleObj, { TEXT("module_kind") });
		if (ModuleKind.Equals(TEXT("assignment"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		const FString ModuleName = GetStringFieldAny(ModuleObj, { TEXT("module_name") });
		if (ModuleName.StartsWith(TEXT("SetVariables"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		const FString ModuleScriptPath = GetStringFieldAny(ModuleObj, { TEXT("module_script_path"), TEXT("module_script_asset_path") });
		return ModuleScriptPath.Contains(TEXT("NiagaraNodeAssignment"));
	}

	static bool IsPrivateInlineNiagaraScriptPath(const FString& ModuleScriptPath)
	{
		return ModuleScriptPath.Contains(TEXT(":"));
	}

	static FString GetNiagaraInputDefaultValue(const TSharedPtr<FJsonObject>& InputObj)
	{
		FString Value;
		if (InputObj.IsValid() && InputObj->TryGetStringField(TEXT("override_default_value"), Value) && !Value.IsEmpty())
		{
			return Value;
		}
		if (InputObj.IsValid() && InputObj->TryGetStringField(TEXT("autogenerated_default_value"), Value))
		{
			return Value;
		}
		return FString();
	}

	static FString GetZeroDefaultValueForNiagaraType(const FNiagaraTypeDefinition& TypeDefinition)
	{
		if (TypeDefinition == FNiagaraTypeDefinition::GetBoolDef())
		{
			return TEXT("false");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetIntDef())
		{
			return TEXT("0");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetFloatDef() || TypeDefinition == FNiagaraTypeHelper::GetDoubleDef())
		{
			return TEXT("0.0");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec2Def())
		{
			return TEXT("X=0.0 Y=0.0");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec3Def() || TypeDefinition == FNiagaraTypeDefinition::GetPositionDef())
		{
			return TEXT("X=0.0 Y=0.0 Z=0.0");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec4Def() || TypeDefinition == FNiagaraTypeDefinition::GetQuatDef())
		{
			return TEXT("X=0.0 Y=0.0 Z=0.0 W=0.0");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetColorDef())
		{
			return TEXT("R=0.0 G=0.0 B=0.0 A=1.0");
		}
		return FString();
	}

	static UNiagaraNodeAssignment* AddAssignmentModuleFromFolder(
		UNiagaraNodeOutput& OutputNode,
		const int32 TargetIndex,
		const TSharedPtr<FJsonObject>& ModuleObj,
		TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (!TryGetArrayField(ModuleObj, TEXT("inputs"), Inputs))
		{
			AddWarning(Warnings, TEXT("assignment_module_inputs_missing"), GetStringFieldAny(ModuleObj, { TEXT("module_name") }), TEXT("Assignment module could not be reconstructed because it has no exported inputs."));
			return nullptr;
		}

		TArray<FNiagaraVariable> AssignmentVariables;
		TArray<FString> AssignmentDefaults;
		for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
		{
			const TSharedPtr<FJsonObject>* InputObjPtr = nullptr;
			if (!InputValue.IsValid() || !InputValue->TryGetObject(InputObjPtr) || !InputObjPtr || !InputObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& InputObj = *InputObjPtr;
			FString InputName = GetStringFieldAny(InputObj, { TEXT("input_name") });
			InputName.TrimStartAndEndInline();
			if (!InputName.StartsWith(TEXT("Module."), ESearchCase::CaseSensitive))
			{
				continue;
			}

			FString TargetName = InputName.Mid(7);
			TargetName.TrimStartAndEndInline();
			if (TargetName.IsEmpty())
			{
				continue;
			}

			const FString TypeText = GetStringFieldAny(InputObj, { TEXT("input_type"), TEXT("parameter_type"), TEXT("type_internal") });
			FNiagaraTypeDefinition TypeDefinition;
			if (!UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(TypeText, TypeDefinition))
			{
				AddWarning(Warnings, TEXT("assignment_input_type_unresolved"), TargetName, TypeText);
				continue;
			}

			FString DefaultValue = GetNiagaraInputDefaultValue(InputObj);
			if (DefaultValue.IsEmpty())
			{
				DefaultValue = GetZeroDefaultValueForNiagaraType(TypeDefinition);
			}
			AssignmentVariables.Add(FNiagaraVariable(TypeDefinition, *TargetName));
			AssignmentDefaults.Add(DefaultValue);
		}

		if (AssignmentVariables.Num() == 0)
		{
			AddWarning(Warnings, TEXT("assignment_module_targets_missing"), GetStringFieldAny(ModuleObj, { TEXT("module_name") }), TEXT("Assignment module had no resolvable assignment targets."));
			return nullptr;
		}

		return FNiagaraStackGraphUtilities::AddParameterModuleToStack(AssignmentVariables, OutputNode, TargetIndex, AssignmentDefaults);
	}

	static bool ApplyModuleInputDefault(UNiagaraGraph& Graph, UNiagaraNodeFunctionCall& ModuleNode, const TSharedPtr<FJsonObject>& InputObj, const FString& ContextPath, TArray<TSharedPtr<FJsonValue>>& Warnings);

	static UNiagaraNodeInput* FindDynamicInputDataInterfaceNode(
		UNiagaraGraph& Graph,
		UNiagaraNodeFunctionCall& DynamicInputNode,
		const TSharedPtr<FJsonObject>& DataInterfaceObj,
		const FString& ContextPath,
		TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		FString NodeGuidText;
		FString InputNameText;
		FString DataInterfaceClassText;
		DataInterfaceObj->TryGetStringField(TEXT("node_guid"), NodeGuidText);
		DataInterfaceObj->TryGetStringField(TEXT("input_name"), InputNameText);
		DataInterfaceObj->TryGetStringField(TEXT("data_interface_class"), DataInterfaceClassText);

		FGuid NodeGuid;
		FGuid::Parse(NodeGuidText, NodeGuid);
		if (NodeGuid.IsValid())
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
				if (InputNode && InputNode->NodeGuid == NodeGuid)
				{
					return InputNode;
				}
			}
		}

		if (!InputNameText.IsEmpty())
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
				if (InputNode && InputNode->Input.GetName().ToString().Equals(InputNameText, ESearchCase::IgnoreCase))
				{
					return InputNode;
				}
			}
		}

		TArray<UNiagaraNodeInput*> LinkedDataInterfaceInputs;
		UeAgentNiagaraOps::CollectLinkedDataInterfaceInputNodesForDynamicInput(DynamicInputNode, LinkedDataInterfaceInputs);
		if (!DataInterfaceClassText.IsEmpty() || !InputNameText.IsEmpty())
		{
			TArray<UNiagaraNodeInput*> Candidates;
			const FString InputNameLower = UeAgentNiagaraOps::MakeLowerTrimmed(InputNameText);
			for (UNiagaraNodeInput* InputNode : LinkedDataInterfaceInputs)
			{
				if (!InputNode)
				{
					continue;
				}

				bool bMatchesInputName = InputNameLower.IsEmpty();
				const FString CandidateInputNameLower = UeAgentNiagaraOps::MakeLowerTrimmed(InputNode->Input.GetName().ToString());
				if (!InputNameLower.IsEmpty())
				{
					bMatchesInputName = CandidateInputNameLower == InputNameLower ||
						CandidateInputNameLower.EndsWith(TEXT(".") + InputNameLower) ||
						InputNameLower.EndsWith(TEXT(".") + CandidateInputNameLower);
				}

				bool bMatchesClass = DataInterfaceClassText.IsEmpty();
				if (!bMatchesClass)
				{
					FProperty* CandidateDataInterfaceProperty = nullptr;
					void* CandidateDataInterfaceValuePtr = nullptr;
					if (UeAgentNiagaraOps::ResolvePropertyPath(InputNode, TEXT("DataInterface"), CandidateDataInterfaceProperty, CandidateDataInterfaceValuePtr))
					{
						FObjectPropertyBase* CandidateObjectProperty = CastField<FObjectPropertyBase>(CandidateDataInterfaceProperty);
						UNiagaraDataInterface* CandidateDataInterface = CandidateObjectProperty
							? Cast<UNiagaraDataInterface>(CandidateObjectProperty->GetObjectPropertyValue(CandidateDataInterfaceValuePtr))
							: nullptr;
						bMatchesClass = CandidateDataInterface
							&& CandidateDataInterface->GetClass()
							&& CandidateDataInterface->GetClass()->GetPathName().Equals(DataInterfaceClassText, ESearchCase::IgnoreCase);
					}
				}

				if (bMatchesInputName && bMatchesClass)
				{
					Candidates.Add(InputNode);
				}
			}

			if (Candidates.Num() == 1)
			{
				return Candidates[0];
			}
			if (Candidates.Num() > 1)
			{
				AddWarning(
					Warnings,
					TEXT("dynamic_input_data_interface_match_ambiguous"),
					ContextPath,
					FString::Printf(TEXT("Found %d linked data interface input nodes for input_name='%s' class='%s'."), Candidates.Num(), *InputNameText, *DataInterfaceClassText));
				return nullptr;
			}
		}

		AddWarning(
			Warnings,
			TEXT("dynamic_input_data_interface_input_node_not_found"),
			ContextPath,
			FString::Printf(TEXT("No linked data interface input node matched node_guid='%s' input_name='%s'."), *NodeGuidText, *InputNameText));
		return nullptr;
	}

	static int32 ApplyDynamicInputDataInterfacesFromJson(
		UNiagaraGraph& Graph,
		UNiagaraNodeFunctionCall& DynamicInputNode,
		const TSharedPtr<FJsonObject>& DynamicInputObj,
		const FString& ContextPath,
		TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		const TArray<TSharedPtr<FJsonValue>>* DataInterfaces = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(DynamicInputObj, TEXT("data_interfaces"), ContextPath + TEXT(".data_interfaces"), DataInterfaces, Warnings, false))
		{
			return 0;
		}

		int32 Applied = 0;
		for (int32 DataInterfaceIndex = 0; DataInterfaceIndex < DataInterfaces->Num(); ++DataInterfaceIndex)
		{
			const FString DataInterfacePath = FString::Printf(TEXT("%s.data_interfaces[%d]"), *ContextPath, DataInterfaceIndex);
			TSharedPtr<FJsonObject> DataInterfaceObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*DataInterfaces)[DataInterfaceIndex], DataInterfacePath, DataInterfaceObj, Warnings))
			{
				continue;
			}

			UeAgentJsonDiagnostics::WarnUnknownFields(
				DataInterfaceObj,
				DataInterfacePath,
				{
					TEXT("node_guid"),
					TEXT("input_name"),
					TEXT("input_type"),
					TEXT("data_interface_class"),
					TEXT("data_interface_object_path"),
					TEXT("details")
				},
				Warnings);

			UNiagaraNodeInput* TargetInputNode = FindDynamicInputDataInterfaceNode(Graph, DynamicInputNode, DataInterfaceObj, DataInterfacePath, Warnings);
			if (!TargetInputNode)
			{
				continue;
			}

			FString DataInterfaceClassText;
			DataInterfaceObj->TryGetStringField(TEXT("data_interface_class"), DataInterfaceClassText);

			FProperty* DataInterfaceProperty = nullptr;
			void* DataInterfaceValuePtr = nullptr;
			if (!UeAgentNiagaraOps::ResolvePropertyPath(TargetInputNode, TEXT("DataInterface"), DataInterfaceProperty, DataInterfaceValuePtr))
			{
				AddWarning(Warnings, TEXT("dynamic_input_data_interface_property_not_found"), DataInterfacePath, TEXT("UNiagaraNodeInput.DataInterface property could not be resolved."));
				continue;
			}

			FObjectPropertyBase* DataInterfaceObjectProperty = CastField<FObjectPropertyBase>(DataInterfaceProperty);
			if (!DataInterfaceObjectProperty)
			{
				AddWarning(Warnings, TEXT("dynamic_input_data_interface_property_not_object"), DataInterfacePath, TEXT("UNiagaraNodeInput.DataInterface is not an object property."));
				continue;
			}

			UNiagaraDataInterface* DataInterface = Cast<UNiagaraDataInterface>(DataInterfaceObjectProperty->GetObjectPropertyValue(DataInterfaceValuePtr));
			if (!DataInterface && !DataInterfaceClassText.IsEmpty())
			{
				UClass* DataInterfaceClass = LoadObject<UClass>(nullptr, *DataInterfaceClassText);
				if (DataInterfaceClass && DataInterfaceClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
				{
					TargetInputNode->Modify();
					DataInterface = NewObject<UNiagaraDataInterface>(TargetInputNode, DataInterfaceClass, NAME_None, RF_Transactional);
					DataInterfaceObjectProperty->SetObjectPropertyValue(DataInterfaceValuePtr, DataInterface);
				}
			}

			if (!DataInterface)
			{
				AddWarning(Warnings, TEXT("dynamic_input_data_interface_instance_missing"), DataInterfacePath, FString::Printf(TEXT("Data interface instance could not be resolved or created for class '%s'."), *DataInterfaceClassText));
				continue;
			}

			DataInterface->Modify();
			++Applied;
		}
		return Applied;
	}

	// Module input, emitter stage, and system apply helpers are split for navigation.
	static bool ApplyModuleInputDynamicInput(
		UNiagaraGraph& Graph,
		UNiagaraNodeFunctionCall& ModuleNode,
		UEdGraphPin& OverridePin,
		const TSharedPtr<FJsonObject>& DynamicInputObj,
		const FString& ContextPath,
		TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			DynamicInputObj,
			ContextPath,
			{
				TEXT("node_guid"),
				TEXT("name"),
				TEXT("catalog_name"),
				TEXT("node_name"),
				TEXT("dynamic_input_name"),
				TEXT("path"),
				TEXT("script_asset_path"),
				TEXT("dynamic_input_script_asset_path"),
				TEXT("module_script_path"),
				TEXT("inputs"),
				TEXT("input_count"),
				TEXT("data_interfaces"),
				TEXT("data_interface_count")
			},
			Warnings);

		FString DynamicInputScriptPath = GetStringFieldAny(DynamicInputObj, {
			TEXT("path"),
			TEXT("script_asset_path"),
			TEXT("dynamic_input_script_asset_path"),
			TEXT("module_script_path")
		});
		if (DynamicInputScriptPath.IsEmpty())
		{
			const FString CatalogName = GetStringFieldAny(DynamicInputObj, { TEXT("catalog_name"), TEXT("name") });
			if (!CatalogName.TrimStartAndEnd().IsEmpty())
			{
				UeAgentPaletteSearch::FCandidate CatalogCandidate;
				FString ResolveError;
				if (UeAgentPaletteSearch::ResolveCatalogNameToCandidate(TEXT("niagara_dynamic_input"), CatalogName, CatalogCandidate, ResolveError))
				{
					DynamicInputScriptPath = CatalogCandidate.FullName;
				}
				else
				{
					AddWarning(Warnings, TEXT("dynamic_input_catalog_name_not_found"), ContextPath + TEXT(".name"), ResolveError);
				}
			}
		}
		if (DynamicInputScriptPath.IsEmpty())
		{
			AddWarning(Warnings, TEXT("dynamic_input_missing_script_asset_path"), ContextPath, TEXT("dynamic_input has no script_asset_path; the dynamic input was skipped."));
			return true;
		}

		UNiagaraScript* DynamicInputScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(DynamicInputScriptPath));
		if (!DynamicInputScript)
		{
			AddWarning(Warnings, TEXT("dynamic_input_script_not_found"), ContextPath + TEXT(".script_asset_path"), DynamicInputScriptPath);
			return true;
		}

		if (OverridePin.LinkedTo.Num() > 0)
		{
			OverridePin.BreakAllPinLinks();
		}

		FString DynamicInputSuggestedName = GetStringFieldAny(DynamicInputObj, { TEXT("node_name"), TEXT("dynamic_input_name") });
		DynamicInputSuggestedName.TrimStartAndEndInline();

		UNiagaraNodeFunctionCall* DynamicInputNode = nullptr;
		FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(
			OverridePin,
			DynamicInputScript,
			DynamicInputNode,
			FGuid(),
			DynamicInputSuggestedName,
			DynamicInputScript->GetExposedVersion().VersionGuid);
		if (!DynamicInputNode)
		{
			AddWarning(Warnings, TEXT("dynamic_input_create_failed"), ContextPath, DynamicInputScriptPath);
			return true;
		}

		FString DynamicInputInitializationSource;
		const bool bDynamicInputInputsInitialized = UeAgentNiagaraOps::InitializeDynamicInputStackInputs(
			nullptr,
			FGuid(),
			&Graph,
			ModuleNode,
			*DynamicInputNode,
			DynamicInputInitializationSource);
		if (!bDynamicInputInputsInitialized)
		{
			AddWarning(
				Warnings,
				TEXT("dynamic_input_inputs_initialization_incomplete"),
				ContextPath,
				DynamicInputInitializationSource);
		}

		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (UeAgentJsonDiagnostics::ReadArrayField(DynamicInputObj, TEXT("inputs"), ContextPath + TEXT(".inputs"), Inputs, Warnings, false))
		{
			for (int32 InputIndex = 0; InputIndex < Inputs->Num(); ++InputIndex)
			{
				TSharedPtr<FJsonObject> NestedInputObj;
				const FString NestedInputPath = FString::Printf(TEXT("%s.inputs[%d]"), *ContextPath, InputIndex);
				if (UeAgentJsonDiagnostics::ReadObjectFromValue((*Inputs)[InputIndex], NestedInputPath, NestedInputObj, Warnings))
				{
					ApplyModuleInputDefault(Graph, *DynamicInputNode, NestedInputObj, NestedInputPath, Warnings);
				}
			}
		}

		ApplyDynamicInputDataInterfacesFromJson(Graph, *DynamicInputNode, DynamicInputObj, ContextPath, Warnings);
		Graph.NotifyGraphChanged();
		return true;
	}

	static bool ApplyModuleInputDefault(UNiagaraGraph& Graph, UNiagaraNodeFunctionCall& ModuleNode, const TSharedPtr<FJsonObject>& InputObj, const FString& ContextPath, TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			InputObj,
			ContextPath,
			{
				TEXT("input_name"),
				TEXT("input_short_name"),
				TEXT("input_type"),
				TEXT("input_type_descriptor"),
				TEXT("has_override"),
				TEXT("has_links"),
				TEXT("override_default_value"),
				TEXT("autogenerated_default_value"),
				TEXT("has_visible_pin"),
				TEXT("enum_type"),
				TEXT("enum_value_name"),
				TEXT("enum_value_display_name"),
				TEXT("enum_value_int"),
				TEXT("override_enum_value_name"),
				TEXT("override_enum_value_display_name"),
				TEXT("override_enum_value_int"),
				TEXT("autogenerated_enum_value_name"),
				TEXT("autogenerated_enum_value_display_name"),
				TEXT("autogenerated_enum_value_int"),
				TEXT("candidates"),
				TEXT("enum_options"),
				TEXT("link_kind"),
				TEXT("linked_parameter_name"),
				TEXT("link_parameter_name"),
				TEXT("link_parameter"),
				TEXT("data_interface"),
				TEXT("data_interface_class"),
				TEXT("dynamic_input")
			},
			Warnings);

		TSharedPtr<FJsonObject> DynamicInputObj;
		const bool bHasDynamicInput = GetObjectField(InputObj, TEXT("dynamic_input"), DynamicInputObj);
		TSharedPtr<FJsonObject> DataInterfaceObj;
		const bool bHasDataInterfaceObject = GetObjectField(InputObj, TEXT("data_interface"), DataInterfaceObj);
		FString LinkKind;
		InputObj->TryGetStringField(TEXT("link_kind"), LinkKind);
		LinkKind.TrimStartAndEndInline();
		FString LinkedParameterName = GetStringFieldAny(InputObj, {
			TEXT("linked_parameter_name"),
			TEXT("link_parameter_name"),
			TEXT("link_parameter")
		});
		LinkedParameterName.TrimStartAndEndInline();
		FString DataInterfaceClassText = GetStringFieldAny(InputObj, { TEXT("data_interface_class") });
		if (DataInterfaceObj.IsValid())
		{
			FString NestedDataInterfaceClassText;
			if (DataInterfaceObj->TryGetStringField(TEXT("data_interface_class"), NestedDataInterfaceClassText) && !NestedDataInterfaceClassText.TrimStartAndEnd().IsEmpty())
			{
				DataInterfaceClassText = NestedDataInterfaceClassText;
			}
		}
		DataInterfaceClassText.TrimStartAndEndInline();
		const bool bHasDataInterface = bHasDataInterfaceObject
			|| LinkKind.Equals(TEXT("data_interface"), ESearchCase::IgnoreCase)
			|| !DataInterfaceClassText.IsEmpty();

		bool bHasOverride = false;
		const bool bReadHasOverride = UeAgentJsonDiagnostics::ReadBoolField(
			InputObj,
			TEXT("has_override"),
			ContextPath + TEXT(".has_override"),
			bHasOverride,
			Warnings,
			false);

		FString ValueText;
		const bool bReadOverrideDefaultValue = UeAgentJsonDiagnostics::ReadStringField(
			InputObj,
			TEXT("override_default_value"),
			ContextPath + TEXT(".override_default_value"),
			ValueText,
			Warnings,
			false);

		if (!bReadHasOverride && (bHasDynamicInput || bHasDataInterface || !LinkedParameterName.IsEmpty() || (bReadOverrideDefaultValue && !ValueText.TrimStartAndEnd().IsEmpty())))
		{
			bHasOverride = true;
			if (!bHasDynamicInput && !bHasDataInterface && LinkedParameterName.IsEmpty())
			{
				AddWarning(
					Warnings,
					TEXT("module_input_has_override_missing_assumed_from_value"),
					ContextPath,
					TEXT("has_override is missing, but override_default_value is present and non-empty; applying it as an override."));
			}
		}

		if (!bHasOverride)
		{
			return true;
		}

		if (!bReadOverrideDefaultValue && !bHasDynamicInput && !bHasDataInterface && LinkedParameterName.IsEmpty())
		{
			AddWarning(
				Warnings,
				TEXT("module_input_missing_override_default_value"),
				ContextPath + TEXT(".override_default_value"),
				TEXT("has_override is true, but override_default_value is missing or not a string; the module input override was skipped."));
			return true;
		}

		bool bHasLinks = false;
		UeAgentJsonDiagnostics::ReadBoolField(
			InputObj,
			TEXT("has_links"),
			ContextPath + TEXT(".has_links"),
			bHasLinks,
			Warnings,
			false);
		if (bHasLinks && !bHasDynamicInput && !bHasDataInterface && LinkedParameterName.IsEmpty())
		{
			return true;
		}

		FString InputName;
		if (!UeAgentJsonDiagnostics::ReadStringField(InputObj, TEXT("input_name"), ContextPath + TEXT(".input_name"), InputName, Warnings, false))
		{
			UeAgentJsonDiagnostics::ReadStringField(InputObj, TEXT("input_short_name"), ContextPath + TEXT(".input_short_name"), InputName, Warnings, false);
		}
		if (InputName.IsEmpty())
		{
			AddWarning(
				Warnings,
				TEXT("module_input_missing_input_name"),
				ContextPath,
				TEXT("Module input has override data but no input_name/input_short_name; the override was skipped."));
			return true;
		}

		TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
		UeAgentNiagaraOps::BuildResolvedModuleInputs(ModuleNode, ModuleInputs);
		FString ResolvedInputName;
		const UeAgentNiagaraOps::FResolvedNiagaraModuleInput* ResolvedInput =
			UeAgentNiagaraOps::ResolveModuleInputByName(ModuleInputs, InputName, ResolvedInputName);
		if (!ResolvedInput || !ResolvedInput->InputVariable.IsValid())
		{
			AddWarning(Warnings, TEXT("module_input_not_found"), ContextPath + TEXT(".input_name"), FString::Printf(TEXT("Module input '%s' listed in folder could not be resolved on the target module."), *InputName));
			return true;
		}

		const FNiagaraParameterHandle ModuleInputHandle(ResolvedInput->InputVariable.GetName());
		const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleInputHandle, &ModuleNode);
		UEdGraphPin* OverridePin = ResolvedInput->VisiblePin;
		if (OverridePin)
		{
			if (UEdGraphNode* OverrideOwnerNode = OverridePin->GetOwningNode())
			{
				OverrideOwnerNode->Modify();
			}
		}
		else
		{
			FString OverridePinError;
			if (!UeAgentNiagaraOps::TryGetOrCreateStackFunctionInputOverridePin(
				ModuleNode,
				AliasedInputHandle,
				ResolvedInput->InputVariable.GetType(),
				ResolvedInput->InputScriptVariableGuid,
				OverridePin,
				OverridePinError) || !OverridePin)
			{
				AddWarning(Warnings, TEXT("module_input_override_unavailable"), ContextPath, FString::Printf(TEXT("%s: %s"), *ResolvedInputName, *OverridePinError));
				return true;
			}
		}

		if (bHasDynamicInput)
		{
			return ApplyModuleInputDynamicInput(Graph, ModuleNode, *OverridePin, DynamicInputObj, ContextPath + TEXT(".dynamic_input"), Warnings);
		}

		if (bHasDataInterface)
		{
			if (!ResolvedInput->InputVariable.GetType().IsDataInterface())
			{
				AddWarning(
					Warnings,
					TEXT("module_input_data_interface_type_mismatch"),
					ContextPath,
					FString::Printf(TEXT("Module input '%s' was requested as a data interface, but its type is '%s'."), *ResolvedInputName, *ResolvedInput->InputVariable.GetType().GetNameText().ToString()));
				return true;
			}

			UClass* DataInterfaceClass = nullptr;
			if (!DataInterfaceClassText.IsEmpty())
			{
				DataInterfaceClass = LoadObject<UClass>(nullptr, *DataInterfaceClassText);
				if (!DataInterfaceClass)
				{
					AddWarning(
						Warnings,
						TEXT("module_input_data_interface_class_not_found"),
						ContextPath,
						FString::Printf(TEXT("Module input '%s' requested data interface class '%s', but it could not be loaded."), *ResolvedInputName, *DataInterfaceClassText));
					return true;
				}
			}
			else
			{
				DataInterfaceClass = ResolvedInput->InputVariable.GetType().GetClass();
			}
			if (!DataInterfaceClass || !DataInterfaceClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
			{
				AddWarning(
					Warnings,
					TEXT("module_input_data_interface_class_invalid"),
					ContextPath,
					FString::Printf(TEXT("Module input '%s' has no valid Niagara data interface class."), *ResolvedInputName));
				return true;
			}

			if (OverridePin->LinkedTo.Num() > 0)
			{
				OverridePin->BreakAllPinLinks();
			}

			const FString InputNodeName = AliasedInputHandle.GetParameterHandleString().ToString();
			UNiagaraDataInterface* DataInterface = nullptr;
			FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(*OverridePin, DataInterfaceClass, InputNodeName, DataInterface);
			if (!DataInterface)
			{
				AddWarning(
					Warnings,
					TEXT("module_input_data_interface_create_failed"),
					ContextPath,
					FString::Printf(TEXT("Module input '%s' failed to create a data interface override node."), *ResolvedInputName));
				return true;
			}

			if (DataInterfaceObj.IsValid())
			{
				DataInterface->Modify();
			}
			Graph.NotifyGraphChanged();
			return true;
		}

		if (!LinkedParameterName.IsEmpty())
		{
			if (Cast<UNiagaraNodeFunctionCall>(OverridePin->GetOwningNode()) != nullptr)
			{
				AddWarning(
					Warnings,
					TEXT("module_input_link_not_supported_for_static_switch"),
					ContextPath + TEXT(".linked_parameter_name"),
					FString::Printf(TEXT("Module input '%s' could not link to '%s' because the override pin belongs to a static switch function call."), *ResolvedInputName, *LinkedParameterName));
				return true;
			}

			if (OverridePin->LinkedTo.Num() > 0)
			{
				OverridePin->BreakAllPinLinks();
			}
			if (const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>())
			{
				NiagaraSchema->TrySetDefaultValue(*OverridePin, FString(), true);
			}
			else
			{
				OverridePin->DefaultValue.Empty();
			}

			TSet<FNiagaraVariableBase> KnownParameters;
			const FNiagaraVariableBase LinkedParameter(ResolvedInput->InputVariable.GetType(), *LinkedParameterName);
			FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(*OverridePin, LinkedParameter, KnownParameters);
			FString LinkedParameterReadback;
			if (!UeAgentNiagaraOps::TryResolveLinkedInputParameterName(OverridePin, LinkedParameterReadback)
				|| !LinkedParameterReadback.Equals(LinkedParameterName, ESearchCase::IgnoreCase))
			{
				AddWarning(
					Warnings,
					TEXT("module_input_link_apply_verification_failed"),
					ContextPath + TEXT(".linked_parameter_name"),
					FString::Printf(
						TEXT("Module input '%s' did not read back linked parameter '%s' after apply; readback='%s'."),
						*ResolvedInputName,
						*LinkedParameterName,
						*LinkedParameterReadback));
			}
			Graph.NotifyGraphChanged();
			return true;
		}

		if (OverridePin->LinkedTo.Num() > 0)
		{
			OverridePin->BreakAllPinLinks();
		}

		FString ValueTextForApply = ValueText;
		FString NormalizeError;
		if (!UeAgentNiagaraOps::NormalizeNiagaraValueTextForPinDefault(ResolvedInput->InputVariable.GetType(), ValueText, ValueTextForApply, NormalizeError))
		{
			AddWarning(
				Warnings,
				TEXT("module_input_invalid_value_text"),
				ContextPath + TEXT(".override_default_value"),
				FString::Printf(TEXT("%s: %s; requested=%s"), *ResolvedInputName, *NormalizeError, *ValueText));
			return true;
		}
		if (const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>())
		{
			NiagaraSchema->TrySetDefaultValue(*OverridePin, ValueTextForApply, true);
		}
		else
		{
			OverridePin->DefaultValue = ValueTextForApply;
		}

		FString ApplyVerificationStatus;
		FString ApplyVerificationError;
		if (!UeAgentNiagaraOps::ValidateNiagaraPinDefaultValueAfterSet(ResolvedInput->InputVariable.GetType(), ValueTextForApply, OverridePin->DefaultValue, ApplyVerificationStatus, ApplyVerificationError))
		{
			AddWarning(
				Warnings,
				TEXT("module_input_default_apply_verification_failed"),
				ContextPath + TEXT(".override_default_value"),
				FString::Printf(
					TEXT("%s: %s; requested=%s; applied=%s; stored=%s"),
					*ResolvedInputName,
					*ApplyVerificationError,
					*ValueText,
					*ValueTextForApply,
					*OverridePin->DefaultValue));
			return true;
		}

		TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputsAfter;
		UeAgentNiagaraOps::BuildResolvedModuleInputs(ModuleNode, ModuleInputsAfter);
		FString ResolvedInputNameAfter;
		const UeAgentNiagaraOps::FResolvedNiagaraModuleInput* ResolvedInputAfter =
			UeAgentNiagaraOps::ResolveModuleInputByName(ModuleInputsAfter, ResolvedInputName, ResolvedInputNameAfter);
		if (!ResolvedInputAfter || !ResolvedInputAfter->InputVariable.IsValid())
		{
			AddWarning(
				Warnings,
				TEXT("module_input_readback_missing_after_apply"),
				ContextPath,
				FString::Printf(TEXT("Module input '%s' could not be resolved after applying its override."), *ResolvedInputName));
		}
		return true;
	}

	static int32 ApplyStagesFromFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("stages_missing_graph"), EmitterFolder, TEXT("Emitter graph source is missing; stages cannot be applied."));
			return 0;
		}

		TSharedPtr<FJsonObject> StagesIndexObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("stages"), TEXT("index.json")), StagesIndexObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!StagesIndexObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(StagesIndexObj, TEXT("stages"), TEXT("stages/index.json.stages"), Stages, Warnings, false))
		{
			return 0;
		}

		int32 Applied = 0;
		int32 StageIndex = 0;
		for (const TSharedPtr<FJsonValue>& StageIndexValue : *Stages)
		{
			TSharedPtr<FJsonObject> StageIndexObj;
			const FString StageIndexPath = FString::Printf(TEXT("stages/index.json.stages[%d]"), StageIndex);
			++StageIndex;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue(StageIndexValue, StageIndexPath, StageIndexObj, Warnings))
			{
				continue;
			}

			FString StageFile;
			if (!UeAgentJsonDiagnostics::ReadStringField(StageIndexObj, TEXT("file"), StageIndexPath + TEXT(".file"), StageFile, Warnings, true) || StageFile.IsEmpty())
			{
				AddWarning(Warnings, TEXT("stage_index_missing_file"), StageIndexPath, TEXT("Stage index entry has no file; it was skipped."));
				continue;
			}

			TSharedPtr<FJsonObject> StageObj;
			if (!LoadJsonFile(FPaths::Combine(EmitterFolder, TEXT("stages"), StageFile), StageObj, OutError))
			{
				return INDEX_NONE;
			}
			const FString StagePath = FString::Printf(TEXT("stages/%s"), *StageFile);
			UeAgentJsonDiagnostics::WarnUnknownFields(
				StageObj,
				StagePath,
				{
					TEXT("file"),
					TEXT("stage_index"),
					TEXT("stage_node_index"),
					TEXT("stage_key"),
					TEXT("stage_type"),
					TEXT("script_usage"),
					TEXT("script_usage_id"),
					TEXT("output_node_guid"),
					TEXT("node_guid"),
					TEXT("node_class"),
					TEXT("node_title"),
					TEXT("node_pos_x"),
					TEXT("node_pos_y"),
					TEXT("is_output_node"),
					TEXT("is_module_node"),
					TEXT("module_name"),
					TEXT("module_script_path"),
					TEXT("module_kind"),
					TEXT("module_has_enabled_state"),
					TEXT("module_enabled"),
					TEXT("input_count"),
					TEXT("inputs"),
					TEXT("modules"),
					TEXT("module_count"),
					TEXT("graph_nodes"),
					TEXT("graph_node_count"),
					TEXT("graph_links"),
					TEXT("graph_link_count"),
					TEXT("custom_hlsl_nodes"),
					TEXT("custom_hlsl_node_count"),
					TEXT("details")
				},
				Warnings);

			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
			FGuid UsageId;
			FString StageKey;
			if (StageObj->TryGetStringField(TEXT("stage_key"), StageKey) && !StageKey.IsEmpty())
			{
				UeAgentNiagaraOps::TryParseStageKey(StageKey, Usage, UsageId);
			}
			if (Usage == ENiagaraScriptUsage::Function)
			{
				FString UsageText;
				StageObj->TryGetStringField(TEXT("script_usage"), UsageText);
				UeAgentNiagaraOps::TryParseScriptUsage(UsageText, Usage);
				FString UsageIdText;
				StageObj->TryGetStringField(TEXT("script_usage_id"), UsageIdText);
				FGuid::Parse(UsageIdText, UsageId);
			}
			if (Usage == ENiagaraScriptUsage::Function)
			{
				AddWarning(Warnings, TEXT("stage_usage_unresolved"), StageFile, TEXT("Stage script usage could not be resolved."));
				continue;
			}

			const FString StageType = GetStringFieldAny(StageObj, { TEXT("stage_type") });
			if (StageType.Equals(TEXT("simulation_stage"), ESearchCase::IgnoreCase))
			{
				UNiagaraSimulationStageBase* SimulationStage = EnsureSimulationStageForFolder(Emitter, EmitterData, *Graph, StageObj, Warnings, OutError);
				if (!SimulationStage)
				{
					return INDEX_NONE;
				}
				int32 StageDetailsApplied = 0;
				if (!ApplyFolderOwnerDetailsIfPresent(
					SimulationStage,
					FPaths::Combine(EmitterFolder, TEXT("stages")),
					StageObj,
					TEXT("niagara_simulation_stage"),
					StageKey,
					TEXT("niagara_emitter_apply_folder"),
					OutError,
					StageDetailsApplied))
				{
					return INDEX_NONE;
				}
				Applied += StageDetailsApplied;
				if (SimulationStage->Script)
				{
					UsageId = SimulationStage->Script->GetUsageId();
				}
			}
			else if (StageType.Equals(TEXT("event_stage"), ESearchCase::IgnoreCase))
			{
				if (!UsageId.IsValid())
				{
					UsageId = FGuid::NewGuid();
				}
				bool bHasEventHandler = false;
				for (const FNiagaraEventScriptProperties& EventHandler : EmitterData.EventHandlerScriptProps)
				{
					if (EventHandler.Script && EventHandler.Script->GetUsageId() == UsageId)
					{
						bHasEventHandler = true;
						break;
					}
				}
				if (!bHasEventHandler)
				{
					TSharedPtr<FJsonObject> MinimalEventObj = MakeShared<FJsonObject>();
					MinimalEventObj->SetStringField(TEXT("script_usage_id"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
					MinimalEventObj->SetStringField(TEXT("source_event_name"), TEXT("Event"));
					FNiagaraEventScriptProperties* EventHandler = nullptr;
					int32 EventIndex = INDEX_NONE;
					if (!EnsureEventHandlerForFolder(Emitter, EmitterData, *Graph, MinimalEventObj, EventHandler, EventIndex, Warnings, OutError))
					{
						return INDEX_NONE;
					}
				}
			}

			UeAgentNiagaraOps::DestroyGraphNodesForStage(*Graph, Usage, UsageId);
			UNiagaraNodeOutput* OutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(*Graph, Usage, UsageId);
			if (!OutputNode)
			{
				AddWarning(Warnings, TEXT("stage_output_bridge_failed"), StageFile, TEXT("Stage output bridge could not be initialized."));
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
			if (UeAgentJsonDiagnostics::ReadArrayField(StageObj, TEXT("modules"), StagePath + TEXT(".modules"), Modules, Warnings, false))
			{
				TArray<TSharedPtr<FJsonValue>> SortedModules = *Modules;
				SortedModules.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
				{
					const TSharedPtr<FJsonObject>* AObj = nullptr;
					const TSharedPtr<FJsonObject>* BObj = nullptr;
					double AIndex = 0.0;
					double BIndex = 0.0;
					if (A.IsValid() && A->TryGetObject(AObj) && AObj && AObj->IsValid())
					{
						(*AObj)->TryGetNumberField(TEXT("module_index"), AIndex);
					}
					if (B.IsValid() && B->TryGetObject(BObj) && BObj && BObj->IsValid())
					{
						(*BObj)->TryGetNumberField(TEXT("module_index"), BIndex);
					}
					return AIndex < BIndex;
				});

				int32 TargetModuleIndex = 0;
				int32 SourceModuleIndex = 0;
				for (const TSharedPtr<FJsonValue>& ModuleValue : SortedModules)
				{
					TSharedPtr<FJsonObject> ModuleObj;
					const FString ModulePath = FString::Printf(TEXT("stages/%s.modules[%d]"), *StageFile, SourceModuleIndex);
					++SourceModuleIndex;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue(ModuleValue, ModulePath, ModuleObj, Warnings))
					{
						continue;
					}

					UeAgentJsonDiagnostics::WarnUnknownFields(
						ModuleObj,
						ModulePath,
						{
							TEXT("module_index"),
							TEXT("module_node_guid"),
							TEXT("path"),
							TEXT("name"),
							TEXT("catalog_name"),
							TEXT("module_name"),
							TEXT("module_script_path"),
							TEXT("module_script_asset_path"),
							TEXT("module_kind"),
							TEXT("module_has_enabled_state"),
							TEXT("module_enabled"),
							TEXT("inputs"),
							TEXT("input_count")
						},
						Warnings);

					FString ModuleScriptPath = GetStringFieldAny(ModuleObj, { TEXT("path"), TEXT("module_script_path"), TEXT("module_script_asset_path") });
					UNiagaraNodeFunctionCall* AddedModule = nullptr;
					if (IsAssignmentModuleFolderObject(ModuleObj))
					{
						AddedModule = AddAssignmentModuleFromFolder(*OutputNode, TargetModuleIndex, ModuleObj, Warnings);
					}
					else
					{
						if (ModuleScriptPath.IsEmpty())
						{
							const FString CatalogName = GetStringFieldAny(ModuleObj, { TEXT("catalog_name"), TEXT("name") });
							if (!CatalogName.TrimStartAndEnd().IsEmpty())
							{
								UeAgentPaletteSearch::FCandidate CatalogCandidate;
								FString ResolveError;
								if (UeAgentPaletteSearch::ResolveCatalogNameToCandidate(TEXT("niagara_module"), CatalogName, CatalogCandidate, ResolveError))
								{
									ModuleScriptPath = CatalogCandidate.FullName;
									if (!ModuleObj->HasField(TEXT("module_name")))
									{
										ModuleObj->SetStringField(TEXT("module_name"), CatalogCandidate.Name);
									}
								}
								else
								{
									AddWarning(Warnings, TEXT("module_catalog_name_not_found"), ModulePath + TEXT(".name"), ResolveError);
								}
							}
						}
						if (ModuleScriptPath.IsEmpty())
						{
							AddWarning(Warnings, TEXT("module_missing_script_path"), ModulePath, TEXT("Script module entry has no path/module_script_path/module_script_asset_path; it was skipped."));
							continue;
						}
						if (IsPrivateInlineNiagaraScriptPath(ModuleScriptPath))
						{
							AddWarning(Warnings, TEXT("private_inline_module_script_skipped"), ModuleScriptPath, TEXT("Private inline module scripts cannot be reused by a standalone emitter without creating illegal package references."));
							continue;
						}

						UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(ModuleScriptPath));
						if (!ModuleScript)
						{
							AddWarning(Warnings, TEXT("module_script_not_found"), ModuleScriptPath, TEXT("Module could not be reconstructed because the referenced script asset was not found."));
							continue;
						}

						FString SuggestedName;
						ModuleObj->TryGetStringField(TEXT("module_name"), SuggestedName);
						AddedModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
							ModuleScript,
							*OutputNode,
							TargetModuleIndex,
							SuggestedName,
							ModuleScript->GetExposedVersion().VersionGuid);
					}
					if (!AddedModule)
					{
						AddWarning(Warnings, TEXT("module_recreate_failed"), ModuleScriptPath, TEXT("Module reconstruction returned null."));
						continue;
					}

					bool bModuleEnabled = true;
					if (ModuleObj->TryGetBoolField(TEXT("module_enabled"), bModuleEnabled))
					{
						FNiagaraStackGraphUtilities::SetModuleIsEnabled(*AddedModule, bModuleEnabled);
					}

					const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
					if (UeAgentJsonDiagnostics::ReadArrayField(ModuleObj, TEXT("inputs"), ModulePath + TEXT(".inputs"), Inputs, Warnings, false))
					{
						int32 InputIndex = 0;
						for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
						{
							TSharedPtr<FJsonObject> InputObj;
							const FString InputPath = FString::Printf(TEXT("%s.inputs[%d]"), *ModulePath, InputIndex);
							++InputIndex;
							if (UeAgentJsonDiagnostics::ReadObjectFromValue(InputValue, InputPath, InputObj, Warnings))
							{
								ApplyModuleInputDefault(*Graph, *AddedModule, InputObj, InputPath, Warnings);
							}
						}
					}
					++Applied;
					++TargetModuleIndex;
				}
			}

			++Applied;
		}
		return Applied;
	}

	static int32 ApplyEmitterFolder(UNiagaraEmitter& Emitter, const FGuid& VersionGuid, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError, const bool bApplyDataInterfacesAfterLocalRefresh = true)
	{
		FVersionedNiagaraEmitterData* EmitterData = VersionGuid.IsValid() ? Emitter.GetEmitterData(VersionGuid) : Emitter.GetLatestEmitterData();
		if (!EmitterData)
		{
			AddWarning(Warnings, TEXT("emitter_apply_missing_version_data"), EmitterFolder, TEXT("Emitter version data is missing."));
			return 0;
		}

		int32 Applied = 0;
		TSharedPtr<FJsonObject> EmitterObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("emitter.json")), EmitterObj, OutError))
		{
			return INDEX_NONE;
		}
		if (EmitterObj.IsValid())
		{
			bool bHasEmitterDetails = false;
			TSharedPtr<FJsonObject> EmitterDetailsPropertiesJson;
			if (!LoadGroupedDetailsForRef(EmitterFolder, EmitterObj, TEXT("details"), bHasEmitterDetails, EmitterDetailsPropertiesJson, OutError))
			{
				return INDEX_NONE;
			}
			const bool bApplyInterpolatedSpawnSideEffects = bHasEmitterDetails
				&& JsonPropertyArrayContainsProperty(EmitterDetailsPropertiesJson, TEXT("properties"), TEXT("InterpolatedSpawnMode"));
			if (bHasEmitterDetails)
			{
				Applied += ApplyStructPropertyArray(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, &Emitter, EmitterDetailsPropertiesJson, TEXT("properties"), Warnings, Emitter.GetPathName());
			}
			if (bApplyInterpolatedSpawnSideEffects)
			{
				const UeAgentNiagaraOps::FEmitterPropertySideEffectResult SideEffects = UeAgentNiagaraOps::ApplyEmitterPropertySideEffects(EmitterData, TEXT("InterpolatedSpawnMode"));
				if (SideEffects.bHandled)
				{
					++Applied;
				}
			}
		}

		const int32 ParametersApplied = ApplyEmitterGraphParameters(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
		if (ParametersApplied == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		Applied += ParametersApplied;

		TSharedPtr<FJsonObject> RenderersObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("renderers.json")), RenderersObj, OutError))
		{
			return INDEX_NONE;
		}
		if (RenderersObj.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Renderers = nullptr;
			if (TryGetArrayField(RenderersObj, TEXT("renderers"), Renderers))
			{
				bool bReplaceRenderers = false;
				RenderersObj->TryGetBoolField(TEXT("replace_renderers"), bReplaceRenderers);
				FString RendererApplyMode;
				if (RenderersObj->TryGetStringField(TEXT("apply_mode"), RendererApplyMode) &&
					RendererApplyMode.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
				{
					bReplaceRenderers = true;
				}
				if (bReplaceRenderers)
				{
					const TArray<UNiagaraRendererProperties*> ExistingRenderers = EmitterData->GetRenderers();
					for (int32 ExistingRendererIndex = ExistingRenderers.Num() - 1; ExistingRendererIndex >= 0; --ExistingRendererIndex)
					{
						UNiagaraRendererProperties* ExistingRenderer = ExistingRenderers[ExistingRendererIndex];
						if (!ExistingRenderer)
						{
							continue;
						}
						Emitter.RemoveRenderer(ExistingRenderer, EmitterData->Version.VersionGuid);
						++Applied;
					}
				}
				for (const TSharedPtr<FJsonValue>& RendererValue : *Renderers)
				{
					const TSharedPtr<FJsonObject>* RendererObjPtr = nullptr;
					if (!RendererValue.IsValid() || !RendererValue->TryGetObject(RendererObjPtr) || !RendererObjPtr || !RendererObjPtr->IsValid())
					{
						continue;
					}

					const TSharedPtr<FJsonObject>& RendererObj = *RendererObjPtr;
					int32 RendererIndex = INDEX_NONE;
					double RendererIndexNumber = 0.0;
					if (RendererObj->TryGetNumberField(TEXT("renderer_index"), RendererIndexNumber))
					{
						RendererIndex = FMath::RoundToInt(RendererIndexNumber);
					}

					const TArray<UNiagaraRendererProperties*>& RenderersArray = EmitterData->GetRenderers();
					UNiagaraRendererProperties* Renderer = RenderersArray.IsValidIndex(RendererIndex) ? RenderersArray[RendererIndex] : nullptr;
					if (!Renderer)
					{
						FString RendererClassText;
						if (RendererObj->TryGetStringField(TEXT("renderer_class"), RendererClassText))
						{
							UClass* RendererClass = UeAgentNiagaraOps::ResolveRendererClass(RendererClassText);
							if (RendererClass)
							{
								Renderer = NewObject<UNiagaraRendererProperties>(&Emitter, RendererClass, NAME_None, RF_Transactional);
								if (Renderer)
								{
									Emitter.AddRenderer(Renderer, EmitterData->Version.VersionGuid);
									RendererIndex = EmitterData->GetRenderers().Find(Renderer);
								}
							}
						}
					}

					if (!Renderer)
					{
						AddWarning(Warnings, TEXT("renderer_apply_missing_target"), FString::Printf(TEXT("%s[%d]"), *EmitterFolder, RendererIndex), TEXT("Renderer was not found and could not be created."));
						continue;
					}

					if (!ApplyFolderOwnerDetailsIfPresent(
						Renderer,
						EmitterFolder,
						RendererObj,
						TEXT("niagara_renderer"),
						FString::Printf(TEXT("%s.renderer_%d"), *FPaths::GetCleanFilename(EmitterFolder), RendererIndex),
						TEXT("niagara_emitter_apply_folder"),
						OutError,
						Applied))
					{
						return INDEX_NONE;
					}
				}
			}
		}

		const int32 EventGeneratorsApplied = ApplyEventGeneratorsFromFolder(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
		if (EventGeneratorsApplied == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		Applied += EventGeneratorsApplied;

		const int32 PrunedDynamicStages = PruneDynamicStagesMissingFromFolder(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
		if (PrunedDynamicStages == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		Applied += PrunedDynamicStages;

		const int32 EventHandlersApplied = ApplyEventHandlersFromFolder(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
		if (EventHandlersApplied == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		Applied += EventHandlersApplied;

		const int32 StagesApplied = ApplyStagesFromFolder(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
		if (StagesApplied == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		Applied += StagesApplied;

		if (EmitterData->GraphSource)
		{
			EmitterData->GraphSource->MarkNotSynchronized(TEXT("Niagara folder apply"));
		}
		EmitterData->InvalidateCompileResults();
		Emitter.UpdateEmitterAfterLoad();

		if (bApplyDataInterfacesAfterLocalRefresh)
		{
			const int32 DataInterfacesApplied = ApplyDataInterfacesFromFolder(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
			if (DataInterfacesApplied == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			Applied += DataInterfacesApplied;
		}
		Emitter.MarkPackageDirty();
		return Applied;
	}

	// System-level data interface and stage apply helpers are split for navigation.
	static int32 ApplySystemDataInterfacesFromFolder(UNiagaraSystem& NiagaraSystem, const FString& FolderPath, const TSharedPtr<FJsonObject>& EmittersIndexObj, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		if (!EmittersIndexObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
		if (!TryGetArrayField(EmittersIndexObj, TEXT("emitters"), Emitters))
		{
			return 0;
		}

		int32 Applied = 0;
		for (int32 EmitterIndex = 0; EmitterIndex < Emitters->Num(); ++EmitterIndex)
		{
			const FString EmitterPath = FString::Printf(TEXT("%s/emitters/index.json.emitters[%d]"), *FolderPath, EmitterIndex);
			TSharedPtr<FJsonObject> EmitterObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Emitters)[EmitterIndex], EmitterPath, EmitterObj, Warnings))
			{
				continue;
			}

			const FString EmitterName = GetStringFieldAny(EmitterObj, { TEXT("emitter_name"), TEXT("name") }, FString::Printf(TEXT("Emitter_%d"), EmitterIndex));
			FString EmitterFolderRelative;
			EmitterObj->TryGetStringField(TEXT("folder"), EmitterFolderRelative);
			if (EmitterName.IsEmpty() || EmitterFolderRelative.IsEmpty())
			{
				continue;
			}

			FNiagaraEmitterHandle* ExistingHandle = nullptr;
			for (FNiagaraEmitterHandle& Handle : NiagaraSystem.GetEmitterHandles())
			{
				if (Handle.GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
				{
					ExistingHandle = &Handle;
					break;
				}
			}
			if (!ExistingHandle)
			{
				AddWarning(Warnings, TEXT("data_interface_emitter_handle_not_found"), EmitterPath, FString::Printf(TEXT("Emitter handle '%s' was not found while applying post-refresh data interfaces."), *EmitterName));
				continue;
			}

			FVersionedNiagaraEmitter VersionedEmitter = ExistingHandle->GetInstance();
			if (!VersionedEmitter.Emitter)
			{
				AddWarning(Warnings, TEXT("data_interface_emitter_instance_missing"), EmitterPath, FString::Printf(TEXT("Emitter handle '%s' has no emitter instance while applying post-refresh data interfaces."), *EmitterName));
				continue;
			}

			FVersionedNiagaraEmitterData* EmitterData = VersionedEmitter.Version.IsValid()
				? VersionedEmitter.Emitter->GetEmitterData(VersionedEmitter.Version)
				: VersionedEmitter.Emitter->GetLatestEmitterData();
			if (!EmitterData)
			{
				AddWarning(Warnings, TEXT("data_interface_emitter_data_missing"), EmitterPath, FString::Printf(TEXT("Emitter handle '%s' has no version data while applying post-refresh data interfaces."), *EmitterName));
				continue;
			}

			const FString EmitterFolder = FPaths::Combine(FolderPath, TEXT("emitters"), EmitterFolderRelative);
			const int32 DataInterfacesApplied = ApplyDataInterfacesFromFolder(*VersionedEmitter.Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
			if (DataInterfacesApplied == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			Applied += DataInterfacesApplied;
			if (DataInterfacesApplied > 0)
			{
				VersionedEmitter.Emitter->MarkPackageDirty();
			}
		}

		return Applied;
	}

	static bool ResolveSystemStageUsageFromJson(const TSharedPtr<FJsonObject>& StageObj, const FString& StageName, ENiagaraScriptUsage& OutUsage, FGuid& OutUsageId)
	{
		OutUsage = ENiagaraScriptUsage::Function;
		OutUsageId.Invalidate();

		if (StageObj.IsValid())
		{
			FString StageKey;
			if (StageObj->TryGetStringField(TEXT("stage_key"), StageKey) && !StageKey.IsEmpty())
			{
				UeAgentNiagaraOps::TryParseStageKey(StageKey, OutUsage, OutUsageId);
			}
			if (!UeAgentNiagaraOps::IsSystemStageUsage(OutUsage))
			{
				FString UsageText;
				if (StageObj->TryGetStringField(TEXT("script_usage"), UsageText))
				{
					UeAgentNiagaraOps::TryParseScriptUsage(UsageText, OutUsage);
				}
				FString UsageIdText;
				if (StageObj->TryGetStringField(TEXT("script_usage_id"), UsageIdText))
				{
					FGuid::Parse(UsageIdText, OutUsageId);
				}
			}
		}

		if (!UeAgentNiagaraOps::IsSystemStageUsage(OutUsage))
		{
			if (StageName.Equals(TEXT("SystemSpawn"), ESearchCase::IgnoreCase))
			{
				OutUsage = ENiagaraScriptUsage::SystemSpawnScript;
			}
			else if (StageName.Equals(TEXT("SystemUpdate"), ESearchCase::IgnoreCase))
			{
				OutUsage = ENiagaraScriptUsage::SystemUpdateScript;
			}
		}

		if (!OutUsageId.IsValid())
		{
			OutUsageId = FGuid();
		}
		return UeAgentNiagaraOps::IsSystemStageUsage(OutUsage);
	}

	static int32 ApplySystemStageJsonToGraph(UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& StageObj, const FString& StageName, const FString& ContextPath, TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		if (!StageObj.IsValid())
		{
			return 0;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(
			StageObj,
			ContextPath,
			{
				TEXT("stage_name"),
				TEXT("stage_index"),
				TEXT("stage_key"),
				TEXT("stage_type"),
				TEXT("script_usage"),
				TEXT("script_usage_id"),
				TEXT("output_node_guid"),
				TEXT("script"),
				TEXT("node_guid"),
				TEXT("node_class"),
				TEXT("node_title"),
				TEXT("node_pos_x"),
				TEXT("node_pos_y"),
				TEXT("is_output_node"),
				TEXT("is_module_node"),
				TEXT("module_name"),
				TEXT("module_script_path"),
				TEXT("module_kind"),
				TEXT("module_has_enabled_state"),
				TEXT("module_enabled"),
				TEXT("input_count"),
				TEXT("inputs"),
				TEXT("modules"),
				TEXT("module_count"),
				TEXT("graph_nodes"),
				TEXT("graph_node_count"),
				TEXT("graph_links"),
				TEXT("graph_link_count"),
				TEXT("custom_hlsl_nodes"),
				TEXT("custom_hlsl_node_count"),
				TEXT("coverage_note")
			},
			Warnings);

		const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(StageObj, TEXT("modules"), ContextPath + TEXT(".modules"), Modules, Warnings, false))
		{
			return 0;
		}

		ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
		FGuid UsageId;
		if (!ResolveSystemStageUsageFromJson(StageObj, StageName, Usage, UsageId))
		{
			AddWarning(Warnings, TEXT("system_stage_usage_unresolved"), ContextPath, TEXT("System stage usage could not be resolved; stage was skipped."));
			return 0;
		}

		UeAgentNiagaraOps::DestroyGraphNodesForStage(Graph, Usage, UsageId);
		UNiagaraNodeOutput* OutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(Graph, Usage, UsageId);
		if (!OutputNode)
		{
			AddWarning(Warnings, TEXT("system_stage_output_bridge_failed"), ContextPath, TEXT("System stage output bridge could not be initialized."));
			return 0;
		}

		TArray<TSharedPtr<FJsonValue>> SortedModules = *Modules;
		SortedModules.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject>* AObj = nullptr;
			const TSharedPtr<FJsonObject>* BObj = nullptr;
			double AIndex = 0.0;
			double BIndex = 0.0;
			if (A.IsValid() && A->TryGetObject(AObj) && AObj && AObj->IsValid())
			{
				(*AObj)->TryGetNumberField(TEXT("module_index"), AIndex);
			}
			if (B.IsValid() && B->TryGetObject(BObj) && BObj && BObj->IsValid())
			{
				(*BObj)->TryGetNumberField(TEXT("module_index"), BIndex);
			}
			return AIndex < BIndex;
		});

		int32 Applied = 0;
		int32 TargetModuleIndex = 0;
		int32 SourceModuleIndex = 0;
		for (const TSharedPtr<FJsonValue>& ModuleValue : SortedModules)
		{
			TSharedPtr<FJsonObject> ModuleObj;
			const FString ModulePath = FString::Printf(TEXT("%s.modules[%d]"), *ContextPath, SourceModuleIndex);
			++SourceModuleIndex;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue(ModuleValue, ModulePath, ModuleObj, Warnings))
			{
				continue;
			}

			UeAgentJsonDiagnostics::WarnUnknownFields(
				ModuleObj,
				ModulePath,
				{
					TEXT("module_index"),
					TEXT("module_node_guid"),
					TEXT("path"),
					TEXT("module_name"),
					TEXT("module_script_path"),
					TEXT("module_script_asset_path"),
					TEXT("module_kind"),
					TEXT("module_has_enabled_state"),
					TEXT("module_enabled"),
					TEXT("inputs"),
					TEXT("input_count")
				},
				Warnings);

			FString ModuleScriptPath = GetStringFieldAny(ModuleObj, { TEXT("path"), TEXT("module_script_path"), TEXT("module_script_asset_path") });
			UNiagaraNodeFunctionCall* AddedModule = nullptr;
			if (IsAssignmentModuleFolderObject(ModuleObj))
			{
				AddedModule = AddAssignmentModuleFromFolder(*OutputNode, TargetModuleIndex, ModuleObj, Warnings);
			}
			else
			{
				if (ModuleScriptPath.IsEmpty())
				{
					const FString CatalogName = GetStringFieldAny(ModuleObj, { TEXT("catalog_name"), TEXT("name") });
					if (!CatalogName.TrimStartAndEnd().IsEmpty())
					{
						UeAgentPaletteSearch::FCandidate CatalogCandidate;
						FString ResolveError;
						if (UeAgentPaletteSearch::ResolveCatalogNameToCandidate(TEXT("niagara_module"), CatalogName, CatalogCandidate, ResolveError))
						{
							ModuleScriptPath = CatalogCandidate.FullName;
							if (!ModuleObj->HasField(TEXT("module_name")))
							{
								ModuleObj->SetStringField(TEXT("module_name"), CatalogCandidate.Name);
							}
						}
						else
						{
							AddWarning(Warnings, TEXT("system_stage_module_catalog_name_not_found"), ModulePath + TEXT(".name"), ResolveError);
						}
					}
					if (ModuleScriptPath.IsEmpty())
					{
						AddWarning(Warnings, TEXT("system_stage_module_missing_script_path"), ModulePath, TEXT("Script module entry has no path/module_script_path/module_script_asset_path; it was skipped."));
						continue;
					}
				}
				if (IsPrivateInlineNiagaraScriptPath(ModuleScriptPath))
				{
					AddWarning(Warnings, TEXT("system_stage_private_inline_module_script_skipped"), ModuleScriptPath, TEXT("Private inline module scripts cannot be reused safely in a system stage folder apply."));
					continue;
				}

				UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(ModuleScriptPath));
				if (!ModuleScript)
				{
					AddWarning(Warnings, TEXT("system_stage_module_script_not_found"), ModuleScriptPath, TEXT("System stage module could not be reconstructed because the referenced script asset was not found."));
					continue;
				}

				FString SuggestedName;
				ModuleObj->TryGetStringField(TEXT("module_name"), SuggestedName);
				AddedModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
					ModuleScript,
					*OutputNode,
					TargetModuleIndex,
					SuggestedName,
					ModuleScript->GetExposedVersion().VersionGuid);
			}

			if (!AddedModule)
			{
				AddWarning(Warnings, TEXT("system_stage_module_recreate_failed"), ModuleScriptPath, TEXT("System stage module reconstruction returned null."));
				continue;
			}

			bool bModuleEnabled = true;
			if (ModuleObj->TryGetBoolField(TEXT("module_enabled"), bModuleEnabled))
			{
				FNiagaraStackGraphUtilities::SetModuleIsEnabled(*AddedModule, bModuleEnabled);
			}

			const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
			if (UeAgentJsonDiagnostics::ReadArrayField(ModuleObj, TEXT("inputs"), ModulePath + TEXT(".inputs"), Inputs, Warnings, false))
			{
				int32 InputIndex = 0;
				for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
				{
					TSharedPtr<FJsonObject> InputObj;
					const FString InputPath = FString::Printf(TEXT("%s.inputs[%d]"), *ModulePath, InputIndex);
					++InputIndex;
					if (UeAgentJsonDiagnostics::ReadObjectFromValue(InputValue, InputPath, InputObj, Warnings))
					{
						ApplyModuleInputDefault(Graph, *AddedModule, InputObj, InputPath, Warnings);
					}
				}
			}

			++Applied;
			++TargetModuleIndex;
		}

		Graph.NotifyGraphChanged();
		return Applied;
	}

	static int32 ApplySystemStagesFromFolder(UNiagaraSystem& NiagaraSystem, const FString& FolderPath, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		UNiagaraScript* SystemSpawnScript = NiagaraSystem.GetSystemSpawnScript();
		UNiagaraScriptSource* ScriptSource = SystemSpawnScript ? Cast<UNiagaraScriptSource>(SystemSpawnScript->GetLatestSource()) : nullptr;
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("system_stages_missing_graph"), FolderPath, TEXT("System graph source is missing; system stages cannot be applied."));
			return 0;
		}

		int32 Applied = 0;
		const TArray<TPair<FString, FString>> StageFiles = {
			TPair<FString, FString>(TEXT("SystemSpawn"), TEXT("SystemSpawn.json")),
			TPair<FString, FString>(TEXT("SystemUpdate"), TEXT("SystemUpdate.json"))
		};
		for (const TPair<FString, FString>& StageFile : StageFiles)
		{
			TSharedPtr<FJsonObject> StageObj;
			const FString StageFilePath = FPaths::Combine(FolderPath, TEXT("system_stages"), StageFile.Value);
			if (!LoadJsonFileOptional(StageFilePath, StageObj, OutError))
			{
				return INDEX_NONE;
			}
			if (!StageObj.IsValid())
			{
				continue;
			}
			Applied += ApplySystemStageJsonToGraph(*Graph, StageObj, StageFile.Key, FString::Printf(TEXT("system_stages/%s"), *StageFile.Value), Warnings);
		}

		if (Applied > 0)
		{
			NiagaraSystem.MarkPackageDirty();
		}
		return Applied;
	}


}

bool FUeAgentHttpServer::CmdNiagaraExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentNiagaraFolderOps::ResolveFolderForAsset(NiagaraSystem->GetOutermost()->GetName(), FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	bool bIncludeRawProperties = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRawProperties);
	UeAgentNiagaraFolderOps::FScopedIncludeRawPropertiesInFolderExport IncludeRawPropertiesScope(bIncludeRawProperties);
	if (bCleanOutputDir)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	TArray<FString> WrittenFiles;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;
	TMap<FString, TSharedPtr<FJsonObject>> ScriptRefs;

	const FString AssetPath = NiagaraSystem->GetOutermost()->GetName();

	TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
	AssetObj->SetNumberField(TEXT("format_version"), UeAgentNiagaraFolderOps::NiagaraFolderFormatVersion);
	AssetObj->SetStringField(TEXT("asset_kind"), TEXT("niagara_system"));
	AssetObj->SetStringField(TEXT("asset_path"), AssetPath);
	AssetObj->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	AssetObj->SetStringField(TEXT("class"), NiagaraSystem->GetClass()->GetPathName());
	AssetObj->SetStringField(TEXT("engine_target"), TEXT("UE_5.6"));
	AssetObj->SetStringField(TEXT("root_folder_kind"), TEXT("NiagaraSystem"));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetObj, &WrittenFiles))
	{
		OutError = TEXT("save_asset_json_failed");
		return false;
	}

	TSharedPtr<FJsonObject> SystemSettingsObj = MakeShared<FJsonObject>();
	SystemSettingsObj->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	if (!UeAgentNiagaraFolderOps::ExportFolderOwnerDetails(
		NiagaraSystem,
		FPaths::Combine(FolderPath, TEXT("settings")),
		TEXT("niagara_export_folder"),
		TEXT("niagara_system_settings"),
		TEXT("system"),
		TEXT("system"),
		SystemSettingsObj,
		OutError))
	{
		return false;
	}
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("system.json")), SystemSettingsObj, &WrittenFiles))
	{
		OutError = TEXT("save_system_settings_failed");
		return false;
	}

	const TArray<FString> SettingsFiles = {
		TEXT("asset_options.json"),
		TEXT("effect_type.json"),
		TEXT("scalability.json"),
		TEXT("fixed_bounds.json"),
		TEXT("performance.json"),
		TEXT("baker.json")
	};
	for (const FString& SettingsFile : SettingsFiles)
	{
		TSharedPtr<FJsonObject> PlaceholderObj = MakeShared<FJsonObject>();
		PlaceholderObj->SetNumberField(TEXT("format_version"), UeAgentNiagaraFolderOps::NiagaraFolderFormatVersion);
		PlaceholderObj->SetStringField(TEXT("asset_path"), AssetPath);
		if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), SettingsFile), PlaceholderObj, &WrittenFiles))
		{
			OutError = TEXT("save_settings_extension_failed");
			return false;
		}
	}

	TArray<FNiagaraVariable> UserParameters;
	NiagaraSystem->GetExposedParameters().GetUserParameters(UserParameters);
	UserParameters.Sort([](const FNiagaraVariable& A, const FNiagaraVariable& B)
	{
		return A.GetName().LexicalLess(B.GetName());
	});
	TArray<TSharedPtr<FJsonValue>> UserParamsJson;
	for (const FNiagaraVariable& Parameter : UserParameters)
	{
		UserParamsJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildUserParameterJson(NiagaraSystem->GetExposedParameters(), Parameter, true)));
	}
	TSharedPtr<FJsonObject> UserParamsObj = MakeShared<FJsonObject>();
	UserParamsObj->SetArrayField(TEXT("parameters"), UserParamsJson);
	UserParamsObj->SetNumberField(TEXT("parameter_count"), UserParamsJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("parameters"), TEXT("user_parameters.json")), UserParamsObj, &WrittenFiles))
	{
		OutError = TEXT("save_user_parameters_failed");
		return false;
	}
	Stats.Add(TEXT("user_parameter_count"), UserParamsJson.Num());

	const TArray<FString> ParameterFiles = {
		TEXT("system_parameters.json"),
		TEXT("parameter_definitions.json"),
		TEXT("parameter_collections.json"),
		TEXT("parameter_collection_overrides.json")
	};
	for (const FString& ParameterFile : ParameterFiles)
	{
		TSharedPtr<FJsonObject> ParameterObj = MakeShared<FJsonObject>();
		ParameterObj->SetArrayField(TEXT("items"), TArray<TSharedPtr<FJsonValue>>());
		if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("parameters"), ParameterFile), ParameterObj, &WrittenFiles))
		{
			OutError = TEXT("save_parameter_extension_failed");
			return false;
		}
	}

	UeAgentNiagaraFolderOps::AddUniqueScriptReference(ScriptRefs, NiagaraSystem->GetSystemSpawnScript(), TEXT("system_stage"), TEXT("SystemSpawn"));
	UeAgentNiagaraFolderOps::AddUniqueScriptReference(ScriptRefs, NiagaraSystem->GetSystemUpdateScript(), TEXT("system_stage"), TEXT("SystemUpdate"));
	TSharedPtr<FJsonObject> SystemSpawnJson = UeAgentNiagaraFolderOps::BuildSystemStageJson(
		NiagaraSystem->GetSystemSpawnScript(),
		TEXT("SystemSpawn"),
		ScriptRefs,
		FPaths::Combine(FolderPath, TEXT("system_stages")),
		TEXT("niagara_export_folder"),
		OutError);
	if (!SystemSpawnJson.IsValid() || !UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("system_stages"), TEXT("SystemSpawn.json")), SystemSpawnJson, &WrittenFiles))
	{
		OutError = TEXT("save_system_spawn_failed");
		return false;
	}
	TSharedPtr<FJsonObject> SystemUpdateJson = UeAgentNiagaraFolderOps::BuildSystemStageJson(
		NiagaraSystem->GetSystemUpdateScript(),
		TEXT("SystemUpdate"),
		ScriptRefs,
		FPaths::Combine(FolderPath, TEXT("system_stages")),
		TEXT("niagara_export_folder"),
		OutError);
	if (!SystemUpdateJson.IsValid() || !UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("system_stages"), TEXT("SystemUpdate.json")), SystemUpdateJson, &WrittenFiles))
	{
		OutError = TEXT("save_system_update_failed");
		return false;
	}

	const TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
	TArray<TSharedPtr<FJsonValue>> EmitterIndexJson;
	for (int32 EmitterIndex = 0; EmitterIndex < Handles.Num(); ++EmitterIndex)
	{
		const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
		const FString EmitterFolderName = UeAgentNiagaraFolderOps::MakeEmitterFolderName(Handle.GetName().ToString(), Handle.GetId(), EmitterIndex);
		TSharedPtr<FJsonObject> EmitterEntry;
		if (!UeAgentNiagaraFolderOps::ExportEmitterFolder(
			FPaths::Combine(FolderPath, TEXT("emitters"), EmitterFolderName),
			EmitterFolderName,
			EmitterIndex,
			Handle,
			TEXT("niagara_export_folder"),
			WrittenFiles,
			Warnings,
			ScriptRefs,
			EmitterEntry,
			OutError))
		{
			OutError = TEXT("save_emitter_folder_failed");
			return false;
		}
		EmitterIndexJson.Add(MakeShared<FJsonValueObject>(EmitterEntry));
	}
	TSharedPtr<FJsonObject> EmittersIndexObj = MakeShared<FJsonObject>();
	EmittersIndexObj->SetArrayField(TEXT("emitters"), EmitterIndexJson);
	EmittersIndexObj->SetNumberField(TEXT("emitter_count"), EmitterIndexJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("emitters"), TEXT("index.json")), EmittersIndexObj, &WrittenFiles))
	{
		OutError = TEXT("save_emitters_index_failed");
		return false;
	}
	Stats.Add(TEXT("emitter_count"), EmitterIndexJson.Num());

	TArray<TSharedPtr<FJsonValue>> ScriptsJson;
	for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : ScriptRefs)
	{
		if (Pair.Value.IsValid())
		{
			ScriptsJson.Add(MakeShared<FJsonValueObject>(Pair.Value));
		}
	}
	TSharedPtr<FJsonObject> ScriptsIndexObj = MakeShared<FJsonObject>();
	ScriptsIndexObj->SetArrayField(TEXT("scripts"), ScriptsJson);
	ScriptsIndexObj->SetNumberField(TEXT("script_count"), ScriptsJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("scripts"), TEXT("index.json")), ScriptsIndexObj, &WrittenFiles))
	{
		OutError = TEXT("save_scripts_index_failed");
		return false;
	}
	Stats.Add(TEXT("script_reference_count"), ScriptsJson.Num());

	TSharedPtr<FJsonObject> ChecksObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ChecksJson;
	TSharedPtr<FJsonObject> CompileCheckObj = MakeShared<FJsonObject>();
	CompileCheckObj->SetStringField(TEXT("kind"), TEXT("compile_success"));
	CompileCheckObj->SetStringField(TEXT("command"), TEXT("niagara_compile_system"));
	ChecksJson.Add(MakeShared<FJsonValueObject>(CompileCheckObj));
	ChecksObj->SetArrayField(TEXT("checks"), ChecksJson);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("checks.json")), ChecksObj, &WrittenFiles))
	{
		OutError = TEXT("save_validation_checks_failed");
		return false;
	}

	TSharedPtr<FJsonObject> CompileSummaryObj = MakeShared<FJsonObject>();
	CompileSummaryObj->SetStringField(TEXT("asset_path"), AssetPath);
	CompileSummaryObj->SetStringField(TEXT("coverage_note"), TEXT("Run niagara_get_compile_log with compile_before_read=true for live compile diagnostics."));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("compile_summary.json")), CompileSummaryObj, &WrittenFiles))
	{
		OutError = TEXT("save_compile_summary_failed");
		return false;
	}

	TSharedPtr<FJsonObject> DependencyReportObj = MakeShared<FJsonObject>();
	DependencyReportObj->SetArrayField(TEXT("script_references"), ScriptsJson);
	DependencyReportObj->SetNumberField(TEXT("script_reference_count"), ScriptsJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("dependency_report.json")), DependencyReportObj, &WrittenFiles))
	{
		OutError = TEXT("save_dependency_report_failed");
		return false;
	}

	TSharedPtr<FJsonObject> RuntimePreviewObj = MakeShared<FJsonObject>();
	RuntimePreviewObj->SetStringField(TEXT("asset_path"), AssetPath);
	RuntimePreviewObj->SetStringField(TEXT("coverage_note"), TEXT("Runtime component overrides, Sequencer tracks and SimCache output are outside the UNiagaraSystem asset folder scope."));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("runtime_preview.json")), RuntimePreviewObj, &WrittenFiles))
	{
		OutError = TEXT("save_runtime_preview_failed");
		return false;
	}

	Stats.Add(TEXT("file_count"), WrittenFiles.Num() + 1);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildCoverageReport(AssetPath, TEXT("export"), Warnings, Stats);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, &WrittenFiles))
	{
		OutError = TEXT("save_coverage_report_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), WrittenFiles.Num());
	OutData->SetNumberField(TEXT("emitter_count"), EmitterIndexJson.Num());
	OutData->SetNumberField(TEXT("user_parameter_count"), UserParamsJson.Num());
	OutData->SetNumberField(TEXT("script_reference_count"), ScriptsJson.Num());
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("emitter_asset_path"), AssetPathInput);
	if (AssetPathInput.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	}
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_emitter_asset_path");
		return false;
	}

	UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraEmitter)
	{
		OutError = TEXT("emitter_asset_not_found");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentNiagaraFolderOps::ResolveEmitterFolderForAsset(NiagaraEmitter->GetOutermost()->GetName(), FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	bool bIncludeRawProperties = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRawProperties);
	UeAgentNiagaraFolderOps::FScopedIncludeRawPropertiesInFolderExport IncludeRawPropertiesScope(bIncludeRawProperties);
	if (bCleanOutputDir)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	TArray<FString> WrittenFiles;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;
	TMap<FString, TSharedPtr<FJsonObject>> ScriptRefs;

	const FString AssetPath = NiagaraEmitter->GetOutermost()->GetName();
	const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
	const FGuid ExportVersion = NiagaraEmitter->GetExposedVersion().VersionGuid;

	TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
	AssetObj->SetNumberField(TEXT("format_version"), UeAgentNiagaraFolderOps::NiagaraFolderFormatVersion);
	AssetObj->SetStringField(TEXT("asset_kind"), TEXT("niagara_emitter"));
	AssetObj->SetStringField(TEXT("asset_path"), AssetPath);
	AssetObj->SetStringField(TEXT("object_path"), NiagaraEmitter->GetPathName());
	AssetObj->SetStringField(TEXT("class"), NiagaraEmitter->GetClass()->GetPathName());
	AssetObj->SetStringField(TEXT("engine_target"), TEXT("UE_5.6"));
	AssetObj->SetStringField(TEXT("root_folder_kind"), TEXT("NiagaraEmitter"));
	AssetObj->SetStringField(TEXT("emitter_version"), ExportVersion.ToString(EGuidFormats::DigitsWithHyphensLower));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetObj, &WrittenFiles))
	{
		OutError = TEXT("save_asset_json_failed");
		return false;
	}

	FNiagaraEmitterHandle StandaloneHandle(*NiagaraEmitter, ExportVersion);
	TSharedPtr<FJsonObject> EmitterIndexObj;
	if (!UeAgentNiagaraFolderOps::ExportEmitterFolder(
		FolderPath,
		AssetName,
		0,
		StandaloneHandle,
		TEXT("niagara_emitter_export_folder"),
		WrittenFiles,
		Warnings,
		ScriptRefs,
		EmitterIndexObj,
		OutError))
	{
		OutError = TEXT("save_emitter_folder_failed");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ScriptsJson;
	for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : ScriptRefs)
	{
		if (Pair.Value.IsValid())
		{
			ScriptsJson.Add(MakeShared<FJsonValueObject>(Pair.Value));
		}
	}
	TSharedPtr<FJsonObject> ScriptsIndexObj = MakeShared<FJsonObject>();
	ScriptsIndexObj->SetArrayField(TEXT("scripts"), ScriptsJson);
	ScriptsIndexObj->SetNumberField(TEXT("script_count"), ScriptsJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("scripts"), TEXT("index.json")), ScriptsIndexObj, &WrittenFiles))
	{
		OutError = TEXT("save_scripts_index_failed");
		return false;
	}
	Stats.Add(TEXT("script_reference_count"), ScriptsJson.Num());

	TSharedPtr<FJsonObject> ChecksObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ChecksJson;
	TSharedPtr<FJsonObject> CompileCheckObj = MakeShared<FJsonObject>();
	CompileCheckObj->SetStringField(TEXT("kind"), TEXT("compile_containing_system"));
	CompileCheckObj->SetStringField(TEXT("command"), TEXT("niagara_compile_system"));
	CompileCheckObj->SetStringField(TEXT("coverage_note"), TEXT("Standalone emitters compile in the context of containing systems."));
	ChecksJson.Add(MakeShared<FJsonValueObject>(CompileCheckObj));
	ChecksObj->SetArrayField(TEXT("checks"), ChecksJson);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("checks.json")), ChecksObj, &WrittenFiles))
	{
		OutError = TEXT("save_validation_checks_failed");
		return false;
	}

	TSharedPtr<FJsonObject> DependencyReportObj = MakeShared<FJsonObject>();
	DependencyReportObj->SetArrayField(TEXT("script_references"), ScriptsJson);
	DependencyReportObj->SetNumberField(TEXT("script_reference_count"), ScriptsJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("dependency_report.json")), DependencyReportObj, &WrittenFiles))
	{
		OutError = TEXT("save_dependency_report_failed");
		return false;
	}

	Stats.Add(TEXT("file_count"), WrittenFiles.Num() + 1);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildEmitterCoverageReport(AssetPath, TEXT("export"), Warnings, Stats);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, &WrittenFiles))
	{
		OutError = TEXT("save_coverage_report_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("emitter_asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), WrittenFiles.Num());
	OutData->SetNumberField(TEXT("script_reference_count"), ScriptsJson.Num());
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	FolderPath.TrimStartAndEndInline();

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("emitter_asset_path"), AssetPathInput);
	if (AssetPathInput.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	}
	AssetPathInput.TrimStartAndEndInline();

	if (FolderPath.IsEmpty())
	{
		if (AssetPathInput.IsEmpty())
		{
			OutError = TEXT("missing_folder_path_or_emitter_asset_path");
			return false;
		}
		if (!UeAgentNiagaraFolderOps::ResolveEmitterFolderForAsset(AssetPathInput, FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	TSharedPtr<FJsonObject> AssetObj;
	const FString AssetJsonPath = FPaths::Combine(FolderPath, TEXT("asset.json"));
	if (!UeAgentNiagaraFolderOps::LoadJsonFileOptional(AssetJsonPath, AssetObj, OutError))
	{
		return false;
	}

	FString AssetPathFromFile;
	if (AssetObj.IsValid())
	{
		AssetObj->TryGetStringField(TEXT("asset_path"), AssetPathFromFile);
	}
	if (AssetPathInput.IsEmpty())
	{
		AssetPathInput = AssetPathFromFile;
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_emitter_asset_path_and_asset_json");
		return false;
	}

	const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(AssetPathInput);
	TSharedPtr<FJsonObject> LegacyRejection;
	if (UeAgentDetailsAuthoring::BuildLegacyPropertiesFolderRejectionIfPresent(
		FolderPath,
		UeAgentNiagaraFolderOps::MakeApplyOwnerInfo(TEXT("niagara_emitter_folder"), AssetPath, TEXT("niagara_emitter_apply_folder")),
		TEXT("details.index_file"),
		LegacyRejection,
		OutError))
	{
		OutData = LegacyRejection;
		OutData->TryGetStringField(TEXT("error_code"), OutError);
		return false;
	}

	UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(UeAgentNiagaraOps::LoadAssetObject(AssetPath));
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bAddDefaultModulesAndRenderers = true;
	JsonTryGetBool(Ctx.Params, TEXT("add_default_modules_and_renderers"), bAddDefaultModulesAndRenderers);
	bool bCreated = false;
	if (!NiagaraEmitter && bCreateIfMissing)
	{
		NiagaraEmitter = UeAgentNiagaraFolderOps::CreateNiagaraEmitterAsset(AssetPath, bAddDefaultModulesAndRenderers, OutError);
		bCreated = NiagaraEmitter != nullptr;
	}
	if (!NiagaraEmitter)
	{
		OutError = TEXT("emitter_asset_not_found");
		return false;
	}

	bool bStrict = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;
	NiagaraEmitter->Modify();

	TSharedPtr<FJsonObject> EmitterObj;
	FGuid EmitterVersionGuid;
	if (UeAgentNiagaraFolderOps::LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("emitter.json")), EmitterObj, OutError) && EmitterObj.IsValid())
	{
		FString VersionText;
		if (!EmitterObj->TryGetStringField(TEXT("version_guid"), VersionText))
		{
			EmitterObj->TryGetStringField(TEXT("emitter_version"), VersionText);
		}
		FGuid::Parse(VersionText, EmitterVersionGuid);
	}
	else if (!OutError.IsEmpty())
	{
		return false;
	}

	const bool bCanUseExportedVersion = EmitterVersionGuid.IsValid() && NiagaraEmitter->GetEmitterData(EmitterVersionGuid) != nullptr;
	const FGuid ApplyVersion = bCanUseExportedVersion ? EmitterVersionGuid : NiagaraEmitter->GetExposedVersion().VersionGuid;
	const int32 AppliedEmitterPropertyCount = UeAgentNiagaraFolderOps::ApplyEmitterFolder(*NiagaraEmitter, ApplyVersion, FolderPath, Warnings, OutError);
	if (AppliedEmitterPropertyCount == INDEX_NONE)
	{
		return false;
	}

	NiagaraEmitter->MarkPackageDirty();

	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	bool bCompileHasError = false;
	bool bCompileLogReadOk = true;
	bool bUsedTransientCompileSystem = false;
	bool bTransientCompileSystemCreated = false;
	bool bTransientHasOutstandingCompilationRequests = false;
	FString TransientCompileSystemPath;
	UeAgentNiagaraOps::FNiagaraSystemRefreshResult TransientRefreshResult;
	FString CompileLogReadError;
	TSharedPtr<FJsonObject> CompileLogData;
	if (bCompileAfterApply)
	{
		UNiagaraSystem* TransientCompileSystem = nullptr;
		TSharedPtr<FNiagaraSystemViewModel> TransientCompileSystemViewModel =
			UeAgentNiagaraOps::CreateDataProcessingEmitterViewModel(*NiagaraEmitter, TransientCompileSystem);
		bTransientCompileSystemCreated = TransientCompileSystemViewModel.IsValid() && TransientCompileSystem != nullptr;
		if (bTransientCompileSystemCreated)
		{
			bUsedTransientCompileSystem = true;
			TransientCompileSystemPath = TransientCompileSystem->GetPathName();
			TransientRefreshResult = UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*TransientCompileSystem, true, false);
			bCompilationRequested = TransientCompileSystem->RequestCompile(bForceCompile);
			if (bWaitForComplete)
			{
				TransientCompileSystem->WaitForCompilationComplete(true, false);
				TransientCompileSystem->PollForCompilationComplete(true);
				TransientCompileSystem->CacheFromCompiledData();
				bCompilationComplete = !TransientCompileSystem->HasOutstandingCompilationRequests(true);
			}
			bTransientHasOutstandingCompilationRequests = TransientCompileSystem->HasOutstandingCompilationRequests(true);
		}
		else
		{
			UeAgentNiagaraFolderOps::AddWarning(
				Warnings,
				TEXT("emitter_apply_transient_compile_system_create_failed"),
				AssetPath,
				TEXT("Falling back to direct emitter script compile log because a data-processing transient system could not be created."));
		}

		FUeAgentRequestContext CompileLogCtx;
		CompileLogCtx.RequestId = TEXT("niagara_emitter_apply_folder_compile_log");
		CompileLogCtx.Command = TEXT("niagara_get_compile_log");
		CompileLogCtx.Params = MakeShared<FJsonObject>();
		CompileLogCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
		CompileLogCtx.Params->SetStringField(TEXT("emitter_version"), ApplyVersion.ToString(EGuidFormats::DigitsWithHyphensLower));
		CompileLogCtx.Params->SetBoolField(TEXT("compile_before_read"), !bUsedTransientCompileSystem);
		CompileLogCtx.Params->SetBoolField(TEXT("force_compile"), bForceCompile);
		CompileLogCtx.Params->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
		CompileLogCtx.Params->SetBoolField(TEXT("include_events"), true);
		CompileLogCtx.Params->SetStringField(TEXT("severity_filter"), TEXT("warning_or_error"));
		CompileLogCtx.Params->SetNumberField(TEXT("max_events_per_script"), 64);
		CompileLogData = MakeShared<FJsonObject>();
		if (!CmdNiagaraGetCompileLog(CompileLogCtx, CompileLogData, CompileLogReadError))
		{
			bCompileLogReadOk = false;
			CompileLogData->SetBoolField(TEXT("ok"), false);
			CompileLogData->SetStringField(TEXT("error"), CompileLogReadError);
		}
		if (CompileLogData.IsValid())
		{
			bool bCompileLogCompilationRequested = false;
			if (CompileLogData->TryGetBoolField(TEXT("compilation_requested"), bCompileLogCompilationRequested))
			{
				bCompilationRequested = bCompilationRequested || bCompileLogCompilationRequested;
			}
			bool bCompileLogCompilationComplete = false;
			if (CompileLogData->TryGetBoolField(TEXT("compilation_complete"), bCompileLogCompilationComplete))
			{
				bCompilationComplete = bCompilationComplete || bCompileLogCompilationComplete;
			}
			CompileLogData->TryGetBoolField(TEXT("has_error"), bCompileHasError);
		}
	}

	UeAgentNiagaraFolderOps::FPostApplyStackIssueReport StackIssueReport;
	if (!UeAgentNiagaraFolderOps::CollectPostApplyStackIssues(this, Ctx, NiagaraEmitter->GetOutermost()->GetName(), bStrict, TEXT("emitter_apply_stack_issues"), StackIssueReport))
	{
		UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("stack_issue_collection_failed"), NiagaraEmitter->GetOutermost()->GetName(), StackIssueReport.CollectionError);
	}

	Stats.Add(TEXT("emitter_properties_applied"), AppliedEmitterPropertyCount);
	Stats.Add(TEXT("stack_total_issue_count"), StackIssueReport.TotalIssueCount);
	Stats.Add(TEXT("stack_error_count"), StackIssueReport.TotalErrorCount);
	Stats.Add(TEXT("stack_warning_count"), StackIssueReport.TotalWarningCount);
	Stats.Add(TEXT("stack_info_count"), StackIssueReport.TotalInfoCount);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildEmitterCoverageReport(AssetPath, TEXT("apply"), Warnings, Stats);
	if (StackIssueReport.Report.IsValid())
	{
		CoverageObj->SetObjectField(TEXT("stack_issue_report"), StackIssueReport.Report);
	}
	UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, nullptr);

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bSaved = false;

	OutData->SetStringField(TEXT("asset_path"), NiagaraEmitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_asset_path"), NiagaraEmitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("asset_json_loaded"), AssetObj.IsValid());
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetNumberField(TEXT("emitter_properties_applied"), AppliedEmitterPropertyCount);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("used_transient_compile_system"), bUsedTransientCompileSystem);
	OutData->SetBoolField(TEXT("transient_compile_system_created"), bTransientCompileSystemCreated);
	OutData->SetStringField(TEXT("transient_compile_system_path"), TransientCompileSystemPath);
	OutData->SetBoolField(TEXT("transient_has_outstanding_compilation_requests"), bTransientHasOutstandingCompilationRequests);
	OutData->SetObjectField(TEXT("transient_system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(TransientRefreshResult));
	OutData->SetBoolField(TEXT("compile_log_read_ok"), bCompileLogReadOk);
	OutData->SetStringField(TEXT("compile_log_read_error"), CompileLogReadError);
	OutData->SetBoolField(TEXT("compile_has_error"), bCompileHasError);
	if (CompileLogData.IsValid())
	{
		OutData->SetObjectField(TEXT("compile_log"), CompileLogData);
	}
	OutData->SetBoolField(TEXT("saved"), bSaved);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	UeAgentNiagaraFolderOps::AppendPostApplyStackIssueFields(OutData, StackIssueReport);

	if (bCompileAfterApply && !bCompileLogReadOk)
	{
		OutError = TEXT("compile_log_read_failed_after_apply");
		return false;
	}
	if (bCompileAfterApply && bCompileHasError)
	{
		OutError = TEXT("compile_failed_after_apply");
		return false;
	}
	if (bStrict && StackIssueReport.bCollectAfterApply && !StackIssueReport.bCollectionOk)
	{
		OutError = TEXT("strict_apply_stack_issue_collection_failed");
		return false;
	}
	if (StackIssueReport.bFailOnStackErrors && StackIssueReport.TotalErrorCount > 0)
	{
		OutError = TEXT("strict_apply_has_stack_errors");
		return false;
	}
	if (bStrict && Warnings.Num() > 0)
	{
		OutError = TEXT("strict_apply_has_warnings");
		return false;
	}
	if (bSaveAfterApply)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraEmitter, OutError))
		{
			return false;
		}
		bSaved = true;
		OutData->SetBoolField(TEXT("saved"), bSaved);
	}
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraScriptExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("script_asset_path"), AssetPathInput);
	if (AssetPathInput.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	}
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_script_asset_path");
		return false;
	}

	UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraScript)
	{
		OutError = TEXT("script_asset_not_found");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentNiagaraFolderOps::ResolveScriptFolderForAsset(NiagaraScript->GetOutermost()->GetName(), FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	bool bIncludeRawProperties = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRawProperties);
	UeAgentNiagaraFolderOps::FScopedIncludeRawPropertiesInFolderExport IncludeRawPropertiesScope(bIncludeRawProperties);
	if (bCleanOutputDir)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	TArray<FString> WrittenFiles;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;

	const FString AssetPath = NiagaraScript->GetOutermost()->GetName();

	TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
	AssetObj->SetNumberField(TEXT("format_version"), UeAgentNiagaraFolderOps::NiagaraFolderFormatVersion);
	AssetObj->SetStringField(TEXT("asset_kind"), TEXT("niagara_script"));
	AssetObj->SetStringField(TEXT("asset_path"), AssetPath);
	AssetObj->SetStringField(TEXT("object_path"), NiagaraScript->GetPathName());
	AssetObj->SetStringField(TEXT("class"), NiagaraScript->GetClass()->GetPathName());
	AssetObj->SetStringField(TEXT("engine_target"), TEXT("UE_5.6"));
	AssetObj->SetStringField(TEXT("root_folder_kind"), TEXT("NiagaraScript"));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetObj, &WrittenFiles))
	{
		OutError = TEXT("save_asset_json_failed");
		return false;
	}

	TSharedPtr<FJsonObject> ScriptObj = MakeShared<FJsonObject>();
	ScriptObj->SetNumberField(TEXT("format_version"), UeAgentNiagaraFolderOps::NiagaraFolderFormatVersion);
	ScriptObj->SetStringField(TEXT("asset_kind"), TEXT("niagara_script"));
	ScriptObj->SetStringField(TEXT("script_name"), NiagaraScript->GetName());
	ScriptObj->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(NiagaraScript->GetUsage()));
	ScriptObj->SetStringField(TEXT("script_usage_id"), NiagaraScript->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
	ScriptObj->SetStringField(TEXT("compile_status"), UeAgentNiagaraOps::ScriptCompileStatusToString(NiagaraScript->GetLastCompileStatus()));
	if (!UeAgentNiagaraFolderOps::ExportFolderOwnerDetails(
		NiagaraScript,
		FolderPath,
		TEXT("niagara_script_export_folder"),
		TEXT("niagara_script_settings"),
		TEXT("script"),
		TEXT("script"),
		ScriptObj,
		OutError))
	{
		return false;
	}
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("script.json")), ScriptObj, &WrittenFiles))
	{
		OutError = TEXT("save_script_json_failed");
		return false;
	}

	if (bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
		UeAgentNiagaraFolderOps::AppendPropertyArray(PropertiesObj, TEXT("raw_properties"), UeAgentNiagaraFolderOps::BuildObjectPropertiesJson(NiagaraScript));
		if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("properties.json")), PropertiesObj, &WrittenFiles))
		{
			OutError = TEXT("save_properties_json_failed");
			return false;
		}
	}

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	TArray<TSharedPtr<FJsonValue>> LinksJson;
	TArray<TSharedPtr<FJsonValue>> CustomHlslNodesJson;
	if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(NiagaraScript->GetLatestSource()))
	{
		if (UNiagaraGraph* Graph = ScriptSource->NodeGraph)
		{
			TSet<UEdGraphNode*> AllNodes;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				AllNodes.Add(Node);
				TSharedPtr<FJsonObject> NodeObj = UeAgentNiagaraOps::BuildStageNodeJson(*Node, NodesJson.Num(), true, true);
				if (UNiagaraNodeCustomHlsl* CustomHlslNode = Cast<UNiagaraNodeCustomHlsl>(Node))
				{
					FString CustomHlslText;
					if (UeAgentNiagaraOps::GetObjectPropertyText(CustomHlslNode, TEXT("CustomHlsl"), CustomHlslText))
					{
						NodeObj->SetStringField(TEXT("custom_hlsl"), CustomHlslText);
					}
					CustomHlslNodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
				}
				NodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
			UeAgentNiagaraFolderOps::ExportGraphLinksForNodes(AllNodes, LinksJson);
		}
		else
		{
			UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("script_missing_graph"), AssetPath, TEXT("NiagaraScript source has no graph."));
		}
	}
	else
	{
		UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("script_missing_source"), AssetPath, TEXT("NiagaraScript has no UNiagaraScriptSource."));
	}

	TSharedPtr<FJsonObject> NodesObj = MakeShared<FJsonObject>();
	NodesObj->SetArrayField(TEXT("nodes"), NodesJson);
	NodesObj->SetNumberField(TEXT("node_count"), NodesJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graph"), TEXT("nodes.json")), NodesObj, &WrittenFiles))
	{
		OutError = TEXT("save_graph_nodes_failed");
		return false;
	}

	TSharedPtr<FJsonObject> LinksObj = MakeShared<FJsonObject>();
	LinksObj->SetArrayField(TEXT("links"), LinksJson);
	LinksObj->SetNumberField(TEXT("link_count"), LinksJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graph"), TEXT("links.json")), LinksObj, &WrittenFiles))
	{
		OutError = TEXT("save_graph_links_failed");
		return false;
	}

	TSharedPtr<FJsonObject> CustomHlslObj = MakeShared<FJsonObject>();
	CustomHlslObj->SetArrayField(TEXT("custom_hlsl_nodes"), CustomHlslNodesJson);
	CustomHlslObj->SetNumberField(TEXT("custom_hlsl_node_count"), CustomHlslNodesJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graph"), TEXT("custom_hlsl_nodes.json")), CustomHlslObj, &WrittenFiles))
	{
		OutError = TEXT("save_custom_hlsl_failed");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> AuthoringNodesJson;
	TArray<TSharedPtr<FJsonValue>> LayoutNodesJson;
	for (const TSharedPtr<FJsonValue>& NodeValue : NodesJson)
	{
		const TSharedPtr<FJsonObject>* NodeObj = nullptr;
		if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
		{
			continue;
		}

		TSharedPtr<FJsonObject> AuthoringNode = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*NodeObj)->Values)
		{
			if (Field.Key != TEXT("node_pos_x") && Field.Key != TEXT("node_pos_y"))
			{
				AuthoringNode->SetField(Field.Key, Field.Value);
			}
		}
		AuthoringNodesJson.Add(MakeShared<FJsonValueObject>(AuthoringNode));

		double PosX = 0.0;
		double PosY = 0.0;
		const bool bHasPosX = (*NodeObj)->TryGetNumberField(TEXT("node_pos_x"), PosX);
		const bool bHasPosY = (*NodeObj)->TryGetNumberField(TEXT("node_pos_y"), PosY);
		if (bHasPosX || bHasPosY)
		{
			FString NodeId;
			if (!(*NodeObj)->TryGetStringField(TEXT("node_guid"), NodeId) || NodeId.IsEmpty())
			{
				(*NodeObj)->TryGetStringField(TEXT("node_title"), NodeId);
			}
			TSharedPtr<FJsonObject> LayoutNode = MakeShared<FJsonObject>();
			LayoutNode->SetStringField(TEXT("id"), NodeId);
			LayoutNode->SetNumberField(TEXT("x"), PosX);
			LayoutNode->SetNumberField(TEXT("y"), PosY);
			LayoutNodesJson.Add(MakeShared<FJsonValueObject>(LayoutNode));
		}
	}

	TSharedPtr<FJsonObject> AuthoringGraphObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphInfoObj = MakeShared<FJsonObject>();
	GraphInfoObj->SetStringField(TEXT("name"), TEXT("main"));
	GraphInfoObj->SetStringField(TEXT("graph_kind"), TEXT("niagara_script_graph"));
	AuthoringGraphObj->SetObjectField(TEXT("graph"), GraphInfoObj);
	AuthoringGraphObj->SetArrayField(TEXT("nodes"), AuthoringNodesJson);
	AuthoringGraphObj->SetArrayField(TEXT("links"), LinksJson);
	AuthoringGraphObj->SetArrayField(TEXT("custom_hlsl_nodes"), CustomHlslNodesJson);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("items"), TEXT("main"), TEXT("graph.json")), AuthoringGraphObj, &WrittenFiles))
	{
		OutError = TEXT("save_authoring_graph_failed");
		return false;
	}

	TSharedPtr<FJsonObject> AuthoringLayoutObj = MakeShared<FJsonObject>();
	AuthoringLayoutObj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.niagara_script_graph_layout.v1"));
	AuthoringLayoutObj->SetArrayField(TEXT("nodes"), LayoutNodesJson);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("items"), TEXT("main"), TEXT("layout.json")), AuthoringLayoutObj, &WrittenFiles))
	{
		OutError = TEXT("save_authoring_layout_failed");
		return false;
	}

	Stats.Add(TEXT("node_count"), NodesJson.Num());
	Stats.Add(TEXT("link_count"), LinksJson.Num());
	Stats.Add(TEXT("custom_hlsl_node_count"), CustomHlslNodesJson.Num());
	Stats.Add(TEXT("file_count"), WrittenFiles.Num() + 1);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildScriptCoverageReport(AssetPath, TEXT("export"), Warnings, Stats);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, &WrittenFiles))
	{
		OutError = TEXT("save_coverage_report_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("script_asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), WrittenFiles.Num());
	OutData->SetNumberField(TEXT("node_count"), NodesJson.Num());
	OutData->SetNumberField(TEXT("link_count"), LinksJson.Num());
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraScriptApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	FolderPath.TrimStartAndEndInline();

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("script_asset_path"), AssetPathInput);
	if (AssetPathInput.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	}
	AssetPathInput.TrimStartAndEndInline();

	if (FolderPath.IsEmpty())
	{
		if (AssetPathInput.IsEmpty())
		{
			OutError = TEXT("missing_folder_path_or_script_asset_path");
			return false;
		}
		if (!UeAgentNiagaraFolderOps::ResolveScriptFolderForAsset(AssetPathInput, FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	TSharedPtr<FJsonObject> AssetObj;
	if (!UeAgentNiagaraFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetObj, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> ScriptObj;
	if (!UeAgentNiagaraFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("script.json")), ScriptObj, OutError))
	{
		return false;
	}

	FString AssetPathFromFile;
	AssetObj->TryGetStringField(TEXT("asset_path"), AssetPathFromFile);
	if (AssetPathInput.IsEmpty())
	{
		AssetPathInput = AssetPathFromFile;
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_script_asset_path");
		return false;
	}

	ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Module;
	FString UsageText;
	if (ScriptObj->TryGetStringField(TEXT("script_usage"), UsageText) && !UsageText.IsEmpty())
	{
		UeAgentNiagaraOps::TryParseScriptUsage(UsageText, Usage);
	}

	const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(AssetPathInput);
	TSharedPtr<FJsonObject> LegacyRejection;
	if (UeAgentDetailsAuthoring::BuildLegacyPropertiesFolderRejectionIfPresent(
		FolderPath,
		UeAgentNiagaraFolderOps::MakeApplyOwnerInfo(TEXT("niagara_script_folder"), AssetPath, TEXT("niagara_script_apply_folder")),
		TEXT("details.index_file"),
		LegacyRejection,
		OutError))
	{
		OutData = LegacyRejection;
		OutData->TryGetStringField(TEXT("error_code"), OutError);
		return false;
	}

	UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(AssetPath));
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bCreated = false;
	if (!NiagaraScript && bCreateIfMissing)
	{
		NiagaraScript = UeAgentNiagaraFolderOps::CreateNiagaraScriptAsset(AssetPath, Usage, OutError);
		bCreated = NiagaraScript != nullptr;
	}
	if (!NiagaraScript)
	{
		OutError = TEXT("script_asset_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;
	NiagaraScript->Modify();
	NiagaraScript->SetUsage(Usage);
	FString UsageIdText;
	if (ScriptObj->TryGetStringField(TEXT("script_usage_id"), UsageIdText) && !UsageIdText.IsEmpty())
	{
		FGuid UsageId;
		if (FGuid::Parse(UsageIdText, UsageId))
		{
			NiagaraScript->SetUsageId(UsageId);
		}
	}

	int32 AppliedPropertyCount = 0;
	if (!UeAgentNiagaraFolderOps::ApplyFolderOwnerDetailsIfPresent(
		NiagaraScript,
		FolderPath,
		ScriptObj,
		TEXT("niagara_script_settings"),
		TEXT("script"),
		TEXT("niagara_script_apply_folder"),
		OutError,
		AppliedPropertyCount))
	{
		return false;
	}

	int32 AppliedNodePropertyCount = 0;
	if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(NiagaraScript->GetLatestSource()))
	{
		if (UNiagaraGraph* Graph = ScriptSource->NodeGraph)
		{
			TSharedPtr<FJsonObject> NodesObj;
			if (UeAgentNiagaraFolderOps::LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("graph"), TEXT("nodes.json")), NodesObj, OutError) && NodesObj.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
				if (UeAgentNiagaraFolderOps::TryGetArrayField(NodesObj, TEXT("nodes"), Nodes))
				{
					for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
					{
						const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
						if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
						{
							continue;
						}
						const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

						FGuid NodeGuid;
						FString NodeGuidText;
						NodeObj->TryGetStringField(TEXT("node_guid"), NodeGuidText);
						FGuid::Parse(NodeGuidText, NodeGuid);
						UEdGraphNode* TargetNode = NodeGuid.IsValid() ? UeAgentNiagaraOps::FindGraphNodeByGuid(*Graph, NodeGuid) : nullptr;
						if (!TargetNode)
						{
							FString NodeClassPath;
							NodeObj->TryGetStringField(TEXT("node_class"), NodeClassPath);
							if (NodeClassPath.TrimStartAndEnd().IsEmpty())
							{
								FString CatalogName;
								if (!NodeObj->TryGetStringField(TEXT("catalog_name"), CatalogName))
								{
									NodeObj->TryGetStringField(TEXT("name"), CatalogName);
								}
								if (!CatalogName.TrimStartAndEnd().IsEmpty())
								{
									UeAgentPaletteSearch::FCandidate CatalogCandidate;
									FString ResolveError;
									if (UeAgentPaletteSearch::ResolveCatalogNameToCandidate(TEXT("niagara_graph_node"), CatalogName, CatalogCandidate, ResolveError, AssetPath, TEXT("Graph")))
									{
										NodeClassPath = CatalogCandidate.FullName;
									}
									else
									{
										UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("script_graph_node_catalog_name_not_found"), CatalogName, ResolveError);
									}
								}
							}
							UClass* NodeClass = UeAgentNiagaraOps::ResolveClassByPath(NodeClassPath);
							if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
							{
								continue;
							}

							TargetNode = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
							if (!TargetNode)
							{
								continue;
							}
							if (NodeGuid.IsValid())
							{
								TargetNode->NodeGuid = NodeGuid;
							}
							else
							{
								TargetNode->CreateNewGuid();
							}
							TargetNode->AllocateDefaultPins();
							Graph->AddNode(TargetNode, true, false);
						}

						if (TargetNode)
						{
							if (!UeAgentNiagaraFolderOps::ApplyFolderOwnerDetailsIfPresent(
								TargetNode,
								FPaths::Combine(FolderPath, TEXT("graph")),
								NodeObj,
								TEXT("niagara_script_graph_node"),
								TargetNode->GetName(),
								TEXT("niagara_script_apply_folder"),
								OutError,
								AppliedNodePropertyCount))
							{
								return false;
							}
							if (UNiagaraNodeCustomHlsl* CustomHlslNode = Cast<UNiagaraNodeCustomHlsl>(TargetNode))
							{
								FString CustomHlsl;
								if (NodeObj->TryGetStringField(TEXT("custom_hlsl"), CustomHlsl))
								{
									FString AppliedCustomHlsl;
									FString CppType;
									FString ImportError;
									if (!UeAgentNiagaraOps::SetObjectPropertyText(CustomHlslNode, TEXT("CustomHlsl"), CustomHlsl, &AppliedCustomHlsl, &CppType, &ImportError))
									{
										UeAgentNiagaraFolderOps::AddWarning(
											Warnings,
											TEXT("custom_hlsl_apply_failed"),
											TargetNode->GetPathName(),
											FString::Printf(TEXT("Failed to apply CustomHlsl. requested_length=%d cpp_type='%s' error='%s'."), CustomHlsl.Len(), *CppType, *ImportError));
									}
									else if (!AppliedCustomHlsl.Equals(CustomHlsl, ESearchCase::CaseSensitive))
									{
										UeAgentNiagaraFolderOps::AddWarning(
											Warnings,
											TEXT("custom_hlsl_value_changed_after_import"),
											TargetNode->GetPathName(),
											FString::Printf(TEXT("CustomHlsl read back a different value. requested_length=%d applied_length=%d cpp_type='%s'."), CustomHlsl.Len(), AppliedCustomHlsl.Len(), *CppType));
									}
								}
							}
						}
					}
				}
			}
			else if (!OutError.IsEmpty())
			{
				return false;
			}
			UeAgentNiagaraOps::NormalizeGraphNodeGuids(Graph);
			Graph->NotifyGraphChanged();
		}
	}
	else
	{
		UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("script_apply_missing_source"), AssetPath, TEXT("NiagaraScript has no graph source after initialization."));
	}

	NiagaraScript->MarkPackageDirty();

	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	bool bCompileHasError = false;
	bool bCompileLogReadOk = true;
	FString CompileLogReadError;
	TSharedPtr<FJsonObject> CompileLogData;
	if (bCompileAfterApply)
	{
		FUeAgentRequestContext CompileLogCtx;
		CompileLogCtx.RequestId = TEXT("niagara_script_apply_folder_compile_log");
		CompileLogCtx.Command = TEXT("niagara_get_compile_log");
		CompileLogCtx.Params = MakeShared<FJsonObject>();
		CompileLogCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
		CompileLogCtx.Params->SetBoolField(TEXT("compile_before_read"), true);
		CompileLogCtx.Params->SetBoolField(TEXT("force_compile"), bForceCompile);
		CompileLogCtx.Params->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
		CompileLogCtx.Params->SetBoolField(TEXT("include_events"), true);
		CompileLogCtx.Params->SetStringField(TEXT("severity_filter"), TEXT("warning_or_error"));
		CompileLogCtx.Params->SetNumberField(TEXT("max_events_per_script"), 64);
		CompileLogData = MakeShared<FJsonObject>();
		if (!CmdNiagaraGetCompileLog(CompileLogCtx, CompileLogData, CompileLogReadError))
		{
			bCompileLogReadOk = false;
			CompileLogData->SetBoolField(TEXT("ok"), false);
			CompileLogData->SetStringField(TEXT("error"), CompileLogReadError);
		}
		if (CompileLogData.IsValid())
		{
			CompileLogData->TryGetBoolField(TEXT("compilation_requested"), bCompilationRequested);
			CompileLogData->TryGetBoolField(TEXT("compilation_complete"), bCompilationComplete);
			CompileLogData->TryGetBoolField(TEXT("has_error"), bCompileHasError);
		}
	}

	Stats.Add(TEXT("script_properties_applied"), AppliedPropertyCount);
	Stats.Add(TEXT("node_properties_applied"), AppliedNodePropertyCount);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildScriptCoverageReport(AssetPath, TEXT("apply"), Warnings, Stats);
	UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, nullptr);

	bool bStrict = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);
	if (bStrict && Warnings.Num() > 0)
	{
		OutError = TEXT("strict_apply_has_warnings");
		OutData->SetArrayField(TEXT("warnings"), Warnings);
		return false;
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bSaved = false;

	OutData->SetStringField(TEXT("asset_path"), NiagaraScript->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("script_asset_path"), NiagaraScript->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetNumberField(TEXT("script_properties_applied"), AppliedPropertyCount);
	OutData->SetNumberField(TEXT("node_properties_applied"), AppliedNodePropertyCount);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("compile_log_read_ok"), bCompileLogReadOk);
	OutData->SetStringField(TEXT("compile_log_read_error"), CompileLogReadError);
	OutData->SetBoolField(TEXT("compile_has_error"), bCompileHasError);
	if (CompileLogData.IsValid())
	{
		OutData->SetObjectField(TEXT("compile_log"), CompileLogData);
	}
	OutData->SetBoolField(TEXT("saved"), bSaved);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	if (bCompileAfterApply && !bCompileLogReadOk)
	{
		OutError = TEXT("compile_log_read_failed_after_apply");
		return false;
	}
	if (bCompileAfterApply && bCompileHasError)
	{
		OutError = TEXT("compile_failed_after_apply");
		return false;
	}
	if (bSaveAfterApply)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraScript, OutError))
		{
			return false;
		}
		bSaved = true;
		OutData->SetBoolField(TEXT("saved"), bSaved);
	}
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	FolderPath.TrimStartAndEndInline();

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();

	if (FolderPath.IsEmpty())
	{
		if (AssetPathInput.IsEmpty())
		{
			OutError = TEXT("missing_folder_path_or_asset_path");
			return false;
		}
		if (!UeAgentNiagaraFolderOps::ResolveFolderForAsset(AssetPathInput, FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	TSharedPtr<FJsonObject> AssetObj;
	if (!UeAgentNiagaraFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetObj, OutError))
	{
		return false;
	}

	FString AssetPathFromFile;
	AssetObj->TryGetStringField(TEXT("asset_path"), AssetPathFromFile);
	if (AssetPathInput.IsEmpty())
	{
		AssetPathInput = AssetPathFromFile;
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(AssetPathInput);
	TSharedPtr<FJsonObject> LegacyRejection;
	if (UeAgentDetailsAuthoring::BuildLegacyPropertiesFolderRejectionIfPresent(
		FolderPath,
		UeAgentNiagaraFolderOps::MakeApplyOwnerInfo(TEXT("niagara_system_folder"), AssetPath, TEXT("niagara_apply_folder")),
		TEXT("details.index_file"),
		LegacyRejection,
		OutError))
	{
		OutData = LegacyRejection;
		OutData->TryGetStringField(TEXT("error_code"), OutError);
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPath));
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bCreated = false;
	if (!NiagaraSystem && bCreateIfMissing)
	{
		NiagaraSystem = UeAgentNiagaraFolderOps::CreateNiagaraSystemAsset(AssetPath, OutError);
		bCreated = NiagaraSystem != nullptr;
	}
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	bool bApplyReferencedEmitters = true;
	JsonTryGetBool(Ctx.Params, TEXT("apply_referenced_emitters"), bApplyReferencedEmitters);
	bool bStrict = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);
	bool bRestorePreviewAfterApply = true;
	UeAgentNiagaraOps::TryGetBoolAnyField(
		Ctx.Params,
		{
			TEXT("restore_preview_after_apply"),
			TEXT("resume_preview_after_apply"),
			TEXT("restore_interactive_preview"),
			TEXT("resume_interactive_preview")
		},
		bRestorePreviewAfterApply);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;
	int32 AppliedSystemPropertyCount = 0;
	int32 AppliedUserParameterCount = 0;
	int32 AppliedSystemStageCount = 0;
	int32 AppliedEmitterHandleCount = 0;
	int32 AppliedEmitterPropertyCount = 0;
	int32 AppliedPostRefreshDataInterfaceCount = 0;

	NiagaraSystem->Modify();

	TSharedPtr<FJsonObject> SystemSettingsObj;
	if (!UeAgentNiagaraFolderOps::LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("system.json")), SystemSettingsObj, OutError))
	{
		return false;
	}
	if (SystemSettingsObj.IsValid())
	{
		if (!UeAgentNiagaraFolderOps::ApplyFolderOwnerDetailsIfPresent(
			NiagaraSystem,
			FPaths::Combine(FolderPath, TEXT("settings")),
			SystemSettingsObj,
			TEXT("niagara_system_settings"),
			TEXT("system"),
			TEXT("niagara_apply_folder"),
			OutError,
			AppliedSystemPropertyCount))
		{
			return false;
		}
	}

	const int32 UserParamsApplied = UeAgentNiagaraFolderOps::ApplyUserParameters(*NiagaraSystem, FolderPath, Warnings, OutError);
	if (UserParamsApplied == INDEX_NONE)
	{
		return false;
	}
	AppliedUserParameterCount = UserParamsApplied;

	TSharedPtr<FJsonObject> EmittersIndexObj;
	if (!UeAgentNiagaraFolderOps::LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("emitters"), TEXT("index.json")), EmittersIndexObj, OutError))
	{
		return false;
	}
	if (EmittersIndexObj.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
		if (UeAgentNiagaraFolderOps::TryGetArrayField(EmittersIndexObj, TEXT("emitters"), Emitters))
		{
			for (int32 EmitterIndex = 0; EmitterIndex < Emitters->Num(); ++EmitterIndex)
			{
				const TSharedPtr<FJsonValue>& EmitterValue = (*Emitters)[EmitterIndex];
				const TSharedPtr<FJsonObject>* EmitterObjPtr = nullptr;
				if (!EmitterValue.IsValid() || !EmitterValue->TryGetObject(EmitterObjPtr) || !EmitterObjPtr || !EmitterObjPtr->IsValid())
				{
					continue;
				}

				const TSharedPtr<FJsonObject>& EmitterObj = *EmitterObjPtr;
				const FString EmitterName = UeAgentNiagaraFolderOps::GetStringFieldAny(EmitterObj, { TEXT("emitter_name"), TEXT("name") }, FString::Printf(TEXT("Emitter_%d"), EmitterIndex));
				FString EmitterFolderRelative;
				EmitterObj->TryGetStringField(TEXT("folder"), EmitterFolderRelative);

				TSharedPtr<FJsonObject> SourceObj;
				FString SourceEmitterAssetPath;
				if (UeAgentNiagaraFolderOps::GetObjectField(EmitterObj, TEXT("source"), SourceObj))
				{
					SourceObj->TryGetStringField(TEXT("emitter_asset_path"), SourceEmitterAssetPath);
				}
				if (SourceEmitterAssetPath.IsEmpty())
				{
					EmitterObj->TryGetStringField(TEXT("emitter_asset_path"), SourceEmitterAssetPath);
				}

				FString EmitterVersionText;
				EmitterObj->TryGetStringField(TEXT("emitter_version"), EmitterVersionText);
				FGuid EmitterVersionGuid;
				FGuid::Parse(EmitterVersionText, EmitterVersionGuid);

				TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
				FNiagaraEmitterHandle* ExistingHandle = nullptr;
				int32 ExistingHandleIndex = INDEX_NONE;
				for (int32 HandleIndex = 0; HandleIndex < Handles.Num(); ++HandleIndex)
				{
					if (Handles[HandleIndex].GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
					{
						ExistingHandle = &Handles[HandleIndex];
						ExistingHandleIndex = HandleIndex;
						break;
					}
				}

				if (!ExistingHandle)
				{
					UNiagaraEmitter* SourceEmitter = Cast<UNiagaraEmitter>(UeAgentNiagaraOps::LoadAssetObject(SourceEmitterAssetPath));
					if (!SourceEmitter)
					{
						SourceEmitter = UeAgentNiagaraFolderOps::CreateTemplateEmitterForFolderApply(*NiagaraSystem, EmitterName);
						if (!SourceEmitter)
						{
							UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("emitter_source_not_found"), EmitterName, SourceEmitterAssetPath);
							continue;
						}
						if (!SourceEmitterAssetPath.IsEmpty())
						{
							const FString SourceMessage = FString::Printf(TEXT("Emitter source asset '%s' could not be loaded; generated a template emitter and applied folder data."), *SourceEmitterAssetPath);
							UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("emitter_source_generated_from_folder"), EmitterName, SourceMessage);
						}
					}

					const bool bCanUseExportedVersion = EmitterVersionGuid.IsValid() && SourceEmitter->GetEmitterData(EmitterVersionGuid) != nullptr;
					const FGuid AddVersion = bCanUseExportedVersion ? EmitterVersionGuid : SourceEmitter->GetExposedVersion().VersionGuid;
					const FNiagaraEmitterHandle NewHandle = NiagaraSystem->AddEmitterHandle(*SourceEmitter, *EmitterName, AddVersion);
					TArray<FNiagaraEmitterHandle>& UpdatedHandles = NiagaraSystem->GetEmitterHandles();
					for (int32 HandleIndex = 0; HandleIndex < UpdatedHandles.Num(); ++HandleIndex)
					{
						if (UpdatedHandles[HandleIndex].GetId() == NewHandle.GetId())
						{
							ExistingHandle = &UpdatedHandles[HandleIndex];
							ExistingHandleIndex = HandleIndex;
							break;
						}
					}
				}

				if (!ExistingHandle)
				{
					continue;
				}

				bool bEnabled = ExistingHandle->GetIsEnabled();
				if (EmitterObj->TryGetBoolField(TEXT("enabled"), bEnabled))
				{
					ExistingHandle->SetIsEnabled(bEnabled, *NiagaraSystem, false);
				}

				FVersionedNiagaraEmitter VersionedEmitterForApply = ExistingHandle->GetInstance();
				TArray<FNiagaraEmitterHandle>& MutableHandles = NiagaraSystem->GetEmitterHandles();
				const int32 TargetIndex = FMath::Clamp(EmitterIndex, 0, FMath::Max(0, MutableHandles.Num() - 1));
				if (MutableHandles.IsValidIndex(ExistingHandleIndex) && ExistingHandleIndex != TargetIndex)
				{
					const FNiagaraEmitterHandle MovedHandle = MutableHandles[ExistingHandleIndex];
					MutableHandles.RemoveAt(ExistingHandleIndex);
					MutableHandles.Insert(MovedHandle, TargetIndex);
				}
				++AppliedEmitterHandleCount;

				if (bApplyReferencedEmitters && !EmitterFolderRelative.IsEmpty())
				{
					if (VersionedEmitterForApply.Emitter)
					{
						const FString EmitterFolder = FPaths::Combine(FolderPath, TEXT("emitters"), EmitterFolderRelative);
						const int32 EmitterApplied = UeAgentNiagaraFolderOps::ApplyEmitterFolder(*VersionedEmitterForApply.Emitter, VersionedEmitterForApply.Version, EmitterFolder, Warnings, OutError, false);
						if (EmitterApplied == INDEX_NONE)
						{
							return false;
						}
						AppliedEmitterPropertyCount += EmitterApplied;
					}
				}
			}
		}
	}

	const int32 SystemStagesApplied = UeAgentNiagaraFolderOps::ApplySystemStagesFromFolder(*NiagaraSystem, FolderPath, Warnings, OutError);
	if (SystemStagesApplied == INDEX_NONE)
	{
		return false;
	}
	AppliedSystemStageCount = SystemStagesApplied;

	UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult =
		UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, true);
	if (EmittersIndexObj.IsValid())
	{
		const int32 PostRefreshDataInterfacesApplied = UeAgentNiagaraFolderOps::ApplySystemDataInterfacesFromFolder(*NiagaraSystem, FolderPath, EmittersIndexObj, Warnings, OutError);
		if (PostRefreshDataInterfacesApplied == INDEX_NONE)
		{
			return false;
		}
		AppliedPostRefreshDataInterfaceCount = PostRefreshDataInterfacesApplied;
		if (AppliedPostRefreshDataInterfaceCount > 0)
		{
			NiagaraSystem->MarkPackageDirty();
			RefreshResult = UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, true);
		}
	}

	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	bool bCompileHasError = false;
	bool bCompileLogReadOk = true;
	FString CompileLogReadError;
	TSharedPtr<FJsonObject> CompileLogData;
	if (bCompileAfterApply)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(bForceCompile);
		if (bWaitForComplete)
		{
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
			NiagaraSystem->CacheFromCompiledData();
			bCompilationComplete = !NiagaraSystem->HasOutstandingCompilationRequests(true);
		}
		FUeAgentRequestContext CompileLogCtx;
		CompileLogCtx.RequestId = TEXT("niagara_apply_folder_compile_log");
		CompileLogCtx.Command = TEXT("niagara_get_compile_log");
		CompileLogCtx.Params = MakeShared<FJsonObject>();
		CompileLogCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
		CompileLogCtx.Params->SetBoolField(TEXT("compile_before_read"), false);
		CompileLogCtx.Params->SetBoolField(TEXT("force_compile"), bForceCompile);
		CompileLogCtx.Params->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
		CompileLogCtx.Params->SetBoolField(TEXT("include_events"), true);
		CompileLogCtx.Params->SetStringField(TEXT("severity_filter"), TEXT("warning_or_error"));
		CompileLogCtx.Params->SetNumberField(TEXT("max_events_per_script"), 64);
		CompileLogData = MakeShared<FJsonObject>();
		if (!CmdNiagaraGetCompileLog(CompileLogCtx, CompileLogData, CompileLogReadError))
		{
			bCompileLogReadOk = false;
			CompileLogData->SetBoolField(TEXT("ok"), false);
			CompileLogData->SetStringField(TEXT("error"), CompileLogReadError);
		}
		if (CompileLogData.IsValid())
		{
			CompileLogData->TryGetBoolField(TEXT("has_error"), bCompileHasError);
		}
	}

	UeAgentNiagaraFolderOps::FPostApplyStackIssueReport StackIssueReport;
	if (!UeAgentNiagaraFolderOps::CollectPostApplyStackIssues(this, Ctx, NiagaraSystem->GetOutermost()->GetName(), bStrict, TEXT("system_apply_stack_issues"), StackIssueReport))
	{
		UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("stack_issue_collection_failed"), NiagaraSystem->GetOutermost()->GetName(), StackIssueReport.CollectionError);
	}
	bool bOutstandingAfterStackIssueRead = false;
	bool bPostStackCompilationRequested = false;
	bool bPostStackCompilationComplete = false;
	bool bFinalOutstandingCompilationRequests = bCompileAfterApply && bWaitForComplete
		? NiagaraSystem->HasOutstandingCompilationRequests(true)
		: false;
	if (bCompileAfterApply && bWaitForComplete && bFinalOutstandingCompilationRequests)
	{
		bOutstandingAfterStackIssueRead = true;
		TSharedPtr<FJsonObject> FinalCompileParams = MakeShared<FJsonObject>();
		FinalCompileParams->SetBoolField(TEXT("compile_after_set"), true);
		FinalCompileParams->SetBoolField(TEXT("force_compile"), bForceCompile);
		FinalCompileParams->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
		const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult FinalCompileResult =
			UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, FinalCompileParams, true);
		bPostStackCompilationRequested = FinalCompileResult.bCompilationRequested;
		bPostStackCompilationComplete = FinalCompileResult.bCompilationComplete;
		bCompilationRequested = bCompilationRequested || FinalCompileResult.bCompilationRequested;
		bCompilationComplete = FinalCompileResult.bCompilationComplete;
		bFinalOutstandingCompilationRequests = FinalCompileResult.bHasOutstandingCompilationRequests;

		CompileLogReadError.Reset();
		bCompileLogReadOk = true;
		CompileLogData = MakeShared<FJsonObject>();
		FUeAgentRequestContext CompileLogCtx;
		CompileLogCtx.RequestId = TEXT("niagara_apply_folder_post_stack_compile_log");
		CompileLogCtx.Command = TEXT("niagara_get_compile_log");
		CompileLogCtx.Params = MakeShared<FJsonObject>();
		CompileLogCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
		CompileLogCtx.Params->SetBoolField(TEXT("compile_before_read"), false);
		CompileLogCtx.Params->SetBoolField(TEXT("force_compile"), bForceCompile);
		CompileLogCtx.Params->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
		CompileLogCtx.Params->SetBoolField(TEXT("include_events"), true);
		CompileLogCtx.Params->SetStringField(TEXT("severity_filter"), TEXT("warning_or_error"));
		CompileLogCtx.Params->SetNumberField(TEXT("max_events_per_script"), 64);
		if (!CmdNiagaraGetCompileLog(CompileLogCtx, CompileLogData, CompileLogReadError))
		{
			bCompileLogReadOk = false;
			CompileLogData->SetBoolField(TEXT("ok"), false);
			CompileLogData->SetStringField(TEXT("error"), CompileLogReadError);
		}
		bCompileHasError = false;
		if (CompileLogData.IsValid())
		{
			CompileLogData->TryGetBoolField(TEXT("has_error"), bCompileHasError);
		}
	}

	Stats.Add(TEXT("system_properties_applied"), AppliedSystemPropertyCount);
	Stats.Add(TEXT("user_parameters_applied"), AppliedUserParameterCount);
	Stats.Add(TEXT("system_stages_applied"), AppliedSystemStageCount);
	Stats.Add(TEXT("emitter_handles_applied"), AppliedEmitterHandleCount);
	Stats.Add(TEXT("emitter_properties_applied"), AppliedEmitterPropertyCount);
	Stats.Add(TEXT("post_refresh_data_interfaces_applied"), AppliedPostRefreshDataInterfaceCount);
	Stats.Add(TEXT("overview_node_count_before"), RefreshResult.OverviewNodeCountBefore);
	Stats.Add(TEXT("overview_node_count_after"), RefreshResult.OverviewNodeCountAfter);
	Stats.Add(TEXT("stack_total_issue_count"), StackIssueReport.TotalIssueCount);
	Stats.Add(TEXT("stack_error_count"), StackIssueReport.TotalErrorCount);
	Stats.Add(TEXT("stack_warning_count"), StackIssueReport.TotalWarningCount);
	Stats.Add(TEXT("stack_info_count"), StackIssueReport.TotalInfoCount);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildCoverageReport(AssetPath, TEXT("apply"), Warnings, Stats);
	UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, nullptr);

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetNumberField(TEXT("system_properties_applied"), AppliedSystemPropertyCount);
	OutData->SetNumberField(TEXT("user_parameters_applied"), AppliedUserParameterCount);
	OutData->SetNumberField(TEXT("system_stages_applied"), AppliedSystemStageCount);
	OutData->SetNumberField(TEXT("emitter_handles_applied"), AppliedEmitterHandleCount);
	OutData->SetNumberField(TEXT("emitter_properties_applied"), AppliedEmitterPropertyCount);
	OutData->SetNumberField(TEXT("post_refresh_data_interfaces_applied"), AppliedPostRefreshDataInterfaceCount);
	OutData->SetBoolField(TEXT("synchronized_overview_graph"), RefreshResult.bSynchronizedOverviewGraph);
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetNumberField(TEXT("overview_node_count_before"), RefreshResult.OverviewNodeCountBefore);
	OutData->SetNumberField(TEXT("overview_node_count_after"), RefreshResult.OverviewNodeCountAfter);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), bFinalOutstandingCompilationRequests);
	OutData->SetBoolField(TEXT("outstanding_after_stack_issue_read"), bOutstandingAfterStackIssueRead);
	OutData->SetBoolField(TEXT("post_stack_compilation_requested"), bPostStackCompilationRequested);
	OutData->SetBoolField(TEXT("post_stack_compilation_complete"), bPostStackCompilationComplete);
	OutData->SetBoolField(TEXT("compile_log_read_ok"), bCompileLogReadOk);
	OutData->SetStringField(TEXT("compile_log_read_error"), CompileLogReadError);
	OutData->SetBoolField(TEXT("compile_has_error"), bCompileHasError);
	if (CompileLogData.IsValid())
	{
		OutData->SetObjectField(TEXT("compile_log"), CompileLogData);
	}
	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bSaved = false;
	OutData->SetBoolField(TEXT("saved"), bSaved);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	UeAgentNiagaraFolderOps::AppendPostApplyStackIssueFields(OutData, StackIssueReport);
	OutData->SetBoolField(TEXT("restore_preview_after_apply"), bRestorePreviewAfterApply);
	if (bRestorePreviewAfterApply)
	{
		TSharedPtr<FJsonObject> PreviewRestoreReport = UeAgentNiagaraOps::RestoreNiagaraPreviewForInteractiveEditing(NiagaraSystem, TSharedPtr<FNiagaraSystemViewModel>());
		OutData->SetObjectField(TEXT("preview_restore"), PreviewRestoreReport);
	}

	if (bCompileAfterApply && bWaitForComplete && bFinalOutstandingCompilationRequests)
	{
		OutError = TEXT("compile_incomplete_after_apply");
		return false;
	}
	if (bCompileAfterApply && !bCompileLogReadOk)
	{
		OutError = TEXT("compile_log_read_failed_after_apply");
		return false;
	}
	if (bCompileAfterApply && bCompileHasError)
	{
		OutError = TEXT("compile_failed_after_apply");
		return false;
	}
	if (bStrict && StackIssueReport.bCollectAfterApply && !StackIssueReport.bCollectionOk)
	{
		OutError = TEXT("strict_apply_stack_issue_collection_failed");
		return false;
	}
	if (StackIssueReport.bFailOnStackErrors && StackIssueReport.TotalErrorCount > 0)
	{
		OutError = TEXT("strict_apply_has_stack_errors");
		return false;
	}
	if (bStrict && Warnings.Num() > 0)
	{
		OutError = TEXT("strict_apply_has_warnings");
		return false;
	}
	if (bSaveAfterApply)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
		bSaved = true;
		OutData->SetBoolField(TEXT("saved"), bSaved);
	}
	return true;
}

