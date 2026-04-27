// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UeAgentJsonDiagnostics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "Engine/CurveTable.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UeAgentCurveJson
{
	static constexpr const TCHAR* SchemaName = TEXT("ue_agent_interface.curve.v1");
	static constexpr float KeyTimeTolerance = 1.0e-4f;
	static constexpr double ValueTolerance = 1.0e-4;

	static TSharedPtr<FJsonObject> MakeCurveIssue(
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message,
		const FString& Expected = FString(),
		const FString& Actual = FString())
	{
		TSharedPtr<FJsonObject> IssueObj = UeAgentJsonDiagnostics::MakeIssue(Severity, Code, Path, Message);
		if (!Expected.IsEmpty())
		{
			IssueObj->SetStringField(TEXT("expected"), Expected);
		}
		if (!Actual.IsEmpty())
		{
			IssueObj->SetStringField(TEXT("actual"), Actual);
		}
		return IssueObj;
	}

	static void AddCurveIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message,
		const FString& Expected = FString(),
		const FString& Actual = FString())
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeCurveIssue(Severity, Code, Path, Message, Expected, Actual)));
	}

	static bool HasError(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
		{
			const TSharedPtr<FJsonObject>* IssueObj = nullptr;
			if (IssueValue.IsValid()
				&& IssueValue->TryGetObject(IssueObj)
				&& IssueObj
				&& IssueObj->IsValid()
				&& (*IssueObj)->GetStringField(TEXT("severity")).Equals(TEXT("error"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

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
		FString ObjectPath = InPath;
		ObjectPath.TrimStartAndEndInline();
		if (ObjectPath.IsEmpty())
		{
			return FString();
		}
		if (ObjectPath.Contains(TEXT(".")))
		{
			return ObjectPath;
		}
		const FString AssetPath = NormalizeAssetPath(ObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadObjectByPath(const FString& InPath)
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

	static FString InterpModeToString(const ERichCurveInterpMode Mode)
	{
		switch (Mode)
		{
		case RCIM_Linear: return TEXT("Linear");
		case RCIM_Constant: return TEXT("Constant");
		case RCIM_Cubic: return TEXT("Cubic");
		case RCIM_None: return TEXT("None");
		default: return TEXT("Unknown");
		}
	}

	static FString TangentModeToString(const ERichCurveTangentMode Mode)
	{
		switch (Mode)
		{
		case RCTM_Auto: return TEXT("Auto");
		case RCTM_User: return TEXT("User");
		case RCTM_Break: return TEXT("Break");
		case RCTM_None: return TEXT("None");
		case RCTM_SmartAuto: return TEXT("SmartAuto");
		default: return TEXT("Unknown");
		}
	}

	static FString TangentWeightModeToString(const ERichCurveTangentWeightMode Mode)
	{
		switch (Mode)
		{
		case RCTWM_WeightedNone: return TEXT("WeightedNone");
		case RCTWM_WeightedArrive: return TEXT("WeightedArrive");
		case RCTWM_WeightedLeave: return TEXT("WeightedLeave");
		case RCTWM_WeightedBoth: return TEXT("WeightedBoth");
		default: return TEXT("Unknown");
		}
	}

	static FString ExtrapModeToString(const ERichCurveExtrapolation Mode)
	{
		switch (Mode)
		{
		case RCCE_Cycle: return TEXT("Cycle");
		case RCCE_CycleWithOffset: return TEXT("CycleWithOffset");
		case RCCE_Oscillate: return TEXT("Oscillate");
		case RCCE_Linear: return TEXT("Linear");
		case RCCE_Constant: return TEXT("Constant");
		case RCCE_None: return TEXT("None");
		default: return TEXT("Unknown");
		}
	}

	static bool ParseInterpMode(const FString& InText, ERichCurveInterpMode& OutMode)
	{
		const FString Text = InText.TrimStartAndEnd();
		if (Text.IsEmpty() || Text.Equals(TEXT("Linear"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCIM_Linear"), ESearchCase::IgnoreCase))
		{
			OutMode = RCIM_Linear;
			return true;
		}
		if (Text.Equals(TEXT("Constant"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCIM_Constant"), ESearchCase::IgnoreCase))
		{
			OutMode = RCIM_Constant;
			return true;
		}
		if (Text.Equals(TEXT("Cubic"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCIM_Cubic"), ESearchCase::IgnoreCase))
		{
			OutMode = RCIM_Cubic;
			return true;
		}
		if (Text.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCIM_None"), ESearchCase::IgnoreCase))
		{
			OutMode = RCIM_None;
			return true;
		}
		return false;
	}

	static bool ParseTangentMode(const FString& InText, ERichCurveTangentMode& OutMode)
	{
		const FString Text = InText.TrimStartAndEnd();
		if (Text.IsEmpty() || Text.Equals(TEXT("Auto"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCTM_Auto"), ESearchCase::IgnoreCase))
		{
			OutMode = RCTM_Auto;
			return true;
		}
		if (Text.Equals(TEXT("User"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCTM_User"), ESearchCase::IgnoreCase))
		{
			OutMode = RCTM_User;
			return true;
		}
		if (Text.Equals(TEXT("Break"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCTM_Break"), ESearchCase::IgnoreCase))
		{
			OutMode = RCTM_Break;
			return true;
		}
		if (Text.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCTM_None"), ESearchCase::IgnoreCase))
		{
			OutMode = RCTM_None;
			return true;
		}
		if (Text.Equals(TEXT("SmartAuto"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCTM_SmartAuto"), ESearchCase::IgnoreCase))
		{
			OutMode = RCTM_SmartAuto;
			return true;
		}
		return false;
	}

	static bool ParseTangentWeightMode(const FString& InText, ERichCurveTangentWeightMode& OutMode)
	{
		const FString Text = InText.TrimStartAndEnd();
		if (Text.IsEmpty() || Text.Equals(TEXT("WeightedNone"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCTWM_WeightedNone"), ESearchCase::IgnoreCase))
		{
			OutMode = RCTWM_WeightedNone;
			return true;
		}
		if (Text.Equals(TEXT("WeightedArrive"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("Arrive"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCTWM_WeightedArrive"), ESearchCase::IgnoreCase))
		{
			OutMode = RCTWM_WeightedArrive;
			return true;
		}
		if (Text.Equals(TEXT("WeightedLeave"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("Leave"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCTWM_WeightedLeave"), ESearchCase::IgnoreCase))
		{
			OutMode = RCTWM_WeightedLeave;
			return true;
		}
		if (Text.Equals(TEXT("WeightedBoth"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("Both"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCTWM_WeightedBoth"), ESearchCase::IgnoreCase))
		{
			OutMode = RCTWM_WeightedBoth;
			return true;
		}
		return false;
	}

	static bool ParseExtrapMode(const FString& InText, ERichCurveExtrapolation& OutMode)
	{
		const FString Text = InText.TrimStartAndEnd();
		if (Text.IsEmpty() || Text.Equals(TEXT("Constant"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCCE_Constant"), ESearchCase::IgnoreCase))
		{
			OutMode = RCCE_Constant;
			return true;
		}
		if (Text.Equals(TEXT("Cycle"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCCE_Cycle"), ESearchCase::IgnoreCase))
		{
			OutMode = RCCE_Cycle;
			return true;
		}
		if (Text.Equals(TEXT("CycleWithOffset"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCCE_CycleWithOffset"), ESearchCase::IgnoreCase))
		{
			OutMode = RCCE_CycleWithOffset;
			return true;
		}
		if (Text.Equals(TEXT("Oscillate"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCCE_Oscillate"), ESearchCase::IgnoreCase))
		{
			OutMode = RCCE_Oscillate;
			return true;
		}
		if (Text.Equals(TEXT("Linear"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCCE_Linear"), ESearchCase::IgnoreCase))
		{
			OutMode = RCCE_Linear;
			return true;
		}
		if (Text.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("RCCE_None"), ESearchCase::IgnoreCase))
		{
			OutMode = RCCE_None;
			return true;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildKeyJson(const FRichCurveKey& Key, const int32 KeyIndex)
	{
		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		KeyObj->SetNumberField(TEXT("key_index"), KeyIndex);
		KeyObj->SetNumberField(TEXT("time"), Key.Time);
		KeyObj->SetNumberField(TEXT("time_seconds"), Key.Time);
		KeyObj->SetNumberField(TEXT("value"), Key.Value);
		KeyObj->SetStringField(TEXT("interp_mode"), InterpModeToString(Key.InterpMode));
		KeyObj->SetStringField(TEXT("tangent_mode"), TangentModeToString(Key.TangentMode));
		KeyObj->SetStringField(TEXT("tangent_weight_mode"), TangentWeightModeToString(Key.TangentWeightMode));
		KeyObj->SetNumberField(TEXT("arrive_tangent"), Key.ArriveTangent);
		KeyObj->SetNumberField(TEXT("leave_tangent"), Key.LeaveTangent);
		KeyObj->SetNumberField(TEXT("arrive_tangent_weight"), Key.ArriveTangentWeight);
		KeyObj->SetNumberField(TEXT("leave_tangent_weight"), Key.LeaveTangentWeight);
		return KeyObj;
	}

	static TSharedPtr<FJsonObject> BuildChannelJson(const FRichCurve& Curve, const FString& ValueType = TEXT("double"))
	{
		TSharedPtr<FJsonObject> ChannelObj = MakeShared<FJsonObject>();
		ChannelObj->SetStringField(TEXT("value_type"), ValueType);
		TSharedPtr<FJsonObject> DefaultObj = MakeShared<FJsonObject>();
		const bool bHasDefault = Curve.GetDefaultValue() != MAX_flt;
		DefaultObj->SetBoolField(TEXT("enabled"), bHasDefault);
		if (bHasDefault)
		{
			DefaultObj->SetNumberField(TEXT("value"), Curve.GetDefaultValue());
		}
		ChannelObj->SetObjectField(TEXT("default_value"), DefaultObj);

		TArray<TSharedPtr<FJsonValue>> KeysJson;
		const TArray<FRichCurveKey>& Keys = Curve.GetConstRefOfKeys();
		for (int32 KeyIndex = 0; KeyIndex < Keys.Num(); ++KeyIndex)
		{
			KeysJson.Add(MakeShared<FJsonValueObject>(BuildKeyJson(Keys[KeyIndex], KeyIndex)));
		}
		ChannelObj->SetArrayField(TEXT("keys"), KeysJson);
		ChannelObj->SetNumberField(TEXT("key_count"), Keys.Num());
		return ChannelObj;
	}

	static TSharedPtr<FJsonObject> MakeCurveRoot(const FString& CurveKind, const FString& Storage, const FString& CarrierCppType)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), SchemaName);
		Root->SetStringField(TEXT("curve_kind"), CurveKind);
		Root->SetStringField(TEXT("storage"), Storage);
		Root->SetStringField(TEXT("carrier_cpp_type"), CarrierCppType);
		TSharedPtr<FJsonObject> TimeDomainObj = MakeShared<FJsonObject>();
		TimeDomainObj->SetStringField(TEXT("unit"), TEXT("seconds"));
		Root->SetObjectField(TEXT("time_domain"), TimeDomainObj);
		return Root;
	}

	static TSharedPtr<FJsonObject> BuildFloatCurveJson(const FRichCurve& Curve, const FString& Storage, const FString& CarrierCppType)
	{
		TSharedPtr<FJsonObject> Root = MakeCurveRoot(TEXT("float"), Storage, CarrierCppType);
		Root->SetStringField(TEXT("pre_infinity_extrap"), ExtrapModeToString(Curve.PreInfinityExtrap));
		Root->SetStringField(TEXT("post_infinity_extrap"), ExtrapModeToString(Curve.PostInfinityExtrap));
		TSharedPtr<FJsonObject> ChannelsObj = MakeShared<FJsonObject>();
		ChannelsObj->SetObjectField(TEXT("value"), BuildChannelJson(Curve));
		Root->SetObjectField(TEXT("channels"), ChannelsObj);
		return Root;
	}

	static TSharedPtr<FJsonObject> BuildVectorCurveJson(const FRichCurve* Curves, const FString& CurveKind, const FString& Storage, const FString& CarrierCppType)
	{
		TSharedPtr<FJsonObject> Root = MakeCurveRoot(CurveKind, Storage, CarrierCppType);
		TSharedPtr<FJsonObject> ChannelsObj = MakeShared<FJsonObject>();
		ChannelsObj->SetObjectField(TEXT("x"), BuildChannelJson(Curves[0]));
		ChannelsObj->SetObjectField(TEXT("y"), BuildChannelJson(Curves[1]));
		ChannelsObj->SetObjectField(TEXT("z"), BuildChannelJson(Curves[2]));
		Root->SetObjectField(TEXT("channels"), ChannelsObj);
		return Root;
	}

	static TSharedPtr<FJsonObject> BuildLinearColorCurveJson(const FRichCurve* Curves, const FString& Storage, const FString& CarrierCppType)
	{
		TSharedPtr<FJsonObject> Root = MakeCurveRoot(TEXT("linear_color"), Storage, CarrierCppType);
		TSharedPtr<FJsonObject> ChannelsObj = MakeShared<FJsonObject>();
		ChannelsObj->SetObjectField(TEXT("r"), BuildChannelJson(Curves[0]));
		ChannelsObj->SetObjectField(TEXT("g"), BuildChannelJson(Curves[1]));
		ChannelsObj->SetObjectField(TEXT("b"), BuildChannelJson(Curves[2]));
		ChannelsObj->SetObjectField(TEXT("a"), BuildChannelJson(Curves[3]));
		Root->SetObjectField(TEXT("channels"), ChannelsObj);
		return Root;
	}

	static TSharedPtr<FJsonObject> BuildAssetReferenceJson(const UObject* Asset, const FString& PropertyCppType = FString())
	{
		TSharedPtr<FJsonObject> Root = MakeCurveRoot(TEXT("asset_reference"), TEXT("asset_reference"), PropertyCppType.IsEmpty() ? TEXT("UObject") : PropertyCppType);
		TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
		RefObj->SetStringField(TEXT("asset_path"), Asset ? NormalizeAssetPath(Asset->GetOutermost()->GetName()) : TEXT(""));
		RefObj->SetStringField(TEXT("object_path"), Asset ? Asset->GetPathName() : TEXT(""));
		RefObj->SetStringField(TEXT("asset_class"), Asset && Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		RefObj->SetBoolField(TEXT("required"), Asset != nullptr);
		Root->SetObjectField(TEXT("asset_reference"), RefObj);
		return Root;
	}

	static TSharedPtr<FJsonObject> BuildCurveTableRowHandleJson(const FCurveTableRowHandle& Handle)
	{
		TSharedPtr<FJsonObject> Root = MakeCurveRoot(TEXT("curve_table_row"), TEXT("table_row"), TEXT("FCurveTableRowHandle"));
		TSharedPtr<FJsonObject> TableObj = MakeShared<FJsonObject>();
		TableObj->SetStringField(TEXT("asset_path"), Handle.CurveTable ? NormalizeAssetPath(Handle.CurveTable->GetOutermost()->GetName()) : TEXT(""));
		TableObj->SetStringField(TEXT("object_path"), Handle.CurveTable ? Handle.CurveTable->GetPathName() : TEXT(""));
		TableObj->SetStringField(TEXT("row_name"), Handle.RowName.ToString());
		TableObj->SetStringField(TEXT("row_type"), TEXT("RichCurve"));
		Root->SetObjectField(TEXT("curve_table"), TableObj);
		if (const FRichCurve* RowCurve = Handle.GetRichCurve(TEXT("UeAgentCurveJson"), false))
		{
			Root->SetObjectField(TEXT("row_curve"), BuildFloatCurveJson(*RowCurve, TEXT("table_row_readonly"), TEXT("FRichCurve")));
		}
		return Root;
	}

	static bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, TSharedPtr<FJsonObject>& OutObj)
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (Obj.IsValid() && Obj->TryGetObjectField(FieldName, ObjPtr) && ObjPtr && ObjPtr->IsValid())
		{
			OutObj = *ObjPtr;
			return true;
		}
		return false;
	}

	static bool TryGetNumberFieldAny(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Fields, double& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& FieldName : Fields)
		{
			if (Obj->TryGetNumberField(FieldName, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static void WarnUnknownKeyFields(const TSharedPtr<FJsonObject>& KeyObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			KeyObj,
			Path,
			{
				TEXT("key_index"),
				TEXT("time"),
				TEXT("time_seconds"),
				TEXT("value"),
				TEXT("interp_mode"),
				TEXT("tangent_mode"),
				TEXT("tangent_weight_mode"),
				TEXT("arrive_tangent"),
				TEXT("leave_tangent"),
				TEXT("arrive_tangent_weight"),
				TEXT("leave_tangent_weight")
			},
			Issues);
	}

	static void WarnUnknownChannelFields(const TSharedPtr<FJsonObject>& ChannelObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			ChannelObj,
			Path,
			{
				TEXT("value_type"),
				TEXT("default_value"),
				TEXT("keys"),
				TEXT("key_count")
			},
			Issues);

		TSharedPtr<FJsonObject> DefaultObj;
		if (TryGetObjectField(ChannelObj, TEXT("default_value"), DefaultObj))
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(
				DefaultObj,
				Path + TEXT(".default_value"),
				{ TEXT("enabled"), TEXT("value") },
				Issues);
		}

		const TArray<TSharedPtr<FJsonValue>>* KeyValues = nullptr;
		if (ChannelObj.IsValid() && ChannelObj->TryGetArrayField(TEXT("keys"), KeyValues) && KeyValues)
		{
			for (int32 KeyIndex = 0; KeyIndex < KeyValues->Num(); ++KeyIndex)
			{
				const FString KeyPath = FString::Printf(TEXT("%s.keys[%d]"), *Path, KeyIndex);
				TSharedPtr<FJsonObject> KeyObj;
				if (UeAgentJsonDiagnostics::ReadObjectFromValue((*KeyValues)[KeyIndex], KeyPath, KeyObj, Issues))
				{
					WarnUnknownKeyFields(KeyObj, KeyPath, Issues);
				}
			}
		}
	}

	static void WarnUnknownCurveFields(const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			RootObj,
			Path,
			{
				TEXT("schema"),
				TEXT("format_version"),
				TEXT("asset_path"),
				TEXT("object_path"),
				TEXT("asset_class"),
				TEXT("curve_kind"),
				TEXT("storage"),
				TEXT("carrier_cpp_type"),
				TEXT("time_domain"),
				TEXT("pre_infinity_extrap"),
				TEXT("post_infinity_extrap"),
				TEXT("channels"),
				TEXT("rows"),
				TEXT("row_count"),
				TEXT("table_curve_mode"),
				TEXT("asset_reference"),
				TEXT("curve_table"),
				TEXT("row_curve")
			},
			Issues);

		TSharedPtr<FJsonObject> TimeDomainObj;
		if (TryGetObjectField(RootObj, TEXT("time_domain"), TimeDomainObj))
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(TimeDomainObj, Path + TEXT(".time_domain"), { TEXT("unit") }, Issues);
		}

		TSharedPtr<FJsonObject> ChannelsObj;
		if (TryGetObjectField(RootObj, TEXT("channels"), ChannelsObj))
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(
				ChannelsObj,
				Path + TEXT(".channels"),
				{ TEXT("value"), TEXT("x"), TEXT("y"), TEXT("z"), TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") },
				Issues);
			const TArray<FString> ChannelNames = { TEXT("value"), TEXT("x"), TEXT("y"), TEXT("z"), TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
			for (const FString& ChannelName : ChannelNames)
			{
				TSharedPtr<FJsonObject> ChannelObj;
				if (TryGetObjectField(ChannelsObj, ChannelName, ChannelObj))
				{
					WarnUnknownChannelFields(ChannelObj, Path + TEXT(".channels.") + ChannelName, Issues);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (RootObj.IsValid() && RootObj->TryGetArrayField(TEXT("rows"), Rows) && Rows)
		{
			for (int32 RowIndex = 0; RowIndex < Rows->Num(); ++RowIndex)
			{
				const FString RowPath = FString::Printf(TEXT("%s.rows[%d]"), *Path, RowIndex);
				TSharedPtr<FJsonObject> RowObj;
				if (UeAgentJsonDiagnostics::ReadObjectFromValue((*Rows)[RowIndex], RowPath, RowObj, Issues))
				{
					UeAgentJsonDiagnostics::WarnUnknownFields(RowObj, RowPath, { TEXT("row_name"), TEXT("row_type"), TEXT("curve") }, Issues);
					TSharedPtr<FJsonObject> RowCurveObj;
					if (TryGetObjectField(RowObj, TEXT("curve"), RowCurveObj))
					{
						WarnUnknownCurveFields(RowCurveObj, RowPath + TEXT(".curve"), Issues);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> RefObj;
		if (TryGetObjectField(RootObj, TEXT("asset_reference"), RefObj))
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(RefObj, Path + TEXT(".asset_reference"), { TEXT("asset_path"), TEXT("object_path"), TEXT("asset_class"), TEXT("required") }, Issues);
		}

		TSharedPtr<FJsonObject> TableObj;
		if (TryGetObjectField(RootObj, TEXT("curve_table"), TableObj))
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(TableObj, Path + TEXT(".curve_table"), { TEXT("asset_path"), TEXT("object_path"), TEXT("row_name"), TEXT("row_type") }, Issues);
		}
	}

	static bool ReadChannelObject(const TSharedPtr<FJsonObject>& RootObj, const FString& ChannelName, const FString& Path, TSharedPtr<FJsonObject>& OutChannelObj, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TSharedPtr<FJsonObject> ChannelsObj;
		if (TryGetObjectField(RootObj, TEXT("channels"), ChannelsObj))
		{
			if (TryGetObjectField(ChannelsObj, ChannelName, OutChannelObj))
			{
				return true;
			}
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_channel_missing"), Path + TEXT(".channels.") + ChannelName, FString::Printf(TEXT("Curve channel '%s' is missing."), *ChannelName));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* LegacyKeys = nullptr;
		if (ChannelName.Equals(TEXT("value"), ESearchCase::IgnoreCase) && RootObj.IsValid() && RootObj->TryGetArrayField(TEXT("keys"), LegacyKeys) && LegacyKeys)
		{
			OutChannelObj = MakeShared<FJsonObject>();
			OutChannelObj->SetArrayField(TEXT("keys"), *LegacyKeys);
			return true;
		}

		AddCurveIssue(Issues, TEXT("error"), TEXT("curve_channel_missing"), Path + TEXT(".channels"), TEXT("Curve JSON must contain a channels object."));
		return false;
	}

	static bool ReadKeyObject(const TSharedPtr<FJsonObject>& KeyObj, const FString& Path, FRichCurveKey& OutKey, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		double Time = 0.0;
		if (!TryGetNumberFieldAny(KeyObj, { TEXT("time"), TEXT("time_seconds") }, Time))
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_key_time_missing"), Path + TEXT(".time"), TEXT("Curve key must contain numeric time or time_seconds."), TEXT("number"), TEXT("missing_or_wrong_type"));
			return false;
		}
		double Value = 0.0;
		if (!KeyObj.IsValid() || !KeyObj->TryGetNumberField(TEXT("value"), Value))
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_key_value_type_mismatch"), Path + TEXT(".value"), TEXT("Curve key must contain numeric value."), TEXT("number"), TEXT("missing_or_wrong_type"));
			return false;
		}

		OutKey = FRichCurveKey(static_cast<float>(Time), static_cast<float>(Value));
		FString InterpText;
		if (KeyObj->TryGetStringField(TEXT("interp_mode"), InterpText))
		{
			ERichCurveInterpMode ParsedInterpMode = RCIM_Linear;
			if (ParseInterpMode(InterpText, ParsedInterpMode))
			{
				OutKey.InterpMode = ParsedInterpMode;
			}
			else
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_interp_mode_invalid"), Path + TEXT(".interp_mode"), FString::Printf(TEXT("Invalid interp_mode '%s'."), *InterpText));
			}
		}
		FString TangentText;
		if (KeyObj->TryGetStringField(TEXT("tangent_mode"), TangentText))
		{
			ERichCurveTangentMode ParsedTangentMode = RCTM_Auto;
			if (ParseTangentMode(TangentText, ParsedTangentMode))
			{
				OutKey.TangentMode = ParsedTangentMode;
			}
			else
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_tangent_mode_invalid"), Path + TEXT(".tangent_mode"), FString::Printf(TEXT("Invalid tangent_mode '%s'."), *TangentText));
			}
		}
		FString TangentWeightText;
		if (KeyObj->TryGetStringField(TEXT("tangent_weight_mode"), TangentWeightText))
		{
			ERichCurveTangentWeightMode ParsedWeightMode = RCTWM_WeightedNone;
			if (ParseTangentWeightMode(TangentWeightText, ParsedWeightMode))
			{
				OutKey.TangentWeightMode = ParsedWeightMode;
			}
			else
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_tangent_weight_mode_invalid"), Path + TEXT(".tangent_weight_mode"), FString::Printf(TEXT("Invalid tangent_weight_mode '%s'."), *TangentWeightText));
			}
		}

		double NumberValue = 0.0;
		if (KeyObj->TryGetNumberField(TEXT("arrive_tangent"), NumberValue)) OutKey.ArriveTangent = static_cast<float>(NumberValue);
		if (KeyObj->TryGetNumberField(TEXT("leave_tangent"), NumberValue)) OutKey.LeaveTangent = static_cast<float>(NumberValue);
		if (KeyObj->TryGetNumberField(TEXT("arrive_tangent_weight"), NumberValue)) OutKey.ArriveTangentWeight = static_cast<float>(NumberValue);
		if (KeyObj->TryGetNumberField(TEXT("leave_tangent_weight"), NumberValue)) OutKey.LeaveTangentWeight = static_cast<float>(NumberValue);
		return !HasError(Issues);
	}

	static bool ApplyChannelJsonToRichCurve(const TSharedPtr<FJsonObject>& RootObj, const FString& ChannelName, FRichCurve& OutCurve, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TSharedPtr<FJsonObject> ChannelObj;
		if (!ReadChannelObject(RootObj, ChannelName, Path, ChannelObj, Issues))
		{
			return false;
		}

		TArray<FRichCurveKey> NewKeys;
		const TArray<TSharedPtr<FJsonValue>>* KeyValues = nullptr;
		if (!ChannelObj->TryGetArrayField(TEXT("keys"), KeyValues) || !KeyValues)
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_missing_required_field"), Path + TEXT(".channels.") + ChannelName + TEXT(".keys"), TEXT("Curve channel must contain keys array."), TEXT("array"), TEXT("missing_or_wrong_type"));
			return false;
		}

		float PreviousTime = -MAX_flt;
		for (int32 KeyIndex = 0; KeyIndex < KeyValues->Num(); ++KeyIndex)
		{
			const FString KeyPath = FString::Printf(TEXT("%s.channels.%s.keys[%d]"), *Path, *ChannelName, KeyIndex);
			TSharedPtr<FJsonObject> KeyObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*KeyValues)[KeyIndex], KeyPath, KeyObj, Issues))
			{
				continue;
			}

			FRichCurveKey NewKey;
			if (!ReadKeyObject(KeyObj, KeyPath, NewKey, Issues))
			{
				continue;
			}

			if (NewKey.Time + KeyTimeTolerance < PreviousTime)
			{
				AddCurveIssue(Issues, TEXT("warning"), TEXT("curve_key_time_not_monotonic"), KeyPath + TEXT(".time"), TEXT("Curve keys were not monotonic; keys will be sorted before apply."));
			}
			for (const FRichCurveKey& ExistingKey : NewKeys)
			{
				if (FMath::Abs(ExistingKey.Time - NewKey.Time) <= KeyTimeTolerance)
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_key_duplicate_time"), KeyPath + TEXT(".time"), FString::Printf(TEXT("Duplicate curve key time %.6f."), NewKey.Time));
					break;
				}
			}
			PreviousTime = NewKey.Time;
			NewKeys.Add(NewKey);
		}

		if (HasError(Issues))
		{
			return false;
		}

		NewKeys.Sort([](const FRichCurveKey& A, const FRichCurveKey& B) { return A.Time < B.Time; });
		FRichCurve NewCurve = OutCurve;
		NewCurve.SetKeys(NewKeys);

		TSharedPtr<FJsonObject> DefaultObj;
		if (TryGetObjectField(ChannelObj, TEXT("default_value"), DefaultObj))
		{
			bool bDefaultEnabled = false;
			DefaultObj->TryGetBoolField(TEXT("enabled"), bDefaultEnabled);
			if (bDefaultEnabled)
			{
				double DefaultValue = 0.0;
				if (DefaultObj->TryGetNumberField(TEXT("value"), DefaultValue))
				{
					NewCurve.SetDefaultValue(static_cast<float>(DefaultValue));
				}
				else
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_field_type_mismatch"), Path + TEXT(".channels.") + ChannelName + TEXT(".default_value.value"), TEXT("Enabled curve default value must be numeric."), TEXT("number"), TEXT("missing_or_wrong_type"));
				}
			}
			else
			{
				NewCurve.ClearDefaultValue();
			}
		}

		FString ExtrapText;
		if (RootObj->TryGetStringField(TEXT("pre_infinity_extrap"), ExtrapText))
		{
			ERichCurveExtrapolation Parsed = RCCE_Constant;
			if (ParseExtrapMode(ExtrapText, Parsed))
			{
				NewCurve.PreInfinityExtrap = Parsed;
			}
			else
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_interp_mode_invalid"), Path + TEXT(".pre_infinity_extrap"), FString::Printf(TEXT("Invalid pre_infinity_extrap '%s'."), *ExtrapText));
			}
		}
		if (RootObj->TryGetStringField(TEXT("post_infinity_extrap"), ExtrapText))
		{
			ERichCurveExtrapolation Parsed = RCCE_Constant;
			if (ParseExtrapMode(ExtrapText, Parsed))
			{
				NewCurve.PostInfinityExtrap = Parsed;
			}
			else
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_interp_mode_invalid"), Path + TEXT(".post_infinity_extrap"), FString::Printf(TEXT("Invalid post_infinity_extrap '%s'."), *ExtrapText));
			}
		}

		if (HasError(Issues))
		{
			return false;
		}

		NewCurve.AutoSetTangents();
		OutCurve = NewCurve;
		return true;
	}

	static TSharedPtr<FJsonObject> BuildCurveAssetJson(UObject* Asset)
	{
		TSharedPtr<FJsonObject> Root;
		if (UCurveFloat* CurveFloat = Cast<UCurveFloat>(Asset))
		{
			Root = BuildFloatCurveJson(CurveFloat->FloatCurve, TEXT("curve_asset"), TEXT("UCurveFloat"));
		}
		else if (UCurveVector* CurveVector = Cast<UCurveVector>(Asset))
		{
			Root = BuildVectorCurveJson(CurveVector->FloatCurves, TEXT("vector"), TEXT("curve_asset"), TEXT("UCurveVector"));
		}
		else if (UCurveLinearColor* CurveColor = Cast<UCurveLinearColor>(Asset))
		{
			Root = BuildLinearColorCurveJson(CurveColor->FloatCurves, TEXT("curve_asset"), TEXT("UCurveLinearColor"));
		}
		else if (UCurveTable* CurveTable = Cast<UCurveTable>(Asset))
		{
			Root = MakeCurveRoot(TEXT("curve_table"), TEXT("curve_asset"), TEXT("UCurveTable"));
			TArray<TSharedPtr<FJsonValue>> RowsJson;
			if (CurveTable->GetCurveTableMode() == ECurveTableMode::SimpleCurves)
			{
				Root->SetStringField(TEXT("table_curve_mode"), TEXT("simple"));
				for (const TPair<FName, FSimpleCurve*>& Pair : CurveTable->GetSimpleCurveRowMap())
				{
					if (!Pair.Value)
					{
						continue;
					}
					FRichCurve RichCurveProxy;
					for (const FSimpleCurveKey& Key : Pair.Value->GetConstRefOfKeys())
					{
						RichCurveProxy.AddKey(Key.Time, Key.Value);
					}
					TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
					RowObj->SetStringField(TEXT("row_name"), Pair.Key.ToString());
					RowObj->SetStringField(TEXT("row_type"), TEXT("SimpleCurve"));
					RowObj->SetObjectField(TEXT("curve"), BuildFloatCurveJson(RichCurveProxy, TEXT("curve_table_row"), TEXT("FSimpleCurve")));
					RowsJson.Add(MakeShared<FJsonValueObject>(RowObj));
				}
			}
			else
			{
				Root->SetStringField(TEXT("table_curve_mode"), TEXT("rich"));
				for (const TPair<FName, FRichCurve*>& Pair : CurveTable->GetRichCurveRowMap())
				{
					if (!Pair.Value)
					{
						continue;
					}
					TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
					RowObj->SetStringField(TEXT("row_name"), Pair.Key.ToString());
					RowObj->SetStringField(TEXT("row_type"), TEXT("RichCurve"));
					RowObj->SetObjectField(TEXT("curve"), BuildFloatCurveJson(*Pair.Value, TEXT("curve_table_row"), TEXT("FRichCurve")));
					RowsJson.Add(MakeShared<FJsonValueObject>(RowObj));
				}
			}
			Root->SetArrayField(TEXT("rows"), RowsJson);
			Root->SetNumberField(TEXT("row_count"), RowsJson.Num());
		}

		if (Root.IsValid() && Asset)
		{
			Root->SetStringField(TEXT("asset_path"), NormalizeAssetPath(Asset->GetOutermost()->GetName()));
			Root->SetStringField(TEXT("object_path"), Asset->GetPathName());
			Root->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		}
		return Root;
	}

	static bool ApplyCurveAssetJson(UObject* Asset, const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Asset)
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_not_found"), Path, TEXT("Target curve asset is null."));
			return false;
		}
		if (!RootObj.IsValid())
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_json_parse_failed"), Path, TEXT("Curve JSON object is invalid."));
			return false;
		}
		WarnUnknownCurveFields(RootObj, Path, Issues);

		if (UCurveFloat* CurveFloat = Cast<UCurveFloat>(Asset))
		{
			return ApplyChannelJsonToRichCurve(RootObj, TEXT("value"), CurveFloat->FloatCurve, Path, Issues);
		}
		if (UCurveVector* CurveVector = Cast<UCurveVector>(Asset))
		{
			FRichCurve TempCurves[3] = {
				CurveVector->FloatCurves[0],
				CurveVector->FloatCurves[1],
				CurveVector->FloatCurves[2]
			};
			bool bOk = true;
			bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("x"), TempCurves[0], Path, Issues);
			bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("y"), TempCurves[1], Path, Issues);
			bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("z"), TempCurves[2], Path, Issues);
			if (!bOk || HasError(Issues))
			{
				return false;
			}
			CurveVector->FloatCurves[0] = TempCurves[0];
			CurveVector->FloatCurves[1] = TempCurves[1];
			CurveVector->FloatCurves[2] = TempCurves[2];
			return true;
		}
		if (UCurveLinearColor* CurveColor = Cast<UCurveLinearColor>(Asset))
		{
			FRichCurve TempCurves[4] = {
				CurveColor->FloatCurves[0],
				CurveColor->FloatCurves[1],
				CurveColor->FloatCurves[2],
				CurveColor->FloatCurves[3]
			};
			bool bOk = true;
			bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("r"), TempCurves[0], Path, Issues);
			bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("g"), TempCurves[1], Path, Issues);
			bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("b"), TempCurves[2], Path, Issues);
			bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("a"), TempCurves[3], Path, Issues);
			if (!bOk || HasError(Issues))
			{
				return false;
			}
			CurveColor->FloatCurves[0] = TempCurves[0];
			CurveColor->FloatCurves[1] = TempCurves[1];
			CurveColor->FloatCurves[2] = TempCurves[2];
			CurveColor->FloatCurves[3] = TempCurves[3];
			return true;
		}
		if (UCurveTable* CurveTable = Cast<UCurveTable>(Asset))
		{
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!RootObj->TryGetArrayField(TEXT("rows"), Rows) || !Rows)
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_missing_required_field"), Path + TEXT(".rows"), TEXT("Curve table JSON requires rows array."));
				return false;
			}
			struct FPendingRichCurveTableRow
			{
				FName RowName;
				FRichCurve Curve;
			};
			struct FPendingSimpleCurveTableRow
			{
				FName RowName;
				TArray<FSimpleCurveKey> Keys;
			};
			TArray<FPendingRichCurveTableRow> PendingRichRows;
			TArray<FPendingSimpleCurveTableRow> PendingSimpleRows;
			for (int32 RowIndex = 0; RowIndex < Rows->Num(); ++RowIndex)
			{
				const FString RowPath = FString::Printf(TEXT("%s.rows[%d]"), *Path, RowIndex);
				TSharedPtr<FJsonObject> RowObj;
				if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Rows)[RowIndex], RowPath, RowObj, Issues))
				{
					continue;
				}
				FString RowNameText;
				if (!UeAgentJsonDiagnostics::ReadStringField(RowObj, TEXT("row_name"), RowPath + TEXT(".row_name"), RowNameText, Issues, true) || RowNameText.TrimStartAndEnd().IsEmpty())
				{
					continue;
				}
				TSharedPtr<FJsonObject> RowCurveObj;
				if (!TryGetObjectField(RowObj, TEXT("curve"), RowCurveObj))
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_missing_required_field"), RowPath + TEXT(".curve"), TEXT("Curve table row requires curve object."));
					continue;
				}
				FString RowType;
				RowObj->TryGetStringField(TEXT("row_type"), RowType);
				FString TableMode;
				RootObj->TryGetStringField(TEXT("table_curve_mode"), TableMode);
				const bool bUseSimpleCurve = RowType.Equals(TEXT("SimpleCurve"), ESearchCase::IgnoreCase) || TableMode.Equals(TEXT("simple"), ESearchCase::IgnoreCase);
				if (bUseSimpleCurve)
				{
					FRichCurve RichCurveProxy;
					if (ApplyChannelJsonToRichCurve(RowCurveObj, TEXT("value"), RichCurveProxy, RowPath + TEXT(".curve"), Issues))
					{
						TArray<FSimpleCurveKey> SimpleKeys;
						for (const FRichCurveKey& RichKey : RichCurveProxy.GetConstRefOfKeys())
						{
							SimpleKeys.Add(FSimpleCurveKey(RichKey.Time, RichKey.Value));
						}
						PendingSimpleRows.Add({ FName(*RowNameText.TrimStartAndEnd()), SimpleKeys });
					}
				}
				else
				{
					FRichCurve RichCurve;
					if (ApplyChannelJsonToRichCurve(RowCurveObj, TEXT("value"), RichCurve, RowPath + TEXT(".curve"), Issues))
					{
						PendingRichRows.Add({ FName(*RowNameText.TrimStartAndEnd()), RichCurve });
					}
				}
			}
			if (HasError(Issues))
			{
				return false;
			}
			for (const FPendingSimpleCurveTableRow& PendingRow : PendingSimpleRows)
			{
				FSimpleCurve& RowCurve = CurveTable->AddSimpleCurve(PendingRow.RowName);
				RowCurve.SetKeys(PendingRow.Keys);
			}
			for (const FPendingRichCurveTableRow& PendingRow : PendingRichRows)
			{
				FRichCurve& RowCurve = CurveTable->AddRichCurve(PendingRow.RowName);
				RowCurve = PendingRow.Curve;
			}
			return true;
		}

		AddCurveIssue(Issues, TEXT("error"), TEXT("curve_unsupported_carrier"), Path, FString::Printf(TEXT("Unsupported curve asset class '%s'."), Asset->GetClass() ? *Asset->GetClass()->GetPathName() : TEXT("")));
		return false;
	}

	static bool TryBuildPropertyCurveJson(FProperty* Property, const void* ValuePtr, TSharedPtr<FJsonObject>& OutCurveJson)
	{
		OutCurveJson.Reset();
		if (!Property || !ValuePtr)
		{
			return false;
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr))
			{
				if (ObjectValue->IsA<UCurveFloat>() || ObjectValue->IsA<UCurveVector>() || ObjectValue->IsA<UCurveLinearColor>() || ObjectValue->IsA<UCurveTable>())
				{
					OutCurveJson = BuildAssetReferenceJson(ObjectValue, Property->GetCPPType());
					return true;
				}
			}
			return ObjectProperty->PropertyClass && ObjectProperty->PropertyClass->IsChildOf(UCurveBase::StaticClass());
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == FRichCurve::StaticStruct())
			{
				OutCurveJson = BuildFloatCurveJson(*static_cast<const FRichCurve*>(ValuePtr), TEXT("rich_curve_property"), TEXT("FRichCurve"));
				return true;
			}
			if (StructProperty->Struct == FSimpleCurve::StaticStruct())
			{
				const FSimpleCurve* SimpleCurve = static_cast<const FSimpleCurve*>(ValuePtr);
				FRichCurve RichCurveProxy;
				for (const FSimpleCurveKey& Key : SimpleCurve->GetConstRefOfKeys())
				{
					RichCurveProxy.AddKey(Key.Time, Key.Value);
				}
				OutCurveJson = BuildFloatCurveJson(RichCurveProxy, TEXT("simple_curve_property"), TEXT("FSimpleCurve"));
				return true;
			}
			if (StructProperty->Struct == FRuntimeFloatCurve::StaticStruct())
			{
				const FRuntimeFloatCurve* RuntimeCurve = static_cast<const FRuntimeFloatCurve*>(ValuePtr);
				if (RuntimeCurve->ExternalCurve)
				{
					OutCurveJson = BuildAssetReferenceJson(RuntimeCurve->ExternalCurve, Property->GetCPPType());
				}
				else if (const FRichCurve* RichCurve = RuntimeCurve->GetRichCurveConst())
				{
					OutCurveJson = BuildFloatCurveJson(*RichCurve, TEXT("runtime_float_curve"), TEXT("FRuntimeFloatCurve"));
				}
				return OutCurveJson.IsValid();
			}
			if (StructProperty->Struct == FRuntimeVectorCurve::StaticStruct())
			{
				const FRuntimeVectorCurve* RuntimeCurve = static_cast<const FRuntimeVectorCurve*>(ValuePtr);
				if (RuntimeCurve->ExternalCurve)
				{
					OutCurveJson = BuildAssetReferenceJson(RuntimeCurve->ExternalCurve, Property->GetCPPType());
				}
				else
				{
					OutCurveJson = BuildVectorCurveJson(RuntimeCurve->VectorCurves, TEXT("vector"), TEXT("runtime_vector_curve"), TEXT("FRuntimeVectorCurve"));
				}
				return OutCurveJson.IsValid();
			}
			if (StructProperty->Struct == FRuntimeCurveLinearColor::StaticStruct())
			{
				const FRuntimeCurveLinearColor* RuntimeCurve = static_cast<const FRuntimeCurveLinearColor*>(ValuePtr);
				if (RuntimeCurve->ExternalCurve)
				{
					OutCurveJson = BuildAssetReferenceJson(RuntimeCurve->ExternalCurve, Property->GetCPPType());
				}
				else
				{
					OutCurveJson = BuildLinearColorCurveJson(RuntimeCurve->ColorCurves, TEXT("runtime_linear_color_curve"), TEXT("FRuntimeCurveLinearColor"));
				}
				return OutCurveJson.IsValid();
			}
			if (StructProperty->Struct == FCurveTableRowHandle::StaticStruct())
			{
				OutCurveJson = BuildCurveTableRowHandleJson(*static_cast<const FCurveTableRowHandle*>(ValuePtr));
				return true;
			}
		}

		return false;
	}

	static bool TryApplyAssetReferenceToProperty(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TSharedPtr<FJsonObject> RefObj;
		if (!TryGetObjectField(RootObj, TEXT("asset_reference"), RefObj))
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_missing_required_field"), Path + TEXT(".asset_reference"), TEXT("Asset reference curve JSON requires asset_reference object."));
			return false;
		}
		FString AssetPath;
		if (!UeAgentJsonDiagnostics::ReadStringField(RefObj, TEXT("asset_path"), Path + TEXT(".asset_reference.asset_path"), AssetPath, Issues, true) || AssetPath.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}
		UObject* Asset = LoadObjectByPath(AssetPath);
		if (!Asset)
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_not_found"), Path + TEXT(".asset_reference.asset_path"), FString::Printf(TEXT("Curve asset '%s' could not be loaded."), *AssetPath));
			return false;
		}

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (!Asset->IsA(ObjectProperty->PropertyClass))
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_class_mismatch"), Path + TEXT(".asset_reference.asset_path"), FString::Printf(TEXT("Asset class '%s' is not assignable to property class '%s'."), *Asset->GetClass()->GetPathName(), *ObjectProperty->PropertyClass->GetPathName()));
				return false;
			}
			ObjectProperty->SetObjectPropertyValue(ValuePtr, Asset);
			return true;
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == FRuntimeFloatCurve::StaticStruct())
			{
				UCurveFloat* CurveFloat = Cast<UCurveFloat>(Asset);
				if (!CurveFloat)
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_class_mismatch"), Path + TEXT(".asset_reference.asset_path"), TEXT("FRuntimeFloatCurve external curve requires UCurveFloat."));
					return false;
				}
				FRuntimeFloatCurve* RuntimeCurve = static_cast<FRuntimeFloatCurve*>(ValuePtr);
				RuntimeCurve->ExternalCurve = CurveFloat;
				return true;
			}
			if (StructProperty->Struct == FRuntimeVectorCurve::StaticStruct())
			{
				UCurveVector* CurveVector = Cast<UCurveVector>(Asset);
				if (!CurveVector)
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_class_mismatch"), Path + TEXT(".asset_reference.asset_path"), TEXT("FRuntimeVectorCurve external curve requires UCurveVector."));
					return false;
				}
				FRuntimeVectorCurve* RuntimeCurve = static_cast<FRuntimeVectorCurve*>(ValuePtr);
				RuntimeCurve->ExternalCurve = CurveVector;
				return true;
			}
			if (StructProperty->Struct == FRuntimeCurveLinearColor::StaticStruct())
			{
				UCurveLinearColor* CurveColor = Cast<UCurveLinearColor>(Asset);
				if (!CurveColor)
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_class_mismatch"), Path + TEXT(".asset_reference.asset_path"), TEXT("FRuntimeCurveLinearColor external curve requires UCurveLinearColor."));
					return false;
				}
				FRuntimeCurveLinearColor* RuntimeCurve = static_cast<FRuntimeCurveLinearColor*>(ValuePtr);
				RuntimeCurve->ExternalCurve = CurveColor;
				return true;
			}
		}

		AddCurveIssue(Issues, TEXT("error"), TEXT("curve_unsupported_carrier"), Path, FString::Printf(TEXT("Property '%s' cannot accept curve asset_reference JSON."), Property ? *Property->GetCPPType() : TEXT("")));
		return false;
	}

	static bool TryApplyPropertyCurveJson(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Property || !ValuePtr)
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_unsupported_carrier"), Path, TEXT("Property or value pointer is null."));
			return false;
		}
		if (!RootObj.IsValid())
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_json_parse_failed"), Path, TEXT("Curve JSON object is invalid."));
			return false;
		}
		WarnUnknownCurveFields(RootObj, Path, Issues);

		FString Storage;
		RootObj->TryGetStringField(TEXT("storage"), Storage);
		FString CurveKind;
		RootObj->TryGetStringField(TEXT("curve_kind"), CurveKind);
		if (Storage.Equals(TEXT("asset_reference"), ESearchCase::IgnoreCase) || CurveKind.Equals(TEXT("asset_reference"), ESearchCase::IgnoreCase))
		{
			return TryApplyAssetReferenceToProperty(Property, ValuePtr, RootObj, Path, Issues);
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == FRichCurve::StaticStruct())
			{
				return ApplyChannelJsonToRichCurve(RootObj, TEXT("value"), *static_cast<FRichCurve*>(ValuePtr), Path, Issues);
			}
			if (StructProperty->Struct == FSimpleCurve::StaticStruct())
			{
				FRichCurve RichCurveProxy;
				const bool bOk = ApplyChannelJsonToRichCurve(RootObj, TEXT("value"), RichCurveProxy, Path, Issues);
				if (bOk && !HasError(Issues))
				{
					TArray<FSimpleCurveKey> SimpleKeys;
					for (const FRichCurveKey& RichKey : RichCurveProxy.GetConstRefOfKeys())
					{
						SimpleKeys.Add(FSimpleCurveKey(RichKey.Time, RichKey.Value));
					}
					static_cast<FSimpleCurve*>(ValuePtr)->SetKeys(SimpleKeys);
				}
				return bOk && !HasError(Issues);
			}
			if (StructProperty->Struct == FRuntimeFloatCurve::StaticStruct())
			{
				FRuntimeFloatCurve* RuntimeCurve = static_cast<FRuntimeFloatCurve*>(ValuePtr);
				FRichCurve TempCurve = RuntimeCurve->EditorCurveData;
				if (!ApplyChannelJsonToRichCurve(RootObj, TEXT("value"), TempCurve, Path, Issues) || HasError(Issues))
				{
					return false;
				}
				RuntimeCurve->ExternalCurve = nullptr;
				RuntimeCurve->EditorCurveData = TempCurve;
				return true;
			}
			if (StructProperty->Struct == FRuntimeVectorCurve::StaticStruct())
			{
				FRuntimeVectorCurve* RuntimeCurve = static_cast<FRuntimeVectorCurve*>(ValuePtr);
				FRichCurve TempCurves[3] = {
					RuntimeCurve->VectorCurves[0],
					RuntimeCurve->VectorCurves[1],
					RuntimeCurve->VectorCurves[2]
				};
				bool bOk = true;
				bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("x"), TempCurves[0], Path, Issues);
				bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("y"), TempCurves[1], Path, Issues);
				bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("z"), TempCurves[2], Path, Issues);
				if (!bOk || HasError(Issues))
				{
					return false;
				}
				RuntimeCurve->ExternalCurve = nullptr;
				RuntimeCurve->VectorCurves[0] = TempCurves[0];
				RuntimeCurve->VectorCurves[1] = TempCurves[1];
				RuntimeCurve->VectorCurves[2] = TempCurves[2];
				return true;
			}
			if (StructProperty->Struct == FRuntimeCurveLinearColor::StaticStruct())
			{
				FRuntimeCurveLinearColor* RuntimeCurve = static_cast<FRuntimeCurveLinearColor*>(ValuePtr);
				FRichCurve TempCurves[4] = {
					RuntimeCurve->ColorCurves[0],
					RuntimeCurve->ColorCurves[1],
					RuntimeCurve->ColorCurves[2],
					RuntimeCurve->ColorCurves[3]
				};
				bool bOk = true;
				bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("r"), TempCurves[0], Path, Issues);
				bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("g"), TempCurves[1], Path, Issues);
				bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("b"), TempCurves[2], Path, Issues);
				bOk &= ApplyChannelJsonToRichCurve(RootObj, TEXT("a"), TempCurves[3], Path, Issues);
				if (!bOk || HasError(Issues))
				{
					return false;
				}
				RuntimeCurve->ExternalCurve = nullptr;
				RuntimeCurve->ColorCurves[0] = TempCurves[0];
				RuntimeCurve->ColorCurves[1] = TempCurves[1];
				RuntimeCurve->ColorCurves[2] = TempCurves[2];
				RuntimeCurve->ColorCurves[3] = TempCurves[3];
				return true;
			}
			if (StructProperty->Struct == FCurveTableRowHandle::StaticStruct())
			{
				TSharedPtr<FJsonObject> TableObj;
				if (!TryGetObjectField(RootObj, TEXT("curve_table"), TableObj))
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_missing_required_field"), Path + TEXT(".curve_table"), TEXT("CurveTableRowHandle JSON requires curve_table object."));
					return false;
				}
				FString AssetPath;
				FString RowNameText;
				UeAgentJsonDiagnostics::ReadStringField(TableObj, TEXT("asset_path"), Path + TEXT(".curve_table.asset_path"), AssetPath, Issues, true);
				UeAgentJsonDiagnostics::ReadStringField(TableObj, TEXT("row_name"), Path + TEXT(".curve_table.row_name"), RowNameText, Issues, true);
				UCurveTable* Table = Cast<UCurveTable>(LoadObjectByPath(AssetPath));
				if (!Table)
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_not_found"), Path + TEXT(".curve_table.asset_path"), FString::Printf(TEXT("CurveTable '%s' could not be loaded."), *AssetPath));
					return false;
				}
				if (RowNameText.TrimStartAndEnd().IsEmpty() || !Table->FindCurve(FName(*RowNameText.TrimStartAndEnd()), TEXT("UeAgentCurveJson"), false))
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_table_row_not_found"), Path + TEXT(".curve_table.row_name"), FString::Printf(TEXT("CurveTable row '%s' was not found."), *RowNameText));
					return false;
				}
				FCurveTableRowHandle* Handle = static_cast<FCurveTableRowHandle*>(ValuePtr);
				Handle->CurveTable = Table;
				Handle->RowName = FName(*RowNameText.TrimStartAndEnd());
				return true;
			}
		}

		AddCurveIssue(Issues, TEXT("error"), TEXT("curve_unsupported_carrier"), Path, FString::Printf(TEXT("Property type '%s' is not a supported curve JSON carrier."), *Property->GetCPPType()));
		return false;
	}

	static bool CompareFloatCurves(const FRichCurve& Requested, const FRichCurve& Readback, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		const TArray<FRichCurveKey>& A = Requested.GetConstRefOfKeys();
		const TArray<FRichCurveKey>& B = Readback.GetConstRefOfKeys();
		if (A.Num() != B.Num())
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_readback_mismatch"), Path + TEXT(".keys"), FString::Printf(TEXT("Curve key count mismatch: requested=%d readback=%d."), A.Num(), B.Num()));
			return false;
		}
		bool bOk = true;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (FMath::Abs(A[Index].Time - B[Index].Time) > ValueTolerance || FMath::Abs(A[Index].Value - B[Index].Value) > ValueTolerance)
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_readback_mismatch"), FString::Printf(TEXT("%s.keys[%d]"), *Path, Index), FString::Printf(TEXT("Curve key mismatch: requested=(%.6f,%.6f) readback=(%.6f,%.6f)."), A[Index].Time, A[Index].Value, B[Index].Time, B[Index].Value));
				bOk = false;
			}
		}
		return bOk;
	}

	static UObject* CreateCurveAsset(const FString& AssetPathInput, const FString& CurveKind, FString& OutError)
	{
		const FString AssetPath = NormalizeAssetPath(AssetPathInput);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_curve_asset_path");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package)
		{
			OutError = TEXT("create_curve_package_failed");
			return nullptr;
		}

		UClass* CurveClass = nullptr;
		if (CurveKind.Equals(TEXT("float"), ESearchCase::IgnoreCase) || CurveKind.Equals(TEXT("curve_float"), ESearchCase::IgnoreCase))
		{
			CurveClass = UCurveFloat::StaticClass();
		}
		else if (CurveKind.Equals(TEXT("vector"), ESearchCase::IgnoreCase) || CurveKind.Equals(TEXT("curve_vector"), ESearchCase::IgnoreCase))
		{
			CurveClass = UCurveVector::StaticClass();
		}
		else if (CurveKind.Equals(TEXT("linear_color"), ESearchCase::IgnoreCase) || CurveKind.Equals(TEXT("color"), ESearchCase::IgnoreCase) || CurveKind.Equals(TEXT("curve_linear_color"), ESearchCase::IgnoreCase))
		{
			CurveClass = UCurveLinearColor::StaticClass();
		}
		else if (CurveKind.Equals(TEXT("curve_table"), ESearchCase::IgnoreCase))
		{
			CurveClass = UCurveTable::StaticClass();
		}
		if (!CurveClass)
		{
			OutError = TEXT("unsupported_curve_kind");
			return nullptr;
		}

		UObject* Asset = NewObject<UObject>(Package, CurveClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset)
		{
			OutError = TEXT("create_curve_asset_failed");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		return Asset;
	}
}
