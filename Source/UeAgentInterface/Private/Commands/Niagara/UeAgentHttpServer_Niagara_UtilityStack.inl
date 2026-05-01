
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
		auto IsUserSettableEnumValue = [&EnumType](int64 EnumValue) -> bool
		{
			if (!EnumType.IsValidEnumValue(EnumValue))
			{
				return false;
			}

			const int32 EnumIndex = EnumType.GetIndexByValue(EnumValue);
			if (EnumIndex == INDEX_NONE)
			{
				return false;
			}
			if (EnumType.HasMetaData(TEXT("Hidden"), EnumIndex) ||
				EnumType.HasMetaData(TEXT("HiddenByDefault"), EnumIndex) ||
				EnumType.HasMetaData(TEXT("Spacer"), EnumIndex))
			{
				return false;
			}

			const FString EnumName = EnumType.GetNameStringByValue(EnumValue);
			const FString EnumNameUpper = EnumName.ToUpper();
			if (EnumNameUpper.Equals(TEXT("MAX")) ||
				EnumNameUpper.EndsWith(TEXT("_MAX")) ||
				EnumNameUpper.Contains(TEXT("INVALID")))
			{
				return false;
			}

			const FString FullEnumName = EnumType.GetNameByValue(EnumValue).ToString().ToUpper();
			if (FullEnumName.EndsWith(TEXT("_MAX")))
			{
				return false;
			}
			return true;
		};

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
			if (IsUserSettableEnumValue(ParsedIntValue))
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
		if (!IsUserSettableEnumValue(ParsedEnumValue))
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
