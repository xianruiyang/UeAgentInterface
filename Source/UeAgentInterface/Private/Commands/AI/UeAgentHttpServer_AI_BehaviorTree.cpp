#include "Server/UeAgentHttpServer.h"

#include "Commands/Assets/UeAgentReflectionAuthoringCommandBridge.h"
#include "Utilities/UeAgentAuthoringFolderIO.h"

#include "UeAgentHttpServer_AIOps.h"

#include "Commands/UeAgentGraphViewUtils.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

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
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Decorators/BTDecorator_BlueprintBase.h"
#include "BehaviorTree/Services/BTService_BlueprintBase.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GenericPlatform/GenericWindow.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/WidgetRenderer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

namespace UeAgentAIOps
{
	UClass* ResolveBTClass(const FString& ClassOrKind, const UClass* RequiredBase)
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

	static TSharedPtr<FJsonObject> ExportBTNode(const UBTNode* Node, const FString& Id, const FString* DetailsEntryDirectory = nullptr, FString* OutError = nullptr)
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Obj, TEXT("id"), Id, Node->NodeName.IsEmpty() ? Id : Node->NodeName, TEXT("AI|Behavior Tree|Nodes"), TEXT("behavior_tree_node"));
		if (DetailsEntryDirectory && OutError && OutError->IsEmpty())
		{
			ExportFolderOwnerDetails(
				const_cast<UBTNode*>(Node),
				*DetailsEntryDirectory,
				TEXT("behavior_tree_export_folder"),
				TEXT("behavior_tree_node"),
				Id,
				TEXT("nodes"),
				Obj,
				*OutError);
		}
		return Obj;
	}

	static void TraverseBTComposite(const UBTCompositeNode* Composite, const FString& Id, TArray<TSharedPtr<FJsonValue>>& Nodes, TArray<TSharedPtr<FJsonValue>>& Edges, int32& Counter, const FString* DetailsEntryDirectory = nullptr, FString* OutError = nullptr)
	{
		if (!Composite)
		{
			return;
		}
		Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Composite, Id, DetailsEntryDirectory, OutError)));

		for (int32 ServiceIndex = 0; ServiceIndex < Composite->Services.Num(); ++ServiceIndex)
		{
			const FString ServiceId = FString::Printf(TEXT("%s_service_%d"), *Id, ServiceIndex);
			Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Composite->Services[ServiceIndex], ServiceId, DetailsEntryDirectory, OutError)));
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
				Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Child.Decorators[DecoratorIndex], DecoratorId, DetailsEntryDirectory, OutError)));
			}
			Edge->SetArrayField(TEXT("decorator_ids"), DecoratorIds);
			Edges.Add(MakeShared<FJsonValueObject>(Edge));

			if (const UBTCompositeNode* ChildComposite = Cast<UBTCompositeNode>(ChildNode))
			{
				TraverseBTComposite(ChildComposite, ChildId, Nodes, Edges, Counter, DetailsEntryDirectory, OutError);
			}
			else
			{
				Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(ChildNode, ChildId, DetailsEntryDirectory, OutError)));
				if (const UBTTaskNode* Task = Cast<UBTTaskNode>(ChildNode))
				{
					for (int32 ServiceIndex = 0; ServiceIndex < Task->Services.Num(); ++ServiceIndex)
					{
						const FString ServiceId = FString::Printf(TEXT("%s_service_%d"), *ChildId, ServiceIndex);
						Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Task->Services[ServiceIndex], ServiceId, DetailsEntryDirectory, OutError)));
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

	TSharedPtr<FJsonObject> ExportBehaviorTree(UBehaviorTree* Tree, const bool bIncludeNodes)
	{
		return ExportBehaviorTree(Tree, bIncludeNodes, nullptr, nullptr);
	}

	TSharedPtr<FJsonObject> ExportBehaviorTree(UBehaviorTree* Tree, const bool bIncludeNodes, const FString* DetailsEntryDirectory, FString* OutError)
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
			TraverseBTComposite(Tree->RootNode, TEXT("root"), Nodes, Edges, Counter, DetailsEntryDirectory, OutError);
			Root->SetArrayField(TEXT("nodes"), Nodes);
			Root->SetArrayField(TEXT("edges"), Edges);
			Root->SetNumberField(TEXT("node_count"), Nodes.Num());
			Root->SetNumberField(TEXT("edge_count"), Edges.Num());
		}
		return Root;
	}

	TSharedPtr<FJsonObject> MakeBTAssetJson(UBehaviorTree* Tree)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("profile"), BehaviorTreeProfile);
		Obj->SetNumberField(TEXT("schema_version"), 1);
		Obj->SetStringField(TEXT("asset_path"), Tree ? Tree->GetOutermost()->GetName() : FString());
		Obj->SetStringField(TEXT("object_path"), Tree ? Tree->GetPathName() : FString());
		Obj->SetStringField(TEXT("blackboard_asset"), (Tree && Tree->BlackboardAsset) ? Tree->BlackboardAsset->GetPathName() : FString());
		return Obj;
	}

	static void ValidateBTBlackboardKeyReferences(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& NodesJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		FString BlackboardPath;
		if (!AssetJson.IsValid() || !AssetJson->TryGetStringField(TEXT("blackboard_asset"), BlackboardPath) || BlackboardPath.TrimStartAndEnd().IsEmpty())
		{
			AddIssue(Issues, TEXT("blackboard_asset_missing"), TEXT("Missing blackboard_asset for blackboard key validation."), TEXT("asset.hjson.blackboard_asset"));
			return;
		}

		UBlackboardData* BlackboardData = LoadAsset<UBlackboardData>(BlackboardPath);
		if (!BlackboardData)
		{
			AddIssue(Issues, TEXT("blackboard_not_found"), FString::Printf(TEXT("Blackboard asset '%s' could not be loaded."), *BlackboardPath), TEXT("asset.hjson.blackboard_asset"));
			return;
		}

		auto IsKnownKey = [BlackboardData](const FString& InKeyName) -> bool
		{
			const FString KeyName = InKeyName.TrimStartAndEnd();
			if (KeyName.IsEmpty() || KeyName.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				return true;
			}
			return BlackboardData->GetKeyID(FName(*KeyName)) != FBlackboard::InvalidKey;
		};

		auto ValidateKeyText = [&Issues, &IsKnownKey](const FString& KeyText, const FString& JsonPath)
		{
			if (!IsKnownKey(KeyText))
			{
				AddIssue(Issues, TEXT("blackboard_key_not_found"), FString::Printf(TEXT("Blackboard key '%s' is not defined."), *KeyText), JsonPath);
			}
		};

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!NodesJson.IsValid() || !NodesJson->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			return;
		}
		for (int32 NodeIndex = 0; NodeIndex < Nodes->Num(); ++NodeIndex)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!(*Nodes)[NodeIndex].IsValid() || !(*Nodes)[NodeIndex]->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (!(*NodeObjPtr)->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
			{
				continue;
			}
			for (int32 PropIndex = 0; PropIndex < Properties->Num(); ++PropIndex)
			{
				const TSharedPtr<FJsonObject>* PropObjPtr = nullptr;
				if (!(*Properties)[PropIndex].IsValid() || !(*Properties)[PropIndex]->TryGetObject(PropObjPtr) || !PropObjPtr || !PropObjPtr->IsValid())
				{
					continue;
				}
				const FString PropPath = FString::Printf(TEXT("nodes.hjson.nodes[%d].properties[%d]"), NodeIndex, PropIndex);
				FString PropertyName;
				(*PropObjPtr)->TryGetStringField(TEXT("property_path"), PropertyName);
				const bool bLooksLikeBlackboardKey =
					PropertyName.Contains(TEXT("BlackboardKey"), ESearchCase::IgnoreCase) ||
					PropertyName.Contains(TEXT("SelectedKeyName"), ESearchCase::IgnoreCase);

				FString KeyText;
				if ((*PropObjPtr)->TryGetStringField(TEXT("selected_key_name"), KeyText) || (*PropObjPtr)->TryGetStringField(TEXT("key_name"), KeyText))
				{
					ValidateKeyText(KeyText, PropPath);
					continue;
				}

				const TSharedPtr<FJsonObject>* ValueObject = nullptr;
				if ((*PropObjPtr)->TryGetObjectField(TEXT("value"), ValueObject) && ValueObject && ValueObject->IsValid())
				{
					if ((*ValueObject)->TryGetStringField(TEXT("selected_key_name"), KeyText) || (*ValueObject)->TryGetStringField(TEXT("key_name"), KeyText))
					{
						ValidateKeyText(KeyText, PropPath + TEXT(".value"));
					}
					else if (bLooksLikeBlackboardKey && (*ValueObject)->TryGetStringField(TEXT("literal"), KeyText))
					{
						ValidateKeyText(KeyText, PropPath + TEXT(".value.literal"));
					}
					continue;
				}
			}
		}
	}

	bool ValidateBTFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& NodesJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		return ValidateBTFolderJson(AssetJson, NodesJson, Issues, false, false);
	}

	bool ValidateBTFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& NodesJson, TArray<TSharedPtr<FJsonValue>>& Issues, const bool bStrict, const bool bValidateBlackboardKeys)
	{
		if (!AssetJson.IsValid())
		{
			AddIssue(Issues, TEXT("missing_required_file"), TEXT("Missing asset.hjson."), TEXT("asset.hjson"));
		}
		else if (bStrict)
		{
			StrictCheckFields(AssetJson, { TEXT("profile"), TEXT("schema_version"), TEXT("asset_path"), TEXT("object_path"), TEXT("blackboard_asset"), TEXT("root_id"), TEXT("dirty"), TEXT("valid") }, TEXT("asset.hjson"), Issues);
		}
		if (NodesJson.IsValid() && bStrict)
		{
			StrictCheckFields(NodesJson, { TEXT("profile"), TEXT("schema_version"), TEXT("root_id"), TEXT("nodes"), TEXT("edges"), TEXT("node_count"), TEXT("edge_count") }, TEXT("nodes.hjson"), Issues);
		}
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!NodesJson.IsValid() || !NodesJson->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing nodes array."), TEXT("nodes.hjson.nodes"));
			return false;
		}

		TSet<FString> Ids;
		for (int32 Index = 0; Index < Nodes->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!(*Nodes)[Index].IsValid() || !(*Nodes)[Index]->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_node_item"), TEXT("Node item must be an object."), FString::Printf(TEXT("nodes.hjson.nodes[%d]"), Index));
				continue;
			}
			if (bStrict)
			{
				StrictCheckFields(*NodeObjPtr, { TEXT("id"), TEXT("kind"), TEXT("class"), TEXT("name"), TEXT("static_description"), TEXT("properties"), TEXT("details"), TEXT("selector"), TEXT("display_name"), TEXT("category_path"), TEXT("value_kind"), TEXT("availability"), TEXT("operations") }, FString::Printf(TEXT("nodes.hjson.nodes[%d]"), Index), Issues);
			}
			FString Id;
			FString Kind;
			FString ClassPath;
			(*NodeObjPtr)->TryGetStringField(TEXT("id"), Id);
			(*NodeObjPtr)->TryGetStringField(TEXT("kind"), Kind);
			(*NodeObjPtr)->TryGetStringField(TEXT("class"), ClassPath);
			if (Id.IsEmpty())
			{
				AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing node id."), FString::Printf(TEXT("nodes.hjson.nodes[%d].id"), Index));
			}
			else if (Ids.Contains(Id))
			{
				AddIssue(Issues, TEXT("duplicate_id"), FString::Printf(TEXT("Duplicate node id '%s'."), *Id), FString::Printf(TEXT("nodes.hjson.nodes[%d].id"), Index));
			}
			Ids.Add(Id);
			const UClass* BaseClass = UBTNode::StaticClass();
			if (Kind == TEXT("composite")) BaseClass = UBTCompositeNode::StaticClass();
			else if (Kind == TEXT("task")) BaseClass = UBTTaskNode::StaticClass();
			else if (Kind == TEXT("decorator")) BaseClass = UBTDecorator::StaticClass();
			else if (Kind == TEXT("service")) BaseClass = UBTService::StaticClass();
			if (!ResolveBTClass(ClassPath.IsEmpty() ? Kind : ClassPath, BaseClass))
			{
				AddIssue(Issues, TEXT("bt_node_class_not_found"), FString::Printf(TEXT("Invalid BT node class '%s'."), *ClassPath), FString::Printf(TEXT("nodes.hjson.nodes[%d].class"), Index));
			}
		}
		if (bValidateBlackboardKeys)
		{
			ValidateBTBlackboardKeyReferences(AssetJson, NodesJson, Issues);
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
	return UeAgentReflectionAuthoringCommandBridge::Export(TEXT("behavior_tree"), Ctx.Params, OutData, OutError);
}

bool FUeAgentHttpServer::CmdBehaviorTreeValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Validate(TEXT("behavior_tree"), Ctx.Params, OutData, OutError);
}

bool FUeAgentHttpServer::CmdBehaviorTreeApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Apply(TEXT("behavior_tree"), Ctx.Params, OutData, OutError, Ctx.RequestId);
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
	bool bSnapToPixelGrid = true;
	JsonTryGetBool(Ctx.Params, TEXT("snap_to_pixel_grid"), bSnapToPixelGrid);
	const float ClampedZoom = (float)FMath::Clamp(Zoom, 0.05, 64.0);
	FVector2f ViewLocation((float)X, (float)Y);
	if (bSnapToPixelGrid)
	{
		ViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(ViewLocation, ClampedZoom);
	}
	OutData->SetNumberField(TEXT("view_x"), ViewLocation.X);
	OutData->SetNumberField(TEXT("view_y"), ViewLocation.Y);
	OutData->SetNumberField(TEXT("zoom"), ClampedZoom);
	OutData->SetBoolField(TEXT("snap_to_pixel_grid"), bSnapToPixelGrid);
	OutData->SetNumberField(TEXT("screen_pixel_x"), ViewLocation.X * ClampedZoom);
	OutData->SetNumberField(TEXT("screen_pixel_y"), ViewLocation.Y * ClampedZoom);
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
