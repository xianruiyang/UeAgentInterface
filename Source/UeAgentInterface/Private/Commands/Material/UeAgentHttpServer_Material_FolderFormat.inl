namespace UeAgentMaterialFolderOps
{
	struct FMaterialSettingSpec
	{
		const TCHAR* Key;
		const TCHAR* PropertyName;
	};

	struct FRootMaterialPropertySpec
	{
		const TCHAR* Name;
		EMaterialProperty Property;
	};

	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("MaterialGraph")));
	}

	static FString NormalizeAssetRelativeFolder(const FString& InAssetPath)
	{
		FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(InAssetPath);
		AssetPath.TrimStartAndEndInline();
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		return AssetPath;
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		const FString RelativeFolder = NormalizeAssetRelativeFolder(AssetPath);
		OutFolderPath = FPaths::Combine(GetFolderRootAbsolute(), RelativeFolder);
		return true;
	}

	static FString MakeJsonString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return JsonText;
	}

	static bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		return FFileHelper::SaveStringToFile(
			MakeJsonString(Obj),
			*FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			OutError = TEXT("json_file_not_found");
			return false;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}
		return true;
	}

	static bool LoadOptionalJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		OutObj.Reset();
		if (!FPaths::FileExists(FilePath))
		{
			return true;
		}

		FString LoadError;
		if (!LoadJsonFile(FilePath, OutObj, LoadError))
		{
			OutError = FString::Printf(TEXT("%s:%s"), *LoadError, *FilePath);
			return false;
		}
		return true;
	}

	static bool TryGetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	static bool TryGetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	static bool TryGetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Key, OutValue);
	}

	static bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, TSharedPtr<FJsonObject>& OutValue)
	{
		const TSharedPtr<FJsonObject>* FoundObject = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(Key, FoundObject) || !FoundObject || !FoundObject->IsValid())
		{
			return false;
		}
		OutValue = *FoundObject;
		return true;
	}

	static bool ExportPropertyText(UObject* Obj, const FString& PropertyPath, FString& OutValue)
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!UeAgentMaterialOps::ResolvePropertyPath(Obj, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			return false;
		}
		Property->ExportTextItem_Direct(OutValue, ValuePtr, ValuePtr, Obj, PPF_None);
		return true;
	}

	static const TArray<FMaterialSettingSpec>& GetSettingSpecs()
	{
		static const TArray<FMaterialSettingSpec> Specs = {
			{ TEXT("material_domain"), TEXT("MaterialDomain") },
			{ TEXT("blend_mode"), TEXT("BlendMode") },
			{ TEXT("shading_models"), TEXT("ShadingModels") },
			{ TEXT("two_sided"), TEXT("TwoSided") },
			{ TEXT("use_material_attributes"), TEXT("bUseMaterialAttributes") },
			{ TEXT("cast_ray_traced_shadows"), TEXT("bCastRayTracedShadows") },
			{ TEXT("subsurface_profile"), TEXT("SubsurfaceProfile") },
			{ TEXT("opacity_mask_clip_value"), TEXT("OpacityMaskClipValue") },
			{ TEXT("translucency_screen_space_reflections"), TEXT("bScreenSpaceReflections") },
			{ TEXT("translucency_contact_shadows"), TEXT("bContactShadows") }
		};
		return Specs;
	}

	static const TArray<FRootMaterialPropertySpec>& GetRootMaterialPropertySpecs()
	{
		static const TArray<FRootMaterialPropertySpec> Specs = {
			{ TEXT("BaseColor"), MP_BaseColor },
			{ TEXT("EmissiveColor"), MP_EmissiveColor },
			{ TEXT("Opacity"), MP_Opacity },
			{ TEXT("OpacityMask"), MP_OpacityMask },
			{ TEXT("Metallic"), MP_Metallic },
			{ TEXT("Specular"), MP_Specular },
			{ TEXT("Roughness"), MP_Roughness },
			{ TEXT("Normal"), MP_Normal },
			{ TEXT("Tangent"), MP_Tangent },
			{ TEXT("WorldPositionOffset"), MP_WorldPositionOffset },
			{ TEXT("SubsurfaceColor"), MP_SubsurfaceColor },
			{ TEXT("AmbientOcclusion"), MP_AmbientOcclusion },
			{ TEXT("Refraction"), MP_Refraction },
			{ TEXT("PixelDepthOffset"), MP_PixelDepthOffset },
			{ TEXT("Anisotropy"), MP_Anisotropy }
		};
		return Specs;
	}

	static FString MakeSlug(const FString& InText)
	{
		FString Out = InText.TrimStartAndEnd().ToLower();
		for (int32 Index = 0; Index < Out.Len(); ++Index)
		{
			TCHAR& Ch = Out[Index];
			const bool bAlphaNum = (Ch >= TEXT('a') && Ch <= TEXT('z'))
				|| (Ch >= TEXT('0') && Ch <= TEXT('9'));
			if (!bAlphaNum)
			{
				Ch = TEXT('_');
			}
		}
		while (Out.Contains(TEXT("__")))
		{
			Out.ReplaceInline(TEXT("__"), TEXT("_"));
		}
		bool bIgnored = false;
		Out.TrimCharInline(TEXT('_'), &bIgnored);
		return Out.IsEmpty() ? TEXT("node") : Out;
	}

	static FString MakeBaseExpressionId(UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return TEXT("expr");
		}

		if (Expression->HasAParameterName())
		{
			const FName ParameterName = Expression->GetParameterName();
			if (!ParameterName.IsNone())
			{
				return FString::Printf(TEXT("param_%s"), *MakeSlug(ParameterName.ToString()));
			}
		}

		if (!Expression->Desc.TrimStartAndEnd().IsEmpty())
		{
			return FString::Printf(TEXT("expr_%s"), *MakeSlug(Expression->Desc));
		}

		return FString::Printf(TEXT("expr_%s"), *MakeSlug(Expression->GetClass()->GetName()));
	}

	static bool ShouldExportExpressionProperty(const FProperty* Property)
	{
		if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
		{
			return false;
		}

		static const TSet<FString> DenyList = {
			TEXT("MaterialExpressionEditorX"),
			TEXT("MaterialExpressionEditorY"),
			TEXT("MaterialExpressionGuid"),
			TEXT("Material"),
			TEXT("Function"),
			TEXT("GraphNode"),
			TEXT("SubgraphExpression")
		};

		if (DenyList.Contains(Property->GetName()))
		{
			return false;
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (!ObjectProperty->PropertyClass)
			{
				return false;
			}
			if (ObjectProperty->PropertyClass->IsChildOf(UMaterialExpression::StaticClass())
				|| ObjectProperty->PropertyClass->IsChildOf(UMaterial::StaticClass())
				|| ObjectProperty->PropertyClass->GetName().Contains(TEXT("EdGraph")))
			{
				return false;
			}
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct)
			{
				const FString StructName = StructProperty->Struct->GetName();
				if (StructName.Contains(TEXT("ExpressionInput")) || StructName.Contains(TEXT("ExpressionOutput")))
				{
					return false;
				}
			}
		}

		if (CastField<FArrayProperty>(Property) || CastField<FSetProperty>(Property) || CastField<FMapProperty>(Property))
		{
			return false;
		}

		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> ExportExpressionProperties(UMaterialExpression* Expression)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		if (!Expression)
		{
			return Result;
		}

		TArray<FProperty*> Properties;
		for (TFieldIterator<FProperty> It(Expression->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (ShouldExportExpressionProperty(Property))
			{
				Properties.Add(Property);
			}
		}
		Properties.Sort([](const FProperty& A, const FProperty& B)
		{
			return A.GetName().Compare(B.GetName(), ESearchCase::IgnoreCase) < 0;
		});

		for (FProperty* Property : Properties)
		{
			FString ValueText;
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Expression);
			if (!ValuePtr)
			{
				continue;
			}
			Property->ExportTextItem_Direct(ValueText, ValuePtr, ValuePtr, Expression, PPF_None);

			TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
			PropertyObj->SetStringField(TEXT("property_name"), Property->GetName());
			PropertyObj->SetStringField(TEXT("value_text"), ValueText);
			Result.Add(MakeShared<FJsonValueObject>(PropertyObj));
		}

		if (const UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			if (FunctionCall->MaterialFunction)
			{
				TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
				PropertyObj->SetStringField(TEXT("property_name"), TEXT("MaterialFunction"));
				PropertyObj->SetStringField(
					TEXT("value_text"),
					FString::Printf(
						TEXT("MaterialFunction'%s'"),
						*FunctionCall->MaterialFunction->GetPathName()));
				Result.Add(MakeShared<FJsonValueObject>(PropertyObj));
			}
		}

		Result.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject>* AObj = nullptr;
			const TSharedPtr<FJsonObject>* BObj = nullptr;
			const FString AName = (A.IsValid() && A->TryGetObject(AObj) && AObj && AObj->IsValid()) ? (*AObj)->GetStringField(TEXT("property_name")) : FString();
			const FString BName = (B.IsValid() && B->TryGetObject(BObj) && BObj && BObj->IsValid()) ? (*BObj)->GetStringField(TEXT("property_name")) : FString();
			return AName.Compare(BName, ESearchCase::IgnoreCase) < 0;
		});

		return Result;
	}

	static FString DetermineParameterType(UMaterialExpression* Expression)
	{
		if (!Expression || !Expression->HasAParameterName())
		{
			return FString();
		}

		const FString ClassName = Expression->GetClass()->GetName();
		if (ClassName.Contains(TEXT("ScalarParameter")))
		{
			return TEXT("scalar");
		}
		if (ClassName.Contains(TEXT("VectorParameter")))
		{
			return TEXT("vector");
		}
		if (ClassName.Contains(TEXT("Texture")) && ClassName.Contains(TEXT("Parameter")))
		{
			return TEXT("texture");
		}
		if (ClassName.Contains(TEXT("StaticSwitchParameter")) || ClassName.Contains(TEXT("StaticBoolParameter")))
		{
			return TEXT("static_switch");
		}
		if (ClassName.Contains(TEXT("StaticComponentMaskParameter")))
		{
			return TEXT("static_component_mask");
		}
		return TEXT("parameter");
	}

	static FString DetermineDefaultPropertyName(UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return FString();
		}

		const FString ClassName = Expression->GetClass()->GetName();
		if (ClassName.Contains(TEXT("ScalarParameter")))
		{
			return TEXT("DefaultValue");
		}
		if (ClassName.Contains(TEXT("VectorParameter")))
		{
			return TEXT("DefaultValue");
		}
		if (ClassName.Contains(TEXT("Texture")) && ClassName.Contains(TEXT("Parameter")))
		{
			return TEXT("Texture");
		}
		if (ClassName.Contains(TEXT("StaticSwitchParameter")) || ClassName.Contains(TEXT("StaticBoolParameter")))
		{
			return TEXT("DefaultValue");
		}
		return FString();
	}

	static void BuildExpressionIdMap(
		UMaterial* Material,
		TArray<UMaterialExpression*>& OutSortedExpressions,
		TMap<const UMaterialExpression*, FString>& OutExpressionToId)
	{
		OutSortedExpressions.Reset();
		OutExpressionToId.Reset();

		if (!Material)
		{
			return;
		}

		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (Expression)
			{
				OutSortedExpressions.Add(Expression);
			}
		}

		OutSortedExpressions.Sort([](const UMaterialExpression& A, const UMaterialExpression& B)
		{
			if (A.MaterialExpressionEditorX != B.MaterialExpressionEditorX)
			{
				return A.MaterialExpressionEditorX < B.MaterialExpressionEditorX;
			}
			if (A.MaterialExpressionEditorY != B.MaterialExpressionEditorY)
			{
				return A.MaterialExpressionEditorY < B.MaterialExpressionEditorY;
			}
			return A.GetName().Compare(B.GetName(), ESearchCase::IgnoreCase) < 0;
		});

		TMap<FString, int32> IdUsage;
		for (UMaterialExpression* Expression : OutSortedExpressions)
		{
			FString BaseId = MakeBaseExpressionId(Expression);
			int32& Counter = IdUsage.FindOrAdd(BaseId);
			const FString FinalId = (Counter++ == 0) ? BaseId : FString::Printf(TEXT("%s_%d"), *BaseId, Counter);
			OutExpressionToId.Add(Expression, FinalId);
		}
	}

	static TSharedPtr<FJsonObject> ExportSettingsJson(UMaterial* Material)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Properties;
		if (!Material)
		{
			Root->SetArrayField(TEXT("properties"), Properties);
			return Root;
		}

		for (const FMaterialSettingSpec& Spec : GetSettingSpecs())
		{
			FString ValueText;
			if (!ExportPropertyText(Material, Spec.PropertyName, ValueText))
			{
				continue;
			}

			TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
			PropertyObj->SetStringField(TEXT("key"), Spec.Key);
			PropertyObj->SetStringField(TEXT("property_name"), Spec.PropertyName);
			PropertyObj->SetStringField(TEXT("value_text"), ValueText);
			Properties.Add(MakeShared<FJsonValueObject>(PropertyObj));
		}

		Root->SetArrayField(TEXT("properties"), Properties);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportAssetJson(UMaterial* Material)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		const FString AssetPath = Material ? Material->GetOutermost()->GetName() : FString();
		Root->SetNumberField(TEXT("format_version"), 1);
		Root->SetStringField(TEXT("asset_kind"), TEXT("material_graph"));
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
		Root->SetNumberField(TEXT("profile_version"), 1);
		Root->SetStringField(TEXT("material_subkind"), TEXT("material"));
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportValidationJson()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Checks;

		TSharedPtr<FJsonObject> CompileCheck = MakeShared<FJsonObject>();
		CompileCheck->SetStringField(TEXT("kind"), TEXT("compile_success"));
		Checks.Add(MakeShared<FJsonValueObject>(CompileCheck));

		Root->SetArrayField(TEXT("checks"), Checks);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportGraphJson(UMaterial* Material)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("name"), TEXT("MaterialGraph"));
		GraphObj->SetStringField(TEXT("graph_kind"), TEXT("material_graph"));
		Root->SetObjectField(TEXT("graph"), GraphObj);

		TArray<UMaterialExpression*> SortedExpressions;
		TMap<const UMaterialExpression*, FString> ExpressionToId;
		BuildExpressionIdMap(Material, SortedExpressions, ExpressionToId);
		TArray<TSharedPtr<FJsonValue>> ExpressionsArray;

		for (UMaterialExpression* Expression : SortedExpressions)
		{
			const FString* FinalIdPtr = ExpressionToId.Find(Expression);
			if (!FinalIdPtr)
			{
				continue;
			}
			const FString& FinalId = *FinalIdPtr;

			TSharedPtr<FJsonObject> ExprObj = MakeShared<FJsonObject>();
			ExprObj->SetStringField(TEXT("id"), FinalId);
			ExprObj->SetStringField(TEXT("ue_expression_guid"), Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			ExprObj->SetStringField(TEXT("expression_name"), Expression->GetName());
			ExprObj->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetPathName());
			if (!Expression->Desc.IsEmpty())
			{
				ExprObj->SetStringField(TEXT("desc"), Expression->Desc);
			}

			TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
			PosObj->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
			PosObj->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
			ExprObj->SetObjectField(TEXT("pos"), PosObj);
			ExprObj->SetArrayField(TEXT("properties"), ExportExpressionProperties(Expression));
			ExpressionsArray.Add(MakeShared<FJsonValueObject>(ExprObj));
		}

		TArray<TSharedPtr<FJsonValue>> EdgesArray;
		for (UMaterialExpression* Expression : SortedExpressions)
		{
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				if (!It->Expression)
				{
					continue;
				}

				const FString* FromId = ExpressionToId.Find(It->Expression);
				const FString* ToId = ExpressionToId.Find(Expression);
				if (!FromId || !ToId)
				{
					continue;
				}

				FString OutputName;
				const TArray<FExpressionOutput>& Outputs = It->Expression->GetOutputs();
				if (Outputs.IsValidIndex(It->OutputIndex))
				{
					OutputName = Outputs[It->OutputIndex].OutputName.ToString();
				}

				FString InputName;
				if (const UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
				{
					InputName = FunctionCall->GetInputNameWithType(It.Index, false).ToString();
				}
				else
				{
					InputName = Expression->GetInputName(It.Index).ToString();
				}
				if (InputName.IsEmpty())
				{
					InputName = It->InputName.ToString();
				}

				TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
				TSharedPtr<FJsonObject> FromObj = MakeShared<FJsonObject>();
				FromObj->SetStringField(TEXT("node"), *FromId);
				FromObj->SetStringField(TEXT("output"), OutputName);
				TSharedPtr<FJsonObject> ToObj = MakeShared<FJsonObject>();
				ToObj->SetStringField(TEXT("node"), *ToId);
				ToObj->SetStringField(TEXT("input"), InputName);
				EdgeObj->SetObjectField(TEXT("from"), FromObj);
				EdgeObj->SetObjectField(TEXT("to"), ToObj);
				EdgesArray.Add(MakeShared<FJsonValueObject>(EdgeObj));
			}
		}

		TArray<TSharedPtr<FJsonValue>> RootInputsArray;
		if (Material)
		{
			for (const FRootMaterialPropertySpec& PropertySpec : GetRootMaterialPropertySpecs())
			{
				FExpressionInput* Input = Material->GetExpressionInputForProperty(PropertySpec.Property);
				if (!Input || !Input->Expression)
				{
					continue;
				}

				const FString* FromId = ExpressionToId.Find(Input->Expression);
				if (!FromId)
				{
					continue;
				}

				FString OutputName;
				const TArray<FExpressionOutput>& Outputs = Input->Expression->GetOutputs();
				if (Outputs.IsValidIndex(Input->OutputIndex))
				{
					OutputName = Outputs[Input->OutputIndex].OutputName.ToString();
				}

				TSharedPtr<FJsonObject> RootInputObj = MakeShared<FJsonObject>();
				RootInputObj->SetStringField(TEXT("material_property"), PropertySpec.Name);
				TSharedPtr<FJsonObject> FromObj = MakeShared<FJsonObject>();
				FromObj->SetStringField(TEXT("node"), *FromId);
				FromObj->SetStringField(TEXT("output"), OutputName);
				RootInputObj->SetObjectField(TEXT("from"), FromObj);
				RootInputsArray.Add(MakeShared<FJsonValueObject>(RootInputObj));
			}
		}

		Root->SetArrayField(TEXT("expressions"), ExpressionsArray);
		Root->SetArrayField(TEXT("edges"), EdgesArray);
		Root->SetArrayField(TEXT("root_inputs"), RootInputsArray);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportParameterInterfaceJson(UMaterial* Material)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ParametersArray;
		if (!Material)
		{
			Root->SetArrayField(TEXT("parameters"), ParametersArray);
			return Root;
		}

		TArray<UMaterialExpression*> SortedExpressions;
		TMap<const UMaterialExpression*, FString> ExpressionToId;
		BuildExpressionIdMap(Material, SortedExpressions, ExpressionToId);

		for (UMaterialExpression* Expression : SortedExpressions)
		{
			if (!Expression || !Expression->HasAParameterName())
			{
				continue;
			}
			const FName ParameterName = Expression->GetParameterName();
			if (ParameterName.IsNone())
			{
				continue;
			}

			const FString* BackingId = ExpressionToId.Find(Expression);
			if (!BackingId)
			{
				continue;
			}

			TSharedPtr<FJsonObject> ParameterObj = MakeShared<FJsonObject>();
			ParameterObj->SetStringField(TEXT("name"), ParameterName.ToString());
			ParameterObj->SetStringField(TEXT("parameter_type"), DetermineParameterType(Expression));
			ParameterObj->SetStringField(TEXT("backing_expression_id"), *BackingId);

			FString GroupText;
			if (ExportPropertyText(Expression, TEXT("Group"), GroupText))
			{
				ParameterObj->SetStringField(TEXT("group"), GroupText);
			}

			FString SortPriorityText;
			if (ExportPropertyText(Expression, TEXT("SortPriority"), SortPriorityText))
			{
				ParameterObj->SetStringField(TEXT("sort_priority"), SortPriorityText);
			}

			const FString DefaultPropertyName = DetermineDefaultPropertyName(Expression);
			if (!DefaultPropertyName.IsEmpty())
			{
				FString DefaultValueText;
				if (ExportPropertyText(Expression, DefaultPropertyName, DefaultValueText))
				{
					ParameterObj->SetStringField(TEXT("default_property_name"), DefaultPropertyName);
					ParameterObj->SetStringField(TEXT("default_value_text"), DefaultValueText);
				}
			}

			ParametersArray.Add(MakeShared<FJsonValueObject>(ParameterObj));
		}

		Root->SetArrayField(TEXT("parameters"), ParametersArray);
		return Root;
	}
}

bool FUeAgentHttpServer::CmdMaterialExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput);
	UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPath);
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}

	FString FolderPath;
	if (!UeAgentMaterialFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	bool bIncludeValidation = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_validation"), bIncludeValidation);

	if (bCleanOutputDir)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}

	const FString SettingsDir = FPaths::Combine(FolderPath, TEXT("settings"));
	const FString ParametersDir = FPaths::Combine(FolderPath, TEXT("parameters"));
	const FString GraphsDir = FPaths::Combine(FolderPath, TEXT("graphs"));
	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));

	IFileManager::Get().MakeDirectory(*SettingsDir, true);
	IFileManager::Get().MakeDirectory(*ParametersDir, true);
	IFileManager::Get().MakeDirectory(*GraphsDir, true);
	if (bIncludeValidation)
	{
		IFileManager::Get().MakeDirectory(*ValidationDir, true);
	}

	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		if (!UeAgentMaterialFolderOps::SaveJsonFile(FilePath, Obj))
		{
			OutError = TEXT("save_export_json_failed");
			return false;
		}
		++FileCount;
		return true;
	};

	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), UeAgentMaterialFolderOps::ExportAssetJson(Material))) return false;
	if (!SaveRequiredJson(FPaths::Combine(SettingsDir, TEXT("material.json")), UeAgentMaterialFolderOps::ExportSettingsJson(Material))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ParametersDir, TEXT("interface.json")), UeAgentMaterialFolderOps::ExportParameterInterfaceJson(Material))) return false;
	if (!SaveRequiredJson(FPaths::Combine(GraphsDir, TEXT("MaterialGraph.json")), UeAgentMaterialFolderOps::ExportGraphJson(Material))) return false;
	if (bIncludeValidation)
	{
		if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("checks.json")), UeAgentMaterialFolderOps::ExportValidationJson())) return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), UeAgentMaterialFolderOps::GetFolderRootAbsolute());
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetBoolField(TEXT("clean_output_dir"), bCleanOutputDir);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentMaterialFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AssetJson;
	if (!UeAgentMaterialFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError))
	{
		return false;
	}

	FString AssetKind;
	if (!UeAgentMaterialFolderOps::TryGetString(AssetJson, TEXT("asset_kind"), AssetKind) || !AssetKind.Equals(TEXT("material_graph"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("invalid_asset_kind");
		return false;
	}

	FString MaterialSubkind = TEXT("material");
	UeAgentMaterialFolderOps::TryGetString(AssetJson, TEXT("material_subkind"), MaterialSubkind);
	if (!MaterialSubkind.Equals(TEXT("material"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("unsupported_material_subkind");
		return false;
	}

	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);

	int32 SettingsApplied = 0;
	int32 ExpressionsDeleted = 0;
	int32 ExpressionsCreated = 0;
	int32 ExpressionPropertiesApplied = 0;
	int32 ParameterInterfaceApplied = 0;
	int32 EdgesCreated = 0;
	int32 RootInputsConnected = 0;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	auto AddWarning = [&Warnings](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
		WarningObj->SetStringField(TEXT("code"), Code);
		WarningObj->SetStringField(TEXT("message"), Message);
		Warnings.Add(MakeShared<FJsonValueObject>(WarningObj));
	};

	auto InvokeMaterialSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("material_folder_apply");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteMaterialCommand(Command, SubCtx, SubData, SubError);
	};

	UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPath);
	if (!Material)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("material_not_found");
			return false;
		}

		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
		CreateParams->SetBoolField(TEXT("compile_after_create"), false);
		CreateParams->SetBoolField(TEXT("save_after_create"), false);
		TSharedPtr<FJsonObject> CreateData;
		if (!InvokeMaterialSubCommand(TEXT("material_create"), CreateParams, CreateData, OutError))
		{
			return false;
		}
		Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPath);
	}

	if (!Material)
	{
		OutError = TEXT("material_load_after_create_failed");
		return false;
	}

	TSharedPtr<FJsonObject> SettingsJson;
	FString SettingsError;
	if (!UeAgentMaterialFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("material.json")), SettingsJson, SettingsError))
	{
		OutError = SettingsError;
		return false;
	}
	if (SettingsJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (SettingsJson->TryGetArrayField(TEXT("properties"), PropertiesArray) && PropertiesArray)
		{
			for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
			{
				const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
				if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
				{
					AddWarning(TEXT("material_property_invalid_item"), TEXT("Skipped material settings property because it is not a JSON object."));
					continue;
				}

				FString PropertyName;
				FString ValueText;
				if (!UeAgentMaterialFolderOps::TryGetString(*PropertyObjPtr, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
				{
					AddWarning(TEXT("material_property_missing_name"), TEXT("Skipped material settings property because property_name is missing."));
					continue;
				}
				if (!UeAgentMaterialFolderOps::TryGetString(*PropertyObjPtr, TEXT("value_text"), ValueText))
				{
					AddWarning(TEXT("material_property_missing_value_text"), FString::Printf(TEXT("Skipped material settings property '%s' because value_text is missing."), *PropertyName));
					continue;
				}

				TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
				SetParams->SetStringField(TEXT("asset_path"), AssetPath);
				SetParams->SetStringField(TEXT("property_name"), PropertyName);
				SetParams->SetStringField(TEXT("value_text"), ValueText);
				SetParams->SetBoolField(TEXT("compile_after_set"), false);
				SetParams->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> SetData;
				if (!InvokeMaterialSubCommand(TEXT("material_set_property"), SetParams, SetData, OutError))
				{
					return false;
				}
				if (SetData.IsValid() && SetData->HasTypedField<EJson::Boolean>(TEXT("value_text_changed_after_import")) && SetData->GetBoolField(TEXT("value_text_changed_after_import")))
				{
					FString AppliedValueText;
					FString CppType;
					SetData->TryGetStringField(TEXT("applied_value_text"), AppliedValueText);
					SetData->TryGetStringField(TEXT("cpp_type"), CppType);
					AddWarning(
						TEXT("material_property_value_changed_after_import"),
						FString::Printf(TEXT("Material property '%s' read back a different value. requested='%s' applied='%s' cpp_type='%s'."),
							*PropertyName,
							*ValueText,
							*AppliedValueText,
							*CppType));
				}
				++SettingsApplied;
			}
		}
	}

	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("asset_path"), AssetPath);
	TSharedPtr<FJsonObject> ListData;
	if (!InvokeMaterialSubCommand(TEXT("material_list_expressions"), ListParams, ListData, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* CurrentExpressions = nullptr;
	if (ListData->TryGetArrayField(TEXT("expressions"), CurrentExpressions) && CurrentExpressions)
	{
		for (const TSharedPtr<FJsonValue>& CurrentExpressionValue : *CurrentExpressions)
		{
			const TSharedPtr<FJsonObject>* CurrentExpressionObjPtr = nullptr;
			if (!CurrentExpressionValue.IsValid() || !CurrentExpressionValue->TryGetObject(CurrentExpressionObjPtr) || !CurrentExpressionObjPtr || !CurrentExpressionObjPtr->IsValid())
			{
				continue;
			}

			FString ExpressionGuid;
			if (!UeAgentMaterialFolderOps::TryGetString(*CurrentExpressionObjPtr, TEXT("guid"), ExpressionGuid) || ExpressionGuid.IsEmpty())
			{
				continue;
			}

			TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
			DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
			DeleteParams->SetStringField(TEXT("expression_guid"), ExpressionGuid);
			DeleteParams->SetBoolField(TEXT("compile_after_delete"), false);
			DeleteParams->SetBoolField(TEXT("save_after_delete"), false);
			TSharedPtr<FJsonObject> DeleteData;
			if (!InvokeMaterialSubCommand(TEXT("material_delete_expression"), DeleteParams, DeleteData, OutError))
			{
				return false;
			}
			++ExpressionsDeleted;
		}
	}

	TSharedPtr<FJsonObject> GraphJson;
	if (!UeAgentMaterialFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("MaterialGraph.json")), GraphJson, OutError))
	{
		return false;
	}

	TMap<FString, FString> ExpressionIdToGuid;
	const TArray<TSharedPtr<FJsonValue>>* ExpressionsArray = nullptr;
	if (GraphJson->TryGetArrayField(TEXT("expressions"), ExpressionsArray) && ExpressionsArray)
	{
		for (const TSharedPtr<FJsonValue>& ExpressionValue : *ExpressionsArray)
		{
			const TSharedPtr<FJsonObject>* ExpressionObjPtr = nullptr;
			if (!ExpressionValue.IsValid() || !ExpressionValue->TryGetObject(ExpressionObjPtr) || !ExpressionObjPtr || !ExpressionObjPtr->IsValid())
			{
				AddWarning(TEXT("material_expression_invalid_item"), TEXT("Skipped material expression because it is not a JSON object."));
				continue;
			}
			const TSharedPtr<FJsonObject>& ExpressionObj = *ExpressionObjPtr;

			FString ExpressionId;
			FString ExpressionClass;
			if (!UeAgentMaterialFolderOps::TryGetString(ExpressionObj, TEXT("id"), ExpressionId) || ExpressionId.IsEmpty())
			{
				OutError = TEXT("missing_expression_id");
				return false;
			}
			if (!UeAgentMaterialFolderOps::TryGetString(ExpressionObj, TEXT("expression_class"), ExpressionClass) || ExpressionClass.IsEmpty())
			{
				OutError = TEXT("missing_expression_class");
				return false;
			}

			double PosX = 0.0;
			double PosY = 0.0;
			TSharedPtr<FJsonObject> PosObj;
			if (UeAgentMaterialFolderOps::TryGetObjectField(ExpressionObj, TEXT("pos"), PosObj))
			{
				UeAgentMaterialFolderOps::TryGetNumber(PosObj, TEXT("x"), PosX);
				UeAgentMaterialFolderOps::TryGetNumber(PosObj, TEXT("y"), PosY);
			}

			TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
			AddParams->SetStringField(TEXT("asset_path"), AssetPath);
			AddParams->SetStringField(TEXT("expression_class"), ExpressionClass);
			AddParams->SetNumberField(TEXT("node_pos_x"), PosX);
			AddParams->SetNumberField(TEXT("node_pos_y"), PosY);
			AddParams->SetBoolField(TEXT("compile_after_add"), false);
			AddParams->SetBoolField(TEXT("save_after_add"), false);

			TSharedPtr<FJsonObject> AddData;
			if (!InvokeMaterialSubCommand(TEXT("material_add_expression"), AddParams, AddData, OutError))
			{
				return false;
			}

			const TSharedPtr<FJsonObject>* AddedExpressionObjPtr = nullptr;
			if (!AddData->TryGetObjectField(TEXT("expression"), AddedExpressionObjPtr) || !AddedExpressionObjPtr || !AddedExpressionObjPtr->IsValid())
			{
				OutError = TEXT("missing_added_expression");
				return false;
			}

			FString AddedGuid;
			if (!UeAgentMaterialFolderOps::TryGetString(*AddedExpressionObjPtr, TEXT("guid"), AddedGuid) || AddedGuid.IsEmpty())
			{
				OutError = TEXT("missing_added_expression_guid");
				return false;
			}

			ExpressionIdToGuid.Add(ExpressionId, AddedGuid);
			++ExpressionsCreated;
		}

		for (const TSharedPtr<FJsonValue>& ExpressionValue : *ExpressionsArray)
		{
			const TSharedPtr<FJsonObject>* ExpressionObjPtr = nullptr;
			if (!ExpressionValue.IsValid() || !ExpressionValue->TryGetObject(ExpressionObjPtr) || !ExpressionObjPtr || !ExpressionObjPtr->IsValid())
			{
				AddWarning(TEXT("material_expression_invalid_item"), TEXT("Skipped material expression properties because the expression entry is not a JSON object."));
				continue;
			}
			const TSharedPtr<FJsonObject>& ExpressionObj = *ExpressionObjPtr;

			FString ExpressionId;
			if (!UeAgentMaterialFolderOps::TryGetString(ExpressionObj, TEXT("id"), ExpressionId) || ExpressionId.IsEmpty())
			{
				AddWarning(TEXT("material_expression_missing_id"), TEXT("Skipped material expression properties because id is missing."));
				continue;
			}

			const FString* ExpressionGuid = ExpressionIdToGuid.Find(ExpressionId);
			if (!ExpressionGuid)
			{
				AddWarning(TEXT("material_expression_unresolved_id"), FString::Printf(TEXT("Skipped material expression properties because id '%s' was not created."), *ExpressionId));
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
			if (!ExpressionObj->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
			{
				continue;
			}

			bool bRefreshFunctionCall = false;

			for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
			{
				const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
				if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
				{
					AddWarning(TEXT("expression_property_invalid_item"), FString::Printf(TEXT("Skipped expression property on '%s' because it is not a JSON object."), *ExpressionId));
					continue;
				}

				FString PropertyName;
				FString ValueText;
				if (!UeAgentMaterialFolderOps::TryGetString(*PropertyObjPtr, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
				{
					AddWarning(TEXT("expression_property_missing_name"), FString::Printf(TEXT("Skipped expression property on '%s' because property_name is missing."), *ExpressionId));
					continue;
				}
				if (!UeAgentMaterialFolderOps::TryGetString(*PropertyObjPtr, TEXT("value_text"), ValueText))
				{
					AddWarning(TEXT("expression_property_missing_value_text"), FString::Printf(TEXT("Skipped expression property '%s' on '%s' because value_text is missing."), *PropertyName, *ExpressionId));
					continue;
				}

				TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
				SetParams->SetStringField(TEXT("asset_path"), AssetPath);
				SetParams->SetStringField(TEXT("expression_guid"), *ExpressionGuid);
				SetParams->SetStringField(TEXT("property_name"), PropertyName);
				SetParams->SetStringField(TEXT("value_text"), ValueText);
				SetParams->SetBoolField(TEXT("compile_after_set"), false);
				SetParams->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> SetData;
				if (!InvokeMaterialSubCommand(TEXT("material_set_expression_property"), SetParams, SetData, OutError))
				{
					return false;
				}
				if (SetData.IsValid() && SetData->HasTypedField<EJson::Boolean>(TEXT("value_text_changed_after_import")) && SetData->GetBoolField(TEXT("value_text_changed_after_import")))
				{
					FString AppliedValueText;
					FString CppType;
					SetData->TryGetStringField(TEXT("applied_value_text"), AppliedValueText);
					SetData->TryGetStringField(TEXT("cpp_type"), CppType);
					AddWarning(
						TEXT("expression_property_value_changed_after_import"),
						FString::Printf(TEXT("Expression property '%s' on '%s' read back a different value. requested='%s' applied='%s' cpp_type='%s'."),
							*PropertyName,
							*ExpressionId,
							*ValueText,
							*AppliedValueText,
							*CppType));
				}

				if (PropertyName.Equals(TEXT("MaterialFunction"), ESearchCase::IgnoreCase))
				{
					bRefreshFunctionCall = true;
				}

				++ExpressionPropertiesApplied;
			}

			if (bRefreshFunctionCall)
			{
				TSharedPtr<FJsonObject> FindParams = MakeShared<FJsonObject>();
				FindParams->SetStringField(TEXT("expression_guid"), *ExpressionGuid);
				if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(UeAgentMaterialOps::FindExpressionByParams(Material, FindParams)))
				{
					FunctionCall->UpdateFromFunctionResource();
					Material->MarkPackageDirty();
					Material->PostEditChange();
				}
			}
		}
	}

	TSharedPtr<FJsonObject> InterfaceJson;
	FString InterfaceError;
	if (!UeAgentMaterialFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("parameters"), TEXT("interface.json")), InterfaceJson, InterfaceError))
	{
		OutError = InterfaceError;
		return false;
	}
	if (InterfaceJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ParametersArray = nullptr;
		if (InterfaceJson->TryGetArrayField(TEXT("parameters"), ParametersArray) && ParametersArray)
		{
			for (const TSharedPtr<FJsonValue>& ParameterValue : *ParametersArray)
			{
				const TSharedPtr<FJsonObject>* ParameterObjPtr = nullptr;
				if (!ParameterValue.IsValid() || !ParameterValue->TryGetObject(ParameterObjPtr) || !ParameterObjPtr || !ParameterObjPtr->IsValid())
				{
					AddWarning(TEXT("material_parameter_invalid_item"), TEXT("Skipped material parameter interface entry because it is not a JSON object."));
					continue;
				}
				const TSharedPtr<FJsonObject>& ParameterObj = *ParameterObjPtr;

				FString BackingExpressionId;
				if (!UeAgentMaterialFolderOps::TryGetString(ParameterObj, TEXT("backing_expression_id"), BackingExpressionId) || BackingExpressionId.IsEmpty())
				{
					AddWarning(TEXT("material_parameter_missing_backing_expression_id"), TEXT("Skipped material parameter interface entry because backing_expression_id is missing."));
					continue;
				}

				const FString* ExpressionGuid = ExpressionIdToGuid.Find(BackingExpressionId);
				if (!ExpressionGuid)
				{
					OutError = TEXT("parameter_backing_expression_not_found");
					return false;
				}

				auto ApplyExpressionProperty = [&InvokeMaterialSubCommand, &AssetPath, &ExpressionGuid, &OutError, &ParameterInterfaceApplied](const FString& PropertyName, const FString& ValueText) -> bool
				{
					TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
					SetParams->SetStringField(TEXT("asset_path"), AssetPath);
					SetParams->SetStringField(TEXT("expression_guid"), *ExpressionGuid);
					SetParams->SetStringField(TEXT("property_name"), PropertyName);
					SetParams->SetStringField(TEXT("value_text"), ValueText);
					SetParams->SetBoolField(TEXT("compile_after_set"), false);
					SetParams->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> SetData;
					if (!InvokeMaterialSubCommand(TEXT("material_set_expression_property"), SetParams, SetData, OutError))
					{
						return false;
					}
					++ParameterInterfaceApplied;
					return true;
				};

				FString ParameterName;
				if (UeAgentMaterialFolderOps::TryGetString(ParameterObj, TEXT("name"), ParameterName) && !ParameterName.IsEmpty())
				{
					if (!ApplyExpressionProperty(TEXT("ParameterName"), ParameterName))
					{
						return false;
					}
				}

				FString GroupText;
				if (UeAgentMaterialFolderOps::TryGetString(ParameterObj, TEXT("group"), GroupText))
				{
					if (!ApplyExpressionProperty(TEXT("Group"), GroupText))
					{
						return false;
					}
				}

				FString SortPriorityText;
				if (UeAgentMaterialFolderOps::TryGetString(ParameterObj, TEXT("sort_priority"), SortPriorityText) && !SortPriorityText.IsEmpty())
				{
					if (!ApplyExpressionProperty(TEXT("SortPriority"), SortPriorityText))
					{
						return false;
					}
				}

				FString DefaultPropertyName;
				FString DefaultValueText;
				if (UeAgentMaterialFolderOps::TryGetString(ParameterObj, TEXT("default_property_name"), DefaultPropertyName)
					&& !DefaultPropertyName.IsEmpty()
					&& UeAgentMaterialFolderOps::TryGetString(ParameterObj, TEXT("default_value_text"), DefaultValueText))
				{
					if (!ApplyExpressionProperty(DefaultPropertyName, DefaultValueText))
					{
						return false;
					}
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* EdgesArray = nullptr;
	if (GraphJson->TryGetArrayField(TEXT("edges"), EdgesArray) && EdgesArray)
	{
		for (const TSharedPtr<FJsonValue>& EdgeValue : *EdgesArray)
		{
			const TSharedPtr<FJsonObject>* EdgeObjPtr = nullptr;
			if (!EdgeValue.IsValid() || !EdgeValue->TryGetObject(EdgeObjPtr) || !EdgeObjPtr || !EdgeObjPtr->IsValid())
			{
				continue;
			}

			TSharedPtr<FJsonObject> FromObj;
			TSharedPtr<FJsonObject> ToObj;
			if (!UeAgentMaterialFolderOps::TryGetObjectField(*EdgeObjPtr, TEXT("from"), FromObj)
				|| !UeAgentMaterialFolderOps::TryGetObjectField(*EdgeObjPtr, TEXT("to"), ToObj))
			{
				OutError = TEXT("invalid_edge_object");
				return false;
			}

			FString FromNodeId;
			FString ToNodeId;
			if (!UeAgentMaterialFolderOps::TryGetString(FromObj, TEXT("node"), FromNodeId) || FromNodeId.IsEmpty()
				|| !UeAgentMaterialFolderOps::TryGetString(ToObj, TEXT("node"), ToNodeId) || ToNodeId.IsEmpty())
			{
				OutError = TEXT("missing_edge_node");
				return false;
			}

			const FString* FromGuid = ExpressionIdToGuid.Find(FromNodeId);
			const FString* ToGuid = ExpressionIdToGuid.Find(ToNodeId);
			if (!FromGuid || !ToGuid)
			{
				OutError = TEXT("edge_expression_not_found");
				return false;
			}

			FString OutputName;
			FString InputName;
			UeAgentMaterialFolderOps::TryGetString(FromObj, TEXT("output"), OutputName);
			UeAgentMaterialFolderOps::TryGetString(ToObj, TEXT("input"), InputName);

			TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
			ConnectParams->SetStringField(TEXT("asset_path"), AssetPath);
			ConnectParams->SetStringField(TEXT("from_expression_guid"), *FromGuid);
			ConnectParams->SetStringField(TEXT("to_expression_guid"), *ToGuid);
			if (!OutputName.IsEmpty())
			{
				ConnectParams->SetStringField(TEXT("from_output_name"), OutputName);
			}
			if (!InputName.IsEmpty())
			{
				ConnectParams->SetStringField(TEXT("to_input_name"), InputName);
			}
			ConnectParams->SetBoolField(TEXT("compile_after_connect"), false);
			ConnectParams->SetBoolField(TEXT("save_after_connect"), false);
			TSharedPtr<FJsonObject> ConnectData;
			if (!InvokeMaterialSubCommand(TEXT("material_connect_expressions"), ConnectParams, ConnectData, OutError))
			{
				return false;
			}
			++EdgesCreated;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RootInputsArray = nullptr;
	if (GraphJson->TryGetArrayField(TEXT("root_inputs"), RootInputsArray) && RootInputsArray)
	{
		for (const TSharedPtr<FJsonValue>& RootInputValue : *RootInputsArray)
		{
			const TSharedPtr<FJsonObject>* RootInputObjPtr = nullptr;
			if (!RootInputValue.IsValid() || !RootInputValue->TryGetObject(RootInputObjPtr) || !RootInputObjPtr || !RootInputObjPtr->IsValid())
			{
				continue;
			}

			FString MaterialPropertyName;
			if (!UeAgentMaterialFolderOps::TryGetString(*RootInputObjPtr, TEXT("material_property"), MaterialPropertyName) || MaterialPropertyName.IsEmpty())
			{
				OutError = TEXT("missing_material_property");
				return false;
			}

			TSharedPtr<FJsonObject> FromObj;
			if (!UeAgentMaterialFolderOps::TryGetObjectField(*RootInputObjPtr, TEXT("from"), FromObj))
			{
				OutError = TEXT("missing_root_input_from");
				return false;
			}

			FString FromNodeId;
			if (!UeAgentMaterialFolderOps::TryGetString(FromObj, TEXT("node"), FromNodeId) || FromNodeId.IsEmpty())
			{
				OutError = TEXT("missing_root_input_node");
				return false;
			}

			const FString* FromGuid = ExpressionIdToGuid.Find(FromNodeId);
			if (!FromGuid)
			{
				OutError = TEXT("root_input_expression_not_found");
				return false;
			}

			FString OutputName;
			UeAgentMaterialFolderOps::TryGetString(FromObj, TEXT("output"), OutputName);

			TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
			ConnectParams->SetStringField(TEXT("asset_path"), AssetPath);
			ConnectParams->SetStringField(TEXT("expression_guid"), *FromGuid);
			ConnectParams->SetStringField(TEXT("material_property"), MaterialPropertyName);
			if (!OutputName.IsEmpty())
			{
				ConnectParams->SetStringField(TEXT("output_name"), OutputName);
			}
			ConnectParams->SetBoolField(TEXT("compile_after_connect"), false);
			ConnectParams->SetBoolField(TEXT("save_after_connect"), false);
			TSharedPtr<FJsonObject> ConnectData;
			if (!InvokeMaterialSubCommand(TEXT("material_connect_expression_to_property"), ConnectParams, ConnectData, OutError))
			{
				return false;
			}
			++RootInputsConnected;
		}
	}

	if (bCompileAfterApply)
	{
		TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
		CompileParams->SetStringField(TEXT("asset_path"), AssetPath);
		CompileParams->SetBoolField(TEXT("include_messages"), true);
		CompileParams->SetNumberField(TEXT("max_messages"), 64);
		CompileParams->SetBoolField(TEXT("save_after_compile"), bSaveAfterApply);
		TSharedPtr<FJsonObject> CompileData;
		if (!InvokeMaterialSubCommand(TEXT("material_compile"), CompileParams, CompileData, OutError))
		{
			return false;
		}
		OutData->SetObjectField(TEXT("compile"), CompileData);
	}
	else if (bSaveAfterApply)
	{
		FString SaveError;
		if (!UeAgentMaterialOps::SaveAssetPackage(Material, SaveError))
		{
			OutError = SaveError;
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("settings_applied"), SettingsApplied);
	OutData->SetNumberField(TEXT("expressions_deleted"), ExpressionsDeleted);
	OutData->SetNumberField(TEXT("expressions_created"), ExpressionsCreated);
	OutData->SetNumberField(TEXT("expression_properties_applied"), ExpressionPropertiesApplied);
	OutData->SetNumberField(TEXT("parameter_interface_applied"), ParameterInterfaceApplied);
	OutData->SetNumberField(TEXT("edges_created"), EdgesCreated);
	OutData->SetNumberField(TEXT("root_inputs_connected"), RootInputsConnected);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	return true;
}

namespace UeAgentMaterialInstanceFolderOps
{
	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("MaterialInstance")));
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		FString RelativeFolder = AssetPath;
		while (RelativeFolder.StartsWith(TEXT("/")))
		{
			RelativeFolder.RightChopInline(1, EAllowShrinking::No);
		}
		OutFolderPath = FPaths::Combine(GetFolderRootAbsolute(), RelativeFolder);
		return true;
	}

	static TSharedPtr<FJsonObject> ExportAssetJson(UMaterialInstanceConstant* MaterialInstance)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		const FString AssetPath = MaterialInstance ? MaterialInstance->GetOutermost()->GetName() : FString();
		Root->SetNumberField(TEXT("format_version"), 1);
		Root->SetStringField(TEXT("asset_kind"), TEXT("material_instance"));
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
		Root->SetNumberField(TEXT("profile_version"), 1);
		Root->SetStringField(TEXT("instance_subkind"), TEXT("material_instance_constant"));
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportParentJson(UMaterialInstanceConstant* MaterialInstance)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(
			TEXT("parent_material_path"),
			(MaterialInstance && MaterialInstance->Parent) ? MaterialInstance->Parent->GetPathName() : FString());
		return Root;
	}

	static TSharedPtr<FJsonObject> MakeScalarParameterObject(const FScalarParameterValue& Parameter)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
		ParamObj->SetNumberField(TEXT("value"), Parameter.ParameterValue);
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> MakeVectorParameterObject(const FVectorParameterValue& Parameter)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("parameter_type"), TEXT("vector"));
		TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
		ValueObj->SetNumberField(TEXT("r"), Parameter.ParameterValue.R);
		ValueObj->SetNumberField(TEXT("g"), Parameter.ParameterValue.G);
		ValueObj->SetNumberField(TEXT("b"), Parameter.ParameterValue.B);
		ValueObj->SetNumberField(TEXT("a"), Parameter.ParameterValue.A);
		ParamObj->SetObjectField(TEXT("value"), ValueObj);
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> MakeTextureParameterObject(const FTextureParameterValue& Parameter)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("parameter_type"), TEXT("texture"));
		ParamObj->SetStringField(TEXT("texture_path"), Parameter.ParameterValue ? Parameter.ParameterValue->GetPathName() : FString());
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> MakeStaticSwitchParameterObject(const FStaticSwitchParameter& Parameter)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("parameter_type"), TEXT("static_switch"));
		ParamObj->SetBoolField(TEXT("value"), Parameter.Value);
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> MakeStaticComponentMaskParameterObject(const FStaticComponentMaskParameter& Parameter)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("parameter_type"), TEXT("static_component_mask"));
		TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
		ValueObj->SetBoolField(TEXT("r"), Parameter.R);
		ValueObj->SetBoolField(TEXT("g"), Parameter.G);
		ValueObj->SetBoolField(TEXT("b"), Parameter.B);
		ValueObj->SetBoolField(TEXT("a"), Parameter.A);
		ParamObj->SetObjectField(TEXT("value"), ValueObj);
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> ExportOverridesJson(UMaterialInstanceConstant* MaterialInstance)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ParametersArray;
		if (!MaterialInstance)
		{
			Root->SetArrayField(TEXT("parameters"), ParametersArray);
			return Root;
		}

		for (const FScalarParameterValue& Parameter : MaterialInstance->ScalarParameterValues)
		{
			ParametersArray.Add(MakeShared<FJsonValueObject>(MakeScalarParameterObject(Parameter)));
		}

		for (const FVectorParameterValue& Parameter : MaterialInstance->VectorParameterValues)
		{
			ParametersArray.Add(MakeShared<FJsonValueObject>(MakeVectorParameterObject(Parameter)));
		}

		for (const FTextureParameterValue& Parameter : MaterialInstance->TextureParameterValues)
		{
			ParametersArray.Add(MakeShared<FJsonValueObject>(MakeTextureParameterObject(Parameter)));
		}

		const FStaticParameterSet StaticParameters = MaterialInstance->GetStaticParameters();
		for (const FStaticSwitchParameter& Parameter : StaticParameters.StaticSwitchParameters)
		{
			if (!Parameter.bOverride)
			{
				continue;
			}
			ParametersArray.Add(MakeShared<FJsonValueObject>(MakeStaticSwitchParameterObject(Parameter)));
		}

		for (const FStaticComponentMaskParameter& Parameter : StaticParameters.EditorOnly.StaticComponentMaskParameters)
		{
			if (!Parameter.bOverride)
			{
				continue;
			}
			ParametersArray.Add(MakeShared<FJsonValueObject>(MakeStaticComponentMaskParameterObject(Parameter)));
		}

		Root->SetArrayField(TEXT("parameters"), ParametersArray);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportValidationJson()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Checks;

		TSharedPtr<FJsonObject> CompileCheck = MakeShared<FJsonObject>();
		CompileCheck->SetStringField(TEXT("kind"), TEXT("compile_success"));
		Checks.Add(MakeShared<FJsonValueObject>(CompileCheck));

		TSharedPtr<FJsonObject> ParentCheck = MakeShared<FJsonObject>();
		ParentCheck->SetStringField(TEXT("kind"), TEXT("parent_exists"));
		Checks.Add(MakeShared<FJsonValueObject>(ParentCheck));

		Root->SetArrayField(TEXT("checks"), Checks);
		return Root;
	}
}

bool FUeAgentHttpServer::CmdMaterialInstanceExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UMaterialInstanceConstant* MaterialInstance = UeAgentMaterialOps::LoadMaterialInstance(AssetPathInput);
	if (!MaterialInstance)
	{
		OutError = TEXT("material_instance_not_found");
		return false;
	}

	const FString AssetPath = MaterialInstance->GetOutermost()->GetName();
	FString FolderPath;
	if (!UeAgentMaterialInstanceFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	bool bIncludeValidation = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_validation"), bIncludeValidation);

	if (bCleanOutputDir)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}

	const FString ParametersDir = FPaths::Combine(FolderPath, TEXT("parameters"));
	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	IFileManager::Get().MakeDirectory(*ParametersDir, true);
	if (bIncludeValidation)
	{
		IFileManager::Get().MakeDirectory(*ValidationDir, true);
	}

	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		if (!UeAgentMaterialFolderOps::SaveJsonFile(FilePath, Obj))
		{
			OutError = TEXT("save_export_json_failed");
			return false;
		}
		++FileCount;
		return true;
	};

	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), UeAgentMaterialInstanceFolderOps::ExportAssetJson(MaterialInstance))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("parent.json")), UeAgentMaterialInstanceFolderOps::ExportParentJson(MaterialInstance))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ParametersDir, TEXT("overrides.json")), UeAgentMaterialInstanceFolderOps::ExportOverridesJson(MaterialInstance))) return false;
	if (bIncludeValidation)
	{
		if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("checks.json")), UeAgentMaterialInstanceFolderOps::ExportValidationJson())) return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), UeAgentMaterialInstanceFolderOps::GetFolderRootAbsolute());
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetBoolField(TEXT("clean_output_dir"), bCleanOutputDir);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialInstanceApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentMaterialInstanceFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AssetJson;
	if (!UeAgentMaterialFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError))
	{
		return false;
	}

	FString AssetKind;
	if (!UeAgentMaterialFolderOps::TryGetString(AssetJson, TEXT("asset_kind"), AssetKind) || !AssetKind.Equals(TEXT("material_instance"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("invalid_asset_kind");
		return false;
	}

	FString InstanceSubkind = TEXT("material_instance_constant");
	UeAgentMaterialFolderOps::TryGetString(AssetJson, TEXT("instance_subkind"), InstanceSubkind);
	if (!InstanceSubkind.Equals(TEXT("material_instance_constant"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("unsupported_instance_subkind");
		return false;
	}

	TSharedPtr<FJsonObject> ParentJson;
	if (!UeAgentMaterialFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("parent.json")), ParentJson, OutError))
	{
		return false;
	}

	FString ParentMaterialPath;
	if (!UeAgentMaterialFolderOps::TryGetString(ParentJson, TEXT("parent_material_path"), ParentMaterialPath) || ParentMaterialPath.IsEmpty())
	{
		OutError = TEXT("missing_parent_material_path");
		return false;
	}

	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bClearExistingOverrides = true;
	JsonTryGetBool(Ctx.Params, TEXT("clear_existing_overrides"), bClearExistingOverrides);

	int32 ParentApplied = 0;
	int32 OverridesCleared = 0;
	int32 ParametersApplied = 0;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	auto AddWarning = [&Warnings](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
		WarningObj->SetStringField(TEXT("code"), Code);
		WarningObj->SetStringField(TEXT("message"), Message);
		Warnings.Add(MakeShared<FJsonValueObject>(WarningObj));
	};

	auto InvokeMaterialSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("material_instance_folder_apply");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteMaterialCommand(Command, SubCtx, SubData, SubError);
	};

	UMaterialInstanceConstant* MaterialInstance = UeAgentMaterialOps::LoadMaterialInstance(AssetPath);
	if (!MaterialInstance)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("material_instance_not_found");
			return false;
		}

		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
		CreateParams->SetStringField(TEXT("parent_material_path"), ParentMaterialPath);
		CreateParams->SetBoolField(TEXT("save_after_create"), false);
		TSharedPtr<FJsonObject> CreateData;
		if (!InvokeMaterialSubCommand(TEXT("material_instance_create"), CreateParams, CreateData, OutError))
		{
			return false;
		}
		MaterialInstance = UeAgentMaterialOps::LoadMaterialInstance(AssetPath);
	}

	if (!MaterialInstance)
	{
		OutError = TEXT("material_instance_load_after_create_failed");
		return false;
	}

	{
		TSharedPtr<FJsonObject> ParentParams = MakeShared<FJsonObject>();
		ParentParams->SetStringField(TEXT("asset_path"), AssetPath);
		ParentParams->SetStringField(TEXT("parent_material_path"), ParentMaterialPath);
		TSharedPtr<FJsonObject> ParentData;
		if (!InvokeMaterialSubCommand(TEXT("material_set_instance_parent"), ParentParams, ParentData, OutError))
		{
			return false;
		}
		++ParentApplied;
	}

	if (bClearExistingOverrides)
	{
		MaterialInstance->ClearParameterValuesEditorOnly();
		UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
		MaterialInstance->MarkPackageDirty();
		MaterialInstance->PostEditChange();
		OverridesCleared = 1;
	}

	TSharedPtr<FJsonObject> OverridesJson;
	FString OverridesError;
	if (!UeAgentMaterialFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("parameters"), TEXT("overrides.json")), OverridesJson, OverridesError))
	{
		OutError = OverridesError;
		return false;
	}
	if (OverridesJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ParametersArray = nullptr;
		if (OverridesJson->TryGetArrayField(TEXT("parameters"), ParametersArray) && ParametersArray)
		{
			for (const TSharedPtr<FJsonValue>& ParameterValue : *ParametersArray)
			{
				const TSharedPtr<FJsonObject>* ParameterObjPtr = nullptr;
				if (!ParameterValue.IsValid() || !ParameterValue->TryGetObject(ParameterObjPtr) || !ParameterObjPtr || !ParameterObjPtr->IsValid())
				{
					AddWarning(TEXT("material_instance_parameter_invalid_item"), TEXT("Skipped material instance parameter override because it is not a JSON object."));
					continue;
				}
				const TSharedPtr<FJsonObject>& ParameterObj = *ParameterObjPtr;

				FString ParameterName;
				FString ParameterType;
				if (!UeAgentMaterialFolderOps::TryGetString(ParameterObj, TEXT("name"), ParameterName) || ParameterName.IsEmpty())
				{
					AddWarning(TEXT("material_instance_parameter_missing_name"), TEXT("Skipped material instance parameter override because name is missing."));
					continue;
				}
				if (!UeAgentMaterialFolderOps::TryGetString(ParameterObj, TEXT("parameter_type"), ParameterType) || ParameterType.IsEmpty())
				{
					AddWarning(TEXT("material_instance_parameter_missing_type"), FString::Printf(TEXT("Skipped material instance parameter '%s' because parameter_type is missing."), *ParameterName));
					continue;
				}

				TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
				SetParams->SetStringField(TEXT("asset_path"), AssetPath);
				SetParams->SetStringField(TEXT("parameter_type"), ParameterType);
				SetParams->SetStringField(TEXT("parameter_name"), ParameterName);

				if (ParameterType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
				{
					double ScalarValue = 0.0;
					if (!UeAgentMaterialFolderOps::TryGetNumber(ParameterObj, TEXT("value"), ScalarValue))
					{
						OutError = TEXT("missing_scalar_value");
						return false;
					}
					SetParams->SetNumberField(TEXT("value"), ScalarValue);
				}
				else if (ParameterType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
				{
					TSharedPtr<FJsonObject> ValueObj;
					if (!UeAgentMaterialFolderOps::TryGetObjectField(ParameterObj, TEXT("value"), ValueObj))
					{
						OutError = TEXT("missing_vector_value");
						return false;
					}
					SetParams->SetObjectField(TEXT("value"), ValueObj);
				}
				else if (ParameterType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
				{
					FString TexturePath;
					if (!UeAgentMaterialFolderOps::TryGetString(ParameterObj, TEXT("texture_path"), TexturePath))
					{
						OutError = TEXT("missing_texture_path");
						return false;
					}
					SetParams->SetStringField(TEXT("texture_path"), TexturePath);
				}
				else if (ParameterType.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
				{
					bool bValue = false;
					if (!UeAgentMaterialFolderOps::TryGetBool(ParameterObj, TEXT("value"), bValue))
					{
						OutError = TEXT("missing_static_switch_value");
						return false;
					}
					SetParams->SetBoolField(TEXT("value"), bValue);
				}
				else if (ParameterType.Equals(TEXT("static_component_mask"), ESearchCase::IgnoreCase))
				{
					TSharedPtr<FJsonObject> ValueObj;
					if (!UeAgentMaterialFolderOps::TryGetObjectField(ParameterObj, TEXT("value"), ValueObj))
					{
						OutError = TEXT("missing_static_component_mask_value");
						return false;
					}
					SetParams->SetObjectField(TEXT("value"), ValueObj);
				}
				else
				{
					continue;
				}

				TSharedPtr<FJsonObject> SetData;
				if (!InvokeMaterialSubCommand(TEXT("material_set_parameter"), SetParams, SetData, OutError))
				{
					return false;
				}
				++ParametersApplied;
			}
		}
	}

	if (bCompileAfterApply)
	{
		TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
		CompileParams->SetStringField(TEXT("asset_path"), AssetPath);
		CompileParams->SetBoolField(TEXT("include_messages"), true);
		CompileParams->SetNumberField(TEXT("max_messages"), 64);
		CompileParams->SetBoolField(TEXT("save_after_compile"), bSaveAfterApply);
		TSharedPtr<FJsonObject> CompileData;
		if (!InvokeMaterialSubCommand(TEXT("material_compile"), CompileParams, CompileData, OutError))
		{
			return false;
		}
		OutData->SetObjectField(TEXT("compile"), CompileData);
	}
	else if (bSaveAfterApply)
	{
		FString SaveError;
		if (!UeAgentMaterialOps::SaveAssetPackage(MaterialInstance, SaveError))
		{
			OutError = SaveError;
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("parent_applied"), ParentApplied);
	OutData->SetNumberField(TEXT("overrides_cleared"), OverridesCleared);
	OutData->SetNumberField(TEXT("parameters_applied"), ParametersApplied);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	return true;
}

namespace UeAgentMaterialFunctionFolderOps
{
	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("MaterialFunction")));
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		FString RelativeFolder = AssetPath;
		while (RelativeFolder.StartsWith(TEXT("/")))
		{
			RelativeFolder.RightChopInline(1, EAllowShrinking::No);
		}
		OutFolderPath = FPaths::Combine(GetFolderRootAbsolute(), RelativeFolder);
		return true;
	}

	static TSharedPtr<FJsonObject> ExportAssetJson(UMaterialFunction* MaterialFunction)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		const FString AssetPath = MaterialFunction ? MaterialFunction->GetOutermost()->GetName() : FString();
		Root->SetNumberField(TEXT("format_version"), 1);
		Root->SetStringField(TEXT("asset_kind"), TEXT("material_function"));
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
		Root->SetNumberField(TEXT("profile_version"), 1);
		Root->SetStringField(TEXT("function_subkind"), TEXT("material_function"));
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportFunctionJson(UMaterialFunction* MaterialFunction)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		if (!MaterialFunction)
		{
			return Root;
		}

		Root->SetStringField(TEXT("description"), MaterialFunction->Description);
		Root->SetBoolField(TEXT("expose_to_library"), MaterialFunction->bExposeToLibrary);
		return Root;
	}

	static void BuildExpressionIdMap(
		UMaterialFunction* MaterialFunction,
		TArray<UMaterialExpression*>& OutSortedExpressions,
		TMap<const UMaterialExpression*, FString>& OutExpressionToId)
	{
		OutSortedExpressions.Reset();
		OutExpressionToId.Reset();

		if (!MaterialFunction)
		{
			return;
		}

		for (UMaterialExpression* Expression : MaterialFunction->GetExpressions())
		{
			if (Expression)
			{
				OutSortedExpressions.Add(Expression);
			}
		}

		OutSortedExpressions.Sort([](const UMaterialExpression& A, const UMaterialExpression& B)
		{
			if (A.MaterialExpressionEditorX != B.MaterialExpressionEditorX)
			{
				return A.MaterialExpressionEditorX < B.MaterialExpressionEditorX;
			}
			if (A.MaterialExpressionEditorY != B.MaterialExpressionEditorY)
			{
				return A.MaterialExpressionEditorY < B.MaterialExpressionEditorY;
			}
			return A.GetName().Compare(B.GetName(), ESearchCase::IgnoreCase) < 0;
		});

		TMap<FString, int32> IdUsage;
		for (UMaterialExpression* Expression : OutSortedExpressions)
		{
			FString BaseId = UeAgentMaterialFolderOps::MakeBaseExpressionId(Expression);
			int32& Counter = IdUsage.FindOrAdd(BaseId);
			const FString FinalId = (Counter++ == 0) ? BaseId : FString::Printf(TEXT("%s_%d"), *BaseId, Counter);
			OutExpressionToId.Add(Expression, FinalId);
		}
	}

	static TSharedPtr<FJsonObject> ExportGraphJson(UMaterialFunction* MaterialFunction)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("name"), TEXT("MaterialFunctionGraph"));
		GraphObj->SetStringField(TEXT("graph_kind"), TEXT("material_function_graph"));
		Root->SetObjectField(TEXT("graph"), GraphObj);

		TArray<UMaterialExpression*> SortedExpressions;
		TMap<const UMaterialExpression*, FString> ExpressionToId;
		BuildExpressionIdMap(MaterialFunction, SortedExpressions, ExpressionToId);
		TArray<TSharedPtr<FJsonValue>> ExpressionsArray;

		for (UMaterialExpression* Expression : SortedExpressions)
		{
			const FString* FinalIdPtr = ExpressionToId.Find(Expression);
			if (!FinalIdPtr)
			{
				continue;
			}
			const FString& FinalId = *FinalIdPtr;

			TSharedPtr<FJsonObject> ExprObj = MakeShared<FJsonObject>();
			ExprObj->SetStringField(TEXT("id"), FinalId);
			ExprObj->SetStringField(TEXT("ue_expression_guid"), Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			ExprObj->SetStringField(TEXT("expression_name"), Expression->GetName());
			ExprObj->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetPathName());
			if (!Expression->Desc.IsEmpty())
			{
				ExprObj->SetStringField(TEXT("desc"), Expression->Desc);
			}

			TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
			PosObj->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
			PosObj->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
			ExprObj->SetObjectField(TEXT("pos"), PosObj);
			ExprObj->SetArrayField(TEXT("properties"), UeAgentMaterialFolderOps::ExportExpressionProperties(Expression));
			ExpressionsArray.Add(MakeShared<FJsonValueObject>(ExprObj));
		}

		TArray<TSharedPtr<FJsonValue>> EdgesArray;
		for (UMaterialExpression* Expression : SortedExpressions)
		{
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				if (!It->Expression)
				{
					continue;
				}

				const FString* FromId = ExpressionToId.Find(It->Expression);
				const FString* ToId = ExpressionToId.Find(Expression);
				if (!FromId || !ToId)
				{
					continue;
				}

				FString OutputName;
				const TArray<FExpressionOutput>& Outputs = It->Expression->GetOutputs();
				if (Outputs.IsValidIndex(It->OutputIndex))
				{
					OutputName = Outputs[It->OutputIndex].OutputName.ToString();
				}

				FString InputName;
				if (const UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
				{
					InputName = FunctionCall->GetInputNameWithType(It.Index, false).ToString();
				}
				else
				{
					InputName = Expression->GetInputName(It.Index).ToString();
				}
				if (InputName.IsEmpty())
				{
					InputName = It->InputName.ToString();
				}

				TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
				TSharedPtr<FJsonObject> FromObj = MakeShared<FJsonObject>();
				FromObj->SetStringField(TEXT("node"), *FromId);
				FromObj->SetStringField(TEXT("output"), OutputName);
				TSharedPtr<FJsonObject> ToObj = MakeShared<FJsonObject>();
				ToObj->SetStringField(TEXT("node"), *ToId);
				ToObj->SetStringField(TEXT("input"), InputName);
				EdgeObj->SetObjectField(TEXT("from"), FromObj);
				EdgeObj->SetObjectField(TEXT("to"), ToObj);
				EdgesArray.Add(MakeShared<FJsonValueObject>(EdgeObj));
			}
		}

		Root->SetArrayField(TEXT("expressions"), ExpressionsArray);
		Root->SetArrayField(TEXT("edges"), EdgesArray);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportValidationJson()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Checks;

		TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("kind"), TEXT("graph_roundtrip"));
		Checks.Add(MakeShared<FJsonValueObject>(Check));

		Root->SetArrayField(TEXT("checks"), Checks);
		return Root;
	}
}

bool FUeAgentHttpServer::CmdMaterialFunctionExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UMaterialFunction* MaterialFunction = UeAgentMaterialOps::LoadMaterialFunctionAsset(AssetPathInput);
	if (!MaterialFunction)
	{
		OutError = TEXT("material_function_not_found");
		return false;
	}

	const FString AssetPath = MaterialFunction->GetOutermost()->GetName();
	FString FolderPath;
	if (!UeAgentMaterialFunctionFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	bool bIncludeValidation = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_validation"), bIncludeValidation);

	if (bCleanOutputDir)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}

	const FString GraphsDir = FPaths::Combine(FolderPath, TEXT("graphs"));
	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	IFileManager::Get().MakeDirectory(*GraphsDir, true);
	if (bIncludeValidation)
	{
		IFileManager::Get().MakeDirectory(*ValidationDir, true);
	}

	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		if (!UeAgentMaterialFolderOps::SaveJsonFile(FilePath, Obj))
		{
			OutError = TEXT("save_export_json_failed");
			return false;
		}
		++FileCount;
		return true;
	};

	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), UeAgentMaterialFunctionFolderOps::ExportAssetJson(MaterialFunction))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("function.json")), UeAgentMaterialFunctionFolderOps::ExportFunctionJson(MaterialFunction))) return false;
	if (!SaveRequiredJson(FPaths::Combine(GraphsDir, TEXT("MaterialFunctionGraph.json")), UeAgentMaterialFunctionFolderOps::ExportGraphJson(MaterialFunction))) return false;
	if (bIncludeValidation)
	{
		if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("checks.json")), UeAgentMaterialFunctionFolderOps::ExportValidationJson())) return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), UeAgentMaterialFunctionFolderOps::GetFolderRootAbsolute());
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetBoolField(TEXT("clean_output_dir"), bCleanOutputDir);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialFunctionApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentMaterialFunctionFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AssetJson;
	if (!UeAgentMaterialFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError))
	{
		return false;
	}

	FString AssetKind;
	if (!UeAgentMaterialFolderOps::TryGetString(AssetJson, TEXT("asset_kind"), AssetKind) || !AssetKind.Equals(TEXT("material_function"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("invalid_asset_kind");
		return false;
	}

	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bUpdateAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("update_after_apply"), bUpdateAfterApply);
	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);

	int32 ExpressionsDeleted = 0;
	int32 ExpressionsCreated = 0;
	int32 ExpressionPropertiesApplied = 0;
	int32 EdgesCreated = 0;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	auto AddWarning = [&Warnings](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
		WarningObj->SetStringField(TEXT("code"), Code);
		WarningObj->SetStringField(TEXT("message"), Message);
		Warnings.Add(MakeShared<FJsonValueObject>(WarningObj));
	};

	UMaterialFunction* MaterialFunction = UeAgentMaterialOps::LoadMaterialFunctionAsset(AssetPath);
	if (!MaterialFunction)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("material_function_not_found");
			return false;
		}

		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return false;
		}

		MaterialFunction = NewObject<UMaterialFunction>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!MaterialFunction)
		{
			OutError = TEXT("create_material_function_failed");
			return false;
		}

		FAssetRegistryModule::AssetCreated(MaterialFunction);
		MaterialFunction->MarkPackageDirty();
	}

	if (!MaterialFunction)
	{
		OutError = TEXT("material_function_load_after_create_failed");
		return false;
	}

	TSharedPtr<FJsonObject> FunctionJson;
	FString FunctionError;
	if (!UeAgentMaterialFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("function.json")), FunctionJson, FunctionError))
	{
		OutError = FunctionError;
		return false;
	}
	if (FunctionJson.IsValid())
	{
		FString Description;
		if (UeAgentMaterialFolderOps::TryGetString(FunctionJson, TEXT("description"), Description))
		{
			MaterialFunction->Description = Description;
		}

		bool bExposeToLibrary = false;
		if (UeAgentMaterialFolderOps::TryGetBool(FunctionJson, TEXT("expose_to_library"), bExposeToLibrary))
		{
			MaterialFunction->bExposeToLibrary = bExposeToLibrary;
		}
	}

	TSharedPtr<FJsonObject> GraphJson;
	if (!UeAgentMaterialFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("MaterialFunctionGraph.json")), GraphJson, OutError))
	{
		return false;
	}

	UMaterialEditingLibrary::DeleteAllMaterialExpressionsInFunction(MaterialFunction);
	ExpressionsDeleted = 1;

	const TArray<TSharedPtr<FJsonValue>>* ExpressionsArray = nullptr;
	if (!GraphJson->TryGetArrayField(TEXT("expressions"), ExpressionsArray) || !ExpressionsArray)
	{
		OutError = TEXT("missing_expressions_array");
		return false;
	}

	TMap<FString, UMaterialExpression*> ExpressionIdToExpression;

	for (const TSharedPtr<FJsonValue>& ExpressionValue : *ExpressionsArray)
	{
		const TSharedPtr<FJsonObject>* ExpressionObjPtr = nullptr;
		if (!ExpressionValue.IsValid() || !ExpressionValue->TryGetObject(ExpressionObjPtr) || !ExpressionObjPtr || !ExpressionObjPtr->IsValid())
		{
			AddWarning(TEXT("material_function_expression_invalid_item"), TEXT("Skipped material function expression because it is not a JSON object."));
			continue;
		}
		const TSharedPtr<FJsonObject>& ExpressionObj = *ExpressionObjPtr;

		FString ExpressionId;
		FString ExpressionClass;
		if (!UeAgentMaterialFolderOps::TryGetString(ExpressionObj, TEXT("id"), ExpressionId) || ExpressionId.IsEmpty())
		{
			OutError = TEXT("missing_expression_id");
			return false;
		}
		if (!UeAgentMaterialFolderOps::TryGetString(ExpressionObj, TEXT("expression_class"), ExpressionClass) || ExpressionClass.IsEmpty())
		{
			OutError = TEXT("missing_expression_class");
			return false;
		}

		UClass* ExpressionUClass = UeAgentMaterialOps::ResolveClassByPath(ExpressionClass);
		if (!ExpressionUClass || !ExpressionUClass->IsChildOf(UMaterialExpression::StaticClass()))
		{
			OutError = TEXT("invalid_expression_class");
			return false;
		}

		double PosX = 0.0;
		double PosY = 0.0;
		TSharedPtr<FJsonObject> PosObj;
		if (UeAgentMaterialFolderOps::TryGetObjectField(ExpressionObj, TEXT("pos"), PosObj))
		{
			UeAgentMaterialFolderOps::TryGetNumber(PosObj, TEXT("x"), PosX);
			UeAgentMaterialFolderOps::TryGetNumber(PosObj, TEXT("y"), PosY);
		}

		UMaterialExpression* NewExpression = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(MaterialFunction, ExpressionUClass, static_cast<int32>(PosX), static_cast<int32>(PosY));
		if (!NewExpression)
		{
			OutError = TEXT("create_function_expression_failed");
			return false;
		}

		ExpressionIdToExpression.Add(ExpressionId, NewExpression);
		++ExpressionsCreated;
	}

	for (const TSharedPtr<FJsonValue>& ExpressionValue : *ExpressionsArray)
	{
		const TSharedPtr<FJsonObject>* ExpressionObjPtr = nullptr;
		if (!ExpressionValue.IsValid() || !ExpressionValue->TryGetObject(ExpressionObjPtr) || !ExpressionObjPtr || !ExpressionObjPtr->IsValid())
		{
			AddWarning(TEXT("material_function_expression_invalid_item"), TEXT("Skipped material function expression properties because the expression entry is not a JSON object."));
			continue;
		}
		const TSharedPtr<FJsonObject>& ExpressionObj = *ExpressionObjPtr;

		FString ExpressionId;
		if (!UeAgentMaterialFolderOps::TryGetString(ExpressionObj, TEXT("id"), ExpressionId) || ExpressionId.IsEmpty())
		{
			AddWarning(TEXT("material_function_expression_missing_id"), TEXT("Skipped material function expression properties because id is missing."));
			continue;
		}

		UMaterialExpression* const* ExpressionPtr = ExpressionIdToExpression.Find(ExpressionId);
		if (!ExpressionPtr || !*ExpressionPtr)
		{
			AddWarning(TEXT("material_function_expression_unresolved_id"), FString::Printf(TEXT("Skipped material function expression properties because id '%s' was not created."), *ExpressionId));
			continue;
		}
		UMaterialExpression* Expression = *ExpressionPtr;

		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (!ExpressionObj->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
		{
			continue;
		}

		for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
		{
			const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
			{
				AddWarning(TEXT("material_function_property_invalid_item"), FString::Printf(TEXT("Skipped property on expression '%s' because it is not a JSON object."), *ExpressionId));
				continue;
			}

			FString PropertyName;
			FString ValueText;
			if (!UeAgentMaterialFolderOps::TryGetString(*PropertyObjPtr, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
			{
				AddWarning(TEXT("material_function_property_missing_name"), FString::Printf(TEXT("Skipped property on expression '%s' because property_name is missing."), *ExpressionId));
				continue;
			}
			if (!UeAgentMaterialFolderOps::TryGetString(*PropertyObjPtr, TEXT("value_text"), ValueText))
			{
				AddWarning(TEXT("material_function_property_missing_value_text"), FString::Printf(TEXT("Skipped property '%s' on expression '%s' because value_text is missing."), *PropertyName, *ExpressionId));
				continue;
			}

			if (PropertyName.Equals(TEXT("MaterialFunction"), ESearchCase::IgnoreCase))
			{
				UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
				UMaterialFunctionInterface* NestedFunction = Cast<UMaterialFunctionInterface>(UeAgentMaterialOps::LoadAssetObject(ValueText));
				if (!FunctionCall || !NestedFunction || !FunctionCall->SetMaterialFunction(NestedFunction))
				{
					OutError = TEXT("set_material_function_failed");
					return false;
				}
				FunctionCall->UpdateFromFunctionResource();
			}
			else
			{
				FString AppliedValueText;
				FString CppType;
				FString ImportError;
				if (!UeAgentMaterialOps::SetObjectPropertyText(Expression, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
				{
					OutError = ImportError.IsEmpty() ? TEXT("set_function_expression_property_failed") : ImportError;
					return false;
				}
			}
			++ExpressionPropertiesApplied;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* EdgesArray = nullptr;
	if (GraphJson->TryGetArrayField(TEXT("edges"), EdgesArray) && EdgesArray)
	{
		for (const TSharedPtr<FJsonValue>& EdgeValue : *EdgesArray)
		{
			const TSharedPtr<FJsonObject>* EdgeObjPtr = nullptr;
			if (!EdgeValue.IsValid() || !EdgeValue->TryGetObject(EdgeObjPtr) || !EdgeObjPtr || !EdgeObjPtr->IsValid())
			{
				AddWarning(TEXT("material_function_edge_invalid_item"), TEXT("Skipped material function edge because it is not a JSON object."));
				continue;
			}

			TSharedPtr<FJsonObject> FromObj;
			TSharedPtr<FJsonObject> ToObj;
			if (!UeAgentMaterialFolderOps::TryGetObjectField(*EdgeObjPtr, TEXT("from"), FromObj)
				|| !UeAgentMaterialFolderOps::TryGetObjectField(*EdgeObjPtr, TEXT("to"), ToObj))
			{
				OutError = TEXT("missing_edge_endpoints");
				return false;
			}

			FString FromNodeId;
			FString ToNodeId;
			if (!UeAgentMaterialFolderOps::TryGetString(FromObj, TEXT("node"), FromNodeId) || FromNodeId.IsEmpty())
			{
				OutError = TEXT("missing_from_node");
				return false;
			}
			if (!UeAgentMaterialFolderOps::TryGetString(ToObj, TEXT("node"), ToNodeId) || ToNodeId.IsEmpty())
			{
				OutError = TEXT("missing_to_node");
				return false;
			}

			UMaterialExpression* const* FromExpression = ExpressionIdToExpression.Find(FromNodeId);
			UMaterialExpression* const* ToExpression = ExpressionIdToExpression.Find(ToNodeId);
			if (!FromExpression || !*FromExpression || !ToExpression || !*ToExpression)
			{
				OutError = TEXT("function_edge_expression_not_found");
				return false;
			}

			FString OutputName;
			FString InputName;
			UeAgentMaterialFolderOps::TryGetString(FromObj, TEXT("output"), OutputName);
			UeAgentMaterialFolderOps::TryGetString(ToObj, TEXT("input"), InputName);

			if (!UMaterialEditingLibrary::ConnectMaterialExpressions(*FromExpression, OutputName, *ToExpression, InputName)
				&& !UeAgentMaterialOps::ManualConnectExpressions(*FromExpression, OutputName, *ToExpression, InputName))
			{
				OutError = TEXT("connect_function_expressions_failed");
				return false;
			}
			++EdgesCreated;
		}
	}

	if (bUpdateAfterApply)
	{
		UMaterialEditingLibrary::UpdateMaterialFunction(MaterialFunction, nullptr);
	}
	else
	{
		MaterialFunction->MarkPackageDirty();
		MaterialFunction->PostEditChange();
	}

	if (bSaveAfterApply)
	{
		if (!UeAgentMaterialOps::SaveAssetPackage(MaterialFunction, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("expressions_deleted"), ExpressionsDeleted);
	OutData->SetNumberField(TEXT("expressions_created"), ExpressionsCreated);
	OutData->SetNumberField(TEXT("expression_properties_applied"), ExpressionPropertiesApplied);
	OutData->SetNumberField(TEXT("edges_created"), EdgesCreated);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	OutData->SetBoolField(TEXT("updated"), bUpdateAfterApply);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	return true;
}
