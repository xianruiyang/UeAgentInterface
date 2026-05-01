namespace UeAgentAIOps
{
	static constexpr const TCHAR* BlackboardProfile = TEXT("ue_agent_interface.blackboard.v1");
	static constexpr const TCHAR* BehaviorTreeProfile = TEXT("ue_agent_interface.behavior_tree.v1");
	static constexpr const TCHAR* StateTreeProfile = TEXT("ue_agent_interface.state_tree.v1");
	static constexpr const TCHAR* EqsProfile = TEXT("ue_agent_interface.eqs.v1");
	static constexpr const TCHAR* SmartObjectDefinitionProfile = TEXT("ue_agent_interface.smart_object_definition.v1");
	static constexpr const TCHAR* AIStackProfile = TEXT("ue_agent_interface.ai_behavior_stack.v1");

	static TMap<FString, FSmartObjectRequestResult> GSmartObjectFindResults;
	static TMap<FString, FSmartObjectClaimHandle> GSmartObjectClaims;

	static bool ResolveGenericAssetEditorWindow(UObject* Asset, const bool bOpenIfNeeded, TSharedPtr<SWindow>& OutWindow, FString& OutError);
	static bool ScreenshotSlateWindow(const TSharedPtr<SWindow>& Window, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);
	static bool IsSlateWindowBackbufferCaptureUnsafe(const TSharedPtr<SWindow>& Window, FString& OutUnsafeReason);
	static int32 CountNonBlackPixels(const TArray<FColor>& Pixels);
	static void AddPixelStats(const TArray<FColor>& Pixels, TSharedPtr<FJsonObject>& OutData);
	static bool SaveBehaviorTreeFolderContents(const FString& FolderPath, UBehaviorTree* Tree, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError);
	static bool SaveStateTreeFolderContents(const FString& FolderPath, UStateTree* Tree, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError);
	static bool SaveEqsFolderContents(const FString& FolderPath, UEnvQuery* Query, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError);

	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();
		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	template<typename T>
	static T* LoadAsset(const FString& InPath)
	{
		return Cast<T>(LoadAssetObject(InPath));
	}

	static bool AssetExists(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return false;
		}
		if (FindObject<UObject>(nullptr, *ObjectPath))
		{
			return true;
		}
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	template<typename T>
	static bool CreateAsset(const FString& AssetPathInput, T*& OutAsset, FString& OutError)
	{
		OutAsset = nullptr;
		const FString PackageName = NormalizeAssetPath(AssetPathInput);
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
		if (AssetExists(PackageName))
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

		T* Asset = NewObject<T>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset)
		{
			OutError = TEXT("create_asset_failed");
			return false;
		}

		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		Package->MarkPackageDirty();
		OutAsset = Asset;
		return true;
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

	static UClass* ResolveClass(const FString& ClassPathInput)
	{
		FString ClassPath = ClassPathInput;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* ExistingClass = FindObject<UClass>(nullptr, *ClassPath))
		{
			return ExistingClass;
		}
		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return LoadedClass;
		}
		if (!ClassPath.StartsWith(TEXT("/Script/")) && !ClassPath.Contains(TEXT(".")))
		{
			return FindFirstObject<UClass>(*ClassPath, EFindFirstObjectOptions::None, ELogVerbosity::NoLogging, TEXT("UeAgentAIOps::ResolveClass"));
		}
		return nullptr;
	}

	static UScriptStruct* ResolveScriptStruct(const FString& StructPathInput)
	{
		FString StructPath = StructPathInput;
		StructPath.TrimStartAndEndInline();
		if (StructPath.IsEmpty())
		{
			return nullptr;
		}
		if (UScriptStruct* ExistingStruct = FindObject<UScriptStruct>(nullptr, *StructPath))
		{
			return ExistingStruct;
		}
		if (UScriptStruct* LoadedStruct = LoadObject<UScriptStruct>(nullptr, *StructPath))
		{
			return LoadedStruct;
		}
		if (!StructPath.StartsWith(TEXT("/Script/")) && !StructPath.Contains(TEXT(".")))
		{
			return FindFirstObject<UScriptStruct>(*StructPath, EFindFirstObjectOptions::None, ELogVerbosity::NoLogging, TEXT("UeAgentAIOps::ResolveScriptStruct"));
		}
		return nullptr;
	}

	static FString ClassPath(const UObject* Object)
	{
		return Object ? Object->GetClass()->GetPathName() : FString();
	}

	static FString StructPath(const FInstancedStruct& InstancedStruct)
	{
		return InstancedStruct.GetScriptStruct() ? InstancedStruct.GetScriptStruct()->GetPathName() : FString();
	}

	static TSharedPtr<FJsonObject> MakeIssue(const FString& Code, const FString& Message, const FString& JsonPath = FString())
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("message"), Message);
		if (!JsonPath.IsEmpty())
		{
			Issue->SetStringField(TEXT("path"), JsonPath);
		}
		return Issue;
	}

	static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Code, const FString& Message, const FString& JsonPath = FString())
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Code, Message, JsonPath)));
	}

	static TSharedPtr<FJsonObject> MakeCoverageReport();

	static void AttachCoverageReport(TSharedPtr<FJsonObject>& OutData)
	{
		if (OutData.IsValid())
		{
			OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport());
		}
	}

	static void SetDiagnostics(TSharedPtr<FJsonObject>& OutData, const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("error_count"), Issues.Num());
		OutData->SetArrayField(TEXT("errors"), Issues);
		OutData->SetNumberField(TEXT("warning_count"), 0);
		OutData->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
		AttachCoverageReport(OutData);
	}

	static bool SaveJsonFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
	{
		OutOutputPath.Reset();
		if (!Root.IsValid())
		{
			OutError = TEXT("json_root_invalid");
			return false;
		}
		FString OutputPath = OutputPathInput;
		OutputPath.TrimStartAndEndInline();
		if (OutputPath.IsEmpty())
		{
			OutError = TEXT("missing_output_file");
			return false;
		}

		OutputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputPath);
		const FString OutputDir = FPaths::GetPath(OutputPath);
		if (!OutputDir.IsEmpty() && !IFileManager::Get().DirectoryExists(*OutputDir))
		{
			IFileManager::Get().MakeDirectory(*OutputDir, true);
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
		{
			OutError = TEXT("json_serialize_failed");
			return false;
		}
		if (!FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("save_json_file_failed");
			return false;
		}

		OutOutputPath = OutputPath;
		return true;
	}

	static bool LoadJsonFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError)
	{
		OutRoot.Reset();
		OutResolvedPath.Reset();
		FString InputPath = InputPathInput;
		InputPath.TrimStartAndEndInline();
		if (InputPath.IsEmpty())
		{
			OutError = TEXT("missing_json_file");
			return false;
		}
		InputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(InputPath);
		if (!FPaths::FileExists(InputPath))
		{
			OutError = TEXT("json_file_not_found");
			return false;
		}
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *InputPath))
		{
			OutError = TEXT("load_json_file_failed");
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}
		OutResolvedPath = InputPath;
		return true;
	}

	static FString ResolveFolderPath(const FString& FolderPathInput, const FString& Profile, const FString& AssetPath)
	{
		FString FolderPath = FolderPathInput;
		FolderPath.TrimStartAndEndInline();
		if (FolderPath.IsEmpty())
		{
			FString RelativeAsset = NormalizeAssetPath(AssetPath);
			while (RelativeAsset.StartsWith(TEXT("/")))
			{
				RelativeAsset.RightChopInline(1, EAllowShrinking::No);
			}
			FolderPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), Profile, RelativeAsset);
		}
		else
		{
			FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
		}
		return FPaths::ConvertRelativePathToFull(FolderPath);
	}

	static bool SaveFolderJson(const FString& FolderPath, const FString& FileName, const TSharedPtr<FJsonObject>& Root, FString& OutError)
	{
		FString SavedPath;
		return SaveJsonFile(FPaths::Combine(FolderPath, FileName), Root, SavedPath, OutError);
	}

	static bool SaveTrackedFolderJson(
		const FString& FolderPath,
		const FString& FileName,
		const TSharedPtr<FJsonObject>& Root,
		TArray<TSharedPtr<FJsonValue>>& WrittenFiles,
		FString& OutError)
	{
		if (!SaveFolderJson(FolderPath, FileName, Root, OutError))
		{
			return false;
		}
		WrittenFiles.Add(MakeShared<FJsonValueString>(FileName.Replace(TEXT("\\"), TEXT("/"))));
		return true;
	}

	static FString AssetFolderNameFromPath(const FString& AssetPath)
	{
		const FString Normalized = NormalizeAssetPath(AssetPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(Normalized);
		return AssetName.IsEmpty() ? FPaths::GetBaseFilename(Normalized) : AssetName;
	}

	static TSharedPtr<FJsonObject> MakeCoverageReport()
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetArrayField(TEXT("pending_profiles"), TArray<TSharedPtr<FJsonValue>>());
		Coverage->SetArrayField(TEXT("blocking_gaps"), TArray<TSharedPtr<FJsonValue>>());
		return Coverage;
	}

	static TSharedPtr<FJsonObject> MakeValidationChecks(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TSharedPtr<FJsonObject> Checks = MakeShared<FJsonObject>();
		Checks->SetNumberField(TEXT("issue_count"), Issues.Num());
		Checks->SetArrayField(TEXT("issues"), Issues);
		return Checks;
	}

	static TSharedPtr<FJsonObject> MakeProfileArrayJson(const FString& Profile, const FString& FieldName, const TArray<TSharedPtr<FJsonValue>>& Items)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("profile"), Profile);
		Obj->SetNumberField(TEXT("schema_version"), 1);
		Obj->SetArrayField(FieldName, Items);
		return Obj;
	}

	static bool LoadFolderJson(const FString& FolderPath, const FString& FileName, TSharedPtr<FJsonObject>& OutRoot, FString& OutError, const bool bRequired = true)
	{
		FString ResolvedPath;
		const FString Path = FPaths::Combine(FolderPath, FileName);
		if (!FPaths::FileExists(Path))
		{
			if (bRequired)
			{
				OutError = FString::Printf(TEXT("json_file_not_found:%s"), *FileName);
				return false;
			}
			OutRoot.Reset();
			return true;
		}
		if (!LoadJsonFile(Path, OutRoot, ResolvedPath, OutError))
		{
			OutError = FString::Printf(TEXT("%s:%s"), *OutError, *FileName);
			return false;
		}
		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Item : Items)
		{
			Out.Add(MakeShared<FJsonValueString>(Item));
		}
		return Out;
	}

	static TArray<TSharedPtr<FJsonValue>> GameplayTagsToJson(const FGameplayTagContainer& Tags)
	{
		TArray<FString> TagStrings;
		for (const FGameplayTag& Tag : Tags)
		{
			if (Tag.IsValid())
			{
				TagStrings.Add(Tag.ToString());
			}
		}
		return StringArrayToJson(TagStrings);
	}

	static FGameplayTagContainer GameplayTagsFromJsonArray(const TArray<TSharedPtr<FJsonValue>>* Values)
	{
		FGameplayTagContainer Tags;
		if (!Values)
		{
			return Tags;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString TagText;
			if (Value.IsValid() && Value->TryGetString(TagText))
			{
				const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagText), false);
				if (Tag.IsValid())
				{
					Tags.AddTag(Tag);
				}
			}
		}
		return Tags;
	}

	static TSharedPtr<FJsonObject> VecToJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> RotToJson(const FRotator& R)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), R.Pitch);
		Obj->SetNumberField(TEXT("yaw"), R.Yaw);
		Obj->SetNumberField(TEXT("roll"), R.Roll);
		return Obj;
	}

	static bool JsonTryGetVector(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
	{
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(Key, ValueObj) || !ValueObj || !ValueObj->IsValid())
		{
			return false;
		}
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		(*ValueObj)->TryGetNumberField(TEXT("x"), X);
		(*ValueObj)->TryGetNumberField(TEXT("y"), Y);
		(*ValueObj)->TryGetNumberField(TEXT("z"), Z);
		OutValue = FVector(X, Y, Z);
		return true;
	}

	static bool JsonTryGetRotator(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue)
	{
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(Key, ValueObj) || !ValueObj || !ValueObj->IsValid())
		{
			return false;
		}
		double Pitch = 0.0;
		double Yaw = 0.0;
		double Roll = 0.0;
		(*ValueObj)->TryGetNumberField(TEXT("pitch"), Pitch);
		(*ValueObj)->TryGetNumberField(TEXT("yaw"), Yaw);
		(*ValueObj)->TryGetNumberField(TEXT("roll"), Roll);
		OutValue = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	static TSharedPtr<FJsonObject> TransformToJson(const FTransform& Transform)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("translation"), VecToJson(Transform.GetLocation()));
		Obj->SetObjectField(TEXT("rotation"), RotToJson(Transform.Rotator()));
		Obj->SetObjectField(TEXT("scale"), VecToJson(Transform.GetScale3D()));
		return Obj;
	}

	static bool TryGetTransform(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FTransform& OutTransform)
	{
		const TSharedPtr<FJsonObject>* TransformObjPtr = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(Key, TransformObjPtr) || !TransformObjPtr || !TransformObjPtr->IsValid())
		{
			return false;
		}
		FVector Translation = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
		JsonTryGetVector(*TransformObjPtr, TEXT("translation"), Translation);
		JsonTryGetRotator(*TransformObjPtr, TEXT("rotation"), Rotation);
		JsonTryGetVector(*TransformObjPtr, TEXT("scale"), Scale);
		OutTransform = FTransform(Rotation, Translation, Scale);
		return true;
	}

	static TSharedPtr<FJsonObject> UObjectSummary(UObject* Object)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (Object)
		{
			Obj->SetStringField(TEXT("name"), Object->GetName());
			Obj->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
			Obj->SetStringField(TEXT("path"), Object->GetPathName());
		}
		return Obj;
	}

	static bool ResolvePropertyPath(UStruct* Struct, void* Container, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Struct || !Container || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Parts;
		PropertyPath.ParseIntoArray(Parts, TEXT("."), true);
		UStruct* CurrentStruct = Struct;
		void* CurrentPtr = Container;
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			FProperty* Property = FindFProperty<FProperty>(CurrentStruct, *Parts[Index]);
			if (!Property)
			{
				return false;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentPtr);
			if (Index == Parts.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}
			if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				CurrentStruct = StructProperty->Struct;
				CurrentPtr = ValuePtr;
			}
			else if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				UObject* NestedObject = ObjectProperty->GetObjectPropertyValue(ValuePtr);
				if (!NestedObject)
				{
					return false;
				}
				CurrentStruct = NestedObject->GetClass();
				CurrentPtr = NestedObject;
			}
			else
			{
				return false;
			}
		}
		return false;
	}

	static bool ImportPropertyValue(UStruct* Struct, void* Container, UObject* Owner, const FString& PropertyPath, const FString& ValueText, FString& OutError)
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Struct, Container, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			OutError = FString::Printf(TEXT("property_not_found:%s"), *PropertyPath);
			return false;
		}
		if (!Property->ImportText_Direct(*ValueText, ValuePtr, Owner, PPF_None))
		{
			OutError = FString::Printf(TEXT("property_import_failed:%s"), *PropertyPath);
			return false;
		}
		return true;
	}

	static bool ReadReflectedBool(UStruct* Struct, const void* Container, const FName PropertyName, bool& OutValue)
	{
		const FBoolProperty* Property = Struct ? FindFProperty<FBoolProperty>(Struct, PropertyName) : nullptr;
		if (!Property || !Container)
		{
			return false;
		}
		OutValue = Property->GetPropertyValue_InContainer(Container);
		return true;
	}

	static bool ReadReflectedFloat(UStruct* Struct, const void* Container, const FName PropertyName, float& OutValue)
	{
		const FFloatProperty* FloatProperty = Struct ? FindFProperty<FFloatProperty>(Struct, PropertyName) : nullptr;
		if (FloatProperty && Container)
		{
			OutValue = FloatProperty->GetPropertyValue_InContainer(Container);
			return true;
		}

		const FDoubleProperty* DoubleProperty = Struct ? FindFProperty<FDoubleProperty>(Struct, PropertyName) : nullptr;
		if (DoubleProperty && Container)
		{
			OutValue = static_cast<float>(DoubleProperty->GetPropertyValue_InContainer(Container));
			return true;
		}
		return false;
	}

	static bool ReadReflectedClassArray(UStruct* Struct, const void* Container, const FName PropertyName, TArray<UClass*>& OutClasses)
	{
		OutClasses.Reset();
		const FArrayProperty* ArrayProperty = Struct ? FindFProperty<FArrayProperty>(Struct, PropertyName) : nullptr;
		if (!ArrayProperty || !Container)
		{
			return false;
		}

		const void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(Container);
		FScriptArrayHelper Helper(ArrayProperty, ArrayPtr);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			if (const FClassProperty* ClassProperty = CastField<FClassProperty>(ArrayProperty->Inner))
			{
				OutClasses.Add(Cast<UClass>(ClassProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index))));
			}
			else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(ArrayProperty->Inner))
			{
				OutClasses.Add(Cast<UClass>(ObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index))));
			}
		}
		return true;
	}

	static bool ApplyProperties(UObject* Object, const TArray<TSharedPtr<FJsonValue>>* Properties, TArray<TSharedPtr<FJsonValue>>& OutResults, FString& OutError)
	{
		if (!Object || !Properties)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropObjPtr = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(PropObjPtr) || !PropObjPtr || !PropObjPtr->IsValid())
			{
				OutError = TEXT("invalid_property_item");
				return false;
			}
			FString Name;
			FString ValueText;
			if (!(*PropObjPtr)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				OutError = TEXT("missing_property_name");
				return false;
			}
			if (!(*PropObjPtr)->TryGetStringField(TEXT("value_text"), ValueText))
			{
				(*PropObjPtr)->TryGetStringField(TEXT("value"), ValueText);
			}
			if (ValueText.IsEmpty())
			{
				OutError = TEXT("missing_property_value_text");
				return false;
			}
			FString ImportError;
			if (!ImportPropertyValue(Object->GetClass(), Object, Object, Name, ValueText, ImportError))
			{
				OutError = ImportError;
				return false;
			}
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("property_name"), Name);
			Result->SetStringField(TEXT("requested_value_text"), ValueText);
			Result->SetStringField(TEXT("status"), TEXT("applied"));
			OutResults.Add(MakeShared<FJsonValueObject>(Result));
		}
		return true;
	}

	static bool ApplyStructProperties(UScriptStruct* Struct, void* StructMemory, UObject* Owner, const TArray<TSharedPtr<FJsonValue>>* Properties, TArray<TSharedPtr<FJsonValue>>& OutResults, FString& OutError)
	{
		if (!Struct || !StructMemory || !Properties)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropObjPtr = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(PropObjPtr) || !PropObjPtr || !PropObjPtr->IsValid())
			{
				OutError = TEXT("invalid_property_item");
				return false;
			}
			FString Name;
			FString ValueText;
			if (!(*PropObjPtr)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				OutError = TEXT("missing_property_name");
				return false;
			}
			if (!(*PropObjPtr)->TryGetStringField(TEXT("value_text"), ValueText))
			{
				(*PropObjPtr)->TryGetStringField(TEXT("value"), ValueText);
			}
			if (ValueText.IsEmpty())
			{
				OutError = TEXT("missing_property_value_text");
				return false;
			}
			FString ImportError;
			if (!ImportPropertyValue(Struct, StructMemory, Owner, Name, ValueText, ImportError))
			{
				OutError = ImportError;
				return false;
			}
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("property_name"), Name);
			Result->SetStringField(TEXT("requested_value_text"), ValueText);
			Result->SetStringField(TEXT("status"), TEXT("applied"));
			OutResults.Add(MakeShared<FJsonValueObject>(Result));
		}
		return true;
	}

	static bool StrictCheckFields(const TSharedPtr<FJsonObject>& Obj, const TSet<FString>& AllowedFields, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Obj.IsValid())
		{
			AddIssue(Issues, TEXT("invalid_json_object"), TEXT("Expected JSON object."), Path);
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			if (!AllowedFields.Contains(Pair.Key))
			{
				AddIssue(Issues, TEXT("unknown_field"), FString::Printf(TEXT("Unknown field '%s'."), *Pair.Key), Path.IsEmpty() ? Pair.Key : Path + TEXT(".") + Pair.Key);
			}
		}
		return Issues.Num() == 0;
	}

	template<typename EnumType>
	static EnumType EnumFromString(const FString& Text, EnumType DefaultValue)
	{
		if (Text.IsEmpty())
		{
			return DefaultValue;
		}
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			const int64 Value = Enum->GetValueByNameString(Text);
			if (Value != INDEX_NONE)
			{
				return static_cast<EnumType>(Value);
			}
			const int64 ShortValue = Enum->GetValueByNameString(Enum->GenerateFullEnumName(*Text));
			if (ShortValue != INDEX_NONE)
			{
				return static_cast<EnumType>(ShortValue);
			}
		}
		return DefaultValue;
	}

	template<typename EnumType>
	static FString EnumToString(EnumType Value)
	{
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	static UClass* BlackboardKeyTypeClassFromString(const FString& TypeText)
	{
		FString Lower = TypeText.ToLower();
		if (Lower == TEXT("bool") || Lower == TEXT("boolean")) return UBlackboardKeyType_Bool::StaticClass();
		if (Lower == TEXT("int") || Lower == TEXT("integer")) return UBlackboardKeyType_Int::StaticClass();
		if (Lower == TEXT("float") || Lower == TEXT("double")) return UBlackboardKeyType_Float::StaticClass();
		if (Lower == TEXT("vector")) return UBlackboardKeyType_Vector::StaticClass();
		if (Lower == TEXT("rotator")) return UBlackboardKeyType_Rotator::StaticClass();
		if (Lower == TEXT("string")) return UBlackboardKeyType_String::StaticClass();
		if (Lower == TEXT("name")) return UBlackboardKeyType_Name::StaticClass();
		if (Lower == TEXT("object") || Lower == TEXT("actor")) return UBlackboardKeyType_Object::StaticClass();
		if (Lower == TEXT("class")) return UBlackboardKeyType_Class::StaticClass();
		if (Lower == TEXT("enum") || Lower == TEXT("native_enum")) return UBlackboardKeyType_Enum::StaticClass();
		return ResolveClass(TypeText);
	}

	static FString BlackboardKeyTypeToString(const UBlackboardKeyType* KeyType)
	{
		if (!KeyType)
		{
			return TEXT("None");
		}
		const UClass* Class = KeyType->GetClass();
		if (Class == UBlackboardKeyType_Bool::StaticClass()) return TEXT("bool");
		if (Class == UBlackboardKeyType_Int::StaticClass()) return TEXT("int");
		if (Class == UBlackboardKeyType_Float::StaticClass()) return TEXT("float");
		if (Class == UBlackboardKeyType_Vector::StaticClass()) return TEXT("vector");
		if (Class == UBlackboardKeyType_Rotator::StaticClass()) return TEXT("rotator");
		if (Class == UBlackboardKeyType_String::StaticClass()) return TEXT("string");
		if (Class == UBlackboardKeyType_Name::StaticClass()) return TEXT("name");
		if (Class == UBlackboardKeyType_Object::StaticClass()) return TEXT("object");
		if (Class == UBlackboardKeyType_Class::StaticClass()) return TEXT("class");
		if (Class == UBlackboardKeyType_Enum::StaticClass()) return TEXT("enum");
		return Class->GetPathName();
	}

	static void ConfigureBlackboardKeyType(UBlackboardKeyType* KeyType, const TSharedPtr<FJsonObject>& KeyObj)
	{
		if (!KeyType || !KeyObj.IsValid())
		{
			return;
		}
		FString BaseClassPath;
		if (KeyObj->TryGetStringField(TEXT("base_class"), BaseClassPath) || KeyObj->TryGetStringField(TEXT("object_base_class"), BaseClassPath))
		{
			if (UClass* BaseClass = ResolveClass(BaseClassPath))
			{
				if (UBlackboardKeyType_Object* ObjectKey = Cast<UBlackboardKeyType_Object>(KeyType))
				{
					ObjectKey->BaseClass = BaseClass;
				}
				if (UBlackboardKeyType_Class* ClassKey = Cast<UBlackboardKeyType_Class>(KeyType))
				{
					ClassKey->BaseClass = BaseClass;
				}
			}
		}
		FString EnumPath;
		if (!KeyObj->TryGetStringField(TEXT("enum"), EnumPath))
		{
			KeyObj->TryGetStringField(TEXT("enum_type"), EnumPath);
		}
		if (!EnumPath.IsEmpty())
		{
			if (UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(KeyType))
			{
				EnumKey->EnumType = LoadObject<UEnum>(nullptr, *EnumPath);
			}
		}
		FString EnumName;
		if (KeyObj->TryGetStringField(TEXT("enum_name"), EnumName))
		{
			if (UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(KeyType))
			{
				EnumKey->EnumName = EnumName;
			}
		}
	}

	static TSharedPtr<FJsonObject> ExportBlackboardKey(const FBlackboardEntry& Entry, const bool bInherited)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
		Obj->SetStringField(TEXT("type"), BlackboardKeyTypeToString(Entry.KeyType));
		Obj->SetStringField(TEXT("key_type"), BlackboardKeyTypeToString(Entry.KeyType));
		Obj->SetStringField(TEXT("key_type_class"), Entry.KeyType ? Entry.KeyType->GetClass()->GetPathName() : FString());
		Obj->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced != 0);
		Obj->SetBoolField(TEXT("inherited"), bInherited);
#if WITH_EDITORONLY_DATA
		Obj->SetStringField(TEXT("description"), Entry.EntryDescription);
		Obj->SetStringField(TEXT("category"), Entry.EntryCategory.ToString());
#endif
		if (const UBlackboardKeyType_Object* ObjectKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
		{
			Obj->SetStringField(TEXT("base_class"), ObjectKey->BaseClass ? ObjectKey->BaseClass->GetPathName() : FString());
		}
		if (const UBlackboardKeyType_Class* ClassKey = Cast<UBlackboardKeyType_Class>(Entry.KeyType))
		{
			Obj->SetStringField(TEXT("base_class"), ClassKey->BaseClass ? ClassKey->BaseClass->GetPathName() : FString());
		}
		if (const UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(Entry.KeyType))
		{
			Obj->SetStringField(TEXT("enum"), EnumKey->EnumType ? EnumKey->EnumType->GetPathName() : FString());
			Obj->SetStringField(TEXT("enum_type"), EnumKey->EnumType ? EnumKey->EnumType->GetPathName() : FString());
			Obj->SetStringField(TEXT("enum_name"), EnumKey->EnumName);
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> ExportBlackboard(UBlackboardData* Blackboard, const bool bIncludeParentKeys)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), BlackboardProfile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		Root->SetStringField(TEXT("asset_path"), Blackboard ? Blackboard->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), Blackboard ? Blackboard->GetPathName() : FString());
		Root->SetStringField(TEXT("parent_blackboard"), (Blackboard && Blackboard->Parent) ? Blackboard->Parent->GetPathName() : FString());
		Root->SetBoolField(TEXT("dirty"), Blackboard && Blackboard->GetOutermost()->IsDirty());
		TArray<TSharedPtr<FJsonValue>> Keys;
		if (Blackboard)
		{
			for (const FBlackboardEntry& Entry : Blackboard->Keys)
			{
				Keys.Add(MakeShared<FJsonValueObject>(ExportBlackboardKey(Entry, false)));
			}
#if WITH_EDITORONLY_DATA
			if (bIncludeParentKeys)
			{
				for (const FBlackboardEntry& Entry : Blackboard->ParentKeys)
				{
					Keys.Add(MakeShared<FJsonValueObject>(ExportBlackboardKey(Entry, true)));
				}
			}
#endif
		}
		Root->SetArrayField(TEXT("keys"), Keys);
		return Root;
	}

	static bool TryGetBlackboardKeyTypeText(const TSharedPtr<FJsonObject>& KeyObj, FString& OutType)
	{
		if (!KeyObj.IsValid())
		{
			return false;
		}
		if (KeyObj->TryGetStringField(TEXT("type"), OutType) && !OutType.IsEmpty())
		{
			return true;
		}
		return KeyObj->TryGetStringField(TEXT("key_type"), OutType) && !OutType.IsEmpty();
	}

	static bool ValidateBlackboardRoot(const TSharedPtr<FJsonObject>& Root, const bool bStrict, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Root.IsValid())
		{
			AddIssue(Issues, TEXT("invalid_json_root"), TEXT("Root must be an object."), TEXT("$"));
			return false;
		}
		if (bStrict)
		{
			StrictCheckFields(Root, { TEXT("profile"), TEXT("schema_version"), TEXT("asset_path"), TEXT("object_path"), TEXT("parent_blackboard"), TEXT("dirty"), TEXT("valid"), TEXT("keys"), TEXT("delete_missing") }, TEXT("$"), Issues);
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!Root->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
		{
			AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing keys array."), TEXT("$.keys"));
			return false;
		}
		TSet<FString> Names;
		for (int32 Index = 0; Index < Keys->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* KeyObjPtr = nullptr;
			if (!(*Keys)[Index].IsValid() || !(*Keys)[Index]->TryGetObject(KeyObjPtr) || !KeyObjPtr || !KeyObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_key_item"), TEXT("Key item must be an object."), FString::Printf(TEXT("$.keys[%d]"), Index));
				continue;
			}
			const TSharedPtr<FJsonObject>& KeyObj = *KeyObjPtr;
			if (bStrict)
			{
				StrictCheckFields(KeyObj, { TEXT("name"), TEXT("type"), TEXT("key_type"), TEXT("key_type_class"), TEXT("base_class"), TEXT("object_base_class"), TEXT("enum"), TEXT("enum_type"), TEXT("enum_name"), TEXT("instance_synced"), TEXT("inherited"), TEXT("description"), TEXT("category"), TEXT("_delete") }, FString::Printf(TEXT("$.keys[%d]"), Index), Issues);
			}
			FString Name;
			FString Type;
			if (!KeyObj->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing key name."), FString::Printf(TEXT("$.keys[%d].name"), Index));
			}
			if (!TryGetBlackboardKeyTypeText(KeyObj, Type))
			{
				AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing key type; provide type or key_type."), FString::Printf(TEXT("$.keys[%d].type"), Index));
			}
			else
			{
				UClass* KeyTypeClass = BlackboardKeyTypeClassFromString(Type);
				if (!KeyTypeClass || !KeyTypeClass->IsChildOf(UBlackboardKeyType::StaticClass()))
				{
					AddIssue(Issues, TEXT("blackboard_key_type_not_found"), FString::Printf(TEXT("Invalid key type '%s'."), *Type), FString::Printf(TEXT("$.keys[%d].type"), Index));
				}
				if (Type.Equals(TEXT("object"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("actor"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("class"), ESearchCase::IgnoreCase))
				{
					FString BaseClassPath;
					if (KeyObj->TryGetStringField(TEXT("base_class"), BaseClassPath) && !BaseClassPath.IsEmpty() && !ResolveClass(BaseClassPath))
					{
						AddIssue(Issues, TEXT("blackboard_base_class_not_found"), FString::Printf(TEXT("Invalid base_class '%s'."), *BaseClassPath), FString::Printf(TEXT("$.keys[%d].base_class"), Index));
					}
				}
				if (Type.Equals(TEXT("enum"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("native_enum"), ESearchCase::IgnoreCase))
				{
					FString EnumPath;
					if (!KeyObj->TryGetStringField(TEXT("enum"), EnumPath))
					{
						KeyObj->TryGetStringField(TEXT("enum_type"), EnumPath);
					}
					if (!EnumPath.IsEmpty() && !LoadObject<UEnum>(nullptr, *EnumPath))
					{
						AddIssue(Issues, TEXT("blackboard_enum_not_found"), FString::Printf(TEXT("Invalid enum '%s'."), *EnumPath), FString::Printf(TEXT("$.keys[%d].enum_type"), Index));
					}
				}
			}
			if (!Name.IsEmpty())
			{
				if (Names.Contains(Name))
				{
					AddIssue(Issues, TEXT("duplicate_id"), FString::Printf(TEXT("Duplicate key name '%s'."), *Name), FString::Printf(TEXT("$.keys[%d].name"), Index));
				}
				Names.Add(Name);
			}
		}
		return Issues.Num() == 0;
	}

	static bool ApplyBlackboardRoot(UBlackboardData* Blackboard, const TSharedPtr<FJsonObject>& Root, const bool bAllowDestructive, const bool bValidateOnly, TSharedPtr<FJsonObject>& OutData, FString& OutError)
	{
		if (!Blackboard || !Root.IsValid())
		{
			OutError = TEXT("blackboard_not_found");
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Issues;
		if (!ValidateBlackboardRoot(Root, false, Issues))
		{
			OutData->SetStringField(TEXT("status"), TEXT("validate_failed"));
			SetDiagnostics(OutData, Issues);
			return false;
		}

		FString ParentPath;
		if (Root->TryGetStringField(TEXT("parent_blackboard"), ParentPath) && !ParentPath.IsEmpty())
		{
			UBlackboardData* Parent = LoadAsset<UBlackboardData>(ParentPath);
			if (!Parent)
			{
				OutError = TEXT("parent_blackboard_not_found");
				return false;
			}
			if (Parent == Blackboard || Parent->IsChildOf(*Blackboard))
			{
				OutError = TEXT("blackboard_parent_cycle");
				return false;
			}
			if (!bValidateOnly)
			{
				Blackboard->Parent = Parent;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		Root->TryGetArrayField(TEXT("keys"), Keys);
		bool bDeleteMissing = false;
		Root->TryGetBoolField(TEXT("delete_missing"), bDeleteMissing);

		TArray<FBlackboardEntry> NewKeys;
		TSet<FName> SourceNames;
		for (const TSharedPtr<FJsonValue>& Value : *Keys)
		{
			const TSharedPtr<FJsonObject>* KeyObjPtr = nullptr;
			Value->TryGetObject(KeyObjPtr);
			const TSharedPtr<FJsonObject>& KeyObj = *KeyObjPtr;
			bool bDelete = false;
			KeyObj->TryGetBoolField(TEXT("_delete"), bDelete);
			if (bDelete)
			{
				if (!bAllowDestructive)
				{
					OutError = TEXT("blackboard_key_delete_requires_opt_in");
					return false;
				}
				continue;
			}

			FString NameText;
			FString TypeText;
			KeyObj->TryGetStringField(TEXT("name"), NameText);
			TryGetBlackboardKeyTypeText(KeyObj, TypeText);
			UClass* KeyTypeClass = BlackboardKeyTypeClassFromString(TypeText);
			if (!KeyTypeClass || !KeyTypeClass->IsChildOf(UBlackboardKeyType::StaticClass()))
			{
				OutError = TEXT("blackboard_key_type_not_found");
				return false;
			}
			FBlackboardEntry Entry;
			Entry.EntryName = FName(*NameText);
			Entry.KeyType = bValidateOnly ? nullptr : NewObject<UBlackboardKeyType>(Blackboard, KeyTypeClass, NAME_None, RF_Transactional);
			bool bInstanceSynced = false;
			KeyObj->TryGetBoolField(TEXT("instance_synced"), bInstanceSynced);
			Entry.bInstanceSynced = bInstanceSynced ? 1 : 0;
#if WITH_EDITORONLY_DATA
			FString Description;
			if (KeyObj->TryGetStringField(TEXT("description"), Description))
			{
				Entry.EntryDescription = Description;
			}
			FString Category;
			if (KeyObj->TryGetStringField(TEXT("category"), Category))
			{
				Entry.EntryCategory = FName(*Category);
			}
#endif
			if (Entry.KeyType)
			{
				ConfigureBlackboardKeyType(Entry.KeyType, KeyObj);
			}
			SourceNames.Add(Entry.EntryName);
			NewKeys.Add(Entry);
		}

		if (!bDeleteMissing)
		{
			for (const FBlackboardEntry& Existing : Blackboard->Keys)
			{
				if (!SourceNames.Contains(Existing.EntryName))
				{
					NewKeys.Add(Existing);
				}
			}
		}
		else if (!bAllowDestructive && Blackboard->Keys.Num() > SourceNames.Num())
		{
			OutError = TEXT("blackboard_key_delete_requires_opt_in");
			return false;
		}

		if (!bValidateOnly)
		{
			Blackboard->Modify();
			Blackboard->Keys = MoveTemp(NewKeys);
			Blackboard->UpdateParentKeys();
			Blackboard->UpdateKeyIDs();
			Blackboard->UpdateIfHasSynchronizedKeys();
			Blackboard->MarkPackageDirty();
			Blackboard->PostEditChange();
		}
		OutData = ExportBlackboard(Blackboard, false);
		OutData->SetBoolField(TEXT("changed"), !bValidateOnly);
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetArrayField(TEXT("property_results"), TArray<TSharedPtr<FJsonValue>>());
		AttachCoverageReport(OutData);
		return true;
	}
}

bool FUeAgentHttpServer::ExecuteAICommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("blackboard_create")) return CmdBlackboardCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blackboard_get_info")) return CmdBlackboardGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blackboard_export_json")) return CmdBlackboardExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blackboard_validate_json")) return CmdBlackboardValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blackboard_apply_json")) return CmdBlackboardApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("behavior_tree_create")) return CmdBehaviorTreeCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("behavior_tree_get_info")) return CmdBehaviorTreeGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("behavior_tree_export_folder")) return CmdBehaviorTreeExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("behavior_tree_validate_folder")) return CmdBehaviorTreeValidateFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("behavior_tree_apply_folder")) return CmdBehaviorTreeApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("behavior_tree_open_editor")) return CmdBehaviorTreeOpenEditor(Ctx, OutData, OutError);
	if (CommandLower == TEXT("behavior_tree_graph_get_view")) return CmdBehaviorTreeGraphGetView(Ctx, OutData, OutError);
	if (CommandLower == TEXT("behavior_tree_graph_set_view")) return CmdBehaviorTreeGraphSetView(Ctx, OutData, OutError);
	if (CommandLower == TEXT("behavior_tree_screenshot")) return CmdBehaviorTreeScreenshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("behavior_tree_runtime_snapshot")) return CmdBehaviorTreeRuntimeSnapshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("bt_node_blueprint_get_info")) return CmdBTNodeBlueprintGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("state_tree_create")) return CmdStateTreeCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("state_tree_get_info")) return CmdStateTreeGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("state_tree_export_folder")) return CmdStateTreeExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("state_tree_validate_folder")) return CmdStateTreeValidateFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("state_tree_apply_folder")) return CmdStateTreeApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("state_tree_open_editor")) return CmdStateTreeOpenEditor(Ctx, OutData, OutError);
	if (CommandLower == TEXT("state_tree_screenshot")) return CmdStateTreeScreenshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("state_tree_runtime_snapshot")) return CmdStateTreeRuntimeSnapshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("state_tree_node_blueprint_get_info")) return CmdStateTreeNodeBlueprintGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("eqs_create")) return CmdEqsCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("eqs_get_info")) return CmdEqsGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("eqs_export_folder")) return CmdEqsExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("eqs_validate_folder")) return CmdEqsValidateFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("eqs_apply_folder")) return CmdEqsApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("eqs_run_query")) return CmdEqsRunQuery(Ctx, OutData, OutError);
	if (CommandLower == TEXT("eqs_debug_snapshot")) return CmdEqsDebugSnapshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ai_perception_get_component_info")) return CmdAIPerceptionGetComponentInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ai_perception_validate_setup")) return CmdAIPerceptionValidateSetup(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ai_perception_runtime_snapshot")) return CmdAIPerceptionRuntimeSnapshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ai_perception_runtime_probe")) return CmdAIPerceptionRuntimeProbe(Ctx, OutData, OutError);
	if (CommandLower == TEXT("navigation_get_info")) return CmdNavigationGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("navigation_export_config_json")) return CmdNavigationExportConfigJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("navigation_validate_level")) return CmdNavigationValidateLevel(Ctx, OutData, OutError);
	if (CommandLower == TEXT("navigation_path_probe")) return CmdNavigationPathProbe(Ctx, OutData, OutError);
	if (CommandLower == TEXT("navigation_area_cost_probe")) return CmdNavigationAreaCostProbe(Ctx, OutData, OutError);
	if (CommandLower == TEXT("navigation_runtime_snapshot")) return CmdNavigationRuntimeSnapshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_definition_create")) return CmdSmartObjectDefinitionCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_definition_get_info")) return CmdSmartObjectDefinitionGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_definition_export_json")) return CmdSmartObjectDefinitionExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_definition_validate_json")) return CmdSmartObjectDefinitionValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_definition_apply_json")) return CmdSmartObjectDefinitionApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_validate_setup")) return CmdSmartObjectValidateSetup(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_find")) return CmdSmartObjectFind(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_claim")) return CmdSmartObjectClaim(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_release")) return CmdSmartObjectRelease(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_runtime_snapshot")) return CmdSmartObjectRuntimeSnapshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("smart_object_runtime_probe")) return CmdSmartObjectRuntimeProbe(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ai_behavior_stack_export_folder")) return CmdAIBehaviorStackExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ai_behavior_stack_validate_folder")) return CmdAIBehaviorStackValidateFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ai_behavior_stack_runtime_probe")) return CmdAIBehaviorStackRuntimeProbe(Ctx, OutData, OutError);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("command"), CommandLower);
	OutData->SetStringField(TEXT("category"), TEXT("ai"));
	OutError = TEXT("unknown_ai_command");
	return false;
}

bool FUeAgentHttpServer::CmdBlackboardCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UBlackboardData* Blackboard = nullptr;
	if (!UeAgentAIOps::CreateAsset<UBlackboardData>(AssetPath, Blackboard, OutError) || !Blackboard)
	{
		return false;
	}

	FString ParentPath;
	if (JsonTryGetString(Ctx.Params, TEXT("parent_blackboard"), ParentPath) && !ParentPath.IsEmpty())
	{
		UBlackboardData* Parent = UeAgentAIOps::LoadAsset<UBlackboardData>(ParentPath);
		if (!Parent)
		{
			OutError = TEXT("parent_blackboard_not_found");
			return false;
		}
		Blackboard->Parent = Parent;
	}

	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("keys"), Keys) && Keys)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("keys"), *Keys);
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		if (!ParentPath.IsEmpty())
		{
			Root->SetStringField(TEXT("parent_blackboard"), ParentPath);
		}
		if (!UeAgentAIOps::ApplyBlackboardRoot(Blackboard, Root, true, false, OutData, OutError))
		{
			return false;
		}
	}
	else
	{
		OutData = UeAgentAIOps::ExportBlackboard(Blackboard, false);
	}

	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Blackboard, OutError))
	{
		return false;
	}
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdBlackboardGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBlackboardData* Blackboard = UeAgentAIOps::LoadAsset<UBlackboardData>(AssetPath);
	if (!Blackboard)
	{
		OutError = TEXT("blackboard_not_found");
		return false;
	}
	bool bIncludeParentKeys = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_parent_keys"), bIncludeParentKeys);
	OutData = UeAgentAIOps::ExportBlackboard(Blackboard, bIncludeParentKeys);
	OutData->SetBoolField(TEXT("valid"), Blackboard->IsValid());
	return true;
}

bool FUeAgentHttpServer::CmdBlackboardExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdBlackboardGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	if (!OutputFile.IsEmpty())
	{
		FString SavedPath;
		if (!UeAgentAIOps::SaveJsonFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdBlackboardValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	FString ResolvedPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.IsEmpty())
	{
		OutError = TEXT("missing_json_file");
		return false;
	}
	if (!UeAgentAIOps::LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	bool bStrict = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bValid = UeAgentAIOps::ValidateBlackboardRoot(Root, bStrict, Issues);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::BlackboardProfile);
	OutData->SetStringField(TEXT("json_file"), ResolvedPath);
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetStringField(TEXT("status"), bValid ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return bValid;
}

bool FUeAgentHttpServer::CmdBlackboardApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	FString ResolvedPath;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentAIOps::LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
		{
			return false;
		}
	}
	else
	{
		Root = Ctx.Params;
	}

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty() && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bAllowDestructive = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);

	UBlackboardData* Blackboard = UeAgentAIOps::LoadAsset<UBlackboardData>(AssetPath);
	bool bCreated = false;
	if (!Blackboard)
	{
		if (bDryRun || bValidateOnly)
		{
			TArray<TSharedPtr<FJsonValue>> Issues;
			const bool bValid = UeAgentAIOps::ValidateBlackboardRoot(Root, false, Issues);
			OutData->SetStringField(TEXT("profile"), UeAgentAIOps::BlackboardProfile);
			OutData->SetStringField(TEXT("asset_path"), AssetPath);
			OutData->SetStringField(TEXT("status"), bValid ? TEXT("ok") : TEXT("validate_failed"));
			OutData->SetBoolField(TEXT("changed"), false);
			OutData->SetBoolField(TEXT("created_asset"), false);
			OutData->SetBoolField(TEXT("would_create_asset"), bCreateIfMissing);
			OutData->SetBoolField(TEXT("dry_run"), bDryRun);
			OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
			UeAgentAIOps::SetDiagnostics(OutData, Issues);
			return bValid;
		}
		if (!bCreateIfMissing)
		{
			OutError = TEXT("blackboard_not_found");
			return false;
		}
		if (!UeAgentAIOps::CreateAsset<UBlackboardData>(AssetPath, Blackboard, OutError) || !Blackboard)
		{
			return false;
		}
		bCreated = true;
	}

	if (!UeAgentAIOps::ApplyBlackboardRoot(Blackboard, Root, bAllowDestructive, bDryRun || bValidateOnly, OutData, OutError))
	{
		return false;
	}

	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSave);
	if (bSave && !bDryRun && !bValidateOnly && !UeAgentAIOps::SaveAssetPackage(Blackboard, OutError))
	{
		return false;
	}
	if (!ResolvedPath.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolvedPath);
	}
	OutData->SetBoolField(TEXT("created_asset"), bCreated);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
	OutData->SetBoolField(TEXT("saved"), bSave && !bDryRun && !bValidateOnly);
	return true;
}
