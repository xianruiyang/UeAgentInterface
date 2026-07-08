static FString BuildPBIKEffectorDefaultText(const TSharedPtr<FJsonObject>& EffectorObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			EffectorObj,
			Path,
			{ TEXT("bone"), TEXT("bone_name"), TEXT("name"), TEXT("transform"), TEXT("position_alpha"), TEXT("rotation_alpha"), TEXT("strength_alpha"), TEXT("chain_depth"), TEXT("pull_chain_alpha"), TEXT("pin_rotation") },
			Issues);

		FString BoneName;
		TryGetStringFieldAny(EffectorObj, { TEXT("bone"), TEXT("bone_name"), TEXT("name") }, BoneName);
		if (BoneName.IsEmpty())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_ik_node_missing_bone"), Path, TEXT("PBIK effector requires bone/bone_name/name."));
		}

		FTransform Transform = FTransform::Identity;
		ReadTransformField(EffectorObj, TEXT("transform"), Path + TEXT(".transform"), Transform, Issues, false);

		double PositionAlpha = 1.0;
		double RotationAlpha = 1.0;
		double StrengthAlpha = 1.0;
		double ChainDepth = 0.0;
		double PullChainAlpha = 0.0;
		double PinRotation = 1.0;
		TryGetNumberFieldAny(EffectorObj, { TEXT("position_alpha"), TEXT("positionAlpha"), TEXT("PositionAlpha") }, PositionAlpha);
		TryGetNumberFieldAny(EffectorObj, { TEXT("rotation_alpha"), TEXT("rotationAlpha"), TEXT("RotationAlpha") }, RotationAlpha);
		TryGetNumberFieldAny(EffectorObj, { TEXT("strength_alpha"), TEXT("strengthAlpha"), TEXT("StrengthAlpha") }, StrengthAlpha);
		TryGetNumberFieldAny(EffectorObj, { TEXT("chain_depth"), TEXT("chainDepth"), TEXT("ChainDepth") }, ChainDepth);
		TryGetNumberFieldAny(EffectorObj, { TEXT("pull_chain_alpha"), TEXT("pullChainAlpha"), TEXT("PullChainAlpha") }, PullChainAlpha);
		TryGetNumberFieldAny(EffectorObj, { TEXT("pin_rotation"), TEXT("pinRotation"), TEXT("PinRotation") }, PinRotation);

		return FString::Printf(
			TEXT("(Bone=%s,Transform=%s,PositionAlpha=%s,RotationAlpha=%s,StrengthAlpha=%s,ChainDepth=%d,PullChainAlpha=%s,PinRotation=%s)"),
			*QuoteDefaultString(BoneName),
			*TransformDefaultText(Transform),
			*NumberDefaultText(PositionAlpha),
			*NumberDefaultText(RotationAlpha),
			*NumberDefaultText(StrengthAlpha),
			FMath::RoundToInt(ChainDepth),
			*NumberDefaultText(PullChainAlpha),
			*NumberDefaultText(PinRotation));
	}

	static FString BuildPBIKBoneSettingDefaultText(const TSharedPtr<FJsonObject>& BoneObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			BoneObj,
			Path,
			{
				TEXT("bone"), TEXT("bone_name"), TEXT("name"), TEXT("rotation_stiffness"), TEXT("position_stiffness"),
				TEXT("x"), TEXT("min_x"), TEXT("max_x"), TEXT("y"), TEXT("min_y"), TEXT("max_y"), TEXT("z"), TEXT("min_z"), TEXT("max_z"),
				TEXT("use_preferred_angles"), TEXT("preferred_angles"), TEXT("limits")
			},
			Issues);

		FString BoneName;
		TryGetStringFieldAny(BoneObj, { TEXT("bone"), TEXT("bone_name"), TEXT("name") }, BoneName);
		if (BoneName.IsEmpty())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_ik_node_missing_bone"), Path, TEXT("PBIK bone setting requires bone/bone_name/name."));
		}

		double RotationStiffness = 0.0;
		double PositionStiffness = 0.0;
		TryGetNumberFieldAny(BoneObj, { TEXT("rotation_stiffness"), TEXT("rotationStiffness"), TEXT("RotationStiffness") }, RotationStiffness);
		TryGetNumberFieldAny(BoneObj, { TEXT("position_stiffness"), TEXT("positionStiffness"), TEXT("PositionStiffness") }, PositionStiffness);

		FString X = TEXT("EPBIKLimitType::Free");
		FString Y = TEXT("EPBIKLimitType::Free");
		FString Z = TEXT("EPBIKLimitType::Free");
		FString AxisText;
		if (TryGetStringFieldAny(BoneObj, { TEXT("x"), TEXT("X") }, AxisText)) X = NormalizePBIKLimitType(AxisText);
		if (TryGetStringFieldAny(BoneObj, { TEXT("y"), TEXT("Y") }, AxisText)) Y = NormalizePBIKLimitType(AxisText);
		if (TryGetStringFieldAny(BoneObj, { TEXT("z"), TEXT("Z") }, AxisText)) Z = NormalizePBIKLimitType(AxisText);

		double MinX = 0.0;
		double MaxX = 0.0;
		double MinY = 0.0;
		double MaxY = 0.0;
		double MinZ = 0.0;
		double MaxZ = 0.0;
		TryGetNumberFieldAny(BoneObj, { TEXT("min_x"), TEXT("minX"), TEXT("MinX") }, MinX);
		TryGetNumberFieldAny(BoneObj, { TEXT("max_x"), TEXT("maxX"), TEXT("MaxX") }, MaxX);
		TryGetNumberFieldAny(BoneObj, { TEXT("min_y"), TEXT("minY"), TEXT("MinY") }, MinY);
		TryGetNumberFieldAny(BoneObj, { TEXT("max_y"), TEXT("maxY"), TEXT("MaxY") }, MaxY);
		TryGetNumberFieldAny(BoneObj, { TEXT("min_z"), TEXT("minZ"), TEXT("MinZ") }, MinZ);
		TryGetNumberFieldAny(BoneObj, { TEXT("max_z"), TEXT("maxZ"), TEXT("MaxZ") }, MaxZ);

		const TSharedPtr<FJsonObject>* LimitsObj = nullptr;
		if (BoneObj->TryGetObjectField(TEXT("limits"), LimitsObj) && LimitsObj && LimitsObj->IsValid())
		{
			auto ReadAxisLimit = [&](const FString& AxisName, FString& InOutLimitType, double& InOutMin, double& InOutMax)
			{
				const TSharedPtr<FJsonObject>* AxisObj = nullptr;
				if ((*LimitsObj)->TryGetObjectField(AxisName, AxisObj) && AxisObj && AxisObj->IsValid())
				{
					FString TypeText;
					if (TryGetStringFieldAny(*AxisObj, { TEXT("type"), TEXT("limit"), TEXT("mode") }, TypeText))
					{
						InOutLimitType = NormalizePBIKLimitType(TypeText, InOutLimitType);
					}
					TryGetNumberFieldAny(*AxisObj, { TEXT("min"), TEXT("minimum") }, InOutMin);
					TryGetNumberFieldAny(*AxisObj, { TEXT("max"), TEXT("maximum") }, InOutMax);
				}
			};
			ReadAxisLimit(TEXT("x"), X, MinX, MaxX);
			ReadAxisLimit(TEXT("y"), Y, MinY, MaxY);
			ReadAxisLimit(TEXT("z"), Z, MinZ, MaxZ);
		}

		bool bUsePreferredAngles = false;
		TryGetBoolFieldAny(BoneObj, { TEXT("use_preferred_angles"), TEXT("bUsePreferredAngles"), TEXT("UsePreferredAngles") }, bUsePreferredAngles);
		FVector PreferredAngles = FVector::ZeroVector;
		UeAgentJsonDiagnostics::ReadVectorField(BoneObj, TEXT("preferred_angles"), Path + TEXT(".preferred_angles"), PreferredAngles, Issues, false);

		return FString::Printf(
			TEXT("(Bone=%s,RotationStiffness=%s,PositionStiffness=%s,X=%s,MinX=%s,MaxX=%s,Y=%s,MinY=%s,MaxY=%s,Z=%s,MinZ=%s,MaxZ=%s,bUsePreferredAngles=%s,PreferredAngles=%s)"),
			*QuoteDefaultString(BoneName),
			*NumberDefaultText(RotationStiffness),
			*NumberDefaultText(PositionStiffness),
			*X,
			*NumberDefaultText(MinX),
			*NumberDefaultText(MaxX),
			*Y,
			*NumberDefaultText(MinY),
			*NumberDefaultText(MaxY),
			*Z,
			*NumberDefaultText(MinZ),
			*NumberDefaultText(MaxZ),
			*BoolDefaultText(bUsePreferredAngles),
			*VectorDefaultText(PreferredAngles));
	}

	static FString BuildPBIKSolverSettingsDefaultText(const TSharedPtr<FJsonObject>& SettingsObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (SettingsObj.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(
				SettingsObj,
				Path,
				{
					TEXT("iterations"), TEXT("sub_iterations"), TEXT("mass_multiplier"), TEXT("allow_stretch"), TEXT("root_behavior"),
					TEXT("pre_pull_root_settings"), TEXT("global_pull_chain_alpha"), TEXT("max_angle"), TEXT("over_relaxation")
				},
				Issues);
		}

		double Iterations = 20.0;
		double SubIterations = 0.0;
		double MassMultiplier = 1.0;
		bool bAllowStretch = false;
		FString RootBehavior = TEXT("EPBIKRootBehavior::PrePull");
		double GlobalPullChainAlpha = 1.0;
		double MaxAngle = 30.0;
		double OverRelaxation = 1.3;

		if (SettingsObj.IsValid())
		{
			TryGetNumberFieldAny(SettingsObj, { TEXT("iterations"), TEXT("Iterations") }, Iterations);
			TryGetNumberFieldAny(SettingsObj, { TEXT("sub_iterations"), TEXT("subIterations"), TEXT("SubIterations") }, SubIterations);
			TryGetNumberFieldAny(SettingsObj, { TEXT("mass_multiplier"), TEXT("massMultiplier"), TEXT("MassMultiplier") }, MassMultiplier);
			TryGetBoolFieldAny(SettingsObj, { TEXT("allow_stretch"), TEXT("bAllowStretch"), TEXT("AllowStretch") }, bAllowStretch);
			FString RootBehaviorText;
			if (TryGetStringFieldAny(SettingsObj, { TEXT("root_behavior"), TEXT("rootBehavior"), TEXT("RootBehavior") }, RootBehaviorText))
			{
				FString Token = RootBehaviorText.TrimStartAndEnd().ToLower();
				Token.ReplaceInline(TEXT("_"), TEXT(""));
				Token.ReplaceInline(TEXT("-"), TEXT(""));
				Token.RemoveFromStart(TEXT("epbikrootbehavior::"));
				if (Token == TEXT("pininput") || Token == TEXT("pintoinput"))
				{
					RootBehavior = TEXT("EPBIKRootBehavior::PinToInput");
				}
				else if (Token == TEXT("free"))
				{
					RootBehavior = TEXT("EPBIKRootBehavior::Free");
				}
				else
				{
					RootBehavior = TEXT("EPBIKRootBehavior::PrePull");
				}
			}
			TryGetNumberFieldAny(SettingsObj, { TEXT("global_pull_chain_alpha"), TEXT("globalPullChainAlpha"), TEXT("GlobalPullChainAlpha") }, GlobalPullChainAlpha);
			TryGetNumberFieldAny(SettingsObj, { TEXT("max_angle"), TEXT("maxAngle"), TEXT("MaxAngle") }, MaxAngle);
			TryGetNumberFieldAny(SettingsObj, { TEXT("over_relaxation"), TEXT("overRelaxation"), TEXT("OverRelaxation") }, OverRelaxation);
		}

		double PrePullRotationAlpha = 0.0;
		double PrePullRotationAlphaX = 1.0;
		double PrePullRotationAlphaY = 1.0;
		double PrePullRotationAlphaZ = 1.0;
		double PrePullPositionAlpha = 1.0;
		double PrePullPositionAlphaX = 1.0;
		double PrePullPositionAlphaY = 1.0;
		double PrePullPositionAlphaZ = 1.0;
		const TSharedPtr<FJsonObject>* PrePullObj = nullptr;
		if (SettingsObj.IsValid() && SettingsObj->TryGetObjectField(TEXT("pre_pull_root_settings"), PrePullObj) && PrePullObj && PrePullObj->IsValid())
		{
			TryGetNumberFieldAny(*PrePullObj, { TEXT("rotation_alpha"), TEXT("RotationAlpha") }, PrePullRotationAlpha);
			TryGetNumberFieldAny(*PrePullObj, { TEXT("rotation_alpha_x"), TEXT("RotationAlphaX") }, PrePullRotationAlphaX);
			TryGetNumberFieldAny(*PrePullObj, { TEXT("rotation_alpha_y"), TEXT("RotationAlphaY") }, PrePullRotationAlphaY);
			TryGetNumberFieldAny(*PrePullObj, { TEXT("rotation_alpha_z"), TEXT("RotationAlphaZ") }, PrePullRotationAlphaZ);
			TryGetNumberFieldAny(*PrePullObj, { TEXT("position_alpha"), TEXT("PositionAlpha") }, PrePullPositionAlpha);
			TryGetNumberFieldAny(*PrePullObj, { TEXT("position_alpha_x"), TEXT("PositionAlphaX") }, PrePullPositionAlphaX);
			TryGetNumberFieldAny(*PrePullObj, { TEXT("position_alpha_y"), TEXT("PositionAlphaY") }, PrePullPositionAlphaY);
			TryGetNumberFieldAny(*PrePullObj, { TEXT("position_alpha_z"), TEXT("PositionAlphaZ") }, PrePullPositionAlphaZ);
		}
		const FString PrePullText = FString::Printf(
			TEXT("(RotationAlpha=%s,RotationAlphaX=%s,RotationAlphaY=%s,RotationAlphaZ=%s,PositionAlpha=%s,PositionAlphaX=%s,PositionAlphaY=%s,PositionAlphaZ=%s)"),
			*NumberDefaultText(PrePullRotationAlpha),
			*NumberDefaultText(PrePullRotationAlphaX),
			*NumberDefaultText(PrePullRotationAlphaY),
			*NumberDefaultText(PrePullRotationAlphaZ),
			*NumberDefaultText(PrePullPositionAlpha),
			*NumberDefaultText(PrePullPositionAlphaX),
			*NumberDefaultText(PrePullPositionAlphaY),
			*NumberDefaultText(PrePullPositionAlphaZ));

		return FString::Printf(
			TEXT("(Iterations=%d,SubIterations=%d,MassMultiplier=%s,bAllowStretch=%s,RootBehavior=%s,PrePullRootSettings=%s,GlobalPullChainAlpha=%s,MaxAngle=%s,OverRelaxation=%s)"),
			FMath::RoundToInt(Iterations),
			FMath::RoundToInt(SubIterations),
			*NumberDefaultText(MassMultiplier),
			*BoolDefaultText(bAllowStretch),
			*RootBehavior,
			*PrePullText,
			*NumberDefaultText(GlobalPullChainAlpha),
			*NumberDefaultText(MaxAngle),
			*NumberDefaultText(OverRelaxation));
	}

	static FString BuildPBIKDebugDefaultText(const TSharedPtr<FJsonObject>& DebugObj)
	{
		double DrawScale = 1.0;
		bool bDrawDebug = false;
		if (DebugObj.IsValid())
		{
			TryGetNumberFieldAny(DebugObj, { TEXT("draw_scale"), TEXT("DrawScale") }, DrawScale);
			TryGetBoolFieldAny(DebugObj, { TEXT("draw_debug"), TEXT("bDrawDebug"), TEXT("DrawDebug") }, bDrawDebug);
		}
		return FString::Printf(TEXT("(DrawScale=%s,bDrawDebug=%s)"), *NumberDefaultText(DrawScale), *BoolDefaultText(bDrawDebug));
	}

	static FString BuildLegacyFBIKEffectorDefaultText(const TSharedPtr<FJsonObject>& EffectorObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		FString BoneName;
		TryGetStringFieldAny(EffectorObj, { TEXT("bone"), TEXT("bone_name"), TEXT("name") }, BoneName);
		if (BoneName.IsEmpty())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_ik_node_missing_bone"), Path, TEXT("FullbodyIK effector requires bone/bone_name/name."));
		}
		FTransform Transform = FTransform::Identity;
		ReadTransformField(EffectorObj, TEXT("transform"), Path + TEXT(".transform"), Transform, Issues, false);
		FVector Position = Transform.GetLocation();
		UeAgentJsonDiagnostics::ReadVectorField(EffectorObj, TEXT("position"), Path + TEXT(".position"), Position, Issues, false);
		FQuat Rotation = Transform.GetRotation();
		double PositionAlpha = 1.0;
		double RotationAlpha = 1.0;
		double PositionDepth = 1000.0;
		double RotationDepth = 1000.0;
		double Pull = 0.0;
		TryGetNumberFieldAny(EffectorObj, { TEXT("position_alpha"), TEXT("PositionAlpha") }, PositionAlpha);
		TryGetNumberFieldAny(EffectorObj, { TEXT("rotation_alpha"), TEXT("RotationAlpha") }, RotationAlpha);
		TryGetNumberFieldAny(EffectorObj, { TEXT("position_depth"), TEXT("PositionDepth") }, PositionDepth);
		TryGetNumberFieldAny(EffectorObj, { TEXT("rotation_depth"), TEXT("RotationDepth") }, RotationDepth);
		TryGetNumberFieldAny(EffectorObj, { TEXT("pull"), TEXT("Pull") }, Pull);
		return FString::Printf(
			TEXT("(Item=(Type=Bone,Name=%s),Position=%s,PositionAlpha=%s,PositionDepth=%d,Rotation=%s,RotationAlpha=%s,RotationDepth=%d,Pull=%s)"),
			*QuoteDefaultString(BoneName),
			*VectorDefaultText(Position),
			*NumberDefaultText(PositionAlpha),
			FMath::RoundToInt(PositionDepth),
			*QuatDefaultText(Rotation),
			*NumberDefaultText(RotationAlpha),
			FMath::RoundToInt(RotationDepth),
			*NumberDefaultText(Pull));
	}

	static FString BuildLegacyFBIKConstraintDefaultText(const TSharedPtr<FJsonObject>& ConstraintObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			ConstraintObj,
			Path,
			{
				TEXT("bone"), TEXT("bone_name"), TEXT("name"), TEXT("enabled"), TEXT("use_stiffness"), TEXT("linear_stiffness"), TEXT("angular_stiffness"),
				TEXT("use_angular_limit"), TEXT("angular_limit"), TEXT("use_pole_vector"), TEXT("pole_vector_option"), TEXT("pole_vector"), TEXT("offset_rotation")
			},
			Issues);

		FString BoneName;
		TryGetStringFieldAny(ConstraintObj, { TEXT("bone"), TEXT("bone_name"), TEXT("name") }, BoneName);
		if (BoneName.IsEmpty())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_ik_node_missing_bone"), Path, TEXT("FullbodyIK constraint requires bone/bone_name/name."));
		}

		bool bEnabled = true;
		bool bUseStiffness = true;
		bool bUseAngularLimit = false;
		bool bUsePoleVector = false;
		TryGetBoolFieldAny(ConstraintObj, { TEXT("enabled"), TEXT("bEnabled") }, bEnabled);
		TryGetBoolFieldAny(ConstraintObj, { TEXT("use_stiffness"), TEXT("bUseStiffness") }, bUseStiffness);
		TryGetBoolFieldAny(ConstraintObj, { TEXT("use_angular_limit"), TEXT("bUseAngularLimit") }, bUseAngularLimit);
		TryGetBoolFieldAny(ConstraintObj, { TEXT("use_pole_vector"), TEXT("bUsePoleVector") }, bUsePoleVector);

		FVector LinearStiffness(1.0, 1.0, 1.0);
		FVector AngularStiffness = FVector::ZeroVector;
		FVector PoleVector = FVector::RightVector;
		FRotator OffsetRotation = FRotator::ZeroRotator;
		UeAgentJsonDiagnostics::ReadVectorField(ConstraintObj, TEXT("linear_stiffness"), Path + TEXT(".linear_stiffness"), LinearStiffness, Issues, false);
		UeAgentJsonDiagnostics::ReadVectorField(ConstraintObj, TEXT("angular_stiffness"), Path + TEXT(".angular_stiffness"), AngularStiffness, Issues, false);
		UeAgentJsonDiagnostics::ReadVectorField(ConstraintObj, TEXT("pole_vector"), Path + TEXT(".pole_vector"), PoleVector, Issues, false);
		UeAgentJsonDiagnostics::ReadRotatorField(ConstraintObj, TEXT("offset_rotation"), Path + TEXT(".offset_rotation"), OffsetRotation, Issues, false);

		FString LimitX = TEXT("EFBIKBoneLimitType::Free");
		FString LimitY = TEXT("EFBIKBoneLimitType::Free");
		FString LimitZ = TEXT("EFBIKBoneLimitType::Free");
		FVector Limit = FVector::ZeroVector;
		const TSharedPtr<FJsonObject>* AngularLimitObj = nullptr;
		if (ConstraintObj->TryGetObjectField(TEXT("angular_limit"), AngularLimitObj) && AngularLimitObj && AngularLimitObj->IsValid())
		{
			FString AxisText;
			if (TryGetStringFieldAny(*AngularLimitObj, { TEXT("x"), TEXT("limit_type_x"), TEXT("LimitType_X") }, AxisText)) LimitX = NormalizeFBIKBoneLimitType(AxisText, LimitX);
			if (TryGetStringFieldAny(*AngularLimitObj, { TEXT("y"), TEXT("limit_type_y"), TEXT("LimitType_Y") }, AxisText)) LimitY = NormalizeFBIKBoneLimitType(AxisText, LimitY);
			if (TryGetStringFieldAny(*AngularLimitObj, { TEXT("z"), TEXT("limit_type_z"), TEXT("LimitType_Z") }, AxisText)) LimitZ = NormalizeFBIKBoneLimitType(AxisText, LimitZ);
			UeAgentJsonDiagnostics::ReadVectorField(*AngularLimitObj, TEXT("limit"), Path + TEXT(".angular_limit.limit"), Limit, Issues, false);
		}
		FString PoleOptionText;
		FString PoleOption = TEXT("EPoleVectorOption::Direction");
		if (TryGetStringFieldAny(ConstraintObj, { TEXT("pole_vector_option"), TEXT("PoleVectorOption") }, PoleOptionText))
		{
			PoleOption = NormalizePoleVectorOption(PoleOptionText);
		}
		const FString AngularLimitText = FString::Printf(
			TEXT("(LimitType_X=%s,LimitType_Y=%s,LimitType_Z=%s,Limit=%s)"),
			*LimitX,
			*LimitY,
			*LimitZ,
			*VectorDefaultText(Limit));

		return FString::Printf(
			TEXT("(Item=(Type=Bone,Name=%s),bEnabled=%s,bUseStiffness=%s,LinearStiffness=%s,AngularStiffness=%s,bUseAngularLimit=%s,AngularLimit=%s,bUsePoleVector=%s,PoleVectorOption=%s,PoleVector=%s,OffsetRotation=%s)"),
			*QuoteDefaultString(BoneName),
			*BoolDefaultText(bEnabled),
			*BoolDefaultText(bUseStiffness),
			*VectorDefaultText(LinearStiffness),
			*VectorDefaultText(AngularStiffness),
			*BoolDefaultText(bUseAngularLimit),
			*AngularLimitText,
			*BoolDefaultText(bUsePoleVector),
			*PoleOption,
			*VectorDefaultText(PoleVector),
			*RotatorDefaultText(OffsetRotation));
	}

	template <typename TEnum>
	static FString EnumToString(const TEnum Value)
	{
		if (const UEnum* Enum = StaticEnum<TEnum>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	template <typename TEnum>
	static bool ParseEnumString(const FString& InText, TEnum& OutValue)
	{
		const UEnum* Enum = StaticEnum<TEnum>();
		if (!Enum)
		{
			return false;
		}

		FString Text = InText.TrimStartAndEnd();
		int64 Value = Enum->GetValueByNameString(Text);
		if (Value == INDEX_NONE)
		{
			const FString EnumName = Enum->GetName();
			if (!Text.StartsWith(EnumName + TEXT("::")))
			{
				Value = Enum->GetValueByNameString(EnumName + TEXT("::") + Text);
			}
		}

		if (Value == INDEX_NONE)
		{
			for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
			{
				if (Enum->GetNameStringByIndex(Index).Equals(Text, ESearchCase::IgnoreCase))
				{
					Value = Enum->GetValueByIndex(Index);
					break;
				}
			}
		}

		if (Value == INDEX_NONE)
		{
			return false;
		}

		OutValue = static_cast<TEnum>(Value);
		return true;
	}

	static ERigElementType ParseElementType(const FString& InText, const ERigElementType DefaultType)
	{
		FString Text = InText.TrimStartAndEnd().ToLower();
		Text.ReplaceInline(TEXT("_"), TEXT(""));
		Text.ReplaceInline(TEXT("-"), TEXT(""));
		if (Text == TEXT("bone"))
		{
			return ERigElementType::Bone;
		}
		if (Text == TEXT("null"))
		{
			return ERigElementType::Null;
		}
		if (Text == TEXT("control"))
		{
			return ERigElementType::Control;
		}
		if (Text == TEXT("curve"))
		{
			return ERigElementType::Curve;
		}
		if (Text == TEXT("connector"))
		{
			return ERigElementType::Connector;
		}
		if (Text == TEXT("socket"))
		{
			return ERigElementType::Socket;
		}
		return DefaultType;
	}

	FString ElementTypeToString(const ERigElementType Type)
	{
		switch (Type)
		{
		case ERigElementType::Bone: return TEXT("Bone");
		case ERigElementType::Null: return TEXT("Null");
		case ERigElementType::Control: return TEXT("Control");
		case ERigElementType::Curve: return TEXT("Curve");
		case ERigElementType::Connector: return TEXT("Connector");
		case ERigElementType::Socket: return TEXT("Socket");
		default: return TEXT("None");
		}
	}

	static FRigElementKey ReadParentKey(const TSharedPtr<FJsonObject>& Obj, const ERigElementType DefaultType)
	{
		const TSharedPtr<FJsonValue>* ParentValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, TEXT("parent"));
		if (!ParentValue)
		{
			FString ParentName;
			if (Obj->TryGetStringField(TEXT("parent_name"), ParentName) && !ParentName.IsEmpty())
			{
				FString ParentType;
				Obj->TryGetStringField(TEXT("parent_type"), ParentType);
				return FRigElementKey(FName(*ParentName), ParseElementType(ParentType, DefaultType));
			}
			return FRigElementKey();
		}

		FString ParentString;
		if ((*ParentValue)->TryGetString(ParentString) && !ParentString.IsEmpty())
		{
			return FRigElementKey(FName(*ParentString), DefaultType);
		}

		const TSharedPtr<FJsonObject>* ParentObj = nullptr;
		if ((*ParentValue)->TryGetObject(ParentObj) && ParentObj && ParentObj->IsValid())
		{
			FString ParentName;
			FString ParentType;
			(*ParentObj)->TryGetStringField(TEXT("name"), ParentName);
			(*ParentObj)->TryGetStringField(TEXT("type"), ParentType);
			if (!ParentName.IsEmpty())
			{
				return FRigElementKey(FName(*ParentName), ParseElementType(ParentType, DefaultType));
			}
		}
		return FRigElementKey();
	}

	static TSharedPtr<FJsonObject> BuildParentJson(const URigHierarchy* Hierarchy, const FRigElementKey& Key)
	{
		TSharedPtr<FJsonObject> ParentObj = MakeShared<FJsonObject>();
		if (!Hierarchy)
		{
			return ParentObj;
		}

		const FRigElementKey ParentKey = Hierarchy->GetFirstParent(Key);
		if (ParentKey.IsValid())
		{
			ParentObj->SetStringField(TEXT("name"), ParentKey.Name.ToString());
			ParentObj->SetStringField(TEXT("type"), ElementTypeToString(ParentKey.Type));
		}
		return ParentObj;
	}

	static TSharedPtr<FJsonObject> BuildShapeDefinitionJson(const FControlRigShapeDefinition& Shape)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Shape.ShapeName.ToString());
		Obj->SetStringField(TEXT("shape_name"), Shape.ShapeName.ToString());
		Obj->SetStringField(TEXT("static_mesh"), Shape.StaticMesh.ToSoftObjectPath().ToString());
		Obj->SetObjectField(TEXT("transform"), MakeTransformJson(Shape.Transform));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Obj,
			TEXT("shape_name"),
			Shape.ShapeName.ToString(),
			Shape.ShapeName.ToString(),
			TEXT("Control Rig/Shape Library"),
			TEXT("control_rig_shape"),
			TEXT("shape_definition"),
			{ TEXT("set"), TEXT("remove") });
		return Obj;
	}

	bool ApplyShapeDefinitionJson(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Path,
		FControlRigShapeDefinition& InOutShape,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Obj.IsValid())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_field_type_mismatch"), Path, TEXT("Expected shape definition object."));
			return false;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("name"), TEXT("shape_name"), TEXT("static_mesh"), TEXT("transform"), TEXT("selector"), TEXT("display_name"), TEXT("category_path"), TEXT("control_type"), TEXT("value_kind"), TEXT("availability"), TEXT("operations") }, Issues);

		FString ShapeName;
		if (Obj->TryGetStringField(TEXT("shape_name"), ShapeName) || Obj->TryGetStringField(TEXT("name"), ShapeName))
		{
			if (ShapeName.TrimStartAndEnd().IsEmpty())
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_empty_required_field"), Path + TEXT(".shape_name"), TEXT("Shape name cannot be empty."));
			}
			else
			{
				InOutShape.ShapeName = FName(*ShapeName);
			}
		}

		FString StaticMeshPath;
		if (Obj->TryGetStringField(TEXT("static_mesh"), StaticMeshPath))
		{
			if (StaticMeshPath.TrimStartAndEnd().IsEmpty())
			{
				InOutShape.StaticMesh = nullptr;
			}
			else if (UStaticMesh* StaticMesh = LoadAsset<UStaticMesh>(StaticMeshPath))
			{
				InOutShape.StaticMesh = StaticMesh;
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("asset_reference_not_found"), Path + TEXT(".static_mesh"), FString::Printf(TEXT("Static mesh '%s' could not be loaded."), *StaticMeshPath));
			}
		}

		FTransform ShapeTransform = InOutShape.Transform;
		if (ReadTransformField(Obj, TEXT("transform"), Path + TEXT(".transform"), ShapeTransform, Issues, false))
		{
			InOutShape.Transform = ShapeTransform;
		}
		return !HasBlockingIssue(Issues);
	}

	TSharedPtr<FJsonObject> BuildShapeLibraryInfo(UControlRigShapeLibrary* Library)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Library)
		{
			return Out;
		}

		const FString AssetPath = NormalizeAssetPath(Library->GetPathName());
		Out->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig_shape_library.v1"));
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetStringField(TEXT("object_path"), Library->GetPathName());
		Out->SetStringField(TEXT("asset_class"), Library->GetClass()->GetPathName());
		Out->SetObjectField(TEXT("default_shape"), BuildShapeDefinitionJson(Library->DefaultShape));
		Out->SetStringField(TEXT("default_material"), Library->DefaultMaterial.ToSoftObjectPath().ToString());
		Out->SetStringField(TEXT("xray_material"), Library->XRayMaterial.ToSoftObjectPath().ToString());
		Out->SetStringField(TEXT("material_color_parameter"), Library->MaterialColorParameter.ToString());

		TArray<TSharedPtr<FJsonValue>> ShapeValues;
		for (const FControlRigShapeDefinition& Shape : Library->Shapes)
		{
			ShapeValues.Add(MakeShared<FJsonValueObject>(BuildShapeDefinitionJson(Shape)));
		}
		Out->SetArrayField(TEXT("shapes"), ShapeValues);
		Out->SetNumberField(TEXT("shape_count"), ShapeValues.Num());
		return Out;
	}

	UControlRigShapeLibrary* CreateShapeLibraryAsset(const FString& AssetPath, FString& OutError)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}
		if (LoadAssetObject(NormalizedAssetPath))
		{
			OutError = TEXT("asset_already_exists");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(NormalizedAssetPath);
		UPackage* Package = CreatePackage(*NormalizedAssetPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UControlRigShapeLibrary* Library = NewObject<UControlRigShapeLibrary>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (!Library)
		{
			OutError = TEXT("create_asset_failed");
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(Library);
		Package->MarkPackageDirty();
		return Library;
	}

	UControlRigBlueprint* CreateControlRigAsset(
		const FString& AssetPath,
		const bool bModularRig,
		const FString& PreviewSkeletalMeshPath,
		const bool bImportHierarchyFromPreview,
		FString& OutError)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}
		if (LoadAssetObject(NormalizedAssetPath))
		{
			OutError = TEXT("asset_already_exists");
			return nullptr;
		}

		UControlRigBlueprint* Blueprint = UControlRigBlueprintFactory::CreateNewControlRigAsset(NormalizedAssetPath, bModularRig);
		if (!Blueprint)
		{
			OutError = TEXT("create_control_rig_failed");
			return nullptr;
		}

		if (!PreviewSkeletalMeshPath.TrimStartAndEnd().IsEmpty())
		{
			USkeletalMesh* PreviewMesh = LoadAsset<USkeletalMesh>(PreviewSkeletalMeshPath);
			if (!PreviewMesh)
			{
				OutError = TEXT("preview_skeletal_mesh_not_found");
				return nullptr;
			}
			Blueprint->SetPreviewMesh(PreviewMesh, true);
			if (bImportHierarchyFromPreview && Blueprint->GetHierarchyController())
			{
				Blueprint->GetHierarchyController()->ImportBonesFromSkeletalMesh(PreviewMesh, NAME_None, true, true, false, false, false);
			}
		}

		Blueprint->MarkPackageDirty();
		return Blueprint;
	}

	static FString ControlRigTypeToString(const EControlRigType Type)
	{
		switch (Type)
		{
		case EControlRigType::IndependentRig: return TEXT("independent_rig");
		case EControlRigType::RigModule: return TEXT("rig_module");
		case EControlRigType::ModularRig: return TEXT("modular_rig");
		default: return TEXT("invalid");
		}
	}

	bool ParseControlRigTypeText(const FString& InText, EControlRigType& OutType)
	{
		FString Text = InText.TrimStartAndEnd().ToLower();
		Text.ReplaceInline(TEXT("-"), TEXT("_"));
		if (Text.IsEmpty() || Text == TEXT("independent") || Text == TEXT("independent_rig") || Text == TEXT("standalone"))
		{
			OutType = EControlRigType::IndependentRig;
			return true;
		}
		if (Text == TEXT("rig_module") || Text == TEXT("module"))
		{
			OutType = EControlRigType::RigModule;
			return true;
		}
		if (Text == TEXT("modular") || Text == TEXT("modular_rig"))
		{
			OutType = EControlRigType::ModularRig;
			return true;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildControlSettingsJson(const FRigControlSettings& Settings)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("animation_type"), EnumToString(Settings.AnimationType));
		Obj->SetStringField(TEXT("control_type"), EnumToString(Settings.ControlType));
		Obj->SetStringField(TEXT("display_name"), Settings.DisplayName.ToString());
		Obj->SetStringField(TEXT("primary_axis"), EnumToString(Settings.PrimaryAxis));
		Obj->SetBoolField(TEXT("shape_visible"), Settings.bShapeVisible);
		Obj->SetStringField(TEXT("shape_visibility"), EnumToString(Settings.ShapeVisibility));
		Obj->SetStringField(TEXT("shape_name"), Settings.ShapeName.ToString());
		Obj->SetObjectField(TEXT("shape_color"), MakeColorJson(Settings.ShapeColor));
		Obj->SetBoolField(TEXT("transient_control"), Settings.bIsTransientControl);
		Obj->SetBoolField(TEXT("group_with_parent_control"), Settings.bGroupWithParentControl);
		Obj->SetBoolField(TEXT("restrict_space_switching"), Settings.bRestrictSpaceSwitching);
		Obj->SetBoolField(TEXT("use_preferred_rotation_order"), Settings.bUsePreferredRotationOrder);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Obj,
			TEXT("settings"),
			TEXT("control_settings"),
			TEXT(""),
			TEXT("Control Rig/Controls"),
			TEXT("control_rig_control_settings"),
			TEXT("settings"),
			{ TEXT("set") });
		return Obj;
	}

	static void ApplyControlSettingsJson(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Path,
		FRigControlSettings& InOutSettings,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(
			Obj,
			Path,
			{
				TEXT("animation_type"), TEXT("control_type"), TEXT("display_name"), TEXT("primary_axis"),
				TEXT("shape_visible"), TEXT("shape_visibility"), TEXT("shape_name"), TEXT("shape_color"),
				TEXT("transient_control"), TEXT("group_with_parent_control"), TEXT("restrict_space_switching"),
				TEXT("use_preferred_rotation_order"),
				TEXT("selector"), TEXT("display_name"), TEXT("category_path"), TEXT("control_type"), TEXT("value_kind"), TEXT("availability"), TEXT("operations")
			},
			Issues);

		FString TextValue;
		if (Obj->TryGetStringField(TEXT("animation_type"), TextValue))
		{
			ERigControlAnimationType Parsed = InOutSettings.AnimationType;
			if (ParseEnumString(TextValue, Parsed))
			{
				InOutSettings.AnimationType = Parsed;
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_enum_parse_failed"), Path + TEXT(".animation_type"), FString::Printf(TEXT("Unknown ERigControlAnimationType '%s'."), *TextValue));
			}
		}
		if (Obj->TryGetStringField(TEXT("control_type"), TextValue))
		{
			ERigControlType Parsed = InOutSettings.ControlType;
			if (ParseEnumString(TextValue, Parsed))
			{
				InOutSettings.ControlType = Parsed;
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_enum_parse_failed"), Path + TEXT(".control_type"), FString::Printf(TEXT("Unknown ERigControlType '%s'."), *TextValue));
			}
		}
		if (Obj->TryGetStringField(TEXT("display_name"), TextValue))
		{
			InOutSettings.DisplayName = FName(*TextValue);
		}
		if (Obj->TryGetStringField(TEXT("primary_axis"), TextValue))
		{
			ERigControlAxis Parsed = InOutSettings.PrimaryAxis;
			if (ParseEnumString(TextValue, Parsed))
			{
				InOutSettings.PrimaryAxis = Parsed;
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_enum_parse_failed"), Path + TEXT(".primary_axis"), FString::Printf(TEXT("Unknown ERigControlAxis '%s'."), *TextValue));
			}
		}
		bool bBool = false;
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("shape_visible"), Path + TEXT(".shape_visible"), bBool, Issues, false))
		{
			InOutSettings.bShapeVisible = bBool;
		}
		if (Obj->TryGetStringField(TEXT("shape_visibility"), TextValue))
		{
			ERigControlVisibility Parsed = InOutSettings.ShapeVisibility;
			if (ParseEnumString(TextValue, Parsed))
			{
				InOutSettings.ShapeVisibility = Parsed;
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_enum_parse_failed"), Path + TEXT(".shape_visibility"), FString::Printf(TEXT("Unknown ERigControlVisibility '%s'."), *TextValue));
			}
		}
		if (Obj->TryGetStringField(TEXT("shape_name"), TextValue))
		{
			InOutSettings.ShapeName = FName(*TextValue);
		}
		FLinearColor Color = InOutSettings.ShapeColor;
		if (ReadLinearColorField(Obj, TEXT("shape_color"), Path + TEXT(".shape_color"), Color, Issues, false))
		{
			InOutSettings.ShapeColor = Color;
		}
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("transient_control"), Path + TEXT(".transient_control"), bBool, Issues, false))
		{
			InOutSettings.bIsTransientControl = bBool;
		}
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("group_with_parent_control"), Path + TEXT(".group_with_parent_control"), bBool, Issues, false))
		{
			InOutSettings.bGroupWithParentControl = bBool;
		}
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("restrict_space_switching"), Path + TEXT(".restrict_space_switching"), bBool, Issues, false))
		{
			InOutSettings.bRestrictSpaceSwitching = bBool;
		}
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("use_preferred_rotation_order"), Path + TEXT(".use_preferred_rotation_order"), bBool, Issues, false))
		{
			InOutSettings.bUsePreferredRotationOrder = bBool;
		}
		InOutSettings.SetupLimitArrayForType();
	}

	static TSharedPtr<FJsonObject> BuildRigElementJson(const URigHierarchy* Hierarchy, const FRigElementKey& Key)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Key.Name.ToString());
		Obj->SetStringField(TEXT("type"), ElementTypeToString(Key.Type));
		Obj->SetObjectField(TEXT("parent"), BuildParentJson(Hierarchy, Key));
		Obj->SetObjectField(TEXT("initial_local_transform"), MakeTransformJson(Hierarchy ? Hierarchy->GetLocalTransform(Key, true) : FTransform::Identity));
		Obj->SetObjectField(TEXT("current_local_transform"), MakeTransformJson(Hierarchy ? Hierarchy->GetLocalTransform(Key, false) : FTransform::Identity));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildControlJson(const URigHierarchy* Hierarchy, const FRigControlElement* Control)
	{
		TSharedPtr<FJsonObject> Obj = BuildRigElementJson(Hierarchy, Control ? Control->GetKey() : FRigElementKey());
		if (!Hierarchy || !Control)
		{
			return Obj;
		}

		Obj->SetObjectField(TEXT("settings"), BuildControlSettingsJson(Control->Settings));
		Obj->SetObjectField(TEXT("value_transform"), MakeTransformJson(Hierarchy->GetControlValue(Control->GetKey(), ERigControlValueType::Initial).GetAsTransform(Control->Settings.ControlType, Control->Settings.PrimaryAxis)));
		Obj->SetObjectField(TEXT("offset_transform"), MakeTransformJson(Hierarchy->GetControlOffsetTransform(const_cast<FRigControlElement*>(Control), ERigTransformType::InitialLocal)));
		Obj->SetObjectField(
			TEXT("shape_transform"),
			MakeTransformJson(Hierarchy->GetControlShapeTransform(const_cast<FRigControlElement*>(Control), ERigTransformType::InitialLocal)));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Obj,
			TEXT("name"),
			Control->GetKey().Name.ToString(),
			Control->Settings.DisplayName.IsNone() ? Control->GetKey().Name.ToString() : Control->Settings.DisplayName.ToString(),
			TEXT("Control Rig/Controls"),
			TEXT("control_rig_control"),
			TEXT("control"),
			{ TEXT("set"), TEXT("remove") });
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildGraphJson(URigVMGraph* Graph)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Graph)
		{
			return Obj;
		}

		Obj->SetStringField(TEXT("name"), Graph->GetNodePath());
		Obj->SetBoolField(TEXT("default_graph"), Graph == Graph->GetRootGraph());

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		for (URigVMNode* Node : Graph->GetNodes())
		{
			if (!Node)
			{
				continue;
			}

			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("name"), Node->GetName());
			NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle());
			NodeObj->SetObjectField(TEXT("position"), MakeVector2DJson(Node->GetPosition()));

			if (const URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("unit"));
				NodeObj->SetStringField(TEXT("script_struct"), UnitNode->GetScriptStruct() ? UnitNode->GetScriptStruct()->GetPathName() : FString());
				NodeObj->SetStringField(TEXT("method"), UnitNode->GetMethodName().ToString());
			}
			else if (const URigVMVariableNode* VariableNode = Cast<URigVMVariableNode>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("variable"));
				NodeObj->SetStringField(TEXT("variable_name"), VariableNode->GetVariableName().ToString());
				NodeObj->SetStringField(TEXT("cpp_type"), VariableNode->GetCPPType());
				NodeObj->SetStringField(TEXT("default_value"), VariableNode->GetDefaultValue());
			}
			else if (const URigVMTemplateNode* TemplateNode = Cast<URigVMTemplateNode>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("template"));
				NodeObj->SetStringField(TEXT("notation"), TemplateNode->GetNotation().ToString());
			}
			else
			{
				NodeObj->SetStringField(TEXT("node_type"), Node->GetClass()->GetName());
			}

			TSharedPtr<FJsonObject> PinDefaults = MakeShared<FJsonObject>();
			for (URigVMPin* Pin : Node->GetAllPinsRecursively())
			{
				if (Pin && !Pin->GetDefaultValue().IsEmpty())
				{
					PinDefaults->SetStringField(Pin->GetPinPath(false), Pin->GetDefaultValue());
				}
			}
			NodeObj->SetObjectField(TEXT("pin_defaults"), PinDefaults);
			UeAgentAuthoringJson::ApplyAuthoringMetadata(
				NodeObj,
				TEXT("name"),
				Node->GetName(),
				Node->GetNodeTitle(),
				TEXT("Control Rig/Graph"),
				TEXT("rigvm_node"),
				TEXT("node"),
				{ TEXT("set"), TEXT("remove") });
			NodeValues.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
		Obj->SetArrayField(TEXT("nodes"), NodeValues);

		TArray<TSharedPtr<FJsonValue>> LinkValues;
		for (URigVMLink* Link : Graph->GetLinks())
		{
			if (!Link)
			{
				continue;
			}

			TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
			LinkObj->SetStringField(TEXT("source"), Link->GetSourcePinPath());
			LinkObj->SetStringField(TEXT("target"), Link->GetTargetPinPath());
			LinkValues.Add(MakeShared<FJsonValueObject>(LinkObj));
		}
		Obj->SetArrayField(TEXT("links"), LinkValues);
		return Obj;
	}

	static TSharedPtr<FJsonObject> FindOrAddGraphJson(TSharedPtr<FJsonObject> GraphsJson, const FString& GraphName)
	{
		if (!GraphsJson.IsValid())
		{
			return nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> GraphValues;
		const TArray<TSharedPtr<FJsonValue>>* ExistingGraphs = nullptr;
		if (GraphsJson->TryGetArrayField(TEXT("graphs"), ExistingGraphs) && ExistingGraphs)
		{
			GraphValues = *ExistingGraphs;
			for (const TSharedPtr<FJsonValue>& GraphValue : GraphValues)
			{
				const TSharedPtr<FJsonObject>* GraphObj = nullptr;
				if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObj) || !GraphObj || !GraphObj->IsValid())
				{
					continue;
				}

				FString ExistingName;
				(*GraphObj)->TryGetStringField(TEXT("name"), ExistingName);
				bool bDefaultGraph = false;
				(*GraphObj)->TryGetBoolField(TEXT("default_graph"), bDefaultGraph);
				if ((!GraphName.TrimStartAndEnd().IsEmpty() && ExistingName.Equals(GraphName, ESearchCase::IgnoreCase))
					|| (GraphName.TrimStartAndEnd().IsEmpty() && bDefaultGraph))
				{
					return *GraphObj;
				}
			}
		}

		if (GraphName.TrimStartAndEnd().IsEmpty() && GraphValues.Num() > 0)
		{
			const TSharedPtr<FJsonObject>* FirstGraphObj = nullptr;
			if (GraphValues[0].IsValid() && GraphValues[0]->TryGetObject(FirstGraphObj) && FirstGraphObj && FirstGraphObj->IsValid())
			{
				return *FirstGraphObj;
			}
		}

		TSharedPtr<FJsonObject> NewGraph = MakeShared<FJsonObject>();
		NewGraph->SetStringField(TEXT("name"), GraphName.TrimStartAndEnd().IsEmpty() ? TEXT("RigVMModel") : GraphName);
		NewGraph->SetBoolField(TEXT("default_graph"), GraphName.TrimStartAndEnd().IsEmpty());
		NewGraph->SetArrayField(TEXT("nodes"), {});
		NewGraph->SetArrayField(TEXT("links"), {});
		GraphValues.Add(MakeShared<FJsonValueObject>(NewGraph));
		GraphsJson->SetArrayField(TEXT("graphs"), GraphValues);
		return NewGraph;
	}

	static TSharedPtr<FJsonObject> FindOrAddNodeJson(
		TSharedPtr<FJsonObject> GraphObj,
		const FString& NodeName,
		const FString& ScriptStruct,
		const FString& Method,
		const FVector2D& Position)
	{
		if (!GraphObj.IsValid() || NodeName.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
		if (GraphObj->TryGetArrayField(TEXT("nodes"), ExistingNodes) && ExistingNodes)
		{
			NodeValues = *ExistingNodes;
			for (const TSharedPtr<FJsonValue>& NodeValue : NodeValues)
			{
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
				{
					continue;
				}

				FString ExistingName;
				(*NodeObj)->TryGetStringField(TEXT("name"), ExistingName);
				if (ExistingName.Equals(NodeName, ESearchCase::IgnoreCase))
				{
					return *NodeObj;
				}
			}
		}

		TSharedPtr<FJsonObject> NewNode = MakeShared<FJsonObject>();
		NewNode->SetStringField(TEXT("node_type"), TEXT("unit"));
		NewNode->SetStringField(TEXT("name"), NodeName);
		NewNode->SetStringField(TEXT("script_struct"), ScriptStruct);
		NewNode->SetStringField(TEXT("method"), Method.TrimStartAndEnd().IsEmpty() ? TEXT("Execute") : Method);
		NewNode->SetObjectField(TEXT("position"), MakeVector2DJson(Position));
		NewNode->SetObjectField(TEXT("pin_defaults"), MakeShared<FJsonObject>());
		NodeValues.Add(MakeShared<FJsonValueObject>(NewNode));
		GraphObj->SetArrayField(TEXT("nodes"), NodeValues);
		return NewNode;
	}

	static void ApplyStructuredPBIKNodeDefaults(
		TSharedPtr<FJsonObject> NodeObj,
		const TSharedPtr<FJsonObject>& SpecObj,
		const FString& SpecPath,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		FString RootBone;
		TryReadBoneNameField(SpecObj, TEXT("root"), RootBone);
		if (RootBone.IsEmpty())
		{
			TryGetStringFieldAny(SpecObj, { TEXT("root_bone"), TEXT("rootBone") }, RootBone);
		}
		if (!RootBone.IsEmpty())
		{
			SetStructuredPinDefault(NodeObj, TEXT("Root"), RootBone);
		}

		const TArray<TSharedPtr<FJsonValue>>* EffectorValues = nullptr;
		if (UeAgentJsonDiagnostics::ReadArrayField(SpecObj, TEXT("effectors"), SpecPath + TEXT(".effectors"), EffectorValues, Issues, false) && EffectorValues)
		{
			TArray<FString> EffectorTexts;
			for (int32 Index = 0; Index < EffectorValues->Num(); ++Index)
			{
				const FString EffectorPath = FString::Printf(TEXT("%s.effectors[%d]"), *SpecPath, Index);
				const TSharedPtr<FJsonObject>* EffectorObj = nullptr;
				if ((*EffectorValues)[Index].IsValid() && (*EffectorValues)[Index]->TryGetObject(EffectorObj) && EffectorObj && EffectorObj->IsValid())
				{
					EffectorTexts.Add(BuildPBIKEffectorDefaultText(*EffectorObj, EffectorPath, Issues));
				}
				else
				{
					FString BoneName;
					if (TryReadBoneNameValue((*EffectorValues)[Index], BoneName))
					{
						TSharedPtr<FJsonObject> BoneOnly = MakeShared<FJsonObject>();
						BoneOnly->SetStringField(TEXT("bone"), BoneName);
						EffectorTexts.Add(BuildPBIKEffectorDefaultText(BoneOnly, EffectorPath, Issues));
					}
					else
					{
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_field_type_mismatch"), EffectorPath, TEXT("Expected PBIK effector object or bone name string."));
					}
				}
			}
			SetStructuredPinDefault(NodeObj, TEXT("Effectors"), ArrayDefaultText(EffectorTexts));
		}

		const TArray<TSharedPtr<FJsonValue>>* BoneSettingValues = nullptr;
		if (UeAgentJsonDiagnostics::ReadArrayField(SpecObj, TEXT("bone_settings"), SpecPath + TEXT(".bone_settings"), BoneSettingValues, Issues, false) && BoneSettingValues)
		{
			TArray<FString> BoneSettingTexts;
			for (int32 Index = 0; Index < BoneSettingValues->Num(); ++Index)
			{
				const FString BoneSettingPath = FString::Printf(TEXT("%s.bone_settings[%d]"), *SpecPath, Index);
				const TSharedPtr<FJsonObject>* BoneObj = nullptr;
				if ((*BoneSettingValues)[Index].IsValid() && (*BoneSettingValues)[Index]->TryGetObject(BoneObj) && BoneObj && BoneObj->IsValid())
				{
					BoneSettingTexts.Add(BuildPBIKBoneSettingDefaultText(*BoneObj, BoneSettingPath, Issues));
				}
				else
				{
					UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_field_type_mismatch"), BoneSettingPath, TEXT("Expected PBIK bone setting object."));
				}
			}
			SetStructuredPinDefault(NodeObj, TEXT("BoneSettings"), ArrayDefaultText(BoneSettingTexts));
		}

		const TArray<TSharedPtr<FJsonValue>>* ExcludedBoneValues = nullptr;
		if (UeAgentJsonDiagnostics::ReadArrayField(SpecObj, TEXT("excluded_bones"), SpecPath + TEXT(".excluded_bones"), ExcludedBoneValues, Issues, false) && ExcludedBoneValues)
		{
			TArray<FString> ExcludedBoneTexts;
			for (int32 Index = 0; Index < ExcludedBoneValues->Num(); ++Index)
			{
				FString BoneName;
				if (TryReadBoneNameValue((*ExcludedBoneValues)[Index], BoneName))
				{
					ExcludedBoneTexts.Add(QuoteDefaultString(BoneName));
				}
				else
				{
					UeAgentJsonDiagnostics::AddIssue(
						Issues,
						TEXT("error"),
						TEXT("json_field_type_mismatch"),
						FString::Printf(TEXT("%s.excluded_bones[%d]"), *SpecPath, Index),
						TEXT("Expected excluded bone name string or object."));
				}
			}
			SetStructuredPinDefault(NodeObj, TEXT("ExcludedBones"), ArrayDefaultText(ExcludedBoneTexts));
		}

		const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
		if (SpecObj->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && SettingsObj->IsValid())
		{
			SetStructuredPinDefault(NodeObj, TEXT("Settings"), BuildPBIKSolverSettingsDefaultText(*SettingsObj, SpecPath + TEXT(".settings"), Issues));
		}

		const TSharedPtr<FJsonObject>* DebugObj = nullptr;
		if (SpecObj->TryGetObjectField(TEXT("debug"), DebugObj) && DebugObj && DebugObj->IsValid())
		{
			SetStructuredPinDefault(NodeObj, TEXT("Debug"), BuildPBIKDebugDefaultText(*DebugObj));
		}
	}

	static void ApplyStructuredLegacyFBIKNodeDefaults(
		TSharedPtr<FJsonObject> NodeObj,
		const TSharedPtr<FJsonObject>& SpecObj,
		const FString& SpecPath,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		FString RootBone;
		TryReadBoneNameField(SpecObj, TEXT("root"), RootBone);
		if (RootBone.IsEmpty())
		{
			TryGetStringFieldAny(SpecObj, { TEXT("root_bone"), TEXT("rootBone") }, RootBone);
		}
		if (!RootBone.IsEmpty())
		{
			SetStructuredPinDefault(NodeObj, TEXT("Root"), FString::Printf(TEXT("(Type=Bone,Name=%s)"), *QuoteDefaultString(RootBone)));
		}

		const TArray<TSharedPtr<FJsonValue>>* EffectorValues = nullptr;
		if (UeAgentJsonDiagnostics::ReadArrayField(SpecObj, TEXT("effectors"), SpecPath + TEXT(".effectors"), EffectorValues, Issues, false) && EffectorValues)
		{
			TArray<FString> EffectorTexts;
			for (int32 Index = 0; Index < EffectorValues->Num(); ++Index)
			{
				const FString EffectorPath = FString::Printf(TEXT("%s.effectors[%d]"), *SpecPath, Index);
				const TSharedPtr<FJsonObject>* EffectorObj = nullptr;
				if ((*EffectorValues)[Index].IsValid() && (*EffectorValues)[Index]->TryGetObject(EffectorObj) && EffectorObj && EffectorObj->IsValid())
				{
					EffectorTexts.Add(BuildLegacyFBIKEffectorDefaultText(*EffectorObj, EffectorPath, Issues));
				}
				else
				{
					FString BoneName;
					if (TryReadBoneNameValue((*EffectorValues)[Index], BoneName))
					{
						TSharedPtr<FJsonObject> BoneOnly = MakeShared<FJsonObject>();
						BoneOnly->SetStringField(TEXT("bone"), BoneName);
						EffectorTexts.Add(BuildLegacyFBIKEffectorDefaultText(BoneOnly, EffectorPath, Issues));
					}
					else
					{
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_field_type_mismatch"), EffectorPath, TEXT("Expected FullbodyIK effector object or bone name string."));
					}
				}
			}
			SetStructuredPinDefault(NodeObj, TEXT("Effectors"), ArrayDefaultText(EffectorTexts));
		}

		const TArray<TSharedPtr<FJsonValue>>* ConstraintValues = nullptr;
		if (UeAgentJsonDiagnostics::ReadArrayField(SpecObj, TEXT("constraints"), SpecPath + TEXT(".constraints"), ConstraintValues, Issues, false) && ConstraintValues)
		{
			TArray<FString> ConstraintTexts;
			for (int32 Index = 0; Index < ConstraintValues->Num(); ++Index)
			{
				const FString ConstraintPath = FString::Printf(TEXT("%s.constraints[%d]"), *SpecPath, Index);
				const TSharedPtr<FJsonObject>* ConstraintObj = nullptr;
				if ((*ConstraintValues)[Index].IsValid() && (*ConstraintValues)[Index]->TryGetObject(ConstraintObj) && ConstraintObj && ConstraintObj->IsValid())
				{
					ConstraintTexts.Add(BuildLegacyFBIKConstraintDefaultText(*ConstraintObj, ConstraintPath, Issues));
				}
				else
				{
					UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_field_type_mismatch"), ConstraintPath, TEXT("Expected FullbodyIK constraint object."));
				}
			}
			SetStructuredPinDefault(NodeObj, TEXT("Constraints"), ArrayDefaultText(ConstraintTexts));
		}

		const TSharedPtr<FJsonObject>* MotionPropertyObj = nullptr;
		if (SpecObj->TryGetObjectField(TEXT("motion_property"), MotionPropertyObj) && MotionPropertyObj && MotionPropertyObj->IsValid())
		{
			bool bForceEffectorRotationTarget = false;
			bool bOnlyApplyWhenReachedToTarget = false;
			TryGetBoolFieldAny(*MotionPropertyObj, { TEXT("force_effector_rotation_target"), TEXT("bForceEffectorRotationTarget") }, bForceEffectorRotationTarget);
			TryGetBoolFieldAny(*MotionPropertyObj, { TEXT("only_apply_when_reached_to_target"), TEXT("bOnlyApplyWhenReachedToTarget") }, bOnlyApplyWhenReachedToTarget);
			SetStructuredPinDefault(
				NodeObj,
				TEXT("MotionProperty"),
				FString::Printf(
					TEXT("(bForceEffectorRotationTarget=%s,bOnlyApplyWhenReachedToTarget=%s)"),
					*BoolDefaultText(bForceEffectorRotationTarget),
					*BoolDefaultText(bOnlyApplyWhenReachedToTarget)));
		}
	}

	void MergeStructuredIkNodesJson(
		TSharedPtr<FJsonObject> GraphsJson,
		const TSharedPtr<FJsonObject>& IkNodesJson,
		const FString& Path,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!GraphsJson.IsValid() || !IkNodesJson.IsValid())
		{
			return;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(IkNodesJson, Path, { TEXT("schema"), TEXT("apply_policy"), TEXT("nodes"), TEXT("node_count") }, Issues);

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(IkNodesJson, TEXT("nodes"), Path + TEXT(".nodes"), Nodes, Issues, false) || !Nodes)
		{
			return;
		}

		for (int32 NodeIndex = 0; NodeIndex < Nodes->Num(); ++NodeIndex)
		{
			TSharedPtr<FJsonObject> SpecObj;
			const FString SpecPath = FString::Printf(TEXT("%s.nodes[%d]"), *Path, NodeIndex);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Nodes)[NodeIndex], SpecPath, SpecObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(
				SpecObj,
				SpecPath,
				{
					TEXT("graph"), TEXT("name"), TEXT("node"), TEXT("solver_type"), TEXT("script_struct"), TEXT("method"), TEXT("position"),
					TEXT("root"), TEXT("root_bone"), TEXT("effectors"), TEXT("bone_settings"), TEXT("excluded_bones"), TEXT("settings"), TEXT("debug"),
					TEXT("constraints"), TEXT("motion_property"), TEXT("create_if_missing")
				},
				Issues);

			FString NodeName;
			if (!TryGetStringFieldAny(SpecObj, { TEXT("name"), TEXT("node") }, NodeName) || NodeName.IsEmpty())
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_ik_node_missing_name"), SpecPath, TEXT("Structured IK node requires name."));
				continue;
			}

			FString SolverType;
			TryGetStringFieldAny(SpecObj, { TEXT("solver_type"), TEXT("solver") }, SolverType);
			FString ScriptStruct;
			TryGetStringFieldAny(SpecObj, { TEXT("script_struct"), TEXT("unit_struct") }, ScriptStruct);
			FString SolverToken = SolverType.ToLower();
			SolverToken.ReplaceInline(TEXT("_"), TEXT(""));
			SolverToken.ReplaceInline(TEXT("-"), TEXT(""));
			const bool bLegacyFBIK = SolverToken.Contains(TEXT("legacy")) || SolverToken.Contains(TEXT("fullbodyiklegacy")) || SolverToken == TEXT("fullbodyik");
			if (ScriptStruct.TrimStartAndEnd().IsEmpty())
			{
				ScriptStruct = bLegacyFBIK ? TEXT("/Script/FullBodyIK.RigUnit_FullbodyIK") : TEXT("/Script/PBIK.RigUnit_PBIK");
			}

			FString GraphName;
			TryGetStringFieldAny(SpecObj, { TEXT("graph"), TEXT("graph_name") }, GraphName);
			TSharedPtr<FJsonObject> GraphObj = FindOrAddGraphJson(GraphsJson, GraphName);
			if (!GraphObj.IsValid())
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_graph_json_missing"), SpecPath + TEXT(".graph"), TEXT("Failed to locate or create graph JSON."));
				continue;
			}

			FVector2D Position = FVector2D::ZeroVector;
			ReadVector2DField(SpecObj, TEXT("position"), SpecPath + TEXT(".position"), Position, Issues, false);
			FString Method;
			TryGetStringFieldAny(SpecObj, { TEXT("method") }, Method);
			TSharedPtr<FJsonObject> NodeObj = FindOrAddNodeJson(GraphObj, NodeName, ScriptStruct, Method, Position);
			if (!NodeObj.IsValid())
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_ik_node_json_missing"), SpecPath, TEXT("Failed to locate or create RigVM node JSON."));
				continue;
			}

			NodeObj->SetStringField(TEXT("node_type"), TEXT("unit"));
			NodeObj->SetStringField(TEXT("name"), NodeName);
			NodeObj->SetStringField(TEXT("script_struct"), ScriptStruct);
			if (!Method.TrimStartAndEnd().IsEmpty())
			{
				NodeObj->SetStringField(TEXT("method"), Method);
			}
			else if (!NodeObj->HasField(TEXT("method")))
			{
				NodeObj->SetStringField(TEXT("method"), TEXT("Execute"));
			}

			if (ScriptStruct.Contains(TEXT("RigUnit_FullbodyIK")) || bLegacyFBIK)
			{
				ApplyStructuredLegacyFBIKNodeDefaults(NodeObj, SpecObj, SpecPath, Issues);
			}
			else
			{
				ApplyStructuredPBIKNodeDefaults(NodeObj, SpecObj, SpecPath, Issues);
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildEventDiagnosticsJson(UClass* ControlRigClass)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> SupportedEventValues;
		TArray<TSharedPtr<FJsonValue>> RequiredAnimationEditingEvents;
		RequiredAnimationEditingEvents.Add(MakeShared<FJsonValueString>(FRigUnit_BeginExecution::EventName.ToString()));
		RequiredAnimationEditingEvents.Add(MakeShared<FJsonValueString>(FRigUnit_InverseExecution::EventName.ToString()));

		const bool bIsFKControlRig = ControlRigClass && ControlRigClass->IsChildOf(UFKControlRig::StaticClass());
		bool bSupportsForwardSolve = false;
		bool bSupportsBackwardSolve = false;
		bool bSupportsLegacyInverse = false;
		if (const UControlRig* DefaultControlRig = ControlRigClass ? ControlRigClass->GetDefaultObject<UControlRig>() : nullptr)
		{
			for (const FName& SupportedEvent : DefaultControlRig->GetSupportedEvents())
			{
				SupportedEventValues.Add(MakeShared<FJsonValueString>(SupportedEvent.ToString()));
			}
			bSupportsForwardSolve = DefaultControlRig->SupportsEvent(FRigUnit_BeginExecution::EventName);
			bSupportsBackwardSolve = DefaultControlRig->SupportsEvent(FRigUnit_InverseExecution::EventName);
			bSupportsLegacyInverse = DefaultControlRig->SupportsEvent(FName(TEXT("Inverse")));
		}

		const bool bSupportsInverseEvent = bSupportsBackwardSolve || bSupportsLegacyInverse;
		const bool bAnimationEditingReady = bSupportsForwardSolve && bSupportsBackwardSolve;
		const bool bBakeToControlRigReady = bIsFKControlRig || bSupportsInverseEvent;

		TArray<TSharedPtr<FJsonValue>> Issues;
		if (!bSupportsForwardSolve)
		{
			UeAgentJsonDiagnostics::AddIssue(
				Issues,
				TEXT("error"),
				TEXT("control_rig_missing_forward_solve_event"),
				TEXT("control_rig.events"),
				TEXT("Animation authoring requires a Forwards Solve event to drive bones from controls."));
		}
		if (!bSupportsBackwardSolve)
		{
			UeAgentJsonDiagnostics::AddIssue(
				Issues,
				TEXT("error"),
				TEXT("control_rig_missing_backward_solve_event"),
				TEXT("control_rig.events"),
				TEXT("Animation authoring from an existing skeletal pose requires a Backwards Solve event to initialize controls from bones."));
		}

		Obj->SetArrayField(TEXT("supported_events"), SupportedEventValues);
		Obj->SetArrayField(TEXT("required_animation_editing_events"), RequiredAnimationEditingEvents);
		Obj->SetBoolField(TEXT("is_fk_control_rig"), bIsFKControlRig);
		Obj->SetBoolField(TEXT("supports_forward_solve"), bSupportsForwardSolve);
		Obj->SetBoolField(TEXT("supports_backward_solve"), bSupportsBackwardSolve);
		Obj->SetBoolField(TEXT("supports_legacy_inverse_event"), bSupportsLegacyInverse);
		Obj->SetBoolField(TEXT("supports_inverse_event"), bSupportsInverseEvent);
		Obj->SetBoolField(TEXT("animation_editing_ready"), bAnimationEditingReady);
		Obj->SetBoolField(TEXT("bake_to_control_rig_ready"), bBakeToControlRigReady);
		Obj->SetArrayField(TEXT("issues"), Issues);
		Obj->SetNumberField(TEXT("issue_count"), Issues.Num());
		return Obj;
	}

	TSharedPtr<FJsonObject> BuildControlRigInfo(UControlRigBlueprint* Blueprint)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Blueprint)
		{
			return Out;
		}

		const FString AssetPath = NormalizeAssetPath(Blueprint->GetPathName());
		Out->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.folder.v1"));
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetStringField(TEXT("object_path"), Blueprint->GetPathName());
		Out->SetStringField(TEXT("asset_class"), Blueprint->GetClass()->GetPathName());
		Out->SetStringField(TEXT("control_rig_type"), ControlRigTypeToString(Blueprint->ControlRigType));
		UClass* ControlRigClass = Blueprint->GetControlRigClass();
		Out->SetStringField(TEXT("control_rig_class"), ControlRigClass ? ControlRigClass->GetPathName() : FString());
		Out->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString());
		Out->SetStringField(TEXT("preview_skeletal_mesh"), Blueprint->GetPreviewMesh() ? Blueprint->GetPreviewMesh()->GetPathName() : FString());
		Out->SetNumberField(TEXT("blueprint_status"), static_cast<int32>(Blueprint->Status));
		TSharedPtr<FJsonObject> EventDiagnostics = BuildEventDiagnosticsJson(ControlRigClass);
		Out->SetObjectField(TEXT("event_diagnostics"), EventDiagnostics);
		Out->SetArrayField(TEXT("supported_events"), EventDiagnostics->GetArrayField(TEXT("supported_events")));
		Out->SetArrayField(TEXT("required_animation_editing_events"), EventDiagnostics->GetArrayField(TEXT("required_animation_editing_events")));
		Out->SetBoolField(TEXT("supports_forward_solve"), EventDiagnostics->GetBoolField(TEXT("supports_forward_solve")));
		Out->SetBoolField(TEXT("supports_backward_solve"), EventDiagnostics->GetBoolField(TEXT("supports_backward_solve")));
		Out->SetBoolField(TEXT("supports_inverse_event"), EventDiagnostics->GetBoolField(TEXT("supports_inverse_event")));
		Out->SetBoolField(TEXT("animation_editing_ready"), EventDiagnostics->GetBoolField(TEXT("animation_editing_ready")));
		Out->SetBoolField(TEXT("bake_to_control_rig_ready"), EventDiagnostics->GetBoolField(TEXT("bake_to_control_rig_ready")));

		TArray<TSharedPtr<FJsonValue>> ShapeLibraryValues;
		for (const TSoftObjectPtr<UControlRigShapeLibrary>& ShapeLibrary : Blueprint->ShapeLibraries)
		{
			ShapeLibraryValues.Add(MakeShared<FJsonValueString>(ShapeLibrary.ToSoftObjectPath().ToString()));
		}
		Out->SetArrayField(TEXT("shape_libraries"), ShapeLibraryValues);

		URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
		TSharedPtr<FJsonObject> HierarchyObj = MakeShared<FJsonObject>();
		if (Hierarchy)
		{
			TArray<TSharedPtr<FJsonValue>> BoneValues;
			for (FRigBoneElement* Bone : Hierarchy->GetBones(true))
			{
				if (Bone)
				{
					TSharedPtr<FJsonObject> BoneObj = BuildRigElementJson(Hierarchy, Bone->GetKey());
					BoneObj->SetStringField(TEXT("bone_type"), EnumToString(Bone->BoneType));
					BoneValues.Add(MakeShared<FJsonValueObject>(BoneObj));
				}
			}
			HierarchyObj->SetArrayField(TEXT("bones"), BoneValues);
			HierarchyObj->SetNumberField(TEXT("bone_count"), BoneValues.Num());

			TArray<TSharedPtr<FJsonValue>> NullValues;
			for (FRigNullElement* Null : Hierarchy->GetNulls(true))
			{
				if (Null)
				{
					NullValues.Add(MakeShared<FJsonValueObject>(BuildRigElementJson(Hierarchy, Null->GetKey())));
				}
			}
			HierarchyObj->SetArrayField(TEXT("nulls"), NullValues);
			HierarchyObj->SetNumberField(TEXT("null_count"), NullValues.Num());

			TArray<TSharedPtr<FJsonValue>> ControlValues;
			for (FRigControlElement* Control : Hierarchy->GetControls(true))
			{
				if (Control)
				{
					ControlValues.Add(MakeShared<FJsonValueObject>(BuildControlJson(Hierarchy, Control)));
				}
			}
			HierarchyObj->SetArrayField(TEXT("controls"), ControlValues);
			HierarchyObj->SetNumberField(TEXT("control_count"), ControlValues.Num());

			TArray<TSharedPtr<FJsonValue>> CurveValues;
			for (FRigCurveElement* Curve : Hierarchy->GetCurves())
			{
				if (Curve)
				{
					TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
					CurveObj->SetStringField(TEXT("name"), Curve->GetName());
					CurveObj->SetNumberField(TEXT("value"), Hierarchy->GetCurveValue(Curve->GetKey()));
					CurveValues.Add(MakeShared<FJsonValueObject>(CurveObj));
				}
			}
			HierarchyObj->SetArrayField(TEXT("curves"), CurveValues);
			HierarchyObj->SetNumberField(TEXT("curve_count"), CurveValues.Num());
		}
		Out->SetObjectField(TEXT("hierarchy"), HierarchyObj);

		TArray<TSharedPtr<FJsonValue>> GraphValues;
		for (URigVMGraph* Graph : Blueprint->GetAllModels())
		{
			GraphValues.Add(MakeShared<FJsonValueObject>(BuildGraphJson(Graph)));
		}
		Out->SetArrayField(TEXT("graphs"), GraphValues);
		Out->SetNumberField(TEXT("graph_count"), GraphValues.Num());
		Out->SetObjectField(TEXT("variables"), BuildVariablesJson(Blueprint));
		return Out;
	}

	bool ApplyPreviewJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return true;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("schema"), TEXT("preview_skeletal_mesh"), TEXT("import_hierarchy_from_preview") }, Issues);
		FString PreviewMeshPath;
		if (Obj->TryGetStringField(TEXT("preview_skeletal_mesh"), PreviewMeshPath))
		{
			if (PreviewMeshPath.TrimStartAndEnd().IsEmpty())
			{
				Blueprint->SetPreviewMesh(nullptr, true);
			}
			else if (USkeletalMesh* PreviewMesh = LoadAsset<USkeletalMesh>(PreviewMeshPath))
			{
				Blueprint->SetPreviewMesh(PreviewMesh, true);
				bool bImportHierarchy = false;
				if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("import_hierarchy_from_preview"), Path + TEXT(".import_hierarchy_from_preview"), bImportHierarchy, Issues, false) && bImportHierarchy)
				{
					Blueprint->GetHierarchyController()->ImportBonesFromSkeletalMesh(PreviewMesh, NAME_None, true, true, false, false, false);
				}
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("asset_reference_not_found"), Path + TEXT(".preview_skeletal_mesh"), FString::Printf(TEXT("Preview skeletal mesh '%s' could not be loaded."), *PreviewMeshPath));
			}
		}
		return !HasBlockingIssue(Issues);
	}

	bool ApplyShapeLibrariesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return true;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("schema"), TEXT("shape_libraries"), TEXT("name_map") }, Issues);
		const TArray<TSharedPtr<FJsonValue>>* Libraries = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("shape_libraries"), Path + TEXT(".shape_libraries"), Libraries, Issues, false) || !Libraries)
		{
			return !HasBlockingIssue(Issues);
		}

		Blueprint->ShapeLibraries.Reset();
		for (int32 Index = 0; Index < Libraries->Num(); ++Index)
		{
			FString LibraryPath;
			if (!(*Libraries)[Index]->TryGetString(LibraryPath))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_array_item_type_mismatch"), FString::Printf(TEXT("%s.shape_libraries[%d]"), *Path, Index), TEXT("Expected shape library asset path string."));
				continue;
			}
			if (!LibraryPath.TrimStartAndEnd().IsEmpty())
			{
				if (!LoadShapeLibrary(LibraryPath))
				{
					UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("asset_reference_not_found"), FString::Printf(TEXT("%s.shape_libraries[%d]"), *Path, Index), FString::Printf(TEXT("Shape library '%s' could not be loaded."), *LibraryPath));
				}
				Blueprint->ShapeLibraries.Add(TSoftObjectPtr<UControlRigShapeLibrary>(FSoftObjectPath(ToObjectPath(LibraryPath))));
			}
		}
		return !HasBlockingIssue(Issues);
	}

	void ApplyBonesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("replace_existing"), TEXT("bones") }, Issues);

		URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
		URigHierarchyController* Controller = Blueprint->GetHierarchyController();
		if (!Hierarchy || !Controller)
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_hierarchy_missing"), Path, TEXT("Control Rig hierarchy/controller is missing."));
			return;
		}

		bool bReplaceExisting = false;
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("replace_existing"), Path + TEXT(".replace_existing"), bReplaceExisting, Issues, false) && bReplaceExisting)
		{
			Hierarchy->Reset();
		}

		const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("bones"), Path + TEXT(".bones"), Bones, Issues, false) || !Bones)
		{
			return;
		}

		for (int32 Index = 0; Index < Bones->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> BoneObj;
			const FString BonePath = FString::Printf(TEXT("%s.bones[%d]"), *Path, Index);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Bones)[Index], BonePath, BoneObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(BoneObj, BonePath, { TEXT("name"), TEXT("type"), TEXT("parent"), TEXT("parent_name"), TEXT("parent_type"), TEXT("bone_type"), TEXT("initial_local_transform"), TEXT("current_local_transform"), TEXT("remove") }, Issues);

			FString Name;
			if (!UeAgentJsonDiagnostics::ReadStringField(BoneObj, TEXT("name"), BonePath + TEXT(".name"), Name, Issues, true) || Name.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			const FRigElementKey Key(FName(*Name), ERigElementType::Bone);
			bool bRemove = false;
			if (UeAgentJsonDiagnostics::ReadBoolField(BoneObj, TEXT("remove"), BonePath + TEXT(".remove"), bRemove, Issues, false) && bRemove)
			{
				Controller->RemoveElement(Key, false, false);
				continue;
			}

			FTransform LocalTransform = FTransform::Identity;
			ReadTransformField(BoneObj, TEXT("initial_local_transform"), BonePath + TEXT(".initial_local_transform"), LocalTransform, Issues, false);
			ERigBoneType BoneType = ERigBoneType::User;
			FString BoneTypeText;
			if (BoneObj->TryGetStringField(TEXT("bone_type"), BoneTypeText) && !ParseEnumString(BoneTypeText, BoneType))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_enum_parse_failed"), BonePath + TEXT(".bone_type"), FString::Printf(TEXT("Unknown ERigBoneType '%s'."), *BoneTypeText));
			}

			if (!Hierarchy->Find(Key))
			{
				Controller->AddBone(FName(*Name), ReadParentKey(BoneObj, ERigElementType::Bone), LocalTransform, false, BoneType, false, false);
			}
			else
			{
				Hierarchy->SetLocalTransform(Key, LocalTransform, true, true, false, false);
				const FRigElementKey ParentKey = ReadParentKey(BoneObj, ERigElementType::Bone);
				if (ParentKey.IsValid())
				{
					Controller->SetParent(Key, ParentKey, false, false, false);
				}
			}
		}
	}

	void ApplyNullsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("nulls") }, Issues);

		URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
		URigHierarchyController* Controller = Blueprint->GetHierarchyController();
		const TArray<TSharedPtr<FJsonValue>>* Nulls = nullptr;
		if (!Hierarchy || !Controller || !UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("nulls"), Path + TEXT(".nulls"), Nulls, Issues, false) || !Nulls)
		{
			return;
		}

		for (int32 Index = 0; Index < Nulls->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> NullObj;
			const FString NullPath = FString::Printf(TEXT("%s.nulls[%d]"), *Path, Index);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Nulls)[Index], NullPath, NullObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(NullObj, NullPath, { TEXT("name"), TEXT("type"), TEXT("parent"), TEXT("parent_name"), TEXT("parent_type"), TEXT("initial_local_transform"), TEXT("current_local_transform"), TEXT("remove") }, Issues);

			FString Name;
			if (!UeAgentJsonDiagnostics::ReadStringField(NullObj, TEXT("name"), NullPath + TEXT(".name"), Name, Issues, true) || Name.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			const FRigElementKey Key(FName(*Name), ERigElementType::Null);
			bool bRemove = false;
			if (UeAgentJsonDiagnostics::ReadBoolField(NullObj, TEXT("remove"), NullPath + TEXT(".remove"), bRemove, Issues, false) && bRemove)
			{
				Controller->RemoveElement(Key, false, false);
				continue;
			}

			FTransform LocalTransform = FTransform::Identity;
			ReadTransformField(NullObj, TEXT("initial_local_transform"), NullPath + TEXT(".initial_local_transform"), LocalTransform, Issues, false);
			if (!Hierarchy->Find(Key))
			{
				Controller->AddNull(FName(*Name), ReadParentKey(NullObj, ERigElementType::Bone), LocalTransform, false, false, false);
			}
			else
			{
				Hierarchy->SetLocalTransform(Key, LocalTransform, true, true, false, false);
				const FRigElementKey ParentKey = ReadParentKey(NullObj, ERigElementType::Bone);
				if (ParentKey.IsValid())
				{
					Controller->SetParent(Key, ParentKey, false, false, false);
				}
			}
		}
	}

	void ApplyControlsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("controls") }, Issues);

		URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
		URigHierarchyController* Controller = Blueprint->GetHierarchyController();
		const TArray<TSharedPtr<FJsonValue>>* Controls = nullptr;
		if (!Hierarchy || !Controller || !UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("controls"), Path + TEXT(".controls"), Controls, Issues, false) || !Controls)
		{
			return;
		}

		for (int32 Index = 0; Index < Controls->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> ControlObj;
			const FString ControlPath = FString::Printf(TEXT("%s.controls[%d]"), *Path, Index);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Controls)[Index], ControlPath, ControlObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(ControlObj, ControlPath, { TEXT("name"), TEXT("type"), TEXT("parent"), TEXT("parent_name"), TEXT("parent_type"), TEXT("initial_local_transform"), TEXT("current_local_transform"), TEXT("settings"), TEXT("value_transform"), TEXT("offset_transform"), TEXT("shape_transform"), TEXT("remove"), TEXT("selector"), TEXT("display_name"), TEXT("category_path"), TEXT("control_type"), TEXT("value_kind"), TEXT("availability"), TEXT("operations") }, Issues);

			FString Name;
			if (!UeAgentJsonDiagnostics::ReadStringField(ControlObj, TEXT("name"), ControlPath + TEXT(".name"), Name, Issues, true) || Name.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			const FRigElementKey Key(FName(*Name), ERigElementType::Control);
			bool bRemove = false;
			if (UeAgentJsonDiagnostics::ReadBoolField(ControlObj, TEXT("remove"), ControlPath + TEXT(".remove"), bRemove, Issues, false) && bRemove)
			{
				Controller->RemoveElement(Key, false, false);
				continue;
			}

			FRigControlSettings Settings;
			if (FRigControlElement* ExistingControl = Hierarchy->Find<FRigControlElement>(Key))
			{
				Settings = ExistingControl->Settings;
			}
			const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
			if (ControlObj->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && SettingsObj->IsValid())
			{
				ApplyControlSettingsJson(*SettingsObj, ControlPath + TEXT(".settings"), Settings, Issues);
			}
			Settings.SetupLimitArrayForType();

			FTransform ValueTransform = FTransform::Identity;
			ReadTransformField(ControlObj, TEXT("value_transform"), ControlPath + TEXT(".value_transform"), ValueTransform, Issues, false);
			FRigControlValue ControlValue;
			ControlValue.SetFromTransform(ValueTransform, Settings.ControlType, Settings.PrimaryAxis);

			FTransform OffsetTransform = FTransform::Identity;
			ReadTransformField(ControlObj, TEXT("offset_transform"), ControlPath + TEXT(".offset_transform"), OffsetTransform, Issues, false);
			FTransform ShapeTransform = Settings.ShapeTransform;
			if (FRigControlElement* ExistingControl = Hierarchy->Find<FRigControlElement>(Key))
			{
				ShapeTransform = Hierarchy->GetControlShapeTransform(ExistingControl, ERigTransformType::InitialLocal);
			}
			ReadTransformField(ControlObj, TEXT("shape_transform"), ControlPath + TEXT(".shape_transform"), ShapeTransform, Issues, false);
			Settings.ShapeTransform = ShapeTransform;

			if (!Hierarchy->Find(Key))
			{
				Controller->AddControl(FName(*Name), ReadParentKey(ControlObj, ERigElementType::Bone), Settings, ControlValue, OffsetTransform, ShapeTransform, false, false);
			}
			else
			{
				Controller->SetControlSettings(Key, Settings, false);
				Hierarchy->SetControlValue(Key, ControlValue, ERigControlValueType::Initial, false, false);
				Hierarchy->SetControlValue(Key, ControlValue, ERigControlValueType::Current, false, false);
				Hierarchy->SetControlOffsetTransform(Key, OffsetTransform, true, true, false, false);
				Hierarchy->SetControlShapeTransform(Key, ShapeTransform, true, false);
				Hierarchy->SetControlShapeTransform(Key, ShapeTransform, false, false);
				const FRigElementKey ParentKey = ReadParentKey(ControlObj, ERigElementType::Bone);
				if (ParentKey.IsValid())
				{
					Controller->SetParent(Key, ParentKey, false, false, false);
				}
			}
		}
	}

	void ApplyCurvesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("curves") }, Issues);

		URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
		URigHierarchyController* Controller = Blueprint->GetHierarchyController();
		const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
		if (!Hierarchy || !Controller || !UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("curves"), Path + TEXT(".curves"), Curves, Issues, false) || !Curves)
		{
			return;
		}

		for (int32 Index = 0; Index < Curves->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> CurveObj;
			const FString CurvePath = FString::Printf(TEXT("%s.curves[%d]"), *Path, Index);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Curves)[Index], CurvePath, CurveObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(CurveObj, CurvePath, { TEXT("name"), TEXT("value"), TEXT("remove") }, Issues);

			FString Name;
			if (!UeAgentJsonDiagnostics::ReadStringField(CurveObj, TEXT("name"), CurvePath + TEXT(".name"), Name, Issues, true) || Name.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			const FRigElementKey Key(FName(*Name), ERigElementType::Curve);
			bool bRemove = false;
			if (UeAgentJsonDiagnostics::ReadBoolField(CurveObj, TEXT("remove"), CurvePath + TEXT(".remove"), bRemove, Issues, false) && bRemove)
			{
				Controller->RemoveElement(Key, false, false);
				continue;
			}

			double Value = 0.0;
			const TSharedPtr<FJsonValue>* ValueField = UeAgentJsonDiagnostics::FindFieldValue(CurveObj, TEXT("value"));
			if (ValueField && !UeAgentJsonDiagnostics::TryReadNumberValue(*ValueField, Value))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_field_type_mismatch"), CurvePath + TEXT(".value"), FString::Printf(TEXT("Expected number but got %s."), *UeAgentJsonDiagnostics::JsonValueTypeToString(*ValueField)));
			}
			if (!Hierarchy->Find(Key))
			{
				Controller->AddCurve(FName(*Name), static_cast<float>(Value), false, false);
			}
			else
			{
				Hierarchy->SetCurveValue(Key, static_cast<float>(Value), false);
			}
		}
	}

	bool ApplyGraphsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return true;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("replace_nodes"), TEXT("graphs") }, Issues);

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("graphs"), Path + TEXT(".graphs"), Graphs, Issues, false) || !Graphs)
		{
			return !HasBlockingIssue(Issues);
		}

		bool bReplaceNodes = false;
		UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("replace_nodes"), Path + TEXT(".replace_nodes"), bReplaceNodes, Issues, false);

		for (int32 GraphIndex = 0; GraphIndex < Graphs->Num(); ++GraphIndex)
		{
			TSharedPtr<FJsonObject> GraphObj;
			const FString GraphPath = FString::Printf(TEXT("%s.graphs[%d]"), *Path, GraphIndex);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Graphs)[GraphIndex], GraphPath, GraphObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(GraphObj, GraphPath, { TEXT("name"), TEXT("default_graph"), TEXT("nodes"), TEXT("links") }, Issues);

			FString GraphName;
			GraphObj->TryGetStringField(TEXT("name"), GraphName);
			URigVMGraph* Graph = GraphName.TrimStartAndEnd().IsEmpty() ? Blueprint->GetDefaultModel() : Blueprint->GetModel(GraphName);
			if (!Graph)
			{
				Graph = Blueprint->AddModel(GraphName.TrimStartAndEnd().IsEmpty() ? TEXT("Rig Graph") : GraphName, true, false);
			}
			URigVMController* Controller = Blueprint->GetController(Graph);
			if (!Graph || !Controller)
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_graph_controller_missing"), GraphPath, TEXT("RigVM graph/controller is missing."));
				continue;
			}

			if (bReplaceNodes)
			{
				TArray<URigVMNode*> ExistingNodes = Graph->GetNodes();
				Controller->RemoveNodes(ExistingNodes, false, false);
			}

			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (UeAgentJsonDiagnostics::ReadArrayField(GraphObj, TEXT("nodes"), GraphPath + TEXT(".nodes"), Nodes, Issues, false) && Nodes)
			{
				for (int32 NodeIndex = 0; NodeIndex < Nodes->Num(); ++NodeIndex)
				{
					TSharedPtr<FJsonObject> NodeObj;
					const FString NodePath = FString::Printf(TEXT("%s.nodes[%d]"), *GraphPath, NodeIndex);
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Nodes)[NodeIndex], NodePath, NodeObj, Issues))
					{
						continue;
					}
					UeAgentJsonDiagnostics::WarnUnknownFields(NodeObj, NodePath, { TEXT("node_type"), TEXT("name"), TEXT("title"), TEXT("position"), TEXT("script_struct"), TEXT("method"), TEXT("notation"), TEXT("variable_name"), TEXT("cpp_type"), TEXT("cpp_type_object_path"), TEXT("is_getter"), TEXT("default_value"), TEXT("pin_defaults"), TEXT("selector"), TEXT("display_name"), TEXT("category_path"), TEXT("control_type"), TEXT("value_kind"), TEXT("availability"), TEXT("operations") }, Issues);

					FString NodeType;
					FString NodeName;
					NodeObj->TryGetStringField(TEXT("node_type"), NodeType);
					NodeObj->TryGetStringField(TEXT("name"), NodeName);
					FVector2D Position = FVector2D::ZeroVector;
					ReadVector2DField(NodeObj, TEXT("position"), NodePath + TEXT(".position"), Position, Issues, false);

					URigVMNode* NewNode = nullptr;
					if (!bReplaceNodes && !NodeName.TrimStartAndEnd().IsEmpty())
					{
						for (URigVMNode* ExistingNode : Graph->GetNodes())
						{
							if (ExistingNode && ExistingNode->GetName().Equals(NodeName, ESearchCase::IgnoreCase))
							{
								NewNode = ExistingNode;
								break;
							}
						}
					}

					if (NodeType.Equals(TEXT("unit"), ESearchCase::IgnoreCase))
					{
						FString ScriptStructPath;
						if (UeAgentJsonDiagnostics::ReadStringField(NodeObj, TEXT("script_struct"), NodePath + TEXT(".script_struct"), ScriptStructPath, Issues, true))
						{
							if (NewNode)
							{
								if (const URigVMUnitNode* ExistingUnitNode = Cast<URigVMUnitNode>(NewNode))
								{
									const FString ExistingStructPath = ExistingUnitNode->GetScriptStruct() ? ExistingUnitNode->GetScriptStruct()->GetPathName() : FString();
									if (!ExistingStructPath.Equals(ScriptStructPath, ESearchCase::IgnoreCase))
									{
										UeAgentJsonDiagnostics::AddIssue(
											Issues,
											TEXT("warning"),
											TEXT("rigvm_existing_unit_struct_mismatch"),
											NodePath + TEXT(".script_struct"),
											FString::Printf(TEXT("Existing node '%s' uses struct '%s', requested '%s'. Pin defaults will still be applied when pins exist."), *NodeName, *ExistingStructPath, *ScriptStructPath));
									}
								}
								else
								{
									UeAgentJsonDiagnostics::AddIssue(
										Issues,
										TEXT("warning"),
										TEXT("rigvm_existing_node_type_mismatch"),
										NodePath,
										FString::Printf(TEXT("Existing node '%s' is not a unit node. Pin defaults will still be applied when pins exist."), *NodeName));
								}
							}
							else
							{
								FString MethodText = TEXT("Execute");
								NodeObj->TryGetStringField(TEXT("method"), MethodText);
								NewNode = Controller->AddUnitNodeFromStructPath(ScriptStructPath, FName(*MethodText), Position, NodeName, true, false);
								if (!NewNode)
								{
									UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("rigvm_add_unit_node_failed"), NodePath, FString::Printf(TEXT("Failed to add unit node for struct '%s'."), *ScriptStructPath));
								}
							}
						}
					}
					else if (NodeType.Equals(TEXT("template"), ESearchCase::IgnoreCase))
					{
						FString Notation;
						if (UeAgentJsonDiagnostics::ReadStringField(NodeObj, TEXT("notation"), NodePath + TEXT(".notation"), Notation, Issues, true))
						{
							if (!NewNode)
							{
								NewNode = Controller->AddTemplateNode(FName(*Notation), Position, NodeName, true, false);
								if (!NewNode)
								{
									UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("rigvm_add_template_node_failed"), NodePath, FString::Printf(TEXT("Failed to add template node '%s'."), *Notation));
								}
							}
						}
					}
					else if (NodeType.Equals(TEXT("variable"), ESearchCase::IgnoreCase))
					{
						FString VariableName;
						FString CppType;
						FString CppTypeObjectPath;
						FString DefaultValue;
						bool bIsGetter = true;
						UeAgentJsonDiagnostics::ReadStringField(NodeObj, TEXT("variable_name"), NodePath + TEXT(".variable_name"), VariableName, Issues, true);
						UeAgentJsonDiagnostics::ReadStringField(NodeObj, TEXT("cpp_type"), NodePath + TEXT(".cpp_type"), CppType, Issues, true);
						NodeObj->TryGetStringField(TEXT("cpp_type_object_path"), CppTypeObjectPath);
						NodeObj->TryGetStringField(TEXT("default_value"), DefaultValue);
						UeAgentJsonDiagnostics::ReadBoolField(NodeObj, TEXT("is_getter"), NodePath + TEXT(".is_getter"), bIsGetter, Issues, false);
						if (!VariableName.IsEmpty() && !CppType.IsEmpty())
						{
							if (!NewNode)
							{
								NewNode = Controller->AddVariableNodeFromObjectPath(FName(*VariableName), CppType, CppTypeObjectPath, bIsGetter, DefaultValue, Position, NodeName, true, false);
								if (!NewNode)
								{
									UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("rigvm_add_variable_node_failed"), NodePath, FString::Printf(TEXT("Failed to add variable node '%s'."), *VariableName));
								}
							}
						}
					}

					const TSharedPtr<FJsonObject>* PinDefaultsObj = nullptr;
					if (NewNode && NodeObj->TryGetObjectField(TEXT("pin_defaults"), PinDefaultsObj) && PinDefaultsObj && PinDefaultsObj->IsValid())
					{
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PinDefaultsObj)->Values)
						{
							FString ValueText;
							if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(ValueText))
							{
								UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_field_type_mismatch"), NodePath + TEXT(".pin_defaults.") + Pair.Key, TEXT("Pin default values must be strings."));
								continue;
							}
							const FString FullPinPath = Pair.Key.StartsWith(NewNode->GetName() + TEXT(".")) ? Pair.Key : NewNode->GetName() + TEXT(".") + Pair.Key;
							FString LeafPinName = FullPinPath;
							int32 LastDotIndex = INDEX_NONE;
							if (FullPinPath.FindLastChar(TEXT('.'), LastDotIndex))
							{
								LeafPinName = FullPinPath.Mid(LastDotIndex + 1);
							}
							if (LeafPinName.Equals(TEXT("WorkData"), ESearchCase::IgnoreCase))
							{
								continue;
							}
							if (!Controller->SetPinDefaultValue(FullPinPath, ValueText, true, true, false, false, true))
							{
								UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("rigvm_set_pin_default_failed"), NodePath + TEXT(".pin_defaults.") + Pair.Key, FString::Printf(TEXT("Failed to set pin default '%s'."), *FullPinPath));
							}
						}
					}
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
			if (UeAgentJsonDiagnostics::ReadArrayField(GraphObj, TEXT("links"), GraphPath + TEXT(".links"), Links, Issues, false) && Links)
			{
				for (int32 LinkIndex = 0; LinkIndex < Links->Num(); ++LinkIndex)
				{
					TSharedPtr<FJsonObject> LinkObj;
					const FString LinkPath = FString::Printf(TEXT("%s.links[%d]"), *GraphPath, LinkIndex);
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Links)[LinkIndex], LinkPath, LinkObj, Issues))
					{
						continue;
					}
					UeAgentJsonDiagnostics::WarnUnknownFields(LinkObj, LinkPath, { TEXT("source"), TEXT("target") }, Issues);

					FString Source;
					FString Target;
					UeAgentJsonDiagnostics::ReadStringField(LinkObj, TEXT("source"), LinkPath + TEXT(".source"), Source, Issues, true);
					UeAgentJsonDiagnostics::ReadStringField(LinkObj, TEXT("target"), LinkPath + TEXT(".target"), Target, Issues, true);
					bool bLinkAlreadyExists = false;
					if (!Source.IsEmpty() && !Target.IsEmpty())
					{
						for (URigVMLink* ExistingLink : Graph->GetLinks())
						{
							if (ExistingLink
								&& ExistingLink->GetSourcePinPath().Equals(Source, ESearchCase::IgnoreCase)
								&& ExistingLink->GetTargetPinPath().Equals(Target, ESearchCase::IgnoreCase))
							{
								bLinkAlreadyExists = true;
								break;
							}
						}
					}
					if (!Source.IsEmpty() && !Target.IsEmpty() && !bLinkAlreadyExists && !Controller->AddLink(Source, Target, true, false))
					{
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("rigvm_add_link_failed"), LinkPath, FString::Printf(TEXT("Failed to link '%s' -> '%s'."), *Source, *Target));
					}
				}
			}
		}
		return !HasBlockingIssue(Issues);
	}

	void MarkControlRigModified(UControlRigBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return;
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<const TCHAR*> Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TCHAR* Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(FString(Item)));
		}
		return Values;
	}

	TSharedPtr<FJsonObject> MakeEmptyItemsJson(const FString& Schema)
	{
		return MakeEmptyItemsJson(Schema, TEXT("items"));
	}

	TSharedPtr<FJsonObject> MakeEmptyItemsJson(const FString& Schema, const FString& FieldName)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Schema.IsEmpty())
		{
			Obj->SetStringField(TEXT("schema"), Schema);
		}
		Obj->SetArrayField(FieldName, {});
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeUnsupportedProfilesJson()
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.unsupported_profiles.v1"));
		Obj->SetStringField(TEXT("authoring_policy"), TEXT("reported_only_not_exported_as_editable_files"));

		TArray<TSharedPtr<FJsonValue>> Profiles;
		auto AddProfile = [&Profiles](const TCHAR* FilePath, const TCHAR* Schema, const TCHAR* Profile, const TCHAR* ApplyPolicy)
		{
			TSharedPtr<FJsonObject> ProfileObj = MakeShared<FJsonObject>();
			ProfileObj->SetStringField(TEXT("file_path"), FilePath);
			ProfileObj->SetStringField(TEXT("schema"), Schema);
			ProfileObj->SetStringField(TEXT("profile"), Profile);
			ProfileObj->SetStringField(TEXT("apply_policy"), ApplyPolicy);
			Profiles.Add(MakeShared<FJsonValueObject>(ProfileObj));
		};

		AddProfile(TEXT("graphs/construction_event.json"), TEXT("ue_agent_interface.control_rig.graph.construction_event.v1"), TEXT("construction_event"), TEXT("use graphs/graphs.json for canonical RigVM graph apply"));
		AddProfile(TEXT("graphs/forward_solve.json"), TEXT("ue_agent_interface.control_rig.graph.forward_solve.v1"), TEXT("forward_solve"), TEXT("use graphs/graphs.json for canonical RigVM graph apply"));
		AddProfile(TEXT("graphs/backward_solve.json"), TEXT("ue_agent_interface.control_rig.graph.backward_solve.v1"), TEXT("backward_solve"), TEXT("use graphs/graphs.json for canonical RigVM graph apply"));
		AddProfile(TEXT("variables/exposed_properties.json"), TEXT("ue_agent_interface.control_rig.exposed_properties.v1"), TEXT("exposed_properties"), TEXT("reserved_for_anim_blueprint_mapping profile"));
		AddProfile(TEXT("functions/functions.json"), TEXT("ue_agent_interface.control_rig.functions.v1"), TEXT("functions"), TEXT("function declaration writes are not silently applied; use graphs/graphs.json for existing function graph nodes"));
		AddProfile(TEXT("modular/modules.json"), TEXT("ue_agent_interface.control_rig.modules.v1"), TEXT("modules"), TEXT("modular rig module writes require Modular Rig controller support"));
		AddProfile(TEXT("modular/connections.json"), TEXT("ue_agent_interface.control_rig.module_connections.v1"), TEXT("module_connections"), TEXT("modular rig connection writes require Modular Rig controller support"));
		AddProfile(TEXT("modular/connectors.json"), TEXT("ue_agent_interface.control_rig.module_connectors.v1"), TEXT("module_connectors"), TEXT("connector writes are validated as unsupported_apply instead of ignored"));
		AddProfile(TEXT("presets/foot_ground_fbik.json"), TEXT("ue_agent_interface.control_rig.preset.foot_ground_fbik.v1"), TEXT("foot_ground_fbik"), TEXT("preset documents expected graph/hierarchy layout; apply only through explicit graphs/hierarchy files"));
		AddProfile(TEXT("raw_properties.json"), TEXT("ue_agent_interface.raw_properties.v1"), TEXT("raw_properties"), TEXT("raw fallback writes must go through asset_apply_property_json until Control Rig conflict checks are complete"));
		AddProfile(TEXT("readonly_properties.json"), TEXT("ue_agent_interface.readonly_properties.v1"), TEXT("readonly_properties"), TEXT("readonly"));

		Obj->SetArrayField(TEXT("profiles"), Profiles);
		Obj->SetNumberField(TEXT("profile_count"), Profiles.Num());
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeCompileReportJson(UControlRigBlueprint* Blueprint)
	{
		return MakeCompileReportJson(Blueprint, nullptr);
	}

	TSharedPtr<FJsonObject> MakeCompileReportJson(UControlRigBlueprint* Blueprint, const FCompilerResultsLog* CompilerLog)
	{
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.compile_report.v1"));
		Report->SetStringField(TEXT("asset_path"), Blueprint ? NormalizeAssetPath(Blueprint->GetPathName()) : FString());
		Report->SetNumberField(TEXT("blueprint_status"), Blueprint ? static_cast<int32>(Blueprint->Status) : 0);
		Report->SetBoolField(TEXT("compiled"), Blueprint ? Blueprint->Status != BS_Error : false);

		TArray<TSharedPtr<FJsonValue>> Messages;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		if (CompilerLog)
		{
			ErrorCount = CompilerLog->NumErrors;
			WarningCount = CompilerLog->NumWarnings;
			for (const TSharedRef<FTokenizedMessage>& Message : CompilerLog->Messages)
			{
				TSharedPtr<FJsonObject> MessageObj = MakeShared<FJsonObject>();
				FString Severity = TEXT("unknown");
				switch (Message->GetSeverity())
				{
				case EMessageSeverity::Error: Severity = TEXT("error"); break;
				case EMessageSeverity::Warning: Severity = TEXT("warning"); break;
				case EMessageSeverity::PerformanceWarning: Severity = TEXT("performance_warning"); break;
				case EMessageSeverity::Info: Severity = TEXT("info"); break;
				default: break;
				}
				MessageObj->SetStringField(TEXT("severity"), Severity);
				MessageObj->SetStringField(TEXT("message"), Message->ToText().ToString());
				MessageObj->SetStringField(TEXT("identifier"), Message->GetIdentifier().ToString());
				Messages.Add(MakeShared<FJsonValueObject>(MessageObj));
			}
		}
		Report->SetNumberField(TEXT("error_count"), ErrorCount);
		Report->SetNumberField(TEXT("warning_count"), WarningCount);
		Report->SetNumberField(TEXT("message_count"), Messages.Num());
		Report->SetArrayField(TEXT("messages"), Messages);
		return Report;
	}

	TSharedPtr<FJsonObject> MakeRuntimeProbeReportJson()
	{
		return MakeRuntimeProbeReportJson(TEXT("not_run"));
	}

	TSharedPtr<FJsonObject> MakeRuntimeProbeReportJson(const FString& Mode)
	{
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.runtime_probe_report.v1"));
		Report->SetStringField(TEXT("mode"), Mode);
		Report->SetBoolField(TEXT("executed"), false);
		Report->SetArrayField(TEXT("issues"), {});
		return Report;
	}

	TSharedPtr<FJsonObject> MakeRigVMUnitRegistryJson()
	{
		TSharedPtr<FJsonObject> Registry = MakeShared<FJsonObject>();
		Registry->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.unit_registry.v1"));
		Registry->SetStringField(TEXT("engine_target"), TEXT("UE_5.6"));

		TArray<TSharedPtr<FJsonValue>> UnitValues;
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			UScriptStruct* Struct = *It;
			if (!Struct || Struct == FRigUnit::StaticStruct() || !Struct->IsChildOf(FRigUnit::StaticStruct()))
			{
				continue;
			}

			TSharedPtr<FJsonObject> UnitObj = MakeShared<FJsonObject>();
			UnitObj->SetStringField(TEXT("struct_path"), Struct->GetPathName());
			UnitObj->SetStringField(TEXT("struct_name"), Struct->GetName());
			UnitObj->SetStringField(TEXT("display_name"), Struct->GetDisplayNameText().ToString());
			UnitObj->SetStringField(TEXT("category"), Struct->GetMetaData(TEXT("Category")));
			UnitObj->SetBoolField(TEXT("deprecated"), Struct->HasMetaData(TEXT("Deprecated")));

			TArray<TSharedPtr<FJsonValue>> PinValues;
			for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
			{
				const FProperty* Prop = *PropIt;
				if (!Prop)
				{
					continue;
				}

				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				FString ExtendedType;
				PinObj->SetStringField(TEXT("name"), Prop->GetName());
				PinObj->SetStringField(TEXT("cpp_type"), Prop->GetCPPType(&ExtendedType) + ExtendedType);
				if (Prop->HasMetaData(TEXT("Input")))
				{
					PinObj->SetStringField(TEXT("direction"), TEXT("input"));
				}
				else if (Prop->HasMetaData(TEXT("Output")))
				{
					PinObj->SetStringField(TEXT("direction"), TEXT("output"));
				}
				else
				{
					PinObj->SetStringField(TEXT("direction"), TEXT("value"));
				}
				PinValues.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			UnitObj->SetArrayField(TEXT("pins"), PinValues);
			UnitValues.Add(MakeShared<FJsonValueObject>(UnitObj));
		}

		Registry->SetArrayField(TEXT("units"), UnitValues);
		Registry->SetNumberField(TEXT("unit_count"), UnitValues.Num());
		return Registry;
	}

	static bool HasUnsupportedWriteContent(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			if (Pair.Key == TEXT("schema") || Pair.Key == TEXT("profile") || Pair.Key == TEXT("apply_policy") || Pair.Key == TEXT("notes"))
			{
				continue;
			}
			if (!Pair.Value.IsValid() || Pair.Value->IsNull())
			{
				continue;
			}
			if (Pair.Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
				if (Pair.Value->TryGetArray(ArrayValue) && ArrayValue && ArrayValue->Num() == 0)
				{
					continue;
				}
				return true;
			}
			if (Pair.Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
				if (Pair.Value->TryGetObject(ObjectValue) && ObjectValue && ObjectValue->IsValid() && (*ObjectValue)->Values.Num() == 0)
				{
					continue;
				}
				return true;
			}
			FString StringValue;
			if (Pair.Value->TryGetString(StringValue) && StringValue.IsEmpty())
			{
				continue;
			}
			return true;
		}
		return false;
	}

	void AddUnsupportedApplyIssueIfNeeded(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& FilePath,
		const FString& Profile,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!HasUnsupportedWriteContent(Obj))
		{
			return;
		}
		UeAgentJsonDiagnostics::AddIssue(
			Issues,
			TEXT("error"),
			TEXT("unsupported_apply_profile"),
			FilePath,
			FString::Printf(TEXT("%s contains requested writes, but this profile is not applied by control_rig_apply_folder. Move supported state into the documented hierarchy/graphs/settings files or use the explicit action workflow."), *Profile));
	}

	TSharedPtr<FJsonObject> MakeCoverageJson()
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetStringField(TEXT("status"), TEXT("complete_folder_profile"));
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Coverage->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.folder.v1"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetArrayField(TEXT("covered_profiles"), MakeStringArray({
			TEXT("asset"),
			TEXT("settings_preview"),
			TEXT("shape_library_reference"),
			TEXT("hierarchy_bones"),
			TEXT("hierarchy_nulls"),
			TEXT("hierarchy_controls"),
			TEXT("hierarchy_curves"),
			TEXT("variables"),
			TEXT("rigvm_graphs"),
			TEXT("compile_report"),
			TEXT("runtime_probe"),
			TEXT("editor_view"),
			TEXT("shape_library_asset")
		}));
		Coverage->SetArrayField(TEXT("readonly_profiles"), MakeStringArray({
			TEXT("generated_class"),
			TEXT("compiled_vm_bytecode"),
			TEXT("editor_session_only_view_state")
		}));
		Coverage->SetArrayField(TEXT("unsupported_apply"), MakeStringArray({
			TEXT("graphs/construction_event.json writes"),
			TEXT("graphs/forward_solve.json writes"),
			TEXT("graphs/backward_solve.json writes"),
			TEXT("variables/exposed_properties.json writes"),
			TEXT("functions/functions.json writes"),
			TEXT("modular/modules.json writes"),
			TEXT("modular/connections.json writes"),
			TEXT("modular/connectors.json writes"),
			TEXT("presets/foot_ground_fbik.json writes"),
			TEXT("raw_properties.json fallback writes"),
			TEXT("readonly_properties.json writes")
		}));
		Coverage->SetNumberField(TEXT("unsupported_apply_count"), 11);
		Coverage->SetArrayField(TEXT("blocking_gaps"), {});
		Coverage->SetStringField(TEXT("covered"), TEXT("asset settings, preview mesh, shape library references, hierarchy bones/nulls/controls/curves, RigVM graph nodes/links/pin defaults, compile report, runtime probe, editor view commands"));
		Coverage->SetStringField(TEXT("notes"), TEXT("Control Rig asset authoring is folder JSON first. Cross-asset AnimBlueprint/Sequencer integration stays in those asset workflows or explicit action commands."));
		return Coverage;
	}
