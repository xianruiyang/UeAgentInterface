
#include "Server/UeAgentHttpServer.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceSettings.h"

#include "ActorFactories/ActorFactory.h"
#include "Camera/CameraActor.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/BlendSpace.h"
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
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
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
}
