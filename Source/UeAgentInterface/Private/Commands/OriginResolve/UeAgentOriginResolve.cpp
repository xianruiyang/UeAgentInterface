// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/OriginResolve/UeAgentOriginResolveTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformStackWalk.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SourceCodeNavigation.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentOriginResolve
{
	FCodePath::FCodePath()
		: Line(0)
		, Column(0)
	{
	}

	FResolveResult::FResolveResult()
		: bResolved(false)
	{
	}

namespace
{
	static FString NormalizeToken(const FString& Text)
	{
		return Text.TrimStartAndEnd().ToLower();
	}

	static void ReadStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FString& InOutValue)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		FString Value;
		if (Obj->TryGetStringField(FieldName, Value) && !Value.TrimStartAndEnd().IsEmpty())
		{
			InOutValue = Value.TrimStartAndEnd();
		}
	}

	static bool IsReferenceSource(const FString& Value)
	{
		const FString Token = NormalizeToken(Value);
		return Token.Equals(TEXT("catalog_candidate"))
			|| Token.Equals(TEXT("json_reference"))
			|| Token.Equals(TEXT("name_reference"))
			|| Token.Equals(TEXT("graph_node"))
			|| Token.Equals(TEXT("ue_object"));
	}

	static bool SplitMemberPath(const FString& FullName, FString& OutOwnerPath, FString& OutMemberName)
	{
		const int32 ColonPos = FullName.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const int32 DotPos = FullName.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const int32 SplitPos = FMath::Max(ColonPos, DotPos);
		if (SplitPos == INDEX_NONE)
		{
			return false;
		}

		OutOwnerPath = FullName.Left(SplitPos);
		OutMemberName = FullName.Mid(SplitPos + 1);
		return !OutOwnerPath.IsEmpty() && !OutMemberName.IsEmpty();
	}

	static UClass* LoadClassByPath(const FString& ClassPath)
	{
		if (ClassPath.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		if (UClass* ExistingClass = FindObject<UClass>(nullptr, *ClassPath))
		{
			return ExistingClass;
		}
		return LoadObject<UClass>(nullptr, *ClassPath);
	}

	static UFunction* FindFunctionByFullName(const FString& FullName)
	{
		if (UFunction* ExistingFunction = FindObject<UFunction>(nullptr, *FullName))
		{
			return ExistingFunction;
		}

		FString OwnerPath;
		FString FunctionName;
		if (!SplitMemberPath(FullName, OwnerPath, FunctionName))
		{
			return nullptr;
		}
		UClass* OwnerClass = LoadClassByPath(OwnerPath);
		return OwnerClass ? OwnerClass->FindFunctionByName(FName(*FunctionName)) : nullptr;
	}

	static const FProperty* FindPropertyByFullName(const FString& FullName)
	{
		FString OwnerPath;
		FString PropertyName;
		if (!SplitMemberPath(FullName, OwnerPath, PropertyName))
		{
			return nullptr;
		}
		UClass* OwnerClass = LoadClassByPath(OwnerPath);
		return OwnerClass ? FindFProperty<FProperty>(OwnerClass, FName(*PropertyName)) : nullptr;
	}

	static FString RebaseSourcePathIfNeeded(FString SourcePath)
	{
		if (FPaths::FileExists(SourcePath))
		{
			return IFileManager::Get().GetFilenameOnDisk(*SourcePath);
		}

		TArray<FString> Tokens;
		SourcePath.ParseIntoArray(Tokens, TEXT("\\"));
		if (Tokens.Num() <= 1)
		{
			return SourcePath;
		}

		auto TryBasePath = [&Tokens](const FString& BasePath, FString& OutPath)
		{
			FString PathTail = Tokens.Last();
			for (int32 Index = Tokens.Num() - 2; Index >= 0; --Index)
			{
				const FString CandidatePath = FPaths::Combine(BasePath, PathTail);
				if (FPaths::FileExists(CandidatePath))
				{
					OutPath = IFileManager::Get().GetFilenameOnDisk(*CandidatePath);
					return true;
				}
				PathTail = Tokens[Index] + TEXT("\\") + PathTail;
			}
			return false;
		};

		FString RebasedPath;
		if (TryBasePath(FPaths::EngineDir(), RebasedPath) || TryBasePath(FPaths::ProjectDir(), RebasedPath))
		{
			return RebasedPath;
		}
		return SourcePath;
	}

	static void AddCodePath(TArray<FCodePath>& CodePaths, const FString& Path, int32 Line, int32 Column, const FString& Symbol, const FString& Module, const FString& Status)
	{
		if (Path.IsEmpty())
		{
			return;
		}
		FCodePath CodePath;
		CodePath.Path = Path;
		CodePath.Line = Line;
		CodePath.Column = Column;
		CodePath.Symbol = Symbol;
		CodePath.Module = Module;
		CodePath.Status = Status;
		CodePaths.Add(MoveTemp(CodePath));
	}

	static void AddClassSourcePaths(UClass* Class, TArray<FCodePath>& CodePaths, const FString& Status)
	{
		if (!Class)
		{
			return;
		}

		FString HeaderPath;
		if (FSourceCodeNavigation::FindClassHeaderPath(Class, HeaderPath))
		{
			AddCodePath(CodePaths, HeaderPath, 0, 0, Class->GetName(), FString(), Status);
		}

		FString SourcePath;
		if (FSourceCodeNavigation::FindClassSourcePath(Class, SourcePath))
		{
			AddCodePath(CodePaths, SourcePath, 0, 0, Class->GetName(), FString(), Status);
		}
	}

	static bool AddFunctionSourcePath(UFunction* Function, TArray<FCodePath>& CodePaths, TArray<FString>& Warnings)
	{
		if (!Function || !Function->GetOwnerClass())
		{
			return false;
		}

		UClass* OwnerClass = Function->GetOwnerClass();
		FString ModuleName;
		if (!FSourceCodeNavigation::FindClassModuleName(OwnerClass, ModuleName))
		{
			Warnings.Add(TEXT("native_function_module_not_found"));
			AddClassSourcePaths(OwnerClass, CodePaths, TEXT("fallback_class_source"));
			return !CodePaths.IsEmpty();
		}

		const FString SymbolName = FString::Printf(TEXT("%s%s::%s"), OwnerClass->GetPrefixCPP(), *OwnerClass->GetName(), *Function->GetName());
		FPlatformStackWalk::InitStackWalking();
		FString SourceFileName;
		uint32 SourceLineNumber = 1;
		uint32 SourceColumnNumber = 0;
		if (FPlatformStackWalk::GetFunctionDefinitionLocation(SymbolName, ModuleName, SourceFileName, SourceLineNumber, SourceColumnNumber))
		{
			SourceFileName = RebaseSourcePathIfNeeded(SourceFileName);
			AddCodePath(CodePaths, SourceFileName, static_cast<int32>(SourceLineNumber), static_cast<int32>(SourceColumnNumber), SymbolName, ModuleName, TEXT("symbol_definition"));
			return true;
		}

		Warnings.Add(FString::Printf(TEXT("native_function_symbol_not_found:%s"), *SymbolName));
		AddClassSourcePaths(OwnerClass, CodePaths, TEXT("fallback_class_source"));
		return !CodePaths.IsEmpty();
	}

	static FString NormalizeObjectPath(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty() || Path.Contains(TEXT(".")) || Path.Contains(TEXT(":")) || Path.StartsWith(TEXT("/Script/")))
		{
			return Path;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
		return AssetName.IsEmpty() ? Path : FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
	}

	static void AddAssetOrigin(const FString& FullName, FResolveResult& OutResult)
	{
		const FString ObjectPath = NormalizeObjectPath(FullName);
		const FString PackagePath = FPackageName::ObjectPathToPackageName(ObjectPath.IsEmpty() ? FullName : ObjectPath);
		if (!ObjectPath.IsEmpty())
		{
			OutResult.AssetPaths.AddUnique(ObjectPath);
		}
		if (!PackagePath.IsEmpty())
		{
			OutResult.PackagePaths.AddUnique(PackagePath);
			FString PackageFilename;
			if (FPackageName::DoesPackageExist(PackagePath, &PackageFilename, true))
			{
				OutResult.PackageFiles.AddUnique(IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*PackageFilename));
			}
			else if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, PackageFilename, FPackageName::GetAssetPackageExtension()))
			{
				OutResult.PackageFiles.AddUnique(PackageFilename);
			}
		}
	}

	static bool LooksLikeAssetPath(const FString& FullName)
	{
		return FullName.StartsWith(TEXT("/Game/"))
			|| FullName.StartsWith(TEXT("/Engine/"))
			|| FullName.StartsWith(TEXT("/Niagara/"))
			|| FullName.StartsWith(TEXT("/Plugins/"));
	}

	static bool ExtractIdentityFromObject(const TSharedPtr<FJsonObject>& Obj, FResolveParams& InOutParams, TArray<FString>& Warnings)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		bool bFound = false;
		auto ReadIdentity = [&Obj, &InOutParams, &bFound]()
		{
			const FString OldKind = InOutParams.Kind;
			const FString OldName = InOutParams.Name;
			const FString OldFullName = InOutParams.FullName;
			ReadStringField(Obj, TEXT("kind"), InOutParams.Kind);
			ReadStringField(Obj, TEXT("name"), InOutParams.Name);
			ReadStringField(Obj, TEXT("full_name"), InOutParams.FullName);
			bFound = bFound || !OldKind.Equals(InOutParams.Kind) || !OldName.Equals(InOutParams.Name) || !OldFullName.Equals(InOutParams.FullName);
		};

		ReadIdentity();

		const TSharedPtr<FJsonObject>* PatchObjPtr = nullptr;
		if (Obj->TryGetObjectField(TEXT("patch"), PatchObjPtr) && PatchObjPtr && PatchObjPtr->IsValid())
		{
			bFound |= ExtractIdentityFromObject(*PatchObjPtr, InOutParams, Warnings);
		}
		const TSharedPtr<FJsonObject>* DynamicInputObjPtr = nullptr;
		if (Obj->TryGetObjectField(TEXT("dynamic_input"), DynamicInputObjPtr) && DynamicInputObjPtr && DynamicInputObjPtr->IsValid())
		{
			bFound |= ExtractIdentityFromObject(*DynamicInputObjPtr, InOutParams, Warnings);
			if (InOutParams.Kind.IsEmpty())
			{
				InOutParams.Kind = TEXT("niagara_dynamic_input");
			}
		}

		FString LegacyValue;
		if (Obj->TryGetStringField(TEXT("module_script_asset_path"), LegacyValue) ||
			Obj->TryGetStringField(TEXT("module_script_path"), LegacyValue) ||
			Obj->TryGetStringField(TEXT("module_script"), LegacyValue))
		{
			InOutParams.Kind = TEXT("niagara_module");
			InOutParams.FullName = LegacyValue;
			ReadStringField(Obj, TEXT("module_name"), InOutParams.Name);
			Warnings.Add(TEXT("legacy_identity_fields_used:niagara_module"));
			return true;
		}
		if (Obj->TryGetStringField(TEXT("dynamic_input_script_asset_path"), LegacyValue) ||
			Obj->TryGetStringField(TEXT("script_asset_path"), LegacyValue))
		{
			InOutParams.Kind = TEXT("niagara_dynamic_input");
			InOutParams.FullName = LegacyValue;
			Warnings.Add(TEXT("legacy_identity_fields_used:niagara_dynamic_input"));
			return true;
		}
		if (Obj->TryGetStringField(TEXT("function_script_asset_path"), LegacyValue) ||
			Obj->TryGetStringField(TEXT("function_script_path"), LegacyValue))
		{
			InOutParams.Kind = TEXT("niagara_graph_function");
			InOutParams.FullName = LegacyValue;
			ReadStringField(Obj, TEXT("function_name"), InOutParams.Name);
			Warnings.Add(TEXT("legacy_identity_fields_used:niagara_graph_function"));
			return true;
		}
		if (Obj->TryGetStringField(TEXT("material_function"), LegacyValue))
		{
			InOutParams.Kind = TEXT("material_function_call");
			InOutParams.FullName = LegacyValue;
			Warnings.Add(TEXT("legacy_identity_fields_used:material_function_call"));
			return true;
		}
		if (Obj->TryGetStringField(TEXT("expression_class"), LegacyValue))
		{
			InOutParams.Kind = TEXT("material_expression");
			InOutParams.FullName = LegacyValue;
			Warnings.Add(TEXT("legacy_identity_fields_used:material_expression"));
			return true;
		}
		if (Obj->TryGetStringField(TEXT("component_class"), LegacyValue))
		{
			InOutParams.Kind = TEXT("actor_component");
			InOutParams.FullName = LegacyValue;
			ReadStringField(Obj, TEXT("component_name"), InOutParams.Name);
			Warnings.Add(TEXT("legacy_identity_fields_used:actor_component"));
			return true;
		}
		if (Obj->TryGetStringField(TEXT("widget_class"), LegacyValue))
		{
			InOutParams.Kind = TEXT("umg_widget");
			InOutParams.FullName = LegacyValue;
			ReadStringField(Obj, TEXT("widget_name"), InOutParams.Name);
			Warnings.Add(TEXT("legacy_identity_fields_used:umg_widget"));
			return true;
		}
		if (Obj->TryGetStringField(TEXT("function_owner_class"), LegacyValue))
		{
			FString FunctionName;
			ReadStringField(Obj, TEXT("function_name"), FunctionName);
			InOutParams.Kind = TEXT("blueprint_call_function");
			InOutParams.FullName = FunctionName.IsEmpty() ? LegacyValue : LegacyValue + TEXT(".") + FunctionName;
			InOutParams.Name = FunctionName;
			Warnings.Add(TEXT("legacy_identity_fields_used:blueprint_call_function"));
			return true;
		}
		if (Obj->TryGetStringField(TEXT("event_class"), LegacyValue))
		{
			FString EventName;
			ReadStringField(Obj, TEXT("event_name"), EventName);
			InOutParams.Kind = TEXT("blueprint_event");
			InOutParams.FullName = EventName.IsEmpty() ? LegacyValue : LegacyValue + TEXT(".") + EventName;
			InOutParams.Name = EventName;
			Warnings.Add(TEXT("legacy_identity_fields_used:blueprint_event"));
			return true;
		}
		if (Obj->TryGetStringField(TEXT("member_parent_class"), LegacyValue))
		{
			FString VariableName;
			ReadStringField(Obj, TEXT("variable_name"), VariableName);
			InOutParams.Kind = TEXT("blueprint_variable_node");
			InOutParams.FullName = VariableName.IsEmpty() ? LegacyValue : LegacyValue + TEXT(".") + VariableName;
			InOutParams.Name = VariableName;
			Warnings.Add(TEXT("legacy_identity_fields_used:blueprint_variable_node"));
			return true;
		}
		if (Obj->TryGetStringField(TEXT("node_class"), LegacyValue))
		{
			InOutParams.Kind = TEXT("blueprint_node");
			InOutParams.FullName = LegacyValue;
			Warnings.Add(TEXT("legacy_identity_fields_used:blueprint_node"));
			return true;
		}

		return bFound;
	}

	static bool ResolveJsonPointer(const TSharedPtr<FJsonObject>& RootObj, const FString& Pointer, TSharedPtr<FJsonObject>& OutObj)
	{
		if (!RootObj.IsValid())
		{
			return false;
		}
		if (Pointer.TrimStartAndEnd().IsEmpty() || Pointer.Equals(TEXT("/")))
		{
			OutObj = RootObj;
			return true;
		}
		if (!Pointer.StartsWith(TEXT("/")))
		{
			return false;
		}

		TSharedPtr<FJsonValue> CurrentValue = MakeShared<FJsonValueObject>(RootObj.ToSharedRef());
		TArray<FString> Segments;
		Pointer.ParseIntoArray(Segments, TEXT("/"), false);
		for (int32 SegmentIndex = 1; SegmentIndex < Segments.Num(); ++SegmentIndex)
		{
			FString Segment = Segments[SegmentIndex];
			Segment.ReplaceInline(TEXT("~1"), TEXT("/"));
			Segment.ReplaceInline(TEXT("~0"), TEXT("~"));

			if (!CurrentValue.IsValid())
			{
				return false;
			}
			if (CurrentValue->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> CurrentObj = CurrentValue->AsObject();
				CurrentValue = CurrentObj.IsValid() ? CurrentObj->TryGetField(Segment) : nullptr;
			}
			else if (CurrentValue->Type == EJson::Array)
			{
				int32 ArrayIndex = INDEX_NONE;
				if (!LexTryParseString(ArrayIndex, *Segment))
				{
					return false;
				}
				const TArray<TSharedPtr<FJsonValue>>& Array = CurrentValue->AsArray();
				CurrentValue = Array.IsValidIndex(ArrayIndex) ? Array[ArrayIndex] : nullptr;
			}
			else
			{
				return false;
			}
		}

		if (CurrentValue.IsValid() && CurrentValue->Type == EJson::Object)
		{
			OutObj = CurrentValue->AsObject();
			return OutObj.IsValid();
		}
		return false;
	}

	static bool ResolveIdentityFromJson(FResolveParams& InOutParams, TArray<FString>& Warnings, FString& OutError)
	{
		FString JsonPath = InOutParams.JsonFile;
		if (!InOutParams.FolderPath.IsEmpty() && FPaths::IsRelative(JsonPath))
		{
			JsonPath = FPaths::Combine(InOutParams.FolderPath, JsonPath);
		}
		if (JsonPath.IsEmpty())
		{
			OutError = TEXT("json_file is required for json_reference.");
			return false;
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *JsonPath))
		{
			OutError = FString::Printf(TEXT("json_file_not_readable:%s"), *JsonPath);
			return false;
		}

		TSharedPtr<FJsonObject> RootObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
		{
			OutError = FString::Printf(TEXT("json_file_parse_failed:%s"), *JsonPath);
			return false;
		}

		TSharedPtr<FJsonObject> TargetObj;
		if (!ResolveJsonPointer(RootObj, InOutParams.JsonPointer, TargetObj))
		{
			OutError = FString::Printf(TEXT("json_pointer_not_found:%s"), *InOutParams.JsonPointer);
			return false;
		}
		if (!ExtractIdentityFromObject(TargetObj, InOutParams, Warnings))
		{
			OutError = TEXT("json_reference_has_no_supported_identity");
			return false;
		}
		return true;
	}

	static void ParseCandidateIdFallback(FResolveParams& InOutParams)
	{
		if (InOutParams.CandidateId.IsEmpty() || !InOutParams.Kind.IsEmpty() || !InOutParams.FullName.IsEmpty())
		{
			return;
		}
		FString Kind;
		FString FullName;
		if (InOutParams.CandidateId.Split(TEXT(":"), &Kind, &FullName))
		{
			InOutParams.Kind = Kind;
			InOutParams.FullName = FullName;
		}
	}

	static bool ResolveCodeOrAsset(FResolveParams& Params, FResolveResult& OutResult)
	{
		OutResult.Kind = Params.Kind;
		OutResult.Name = Params.Name;
		OutResult.FullName = Params.FullName;
		if (OutResult.FullName.IsEmpty())
		{
			OutResult.Status = TEXT("missing_full_name");
			return false;
		}

		if (LooksLikeAssetPath(OutResult.FullName))
		{
			AddAssetOrigin(OutResult.FullName, OutResult);
			OutResult.bResolved = !OutResult.AssetPaths.IsEmpty() || !OutResult.PackagePaths.IsEmpty();
			OutResult.Status = OutResult.bResolved ? TEXT("resolved_asset") : TEXT("asset_path_unresolved");
			return OutResult.bResolved;
		}

		if (UFunction* Function = FindFunctionByFullName(OutResult.FullName))
		{
			OutResult.bResolved = AddFunctionSourcePath(Function, OutResult.CodePaths, OutResult.Warnings);
			OutResult.Status = OutResult.bResolved ? TEXT("resolved_native_function") : TEXT("native_function_unresolved");
			return OutResult.bResolved;
		}
		if (UClass* Class = LoadClassByPath(OutResult.FullName))
		{
			AddClassSourcePaths(Class, OutResult.CodePaths, TEXT("class_source"));
			OutResult.bResolved = !OutResult.CodePaths.IsEmpty();
			OutResult.Status = OutResult.bResolved ? TEXT("resolved_native_class") : TEXT("native_class_source_unresolved");
			return OutResult.bResolved;
		}
		if (const FProperty* Property = FindPropertyByFullName(OutResult.FullName))
		{
			UClass* OwnerClass = Property->GetOwnerClass();
			AddClassSourcePaths(OwnerClass, OutResult.CodePaths, TEXT("property_owner_class_source"));
			OutResult.bResolved = !OutResult.CodePaths.IsEmpty();
			OutResult.Status = OutResult.bResolved ? TEXT("resolved_native_property_owner") : TEXT("native_property_source_unresolved");
			return OutResult.bResolved;
		}

		if (OutResult.FullName.StartsWith(TEXT("/")))
		{
			AddAssetOrigin(OutResult.FullName, OutResult);
			OutResult.bResolved = !OutResult.AssetPaths.IsEmpty() || !OutResult.PackagePaths.IsEmpty();
			OutResult.Status = OutResult.bResolved ? TEXT("resolved_asset_like_path") : TEXT("unresolved_path");
			return OutResult.bResolved;
		}

		OutResult.Status = TEXT("unsupported_identity");
		return false;
	}

	static TSharedPtr<FJsonObject> CodePathToJson(const FCodePath& CodePath)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), CodePath.Path);
		Obj->SetNumberField(TEXT("line"), CodePath.Line);
		Obj->SetNumberField(TEXT("column"), CodePath.Column);
		Obj->SetStringField(TEXT("symbol"), CodePath.Symbol);
		Obj->SetStringField(TEXT("module"), CodePath.Module);
		Obj->SetStringField(TEXT("status"), CodePath.Status);
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}
}

bool ParseResolveParams(const TSharedPtr<FJsonObject>& Params, FResolveParams& OutParams, FString& OutError)
{
	if (!Params.IsValid())
	{
		OutError = TEXT("params object is required.");
		return false;
	}

	ReadStringField(Params, TEXT("source"), OutParams.TargetSource);
	ReadStringField(Params, TEXT("target_source"), OutParams.TargetSource);
	ReadStringField(Params, TEXT("kind"), OutParams.Kind);
	ReadStringField(Params, TEXT("name"), OutParams.Name);
	ReadStringField(Params, TEXT("full_name"), OutParams.FullName);
	ReadStringField(Params, TEXT("candidate_id"), OutParams.CandidateId);
	ReadStringField(Params, TEXT("folder_path"), OutParams.FolderPath);
	ReadStringField(Params, TEXT("json_file"), OutParams.JsonFile);
	ReadStringField(Params, TEXT("json_pointer"), OutParams.JsonPointer);
	ReadStringField(Params, TEXT("asset_path"), OutParams.AssetPath);
	ReadStringField(Params, TEXT("graph_name"), OutParams.GraphName);
	ReadStringField(Params, TEXT("node_guid"), OutParams.NodeGuid);

	const TSharedPtr<FJsonObject>* TargetObjPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("target"), TargetObjPtr) && TargetObjPtr && TargetObjPtr->IsValid())
	{
		FString TargetKind;
		ReadStringField(*TargetObjPtr, TEXT("source"), OutParams.TargetSource);
		ReadStringField(*TargetObjPtr, TEXT("target_source"), OutParams.TargetSource);
		ReadStringField(*TargetObjPtr, TEXT("kind"), TargetKind);
		if (IsReferenceSource(TargetKind))
		{
			OutParams.TargetSource = TargetKind;
		}
		else if (!TargetKind.IsEmpty())
		{
			OutParams.Kind = TargetKind;
		}

		TArray<FString> Warnings;
		ExtractIdentityFromObject(*TargetObjPtr, OutParams, Warnings);
		ReadStringField(*TargetObjPtr, TEXT("candidate_id"), OutParams.CandidateId);
		ReadStringField(*TargetObjPtr, TEXT("folder_path"), OutParams.FolderPath);
		ReadStringField(*TargetObjPtr, TEXT("json_file"), OutParams.JsonFile);
		ReadStringField(*TargetObjPtr, TEXT("json_pointer"), OutParams.JsonPointer);

		const TSharedPtr<FJsonObject>* CandidateObjPtr = nullptr;
		if ((*TargetObjPtr)->TryGetObjectField(TEXT("candidate"), CandidateObjPtr) && CandidateObjPtr && CandidateObjPtr->IsValid())
		{
			ExtractIdentityFromObject(*CandidateObjPtr, OutParams, Warnings);
		}
		const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
		if ((*TargetObjPtr)->TryGetObjectField(TEXT("node"), NodeObjPtr) && NodeObjPtr && NodeObjPtr->IsValid())
		{
			ExtractIdentityFromObject(*NodeObjPtr, OutParams, Warnings);
		}
	}

	ParseCandidateIdFallback(OutParams);
	if (OutParams.TargetSource.IsEmpty())
	{
		if (!OutParams.JsonFile.IsEmpty())
		{
			OutParams.TargetSource = TEXT("json_reference");
		}
		else if (!OutParams.CandidateId.IsEmpty())
		{
			OutParams.TargetSource = TEXT("catalog_candidate");
		}
		else
		{
			OutParams.TargetSource = TEXT("name_reference");
		}
	}
	OutParams.TargetSource = NormalizeToken(OutParams.TargetSource);

	return true;
}

bool ResolveOrigin(const FResolveParams& Params, FResolveResult& OutResult, FString& OutError)
{
	FResolveParams WorkingParams = Params;
	if (WorkingParams.TargetSource.Equals(TEXT("json_reference")))
	{
		if (!ResolveIdentityFromJson(WorkingParams, OutResult.Warnings, OutError))
		{
			return false;
		}
	}
	else if (WorkingParams.TargetSource.Equals(TEXT("catalog_candidate")))
	{
		ParseCandidateIdFallback(WorkingParams);
	}
	else if (!WorkingParams.TargetSource.Equals(TEXT("name_reference")) &&
		!WorkingParams.TargetSource.Equals(TEXT("ue_object")) &&
		!WorkingParams.TargetSource.Equals(TEXT("graph_node")))
	{
		OutError = FString::Printf(TEXT("unsupported target source '%s'."), *WorkingParams.TargetSource);
		return false;
	}

	if (!ResolveCodeOrAsset(WorkingParams, OutResult))
	{
		OutError = OutResult.Status.IsEmpty() ? TEXT("origin_unresolved") : OutResult.Status;
		return false;
	}
	return true;
}

TSharedRef<FJsonObject> BuildResolveResponse(const FResolveParams& Params, const FResolveResult& Result)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("uai.node_origin.resolve.v3"));
	Root->SetBoolField(TEXT("resolved"), Result.bResolved);
	Root->SetStringField(TEXT("status"), Result.Status);

	TSharedRef<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TargetObj->SetStringField(TEXT("source"), Params.TargetSource);
	TargetObj->SetStringField(TEXT("kind"), Result.Kind);
	TargetObj->SetStringField(TEXT("name"), Result.Name);
	TargetObj->SetStringField(TEXT("full_name"), Result.FullName);
	TargetObj->SetStringField(TEXT("candidate_id"), Params.CandidateId);
	Root->SetObjectField(TEXT("target"), TargetObj);

	TSharedRef<FJsonObject> OriginObj = MakeShared<FJsonObject>();
	OriginObj->SetArrayField(TEXT("asset_paths"), StringArrayToJson(Result.AssetPaths));
	OriginObj->SetArrayField(TEXT("package_paths"), StringArrayToJson(Result.PackagePaths));
	OriginObj->SetArrayField(TEXT("package_files"), StringArrayToJson(Result.PackageFiles));
	TArray<TSharedPtr<FJsonValue>> CodePathValues;
	for (const FCodePath& CodePath : Result.CodePaths)
	{
		CodePathValues.Add(MakeShared<FJsonValueObject>(CodePathToJson(CodePath)));
	}
	OriginObj->SetArrayField(TEXT("code_paths"), CodePathValues);
	Root->SetObjectField(TEXT("origin"), OriginObj);
	Root->SetArrayField(TEXT("warnings"), StringArrayToJson(Result.Warnings));
	return Root;
}
}
