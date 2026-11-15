// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentNativeNumberFormat.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "JsonUtils/JsonPointer.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Curves/SimpleCurve.h"
#include "Engine/CurveTable.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UeAgentCurveJson
{
	const TCHAR* const SchemaName = TEXT("ue_agent_interface.curve.v1");
	static constexpr double ValueTolerance = 1.0e-4;

	static const TArray<FAssetCurveContract>& AssetCurveContracts()
	{
		static const TArray<FAssetCurveContract> Contracts{
			{UCurveFloat::StaticClass(), TEXT("CurveFloat"), TEXT("float"), {TEXT("value")}},
			{UCurveVector::StaticClass(), TEXT("CurveVector"), TEXT("vector"), {TEXT("x"), TEXT("y"), TEXT("z")}},
			{UCurveLinearColor::StaticClass(), TEXT("CurveLinearColor"), TEXT("linear_color"), {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")}},
		};
		return Contracts;
	}
	const FAssetCurveContract* FindAssetCurveContract(const FString& ClassPath)
	{
		return AssetCurveContracts().FindByPredicate([&](const auto& Contract) { return Contract.AssetClass->GetPathName() == ClassPath; });
	}
	const FAssetCurveContract* FindAssetCurveContract(const UObject* Asset)
	{
		return Asset ? AssetCurveContracts().FindByPredicate([&](const auto& Contract) { return Asset->IsA(Contract.AssetClass); }) : nullptr;
	}
	static UE::UAI::Contract::FDiagnostic MakeCurveIssue(
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message,
		const FString& Expected = FString(),
		const FString& Actual = FString())
	{
		auto IssueObj = UeAgentJsonDiagnostics::MakeIssue(Severity, Code, Path, Message);
		IssueObj.PropertyPath.Reset();
		IssueObj.SemanticPointer = Path.IsEmpty() ? TEXT("/") : Path;
		IssueObj.Source.JsonPath = IssueObj.SemanticPointer;
		if (!Expected.IsEmpty())
		{
			IssueObj.ExpectedValue = MakeShared<FJsonValueString>(Expected);
		}
		if (!Actual.IsEmpty())
		{
			IssueObj.ActualValue = MakeShared<FJsonValueString>(Actual);
		}
		return IssueObj;
	}

	void AddCurveIssue(
		TArray<UE::UAI::Contract::FDiagnostic>& Issues,
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message,
		const FString& Expected,
		const FString& Actual)
	{
		Issues.Add(MakeCurveIssue(Severity, Code, Path, Message, Expected, Actual));
	}

	bool HasError(const TArray<UE::UAI::Contract::FDiagnostic>& Issues)
    { return UeAgentJsonDiagnostics::HasErrorIssue(Issues); }

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

	static TSharedPtr<FJsonObject> BuildKeyJson(const FRichCurveKey& Key, const int32 KeyIndex, bool bAuthoring = false)
	{
		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		if (!bAuthoring) KeyObj->SetNumberField(TEXT("key_index"), KeyIndex);
		KeyObj->SetField(TEXT("time"), UeAgentNumericFormatting::MakeNativeValue(Key.Time));
		if (!bAuthoring) KeyObj->SetField(TEXT("time_seconds"), UeAgentNumericFormatting::MakeNativeValue(Key.Time));
		KeyObj->SetField(TEXT("value"), UeAgentNumericFormatting::MakeNativeValue(Key.Value));
		KeyObj->SetStringField(TEXT("interp_mode"), InterpModeToString(Key.InterpMode));
		KeyObj->SetStringField(TEXT("tangent_mode"), TangentModeToString(Key.TangentMode));
		KeyObj->SetStringField(TEXT("tangent_weight_mode"), TangentWeightModeToString(Key.TangentWeightMode));
		KeyObj->SetField(TEXT("arrive_tangent"), UeAgentNumericFormatting::MakeNativeValue(Key.ArriveTangent));
		KeyObj->SetField(TEXT("leave_tangent"), UeAgentNumericFormatting::MakeNativeValue(Key.LeaveTangent));
		KeyObj->SetField(TEXT("arrive_tangent_weight"), UeAgentNumericFormatting::MakeNativeValue(Key.ArriveTangentWeight));
		KeyObj->SetField(TEXT("leave_tangent_weight"), UeAgentNumericFormatting::MakeNativeValue(Key.LeaveTangentWeight));
		return KeyObj;
	}

	static TSharedPtr<FJsonObject> BuildKeyJson(const FSimpleCurveKey& Key, int32 KeyIndex, bool bAuthoring)
	{
		const auto Result = MakeShared<FJsonObject>();
		Result->SetField(TEXT("time"), UeAgentNumericFormatting::MakeNativeValue(Key.Time));
		Result->SetField(TEXT("value"), UeAgentNumericFormatting::MakeNativeValue(Key.Value));
		if (!bAuthoring)
		{
			Result->SetNumberField(TEXT("key_index"), KeyIndex);
			Result->SetField(TEXT("time_seconds"), UeAgentNumericFormatting::MakeNativeValue(Key.Time));
		}
		return Result;
	}

	template <typename CurveType>
	static TSharedPtr<FJsonObject> BuildChannelJson(const CurveType& Curve, const FString& ValueType = TEXT("double"), bool bAuthoring = false)
	{
		TSharedPtr<FJsonObject> ChannelObj = MakeShared<FJsonObject>();
		ChannelObj->SetStringField(TEXT("value_type"), bAuthoring ? TEXT("float") : ValueType);
		TSharedPtr<FJsonObject> DefaultObj = MakeShared<FJsonObject>();
		const bool bHasDefault = Curve.GetDefaultValue() != MAX_flt;
		DefaultObj->SetBoolField(TEXT("enabled"), bHasDefault);
		if (bHasDefault)
		{
			DefaultObj->SetField(TEXT("value"), UeAgentNumericFormatting::MakeNativeValue(Curve.GetDefaultValue()));
		}
		ChannelObj->SetObjectField(TEXT("default_value"), DefaultObj);

		TArray<TSharedPtr<FJsonValue>> KeysJson;
		const auto& Keys = Curve.GetConstRefOfKeys();
		for (int32 KeyIndex = 0; KeyIndex < Keys.Num(); ++KeyIndex)
		{
			KeysJson.Add(MakeShared<FJsonValueObject>(BuildKeyJson(Keys[KeyIndex], KeyIndex, bAuthoring)));
		}
		ChannelObj->SetArrayField(TEXT("keys"), KeysJson);
		if (!bAuthoring) ChannelObj->SetNumberField(TEXT("key_count"), Keys.Num());
		return ChannelObj;
	}

	static TSharedPtr<FJsonObject> MakeCurveRoot(const FString& CurveKind, const FString& Storage, const FString& CarrierCppType, bool bAuthoring = false, const FString& TimeUnit = TEXT("seconds"))
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), SchemaName);
		Root->SetStringField(TEXT("curve_kind"), CurveKind);
		if (!bAuthoring)
		{
			Root->SetStringField(TEXT("storage"), Storage);
			Root->SetStringField(TEXT("carrier_cpp_type"), CarrierCppType);
		}
		TSharedPtr<FJsonObject> TimeDomainObj = MakeShared<FJsonObject>();
		TimeDomainObj->SetStringField(TEXT("unit"), TimeUnit);
		if (!TimeUnit.IsEmpty()) Root->SetObjectField(TEXT("time_domain"), TimeDomainObj);
		return Root;
	}

	TSharedPtr<FJsonObject> BuildFloatCurveJson(const FRichCurve& Curve, const FString& Storage, const FString& CarrierCppType, bool bAuthoring, const FString& TimeUnit)
	{
		TSharedPtr<FJsonObject> Root = MakeCurveRoot(TEXT("float"), Storage, CarrierCppType, bAuthoring, TimeUnit);
		Root->SetStringField(TEXT("pre_infinity_extrap"), ExtrapModeToString(Curve.PreInfinityExtrap));
		Root->SetStringField(TEXT("post_infinity_extrap"), ExtrapModeToString(Curve.PostInfinityExtrap));
		TSharedPtr<FJsonObject> ChannelsObj = MakeShared<FJsonObject>();
		ChannelsObj->SetObjectField(TEXT("value"), BuildChannelJson(Curve, TEXT("double"), bAuthoring));
		Root->SetObjectField(TEXT("channels"), ChannelsObj);
		return Root;
	}

	TSharedPtr<FJsonObject> BuildSimpleCurveJson(const FSimpleCurve& Curve, const FString& Storage, const FString& CarrierCppType, bool bAuthoring, const FString& TimeUnit)
	{
		const auto Root = MakeCurveRoot(TEXT("float"), Storage, CarrierCppType, bAuthoring, TimeUnit);
		Root->SetStringField(TEXT("interp_mode"), InterpModeToString(Curve.GetKeyInterpMode()));
		Root->SetStringField(TEXT("pre_infinity_extrap"), ExtrapModeToString(Curve.PreInfinityExtrap));
		Root->SetStringField(TEXT("post_infinity_extrap"), ExtrapModeToString(Curve.PostInfinityExtrap));
		const auto Channels = MakeShared<FJsonObject>();
		Channels->SetObjectField(TEXT("value"), BuildChannelJson(Curve, TEXT("double"), bAuthoring));
		Root->SetObjectField(TEXT("channels"), Channels);
		return Root;
	}

	TSharedPtr<FJsonObject> BuildCurveTableRowSchema(bool bSimple)
	{
		const auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), bSimple ? TEXT("SimpleCurve") : TEXT("RichCurve"));
		const auto Property = MakeShared<FJsonObject>();
		Property->SetStringField(TEXT("type"), TEXT("curve"));
		Property->SetObjectField(TEXT("value"), bSimple ? BuildSimpleCurveJson(FSimpleCurve(), {}, {}, true, {})
			: BuildFloatCurveJson(FRichCurve(), {}, {}, true, {}));
		const auto Properties = MakeShared<FJsonObject>();
		Properties->SetObjectField(TEXT("Curve"), Property);
		Schema->SetObjectField(TEXT("properties"), Properties);
		const auto Definitions = BuildCurveValueDefinition(bSimple);
		Definitions->SetArrayField(TEXT("table_curve_mode"), {MakeShared<FJsonValueString>(bSimple ? TEXT("simple") : TEXT("rich"))});
		Schema->SetObjectField(TEXT("definitions"), Definitions);
		return Schema;
	}

	TSharedPtr<FJsonObject> BuildCurveValueDefinition(bool bSimple)
	{
		TArray<TSharedPtr<FJsonValue>> Interpolation, Extrapolation;
		for (ERichCurveInterpMode Mode : {RCIM_Linear, RCIM_Constant, RCIM_Cubic, RCIM_None})
			if (!bSimple || Mode != RCIM_Cubic) Interpolation.Add(MakeShared<FJsonValueString>(InterpModeToString(Mode)));
		for (ERichCurveExtrapolation Mode : {RCCE_Cycle, RCCE_CycleWithOffset, RCCE_Oscillate, RCCE_Linear, RCCE_Constant, RCCE_None})
			Extrapolation.Add(MakeShared<FJsonValueString>(ExtrapModeToString(Mode)));
		const auto Definitions = MakeShared<FJsonObject>();
		Definitions->SetArrayField(TEXT("interp_mode"), Interpolation);
		Definitions->SetArrayField(TEXT("infinity_extrap"), Extrapolation);
		Definitions->SetObjectField(TEXT("key_defaults"), bSimple
			? BuildKeyJson(FSimpleCurveKey(0.f, 0.f), 0, true)
			: BuildKeyJson(FRichCurveKey(0.f, 0.f), 0, true));
		if (!bSimple)
		{
			TArray<TSharedPtr<FJsonValue>> Tangents, Weights;
			for (ERichCurveTangentMode Mode : {RCTM_Auto, RCTM_User, RCTM_Break, RCTM_None, RCTM_SmartAuto})
				Tangents.Add(MakeShared<FJsonValueString>(TangentModeToString(Mode)));
			for (ERichCurveTangentWeightMode Mode : {RCTWM_WeightedNone, RCTWM_WeightedArrive, RCTWM_WeightedLeave, RCTWM_WeightedBoth})
				Weights.Add(MakeShared<FJsonValueString>(TangentWeightModeToString(Mode)));
			Definitions->SetArrayField(TEXT("tangent_mode"), Tangents);
			Definitions->SetArrayField(TEXT("tangent_weight_mode"), Weights);
		}
		return Definitions;
	}

	TSharedPtr<FJsonObject> BuildEmptyCurveTableRowSchema()
	{
		const auto Schema = BuildCurveTableRowSchema(false);
		Schema->SetStringField(TEXT("type"), TEXT("UnspecifiedCurve"));
		const auto Definitions = Schema->GetObjectField(TEXT("definitions"));
		Definitions->SetArrayField(TEXT("table_curve_mode"), {
			MakeShared<FJsonValueString>(TEXT("empty")), MakeShared<FJsonValueString>(TEXT("simple")), MakeShared<FJsonValueString>(TEXT("rich"))});
		const auto Templates = MakeShared<FJsonObject>();
		Templates->SetObjectField(TEXT("simple"), BuildCurveTableRowSchema(true));
		Templates->SetObjectField(TEXT("rich"), BuildCurveTableRowSchema(false));
		Definitions->SetObjectField(TEXT("mode_templates"), Templates);
		return Schema;
	}

	static TSharedPtr<FJsonObject> BuildMultiChannelCurveJson(const FRichCurve* Curves,
		const TArray<FString>& ChannelNames, const FString& CurveKind,
		const FString& Storage, const FString& CarrierCppType, bool bAuthoring = false)
	{
		TSharedPtr<FJsonObject> Root = MakeCurveRoot(CurveKind, Storage, CarrierCppType, bAuthoring, bAuthoring ? FString() : TEXT("seconds"));
		TSharedPtr<FJsonObject> ChannelsObj = MakeShared<FJsonObject>();
		int32 Index = 0;
		for (const FString& Name : ChannelNames)
		{
			const FRichCurve& Curve = Curves[Index++];
			const auto Channel = BuildChannelJson(Curve, TEXT("double"), bAuthoring);
			Channel->SetStringField(TEXT("pre_infinity_extrap"), ExtrapModeToString(Curve.PreInfinityExtrap));
			Channel->SetStringField(TEXT("post_infinity_extrap"), ExtrapModeToString(Curve.PostInfinityExtrap));
			ChannelsObj->SetObjectField(Name, Channel);
		}
		Root->SetObjectField(TEXT("channels"), ChannelsObj);
		return Root;
	}

	TSharedPtr<FJsonObject> BuildCurveAssetValueDefinition(const FAssetCurveContract& Contract)
	{
		const auto Definition = MakeShared<FJsonObject>();
		Definition->SetStringField(TEXT("type"), Contract.ValueType);
		const auto ValueContract = BuildCurveValueDefinition(false);
		const auto Templates = MakeShared<FJsonObject>();
		TArray<FRichCurve> Channels;
		Channels.SetNum(Contract.Channels.Num());
		Templates->SetObjectField(TEXT("embedded"), Contract.CurveKind == TEXT("float")
			? BuildFloatCurveJson(Channels[0], {}, {}, true, {})
			: BuildMultiChannelCurveJson(Channels.GetData(), Contract.Channels, Contract.CurveKind, {}, {}, true));
		ValueContract->SetObjectField(TEXT("value_templates"), Templates);
		Definition->SetObjectField(TEXT("contract"), ValueContract);
		return Definition;
	}

	static TSharedPtr<FJsonObject> BuildAssetReferenceJson(const UObject* Asset, const FString& PropertyCppType = FString(), bool bAuthoring = false)
	{
		TSharedPtr<FJsonObject> Root = MakeCurveRoot(TEXT("asset_reference"), TEXT("asset_reference"), PropertyCppType.IsEmpty() ? TEXT("UObject") : PropertyCppType,
			bAuthoring, bAuthoring ? FString() : TEXT("seconds"));
		TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
		RefObj->SetStringField(TEXT("asset_path"), Asset ? NormalizeAssetPath(Asset->GetOutermost()->GetName()) : TEXT(""));
		if (!bAuthoring)
		{
			RefObj->SetStringField(TEXT("object_path"), Asset ? Asset->GetPathName() : TEXT(""));
			RefObj->SetStringField(TEXT("asset_class"), Asset && Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
			RefObj->SetBoolField(TEXT("required"), Asset != nullptr);
		}
		Root->SetObjectField(TEXT("asset_reference"), RefObj);
		return Root;
	}

	enum class ERuntimeCurveKind { Float, Vector, Color };

	static TSharedPtr<FJsonObject> BuildRuntimeCurveValueDefinition(ERuntimeCurveKind Kind)
	{
		const auto Definition = MakeShared<FJsonObject>();
		const auto Contract = BuildCurveValueDefinition(false);
		const auto Templates = MakeShared<FJsonObject>();
		FRichCurve EmptyChannels[4];
		UClass* ReferenceClass = nullptr;
		switch (Kind)
		{
		case ERuntimeCurveKind::Float:
			Definition->SetStringField(TEXT("type"), TEXT("RuntimeFloatCurve"));
			Templates->SetObjectField(TEXT("embedded"), BuildFloatCurveJson(EmptyChannels[0], {}, {}, true, {}));
			ReferenceClass = UCurveFloat::StaticClass();
			break;
		case ERuntimeCurveKind::Vector:
			Definition->SetStringField(TEXT("type"), TEXT("RuntimeVectorCurve"));
			Templates->SetObjectField(TEXT("embedded"), BuildMultiChannelCurveJson(EmptyChannels, {TEXT("x"), TEXT("y"), TEXT("z")}, TEXT("vector"), {}, {}, true));
			ReferenceClass = UCurveVector::StaticClass();
			break;
		case ERuntimeCurveKind::Color:
			Definition->SetStringField(TEXT("type"), TEXT("RuntimeColorCurve"));
			Templates->SetObjectField(TEXT("embedded"), BuildMultiChannelCurveJson(EmptyChannels, {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")}, TEXT("linear_color"), {}, {}, true));
			ReferenceClass = UCurveLinearColor::StaticClass();
			break;
		}
		Templates->SetObjectField(TEXT("asset_reference"), BuildAssetReferenceJson(nullptr, {}, true));
		Contract->SetObjectField(TEXT("value_templates"), Templates);
		const auto Reference = MakeShared<FJsonObject>();
		Reference->SetStringField(TEXT("asset_class"), ReferenceClass->GetPathName());
		const auto Path = MakeShared<FJsonObject>();
		Path->SetStringField(TEXT("type"), TEXT("string"));
		Path->SetBoolField(TEXT("non_empty"), true);
		Reference->SetObjectField(TEXT("asset_path"), Path);
		Contract->SetObjectField(TEXT("asset_reference"), Reference);
		Definition->SetObjectField(TEXT("contract"), Contract);
		return Definition;
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

	static bool ValidateKeyFields(const TSharedPtr<FJsonObject>& KeyObj,
		std::initializer_list<UeAgentAuthoringJsonFields::FFieldType> Fields,
		const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		UE::UAI::Contract::FDiagnostic Error;
		if (UeAgentAuthoringJsonFields::ValidateDeclaredFields(*KeyObj, Fields, Error)) return true;
		UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Path);
		Issues.Add(MoveTemp(Error));
		return false;
	}

	static void WarnUnknownChannelFields(const TSharedPtr<FJsonObject>& ChannelObj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			ChannelObj,
			Path,
			{
				TEXT("value_type"),
				TEXT("default_value"),
				TEXT("keys"),
				TEXT("key_count"),
				TEXT("pre_infinity_extrap"),
				TEXT("post_infinity_extrap")
			},
			Issues);

		TSharedPtr<FJsonObject> DefaultObj;
		if (TryGetObjectField(ChannelObj, TEXT("default_value"), DefaultObj))
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(
				DefaultObj,
				Path + TEXT("/default_value"),
				{ TEXT("enabled"), TEXT("value") },
				Issues);
		}

	}

	void WarnUnknownCurveFields(const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			RootObj,
			Path.IsEmpty() ? TEXT("/") : Path,
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
				TEXT("interp_mode"),
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
			UeAgentJsonDiagnostics::WarnUnknownFields(TimeDomainObj, Path + TEXT("/time_domain"), { TEXT("unit") }, Issues);
		}

		TSharedPtr<FJsonObject> ChannelsObj;
		if (TryGetObjectField(RootObj, TEXT("channels"), ChannelsObj))
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(
				ChannelsObj,
				Path + TEXT("/channels"),
				{ TEXT("value"), TEXT("x"), TEXT("y"), TEXT("z"), TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") },
				Issues);
			const TArray<FString> ChannelNames = { TEXT("value"), TEXT("x"), TEXT("y"), TEXT("z"), TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
			for (const FString& ChannelName : ChannelNames)
			{
				TSharedPtr<FJsonObject> ChannelObj;
				if (TryGetObjectField(ChannelsObj, ChannelName, ChannelObj))
				{
					WarnUnknownChannelFields(ChannelObj, Path + TEXT("/channels/") + UE::Json::FJsonPointer::EscapePart(ChannelName), Issues);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (RootObj.IsValid() && RootObj->TryGetArrayField(TEXT("rows"), Rows) && Rows)
		{
			for (int32 RowIndex = 0; RowIndex < Rows->Num(); ++RowIndex)
			{
				const FString RowPath = FString::Printf(TEXT("%s/rows/%d"), *Path, RowIndex);
				TSharedPtr<FJsonObject> RowObj;
				if (UeAgentJsonDiagnostics::ReadObjectFromValue((*Rows)[RowIndex], RowPath, RowObj, Issues))
				{
					UeAgentJsonDiagnostics::WarnUnknownFields(RowObj, RowPath, { TEXT("row_name"), TEXT("row_type"), TEXT("curve") }, Issues);
					TSharedPtr<FJsonObject> RowCurveObj;
					if (TryGetObjectField(RowObj, TEXT("curve"), RowCurveObj))
					{
						WarnUnknownCurveFields(RowCurveObj, RowPath + TEXT("/curve"), Issues);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> RefObj;
		if (TryGetObjectField(RootObj, TEXT("asset_reference"), RefObj))
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(RefObj, Path + TEXT("/asset_reference"), { TEXT("asset_path"), TEXT("object_path"), TEXT("asset_class"), TEXT("required") }, Issues);
		}

		TSharedPtr<FJsonObject> TableObj;
		if (TryGetObjectField(RootObj, TEXT("curve_table"), TableObj))
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(TableObj, Path + TEXT("/curve_table"), { TEXT("asset_path"), TEXT("object_path"), TEXT("row_name"), TEXT("row_type") }, Issues);
		}
	}

	static bool ReadChannelObject(const TSharedPtr<FJsonObject>& RootObj, const FString& ChannelName, const FString& Path, TSharedPtr<FJsonObject>& OutChannelObj, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		TSharedPtr<FJsonObject> ChannelsObj;
		if (TryGetObjectField(RootObj, TEXT("channels"), ChannelsObj))
		{
			if (TryGetObjectField(ChannelsObj, ChannelName, OutChannelObj))
			{
				return true;
			}
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_channel_missing"), Path + TEXT("/channels/") + UE::Json::FJsonPointer::EscapePart(ChannelName), FString::Printf(TEXT("Curve channel '%s' is missing."), *ChannelName));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* LegacyKeys = nullptr;
		if (ChannelName.Equals(TEXT("value"), ESearchCase::IgnoreCase) && RootObj.IsValid() && RootObj->TryGetArrayField(TEXT("keys"), LegacyKeys) && LegacyKeys)
		{
			OutChannelObj = MakeShared<FJsonObject>();
			OutChannelObj->SetArrayField(TEXT("keys"), *LegacyKeys);
			return true;
		}

		AddCurveIssue(Issues, TEXT("error"), TEXT("curve_channel_missing"), Path + TEXT("/channels"), TEXT("Curve JSON must contain a channels object."));
		return false;
	}

	static bool ReadKeyCoordinates(const TSharedPtr<FJsonObject>& KeyObj, const FString& Path,
		float& OutTime, float& OutValue, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		const bool bTime = KeyObj->HasField(TEXT("time"));
		const bool bSeconds = KeyObj->HasField(TEXT("time_seconds"));
		if (!bTime && !bSeconds)
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_key_time_missing"), Path + TEXT("/time"), TEXT("Curve key must contain numeric time or time_seconds."), TEXT("number"), TEXT("missing_or_wrong_type"));
			return false;
		}
		float Time = 0.f, Seconds = 0.f;
		UE::UAI::Contract::FDiagnostic Error;
		for (const TCHAR* Field : {TEXT("time"), TEXT("time_seconds")})
		{
			if (!UeAgentAuthoringJsonFields::ReadFloat32Field(*KeyObj, Field,
				FCString::Strcmp(Field, TEXT("time")) == 0 ? Time : Seconds, Error))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Path);
				Issues.Add(MoveTemp(Error));
				return false;
			}
		}
		if (bTime && bSeconds && Time != Seconds)
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_key_time_conflict"), Path + TEXT("/time_seconds"),
				TEXT("time and time_seconds describe the same native coordinate and must agree."));
			return false;
		}
		double Value = 0.0;
		if (!KeyObj.IsValid() || !KeyObj->HasTypedField<EJson::Number>(TEXT("value")) || !KeyObj->TryGetNumberField(TEXT("value"), Value))
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_key_value_type_mismatch"), Path + TEXT("/value"), TEXT("Curve key must contain numeric value."), TEXT("number"), TEXT("missing_or_wrong_type"));
			return false;
		}

		OutTime = bTime ? Time : Seconds;
		OutValue = static_cast<float>(Value);
		if (!FMath::IsFinite(OutTime) || !FMath::IsFinite(OutValue))
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_key_value_not_finite"), Path, TEXT("Curve coordinates must be finite native floats."));
			return false;
		}
		return true;
	}

	static bool ReadKeyObject(const TSharedPtr<FJsonObject>& KeyObj, const FString& Path,
		FSimpleCurveKey& OutKey, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!ValidateKeyFields(KeyObj, {{TEXT("time"), EJson::Number}, {TEXT("time_seconds"), EJson::Number},
			{TEXT("value"), EJson::Number}, {TEXT("key_index"), EJson::Number}}, Path, Issues)) return false;
		return ReadKeyCoordinates(KeyObj, Path, OutKey.Time, OutKey.Value, Issues);
	}

	template <typename EnumType, typename Parser>
	static bool ReadOptionalKeyEnum(const TSharedPtr<FJsonObject>& Object, const FString& Field,
		const FString& Path, TEnumAsByte<EnumType>& Value, Parser Parse, const FString& InvalidCode,
		TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!Object->HasField(Field)) return true;
		FString Text;
		EnumType Parsed;
		if (!Object->HasTypedField<EJson::String>(Field) || !Object->TryGetStringField(Field, Text))
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_field_type_mismatch"), Path + TEXT("/") + UE::Json::FJsonPointer::EscapePart(Field), TEXT("Curve key enum must be a string."));
			return false;
		}
		if (!Parse(Text, Parsed))
		{
			AddCurveIssue(Issues, TEXT("error"), InvalidCode, Path + TEXT("/") + UE::Json::FJsonPointer::EscapePart(Field),
				FString::Printf(TEXT("Curve key field '%s' has an unsupported value."), *Field), TEXT("declared ") + Field + TEXT(" value"), Text);
			return false;
		}
		Value = Parsed;
		return true;
	}

	static bool ReadOptionalKeyFloat(const TSharedPtr<FJsonObject>& Object, const FString& Field,
		const FString& Path, float& Value, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!Object->HasField(Field)) return true;
		double Number = 0;
		if (!Object->HasTypedField<EJson::Number>(Field) || !Object->TryGetNumberField(Field, Number)
			|| !FMath::IsFinite(static_cast<float>(Number)))
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_field_type_mismatch"), Path + TEXT("/") + UE::Json::FJsonPointer::EscapePart(Field), TEXT("Curve key tangent must be a finite native float."));
			return false;
		}
		Value = static_cast<float>(Number);
		return true;
	}

	static bool ReadKeyObject(const TSharedPtr<FJsonObject>& KeyObj, const FString& Path, FRichCurveKey& OutKey, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!ValidateKeyFields(KeyObj, {{TEXT("time"), EJson::Number}, {TEXT("time_seconds"), EJson::Number},
			{TEXT("value"), EJson::Number}, {TEXT("key_index"), EJson::Number},
			{TEXT("interp_mode"), EJson::String}, {TEXT("tangent_mode"), EJson::String},
			{TEXT("tangent_weight_mode"), EJson::String}, {TEXT("arrive_tangent"), EJson::Number},
			{TEXT("leave_tangent"), EJson::Number}, {TEXT("arrive_tangent_weight"), EJson::Number},
			{TEXT("leave_tangent_weight"), EJson::Number}}, Path, Issues)) return false;
		float Time = 0, Value = 0;
		if (!ReadKeyCoordinates(KeyObj, Path, Time, Value, Issues)) return false;
		OutKey = FRichCurveKey(Time, Value);
		return ReadOptionalKeyEnum(KeyObj, TEXT("interp_mode"), Path, OutKey.InterpMode, &ParseInterpMode, TEXT("curve_interp_mode_invalid"), Issues)
			&& ReadOptionalKeyEnum(KeyObj, TEXT("tangent_mode"), Path, OutKey.TangentMode, &ParseTangentMode, TEXT("curve_tangent_mode_invalid"), Issues)
			&& ReadOptionalKeyEnum(KeyObj, TEXT("tangent_weight_mode"), Path, OutKey.TangentWeightMode, &ParseTangentWeightMode, TEXT("curve_tangent_weight_mode_invalid"), Issues)
			&& ReadOptionalKeyFloat(KeyObj, TEXT("arrive_tangent"), Path, OutKey.ArriveTangent, Issues)
			&& ReadOptionalKeyFloat(KeyObj, TEXT("leave_tangent"), Path, OutKey.LeaveTangent, Issues)
			&& ReadOptionalKeyFloat(KeyObj, TEXT("arrive_tangent_weight"), Path, OutKey.ArriveTangentWeight, Issues)
			&& ReadOptionalKeyFloat(KeyObj, TEXT("leave_tangent_weight"), Path, OutKey.LeaveTangentWeight, Issues);
	}
	template <typename CurveType, typename KeyType>
	static bool ApplyChannelJson(const TSharedPtr<FJsonObject>& RootObj, const FString& ChannelName, CurveType& OutCurve, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		TSharedPtr<FJsonObject> ChannelObj;
		if (!ReadChannelObject(RootObj, ChannelName, Path, ChannelObj, Issues))
		{
			return false;
		}

		TArray<KeyType> NewKeys;
		const FString KeysPath = RootObj->HasTypedField<EJson::Object>(TEXT("channels"))
			? Path + TEXT("/channels/") + UE::Json::FJsonPointer::EscapePart(ChannelName) + TEXT("/keys")
			: Path + TEXT("/keys");
		const TArray<TSharedPtr<FJsonValue>>* KeyValues = nullptr;
		if (!ChannelObj->TryGetArrayField(TEXT("keys"), KeyValues) || !KeyValues)
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_missing_required_field"), KeysPath, TEXT("Curve channel must contain keys array."), TEXT("array"), TEXT("missing_or_wrong_type"));
			return false;
		}

		float PreviousTime = -MAX_flt;
		for (int32 KeyIndex = 0; KeyIndex < KeyValues->Num(); ++KeyIndex)
		{
			const FString KeyPath = FString::Printf(TEXT("%s/%d"), *KeysPath, KeyIndex);
			TSharedPtr<FJsonObject> KeyObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*KeyValues)[KeyIndex], KeyPath, KeyObj, Issues))
			{
				continue;
			}

			KeyType NewKey;
			if (!ReadKeyObject(KeyObj, KeyPath, NewKey, Issues))
			{
				continue;
			}

			if (NewKey.Time < PreviousTime)
			{
				AddCurveIssue(Issues, TEXT("warning"), TEXT("curve_key_time_not_monotonic"),
					KeyPath + (KeyObj->HasField(TEXT("time")) ? TEXT("/time") : TEXT("/time_seconds")),
					TEXT("Curve keys were not monotonic; keys will be sorted before apply."));
			}
			PreviousTime = NewKey.Time;
			NewKeys.Add(NewKey);
		}

		if (HasError(Issues))
		{
			return false;
		}

		// SetKeys preserves distinct near-neighbour keys, including equal-time
		// keys. Keep their authored order instead of imposing an import tolerance.
		NewKeys.StableSort([](const KeyType& A, const KeyType& B) { return A.Time < B.Time; });
		CurveType NewCurve = OutCurve;
		NewCurve.SetKeys(NewKeys);

		TSharedPtr<FJsonObject> DefaultObj;
		if (ChannelObj->HasField(TEXT("default_value")) && !TryGetObjectField(ChannelObj, TEXT("default_value"), DefaultObj))
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_field_type_mismatch"), Path + TEXT("/channels/") + UE::Json::FJsonPointer::EscapePart(ChannelName) + TEXT("/default_value"), TEXT("Curve default value must be an object."));
			return false;
		}
		if (TryGetObjectField(ChannelObj, TEXT("default_value"), DefaultObj))
		{
			bool bDefaultEnabled = false;
			if (!DefaultObj->HasTypedField<EJson::Boolean>(TEXT("enabled")) || !DefaultObj->TryGetBoolField(TEXT("enabled"), bDefaultEnabled))
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_field_type_mismatch"), Path + TEXT("/channels/") + UE::Json::FJsonPointer::EscapePart(ChannelName) + TEXT("/default_value/enabled"), TEXT("Curve default enabled must be boolean."));
				return false;
			}
			if (bDefaultEnabled)
			{
				double DefaultValue = 0.0;
				if (DefaultObj->HasTypedField<EJson::Number>(TEXT("value"))
					&& DefaultObj->TryGetNumberField(TEXT("value"), DefaultValue)
					&& FMath::IsFinite(static_cast<float>(DefaultValue)))
				{
					if (static_cast<float>(DefaultValue) == MAX_flt)
					{
						AddCurveIssue(Issues, TEXT("error"), TEXT("curve_default_value_unrepresentable"), Path + TEXT("/channels/") + UE::Json::FJsonPointer::EscapePart(ChannelName) + TEXT("/default_value/value"), TEXT("Enabled default cannot equal the native unset sentinel."));
						return false;
					}
					NewCurve.SetDefaultValue(static_cast<float>(DefaultValue));
				}
				else
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_field_type_mismatch"), Path + TEXT("/channels/") + UE::Json::FJsonPointer::EscapePart(ChannelName) + TEXT("/default_value/value"), TEXT("Enabled curve default value must be numeric."), TEXT("number"), TEXT("missing_or_wrong_type"));
				}
			}
			else
			{
				NewCurve.ClearDefaultValue();
			}
		}

		for (const FString Field : {FString(TEXT("pre_infinity_extrap")), FString(TEXT("post_infinity_extrap"))})
		{
			const bool bRoot = RootObj->HasField(Field), bChannel = ChannelObj->HasField(Field);
			if (!bRoot && !bChannel) continue;
			FString RootText, ChannelText;
			ERichCurveExtrapolation RootMode = RCCE_Constant, ChannelMode = RCCE_Constant;
			auto ReadExtrapolation = [&](const TSharedPtr<FJsonObject>& Object, const FString& FieldPath,
				FString& Text, ERichCurveExtrapolation& Mode)
			{
				if (!Object->TryGetStringField(Field, Text))
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_field_type_mismatch"), FieldPath,
						TEXT("Curve extrapolation must be a string."), TEXT("string"), UeAgentJsonDiagnostics::JsonValueTypeToString(Object->TryGetField(Field)));
					return false;
				}
				if (!ParseExtrapMode(Text, Mode))
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_interp_mode_invalid"), FieldPath,
						TEXT("Invalid curve extrapolation."), TEXT("curve extrapolation mode"), Text);
					return false;
				}
				return true;
			};
			if ((bRoot && !ReadExtrapolation(RootObj, Path + TEXT("/") + UE::Json::FJsonPointer::EscapePart(Field), RootText, RootMode))
				|| (bChannel && !ReadExtrapolation(ChannelObj, Path + TEXT("/channels/") + UE::Json::FJsonPointer::EscapePart(ChannelName)
					+ TEXT("/") + UE::Json::FJsonPointer::EscapePart(Field), ChannelText, ChannelMode))) return false;
			if (bRoot && bChannel && RootMode != ChannelMode)
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_extrapolation_conflict"), Path + TEXT("/channels/") + UE::Json::FJsonPointer::EscapePart(ChannelName) + TEXT("/") + UE::Json::FJsonPointer::EscapePart(Field), TEXT("Root and channel extrapolation disagree."));
				return false;
			}
			const ERichCurveExtrapolation Mode = bChannel ? ChannelMode : RootMode;
			if (Field == TEXT("pre_infinity_extrap")) NewCurve.PreInfinityExtrap = Mode;
			else NewCurve.PostInfinityExtrap = Mode;
		}

		if (HasError(Issues))
		{
			return false;
		}

		OutCurve = NewCurve;
		return true;
	}

	bool ApplyChannelJsonToRichCurve(const TSharedPtr<FJsonObject>& RootObj, const FString& ChannelName,
		FRichCurve& OutCurve, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!ApplyChannelJson<FRichCurve, FRichCurveKey>(RootObj, ChannelName, OutCurve, Path, Issues)) return false;
		OutCurve.AutoSetTangents();
		return true;
	}

	bool ApplyChannelJsonToSimpleCurve(const TSharedPtr<FJsonObject>& RootObj, const FString& ChannelName,
		FSimpleCurve& OutCurve, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		FSimpleCurve Proposed = OutCurve;
		if (RootObj.IsValid() && RootObj->HasField(TEXT("interp_mode")))
		{
			FString Text;
			ERichCurveInterpMode Mode;
			if (!RootObj->HasTypedField<EJson::String>(TEXT("interp_mode")) || !RootObj->TryGetStringField(TEXT("interp_mode"), Text) || !ParseInterpMode(Text, Mode) || Mode == RCIM_Cubic)
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_simple_interp_mode_invalid"), Path + TEXT("/interp_mode"), TEXT("Simple curves require a non-cubic interpolation mode."));
				return false;
			}
			Proposed.SetKeyInterpMode(Mode);
		}
		if (!ApplyChannelJson<FSimpleCurve, FSimpleCurveKey>(RootObj, ChannelName, Proposed, Path, Issues)) return false;
		OutCurve = MoveTemp(Proposed);
		return true;
	}
	static bool PrepareAssetCurveChannels(const FAssetCurveContract& Contract,
		const TSharedPtr<FJsonObject>& Root, TArray<FRichCurve>& Curves,
		const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!Root.IsValid()) { AddCurveIssue(Issues, TEXT("error"), TEXT("curve_json_parse_failed"), Path, TEXT("Curve JSON object is invalid.")); return false; }
		FString Kind;
		if (Root->HasField(TEXT("curve_kind")) && (!Root->TryGetStringField(TEXT("curve_kind"), Kind) || Kind != Contract.CurveKind))
		{ AddCurveIssue(Issues, TEXT("error"), TEXT("curve_kind_mismatch"), Path + TEXT("/curve_kind"), TEXT("Curve kind must match the native asset."), Contract.CurveKind, Kind); return false; }
		if (Curves.Num() != Contract.Channels.Num()) return false;
		bool bOk = true;
		for (int32 Index = 0; Index < Curves.Num(); ++Index)
			bOk &= ApplyChannelJsonToRichCurve(Root, Contract.Channels[Index], Curves[Index], Path, Issues);
		return bOk && !HasError(Issues);
	}
	bool ValidateCurveAssetJson(const FAssetCurveContract& Contract, const TSharedPtr<FJsonObject>& Value,
		const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		TArray<FRichCurve> Curves; Curves.SetNum(Contract.Channels.Num());
		return PrepareAssetCurveChannels(Contract, Value, Curves, Path, Issues);
	}
	TSharedPtr<FJsonObject> BuildCurveAssetJson(UObject* Asset, const bool bAuthoring)
	{
		TSharedPtr<FJsonObject> Root;
		if (const auto* Contract = FindAssetCurveContract(Asset))
		{
			const auto Native = CastChecked<UCurveBase>(Asset)->GetCurves();
			if (Native.Num() != Contract->Channels.Num()) return nullptr;
			TArray<FRichCurve> Curves;
			// The registered native asset classes expose FRichCurve storage through FRealCurve pointers.
			for (const auto& Entry : Native) { if (!Entry.CurveToEdit) return nullptr; Curves.Add(*static_cast<const FRichCurve*>(Entry.CurveToEdit)); }
			const FString Carrier = TEXT("U") + Contract->ValueType;
			Root = Curves.Num() == 1
				? BuildFloatCurveJson(Curves[0], TEXT("curve_asset"), Carrier, bAuthoring, bAuthoring ? FString() : TEXT("seconds"))
				: BuildMultiChannelCurveJson(Curves.GetData(), Contract->Channels, Contract->CurveKind, TEXT("curve_asset"), Carrier, bAuthoring);
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
					TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
					RowObj->SetStringField(TEXT("row_name"), Pair.Key.ToString());
					RowObj->SetStringField(TEXT("row_type"), TEXT("SimpleCurve"));
					RowObj->SetObjectField(TEXT("curve"), BuildSimpleCurveJson(*Pair.Value, TEXT("curve_table_row"), TEXT("FSimpleCurve"), false, {}));
					RowsJson.Add(MakeShared<FJsonValueObject>(RowObj));
				}
			}
			else
			{
				Root->SetStringField(TEXT("table_curve_mode"), CurveTable->GetCurveTableMode() == ECurveTableMode::Empty ? TEXT("empty") : TEXT("rich"));
				for (const TPair<FName, FRichCurve*>& Pair : CurveTable->GetRichCurveRowMap())
				{
					if (!Pair.Value)
					{
						continue;
					}
					TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
					RowObj->SetStringField(TEXT("row_name"), Pair.Key.ToString());
					RowObj->SetStringField(TEXT("row_type"), TEXT("RichCurve"));
					RowObj->SetObjectField(TEXT("curve"), BuildFloatCurveJson(*Pair.Value, TEXT("curve_table_row"), TEXT("FRichCurve"), false, {}));
					RowsJson.Add(MakeShared<FJsonValueObject>(RowObj));
				}
			}
			Root->SetArrayField(TEXT("rows"), RowsJson);
			Root->SetNumberField(TEXT("row_count"), RowsJson.Num());
		}

		if (Root.IsValid() && Asset && !bAuthoring)
		{
			Root->SetStringField(TEXT("asset_path"), NormalizeAssetPath(Asset->GetOutermost()->GetName()));
			Root->SetStringField(TEXT("object_path"), Asset->GetPathName());
			Root->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		}
		return Root;
	}

	bool ApplyCurveAssetJson(UObject* Asset, const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
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

		if (const auto* Contract = FindAssetCurveContract(Asset))
		{
			const auto Native = CastChecked<UCurveBase>(Asset)->GetCurves();
			if (Native.Num() != Contract->Channels.Num())
			{
				auto Issue = MakeCurveIssue(TEXT("error"), TEXT("curve_channel_count_mismatch"), Path,
					TEXT("The native curve channel count differs from the asset contract."));
				Issue.TargetPath = Asset->GetPathName();
				Issue.ExpectedValue = MakeShared<FJsonValueNumber>(Contract->Channels.Num());
				Issue.ActualValue = MakeShared<FJsonValueNumber>(Native.Num());
				Issues.Add(MoveTemp(Issue));
				return false;
			}
			TArray<FRichCurve> Proposed;
			for (int32 Index = 0; Index < Native.Num(); ++Index)
			{
				if (!Native[Index].CurveToEdit)
				{
					auto Issue = MakeCurveIssue(TEXT("error"), TEXT("curve_channel_unavailable"), Path,
						TEXT("The native curve channel is unavailable for editing."));
					Issue.TargetPath = Asset->GetPathName();
					Issue.CandidateName = Contract->Channels[Index];
					Issue.ActualValuePresent = false;
					Issues.Add(MoveTemp(Issue));
					return false;
				}
				Proposed.Add(*static_cast<const FRichCurve*>(Native[Index].CurveToEdit));
			}
			if (!PrepareAssetCurveChannels(*Contract, RootObj, Proposed, Path, Issues)) return false;
			for (int32 Index = 0; Index < Native.Num(); ++Index) *static_cast<FRichCurve*>(Native[Index].CurveToEdit) = MoveTemp(Proposed[Index]);
			return true;
		}
		if (UCurveTable* CurveTable = Cast<UCurveTable>(Asset))
		{
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!RootObj->TryGetArrayField(TEXT("rows"), Rows) || !Rows)
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_missing_required_field"), Path + TEXT("/rows"), TEXT("Curve table JSON requires rows array."));
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
				FSimpleCurve Curve;
			};
			TArray<FPendingRichCurveTableRow> PendingRichRows;
			TArray<FPendingSimpleCurveTableRow> PendingSimpleRows;
			TSet<FName> RowNames;
			for (int32 RowIndex = 0; RowIndex < Rows->Num(); ++RowIndex)
			{
				const FString RowPath = FString::Printf(TEXT("%s/rows/%d"), *Path, RowIndex);
				TSharedPtr<FJsonObject> RowObj;
				if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Rows)[RowIndex], RowPath, RowObj, Issues))
				{
					continue;
				}
				FString RowNameText;
				if (!UeAgentJsonDiagnostics::ReadStringField(RowObj, TEXT("row_name"), RowPath + TEXT("/row_name"), RowNameText, Issues, true) || RowNameText.TrimStartAndEnd().IsEmpty())
				{
					continue;
				}
				TSharedPtr<FJsonObject> RowCurveObj;
				if (!TryGetObjectField(RowObj, TEXT("curve"), RowCurveObj))
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_missing_required_field"), RowPath + TEXT("/curve"), TEXT("Curve table row requires curve object."));
					continue;
				}
				FString RowType;
				RowObj->TryGetStringField(TEXT("row_type"), RowType);
				FString TableMode;
				RootObj->TryGetStringField(TEXT("table_curve_mode"), TableMode);
				const FName RowName(*RowNameText.TrimStartAndEnd());
				if (RowName.IsNone() || RowNames.Contains(RowName)
					|| (!RowType.IsEmpty() && !RowType.Equals(TEXT("SimpleCurve"), ESearchCase::IgnoreCase) && !RowType.Equals(TEXT("RichCurve"), ESearchCase::IgnoreCase))
					|| (!TableMode.IsEmpty() && !TableMode.Equals(TEXT("simple"), ESearchCase::IgnoreCase) && !TableMode.Equals(TEXT("rich"), ESearchCase::IgnoreCase))
					|| (!RowType.IsEmpty() && !TableMode.IsEmpty()
						&& RowType.Equals(TEXT("SimpleCurve"), ESearchCase::IgnoreCase) != TableMode.Equals(TEXT("simple"), ESearchCase::IgnoreCase)))
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_table_row_mode_invalid"), RowPath, TEXT("Rows require unique names and a consistent table mode."));
					continue;
				}
				RowNames.Add(RowName);
				const bool bUseSimpleCurve = RowType.Equals(TEXT("SimpleCurve"), ESearchCase::IgnoreCase) || TableMode.Equals(TEXT("simple"), ESearchCase::IgnoreCase);
				if (bUseSimpleCurve)
				{
					FSimpleCurve SimpleCurve;
					if (ApplyChannelJsonToSimpleCurve(RowCurveObj, TEXT("value"), SimpleCurve, RowPath + TEXT("/curve"), Issues))
					{
						PendingSimpleRows.Add({ RowName, SimpleCurve });
					}
				}
				else
				{
					FRichCurve RichCurve;
					if (ApplyChannelJsonToRichCurve(RowCurveObj, TEXT("value"), RichCurve, RowPath + TEXT("/curve"), Issues))
					{
						PendingRichRows.Add({ FName(*RowNameText.TrimStartAndEnd()), RichCurve });
					}
				}
			}
			if ((!PendingSimpleRows.IsEmpty() && (!PendingRichRows.IsEmpty() || CurveTable->GetCurveTableMode() == ECurveTableMode::RichCurves))
				|| (!PendingRichRows.IsEmpty() && CurveTable->GetCurveTableMode() == ECurveTableMode::SimpleCurves))
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_table_mixed_modes_unsupported"), Path, TEXT("A curve table cannot mix Simple and Rich storage."));
			if (HasError(Issues))
			{
				return false;
			}
			for (const FPendingSimpleCurveTableRow& PendingRow : PendingSimpleRows)
			{
				FSimpleCurve& RowCurve = CurveTable->AddSimpleCurve(PendingRow.RowName);
				RowCurve = PendingRow.Curve;
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

	bool TryBuildPropertyCurveJson(FProperty* Property, const void* ValuePtr, TSharedPtr<FJsonObject>& OutCurveJson, TSharedPtr<FJsonObject>* OutValueDefinition)
	{
		if (OutValueDefinition) OutValueDefinition->Reset();
		OutCurveJson.Reset();
		if (!Property || !ValuePtr)
		{
			return false;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == FRichCurve::StaticStruct())
			{
				OutCurveJson = BuildFloatCurveJson(*static_cast<const FRichCurve*>(ValuePtr), {}, {}, true, {});
				if (OutValueDefinition)
				{
					const auto Definition = MakeShared<FJsonObject>();
					Definition->SetStringField(TEXT("type"), TEXT("RichCurve"));
					Definition->SetObjectField(TEXT("contract"), BuildCurveValueDefinition(false));
					*OutValueDefinition = Definition;
				}
				return true;
			}
			if (StructProperty->Struct == FSimpleCurve::StaticStruct())
			{
				const FSimpleCurve* SimpleCurve = static_cast<const FSimpleCurve*>(ValuePtr);
				OutCurveJson = BuildSimpleCurveJson(*SimpleCurve, TEXT("simple_curve_property"), TEXT("FSimpleCurve"), true, {});
				if (OutValueDefinition)
				{
					const auto Definition = MakeShared<FJsonObject>();
					Definition->SetStringField(TEXT("type"), TEXT("SimpleCurve"));
					Definition->SetObjectField(TEXT("contract"), BuildCurveValueDefinition(true));
					*OutValueDefinition = Definition;
				}
				return true;
			}
			if (StructProperty->Struct == FRuntimeFloatCurve::StaticStruct())
			{
				const FRuntimeFloatCurve* RuntimeCurve = static_cast<const FRuntimeFloatCurve*>(ValuePtr);
				if (OutValueDefinition) *OutValueDefinition = BuildRuntimeCurveValueDefinition(ERuntimeCurveKind::Float);
				if (RuntimeCurve->ExternalCurve)
				{
					OutCurveJson = BuildAssetReferenceJson(RuntimeCurve->ExternalCurve, Property->GetCPPType(), true);
				}
				else if (const FRichCurve* RichCurve = RuntimeCurve->GetRichCurveConst())
				{
					OutCurveJson = BuildFloatCurveJson(*RichCurve, TEXT("runtime_float_curve"), TEXT("FRuntimeFloatCurve"), true, {});
				}
				return OutCurveJson.IsValid();
			}
			if (StructProperty->Struct == FRuntimeVectorCurve::StaticStruct())
			{
				const FRuntimeVectorCurve* RuntimeCurve = static_cast<const FRuntimeVectorCurve*>(ValuePtr);
				if (OutValueDefinition) *OutValueDefinition = BuildRuntimeCurveValueDefinition(ERuntimeCurveKind::Vector);
				if (RuntimeCurve->ExternalCurve)
				{
					OutCurveJson = BuildAssetReferenceJson(RuntimeCurve->ExternalCurve, Property->GetCPPType(), true);
				}
				else
				{
					OutCurveJson = BuildMultiChannelCurveJson(RuntimeCurve->VectorCurves, {TEXT("x"), TEXT("y"), TEXT("z")}, TEXT("vector"), {}, {}, true);
				}
				return OutCurveJson.IsValid();
			}
			if (StructProperty->Struct == FRuntimeCurveLinearColor::StaticStruct())
			{
				const FRuntimeCurveLinearColor* RuntimeCurve = static_cast<const FRuntimeCurveLinearColor*>(ValuePtr);
				if (OutValueDefinition) *OutValueDefinition = BuildRuntimeCurveValueDefinition(ERuntimeCurveKind::Color);
				if (RuntimeCurve->ExternalCurve)
				{
					OutCurveJson = BuildAssetReferenceJson(RuntimeCurve->ExternalCurve, Property->GetCPPType(), true);
				}
				else
				{
					OutCurveJson = BuildMultiChannelCurveJson(RuntimeCurve->ColorCurves, {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")}, TEXT("linear_color"), {}, {}, true);
				}
				return OutCurveJson.IsValid();
			}
		}

		return false;
	}

	static bool TryApplyAssetReferenceToProperty(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		TSharedPtr<FJsonObject> RefObj;
		if (!TryGetObjectField(RootObj, TEXT("asset_reference"), RefObj))
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_missing_required_field"), Path + TEXT("/asset_reference"), TEXT("Asset reference curve JSON requires asset_reference object."));
			return false;
		}
		FString AssetPath;
		if (!UeAgentJsonDiagnostics::ReadStringField(RefObj, TEXT("asset_path"), Path + TEXT("/asset_reference/asset_path"), AssetPath, Issues, true) || AssetPath.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}
		UObject* Asset = LoadObjectByPath(AssetPath);
		if (!Asset)
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_not_found"), Path + TEXT("/asset_reference/asset_path"), FString::Printf(TEXT("Curve asset '%s' could not be loaded."), *AssetPath));
			return false;
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == FRuntimeFloatCurve::StaticStruct())
			{
				UCurveFloat* CurveFloat = Cast<UCurveFloat>(Asset);
				if (!CurveFloat)
				{
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_class_mismatch"), Path + TEXT("/asset_reference/asset_path"), TEXT("FRuntimeFloatCurve external curve requires UCurveFloat."));
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
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_class_mismatch"), Path + TEXT("/asset_reference/asset_path"), TEXT("FRuntimeVectorCurve external curve requires UCurveVector."));
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
					AddCurveIssue(Issues, TEXT("error"), TEXT("curve_asset_class_mismatch"), Path + TEXT("/asset_reference/asset_path"), TEXT("FRuntimeCurveLinearColor external curve requires UCurveLinearColor."));
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

	bool TryApplyPropertyCurveJson(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
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
				return ApplyChannelJsonToSimpleCurve(RootObj, TEXT("value"), *static_cast<FSimpleCurve*>(ValuePtr), Path, Issues);
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
		}

		AddCurveIssue(Issues, TEXT("error"), TEXT("curve_unsupported_carrier"), Path, FString::Printf(TEXT("Property type '%s' is not a supported curve JSON carrier."), *Property->GetCPPType()));
		return false;
	}

	static bool CompareFloatCurves(const FRichCurve& Requested, const FRichCurve& Readback, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		const TArray<FRichCurveKey>& A = Requested.GetConstRefOfKeys();
		const TArray<FRichCurveKey>& B = Readback.GetConstRefOfKeys();
		if (A.Num() != B.Num())
		{
			AddCurveIssue(Issues, TEXT("error"), TEXT("curve_readback_mismatch"), Path + TEXT("/keys"), FString::Printf(TEXT("Curve key count mismatch: requested=%d readback=%d."), A.Num(), B.Num()));
			return false;
		}
		bool bOk = true;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (FMath::Abs(A[Index].Time - B[Index].Time) > ValueTolerance || FMath::Abs(A[Index].Value - B[Index].Value) > ValueTolerance)
			{
				AddCurveIssue(Issues, TEXT("error"), TEXT("curve_readback_mismatch"), FString::Printf(TEXT("%s/keys/%d"), *Path, Index), FString::Printf(TEXT("Curve key mismatch: requested=(%.6f,%.6f) readback=(%.6f,%.6f)."), A[Index].Time, A[Index].Value, B[Index].Time, B[Index].Value));
				bOk = false;
			}
		}
		return bOk;
	}

	UObject* CreateCurveAsset(const FString& AssetPathInput, const FString& CurveKind, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FString AssetPath = NormalizeAssetPath(AssetPathInput);
		FText PathReason;
		if (!FPackageName::IsValidLongPackageName(AssetPath, false, &PathReason))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_curve_asset_path"),
				PathReason.IsEmpty() ? TEXT("The curve asset path is not a valid writable package name.") : PathReason.ToString(),
				TEXT("curve_asset_create"));
			OutError.PropertyPath = TEXT("asset_path");
			OutError.TargetPath = AssetPathInput;
			OutError.ActualValue = MakeShared<FJsonValueString>(AssetPathInput);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("valid writable long package name"));
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("create_curve_package_failed"),
				TEXT("Unreal could not create the package for the curve asset."), TEXT("curve_asset_create"));
			OutError.TargetPath = AssetPath;
			OutError.SourceKind = TEXT("native_result");
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
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unsupported_curve_kind"),
				TEXT("The requested curve kind does not identify a supported curve asset type."),
				TEXT("curve_asset_create"));
			OutError.PropertyPath = TEXT("curve_kind");
			OutError.TargetPath = AssetPath;
			OutError.ActualValue = MakeShared<FJsonValueString>(CurveKind);
			TArray<TSharedPtr<FJsonValue>> SupportedKinds;
			for (const TCHAR* Kind : {TEXT("float"), TEXT("curve_float"), TEXT("vector"), TEXT("curve_vector"),
				TEXT("linear_color"), TEXT("color"), TEXT("curve_linear_color"), TEXT("curve_table")})
				SupportedKinds.Add(MakeShared<FJsonValueString>(Kind));
			OutError.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(SupportedKinds));
			return nullptr;
		}

		UObject* Asset = NewObject<UObject>(Package, CurveClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("create_curve_asset_failed"),
				TEXT("Unreal could not create the curve asset in the requested package."), TEXT("curve_asset_create"));
			OutError.TargetPath = AssetPath;
			OutError.SourceKind = TEXT("native_result");
			OutError.InternalContext = CurveClass->GetPathName();
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		return Asset;
	}
}
