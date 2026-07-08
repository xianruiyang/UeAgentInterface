// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Assets.h"
#include "UeAgentHttpServer_AssetsOps.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Module/UeAgentInterfaceLogger.h"

#include "AssetToolsModule.h"
#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Animation/SkinWeightProfile.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AutomatedAssetImportData.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/TextureFactory.h"
#include "FileHelpers.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace UeAgentAssetOps
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
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
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

	bool DetachStaleObjectAtPath(const FString& ObjectPath, bool* bOutFoundStaleObject)
	{
		if (bOutFoundStaleObject)
		{
			*bOutFoundStaleObject = false;
		}
		if (ObjectPath.IsEmpty())
		{
			return false;
		}

		UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Existing)
		{
			return false;
		}
		if (bOutFoundStaleObject)
		{
			*bOutFoundStaleObject = true;
		}
		if (IsValid(Existing) && Existing->HasAnyFlags(RF_Public | RF_Standalone))
		{
			return false;
		}

		if (!IsValid(Existing))
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			return false;
		}

		Existing->ClearFlags(RF_Public | RF_Standalone);
		if (Existing->IsRooted())
		{
			Existing->RemoveFromRoot();
		}
		const ERenameFlags RenameFlags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty | REN_ForceNoResetLoaders;
		const FName TrashName = MakeUniqueObjectName(GetTransientPackage(), Existing->GetClass(), Existing->GetFName());
		const bool bRenamed = Existing->Rename(*TrashName.ToString(), GetTransientPackage(), RenameFlags);
		Existing->MarkAsGarbage();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		return bRenamed;
	}

	FString NormalizeReferenceInputToPackageName(const FString& InPath)
	{
		FString Path = InPath;
		Path.TrimStartAndEndInline();
		if (Path.IsEmpty())
		{
			return FString();
		}

		int32 FirstQuote = INDEX_NONE;
		int32 LastQuote = INDEX_NONE;
		if (Path.FindChar(TEXT('\''), FirstQuote) && Path.FindLastChar(TEXT('\''), LastQuote) && LastQuote > FirstQuote)
		{
			Path = Path.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
			Path.TrimStartAndEndInline();
		}

		if (Path.StartsWith(TEXT("\"")) && Path.EndsWith(TEXT("\"")) && Path.Len() >= 2)
		{
			Path = Path.Mid(1, Path.Len() - 2);
			Path.TrimStartAndEndInline();
		}

		return NormalizeAssetPath(Path);
	}

	static FString DependencyCategoryToString(UE::AssetRegistry::EDependencyCategory Category)
	{
		using UE::AssetRegistry::EDependencyCategory;
		if (EnumHasAnyFlags(Category, EDependencyCategory::Package))
		{
			return TEXT("package");
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::Manage))
		{
			return TEXT("manage");
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::SearchableName))
		{
			return TEXT("searchable_name");
		}
		return TEXT("none");
	}

	static TArray<TSharedPtr<FJsonValue>> DependencyPropertyNames(UE::AssetRegistry::EDependencyCategory Category, UE::AssetRegistry::EDependencyProperty Properties)
	{
		using UE::AssetRegistry::EDependencyCategory;
		using UE::AssetRegistry::EDependencyProperty;

		TArray<TSharedPtr<FJsonValue>> Names;
		if (EnumHasAnyFlags(Category, EDependencyCategory::Package))
		{
			Names.Add(MakeShared<FJsonValueString>(EnumHasAnyFlags(Properties, EDependencyProperty::Hard) ? TEXT("hard") : TEXT("soft")));
			if (EnumHasAnyFlags(Properties, EDependencyProperty::Game))
			{
				Names.Add(MakeShared<FJsonValueString>(TEXT("game")));
			}
			else
			{
				Names.Add(MakeShared<FJsonValueString>(TEXT("editor_only")));
			}
			if (EnumHasAnyFlags(Properties, EDependencyProperty::Build))
			{
				Names.Add(MakeShared<FJsonValueString>(TEXT("build")));
			}
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::Manage))
		{
			Names.Add(MakeShared<FJsonValueString>(EnumHasAnyFlags(Properties, EDependencyProperty::Direct) ? TEXT("direct") : TEXT("indirect")));
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::SearchableName))
		{
			Names.Add(MakeShared<FJsonValueString>(TEXT("searchable_name")));
		}
		return Names;
	}

	static FString DependencyRelationKind(UE::AssetRegistry::EDependencyCategory Category, UE::AssetRegistry::EDependencyProperty Properties)
	{
		using UE::AssetRegistry::EDependencyCategory;
		using UE::AssetRegistry::EDependencyProperty;
		if (EnumHasAnyFlags(Category, EDependencyCategory::Package))
		{
			return EnumHasAnyFlags(Properties, EDependencyProperty::Hard) ? TEXT("hard_package") : TEXT("soft_package");
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::Manage))
		{
			return EnumHasAnyFlags(Properties, EDependencyProperty::Direct) ? TEXT("direct_manage") : TEXT("indirect_manage");
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::SearchableName))
		{
			return TEXT("searchable_name");
		}
		return TEXT("unknown");
	}

	bool ShouldIncludeDependencyEdge(
		UE::AssetRegistry::EDependencyCategory Category,
		UE::AssetRegistry::EDependencyProperty Properties,
		bool bIncludeHard,
		bool bIncludeSoft,
		bool bIncludeManage,
		bool bIncludeSearchableNames)
	{
		using UE::AssetRegistry::EDependencyCategory;
		using UE::AssetRegistry::EDependencyProperty;
		if (EnumHasAnyFlags(Category, EDependencyCategory::Package))
		{
			const bool bHard = EnumHasAnyFlags(Properties, EDependencyProperty::Hard);
			return bHard ? bIncludeHard : bIncludeSoft;
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::Manage))
		{
			return bIncludeManage;
		}
		if (EnumHasAnyFlags(Category, EDependencyCategory::SearchableName))
		{
			return bIncludeSearchableNames;
		}
		return false;
	}

	static FString AssetIdentifierPackageName(const FAssetIdentifier& Identifier)
	{
		return Identifier.PackageName.IsNone() ? FString() : Identifier.PackageName.ToString();
	}

	FString AssetDataClassPathString(const FAssetData& AssetData)
	{
		return AssetData.AssetClassPath.IsValid() ? AssetData.AssetClassPath.ToString() : FString();
	}

	static TSharedPtr<FJsonObject> MakeAssetRegistryNodeJson(IAssetRegistry& AssetRegistry, const FAssetIdentifier& Identifier, const int32 Depth, const FString& Role)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		const FString IdentifierString = Identifier.ToString();
		const FString PackageName = AssetIdentifierPackageName(Identifier);
		Node->SetStringField(TEXT("id"), IdentifierString);
		Node->SetStringField(TEXT("identifier"), IdentifierString);
		Node->SetStringField(TEXT("package_name"), PackageName);
		Node->SetStringField(TEXT("primary_asset_id"), Identifier.GetPrimaryAssetId().ToString());
		Node->SetStringField(TEXT("object_name"), Identifier.ObjectName.ToString());
		Node->SetStringField(TEXT("value_name"), Identifier.ValueName.ToString());
		Node->SetBoolField(TEXT("is_package_identifier"), Identifier.IsPackage());
		Node->SetBoolField(TEXT("is_object_identifier"), Identifier.IsObject());
		Node->SetBoolField(TEXT("is_value_identifier"), Identifier.IsValue());
		Node->SetNumberField(TEXT("depth"), Depth);
		Node->SetStringField(TEXT("role"), Role);

		TArray<FAssetData> Assets;
		if (!PackageName.IsEmpty())
		{
			AssetRegistry.GetAssetsByPackageName(FName(*PackageName), Assets, false);
		}

		Node->SetBoolField(TEXT("asset_found"), Assets.Num() > 0);
		Node->SetNumberField(TEXT("asset_data_count"), Assets.Num());
		TArray<TSharedPtr<FJsonValue>> AssetDataJson;
		for (const FAssetData& AssetData : Assets)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
			Item->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
			Item->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
			Item->SetStringField(TEXT("object_path"), AssetData.GetSoftObjectPath().ToString());
			Item->SetStringField(TEXT("asset_class"), AssetDataClassPathString(AssetData));
			Item->SetBoolField(TEXT("is_redirector"), AssetDataClassPathString(AssetData).EndsWith(TEXT("ObjectRedirector")));
			AssetDataJson.Add(MakeShared<FJsonValueObject>(Item));
		}
		Node->SetArrayField(TEXT("asset_data"), AssetDataJson);
		if (Assets.Num() > 0)
		{
			const FAssetData& PrimaryAssetData = Assets[0];
			Node->SetStringField(TEXT("asset_name"), PrimaryAssetData.AssetName.ToString());
			Node->SetStringField(TEXT("object_path"), PrimaryAssetData.GetSoftObjectPath().ToString());
			Node->SetStringField(TEXT("asset_class"), AssetDataClassPathString(PrimaryAssetData));
			Node->SetBoolField(TEXT("is_redirector"), AssetDataClassPathString(PrimaryAssetData).EndsWith(TEXT("ObjectRedirector")));
		}
		else
		{
			Node->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(PackageName));
			Node->SetStringField(TEXT("object_path"), PackageName.IsEmpty() ? FString() : ToObjectPath(PackageName));
			Node->SetStringField(TEXT("asset_class"), FString());
			Node->SetBoolField(TEXT("is_redirector"), false);
		}
		return Node;
	}

	TSharedPtr<FJsonObject> MakeAssetRegistryEdgeJson(
		const FAssetIdentifier& Source,
		const FAssetIdentifier& Target,
		const FString& QueryDirection,
		const int32 Depth,
		UE::AssetRegistry::EDependencyCategory Category,
		UE::AssetRegistry::EDependencyProperty Properties)
	{
		using UE::AssetRegistry::EDependencyCategory;
		using UE::AssetRegistry::EDependencyProperty;

		TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
		Edge->SetStringField(TEXT("source"), Source.ToString());
		Edge->SetStringField(TEXT("source_package"), AssetIdentifierPackageName(Source));
		Edge->SetStringField(TEXT("target"), Target.ToString());
		Edge->SetStringField(TEXT("target_package"), AssetIdentifierPackageName(Target));
		Edge->SetStringField(TEXT("query_direction"), QueryDirection);
		Edge->SetNumberField(TEXT("depth"), Depth);
		Edge->SetNumberField(TEXT("category_flags"), static_cast<int64>(Category));
		Edge->SetNumberField(TEXT("property_flags"), static_cast<int64>(Properties));
		Edge->SetStringField(TEXT("category"), DependencyCategoryToString(Category));
		Edge->SetStringField(TEXT("relation_kind"), DependencyRelationKind(Category, Properties));
		Edge->SetArrayField(TEXT("properties"), DependencyPropertyNames(Category, Properties));
		Edge->SetBoolField(TEXT("hard"), EnumHasAnyFlags(Category, EDependencyCategory::Package) && EnumHasAnyFlags(Properties, EDependencyProperty::Hard));
		Edge->SetBoolField(TEXT("soft"), EnumHasAnyFlags(Category, EDependencyCategory::Package) && !EnumHasAnyFlags(Properties, EDependencyProperty::Hard));
		Edge->SetBoolField(TEXT("game"), EnumHasAnyFlags(Category, EDependencyCategory::Package) && EnumHasAnyFlags(Properties, EDependencyProperty::Game));
		Edge->SetBoolField(TEXT("build"), EnumHasAnyFlags(Category, EDependencyCategory::Package) && EnumHasAnyFlags(Properties, EDependencyProperty::Build));
		Edge->SetBoolField(TEXT("direct_manage"), EnumHasAnyFlags(Category, EDependencyCategory::Manage) && EnumHasAnyFlags(Properties, EDependencyProperty::Direct));
		Edge->SetBoolField(TEXT("indirect_manage"), EnumHasAnyFlags(Category, EDependencyCategory::Manage) && !EnumHasAnyFlags(Properties, EDependencyProperty::Direct));
		return Edge;
	}

	static void AddOrUpdateGraphNode(
		IAssetRegistry& AssetRegistry,
		TMap<FString, TSharedPtr<FJsonObject>>& NodesById,
		const FAssetIdentifier& Identifier,
		const int32 Depth,
		const FString& Role)
	{
		const FString Id = Identifier.ToString();
		if (TSharedPtr<FJsonObject>* Existing = NodesById.Find(Id))
		{
			const int32 ExistingDepth = (*Existing)->HasTypedField<EJson::Number>(TEXT("depth")) ? static_cast<int32>((*Existing)->GetIntegerField(TEXT("depth"))) : Depth;
			if (Depth < ExistingDepth)
			{
				(*Existing)->SetNumberField(TEXT("depth"), Depth);
			}
			if (!Role.IsEmpty())
			{
				const FString ExistingRole = (*Existing)->GetStringField(TEXT("role"));
				if (!ExistingRole.Contains(Role))
				{
					(*Existing)->SetStringField(TEXT("role"), ExistingRole.IsEmpty() ? Role : ExistingRole + TEXT(",") + Role);
				}
			}
			return;
		}
		NodesById.Add(Id, MakeAssetRegistryNodeJson(AssetRegistry, Identifier, Depth, Role));
	}

	static TArray<TSharedPtr<FJsonValue>> SortedNodeValues(const TMap<FString, TSharedPtr<FJsonObject>>& NodesById)
	{
		TArray<FString> Keys;
		NodesById.GetKeys(Keys);
		Keys.Sort();
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Key : Keys)
		{
			if (const TSharedPtr<FJsonObject>* Node = NodesById.Find(Key))
			{
				Values.Add(MakeShared<FJsonValueObject>(*Node));
			}
		}
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> CollectPackagePathStringHits(
		const FString& PackageName,
		const int32 MaxHits,
		bool& bOutTruncated)
	{
		bOutTruncated = false;

		TArray<FString> PackageFiles;
		IFileManager::Get().FindFilesRecursive(PackageFiles, *FPaths::ProjectContentDir(), TEXT("*.uasset"), true, false);
		IFileManager::Get().FindFilesRecursive(PackageFiles, *FPaths::ProjectContentDir(), TEXT("*.umap"), true, false);
		PackageFiles.Sort();

		TArray<TSharedPtr<FJsonValue>> HitsJson;
		const FTCHARToUTF8 SearchBytes(*PackageName);
		const TArrayView<const uint8> SearchView(reinterpret_cast<const uint8*>(SearchBytes.Get()), SearchBytes.Length());
		if (SearchView.Num() <= 0)
		{
			return HitsJson;
		}

		for (const FString& Filename : PackageFiles)
		{
			if (HitsJson.Num() >= MaxHits)
			{
				bOutTruncated = true;
				break;
			}

			TArray<uint8> FileBytes;
			if (!FFileHelper::LoadFileToArray(FileBytes, *Filename) || FileBytes.Num() < SearchView.Num())
			{
				continue;
			}

			bool bFound = false;
			for (int32 Index = 0; Index <= FileBytes.Num() - SearchView.Num(); ++Index)
			{
				if (FMemory::Memcmp(FileBytes.GetData() + Index, SearchView.GetData(), SearchView.Num()) == 0)
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				continue;
			}

			FString HitPackageName;
			FPackageName::TryConvertFilenameToLongPackageName(Filename, HitPackageName);
			TSharedPtr<FJsonObject> Hit = MakeShared<FJsonObject>();
			Hit->SetStringField(TEXT("package_name"), HitPackageName);
			Hit->SetStringField(TEXT("filename"), Filename);
			Hit->SetBoolField(TEXT("self_hit"), HitPackageName == PackageName);
			HitsJson.Add(MakeShared<FJsonValueObject>(Hit));
		}

		return HitsJson;
	}

	bool ReadStringArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues)
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
		TSharedPtr<FJsonObject> SaveRoot = UeAgentDetailsAuthoring::CloneObjectTree(Root);
		if (!UeAgentDetailsAuthoring::ExternalizeInlineDetailsInObjectTree(OutputPath, SaveRoot, TEXT("asset_json_export"), OutError))
		{
			return false;
		}
		if (!UeAgentJsonDiagnostics::SerializeJsonObjectCompact(SaveRoot, JsonText))
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
		return UeAgentDetailsAuthoring::RehydrateGroupedDetailsInObjectTree(InputPath, OutRoot, OutError);
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
			if (!Property)
			{
				continue;
			}

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

			if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				CurrentStruct = StructProperty->Struct;
				CurrentContainer = ValuePtr;
				continue;
			}

			if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				UObject* NestedObject = ObjectProperty->GetObjectPropertyValue(ValuePtr);
				if (!NestedObject)
				{
					return false;
				}

				CurrentStruct = NestedObject->GetClass();
				CurrentContainer = NestedObject;
				continue;
			}

			return false;
		}

		return false;
	}

	static bool GetObjectPropertyText(UObject* Target, const FString& PropertyPath, FString& OutValueText, FString& OutCppType)
	{
		OutValueText.Reset();
		OutCppType.Reset();

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			return false;
		}

		OutCppType = Property->GetCPPType();
		Property->ExportTextItem_Direct(OutValueText, ValuePtr, nullptr, Target, PPF_None);
		return true;
	}

	static bool SetObjectPropertyText(UObject* Target, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText = nullptr, FString* OutCppType = nullptr, FString* OutImportError = nullptr)
	{
		if (OutImportError)
		{
			OutImportError->Reset();
		}

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			if (OutImportError)
			{
				*OutImportError = TEXT("property_not_found");
			}
			return false;
		}

		if (OutCppType)
		{
			*OutCppType = Property->GetCPPType();
		}
		if (Property->ImportText_Direct(*ValueText, ValuePtr, Target, PPF_None, GLog) == nullptr)
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

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset || !Asset->GetOutermost())
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

	static void GetDefaultAssetPropertyPaths(UObject* Asset, TArray<FString>& OutPropertyPaths, FString& OutPresetName)
	{
		OutPropertyPaths.Reset();
		OutPresetName.Reset();
		if (!Asset)
		{
			return;
		}

		if (Asset->IsA<UAnimSequence>())
		{
			OutPresetName = TEXT("anim_sequence_defaults");
			OutPropertyPaths = {
				TEXT("RateScale"),
				TEXT("bEnableRootMotion"),
				TEXT("bForceRootLock"),
				TEXT("RootMotionRootLock"),
				TEXT("Interpolation")
			};
			return;
		}

		if (Asset->IsA<UTexture>())
		{
			OutPresetName = TEXT("texture_defaults");
			OutPropertyPaths = {
				TEXT("SRGB"),
				TEXT("CompressionSettings"),
				TEXT("MipGenSettings"),
				TEXT("LODGroup"),
				TEXT("NeverStream"),
				TEXT("VirtualTextureStreaming"),
				TEXT("Filter")
			};
			return;
		}

		if (Asset->IsA<UStaticMesh>())
		{
			OutPresetName = TEXT("static_mesh_defaults");
			OutPropertyPaths = {
				TEXT("LightMapResolution"),
				TEXT("LightMapCoordinateIndex"),
				TEXT("LODGroup"),
				TEXT("bAllowCPUAccess")
			};
			return;
		}

		if (Asset->IsA<USkeletalMesh>())
		{
			OutPresetName = TEXT("skeletal_mesh_defaults");
			OutPropertyPaths = {
				TEXT("bEnablePerPolyCollision"),
				TEXT("LODSettings"),
				TEXT("DefaultAnimatingRig")
			};
			return;
		}
	}

}

bool FUeAgentHttpServer::CmdAssetMove(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceAssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_asset_path"), SourceAssetPath) || SourceAssetPath.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), SourceAssetPath);
	}
	if (SourceAssetPath.IsEmpty())
	{
		OutError = TEXT("missing_source_asset_path");
		return false;
	}

	UObject* SourceAsset = UeAgentAssetOps::LoadAssetObject(SourceAssetPath);
	if (!SourceAsset)
	{
		OutError = TEXT("source_asset_not_found");
		return false;
	}

	const FString SourcePackageName = UeAgentAssetOps::NormalizeAssetPath(SourceAsset->GetOutermost()->GetName());
	const FString SourceAssetName = SourceAsset->GetName();

	FString DestinationAssetPath;
	JsonTryGetString(Ctx.Params, TEXT("destination_asset_path"), DestinationAssetPath);
	DestinationAssetPath.TrimStartAndEndInline();

	FString DestinationFolder;
	JsonTryGetString(Ctx.Params, TEXT("destination_folder"), DestinationFolder);
	DestinationFolder.TrimStartAndEndInline();

	FString DestinationName;
	JsonTryGetString(Ctx.Params, TEXT("destination_name"), DestinationName);
	DestinationName.TrimStartAndEndInline();

	if (DestinationAssetPath.IsEmpty())
	{
		if (DestinationFolder.IsEmpty())
		{
			OutError = TEXT("missing_destination_asset_path_or_folder");
			return false;
		}
		FString NormalizedFolder = UeAgentAssetOps::NormalizeAssetPath(DestinationFolder);
		if (NormalizedFolder.EndsWith(TEXT("/")))
		{
			NormalizedFolder.LeftChopInline(1);
		}
		DestinationAssetPath = NormalizedFolder / (DestinationName.IsEmpty() ? SourceAssetName : DestinationName);
	}

	const FString NormalizedDestinationAssetPath = UeAgentAssetOps::NormalizeAssetPath(DestinationAssetPath);
	if (!FPackageName::IsValidLongPackageName(NormalizedDestinationAssetPath))
	{
		OutError = TEXT("invalid_destination_asset_path");
		return false;
	}

	const FString DestinationAssetName = FPackageName::GetLongPackageAssetName(NormalizedDestinationAssetPath);
	const FString DestinationPackagePath = FPackageName::GetLongPackagePath(NormalizedDestinationAssetPath);
	if (DestinationAssetName.IsEmpty() || DestinationPackagePath.IsEmpty())
	{
		OutError = TEXT("invalid_destination_asset_path");
		return false;
	}

	if (SourcePackageName.Equals(NormalizedDestinationAssetPath, ESearchCase::IgnoreCase))
	{
		OutData->SetStringField(TEXT("source_asset_path"), SourcePackageName);
		OutData->SetStringField(TEXT("asset_path"), NormalizedDestinationAssetPath);
		OutData->SetStringField(TEXT("object_path"), SourceAsset->GetPathName());
		OutData->SetBoolField(TEXT("changed"), false);
		OutData->SetBoolField(TEXT("saved"), false);
		return true;
	}

	bool bOverwriteExisting = false;
	JsonTryGetBool(Ctx.Params, TEXT("overwrite_existing"), bOverwriteExisting);
	if (!bOverwriteExisting && UeAgentAssetOps::LoadAssetObject(NormalizedDestinationAssetPath))
	{
		OutError = TEXT("destination_asset_already_exists");
		return false;
	}

	bool bDryRun = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	if (bDryRun)
	{
		OutData->SetStringField(TEXT("source_asset_path"), SourcePackageName);
		OutData->SetStringField(TEXT("asset_path"), NormalizedDestinationAssetPath);
		OutData->SetStringField(TEXT("destination_folder"), DestinationPackagePath);
		OutData->SetStringField(TEXT("destination_name"), DestinationAssetName);
		OutData->SetBoolField(TEXT("changed"), false);
		OutData->SetBoolField(TEXT("dry_run"), true);
		return true;
	}

	if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
	{
		if (AssetEditorSubsystem->FindEditorForAsset(SourceAsset, false))
		{
			AssetEditorSubsystem->CloseAllEditorsForAsset(SourceAsset);
		}
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<FAssetRenameData> RenameData;
	RenameData.Emplace(SourceAsset, DestinationPackagePath, DestinationAssetName);
	{
		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
		if (!AssetToolsModule.Get().RenameAssets(RenameData))
		{
			OutError = TEXT("rename_asset_failed");
			return false;
		}
	}

	UObject* MovedAsset = UeAgentAssetOps::LoadAssetObject(NormalizedDestinationAssetPath);
	if (!MovedAsset)
	{
		OutError = TEXT("moved_asset_not_found_after_rename");
		return false;
	}

	bool bSaveAfterMove = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_move"), bSaveAfterMove);
	if (bSaveAfterMove && !UeAgentAssetOps::SaveAssetPackage(MovedAsset, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("source_asset_path"), SourcePackageName);
	OutData->SetStringField(TEXT("asset_path"), NormalizedDestinationAssetPath);
	OutData->SetStringField(TEXT("object_path"), MovedAsset->GetPathName());
	OutData->SetStringField(TEXT("destination_folder"), DestinationPackagePath);
	OutData->SetStringField(TEXT("destination_name"), DestinationAssetName);
	OutData->SetStringField(TEXT("class"), MovedAsset->GetClass()->GetPathName());
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterMove);
	return true;
}

bool FUeAgentHttpServer::CmdAssetFixupRedirectors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TArray<FString> RequestedAssetPaths;
	auto AddStringField = [&Ctx, &RequestedAssetPaths](const TCHAR* Key)
	{
		FString Value;
		if (JsonTryGetString(Ctx.Params, Key, Value))
		{
			Value.TrimStartAndEndInline();
			if (!Value.IsEmpty())
			{
				RequestedAssetPaths.Add(Value);
			}
		}
	};
	auto AddStringArrayField = [&Ctx, &RequestedAssetPaths](const TCHAR* Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(Key, Values) || !Values)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			FString Text;
			if (Value->TryGetString(Text))
			{
				Text.TrimStartAndEndInline();
				if (!Text.IsEmpty())
				{
					RequestedAssetPaths.Add(Text);
				}
			}
		}
	};
	AddStringField(TEXT("asset_path"));
	AddStringField(TEXT("redirector_path"));
	AddStringArrayField(TEXT("asset_paths"));
	AddStringArrayField(TEXT("redirector_paths"));

	TArray<FString> RequestedFolderPaths;
	auto AddFolderField = [&Ctx, &RequestedFolderPaths](const TCHAR* Key)
	{
		FString Value;
		if (JsonTryGetString(Ctx.Params, Key, Value))
		{
			Value.TrimStartAndEndInline();
			if (!Value.IsEmpty())
			{
				RequestedFolderPaths.Add(Value);
			}
		}
	};
	auto AddFolderArrayField = [&Ctx, &RequestedFolderPaths](const TCHAR* Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(Key, Values) || !Values)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			FString Text;
			if (Value->TryGetString(Text))
			{
				Text.TrimStartAndEndInline();
				if (!Text.IsEmpty())
				{
					RequestedFolderPaths.Add(Text);
				}
			}
		}
	};
	AddFolderField(TEXT("folder_path"));
	AddFolderField(TEXT("redirector_folder"));
	AddFolderArrayField(TEXT("folder_paths"));
	AddFolderArrayField(TEXT("redirector_folders"));

	if (RequestedAssetPaths.IsEmpty() && RequestedFolderPaths.IsEmpty())
	{
		OutError = TEXT("missing_asset_path_or_folder_path");
		return false;
	}

	bool bRecursive = true;
	JsonTryGetBool(Ctx.Params, TEXT("recursive"), bRecursive);

	TMap<FString, UObjectRedirector*> RedirectorByPackage;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	auto AddSkipped = [&Skipped](const FString& Path, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), Path);
		Entry->SetStringField(TEXT("reason"), Reason);
		Skipped.Add(MakeShared<FJsonValueObject>(Entry));
	};

	for (const FString& RequestedPath : RequestedAssetPaths)
	{
		const FString PackageName = UeAgentAssetOps::NormalizeAssetPath(RequestedPath);
		UObject* LoadedObject = UeAgentAssetOps::LoadAssetObject(PackageName);
		UObjectRedirector* Redirector = Cast<UObjectRedirector>(LoadedObject);
		if (!Redirector)
		{
			AddSkipped(PackageName, LoadedObject ? TEXT("not_redirector") : TEXT("asset_not_found"));
			continue;
		}
		RedirectorByPackage.Add(PackageName, Redirector);
	}

	if (!RequestedFolderPaths.IsEmpty())
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		for (FString FolderPath : RequestedFolderPaths)
		{
			FolderPath.TrimStartAndEndInline();
			FolderPath.RemoveFromEnd(TEXT("/"));
			if (FolderPath.IsEmpty())
			{
				continue;
			}

			FARFilter Filter;
			Filter.PackagePaths.Add(*FolderPath);
			Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = bRecursive;

			TArray<FAssetData> RedirectorAssets;
			AssetRegistry.GetAssets(Filter, RedirectorAssets);
			for (const FAssetData& RedirectorAsset : RedirectorAssets)
			{
				UObjectRedirector* Redirector = Cast<UObjectRedirector>(RedirectorAsset.GetAsset());
				if (Redirector)
				{
					RedirectorByPackage.Add(RedirectorAsset.PackageName.ToString(), Redirector);
				}
			}
		}
	}

	TArray<UObjectRedirector*> Redirectors;
	RedirectorByPackage.GenerateValueArray(Redirectors);

	TArray<TSharedPtr<FJsonValue>> RedirectorReports;
	for (const TPair<FString, UObjectRedirector*>& Pair : RedirectorByPackage)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Pair.Key);
		Entry->SetStringField(TEXT("object_path"), Pair.Value ? Pair.Value->GetPathName() : FString());
		if (Pair.Value && Pair.Value->DestinationObject)
		{
			Entry->SetStringField(TEXT("destination_object_path"), Pair.Value->DestinationObject->GetPathName());
			Entry->SetStringField(TEXT("destination_asset_path"), UeAgentAssetOps::NormalizeAssetPath(Pair.Value->DestinationObject->GetOutermost()->GetName()));
		}
		RedirectorReports.Add(MakeShared<FJsonValueObject>(Entry));
	}

	bool bDryRun = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<TSharedPtr<FJsonValue>> Blocked;
	TArray<UObject*> RedirectorsToDelete;
	for (UObjectRedirector* Redirector : Redirectors)
	{
		if (!Redirector)
		{
			continue;
		}

		const FString PackageName = UeAgentAssetOps::NormalizeAssetPath(Redirector->GetOutermost()->GetName());
		TArray<FName> ReferencerNames;
		AssetRegistry.GetReferencers(*PackageName, ReferencerNames);
		if (!ReferencerNames.IsEmpty())
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), PackageName);
			TArray<TSharedPtr<FJsonValue>> ReferencerValues;
			for (const FName& ReferencerName : ReferencerNames)
			{
				ReferencerValues.Add(MakeShared<FJsonValueString>(ReferencerName.ToString()));
			}
			Entry->SetArrayField(TEXT("referencers"), ReferencerValues);
			Entry->SetNumberField(TEXT("referencer_count"), ReferencerNames.Num());
			Entry->SetStringField(TEXT("reason"), TEXT("redirector_still_referenced"));
			Blocked.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		RedirectorsToDelete.Add(Redirector);
	}

	bool bFailOnBlocked = true;
	JsonTryGetBool(Ctx.Params, TEXT("fail_on_blocked"), bFailOnBlocked);
	if (!Blocked.IsEmpty() && bFailOnBlocked)
	{
		OutData->SetNumberField(TEXT("redirector_count"), Redirectors.Num());
		OutData->SetNumberField(TEXT("deletable_redirector_count"), RedirectorsToDelete.Num());
		OutData->SetNumberField(TEXT("blocked_redirector_count"), Blocked.Num());
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetArrayField(TEXT("redirectors"), RedirectorReports);
		OutData->SetArrayField(TEXT("blocked"), Blocked);
		OutData->SetArrayField(TEXT("skipped"), Skipped);
		OutError = TEXT("redirector_still_referenced");
		return false;
	}

	int32 DeletedCount = 0;
	if (!bDryRun && !RedirectorsToDelete.IsEmpty())
	{
		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
		DeletedCount = ObjectTools::DeleteObjectsUnchecked(RedirectorsToDelete);
	}

	int32 RemainingPackageCount = 0;
	TArray<TSharedPtr<FJsonValue>> RemainingPackages;
	for (const TPair<FString, UObjectRedirector*>& Pair : RedirectorByPackage)
	{
		FString ExistingPackageFilename;
		if (FPackageName::DoesPackageExist(Pair.Key, &ExistingPackageFilename))
		{
			++RemainingPackageCount;
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), Pair.Key);
			Entry->SetStringField(TEXT("filename"), ExistingPackageFilename);
			RemainingPackages.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	OutData->SetNumberField(TEXT("redirector_count"), Redirectors.Num());
	OutData->SetNumberField(TEXT("deletable_redirector_count"), RedirectorsToDelete.Num());
	OutData->SetNumberField(TEXT("blocked_redirector_count"), Blocked.Num());
	OutData->SetNumberField(TEXT("deleted_redirector_count"), bDryRun ? 0 : DeletedCount);
	OutData->SetNumberField(TEXT("remaining_package_count"), RemainingPackageCount);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("recursive"), bRecursive);
	OutData->SetArrayField(TEXT("redirectors"), RedirectorReports);
	OutData->SetArrayField(TEXT("blocked"), Blocked);
	OutData->SetArrayField(TEXT("skipped"), Skipped);
	OutData->SetArrayField(TEXT("remaining_packages"), RemainingPackages);
	return true;
}

bool FUeAgentHttpServer::CmdAssetReferenceGraph(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAssetOps;
	using UE::AssetRegistry::EDependencyCategory;

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.TrimStartAndEnd().IsEmpty())
	{
		if (!JsonTryGetString(Ctx.Params, TEXT("package_name"), AssetPathInput) || AssetPathInput.TrimStartAndEnd().IsEmpty())
		{
			JsonTryGetString(Ctx.Params, TEXT("object_path"), AssetPathInput);
		}
	}
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = NormalizeReferenceInputToPackageName(AssetPathInput);
	if (PackageName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}

	FString Direction = TEXT("both");
	JsonTryGetString(Ctx.Params, TEXT("direction"), Direction);
	Direction.TrimStartAndEndInline();
	Direction = Direction.ToLower();
	if (Direction.IsEmpty())
	{
		Direction = TEXT("both");
	}
	const bool bQueryDependencies = Direction == TEXT("both") || Direction == TEXT("dependencies") || Direction == TEXT("dependency") || Direction == TEXT("downstream");
	const bool bQueryReferencers = Direction == TEXT("both") || Direction == TEXT("referencers") || Direction == TEXT("referencer") || Direction == TEXT("upstream");
	if (!bQueryDependencies && !bQueryReferencers)
	{
		OutError = TEXT("invalid_direction");
		return false;
	}

	bool bRecursive = false;
	JsonTryGetBool(Ctx.Params, TEXT("recursive"), bRecursive);

	double MaxDepthNumber = bRecursive ? 3.0 : 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_depth"), MaxDepthNumber);
	const int32 MaxDepth = FMath::Clamp(static_cast<int32>(MaxDepthNumber), 1, 16);

	double MaxNodesNumber = 1024.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_nodes"), MaxNodesNumber);
	const int32 MaxNodes = FMath::Clamp(static_cast<int32>(MaxNodesNumber), 1, 20000);

	bool bIncludeHard = true;
	bool bIncludeSoft = true;
	bool bIncludeManage = true;
	bool bIncludeSearchableNames = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_hard"), bIncludeHard);
	JsonTryGetBool(Ctx.Params, TEXT("include_soft"), bIncludeSoft);
	JsonTryGetBool(Ctx.Params, TEXT("include_manage"), bIncludeManage);
	JsonTryGetBool(Ctx.Params, TEXT("include_searchable_names"), bIncludeSearchableNames);

	bool bForceScan = false;
	JsonTryGetBool(Ctx.Params, TEXT("force_scan"), bForceScan);

	bool bAllowMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_missing"), bAllowMissing);

	bool bIncludePathStringHits = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_path_string_hits"), bIncludePathStringHits);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	if (bForceScan)
	{
		AssetRegistry.SearchAllAssets(true);
		FString PackageFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename, FPackageName::GetAssetPackageExtension()) && FPaths::FileExists(PackageFilename))
		{
			AssetRegistry.ScanFilesSynchronous({ PackageFilename }, true);
		}
		else if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename, FPackageName::GetMapPackageExtension()) && FPaths::FileExists(PackageFilename))
		{
			AssetRegistry.ScanFilesSynchronous({ PackageFilename }, true);
		}
		AssetRegistry.WaitForCompletion();
	}

	const FAssetIdentifier RootIdentifier{ FName(*PackageName) };
	TArray<FAssetData> RootAssetData;
	AssetRegistry.GetAssetsByPackageName(FName(*PackageName), RootAssetData, false);
	if (RootAssetData.Num() == 0 && !bAllowMissing)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	EDependencyCategory CategoryMask = EDependencyCategory::None;
	if (bIncludeHard || bIncludeSoft)
	{
		CategoryMask |= EDependencyCategory::Package;
	}
	if (bIncludeManage)
	{
		CategoryMask |= EDependencyCategory::Manage;
	}
	if (bIncludeSearchableNames)
	{
		CategoryMask |= EDependencyCategory::SearchableName;
	}

	struct FQueuedReferenceNode
	{
		FAssetIdentifier Identifier;
		int32 Depth = 0;
		FString QueryDirection;
	};

	TMap<FString, TSharedPtr<FJsonObject>> NodesById;
	TArray<TSharedPtr<FJsonValue>> EdgesJson;
	TArray<TSharedPtr<FJsonValue>> DirectDependenciesJson;
	TArray<TSharedPtr<FJsonValue>> DirectReferencersJson;
	TSet<FString> VisitedDependencyNodes;
	TSet<FString> VisitedReferencerNodes;
	TSet<FString> EdgeKeys;
	TArray<FQueuedReferenceNode> Queue;
	bool bTruncated = false;

	AddOrUpdateGraphNode(AssetRegistry, NodesById, RootIdentifier, 0, TEXT("root"));
	if (bQueryDependencies)
	{
		VisitedDependencyNodes.Add(RootIdentifier.ToString());
		Queue.Add({ RootIdentifier, 0, TEXT("dependencies") });
	}
	if (bQueryReferencers)
	{
		VisitedReferencerNodes.Add(RootIdentifier.ToString());
		Queue.Add({ RootIdentifier, 0, TEXT("referencers") });
	}

	for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
	{
		if (NodesById.Num() >= MaxNodes)
		{
			bTruncated = true;
			break;
		}

		const FQueuedReferenceNode Current = Queue[QueueIndex];
		if (Current.Depth >= MaxDepth)
		{
			continue;
		}

		TArray<FAssetDependency> FoundEdges;
		const bool bIsDependencyQuery = Current.QueryDirection == TEXT("dependencies");
		const bool bQueryOk = bIsDependencyQuery
			? AssetRegistry.GetDependencies(Current.Identifier, FoundEdges, CategoryMask)
			: AssetRegistry.GetReferencers(Current.Identifier, FoundEdges, CategoryMask);
		if (!bQueryOk)
		{
			continue;
		}

		for (const FAssetDependency& FoundEdge : FoundEdges)
		{
			if (!ShouldIncludeDependencyEdge(FoundEdge.Category, FoundEdge.Properties, bIncludeHard, bIncludeSoft, bIncludeManage, bIncludeSearchableNames))
			{
				continue;
			}

			const FAssetIdentifier SourceIdentifier = bIsDependencyQuery ? Current.Identifier : FoundEdge.AssetId;
			const FAssetIdentifier TargetIdentifier = bIsDependencyQuery ? FoundEdge.AssetId : Current.Identifier;
			const FAssetIdentifier OtherIdentifier = bIsDependencyQuery ? TargetIdentifier : SourceIdentifier;
			const int32 EdgeDepth = Current.Depth + 1;

			AddOrUpdateGraphNode(AssetRegistry, NodesById, SourceIdentifier, bIsDependencyQuery ? Current.Depth : EdgeDepth, bIsDependencyQuery ? TEXT("source") : TEXT("referencer"));
			AddOrUpdateGraphNode(AssetRegistry, NodesById, TargetIdentifier, bIsDependencyQuery ? EdgeDepth : Current.Depth, bIsDependencyQuery ? TEXT("dependency") : TEXT("target"));

			const FString EdgeKey = SourceIdentifier.ToString()
				+ TEXT("->") + TargetIdentifier.ToString()
				+ TEXT("|") + LexToString(static_cast<int64>(FoundEdge.Category))
				+ TEXT("|") + LexToString(static_cast<int64>(FoundEdge.Properties));
			if (!EdgeKeys.Contains(EdgeKey))
			{
				EdgeKeys.Add(EdgeKey);
				TSharedPtr<FJsonObject> EdgeJson = MakeAssetRegistryEdgeJson(SourceIdentifier, TargetIdentifier, Current.QueryDirection, EdgeDepth, FoundEdge.Category, FoundEdge.Properties);
				EdgesJson.Add(MakeShared<FJsonValueObject>(EdgeJson));
				if (Current.Identifier == RootIdentifier && bIsDependencyQuery)
				{
					DirectDependenciesJson.Add(MakeShared<FJsonValueObject>(EdgeJson));
				}
				if (Current.Identifier == RootIdentifier && !bIsDependencyQuery)
				{
					DirectReferencersJson.Add(MakeShared<FJsonValueObject>(EdgeJson));
				}
			}

			if (bRecursive && EdgeDepth < MaxDepth && NodesById.Num() < MaxNodes)
			{
				TSet<FString>& VisitedSet = bIsDependencyQuery ? VisitedDependencyNodes : VisitedReferencerNodes;
				const FString OtherKey = OtherIdentifier.ToString();
				if (!VisitedSet.Contains(OtherKey))
				{
					VisitedSet.Add(OtherKey);
					Queue.Add({ OtherIdentifier, EdgeDepth, Current.QueryDirection });
				}
			}
			else if (NodesById.Num() >= MaxNodes)
			{
				bTruncated = true;
				break;
			}
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("package_name"), PackageName);
	OutData->SetStringField(TEXT("input_asset_path"), AssetPathInput);
	OutData->SetStringField(TEXT("direction"), Direction);
	OutData->SetBoolField(TEXT("recursive"), bRecursive);
	OutData->SetNumberField(TEXT("max_depth"), MaxDepth);
	OutData->SetNumberField(TEXT("max_nodes"), MaxNodes);
	OutData->SetBoolField(TEXT("asset_found"), RootAssetData.Num() > 0);
	OutData->SetNumberField(TEXT("root_asset_data_count"), RootAssetData.Num());
	OutData->SetBoolField(TEXT("truncated"), bTruncated);
	OutData->SetNumberField(TEXT("dependency_count"), DirectDependenciesJson.Num());
	OutData->SetNumberField(TEXT("referencer_count"), DirectReferencersJson.Num());
	OutData->SetNumberField(TEXT("edge_count"), EdgesJson.Num());
	OutData->SetNumberField(TEXT("node_count"), NodesById.Num());
	OutData->SetArrayField(TEXT("dependencies"), DirectDependenciesJson);
	OutData->SetArrayField(TEXT("referencers"), DirectReferencersJson);
	OutData->SetArrayField(TEXT("edges"), EdgesJson);
	OutData->SetArrayField(TEXT("nodes"), SortedNodeValues(NodesById));

	TSharedPtr<FJsonObject> OptionsJson = MakeShared<FJsonObject>();
	OptionsJson->SetBoolField(TEXT("include_hard"), bIncludeHard);
	OptionsJson->SetBoolField(TEXT("include_soft"), bIncludeSoft);
	OptionsJson->SetBoolField(TEXT("include_manage"), bIncludeManage);
	OptionsJson->SetBoolField(TEXT("include_searchable_names"), bIncludeSearchableNames);
	OptionsJson->SetBoolField(TEXT("force_scan"), bForceScan);
	OptionsJson->SetBoolField(TEXT("allow_missing"), bAllowMissing);
	OptionsJson->SetBoolField(TEXT("include_path_string_hits"), bIncludePathStringHits);
	OutData->SetObjectField(TEXT("options"), OptionsJson);

	if (bIncludePathStringHits)
	{
		double MaxHitsNumber = 200.0;
		JsonTryGetNumber(Ctx.Params, TEXT("max_path_string_hits"), MaxHitsNumber);
		const int32 MaxHits = FMath::Clamp(static_cast<int32>(MaxHitsNumber), 1, 5000);

		bool bPathStringHitsTruncated = false;
		TArray<TSharedPtr<FJsonValue>> HitsJson = CollectPackagePathStringHits(PackageName, MaxHits, bPathStringHitsTruncated);
		bTruncated |= bPathStringHitsTruncated;

		OutData->SetArrayField(TEXT("path_string_hits"), HitsJson);
		OutData->SetNumberField(TEXT("path_string_hit_count"), HitsJson.Num());
		OutData->SetBoolField(TEXT("truncated"), bTruncated);
	}

	return true;
}

bool FUeAgentHttpServer::CmdAssetExportPropertyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	TArray<FString> PropertyNames;
	const bool bHasExplicitPropertyNames = UeAgentAssetOps::ReadStringArrayParam(Ctx.Params, TEXT("property_names"), PropertyNames) && PropertyNames.Num() > 0;
	FString PresetName;
	if (!bHasExplicitPropertyNames)
	{
		UeAgentAssetOps::GetDefaultAssetPropertyPaths(Asset, PropertyNames, PresetName);
	}

	if (PropertyNames.Num() == 0)
	{
		OutError = TEXT("missing_property_names_and_no_default_preset");
		return false;
	}

	PropertyNames.Sort();
	for (int32 Index = PropertyNames.Num() - 1; Index > 0; --Index)
	{
		if (PropertyNames[Index].Equals(PropertyNames[Index - 1], ESearchCase::CaseSensitive))
		{
			PropertyNames.RemoveAt(Index);
		}
	}

	TArray<TSharedPtr<FJsonValue>> PropertiesJson;
	TArray<TSharedPtr<FJsonValue>> MissingPropertiesJson;
	int32 ExportedPropertyCount = 0;
	for (const FString& PropertyPath : PropertyNames)
	{
		FString ValueText;
		FString CppType;
		if (!UeAgentAssetOps::GetObjectPropertyText(Asset, PropertyPath, ValueText, CppType))
		{
			MissingPropertiesJson.Add(MakeShared<FJsonValueString>(PropertyPath));
			continue;
		}

		TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
		PropertyObj->SetStringField(TEXT("property_name"), PropertyPath);
		PropertyObj->SetStringField(TEXT("value_text"), ValueText);
		if (!CppType.IsEmpty())
		{
			PropertyObj->SetStringField(TEXT("cpp_type"), CppType);
		}
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (UeAgentAssetOps::ResolvePropertyPath(Asset, PropertyPath, Property, ValuePtr) && Property && ValuePtr)
		{
			TSharedPtr<FJsonObject> CurveJson;
			if (UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr, CurveJson) && CurveJson.IsValid())
			{
				PropertyObj->SetObjectField(TEXT("value_json"), CurveJson);
				PropertyObj->SetObjectField(TEXT("curve_json"), CurveJson);
				PropertyObj->SetStringField(TEXT("value_json_schema"), UeAgentCurveJson::SchemaName);
			}
		}
		PropertiesJson.Add(MakeShared<FJsonValueObject>(PropertyObj));
		++ExportedPropertyCount;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("format_version"), 1);
	Root->SetStringField(TEXT("asset_path"), UeAgentAssetOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	Root->SetStringField(TEXT("object_path"), Asset->GetPathName());
	Root->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	if (!PresetName.IsEmpty())
	{
		Root->SetStringField(TEXT("property_preset"), PresetName);
	}
	Root->SetArrayField(TEXT("properties"), PropertiesJson);
	if (MissingPropertiesJson.Num() > 0)
	{
		Root->SetArrayField(TEXT("missing_properties"), MissingPropertiesJson);
	}

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty())
	{
		if (!UeAgentAssetOps::SaveJsonFile(OutputFile, Root, SavedOutputFile, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentAssetOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	if (!PresetName.IsEmpty())
	{
		OutData->SetStringField(TEXT("property_preset"), PresetName);
	}
	OutData->SetArrayField(TEXT("properties"), PropertiesJson);
	OutData->SetNumberField(TEXT("property_count"), ExportedPropertyCount);
	if (MissingPropertiesJson.Num() > 0)
	{
		OutData->SetArrayField(TEXT("missing_properties"), MissingPropertiesJson);
		OutData->SetNumberField(TEXT("missing_property_count"), MissingPropertiesJson.Num());
	}
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdAssetApplyPropertyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> JsonRoot;
	FString JsonFile;
	FString ResolvedJsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	JsonFile.TrimStartAndEndInline();
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentAssetOps::LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
		{
			return false;
		}
	}

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty() && JsonRoot.IsValid())
	{
		JsonRoot->TryGetStringField(TEXT("asset_path"), AssetPathInput);
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
	if (!Ctx.Params->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
	{
		if (!JsonRoot.IsValid() || !JsonRoot->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
		{
			OutError = TEXT("missing_properties");
			return false;
		}
	}

	int32 AppliedPropertyCount = 0;
	TArray<TSharedPtr<FJsonValue>> PropertyResultsJson;
	int32 PropertyIndex = 0;
	for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
	{
		const FString PropertyPath = FString::Printf(TEXT("properties[%d]"), PropertyIndex++);
		TArray<TSharedPtr<FJsonValue>> PropertyJsonIssues;
		TSharedPtr<FJsonObject> PropertyObj;
		if (!UeAgentJsonDiagnostics::ReadObjectFromValue(PropertyValue, PropertyPath, PropertyObj, PropertyJsonIssues))
		{
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(TEXT(""), nullptr, TEXT(""), TEXT(""), TEXT("invalid_property_entry"));
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = TEXT("invalid_property_entry");
			return false;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(
			PropertyObj,
			PropertyPath,
			{ TEXT("property_name"), TEXT("value_text"), TEXT("cpp_type"), TEXT("value_json"), TEXT("curve_json"), TEXT("value_json_schema") },
			PropertyJsonIssues);

		FString PropertyName;
		FString ValueText;
		if (!UeAgentJsonDiagnostics::ReadStringField(PropertyObj, TEXT("property_name"), PropertyPath + TEXT(".property_name"), PropertyName, PropertyJsonIssues, true)
			|| PropertyName.IsEmpty())
		{
			if (PropertyName.IsEmpty())
			{
				UeAgentJsonDiagnostics::AddIssue(PropertyJsonIssues, TEXT("warning"), TEXT("json_missing_required_field"), PropertyPath + TEXT(".property_name"), TEXT("Required string field is empty."));
			}
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(TEXT(""), nullptr, TEXT(""), TEXT(""), TEXT("missing_property_name"));
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = TEXT("missing_property_name");
			return false;
		}

		TSharedPtr<FJsonObject> TypedCurveJson;
		bool bHasTypedCurveJson = false;
		const TSharedPtr<FJsonObject>* CurveJsonPtr = nullptr;
		FString TypedCurveJsonFieldName;
		if (PropertyObj->TryGetObjectField(TEXT("curve_json"), CurveJsonPtr) && CurveJsonPtr && CurveJsonPtr->IsValid())
		{
			TypedCurveJson = *CurveJsonPtr;
			bHasTypedCurveJson = true;
			TypedCurveJsonFieldName = TEXT("curve_json");
		}
		else if (PropertyObj->TryGetObjectField(TEXT("value_json"), CurveJsonPtr) && CurveJsonPtr && CurveJsonPtr->IsValid())
		{
			TypedCurveJson = *CurveJsonPtr;
			bHasTypedCurveJson = true;
			TypedCurveJsonFieldName = TEXT("value_json");
		}

		if (!bHasTypedCurveJson && !UeAgentJsonDiagnostics::ReadStringField(PropertyObj, TEXT("value_text"), PropertyPath + TEXT(".value_text"), ValueText, PropertyJsonIssues, true))
		{
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, TEXT(""), TEXT(""), TEXT("missing_value_text"));
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = TEXT("missing_value_text");
			return false;
		}

		FString AppliedValueText;
		FString CppType;
		FString ImportError;
		if (bHasTypedCurveJson)
		{
			FProperty* Property = nullptr;
			void* ValuePtr = nullptr;
			if (!UeAgentAssetOps::ResolvePropertyPath(Asset, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
			{
				TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, TEXT(""), TEXT(""), TEXT("property_not_found"), TEXT("property_not_found"));
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
				PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
				OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
				OutError = TEXT("property_not_found");
				return false;
			}

			if (!UeAgentCurveJson::TryApplyPropertyCurveJson(Property, ValuePtr, TypedCurveJson, PropertyPath + TEXT(".") + TypedCurveJsonFieldName, PropertyJsonIssues))
			{
				TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, Property, TEXT("<curve_json>"), TEXT(""), TEXT("curve_json_apply_failed"), TEXT("curve_json_apply_failed"));
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
				PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
				OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
				OutError = TEXT("curve_json_apply_failed");
				return false;
			}

			Property->ExportTextItem_Direct(AppliedValueText, ValuePtr, nullptr, Asset, PPF_None);
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, Property, TEXT("<curve_json>"), AppliedValueText, TEXT("curve_json_applied_and_read_back"));
			TSharedPtr<FJsonObject> ReadbackCurveJson;
			if (UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr, ReadbackCurveJson) && ReadbackCurveJson.IsValid())
			{
				ResultObj->SetObjectField(TEXT("value_json_read_back"), ReadbackCurveJson);
			}
			if (PropertyJsonIssues.Num() > 0)
			{
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			}
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			++AppliedPropertyCount;
			continue;
		}

		if (!UeAgentAssetOps::SetObjectPropertyText(Asset, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
		{
			const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
			ResultObj->SetStringField(TEXT("cpp_type"), CppType);
			if (PropertyJsonIssues.Num() > 0)
			{
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			}
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = ImportError.IsEmpty() ? FString::Printf(TEXT("asset_property_apply_failed:%s:%s"), *PropertyName, *ValueText) : ImportError;
			return false;
		}

		TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
		ResultObj->SetStringField(TEXT("cpp_type"), CppType);
		if (PropertyJsonIssues.Num() > 0)
		{
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
		}
		PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
		++AppliedPropertyCount;
	}

	if (AppliedPropertyCount > 0)
	{
		Asset->Modify();
	}
	Asset->MarkPackageDirty();
	Asset->PostEditChange();

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentAssetOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentAssetOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("applied_property_count"), AppliedPropertyCount);
	OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	if (!ResolvedJsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdCurveExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("curve_asset_not_found");
		return false;
	}

	TSharedPtr<FJsonObject> CurveJson = UeAgentCurveJson::BuildCurveAssetJson(Asset);
	if (!CurveJson.IsValid())
	{
		OutError = TEXT("unsupported_curve_asset");
		return false;
	}

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty() && !UeAgentAssetOps::SaveJsonFile(OutputFile, CurveJson, SavedOutputFile, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentCurveJson::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetObjectField(TEXT("curve"), CurveJson);
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdCurveApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> JsonRoot;
	FString JsonFile;
	FString ResolvedJsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	JsonFile.TrimStartAndEndInline();
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentAssetOps::LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> CurveJson;
	const TSharedPtr<FJsonObject>* CurveObjPtr = nullptr;
	if (Ctx.Params->TryGetObjectField(TEXT("curve"), CurveObjPtr) && CurveObjPtr && CurveObjPtr->IsValid())
	{
		CurveJson = *CurveObjPtr;
	}
	else if (JsonRoot.IsValid() && JsonRoot->TryGetObjectField(TEXT("curve"), CurveObjPtr) && CurveObjPtr && CurveObjPtr->IsValid())
	{
		CurveJson = *CurveObjPtr;
	}
	else if (JsonRoot.IsValid())
	{
		CurveJson = JsonRoot;
	}
	else
	{
		CurveJson = Ctx.Params;
	}

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty() && CurveJson.IsValid())
	{
		CurveJson->TryGetStringField(TEXT("asset_path"), AssetPathInput);
		AssetPathInput.TrimStartAndEndInline();
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	bool bCreatedAsset = false;
	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	if (!Asset && bCreateIfMissing)
	{
		FString CurveKind;
		JsonTryGetString(Ctx.Params, TEXT("curve_kind"), CurveKind);
		if (CurveKind.TrimStartAndEnd().IsEmpty() && CurveJson.IsValid())
		{
			CurveJson->TryGetStringField(TEXT("curve_kind"), CurveKind);
		}
		Asset = UeAgentCurveJson::CreateCurveAsset(AssetPathInput, CurveKind, OutError);
		bCreatedAsset = Asset != nullptr;
	}
	if (!Asset)
	{
		OutError = TEXT("curve_asset_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> JsonIssues;
	if (!UeAgentCurveJson::ApplyCurveAssetJson(Asset, CurveJson, TEXT("curve"), JsonIssues))
	{
		OutData->SetStringField(TEXT("asset_path"), UeAgentCurveJson::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
		OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
		OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		OutData->SetArrayField(TEXT("json_issues"), JsonIssues);
		if (!ResolvedJsonFile.IsEmpty())
		{
			OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
		}
		OutError = TEXT("curve_json_apply_failed");
		return false;
	}

	Asset->Modify();
	Asset->MarkPackageDirty();
	Asset->PostEditChange();

	TSharedPtr<FJsonObject> ReadbackCurveJson = UeAgentCurveJson::BuildCurveAssetJson(Asset);
	if (ReadbackCurveJson.IsValid())
	{
		OutData->SetObjectField(TEXT("curve_read_back"), ReadbackCurveJson);
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentAssetOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty() && ReadbackCurveJson.IsValid() && !UeAgentAssetOps::SaveJsonFile(OutputFile, ReadbackCurveJson, SavedOutputFile, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentCurveJson::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("created"), bCreatedAsset);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetArrayField(TEXT("json_issues"), JsonIssues);
	if (!ResolvedJsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
	}
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}
