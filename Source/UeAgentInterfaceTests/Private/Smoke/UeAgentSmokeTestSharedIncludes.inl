
#include "Server/UeAgentHttpServer.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceSettings.h"

#include "ActorFactories/ActorFactory.h"
#include "Camera/CameraActor.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/BlendSpace.h"
#include "Animation/SkeletalMeshActor.h"
#include "AnimationEditorUtils.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Builders/CubeBuilder.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "Engine/SkeletalMesh.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationSystem.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelSequence.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "MovieSceneCommonHelpers.h"
#include "MovieSceneFolder.h"
#include "MovieScenePossessable.h"
#include "Sections/MovieSceneCinematicShotSection.h"
#include "Sections/MovieSceneSpawnSection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeAgentInterfaceSmoke, Log, All);

#include "UeAgentInterfaceSmokeTestCommon.inl"

namespace
{
	template<typename NodeType>
	NodeType* AddAutomationNodeToGraph(UEdGraph* Graph)
	{
		if (!Graph)
		{
			return nullptr;
		}

		NodeType* NewNode = NewObject<NodeType>(Graph);
		Graph->AddNode(NewNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
		NewNode->CreateNewGuid();
		NewNode->PostPlacedNewNode();
		NewNode->AllocateDefaultPins();
		return NewNode;
	}

	static bool CheckSmokeLinearColorField(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& OwnerObject,
		const TCHAR* FieldName,
		const FLinearColor& ExpectedColor,
		const TCHAR* ContextLabel)
	{
		if (!Test || !OwnerObject.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ColorObject = nullptr;
		const bool bHasField = OwnerObject->TryGetObjectField(FieldName, ColorObject) && ColorObject && ColorObject->IsValid();
		Test->TestTrue(FString::Printf(TEXT("%s color field exists"), ContextLabel), bHasField);
		if (!bHasField)
		{
			return false;
		}

		const double R = (*ColorObject)->GetNumberField(TEXT("r"));
		const double G = (*ColorObject)->GetNumberField(TEXT("g"));
		const double B = (*ColorObject)->GetNumberField(TEXT("b"));
		const double A = (*ColorObject)->GetNumberField(TEXT("a"));
		Test->TestTrue(FString::Printf(TEXT("%s color r matches"), ContextLabel), FMath::IsNearlyEqual(static_cast<float>(R), ExpectedColor.R, 0.01f));
		Test->TestTrue(FString::Printf(TEXT("%s color g matches"), ContextLabel), FMath::IsNearlyEqual(static_cast<float>(G), ExpectedColor.G, 0.01f));
		Test->TestTrue(FString::Printf(TEXT("%s color b matches"), ContextLabel), FMath::IsNearlyEqual(static_cast<float>(B), ExpectedColor.B, 0.01f));
		Test->TestTrue(FString::Printf(TEXT("%s color a matches"), ContextLabel), FMath::IsNearlyEqual(static_cast<float>(A), ExpectedColor.A, 0.01f));
		return true;
	}

	static FString MakeSmokeAssetFolderPath(const FString& Profile, const FString& AssetPath)
	{
		FString RelativeFolder = AssetPath;
		while (RelativeFolder.StartsWith(TEXT("/")))
		{
			RelativeFolder.RightChopInline(1, EAllowShrinking::No);
		}

		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), Profile, RelativeFolder));
	}

	static FString ToSmokeJsonPath(const FString& FilePath)
	{
		return FilePath.Replace(TEXT("\\"), TEXT("/"));
	}

	static bool LoadSmokeJsonFile(FAutomationTestBase* Test, const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, const TCHAR* ContextLabel)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s load failed: %s"), ContextLabel, *FilePath));
			}
			return false;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s parse failed: %s"), ContextLabel, *FilePath));
			}
			return false;
		}
		return true;
	}

	static bool SmokeLoadGroupedDetailsForRef(
		FAutomationTestBase* Test,
		const FString& EntryFilePath,
		const TSharedPtr<FJsonObject>& Container,
		const TCHAR* RefFieldName,
		const FString& ExpectedOwnerKind,
		TSharedPtr<FJsonObject>& OutPropertiesJson)
	{
		OutPropertiesJson.Reset();
		const TSharedPtr<FJsonObject>* DetailsRef = nullptr;
		const bool bHasDetailsRef = Container.IsValid()
			&& Container->TryGetObjectField(RefFieldName, DetailsRef)
			&& DetailsRef
			&& DetailsRef->IsValid();
		if (Test)
		{
			Test->TestTrue(FString::Printf(TEXT("%s has grouped details ref"), RefFieldName), bHasDetailsRef);
		}
		if (!bHasDetailsRef)
		{
			return false;
		}

		if (Test)
		{
			Test->TestEqual(FString::Printf(TEXT("%s details schema"), RefFieldName), (*DetailsRef)->GetStringField(TEXT("schema")), FString(TEXT("uai.details_ref.v1")));
			if (!ExpectedOwnerKind.IsEmpty())
			{
				Test->TestEqual(FString::Printf(TEXT("%s details owner kind"), RefFieldName), (*DetailsRef)->GetStringField(TEXT("owner_kind")), ExpectedOwnerKind);
			}
			Test->TestEqual(FString::Printf(TEXT("%s details ref field"), RefFieldName), (*DetailsRef)->GetStringField(TEXT("ref_field")), FString(RefFieldName));
		}

		FString IndexFile;
		if (!(*DetailsRef)->TryGetStringField(TEXT("index_file"), IndexFile) || IndexFile.IsEmpty())
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s details index_file missing"), RefFieldName));
			}
			return false;
		}

		const FString DetailsRoot = FPaths::Combine(FPaths::GetPath(EntryFilePath), FPaths::GetPath(IndexFile));
		const FString DetailsIndexPath = FPaths::Combine(FPaths::GetPath(EntryFilePath), IndexFile);
		double FieldCount = -1.0;
		if ((*DetailsRef)->TryGetNumberField(TEXT("field_count"), FieldCount)
			&& FieldCount <= 0.0
			&& !FPaths::FileExists(DetailsIndexPath))
		{
			OutPropertiesJson = MakeShared<FJsonObject>();
			OutPropertiesJson->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v1"));
			OutPropertiesJson->SetStringField(TEXT("profile"), TEXT("authoring"));
			OutPropertiesJson->SetNumberField(TEXT("property_count"), 0);
			OutPropertiesJson->SetBoolField(TEXT("truncated"), false);
			TArray<TSharedPtr<FJsonValue>> EmptyProperties;
			OutPropertiesJson->SetArrayField(TEXT("properties"), EmptyProperties);
			return true;
		}
		FString Error;
		const bool bLoaded = UeAgentDetailsAuthoring::LoadGroupedDetails(DetailsRoot, OutPropertiesJson, Error);
		if (Test)
		{
			Test->TestTrue(FString::Printf(TEXT("%s details load grouped properties"), RefFieldName), bLoaded);
			if (!bLoaded)
			{
				Test->AddError(FString::Printf(TEXT("LoadGroupedDetails failed: %s"), *Error));
			}
		}
		return bLoaded;
	}

	static bool SmokeDetailsPropertiesContain(
		const TSharedPtr<FJsonObject>& PropertiesJson,
		const FString& PropertyName,
		const FString& ValueTextSubstring = FString())
	{
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!PropertiesJson.IsValid() || !PropertiesJson->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyObj = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObj) || !PropertyObj || !PropertyObj->IsValid())
			{
				continue;
			}

			FString CandidateName;
			if (!(*PropertyObj)->TryGetStringField(TEXT("property_path"), CandidateName) || CandidateName.IsEmpty())
			{
				(*PropertyObj)->TryGetStringField(TEXT("property_name"), CandidateName);
			}
			if (CandidateName != PropertyName)
			{
				continue;
			}

			if (ValueTextSubstring.IsEmpty())
			{
				return true;
			}

			FString ValueText;
			if ((*PropertyObj)->TryGetStringField(TEXT("value_text"), ValueText) && ValueText.Contains(ValueTextSubstring))
			{
				return true;
			}
			if ((*PropertyObj)->TryGetStringField(TEXT("value"), ValueText) && ValueText.Contains(ValueTextSubstring))
			{
				return true;
			}
		}
		return false;
	}

	static bool SmokeResolveDetailsRootForRef(
		FAutomationTestBase* Test,
		const FString& EntryFilePath,
		const TSharedPtr<FJsonObject>& Container,
		const TCHAR* RefFieldName,
		FString& OutDetailsRoot,
		FString& OutIndexPath)
	{
		OutDetailsRoot.Reset();
		OutIndexPath.Reset();
		const TSharedPtr<FJsonObject>* DetailsRef = nullptr;
		const bool bHasDetailsRef = Container.IsValid()
			&& Container->TryGetObjectField(RefFieldName, DetailsRef)
			&& DetailsRef
			&& DetailsRef->IsValid();
		if (!bHasDetailsRef)
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s details ref missing: %s"), RefFieldName, *EntryFilePath));
			}
			return false;
		}

		FString IndexFile;
		if (!(*DetailsRef)->TryGetStringField(TEXT("index_file"), IndexFile) || IndexFile.IsEmpty())
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s details index_file missing: %s"), RefFieldName, *EntryFilePath));
			}
			return false;
		}

		OutIndexPath = FPaths::Combine(FPaths::GetPath(EntryFilePath), IndexFile);
		OutDetailsRoot = FPaths::GetPath(OutIndexPath);
		return true;
	}

	static bool SaveSmokeJsonFile(FAutomationTestBase* Test, const FString& FilePath, const TSharedPtr<FJsonObject>& Obj, const TCHAR* ContextLabel)
	{
		if (!Obj.IsValid())
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s save failed, invalid object: %s"), ContextLabel, *FilePath));
			}
			return false;
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer))
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s serialize failed: %s"), ContextLabel, *FilePath));
			}
			return false;
		}
		const bool bSaved = FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (Test)
		{
			Test->TestTrue(FString::Printf(TEXT("%s save: %s"), ContextLabel, *FilePath), bSaved);
		}
		return bSaved;
	}

	static TSharedPtr<FJsonObject> SmokeMakeDetailsProperty(const FString& PropertyPath, const FString& ValueText)
	{
		TSharedPtr<FJsonObject> Property = MakeShared<FJsonObject>();
		Property->SetStringField(TEXT("property_path"), PropertyPath);
		Property->SetStringField(TEXT("property_name"), PropertyPath);
		Property->SetStringField(TEXT("value"), ValueText);
		Property->SetStringField(TEXT("value_text"), ValueText);
		TArray<TSharedPtr<FJsonValue>> Operations;
		Operations.Add(MakeShared<FJsonValueString>(TEXT("set")));
		Property->SetArrayField(TEXT("operations"), MoveTemp(Operations));
		return Property;
	}

	static bool SmokeWriteGroupedDetailsForRef(
		FAutomationTestBase* Test,
		const FString& EntryFilePath,
		const TSharedPtr<FJsonObject>& Container,
		const TCHAR* RefFieldName,
		const FString& DetailsIndexFile,
		const UeAgentDetailsAuthoring::FOwnerInfo& OwnerInfo,
		const TArray<TSharedPtr<FJsonValue>>& Properties)
	{
		if (!Container.IsValid())
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s grouped details container invalid: %s"), RefFieldName, *EntryFilePath));
			}
			return false;
		}

		TSharedPtr<FJsonObject> PropertiesJson = MakeShared<FJsonObject>();
		PropertiesJson->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v1"));
		PropertiesJson->SetStringField(TEXT("profile"), TEXT("authoring"));
		PropertiesJson->SetArrayField(TEXT("properties"), Properties);

		const FString DetailsRoot = FPaths::Combine(FPaths::GetPath(EntryFilePath), FPaths::GetPath(DetailsIndexFile));
		TSharedPtr<FJsonObject> DetailsSummary;
		FString Error;
		const bool bWritten = UeAgentDetailsAuthoring::WriteGroupedDetails(DetailsRoot, OwnerInfo, PropertiesJson, DetailsSummary, Error);
		if (Test)
		{
			Test->TestTrue(FString::Printf(TEXT("%s grouped details write: %s"), RefFieldName, *DetailsRoot), bWritten);
			if (!bWritten)
			{
				Test->AddError(FString::Printf(TEXT("WriteGroupedDetails failed: %s"), *Error));
			}
		}
		if (!bWritten || !DetailsSummary.IsValid())
		{
			return false;
		}

		DetailsSummary->SetStringField(TEXT("index_file"), DetailsIndexFile);
		DetailsSummary->SetStringField(TEXT("ref_field"), RefFieldName);
		Container->SetObjectField(RefFieldName, DetailsSummary);
		return true;
	}

	static bool SmokeSetGroupedDetailsFieldStringValue(
		FAutomationTestBase* Test,
		const FString& EntryFilePath,
		const TSharedPtr<FJsonObject>& Container,
		const TCHAR* RefFieldName,
		const FString& PropertyPath,
		const FString& NewValueText,
		FString* OutOriginalValueText = nullptr)
	{
		FString DetailsRoot;
		FString IndexPath;
		if (!SmokeResolveDetailsRootForRef(Test, EntryFilePath, Container, RefFieldName, DetailsRoot, IndexPath))
		{
			return false;
		}

		TSharedPtr<FJsonObject> IndexJson;
		if (!LoadSmokeJsonFile(Test, IndexPath, IndexJson, TEXT("details index")))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (!IndexJson->TryGetArrayField(TEXT("groups"), Groups) || !Groups)
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("details index groups missing: %s"), *IndexPath));
			}
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GroupValue : *Groups)
		{
			const TSharedPtr<FJsonObject>* GroupEntry = nullptr;
			if (!GroupValue.IsValid() || !GroupValue->TryGetObject(GroupEntry) || !GroupEntry || !GroupEntry->IsValid())
			{
				continue;
			}

			FString GroupFile;
			if (!(*GroupEntry)->TryGetStringField(TEXT("file"), GroupFile) || GroupFile.IsEmpty())
			{
				continue;
			}

			const FString GroupPath = FPaths::Combine(DetailsRoot, GroupFile);
			TSharedPtr<FJsonObject> GroupJson;
			if (!LoadSmokeJsonFile(Test, GroupPath, GroupJson, TEXT("details group")))
			{
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
			if (!GroupJson->TryGetArrayField(TEXT("fields"), Fields) || !Fields)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& FieldValue : *Fields)
			{
				const TSharedPtr<FJsonObject>* FieldObj = nullptr;
				if (!FieldValue.IsValid() || !FieldValue->TryGetObject(FieldObj) || !FieldObj || !FieldObj->IsValid())
				{
					continue;
				}

				FString CandidatePath;
				if (!(*FieldObj)->TryGetStringField(TEXT("property_path"), CandidatePath) || CandidatePath.IsEmpty())
				{
					(*FieldObj)->TryGetStringField(TEXT("property_name"), CandidatePath);
				}
				if (CandidatePath != PropertyPath)
				{
					continue;
				}

				if (OutOriginalValueText)
				{
					if (!(*FieldObj)->TryGetStringField(TEXT("value_text"), *OutOriginalValueText))
					{
						(*FieldObj)->TryGetStringField(TEXT("value"), *OutOriginalValueText);
					}
				}
				(*FieldObj)->SetStringField(TEXT("value"), NewValueText);
				(*FieldObj)->SetStringField(TEXT("value_text"), NewValueText);
				return SaveSmokeJsonFile(Test, GroupPath, GroupJson, TEXT("details group"));
			}
		}

		if (Test)
		{
			Test->AddError(FString::Printf(TEXT("details field not found: %s in %s"), *PropertyPath, *IndexPath));
		}
		return false;
	}

	static bool SmokeRehydrateGroupedDetails(FAutomationTestBase* Test, const FString& EntryFilePath, const TSharedPtr<FJsonObject>& EntryObject)
	{
		FString Error;
		const bool bOk = UeAgentDetailsAuthoring::RehydrateGroupedDetailsInObjectTree(EntryFilePath, EntryObject, Error);
		if (Test)
		{
			Test->TestTrue(FString::Printf(TEXT("rehydrate grouped details: %s"), *EntryFilePath), bOk);
			if (!bOk)
			{
				Test->AddError(FString::Printf(TEXT("RehydrateGroupedDetails failed: %s"), *Error));
			}
		}
		return bOk;
	}

	static bool SmokeWriteLegacyRawPropertiesFromDetails(
		FAutomationTestBase* Test,
		const FString& EntryFilePath,
		const FString& LegacyPropertiesPath,
		const FString& ExpectedOwnerKind)
	{
		TSharedPtr<FJsonObject> EntryJson;
		if (!LoadSmokeJsonFile(Test, EntryFilePath, EntryJson, TEXT("grouped details legacy bridge entry")))
		{
			return false;
		}

		TSharedPtr<FJsonObject> DetailsPropertiesJson;
		if (!SmokeLoadGroupedDetailsForRef(Test, EntryFilePath, EntryJson, TEXT("details"), ExpectedOwnerKind, DetailsPropertiesJson))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!DetailsPropertiesJson.IsValid() || !DetailsPropertiesJson->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("Grouped details has no properties: %s"), *EntryFilePath));
			}
			return false;
		}

		TSharedPtr<FJsonObject> LegacyJson = MakeShared<FJsonObject>();
		LegacyJson->SetArrayField(TEXT("raw_properties"), *Properties);
		LegacyJson->SetNumberField(TEXT("raw_properties_count"), Properties->Num());

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(LegacyJson.ToSharedRef(), Writer))
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("Serialize legacy properties failed: %s"), *LegacyPropertiesPath));
			}
			return false;
		}

		const bool bSaved = FFileHelper::SaveStringToFile(JsonText, *LegacyPropertiesPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (Test)
		{
			Test->TestTrue(FString::Printf(TEXT("Save legacy properties bridge: %s"), *LegacyPropertiesPath), bSaved);
		}
		return bSaved;
	}
}
