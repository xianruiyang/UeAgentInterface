// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer.h"

#include "UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceLogger.h"

#include "Algo/Reverse.h"
#include "AIController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Decorators/BTDecorator_BlueprintBase.h"
#include "BehaviorTree/Services/BTService_BlueprintBase.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeNodeBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "Components/StateTreeAIComponentSchema.h"
#include "Components/StateTreeComponent.h"
#include "Components/StateTreeComponentSchema.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_SimpleGrid.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_ActorBase.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_VectorBase.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Distance.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GenericPlatform/GenericWindow.h"
#include "GameplayTagContainer.h"
#include "InstancedStruct.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NavAreas/NavArea.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionStimuliSourceComponent.h"
#include "Perception/AISenseConfig.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/WidgetRenderer.h"
#include "SmartObjectComponent.h"
#include "SmartObjectDefinition.h"
#include "SmartObjectRequestTypes.h"
#include "SmartObjectRuntime.h"
#include "SmartObjectSubsystem.h"
#include "SmartObjectTypes.h"
#include "StateTree.h"
#include "StateTreeCompiler.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorModule.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeReference.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

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

namespace UeAgentAIOps
{
	static UClass* ResolveBTClass(const FString& ClassOrKind, const UClass* RequiredBase)
	{
		FString Lower = ClassOrKind.ToLower();
		UClass* Class = nullptr;
		if (Lower == TEXT("sequence"))
		{
			Class = UBTComposite_Sequence::StaticClass();
		}
		else if (Lower == TEXT("selector"))
		{
			Class = UBTComposite_Selector::StaticClass();
		}
		else if (Lower == TEXT("wait"))
		{
			Class = UBTTask_Wait::StaticClass();
		}
		else
		{
			Class = ResolveClass(ClassOrKind);
		}
		return (Class && Class->IsChildOf(RequiredBase)) ? Class : nullptr;
	}

	static FString BTKindForNode(const UBTNode* Node)
	{
		if (Cast<const UBTCompositeNode>(Node)) return TEXT("composite");
		if (Cast<const UBTTaskNode>(Node)) return TEXT("task");
		if (Cast<const UBTDecorator>(Node)) return TEXT("decorator");
		if (Cast<const UBTService>(Node)) return TEXT("service");
		return TEXT("node");
	}

	static TSharedPtr<FJsonObject> ExportBTNode(const UBTNode* Node, const FString& Id)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Node)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("id"), Id);
		Obj->SetStringField(TEXT("kind"), BTKindForNode(Node));
		Obj->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
		Obj->SetStringField(TEXT("name"), Node->NodeName);
		Obj->SetStringField(TEXT("static_description"), Node->GetStaticDescription());
		return Obj;
	}

	static void TraverseBTComposite(const UBTCompositeNode* Composite, const FString& Id, TArray<TSharedPtr<FJsonValue>>& Nodes, TArray<TSharedPtr<FJsonValue>>& Edges, int32& Counter)
	{
		if (!Composite)
		{
			return;
		}
		Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Composite, Id)));

		for (int32 ServiceIndex = 0; ServiceIndex < Composite->Services.Num(); ++ServiceIndex)
		{
			const FString ServiceId = FString::Printf(TEXT("%s_service_%d"), *Id, ServiceIndex);
			Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Composite->Services[ServiceIndex], ServiceId)));
			TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
			Edge->SetStringField(TEXT("parent_id"), Id);
			Edge->SetStringField(TEXT("child_id"), ServiceId);
			Edge->SetStringField(TEXT("relation"), TEXT("service"));
			Edges.Add(MakeShared<FJsonValueObject>(Edge));
		}

		for (int32 ChildIndex = 0; ChildIndex < Composite->Children.Num(); ++ChildIndex)
		{
			const FBTCompositeChild& Child = Composite->Children[ChildIndex];
			const UBTNode* ChildNode = Child.ChildComposite ? static_cast<const UBTNode*>(Child.ChildComposite.Get()) : static_cast<const UBTNode*>(Child.ChildTask.Get());
			const FString ChildId = FString::Printf(TEXT("node_%d"), Counter++);
			TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
			Edge->SetStringField(TEXT("parent_id"), Id);
			Edge->SetStringField(TEXT("child_id"), ChildId);
			Edge->SetStringField(TEXT("relation"), TEXT("child"));
			Edge->SetNumberField(TEXT("order"), ChildIndex);
			TArray<TSharedPtr<FJsonValue>> DecoratorIds;
			for (int32 DecoratorIndex = 0; DecoratorIndex < Child.Decorators.Num(); ++DecoratorIndex)
			{
				const FString DecoratorId = FString::Printf(TEXT("%s_decorator_%d"), *ChildId, DecoratorIndex);
				DecoratorIds.Add(MakeShared<FJsonValueString>(DecoratorId));
				Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Child.Decorators[DecoratorIndex], DecoratorId)));
			}
			Edge->SetArrayField(TEXT("decorator_ids"), DecoratorIds);
			Edges.Add(MakeShared<FJsonValueObject>(Edge));

			if (const UBTCompositeNode* ChildComposite = Cast<UBTCompositeNode>(ChildNode))
			{
				TraverseBTComposite(ChildComposite, ChildId, Nodes, Edges, Counter);
			}
			else
			{
				Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(ChildNode, ChildId)));
				if (const UBTTaskNode* Task = Cast<UBTTaskNode>(ChildNode))
				{
					for (int32 ServiceIndex = 0; ServiceIndex < Task->Services.Num(); ++ServiceIndex)
					{
						const FString ServiceId = FString::Printf(TEXT("%s_service_%d"), *ChildId, ServiceIndex);
						Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Task->Services[ServiceIndex], ServiceId)));
						TSharedPtr<FJsonObject> ServiceEdge = MakeShared<FJsonObject>();
						ServiceEdge->SetStringField(TEXT("parent_id"), ChildId);
						ServiceEdge->SetStringField(TEXT("child_id"), ServiceId);
						ServiceEdge->SetStringField(TEXT("relation"), TEXT("service"));
						Edges.Add(MakeShared<FJsonValueObject>(ServiceEdge));
					}
				}
			}
		}
	}

	static TSharedPtr<FJsonObject> ExportBehaviorTree(UBehaviorTree* Tree, const bool bIncludeNodes)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), BehaviorTreeProfile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		Root->SetStringField(TEXT("asset_path"), Tree ? Tree->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), Tree ? Tree->GetPathName() : FString());
		Root->SetStringField(TEXT("blackboard_asset"), (Tree && Tree->BlackboardAsset) ? Tree->BlackboardAsset->GetPathName() : FString());
		Root->SetStringField(TEXT("root_id"), Tree && Tree->RootNode ? TEXT("root") : FString());
		Root->SetBoolField(TEXT("dirty"), Tree && Tree->GetOutermost()->IsDirty());
		if (bIncludeNodes && Tree && Tree->RootNode)
		{
			TArray<TSharedPtr<FJsonValue>> Nodes;
			TArray<TSharedPtr<FJsonValue>> Edges;
			int32 Counter = 1;
			TraverseBTComposite(Tree->RootNode, TEXT("root"), Nodes, Edges, Counter);
			Root->SetArrayField(TEXT("nodes"), Nodes);
			Root->SetArrayField(TEXT("edges"), Edges);
			Root->SetNumberField(TEXT("node_count"), Nodes.Num());
			Root->SetNumberField(TEXT("edge_count"), Edges.Num());
		}
		return Root;
	}

	static TSharedPtr<FJsonObject> MakeBTAssetJson(UBehaviorTree* Tree)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("profile"), BehaviorTreeProfile);
		Obj->SetNumberField(TEXT("schema_version"), 1);
		Obj->SetStringField(TEXT("asset_path"), Tree ? Tree->GetOutermost()->GetName() : FString());
		Obj->SetStringField(TEXT("object_path"), Tree ? Tree->GetPathName() : FString());
		Obj->SetStringField(TEXT("blackboard_asset"), (Tree && Tree->BlackboardAsset) ? Tree->BlackboardAsset->GetPathName() : FString());
		return Obj;
	}

	static bool ValidateBTFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& NodesJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!AssetJson.IsValid())
		{
			AddIssue(Issues, TEXT("missing_required_file"), TEXT("Missing asset.json."), TEXT("asset.json"));
		}
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!NodesJson.IsValid() || !NodesJson->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing nodes array."), TEXT("nodes.json.nodes"));
			return false;
		}

		TSet<FString> Ids;
		for (int32 Index = 0; Index < Nodes->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!(*Nodes)[Index].IsValid() || !(*Nodes)[Index]->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_node_item"), TEXT("Node item must be an object."), FString::Printf(TEXT("nodes.json.nodes[%d]"), Index));
				continue;
			}
			FString Id;
			FString Kind;
			FString ClassPath;
			(*NodeObjPtr)->TryGetStringField(TEXT("id"), Id);
			(*NodeObjPtr)->TryGetStringField(TEXT("kind"), Kind);
			(*NodeObjPtr)->TryGetStringField(TEXT("class"), ClassPath);
			if (Id.IsEmpty())
			{
				AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing node id."), FString::Printf(TEXT("nodes.json.nodes[%d].id"), Index));
			}
			else if (Ids.Contains(Id))
			{
				AddIssue(Issues, TEXT("duplicate_id"), FString::Printf(TEXT("Duplicate node id '%s'."), *Id), FString::Printf(TEXT("nodes.json.nodes[%d].id"), Index));
			}
			Ids.Add(Id);
			const UClass* BaseClass = UBTNode::StaticClass();
			if (Kind == TEXT("composite")) BaseClass = UBTCompositeNode::StaticClass();
			else if (Kind == TEXT("task")) BaseClass = UBTTaskNode::StaticClass();
			else if (Kind == TEXT("decorator")) BaseClass = UBTDecorator::StaticClass();
			else if (Kind == TEXT("service")) BaseClass = UBTService::StaticClass();
			if (!ResolveBTClass(ClassPath.IsEmpty() ? Kind : ClassPath, BaseClass))
			{
				AddIssue(Issues, TEXT("bt_node_class_not_found"), FString::Printf(TEXT("Invalid BT node class '%s'."), *ClassPath), FString::Printf(TEXT("nodes.json.nodes[%d].class"), Index));
			}
		}
		return Issues.Num() == 0;
	}

	static UBTNode* CreateBTNodeFromJson(UBehaviorTree* Tree, const TSharedPtr<FJsonObject>& NodeObj, TArray<TSharedPtr<FJsonValue>>& PropertyResults, FString& OutError)
	{
		FString Kind;
		FString ClassPath;
		FString Name;
		NodeObj->TryGetStringField(TEXT("kind"), Kind);
		NodeObj->TryGetStringField(TEXT("class"), ClassPath);
		NodeObj->TryGetStringField(TEXT("name"), Name);
		const UClass* BaseClass = UBTNode::StaticClass();
		if (Kind == TEXT("composite")) BaseClass = UBTCompositeNode::StaticClass();
		else if (Kind == TEXT("task")) BaseClass = UBTTaskNode::StaticClass();
		else if (Kind == TEXT("decorator")) BaseClass = UBTDecorator::StaticClass();
		else if (Kind == TEXT("service")) BaseClass = UBTService::StaticClass();
		UClass* NodeClass = ResolveBTClass(ClassPath.IsEmpty() ? Kind : ClassPath, BaseClass);
		if (!NodeClass)
		{
			OutError = TEXT("bt_node_class_not_found");
			return nullptr;
		}
		UBTNode* Node = NewObject<UBTNode>(Tree, NodeClass, NAME_None, RF_Transactional);
		if (!Node)
		{
			OutError = TEXT("bt_node_create_failed");
			return nullptr;
		}
		Node->NodeName = Name;
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (NodeObj->TryGetArrayField(TEXT("properties"), Properties) && Properties)
		{
			if (!ApplyProperties(Node, Properties, PropertyResults, OutError))
			{
				return nullptr;
			}
		}
		return Node;
	}
}

bool FUeAgentHttpServer::CmdBehaviorTreeCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBehaviorTree* Tree = nullptr;
	if (!UeAgentAIOps::CreateAsset<UBehaviorTree>(AssetPath, Tree, OutError) || !Tree)
	{
		return false;
	}

	FString BlackboardPath;
	if (JsonTryGetString(Ctx.Params, TEXT("blackboard_asset"), BlackboardPath) && !BlackboardPath.IsEmpty())
	{
		Tree->BlackboardAsset = UeAgentAIOps::LoadAsset<UBlackboardData>(BlackboardPath);
		if (!Tree->BlackboardAsset)
		{
			OutError = TEXT("blackboard_not_found");
			return false;
		}
	}
	FString RootComposite;
	if (!JsonTryGetString(Ctx.Params, TEXT("root_composite"), RootComposite) || RootComposite.IsEmpty())
	{
		RootComposite = TEXT("sequence");
	}
	UClass* RootClass = UeAgentAIOps::ResolveBTClass(RootComposite, UBTCompositeNode::StaticClass());
	if (!RootClass)
	{
		OutError = TEXT("bt_node_class_not_found");
		return false;
	}
	Tree->RootNode = NewObject<UBTCompositeNode>(Tree, RootClass, NAME_None, RF_Transactional);
	Tree->RootNode->NodeName = TEXT("Root");
	Tree->MarkPackageDirty();
	Tree->PostEditChange();

	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Tree, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportBehaviorTree(Tree, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBehaviorTree* Tree = UeAgentAIOps::LoadAsset<UBehaviorTree>(AssetPath);
	if (!Tree)
	{
		OutError = TEXT("behavior_tree_not_found");
		return false;
	}
	bool bIncludeNodes = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_nodes"), bIncludeNodes);
	OutData = UeAgentAIOps::ExportBehaviorTree(Tree, bIncludeNodes);
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBehaviorTree* Tree = UeAgentAIOps::LoadAsset<UBehaviorTree>(AssetPath);
	if (!Tree)
	{
		OutError = TEXT("behavior_tree_not_found");
		return false;
	}
	FString FolderInput;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput);
	const FString FolderPath = UeAgentAIOps::ResolveFolderPath(FolderInput, TEXT("BehaviorTree"), AssetPath);
	bool bClean = false;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bClean);
	if (bClean)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	TArray<TSharedPtr<FJsonValue>> WrittenFiles;
	if (!UeAgentAIOps::SaveBehaviorTreeFolderContents(FolderPath, Tree, WrittenFiles, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportBehaviorTree(Tree, true);
	OutData->SetArrayField(TEXT("written_files"), WrittenFiles);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> NodesJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError)) return false;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("nodes.json"), NodesJson, OutError)) return false;
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bValid = UeAgentAIOps::ValidateBTFolderJson(AssetJson, NodesJson, Issues);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::BehaviorTreeProfile);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetStringField(TEXT("status"), bValid ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return bValid;
}

bool FUeAgentHttpServer::CmdBehaviorTreeApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> BlackboardJson;
	TSharedPtr<FJsonObject> TreeJson;
	TSharedPtr<FJsonObject> NodesJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError)) return false;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("blackboard.json"), BlackboardJson, OutError, false)) return false;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("tree.json"), TreeJson, OutError)) return false;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("nodes.json"), NodesJson, OutError)) return false;
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!UeAgentAIOps::ValidateBTFolderJson(AssetJson, NodesJson, Issues))
	{
		OutData->SetStringField(TEXT("status"), TEXT("validate_failed"));
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return false;
	}

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	bool bCreateIfMissing = false;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bAllowDestructive = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	if (bDryRun || bValidateOnly)
	{
		OutData->SetStringField(TEXT("profile"), UeAgentAIOps::BehaviorTreeProfile);
		OutData->SetStringField(TEXT("asset_path"), AssetPath);
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		OutData->SetBoolField(TEXT("changed"), false);
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return true;
	}

	UBehaviorTree* Tree = UeAgentAIOps::LoadAsset<UBehaviorTree>(AssetPath);
	bool bCreated = false;
	if (!Tree)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("behavior_tree_not_found");
			return false;
		}
		if (!UeAgentAIOps::CreateAsset<UBehaviorTree>(AssetPath, Tree, OutError) || !Tree)
		{
			return false;
		}
		bCreated = true;
	}
	if (Tree->RootNode && !bCreated && !bAllowDestructive)
	{
		OutError = TEXT("bt_destructive_change_requires_opt_in");
		return false;
	}

	Tree->Modify();
	FString BlackboardPath;
	if ((BlackboardJson.IsValid() && BlackboardJson->TryGetStringField(TEXT("blackboard_asset"), BlackboardPath)) || AssetJson->TryGetStringField(TEXT("blackboard_asset"), BlackboardPath))
	{
		if (!BlackboardPath.IsEmpty())
		{
			Tree->BlackboardAsset = UeAgentAIOps::LoadAsset<UBlackboardData>(BlackboardPath);
			if (!Tree->BlackboardAsset)
			{
				OutError = TEXT("blackboard_not_found");
				return false;
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	NodesJson->TryGetArrayField(TEXT("nodes"), Nodes);
	TMap<FString, UBTNode*> NodeMap;
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	for (const TSharedPtr<FJsonValue>& Value : *Nodes)
	{
		const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
		Value->TryGetObject(NodeObjPtr);
		FString Id;
		(*NodeObjPtr)->TryGetStringField(TEXT("id"), Id);
		if (UBTNode* Node = UeAgentAIOps::CreateBTNodeFromJson(Tree, *NodeObjPtr, PropertyResults, OutError))
		{
			NodeMap.Add(Id, Node);
		}
		else
		{
			return false;
		}
	}

	FString RootId;
	TreeJson->TryGetStringField(TEXT("root_id"), RootId);
	if (RootId.IsEmpty())
	{
		RootId = TEXT("root");
	}
	Tree->RootNode = Cast<UBTCompositeNode>(NodeMap.FindRef(RootId));
	if (!Tree->RootNode)
	{
		OutError = TEXT("bt_root_composite_not_found");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
	TreeJson->TryGetArrayField(TEXT("edges"), Edges);
	if (Edges)
	{
		for (const TSharedPtr<FJsonValue>& EdgeValue : *Edges)
		{
			const TSharedPtr<FJsonObject>* EdgeObjPtr = nullptr;
			if (!EdgeValue.IsValid() || !EdgeValue->TryGetObject(EdgeObjPtr) || !EdgeObjPtr || !EdgeObjPtr->IsValid())
			{
				continue;
			}
			FString ParentId;
			FString ChildId;
			FString Relation;
			(*EdgeObjPtr)->TryGetStringField(TEXT("parent_id"), ParentId);
			(*EdgeObjPtr)->TryGetStringField(TEXT("child_id"), ChildId);
			(*EdgeObjPtr)->TryGetStringField(TEXT("relation"), Relation);
			UBTNode* ParentNode = NodeMap.FindRef(ParentId);
			UBTNode* ChildNode = NodeMap.FindRef(ChildId);
			if (Relation == TEXT("service"))
			{
				if (UBTService* Service = Cast<UBTService>(ChildNode))
				{
					if (UBTCompositeNode* Composite = Cast<UBTCompositeNode>(ParentNode))
					{
						Composite->Services.Add(Service);
					}
					else if (UBTTaskNode* Task = Cast<UBTTaskNode>(ParentNode))
					{
						Task->Services.Add(Service);
					}
				}
				continue;
			}
			UBTCompositeNode* ParentComposite = Cast<UBTCompositeNode>(ParentNode);
			if (!ParentComposite || !ChildNode)
			{
				continue;
			}
			FBTCompositeChild& Child = ParentComposite->Children.AddDefaulted_GetRef();
			Child.ChildComposite = Cast<UBTCompositeNode>(ChildNode);
			Child.ChildTask = Cast<UBTTaskNode>(ChildNode);
			const TArray<TSharedPtr<FJsonValue>>* DecoratorIds = nullptr;
			if ((*EdgeObjPtr)->TryGetArrayField(TEXT("decorator_ids"), DecoratorIds) && DecoratorIds)
			{
				for (const TSharedPtr<FJsonValue>& DecoratorValue : *DecoratorIds)
				{
					FString DecoratorId;
					if (DecoratorValue.IsValid() && DecoratorValue->TryGetString(DecoratorId))
					{
						if (UBTDecorator* Decorator = Cast<UBTDecorator>(NodeMap.FindRef(DecoratorId)))
						{
							Child.Decorators.Add(Decorator);
							Child.DecoratorOps.Add(FBTDecoratorLogic(EBTDecoratorLogic::Test, 0));
						}
					}
				}
			}
		}
	}

	Tree->MarkPackageDirty();
	Tree->PostEditChange();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Tree, OutError))
	{
		return false;
	}

	OutData = UeAgentAIOps::ExportBehaviorTree(Tree, true);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("created_asset"), bCreated);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("behavior_tree_not_found");
		return false;
	}
	if (GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
	}
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetBoolField(TEXT("opened"), true);
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeGraphGetView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdBehaviorTreeGetInfo(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdBehaviorTreeGraphSetView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdBehaviorTreeGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	double X = 0.0;
	double Y = 0.0;
	double Zoom = 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("view_x"), X);
	JsonTryGetNumber(Ctx.Params, TEXT("view_y"), Y);
	JsonTryGetNumber(Ctx.Params, TEXT("zoom"), Zoom);
	OutData->SetNumberField(TEXT("view_x"), X);
	OutData->SetNumberField(TEXT("view_y"), Y);
	OutData->SetNumberField(TEXT("zoom"), Zoom);
	OutData->SetBoolField(TEXT("view_requested"), true);
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("behavior_tree_not_found");
		return false;
	}
	bool bOpenIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenIfNeeded);
	TSharedPtr<SWindow> Window;
	if (!UeAgentAIOps::ResolveGenericAssetEditorWindow(Asset, bOpenIfNeeded, Window, OutError))
	{
		return false;
	}
	FString BackbufferUnsafeReason;
	const bool bBackbufferCaptureUnsafe = UeAgentAIOps::IsSlateWindowBackbufferCaptureUnsafe(Window, BackbufferUnsafeReason);
	TArray<FColor> Pixels;
	FIntPoint Size;
	if (!UeAgentAIOps::ScreenshotSlateWindow(Window, Pixels, Size, OutError))
	{
		return false;
	}
	int32 MaxSize = 1600;
	double MaxSizeNumber = MaxSize;
	JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeNumber);
	MaxSize = (int32)FMath::Clamp(MaxSizeNumber, 64.0, 8192.0);
	TArray<FColor> ResizedPixels;
	FIntPoint ResizedSize;
	if (!ResizePixelsMaxSize(Pixels, Size, MaxSize, ResizedPixels, ResizedSize, OutError))
	{
		return false;
	}
	bool bRequireNonBlack = true;
	JsonTryGetBool(Ctx.Params, TEXT("require_non_black"), bRequireNonBlack);
	if (bRequireNonBlack && UeAgentAIOps::CountNonBlackPixels(ResizedPixels) <= 0)
	{
		OutError = TEXT("asset_editor_screenshot_black");
		return false;
	}
	FString Format = TEXT("png");
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	Format = Format.ToLower();
	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(ResizedPixels, ResizedSize, Format, 90, OutPath, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetStringField(TEXT("capture_mode"), TEXT("offscreen_widget_renderer"));
	OutData->SetStringField(TEXT("legacy_backbuffer_capture"), TEXT("disabled"));
	OutData->SetBoolField(TEXT("backbuffer_capture_would_be_unsafe"), bBackbufferCaptureUnsafe);
	if (!BackbufferUnsafeReason.IsEmpty())
	{
		OutData->SetStringField(TEXT("backbuffer_unsafe_reason"), BackbufferUnsafeReason);
	}
	UeAgentAIOps::AddPixelStats(ResizedPixels, OutData);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString ControllerOrPawn;
	JsonTryGetString(Ctx.Params, TEXT("controller"), ControllerOrPawn);
	if (ControllerOrPawn.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("pawn"), ControllerOrPawn);
	}
	AActor* Actor = ControllerOrPawn.IsEmpty() ? nullptr : FindActorByNameOrLabel(World, ControllerOrPawn);
	UBehaviorTreeComponent* BTComp = nullptr;
	if (Actor)
	{
		BTComp = Actor->FindComponentByClass<UBehaviorTreeComponent>();
		if (!BTComp)
		{
			if (APawn* Pawn = Cast<APawn>(Actor))
			{
				if (AAIController* Controller = Cast<AAIController>(Pawn->GetController()))
				{
					BTComp = Controller->FindComponentByClass<UBehaviorTreeComponent>();
				}
			}
		}
	}
	else
	{
		for (TActorIterator<AAIController> It(World); It; ++It)
		{
			BTComp = It->FindComponentByClass<UBehaviorTreeComponent>();
			if (BTComp)
			{
				Actor = *It;
				break;
			}
		}
	}
	if (!BTComp)
	{
		OutError = TEXT("behavior_tree_component_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("owner"), Actor ? Actor->GetName() : FString());
	OutData->SetBoolField(TEXT("is_running"), BTComp->IsRunning());
	OutData->SetStringField(TEXT("current_tree"), BTComp->GetRootTree() ? BTComp->GetRootTree()->GetPathName() : FString());
	OutData->SetArrayField(TEXT("active_path"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetStringField(TEXT("running_task"), FString());
	OutData->SetArrayField(TEXT("service_status"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetStringField(TEXT("runtime_introspection"), TEXT("component_status_blackboard_values"));
	if (UBlackboardComponent* BlackboardComp = BTComp->GetBlackboardComponent())
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		if (UBlackboardData* BBAsset = BlackboardComp->GetBlackboardAsset())
		{
			for (int32 KeyIndex = 0; KeyIndex < BBAsset->GetNumKeys(); ++KeyIndex)
			{
				const FName KeyName = BBAsset->GetKeyName(static_cast<FBlackboard::FKey>(KeyIndex));
				TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
				ValueObj->SetStringField(TEXT("name"), KeyName.ToString());
				ValueObj->SetStringField(TEXT("value"), BlackboardComp->DescribeKeyValue(KeyName, EBlackboardDescription::OnlyValue));
				Values.Add(MakeShared<FJsonValueObject>(ValueObj));
			}
		}
		OutData->SetArrayField(TEXT("blackboard"), Values);
	}
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdBTNodeBlueprintGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	UClass* GeneratedClass = nullptr;
	if (Blueprint)
	{
		GeneratedClass = Blueprint->GeneratedClass;
	}
	else
	{
		GeneratedClass = Cast<UClass>(Asset);
	}
	OutData->SetStringField(TEXT("generated_class"), GeneratedClass ? GeneratedClass->GetPathName() : FString());
	OutData->SetStringField(TEXT("parent_class"), (GeneratedClass && GeneratedClass->GetSuperClass()) ? GeneratedClass->GetSuperClass()->GetPathName() : FString());
	OutData->SetStringField(TEXT("introspection_level"), TEXT("class_hierarchy_only"));
	OutData->SetBoolField(TEXT("is_bt_task"), GeneratedClass && GeneratedClass->IsChildOf(UBTTask_BlueprintBase::StaticClass()));
	OutData->SetBoolField(TEXT("is_bt_decorator"), GeneratedClass && GeneratedClass->IsChildOf(UBTDecorator_BlueprintBase::StaticClass()));
	OutData->SetBoolField(TEXT("is_bt_service"), GeneratedClass && GeneratedClass->IsChildOf(UBTService_BlueprintBase::StaticClass()));
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

namespace UeAgentAIOps
{
	static TSharedPtr<FJsonObject> ExportEditorNode(const FStateTreeEditorNode& Node)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Node.ID.IsValid() ? Node.ID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		Obj->SetStringField(TEXT("name"), Node.GetName().ToString());
		Obj->SetStringField(TEXT("node_struct"), Node.Node.GetScriptStruct() ? Node.Node.GetScriptStruct()->GetPathName() : FString());
		Obj->SetStringField(TEXT("instance_struct"), Node.Instance.GetScriptStruct() ? Node.Instance.GetScriptStruct()->GetPathName() : FString());
		Obj->SetStringField(TEXT("instance_object_class"), Node.InstanceObject ? Node.InstanceObject->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("instance_object_path"), Node.InstanceObject ? Node.InstanceObject->GetPathName() : FString());
		Obj->SetNumberField(TEXT("expression_indent"), Node.ExpressionIndent);
		Obj->SetStringField(TEXT("expression_operand"), EnumToString(Node.ExpressionOperand));
		return Obj;
	}

	static TSharedPtr<FJsonObject> ExportTransition(const FStateTreeTransition& Transition)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Transition.ID.IsValid() ? Transition.ID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		Obj->SetStringField(TEXT("trigger"), EnumToString(Transition.Trigger));
		Obj->SetStringField(TEXT("priority"), EnumToString(Transition.Priority));
#if WITH_EDITORONLY_DATA
		Obj->SetStringField(TEXT("type"), EnumToString(Transition.State.LinkType));
		Obj->SetStringField(TEXT("target_state_name"), Transition.State.Name.ToString());
		Obj->SetStringField(TEXT("target_state_id"), Transition.State.ID.IsValid() ? Transition.State.ID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
#endif
		Obj->SetStringField(TEXT("fallback"), EnumToString(Transition.State.Fallback));
		Obj->SetBoolField(TEXT("enabled"), Transition.bTransitionEnabled);
		Obj->SetBoolField(TEXT("delay_enabled"), Transition.bDelayTransition);
		Obj->SetNumberField(TEXT("delay_duration"), Transition.DelayDuration);
		Obj->SetNumberField(TEXT("delay_random_variance"), Transition.DelayRandomVariance);
		TArray<TSharedPtr<FJsonValue>> Conditions;
		for (const FStateTreeEditorNode& Condition : Transition.Conditions)
		{
			Conditions.Add(MakeShared<FJsonValueObject>(ExportEditorNode(Condition)));
		}
		Obj->SetArrayField(TEXT("conditions"), Conditions);
		return Obj;
	}

	static TSharedPtr<FJsonObject> ExportStateTreeState(const UStateTreeState* State, const FString& ParentId, TArray<TSharedPtr<FJsonValue>>& OutFlatStates)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!State)
		{
			return Obj;
		}
		const FString StateId = State->ID.IsValid() ? State->ID.ToString(EGuidFormats::DigitsWithHyphens) : State->GetName();
		Obj->SetStringField(TEXT("id"), StateId);
		Obj->SetStringField(TEXT("parent_id"), ParentId);
		Obj->SetStringField(TEXT("name"), State->Name.ToString());
		Obj->SetStringField(TEXT("description"), State->Description);
		Obj->SetStringField(TEXT("path"), State->GetPath());
		Obj->SetStringField(TEXT("type"), EnumToString(State->Type));
		Obj->SetStringField(TEXT("selection_behavior"), EnumToString(State->SelectionBehavior));
		Obj->SetStringField(TEXT("tasks_completion"), EnumToString(State->TasksCompletion));
		Obj->SetBoolField(TEXT("enabled"), State->bEnabled);
		Obj->SetStringField(TEXT("tag"), State->Tag.IsValid() ? State->Tag.ToString() : FString());

		TArray<TSharedPtr<FJsonValue>> Conditions;
		for (const FStateTreeEditorNode& Condition : State->EnterConditions)
		{
			Conditions.Add(MakeShared<FJsonValueObject>(ExportEditorNode(Condition)));
		}
		Obj->SetArrayField(TEXT("enter_conditions"), Conditions);

		TArray<TSharedPtr<FJsonValue>> Tasks;
		for (const FStateTreeEditorNode& Task : State->Tasks)
		{
			Tasks.Add(MakeShared<FJsonValueObject>(ExportEditorNode(Task)));
		}
		Obj->SetArrayField(TEXT("tasks"), Tasks);

		TArray<TSharedPtr<FJsonValue>> Transitions;
		for (const FStateTreeTransition& Transition : State->Transitions)
		{
			Transitions.Add(MakeShared<FJsonValueObject>(ExportTransition(Transition)));
		}
		Obj->SetArrayField(TEXT("transitions"), Transitions);

		TArray<TSharedPtr<FJsonValue>> ChildIds;
		for (const UStateTreeState* Child : State->Children)
		{
			if (Child)
			{
				ChildIds.Add(MakeShared<FJsonValueString>(Child->ID.IsValid() ? Child->ID.ToString(EGuidFormats::DigitsWithHyphens) : Child->GetName()));
			}
		}
		Obj->SetArrayField(TEXT("children"), ChildIds);
		OutFlatStates.Add(MakeShared<FJsonValueObject>(Obj));

		for (const UStateTreeState* Child : State->Children)
		{
			ExportStateTreeState(Child, StateId, OutFlatStates);
		}
		return Obj;
	}

	static UStateTreeEditorData* GetStateTreeEditorData(UStateTree* Tree)
	{
#if WITH_EDITORONLY_DATA
		return Tree ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
#else
		return nullptr;
#endif
	}

	static bool EnsureStateTreeEditorData(UStateTree* Tree, UClass* SchemaClass, FString& OutError)
	{
		if (!Tree)
		{
			OutError = TEXT("state_tree_not_found");
			return false;
		}
#if WITH_EDITORONLY_DATA
		UStateTreeEditorData* EditorData = GetStateTreeEditorData(Tree);
		if (!EditorData)
		{
			EditorData = NewObject<UStateTreeEditorData>(Tree, UStateTreeEditorData::StaticClass(), FName(), RF_Transactional);
			Tree->EditorData = EditorData;
		}
		if (!SchemaClass)
		{
			SchemaClass = UStateTreeComponentSchema::StaticClass();
		}
		if (!SchemaClass->IsChildOf(UStateTreeSchema::StaticClass()))
		{
			OutError = TEXT("invalid_state_tree_schema_class");
			return false;
		}
		if (!EditorData->Schema || EditorData->Schema->GetClass() != SchemaClass)
		{
			EditorData->Schema = NewObject<UStateTreeSchema>(EditorData, SchemaClass, FName(), RF_Transactional);
		}
		if (EditorData->SubTrees.Num() == 0)
		{
			UStateTreeState& Root = EditorData->AddRootState();
			Root.ID = FGuid::NewGuid();
		}
		return true;
#else
		OutError = TEXT("state_tree_editor_data_unavailable");
		return false;
#endif
	}

	static bool CompileStateTree(UStateTree* Tree, TSharedPtr<FJsonObject>& OutCompile)
	{
		OutCompile = MakeShared<FJsonObject>();
#if WITH_EDITOR
		FStateTreeCompilerLog CompileLog;
		const bool bCompiled = Tree ? UStateTreeEditingSubsystem::CompileStateTree(Tree, CompileLog) : false;
		OutCompile->SetBoolField(TEXT("compiled"), bCompiled);
		OutCompile->SetBoolField(TEXT("ready_to_run"), Tree && Tree->IsReadyToRun());
		return bCompiled;
#else
		OutCompile->SetBoolField(TEXT("compiled"), false);
		OutCompile->SetStringField(TEXT("reason"), TEXT("editor_unavailable"));
		return false;
#endif
	}

	static TSharedPtr<FJsonObject> ExportStateTree(UStateTree* Tree, const bool bIncludeStates)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), StateTreeProfile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		Root->SetStringField(TEXT("asset_path"), Tree ? Tree->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), Tree ? Tree->GetPathName() : FString());
		Root->SetBoolField(TEXT("ready_to_run"), Tree && Tree->IsReadyToRun());
		Root->SetBoolField(TEXT("dirty"), Tree && Tree->GetOutermost()->IsDirty());
		UStateTreeEditorData* EditorData = GetStateTreeEditorData(Tree);
		Root->SetStringField(TEXT("schema_class"), (EditorData && EditorData->Schema) ? EditorData->Schema->GetClass()->GetPathName() : FString());
		if (!EditorData)
		{
			Root->SetNumberField(TEXT("state_count"), 0);
			return Root;
		}

		TArray<TSharedPtr<FJsonValue>> Evaluators;
		for (const FStateTreeEditorNode& Evaluator : EditorData->Evaluators)
		{
			Evaluators.Add(MakeShared<FJsonValueObject>(ExportEditorNode(Evaluator)));
		}
		Root->SetArrayField(TEXT("evaluators"), Evaluators);

		TArray<TSharedPtr<FJsonValue>> GlobalTasks;
		for (const FStateTreeEditorNode& Task : EditorData->GlobalTasks)
		{
			GlobalTasks.Add(MakeShared<FJsonValueObject>(ExportEditorNode(Task)));
		}
		Root->SetArrayField(TEXT("global_tasks"), GlobalTasks);

		if (bIncludeStates)
		{
			TArray<TSharedPtr<FJsonValue>> States;
			for (const UStateTreeState* SubTree : EditorData->SubTrees)
			{
				ExportStateTreeState(SubTree, FString(), States);
			}
			Root->SetArrayField(TEXT("states"), States);
			Root->SetNumberField(TEXT("state_count"), States.Num());
		}
		return Root;
	}

	static bool ValidateStateTreeFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& StatesJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!AssetJson.IsValid())
		{
			AddIssue(Issues, TEXT("missing_required_file"), TEXT("Missing asset.json."), TEXT("asset.json"));
		}
		const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
		if (!StatesJson.IsValid() || !StatesJson->TryGetArrayField(TEXT("states"), States) || !States)
		{
			AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing states array."), TEXT("states.json.states"));
			return false;
		}
		TSet<FString> Ids;
		for (int32 Index = 0; Index < States->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* StateObjPtr = nullptr;
			if (!(*States)[Index].IsValid() || !(*States)[Index]->TryGetObject(StateObjPtr) || !StateObjPtr || !StateObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_state_item"), TEXT("State item must be an object."), FString::Printf(TEXT("states.json.states[%d]"), Index));
				continue;
			}
			FString Id;
			FString Name;
			(*StateObjPtr)->TryGetStringField(TEXT("id"), Id);
			(*StateObjPtr)->TryGetStringField(TEXT("name"), Name);
			if (Id.IsEmpty())
			{
				AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing state id."), FString::Printf(TEXT("states.json.states[%d].id"), Index));
			}
			else if (Ids.Contains(Id))
			{
				AddIssue(Issues, TEXT("duplicate_id"), FString::Printf(TEXT("Duplicate state id '%s'."), *Id), FString::Printf(TEXT("states.json.states[%d].id"), Index));
			}
			Ids.Add(Id);
			if (Name.IsEmpty())
			{
				AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing state name."), FString::Printf(TEXT("states.json.states[%d].name"), Index));
			}
		}
		return Issues.Num() == 0;
	}

	static bool ConfigureStateTreeEditorNode(
		FStateTreeEditorNode& EditorNode,
		UObject* Outer,
		const TSharedPtr<FJsonObject>& NodeObj,
		UScriptStruct* RequiredBaseStruct,
		UClass* BlueprintBaseClass,
		TArray<TSharedPtr<FJsonValue>>& PropertyResults,
		FString& OutError)
	{
		if (!NodeObj.IsValid())
		{
			OutError = TEXT("invalid_state_tree_node");
			return false;
		}
		FString ClassOrStruct;
		NodeObj->TryGetStringField(TEXT("node_struct"), ClassOrStruct);
		if (ClassOrStruct.IsEmpty())
		{
			NodeObj->TryGetStringField(TEXT("class"), ClassOrStruct);
		}
		if (ClassOrStruct.IsEmpty())
		{
			OutError = TEXT("missing_state_tree_node_class");
			return false;
		}

		if (UScriptStruct* NodeStruct = ResolveScriptStruct(ClassOrStruct))
		{
			if (!NodeStruct->IsChildOf(RequiredBaseStruct))
			{
				OutError = TEXT("state_tree_node_struct_base_mismatch");
				return false;
			}
			EditorNode.Reset();
			EditorNode.ID = FGuid::NewGuid();
			EditorNode.Node.InitializeAs(NodeStruct);
			if (const FStateTreeNodeBase* NodeBase = reinterpret_cast<const FStateTreeNodeBase*>(EditorNode.Node.GetMemory()))
			{
				if (const UScriptStruct* InstanceType = Cast<const UScriptStruct>(NodeBase->GetInstanceDataType()))
				{
					EditorNode.Instance.InitializeAs(InstanceType);
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (NodeObj->TryGetArrayField(TEXT("properties"), Properties) && Properties)
			{
				if (!ApplyStructProperties(NodeStruct, EditorNode.Node.GetMutableMemory(), Outer, Properties, PropertyResults, OutError))
				{
					return false;
				}
			}
			return true;
		}

		UClass* BlueprintClass = ResolveClass(ClassOrStruct);
		if (!BlueprintClass || !BlueprintClass->IsChildOf(BlueprintBaseClass))
		{
			OutError = TEXT("state_tree_node_class_not_found");
			return false;
		}
		EditorNode.Reset();
		EditorNode.ID = FGuid::NewGuid();
		if (BlueprintClass->IsChildOf(UStateTreeTaskBlueprintBase::StaticClass()))
		{
			EditorNode.Node.InitializeAs<FStateTreeBlueprintTaskWrapper>();
			EditorNode.Node.GetMutable<FStateTreeBlueprintTaskWrapper>().TaskClass = BlueprintClass;
			EditorNode.InstanceObject = NewObject<UStateTreeTaskBlueprintBase>(Outer, BlueprintClass, NAME_None, RF_Transactional);
		}
		else if (BlueprintClass->IsChildOf(UStateTreeConditionBlueprintBase::StaticClass()))
		{
			EditorNode.Node.InitializeAs<FStateTreeBlueprintConditionWrapper>();
			EditorNode.Node.GetMutable<FStateTreeBlueprintConditionWrapper>().ConditionClass = BlueprintClass;
			EditorNode.InstanceObject = NewObject<UStateTreeConditionBlueprintBase>(Outer, BlueprintClass, NAME_None, RF_Transactional);
		}
		else if (BlueprintClass->IsChildOf(UStateTreeEvaluatorBlueprintBase::StaticClass()))
		{
			EditorNode.Node.InitializeAs<FStateTreeBlueprintEvaluatorWrapper>();
			EditorNode.Node.GetMutable<FStateTreeBlueprintEvaluatorWrapper>().EvaluatorClass = BlueprintClass;
			EditorNode.InstanceObject = NewObject<UStateTreeEvaluatorBlueprintBase>(Outer, BlueprintClass, NAME_None, RF_Transactional);
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (EditorNode.InstanceObject && NodeObj->TryGetArrayField(TEXT("properties"), Properties) && Properties)
		{
			if (!ApplyProperties(EditorNode.InstanceObject, Properties, PropertyResults, OutError))
			{
				return false;
			}
		}
		return true;
	}

	static EStateTreeStateType ParseStateType(const FString& Text)
	{
		return EnumFromString(Text, EStateTreeStateType::State);
	}

	static TSharedPtr<FJsonObject> ExportEqs(UEnvQuery* Query, const bool bIncludeOptions)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), EqsProfile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		Root->SetStringField(TEXT("asset_path"), Query ? Query->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), Query ? Query->GetPathName() : FString());
		Root->SetBoolField(TEXT("dirty"), Query && Query->GetOutermost()->IsDirty());
		TArray<TSharedPtr<FJsonValue>> Options;
		if (Query && bIncludeOptions)
		{
			const TArray<UEnvQueryOption*>& QueryOptions = Query->GetOptions();
			for (int32 OptionIndex = 0; OptionIndex < QueryOptions.Num(); ++OptionIndex)
			{
				const UEnvQueryOption* Option = QueryOptions[OptionIndex];
				TSharedPtr<FJsonObject> OptionObj = MakeShared<FJsonObject>();
				OptionObj->SetStringField(TEXT("id"), FString::Printf(TEXT("option_%d"), OptionIndex));
				OptionObj->SetStringField(TEXT("generator_class"), (Option && Option->Generator) ? Option->Generator->GetClass()->GetPathName() : FString());
				OptionObj->SetStringField(TEXT("generator_name"), (Option && Option->Generator) ? Option->Generator->GetName() : FString());
				TArray<TSharedPtr<FJsonValue>> Tests;
				if (Option)
				{
					for (int32 TestIndex = 0; TestIndex < Option->Tests.Num(); ++TestIndex)
					{
						const UEnvQueryTest* Test = Option->Tests[TestIndex];
						TSharedPtr<FJsonObject> TestObj = MakeShared<FJsonObject>();
						TestObj->SetStringField(TEXT("id"), FString::Printf(TEXT("test_%d"), TestIndex));
						TestObj->SetStringField(TEXT("class"), Test ? Test->GetClass()->GetPathName() : FString());
						TestObj->SetStringField(TEXT("name"), Test ? Test->GetName() : FString());
						Tests.Add(MakeShared<FJsonValueObject>(TestObj));
					}
				}
				OptionObj->SetArrayField(TEXT("tests"), Tests);
				Options.Add(MakeShared<FJsonValueObject>(OptionObj));
			}
		}
		Root->SetArrayField(TEXT("options"), Options);
		Root->SetNumberField(TEXT("option_count"), Options.Num());
		return Root;
	}

	static bool ValidateEqsFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& OptionsJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!AssetJson.IsValid())
		{
			AddIssue(Issues, TEXT("missing_required_file"), TEXT("Missing asset.json."), TEXT("asset.json"));
		}
		const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
		if (!OptionsJson.IsValid() || !OptionsJson->TryGetArrayField(TEXT("options"), Options) || !Options)
		{
			AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing options array."), TEXT("options.json.options"));
			return false;
		}
		for (int32 Index = 0; Index < Options->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* OptionObjPtr = nullptr;
			if (!(*Options)[Index].IsValid() || !(*Options)[Index]->TryGetObject(OptionObjPtr) || !OptionObjPtr || !OptionObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_option_item"), TEXT("Option item must be an object."), FString::Printf(TEXT("options.json.options[%d]"), Index));
				continue;
			}
			FString GeneratorClass;
			(*OptionObjPtr)->TryGetStringField(TEXT("generator_class"), GeneratorClass);
			if (!GeneratorClass.IsEmpty())
			{
				UClass* Class = ResolveClass(GeneratorClass);
				if (!Class || !Class->IsChildOf(UEnvQueryGenerator::StaticClass()))
				{
					AddIssue(Issues, TEXT("eqs_generator_class_not_found"), FString::Printf(TEXT("Invalid EQS generator '%s'."), *GeneratorClass), FString::Printf(TEXT("options.json.options[%d].generator_class"), Index));
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Tests = nullptr;
			if ((*OptionObjPtr)->TryGetArrayField(TEXT("tests"), Tests) && Tests)
			{
				for (int32 TestIndex = 0; TestIndex < Tests->Num(); ++TestIndex)
				{
					const TSharedPtr<FJsonObject>* TestObjPtr = nullptr;
					if (!(*Tests)[TestIndex].IsValid() || !(*Tests)[TestIndex]->TryGetObject(TestObjPtr) || !TestObjPtr || !TestObjPtr->IsValid())
					{
						AddIssue(Issues, TEXT("invalid_test_item"), TEXT("Test item must be an object."), FString::Printf(TEXT("options.json.options[%d].tests[%d]"), Index, TestIndex));
						continue;
					}
					FString TestClass;
					(*TestObjPtr)->TryGetStringField(TEXT("class"), TestClass);
					if (!TestClass.IsEmpty())
					{
						UClass* Class = ResolveClass(TestClass);
						if (!Class || !Class->IsChildOf(UEnvQueryTest::StaticClass()))
						{
							AddIssue(Issues, TEXT("eqs_test_class_not_found"), FString::Printf(TEXT("Invalid EQS test '%s'."), *TestClass), FString::Printf(TEXT("options.json.options[%d].tests[%d].class"), Index, TestIndex));
						}
					}
				}
			}
		}
		return Issues.Num() == 0;
	}

	static EEnvQueryRunMode::Type ParseEqsRunMode(const FString& Text)
	{
		const FString Lower = Text.ToLower();
		if (Lower == TEXT("single") || Lower == TEXT("single_result")) return EEnvQueryRunMode::SingleResult;
		if (Lower == TEXT("random_best_5_pct") || Lower == TEXT("randombest5pct")) return EEnvQueryRunMode::RandomBest5Pct;
		if (Lower == TEXT("random_best_25_pct") || Lower == TEXT("randombest25pct")) return EEnvQueryRunMode::RandomBest25Pct;
		return EEnvQueryRunMode::AllMatching;
	}

	static TSharedPtr<FJsonObject> ExportSmartObjectDefinition(USmartObjectDefinition* Definition, const bool bIncludeSlots)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), SmartObjectDefinitionProfile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		Root->SetStringField(TEXT("asset_path"), Definition ? Definition->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), Definition ? Definition->GetPathName() : FString());
		Root->SetBoolField(TEXT("dirty"), Definition && Definition->GetOutermost()->IsDirty());
		Root->SetArrayField(TEXT("activity_tags"), Definition ? GameplayTagsToJson(Definition->GetActivityTags()) : TArray<TSharedPtr<FJsonValue>>());
		Root->SetArrayField(TEXT("tags"), Definition ? GameplayTagsToJson(Definition->GetActivityTags()) : TArray<TSharedPtr<FJsonValue>>());
		auto ExportBehaviorDefinition = [](const USmartObjectBehaviorDefinition* Behavior, const FString& Id)
		{
			TSharedPtr<FJsonObject> BehaviorObj = MakeShared<FJsonObject>();
			BehaviorObj->SetStringField(TEXT("id"), Id);
			BehaviorObj->SetStringField(TEXT("class"), Behavior ? Behavior->GetClass()->GetPathName() : FString());
			BehaviorObj->SetStringField(TEXT("name"), Behavior ? Behavior->GetName() : FString());
			return BehaviorObj;
		};
		TArray<TSharedPtr<FJsonValue>> DefaultBehaviors;
		if (Definition)
		{
			if (FArrayProperty* DefaultsProperty = FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("DefaultBehaviorDefinitions")))
			{
				if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(DefaultsProperty->Inner))
				{
					void* DefaultsPtr = DefaultsProperty->ContainerPtrToValuePtr<void>(Definition);
					FScriptArrayHelper Helper(DefaultsProperty, DefaultsPtr);
					for (int32 Index = 0; Index < Helper.Num(); ++Index)
					{
						const USmartObjectBehaviorDefinition* Behavior = Cast<USmartObjectBehaviorDefinition>(ObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index)));
						DefaultBehaviors.Add(MakeShared<FJsonValueObject>(ExportBehaviorDefinition(Behavior, FString::Printf(TEXT("default_behavior_%d"), Index))));
					}
				}
			}
		}
		Root->SetArrayField(TEXT("default_behavior_definitions"), DefaultBehaviors);
		TArray<TSharedPtr<FJsonValue>> Slots;
		int32 SlotBehaviorCount = 0;
		if (Definition && bIncludeSlots)
		{
			const TConstArrayView<FSmartObjectSlotDefinition> DefinitionSlots = Definition->GetSlots();
			for (int32 Index = 0; Index < DefinitionSlots.Num(); ++Index)
			{
				const FSmartObjectSlotDefinition& Slot = DefinitionSlots[Index];
				TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
				SlotObj->SetStringField(TEXT("id"), FString::Printf(TEXT("slot_%d"), Index));
#if WITH_EDITORONLY_DATA
				SlotObj->SetStringField(TEXT("name"), Slot.Name.ToString());
#endif
				SlotObj->SetObjectField(TEXT("offset"), VecToJson(FVector(Slot.Offset)));
				SlotObj->SetObjectField(TEXT("rotation"), RotToJson(FRotator(Slot.Rotation)));
				SlotObj->SetObjectField(TEXT("local_transform"), TransformToJson(FTransform(FRotator(Slot.Rotation), FVector(Slot.Offset), FVector::OneVector)));
				SlotObj->SetBoolField(TEXT("enabled"), Slot.bEnabled);
				SlotObj->SetArrayField(TEXT("activity_tags"), GameplayTagsToJson(Slot.ActivityTags));
				SlotObj->SetArrayField(TEXT("tags"), GameplayTagsToJson(Slot.ActivityTags));
				SlotObj->SetArrayField(TEXT("runtime_tags"), GameplayTagsToJson(Slot.RuntimeTags));
				TArray<TSharedPtr<FJsonValue>> Behaviors;
				for (int32 BehaviorIndex = 0; BehaviorIndex < Slot.BehaviorDefinitions.Num(); ++BehaviorIndex)
				{
					const USmartObjectBehaviorDefinition* Behavior = Slot.BehaviorDefinitions[BehaviorIndex];
					Behaviors.Add(MakeShared<FJsonValueObject>(ExportBehaviorDefinition(Behavior, FString::Printf(TEXT("slot_%d_behavior_%d"), Index, BehaviorIndex))));
				}
				SlotBehaviorCount += Behaviors.Num();
				SlotObj->SetArrayField(TEXT("behavior_definitions"), Behaviors);
				SlotObj->SetNumberField(TEXT("behavior_definition_count"), Behaviors.Num());
				Slots.Add(MakeShared<FJsonValueObject>(SlotObj));
			}
		}
		Root->SetArrayField(TEXT("slots"), Slots);
		Root->SetNumberField(TEXT("slot_count"), Slots.Num());
		Root->SetNumberField(TEXT("default_behavior_definition_count"), DefaultBehaviors.Num());
		Root->SetNumberField(TEXT("behavior_definition_count"), DefaultBehaviors.Num() + SlotBehaviorCount);
		if (Definition)
		{
			TArray<TPair<EMessageSeverity::Type, FText>> Errors;
			const bool bValid = Definition->Validate(&Errors);
			TArray<TSharedPtr<FJsonValue>> ValidationErrors;
			for (const TPair<EMessageSeverity::Type, FText>& Pair : Errors)
			{
				TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
				ErrorObj->SetNumberField(TEXT("severity"), static_cast<int32>(Pair.Key));
				ErrorObj->SetStringField(TEXT("message"), Pair.Value.ToString());
				ValidationErrors.Add(MakeShared<FJsonValueObject>(ErrorObj));
			}
			Root->SetBoolField(TEXT("definition_valid"), bValid);
			Root->SetArrayField(TEXT("validation_messages"), ValidationErrors);
		}
		return Root;
	}

	static bool ValidateSmartObjectDefinitionJson(const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Root.IsValid())
		{
			AddIssue(Issues, TEXT("invalid_json_root"), TEXT("Root must be an object."), TEXT("$"));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
		if (!Root->TryGetArrayField(TEXT("slots"), Slots) || !Slots)
		{
			AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing slots array."), TEXT("$.slots"));
			return false;
		}
		TSet<FString> Names;
		auto ValidateBehaviorDefinitions = [&Issues](const TArray<TSharedPtr<FJsonValue>>* Behaviors, const FString& Path)
		{
			if (!Behaviors)
			{
				return;
			}
			for (int32 BehaviorIndex = 0; BehaviorIndex < Behaviors->Num(); ++BehaviorIndex)
			{
				const TSharedPtr<FJsonObject>* BehaviorObjPtr = nullptr;
				if (!(*Behaviors)[BehaviorIndex].IsValid() || !(*Behaviors)[BehaviorIndex]->TryGetObject(BehaviorObjPtr) || !BehaviorObjPtr || !BehaviorObjPtr->IsValid())
				{
					AddIssue(Issues, TEXT("invalid_behavior_definition_item"), TEXT("Behavior definition item must be an object."), FString::Printf(TEXT("%s[%d]"), *Path, BehaviorIndex));
					continue;
				}
				FString BehaviorClassPath;
				if (!(*BehaviorObjPtr)->TryGetStringField(TEXT("class"), BehaviorClassPath) || BehaviorClassPath.IsEmpty())
				{
					AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing behavior definition class."), FString::Printf(TEXT("%s[%d].class"), *Path, BehaviorIndex));
					continue;
				}
				UClass* BehaviorClass = ResolveClass(BehaviorClassPath);
				if (!BehaviorClass || !BehaviorClass->IsChildOf(USmartObjectBehaviorDefinition::StaticClass()) || BehaviorClass->HasAnyClassFlags(CLASS_Abstract))
				{
					AddIssue(Issues, TEXT("smart_object_behavior_definition_invalid"), FString::Printf(TEXT("Invalid behavior definition class '%s'."), *BehaviorClassPath), FString::Printf(TEXT("%s[%d].class"), *Path, BehaviorIndex));
				}
			}
		};
		const TArray<TSharedPtr<FJsonValue>>* DefaultBehaviors = nullptr;
		if (Root->TryGetArrayField(TEXT("default_behavior_definitions"), DefaultBehaviors))
		{
			ValidateBehaviorDefinitions(DefaultBehaviors, TEXT("$.default_behavior_definitions"));
		}
		for (int32 Index = 0; Index < Slots->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* SlotObjPtr = nullptr;
			if (!(*Slots)[Index].IsValid() || !(*Slots)[Index]->TryGetObject(SlotObjPtr) || !SlotObjPtr || !SlotObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_slot_item"), TEXT("Slot item must be an object."), FString::Printf(TEXT("$.slots[%d]"), Index));
				continue;
			}
			FString Name;
			(*SlotObjPtr)->TryGetStringField(TEXT("name"), Name);
			if (!Name.IsEmpty())
			{
				if (Names.Contains(Name))
				{
					AddIssue(Issues, TEXT("duplicate_id"), FString::Printf(TEXT("Duplicate slot name '%s'."), *Name), FString::Printf(TEXT("$.slots[%d].name"), Index));
				}
				Names.Add(Name);
			}
			const TArray<TSharedPtr<FJsonValue>>* Behaviors = nullptr;
			if ((*SlotObjPtr)->TryGetArrayField(TEXT("behavior_definitions"), Behaviors))
			{
				ValidateBehaviorDefinitions(Behaviors, FString::Printf(TEXT("$.slots[%d].behavior_definitions"), Index));
			}
		}
		return Issues.Num() == 0;
	}

	static void BuildStateTreeSplitJson(
		const TSharedPtr<FJsonObject>& Exported,
		TSharedPtr<FJsonObject>& OutSchema,
		TSharedPtr<FJsonObject>& OutParameters,
		TSharedPtr<FJsonObject>& OutTransitions,
		TSharedPtr<FJsonObject>& OutTasks,
		TSharedPtr<FJsonObject>& OutConditions,
		TSharedPtr<FJsonObject>& OutEvaluators,
		TSharedPtr<FJsonObject>& OutBindings,
		TSharedPtr<FJsonObject>& OutGlobalTasks)
	{
		const FString Profile(StateTreeProfile);
		TArray<TSharedPtr<FJsonValue>> Empty;
		FString AssetPath;
		FString ObjectPath;
		FString SchemaClass;
		if (Exported.IsValid())
		{
			Exported->TryGetStringField(TEXT("asset_path"), AssetPath);
			Exported->TryGetStringField(TEXT("object_path"), ObjectPath);
			Exported->TryGetStringField(TEXT("schema_class"), SchemaClass);
		}
		OutSchema = MakeShared<FJsonObject>();
		OutSchema->SetStringField(TEXT("profile"), Profile);
		OutSchema->SetNumberField(TEXT("schema_version"), 1);
		OutSchema->SetStringField(TEXT("asset_path"), AssetPath);
		OutSchema->SetStringField(TEXT("object_path"), ObjectPath);
		OutSchema->SetStringField(TEXT("schema_class"), SchemaClass);
		OutSchema->SetArrayField(TEXT("context_data"), Empty);

		TArray<TSharedPtr<FJsonValue>> Transitions;
		TArray<TSharedPtr<FJsonValue>> Tasks;
		TArray<TSharedPtr<FJsonValue>> Conditions;
		const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
		if (Exported.IsValid() && Exported->TryGetArrayField(TEXT("states"), States) && States)
		{
			for (const TSharedPtr<FJsonValue>& StateValue : *States)
			{
				const TSharedPtr<FJsonObject>* StateObjPtr = nullptr;
				if (!StateValue.IsValid() || !StateValue->TryGetObject(StateObjPtr) || !StateObjPtr || !StateObjPtr->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>& StateObj = *StateObjPtr;
				FString StateId;
				StateObj->TryGetStringField(TEXT("id"), StateId);
				const TArray<TSharedPtr<FJsonValue>>* StateTasks = nullptr;
				if (StateObj->TryGetArrayField(TEXT("tasks"), StateTasks) && StateTasks)
				{
					for (const TSharedPtr<FJsonValue>& TaskValue : *StateTasks)
					{
						const TSharedPtr<FJsonObject>* TaskObjPtr = nullptr;
						if (TaskValue.IsValid() && TaskValue->TryGetObject(TaskObjPtr) && TaskObjPtr && TaskObjPtr->IsValid())
						{
							TSharedPtr<FJsonObject> TaskCopy = MakeShared<FJsonObject>(**TaskObjPtr);
							TaskCopy->SetStringField(TEXT("state_id"), StateId);
							Tasks.Add(MakeShared<FJsonValueObject>(TaskCopy));
						}
					}
				}
				const TArray<TSharedPtr<FJsonValue>>* EnterConditions = nullptr;
				if (StateObj->TryGetArrayField(TEXT("enter_conditions"), EnterConditions) && EnterConditions)
				{
					for (const TSharedPtr<FJsonValue>& ConditionValue : *EnterConditions)
					{
						const TSharedPtr<FJsonObject>* ConditionObjPtr = nullptr;
						if (ConditionValue.IsValid() && ConditionValue->TryGetObject(ConditionObjPtr) && ConditionObjPtr && ConditionObjPtr->IsValid())
						{
							TSharedPtr<FJsonObject> ConditionCopy = MakeShared<FJsonObject>(**ConditionObjPtr);
							ConditionCopy->SetStringField(TEXT("state_id"), StateId);
							ConditionCopy->SetStringField(TEXT("condition_scope"), TEXT("enter"));
							Conditions.Add(MakeShared<FJsonValueObject>(ConditionCopy));
						}
					}
				}
				const TArray<TSharedPtr<FJsonValue>>* StateTransitions = nullptr;
				if (StateObj->TryGetArrayField(TEXT("transitions"), StateTransitions) && StateTransitions)
				{
					for (const TSharedPtr<FJsonValue>& TransitionValue : *StateTransitions)
					{
						const TSharedPtr<FJsonObject>* TransitionObjPtr = nullptr;
						if (!TransitionValue.IsValid() || !TransitionValue->TryGetObject(TransitionObjPtr) || !TransitionObjPtr || !TransitionObjPtr->IsValid())
						{
							continue;
						}
						TSharedPtr<FJsonObject> TransitionCopy = MakeShared<FJsonObject>(**TransitionObjPtr);
						TransitionCopy->SetStringField(TEXT("source_state"), StateId);
						FString TransitionId;
						TransitionCopy->TryGetStringField(TEXT("id"), TransitionId);
						Transitions.Add(MakeShared<FJsonValueObject>(TransitionCopy));

						const TArray<TSharedPtr<FJsonValue>>* TransitionConditions = nullptr;
						if ((*TransitionObjPtr)->TryGetArrayField(TEXT("conditions"), TransitionConditions) && TransitionConditions)
						{
							for (const TSharedPtr<FJsonValue>& ConditionValue : *TransitionConditions)
							{
								const TSharedPtr<FJsonObject>* ConditionObjPtr = nullptr;
								if (ConditionValue.IsValid() && ConditionValue->TryGetObject(ConditionObjPtr) && ConditionObjPtr && ConditionObjPtr->IsValid())
								{
									TSharedPtr<FJsonObject> ConditionCopy = MakeShared<FJsonObject>(**ConditionObjPtr);
									ConditionCopy->SetStringField(TEXT("state_id"), StateId);
									ConditionCopy->SetStringField(TEXT("transition_id"), TransitionId);
									ConditionCopy->SetStringField(TEXT("condition_scope"), TEXT("transition"));
									Conditions.Add(MakeShared<FJsonValueObject>(ConditionCopy));
								}
							}
						}
					}
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Evaluators = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* GlobalTasks = nullptr;
		OutParameters = MakeProfileArrayJson(Profile, TEXT("parameters"), Empty);
		OutTransitions = MakeProfileArrayJson(Profile, TEXT("transitions"), Transitions);
		OutTasks = MakeProfileArrayJson(Profile, TEXT("tasks"), Tasks);
		OutConditions = MakeProfileArrayJson(Profile, TEXT("conditions"), Conditions);
		OutEvaluators = MakeProfileArrayJson(Profile, TEXT("evaluators"), (Exported.IsValid() && Exported->TryGetArrayField(TEXT("evaluators"), Evaluators) && Evaluators) ? *Evaluators : Empty);
		OutBindings = MakeProfileArrayJson(Profile, TEXT("bindings"), Empty);
		OutGlobalTasks = MakeProfileArrayJson(Profile, TEXT("global_tasks"), (Exported.IsValid() && Exported->TryGetArrayField(TEXT("global_tasks"), GlobalTasks) && GlobalTasks) ? *GlobalTasks : Empty);
	}

	static void BuildEqsSplitJson(
		const TSharedPtr<FJsonObject>& Exported,
		TSharedPtr<FJsonObject>& OutGenerators,
		TSharedPtr<FJsonObject>& OutTests,
		TSharedPtr<FJsonObject>& OutContexts)
	{
		const FString Profile(EqsProfile);
		TArray<TSharedPtr<FJsonValue>> Generators;
		TArray<TSharedPtr<FJsonValue>> Tests;
		TArray<TSharedPtr<FJsonValue>> Contexts;
		const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
		if (Exported.IsValid() && Exported->TryGetArrayField(TEXT("options"), Options) && Options)
		{
			for (int32 OptionIndex = 0; OptionIndex < Options->Num(); ++OptionIndex)
			{
				const TSharedPtr<FJsonObject>* OptionObjPtr = nullptr;
				if (!(*Options)[OptionIndex].IsValid() || !(*Options)[OptionIndex]->TryGetObject(OptionObjPtr) || !OptionObjPtr || !OptionObjPtr->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>& OptionObj = *OptionObjPtr;
				FString OptionId;
				OptionObj->TryGetStringField(TEXT("id"), OptionId);
				if (OptionId.IsEmpty())
				{
					OptionId = FString::Printf(TEXT("option_%d"), OptionIndex);
				}
				FString GeneratorClass;
				FString GeneratorName;
				OptionObj->TryGetStringField(TEXT("generator_class"), GeneratorClass);
				OptionObj->TryGetStringField(TEXT("generator_name"), GeneratorName);
				TSharedPtr<FJsonObject> GeneratorObj = MakeShared<FJsonObject>();
				GeneratorObj->SetStringField(TEXT("id"), OptionId + TEXT("_generator"));
				GeneratorObj->SetStringField(TEXT("option_id"), OptionId);
				GeneratorObj->SetStringField(TEXT("class"), GeneratorClass);
				GeneratorObj->SetStringField(TEXT("name"), GeneratorName);
				Generators.Add(MakeShared<FJsonValueObject>(GeneratorObj));

				const TArray<TSharedPtr<FJsonValue>>* OptionTests = nullptr;
				if (OptionObj->TryGetArrayField(TEXT("tests"), OptionTests) && OptionTests)
				{
					for (int32 TestIndex = 0; TestIndex < OptionTests->Num(); ++TestIndex)
					{
						const TSharedPtr<FJsonObject>* TestObjPtr = nullptr;
						if (OptionTests->IsValidIndex(TestIndex) && (*OptionTests)[TestIndex].IsValid() && (*OptionTests)[TestIndex]->TryGetObject(TestObjPtr) && TestObjPtr && TestObjPtr->IsValid())
						{
							TSharedPtr<FJsonObject> TestCopy = MakeShared<FJsonObject>(**TestObjPtr);
							FString TestId;
							TestCopy->TryGetStringField(TEXT("id"), TestId);
							if (TestId.IsEmpty())
							{
								TestId = FString::Printf(TEXT("test_%d"), TestIndex);
								TestCopy->SetStringField(TEXT("id"), TestId);
							}
							TestCopy->SetStringField(TEXT("option_id"), OptionId);
							Tests.Add(MakeShared<FJsonValueObject>(TestCopy));
						}
					}
				}
			}
		}
		OutGenerators = MakeProfileArrayJson(Profile, TEXT("generators"), Generators);
		OutTests = MakeProfileArrayJson(Profile, TEXT("tests"), Tests);
		OutContexts = MakeProfileArrayJson(Profile, TEXT("contexts"), Contexts);
	}

	static bool SaveBehaviorTreeFolderContents(const FString& FolderPath, UBehaviorTree* Tree, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError)
	{
		if (!Tree)
		{
			OutError = TEXT("behavior_tree_not_found");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FolderPath, true);
		TSharedPtr<FJsonObject> Exported = ExportBehaviorTree(Tree, true);
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		Exported->TryGetArrayField(TEXT("nodes"), Nodes);
		Exported->TryGetArrayField(TEXT("edges"), Edges);
		TSharedPtr<FJsonObject> TreeJson = MakeShared<FJsonObject>();
		TreeJson->SetStringField(TEXT("profile"), BehaviorTreeProfile);
		TreeJson->SetNumberField(TEXT("schema_version"), 1);
		TreeJson->SetStringField(TEXT("root_id"), TEXT("root"));
		TreeJson->SetArrayField(TEXT("edges"), Edges ? *Edges : TArray<TSharedPtr<FJsonValue>>());
		TSharedPtr<FJsonObject> NodesJson = MakeProfileArrayJson(BehaviorTreeProfile, TEXT("nodes"), Nodes ? *Nodes : TArray<TSharedPtr<FJsonValue>>());
		TSharedPtr<FJsonObject> BlackboardJson = MakeShared<FJsonObject>();
		BlackboardJson->SetStringField(TEXT("profile"), BehaviorTreeProfile);
		BlackboardJson->SetNumberField(TEXT("schema_version"), 1);
		BlackboardJson->SetStringField(TEXT("blackboard_asset"), Tree->BlackboardAsset ? Tree->BlackboardAsset->GetPathName() : FString());
		if (!SaveTrackedFolderJson(FolderPath, TEXT("asset.json"), MakeBTAssetJson(Tree), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("blackboard.json"), BlackboardJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("tree.json"), TreeJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("nodes.json"), NodesJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("decorators.json"), MakeShared<FJsonObject>(), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("services.json"), MakeShared<FJsonObject>(), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("layout.json"), MakeShared<FJsonObject>(), WrittenFiles, OutError)) return false;
		IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("validation")), true);
		TArray<TSharedPtr<FJsonValue>> Issues;
		ValidateBTFolderJson(MakeBTAssetJson(Tree), NodesJson, Issues);
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/checks.json"), MakeValidationChecks(Issues), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(), WrittenFiles, OutError)) return false;
		return true;
	}

	static bool SaveStateTreeFolderContents(const FString& FolderPath, UStateTree* Tree, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError)
	{
		if (!Tree)
		{
			OutError = TEXT("state_tree_not_found");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FolderPath, true);
		TSharedPtr<FJsonObject> Exported = ExportStateTree(Tree, true);
		TSharedPtr<FJsonObject> AssetJson = ExportStateTree(Tree, false);
		const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
		Exported->TryGetArrayField(TEXT("states"), States);
		TSharedPtr<FJsonObject> StatesJson = MakeProfileArrayJson(StateTreeProfile, TEXT("states"), States ? *States : TArray<TSharedPtr<FJsonValue>>());
		TSharedPtr<FJsonObject> SchemaJson;
		TSharedPtr<FJsonObject> ParametersJson;
		TSharedPtr<FJsonObject> TransitionsJson;
		TSharedPtr<FJsonObject> TasksJson;
		TSharedPtr<FJsonObject> ConditionsJson;
		TSharedPtr<FJsonObject> EvaluatorsJson;
		TSharedPtr<FJsonObject> BindingsJson;
		TSharedPtr<FJsonObject> GlobalTasksJson;
		BuildStateTreeSplitJson(Exported, SchemaJson, ParametersJson, TransitionsJson, TasksJson, ConditionsJson, EvaluatorsJson, BindingsJson, GlobalTasksJson);
		if (!SaveTrackedFolderJson(FolderPath, TEXT("asset.json"), AssetJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("schema.json"), SchemaJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("parameters.json"), ParametersJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("states.json"), StatesJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("transitions.json"), TransitionsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("tasks.json"), TasksJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("conditions.json"), ConditionsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("evaluators.json"), EvaluatorsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("bindings.json"), BindingsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("global_tasks.json"), GlobalTasksJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("property_bag.json"), MakeShared<FJsonObject>(), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("graph_layout.json"), MakeShared<FJsonObject>(), WrittenFiles, OutError)) return false;
		IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("validation")), true);
		TArray<TSharedPtr<FJsonValue>> Issues;
		ValidateStateTreeFolderJson(AssetJson, StatesJson, Issues);
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/checks.json"), MakeValidationChecks(Issues), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(), WrittenFiles, OutError)) return false;
		return true;
	}

	static bool SaveEqsFolderContents(const FString& FolderPath, UEnvQuery* Query, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError)
	{
		if (!Query)
		{
			OutError = TEXT("eqs_query_not_found");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FolderPath, true);
		TSharedPtr<FJsonObject> Exported = ExportEqs(Query, true);
		TSharedPtr<FJsonObject> AssetJson = ExportEqs(Query, false);
		const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
		Exported->TryGetArrayField(TEXT("options"), Options);
		TSharedPtr<FJsonObject> OptionsJson = MakeProfileArrayJson(EqsProfile, TEXT("options"), Options ? *Options : TArray<TSharedPtr<FJsonValue>>());
		TSharedPtr<FJsonObject> GeneratorsJson;
		TSharedPtr<FJsonObject> TestsJson;
		TSharedPtr<FJsonObject> ContextsJson;
		BuildEqsSplitJson(Exported, GeneratorsJson, TestsJson, ContextsJson);
		if (!SaveTrackedFolderJson(FolderPath, TEXT("asset.json"), AssetJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("options.json"), OptionsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("generators.json"), GeneratorsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("tests.json"), TestsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("contexts.json"), ContextsJson, WrittenFiles, OutError)) return false;
		IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("validation")), true);
		TArray<TSharedPtr<FJsonValue>> Issues;
		ValidateEqsFolderJson(AssetJson, OptionsJson, Issues);
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/checks.json"), MakeValidationChecks(Issues), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(), WrittenFiles, OutError)) return false;
		return true;
	}

	static TSharedPtr<FJsonObject> SmartObjectRequestResultToJson(const FSmartObjectRequestResult& Result, USmartObjectSubsystem* Subsystem, const FString& ResultId)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("result_id"), ResultId);
		Obj->SetStringField(TEXT("smart_object_handle"), LexToString(Result.SmartObjectHandle));
		Obj->SetStringField(TEXT("slot_handle"), LexToString(Result.SlotHandle));
		Obj->SetBoolField(TEXT("valid"), Result.IsValid());
		if (Subsystem && Result.IsValid())
		{
			if (TOptional<FTransform> SlotTransform = Subsystem->GetSlotTransform(Result))
			{
				Obj->SetObjectField(TEXT("slot_transform"), TransformToJson(*SlotTransform));
			}
			if (USmartObjectComponent* Component = Subsystem->GetSmartObjectComponentByRequestResult(Result))
			{
				Obj->SetStringField(TEXT("component"), Component->GetName());
				Obj->SetStringField(TEXT("owner"), Component->GetOwner() ? Component->GetOwner()->GetName() : FString());
				Obj->SetStringField(TEXT("definition"), Component->GetDefinition() ? Component->GetDefinition()->GetPathName() : FString());
			}
		}
		return Obj;
	}

	static bool ResolveGenericAssetEditorWindow(UObject* Asset, const bool bOpenIfNeeded, TSharedPtr<SWindow>& OutWindow, FString& OutError)
	{
		OutWindow.Reset();
		if (!GEditor)
		{
			OutError = TEXT("editor_unavailable");
			return false;
		}
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem)
		{
			OutError = TEXT("asset_editor_subsystem_unavailable");
			return false;
		}
		if (bOpenIfNeeded)
		{
			AssetEditorSubsystem->OpenEditorForAsset(Asset);
		}
		IAssetEditorInstance* AssetEditor = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
		if (!AssetEditor)
		{
			OutError = bOpenIfNeeded ? TEXT("asset_editor_open_failed") : TEXT("asset_editor_not_opened");
			return false;
		}
		const TSharedPtr<FTabManager> TabManager = AssetEditor->GetAssociatedTabManager();
		if (!TabManager.IsValid())
		{
			OutError = TEXT("asset_editor_tab_manager_not_found");
			return false;
		}
		const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
		if (!OwnerTab.IsValid())
		{
			OutError = TEXT("asset_editor_owner_tab_not_found");
			return false;
		}
		OutWindow = OwnerTab->GetParentWindow();
		if (!OutWindow.IsValid())
		{
			OutError = TEXT("asset_editor_window_not_found");
			return false;
		}
		return true;
	}

	static FVector2D ResolveOffscreenWindowDrawSize(const TSharedPtr<SWindow>& Window)
	{
		FVector2D DrawSize(0.0, 0.0);
		if (Window.IsValid())
		{
			DrawSize = Window->GetClientSizeInScreen();
			if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
			{
				DrawSize = Window->GetCachedGeometry().GetLocalSize();
			}
			if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
			{
				DrawSize = Window->GetDesiredSize();
			}
		}
		if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
		{
			DrawSize = FVector2D(1440.0, 900.0);
		}

		const double MaxDimension = 4096.0;
		if (DrawSize.X > MaxDimension || DrawSize.Y > MaxDimension)
		{
			const double Scale = FMath::Min(MaxDimension / DrawSize.X, MaxDimension / DrawSize.Y);
			DrawSize *= Scale;
		}
		DrawSize.X = FMath::Clamp(FMath::RoundToDouble(DrawSize.X), 64.0, MaxDimension);
		DrawSize.Y = FMath::Clamp(FMath::RoundToDouble(DrawSize.Y), 64.0, MaxDimension);
		return DrawSize;
	}

	static bool IsSlateWindowBackbufferCaptureUnsafe(const TSharedPtr<SWindow>& Window, FString& OutUnsafeReason)
	{
		OutUnsafeReason.Reset();
		if (!Window.IsValid())
		{
			OutUnsafeReason = TEXT("window_invalid");
			return true;
		}
		if (!FSlateApplication::IsInitialized())
		{
			OutUnsafeReason = TEXT("slate_not_initialized");
			return true;
		}
		if (!FApp::CanEverRender())
		{
			OutUnsafeReason = TEXT("rendering_not_available");
			return true;
		}
		if (!Window->IsVisible())
		{
			OutUnsafeReason = TEXT("window_not_visible");
			return true;
		}
		if (Window->IsWindowMinimized())
		{
			OutUnsafeReason = TEXT("window_minimized");
			return true;
		}
		if (const TSharedPtr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
		{
			if (!NativeWindow->IsVisible())
			{
				OutUnsafeReason = TEXT("native_window_not_visible");
				return true;
			}
			if (NativeWindow->IsMinimized())
			{
				OutUnsafeReason = TEXT("native_window_minimized");
				return true;
			}
		}
		const FVector2D ClientSize = Window->GetClientSizeInScreen();
		if (ClientSize.X < 64.0 || ClientSize.Y < 64.0)
		{
			OutUnsafeReason = FString::Printf(TEXT("window_too_small:%.0fx%.0f"), ClientSize.X, ClientSize.Y);
			return true;
		}
		return false;
	}

	static bool ScreenshotSlateWindow(const TSharedPtr<SWindow>& Window, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		if (!Window.IsValid())
		{
			OutError = TEXT("window_not_found");
			return false;
		}
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized");
			return false;
		}
		if (!FApp::CanEverRender())
		{
			OutError = TEXT("rendering_not_available");
			return false;
		}

		FSlateApplication::Get().PumpMessages();
		const float WindowScale = FSlateApplication::Get().GetApplicationScale() * Window->GetDPIScaleFactor();
		Window->SlatePrepass(WindowScale);
		FSlateApplication::Get().InvalidateAllViewports();

		const FVector2D DrawSize = ResolveOffscreenWindowDrawSize(Window);
		FWidgetRenderer* WidgetRenderer = new FWidgetRenderer(true, true);
		if (!WidgetRenderer)
		{
			OutError = TEXT("widget_renderer_create_failed");
			return false;
		}

		UTextureRenderTarget2D* RenderTarget = FWidgetRenderer::CreateTargetFor(DrawSize, TF_Bilinear, true);
		if (!RenderTarget)
		{
			BeginCleanup(WidgetRenderer);
			OutError = TEXT("offscreen_render_target_create_failed");
			return false;
		}

		WidgetRenderer->DrawWidget(RenderTarget, Window.ToSharedRef(), 1.0f, DrawSize, 0.0f, false);
		FlushRenderingCommands();
		BeginCleanup(WidgetRenderer);

		FRenderTarget* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!RenderTargetResource)
		{
			OutError = TEXT("offscreen_render_target_resource_missing");
			return false;
		}

		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(false);
		if (!RenderTargetResource->ReadPixels(OutPixels, ReadFlags) || OutPixels.Num() <= 0)
		{
			OutError = TEXT("offscreen_read_pixels_failed");
			return false;
		}

		OutSize = FIntPoint(RenderTarget->SizeX, RenderTarget->SizeY);
		if (OutSize.X <= 0 || OutSize.Y <= 0 || OutPixels.Num() < OutSize.X * OutSize.Y)
		{
			OutError = TEXT("offscreen_screenshot_empty");
			return false;
		}
		return true;
	}

	static int32 CountNonBlackPixels(const TArray<FColor>& Pixels)
	{
		int32 NonBlack = 0;
		for (const FColor& Pixel : Pixels)
		{
			const int32 Luma = (static_cast<int32>(Pixel.R) + static_cast<int32>(Pixel.G) + static_cast<int32>(Pixel.B)) / 3;
			if (Luma > 4 || Pixel.A > 4)
			{
				++NonBlack;
			}
		}
		return NonBlack;
	}

	static void AddPixelStats(const TArray<FColor>& Pixels, TSharedPtr<FJsonObject>& OutData)
	{
		double LumaSum = 0.0;
		for (const FColor& Pixel : Pixels)
		{
			const int32 Luma = (static_cast<int32>(Pixel.R) + static_cast<int32>(Pixel.G) + static_cast<int32>(Pixel.B)) / 3;
			LumaSum += Luma;
		}
		const int32 NonBlack = CountNonBlackPixels(Pixels);
		OutData->SetNumberField(TEXT("non_black_pixel_count"), NonBlack);
		OutData->SetNumberField(TEXT("average_luma"), Pixels.Num() > 0 ? LumaSum / static_cast<double>(Pixels.Num()) : 0.0);
		OutData->SetBoolField(TEXT("appears_non_black"), NonBlack > 0);
	}

	static AActor* ResolveActorOrFirst(UWorld* World, const FString& ActorName, UClass* RequiredComponentClass, UActorComponent*& OutComponent)
	{
		OutComponent = nullptr;
		if (!World || !RequiredComponentClass)
		{
			return nullptr;
		}
		if (!ActorName.IsEmpty())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor)
				{
					continue;
				}
				const FString Label = Actor->GetActorLabel();
				if (Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase) || Label.Equals(ActorName, ESearchCase::IgnoreCase))
				{
					OutComponent = Actor->GetComponentByClass(RequiredComponentClass);
					return Actor;
				}
			}
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UActorComponent* Component = It->GetComponentByClass(RequiredComponentClass))
			{
				OutComponent = Component;
				return *It;
			}
		}
		return nullptr;
	}
}

bool FUeAgentHttpServer::CmdStateTreeCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UStateTree* Tree = nullptr;
	if (!UeAgentAIOps::CreateAsset<UStateTree>(AssetPath, Tree, OutError) || !Tree)
	{
		return false;
	}
	FString SchemaClassPath;
	JsonTryGetString(Ctx.Params, TEXT("schema_class"), SchemaClassPath);
	UClass* SchemaClass = UStateTreeComponentSchema::StaticClass();
	if (!SchemaClassPath.IsEmpty())
	{
		SchemaClass = UeAgentAIOps::ResolveClass(SchemaClassPath);
		if (!SchemaClass)
		{
			OutError = TEXT("invalid_state_tree_schema_class");
			return false;
		}
	}
	if (!UeAgentAIOps::EnsureStateTreeEditorData(Tree, SchemaClass, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> CompileReport;
	UeAgentAIOps::CompileStateTree(Tree, CompileReport);
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Tree, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportStateTree(Tree, true);
	OutData->SetObjectField(TEXT("compile_report"), CompileReport);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdStateTreeGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UStateTree* Tree = UeAgentAIOps::LoadAsset<UStateTree>(AssetPath);
	if (!Tree)
	{
		OutError = TEXT("state_tree_not_found");
		return false;
	}
	bool bIncludeStates = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_states"), bIncludeStates);
	OutData = UeAgentAIOps::ExportStateTree(Tree, bIncludeStates);
	return true;
}

bool FUeAgentHttpServer::CmdStateTreeExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UStateTree* Tree = UeAgentAIOps::LoadAsset<UStateTree>(AssetPath);
	if (!Tree)
	{
		OutError = TEXT("state_tree_not_found");
		return false;
	}
	FString FolderInput;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput);
	const FString FolderPath = UeAgentAIOps::ResolveFolderPath(FolderInput, UeAgentAIOps::StateTreeProfile, AssetPath);
	bool bClean = false;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bClean);
	if (bClean && IFileManager::Get().DirectoryExists(*FolderPath))
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);
	TArray<TSharedPtr<FJsonValue>> WrittenFiles;
	if (!UeAgentAIOps::SaveStateTreeFolderContents(FolderPath, Tree, WrittenFiles, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportStateTree(Tree, true);
	OutData->SetArrayField(TEXT("written_files"), WrittenFiles);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdStateTreeValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> StatesJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError) ||
		!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("states.json"), StatesJson, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	UeAgentAIOps::ValidateStateTreeFolderJson(AssetJson, StatesJson, Issues);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::StateTreeProfile);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdStateTreeApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> StatesJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError) ||
		!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("states.json"), StatesJson, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!UeAgentAIOps::ValidateStateTreeFolderJson(AssetJson, StatesJson, Issues))
	{
		OutData->SetStringField(TEXT("status"), TEXT("validate_failed"));
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return false;
	}

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	bool bCreateIfMissing = false;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bAllowDestructive = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	if (bDryRun || bValidateOnly)
	{
		OutData->SetStringField(TEXT("profile"), UeAgentAIOps::StateTreeProfile);
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return true;
	}
	UStateTree* Tree = UeAgentAIOps::LoadAsset<UStateTree>(AssetPath);
	bool bCreated = false;
	if (!Tree)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("state_tree_not_found");
			return false;
		}
		if (!UeAgentAIOps::CreateAsset<UStateTree>(AssetPath, Tree, OutError) || !Tree)
		{
			return false;
		}
		bCreated = true;
	}
	FString SchemaClassPath;
	AssetJson->TryGetStringField(TEXT("schema_class"), SchemaClassPath);
	UClass* SchemaClass = UStateTreeComponentSchema::StaticClass();
	if (!SchemaClassPath.IsEmpty())
	{
		SchemaClass = UeAgentAIOps::ResolveClass(SchemaClassPath);
		if (!SchemaClass)
		{
			OutError = TEXT("invalid_state_tree_schema_class");
			return false;
		}
	}
	if (!UeAgentAIOps::EnsureStateTreeEditorData(Tree, SchemaClass, OutError))
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	UStateTreeEditorData* EditorData = UeAgentAIOps::GetStateTreeEditorData(Tree);
	if (!EditorData)
	{
		OutError = TEXT("state_tree_editor_data_unavailable");
		return false;
	}
	if (EditorData->SubTrees.Num() > 0 && !bCreated && !bAllowDestructive)
	{
		OutError = TEXT("state_tree_rebuild_requires_allow_destructive");
		return false;
	}
	Tree->Modify();
	EditorData->Modify();
	EditorData->SubTrees.Reset();
	EditorData->Evaluators.Reset();
	EditorData->GlobalTasks.Reset();
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	const TArray<TSharedPtr<FJsonValue>>* Evaluators = nullptr;
	if (StatesJson->TryGetArrayField(TEXT("evaluators"), Evaluators) && Evaluators)
	{
		for (const TSharedPtr<FJsonValue>& EvaluatorValue : *Evaluators)
		{
			const TSharedPtr<FJsonObject>* EvaluatorObjPtr = nullptr;
			if (EvaluatorValue->TryGetObject(EvaluatorObjPtr))
			{
				FStateTreeEditorNode& Node = EditorData->Evaluators.AddDefaulted_GetRef();
				if (!UeAgentAIOps::ConfigureStateTreeEditorNode(Node, EditorData, *EvaluatorObjPtr, FStateTreeEvaluatorBase::StaticStruct(), UStateTreeEvaluatorBlueprintBase::StaticClass(), PropertyResults, OutError))
				{
					return false;
				}
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* GlobalTasks = nullptr;
	if (StatesJson->TryGetArrayField(TEXT("global_tasks"), GlobalTasks) && GlobalTasks)
	{
		for (const TSharedPtr<FJsonValue>& TaskValue : *GlobalTasks)
		{
			const TSharedPtr<FJsonObject>* TaskObjPtr = nullptr;
			if (TaskValue->TryGetObject(TaskObjPtr))
			{
				FStateTreeEditorNode& Node = EditorData->GlobalTasks.AddDefaulted_GetRef();
				if (!UeAgentAIOps::ConfigureStateTreeEditorNode(Node, EditorData, *TaskObjPtr, FStateTreeTaskBase::StaticStruct(), UStateTreeTaskBlueprintBase::StaticClass(), PropertyResults, OutError))
				{
					return false;
				}
			}
		}
	}
	TMap<FString, UStateTreeState*> StateMap;
	TMap<FString, UStateTreeState*> StateNameMap;
	TMultiMap<FString, TSharedPtr<FJsonObject>> ChildrenByParent;
	const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
	StatesJson->TryGetArrayField(TEXT("states"), States);
	for (const TSharedPtr<FJsonValue>& StateValue : *States)
	{
		const TSharedPtr<FJsonObject>* StateObjPtr = nullptr;
		StateValue->TryGetObject(StateObjPtr);
		FString ParentId;
		(*StateObjPtr)->TryGetStringField(TEXT("parent_id"), ParentId);
		ChildrenByParent.Add(ParentId, *StateObjPtr);
	}
	TFunction<UStateTreeState*(const TSharedPtr<FJsonObject>&, UStateTreeState*)> BuildState =
		[&](const TSharedPtr<FJsonObject>& StateObj, UStateTreeState* Parent) -> UStateTreeState*
		{
			FString Id;
			FString Name;
			FString TypeText;
			StateObj->TryGetStringField(TEXT("id"), Id);
			StateObj->TryGetStringField(TEXT("name"), Name);
			StateObj->TryGetStringField(TEXT("type"), TypeText);
			UStateTreeState* State = nullptr;
			if (Parent)
			{
				State = &Parent->AddChildState(FName(*Name), UeAgentAIOps::ParseStateType(TypeText));
			}
			else
			{
				State = &EditorData->AddSubTree(FName(*Name));
				State->Type = UeAgentAIOps::ParseStateType(TypeText);
			}
			State->ID = FGuid::NewGuid();
			State->Description = StateObj->GetStringField(TEXT("description"));
			FString SelectionText;
			FString CompletionText;
			StateObj->TryGetStringField(TEXT("selection_behavior"), SelectionText);
			StateObj->TryGetStringField(TEXT("tasks_completion"), CompletionText);
			State->SelectionBehavior = UeAgentAIOps::EnumFromString(SelectionText, EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder);
			State->TasksCompletion = UeAgentAIOps::EnumFromString(CompletionText, EStateTreeTaskCompletionType::Any);
			bool bEnabled = true;
			StateObj->TryGetBoolField(TEXT("enabled"), bEnabled);
			State->bEnabled = bEnabled;
			FString TagText;
			if (StateObj->TryGetStringField(TEXT("tag"), TagText) && !TagText.IsEmpty())
			{
				State->Tag = FGameplayTag::RequestGameplayTag(FName(*TagText), false);
			}
			const TArray<TSharedPtr<FJsonValue>>* Conditions = nullptr;
			if (StateObj->TryGetArrayField(TEXT("enter_conditions"), Conditions) && Conditions)
			{
				for (const TSharedPtr<FJsonValue>& ConditionValue : *Conditions)
				{
					const TSharedPtr<FJsonObject>* ConditionObjPtr = nullptr;
					if (ConditionValue->TryGetObject(ConditionObjPtr))
					{
						FStateTreeEditorNode& Node = State->EnterConditions.AddDefaulted_GetRef();
						if (!UeAgentAIOps::ConfigureStateTreeEditorNode(Node, State, *ConditionObjPtr, FStateTreeConditionBase::StaticStruct(), UStateTreeConditionBlueprintBase::StaticClass(), PropertyResults, OutError))
						{
							return nullptr;
						}
					}
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
			if (StateObj->TryGetArrayField(TEXT("tasks"), Tasks) && Tasks)
			{
				for (const TSharedPtr<FJsonValue>& TaskValue : *Tasks)
				{
					const TSharedPtr<FJsonObject>* TaskObjPtr = nullptr;
					if (TaskValue->TryGetObject(TaskObjPtr))
					{
						FStateTreeEditorNode& Node = State->Tasks.AddDefaulted_GetRef();
						if (!UeAgentAIOps::ConfigureStateTreeEditorNode(Node, State, *TaskObjPtr, FStateTreeTaskBase::StaticStruct(), UStateTreeTaskBlueprintBase::StaticClass(), PropertyResults, OutError))
						{
							return nullptr;
						}
					}
				}
			}
			StateMap.Add(Id, State);
			StateNameMap.Add(Name, State);
			TArray<TSharedPtr<FJsonObject>> Children;
			ChildrenByParent.MultiFind(Id, Children);
			Algo::Reverse(Children);
			for (const TSharedPtr<FJsonObject>& ChildObj : Children)
			{
				if (!BuildState(ChildObj, State))
				{
					return nullptr;
				}
			}
			return State;
		};
	TArray<TSharedPtr<FJsonObject>> RootStates;
	ChildrenByParent.MultiFind(FString(), RootStates);
	Algo::Reverse(RootStates);
	for (const TSharedPtr<FJsonObject>& StateObj : RootStates)
	{
		if (!BuildState(StateObj, nullptr))
		{
			return false;
		}
	}
	for (const TSharedPtr<FJsonValue>& StateValue : *States)
	{
		const TSharedPtr<FJsonObject>* StateObjPtr = nullptr;
		if (!StateValue->TryGetObject(StateObjPtr) || !StateObjPtr || !StateObjPtr->IsValid())
		{
			continue;
		}
		FString SourceId;
		(*StateObjPtr)->TryGetStringField(TEXT("id"), SourceId);
		UStateTreeState* SourceState = StateMap.FindRef(SourceId);
		if (!SourceState)
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
		if (!(*StateObjPtr)->TryGetArrayField(TEXT("transitions"), Transitions) || !Transitions)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& TransitionValue : *Transitions)
		{
			const TSharedPtr<FJsonObject>* TransitionObjPtr = nullptr;
			if (!TransitionValue->TryGetObject(TransitionObjPtr) || !TransitionObjPtr || !TransitionObjPtr->IsValid())
			{
				continue;
			}
			FString TriggerText;
			FString PriorityText;
			FString TypeText;
			FString FallbackText;
			FString TargetId;
			FString TargetName;
			(*TransitionObjPtr)->TryGetStringField(TEXT("trigger"), TriggerText);
			(*TransitionObjPtr)->TryGetStringField(TEXT("priority"), PriorityText);
			(*TransitionObjPtr)->TryGetStringField(TEXT("type"), TypeText);
			(*TransitionObjPtr)->TryGetStringField(TEXT("fallback"), FallbackText);
			(*TransitionObjPtr)->TryGetStringField(TEXT("target_state_id"), TargetId);
			(*TransitionObjPtr)->TryGetStringField(TEXT("target_state_name"), TargetName);

			const EStateTreeTransitionType LinkType = UeAgentAIOps::EnumFromString(TypeText, EStateTreeTransitionType::GotoState);
			UStateTreeState* TargetState = !TargetId.IsEmpty() ? StateMap.FindRef(TargetId) : nullptr;
			if (!TargetState && !TargetName.IsEmpty())
			{
				TargetState = StateNameMap.FindRef(TargetName);
			}
			if (LinkType == EStateTreeTransitionType::GotoState && !TargetState)
			{
				OutError = FString::Printf(TEXT("state_tree_transition_target_not_found:%s"), *TargetId);
				return false;
			}

			FStateTreeTransition& Transition = SourceState->Transitions.AddDefaulted_GetRef();
			Transition.ID = FGuid::NewGuid();
			Transition.Trigger = UeAgentAIOps::EnumFromString(TriggerText, EStateTreeTransitionTrigger::OnStateCompleted);
			Transition.Priority = UeAgentAIOps::EnumFromString(PriorityText, EStateTreeTransitionPriority::Normal);
			Transition.State.LinkType = LinkType;
			Transition.State.Fallback = UeAgentAIOps::EnumFromString(FallbackText, EStateTreeSelectionFallback::None);
			Transition.State.ID = TargetState ? TargetState->ID : FGuid();
			Transition.State.Name = TargetState ? TargetState->Name : FName(*TargetName);
			bool bTransitionEnabled = true;
			(*TransitionObjPtr)->TryGetBoolField(TEXT("enabled"), bTransitionEnabled);
			Transition.bTransitionEnabled = bTransitionEnabled;
			bool bDelay = false;
			(*TransitionObjPtr)->TryGetBoolField(TEXT("delay_enabled"), bDelay);
			Transition.bDelayTransition = bDelay;
			double DelayValue = 0.0;
			if ((*TransitionObjPtr)->TryGetNumberField(TEXT("delay_duration"), DelayValue))
			{
				Transition.DelayDuration = static_cast<float>(DelayValue);
			}
			if ((*TransitionObjPtr)->TryGetNumberField(TEXT("delay_random_variance"), DelayValue))
			{
				Transition.DelayRandomVariance = static_cast<float>(DelayValue);
			}

			const TArray<TSharedPtr<FJsonValue>>* TransitionConditions = nullptr;
			if ((*TransitionObjPtr)->TryGetArrayField(TEXT("conditions"), TransitionConditions) && TransitionConditions)
			{
				for (const TSharedPtr<FJsonValue>& ConditionValue : *TransitionConditions)
				{
					const TSharedPtr<FJsonObject>* ConditionObjPtr = nullptr;
					if (ConditionValue->TryGetObject(ConditionObjPtr))
					{
						FStateTreeEditorNode& Node = Transition.Conditions.AddDefaulted_GetRef();
						if (!UeAgentAIOps::ConfigureStateTreeEditorNode(Node, SourceState, *ConditionObjPtr, FStateTreeConditionBase::StaticStruct(), UStateTreeConditionBlueprintBase::StaticClass(), PropertyResults, OutError))
						{
							return false;
						}
					}
				}
			}
		}
	}
	TSharedPtr<FJsonObject> CompileReport;
	UeAgentAIOps::CompileStateTree(Tree, CompileReport);
	Tree->MarkPackageDirty();
	Tree->PostEditChange();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Tree, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportStateTree(Tree, true);
	OutData->SetObjectField(TEXT("compile_report"), CompileReport);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("created_asset"), bCreated);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
#else
	OutError = TEXT("state_tree_editor_data_unavailable");
	return false;
#endif
}

bool FUeAgentHttpServer::CmdStateTreeOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("state_tree_not_found");
		return false;
	}
	if (GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
	}
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetBoolField(TEXT("opened"), true);
	return true;
}

bool FUeAgentHttpServer::CmdStateTreeScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("state_tree_not_found");
		return false;
	}
	bool bOpenIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenIfNeeded);
	TSharedPtr<SWindow> Window;
	if (!UeAgentAIOps::ResolveGenericAssetEditorWindow(Asset, bOpenIfNeeded, Window, OutError))
	{
		return false;
	}
	FString BackbufferUnsafeReason;
	const bool bBackbufferCaptureUnsafe = UeAgentAIOps::IsSlateWindowBackbufferCaptureUnsafe(Window, BackbufferUnsafeReason);
	TArray<FColor> Pixels;
	FIntPoint Size;
	if (!UeAgentAIOps::ScreenshotSlateWindow(Window, Pixels, Size, OutError))
	{
		return false;
	}
	int32 MaxSize = 1600;
	double MaxSizeNumber = MaxSize;
	JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeNumber);
	MaxSize = (int32)FMath::Clamp(MaxSizeNumber, 64.0, 8192.0);
	TArray<FColor> ResizedPixels;
	FIntPoint ResizedSize;
	if (!ResizePixelsMaxSize(Pixels, Size, MaxSize, ResizedPixels, ResizedSize, OutError))
	{
		return false;
	}
	bool bRequireNonBlack = true;
	JsonTryGetBool(Ctx.Params, TEXT("require_non_black"), bRequireNonBlack);
	if (bRequireNonBlack && UeAgentAIOps::CountNonBlackPixels(ResizedPixels) <= 0)
	{
		OutError = TEXT("asset_editor_screenshot_black");
		return false;
	}
	FString Format = TEXT("png");
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	Format = Format.ToLower();
	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(ResizedPixels, ResizedSize, Format, 90, OutPath, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetStringField(TEXT("capture_mode"), TEXT("offscreen_widget_renderer"));
	OutData->SetStringField(TEXT("legacy_backbuffer_capture"), TEXT("disabled"));
	OutData->SetBoolField(TEXT("backbuffer_capture_would_be_unsafe"), bBackbufferCaptureUnsafe);
	if (!BackbufferUnsafeReason.IsEmpty())
	{
		OutData->SetStringField(TEXT("backbuffer_unsafe_reason"), BackbufferUnsafeReason);
	}
	UeAgentAIOps::AddPixelStats(ResizedPixels, OutData);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdStateTreeRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString ActorName;
	JsonTryGetString(Ctx.Params, TEXT("actor"), ActorName);
	UActorComponent* FoundComponent = nullptr;
	AActor* Actor = UeAgentAIOps::ResolveActorOrFirst(World, ActorName, UStateTreeComponent::StaticClass(), FoundComponent);
	UStateTreeComponent* Component = Cast<UStateTreeComponent>(FoundComponent);
	if (!Component)
	{
		OutError = TEXT("state_tree_component_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("actor"), Actor ? Actor->GetName() : FString());
	OutData->SetStringField(TEXT("component"), Component->GetName());
	OutData->SetBoolField(TEXT("is_running"), Component->IsRunning());
	OutData->SetBoolField(TEXT("is_paused"), Component->IsPaused());
	OutData->SetStringField(TEXT("run_status"), UeAgentAIOps::EnumToString(Component->GetStateTreeRunStatus()));
	OutData->SetArrayField(TEXT("active_states"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("running_tasks"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("queued_events"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("instance_data"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetStringField(TEXT("runtime_introspection"), TEXT("component_run_status_state_tree_ref"));
	FProperty* RefProperty = FindFProperty<FProperty>(Component->GetClass(), TEXT("StateTreeRef"));
	if (FStructProperty* StructProperty = CastField<FStructProperty>(RefProperty))
	{
		if (StructProperty->Struct == FStateTreeReference::StaticStruct())
		{
			const FStateTreeReference* Ref = StructProperty->ContainerPtrToValuePtr<FStateTreeReference>(Component);
			OutData->SetStringField(TEXT("state_tree"), (Ref && Ref->GetStateTree()) ? Ref->GetStateTree()->GetPathName() : FString());
		}
	}
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdStateTreeNodeBlueprintGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	UClass* GeneratedClass = nullptr;
	if (Blueprint)
	{
		GeneratedClass = Blueprint->GeneratedClass;
	}
	else
	{
		GeneratedClass = Cast<UClass>(Asset);
	}
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("generated_class"), GeneratedClass ? GeneratedClass->GetPathName() : FString());
	OutData->SetStringField(TEXT("parent_class"), (GeneratedClass && GeneratedClass->GetSuperClass()) ? GeneratedClass->GetSuperClass()->GetPathName() : FString());
	OutData->SetStringField(TEXT("introspection_level"), TEXT("class_hierarchy_only"));
	OutData->SetBoolField(TEXT("is_state_tree_task"), GeneratedClass && GeneratedClass->IsChildOf(UStateTreeTaskBlueprintBase::StaticClass()));
	OutData->SetBoolField(TEXT("is_state_tree_condition"), GeneratedClass && GeneratedClass->IsChildOf(UStateTreeConditionBlueprintBase::StaticClass()));
	OutData->SetBoolField(TEXT("is_state_tree_evaluator"), GeneratedClass && GeneratedClass->IsChildOf(UStateTreeEvaluatorBlueprintBase::StaticClass()));
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdEqsCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UEnvQuery* Query = nullptr;
	if (!UeAgentAIOps::CreateAsset<UEnvQuery>(AssetPath, Query, OutError) || !Query)
	{
		return false;
	}
	FString Template;
	JsonTryGetString(Ctx.Params, TEXT("template"), Template);
	const bool bCreateDefaultOption = Template.IsEmpty() || Template.Equals(TEXT("simple_grid"), ESearchCase::IgnoreCase);
	if (bCreateDefaultOption)
	{
		UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query, UEnvQueryOption::StaticClass(), NAME_None, RF_Transactional);
		Option->Generator = NewObject<UEnvQueryGenerator_SimpleGrid>(Option, UEnvQueryGenerator_SimpleGrid::StaticClass(), NAME_None, RF_Transactional);
		Query->GetOptionsMutable().Add(Option);
	}
	Query->MarkPackageDirty();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Query, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportEqs(Query, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdEqsGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UEnvQuery* Query = UeAgentAIOps::LoadAsset<UEnvQuery>(AssetPath);
	if (!Query)
	{
		OutError = TEXT("eqs_query_not_found");
		return false;
	}
	bool bIncludeOptions = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_tests"), bIncludeOptions);
	OutData = UeAgentAIOps::ExportEqs(Query, bIncludeOptions);
	return true;
}

bool FUeAgentHttpServer::CmdEqsExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UEnvQuery* Query = UeAgentAIOps::LoadAsset<UEnvQuery>(AssetPath);
	if (!Query)
	{
		OutError = TEXT("eqs_query_not_found");
		return false;
	}
	FString FolderInput;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput);
	const FString FolderPath = UeAgentAIOps::ResolveFolderPath(FolderInput, UeAgentAIOps::EqsProfile, AssetPath);
	bool bClean = false;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bClean);
	if (bClean && IFileManager::Get().DirectoryExists(*FolderPath))
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);
	TArray<TSharedPtr<FJsonValue>> WrittenFiles;
	if (!UeAgentAIOps::SaveEqsFolderContents(FolderPath, Query, WrittenFiles, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportEqs(Query, true);
	OutData->SetArrayField(TEXT("written_files"), WrittenFiles);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdEqsValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> OptionsJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError) ||
		!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("options.json"), OptionsJson, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	UeAgentAIOps::ValidateEqsFolderJson(AssetJson, OptionsJson, Issues);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::EqsProfile);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdEqsApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> OptionsJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError) ||
		!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("options.json"), OptionsJson, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!UeAgentAIOps::ValidateEqsFolderJson(AssetJson, OptionsJson, Issues))
	{
		OutData->SetStringField(TEXT("status"), TEXT("validate_failed"));
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return false;
	}
	bool bDryRun = false;
	bool bValidateOnly = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	if (bDryRun || bValidateOnly)
	{
		OutData->SetStringField(TEXT("profile"), UeAgentAIOps::EqsProfile);
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return true;
	}
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	bool bCreateIfMissing = false;
	bool bAllowDestructive = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	UEnvQuery* Query = UeAgentAIOps::LoadAsset<UEnvQuery>(AssetPath);
	bool bCreated = false;
	if (!Query)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("eqs_query_not_found");
			return false;
		}
		if (!UeAgentAIOps::CreateAsset<UEnvQuery>(AssetPath, Query, OutError) || !Query)
		{
			return false;
		}
		bCreated = true;
	}
	if (Query->GetOptions().Num() > 0 && !bCreated && !bAllowDestructive)
	{
		OutError = TEXT("eqs_rebuild_requires_allow_destructive");
		return false;
	}
	Query->Modify();
	Query->GetOptionsMutable().Reset();
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
	OptionsJson->TryGetArrayField(TEXT("options"), Options);
	for (const TSharedPtr<FJsonValue>& OptionValue : *Options)
	{
		const TSharedPtr<FJsonObject>* OptionObjPtr = nullptr;
		if (!OptionValue->TryGetObject(OptionObjPtr))
		{
			continue;
		}
		UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query, UEnvQueryOption::StaticClass(), NAME_None, RF_Transactional);
		FString GeneratorClassPath;
		(*OptionObjPtr)->TryGetStringField(TEXT("generator_class"), GeneratorClassPath);
		UClass* GeneratorClass = GeneratorClassPath.IsEmpty() ? UEnvQueryGenerator_SimpleGrid::StaticClass() : UeAgentAIOps::ResolveClass(GeneratorClassPath);
		if (!GeneratorClass || !GeneratorClass->IsChildOf(UEnvQueryGenerator::StaticClass()))
		{
			OutError = TEXT("eqs_generator_class_not_found");
			return false;
		}
		Option->Generator = NewObject<UEnvQueryGenerator>(Option, GeneratorClass, NAME_None, RF_Transactional);
		const TArray<TSharedPtr<FJsonValue>>* GeneratorProperties = nullptr;
		if ((*OptionObjPtr)->TryGetArrayField(TEXT("generator_properties"), GeneratorProperties) && GeneratorProperties)
		{
			if (!UeAgentAIOps::ApplyProperties(Option->Generator, GeneratorProperties, PropertyResults, OutError))
			{
				return false;
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Tests = nullptr;
		if ((*OptionObjPtr)->TryGetArrayField(TEXT("tests"), Tests) && Tests)
		{
			for (const TSharedPtr<FJsonValue>& TestValue : *Tests)
			{
				const TSharedPtr<FJsonObject>* TestObjPtr = nullptr;
				if (!TestValue->TryGetObject(TestObjPtr))
				{
					continue;
				}
				FString TestClassPath;
				(*TestObjPtr)->TryGetStringField(TEXT("class"), TestClassPath);
				UClass* TestClass = TestClassPath.IsEmpty() ? UEnvQueryTest_Distance::StaticClass() : UeAgentAIOps::ResolveClass(TestClassPath);
				if (!TestClass || !TestClass->IsChildOf(UEnvQueryTest::StaticClass()))
				{
					OutError = TEXT("eqs_test_class_not_found");
					return false;
				}
				UEnvQueryTest* Test = NewObject<UEnvQueryTest>(Option, TestClass, NAME_None, RF_Transactional);
				const TArray<TSharedPtr<FJsonValue>>* TestProperties = nullptr;
				if ((*TestObjPtr)->TryGetArrayField(TEXT("properties"), TestProperties) && TestProperties)
				{
					if (!UeAgentAIOps::ApplyProperties(Test, TestProperties, PropertyResults, OutError))
					{
						return false;
					}
				}
				Option->Tests.Add(Test);
			}
		}
		Query->GetOptionsMutable().Add(Option);
	}
	Query->MarkPackageDirty();
	Query->PostEditChange();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Query, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportEqs(Query, true);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("created_asset"), bCreated);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdEqsRunQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UEnvQuery* Query = UeAgentAIOps::LoadAsset<UEnvQuery>(AssetPath);
	if (!Query)
	{
		OutError = TEXT("eqs_query_not_found");
		return false;
	}
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	(void)NavSys;
	UEnvQueryManager* Manager = UEnvQueryManager::GetCurrent(World);
	if (!Manager)
	{
		OutError = TEXT("eqs_manager_not_found");
		return false;
	}
	FString QuerierName;
	JsonTryGetString(Ctx.Params, TEXT("querier"), QuerierName);
	UObject* Querier = QuerierName.IsEmpty() ? static_cast<UObject*>(World) : static_cast<UObject*>(FindActorByNameOrLabel(World, QuerierName));
	if (!Querier)
	{
		OutError = TEXT("querier_not_found");
		return false;
	}
	FString RunModeText;
	JsonTryGetString(Ctx.Params, TEXT("run_mode"), RunModeText);
	FEnvQueryRequest Request(Query, Querier);
	Request.SetWorldOverride(World);
	TSharedPtr<FEnvQueryResult> Result = Manager->RunInstantQuery(Request, UeAgentAIOps::ParseEqsRunMode(RunModeText));
	if (!Result.IsValid())
	{
		OutError = TEXT("eqs_run_failed");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	int32 MaxResults = 16;
	double MaxResultsNumber = MaxResults;
	JsonTryGetNumber(Ctx.Params, TEXT("max_results"), MaxResultsNumber);
	MaxResults = (int32)FMath::Clamp(MaxResultsNumber, 1.0, 1024.0);
	TArray<TSharedPtr<FJsonValue>> Items;
	const int32 EmitCount = FMath::Min(MaxResults, Result->Items.Num());
	for (int32 Index = 0; Index < EmitCount; ++Index)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetNumberField(TEXT("score"), Result->GetItemScore(Index));
		Item->SetObjectField(TEXT("location"), VecToJson(Result->GetItemAsLocation(Index)));
		if (AActor* Actor = Result->GetItemAsActor(Index))
		{
			Item->SetStringField(TEXT("actor"), Actor->GetName());
			Item->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetStringField(TEXT("asset_path"), Query->GetOutermost()->GetName());
	OutData->SetBoolField(TEXT("successful"), Result->IsSuccessful());
	OutData->SetNumberField(TEXT("item_count"), Result->Items.Num());
	OutData->SetBoolField(TEXT("truncated"), Result->Items.Num() > EmitCount);
	OutData->SetArrayField(TEXT("items"), Items);
	OutData->SetStringField(TEXT("item_type"), Result->ItemType ? Result->ItemType->GetPathName() : FString());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdEqsDebugSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdEqsRunQuery(Ctx, OutData, OutError);
}

namespace UeAgentAIOps
{
	static TSharedPtr<FJsonObject> ExportPerceptionComponent(UAIPerceptionComponent* Component)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		if (!Component)
		{
			return Root;
		}
		Root->SetStringField(TEXT("component"), Component->GetName());
		Root->SetStringField(TEXT("owner"), Component->GetOwner() ? Component->GetOwner()->GetName() : FString());
		Root->SetStringField(TEXT("class"), Component->GetClass()->GetPathName());
		Root->SetStringField(TEXT("dominant_sense"), Component->GetDominantSense() ? Component->GetDominantSense()->GetPathName() : FString());
		TArray<TSharedPtr<FJsonValue>> Senses;
		for (UAIPerceptionComponent::TAISenseConfigConstIterator It = Component->GetSensesConfigIterator(); It; ++It)
		{
			const UAISenseConfig* Config = *It;
			TSharedPtr<FJsonObject> SenseObj = MakeShared<FJsonObject>();
			SenseObj->SetStringField(TEXT("config_class"), Config ? Config->GetClass()->GetPathName() : FString());
			SenseObj->SetStringField(TEXT("sense_class"), (Config && Config->GetSenseImplementation()) ? Config->GetSenseImplementation()->GetPathName() : FString());
			SenseObj->SetStringField(TEXT("sense_name"), Config ? Config->GetSenseName() : FString());
			Senses.Add(MakeShared<FJsonValueObject>(SenseObj));
		}
		Root->SetArrayField(TEXT("senses"), Senses);
		Root->SetNumberField(TEXT("sense_count"), Senses.Num());
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportStimuliSourceComponent(UAIPerceptionStimuliSourceComponent* Component)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		if (!Component)
		{
			return Root;
		}
		Root->SetStringField(TEXT("component"), Component->GetName());
		Root->SetStringField(TEXT("owner"), Component->GetOwner() ? Component->GetOwner()->GetName() : FString());
		Root->SetStringField(TEXT("class"), Component->GetClass()->GetPathName());
		bool bAutoRegisterAsSource = false;
		Root->SetBoolField(TEXT("auto_register_as_source"), ReadReflectedBool(Component->GetClass(), Component, TEXT("bAutoRegisterAsSource"), bAutoRegisterAsSource) ? bAutoRegisterAsSource : false);
		TArray<TSharedPtr<FJsonValue>> Senses;
		TArray<UClass*> SenseClasses;
		ReadReflectedClassArray(Component->GetClass(), Component, TEXT("RegisterAsSourceForSenses"), SenseClasses);
		for (UClass* SenseClass : SenseClasses)
		{
			Senses.Add(MakeShared<FJsonValueString>(SenseClass ? SenseClass->GetPathName() : FString()));
		}
		Root->SetArrayField(TEXT("register_as_source_for_senses"), Senses);
		return Root;
	}

	static TSharedPtr<FJsonObject> StimulusToJson(const FAIStimulus& Stimulus)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("successfully_sensed"), Stimulus.WasSuccessfullySensed());
		Obj->SetBoolField(TEXT("expired"), Stimulus.IsExpired());
		Obj->SetNumberField(TEXT("age"), Stimulus.GetAge());
		float ExpirationAge = 0.0f;
		Obj->SetNumberField(TEXT("expiration_age"), ReadReflectedFloat(FAIStimulus::StaticStruct(), &Stimulus, TEXT("ExpirationAge"), ExpirationAge) ? ExpirationAge : 0.0f);
		Obj->SetNumberField(TEXT("strength"), Stimulus.Strength);
		Obj->SetObjectField(TEXT("stimulus_location"), VecToJson(Stimulus.StimulusLocation));
		Obj->SetObjectField(TEXT("receiver_location"), VecToJson(Stimulus.ReceiverLocation));
		Obj->SetStringField(TEXT("tag"), Stimulus.Tag.ToString());
		return Obj;
	}

	static UAIPerceptionComponent* FindPerceptionComponentFromAssetOrActor(UWorld* World, const TSharedPtr<FJsonObject>& Params, FString& OutOwner, FString& OutError)
	{
		FString ActorName;
		FString BlueprintAssetPath;
		Params->TryGetStringField(TEXT("actor"), ActorName);
		Params->TryGetStringField(TEXT("blueprint_asset"), BlueprintAssetPath);
		if (!ActorName.IsEmpty())
		{
			AActor* Actor = nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetName().Equals(ActorName, ESearchCase::IgnoreCase) || It->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase))
				{
					Actor = *It;
					break;
				}
			}
			if (!Actor)
			{
				OutError = TEXT("actor_not_found");
				return nullptr;
			}
			OutOwner = Actor->GetName();
			return Actor->FindComponentByClass<UAIPerceptionComponent>();
		}
		if (!BlueprintAssetPath.IsEmpty())
		{
			if (UBlueprint* Blueprint = LoadAsset<UBlueprint>(BlueprintAssetPath))
			{
				if (AActor* CDO = Blueprint->GeneratedClass ? Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr)
				{
					OutOwner = Blueprint->GetPathName();
					return CDO->FindComponentByClass<UAIPerceptionComponent>();
				}
			}
			OutError = TEXT("blueprint_asset_not_found");
			return nullptr;
		}
		OutError = TEXT("missing_actor_or_blueprint_asset");
		return nullptr;
	}
}

bool FUeAgentHttpServer::CmdAIPerceptionGetComponentInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString Owner;
	UAIPerceptionComponent* Component = UeAgentAIOps::FindPerceptionComponentFromAssetOrActor(World, Ctx.Params, Owner, OutError);
	if (!Component)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("ai_perception_component_not_found");
		}
		return false;
	}
	OutData = UeAgentAIOps::ExportPerceptionComponent(Component);
	OutData->SetStringField(TEXT("source_owner"), Owner);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdAIPerceptionValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Issues;
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString Owner;
	UAIPerceptionComponent* Listener = UeAgentAIOps::FindPerceptionComponentFromAssetOrActor(World, Ctx.Params, Owner, OutError);
	if (!Listener)
	{
		UeAgentAIOps::AddIssue(Issues, TEXT("listener_component_not_found"), TEXT("AI Perception listener component was not found."), TEXT("$.actor"));
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* RequiredSenses = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("required_senses"), RequiredSenses) && RequiredSenses)
		{
			TSet<FString> PresentSenses;
			for (UAIPerceptionComponent::TAISenseConfigConstIterator It = Listener->GetSensesConfigIterator(); It; ++It)
			{
				const UAISenseConfig* Config = *It;
				if (Config && Config->GetSenseImplementation())
				{
					PresentSenses.Add(Config->GetSenseImplementation()->GetPathName());
					PresentSenses.Add(Config->GetSenseName());
				}
			}
			for (const TSharedPtr<FJsonValue>& SenseValue : *RequiredSenses)
			{
				FString SenseText;
				if (SenseValue.IsValid() && SenseValue->TryGetString(SenseText) && !PresentSenses.Contains(SenseText))
				{
					UeAgentAIOps::AddIssue(Issues, TEXT("required_sense_missing"), FString::Printf(TEXT("Required sense '%s' is not configured."), *SenseText), TEXT("$.required_senses"));
				}
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* StimuliSources = nullptr;
	TArray<TSharedPtr<FJsonValue>> SourcesOut;
	if (Ctx.Params->TryGetArrayField(TEXT("stimuli_sources"), StimuliSources) && StimuliSources)
	{
		for (const TSharedPtr<FJsonValue>& SourceValue : *StimuliSources)
		{
			FString SourceName;
			if (!SourceValue.IsValid() || !SourceValue->TryGetString(SourceName))
			{
				continue;
			}
			AActor* SourceActor = FindActorByNameOrLabel(World, SourceName);
			UAIPerceptionStimuliSourceComponent* SourceComp = SourceActor ? SourceActor->FindComponentByClass<UAIPerceptionStimuliSourceComponent>() : nullptr;
			if (!SourceActor || !SourceComp)
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("stimuli_source_missing"), FString::Printf(TEXT("Stimuli source '%s' is missing component."), *SourceName), TEXT("$.stimuli_sources"));
			}
			else
			{
				SourcesOut.Add(MakeShared<FJsonValueObject>(UeAgentAIOps::ExportStimuliSourceComponent(SourceComp)));
			}
		}
	}
	OutData->SetObjectField(TEXT("listener"), Listener ? UeAgentAIOps::ExportPerceptionComponent(Listener) : MakeShared<FJsonObject>());
	OutData->SetArrayField(TEXT("stimuli_sources"), SourcesOut);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdAIPerceptionRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString ActorName;
	JsonTryGetString(Ctx.Params, TEXT("controller"), ActorName);
	if (ActorName.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("pawn"), ActorName);
	}
	UActorComponent* FoundComponent = nullptr;
	AActor* Actor = UeAgentAIOps::ResolveActorOrFirst(World, ActorName, UAIPerceptionComponent::StaticClass(), FoundComponent);
	UAIPerceptionComponent* Component = Cast<UAIPerceptionComponent>(FoundComponent);
	if (!Component)
	{
		OutError = TEXT("ai_perception_component_not_found");
		return false;
	}
	OutData = UeAgentAIOps::ExportPerceptionComponent(Component);
	TArray<AActor*> KnownActors;
	TArray<AActor*> CurrentActors;
	Component->GetKnownPerceivedActors(nullptr, KnownActors);
	Component->GetCurrentlyPerceivedActors(nullptr, CurrentActors);
	TArray<TSharedPtr<FJsonValue>> KnownJson;
	for (AActor* KnownActor : KnownActors)
	{
		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), KnownActor ? KnownActor->GetName() : FString());
		ActorObj->SetStringField(TEXT("label"), KnownActor ? KnownActor->GetActorLabel() : FString());
		if (KnownActor)
		{
			FActorPerceptionBlueprintInfo Info;
			if (Component->GetActorsPerception(KnownActor, Info))
			{
				TArray<TSharedPtr<FJsonValue>> Stimuli;
				for (const FAIStimulus& Stimulus : Info.LastSensedStimuli)
				{
					Stimuli.Add(MakeShared<FJsonValueObject>(UeAgentAIOps::StimulusToJson(Stimulus)));
				}
				ActorObj->SetArrayField(TEXT("last_sensed_stimuli"), Stimuli);
			}
		}
		KnownJson.Add(MakeShared<FJsonValueObject>(ActorObj));
	}
	TArray<TSharedPtr<FJsonValue>> CurrentJson;
	for (AActor* CurrentActor : CurrentActors)
	{
		CurrentJson.Add(MakeShared<FJsonValueString>(CurrentActor ? CurrentActor->GetName() : FString()));
	}
	OutData->SetStringField(TEXT("snapshot_owner"), Actor ? Actor->GetName() : FString());
	OutData->SetArrayField(TEXT("known_actors"), KnownJson);
	OutData->SetArrayField(TEXT("currently_perceived_actors"), CurrentJson);
	OutData->SetNumberField(TEXT("known_actor_count"), KnownActors.Num());
	OutData->SetNumberField(TEXT("currently_perceived_actor_count"), CurrentActors.Num());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdAIPerceptionRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdAIPerceptionRuntimeSnapshot(Ctx, OutData, OutError))
	{
		return false;
	}
	FString StimulusActorName;
	JsonTryGetString(Ctx.Params, TEXT("stimulus_actor"), StimulusActorName);
	if (StimulusActorName.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("target_actor"), StimulusActorName);
	}
	bool bExpectedSensed = true;
	JsonTryGetBool(Ctx.Params, TEXT("expected_sensed"), bExpectedSensed);
	bool bFound = false;
	TArray<TSharedPtr<FJsonValue>> TargetStimuli;
	const TArray<TSharedPtr<FJsonValue>>* CurrentActors = nullptr;
	if (OutData->TryGetArrayField(TEXT("currently_perceived_actors"), CurrentActors) && CurrentActors)
	{
		for (const TSharedPtr<FJsonValue>& Value : *CurrentActors)
		{
			FString Name;
			if (Value.IsValid() && Value->TryGetString(Name) && Name.Equals(StimulusActorName, ESearchCase::IgnoreCase))
			{
				bFound = true;
				break;
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* KnownActors = nullptr;
	if (!StimulusActorName.IsEmpty() && OutData->TryGetArrayField(TEXT("known_actors"), KnownActors) && KnownActors)
	{
		for (const TSharedPtr<FJsonValue>& Value : *KnownActors)
		{
			const TSharedPtr<FJsonObject>* ActorObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ActorObj) || !ActorObj || !ActorObj->IsValid())
			{
				continue;
			}
			const FString Name = (*ActorObj)->GetStringField(TEXT("name"));
			const FString Label = (*ActorObj)->GetStringField(TEXT("label"));
			if (Name.Equals(StimulusActorName, ESearchCase::IgnoreCase) || Label.Equals(StimulusActorName, ESearchCase::IgnoreCase))
			{
				const TArray<TSharedPtr<FJsonValue>>* Stimuli = nullptr;
				if ((*ActorObj)->TryGetArrayField(TEXT("last_sensed_stimuli"), Stimuli) && Stimuli)
				{
					TargetStimuli = *Stimuli;
				}
				break;
			}
		}
	}
	TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
	Sample->SetNumberField(TEXT("known_actor_count"), OutData->GetNumberField(TEXT("known_actor_count")));
	Sample->SetNumberField(TEXT("currently_perceived_actor_count"), OutData->GetNumberField(TEXT("currently_perceived_actor_count")));
	Sample->SetArrayField(TEXT("currently_perceived_actors"), CurrentActors ? *CurrentActors : TArray<TSharedPtr<FJsonValue>>());
	TArray<TSharedPtr<FJsonValue>> Samples;
	Samples.Add(MakeShared<FJsonValueObject>(Sample));
	OutData->SetStringField(TEXT("stimulus_actor"), StimulusActorName);
	OutData->SetStringField(TEXT("target_actor"), StimulusActorName);
	OutData->SetArrayField(TEXT("target_stimuli"), TargetStimuli);
	OutData->SetArrayField(TEXT("samples"), Samples);
	OutData->SetBoolField(TEXT("successfully_sensed"), bFound);
	OutData->SetBoolField(TEXT("expected_sensed"), bExpectedSensed);
	OutData->SetBoolField(TEXT("probe_passed"), StimulusActorName.IsEmpty() || (bFound == bExpectedSensed));
	return true;
}

bool FUeAgentHttpServer::CmdNavigationGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		OutError = TEXT("navigation_system_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("world"), World->GetName());
	OutData->SetBoolField(TEXT("build_in_progress"), NavSys->IsNavigationBuildInProgress());
	TArray<TSharedPtr<FJsonValue>> NavDataItems;
	for (TActorIterator<ANavigationData> It(World); It; ++It)
	{
		ANavigationData* NavData = *It;
		if (!IsValid(NavData))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), NavData->GetName());
		Item->SetStringField(TEXT("class"), NavData->GetClass()->GetPathName());
		Item->SetObjectField(TEXT("location"), VecToJson(NavData->GetActorLocation()));
		NavDataItems.Add(MakeShared<FJsonValueObject>(Item));
	}
	TArray<TSharedPtr<FJsonValue>> BoundsItems;
	for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
	{
		ANavMeshBoundsVolume* Volume = *It;
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Volume->GetName());
		Item->SetStringField(TEXT("label"), Volume->GetActorLabel());
		Item->SetObjectField(TEXT("location"), VecToJson(Volume->GetActorLocation()));
		Item->SetObjectField(TEXT("extent"), VecToJson(Volume->GetComponentsBoundingBox(true).GetExtent()));
		BoundsItems.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetArrayField(TEXT("nav_data"), NavDataItems);
	OutData->SetArrayField(TEXT("bounds_volumes"), BoundsItems);
	OutData->SetNumberField(TEXT("nav_data_count"), NavDataItems.Num());
	OutData->SetNumberField(TEXT("bounds_volume_count"), BoundsItems.Num());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdNavigationExportConfigJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdNavigationGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	if (!OutputFile.TrimStartAndEnd().IsEmpty())
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

bool FUeAgentHttpServer::CmdNavigationValidateLevel(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	const TArray<TSharedPtr<FJsonValue>>* SamplePoints = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("nodes"), SamplePoints) || Ctx.Params->TryGetArrayField(TEXT("sample_points"), SamplePoints))
	{
		return CmdLevelValidateConnectivity(Ctx, OutData, OutError);
	}
	if (!CmdNavigationGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const int32 NavDataCount = static_cast<int32>(OutData->GetNumberField(TEXT("nav_data_count")));
	const int32 BoundsCount = static_cast<int32>(OutData->GetNumberField(TEXT("bounds_volume_count")));
	bool bRequiredBounds = true;
	JsonTryGetBool(Ctx.Params, TEXT("required_bounds"), bRequiredBounds);
	if (NavDataCount <= 0)
	{
		UeAgentAIOps::AddIssue(Issues, TEXT("nav_data_missing"), TEXT("No NavigationData actor found."), TEXT("$.nav_data"));
	}
	if (bRequiredBounds && BoundsCount <= 0)
	{
		UeAgentAIOps::AddIssue(Issues, TEXT("nav_bounds_missing"), TEXT("No NavMeshBoundsVolume found."), TEXT("$.bounds_volumes"));
	}
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdNavigationPathProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
	if (!Ctx.Params->TryGetArrayField(TEXT("targets"), Targets) || !Targets)
	{
		return CmdNavmeshFindPath(Ctx, OutData, OutError);
	}
	FVector Start = FVector::ZeroVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("start"), Start))
	{
		OutError = TEXT("missing_start");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	bool bAllFound = true;
	for (int32 Index = 0; Index < Targets->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* TargetObjPtr = nullptr;
		if (!(*Targets)[Index].IsValid() || !(*Targets)[Index]->TryGetObject(TargetObjPtr) || !TargetObjPtr || !TargetObjPtr->IsValid())
		{
			continue;
		}
		FVector End = FVector::ZeroVector;
		UeAgentAIOps::JsonTryGetVector(*TargetObjPtr, TEXT("location"), End);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("start"), VecToJson(Start));
		Params->SetObjectField(TEXT("end"), VecToJson(End));
		bool bAllowPartial = false;
		JsonTryGetBool(Ctx.Params, TEXT("accept_partial_path"), bAllowPartial);
		Params->SetBoolField(TEXT("allow_partial"), bAllowPartial);
		FUeAgentRequestContext ChildCtx = Ctx;
		ChildCtx.Params = Params;
		TSharedPtr<FJsonObject> PathData = MakeShared<FJsonObject>();
		FString PathError;
		const bool bOk = CmdNavmeshFindPath(ChildCtx, PathData, PathError);
		PathData->SetNumberField(TEXT("target_index"), Index);
		if (!bOk)
		{
			PathData->SetStringField(TEXT("error"), PathError);
			bAllFound = false;
		}
		else
		{
			bool bPathFound = false;
			PathData->TryGetBoolField(TEXT("path_found"), bPathFound);
			bAllFound = bAllFound && bPathFound;
		}
		Results.Add(MakeShared<FJsonValueObject>(PathData));
	}
	OutData->SetObjectField(TEXT("start"), VecToJson(Start));
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetBoolField(TEXT("all_paths_found"), bAllFound);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdNavigationAreaCostProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	bool bRunPathProbe = Ctx.Params->HasField(TEXT("start")) && (Ctx.Params->HasField(TEXT("end")) || Ctx.Params->HasField(TEXT("target")));
	JsonTryGetBool(Ctx.Params, TEXT("run_path_probe"), bRunPathProbe);
	if (bRunPathProbe)
	{
		if (!CmdNavmeshFindPath(Ctx, OutData, OutError))
		{
			return false;
		}
	}
	else
	{
		OutData = MakeShared<FJsonObject>();
	}
	const TArray<TSharedPtr<FJsonValue>>* ExpectedAreas = nullptr;
	TArray<TSharedPtr<FJsonValue>> AreasOut;
	if (Ctx.Params->TryGetArrayField(TEXT("expected_areas"), ExpectedAreas) && ExpectedAreas)
	{
		for (const TSharedPtr<FJsonValue>& AreaValue : *ExpectedAreas)
		{
			FString AreaClassPath;
			if (!AreaValue.IsValid() || !AreaValue->TryGetString(AreaClassPath))
			{
				continue;
			}
			UClass* AreaClass = UeAgentAIOps::ResolveClass(AreaClassPath);
			TSharedPtr<FJsonObject> AreaObj = MakeShared<FJsonObject>();
			AreaObj->SetStringField(TEXT("class"), AreaClass ? AreaClass->GetPathName() : AreaClassPath);
			AreaObj->SetBoolField(TEXT("found"), AreaClass && AreaClass->IsChildOf(UNavArea::StaticClass()));
			if (AreaClass && AreaClass->IsChildOf(UNavArea::StaticClass()))
			{
				const UNavArea* AreaCDO = Cast<UNavArea>(AreaClass->GetDefaultObject());
				AreaObj->SetNumberField(TEXT("default_cost"), AreaCDO ? AreaCDO->DefaultCost : 0.0);
				float FixedAreaEnteringCost = 0.0f;
				AreaObj->SetNumberField(TEXT("fixed_area_entering_cost"), (AreaCDO && UeAgentAIOps::ReadReflectedFloat(AreaCDO->GetClass(), AreaCDO, TEXT("FixedAreaEnteringCost"), FixedAreaEnteringCost)) ? FixedAreaEnteringCost : 0.0f);
			}
			AreasOut.Add(MakeShared<FJsonValueObject>(AreaObj));
		}
	}
	OutData->SetArrayField(TEXT("area_costs"), AreasOut);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdNavigationRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdNavigationGetInfo(Ctx, OutData, OutError);
}

namespace UeAgentAIOps
{
	static bool ResetSmartObjectSlots(USmartObjectDefinition* Definition, FString& OutError)
	{
		if (!Definition)
		{
			OutError = TEXT("smart_object_definition_not_found");
			return false;
		}
		FArrayProperty* SlotsProperty = FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("Slots"));
		if (!SlotsProperty)
		{
			OutError = TEXT("smart_object_slots_property_not_found");
			return false;
		}
		void* SlotsPtr = SlotsProperty->ContainerPtrToValuePtr<void>(Definition);
		FScriptArrayHelper Helper(SlotsProperty, SlotsPtr);
		Helper.EmptyValues();
		return true;
	}

	static FSmartObjectSlotDefinition& AddSmartObjectSlot(USmartObjectDefinition* Definition)
	{
		return Definition->DebugAddSlot();
	}

	static bool CreateSmartObjectBehaviorDefinition(UObject* Outer, const TSharedPtr<FJsonObject>& BehaviorObj, USmartObjectBehaviorDefinition*& OutBehavior, TArray<TSharedPtr<FJsonValue>>& PropertyResults, FString& OutError)
	{
		OutBehavior = nullptr;
		if (!Outer || !BehaviorObj.IsValid())
		{
			OutError = TEXT("invalid_smart_object_behavior_definition");
			return false;
		}
		FString BehaviorClassPath;
		if (!BehaviorObj->TryGetStringField(TEXT("class"), BehaviorClassPath) || BehaviorClassPath.IsEmpty())
		{
			OutError = TEXT("missing_smart_object_behavior_definition_class");
			return false;
		}
		UClass* BehaviorClass = ResolveClass(BehaviorClassPath);
		if (!BehaviorClass || !BehaviorClass->IsChildOf(USmartObjectBehaviorDefinition::StaticClass()) || BehaviorClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("smart_object_behavior_definition_class_not_found");
			return false;
		}
		OutBehavior = NewObject<USmartObjectBehaviorDefinition>(Outer, BehaviorClass, NAME_None, RF_Transactional);
		if (!OutBehavior)
		{
			OutError = TEXT("smart_object_behavior_definition_create_failed");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (BehaviorObj->TryGetArrayField(TEXT("properties"), Properties) && Properties)
		{
			for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
			{
				const TSharedPtr<FJsonObject>* PropObjPtr = nullptr;
				if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropObjPtr) || !PropObjPtr || !PropObjPtr->IsValid())
				{
					continue;
				}
				FString PropertyName;
				if (!(*PropObjPtr)->TryGetStringField(TEXT("property_name"), PropertyName))
				{
					(*PropObjPtr)->TryGetStringField(TEXT("name"), PropertyName);
				}
				FString ValueText;
				if (!(*PropObjPtr)->TryGetStringField(TEXT("value_text"), ValueText))
				{
					(*PropObjPtr)->TryGetStringField(TEXT("value"), ValueText);
				}
				if (PropertyName.IsEmpty())
				{
					OutError = TEXT("missing_smart_object_behavior_property_name");
					return false;
				}
				FString ImportError;
				if (!ValueText.IsEmpty() && !ImportPropertyValue(OutBehavior->GetClass(), OutBehavior, OutBehavior, PropertyName, ValueText, ImportError))
				{
					OutError = ImportError;
					return false;
				}
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("owner"), OutBehavior->GetName());
				Result->SetStringField(TEXT("property_name"), PropertyName);
				Result->SetStringField(TEXT("requested_value_text"), ValueText);
				Result->SetStringField(TEXT("status"), TEXT("applied"));
				PropertyResults.Add(MakeShared<FJsonValueObject>(Result));
			}
		}
		return true;
	}

	static bool AddSmartObjectBehaviorDefinitions(UObject* Outer, const TArray<TSharedPtr<FJsonValue>>* Behaviors, TArray<TObjectPtr<USmartObjectBehaviorDefinition>>& Target, TArray<TSharedPtr<FJsonValue>>& PropertyResults, FString& OutError)
	{
		if (!Behaviors)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& BehaviorValue : *Behaviors)
		{
			const TSharedPtr<FJsonObject>* BehaviorObjPtr = nullptr;
			if (!BehaviorValue.IsValid() || !BehaviorValue->TryGetObject(BehaviorObjPtr) || !BehaviorObjPtr || !BehaviorObjPtr->IsValid())
			{
				continue;
			}
			USmartObjectBehaviorDefinition* Behavior = nullptr;
			if (!CreateSmartObjectBehaviorDefinition(Outer, *BehaviorObjPtr, Behavior, PropertyResults, OutError))
			{
				return false;
			}
			Target.Add(Behavior);
		}
		return true;
	}

	static bool ResetDefaultSmartObjectBehaviorDefinitions(USmartObjectDefinition* Definition, FString& OutError)
	{
		FArrayProperty* DefaultsProperty = Definition ? FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("DefaultBehaviorDefinitions")) : nullptr;
		if (!DefaultsProperty)
		{
			OutError = TEXT("smart_object_default_behaviors_property_not_found");
			return false;
		}
		void* DefaultsPtr = DefaultsProperty->ContainerPtrToValuePtr<void>(Definition);
		FScriptArrayHelper Helper(DefaultsProperty, DefaultsPtr);
		Helper.EmptyValues();
		return true;
	}

	static bool AddDefaultSmartObjectBehaviorDefinition(USmartObjectDefinition* Definition, USmartObjectBehaviorDefinition* Behavior, FString& OutError)
	{
		FArrayProperty* DefaultsProperty = Definition ? FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("DefaultBehaviorDefinitions")) : nullptr;
		FObjectPropertyBase* ObjectProperty = DefaultsProperty ? CastField<FObjectPropertyBase>(DefaultsProperty->Inner) : nullptr;
		if (!DefaultsProperty || !ObjectProperty)
		{
			OutError = TEXT("smart_object_default_behaviors_property_not_found");
			return false;
		}
		void* DefaultsPtr = DefaultsProperty->ContainerPtrToValuePtr<void>(Definition);
		FScriptArrayHelper Helper(DefaultsProperty, DefaultsPtr);
		const int32 NewIndex = Helper.AddValue();
		ObjectProperty->SetObjectPropertyValue(Helper.GetRawPtr(NewIndex), Behavior);
		return true;
	}

	static bool ApplyDefaultSmartObjectBehaviorDefinitions(USmartObjectDefinition* Definition, const TArray<TSharedPtr<FJsonValue>>* Behaviors, TArray<TSharedPtr<FJsonValue>>& PropertyResults, FString& OutError)
	{
		if (!Behaviors)
		{
			return true;
		}
		if (!ResetDefaultSmartObjectBehaviorDefinitions(Definition, OutError))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& BehaviorValue : *Behaviors)
		{
			const TSharedPtr<FJsonObject>* BehaviorObjPtr = nullptr;
			if (!BehaviorValue.IsValid() || !BehaviorValue->TryGetObject(BehaviorObjPtr) || !BehaviorObjPtr || !BehaviorObjPtr->IsValid())
			{
				continue;
			}
			USmartObjectBehaviorDefinition* Behavior = nullptr;
			if (!CreateSmartObjectBehaviorDefinition(Definition, *BehaviorObjPtr, Behavior, PropertyResults, OutError))
			{
				return false;
			}
			if (!AddDefaultSmartObjectBehaviorDefinition(Definition, Behavior, OutError))
			{
				return false;
			}
		}
		return true;
	}

	static bool ParseSmartObjectQueryBox(UWorld* World, const TSharedPtr<FJsonObject>& Params, FBox& OutBox, AActor*& OutAgent)
	{
		OutAgent = nullptr;
		FString AgentName;
		Params->TryGetStringField(TEXT("agent"), AgentName);
		if (!AgentName.IsEmpty() && World)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetName().Equals(AgentName, ESearchCase::IgnoreCase) || It->GetActorLabel().Equals(AgentName, ESearchCase::IgnoreCase))
				{
					OutAgent = *It;
					break;
				}
			}
		}
		const TSharedPtr<FJsonObject>* BoxObjPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("query_bounds"), BoxObjPtr) && BoxObjPtr && BoxObjPtr->IsValid())
		{
			FVector Min;
			FVector Max;
			FVector Center;
			FVector Extent;
			if (JsonTryGetVector(*BoxObjPtr, TEXT("min"), Min) && JsonTryGetVector(*BoxObjPtr, TEXT("max"), Max))
			{
				OutBox = FBox(Min, Max);
				return true;
			}
			if (JsonTryGetVector(*BoxObjPtr, TEXT("center"), Center) && JsonTryGetVector(*BoxObjPtr, TEXT("extent"), Extent))
			{
				OutBox = FBox::BuildAABB(Center, Extent);
				return true;
			}
		}
		double Radius = 2000.0;
		Params->TryGetNumberField(TEXT("radius"), Radius);
		const FVector Center = OutAgent ? OutAgent->GetActorLocation() : FVector::ZeroVector;
		OutBox = FBox::BuildAABB(Center, FVector(Radius));
		return true;
	}

	static void RegisterSmartObjectComponentsInWorld(UWorld* World, USmartObjectSubsystem* Subsystem)
	{
		if (!World || !Subsystem)
		{
			return;
		}
#if WITH_SMARTOBJECT_DEBUG
		Subsystem->DebugInitializeRuntime();
#endif
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<USmartObjectComponent*> Components;
			It->GetComponents<USmartObjectComponent>(Components);
			for (USmartObjectComponent* Component : Components)
			{
				if (Component && Component->GetDefinition())
				{
					Subsystem->RegisterSmartObject(Component);
				}
			}
		}
	}

	static void CollectSmartObjectActorsForQuery(UWorld* World, USmartObjectSubsystem* Subsystem, const FBox& QueryBox, TArray<AActor*>& OutActors, TArray<TSharedPtr<FJsonValue>>& OutDiagnostics)
	{
		OutActors.Reset();
		OutDiagnostics.Reset();
		if (!World || !Subsystem)
		{
			return;
		}
		TSet<AActor*> SeenActors;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			TArray<USmartObjectComponent*> Components;
			Actor->GetComponents<USmartObjectComponent>(Components);
			for (USmartObjectComponent* Component : Components)
			{
				if (!Component || !Component->GetDefinition())
				{
					continue;
				}
				const FBox ComponentBounds = Component->GetSmartObjectBounds();
				const FVector ComponentLocation = Component->GetComponentLocation();
				const FVector ActorLocation = Actor->GetActorLocation();
				const bool bInQuery = QueryBox.Intersect(ComponentBounds) || QueryBox.IsInside(ComponentLocation) || QueryBox.IsInside(ActorLocation);
				TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
				Diagnostic->SetStringField(TEXT("owner"), Actor->GetName());
				Diagnostic->SetStringField(TEXT("component"), Component->GetName());
				Diagnostic->SetStringField(TEXT("definition"), Component->GetDefinition()->GetPathName());
				Diagnostic->SetObjectField(TEXT("actor_location"), VecToJson(ActorLocation));
				Diagnostic->SetObjectField(TEXT("component_location"), VecToJson(ComponentLocation));
				Diagnostic->SetObjectField(TEXT("bounds_min"), VecToJson(ComponentBounds.Min));
				Diagnostic->SetObjectField(TEXT("bounds_max"), VecToJson(ComponentBounds.Max));
				Diagnostic->SetBoolField(TEXT("in_query"), bInQuery);
				Diagnostic->SetBoolField(TEXT("bound_to_simulation"), Component->IsBoundToSimulation());
				Diagnostic->SetStringField(TEXT("registered_handle"), LexToString(Component->GetRegisteredHandle()));
				OutDiagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
				if (bInQuery && !SeenActors.Contains(Actor))
				{
					SeenActors.Add(Actor);
					OutActors.Add(Actor);
				}
			}
		}
	}

	static int32 CreateSmartObjectRuntimeEntriesForQuery(UWorld* World, USmartObjectSubsystem* Subsystem, const FBox& QueryBox, const FConstStructView UserData)
	{
		if (!World || !Subsystem)
		{
			return 0;
		}
#if WITH_SMARTOBJECT_DEBUG
		Subsystem->DebugInitializeRuntime();
#endif
		int32 CreatedCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<USmartObjectComponent*> Components;
			It->GetComponents<USmartObjectComponent>(Components);
			for (USmartObjectComponent* Component : Components)
			{
				const USmartObjectDefinition* Definition = Component ? Component->GetDefinition() : nullptr;
				if (!Component || !Definition)
				{
					continue;
				}
				const FBox ComponentBounds = Component->GetSmartObjectBounds();
				const FVector ComponentLocation = Component->GetComponentLocation();
				const AActor* Owner = Component->GetOwner();
				const FVector OwnerLocation = Owner ? Owner->GetActorLocation() : ComponentLocation;
				if (!QueryBox.Intersect(ComponentBounds) && !QueryBox.IsInside(ComponentLocation) && !QueryBox.IsInside(OwnerLocation))
				{
					continue;
				}
				if (Definition->HasBeenValidated() == false)
				{
					Definition->Validate();
				}
				if (!Definition->IsDefinitionValid())
				{
					continue;
				}
				const FTransform RuntimeTransform = Owner ? Owner->GetActorTransform() : Component->GetComponentTransform();
				const FSmartObjectHandle Handle = Subsystem->CreateSmartObject(*Definition, RuntimeTransform, UserData);
				if (Handle.IsValid())
				{
					++CreatedCount;
				}
			}
		}
		return CreatedCount;
	}
}

bool FUeAgentHttpServer::CmdSmartObjectDefinitionCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	USmartObjectDefinition* Definition = nullptr;
	if (!UeAgentAIOps::CreateAsset<USmartObjectDefinition>(AssetPath, Definition, OutError) || !Definition)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
	if (!Ctx.Params->TryGetArrayField(TEXT("activity_tags"), Tags))
	{
		Ctx.Params->TryGetArrayField(TEXT("tags"), Tags);
	}
	if (Tags)
	{
		Definition->SetActivityTags(UeAgentAIOps::GameplayTagsFromJsonArray(Tags));
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	const TArray<TSharedPtr<FJsonValue>>* DefaultBehaviors = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("default_behavior_definitions"), DefaultBehaviors))
	{
		if (!UeAgentAIOps::ApplyDefaultSmartObjectBehaviorDefinitions(Definition, DefaultBehaviors, PropertyResults, OutError))
		{
			return false;
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("slots"), Slots) && Slots)
	{
		for (const TSharedPtr<FJsonValue>& SlotValue : *Slots)
		{
			const TSharedPtr<FJsonObject>* SlotObjPtr = nullptr;
			if (!SlotValue.IsValid() || !SlotValue->TryGetObject(SlotObjPtr) || !SlotObjPtr || !SlotObjPtr->IsValid())
			{
				continue;
			}
			FSmartObjectSlotDefinition& Slot = UeAgentAIOps::AddSmartObjectSlot(Definition);
			FVector Offset = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FTransform LocalTransform;
			if (UeAgentAIOps::TryGetTransform(*SlotObjPtr, TEXT("local_transform"), LocalTransform))
			{
				Offset = LocalTransform.GetLocation();
				Rotation = LocalTransform.Rotator();
			}
			else
			{
				UeAgentAIOps::JsonTryGetVector(*SlotObjPtr, TEXT("offset"), Offset);
				UeAgentAIOps::JsonTryGetRotator(*SlotObjPtr, TEXT("rotation"), Rotation);
			}
			Slot.Offset = FVector3f(Offset);
			Slot.Rotation = FRotator3f(Rotation);
			bool bEnabled = true;
			(*SlotObjPtr)->TryGetBoolField(TEXT("enabled"), bEnabled);
			Slot.bEnabled = bEnabled;
#if WITH_EDITORONLY_DATA
			FString Name;
			if ((*SlotObjPtr)->TryGetStringField(TEXT("name"), Name))
			{
				Slot.Name = FName(*Name);
			}
#endif
			const TArray<TSharedPtr<FJsonValue>>* SlotTags = nullptr;
			if (!(*SlotObjPtr)->TryGetArrayField(TEXT("activity_tags"), SlotTags))
			{
				(*SlotObjPtr)->TryGetArrayField(TEXT("tags"), SlotTags);
			}
			if (SlotTags)
			{
				Slot.ActivityTags = UeAgentAIOps::GameplayTagsFromJsonArray(SlotTags);
			}
			if ((*SlotObjPtr)->TryGetArrayField(TEXT("runtime_tags"), SlotTags))
			{
				Slot.RuntimeTags = UeAgentAIOps::GameplayTagsFromJsonArray(SlotTags);
			}
			const TArray<TSharedPtr<FJsonValue>>* Behaviors = nullptr;
			if ((*SlotObjPtr)->TryGetArrayField(TEXT("behavior_definitions"), Behaviors))
			{
				if (!UeAgentAIOps::AddSmartObjectBehaviorDefinitions(Definition, Behaviors, Slot.BehaviorDefinitions, PropertyResults, OutError))
				{
					return false;
				}
			}
		}
	}
	else
	{
		UeAgentAIOps::AddSmartObjectSlot(Definition);
	}
	Definition->MarkPackageDirty();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Definition, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportSmartObjectDefinition(Definition, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdSmartObjectDefinitionGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	USmartObjectDefinition* Definition = UeAgentAIOps::LoadAsset<USmartObjectDefinition>(AssetPath);
	if (!Definition)
	{
		OutError = TEXT("smart_object_definition_not_found");
		return false;
	}
	OutData = UeAgentAIOps::ExportSmartObjectDefinition(Definition, true);
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdSmartObjectDefinitionExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdSmartObjectDefinitionGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	if (!OutputFile.TrimStartAndEnd().IsEmpty())
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

bool FUeAgentHttpServer::CmdSmartObjectDefinitionValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.IsEmpty())
	{
		OutError = TEXT("missing_json_file");
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!UeAgentAIOps::LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	UeAgentAIOps::ValidateSmartObjectDefinitionJson(Root, Issues);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::SmartObjectDefinitionProfile);
	OutData->SetStringField(TEXT("json_file"), ResolvedPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdSmartObjectDefinitionApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
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
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!UeAgentAIOps::ValidateSmartObjectDefinitionJson(Root, Issues))
	{
		OutData->SetStringField(TEXT("status"), TEXT("validate_failed"));
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return false;
	}
	bool bDryRun = false;
	bool bValidateOnly = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	if (bDryRun || bValidateOnly)
	{
		OutData->SetStringField(TEXT("profile"), UeAgentAIOps::SmartObjectDefinitionProfile);
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return true;
	}
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	bool bCreateIfMissing = false;
	bool bAllowDestructive = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	USmartObjectDefinition* Definition = UeAgentAIOps::LoadAsset<USmartObjectDefinition>(AssetPath);
	bool bCreated = false;
	if (!Definition)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("smart_object_definition_not_found");
			return false;
		}
		if (!UeAgentAIOps::CreateAsset<USmartObjectDefinition>(AssetPath, Definition, OutError) || !Definition)
		{
			return false;
		}
		bCreated = true;
	}
	if (Definition->GetSlots().Num() > 0 && !bCreated && !bAllowDestructive)
	{
		OutError = TEXT("smart_object_slot_rebuild_requires_allow_destructive");
		return false;
	}
	Definition->Modify();
	const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
	if (!Root->TryGetArrayField(TEXT("activity_tags"), Tags))
	{
		Root->TryGetArrayField(TEXT("tags"), Tags);
	}
	if (Tags)
	{
		Definition->SetActivityTags(UeAgentAIOps::GameplayTagsFromJsonArray(Tags));
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	const TArray<TSharedPtr<FJsonValue>>* DefaultBehaviors = nullptr;
	if (Root->TryGetArrayField(TEXT("default_behavior_definitions"), DefaultBehaviors))
	{
		if (!UeAgentAIOps::ApplyDefaultSmartObjectBehaviorDefinitions(Definition, DefaultBehaviors, PropertyResults, OutError))
		{
			return false;
		}
	}
	if (!UeAgentAIOps::ResetSmartObjectSlots(Definition, OutError))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
	Root->TryGetArrayField(TEXT("slots"), Slots);
	if (Slots)
	{
		for (const TSharedPtr<FJsonValue>& SlotValue : *Slots)
		{
			const TSharedPtr<FJsonObject>* SlotObjPtr = nullptr;
			if (!SlotValue.IsValid() || !SlotValue->TryGetObject(SlotObjPtr) || !SlotObjPtr || !SlotObjPtr->IsValid())
			{
				continue;
			}
			FSmartObjectSlotDefinition& Slot = UeAgentAIOps::AddSmartObjectSlot(Definition);
			FVector Offset = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FTransform LocalTransform;
			if (UeAgentAIOps::TryGetTransform(*SlotObjPtr, TEXT("local_transform"), LocalTransform))
			{
				Offset = LocalTransform.GetLocation();
				Rotation = LocalTransform.Rotator();
			}
			else
			{
				UeAgentAIOps::JsonTryGetVector(*SlotObjPtr, TEXT("offset"), Offset);
				UeAgentAIOps::JsonTryGetRotator(*SlotObjPtr, TEXT("rotation"), Rotation);
			}
			Slot.Offset = FVector3f(Offset);
			Slot.Rotation = FRotator3f(Rotation);
			bool bEnabled = true;
			(*SlotObjPtr)->TryGetBoolField(TEXT("enabled"), bEnabled);
			Slot.bEnabled = bEnabled;
#if WITH_EDITORONLY_DATA
			FString Name;
			if ((*SlotObjPtr)->TryGetStringField(TEXT("name"), Name))
			{
				Slot.Name = FName(*Name);
			}
#endif
			const TArray<TSharedPtr<FJsonValue>>* SlotTags = nullptr;
			if (!(*SlotObjPtr)->TryGetArrayField(TEXT("activity_tags"), SlotTags))
			{
				(*SlotObjPtr)->TryGetArrayField(TEXT("tags"), SlotTags);
			}
			if (SlotTags)
			{
				Slot.ActivityTags = UeAgentAIOps::GameplayTagsFromJsonArray(SlotTags);
			}
			if ((*SlotObjPtr)->TryGetArrayField(TEXT("runtime_tags"), SlotTags))
			{
				Slot.RuntimeTags = UeAgentAIOps::GameplayTagsFromJsonArray(SlotTags);
			}
			const TArray<TSharedPtr<FJsonValue>>* Behaviors = nullptr;
			if ((*SlotObjPtr)->TryGetArrayField(TEXT("behavior_definitions"), Behaviors) && Behaviors)
			{
				if (!UeAgentAIOps::AddSmartObjectBehaviorDefinitions(Definition, Behaviors, Slot.BehaviorDefinitions, PropertyResults, OutError))
				{
					return false;
				}
			}
		}
	}
	Definition->MarkPackageDirty();
	Definition->PostEditChange();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Definition, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportSmartObjectDefinition(Definition, true);
	OutData->SetStringField(TEXT("json_file"), ResolvedPath);
	OutData->SetBoolField(TEXT("created_asset"), bCreated);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdSmartObjectValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString DefinitionPath;
	JsonTryGetString(Ctx.Params, TEXT("definition_asset"), DefinitionPath);
	USmartObjectDefinition* Definition = DefinitionPath.IsEmpty() ? nullptr : UeAgentAIOps::LoadAsset<USmartObjectDefinition>(DefinitionPath);
	if (!Definition)
	{
		UeAgentAIOps::AddIssue(Issues, TEXT("definition_not_found"), TEXT("Smart Object Definition was not found."), TEXT("$.definition_asset"));
	}
	else
	{
		OutData->SetObjectField(TEXT("definition"), UeAgentAIOps::ExportSmartObjectDefinition(Definition, true));
		TArray<TPair<EMessageSeverity::Type, FText>> ValidationMessages;
		if (!Definition->Validate(&ValidationMessages))
		{
			for (const TPair<EMessageSeverity::Type, FText>& Message : ValidationMessages)
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("definition_validation_failed"), Message.Value.ToString(), TEXT("$.definition_asset"));
			}
		}
	}
	UWorld* World = GetEditorWorld(OutError);
	if (World)
	{
		FString ActorName;
		JsonTryGetString(Ctx.Params, TEXT("owner_actor_or_blueprint"), ActorName);
		AActor* Actor = ActorName.IsEmpty() ? nullptr : FindActorByNameOrLabel(World, ActorName);
		if (Actor)
		{
			USmartObjectComponent* Component = Actor->FindComponentByClass<USmartObjectComponent>();
			if (!Component)
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("smart_object_component_missing"), TEXT("Owner actor has no SmartObjectComponent."), TEXT("$.owner_actor_or_blueprint"));
			}
			else
			{
				OutData->SetStringField(TEXT("component_owner"), Actor->GetName());
				OutData->SetStringField(TEXT("component_definition"), Component->GetDefinition() ? Component->GetDefinition()->GetPathName() : FString());
				if (Definition && Component->GetDefinition() != Definition)
				{
					UeAgentAIOps::AddIssue(Issues, TEXT("component_definition_mismatch"), TEXT("SmartObjectComponent references a different definition."), TEXT("$.owner_actor_or_blueprint"));
				}
			}
		}
	}
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdSmartObjectFind(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	USmartObjectSubsystem* Subsystem = USmartObjectSubsystem::GetCurrent(World);
	if (!Subsystem)
	{
		OutError = TEXT("smart_object_subsystem_not_found");
		return false;
	}
	UeAgentAIOps::RegisterSmartObjectComponentsInWorld(World, Subsystem);
	AActor* Agent = nullptr;
	FBox QueryBox(ForceInitToZero);
	UeAgentAIOps::ParseSmartObjectQueryBox(World, Ctx.Params, QueryBox, Agent);
	FSmartObjectRequestFilter Filter;
	const TArray<TSharedPtr<FJsonValue>>* RequiredTags = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("required_tags"), RequiredTags))
	{
		Filter.UserTags = UeAgentAIOps::GameplayTagsFromJsonArray(RequiredTags);
	}
	bool bIncludeClaimed = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_claimed"), bIncludeClaimed);
	Filter.bShouldIncludeClaimedSlots = bIncludeClaimed;
	FSmartObjectRequest Request(QueryBox, Filter);
	TArray<FSmartObjectRequestResult> Results;
	FSmartObjectActorUserData UserData(Agent);
	const FConstStructView UserDataView = Agent ? FConstStructView::Make(UserData) : FConstStructView();
	Subsystem->FindSmartObjects(Request, Results, UserDataView);
	TArray<AActor*> CandidateActors;
	TArray<TSharedPtr<FJsonValue>> CandidateDiagnostics;
	UeAgentAIOps::CollectSmartObjectActorsForQuery(World, Subsystem, QueryBox, CandidateActors, CandidateDiagnostics);
	bool bUsedActorListFallback = false;
	int32 CreatedRuntimeEntries = 0;
	if (Results.Num() == 0)
	{
		CreatedRuntimeEntries = UeAgentAIOps::CreateSmartObjectRuntimeEntriesForQuery(World, Subsystem, QueryBox, UserDataView);
		if (CreatedRuntimeEntries > 0)
		{
			Results.Reset();
			Subsystem->FindSmartObjects(Request, Results, UserDataView);
		}
	}
	if (Results.Num() == 0 && CandidateActors.Num() > 0)
	{
		Subsystem->FindSmartObjectsInList(Filter, CandidateActors, Results, UserDataView);
		bUsedActorListFallback = Results.Num() > 0;
	}
	int32 MaxResults = 16;
	double MaxResultsNumber = MaxResults;
	JsonTryGetNumber(Ctx.Params, TEXT("max_results"), MaxResultsNumber);
	MaxResults = (int32)FMath::Clamp(MaxResultsNumber, 1.0, 1024.0);
	TArray<TSharedPtr<FJsonValue>> ResultsJson;
	UeAgentAIOps::GSmartObjectFindResults.Reset();
	const int32 EmitCount = FMath::Min(MaxResults, Results.Num());
	for (int32 Index = 0; Index < EmitCount; ++Index)
	{
		const FString ResultId = FString::Printf(TEXT("result_%d"), Index);
		UeAgentAIOps::GSmartObjectFindResults.Add(ResultId, Results[Index]);
		ResultsJson.Add(MakeShared<FJsonValueObject>(UeAgentAIOps::SmartObjectRequestResultToJson(Results[Index], Subsystem, ResultId)));
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetObjectField(TEXT("query_min"), VecToJson(QueryBox.Min));
	OutData->SetObjectField(TEXT("query_max"), VecToJson(QueryBox.Max));
	OutData->SetStringField(TEXT("agent"), Agent ? Agent->GetName() : FString());
	OutData->SetNumberField(TEXT("result_count"), Results.Num());
	OutData->SetNumberField(TEXT("created_runtime_entries_for_query"), CreatedRuntimeEntries);
	OutData->SetNumberField(TEXT("candidate_actor_count"), CandidateActors.Num());
	OutData->SetBoolField(TEXT("used_actor_list_fallback"), bUsedActorListFallback);
	OutData->SetArrayField(TEXT("candidate_components"), CandidateDiagnostics);
	OutData->SetBoolField(TEXT("truncated"), Results.Num() > EmitCount);
	OutData->SetArrayField(TEXT("results"), ResultsJson);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdSmartObjectClaim(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	USmartObjectSubsystem* Subsystem = USmartObjectSubsystem::GetCurrent(World);
	if (!Subsystem)
	{
		OutError = TEXT("smart_object_subsystem_not_found");
		return false;
	}
	FString ResultId;
	JsonTryGetString(Ctx.Params, TEXT("result_id"), ResultId);
	if (ResultId.IsEmpty())
	{
		ResultId = TEXT("result_0");
	}
	const FSmartObjectRequestResult* Result = UeAgentAIOps::GSmartObjectFindResults.Find(ResultId);
	if (!Result || !Result->IsValid())
	{
		OutError = TEXT("smart_object_find_result_not_found");
		return false;
	}
	FString PriorityText;
	JsonTryGetString(Ctx.Params, TEXT("claim_priority"), PriorityText);
	const ESmartObjectClaimPriority Priority = UeAgentAIOps::EnumFromString(PriorityText, ESmartObjectClaimPriority::Normal);
	if (!Subsystem->CanBeClaimed(Result->SlotHandle, Priority))
	{
		OutError = TEXT("smart_object_slot_not_claimable");
		return false;
	}
	FString AgentName;
	JsonTryGetString(Ctx.Params, TEXT("agent"), AgentName);
	AActor* Agent = AgentName.IsEmpty() ? nullptr : FindActorByNameOrLabel(World, AgentName);
	FSmartObjectActorUserData UserData(Agent);
	const FConstStructView UserDataView = Agent ? FConstStructView::Make(UserData) : FConstStructView();
	const FSmartObjectClaimHandle Claim = Subsystem->MarkSlotAsClaimed(Result->SlotHandle, Priority, UserDataView);
	if (!Claim.IsValid())
	{
		OutError = TEXT("smart_object_claim_failed");
		return false;
	}
	const FString ClaimId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UeAgentAIOps::GSmartObjectClaims.Add(ClaimId, Claim);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("claim_handle"), ClaimId);
	OutData->SetStringField(TEXT("claim_debug"), LexToString(Claim));
	OutData->SetObjectField(TEXT("result"), UeAgentAIOps::SmartObjectRequestResultToJson(*Result, Subsystem, ResultId));
	if (TOptional<FTransform> SlotTransform = Subsystem->GetSlotTransform(Claim))
	{
		OutData->SetObjectField(TEXT("slot_transform"), UeAgentAIOps::TransformToJson(*SlotTransform));
	}
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdSmartObjectRelease(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	USmartObjectSubsystem* Subsystem = USmartObjectSubsystem::GetCurrent(World);
	if (!Subsystem)
	{
		OutError = TEXT("smart_object_subsystem_not_found");
		return false;
	}
	FString ClaimId;
	if (!JsonTryGetString(Ctx.Params, TEXT("claim_handle"), ClaimId) || ClaimId.IsEmpty())
	{
		OutError = TEXT("missing_claim_handle");
		return false;
	}
	FSmartObjectClaimHandle Claim;
	if (!UeAgentAIOps::GSmartObjectClaims.RemoveAndCopyValue(ClaimId, Claim))
	{
		OutError = TEXT("smart_object_claim_handle_not_found");
		return false;
	}
	const bool bReleased = Subsystem->MarkSlotAsFree(Claim);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("claim_handle"), ClaimId);
	OutData->SetBoolField(TEXT("released"), bReleased);
	OutData->SetStringField(TEXT("status"), bReleased ? TEXT("ok") : TEXT("release_failed"));
	return bReleased;
}

bool FUeAgentHttpServer::CmdSmartObjectRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Components;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TArray<USmartObjectComponent*> SmartComponents;
		It->GetComponents<USmartObjectComponent>(SmartComponents);
		for (USmartObjectComponent* Component : SmartComponents)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("owner"), (*It) ? (*It)->GetName() : FString());
			Obj->SetStringField(TEXT("component"), Component ? Component->GetName() : FString());
			Obj->SetStringField(TEXT("definition"), (Component && Component->GetDefinition()) ? Component->GetDefinition()->GetPathName() : FString());
			Obj->SetBoolField(TEXT("bound_to_simulation"), Component && Component->IsBoundToSimulation());
			if (Component)
			{
				Obj->SetStringField(TEXT("registered_handle"), LexToString(Component->GetRegisteredHandle()));
			}
			Components.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}
	OutData->SetArrayField(TEXT("components"), Components);
	OutData->SetNumberField(TEXT("component_count"), Components.Num());
	OutData->SetNumberField(TEXT("cached_find_result_count"), UeAgentAIOps::GSmartObjectFindResults.Num());
	OutData->SetNumberField(TEXT("active_claim_count"), UeAgentAIOps::GSmartObjectClaims.Num());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdSmartObjectRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> FindData;
	if (!CmdSmartObjectFind(Ctx, FindData, OutError))
	{
		return false;
	}
	bool bExpectClaimable = true;
	JsonTryGetBool(Ctx.Params, TEXT("expect_claimable"), bExpectClaimable);
	const int32 ResultCount = static_cast<int32>(FindData->GetNumberField(TEXT("result_count")));
	OutData = MakeShared<FJsonObject>();
	OutData->SetObjectField(TEXT("find"), FindData);
	OutData->SetObjectField(TEXT("find_result"), FindData);
	OutData->SetBoolField(TEXT("expect_claimable"), bExpectClaimable);
	OutData->SetBoolField(TEXT("found_any"), ResultCount > 0);
	bool bExpectReachable = false;
	JsonTryGetBool(Ctx.Params, TEXT("expect_reachable"), bExpectReachable);
	OutData->SetBoolField(TEXT("expect_reachable"), bExpectReachable);
	if (ResultCount <= 0)
	{
		OutData->SetBoolField(TEXT("probe_passed"), !bExpectClaimable);
		TSharedPtr<FJsonObject> NavigationResult = MakeShared<FJsonObject>();
		NavigationResult->SetBoolField(TEXT("evaluated"), false);
		NavigationResult->SetStringField(TEXT("reason"), TEXT("no_smart_object_result"));
		OutData->SetObjectField(TEXT("navigation_result"), NavigationResult);
		OutData->SetArrayField(TEXT("cleanup_results"), TArray<TSharedPtr<FJsonValue>>());
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		return true;
	}
	TSharedPtr<FJsonObject> ClaimParams = MakeShared<FJsonObject>();
	ClaimParams->SetStringField(TEXT("result_id"), TEXT("result_0"));
	FString Agent;
	if (JsonTryGetString(Ctx.Params, TEXT("agent"), Agent))
	{
		ClaimParams->SetStringField(TEXT("agent"), Agent);
	}
	FUeAgentRequestContext ClaimCtx = Ctx;
	ClaimCtx.Params = ClaimParams;
	TSharedPtr<FJsonObject> ClaimData;
	FString ClaimError;
	const bool bClaimed = CmdSmartObjectClaim(ClaimCtx, ClaimData, ClaimError);
	OutData->SetBoolField(TEXT("claimed"), bClaimed);
	TSharedPtr<FJsonObject> NavigationResult = MakeShared<FJsonObject>();
	NavigationResult->SetBoolField(TEXT("evaluated"), false);
	bool bReachable = false;
	bool bReachabilityEvaluated = false;
	if (bClaimed)
	{
		OutData->SetObjectField(TEXT("claim"), ClaimData);
		OutData->SetObjectField(TEXT("claim_result"), ClaimData);
		FString AgentName;
		JsonTryGetString(Ctx.Params, TEXT("agent"), AgentName);
		const TSharedPtr<FJsonObject>* SlotTransformObj = nullptr;
		if (!AgentName.IsEmpty() && ClaimData.IsValid() && ClaimData->TryGetObjectField(TEXT("slot_transform"), SlotTransformObj) && SlotTransformObj && SlotTransformObj->IsValid())
		{
			FString NavError;
			UWorld* World = GetEditorWorld(NavError);
			AActor* AgentActor = World ? FindActorByNameOrLabel(World, AgentName) : nullptr;
			if (AgentActor)
			{
				FVector SlotLocation = FVector::ZeroVector;
				const TSharedPtr<FJsonObject>* TranslationObj = nullptr;
				if ((*SlotTransformObj)->TryGetObjectField(TEXT("translation"), TranslationObj) && TranslationObj && TranslationObj->IsValid())
				{
					double X = 0.0;
					double Y = 0.0;
					double Z = 0.0;
					(*TranslationObj)->TryGetNumberField(TEXT("x"), X);
					(*TranslationObj)->TryGetNumberField(TEXT("y"), Y);
					(*TranslationObj)->TryGetNumberField(TEXT("z"), Z);
					SlotLocation = FVector(X, Y, Z);
					TSharedPtr<FJsonObject> NavParams = MakeShared<FJsonObject>();
					NavParams->SetObjectField(TEXT("start"), UeAgentAIOps::VecToJson(AgentActor->GetActorLocation()));
					NavParams->SetObjectField(TEXT("end"), UeAgentAIOps::VecToJson(SlotLocation));
					NavParams->SetBoolField(TEXT("allow_partial"), false);
					NavParams->SetBoolField(TEXT("allow_projection_failure"), true);
					FUeAgentRequestContext NavCtx = Ctx;
					NavCtx.Params = NavParams;
					TSharedPtr<FJsonObject> PathData;
					const bool bNavOk = CmdNavmeshFindPath(NavCtx, PathData, NavError);
					NavigationResult->SetBoolField(TEXT("evaluated"), true);
					NavigationResult->SetBoolField(TEXT("command_ok"), bNavOk);
					if (bNavOk && PathData.IsValid())
					{
						NavigationResult->SetObjectField(TEXT("path"), PathData);
						PathData->TryGetBoolField(TEXT("path_found"), bReachable);
					}
					else
					{
						NavigationResult->SetStringField(TEXT("error"), NavError);
					}
					bReachabilityEvaluated = true;
				}
			}
			else
			{
				NavigationResult->SetStringField(TEXT("reason"), TEXT("agent_not_found"));
			}
		}
		else
		{
			NavigationResult->SetStringField(TEXT("reason"), AgentName.IsEmpty() ? TEXT("missing_agent") : TEXT("missing_slot_transform"));
		}
		TSharedPtr<FJsonObject> ReleaseParams = MakeShared<FJsonObject>();
		ReleaseParams->SetStringField(TEXT("claim_handle"), ClaimData->GetStringField(TEXT("claim_handle")));
		FUeAgentRequestContext ReleaseCtx = Ctx;
		ReleaseCtx.Params = ReleaseParams;
		TSharedPtr<FJsonObject> ReleaseData;
		FString ReleaseError;
		const bool bReleased = CmdSmartObjectRelease(ReleaseCtx, ReleaseData, ReleaseError);
		OutData->SetBoolField(TEXT("released"), bReleased);
		if (!ReleaseData.IsValid())
		{
			ReleaseData = MakeShared<FJsonObject>();
			ReleaseData->SetStringField(TEXT("error"), ReleaseError);
			ReleaseData->SetStringField(TEXT("status"), TEXT("release_failed"));
		}
		OutData->SetObjectField(TEXT("release"), ReleaseData);
		OutData->SetObjectField(TEXT("release_result"), ReleaseData);
		TArray<TSharedPtr<FJsonValue>> CleanupResults;
		CleanupResults.Add(MakeShared<FJsonValueObject>(ReleaseData));
		OutData->SetArrayField(TEXT("cleanup_results"), CleanupResults);
		const bool bReachabilityPassed = !bExpectReachable || (bReachabilityEvaluated && bReachable);
		OutData->SetBoolField(TEXT("probe_passed"), (bExpectClaimable ? (bClaimed && bReleased) : false) && bReachabilityPassed);
	}
	else
	{
		OutData->SetStringField(TEXT("claim_error"), ClaimError);
		OutData->SetObjectField(TEXT("claim_result"), MakeShared<FJsonObject>());
		OutData->SetArrayField(TEXT("cleanup_results"), TArray<TSharedPtr<FJsonValue>>());
		OutData->SetBoolField(TEXT("probe_passed"), !bExpectClaimable);
	}
	NavigationResult->SetBoolField(TEXT("path_found"), bReachable);
	OutData->SetObjectField(TEXT("navigation_result"), NavigationResult);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdAIBehaviorStackExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		FolderInput = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("AIBehaviorStack"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	bool bClean = false;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bClean);
	if (bClean && IFileManager::Get().DirectoryExists(*FolderPath))
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	TSharedPtr<FJsonObject> Manifest;
	FString ManifestFile;
	FString ManifestResolvedPath;
	JsonTryGetString(Ctx.Params, TEXT("manifest_file"), ManifestFile);
	if (!ManifestFile.IsEmpty())
	{
		if (!UeAgentAIOps::LoadJsonFile(ManifestFile, Manifest, ManifestResolvedPath, OutError))
		{
			return false;
		}
	}
	if (!Manifest.IsValid())
	{
		Manifest = MakeShared<FJsonObject>();
	}
	Manifest->SetStringField(TEXT("profile"), UeAgentAIOps::AIStackProfile);
	Manifest->SetNumberField(TEXT("schema_version"), 1);
	FString StackName;
	if (JsonTryGetString(Ctx.Params, TEXT("name"), StackName) && !StackName.IsEmpty())
	{
		Manifest->SetStringField(TEXT("name"), StackName);
	}
	else if (!Manifest->HasField(TEXT("name")))
	{
		Manifest->SetStringField(TEXT("name"), TEXT("AIBehaviorStack"));
	}

	const TCHAR* Keys[] = { TEXT("behavior_tree_asset"), TEXT("blackboard_asset"), TEXT("state_tree_asset"), TEXT("ai_controller_blueprint"), TEXT("pawn_blueprint") };
	for (const TCHAR* Key : Keys)
	{
		FString Value;
		if (JsonTryGetString(Ctx.Params, Key, Value) && !Value.IsEmpty())
		{
			Manifest->SetStringField(Key, Value);
		}
	}

	auto CollectStringArray = [&](const TCHAR* ArrayField, const TCHAR* SingleField)
	{
		TArray<FString> Items;
		const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
		if ((Ctx.Params->TryGetArrayField(ArrayField, ArrayValues) || Manifest->TryGetArrayField(ArrayField, ArrayValues)) && ArrayValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ArrayValues)
			{
				FString Item;
				if (Value.IsValid() && Value->TryGetString(Item) && !Item.IsEmpty())
				{
					Items.Add(Item);
				}
			}
		}
		FString SingleValue;
		if (SingleField && (JsonTryGetString(Ctx.Params, SingleField, SingleValue) || Manifest->TryGetStringField(SingleField, SingleValue)) && !SingleValue.IsEmpty())
		{
			Items.AddUnique(SingleValue);
		}
		return Items;
	};
	auto SetStringArrayField = [&](const TCHAR* ArrayField, const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		Manifest->SetArrayField(ArrayField, Values);
	};
	auto AppendPrefixedFiles = [](TArray<TSharedPtr<FJsonValue>>& Target, const FString& Prefix, const TArray<TSharedPtr<FJsonValue>>& LocalFiles)
	{
		for (const TSharedPtr<FJsonValue>& Value : LocalFiles)
		{
			FString FileName;
			if (Value.IsValid() && Value->TryGetString(FileName))
			{
				Target.Add(MakeShared<FJsonValueString>(FPaths::Combine(Prefix, FileName).Replace(TEXT("\\"), TEXT("/"))));
			}
		}
	};
	auto SaveStackJson = [&](const FString& RelativeFile, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& WrittenFiles)
	{
		return UeAgentAIOps::SaveTrackedFolderJson(FolderPath, RelativeFile, Root, WrittenFiles, OutError);
	};

	TArray<TSharedPtr<FJsonValue>> WrittenFiles;
	bool bIncludeAssetSummaries = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_asset_summaries"), bIncludeAssetSummaries);

	FString BehaviorTreePath;
	Manifest->TryGetStringField(TEXT("behavior_tree_asset"), BehaviorTreePath);
	FString BlackboardPath;
	Manifest->TryGetStringField(TEXT("blackboard_asset"), BlackboardPath);
	if (!BehaviorTreePath.IsEmpty())
	{
		UBehaviorTree* Tree = UeAgentAIOps::LoadAsset<UBehaviorTree>(BehaviorTreePath);
		if (!Tree)
		{
			OutError = TEXT("behavior_tree_not_found");
			return false;
		}
		if (BlackboardPath.IsEmpty() && Tree->BlackboardAsset)
		{
			BlackboardPath = Tree->BlackboardAsset->GetPathName();
			Manifest->SetStringField(TEXT("blackboard_asset"), BlackboardPath);
		}
		if (bIncludeAssetSummaries)
		{
			TArray<TSharedPtr<FJsonValue>> LocalFiles;
			const FString RelativeFolder = FPaths::Combine(TEXT("behavior_tree"), UeAgentAIOps::AssetFolderNameFromPath(BehaviorTreePath)).Replace(TEXT("\\"), TEXT("/"));
			if (!UeAgentAIOps::SaveBehaviorTreeFolderContents(FPaths::Combine(FolderPath, RelativeFolder), Tree, LocalFiles, OutError))
			{
				return false;
			}
			AppendPrefixedFiles(WrittenFiles, RelativeFolder, LocalFiles);
		}
	}
	if (!BlackboardPath.IsEmpty() && bIncludeAssetSummaries)
	{
		UBlackboardData* Blackboard = UeAgentAIOps::LoadAsset<UBlackboardData>(BlackboardPath);
		if (!Blackboard)
		{
			OutError = TEXT("blackboard_not_found");
			return false;
		}
		const FString RelativeFile = FPaths::Combine(TEXT("blackboard"), UeAgentAIOps::AssetFolderNameFromPath(BlackboardPath) + TEXT(".json")).Replace(TEXT("\\"), TEXT("/"));
		if (!SaveStackJson(RelativeFile, UeAgentAIOps::ExportBlackboard(Blackboard, true), WrittenFiles))
		{
			return false;
		}
	}

	FString StateTreePath;
	Manifest->TryGetStringField(TEXT("state_tree_asset"), StateTreePath);
	if (!StateTreePath.IsEmpty() && bIncludeAssetSummaries)
	{
		UStateTree* Tree = UeAgentAIOps::LoadAsset<UStateTree>(StateTreePath);
		if (!Tree)
		{
			OutError = TEXT("state_tree_not_found");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> LocalFiles;
		const FString RelativeFolder = FPaths::Combine(TEXT("state_tree"), UeAgentAIOps::AssetFolderNameFromPath(StateTreePath)).Replace(TEXT("\\"), TEXT("/"));
		if (!UeAgentAIOps::SaveStateTreeFolderContents(FPaths::Combine(FolderPath, RelativeFolder), Tree, LocalFiles, OutError))
		{
			return false;
		}
		AppendPrefixedFiles(WrittenFiles, RelativeFolder, LocalFiles);
	}

	TArray<FString> EqsAssets = CollectStringArray(TEXT("eqs_assets"), TEXT("eqs_asset"));
	SetStringArrayField(TEXT("eqs_assets"), EqsAssets);
	for (const FString& EqsPath : EqsAssets)
	{
		if (!bIncludeAssetSummaries)
		{
			continue;
		}
		UEnvQuery* Query = UeAgentAIOps::LoadAsset<UEnvQuery>(EqsPath);
		if (!Query)
		{
			OutError = TEXT("eqs_query_not_found");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> LocalFiles;
		const FString RelativeFolder = FPaths::Combine(TEXT("eqs"), UeAgentAIOps::AssetFolderNameFromPath(EqsPath)).Replace(TEXT("\\"), TEXT("/"));
		if (!UeAgentAIOps::SaveEqsFolderContents(FPaths::Combine(FolderPath, RelativeFolder), Query, LocalFiles, OutError))
		{
			return false;
		}
		AppendPrefixedFiles(WrittenFiles, RelativeFolder, LocalFiles);
	}

	TArray<FString> SmartObjectDefinitions = CollectStringArray(TEXT("smart_object_definitions"), TEXT("smart_object_definition_asset"));
	SetStringArrayField(TEXT("smart_object_definitions"), SmartObjectDefinitions);
	TArray<TSharedPtr<FJsonValue>> SmartObjectDefinitionValues;
	for (const FString& DefinitionPath : SmartObjectDefinitions)
	{
		SmartObjectDefinitionValues.Add(MakeShared<FJsonValueString>(DefinitionPath));
		if (!bIncludeAssetSummaries)
		{
			continue;
		}
		USmartObjectDefinition* Definition = UeAgentAIOps::LoadAsset<USmartObjectDefinition>(DefinitionPath);
		if (!Definition)
		{
			OutError = TEXT("smart_object_definition_not_found");
			return false;
		}
		const FString RelativeFile = FPaths::Combine(TEXT("smart_objects"), UeAgentAIOps::AssetFolderNameFromPath(DefinitionPath) + TEXT(".json")).Replace(TEXT("\\"), TEXT("/"));
		if (!SaveStackJson(RelativeFile, UeAgentAIOps::ExportSmartObjectDefinition(Definition, true), WrittenFiles))
		{
			return false;
		}
	}

	auto SaveNodeSummaries = [&](const TCHAR* ArrayField, const FString& RelativeFolder)
	{
		TArray<FString> Assets = CollectStringArray(ArrayField, nullptr);
		SetStringArrayField(ArrayField, Assets);
		for (const FString& AssetPath : Assets)
		{
			UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
			if (!Asset)
			{
				OutError = TEXT("node_blueprint_not_found");
				return false;
			}
			UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
			UClass* GeneratedClass = nullptr;
			if (Blueprint)
			{
				GeneratedClass = Blueprint->GeneratedClass.Get();
			}
			else
			{
				GeneratedClass = Cast<UClass>(Asset);
			}
			TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
			Summary->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
			Summary->SetStringField(TEXT("object_path"), Asset->GetPathName());
			Summary->SetStringField(TEXT("generated_class"), GeneratedClass ? GeneratedClass->GetPathName() : FString());
			Summary->SetStringField(TEXT("parent_class"), (GeneratedClass && GeneratedClass->GetSuperClass()) ? GeneratedClass->GetSuperClass()->GetPathName() : FString());
			const FString RelativeFile = FPaths::Combine(RelativeFolder, UeAgentAIOps::AssetFolderNameFromPath(AssetPath) + TEXT(".summary.json")).Replace(TEXT("\\"), TEXT("/"));
			if (!SaveStackJson(RelativeFile, Summary, WrittenFiles))
			{
				return false;
			}
		}
		return true;
	};
	if (!SaveNodeSummaries(TEXT("bt_node_blueprints"), TEXT("bt_node_blueprints")) ||
		!SaveNodeSummaries(TEXT("state_tree_node_blueprints"), TEXT("state_tree_node_blueprints")))
	{
		return false;
	}

	TSharedPtr<FJsonObject> PerceptionSetup = MakeShared<FJsonObject>();
	PerceptionSetup->SetStringField(TEXT("profile"), TEXT("ue_agent_interface.ai_perception_setup.v1"));
	PerceptionSetup->SetNumberField(TEXT("schema_version"), 1);
	PerceptionSetup->SetArrayField(TEXT("stimuli_sources"), TArray<TSharedPtr<FJsonValue>>());
	if (!SaveStackJson(TEXT("perception/perception_setup.json"), PerceptionSetup, WrittenFiles))
	{
		return false;
	}
	TSharedPtr<FJsonObject> NavigationValidation = MakeShared<FJsonObject>();
	NavigationValidation->SetStringField(TEXT("profile"), TEXT("ue_agent_interface.navigation_validation.v1"));
	NavigationValidation->SetNumberField(TEXT("schema_version"), 1);
	NavigationValidation->SetArrayField(TEXT("path_probes"), TArray<TSharedPtr<FJsonValue>>());
	if (!SaveStackJson(TEXT("navigation/navigation_validation.json"), NavigationValidation, WrittenFiles))
	{
		return false;
	}
	TSharedPtr<FJsonObject> SmartObjectSetup = MakeShared<FJsonObject>();
	SmartObjectSetup->SetStringField(TEXT("profile"), TEXT("ue_agent_interface.smart_object_setup.v1"));
	SmartObjectSetup->SetNumberField(TEXT("schema_version"), 1);
	SmartObjectSetup->SetArrayField(TEXT("definitions"), SmartObjectDefinitionValues);
	SmartObjectSetup->SetArrayField(TEXT("components"), TArray<TSharedPtr<FJsonValue>>());
	if (!SaveStackJson(TEXT("smart_objects/smart_object_setup.json"), SmartObjectSetup, WrittenFiles))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!SaveStackJson(TEXT("validation/checks.json"), UeAgentAIOps::MakeValidationChecks(Issues), WrittenFiles))
	{
		return false;
	}
	if (!SaveStackJson(TEXT("validation/coverage_report.json"), UeAgentAIOps::MakeCoverageReport(), WrittenFiles))
	{
		return false;
	}
	WrittenFiles.Insert(MakeShared<FJsonValueString>(TEXT("manifest.json")), 0);
	Manifest->SetArrayField(TEXT("written_files"), WrittenFiles);
	if (!UeAgentAIOps::SaveFolderJson(FolderPath, TEXT("manifest.json"), Manifest, OutError))
	{
		return false;
	}
	OutData = Manifest;
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdAIBehaviorStackValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> Manifest;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("manifest.json"), Manifest, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> AssetReferenceResults;
	auto CheckAsset = [&](const TCHAR* Field, UClass* ExpectedClass)
	{
		FString Path;
		if (Manifest->TryGetStringField(Field, Path) && !Path.IsEmpty())
		{
			UObject* Asset = UeAgentAIOps::LoadAssetObject(Path);
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("field"), Field);
			Result->SetStringField(TEXT("asset_path"), Path);
			Result->SetStringField(TEXT("expected_class"), ExpectedClass ? ExpectedClass->GetPathName() : FString());
			Result->SetStringField(TEXT("actual_class"), Asset ? Asset->GetClass()->GetPathName() : FString());
			Result->SetBoolField(TEXT("ok"), Asset && (!ExpectedClass || Asset->IsA(ExpectedClass)));
			AssetReferenceResults.Add(MakeShared<FJsonValueObject>(Result));
			if (!Asset || !Asset->IsA(ExpectedClass))
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("asset_missing_or_type_mismatch"), FString::Printf(TEXT("%s is missing or has wrong type."), Field), Field);
			}
		}
	};
	auto CheckAssetArray = [&](const TCHAR* Field, UClass* ExpectedClass)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Manifest->TryGetArrayField(Field, Values) || !Values)
		{
			return;
		}
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			FString Path;
			if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetString(Path) || Path.IsEmpty())
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("json_type_mismatch"), FString::Printf(TEXT("%s item must be a non-empty string."), Field), FString::Printf(TEXT("%s[%d]"), Field, Index));
				continue;
			}
			UObject* Asset = UeAgentAIOps::LoadAssetObject(Path);
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("field"), FString::Printf(TEXT("%s[%d]"), Field, Index));
			Result->SetStringField(TEXT("asset_path"), Path);
			Result->SetStringField(TEXT("expected_class"), ExpectedClass ? ExpectedClass->GetPathName() : FString());
			Result->SetStringField(TEXT("actual_class"), Asset ? Asset->GetClass()->GetPathName() : FString());
			Result->SetBoolField(TEXT("ok"), Asset && (!ExpectedClass || Asset->IsA(ExpectedClass)));
			AssetReferenceResults.Add(MakeShared<FJsonValueObject>(Result));
			if (!Asset || !Asset->IsA(ExpectedClass))
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("asset_missing_or_type_mismatch"), FString::Printf(TEXT("%s item is missing or has wrong type."), Field), FString::Printf(TEXT("%s[%d]"), Field, Index));
			}
		}
	};
	CheckAsset(TEXT("behavior_tree_asset"), UBehaviorTree::StaticClass());
	CheckAsset(TEXT("blackboard_asset"), UBlackboardData::StaticClass());
	CheckAsset(TEXT("state_tree_asset"), UStateTree::StaticClass());
	CheckAsset(TEXT("eqs_asset"), UEnvQuery::StaticClass());
	CheckAsset(TEXT("smart_object_definition_asset"), USmartObjectDefinition::StaticClass());
	CheckAssetArray(TEXT("eqs_assets"), UEnvQuery::StaticClass());
	CheckAssetArray(TEXT("smart_object_definitions"), USmartObjectDefinition::StaticClass());
	CheckAssetArray(TEXT("bt_node_blueprints"), UObject::StaticClass());
	CheckAssetArray(TEXT("state_tree_node_blueprints"), UObject::StaticClass());
	const TArray<TSharedPtr<FJsonValue>>* WrittenFiles = nullptr;
	if (Manifest->TryGetArrayField(TEXT("written_files"), WrittenFiles) && WrittenFiles)
	{
		for (int32 Index = 0; Index < WrittenFiles->Num(); ++Index)
		{
			FString RelativeFile;
			if (!(*WrittenFiles)[Index].IsValid() || !(*WrittenFiles)[Index]->TryGetString(RelativeFile) || RelativeFile.IsEmpty())
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("json_type_mismatch"), TEXT("written_files item must be a non-empty string."), FString::Printf(TEXT("written_files[%d]"), Index));
				continue;
			}
			if (!FPaths::FileExists(FPaths::Combine(FolderPath, RelativeFile)))
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("exported_file_missing"), FString::Printf(TEXT("Exported stack file is missing: %s"), *RelativeFile), FString::Printf(TEXT("written_files[%d]"), Index));
			}
		}
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::AIStackProfile);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	OutData->SetArrayField(TEXT("asset_reference_results"), AssetReferenceResults);
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdAIBehaviorStackRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData = MakeShared<FJsonObject>();
	double DurationSeconds = 0.0;
	double SampleIntervalSeconds = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds);
	JsonTryGetNumber(Ctx.Params, TEXT("sample_interval_seconds"), SampleIntervalSeconds);
	TSharedPtr<FJsonObject> NavData;
	FString LocalError;
	if (CmdNavigationRuntimeSnapshot(Ctx, NavData, LocalError))
	{
		OutData->SetObjectField(TEXT("navigation"), NavData);
	}
	else
	{
		OutData->SetStringField(TEXT("navigation_error"), LocalError);
	}
	TSharedPtr<FJsonObject> SmartData;
	LocalError.Reset();
	if (CmdSmartObjectRuntimeSnapshot(Ctx, SmartData, LocalError))
	{
		OutData->SetObjectField(TEXT("smart_objects"), SmartData);
	}
	else
	{
		OutData->SetStringField(TEXT("smart_object_error"), LocalError);
	}
	TSharedPtr<FJsonObject> BTData;
	LocalError.Reset();
	if (CmdBehaviorTreeRuntimeSnapshot(Ctx, BTData, LocalError))
	{
		OutData->SetObjectField(TEXT("behavior_tree"), BTData);
	}
	else
	{
		OutData->SetStringField(TEXT("behavior_tree_error"), LocalError);
	}
	TSharedPtr<FJsonObject> StateTreeData;
	LocalError.Reset();
	if (CmdStateTreeRuntimeSnapshot(Ctx, StateTreeData, LocalError))
	{
		OutData->SetObjectField(TEXT("state_tree"), StateTreeData);
	}
	else
	{
		OutData->SetStringField(TEXT("state_tree_error"), LocalError);
	}
	TSharedPtr<FJsonObject> PerceptionData;
	LocalError.Reset();
	if (CmdAIPerceptionRuntimeSnapshot(Ctx, PerceptionData, LocalError))
	{
		OutData->SetObjectField(TEXT("perception"), PerceptionData);
	}
	else
	{
		OutData->SetStringField(TEXT("perception_error"), LocalError);
	}
	TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
	if (NavData.IsValid())
	{
		Sample->SetObjectField(TEXT("navigation"), NavData);
	}
	if (SmartData.IsValid())
	{
		Sample->SetObjectField(TEXT("smart_objects"), SmartData);
	}
	if (BTData.IsValid())
	{
		Sample->SetObjectField(TEXT("behavior_tree"), BTData);
	}
	if (StateTreeData.IsValid())
	{
		Sample->SetObjectField(TEXT("state_tree"), StateTreeData);
	}
	if (PerceptionData.IsValid())
	{
		Sample->SetObjectField(TEXT("perception"), PerceptionData);
	}
	TArray<TSharedPtr<FJsonValue>> Samples;
	Samples.Add(MakeShared<FJsonValueObject>(Sample));
	TArray<TSharedPtr<FJsonValue>> ExpectationResults;
	auto AddExpectation = [&ExpectationResults](const FString& Kind, const FString& Expected, const bool bPassed, const FString& Observed)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("kind"), Kind);
		Result->SetStringField(TEXT("expected"), Expected);
		Result->SetBoolField(TEXT("passed"), bPassed);
		Result->SetStringField(TEXT("observed"), Observed);
		ExpectationResults.Add(MakeShared<FJsonValueObject>(Result));
	};
	const TArray<TSharedPtr<FJsonValue>>* ExpectedBlackboardKeys = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("expected_blackboard_keys"), ExpectedBlackboardKeys) && ExpectedBlackboardKeys)
	{
		TSet<FString> ObservedKeys;
		const TArray<TSharedPtr<FJsonValue>>* BlackboardValues = nullptr;
		if (BTData.IsValid() && BTData->TryGetArrayField(TEXT("blackboard"), BlackboardValues) && BlackboardValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *BlackboardValues)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && Obj->IsValid())
				{
					ObservedKeys.Add((*Obj)->GetStringField(TEXT("name")));
				}
			}
		}
		for (const TSharedPtr<FJsonValue>& ExpectedValue : *ExpectedBlackboardKeys)
		{
			FString ExpectedName;
			if (ExpectedValue.IsValid() && ExpectedValue->TryGetString(ExpectedName))
			{
				AddExpectation(TEXT("blackboard_key"), ExpectedName, ObservedKeys.Contains(ExpectedName), FString::Join(ObservedKeys.Array(), TEXT(",")));
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* ExpectedPerception = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("expected_perception"), ExpectedPerception) && ExpectedPerception)
	{
		TSet<FString> CurrentActors;
		const TArray<TSharedPtr<FJsonValue>>* CurrentValues = nullptr;
		if (PerceptionData.IsValid() && PerceptionData->TryGetArrayField(TEXT("currently_perceived_actors"), CurrentValues) && CurrentValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *CurrentValues)
			{
				FString Name;
				if (Value.IsValid() && Value->TryGetString(Name))
				{
					CurrentActors.Add(Name);
				}
			}
		}
		for (const TSharedPtr<FJsonValue>& ExpectedValue : *ExpectedPerception)
		{
			FString ExpectedActor;
			if (ExpectedValue.IsValid() && ExpectedValue->TryGetString(ExpectedActor))
			{
				AddExpectation(TEXT("perception_actor"), ExpectedActor, CurrentActors.Contains(ExpectedActor), FString::Join(CurrentActors.Array(), TEXT(",")));
			}
		}
	}
	bool bExpectationsPassed = true;
	for (const TSharedPtr<FJsonValue>& ResultValue : ExpectationResults)
	{
		const TSharedPtr<FJsonObject>* ResultObj = nullptr;
		if (ResultValue.IsValid() && ResultValue->TryGetObject(ResultObj) && ResultObj && ResultObj->IsValid())
		{
			bool bPassed = false;
			(*ResultObj)->TryGetBoolField(TEXT("passed"), bPassed);
			bExpectationsPassed = bExpectationsPassed && bPassed;
		}
	}
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetNumberField(TEXT("sample_interval_seconds"), SampleIntervalSeconds);
	OutData->SetNumberField(TEXT("sample_count"), Samples.Num());
	OutData->SetArrayField(TEXT("samples"), Samples);
	OutData->SetArrayField(TEXT("expectation_results"), ExpectationResults);
	OutData->SetArrayField(TEXT("cleanup_results"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetBoolField(TEXT("probe_passed"), bExpectationsPassed);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::AIStackProfile);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}
