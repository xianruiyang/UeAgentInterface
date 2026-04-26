// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Material.h"

#include "UeAgentJsonDiagnostics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "FileHelpers.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UnrealType.h"

namespace UeAgentMaterialOps
{
	enum class ECompileMessageSeverityFilter : uint8
	{
		All,
		Error,
		Warning,
		Info,
		WarningOrError
	};

	enum class EMaterialInstanceParameterType : uint8
	{
		Scalar,
		Vector,
		Texture,
		StaticSwitch,
		StaticComponentMask
	};

	static bool ParseMaterialInstanceParameterType(const FString& InText, EMaterialInstanceParameterType& OutType)
	{
		FString Value = InText.TrimStartAndEnd().ToLower();
		Value.ReplaceInline(TEXT("_"), TEXT(""));
		Value.ReplaceInline(TEXT("-"), TEXT(""));
		Value.ReplaceInline(TEXT(" "), TEXT(""));

		if (Value == TEXT("scalar") || Value == TEXT("float"))
		{
			OutType = EMaterialInstanceParameterType::Scalar;
			return true;
		}
		if (Value == TEXT("vector") || Value == TEXT("color") || Value == TEXT("linearcolor"))
		{
			OutType = EMaterialInstanceParameterType::Vector;
			return true;
		}
		if (Value == TEXT("texture") || Value == TEXT("textureobject"))
		{
			OutType = EMaterialInstanceParameterType::Texture;
			return true;
		}
		if (Value == TEXT("staticswitch") || Value == TEXT("switch") || Value == TEXT("bool") || Value == TEXT("boolean"))
		{
			OutType = EMaterialInstanceParameterType::StaticSwitch;
			return true;
		}
		if (Value == TEXT("staticcomponentmask") || Value == TEXT("componentmask") || Value == TEXT("mask"))
		{
			OutType = EMaterialInstanceParameterType::StaticComponentMask;
			return true;
		}
		return false;
	}

	static bool ResolveStaticComponentMaskValue(
		const UMaterialInterface* MaterialInterface,
		const FMaterialParameterInfo& ParameterInfo,
		const FGuid& FallbackExpressionGuid,
		bool& OutR,
		bool& OutG,
		bool& OutB,
		bool& OutA,
		FGuid& OutExpressionGuid)
	{
		OutR = false;
		OutG = false;
		OutB = false;
		OutA = false;
		OutExpressionGuid = FallbackExpressionGuid;

		if (!MaterialInterface)
		{
			return false;
		}

		if (const UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(MaterialInterface))
		{
			const FStaticParameterSet StaticParameters = MaterialInstance->GetStaticParameters();
			for (const FStaticComponentMaskParameter& Parameter : StaticParameters.EditorOnly.StaticComponentMaskParameters)
			{
				if (Parameter.ParameterInfo == ParameterInfo)
				{
					OutR = Parameter.R;
					OutG = Parameter.G;
					OutB = Parameter.B;
					OutA = Parameter.A;
					OutExpressionGuid = Parameter.ExpressionGUID;
					return true;
				}
			}

			if (MaterialInstance->Parent)
			{
				return MaterialInstance->Parent->GetStaticComponentMaskParameterDefaultValue(
					FHashedMaterialParameterInfo(ParameterInfo),
					OutR,
					OutG,
					OutB,
					OutA,
					OutExpressionGuid);
			}
			return false;
		}

		return MaterialInterface->GetStaticComponentMaskParameterDefaultValue(
			FHashedMaterialParameterInfo(ParameterInfo),
			OutR,
			OutG,
			OutB,
			OutA,
			OutExpressionGuid);
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
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
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

	static UMaterial* LoadMaterialAsset(const FString& InPath)
	{
		return Cast<UMaterial>(LoadAssetObject(InPath));
	}

	static UMaterialInterface* LoadMaterialInterface(const FString& InPath)
	{
		return Cast<UMaterialInterface>(LoadAssetObject(InPath));
	}

	static UMaterialFunction* LoadMaterialFunctionAsset(const FString& InPath)
	{
		return Cast<UMaterialFunction>(LoadAssetObject(InPath));
	}

	static UMaterialInstanceConstant* LoadMaterialInstance(const FString& InPath)
	{
		return Cast<UMaterialInstanceConstant>(LoadAssetObject(InPath));
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

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.IsEmpty())
		{
			return nullptr;
		}
		const FString Normalized = Segment.TrimStartAndEnd();
		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			const FString PropertyNameWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
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

	static bool ParseLinearColor(const TSharedPtr<FJsonObject>& Params, const FString& Key, FLinearColor& OutColor)
	{
		const TSharedPtr<FJsonObject>* ColorObjPtr = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(Key, ColorObjPtr) || !ColorObjPtr || !ColorObjPtr->IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>& ColorObj = *ColorObjPtr;
		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 1.0;

		const bool bHasRgb =
			UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(ColorObj, { TEXT("r"), TEXT("R"), TEXT("red"), TEXT("Red"), TEXT("x"), TEXT("X") }, R)
			&& UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(ColorObj, { TEXT("g"), TEXT("G"), TEXT("green"), TEXT("Green"), TEXT("y"), TEXT("Y") }, G)
			&& UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(ColorObj, { TEXT("b"), TEXT("B"), TEXT("blue"), TEXT("Blue"), TEXT("z"), TEXT("Z") }, B);
		if (!bHasRgb)
		{
			return false;
		}

		UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(ColorObj, { TEXT("a"), TEXT("A"), TEXT("alpha"), TEXT("Alpha"), TEXT("w"), TEXT("W") }, A);
		OutColor = FLinearColor((float)R, (float)G, (float)B, (float)A);
		return true;
	}

	static TSharedPtr<FJsonObject> ExpressionToJson(UMaterialExpression* Expression)
	{
		TSharedPtr<FJsonObject> ExprObj = MakeShared<FJsonObject>();
		if (!Expression)
		{
			return ExprObj;
		}

		ExprObj->SetStringField(TEXT("name"), Expression->GetName());
		ExprObj->SetStringField(TEXT("class"), Expression->GetClass() ? Expression->GetClass()->GetPathName() : TEXT(""));
		ExprObj->SetStringField(TEXT("guid"), Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		ExprObj->SetNumberField(TEXT("node_pos_x"), Expression->MaterialExpressionEditorX);
		ExprObj->SetNumberField(TEXT("node_pos_y"), Expression->MaterialExpressionEditorY);
		ExprObj->SetStringField(TEXT("desc"), Expression->Desc);

		TArray<TSharedPtr<FJsonValue>> OutputNames;
		const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
		for (const FExpressionOutput& Output : Outputs)
		{
			const FString OutputName = Output.OutputName.IsNone() ? TEXT("") : Output.OutputName.ToString();
			OutputNames.Add(MakeShared<FJsonValueString>(OutputName));
		}
		ExprObj->SetArrayField(TEXT("outputs"), OutputNames);

		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (FExpressionInputIterator It{ Expression }; It; ++It)
		{
			TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
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
			InputObj->SetStringField(TEXT("name"), InputName);
			InputObj->SetNumberField(TEXT("index"), It.Index);
			InputObj->SetBoolField(TEXT("connected"), It->Expression != nullptr);
			Inputs.Add(MakeShared<FJsonValueObject>(InputObj));
		}
		ExprObj->SetArrayField(TEXT("inputs"), Inputs);
		return ExprObj;
	}

	static FString GetExpressionInputDisplayName(UMaterialExpression* Expression, int32 InputIndex, const FExpressionInput& Input)
	{
		if (!Expression)
		{
			return FString();
		}

		FString InputName;
		if (const UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			InputName = FunctionCall->GetInputNameWithType(InputIndex, false).ToString();
		}
		else
		{
			InputName = Expression->GetInputName(InputIndex).ToString();
		}

		if (InputName.IsEmpty())
		{
			InputName = Input.InputName.ToString();
		}
		return InputName;
	}

	static bool ResolveExpressionOutputIndex(UMaterialExpression* Expression, const FString& OutputName, int32& OutOutputIndex)
	{
		OutOutputIndex = INDEX_NONE;
		if (!Expression)
		{
			return false;
		}

		const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
		if (Outputs.IsEmpty())
		{
			return false;
		}

		const FString TrimmedOutputName = OutputName.TrimStartAndEnd();
		if (TrimmedOutputName.IsEmpty())
		{
			OutOutputIndex = 0;
			return true;
		}

		for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
		{
			const FString CurrentName = Outputs[OutputIndex].OutputName.IsNone() ? TEXT("") : Outputs[OutputIndex].OutputName.ToString();
			if (CurrentName.Equals(TrimmedOutputName, ESearchCase::IgnoreCase))
			{
				OutOutputIndex = OutputIndex;
				return true;
			}
		}

		return false;
	}

	static bool ResolveExpressionInput(UMaterialExpression* Expression, const FString& InputName, FExpressionInput*& OutInput)
	{
		OutInput = nullptr;
		if (!Expression)
		{
			return false;
		}

		const FString TrimmedInputName = InputName.TrimStartAndEnd();
		FExpressionInput* FirstInput = nullptr;
		int32 InputCount = 0;
		for (FExpressionInputIterator It{ Expression }; It; ++It)
		{
			if (!FirstInput)
			{
				FirstInput = It.Input;
			}
			++InputCount;

			const FString CurrentName = GetExpressionInputDisplayName(Expression, It.Index, *It.Input);
			if (!TrimmedInputName.IsEmpty() && CurrentName.Equals(TrimmedInputName, ESearchCase::IgnoreCase))
			{
				OutInput = It.Input;
				return true;
			}
		}

		if (TrimmedInputName.IsEmpty() && InputCount == 1)
		{
			OutInput = FirstInput;
			return true;
		}

		return false;
	}

	static bool ManualConnectExpressions(UMaterialExpression* FromExpression, const FString& FromOutputName, UMaterialExpression* ToExpression, const FString& ToInputName)
	{
		int32 OutputIndex = INDEX_NONE;
		if (!ResolveExpressionOutputIndex(FromExpression, FromOutputName, OutputIndex))
		{
			return false;
		}

		FExpressionInput* Input = nullptr;
		if (!ResolveExpressionInput(ToExpression, ToInputName, Input) || !Input)
		{
			return false;
		}

		ToExpression->Modify();
		FromExpression->ConnectExpression(Input, OutputIndex);
		return true;
	}

	static UMaterialExpression* FindExpressionByParams(UMaterial* Material, const TSharedPtr<FJsonObject>& Params)
	{
		if (!Material || !Params.IsValid())
		{
			return nullptr;
		}

		FString ExpressionGuidText;
		if (Params->TryGetStringField(TEXT("expression_guid"), ExpressionGuidText))
		{
			FGuid ExpressionGuid;
			if (FGuid::Parse(ExpressionGuidText, ExpressionGuid))
			{
				for (UMaterialExpression* Expression : Material->GetExpressions())
				{
					if (Expression && Expression->MaterialExpressionGuid == ExpressionGuid)
					{
						return Expression;
					}
				}
			}
		}

		FString ExpressionName;
		if (Params->TryGetStringField(TEXT("expression_name"), ExpressionName))
		{
			const FString NameTrim = ExpressionName.TrimStartAndEnd();
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (Expression && Expression->GetName().Equals(NameTrim, ESearchCase::IgnoreCase))
				{
					return Expression;
				}
			}
		}

		double ExpressionIndex = 0.0;
		if (Params->TryGetNumberField(TEXT("expression_index"), ExpressionIndex))
		{
			const int32 Index = (int32)ExpressionIndex;
			const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();
			if (Expressions.IsValidIndex(Index))
			{
				return Expressions[Index];
			}
		}

		return nullptr;
	}

	static UMaterialExpression* FindExpressionBySelector(UMaterial* Material, const TSharedPtr<FJsonObject>& Params, const FString& Prefix)
	{
		if (!Material || !Params.IsValid())
		{
			return nullptr;
		}

		const FString GuidKey = Prefix.IsEmpty() ? TEXT("expression_guid") : (Prefix + TEXT("_guid"));
		const FString NameKey = Prefix.IsEmpty() ? TEXT("expression_name") : (Prefix + TEXT("_name"));
		const FString IndexKey = Prefix.IsEmpty() ? TEXT("expression_index") : (Prefix + TEXT("_index"));

		FString ExpressionGuidText;
		if (Params->TryGetStringField(GuidKey, ExpressionGuidText))
		{
			FGuid ExpressionGuid;
			if (FGuid::Parse(ExpressionGuidText, ExpressionGuid))
			{
				for (UMaterialExpression* Expression : Material->GetExpressions())
				{
					if (Expression && Expression->MaterialExpressionGuid == ExpressionGuid)
					{
						return Expression;
					}
				}
			}
		}

		FString ExpressionName;
		if (Params->TryGetStringField(NameKey, ExpressionName))
		{
			const FString NameTrim = ExpressionName.TrimStartAndEnd();
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (Expression && Expression->GetName().Equals(NameTrim, ESearchCase::IgnoreCase))
				{
					return Expression;
				}
			}
		}

		double ExpressionIndex = 0.0;
		if (Params->TryGetNumberField(IndexKey, ExpressionIndex))
		{
			const int32 Index = (int32)ExpressionIndex;
			const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();
			if (Expressions.IsValidIndex(Index))
			{
				return Expressions[Index];
			}
		}

		return nullptr;
	}

	static bool ParseCompileSeverityFilter(const FString& InFilterText, ECompileMessageSeverityFilter& OutFilter)
	{
		FString Normalized = InFilterText.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));

		if (Normalized.IsEmpty() || Normalized == TEXT("all"))
		{
			OutFilter = ECompileMessageSeverityFilter::All;
			return true;
		}
		if (Normalized == TEXT("error") || Normalized == TEXT("errors"))
		{
			OutFilter = ECompileMessageSeverityFilter::Error;
			return true;
		}
		if (Normalized == TEXT("warning") || Normalized == TEXT("warnings") || Normalized == TEXT("warn"))
		{
			OutFilter = ECompileMessageSeverityFilter::Warning;
			return true;
		}
		if (Normalized == TEXT("info") || Normalized == TEXT("note") || Normalized == TEXT("notes"))
		{
			OutFilter = ECompileMessageSeverityFilter::Info;
			return true;
		}
		if (Normalized == TEXT("warningorerror") || Normalized == TEXT("warnorerror") || Normalized == TEXT("errorsandwarnings"))
		{
			OutFilter = ECompileMessageSeverityFilter::WarningOrError;
			return true;
		}
		return false;
	}

	static bool ShouldIncludeCompileMessage(const ECompileMessageSeverityFilter InFilter)
	{
		switch (InFilter)
		{
		case ECompileMessageSeverityFilter::All:
		case ECompileMessageSeverityFilter::Error:
		case ECompileMessageSeverityFilter::WarningOrError:
			return true;
		case ECompileMessageSeverityFilter::Warning:
		case ECompileMessageSeverityFilter::Info:
		default:
			return false;
		}
	}

	static FString FeatureLevelToString(const ERHIFeatureLevel::Type InFeatureLevel)
	{
		switch (InFeatureLevel)
		{
		case ERHIFeatureLevel::ES2_REMOVED:
			return TEXT("ES2_REMOVED");
		case ERHIFeatureLevel::ES3_1:
			return TEXT("ES3_1");
		case ERHIFeatureLevel::SM4_REMOVED:
			return TEXT("SM4_REMOVED");
		case ERHIFeatureLevel::SM5:
			return TEXT("SM5");
		case ERHIFeatureLevel::SM6:
			return TEXT("SM6");
		case ERHIFeatureLevel::Num:
			return TEXT("Num");
		default:
			return TEXT("Unknown");
		}
	}

	static void CollectMaterialCompileMessages(
		UMaterialInterface* MaterialInterface,
		const ECompileMessageSeverityFilter InFilter,
		const int32 InMaxMessages,
		TArray<TSharedPtr<FJsonValue>>& OutMessages,
		int32& OutErrorCount)
	{
		OutMessages.Reset();
		OutErrorCount = 0;
		if (!MaterialInterface || !ShouldIncludeCompileMessage(InFilter))
		{
			return;
		}

		const int32 MaxMessages = FMath::Clamp(InMaxMessages, 1, 2000);
		TSet<FString> DedupSet;
		UMaterialInterface::IterateOverActiveFeatureLevels([&](const ERHIFeatureLevel::Type FeatureLevel)
		{
			const FMaterialResource* MaterialResource = MaterialInterface->GetMaterialResource(FeatureLevel);
			if (!MaterialResource)
			{
				return;
			}

			const TArray<FString>& CompileErrors = MaterialResource->GetCompileErrors();
			if (CompileErrors.Num() == 0)
			{
				return;
			}

			const FString FeatureLevelName = FeatureLevelToString(FeatureLevel);
			for (const FString& CompileError : CompileErrors)
			{
				const FString MessageText = CompileError.TrimStartAndEnd();
				if (MessageText.IsEmpty())
				{
					continue;
				}

				const FString DedupKey = FeatureLevelName + TEXT("::") + MessageText;
				if (DedupSet.Contains(DedupKey))
				{
					continue;
				}
				DedupSet.Add(DedupKey);
				++OutErrorCount;

				if (OutMessages.Num() >= MaxMessages)
				{
					continue;
				}

				TSharedPtr<FJsonObject> MessageObj = MakeShared<FJsonObject>();
				MessageObj->SetStringField(TEXT("severity"), TEXT("Error"));
				MessageObj->SetStringField(TEXT("feature_level"), FeatureLevelName);
				MessageObj->SetStringField(TEXT("message"), MessageText);
				OutMessages.Add(MakeShared<FJsonValueObject>(MessageObj));
			}
		});
	}

	static bool ParseMaterialProperty(const FString& InPropertyName, EMaterialProperty& OutProperty)
	{
		if (InPropertyName.IsEmpty())
		{
			return false;
		}

		FString Key = InPropertyName.TrimStartAndEnd().ToLower();
		Key.ReplaceInline(TEXT("_"), TEXT(""));

		if (Key == TEXT("basecolor") || Key == TEXT("diffusecolor")) { OutProperty = MP_BaseColor; return true; }
		if (Key == TEXT("emissivecolor") || Key == TEXT("emissive")) { OutProperty = MP_EmissiveColor; return true; }
		if (Key == TEXT("opacity")) { OutProperty = MP_Opacity; return true; }
		if (Key == TEXT("opacitymask")) { OutProperty = MP_OpacityMask; return true; }
		if (Key == TEXT("metallic")) { OutProperty = MP_Metallic; return true; }
		if (Key == TEXT("specular")) { OutProperty = MP_Specular; return true; }
		if (Key == TEXT("roughness")) { OutProperty = MP_Roughness; return true; }
		if (Key == TEXT("normal")) { OutProperty = MP_Normal; return true; }
		if (Key == TEXT("tangent")) { OutProperty = MP_Tangent; return true; }
		if (Key == TEXT("worldpositionoffset") || Key == TEXT("wpo")) { OutProperty = MP_WorldPositionOffset; return true; }
		if (Key == TEXT("subsurfacecolor")) { OutProperty = MP_SubsurfaceColor; return true; }
		if (Key == TEXT("ambientocclusion") || Key == TEXT("ao")) { OutProperty = MP_AmbientOcclusion; return true; }
		if (Key == TEXT("refraction")) { OutProperty = MP_Refraction; return true; }
		if (Key == TEXT("pixeldepthoffset") || Key == TEXT("pdo")) { OutProperty = MP_PixelDepthOffset; return true; }
		if (Key == TEXT("anisotropy")) { OutProperty = MP_Anisotropy; return true; }

		return false;
	}
}


#include "UeAgentHttpServer_Material_DispatchAndAssetInfo.inl"
#include "UeAgentHttpServer_Material_CompileAndParameters.inl"
#include "UeAgentHttpServer_Material_ExpressionGraph.inl"
#include "UeAgentHttpServer_Material_FolderFormat.inl"
