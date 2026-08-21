namespace UeAgentTextureOps
{
	FString NormalizeAssetPath(const FString& InPath)
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

	FString ToObjectPath(const FString& InPath)
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

	UObject* LoadAssetObject(const FString& InPath)
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

	template<typename TObjectType>
	static TObjectType* LoadAsset(const FString& InPath)
	{
		return Cast<TObjectType>(LoadAssetObject(InPath));
	}

	bool GetRequiredString(const FUeAgentRequestContext& Ctx, const TCHAR* FieldName, FString& OutValue, FString& OutError)
	{
		OutValue.Reset();
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(FieldName, OutValue) || OutValue.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("missing_%s"), FieldName);
			return false;
		}
		OutValue.TrimStartAndEndInline();
		return true;
	}

	bool GetRequiredStringAny(const FUeAgentRequestContext& Ctx, const TArray<const TCHAR*>& FieldNames, FString& OutValue, FString& OutError)
	{
		OutValue.Reset();
		if (Ctx.Params.IsValid())
		{
			for (const TCHAR* FieldName : FieldNames)
			{
				if (FieldName && Ctx.Params->TryGetStringField(FieldName, OutValue) && !OutValue.TrimStartAndEnd().IsEmpty())
				{
					OutValue.TrimStartAndEndInline();
					return true;
				}
			}
		}
		const TCHAR* FirstField = FieldNames.Num() > 0 && FieldNames[0] ? FieldNames[0] : TEXT("string");
		OutError = FString::Printf(TEXT("missing_%s"), FirstField);
		return false;
	}

	bool JsonTryGetStringAny(const TSharedPtr<FJsonObject>& Obj, const TArray<const TCHAR*>& FieldNames, FString& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const TCHAR* FieldName : FieldNames)
		{
			if (FieldName && Obj->TryGetStringField(FieldName, OutValue) && !OutValue.TrimStartAndEnd().IsEmpty())
			{
				OutValue.TrimStartAndEndInline();
				return true;
			}
		}
		return false;
	}

	bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->TryGetBoolField(FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("invalid_%s"), FieldName);
			return false;
		}
		return true;
	}

	void EchoOptionalBool(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, const TCHAR* FieldName, bool DefaultValue, FString& OutError)
	{
		bool Value = DefaultValue;
		GetOptionalBool(Params, FieldName, Value, OutError);
		if (OutData.IsValid())
		{
			OutData->SetBoolField(FieldName, Value);
		}
	}

	bool GetOptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		double Number = InOutValue;
		if (!Params->TryGetNumberField(FieldName, Number))
		{
			OutError = FString::Printf(TEXT("invalid_%s"), FieldName);
			return false;
		}
		InOutValue = static_cast<int32>(Number);
		return true;
	}

	bool GetOptionalFloat(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, float& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		double Number = InOutValue;
		if (!Params->TryGetNumberField(FieldName, Number))
		{
			OutError = FString::Printf(TEXT("invalid_%s"), FieldName);
			return false;
		}
		InOutValue = static_cast<float>(Number);
		return true;
	}

	bool ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		if (!Params.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			FString Item = Value->AsString();
			Item.TrimStartAndEndInline();
			if (!Item.IsEmpty())
			{
				OutValues.Add(Item);
			}
		}
		return true;
	}

	FString ResolveFilePath(const FString& FilePath)
	{
		return UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FilePath);
	}

	bool SaveAuthoringFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError, bool bStandaloneExport)
	{
		UeAgentAuthoringFileIO::FSaveOptions Options;
		Options.SourceCommand = TEXT("texture_json_export");
		Options.bScopeInlineDetailsToEntryFile = bStandaloneExport;
		return UeAgentAuthoringFileIO::SaveObject(OutputPathInput, Root, Options, OutOutputPath, OutError);
	}

	bool SaveAuthoringFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
	{
		return SaveAuthoringFile(OutputPathInput, Root, OutOutputPath, OutError, false);
	}

	bool LoadAuthoringFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError)
	{
		OutRoot.Reset();
		OutResolvedPath.Reset();
		FString InputPath = UeAgentStructuredAuthoringIO::NormalizeAuthoringPath(InputPathInput);
		InputPath.TrimStartAndEndInline();
		if (InputPath.IsEmpty())
		{
			OutError = TEXT("missing_json_file");
			return false;
		}
		if (!UeAgentStructuredAuthoringIO::LoadObject(InputPath, OutRoot, OutError))
		{
			return false;
		}
		OutResolvedPath = InputPath;
		return UeAgentDetailsAuthoring::RehydrateGroupedDetailsInObjectTree(InputPath, OutRoot, OutError);
	}

	TSharedPtr<FJsonObject> MakeCoverageReport(const FString& Profile)
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetStringField(TEXT("profile"), Profile);
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetArrayField(TEXT("pending_profiles"), {});
		Coverage->SetArrayField(TEXT("blocking_gaps"), {});
		return Coverage;
	}

	TSharedPtr<FJsonObject> MakeIssue(const FString& Code, const FString& Severity, const FString& Message)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("message"), Message);
		return Issue;
	}

	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Code, const FString& Severity, const FString& Message)
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Code, Severity, Message)));
	}

	bool SavePackagesForObjects(const TArray<UObject*>& Objects, FString& OutError)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* Object : Objects)
		{
			if (Object && Object->GetOutermost())
			{
				PackagesToSave.AddUnique(Object->GetOutermost());
			}
		}
		if (PackagesToSave.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	bool ValidateAssetPathForCreate(const FString& AssetPathInput, FString& OutAssetPath, FString& OutAssetName, FString& OutError)
	{
		OutAssetPath = NormalizeAssetPath(AssetPathInput);
		OutAssetName = FPackageName::GetLongPackageAssetName(OutAssetPath);
		if (OutAssetPath.IsEmpty())
		{
			OutError = TEXT("missing_asset_path");
			return false;
		}
		if (!FPackageName::IsValidLongPackageName(OutAssetPath) || OutAssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}
		return true;
	}

	UObject* CreateAssetObject(UClass* AssetClass, const FString& AssetPathInput, bool bReplaceExisting, FString& OutError)
	{
		if (!AssetClass)
		{
			OutError = TEXT("asset_class_not_found");
			return nullptr;
		}
		if (AssetClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("asset_class_is_abstract");
			return nullptr;
		}

		FString AssetPath;
		FString AssetName;
		if (!ValidateAssetPathForCreate(AssetPathInput, AssetPath, AssetName, OutError))
		{
			return nullptr;
		}

		UObject* Existing = LoadAssetObject(AssetPath);
		if (Existing)
		{
			if (!bReplaceExisting)
			{
				OutError = TEXT("asset_already_exists");
				return nullptr;
			}
			if (!Existing->IsA(AssetClass))
			{
				OutError = TEXT("existing_asset_class_mismatch");
				return nullptr;
			}
			Existing->Modify();
			return Existing;
		}

		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UObject* Asset = NewObject<UObject>(Package, AssetClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset)
		{
			OutError = TEXT("create_asset_failed");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		Asset->MarkPackageDirty();
		return Asset;
	}

	void FillAssetIdentity(UObject* Asset, const TSharedPtr<FJsonObject>& OutData)
	{
		if (!Asset || !OutData.IsValid())
		{
			return;
		}
		OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost() ? NormalizeAssetPath(Asset->GetOutermost()->GetName()) : FString());
		OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
		OutData->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetName() : FString());
		OutData->SetStringField(TEXT("class_path"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : FString());
	}

	template<typename EnumType>
	FString EnumToString(EnumType Value)
	{
		const UEnum* Enum = StaticEnum<EnumType>();
		return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Value)) : FString::FromInt(static_cast<int32>(Value));
	}

	FString PixelFormatToString(EPixelFormat Value)
	{
		if (const UEnum* Enum = UTexture::GetPixelFormatEnum())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	bool ParseRenderTargetFormat(const FString& FormatTextInput, ETextureRenderTargetFormat& OutFormat)
	{
		FString FormatText = FormatTextInput.TrimStartAndEnd();
		if (FormatText.IsEmpty())
		{
			OutFormat = RTF_RGBA8;
			return true;
		}
		if (const UEnum* Enum = StaticEnum<ETextureRenderTargetFormat>())
		{
			int64 Value = Enum->GetValueByNameString(FormatText);
			if (Value == INDEX_NONE && !FormatText.StartsWith(TEXT("RTF_")))
			{
				Value = Enum->GetValueByNameString(TEXT("RTF_") + FormatText);
			}
			if (Value != INDEX_NONE)
			{
				OutFormat = static_cast<ETextureRenderTargetFormat>(Value);
				return true;
			}
		}
		return false;
	}

	EPixelFormat PixelFormatFromRTFormat(ETextureRenderTargetFormat Format)
	{
		return GetPixelFormatFromRenderTargetFormat(Format);
	}

	TSharedPtr<FJsonObject> MakeColorJson(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Color.R);
		Obj->SetNumberField(TEXT("g"), Color.G);
		Obj->SetNumberField(TEXT("b"), Color.B);
		Obj->SetNumberField(TEXT("a"), Color.A);
		return Obj;
	}

	bool ParseColor(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FLinearColor& InOutColor, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		const TSharedPtr<FJsonValue>* ColorValue = UeAgentJsonDiagnostics::FindFieldValue(Params, FieldName);
		if (!ColorValue || !UeAgentJsonDiagnostics::TryReadLinearColorValue(*ColorValue, InOutColor))
		{
			OutError = FString::Printf(TEXT("invalid_%s"), FieldName);
			return false;
		}
		return true;
	}

	bool ParseColorAny(const TSharedPtr<FJsonObject>& Params, const TArray<const TCHAR*>& FieldNames, FLinearColor& InOutColor, FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}
		for (const TCHAR* FieldName : FieldNames)
		{
			if (FieldName && Params->HasField(FieldName))
			{
				return ParseColor(Params, FieldName, InOutColor, OutError);
			}
		}
		return true;
	}

	ERawImageFormat::Type TextureSourceFormatToRaw(ETextureSourceFormat Format)
	{
		switch (Format)
		{
		case TSF_G8: return ERawImageFormat::G8;
		case TSF_BGRA8: return ERawImageFormat::BGRA8;
		case TSF_BGRE8: return ERawImageFormat::BGRE8;
		case TSF_RGBA16: return ERawImageFormat::RGBA16;
		case TSF_RGBA16F: return ERawImageFormat::RGBA16F;
		case TSF_G16: return ERawImageFormat::G16;
		case TSF_RGBA32F: return ERawImageFormat::RGBA32F;
		case TSF_R16F: return ERawImageFormat::R16F;
		case TSF_R32F: return ERawImageFormat::R32F;
		default: return ERawImageFormat::Invalid;
		}
	}

	ETextureSourceFormat RawImageFormatToTextureSource(ERawImageFormat::Type Format)
	{
		switch (Format)
		{
		case ERawImageFormat::G8: return TSF_G8;
		case ERawImageFormat::BGRA8: return TSF_BGRA8;
		case ERawImageFormat::BGRE8: return TSF_BGRE8;
		case ERawImageFormat::RGBA16: return TSF_RGBA16;
		case ERawImageFormat::RGBA16F: return TSF_RGBA16F;
		case ERawImageFormat::G16: return TSF_G16;
		case ERawImageFormat::RGBA32F: return TSF_RGBA32F;
		case ERawImageFormat::R16F: return TSF_R16F;
		case ERawImageFormat::R32F: return TSF_R32F;
		default: return TSF_Invalid;
		}
	}

	bool ReadTextureSourceImage(UTexture* Texture, int32 MipIndex, int32 SliceIndex, FImage& OutImage, FString& OutError)
	{
		if (!Texture)
		{
			OutError = TEXT("texture_not_found");
			return false;
		}
		if (!Texture->Source.IsValid())
		{
			OutError = TEXT("texture_source_unavailable");
			return false;
		}
		MipIndex = FMath::Clamp(MipIndex, 0, FMath::Max(0, Texture->Source.GetNumMips() - 1));
		SliceIndex = FMath::Clamp(SliceIndex, 0, FMath::Max(0, Texture->Source.GetNumSlices() - 1));

		FImage SourceImage;
		if (!Texture->Source.GetMipImage(SourceImage, 0, 0, MipIndex))
		{
			OutError = TEXT("texture_source_read_failed");
			return false;
		}
		if (SourceImage.NumSlices > 1)
		{
			SourceImage.GetSlice(SliceIndex).CopyTo(OutImage, ERawImageFormat::BGRA8, Texture->SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);
		}
		else
		{
			SourceImage.CopyTo(OutImage, ERawImageFormat::BGRA8, Texture->SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);
		}
		return true;
	}

	void ImageToColors(const FImage& Image, TArray<FColor>& OutColors)
	{
		FImage BGRA;
		Image.CopyTo(BGRA, ERawImageFormat::BGRA8, Image.GammaSpace);
		const FImageView BGRAView = BGRA;
		const TArrayView64<FColor> View = BGRAView.AsBGRA8();
		OutColors.Reset();
		OutColors.Append(View.GetData(), static_cast<int32>(View.Num()));
	}

	TSharedPtr<FJsonObject> BuildPixelStatsFromColors(const TArray<FColor>& Pixels, const FIntPoint& Size)
	{
		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetNumberField(TEXT("width"), Size.X);
		Stats->SetNumberField(TEXT("height"), Size.Y);
		Stats->SetNumberField(TEXT("sampled_pixel_count"), Pixels.Num());

		if (Pixels.Num() <= 0)
		{
			Stats->SetBoolField(TEXT("appears_non_black"), false);
			Stats->SetNumberField(TEXT("non_black_pixel_count"), 0);
			Stats->SetNumberField(TEXT("transparent_pixel_count"), 0);
			Stats->SetNumberField(TEXT("unique_color_estimate"), 0);
			return Stats;
		}

		FLinearColor Min(1.f, 1.f, 1.f, 1.f);
		FLinearColor Max(0.f, 0.f, 0.f, 0.f);
		FLinearColor Sum(0.f, 0.f, 0.f, 0.f);
		int32 NonBlack = 0;
		int32 Transparent = 0;
		TSet<uint32> Unique;
		for (const FColor& Pixel : Pixels)
		{
			const FLinearColor C = Pixel.ReinterpretAsLinear();
			Min.R = FMath::Min(Min.R, C.R); Min.G = FMath::Min(Min.G, C.G); Min.B = FMath::Min(Min.B, C.B); Min.A = FMath::Min(Min.A, C.A);
			Max.R = FMath::Max(Max.R, C.R); Max.G = FMath::Max(Max.G, C.G); Max.B = FMath::Max(Max.B, C.B); Max.A = FMath::Max(Max.A, C.A);
			Sum += C;
			if ((Pixel.R + Pixel.G + Pixel.B) > 12)
			{
				++NonBlack;
			}
			if (Pixel.A < 250)
			{
				++Transparent;
			}
			if (Unique.Num() < 65536)
			{
				Unique.Add(Pixel.ToPackedARGB());
			}
		}
		const float Count = static_cast<float>(Pixels.Num());
		Stats->SetObjectField(TEXT("min"), MakeColorJson(Min));
		Stats->SetObjectField(TEXT("max"), MakeColorJson(Max));
		Stats->SetObjectField(TEXT("mean"), MakeColorJson(Sum / Count));
		Stats->SetNumberField(TEXT("non_black_pixel_count"), NonBlack);
		Stats->SetNumberField(TEXT("transparent_pixel_count"), Transparent);
		Stats->SetNumberField(TEXT("unique_color_estimate"), Unique.Num());
		Stats->SetBoolField(TEXT("appears_non_black"), NonBlack > 0);
		return Stats;
	}

	TSharedPtr<FJsonObject> BuildPixelStatsFromImage(const FImage& Image)
	{
		TArray<FColor> Colors;
		ImageToColors(Image, Colors);
		return BuildPixelStatsFromColors(Colors, FIntPoint(Image.SizeX, Image.SizeY));
	}

	bool ReadRenderTarget2DPixels(UTextureRenderTarget2D* RenderTarget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		if (!FApp::CanEverRender())
		{
			OutError = TEXT("render_resource_unavailable_null_rhi");
			return false;
		}
		if (!RenderTarget)
		{
			OutError = TEXT("render_target_not_found");
			return false;
		}
		FRenderTarget* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!RenderTargetResource)
		{
			OutError = TEXT("render_resource_unavailable");
			return false;
		}
		FlushRenderingCommands();
		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(false);
		if (!RenderTargetResource->ReadPixels(OutPixels, ReadFlags) || OutPixels.Num() <= 0)
		{
			OutError = TEXT("read_pixels_failed");
			return false;
		}
		OutSize = FIntPoint(RenderTarget->SizeX, RenderTarget->SizeY);
		return true;
	}

	bool RequireRenderResourceAccess(FString& OutError)
	{
		if (!FApp::CanEverRender())
		{
			OutError = TEXT("render_resource_unavailable_null_rhi");
			return false;
		}
		return true;
	}

	bool ApplyCanonicalProperty(UObject* Obj, const FString& PropertyPath, const TSharedPtr<FJsonValue>& CanonicalValue, TSharedPtr<FJsonObject>& OutSummary, FString& OutError)
	{
		OutSummary.Reset();
		if (!Obj || PropertyPath.IsEmpty() || !CanonicalValue.IsValid())
		{
			OutError = TEXT("invalid_canonical_property_request");
			return false;
		}
		FProperty* Property = FindFProperty<FProperty>(Obj->GetClass(), *PropertyPath);
		if (!Property)
		{
			OutError = TEXT("property_not_found");
			return false;
		}
		UeAgentUObjectProperties::FExportOptions ExportOptions;
		TSharedPtr<FJsonObject> Descriptor = UeAgentUObjectProperties::BuildPropertyDescriptor(Obj, Property, ExportOptions);
		Descriptor->SetField(TEXT("value"), CanonicalValue);
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		Payload->SetArrayField(TEXT("properties"), { MakeShared<FJsonValueObject>(Descriptor) });
		OutSummary = UeAgentUObjectProperties::ApplyObjectProperties(Obj, Payload, UeAgentUObjectProperties::FApplyOptions());
		FString Status;
		if (!OutSummary.IsValid() || !OutSummary->TryGetStringField(TEXT("status"), Status) || Status != TEXT("applied"))
		{
			OutError = TEXT("canonical_property_apply_failed");
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> BuildTextureInfo(UTexture* Texture, bool bIncludeSource, bool bIncludePixelStats)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Texture, Info);
		if (!Texture)
		{
			return Info;
		}
		Info->SetStringField(TEXT("type"), Texture->GetClass()->GetName());
		Info->SetNumberField(TEXT("surface_width"), Texture->GetSurfaceWidth());
		Info->SetNumberField(TEXT("surface_height"), Texture->GetSurfaceHeight());
		Info->SetNumberField(TEXT("surface_depth"), Texture->GetSurfaceDepth());
		Info->SetNumberField(TEXT("surface_array_size"), Texture->GetSurfaceArraySize());
		Info->SetBoolField(TEXT("srgb"), Texture->SRGB != 0);
		Info->SetBoolField(TEXT("virtual_texture_streaming"), Texture->VirtualTextureStreaming != 0);
		Info->SetStringField(TEXT("compression_settings"), EnumToString<TextureCompressionSettings>(static_cast<TextureCompressionSettings>(Texture->CompressionSettings.GetValue())));
		Info->SetStringField(TEXT("compression"), Info->GetStringField(TEXT("compression_settings")));
		Info->SetStringField(TEXT("mip_gen_settings"), EnumToString<TextureMipGenSettings>(static_cast<TextureMipGenSettings>(Texture->MipGenSettings.GetValue())));
		Info->SetStringField(TEXT("lod_group"), EnumToString<TextureGroup>(static_cast<TextureGroup>(Texture->LODGroup.GetValue())));
		Info->SetStringField(TEXT("filter"), EnumToString<TextureFilter>(static_cast<TextureFilter>(Texture->Filter.GetValue())));
		Info->SetBoolField(TEXT("has_alpha"), Texture->Source.IsValid() && ERawImageFormat::HasAlphaChannel(TextureSourceFormatToRaw(Texture->Source.GetFormat())));
		Info->SetNumberField(TEXT("mips"), Texture->Source.IsValid() ? Texture->Source.GetNumMips() : 0);

		if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
		{
			Info->SetNumberField(TEXT("size_x"), Texture2D->GetSizeX());
			Info->SetNumberField(TEXT("size_y"), Texture2D->GetSizeY());
			Info->SetNumberField(TEXT("num_mips"), Texture2D->GetNumMips());
			if (Texture2D->GetPlatformData())
			{
				Info->SetStringField(TEXT("pixel_format"), PixelFormatToString(Texture2D->GetPlatformData()->PixelFormat));
			}
		}
		else if (UTexture2DArray* TextureArray = Cast<UTexture2DArray>(Texture))
		{
			Info->SetNumberField(TEXT("size_x"), TextureArray->GetSizeX());
			Info->SetNumberField(TEXT("size_y"), TextureArray->GetSizeY());
			Info->SetNumberField(TEXT("slice_count"), TextureArray->GetArraySize());
			Info->SetStringField(TEXT("pixel_format"), PixelFormatToString(TextureArray->GetPixelFormat()));
		}
		else if (UTextureCube* TextureCube = Cast<UTextureCube>(Texture))
		{
			Info->SetNumberField(TEXT("size_x"), TextureCube->GetSizeX());
			Info->SetNumberField(TEXT("size_y"), TextureCube->GetSizeY());
			Info->SetNumberField(TEXT("face_count"), 6);
			Info->SetStringField(TEXT("pixel_format"), PixelFormatToString(TextureCube->GetPixelFormat()));
		}
		else if (UTextureCubeArray* TextureCubeArray = Cast<UTextureCubeArray>(Texture))
		{
			Info->SetNumberField(TEXT("size_x"), TextureCubeArray->GetSizeX());
			Info->SetNumberField(TEXT("size_y"), TextureCubeArray->GetSizeY());
			Info->SetNumberField(TEXT("cube_count"), TextureCubeArray->GetNumSlices());
			Info->SetStringField(TEXT("pixel_format"), PixelFormatToString(TextureCubeArray->GetPixelFormat()));
		}
		else if (UVolumeTexture* Volume = Cast<UVolumeTexture>(Texture))
		{
			Info->SetNumberField(TEXT("size_x"), Volume->GetSizeX());
			Info->SetNumberField(TEXT("size_y"), Volume->GetSizeY());
			Info->SetNumberField(TEXT("size_z"), Volume->GetSizeZ());
			Info->SetStringField(TEXT("pixel_format"), PixelFormatToString(Volume->GetPixelFormat()));
		}

		if (bIncludeSource)
		{
			TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
			Source->SetBoolField(TEXT("valid"), Texture->Source.IsValid());
			Source->SetStringField(TEXT("source_hash"), Texture->Source.GetIdString());
			Source->SetNumberField(TEXT("width"), Texture->Source.GetSizeX());
			Source->SetNumberField(TEXT("height"), Texture->Source.GetSizeY());
			Source->SetNumberField(TEXT("slices"), Texture->Source.GetNumSlices());
			Source->SetNumberField(TEXT("mips"), Texture->Source.GetNumMips());
			Source->SetNumberField(TEXT("blocks"), Texture->Source.GetNumBlocks());
			Source->SetStringField(TEXT("format"), Texture->Source.IsValid() ? FString(ERawImageFormat::GetName(TextureSourceFormatToRaw(Texture->Source.GetFormat()))) : FString());
			Info->SetObjectField(TEXT("source"), Source);
		}

		if (bIncludePixelStats)
		{
			FImage Image;
			FString PixelError;
			if (ReadTextureSourceImage(Texture, 0, 0, Image, PixelError))
			{
				Info->SetObjectField(TEXT("stats"), BuildPixelStatsFromImage(Image));
				Info->SetBoolField(TEXT("pixel_stats_available"), true);
			}
			else
			{
				Info->SetBoolField(TEXT("pixel_stats_available"), false);
				Info->SetStringField(TEXT("pixel_stats_error"), PixelError);
			}
		}
		return Info;
	}

	TSharedPtr<FJsonObject> BuildRenderTargetInfo(UTextureRenderTarget* RenderTarget)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(RenderTarget, Info);
		if (!RenderTarget)
		{
			return Info;
		}
		Info->SetStringField(TEXT("type"), RenderTarget->GetClass()->GetName());
		Info->SetNumberField(TEXT("surface_width"), RenderTarget->GetSurfaceWidth());
		Info->SetNumberField(TEXT("surface_height"), RenderTarget->GetSurfaceHeight());
		Info->SetNumberField(TEXT("surface_depth"), RenderTarget->GetSurfaceDepth());
		Info->SetNumberField(TEXT("surface_array_size"), RenderTarget->GetSurfaceArraySize());
		Info->SetStringField(TEXT("pixel_format"), PixelFormatToString(RenderTarget->GetFormat()));
		Info->SetBoolField(TEXT("resource_valid"), RenderTarget->GameThread_GetRenderTargetResource() != nullptr);
		Info->SetStringField(TEXT("resource_state"), FApp::CanEverRender() ? TEXT("rendering_available") : TEXT("null_rhi"));
		if (UTextureRenderTarget2D* RT2D = Cast<UTextureRenderTarget2D>(RenderTarget))
		{
			Info->SetStringField(TEXT("target_type"), RT2D->IsA<UCanvasRenderTarget2D>() ? TEXT("canvas2d") : TEXT("2d"));
			Info->SetNumberField(TEXT("width"), RT2D->SizeX);
			Info->SetNumberField(TEXT("height"), RT2D->SizeY);
			Info->SetStringField(TEXT("render_target_format"), EnumToString<ETextureRenderTargetFormat>(static_cast<ETextureRenderTargetFormat>(RT2D->RenderTargetFormat.GetValue())));
			Info->SetObjectField(TEXT("clear_color"), MakeColorJson(RT2D->ClearColor));
			Info->SetBoolField(TEXT("auto_generate_mips"), RT2D->bAutoGenerateMips);
			Info->SetBoolField(TEXT("can_read_pixels"), true);
		}
		else if (UTextureRenderTargetCube* Cube = Cast<UTextureRenderTargetCube>(RenderTarget))
		{
			Info->SetStringField(TEXT("target_type"), TEXT("cube"));
			Info->SetNumberField(TEXT("width"), Cube->SizeX);
			Info->SetNumberField(TEXT("height"), Cube->SizeX);
			Info->SetNumberField(TEXT("face_count"), 6);
			Info->SetBoolField(TEXT("can_read_pixels"), false);
		}
		else if (UTextureRenderTarget2DArray* Array = Cast<UTextureRenderTarget2DArray>(RenderTarget))
		{
			Info->SetStringField(TEXT("target_type"), TEXT("2d_array"));
			Info->SetNumberField(TEXT("width"), Array->SizeX);
			Info->SetNumberField(TEXT("height"), Array->SizeY);
			Info->SetNumberField(TEXT("slice_count"), Array->Slices);
			Info->SetBoolField(TEXT("can_read_pixels"), false);
		}
		else if (UTextureRenderTargetVolume* Volume = Cast<UTextureRenderTargetVolume>(RenderTarget))
		{
			Info->SetStringField(TEXT("target_type"), TEXT("volume"));
			Info->SetNumberField(TEXT("width"), Volume->SizeX);
			Info->SetNumberField(TEXT("height"), Volume->SizeY);
			Info->SetNumberField(TEXT("depth"), Volume->SizeZ);
			Info->SetBoolField(TEXT("can_read_pixels"), false);
		}
		return Info;
	}

	TSharedPtr<FJsonObject> BuildGenericAssetJson(UObject* Asset, const FString& Profile, bool bIncludeProperties)
	{
		UeAgentUObjectProperties::FExportOptions Options;
		Options.Profile = bIncludeProperties
			? UeAgentUObjectProperties::EExportProfile::Authoring
			: UeAgentUObjectProperties::EExportProfile::Index;
		TSharedPtr<FJsonObject> Root = UeAgentUObjectProperties::ExportObjectProperties(Asset, Options);
		Root->SetStringField(TEXT("domain_profile"), Profile);
		FillAssetIdentity(Asset, Root);
		return Root;
	}

	bool ApplyGenericProperties(UObject* Asset, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& OutResults, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			return true;
		}
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		Payload->SetArrayField(TEXT("properties"), *Properties);
		const TSharedPtr<FJsonObject> Summary = UeAgentUObjectProperties::ApplyObjectProperties(
			Asset,
			Payload,
			UeAgentUObjectProperties::FApplyOptions());
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (Summary.IsValid() && Summary->TryGetArrayField(TEXT("properties"), Results) && Results) OutResults = *Results;
		FString Status;
		if (!Summary.IsValid() || !Summary->TryGetStringField(TEXT("status"), Status) || Status != TEXT("applied"))
		{
			OutError = TEXT("generic_properties_apply_failed");
			return false;
		}
		return true;
	}

	bool RejectAuthoringPropertiesFolderIfPresent(
		const FString& FolderPath,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& SourceCommand,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> AuthoringRejection;
		if (!UeAgentDetailsAuthoring::BuildAuthoringPropertiesFolderRejectionIfPresent(
			ResolveFilePath(FolderPath),
			UeAgentDetailsAuthoring::MakeOwnerInfoForObject(nullptr, OwnerKind, OwnerId, SourceCommand),
			TEXT("asset.hjson.details.index_file"),
			AuthoringRejection,
			OutError))
		{
			return false;
		}

		OutData = AuthoringRejection;
		return true;
	}

	bool ApplyAssetDetailsFromFolderIfPresent(
		UObject* Asset,
		const FString& FolderPath,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& OwnerKind,
		const FString& SourceCommand,
		bool bApply,
		TSharedPtr<FJsonObject>& OutApplySummary,
		int32& InOutAppliedCount,
		FString& OutError)
	{
		const FString OwnerId = Asset ? Asset->GetPathName() : FString();
		return UeAgentDetailsAuthoring::ApplyOwnerDetailsFromEntryIfPresent(
			Asset,
			ResolveFilePath(FolderPath),
			EntryObject,
			UeAgentDetailsAuthoring::MakeOwnerInfoForObject(Asset, OwnerKind, OwnerId, SourceCommand),
			TEXT("details"),
			bApply,
			OutApplySummary,
			InOutAppliedCount,
			OutError);
	}

	bool RenderTextureGraphViaReflection(UObject* WorldContextObject, UTextureGraphBase* Graph, TArray<UTextureRenderTarget2D*>& OutOutputs, FString& OutError)
	{
		OutOutputs.Reset();
		if (!FApp::CanEverRender())
		{
			OutError = TEXT("texture_graph_render_unavailable_null_rhi");
			return false;
		}
		UClass* LibraryClass = FindObject<UClass>(nullptr, TEXT("/Script/TextureGraph.TG_BlueprintFunctionLibrary"));
		if (!LibraryClass)
		{
			LibraryClass = LoadObject<UClass>(nullptr, TEXT("/Script/TextureGraph.TG_BlueprintFunctionLibrary"));
		}
		if (!LibraryClass)
		{
			OutError = TEXT("texture_graph_blueprint_library_not_found");
			return false;
		}
		UFunction* Function = LibraryClass->FindFunctionByName(TEXT("RenderTextureGraph"));
		if (!Function)
		{
			OutError = TEXT("texture_graph_render_function_not_found");
			return false;
		}
		struct FRenderTextureGraphParams
		{
			UObject* WorldContextObject = nullptr;
			UTextureGraphBase* InTextureGraph = nullptr;
			TArray<UTextureRenderTarget2D*> ReturnValue;
		};
		FRenderTextureGraphParams Params;
		Params.WorldContextObject = WorldContextObject;
		Params.InTextureGraph = Graph;
		UObject* DefaultObject = LibraryClass->GetDefaultObject();
		if (!DefaultObject)
		{
			OutError = TEXT("texture_graph_blueprint_library_default_object_missing");
			return false;
		}
		DefaultObject->ProcessEvent(Function, &Params);
		OutOutputs = Params.ReturnValue;
		return true;
	}
}

namespace
{
	using FTextureCommandHandler = bool (FUeAgentLegacyMechanics::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;

	struct FTextureCommandEntry
	{
		const TCHAR* Name;
		FTextureCommandHandler Handler;
	};

	static const FTextureCommandEntry GTextureCommandEntries[] = {
		{ TEXT("texture_reimport"), &FUeAgentLegacyMechanics::CmdTextureReimport },
		{ TEXT("texture_get_info"), &FUeAgentLegacyMechanics::CmdTextureGetInfo },
		{ TEXT("texture_validate_settings"), &FUeAgentLegacyMechanics::CmdTextureValidateSettings },
		{ TEXT("texture_export_image"), &FUeAgentLegacyMechanics::CmdTextureExportImage },
		{ TEXT("texture_pixel_probe"), &FUeAgentLegacyMechanics::CmdTexturePixelProbe },
		{ TEXT("texture_screenshot"), &FUeAgentLegacyMechanics::CmdTextureScreenshot },
		{ TEXT("texture_array_create"), &FUeAgentLegacyMechanics::CmdTextureArrayCreate },
		{ TEXT("texture_cube_create"), &FUeAgentLegacyMechanics::CmdTextureCubeCreate },
		{ TEXT("texture_cube_array_create"), &FUeAgentLegacyMechanics::CmdTextureCubeArrayCreate },
		{ TEXT("volume_texture_create"), &FUeAgentLegacyMechanics::CmdVolumeTextureCreate },
		{ TEXT("virtual_texture_get_info"), &FUeAgentLegacyMechanics::CmdVirtualTextureGetInfo },
		{ TEXT("virtual_texture_validate_setup"), &FUeAgentLegacyMechanics::CmdVirtualTextureValidateSetup },
		{ TEXT("runtime_virtual_texture_create"), &FUeAgentLegacyMechanics::CmdRuntimeVirtualTextureCreate },
		{ TEXT("runtime_virtual_texture_get_info"), &FUeAgentLegacyMechanics::CmdRuntimeVirtualTextureGetInfo },
		{ TEXT("runtime_virtual_texture_validate_setup"), &FUeAgentLegacyMechanics::CmdRuntimeVirtualTextureValidateSetup },
		{ TEXT("render_target_create"), &FUeAgentLegacyMechanics::CmdRenderTargetCreate },
		{ TEXT("render_target_get_info"), &FUeAgentLegacyMechanics::CmdRenderTargetGetInfo },
		{ TEXT("render_target_resize"), &FUeAgentLegacyMechanics::CmdRenderTargetResize },
		{ TEXT("render_target_clear"), &FUeAgentLegacyMechanics::CmdRenderTargetClear },
		{ TEXT("render_target_draw_material"), &FUeAgentLegacyMechanics::CmdRenderTargetDrawMaterial },
		{ TEXT("render_target_copy_texture"), &FUeAgentLegacyMechanics::CmdRenderTargetCopyTexture },
		{ TEXT("render_target_capture_scene"), &FUeAgentLegacyMechanics::CmdRenderTargetCaptureScene },
		{ TEXT("render_target_read_pixels"), &FUeAgentLegacyMechanics::CmdRenderTargetReadPixels },
		{ TEXT("render_target_sample_uv"), &FUeAgentLegacyMechanics::CmdRenderTargetSampleUv },
		{ TEXT("render_target_export_image"), &FUeAgentLegacyMechanics::CmdRenderTargetExportImage },
		{ TEXT("render_target_create_static_texture"), &FUeAgentLegacyMechanics::CmdRenderTargetCreateStaticTexture },
		{ TEXT("render_target_probe"), &FUeAgentLegacyMechanics::CmdRenderTargetProbe },
		{ TEXT("scene_capture_validate_render_target"), &FUeAgentLegacyMechanics::CmdSceneCaptureValidateRenderTarget },
		{ TEXT("scene_capture_runtime_probe"), &FUeAgentLegacyMechanics::CmdSceneCaptureRuntimeProbe },
		{ TEXT("media_source_create"), &FUeAgentLegacyMechanics::CmdMediaSourceCreate },
		{ TEXT("media_source_get_info"), &FUeAgentLegacyMechanics::CmdMediaSourceGetInfo },
		{ TEXT("media_player_create"), &FUeAgentLegacyMechanics::CmdMediaPlayerCreate },
		{ TEXT("media_texture_create"), &FUeAgentLegacyMechanics::CmdMediaTextureCreate },
		{ TEXT("media_runtime_probe"), &FUeAgentLegacyMechanics::CmdMediaRuntimeProbe },
		{ TEXT("sparse_volume_texture_import"), &FUeAgentLegacyMechanics::CmdSparseVolumeTextureImport },
		{ TEXT("sparse_volume_texture_reimport"), &FUeAgentLegacyMechanics::CmdSparseVolumeTextureReimport },
		{ TEXT("sparse_volume_texture_get_info"), &FUeAgentLegacyMechanics::CmdSparseVolumeTextureGetInfo },
		{ TEXT("sparse_volume_texture_preview_probe"), &FUeAgentLegacyMechanics::CmdSparseVolumeTexturePreviewProbe },
		{ TEXT("sparse_volume_texture_validate_setup"), &FUeAgentLegacyMechanics::CmdSparseVolumeTextureValidateSetup },
		{ TEXT("texture_graph_create"), &FUeAgentLegacyMechanics::CmdTextureGraphCreate },
		{ TEXT("texture_graph_get_info"), &FUeAgentLegacyMechanics::CmdTextureGraphGetInfo },
		{ TEXT("texture_graph_bake_outputs"), &FUeAgentLegacyMechanics::CmdTextureGraphBakeOutputs },
		{ TEXT("texture_graph_runtime_probe"), &FUeAgentLegacyMechanics::CmdTextureGraphRuntimeProbe },
		{ TEXT("subuv_animation_create"), &FUeAgentLegacyMechanics::CmdSubUVAnimationCreate },
		{ TEXT("subuv_animation_get_info"), &FUeAgentLegacyMechanics::CmdSubUVAnimationGetInfo },
		{ TEXT("subuv_animation_preview_probe"), &FUeAgentLegacyMechanics::CmdSubUVAnimationPreviewProbe },
		{ TEXT("paper_sprite_create"), &FUeAgentLegacyMechanics::CmdPaperSpriteCreate },
		{ TEXT("paper_flipbook_create"), &FUeAgentLegacyMechanics::CmdPaperFlipbookCreate },
		{ TEXT("paper2d_preview_probe"), &FUeAgentLegacyMechanics::CmdPaper2DPreviewProbe },
		{ TEXT("light_profile_texture_import"), &FUeAgentLegacyMechanics::CmdLightProfileTextureImport },
		{ TEXT("light_profile_texture_get_info"), &FUeAgentLegacyMechanics::CmdLightProfileTextureGetInfo },
		{ TEXT("light_profile_validate_setup"), &FUeAgentLegacyMechanics::CmdLightProfileValidateSetup },
		{ TEXT("texture_collection_create"), &FUeAgentLegacyMechanics::CmdTextureCollectionCreate },
		{ TEXT("texture_collection_get_info"), &FUeAgentLegacyMechanics::CmdTextureCollectionGetInfo },
		{ TEXT("texture_collection_validate_usage"), &FUeAgentLegacyMechanics::CmdTextureCollectionValidateUsage },
		{ TEXT("texture_usage_validate"), &FUeAgentLegacyMechanics::CmdTextureUsageValidate },
		{ TEXT("texture_usage_get_references"), &FUeAgentLegacyMechanics::CmdTextureUsageGetReferences },
		{ TEXT("texture_usage_probe_media"), &FUeAgentLegacyMechanics::CmdTextureUsageProbeMedia },
		{ TEXT("texture_usage_probe_sparse_volume"), &FUeAgentLegacyMechanics::CmdTextureUsageProbeSparseVolume },
	};

	static FTextureCommandHandler FindTextureCommandHandler(const FString& CommandLower)
	{
		for (const FTextureCommandEntry& Entry : GTextureCommandEntries)
		{
			if (CommandLower == Entry.Name)
			{
				return Entry.Handler;
			}
		}
		return nullptr;
	}

	enum class EReflectionAuthoringOperation : uint8
	{
		Export,
		Apply,
		Validate
	};

	struct FReflectionAuthoringCommandEntry
	{
		const TCHAR* Command;
		const TCHAR* TypeId;
		EReflectionAuthoringOperation Operation;
	};

	static const FReflectionAuthoringCommandEntry GReflectionAuthoringCommandEntries[] =
	{
		{ TEXT("texture_array_export_folder"), TEXT("texture_array"), EReflectionAuthoringOperation::Export },
		{ TEXT("texture_array_apply_folder"), TEXT("texture_array"), EReflectionAuthoringOperation::Apply },
		{ TEXT("texture_cube_export_folder"), TEXT("texture_cube"), EReflectionAuthoringOperation::Export },
		{ TEXT("texture_cube_apply_folder"), TEXT("texture_cube"), EReflectionAuthoringOperation::Apply },
		{ TEXT("texture_cube_array_export_folder"), TEXT("texture_cube_array"), EReflectionAuthoringOperation::Export },
		{ TEXT("texture_cube_array_apply_folder"), TEXT("texture_cube_array"), EReflectionAuthoringOperation::Apply },
		{ TEXT("volume_texture_export_folder"), TEXT("volume_texture"), EReflectionAuthoringOperation::Export },
		{ TEXT("volume_texture_apply_folder"), TEXT("volume_texture"), EReflectionAuthoringOperation::Apply },
		{ TEXT("runtime_virtual_texture_export_json"), TEXT("runtime_virtual_texture"), EReflectionAuthoringOperation::Export },
		{ TEXT("runtime_virtual_texture_apply_json"), TEXT("runtime_virtual_texture"), EReflectionAuthoringOperation::Apply },
		{ TEXT("render_target_export_json"), TEXT("render_target"), EReflectionAuthoringOperation::Export },
		{ TEXT("render_target_validate_json"), TEXT("render_target"), EReflectionAuthoringOperation::Validate },
		{ TEXT("render_target_apply_json"), TEXT("render_target"), EReflectionAuthoringOperation::Apply },
		{ TEXT("media_source_export_json"), TEXT("media_source"), EReflectionAuthoringOperation::Export },
		{ TEXT("media_source_apply_json"), TEXT("media_source"), EReflectionAuthoringOperation::Apply },
		{ TEXT("media_player_export_json"), TEXT("media_player"), EReflectionAuthoringOperation::Export },
		{ TEXT("media_player_apply_json"), TEXT("media_player"), EReflectionAuthoringOperation::Apply },
		{ TEXT("media_texture_export_json"), TEXT("media_texture"), EReflectionAuthoringOperation::Export },
		{ TEXT("media_texture_apply_json"), TEXT("media_texture"), EReflectionAuthoringOperation::Apply },
		{ TEXT("sparse_volume_texture_export_json"), TEXT("sparse_volume_texture"), EReflectionAuthoringOperation::Export },
		{ TEXT("sparse_volume_texture_apply_json"), TEXT("sparse_volume_texture"), EReflectionAuthoringOperation::Apply },
		{ TEXT("texture_graph_export_folder"), TEXT("texture_graph"), EReflectionAuthoringOperation::Export },
		{ TEXT("texture_graph_validate_folder"), TEXT("texture_graph"), EReflectionAuthoringOperation::Validate },
		{ TEXT("texture_graph_apply_folder"), TEXT("texture_graph"), EReflectionAuthoringOperation::Apply },
		{ TEXT("subuv_animation_export_json"), TEXT("subuv_animation"), EReflectionAuthoringOperation::Export },
		{ TEXT("subuv_animation_apply_json"), TEXT("subuv_animation"), EReflectionAuthoringOperation::Apply },
		{ TEXT("paper_sprite_export_json"), TEXT("paper_sprite"), EReflectionAuthoringOperation::Export },
		{ TEXT("paper_sprite_apply_json"), TEXT("paper_sprite"), EReflectionAuthoringOperation::Apply },
		{ TEXT("paper_flipbook_export_json"), TEXT("paper_flipbook"), EReflectionAuthoringOperation::Export },
		{ TEXT("paper_flipbook_apply_json"), TEXT("paper_flipbook"), EReflectionAuthoringOperation::Apply },
		{ TEXT("paper_tileset_export_folder"), TEXT("paper_tileset"), EReflectionAuthoringOperation::Export },
		{ TEXT("paper_tileset_apply_folder"), TEXT("paper_tileset"), EReflectionAuthoringOperation::Apply },
		{ TEXT("paper_tilemap_export_folder"), TEXT("paper_tilemap"), EReflectionAuthoringOperation::Export },
		{ TEXT("paper_tilemap_apply_folder"), TEXT("paper_tilemap"), EReflectionAuthoringOperation::Apply },
		{ TEXT("texture_collection_export_json"), TEXT("texture_collection"), EReflectionAuthoringOperation::Export },
		{ TEXT("texture_collection_apply_json"), TEXT("texture_collection"), EReflectionAuthoringOperation::Apply }
	};

	static const FReflectionAuthoringCommandEntry* FindReflectionAuthoringCommand(const FString& CommandLower)
	{
		for (const FReflectionAuthoringCommandEntry& Entry : GReflectionAuthoringCommandEntries)
		{
			if (CommandLower == Entry.Command)
			{
				return &Entry;
			}
		}
		return nullptr;
	}
}

bool GetReferencesForAsset(const FString& AssetPath, TArray<TSharedPtr<FJsonValue>>& OutReferencers);

bool FUeAgentLegacyMechanics::ExecuteTextureRenderTargetCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (const FReflectionAuthoringCommandEntry* Entry = FindReflectionAuthoringCommand(CommandLower))
	{
		switch (Entry->Operation)
		{
		case EReflectionAuthoringOperation::Export:
			return UeAgentReflectionAuthoringCommandBridge::Export(Entry->TypeId, Ctx.Params, OutData, OutError);
		case EReflectionAuthoringOperation::Apply:
			return UeAgentReflectionAuthoringCommandBridge::Apply(Entry->TypeId, Ctx.Params, OutData, OutError, Ctx.RequestId);
		case EReflectionAuthoringOperation::Validate:
			return UeAgentReflectionAuthoringCommandBridge::Validate(Entry->TypeId, Ctx.Params, OutData, OutError);
		default:
			OutError = TEXT("unsupported_reflection_authoring_operation");
			return false;
		}
	}
	if (const FTextureCommandHandler Handler = FindTextureCommandHandler(CommandLower))
	{
		return (this->*Handler)(Ctx, OutData, OutError);
	}
	OutError = TEXT("unknown_texture_render_target_command");
	return false;
}

bool FUeAgentLegacyMechanics::CmdTextureGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTexture* Texture = LoadAsset<UTexture>(AssetPath);
	if (!Texture)
	{
		OutError = TEXT("texture_not_found");
		return false;
	}
	bool bIncludeSource = true;
	bool bIncludePixelStats = false;
	bool bIncludeBuildSettings = false;
	bool bIncludePlatformData = false;
	GetOptionalBool(Ctx.Params, TEXT("include_source"), bIncludeSource, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_pixel_stats"), bIncludePixelStats, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_build_settings"), bIncludeBuildSettings, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_platform_data"), bIncludePlatformData, OutError);
	OutData = BuildTextureInfo(Texture, bIncludeSource, bIncludePixelStats);
	OutData->SetBoolField(TEXT("include_build_settings"), bIncludeBuildSettings);
	OutData->SetBoolField(TEXT("include_platform_data"), bIncludePlatformData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdTextureReimport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTexture* Texture = LoadAsset<UTexture>(AssetPath);
	if (!Texture)
	{
		OutError = TEXT("texture_not_found");
		return false;
	}

	TSharedPtr<FJsonObject> BeforeInfo = BuildTextureInfo(Texture, true, false);
	OutData = MakeShared<FJsonObject>();
	OutData->SetObjectField(TEXT("before"), BeforeInfo);
	OutData->SetObjectField(TEXT("size_before"), BeforeInfo->HasTypedField<EJson::Object>(TEXT("source")) ? BeforeInfo->GetObjectField(TEXT("source")) : MakeShared<FJsonObject>());
	OutData->SetStringField(TEXT("format_before"), BeforeInfo->HasTypedField<EJson::String>(TEXT("pixel_format")) ? BeforeInfo->GetStringField(TEXT("pixel_format")) : FString());

	FString SourceFilename;
	if (JsonTryGetString(Ctx.Params, TEXT("source_filename"), SourceFilename) && !SourceFilename.TrimStartAndEnd().IsEmpty())
	{
		SourceFilename = ResolveFilePath(SourceFilename);
		if (!FPaths::FileExists(SourceFilename))
		{
			OutError = TEXT("source_file_not_found");
			return false;
		}
		const FString DestinationPath = FPackageName::GetLongPackagePath(NormalizeAssetPath(Texture->GetOutermost()->GetName()));
		const FString DestinationName = FPackageName::GetLongPackageAssetName(Texture->GetOutermost()->GetName());

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_filename"), SourceFilename);
		Params->SetStringField(TEXT("destination_path"), DestinationPath);
		Params->SetStringField(TEXT("destination_name"), DestinationName);
		Params->SetBoolField(TEXT("replace_existing"), true);
		bool bReplaceExistingSettings = true;
		GetOptionalBool(Ctx.Params, TEXT("replace_existing_settings"), bReplaceExistingSettings, OutError);
		Params->SetBoolField(TEXT("replace_existing_settings"), bReplaceExistingSettings);
		bool bSaveAfterReimport = true;
		GetOptionalBool(Ctx.Params, TEXT("save_after_reimport"), bSaveAfterReimport, OutError);
		Params->SetBoolField(TEXT("save_after_import"), bSaveAfterReimport);
		FUeAgentRequestContext ImportCtx = Ctx;
		ImportCtx.Command = TEXT("asset_import_texture");
		ImportCtx.Params = Params;
		TSharedPtr<FJsonObject> ImportData = MakeShared<FJsonObject>();
		if (!CmdAssetImportTexture(ImportCtx, ImportData, OutError))
		{
			OutData->SetObjectField(TEXT("import_attempt"), ImportData);
			return false;
		}
		Texture = LoadAsset<UTexture>(AssetPath);
		OutData->SetObjectField(TEXT("import_result"), ImportData);
	}
	else
	{
		if (!FReimportManager::Instance()->Reimport(Texture, true, true))
		{
			OutError = TEXT("reimport_failed");
			return false;
		}
		bool bSaveAfterReimport = true;
		GetOptionalBool(Ctx.Params, TEXT("save_after_reimport"), bSaveAfterReimport, OutError);
		if (bSaveAfterReimport && !SavePackagesForObjects({ Texture }, OutError))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> AfterInfo = BuildTextureInfo(Texture, true, false);
	OutData->SetBoolField(TEXT("reimported"), true);
	OutData->SetObjectField(TEXT("after"), AfterInfo);
	OutData->SetObjectField(TEXT("size_after"), AfterInfo->HasTypedField<EJson::Object>(TEXT("source")) ? AfterInfo->GetObjectField(TEXT("source")) : MakeShared<FJsonObject>());
	OutData->SetStringField(TEXT("format_after"), AfterInfo->HasTypedField<EJson::String>(TEXT("pixel_format")) ? AfterInfo->GetStringField(TEXT("pixel_format")) : FString());
	OutData->SetArrayField(TEXT("warnings"), {});
	return true;
}

bool FUeAgentLegacyMechanics::CmdTextureValidateSettings(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTexture* Texture = LoadAsset<UTexture>(AssetPath);
	if (!Texture)
	{
		OutError = TEXT("texture_not_found");
		return false;
	}
	FString Usage = TEXT("color");
	JsonTryGetString(Ctx.Params, TEXT("expected_usage"), Usage);
	Usage = Usage.ToLower();
	bool bStrict = false;
	GetOptionalBool(Ctx.Params, TEXT("strict"), bStrict, OutError);

	TArray<TSharedPtr<FJsonValue>> Issues;
	if (Usage == TEXT("normal"))
	{
		if (Texture->SRGB)
		{
			AddIssue(Issues, TEXT("normal_map_srgb_enabled"), bStrict ? TEXT("error") : TEXT("warning"), TEXT("Normal maps should normally use sRGB=false."));
		}
		if (Texture->CompressionSettings != TC_Normalmap)
		{
			AddIssue(Issues, TEXT("normal_map_compression_mismatch"), bStrict ? TEXT("error") : TEXT("warning"), TEXT("Normal maps should normally use TC_Normalmap."));
		}
	}
	else if (Usage == TEXT("mask") || Usage == TEXT("data"))
	{
		if (Texture->SRGB)
		{
			AddIssue(Issues, TEXT("data_texture_srgb_enabled"), bStrict ? TEXT("error") : TEXT("warning"), TEXT("Mask/data textures should normally use sRGB=false."));
		}
	}
	else if (Usage == TEXT("ui"))
	{
		if (!Texture->SRGB)
		{
			AddIssue(Issues, TEXT("ui_texture_srgb_disabled"), bStrict ? TEXT("error") : TEXT("warning"), TEXT("UI textures should normally use sRGB=true."));
		}
		if (Texture->LODGroup != TEXTUREGROUP_UI)
		{
			AddIssue(Issues, TEXT("ui_lod_group_mismatch"), TEXT("warning"), TEXT("UI textures should normally use TEXTUREGROUP_UI."));
		}
	}

	const TSharedPtr<FJsonObject>* ExpectedSettings = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("expected_settings"), ExpectedSettings) && ExpectedSettings && ExpectedSettings->IsValid())
	{
		if ((*ExpectedSettings)->HasField(TEXT("srgb")))
		{
			bool bExpectedSRGB = false;
			if ((*ExpectedSettings)->TryGetBoolField(TEXT("srgb"), bExpectedSRGB) && ((Texture->SRGB != 0) != bExpectedSRGB))
			{
				AddIssue(Issues, TEXT("srgb_expected_mismatch"), bStrict ? TEXT("error") : TEXT("warning"), TEXT("Texture sRGB does not match expected_settings.srgb."));
			}
		}
		FString ExpectedCompression;
		if ((*ExpectedSettings)->TryGetStringField(TEXT("compression_settings"), ExpectedCompression))
		{
			const FString Actual = EnumToString<TextureCompressionSettings>(static_cast<TextureCompressionSettings>(Texture->CompressionSettings.GetValue()));
			if (!Actual.Equals(ExpectedCompression, ESearchCase::IgnoreCase))
			{
				AddIssue(Issues, TEXT("compression_expected_mismatch"), bStrict ? TEXT("error") : TEXT("warning"), TEXT("Texture compression does not match expected_settings.compression_settings."));
			}
		}
	}

	bool bPassed = true;
	for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
	{
		const TSharedPtr<FJsonObject>* Issue = nullptr;
		FString Severity;
		if (IssueValue->TryGetObject(Issue) && Issue && (*Issue)->TryGetStringField(TEXT("severity"), Severity) && Severity == TEXT("error"))
		{
			bPassed = false;
			break;
		}
	}

	OutData = BuildTextureInfo(Texture, true, false);
	OutData->SetStringField(TEXT("expected_usage"), Usage);
	OutData->SetBoolField(TEXT("validation_passed"), bPassed);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.texture_validate_settings.v1")));
	return true;
}

bool FUeAgentLegacyMechanics::CmdTexturePixelProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTexture* Texture = LoadAsset<UTexture>(AssetPath);
	if (!Texture)
	{
		OutError = TEXT("texture_not_found");
		return false;
	}
	int32 MipIndex = 0;
	int32 SliceIndex = 0;
	GetOptionalInt(Ctx.Params, TEXT("mip_index"), MipIndex, OutError);
	GetOptionalInt(Ctx.Params, TEXT("slice"), SliceIndex, OutError);
	FImage Image;
	if (!ReadTextureSourceImage(Texture, MipIndex, SliceIndex, Image, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Texture, OutData);
	TArray<FColor> FullColors;
	ImageToColors(Image, FullColors);
	TArray<FColor> StatColors = FullColors;
	FIntPoint StatSize(Image.SizeX, Image.SizeY);
	FIntRect Rect(0, 0, Image.SizeX, Image.SizeY);
	const TSharedPtr<FJsonObject>* RectObj = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("rect"), RectObj) && RectObj && RectObj->IsValid())
	{
		double X = 0;
		double Y = 0;
		double W = Image.SizeX;
		double H = Image.SizeY;
		(*RectObj)->TryGetNumberField(TEXT("x"), X);
		(*RectObj)->TryGetNumberField(TEXT("y"), Y);
		(*RectObj)->TryGetNumberField(TEXT("w"), W);
		(*RectObj)->TryGetNumberField(TEXT("width"), W);
		(*RectObj)->TryGetNumberField(TEXT("h"), H);
		(*RectObj)->TryGetNumberField(TEXT("height"), H);
		Rect.Min.X = FMath::Clamp(static_cast<int32>(X), 0, Image.SizeX);
		Rect.Min.Y = FMath::Clamp(static_cast<int32>(Y), 0, Image.SizeY);
		Rect.Max.X = FMath::Clamp(Rect.Min.X + FMath::Max(1, static_cast<int32>(W)), Rect.Min.X, Image.SizeX);
		Rect.Max.Y = FMath::Clamp(Rect.Min.Y + FMath::Max(1, static_cast<int32>(H)), Rect.Min.Y, Image.SizeY);
		StatColors.Reset();
		StatColors.Reserve(Rect.Width() * Rect.Height());
		for (int32 Row = Rect.Min.Y; Row < Rect.Max.Y; ++Row)
		{
			for (int32 Col = Rect.Min.X; Col < Rect.Max.X; ++Col)
			{
				StatColors.Add(FullColors[Row * Image.SizeX + Col]);
			}
		}
		StatSize = FIntPoint(Rect.Width(), Rect.Height());
	}
	TSharedPtr<FJsonObject> RectJson = MakeShared<FJsonObject>();
	RectJson->SetNumberField(TEXT("x"), Rect.Min.X);
	RectJson->SetNumberField(TEXT("y"), Rect.Min.Y);
	RectJson->SetNumberField(TEXT("width"), StatSize.X);
	RectJson->SetNumberField(TEXT("height"), StatSize.Y);
	OutData->SetObjectField(TEXT("rect"), RectJson);
	TSharedPtr<FJsonObject> Stats = BuildPixelStatsFromColors(StatColors, StatSize);
	OutData->SetObjectField(TEXT("stats"), Stats);
	TArray<TSharedPtr<FJsonValue>> Samples;
	const TArray<TSharedPtr<FJsonValue>>* UvValues = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("uvs"), UvValues) && UvValues)
	{
		for (int32 Index = 0; Index < UvValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* UvObj = nullptr;
			double U = 0.5;
			double V = 0.5;
			if ((*UvValues)[Index].IsValid() && (*UvValues)[Index]->TryGetObject(UvObj) && UvObj && UvObj->IsValid())
			{
				(*UvObj)->TryGetNumberField(TEXT("u"), U);
				(*UvObj)->TryGetNumberField(TEXT("v"), V);
			}
			const int32 X = FMath::Clamp(static_cast<int32>(U * Image.SizeX), 0, Image.SizeX - 1);
			const int32 Y = FMath::Clamp(static_cast<int32>(V * Image.SizeY), 0, Image.SizeY - 1);
			TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
			Sample->SetNumberField(TEXT("index"), Index);
			Sample->SetNumberField(TEXT("u"), U);
			Sample->SetNumberField(TEXT("v"), V);
			Sample->SetObjectField(TEXT("color"), MakeColorJson(FullColors.IsValidIndex(Y * Image.SizeX + X) ? FullColors[Y * Image.SizeX + X].ReinterpretAsLinear() : FLinearColor::Black));
			Samples.Add(MakeShared<FJsonValueObject>(Sample));
		}
	}
	OutData->SetArrayField(TEXT("samples"), Samples);
	FString Channels = TEXT("rgba");
	JsonTryGetString(Ctx.Params, TEXT("channels"), Channels);
	OutData->SetStringField(TEXT("channels"), Channels);
	bool bProbePassed = true;
	const TSharedPtr<FJsonObject>* ExpectedStats = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("expected_stats"), ExpectedStats) && ExpectedStats && ExpectedStats->IsValid())
	{
		double MinNonBlack = 0;
		if ((*ExpectedStats)->TryGetNumberField(TEXT("min_non_black_pixel_count"), MinNonBlack) && Stats->GetNumberField(TEXT("non_black_pixel_count")) < MinNonBlack)
		{
			bProbePassed = false;
		}
		OutData->SetObjectField(TEXT("expected_stats"), *ExpectedStats);
	}
	OutData->SetBoolField(TEXT("probe_passed"), bProbePassed);
	OutData->SetNumberField(TEXT("mip_index"), MipIndex);
	OutData->SetNumberField(TEXT("slice"), SliceIndex);
	return true;
}

bool FUeAgentLegacyMechanics::CmdTextureExportImage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTexture* Texture = LoadAsset<UTexture>(AssetPath);
	if (!Texture)
	{
		OutError = TEXT("texture_not_found");
		return false;
	}
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	FString Format = TEXT("png");
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	Format = Format.ToLower();
	if (OutputFile.TrimStartAndEnd().IsEmpty())
	{
		OutputFile = MakeShotPath(Format);
	}
	else
	{
		OutputFile = ResolveFilePath(OutputFile);
	}
	int32 MipIndex = 0;
	int32 SliceIndex = 0;
	GetOptionalInt(Ctx.Params, TEXT("mip_index"), MipIndex, OutError);
	GetOptionalInt(Ctx.Params, TEXT("slice"), SliceIndex, OutError);
	GetOptionalInt(Ctx.Params, TEXT("face"), SliceIndex, OutError);
	FImage Image;
	if (!ReadTextureSourceImage(Texture, MipIndex, SliceIndex, Image, OutError))
	{
		return false;
	}
	TArray<FColor> Colors;
	ImageToColors(Image, Colors);
	if (!WriteCompressedImage(Colors, FIntPoint(Image.SizeX, Image.SizeY), Format, 90, OutputFile, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Texture, OutData);
	OutData->SetStringField(TEXT("file_path"), OutputFile);
	OutData->SetNumberField(TEXT("width"), Image.SizeX);
	OutData->SetNumberField(TEXT("height"), Image.SizeY);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("mip_index"), MipIndex);
	OutData->SetNumberField(TEXT("slice"), SliceIndex);
	OutData->SetNumberField(TEXT("face"), SliceIndex);
	OutData->SetNumberField(TEXT("bytes"), static_cast<double>(IFileManager::Get().FileSize(*OutputFile)));
	return true;
}

bool FUeAgentLegacyMechanics::CmdTextureScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdTextureExportImage(Ctx, OutData, OutError))
	{
		return false;
	}
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTexture* Texture = LoadAsset<UTexture>(AssetPath);
	FImage Image;
	FString PixelError;
	int32 MipIndex = 0;
	GetOptionalInt(Ctx.Params, TEXT("mip_index"), MipIndex, OutError);
	int32 MaxSize = 0;
	GetOptionalInt(Ctx.Params, TEXT("max_size"), MaxSize, OutError);
	OutData->SetNumberField(TEXT("mip_index"), MipIndex);
	OutData->SetNumberField(TEXT("max_size"), MaxSize);
	if (Texture && ReadTextureSourceImage(Texture, MipIndex, 0, Image, PixelError))
	{
		TSharedPtr<FJsonObject> Stats = BuildPixelStatsFromImage(Image);
		OutData->SetObjectField(TEXT("channel_stats"), Stats);
		OutData->SetBoolField(TEXT("appears_non_black"), Stats->GetNumberField(TEXT("non_black_pixel_count")) > 0);
	}
	else
	{
		OutData->SetBoolField(TEXT("appears_non_black"), false);
		OutData->SetStringField(TEXT("channel_stats_error"), PixelError);
	}
	bool bRequireNonBlack = false;
	GetOptionalBool(Ctx.Params, TEXT("require_non_black"), bRequireNonBlack, OutError);
	OutData->SetBoolField(TEXT("require_non_black"), bRequireNonBlack);
	if (bRequireNonBlack && !OutData->GetBoolField(TEXT("appears_non_black")))
	{
		OutError = TEXT("texture_screenshot_appears_black");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdTextureArrayCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	TArray<FString> SourcePaths;
	if (!ReadStringArray(Ctx.Params, TEXT("source_textures"), SourcePaths) || SourcePaths.Num() <= 0)
	{
		OutError = TEXT("missing_source_textures");
		return false;
	}
	TArray<UTexture2D*> Sources;
	for (const FString& SourcePath : SourcePaths)
	{
		UTexture2D* Source = LoadAsset<UTexture2D>(SourcePath);
		if (!Source)
		{
			OutError = TEXT("source_texture_not_found");
			return false;
		}
		Sources.Add(Source);
	}
	const int32 SizeX = Sources[0]->GetSizeX();
	const int32 SizeY = Sources[0]->GetSizeY();
	for (UTexture2D* Source : Sources)
	{
		if (Source->GetSizeX() != SizeX || Source->GetSizeY() != SizeY)
		{
			OutError = TEXT("source_texture_size_mismatch");
			return false;
		}
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UTexture2DArray* TextureArray = Cast<UTexture2DArray>(CreateAssetObject(UTexture2DArray::StaticClass(), AssetPath, bReplaceExisting, OutError));
	if (!TextureArray)
	{
		return false;
	}
	TextureArray->SourceTextures = Sources;
	TextureArray->bSourceGeneratedFromSourceTexturesArray = true;
	if (!TextureArray->UpdateSourceFromSourceTextures(!bReplaceExisting))
	{
		OutError = TEXT("texture_array_update_source_failed");
		return false;
	}
	TextureArray->PostEditChange();
	TextureArray->UpdateResource();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ TextureArray }, OutError))
	{
		return false;
	}
	OutData = BuildTextureInfo(TextureArray, true, false);
	OutData->SetNumberField(TEXT("slice_count"), Sources.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdTextureArrayExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Export(TEXT("texture_array"), Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdTextureArrayApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Apply(TEXT("texture_array"), Ctx.Params, OutData, OutError, Ctx.RequestId);
}

bool FUeAgentLegacyMechanics::CmdTextureCubeCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	TArray<FString> FacePaths;
	if (!ReadStringArray(Ctx.Params, TEXT("face_textures"), FacePaths))
	{
		const TSharedPtr<FJsonObject>* FacesObj = nullptr;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("faces"), FacesObj) && FacesObj && FacesObj->IsValid())
		{
			auto ReadFace = [&FacesObj](const TArray<const TCHAR*>& Keys, FString& OutFacePath) -> bool
			{
				for (const TCHAR* Key : Keys)
				{
					if (Key && (*FacesObj)->TryGetStringField(Key, OutFacePath) && !OutFacePath.TrimStartAndEnd().IsEmpty())
					{
						OutFacePath.TrimStartAndEndInline();
						return true;
					}
				}
				return false;
			};
			const TArray<TArray<const TCHAR*>> OrderedKeys = {
				{ TEXT("+X"), TEXT("px"), TEXT("positive_x"), TEXT("pos_x"), TEXT("right") },
				{ TEXT("-X"), TEXT("nx"), TEXT("negative_x"), TEXT("neg_x"), TEXT("left") },
				{ TEXT("+Y"), TEXT("py"), TEXT("positive_y"), TEXT("pos_y"), TEXT("up") },
				{ TEXT("-Y"), TEXT("ny"), TEXT("negative_y"), TEXT("neg_y"), TEXT("down") },
				{ TEXT("+Z"), TEXT("pz"), TEXT("positive_z"), TEXT("pos_z"), TEXT("front") },
				{ TEXT("-Z"), TEXT("nz"), TEXT("negative_z"), TEXT("neg_z"), TEXT("back") }
			};
			for (const TArray<const TCHAR*>& Keys : OrderedKeys)
			{
				FString FacePath;
				if (!ReadFace(Keys, FacePath))
				{
					FacePaths.Reset();
					break;
				}
				FacePaths.Add(FacePath);
			}
		}
	}
	if (FacePaths.Num() != 6)
	{
		OutError = TEXT("texture_cube_requires_six_faces");
		return false;
	}
	TArray<FImage> FaceImages;
	int32 SizeX = 0;
	int32 SizeY = 0;
	for (const FString& FacePath : FacePaths)
	{
		UTexture* FaceTexture = LoadAsset<UTexture>(FacePath);
		if (!FaceTexture)
		{
			OutError = TEXT("face_texture_not_found");
			return false;
		}
		FImage Image;
		if (!ReadTextureSourceImage(FaceTexture, 0, 0, Image, OutError))
		{
			return false;
		}
		if (FaceImages.Num() == 0)
		{
			SizeX = Image.SizeX;
			SizeY = Image.SizeY;
		}
		else if (Image.SizeX != SizeX || Image.SizeY != SizeY)
		{
			OutError = TEXT("face_texture_size_mismatch");
			return false;
		}
		FaceImages.Add(MoveTemp(Image));
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UTextureCube* Cube = Cast<UTextureCube>(CreateAssetObject(UTextureCube::StaticClass(), AssetPath, bReplaceExisting, OutError));
	if (!Cube)
	{
		return false;
	}
	TArray64<uint8> CombinedData;
	CombinedData.SetNum(static_cast<int64>(SizeX) * SizeY * 6 * sizeof(FColor));
	for (int32 FaceIndex = 0; FaceIndex < FaceImages.Num(); ++FaceIndex)
	{
		TArray<FColor> Colors;
		ImageToColors(FaceImages[FaceIndex], Colors);
		FMemory::Memcpy(CombinedData.GetData() + static_cast<int64>(FaceIndex) * SizeX * SizeY * sizeof(FColor), Colors.GetData(), static_cast<SIZE_T>(Colors.Num() * sizeof(FColor)));
	}
	Cube->Source.Init(SizeX, SizeY, 6, 1, TSF_BGRA8, CombinedData.GetData());
	Cube->SRGB = true;
	Cube->PostEditChange();
	Cube->UpdateResource();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Cube }, OutError))
	{
		return false;
	}
	OutData = BuildTextureInfo(Cube, true, false);
	OutData->SetNumberField(TEXT("face_count"), 6);
	return true;
}

bool FUeAgentLegacyMechanics::CmdTextureCubeExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Export(TEXT("texture_cube"), Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdTextureCubeApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Apply(TEXT("texture_cube"), Ctx.Params, OutData, OutError, Ctx.RequestId);
}

bool FUeAgentLegacyMechanics::CmdTextureCubeArrayCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	TArray<FString> SourcePaths;
	if (!ReadStringArray(Ctx.Params, TEXT("source_cubes"), SourcePaths) || SourcePaths.Num() <= 0)
	{
		OutError = TEXT("missing_source_cubes");
		return false;
	}
	TArray<UTextureCube*> Sources;
	for (const FString& SourcePath : SourcePaths)
	{
		UTextureCube* Source = LoadAsset<UTextureCube>(SourcePath);
		if (!Source)
		{
			OutError = TEXT("source_cube_not_found");
			return false;
		}
		Sources.Add(Source);
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UTextureCubeArray* CubeArray = Cast<UTextureCubeArray>(CreateAssetObject(UTextureCubeArray::StaticClass(), AssetPath, bReplaceExisting, OutError));
	if (!CubeArray)
	{
		return false;
	}
	CubeArray->SourceTextures = Sources;
	CubeArray->bSourceGeneratedFromSourceTexturesArray = true;
	if (!CubeArray->UpdateSourceFromSourceTextures(!bReplaceExisting))
	{
		OutError = TEXT("texture_cube_array_update_source_failed");
		return false;
	}
	CubeArray->PostEditChange();
	CubeArray->UpdateResource();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ CubeArray }, OutError))
	{
		return false;
	}
	OutData = BuildTextureInfo(CubeArray, true, false);
	OutData->SetNumberField(TEXT("cube_count"), Sources.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdTextureCubeArrayExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Export(TEXT("texture_cube_array"), Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdTextureCubeArrayApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Apply(TEXT("texture_cube_array"), Ctx.Params, OutData, OutError, Ctx.RequestId);
}

bool FUeAgentLegacyMechanics::CmdVolumeTextureCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	TArray<FString> SourceTexturePaths;
	const bool bHasSourceTextureArray = ReadStringArray(Ctx.Params, TEXT("source_textures"), SourceTexturePaths) && SourceTexturePaths.Num() > 0;
	FString SourceTexturePath;
	if (bHasSourceTextureArray)
	{
		SourceTexturePath = SourceTexturePaths[0];
	}
	else if (!GetRequiredString(Ctx, TEXT("source_texture"), SourceTexturePath, OutError))
	{
		return false;
	}
	UTexture2D* SourceTexture = LoadAsset<UTexture2D>(SourceTexturePath);
	if (!SourceTexture)
	{
		OutError = TEXT("source_texture_not_found");
		return false;
	}
	UTexture2D* GeneratedStackTexture = nullptr;
	if (bHasSourceTextureArray && SourceTexturePaths.Num() > 1)
	{
		TArray<FImage> SliceImages;
		int32 SliceSizeX = 0;
		int32 SliceSizeY = 0;
		for (const FString& SlicePath : SourceTexturePaths)
		{
			UTexture2D* SliceTexture = LoadAsset<UTexture2D>(SlicePath);
			if (!SliceTexture)
			{
				OutError = TEXT("source_texture_not_found");
				return false;
			}
			FImage SliceImage;
			if (!ReadTextureSourceImage(SliceTexture, 0, 0, SliceImage, OutError))
			{
				return false;
			}
			if (SliceImages.Num() == 0)
			{
				SliceSizeX = SliceImage.SizeX;
				SliceSizeY = SliceImage.SizeY;
			}
			else if (SliceImage.SizeX != SliceSizeX || SliceImage.SizeY != SliceSizeY)
			{
				OutError = TEXT("source_texture_size_mismatch");
				return false;
			}
			SliceImages.Add(MoveTemp(SliceImage));
		}
		const FString StackAssetPath = FString::Printf(TEXT("%s/%s_SourceStack"), *FPaths::GetPath(NormalizeAssetPath(AssetPath)), *FPackageName::GetLongPackageAssetName(NormalizeAssetPath(AssetPath)));
		GeneratedStackTexture = Cast<UTexture2D>(CreateAssetObject(UTexture2D::StaticClass(), StackAssetPath, true, OutError));
		if (!GeneratedStackTexture)
		{
			return false;
		}
		TArray<FColor> StackPixels;
		StackPixels.SetNum(SliceSizeX * SliceSizeY * SliceImages.Num());
		for (int32 SliceIndex = 0; SliceIndex < SliceImages.Num(); ++SliceIndex)
		{
			TArray<FColor> SlicePixels;
			ImageToColors(SliceImages[SliceIndex], SlicePixels);
			for (int32 Y = 0; Y < SliceSizeY; ++Y)
			{
				FMemory::Memcpy(
					&StackPixels[(SliceIndex * SliceSizeY + Y) * SliceSizeX],
					&SlicePixels[Y * SliceSizeX],
					static_cast<SIZE_T>(SliceSizeX * sizeof(FColor)));
			}
		}
		GeneratedStackTexture->Source.Init(SliceSizeX, SliceSizeY * SliceImages.Num(), 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(StackPixels.GetData()));
		GeneratedStackTexture->SRGB = SourceTexture->SRGB;
		GeneratedStackTexture->CompressionSettings = SourceTexture->CompressionSettings;
		GeneratedStackTexture->MipGenSettings = TMGS_NoMipmaps;
		GeneratedStackTexture->PostEditChange();
		GeneratedStackTexture->UpdateResource();
		SourceTexture = GeneratedStackTexture;
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UVolumeTexture* Volume = Cast<UVolumeTexture>(CreateAssetObject(UVolumeTexture::StaticClass(), AssetPath, bReplaceExisting, OutError));
	if (!Volume)
	{
		return false;
	}
	int32 TileSizeX = SourceTexture->GetSizeX();
	int32 TileSizeY = SourceTexture->GetSizeY();
	GetOptionalInt(Ctx.Params, TEXT("tile_size_x"), TileSizeX, OutError);
	GetOptionalInt(Ctx.Params, TEXT("tile_size_y"), TileSizeY, OutError);
	int32 Depth = bHasSourceTextureArray ? SourceTexturePaths.Num() : 1;
	GetOptionalInt(Ctx.Params, TEXT("depth"), Depth, OutError);
	if ((!Ctx.Params.IsValid() || !Ctx.Params->HasField(TEXT("tile_size_y"))) && Depth > 1 && SourceTexture->GetSizeY() % Depth == 0)
	{
		TileSizeY = SourceTexture->GetSizeY() / Depth;
	}
	Volume->Source2DTexture = SourceTexture;
	Volume->Source2DTileSizeX = FMath::Max(1, TileSizeX);
	Volume->Source2DTileSizeY = FMath::Max(1, TileSizeY);
	if (!Volume->UpdateSourceFromSourceTexture())
	{
		OutError = TEXT("volume_texture_update_source_failed");
		return false;
	}
	Volume->PostEditChange();
	Volume->UpdateResource();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	TArray<UObject*> ObjectsToSave = { Volume };
	if (GeneratedStackTexture)
	{
		ObjectsToSave.Add(GeneratedStackTexture);
	}
	if (bSave && !SavePackagesForObjects(ObjectsToSave, OutError))
	{
		return false;
	}
	OutData = BuildTextureInfo(Volume, true, false);
	OutData->SetNumberField(TEXT("size_x"), Volume->GetSizeX());
	OutData->SetNumberField(TEXT("size_y"), Volume->GetSizeY());
	OutData->SetNumberField(TEXT("size_z"), Volume->GetSizeZ());
	OutData->SetStringField(TEXT("source_texture"), NormalizeAssetPath(SourceTexture->GetOutermost()->GetName()));
	if (GeneratedStackTexture)
	{
		OutData->SetStringField(TEXT("source_texture_stack_asset"), NormalizeAssetPath(GeneratedStackTexture->GetOutermost()->GetName()));
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdVolumeTextureExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Export(TEXT("volume_texture"), Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdVolumeTextureApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Apply(TEXT("volume_texture"), Ctx.Params, OutData, OutError, Ctx.RequestId);
}

bool FUeAgentLegacyMechanics::CmdVirtualTextureGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	if (!CmdTextureGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	IConsoleVariable* VTVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VirtualTextures"));
	OutData->SetBoolField(TEXT("project_vt_enabled"), VTVar ? VTVar->GetInt() != 0 : false);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_project_settings"), true, OutError);
	OutData->SetBoolField(TEXT("virtual_texture_streaming"), OutData->HasTypedField<EJson::Boolean>(TEXT("virtual_texture_streaming")) ? OutData->GetBoolField(TEXT("virtual_texture_streaming")) : false);
	OutData->SetNumberField(TEXT("tile_size"), 0);
	TArray<TSharedPtr<FJsonValue>> Referencers;
	GetReferencesForAsset(Ctx.Params->GetStringField(TEXT("asset_path")), Referencers);
	OutData->SetArrayField(TEXT("referencers"), Referencers);
	return true;
}

bool FUeAgentLegacyMechanics::CmdVirtualTextureValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTexture* Texture = LoadAsset<UTexture>(AssetPath);
	if (!Texture)
	{
		OutError = TEXT("texture_not_found");
		return false;
	}
	bool bStrict = false;
	GetOptionalBool(Ctx.Params, TEXT("strict"), bStrict, OutError);
	TArray<TSharedPtr<FJsonValue>> Issues;
	bool bExpectedProjectVT = false;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetBoolField(TEXT("expected_project_vt_enabled"), bExpectedProjectVT))
	{
		IConsoleVariable* VTVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VirtualTextures"));
		const bool bProjectVT = VTVar ? VTVar->GetInt() != 0 : false;
		if (bProjectVT != bExpectedProjectVT)
		{
			AddIssue(Issues, TEXT("project_vt_enabled_mismatch"), bStrict ? TEXT("error") : TEXT("warning"), TEXT("Project virtual texture setting does not match expected_project_vt_enabled."));
		}
	}
	if (!Texture->VirtualTextureStreaming)
	{
		AddIssue(Issues, TEXT("virtual_texture_streaming_disabled"), bStrict ? TEXT("error") : TEXT("warning"), TEXT("Texture has VirtualTextureStreaming disabled."));
	}
	TArray<FString> ExpectedReferencers;
	ReadStringArray(Ctx.Params, TEXT("expected_referencers"), ExpectedReferencers);
	TArray<TSharedPtr<FJsonValue>> Referencers;
	GetReferencesForAsset(AssetPath, Referencers);
	for (const FString& ExpectedRef : ExpectedReferencers)
	{
		bool bFound = false;
		for (const TSharedPtr<FJsonValue>& Value : Referencers)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && (*Obj)->GetStringField(TEXT("package")).Contains(ExpectedRef))
			{
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			AddIssue(Issues, TEXT("expected_referencer_missing"), bStrict ? TEXT("error") : TEXT("warning"), FString::Printf(TEXT("Expected referencer not found: %s"), *ExpectedRef));
		}
	}
	OutData = BuildTextureInfo(Texture, true, false);
	OutData->SetArrayField(TEXT("referencers"), Referencers);
	OutData->SetBoolField(TEXT("validation_passed"), !(bStrict && Issues.Num() > 0));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.virtual_texture_validate_setup.v1")));
	return true;
}

bool FUeAgentLegacyMechanics::CmdRuntimeVirtualTextureCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	URuntimeVirtualTexture* RVT = Cast<URuntimeVirtualTexture>(CreateAssetObject(URuntimeVirtualTexture::StaticClass(), AssetPath, bReplaceExisting, OutError));
	if (!RVT)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("properties"), Properties) && Properties && !Properties->IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Results;
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("properties"), *Properties);
		if (!ApplyGenericProperties(RVT, Root, Results, OutError)) return false;
	}
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ RVT }, OutError))
	{
		return false;
	}
	return CmdRuntimeVirtualTextureGetInfo(Ctx, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdRuntimeVirtualTextureGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	URuntimeVirtualTexture* RVT = LoadAsset<URuntimeVirtualTexture>(AssetPath);
	if (!RVT)
	{
		OutError = TEXT("runtime_virtual_texture_not_found");
		return false;
	}
	OutData = BuildGenericAssetJson(RVT, TEXT("ue_agent_interface.runtime_virtual_texture.v1"), false);
	OutData->SetNumberField(TEXT("size"), RVT->GetSize());
	OutData->SetNumberField(TEXT("tile_count"), RVT->GetTileCount());
	OutData->SetNumberField(TEXT("tile_size"), RVT->GetTileSize());
	OutData->SetNumberField(TEXT("tile_border_size"), RVT->GetTileBorderSize());
	OutData->SetStringField(TEXT("material_type"), StaticEnum<ERuntimeVirtualTextureMaterialType>()->GetNameStringByValue(static_cast<int64>(RVT->GetMaterialType())));
	OutData->SetBoolField(TEXT("compress_textures"), RVT->GetCompressTextures());
	OutData->SetBoolField(TEXT("clear_textures"), RVT->GetClearTextures());
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_references"), true, OutError);
	TArray<TSharedPtr<FJsonValue>> Referencers;
	GetReferencesForAsset(AssetPath, Referencers);
	OutData->SetArrayField(TEXT("referencers"), Referencers);
	return true;
}

bool FUeAgentLegacyMechanics::CmdRuntimeVirtualTextureExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Export(TEXT("runtime_virtual_texture"), Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdRuntimeVirtualTextureApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Apply(TEXT("runtime_virtual_texture"), Ctx.Params, OutData, OutError, Ctx.RequestId);
}

bool FUeAgentLegacyMechanics::CmdRuntimeVirtualTextureValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	if (!CmdRuntimeVirtualTextureGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<FString> ExpectedComponents;
	ReadStringArray(Ctx.Params, TEXT("expected_components"), ExpectedComponents);
	TArray<FString> ExpectedMaterials;
	ReadStringArray(Ctx.Params, TEXT("expected_materials"), ExpectedMaterials);
	const TArray<TSharedPtr<FJsonValue>>* Referencers = nullptr;
	OutData->TryGetArrayField(TEXT("referencers"), Referencers);
	auto CheckExpected = [&Issues, Referencers](const TArray<FString>& ExpectedList, const FString& Code)
	{
		for (const FString& Expected : ExpectedList)
		{
			bool bFound = false;
			if (Referencers)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Referencers)
				{
					const TSharedPtr<FJsonObject>* Obj = nullptr;
					if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && (*Obj)->GetStringField(TEXT("package")).Contains(Expected))
					{
						bFound = true;
						break;
					}
				}
			}
			if (!bFound)
			{
				UeAgentTextureOps::AddIssue(Issues, Code, TEXT("warning"), FString::Printf(TEXT("Expected reference not found: %s"), *Expected));
			}
		}
	};
	CheckExpected(ExpectedComponents, TEXT("expected_component_missing"));
	CheckExpected(ExpectedMaterials, TEXT("expected_material_missing"));
	OutData->SetBoolField(TEXT("validation_passed"), Issues.Num() == 0);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), UeAgentTextureOps::MakeCoverageReport(TEXT("ue_agent_interface.runtime_virtual_texture_validate_setup.v1")));
	return true;
}
