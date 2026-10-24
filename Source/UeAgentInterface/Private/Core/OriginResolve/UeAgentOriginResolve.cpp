// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/OriginResolve/UeAgentOriginResolveTypes.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Contract/UeAgentContractJson.h"

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformStackWalk.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SourceCodeNavigation.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

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
	using UE::UAI::Contract::FDiagnostic;
	static FDiagnostic InputFailure(const TCHAR* Code, const TCHAR* Reason, const TCHAR* Field,
		TSharedPtr<FJsonValue> Expected = nullptr, TSharedPtr<FJsonValue> Actual = nullptr)
	{
		FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("origin_resolve"));
		Error.PropertyPath = FString(TEXT("target.")) + Field;
		Error.Source.JsonPath = FString(TEXT("/target/")) + Field; Error.SemanticPointer = Error.Source.JsonPath;
		Error.SourceKind = TEXT("command_input"); Error.ExpectedValue = MoveTemp(Expected); Error.ActualValue = MoveTemp(Actual);
		return Error;
	}
	static void BindJsonSource(FDiagnostic& Error, const FResolveParams& Params, const FString& PhysicalPath, bool bDocumentRoot = false)
	{
		if (Error.Source.SourceFile.IsEmpty()) Error.Source.SourceFile = Params.JsonFile;
		if (Error.Source.JsonPath.IsEmpty()) Error.Source.JsonPath = bDocumentRoot || Params.JsonPointer.IsEmpty() ? TEXT("/") : Params.JsonPointer;
		Error.InternalContext += Error.InternalContext.IsEmpty() ? PhysicalPath : TEXT("; source_file=") + PhysicalPath;
		for (auto& Child : Error.Diagnostics) BindJsonSource(Child, Params, PhysicalPath, bDocumentRoot);
		if (Error.PublicCause) { auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause); BindJsonSource(*Cause, Params, PhysicalPath, bDocumentRoot); Error.PublicCause = MoveTemp(Cause); }
	}
	static bool ResolutionFailure(const TCHAR* Reason, const FResolveParams& Params, const FResolveResult& Result, FDiagnostic& Error)
	{
		Error = InputFailure(*Result.Status, Reason, TEXT("full_name"), nullptr, MakeShared<FJsonValueString>(Params.FullName));
		Error.CandidateName = Params.Name; Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("resolvable object, asset or native symbol origin"));
		return false;
	}
	static FString NormalizeToken(const FString& Text)
	{
		return Text.TrimStartAndEnd().ToLower();
	}

	static bool ReadStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FString& InOutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		FString Value;
		if (Obj->TryGetStringField(FieldName, Value) && !Value.TrimStartAndEnd().IsEmpty())
		{
			InOutValue = Value.TrimStartAndEnd();
			return true;
		}
		return false;
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

	static bool AddFunctionSourcePath(UFunction* Function, TArray<FCodePath>& CodePaths, TArray<FString>& Warnings, FDiagnostic& Error)
	{
		const auto Reject = [&](const TCHAR* Code, const TCHAR* Reason) {
			Error = FDiagnostic::Failure(Code, Reason, TEXT("origin_resolve"));
			Error.SourceKind = TEXT("native_result");
			if (Function) Error.CandidateName = Function->GetName();
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("function definition or owning class source location"));
			Error.ActualValuePresent = false; return false;
		};
		if (!Function || !Function->GetOwnerClass())
		{
			return Reject(TEXT("native_function_owner_missing"), Function ? TEXT("The function has no owning class from which to resolve its module and source.") : TEXT("The native function object is unavailable."));
		}

		UClass* OwnerClass = Function->GetOwnerClass();
		FString ModuleName;
		if (!FSourceCodeNavigation::FindClassModuleName(OwnerClass, ModuleName))
		{
			Warnings.Add(TEXT("native_function_module_not_found"));
			AddClassSourcePaths(OwnerClass, CodePaths, TEXT("fallback_class_source"));
			if (!CodePaths.IsEmpty()) return true;
			return Reject(TEXT("native_function_module_not_found"), TEXT("The function's owning module could not be found, and the owning class has no discoverable source location."));
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
		if (!CodePaths.IsEmpty()) return true;
		Reject(TEXT("native_function_symbol_not_found"), TEXT("The function definition could not be located in its module, and the owning class has no discoverable source location."));
		Error.CandidateName = SymbolName; Error.InternalContext = ModuleName; return false;
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
			bFound = ReadStringField(Obj, TEXT("kind"), InOutParams.Kind) || bFound;
			bFound = ReadStringField(Obj, TEXT("name"), InOutParams.Name) || bFound;
			bFound = ReadStringField(Obj, TEXT("full_name"), InOutParams.FullName) || bFound;
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

	static bool ResolveJsonPointer(const TSharedPtr<FJsonObject>& RootObj, const FString& Pointer, TSharedPtr<FJsonObject>& OutObj, FDiagnostic& Error)
	{
		const auto Reject = [&](const TCHAR* Reason, const FString& Path, TSharedPtr<FJsonValue> Expected = nullptr, TSharedPtr<FJsonValue> Actual = nullptr) {
			Error = FDiagnostic::Failure(TEXT("json_pointer_not_found"), Reason, TEXT("origin_resolve"));
			Error.Source.JsonPath = Path; Error.SemanticPointer = Path;
			Error.ExpectedValue = MoveTemp(Expected); Error.ActualValue = MoveTemp(Actual); return false;
		};
		if (!RootObj.IsValid())
		{
			return Reject(TEXT("The reference document has no root object."), TEXT("/"), MakeShared<FJsonValueString>(TEXT("object")));
		}
		if (Pointer.TrimStartAndEnd().IsEmpty() || Pointer.Equals(TEXT("/")))
		{
			OutObj = RootObj;
			return true;
		}
		if (!Pointer.StartsWith(TEXT("/")))
		{
			Error = InputFailure(TEXT("json_pointer_not_found"), TEXT("The JSON reference pointer must start with '/' or be empty for the document root."), TEXT("json_pointer"),
				MakeShared<FJsonValueString>(TEXT("absolute JSON pointer")), MakeShared<FJsonValueString>(Pointer)); return false;
		}

		TSharedPtr<FJsonValue> CurrentValue = MakeShared<FJsonValueObject>(RootObj.ToSharedRef());
		TArray<FString> Segments;
		Pointer.ParseIntoArray(Segments, TEXT("/"), false);
		FString CurrentPointer;
		for (int32 SegmentIndex = 1; SegmentIndex < Segments.Num(); ++SegmentIndex)
		{
			FString Segment = Segments[SegmentIndex];
			Segment.ReplaceInline(TEXT("~1"), TEXT("/"));
			Segment.ReplaceInline(TEXT("~0"), TEXT("~"));

			if (!CurrentValue.IsValid())
			{
				Reject(TEXT("A JSON pointer segment does not exist in the reference document."), CurrentPointer);
				Error.ExpectedValuePresent = true; Error.ActualValuePresent = false; return false;
			}
			CurrentPointer += TEXT("/") + Segments[SegmentIndex];
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
					return Reject(TEXT("A JSON pointer segment addressing an array is not an integer index."), CurrentPointer,
						MakeShared<FJsonValueString>(TEXT("array index")), MakeShared<FJsonValueString>(Segment));
				}
				const TArray<TSharedPtr<FJsonValue>>& Array = CurrentValue->AsArray();
				if (!Array.IsValidIndex(ArrayIndex)) {
					return Reject(TEXT("The JSON pointer array index is outside the available elements."), CurrentPointer,
						MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Array.Num())), MakeShared<FJsonValueNumber>(ArrayIndex));
				}
				CurrentValue = Array[ArrayIndex];
			}
			else
			{
				return Reject(TEXT("The JSON pointer cannot traverse a scalar value."), CurrentPointer,
					MakeShared<FJsonValueString>(TEXT("object or array")), MakeShared<FJsonValueString>(CurrentValue->Type == EJson::String ? TEXT("string") : CurrentValue->Type == EJson::Number ? TEXT("number") : CurrentValue->Type == EJson::Boolean ? TEXT("boolean") : TEXT("null")));
			}
		}

		if (CurrentValue.IsValid() && CurrentValue->Type == EJson::Object)
		{
			OutObj = CurrentValue->AsObject();
			if (OutObj.IsValid()) return true;
		}
		Reject(CurrentValue.IsValid() ? TEXT("The JSON reference pointer must select an object containing an identity.") : TEXT("The JSON reference pointer selects a missing value."),
			Pointer, MakeShared<FJsonValueString>(TEXT("object")), CurrentValue.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(CurrentValue->Type == EJson::Array ? TEXT("array") : CurrentValue->Type == EJson::String ? TEXT("string") : CurrentValue->Type == EJson::Number ? TEXT("number") : CurrentValue->Type == EJson::Boolean ? TEXT("boolean") : TEXT("null"))) : nullptr);
		Error.ActualValuePresent = CurrentValue.IsValid(); return false;
	}

	static bool ResolveIdentityFromJson(FResolveParams& InOutParams, TArray<FString>& Warnings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FString JsonPath = InOutParams.JsonFile;
		if (!InOutParams.FolderPath.IsEmpty() && FPaths::IsRelative(JsonPath))
		{
			JsonPath = FPaths::Combine(InOutParams.FolderPath, JsonPath);
		}
		if (JsonPath.IsEmpty())
		{
			OutError = InputFailure(TEXT("origin_json_file_required"), TEXT("A JSON reference requires a non-empty json_file."), TEXT("json_file"),
				MakeShared<FJsonValueString>(TEXT("readable JSON file")), MakeShared<FJsonValueString>(InOutParams.JsonFile));
			return false;
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *JsonPath))
		{
			OutError = InputFailure(TEXT("json_file_not_readable"), TEXT("The referenced JSON file could not be opened or read."), TEXT("json_file"),
				MakeShared<FJsonValueString>(TEXT("readable JSON file")), MakeShared<FJsonValueString>(InOutParams.JsonFile));
			OutError.InternalContext = JsonPath;
			return false;
		}

		TSharedPtr<FJsonObject> RootObj;
		FDiagnostic ParseError;
		if (!UE::UAI::Contract::Json::TryParseObject(JsonText, RootObj, ParseError))
		{
			OutError = FDiagnostic::WrapFailure(TEXT("json_file_parse_failed"), ParseError, TEXT("origin_resolve"));
			BindJsonSource(OutError, InOutParams, JsonPath, true);
			return false;
		}

		TSharedPtr<FJsonObject> TargetObj;
		if (!ResolveJsonPointer(RootObj, InOutParams.JsonPointer, TargetObj, OutError))
		{
			if (OutError.SourceKind != TEXT("command_input")) BindJsonSource(OutError, InOutParams, JsonPath);
			return false;
		}
		if (!ExtractIdentityFromObject(TargetObj, InOutParams, Warnings))
		{
			OutError = FDiagnostic::Failure(TEXT("json_reference_has_no_supported_identity"), TEXT("The selected JSON object has no supported non-empty object, asset or native-symbol identity fields."), TEXT("origin_resolve"));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a supported identity containing full_name or an asset/class/function reference"));
			OutError.ActualValuePresent = false; BindJsonSource(OutError, InOutParams, JsonPath);
			return false;
		}
		return true;
	}

	static bool ResolveCatalogCandidateIdentity(FResolveParams& InOutParams, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (InOutParams.CandidateId.IsEmpty())
		{
			OutError = InputFailure(TEXT("target_query_invalid"), TEXT("Catalog candidate origin resolution requires a candidate reference."), TEXT("candidate_id"));
			OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return false;
		}

		UeAgentAuthoringIdentity::FCanonicalIdentity ParsedIdentity;
		UE::UAI::Contract::FDiagnostic IdentityError;
		if (!UeAgentAuthoringIdentity::ParseCanonicalKey(InOutParams.CandidateId, ParsedIdentity, IdentityError))
		{
			OutError = FDiagnostic::WrapFailure(TEXT("target_query_invalid"), IdentityError, TEXT("origin_resolve"));
			OutError.PropertyPath = TEXT("target.candidate_id"); OutError.Source.JsonPath = TEXT("/target/candidate_id");
			OutError.SemanticPointer = OutError.Source.JsonPath; OutError.SourceKind = TEXT("command_input");
			return false;
		}
		if (ParsedIdentity.ModelId != TEXT("catalog_candidate_identity")) {
			OutError = InputFailure(TEXT("target_query_invalid"), TEXT("The candidate reference belongs to a different identity model."), TEXT("candidate_id"),
				MakeShared<FJsonValueString>(TEXT("catalog candidate reference")));
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("catalog_candidate_identity"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(ParsedIdentity.ModelId); return false;
		}

		UeAgentPaletteSearch::FSearchParams SearchParams;
		SearchParams.CatalogType = ParsedIdentity.GetSelector(TEXT("catalog_type"));
		SearchParams.CandidateId = InOutParams.CandidateId;
		UeAgentPaletteSearch::FSearchResult SearchResult;
		if (!UeAgentPaletteSearch::ExecuteSearch(SearchParams, SearchResult, OutError)) return false;
		if (SearchResult.MatchingCandidates.Num() != 1)
		{
			if (SearchResult.bHasTargetResolution && SearchResult.TargetResolution.Diagnostic.IsValid()) OutError = SearchResult.TargetResolution.Diagnostic;
			else OutError = InputFailure(TEXT("target_not_found"), TEXT("The candidate reference must resolve to exactly one catalog entry."), TEXT("candidate_id"),
				MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(SearchResult.MatchingCandidates.Num()));
			return false;
		}

		const UeAgentPaletteSearch::FCandidate& Candidate = SearchResult.MatchingCandidates[0];
		if ((!InOutParams.Kind.IsEmpty() && !InOutParams.Kind.Equals(Candidate.Kind, ESearchCase::IgnoreCase))
			|| (!InOutParams.FullName.IsEmpty() && !InOutParams.FullName.Equals(Candidate.FullName, ESearchCase::IgnoreCase)))
		{
			const bool bKindMismatch = !InOutParams.Kind.IsEmpty() && !InOutParams.Kind.Equals(Candidate.Kind, ESearchCase::IgnoreCase);
			OutError = InputFailure(TEXT("target_identity_conflict"), bKindMismatch ? TEXT("The requested kind does not match the resolved catalog candidate kind.") : TEXT("The requested full name does not match the resolved catalog candidate."),
				bKindMismatch ? TEXT("kind") : TEXT("full_name"), MakeShared<FJsonValueString>(bKindMismatch ? Candidate.Kind : Candidate.FullName),
				MakeShared<FJsonValueString>(bKindMismatch ? InOutParams.Kind : InOutParams.FullName));
			OutError.CandidateName = Candidate.Name;
			return false;
		}
		InOutParams.Kind = Candidate.Kind;
		InOutParams.FullName = Candidate.FullName;
		if (InOutParams.Name.IsEmpty()) InOutParams.Name = Candidate.Name;
		OutError.Reset();
		return true;
	}

	static bool ResolveCodeOrAsset(FResolveParams& Params, FResolveResult& OutResult, FDiagnostic& OutError)
	{
		OutResult.Kind = Params.Kind;
		OutResult.Name = Params.Name;
		OutResult.FullName = Params.FullName;
		if (OutResult.FullName.IsEmpty())
		{
			OutResult.Status = TEXT("missing_full_name");
			return ResolutionFailure(TEXT("Origin resolution requires a non-empty full name identifying an object, asset or native symbol."), Params, OutResult, OutError);
		}

		if (Params.TargetSource == TEXT("ue_object"))
		{
			UObject* Object = FindObject<UObject>(nullptr, *OutResult.FullName);
			if (!Object && LooksLikeAssetPath(OutResult.FullName))
			{
				Object = LoadObject<UObject>(nullptr, *OutResult.FullName);
			}
			if (!Object)
			{
				OutResult.Status = TEXT("ue_object_not_found");
				return ResolutionFailure(TEXT("No loaded object or loadable asset matches the supplied full name."), Params, OutResult, OutError);
			}
			OutResult.FullName = Object->GetPathName();
			if (OutResult.Name.IsEmpty())
			{
				OutResult.Name = Object->GetName();
			}
			if (OutResult.Kind.IsEmpty())
			{
				OutResult.Kind = Object->GetClass()->GetName();
			}
			if (UFunction* Function = Cast<UFunction>(Object))
			{
				OutResult.bResolved = AddFunctionSourcePath(
					Function,
					OutResult.CodePaths,
					OutResult.Warnings, OutError);
			}
			else if (UClass* Class = Cast<UClass>(Object))
			{
				AddClassSourcePaths(
					Class,
					OutResult.CodePaths,
					TEXT("class_source"));
				OutResult.bResolved = !OutResult.CodePaths.IsEmpty();
			}
			else
			{
				AddAssetOrigin(OutResult.FullName, OutResult);
				OutResult.bResolved = !OutResult.AssetPaths.IsEmpty()
					|| !OutResult.PackagePaths.IsEmpty();
			}
			OutResult.Status = OutResult.bResolved
				? FString(TEXT("resolved_ue_object"))
				: FString(TEXT("ue_object_origin_unresolved"));
			if (!OutResult.bResolved && !OutError.IsEmpty()) { OutError = FDiagnostic::WrapFailure(OutResult.Status, OutError, TEXT("origin_resolve")); return false; }
			if (!OutResult.bResolved) return ResolutionFailure(Cast<UFunction>(Object) ? TEXT("The object exists, but its function has no discoverable source location.")
				: Cast<UClass>(Object) ? TEXT("The class exists, but no source header or implementation location could be discovered.")
				: TEXT("The object exists, but no asset or package origin could be derived."), Params, OutResult, OutError);
			return OutResult.bResolved;
		}

		if (LooksLikeAssetPath(OutResult.FullName))
		{
			AddAssetOrigin(OutResult.FullName, OutResult);
			OutResult.bResolved = !OutResult.AssetPaths.IsEmpty() || !OutResult.PackagePaths.IsEmpty();
			OutResult.Status = OutResult.bResolved ? TEXT("resolved_asset") : TEXT("asset_path_unresolved");
			if (!OutResult.bResolved) return ResolutionFailure(TEXT("The asset reference could not be converted to an asset or package origin."), Params, OutResult, OutError);
			return OutResult.bResolved;
		}

		if (UFunction* Function = FindFunctionByFullName(OutResult.FullName))
		{
			OutResult.bResolved = AddFunctionSourcePath(Function, OutResult.CodePaths, OutResult.Warnings, OutError);
			OutResult.Status = OutResult.bResolved ? TEXT("resolved_native_function") : TEXT("native_function_unresolved");
			if (!OutResult.bResolved) { OutError = FDiagnostic::WrapFailure(OutResult.Status, OutError, TEXT("origin_resolve")); return false; }
			return OutResult.bResolved;
		}
		if (UClass* Class = LoadClassByPath(OutResult.FullName))
		{
			AddClassSourcePaths(Class, OutResult.CodePaths, TEXT("class_source"));
			OutResult.bResolved = !OutResult.CodePaths.IsEmpty();
			OutResult.Status = OutResult.bResolved ? TEXT("resolved_native_class") : TEXT("native_class_source_unresolved");
			if (!OutResult.bResolved) return ResolutionFailure(TEXT("The native class exists, but no source header or implementation location could be discovered."), Params, OutResult, OutError);
			return OutResult.bResolved;
		}
		if (const FProperty* Property = FindPropertyByFullName(OutResult.FullName))
		{
			UClass* OwnerClass = Property->GetOwnerClass();
			AddClassSourcePaths(OwnerClass, OutResult.CodePaths, TEXT("property_owner_class_source"));
			OutResult.bResolved = !OutResult.CodePaths.IsEmpty();
			OutResult.Status = OutResult.bResolved ? TEXT("resolved_native_property_owner") : TEXT("native_property_source_unresolved");
			if (!OutResult.bResolved) return ResolutionFailure(OwnerClass ? TEXT("The property exists, but its owning class has no discoverable source location.")
				: TEXT("The property exists, but has no owning class from which to locate source."), Params, OutResult, OutError);
			return OutResult.bResolved;
		}

		if (OutResult.FullName.StartsWith(TEXT("/")))
		{
			AddAssetOrigin(OutResult.FullName, OutResult);
			OutResult.bResolved = !OutResult.AssetPaths.IsEmpty() || !OutResult.PackagePaths.IsEmpty();
			OutResult.Status = OutResult.bResolved ? TEXT("resolved_asset_like_path") : TEXT("unresolved_path");
			if (!OutResult.bResolved) return ResolutionFailure(TEXT("The path could not be converted to an asset or package origin."), Params, OutResult, OutError);
			return OutResult.bResolved;
		}

		OutResult.Status = TEXT("unsupported_identity");
		return ResolutionFailure(TEXT("The full name matches no supported asset, native function, class or property identity."), Params, OutResult, OutError);
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

bool ParseResolveParams(const TSharedPtr<FJsonObject>& Params, FResolveParams& OutParams, UE::UAI::Contract::FDiagnostic& OutError)
{
	if (!Params.IsValid())
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("origin_params_required"), TEXT("params object is required."), TEXT("origin_resolve"));
		OutError.SourceKind = TEXT("command_input"); OutError.Source.JsonPath = TEXT("/");
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object")); OutError.ActualValuePresent = false;
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

bool ResolveOrigin(const FResolveParams& Params, FResolveResult& OutResult, UE::UAI::Contract::FDiagnostic& OutError)
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
		if (!ResolveCatalogCandidateIdentity(WorkingParams, OutError)) return false;
	}
	else if (!WorkingParams.TargetSource.Equals(TEXT("name_reference")) &&
		!WorkingParams.TargetSource.Equals(TEXT("ue_object")) &&
		!WorkingParams.TargetSource.Equals(TEXT("graph_node")))
	{
		OutError = InputFailure(TEXT("origin_target_source_unsupported"), TEXT("The reference source kind is not supported."), TEXT("source"),
			MakeShared<FJsonValueString>(TEXT("json_reference, catalog_candidate, name_reference, ue_object or graph_node")), MakeShared<FJsonValueString>(WorkingParams.TargetSource));
		return false;
	}

	if (ResolveCodeOrAsset(WorkingParams, OutResult, OutError)) return true;
	if (WorkingParams.TargetSource == TEXT("json_reference")) {
		if (OutError.SourceKind == TEXT("command_input")) OutError.SourceKind.Reset();
		OutError.PropertyPath.Reset();
		OutError.Source.JsonPath = Params.JsonPointer.IsEmpty() ? TEXT("/") : Params.JsonPointer;
		OutError.SemanticPointer = OutError.Source.JsonPath;
		const FString PhysicalPath = !Params.FolderPath.IsEmpty() && FPaths::IsRelative(Params.JsonFile)
			? FPaths::Combine(Params.FolderPath, Params.JsonFile) : Params.JsonFile;
		BindJsonSource(OutError, Params, PhysicalPath);
	}
	return false;
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
