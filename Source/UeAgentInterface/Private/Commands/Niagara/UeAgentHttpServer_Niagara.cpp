// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Niagara.h"
#include "UeAgentHttpServer_NiagaraOps.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Domains/Niagara/UeAgentNiagaraSystemStructuralRefresh.h"
#include "Domains/Niagara/UeAgentNiagaraRuntimeParticleCapture.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Commands/Level/UeAgentHttpServer_LevelOps.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringApplyProvider.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringScriptResolver.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStackApplyProvider.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraUserParameterAuthoring.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Domains/Niagara/UeAgentNiagaraEmitterCompileContext.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentAuthoringFolderIO.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentAuthoringFolderStats.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Utilities/UeAgentUObjectProperties.h"
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
#include "HAL/PlatformTime.h"
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

namespace
{
	constexpr int32 MaxNiagaraRuntimeParticleSamples = 1024;

	class FScopedPackageDirtyState final
	{
	public:
		explicit FScopedPackageDirtyState(UPackage* InPackage)
			: Package(InPackage)
			, bWasDirty(InPackage && InPackage->IsDirty())
		{
		}

		~FScopedPackageDirtyState()
		{
			Restore();
		}

		bool Restore()
		{
			if (Package && Package->IsDirty() != bWasDirty)
			{
				Package->SetDirtyFlag(bWasDirty);
				bRestored = true;
			}
			return bRestored;
		}

		bool WasDirty() const
		{
			return bWasDirty;
		}

	private:
		UPackage* Package = nullptr;
		bool bWasDirty = false;
		bool bRestored = false;
	};
}
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
	static TSharedPtr<FJsonObject> CollectNiagaraComponentStats(
		UNiagaraSystem* NiagaraSystem,
		UNiagaraComponent* PreviewComponent,
		FString& OutError,
		const UeAgentNiagaraRuntimeParticleCapture::FOptions* ParticleCaptureOptions = nullptr)
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
			if (ParticleCaptureOptions && ParticleCaptureOptions->IsEnabled())
			{
				EmitterObj->SetObjectField(
					TEXT("particle_attribute_capture"),
					UeAgentNiagaraRuntimeParticleCapture::Capture(
						EmitterInstance, *ParticleCaptureOptions));
			}
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

	static bool SetDeprecatedStructPropertyText(UStruct* RootStruct, void* RootContainer, UObject* OwnerForImport, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText = nullptr, FString* OutCppType = nullptr, FString* OutImportError = nullptr)
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

	static bool SetDeprecatedObjectPropertyText(UObject* Target, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText = nullptr, FString* OutCppType = nullptr, FString* OutImportError = nullptr)
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

	static bool SetCanonicalStringProperty(
		UObject* Target,
		const FString& PropertyPath,
		const FString& Value,
		FString& OutAppliedValue,
		FString& OutError)
	{
		OutAppliedValue.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
		Literal->SetStringField(TEXT("literal"), Value);
		TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("property_path"), PropertyPath);
		Descriptor->SetStringField(TEXT("value_kind"), TEXT("string"));
		Descriptor->SetObjectField(TEXT("value"), Literal);
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		Payload->SetArrayField(TEXT("properties"), { MakeShared<FJsonValueObject>(Descriptor) });

		UeAgentUObjectProperties::FApplyOptions ApplyOptions;
		const TSharedPtr<FJsonObject> Summary = UeAgentUObjectProperties::ApplyObjectProperties(Target, Payload, ApplyOptions);
		FString Status;
		if (!Summary.IsValid() || !Summary->TryGetStringField(TEXT("status"), Status) || Status != TEXT("applied"))
		{
			if (!Summary.IsValid() || !Summary->TryGetStringField(TEXT("reason"), OutError))
			{
				OutError = TEXT("canonical_string_property_apply_failed");
			}
			return false;
		}

		FStrProperty* StringProperty = Target ? FindFProperty<FStrProperty>(Target->GetClass(), *PropertyPath) : nullptr;
		if (!StringProperty)
		{
			OutError = TEXT("canonical_string_property_readback_failed");
			return false;
		}
		OutAppliedValue = StringProperty->GetPropertyValue_InContainer(Target);
		return true;
	}

	static TSharedPtr<FJsonObject> BuildPropertyValueJson(UObject& Target, FProperty& Property, void* ValuePtr)
	{
		UeAgentUObjectProperties::FExportOptions ExportOptions;
		ExportOptions.Profile = UeAgentUObjectProperties::EExportProfile::Authoring;
		return UeAgentUObjectProperties::BuildPropertyDescriptor(&Target, &Property, ExportOptions);
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
		FString SharedResolveError;
		if (UeAgentNiagaraStageModuleAuthoring::ResolveNiagaraTypeDefinition(
			InTypeText, OutTypeDefinition, SharedResolveError))
		{
			return true;
		}
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(ParameterObj, TEXT("parameter_name"), FullName, ShortName, TEXT("Niagara/UserParameters"), TEXT("typed_value"));

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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(ParameterObj, TEXT("parameter_name"), FullName, GetVariableLeafName(FullName), TEXT("Niagara/GraphParameters"), TEXT("typed_value"));

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

		bool bModuleIsLinkedAsDynamicInput = false;
		for (UEdGraphPin* Pin : ModuleNode.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || IsParameterMapPin(Pin))
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				for (const UClass* Class = LinkedNode ? LinkedNode->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
				{
					if (Class->GetName().Equals(TEXT("NiagaraNodeParameterMapSet"), ESearchCase::CaseSensitive))
					{
						bModuleIsLinkedAsDynamicInput = true;
						break;
					}
				}
				if (bModuleIsLinkedAsDynamicInput)
				{
					break;
				}
			}
			if (bModuleIsLinkedAsDynamicInput)
			{
				break;
			}
		}

		if (!bModuleIsInOutputStack && !bModuleIsLinkedAsDynamicInput)
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

	static void GatherUpstreamNodesIterative(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes, TArray<UEdGraphNode*>& OutNodes)
	{
		if (!Node)
		{
			return;
		}

		TArray<UEdGraphNode*> PendingNodes;
		PendingNodes.Add(Node);
		while (!PendingNodes.IsEmpty())
		{
			UEdGraphNode* CurrentNode = PendingNodes.Pop(EAllowShrinking::No);
			if (!CurrentNode || VisitedNodes.Contains(CurrentNode))
			{
				continue;
			}

			VisitedNodes.Add(CurrentNode);
			OutNodes.Add(CurrentNode);

			TArray<UEdGraphNode*> UpstreamNodes;
			for (UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input)
				{
					continue;
				}
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin)
					{
						UpstreamNodes.Add(LinkedPin->GetOwningNode());
					}
				}
			}
			for (int32 Index = UpstreamNodes.Num() - 1; Index >= 0; --Index)
			{
				PendingNodes.Add(UpstreamNodes[Index]);
			}
		}
	}

	static void GetStageModuleNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules)
	{
		OutModules.Reset();

		TSet<UEdGraphNode*> VisitedNodes;
		TArray<UEdGraphNode*> StageNodes;
		GatherUpstreamNodesIterative(&OutputNode, VisitedNodes, StageNodes);

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

	static FString BuildInputNameNotFoundError(
		const UNiagaraNodeFunctionCall& ModuleNode,
		const TArray<FResolvedNiagaraModuleInput>& ModuleInputs,
		const FString& InputName)
	{
		TArray<FString> AvailableInputNames;
		AvailableInputNames.Reserve(ModuleInputs.Num());
		for (const FResolvedNiagaraModuleInput& Candidate : ModuleInputs)
		{
			if (!Candidate.FullInputName.IsEmpty()) AvailableInputNames.Add(Candidate.FullInputName);
		}
		return UeAgentNiagaraStageModuleAuthoring::BuildInputNameNotFoundError(
			InputName,
			UeAgentNiagaraStageModuleAuthoring::GetModuleScriptPath(ModuleNode),
			AvailableInputNames);
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
		UeAgentUObjectProperties::FExportOptions ExportOptions;
		ExportOptions.Profile = UeAgentUObjectProperties::EExportProfile::Authoring;
		const TSharedPtr<FJsonObject> Exported = UeAgentUObjectProperties::ExportObjectProperties(Object, ExportOptions);
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		return Exported.IsValid() && Exported->TryGetArrayField(TEXT("properties"), Properties) && Properties
			? *Properties
			: TArray<TSharedPtr<FJsonValue>>();
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

	static UEdGraphPin* FindOverridePinLinkedToDynamicInput(
		UNiagaraNodeFunctionCall& DynamicInputNode)
	{
		for (UEdGraphPin* Pin : DynamicInputNode.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || IsParameterMapPin(Pin))
			{
				continue;
			}
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin
					&& LinkedPin->Direction == EGPD_Input
					&& IsNiagaraParameterMapSetNode(LinkedPin->GetOwningNode()))
				{
					return LinkedPin;
				}
			}
		}
		return nullptr;
	}

	static bool BuildModuleInputJson(
		UNiagaraNodeFunctionCall& ModuleNode,
		const UeAgentNiagaraStageModuleAuthoring::FResolvedModuleInput& InputData,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		const FString& JsonPointer,
		TSharedPtr<FJsonObject>& OutInputObj,
		FString& OutError)
	{
		// Shared budget failures propagate unchanged with
		// resource/limit/consumed/pointer diagnostics.
		enum class ETraversalPhase : uint8
		{
			Enter,
			ExitDynamicInput,
		};

		struct FTraversalFrame
		{
			ETraversalPhase Phase = ETraversalPhase::Enter;
			UNiagaraNodeFunctionCall* OwnerNode = nullptr;
			UeAgentNiagaraStageModuleAuthoring::FResolvedModuleInput Input;
			TSharedPtr<FJsonObject> Output;
			FString Pointer;
			UNiagaraNodeFunctionCall* ActiveDynamicInput = nullptr;
		};

		OutInputObj.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> RootInputObj = MakeShared<FJsonObject>();
		TArray<FTraversalFrame> Pending;
		FTraversalFrame& RootFrame = Pending.AddDefaulted_GetRef();
		RootFrame.OwnerNode = &ModuleNode;
		RootFrame.Input = InputData;
		RootFrame.Output = RootInputObj;
		RootFrame.Pointer = JsonPointer.IsEmpty() ? FString(TEXT("/")) : JsonPointer;

		TSet<const UNiagaraNodeFunctionCall*> ActiveDynamicInputs;
		while (!Pending.IsEmpty())
		{
			FTraversalFrame Frame = Pending.Pop(EAllowShrinking::No);
			if (!ResourceTracker.ChargeWorkUnits(1, Frame.Pointer, OutError))
			{
				return false;
			}
			if (Frame.Phase == ETraversalPhase::ExitDynamicInput)
			{
				ActiveDynamicInputs.Remove(Frame.ActiveDynamicInput);
				continue;
			}
			if (!Frame.OwnerNode || !Frame.Output.IsValid())
			{
				OutError = TEXT("niagara_module_input_query_frame_invalid:pointer=")
					+ Frame.Pointer;
				return false;
			}
			if (!ResourceTracker.ChargeNodes(1, Frame.Pointer, OutError)
				|| !ResourceTracker.ChargeEstimatedMemoryBytes(
					static_cast<int64>(
						sizeof(FTraversalFrame) + sizeof(FJsonObject)),
					Frame.Pointer,
					OutError))
			{
				return false;
			}

			UNiagaraNodeFunctionCall& OwnerNode = *Frame.OwnerNode;
			const UeAgentNiagaraStageModuleAuthoring::FResolvedModuleInput& ResolvedInput =
				Frame.Input;
			const FNiagaraTypeDefinition InputType =
				ResolvedInput.InputVariable.GetType();
			FString ResolvedDynamicInputName;
			UNiagaraNodeFunctionCall* DynamicInputNode =
				UeAgentNiagaraStageModuleAuthoring::
					FindDynamicInputForModuleInput(
						OwnerNode,
						ResolvedInput.FullInputName,
						ResolvedDynamicInputName);

			UEdGraphPin* EffectiveInputPin = DynamicInputNode
				? FindOverridePinLinkedToDynamicInput(*DynamicInputNode)
				: nullptr;
			if (!EffectiveInputPin)
			{
				EffectiveInputPin = ResolvedInput.VisiblePin;
			}
			if (!EffectiveInputPin && ResolvedInput.InputVariable.IsValid())
			{
				const FNiagaraParameterHandle InputHandle(
					ResolvedInput.InputVariable.GetName());
				const FNiagaraParameterHandle AliasedHandle =
					FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
						InputHandle,
						&OwnerNode);
				EffectiveInputPin = FindOverridePinByAliasedInputName(
					Cast<UNiagaraGraph>(OwnerNode.GetGraph()),
					AliasedHandle.GetParameterHandleString());
			}

			const bool bHasLinks = DynamicInputNode
				|| (EffectiveInputPin && !EffectiveInputPin->LinkedTo.IsEmpty());
			bool bHasDefaultOverride = false;
			FString OverrideDefaultValue;
			FString AutogeneratedDefaultValue;
			if (EffectiveInputPin)
			{
				bHasDefaultOverride =
					EffectiveInputPin == ResolvedInput.VisiblePin
						? EffectiveInputPin->DefaultValue
							!= EffectiveInputPin->AutogeneratedDefaultValue
						: !EffectiveInputPin->DefaultValue.IsEmpty()
							|| EffectiveInputPin->DefaultValue
								!= EffectiveInputPin->AutogeneratedDefaultValue;
				OverrideDefaultValue = EffectiveInputPin->DefaultValue;
				AutogeneratedDefaultValue =
					EffectiveInputPin->AutogeneratedDefaultValue;
			}

			Frame.Output->SetStringField(
				TEXT("input_name"),
				ResolvedInput.FullInputName);
			Frame.Output->SetStringField(
				TEXT("input_short_name"),
				ResolvedInput.ShortInputName);
			Frame.Output->SetStringField(
				TEXT("input_type"),
				InputType.GetNameText().ToString());
			Frame.Output->SetObjectField(
				TEXT("input_type_descriptor"),
				BuildNiagaraInputTypeDescriptorJson(
					InputType.GetNameText().ToString(),
					InputType));
			Frame.Output->SetBoolField(
				TEXT("has_override"),
				bHasLinks || bHasDefaultOverride);
			Frame.Output->SetBoolField(TEXT("has_links"), bHasLinks);
			Frame.Output->SetStringField(
				TEXT("override_default_value"),
				OverrideDefaultValue);
			Frame.Output->SetStringField(
				TEXT("autogenerated_default_value"),
				AutogeneratedDefaultValue);
			Frame.Output->SetBoolField(
				TEXT("has_visible_pin"),
				ResolvedInput.VisiblePin != nullptr);

			if (InputType.IsEnum())
			{
				if (UEnum* EnumType = InputType.GetEnum())
				{
					Frame.Output->SetStringField(
						TEXT("enum_type"),
						EnumType->GetPathName());
					auto AppendEnumValueFields =
						[EnumType](
							const TSharedPtr<FJsonObject>& TargetObj,
							const FString& Prefix,
							const FString& ValueText) -> bool
					{
						int64 EnumValue = 0;
						if (ValueText.TrimStartAndEnd().IsEmpty()
							|| !TryResolveNiagaraEnumValueFromText(
								*EnumType,
								ValueText,
								EnumValue))
						{
							return false;
						}
						TargetObj->SetStringField(
							Prefix + TEXT("enum_value_name"),
							EnumType->GetNameStringByValue(EnumValue));
						TargetObj->SetStringField(
							Prefix + TEXT("enum_value_display_name"),
							EnumType->GetDisplayNameTextByValue(EnumValue)
								.ToString());
						TargetObj->SetNumberField(
							Prefix + TEXT("enum_value_int"),
							static_cast<double>(EnumValue));
						return true;
					};

					const bool bWroteOverrideEnumFields =
						AppendEnumValueFields(
							Frame.Output,
							TEXT("override_"),
							OverrideDefaultValue);
					AppendEnumValueFields(
						Frame.Output,
						TEXT("autogenerated_"),
						AutogeneratedDefaultValue);
					const FString& EffectiveEnumValue =
						bWroteOverrideEnumFields
							? OverrideDefaultValue
							: AutogeneratedDefaultValue;
					int64 EnumValue = 0;
					if (TryResolveNiagaraEnumValueFromText(
							*EnumType,
							EffectiveEnumValue,
							EnumValue))
					{
						Frame.Output->SetStringField(
							TEXT("enum_value_name"),
							EnumType->GetNameStringByValue(EnumValue));
						Frame.Output->SetStringField(
							TEXT("enum_value_display_name"),
							EnumType->GetDisplayNameTextByValue(EnumValue)
								.ToString());
						Frame.Output->SetNumberField(
							TEXT("enum_value_int"),
							static_cast<double>(EnumValue));
					}

					TArray<TSharedPtr<FJsonValue>> OptionsJson;
					for (int32 EnumIndex = 0;
						 EnumIndex < EnumType->NumEnums();
						 ++EnumIndex)
					{
						const int64 OptionValue =
							EnumType->GetValueByIndex(EnumIndex);
						if (!IsUserSettableNiagaraEnumValue(
								*EnumType,
								OptionValue))
						{
							continue;
						}
						TSharedPtr<FJsonObject> OptionObj =
							MakeShared<FJsonObject>();
						OptionObj->SetNumberField(TEXT("index"), EnumIndex);
						OptionObj->SetNumberField(
							TEXT("value"),
							static_cast<double>(OptionValue));
						OptionObj->SetStringField(
							TEXT("name"),
							EnumType->GetNameStringByIndex(EnumIndex));
						OptionObj->SetStringField(
							TEXT("display_name"),
							EnumType->GetDisplayNameTextByIndex(EnumIndex)
								.ToString());
						OptionsJson.Add(
							MakeShared<FJsonValueObject>(OptionObj));
					}
					Frame.Output->SetArrayField(
						TEXT("candidates"),
						OptionsJson);
				}
			}

			if (!bHasLinks)
			{
				continue;
			}
			if (!DynamicInputNode)
			{
				UNiagaraNodeInput* LinkedInputNode =
					EffectiveInputPin
					&& EffectiveInputPin->LinkedTo.Num() == 1
					? Cast<UNiagaraNodeInput>(
						EffectiveInputPin->LinkedTo[0]->GetOwningNode())
					: nullptr;
				TSharedPtr<FJsonObject> DataInterfaceObj =
					LinkedInputNode
					? BuildDataInterfaceNodeJson(*LinkedInputNode)
					: nullptr;
				if (DataInterfaceObj.IsValid())
				{
					Frame.Output->SetStringField(
						TEXT("link_kind"),
						TEXT("data_interface"));
					Frame.Output->SetObjectField(
						TEXT("data_interface"),
						DataInterfaceObj);
				}
				else
				{
					FString LinkedParameterName;
					if (TryResolveLinkedInputParameterName(
							EffectiveInputPin,
							LinkedParameterName))
					{
						Frame.Output->SetStringField(
							TEXT("link_kind"),
							TEXT("linked_input"));
						Frame.Output->SetStringField(
							TEXT("linked_parameter_name"),
							LinkedParameterName);
					}
				}
				continue;
			}

			const FString DynamicPointer =
				Frame.Pointer + TEXT("/dynamic_input");
			if (ActiveDynamicInputs.Contains(DynamicInputNode))
			{
				OutError =
					TEXT("niagara_dynamic_input_cycle_detected:pointer=")
					+ DynamicPointer;
				return false;
			}
			if (!ResourceTracker.ChargeNodes(1, DynamicPointer, OutError)
				|| !ResourceTracker.ChargeEstimatedMemoryBytes(
					static_cast<int64>(sizeof(FJsonObject)),
					DynamicPointer,
					OutError))
			{
				return false;
			}
			ActiveDynamicInputs.Add(DynamicInputNode);

			TSharedPtr<FJsonObject> DynamicInputObj =
				MakeShared<FJsonObject>();
			DynamicInputObj->SetStringField(
				TEXT("node_guid"),
				DynamicInputNode->NodeGuid.ToString(
					EGuidFormats::DigitsWithHyphensLower));
			DynamicInputObj->SetStringField(
				TEXT("node_name"),
				DynamicInputNode->GetFunctionName());
			DynamicInputObj->SetStringField(
				TEXT("script_asset_path"),
				GetModuleScriptPath(*DynamicInputNode));

			TArray<UeAgentNiagaraStageModuleAuthoring::FResolvedModuleInput>
				DynamicInputs;
			UeAgentNiagaraStageModuleAuthoring::BuildResolvedModuleInputs(
				*DynamicInputNode,
				DynamicInputs);
			if (!ResourceTracker.ChargeItems(
					DynamicInputs.Num(),
					DynamicPointer + TEXT("/inputs"),
					OutError))
			{
				return false;
			}
			TArray<TSharedPtr<FJsonValue>> DynamicInputsJson;
			DynamicInputsJson.Reserve(DynamicInputs.Num());
			TArray<FTraversalFrame> ChildFrames;
			ChildFrames.Reserve(DynamicInputs.Num());
			for (int32 InputIndex = 0;
				 InputIndex < DynamicInputs.Num();
				 ++InputIndex)
			{
				TSharedPtr<FJsonObject> ChildObj =
					MakeShared<FJsonObject>();
				DynamicInputsJson.Add(
					MakeShared<FJsonValueObject>(ChildObj));
				FTraversalFrame& ChildFrame =
					ChildFrames.AddDefaulted_GetRef();
				ChildFrame.OwnerNode = DynamicInputNode;
				ChildFrame.Input = DynamicInputs[InputIndex];
				ChildFrame.Output = ChildObj;
				ChildFrame.Pointer = FString::Printf(
					TEXT("%s/inputs/%d"),
					*DynamicPointer,
					InputIndex);
			}
			DynamicInputObj->SetArrayField(
				TEXT("inputs"),
				DynamicInputsJson);
			DynamicInputObj->SetNumberField(
				TEXT("input_count"),
				DynamicInputsJson.Num());

			TArray<UNiagaraNodeInput*> DataInterfaceInputNodes;
			CollectLinkedDataInterfaceInputNodesForDynamicInput(
				*DynamicInputNode,
				DataInterfaceInputNodes);
			if (!ResourceTracker.ChargeItems(
					DataInterfaceInputNodes.Num(),
					DynamicPointer + TEXT("/data_interfaces"),
					OutError))
			{
				return false;
			}
			TArray<TSharedPtr<FJsonValue>> DataInterfacesJson;
			DataInterfacesJson.Reserve(DataInterfaceInputNodes.Num());
			for (int32 DataInterfaceIndex = 0;
				 DataInterfaceIndex < DataInterfaceInputNodes.Num();
				 ++DataInterfaceIndex)
			{
				UNiagaraNodeInput* DataInterfaceInputNode =
					DataInterfaceInputNodes[DataInterfaceIndex];
				TSharedPtr<FJsonObject> DataInterfaceObj =
					DataInterfaceInputNode
						? BuildDataInterfaceNodeJson(
							*DataInterfaceInputNode)
						: nullptr;
				if (!DataInterfaceObj.IsValid())
				{
					continue;
				}
				const FString DataInterfacePointer = FString::Printf(
					TEXT("%s/data_interfaces/%d"),
					*DynamicPointer,
					DataInterfaceIndex);
				if (!ResourceTracker.ChargeNodes(
						1,
						DataInterfacePointer,
						OutError)
					|| !ResourceTracker.ChargeEstimatedMemoryBytes(
						static_cast<int64>(sizeof(FJsonObject)),
						DataInterfacePointer,
						OutError))
				{
					return false;
				}
				DataInterfacesJson.Add(
					MakeShared<FJsonValueObject>(DataInterfaceObj));
			}
			DynamicInputObj->SetArrayField(
				TEXT("data_interfaces"),
				DataInterfacesJson);
			DynamicInputObj->SetNumberField(
				TEXT("data_interface_count"),
				DataInterfacesJson.Num());
			Frame.Output->SetStringField(
				TEXT("link_kind"),
				TEXT("dynamic_input"));
			Frame.Output->SetObjectField(
				TEXT("dynamic_input"),
				DynamicInputObj);

			FTraversalFrame& ExitFrame =
				Pending.AddDefaulted_GetRef();
			ExitFrame.Phase = ETraversalPhase::ExitDynamicInput;
			ExitFrame.Pointer = DynamicPointer;
			ExitFrame.ActiveDynamicInput = DynamicInputNode;
			for (int32 ChildIndex = ChildFrames.Num() - 1;
				 ChildIndex >= 0;
				 --ChildIndex)
			{
				Pending.Add(MoveTemp(ChildFrames[ChildIndex]));
			}
		}

		if (!ResourceTracker.Checkpoint(
				JsonPointer.IsEmpty() ? FString(TEXT("/")) : JsonPointer,
				OutError))
		{
			return false;
		}
		OutInputObj = RootInputObj;
		return true;
	}

	static bool BuildModuleInputsJson(
		UNiagaraNodeFunctionCall& ModuleNode,
		const TArray<UeAgentNiagaraStageModuleAuthoring::FResolvedModuleInput>&
			Inputs,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		const FString& JsonPointer,
		TArray<TSharedPtr<FJsonValue>>& OutInputsJson,
		FString& OutError)
	{
		OutInputsJson.Reset();
		if (!ResourceTracker.ChargeItems(
				Inputs.Num(),
				JsonPointer,
				OutError))
		{
			return false;
		}
		OutInputsJson.Reserve(Inputs.Num());
		for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
		{
			TSharedPtr<FJsonObject> InputObj;
			if (!BuildModuleInputJson(
					ModuleNode,
					Inputs[InputIndex],
					ResourceTracker,
					FString::Printf(
						TEXT("%s/%d"),
						*JsonPointer,
						InputIndex),
					InputObj,
					OutError))
			{
				OutInputsJson.Reset();
				return false;
			}
			OutInputsJson.Add(
				MakeShared<FJsonValueObject>(InputObj));
		}
		return true;
	}

	static bool BuildModuleNodeJson(
		UNiagaraNodeFunctionCall& ModuleNode,
		const int32 ModuleIndex,
		const bool bIncludeInputs,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		const FString& JsonPointer,
		TSharedPtr<FJsonObject>& OutModuleObj,
		FString& OutError)
	{
		OutModuleObj.Reset();
		if (!ResourceTracker.ChargeWorkUnits(1, JsonPointer, OutError)
			|| !ResourceTracker.ChargeNodes(1, JsonPointer, OutError)
			|| !ResourceTracker.ChargeEstimatedMemoryBytes(
				static_cast<int64>(sizeof(FJsonObject)),
				JsonPointer,
				OutError))
		{
			return false;
		}

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
			TArray<UeAgentNiagaraStageModuleAuthoring::FResolvedModuleInput>
				Inputs;
			UeAgentNiagaraStageModuleAuthoring::BuildResolvedModuleInputs(
				ModuleNode,
				Inputs);
			TArray<TSharedPtr<FJsonValue>> InputsJson;
			if (!BuildModuleInputsJson(
					ModuleNode,
					Inputs,
					ResourceTracker,
					JsonPointer + TEXT("/inputs"),
					InputsJson,
					OutError))
			{
				return false;
			}
			ModuleObj->SetArrayField(TEXT("inputs"), InputsJson);
			ModuleObj->SetNumberField(TEXT("input_count"), InputsJson.Num());
		}

		if (!ResourceTracker.Checkpoint(JsonPointer, OutError))
		{
			return false;
		}
		OutModuleObj = ModuleObj;
		return true;
	}

	static bool BuildStageNodeJson(
		UEdGraphNode& StageNode,
		const int32 StageNodeIndex,
		const bool bIncludeProperties,
		const bool bIncludeModuleInputs,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		const FString& JsonPointer,
		TSharedPtr<FJsonObject>& OutNodeObj,
		FString& OutError)
	{
		OutNodeObj.Reset();
		if (!ResourceTracker.ChargeWorkUnits(1, JsonPointer, OutError)
			|| !ResourceTracker.ChargeNodes(1, JsonPointer, OutError)
			|| !ResourceTracker.ChargeEstimatedMemoryBytes(
				static_cast<int64>(sizeof(FJsonObject)),
				JsonPointer,
				OutError))
		{
			return false;
		}

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
				TArray<UeAgentNiagaraStageModuleAuthoring::FResolvedModuleInput>
					Inputs;
				UeAgentNiagaraStageModuleAuthoring::BuildResolvedModuleInputs(
					*ModuleNode,
					Inputs);

				TArray<TSharedPtr<FJsonValue>> InputsJson;
				if (!BuildModuleInputsJson(
						*ModuleNode,
						Inputs,
						ResourceTracker,
						JsonPointer + TEXT("/inputs"),
						InputsJson,
						OutError))
				{
					return false;
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

				const FString PropertyPointer = FString::Printf(
					TEXT("%s/properties/%d"),
					*JsonPointer,
					PropertiesJson.Num());
				if (!ResourceTracker.ChargeWorkUnits(
						1,
						PropertyPointer,
						OutError)
					|| !ResourceTracker.ChargeItems(
						1,
						PropertyPointer,
						OutError)
					|| !ResourceTracker.ChargeNodes(
						1,
						PropertyPointer,
						OutError)
					|| !ResourceTracker.ChargeEstimatedMemoryBytes(
						static_cast<int64>(sizeof(FJsonObject)),
						PropertyPointer,
						OutError))
				{
					return false;
				}
				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(&StageNode);
				PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildPropertyValueJson(StageNode, *Property, ValuePtr)));
			}
			NodeObj->SetArrayField(TEXT("properties"), PropertiesJson);
			NodeObj->SetNumberField(TEXT("property_count"), PropertiesJson.Num());
		}

		if (!ResourceTracker.Checkpoint(JsonPointer, OutError))
		{
			return false;
		}
		OutNodeObj = NodeObj;
		return true;
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
		GatherUpstreamNodesIterative(OutStage.OutputNode, VisitedNodes, StageNodes);

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
			GatherUpstreamNodesIterative(OutputNode, VisitedNodes, StageNodes);
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
		FString EmitterIdString;
		FString EmitterName;
		const bool bHasEmitterIndex = Params.IsValid() && Params->TryGetNumberField(TEXT("emitter_index"), HandleIndexNumber);
		const bool bHasEmitterId = Params.IsValid()
			&& Params->TryGetStringField(TEXT("emitter_id"), EmitterIdString)
			&& !EmitterIdString.IsEmpty();
		const bool bHasEmitterName = Params.IsValid()
			&& Params->TryGetStringField(TEXT("emitter_name"), EmitterName)
			&& !EmitterName.IsEmpty();
		const int32 SelectorCount = (bHasEmitterIndex ? 1 : 0) + (bHasEmitterId ? 1 : 0) + (bHasEmitterName ? 1 : 0);
		if (SelectorCount > 1)
		{
			OutError = TEXT("emitter_selector_conflict");
			return false;
		}

		if (bHasEmitterIndex)
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

		if (bHasEmitterId)
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

		if (bHasEmitterName)
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



namespace UeAgentNiagaraOps
{
	using FNiagaraModuleSelector = UeAgentNiagaraStageModuleAuthoring::FModuleSelector;

	static bool TryReadNiagaraModuleSelector(
		const TSharedPtr<FJsonObject>& AuthoringObject,
		FNiagaraModuleSelector& OutSelector,
		FString& OutError)
	{
		OutSelector = FNiagaraModuleSelector();
		OutError.Reset();
		if (!AuthoringObject.IsValid())
		{
			OutError = TEXT("module_selector_object_missing");
			return false;
		}

		TSharedPtr<FJsonObject> SelectorObject = AuthoringObject;
		const TSharedPtr<FJsonObject>* NestedSelector = nullptr;
		if (AuthoringObject->TryGetObjectField(TEXT("selector"), NestedSelector) && NestedSelector && NestedSelector->IsValid())
		{
			SelectorObject = *NestedSelector;
		}

		FString NodeGuidText;
		SelectorObject->TryGetStringField(TEXT("module_node_guid"), NodeGuidText);
		NodeGuidText.TrimStartAndEndInline();
		if (!NodeGuidText.IsEmpty() && !FGuid::Parse(NodeGuidText, OutSelector.NodeGuid))
		{
			OutError = TEXT("invalid_module_node_guid");
			return false;
		}
		if (!SelectorObject->TryGetStringField(TEXT("path"), OutSelector.ScriptPath)
			&& !SelectorObject->TryGetStringField(TEXT("module_script_path"), OutSelector.ScriptPath))
		{
			SelectorObject->TryGetStringField(TEXT("module_script_asset_path"), OutSelector.ScriptPath);
		}
		OutSelector.ScriptPath.TrimStartAndEndInline();
		if (!SelectorObject->TryGetStringField(TEXT("module_name"), OutSelector.Name))
		{
			SelectorObject->TryGetStringField(TEXT("name"), OutSelector.Name);
		}
		OutSelector.Name.TrimStartAndEndInline();

		double Occurrence = 0.0;
		if (SelectorObject->TryGetNumberField(TEXT("occurrence"), Occurrence))
		{
			OutSelector.Occurrence = FMath::Max(0, FMath::RoundToInt(Occurrence));
			OutSelector.bOccurrenceExplicit = true;
		}
		return true;
	}

	static bool ResolveStageModuleBySelector(
		UNiagaraNodeOutput& OutputNode,
		const FNiagaraModuleSelector& Selector,
		UNiagaraNodeFunctionCall*& OutModuleNode,
		int32& OutModuleIndex,
		FString& OutError)
	{
		return UeAgentNiagaraStageModuleAuthoring::ResolveModule(
			OutputNode,
			Selector,
			OutModuleNode,
			OutModuleIndex,
			OutError);
	}

	static bool ResolveNiagaraModuleScriptFromAuthoring(
		const TSharedPtr<FJsonObject>& ModuleObject,
		UNiagaraScript*& OutModuleScript,
		FString& OutResolvedPath,
		FString& OutError)
	{
		UeAgentNiagaraAuthoring::FScriptResolveRequest ResolveRequest;
		ResolveRequest.Kind = UeAgentNiagaraAuthoring::EScriptKind::Module;
		ResolveRequest.Input = ModuleObject;
		UeAgentNiagaraAuthoring::FScriptResolveResult ResolveResult;
		if (!UeAgentNiagaraAuthoring::ResolveScript(ResolveRequest, ResolveResult, OutError))
		{
			OutModuleScript = nullptr;
			OutResolvedPath.Reset();
			return false;
		}
		OutModuleScript = ResolveResult.Script;
		OutResolvedPath = ResolveResult.ResolvedObjectPath;
		return true;
	}

	static bool HasNiagaraModulePositionDirective(const TSharedPtr<FJsonObject>& ModuleObject)
	{
		return ModuleObject.IsValid()
			&& (ModuleObject->HasField(TEXT("target_index"))
				|| ModuleObject->HasField(TEXT("insert_before"))
				|| ModuleObject->HasField(TEXT("insert_after")));
	}

	static bool ResolveNiagaraModuleTargetIndex(
		UNiagaraNodeOutput& OutputNode,
		const TSharedPtr<FJsonObject>& ModuleObject,
		int32& OutTargetIndex,
		FString& OutError,
		const UNiagaraNodeFunctionCall* MovingModule = nullptr)
	{
		OutTargetIndex = INDEX_NONE;
		OutError.Reset();
		if (!ModuleObject.IsValid())
		{
			return true;
		}
		double TargetIndex = 0.0;
		if (ModuleObject->TryGetNumberField(TEXT("target_index"), TargetIndex))
		{
			OutTargetIndex = FMath::Max(0, FMath::RoundToInt(TargetIndex));
			return true;
		}

		auto ResolveRelativeIndex = [&OutputNode, &ModuleObject, &OutTargetIndex, &OutError, MovingModule](const TCHAR* FieldName, const bool bAfter)
		{
			TSharedPtr<FJsonObject> SelectorObject;
			const TSharedPtr<FJsonObject>* SelectorObjectPtr = nullptr;
			if (ModuleObject->TryGetObjectField(FieldName, SelectorObjectPtr) && SelectorObjectPtr && SelectorObjectPtr->IsValid())
			{
				SelectorObject = *SelectorObjectPtr;
			}
			else
			{
				FString Name;
				if (!ModuleObject->TryGetStringField(FieldName, Name) || Name.TrimStartAndEnd().IsEmpty())
				{
					return true;
				}
				SelectorObject = MakeShared<FJsonObject>();
				SelectorObject->SetStringField(TEXT("name"), Name);
			}
			FNiagaraModuleSelector Selector;
			if (!TryReadNiagaraModuleSelector(SelectorObject, Selector, OutError))
			{
				return false;
			}
			UNiagaraNodeFunctionCall* RelativeModule = nullptr;
			int32 RelativeIndex = INDEX_NONE;
			if (!ResolveStageModuleBySelector(OutputNode, Selector, RelativeModule, RelativeIndex, OutError))
			{
				return false;
			}
			int32 SourceIndex = INDEX_NONE;
			if (MovingModule)
			{
				TArray<UNiagaraNodeFunctionCall*> Modules;
				UeAgentNiagaraStageModuleAuthoring::GetModules(OutputNode, Modules);
				SourceIndex = Modules.IndexOfByKey(MovingModule);
			}
			OutTargetIndex = RelativeIndex + (bAfter ? 1 : 0);
			if (SourceIndex != INDEX_NONE && SourceIndex < RelativeIndex)
			{
				--OutTargetIndex;
			}
			return true;
		};

		if (ModuleObject->HasField(TEXT("insert_before")))
		{
			return ResolveRelativeIndex(TEXT("insert_before"), false);
		}
		if (ModuleObject->HasField(TEXT("insert_after")))
		{
			return ResolveRelativeIndex(TEXT("insert_after"), true);
		}
		return true;
	}

	static bool MoveNiagaraStageModuleToIndex(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNodeFunctionCall& ModuleNode,
		const int32 TargetIndex,
		int32& OutNewIndex,
		FString& OutError)
	{
		return UeAgentNiagaraStageModuleAuthoring::MoveModule(
			OutputNode,
			ModuleNode,
			TargetIndex,
			OutNewIndex,
			OutError);
	}

	static UNiagaraNodeFunctionCall* AddNiagaraStageModuleToStack(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraScript& ModuleScript,
		const FString& SuggestedName,
		const int32 TargetIndex,
		FString& OutError)
	{
		return UeAgentNiagaraStageModuleAuthoring::AddModule(
			OutputNode,
			ModuleScript,
			SuggestedName,
			TargetIndex,
			OutError);
	}

	static UNiagaraNodeFunctionCall* AddNiagaraStageModuleFromAuthoring(
		UNiagaraNodeOutput& OutputNode,
		const TSharedPtr<FJsonObject>& ModuleObject,
		const int32 TargetIndex,
		FString& OutResolvedPath,
		FString& OutError)
	{
		UNiagaraScript* ModuleScript = nullptr;
		if (!ResolveNiagaraModuleScriptFromAuthoring(ModuleObject, ModuleScript, OutResolvedPath, OutError))
		{
			return nullptr;
		}
		FString SuggestedName;
		ModuleObject->TryGetStringField(TEXT("module_name"), SuggestedName);
		SuggestedName.TrimStartAndEndInline();
		return AddNiagaraStageModuleToStack(OutputNode, *ModuleScript, SuggestedName, TargetIndex, OutError);
	}
}
#include "Commands/EditorViewport/UeAgentEditorViewportLegacy.h"
#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"

bool FUeAgentLegacyMechanics::CmdNiagaraListAssets(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraListModuleLibrary(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraCreateSystem(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	bool bCreateDefaultNodes = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_default_nodes"), bCreateDefaultNodes);
	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);

	UeAgentNiagaraAuthoring::FAssetApplyRequest ApplyRequest;
	ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Create;
	ApplyRequest.AssetPath = AssetPathInput;
	ApplyRequest.AssetKind = TEXT("system");
	ApplyRequest.bCreateDefaultSystemNodes = bCreateDefaultNodes;
	ApplyRequest.bSave = bSaveAfterCreate;
	UeAgentNiagaraAuthoring::FAssetApplyResult ApplyResult;
	if (!UeAgentNiagaraAuthoring::ApplyAsset(ApplyRequest, ApplyResult, Ctx.RequestId))
	{
		OutError = ApplyResult.ErrorCode;
		return false;
	}
	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(ApplyResult.Asset);
	if (!NiagaraSystem)
	{
		OutError = TEXT("create_niagara_system_failed");
		return false;
	}

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

	OutData->SetStringField(TEXT("asset_path"), ApplyResult.AssetPath);
	OutData->SetStringField(TEXT("asset_name"), NiagaraSystem->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetBoolField(TEXT("created_default_nodes"), bCreateDefaultNodes);
	OutData->SetBoolField(TEXT("found_stale_object_before_create"), false);
	OutData->SetBoolField(TEXT("detached_stale_object_before_create"), false);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	OutData->SetStringField(TEXT("apply_backend"), TEXT("unified_operation_apply"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraCreateEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	bool bAddDefaultModulesAndRenderers = true;
	JsonTryGetBool(Ctx.Params, TEXT("add_default_modules_and_renderers"), bAddDefaultModulesAndRenderers);
	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);

	UeAgentNiagaraAuthoring::FAssetApplyRequest ApplyRequest;
	ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Create;
	ApplyRequest.AssetPath = AssetPathInput;
	ApplyRequest.AssetKind = TEXT("emitter");
	ApplyRequest.bAddDefaultEmitterModulesAndRenderers = bAddDefaultModulesAndRenderers;
	ApplyRequest.bSave = bSaveAfterCreate;
	UeAgentNiagaraAuthoring::FAssetApplyResult ApplyResult;
	if (!UeAgentNiagaraAuthoring::ApplyAsset(ApplyRequest, ApplyResult, Ctx.RequestId))
	{
		OutError = ApplyResult.ErrorCode;
		return false;
	}
	UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(ApplyResult.Asset);
	if (!NiagaraEmitter)
	{
		OutError = TEXT("create_niagara_emitter_failed");
		return false;
	}

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(NiagaraEmitter);
		}
	}

	OutData->SetStringField(TEXT("asset_path"), ApplyResult.AssetPath);
	OutData->SetStringField(TEXT("asset_name"), NiagaraEmitter->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraEmitter->GetPathName());
	OutData->SetBoolField(TEXT("added_default_modules_and_renderers"), bAddDefaultModulesAndRenderers);
	OutData->SetBoolField(TEXT("found_stale_object_before_create"), false);
	OutData->SetBoolField(TEXT("detached_stale_object_before_create"), false);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	OutData->SetStringField(TEXT("apply_backend"), TEXT("unified_operation_apply"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraDeleteAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	UeAgentNiagaraAuthoring::FAssetApplyRequest ApplyRequest;
	ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Remove;
	ApplyRequest.AssetPath = DeletedAssetPath;
	ApplyRequest.AssetKind = AssetKind;
	ApplyRequest.bForceDelete = bForceDelete;
	ApplyRequest.bUseUncheckedDelete = bUseUncheckedDeletePath;
	UeAgentNiagaraAuthoring::FAssetApplyResult ApplyResult;
	if (!UeAgentNiagaraAuthoring::ApplyAsset(ApplyRequest, ApplyResult, Ctx.RequestId))
	{
		OutError = ApplyResult.ErrorCode;
		return false;
	}

	OutData->SetStringField(TEXT("deleted_asset_path"), DeletedAssetPath);
	OutData->SetStringField(TEXT("deleted_object_path"), DeletedObjectPath);
	OutData->SetStringField(TEXT("asset_kind"), AssetKind);
	OutData->SetBoolField(TEXT("force_delete"), bForceDelete);
	OutData->SetBoolField(TEXT("package_exists_on_disk"), bPackageExistsOnDisk);
	OutData->SetBoolField(TEXT("use_unchecked_delete"), bUseUncheckedDeletePath);
	OutData->SetBoolField(TEXT("found_residual_object_after_delete"), false);
	OutData->SetBoolField(TEXT("detached_residual_object_after_delete"), false);
	OutData->SetStringField(TEXT("delete_strategy"), bUseUncheckedDeletePath ? TEXT("delete_objects_unchecked") : (bForceDelete ? TEXT("force_delete_objects") : TEXT("delete_single_object")));
	OutData->SetStringField(TEXT("apply_backend"), TEXT("unified_operation_apply"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraDuplicateAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	UeAgentNiagaraAuthoring::FAssetApplyRequest ApplyRequest;
	ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Clone;
	ApplyRequest.AssetPath = TargetAssetPathInput;
	ApplyRequest.AssetKind = UeAgentNiagaraOps::GetNiagaraAssetKind(SourceAsset);
	ApplyRequest.SourceAssetPath = SourceAssetPathInput;
	ApplyRequest.bSave = bSaveAfterCreate;
	UeAgentNiagaraAuthoring::FAssetApplyResult ApplyResult;
	if (!UeAgentNiagaraAuthoring::ApplyAsset(ApplyRequest, ApplyResult, Ctx.RequestId))
	{
		OutError = ApplyResult.ErrorCode;
		return false;
	}
	UObject* DuplicatedAsset = ApplyResult.Asset;
	if (!DuplicatedAsset)
	{
		OutError = TEXT("duplicate_asset_failed");
		return false;
	}

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(DuplicatedAsset);
		}
	}

	OutData->SetStringField(TEXT("source_asset_path"), SourceAsset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("target_asset_path"), ApplyResult.AssetPath);
	OutData->SetStringField(TEXT("target_object_path"), DuplicatedAsset->GetPathName());
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(DuplicatedAsset));
	OutData->SetNumberField(TEXT("repaired_emitter_version_count"), ApplyResult.RepairedEmitterVersionCount);
	OutData->SetBoolField(TEXT("found_stale_target_object_before_duplicate"), false);
	OutData->SetBoolField(TEXT("detached_stale_target_object_before_duplicate"), false);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	OutData->SetStringField(TEXT("apply_backend"), TEXT("unified_operation_apply"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraPreviewAdvance(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	FScopedPackageDirtyState PackageDirtyState(NiagaraSystem->GetOutermost());

	bool bOpenEditorIfNeeded = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	if (NiagaraSystem->HasOutstandingCompilationRequests(true))
	{
		NiagaraSystem->WaitForCompilationComplete(true, false);
		NiagaraSystem->PollForCompilationComplete(true);
	}
	NiagaraSystem->CacheFromCompiledData();

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

	FString StatsError;
	TSharedPtr<FJsonObject> Stats = UeAgentNiagaraOps::CollectNiagaraComponentStats(NiagaraSystem, PreviewComponent, StatsError);
	const bool bRestoredDirtyState = PackageDirtyState.Restore();

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
	OutData->SetBoolField(TEXT("was_package_dirty_before_advance"), PackageDirtyState.WasDirty());
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

bool FUeAgentLegacyMechanics::CmdNiagaraScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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


bool FUeAgentLegacyMechanics::CmdNiagaraSystemRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	FScopedPackageDirtyState PackageDirtyState(NiagaraSystem->GetOutermost());

	UeAgentNiagaraRuntimeParticleCapture::FOptions ParticleCaptureOptions;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("particle_attributes")))
	{
		const TArray<TSharedPtr<FJsonValue>>* AttributeValues = nullptr;
		if (!Ctx.Params->TryGetArrayField(TEXT("particle_attributes"), AttributeValues)
			|| !AttributeValues || AttributeValues->Num() > 16)
		{
			OutError = TEXT("particle_attributes_invalid");
			return false;
		}
		TSet<FString> SeenAttributes;
		for (const TSharedPtr<FJsonValue>& AttributeValue : *AttributeValues)
		{
			FString AttributeName;
			if (!AttributeValue.IsValid() || !AttributeValue->TryGetString(AttributeName))
			{
				OutError = TEXT("particle_attribute_name_invalid");
				return false;
			}
			AttributeName.TrimStartAndEndInline();
			FString ComparisonName = AttributeName.ToLower();
			if (AttributeName.IsEmpty() || AttributeName.Len() > 128 || SeenAttributes.Contains(ComparisonName))
			{
				OutError = TEXT("particle_attribute_name_invalid");
				return false;
			}
			SeenAttributes.Add(ComparisonName);
			ParticleCaptureOptions.AttributeNames.Add(AttributeName);
		}
	}
	double MaxParticleSamplesNumber = 64.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_particle_samples"), MaxParticleSamplesNumber);
	if (!FMath::IsFinite(MaxParticleSamplesNumber)
		|| MaxParticleSamplesNumber < 1.0
		|| MaxParticleSamplesNumber > static_cast<double>(MaxNiagaraRuntimeParticleSamples)
		|| !FMath::IsNearlyEqual(MaxParticleSamplesNumber, FMath::RoundToDouble(MaxParticleSamplesNumber)))
	{
		OutError = TEXT("max_particle_samples_invalid");
		return false;
	}
	ParticleCaptureOptions.MaxParticleSamples = FMath::RoundToInt(MaxParticleSamplesNumber);

	bool bOpenEditorIfNeeded = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	if (NiagaraSystem->HasOutstandingCompilationRequests(true))
	{
		NiagaraSystem->WaitForCompilationComplete(true, false);
		NiagaraSystem->PollForCompilationComplete(true);
	}
	NiagaraSystem->CacheFromCompiledData();
	FString SampleMode = TEXT("simulate");
	if (!JsonTryGetString(Ctx.Params, TEXT("sample_mode"), SampleMode))
	{
		JsonTryGetString(Ctx.Params, TEXT("probe_mode"), SampleMode);
	}
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
	const bool bSampleCurrentLevelComponent =
		NormalizedSampleMode == TEXT("current_level") ||
		NormalizedSampleMode == TEXT("current_level_component") ||
		NormalizedSampleMode == TEXT("readonly_current_level_component");

	FString LevelActorId;
	FString LevelComponentId;
	JsonTryGetString(Ctx.Params, TEXT("actor_id"), LevelActorId);
	JsonTryGetString(Ctx.Params, TEXT("component"), LevelComponentId);
	LevelActorId.TrimStartAndEndInline();
	LevelComponentId.TrimStartAndEndInline();
	const bool bHasLevelActorId = !LevelActorId.IsEmpty();
	const bool bHasLevelComponentId = !LevelComponentId.IsEmpty();
	if (bHasLevelActorId != bHasLevelComponentId)
	{
		OutError = TEXT("current_level_sample_requires_actor_component");
		return false;
	}
	const bool bUseLevelComponent = bHasLevelActorId && bHasLevelComponentId;
	if (bUseLevelComponent != bSampleCurrentLevelComponent)
	{
		OutError = bUseLevelComponent
			? TEXT("level_component_sample_requires_current_level")
			: TEXT("current_level_sample_requires_actor_component");
		return false;
	}

	bool bUseTransientComponent = false;
	JsonTryGetBool(Ctx.Params, TEXT("use_transient_component"), bUseTransientComponent);
	if (!bUseTransientComponent)
	{
		JsonTryGetBool(Ctx.Params, TEXT("use_transient_preview_component"), bUseTransientComponent);
	}
	if ((bSampleCurrentPreview || bSampleCurrentLevelComponent) && bUseTransientComponent)
	{
		OutError = bSampleCurrentPreview
			? TEXT("current_preview_sample_does_not_support_transient_component")
			: TEXT("current_level_sample_does_not_support_transient_component");
		return false;
	}

	TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
	UNiagaraComponent* PreviewComponent = nullptr;
	if (bUseLevelComponent)
	{
		if (!GEditor)
		{
			OutError = TEXT("editor_unavailable");
			return false;
		}
		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		if (!EditorWorld)
		{
			OutError = TEXT("editor_world_unavailable");
			return false;
		}

		AActor* Actor = FindActorByNameOrLabel(EditorWorld, LevelActorId);
		if (!Actor)
		{
			OutError = TEXT("level_component_actor_not_found");
			return false;
		}
		UActorComponent* ActorComponent = UeAgentLevelOps::FindComponentByNameOrPath(Actor, LevelComponentId);
		if (!ActorComponent)
		{
			OutError = TEXT("level_component_not_found");
			return false;
		}
		PreviewComponent = Cast<UNiagaraComponent>(ActorComponent);
		if (!PreviewComponent)
		{
			OutError = TEXT("level_component_not_niagara");
			return false;
		}
		if (PreviewComponent->GetAsset() != NiagaraSystem)
		{
			OutError = TEXT("level_component_asset_mismatch");
			return false;
		}
	}
	else if (bUseTransientComponent)
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

	bool bMeasurePerformance = false;
	JsonTryGetBool(Ctx.Params, TEXT("measure_performance"), bMeasurePerformance);
	double PerformanceWarmupTickCountNumber = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("performance_warmup_ticks"), PerformanceWarmupTickCountNumber);
	if (!FMath::IsFinite(PerformanceWarmupTickCountNumber)
		|| PerformanceWarmupTickCountNumber < 0.0
		|| PerformanceWarmupTickCountNumber > static_cast<double>(TickCount)
		|| !FMath::IsNearlyEqual(PerformanceWarmupTickCountNumber, FMath::RoundToDouble(PerformanceWarmupTickCountNumber)))
	{
		OutError = TEXT("performance_warmup_ticks_invalid");
		return false;
	}
	const int32 PerformanceWarmupTickCount = FMath::RoundToInt(PerformanceWarmupTickCountNumber);
	if (bMeasurePerformance && (bSampleCurrentPreview || bSampleCurrentLevelComponent))
	{
		OutError = TEXT("performance_measurement_requires_simulate");
		return false;
	}
	if (bMeasurePerformance && PerformanceWarmupTickCount >= TickCount)
	{
		OutError = TEXT("performance_warmup_ticks_invalid");
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
	TArray<double> PerformanceAdvanceSeconds;
	TArray<double> PerformanceFlushSeconds;
	TArray<double> PerformanceCombinedSeconds;

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

	auto AddSnapshot = [&Snapshots, &EmitterPeaks, &CollectedSnapshotCount, &FirstSnapshotLabel, &FinalSnapshotLabel, &InitialStats, &FinalStats, &UpdateEmitterPeaks, bIncludeSnapshots, NiagaraSystem, &PreviewComponent, &ParticleCaptureOptions](const FString& Label)
	{
		FString StatsError;
		TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetStringField(TEXT("label"), Label);
		TSharedPtr<FJsonObject> Stats = UeAgentNiagaraOps::CollectNiagaraComponentStats(
			NiagaraSystem,
			PreviewComponent,
			StatsError,
			ParticleCaptureOptions.IsEnabled() ? &ParticleCaptureOptions : nullptr);
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

	if (bSampleCurrentLevelComponent)
	{
		AddSnapshot(TEXT("current_level_component"));

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
		OutData->SetStringField(TEXT("sample_mode"), TEXT("current_level_component"));
		OutData->SetStringField(TEXT("component_source"), TEXT("level_component"));
		OutData->SetStringField(TEXT("actor_id"), LevelActorId);
		OutData->SetStringField(TEXT("component"), LevelComponentId);
		OutData->SetStringField(TEXT("component_name"), PreviewComponent->GetName());
		OutData->SetStringField(TEXT("component_path"), PreviewComponent->GetPathName());
		OutData->SetStringField(TEXT("component_world"), PreviewComponent->GetWorld() ? PreviewComponent->GetWorld()->GetPathName() : FString());
		OutData->SetStringField(TEXT("advance_mode"), TEXT("none"));
		OutData->SetStringField(TEXT("advance_semantics"), TEXT("readonly_current_level_component"));
		OutData->SetNumberField(TEXT("tick_count"), 0);
		OutData->SetNumberField(TEXT("tick_delta_seconds"), TickDeltaSeconds);
		OutData->SetBoolField(TEXT("reset_preview"), false);
		OutData->SetBoolField(TEXT("include_snapshots"), bIncludeSnapshots);
		OutData->SetBoolField(TEXT("mutated_preview_component"), false);
		OutData->SetBoolField(TEXT("used_seek"), false);
		OutData->SetBoolField(TEXT("restored_dirty_state"), false);
		if (InitialStats.IsValid())
		{
			OutData->SetObjectField(TEXT("initial_stats"), InitialStats);
			OutData->SetObjectField(TEXT("final_stats"), FinalStats);
		}
		OutData->SetObjectField(TEXT("summary"), Summary);
		OutData->SetArrayField(TEXT("snapshots"), Snapshots);
		OutData->SetNumberField(TEXT("snapshot_count"), CollectedSnapshotCount);
		OutData->SetNumberField(TEXT("snapshots_emitted_count"), Snapshots.Num());
		return FinalStats.IsValid();
	}

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
		TSharedPtr<FJsonObject> Stats = UeAgentNiagaraOps::CollectNiagaraComponentStats(
			NiagaraSystem,
			PreviewComponent,
			StatsError,
			ParticleCaptureOptions.IsEnabled() ? &ParticleCaptureOptions : nullptr);
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
		const double TickStartSeconds = FPlatformTime::Seconds();
		if (bUseAdvanceSimulation)
		{
			PreviewComponent->AdvanceSimulation(1, TickDeltaSeconds);
		}
		else
		{
			PreviewComponent->TickComponent(TickDeltaSeconds, ELevelTick::LEVELTICK_All, nullptr);
		}
		const double AdvanceEndSeconds = FPlatformTime::Seconds();
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		const double FlushEndSeconds = FPlatformTime::Seconds();
		if (bMeasurePerformance && TickIndex >= PerformanceWarmupTickCount)
		{
			PerformanceAdvanceSeconds.Add(AdvanceEndSeconds - TickStartSeconds);
			PerformanceFlushSeconds.Add(FlushEndSeconds - AdvanceEndSeconds);
			PerformanceCombinedSeconds.Add(FlushEndSeconds - TickStartSeconds);
		}
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
	const bool bRestoredDirtyState = PackageDirtyState.Restore();

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
	OutData->SetBoolField(TEXT("was_package_dirty_before_probe"), PackageDirtyState.WasDirty());
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
	if (bMeasurePerformance)
	{
		auto BuildTimingSummary = [](const TArray<double>& SamplesSeconds) -> TSharedPtr<FJsonObject>
		{
			TArray<double> SamplesMilliseconds;
			SamplesMilliseconds.Reserve(SamplesSeconds.Num());
			double TotalMilliseconds = 0.0;
			for (const double SampleSeconds : SamplesSeconds)
			{
				const double SampleMilliseconds = SampleSeconds * 1000.0;
				SamplesMilliseconds.Add(SampleMilliseconds);
				TotalMilliseconds += SampleMilliseconds;
			}
			SamplesMilliseconds.Sort();

			TSharedPtr<FJsonObject> Timing = MakeShared<FJsonObject>();
			Timing->SetNumberField(TEXT("sample_count"), SamplesMilliseconds.Num());
			Timing->SetNumberField(TEXT("total_ms"), TotalMilliseconds);
			if (SamplesMilliseconds.Num() > 0)
			{
				const int32 MedianIndex = FMath::Clamp(
					FMath::CeilToInt(0.50 * SamplesMilliseconds.Num()) - 1,
					0,
					SamplesMilliseconds.Num() - 1);
				const int32 P95Index = FMath::Clamp(
					FMath::CeilToInt(0.95 * SamplesMilliseconds.Num()) - 1,
					0,
					SamplesMilliseconds.Num() - 1);
				Timing->SetNumberField(TEXT("min_ms"), SamplesMilliseconds[0]);
				Timing->SetNumberField(TEXT("mean_ms"), TotalMilliseconds / SamplesMilliseconds.Num());
				Timing->SetNumberField(TEXT("median_ms"), SamplesMilliseconds[MedianIndex]);
				Timing->SetNumberField(TEXT("p95_ms"), SamplesMilliseconds[P95Index]);
				Timing->SetNumberField(TEXT("max_ms"), SamplesMilliseconds.Last());
			}
			return Timing;
		};

		TSharedPtr<FJsonObject> Performance = MakeShared<FJsonObject>();
		Performance->SetStringField(TEXT("measurement_kind"), TEXT("synchronous_preview_component_wall_clock"));
		Performance->SetStringField(TEXT("clock_source"), TEXT("FPlatformTime::Seconds"));
		Performance->SetStringField(TEXT("advance_mode"), bUseAdvanceSimulation ? TEXT("advance_simulation") : TEXT("tick_component"));
		Performance->SetNumberField(TEXT("warmup_tick_count"), PerformanceWarmupTickCount);
		Performance->SetNumberField(TEXT("measured_tick_count"), PerformanceCombinedSeconds.Num());
		Performance->SetNumberField(TEXT("tick_delta_seconds"), TickDeltaSeconds);
		Performance->SetNumberField(TEXT("simulated_seconds"), PerformanceCombinedSeconds.Num() * TickDeltaSeconds);
		Performance->SetBoolField(TEXT("includes_snapshot_collection"), false);
		Performance->SetBoolField(TEXT("includes_particle_attribute_serialization"), false);
		Performance->SetBoolField(TEXT("includes_world_queue_flush"), true);
		Performance->SetBoolField(TEXT("includes_gpu_completion_wait"), false);
		Performance->SetBoolField(TEXT("is_gpu_frame_time"), false);
		Performance->SetStringField(
			TEXT("interpretation"),
			TEXT("Host wall-clock cost of synchronous component advance plus Niagara world queue flush; use for CPU simulation or dispatch-cost comparisons, not as GPU frame time."));
		Performance->SetObjectField(TEXT("advance"), BuildTimingSummary(PerformanceAdvanceSeconds));
		Performance->SetObjectField(TEXT("flush"), BuildTimingSummary(PerformanceFlushSeconds));
		Performance->SetObjectField(TEXT("combined"), BuildTimingSummary(PerformanceCombinedSeconds));
		OutData->SetObjectField(TEXT("performance"), Performance);
	}
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

bool FUeAgentLegacyMechanics::CmdNiagaraGetStackIssues(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	FScopedPackageDirtyState PackageDirtyState(Asset->GetOutermost());

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
	OutData->SetBoolField(TEXT("compile_has_error"), StackCompileResult.bCompileHasError);
	OutData->SetNumberField(TEXT("compile_error_script_count"), StackCompileResult.CompileErrorScriptCount);
	if (!StackCompileResult.FirstCompileError.IsEmpty())
	{
		OutData->SetStringField(TEXT("first_compile_error"), StackCompileResult.FirstCompileError);
	}
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

bool FUeAgentLegacyMechanics::CmdNiagaraApplyStackIssueFix(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	bool bOpenEditorIfNeeded = false;
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterClearParent(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraSystemGetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraSetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	if (!UeAgentNiagaraOps::SetDeprecatedObjectPropertyText(Asset, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		TSharedPtr<FJsonObject> ResultObj = FUeAgentLegacyMechanics::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
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

bool FUeAgentLegacyMechanics::CmdNiagaraRefreshSystem(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	UeAgentNiagaraCompileValidation::FResult CompileValidation;
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
		if (bCompilationComplete)
		{
			CompileValidation = UeAgentNiagaraCompileValidation::InspectAsset(NiagaraSystem);
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
		if (CompileValidation.bHasError)
		{
			OutError = TEXT("niagara_compile_failed");
			return false;
		}
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
	OutData->SetBoolField(TEXT("compile_has_error"), CompileValidation.bHasError);
	OutData->SetNumberField(TEXT("compile_error_script_count"), CompileValidation.ErrorScriptCount);
	if (!CompileValidation.FirstError.IsEmpty())
	{
		OutData->SetStringField(TEXT("first_compile_error"), CompileValidation.FirstError);
	}
	OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
	OutData->SetBoolField(TEXT("is_ready_to_run"), NiagaraSystem->IsReadyToRun());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRefresh);
	OutData->SetBoolField(TEXT("restored_dirty_state"), bRestoredDirtyState);
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraCompileSystem(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	UeAgentNiagaraCompileValidation::FResult CompileValidation;
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
		if (bCompilationComplete)
		{
			CompileValidation = UeAgentNiagaraCompileValidation::InspectAsset(NiagaraSystem);
		}
	}
	else if (UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(Asset))
	{
		NiagaraScript->RequestCompile(NiagaraScript->GetExposedVersion().VersionGuid, bForceCompile);
		bCompilationRequested = true;
		bCompilationComplete = true;
		CompileTarget = TEXT("script");
		CompileValidation = UeAgentNiagaraCompileValidation::InspectAsset(NiagaraScript);
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
		if (CompileValidation.bHasError)
		{
			OutError = TEXT("niagara_compile_failed");
			return false;
		}
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
	OutData->SetBoolField(TEXT("compile_has_error"), CompileValidation.bHasError);
	OutData->SetNumberField(TEXT("compile_error_script_count"), CompileValidation.ErrorScriptCount);
	if (!CompileValidation.FirstError.IsEmpty())
	{
		OutData->SetStringField(TEXT("first_compile_error"), CompileValidation.FirstError);
	}
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

bool FUeAgentLegacyMechanics::CmdNiagaraGetCompileLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if ((!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
		&& (!JsonTryGetString(Ctx.Params, TEXT("emitter_asset_path"), AssetPathInput) || AssetPathInput.IsEmpty()))
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
		if (!bCompileBeforeRead)
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
	bool bWaitForCompleteEffective = false;
	FString CompileContextSource = TEXT("cached_asset_scripts");
	UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult;
	UeAgentNiagaraEmitterCompileContext::FResult EmitterCompileContext;

	if (bCompileBeforeRead)
	{
		if (NiagaraSystem)
		{
			bWaitForCompleteSupported = true;
			bWaitForCompleteEffective = bWaitForComplete;
			CompileContextSource = TEXT("system_asset");
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
			CompileContextSource = TEXT("script_asset");
			NiagaraScript->RequestCompile(NiagaraScript->GetExposedVersion().VersionGuid, bForceCompile);
			bCompilationRequested = true;
			bCompilationComplete = true;
		}
		else if (NiagaraEmitter)
		{
			bWaitForCompleteSupported = true;
			bWaitForCompleteEffective = true;
			CompileContextSource = TEXT("transient_system_for_emitter");
			if (!UeAgentNiagaraEmitterCompileContext::Compile(
				*NiagaraEmitter,
				EmitterEditContext.VersionGuid,
				bForceCompile,
				EmitterCompileContext,
				OutError))
			{
				return false;
			}

			bCompilationRequested = EmitterCompileContext.bCompilationRequested;
			bCompilationComplete = EmitterCompileContext.bCompilationComplete;
			TArray<UNiagaraScript*> CompiledEmitterScripts;
			UeAgentNiagaraEmitterCompileContext::GetCompiledScripts(EmitterCompileContext, CompiledEmitterScripts);
			TSet<const UNiagaraScript*> SeenScripts;
			const FString EmitterVersion = EmitterEditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
			for (UNiagaraScript* Script : CompiledEmitterScripts)
			{
				UeAgentNiagaraOps::AppendUniqueCollectedScript(
					CollectedScripts,
					SeenScripts,
					Script,
					TEXT("emitter_transient_system"),
					EmitterEditContext.Emitter->GetName(),
					EmitterEditContext.Emitter->GetPathName(),
					FString(),
					EmitterVersion,
					true);
			}
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
	OutData->SetBoolField(TEXT("wait_for_complete_effective"), bWaitForCompleteEffective);
	OutData->SetStringField(TEXT("compile_context"), CompileContextSource);
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



bool FUeAgentLegacyMechanics::CmdNiagaraUserParameterList(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraUserParameterAdd(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraUserParameterRemove(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraUserParameterGet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraUserParameterSet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterParameterList(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterParameterAdd(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterParameterRemove(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterParameterGet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterParameterSet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraSystemListEmitters(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraSystemAddEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraSystemRemoveEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	int32 HandleIndex = INDEX_NONE;
	FNiagaraEmitterHandle* HandleToRemove = nullptr;
	if (!UeAgentNiagaraOps::ResolveEmitterHandleFromParams(*NiagaraSystem, Ctx.Params, HandleIndex, HandleToRemove, OutError))
	{
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

bool FUeAgentLegacyMechanics::CmdNiagaraSystemMoveEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraSystemSetEmitterEnabled(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraSystemSetEmitterVersion(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterGetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterSetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	if (!UeAgentNiagaraOps::SetDeprecatedStructPropertyText(FVersionedNiagaraEmitterData::StaticStruct(), EditContext.EmitterData, EditContext.Emitter, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
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
		CompileResult = UeAgentNiagaraOps::CompileEmitterAfterSet(EditContext.Emitter, EditContext.VersionGuid, Ctx.Params, true);
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


bool FUeAgentLegacyMechanics::CmdNiagaraEmitterListRenderers(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterAddRenderer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
		if (!UeAgentNiagaraOps::SetDeprecatedObjectPropertyText(NewRenderer, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
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

		InitialPropertyResult = FUeAgentLegacyMechanics::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
		InitialPropertyResult->SetStringField(TEXT("cpp_type"), CppType);
	}

	EditContext.Emitter->MarkPackageDirty();

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterAfterSet(EditContext.Emitter, EditContext.VersionGuid, Ctx.Params, true);

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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterRemoveRenderer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterAfterSet(EditContext.Emitter, EditContext.VersionGuid, Ctx.Params, true);

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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterMoveRenderer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterAfterSet(EditContext.Emitter, EditContext.VersionGuid, Ctx.Params, true);

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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterGetRendererProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterSetRendererProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	if (!UeAgentNiagaraOps::SetDeprecatedObjectPropertyText(Renderer, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
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

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterAfterSet(EditContext.Emitter, EditContext.VersionGuid, Ctx.Params, true);

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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterListEventHandlers(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterAddEventHandler(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterAfterSet(EditContext.Emitter, EditContext.VersionGuid, Ctx.Params, true);

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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterRemoveEventHandler(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterAfterSet(EditContext.Emitter, EditContext.VersionGuid, Ctx.Params, true);

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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterGetEventHandlerProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterSetEventHandlerProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	if (!UeAgentNiagaraOps::SetDeprecatedStructPropertyText(FNiagaraEventScriptProperties::StaticStruct(), EventHandler, EditContext.Emitter, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
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

	const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult CompileResult = UeAgentNiagaraOps::CompileEmitterAfterSet(EditContext.Emitter, EditContext.VersionGuid, Ctx.Params, true);

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


bool FUeAgentLegacyMechanics::CmdNiagaraEmitterListStages(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FUeAgentStructuredResourceTracker ResourceTracker;
	if (!ResourceTracker.ChargeItems(
			OutputNodes.Num(),
			TEXT("/stages"),
			OutError))
	{
		return false;
	}
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
			const FString ModulesPointer = FString::Printf(
				TEXT("/stages/%d/modules"),
				StagesJson.Num());
			if (!ResourceTracker.ChargeItems(
					ModuleNodes.Num(),
					ModulesPointer,
					OutError))
			{
				return false;
			}
			TArray<TSharedPtr<FJsonValue>> ModulesJson;
			ModulesJson.Reserve(ModuleNodes.Num());
			for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
			{
				UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
				if (!ModuleNode)
				{
					continue;
				}
				TSharedPtr<FJsonObject> ModuleObj;
				if (!UeAgentNiagaraOps::BuildModuleNodeJson(
						*ModuleNode,
						ModuleIndex,
						bIncludeModuleInputs,
						ResourceTracker,
						FString::Printf(
							TEXT("%s/%d"),
							*ModulesPointer,
							ModuleIndex),
						ModuleObj,
						OutError))
				{
					return false;
				}
				ModulesJson.Add(MakeShared<FJsonValueObject>(ModuleObj));
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterAddSimulationStage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterRemoveStage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterSetStageProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	if (!UeAgentNiagaraOps::SetDeprecatedObjectPropertyText(TargetObject, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterListStageModules(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FUeAgentStructuredResourceTracker ResourceTracker;
	if (!ResourceTracker.ChargeItems(
			ModuleNodes.Num(),
			TEXT("/modules"),
			OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> ModulesJson;
	ModulesJson.Reserve(ModuleNodes.Num());
	for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
	{
		UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
		if (!ModuleNode)
		{
			continue;
		}
		TSharedPtr<FJsonObject> ModuleObj;
		if (!UeAgentNiagaraOps::BuildModuleNodeJson(
				*ModuleNode,
				ModuleIndex,
				bIncludeInputs,
				ResourceTracker,
				FString::Printf(TEXT("/modules/%d"), ModuleIndex),
				ModuleObj,
				OutError))
		{
			return false;
		}
		ModulesJson.Add(MakeShared<FJsonValueObject>(ModuleObj));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("modules"), ModulesJson);
	OutData->SetNumberField(TEXT("module_count"), ModulesJson.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraSystemListStageModules(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FUeAgentStructuredResourceTracker ResourceTracker;
	if (!ResourceTracker.ChargeItems(
			ModuleNodes.Num(),
			TEXT("/modules"),
			OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> ModulesJson;
	ModulesJson.Reserve(ModuleNodes.Num());
	for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
	{
		UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
		if (!ModuleNode)
		{
			continue;
		}
		TSharedPtr<FJsonObject> ModuleObj;
		if (!UeAgentNiagaraOps::BuildModuleNodeJson(
				*ModuleNode,
				ModuleIndex,
				bIncludeInputs,
				ResourceTracker,
				FString::Printf(TEXT("/modules/%d"), ModuleIndex),
				ModuleObj,
				OutError))
		{
			return false;
		}
		ModulesJson.Add(MakeShared<FJsonValueObject>(ModuleObj));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.System->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("modules"), ModulesJson);
	OutData->SetNumberField(TEXT("module_count"), ModulesJson.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterAddStageModule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	int32 TargetIndex = INDEX_NONE;
	double TargetIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		TargetIndex = FMath::RoundToInt(TargetIndexNumber);
	}

	FString SuggestedName;
	JsonTryGetString(Ctx.Params, TEXT("module_name"), SuggestedName);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	UeAgentNiagaraAuthoring::FStackApplyRequest ApplyRequest;
	ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Create;
	ApplyRequest.TargetKind = UeAgentNiagaraAuthoring::EStackTargetKind::Module;
	ApplyRequest.Stage.AssetPath = EditContext.Emitter->GetOutermost()->GetName();
	ApplyRequest.Stage.Usage = Stage.Usage;
	ApplyRequest.Stage.UsageId = Stage.UsageId;
	ApplyRequest.Stage.bUsageExplicit = true;
	ApplyRequest.Stage.bUsageIdExplicit = true;
	ApplyRequest.ScriptReference = Ctx.Params;
	ApplyRequest.TargetIndex = TargetIndex;
	ApplyRequest.SuggestedName = SuggestedName;
	ApplyRequest.bSave = bSaveAfterSet;
	UeAgentNiagaraAuthoring::FStackApplyResult ApplyResult;
	if (!UeAgentNiagaraAuthoring::ApplyStack(ApplyRequest, ApplyResult, Ctx.RequestId))
	{
		OutError = ApplyResult.ErrorCode.IsEmpty() ? ApplyResult.ErrorDetail : ApplyResult.ErrorCode;
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), ApplyResult.StageKey);
	OutData->SetStringField(TEXT("module_node_guid"), ApplyResult.TargetGuid);
	OutData->SetNumberField(TEXT("module_index"), ApplyResult.ModuleIndex);
	OutData->SetStringField(TEXT("apply_backend"), TEXT("unified_operation_apply"));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterRemoveStageModule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = nullptr;
	int32 ModuleIndex = INDEX_NONE;
	FString StageKey;
	ENiagaraScriptUsage StageUsage = ENiagaraScriptUsage::Function;
	FGuid StageUsageId;

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
		UNiagaraNodeOutput* OwningOutputNode = nullptr;
		if (!UeAgentNiagaraOps::FindOwningOutputNodeForModule(*EditContext.Graph, *ModuleNode, OwningOutputNode) || !OwningOutputNode)
		{
			OutError = TEXT("module_stage_not_found");
			return false;
		}
		StageUsage = OwningOutputNode->GetUsage();
		StageUsageId = OwningOutputNode->GetUsageId();
		StageKey = UeAgentNiagaraOps::MakeStageKey(StageUsage, StageUsageId);
	}
	else
	{
		UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
		if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
		{
			return false;
		}
		StageKey = Stage.StageKey;
		StageUsage = Stage.Usage;
		StageUsageId = Stage.UsageId;

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

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	UeAgentNiagaraAuthoring::FStackApplyRequest ApplyRequest;
	ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Remove;
	ApplyRequest.TargetKind = UeAgentNiagaraAuthoring::EStackTargetKind::Module;
	ApplyRequest.Stage.AssetPath = EditContext.Emitter->GetOutermost()->GetName();
	ApplyRequest.Stage.Usage = StageUsage;
	ApplyRequest.Stage.UsageId = StageUsageId;
	ApplyRequest.Stage.bUsageExplicit = true;
	ApplyRequest.Stage.bUsageIdExplicit = true;
	ApplyRequest.Module.NodeGuid = ModuleNode->NodeGuid;
	ApplyRequest.bSave = bSaveAfterSet;
	UeAgentNiagaraAuthoring::FStackApplyResult ApplyResult;
	if (!UeAgentNiagaraAuthoring::ApplyStack(ApplyRequest, ApplyResult, Ctx.RequestId))
	{
		OutError = ApplyResult.ErrorCode.IsEmpty() ? ApplyResult.ErrorDetail : ApplyResult.ErrorCode;
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), StageKey);
	OutData->SetStringField(TEXT("removed_module_node_guid"), RemovedModuleGuid);
	OutData->SetStringField(TEXT("removed_module_name"), RemovedModuleName);
	OutData->SetNumberField(TEXT("removed_module_index"), ModuleIndex);
	OutData->SetBoolField(TEXT("soft_removed"), false);
	OutData->SetStringField(TEXT("apply_backend"), TEXT("unified_operation_apply"));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterMoveStageModule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	const FString ModuleGuid = ModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString ModuleName = ModuleNode->GetFunctionName();
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	UeAgentNiagaraAuthoring::FStackApplyRequest ApplyRequest;
	ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Update;
	ApplyRequest.TargetKind = UeAgentNiagaraAuthoring::EStackTargetKind::Module;
	ApplyRequest.Stage.AssetPath = EditContext.Emitter->GetOutermost()->GetName();
	ApplyRequest.Stage.Usage = Stage.Usage;
	ApplyRequest.Stage.UsageId = Stage.UsageId;
	ApplyRequest.Stage.bUsageExplicit = true;
	ApplyRequest.Stage.bUsageIdExplicit = true;
	ApplyRequest.Module.NodeGuid = ModuleNode->NodeGuid;
	ApplyRequest.TargetIndex = TargetIndex;
	ApplyRequest.bSave = bSaveAfterSet;
	UeAgentNiagaraAuthoring::FStackApplyResult ApplyResult;
	if (!UeAgentNiagaraAuthoring::ApplyStack(ApplyRequest, ApplyResult, Ctx.RequestId))
	{
		OutError = ApplyResult.ErrorCode.IsEmpty() ? ApplyResult.ErrorDetail : ApplyResult.ErrorCode;
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), ApplyResult.StageKey);
	OutData->SetStringField(TEXT("module_node_guid"), ModuleGuid);
	OutData->SetStringField(TEXT("module_name"), ModuleName);
	OutData->SetNumberField(TEXT("old_index"), ModuleIndex);
	OutData->SetNumberField(TEXT("target_index"), TargetIndex);
	OutData->SetNumberField(TEXT("new_index"), ApplyResult.ModuleIndex);
	OutData->SetStringField(TEXT("apply_backend"), TEXT("unified_operation_apply"));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	OutData->SetBoolField(TEXT("moved"), ModuleIndex != ApplyResult.ModuleIndex);
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterListStageNodes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	UeAgentNiagaraOps::GatherUpstreamNodesIterative(Stage.OutputNode, VisitedNodes, StageNodes);
	StageNodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
	{
		if (A.NodePosX != B.NodePosX)
		{
			return A.NodePosX < B.NodePosX;
		}
		return A.NodePosY < B.NodePosY;
	});

	FUeAgentStructuredResourceTracker ResourceTracker;
	if (!ResourceTracker.ChargeItems(
			StageNodes.Num(),
			TEXT("/nodes"),
			OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> NodesJson;
	NodesJson.Reserve(StageNodes.Num());
	for (int32 NodeIndex = 0; NodeIndex < StageNodes.Num(); ++NodeIndex)
	{
		UEdGraphNode* StageNode = StageNodes[NodeIndex];
		if (!StageNode)
		{
			continue;
		}
		TSharedPtr<FJsonObject> NodeObj;
		if (!UeAgentNiagaraOps::BuildStageNodeJson(
				*StageNode,
				NodeIndex,
				bIncludeProperties,
				bIncludeModuleInputs,
				ResourceTracker,
				FString::Printf(TEXT("/nodes/%d"), NodeIndex),
				NodeObj,
				OutError))
		{
			return false;
		}
		NodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("nodes"), NodesJson);
	OutData->SetNumberField(TEXT("node_count"), NodesJson.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterGetStageNodeProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterSetStageNodeProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	if (!UeAgentNiagaraOps::SetDeprecatedObjectPropertyText(StageNode, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterRemoveStageNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterSetStageModuleEnabled(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterListModuleInputs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	TArray<UeAgentNiagaraStageModuleAuthoring::FResolvedModuleInput>
		ModuleInputs;
	UeAgentNiagaraStageModuleAuthoring::BuildResolvedModuleInputs(
		*ModuleNode,
		ModuleInputs);
	FUeAgentStructuredResourceTracker ResourceTracker;
	TArray<TSharedPtr<FJsonValue>> InputsJson;
	if (!UeAgentNiagaraOps::BuildModuleInputsJson(
			*ModuleNode,
			ModuleInputs,
			ResourceTracker,
			TEXT("/inputs"),
			InputsJson,
			OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetArrayField(TEXT("inputs"), InputsJson);
	OutData->SetNumberField(TEXT("input_count"), InputsJson.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraSystemListModuleInputs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraSystemEditContext EditContext;
	if (!UeAgentNiagaraOps::GetSystemEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UNiagaraGraph* TargetGraph = EditContext.Graph;
	UeAgentNiagaraStageModuleAuthoring::FResolvedEmitterOwner ResolvedOwner;
	FString EmitterHandleIdText;
	FString EmitterName;
	const bool bHasEmitterHandleId = JsonTryGetString(Ctx.Params, TEXT("emitter_handle_id"), EmitterHandleIdText)
		&& !EmitterHandleIdText.IsEmpty();
	const bool bHasEmitterName = JsonTryGetString(Ctx.Params, TEXT("emitter_name"), EmitterName)
		&& !EmitterName.IsEmpty();
	if (bHasEmitterHandleId || bHasEmitterName)
	{
		UeAgentNiagaraStageModuleAuthoring::FEmitterOwnerTarget OwnerTarget;
		OwnerTarget.AssetPath = EditContext.System->GetOutermost()->GetName();
		OwnerTarget.EmitterName = EmitterName;
		if (bHasEmitterHandleId)
		{
			if (!FGuid::Parse(EmitterHandleIdText, OwnerTarget.EmitterHandleId))
			{
				OutError = TEXT("invalid_emitter_handle_id");
				return false;
			}
			OwnerTarget.bEmitterHandleIdExplicit = true;
		}
		if (!UeAgentNiagaraStageModuleAuthoring::ResolveEmitterOwner(
			OwnerTarget,
			ResolvedOwner,
			OutError,
			EditContext.System))
		{
			return false;
		}
		TargetGraph = ResolvedOwner.Graph;
		if (!TargetGraph)
		{
			OutError = TEXT("missing_emitter_graph_source");
			return false;
		}
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

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(TargetGraph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraStageModuleAuthoring::FResolvedModuleInput>
		ModuleInputs;
	UeAgentNiagaraStageModuleAuthoring::BuildResolvedModuleInputs(
		*ModuleNode,
		ModuleInputs);
	FUeAgentStructuredResourceTracker ResourceTracker;
	TArray<TSharedPtr<FJsonValue>> InputsJson;
	if (!UeAgentNiagaraOps::BuildModuleInputsJson(
			*ModuleNode,
			ModuleInputs,
			ResourceTracker,
			TEXT("/inputs"),
			InputsJson,
			OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.System->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetArrayField(TEXT("inputs"), InputsJson);
	OutData->SetNumberField(TEXT("input_count"), InputsJson.Num());
	if (ResolvedOwner.EmitterHandleId.IsValid())
	{
		OutData->SetStringField(TEXT("emitter_handle_id"), ResolvedOwner.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("emitter_name"), ResolvedOwner.EmitterName);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterSetModuleInput(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	const bool bHasDynamicInputRequest =
		Ctx.Params->HasTypedField<EJson::String>(TEXT("dynamic_input_script_asset_path"))
		|| Ctx.Params->HasTypedField<EJson::String>(TEXT("dynamic_input_script_path"))
		|| Ctx.Params->HasTypedField<EJson::String>(TEXT("dynamic_input"))
		|| Ctx.Params->HasTypedField<EJson::String>(TEXT("catalog_name"))
		|| Ctx.Params->HasTypedField<EJson::String>(TEXT("name"));
	UeAgentNiagaraAuthoring::FScriptResolveResult DynamicInputResolveResult;
	if (bHasDynamicInputRequest)
	{
		UeAgentNiagaraAuthoring::FScriptResolveRequest ResolveRequest;
		ResolveRequest.Kind = UeAgentNiagaraAuthoring::EScriptKind::DynamicInput;
		ResolveRequest.Input = Ctx.Params;
		if (!UeAgentNiagaraAuthoring::ResolveScript(ResolveRequest, DynamicInputResolveResult, OutError))
		{
			return false;
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
		OutError = UeAgentNiagaraOps::BuildInputNameNotFoundError(*ModuleNode, ModuleInputs, InputName);
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

	if (DynamicInputResolveResult.IsResolved())
	{
		if (OverridePin.LinkedTo.Num() > 0 && !bBreakInputLinks)
		{
			return CancelAndFail(TEXT("input_has_linked_override_set_break_input_links_true"));
		}

		UNiagaraScript* DynamicInputScript = DynamicInputResolveResult.Script;

		FString DynamicInputSuggestedName;
		if (!JsonTryGetString(Ctx.Params, TEXT("dynamic_input_name"), DynamicInputSuggestedName))
		{
			JsonTryGetString(Ctx.Params, TEXT("suggested_dynamic_input_name"), DynamicInputSuggestedName);
		}

		UNiagaraNodeFunctionCall* DynamicInputNode = nullptr;
		FString DynamicInputApplyError;
		if (!UeAgentNiagaraStageModuleAuthoring::SetDynamicInputForPin(
			OverridePin,
			*DynamicInputScript,
			DynamicInputSuggestedName,
			DynamicInputNode,
			DynamicInputApplyError))
		{
			return CancelAndFail(DynamicInputApplyError);
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
		OutData->SetStringField(TEXT("requested_dynamic_input_script_asset_path"), DynamicInputResolveResult.ResolvedObjectPath);
		OutData->SetStringField(TEXT("dynamic_input_node_guid"), DynamicInputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("dynamic_input_node_name"), DynamicInputNode->GetFunctionName());
		OutData->SetStringField(TEXT("dynamic_input_script_asset_path"), UeAgentNiagaraOps::GetModuleScriptPath(*DynamicInputNode));
		OutData->SetBoolField(TEXT("is_dynamic_input"), true);
		OutData->SetBoolField(TEXT("dynamic_input_extension_deprecated"), true);
		OutData->SetStringField(TEXT("dynamic_input_extension_replacement"), TEXT("Use niagara_export_folder / niagara_apply_folder for stage, module, and dynamic-input script authoring. Data Interface properties such as curve_json are not covered by the unified folder contract; use the dedicated typed command until a Niagara data-interface semantic is available."));
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

bool FUeAgentLegacyMechanics::CmdNiagaraSystemSetModuleInput(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
		OutError = UeAgentNiagaraOps::BuildInputNameNotFoundError(*ModuleNode, ModuleInputs, InputName);
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterClearModuleInput(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
		OutError = UeAgentNiagaraOps::BuildInputNameNotFoundError(*ModuleNode, ModuleInputs, InputName);
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterAddStageNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
		UeAgentNiagaraAuthoring::FScriptResolveRequest ResolveRequest;
		ResolveRequest.Kind = UeAgentNiagaraAuthoring::EScriptKind::Module;
		ResolveRequest.Input = Ctx.Params;
		UeAgentNiagaraAuthoring::FScriptResolveResult ResolveResult;
		if (!UeAgentNiagaraAuthoring::ResolveScript(ResolveRequest, ResolveResult, OutError))
		{
			return false;
		}
		UNiagaraScript* ModuleScript = ResolveResult.Script;

		int32 TargetIndex = INDEX_NONE;
		double TargetIndexNumber = 0.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
		{
			TargetIndex = FMath::RoundToInt(TargetIndexNumber);
		}

		FString SuggestedName;
		JsonTryGetString(Ctx.Params, TEXT("module_name"), SuggestedName);

		FString AddModuleError;
		UNiagaraNodeFunctionCall* AddedModule = UeAgentNiagaraStageModuleAuthoring::AddModule(
			*Stage.OutputNode,
			*ModuleScript,
			SuggestedName,
			TargetIndex,
			AddModuleError);
		if (!AddedModule)
		{
			OutError = AddModuleError.IsEmpty() ? TEXT("add_stage_node_failed") : AddModuleError;
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

	FUeAgentStructuredResourceTracker ResourceTracker;
	TSharedPtr<FJsonObject> NodeObj;
	if (!UeAgentNiagaraOps::BuildStageNodeJson(
			*AddedNode,
			INDEX_NONE,
			true,
			true,
			ResourceTracker,
			TEXT("/node"),
			NodeObj,
			OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetObjectField(TEXT("node"), NodeObj);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterConnectStageNodes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterDisconnectStageNodes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
