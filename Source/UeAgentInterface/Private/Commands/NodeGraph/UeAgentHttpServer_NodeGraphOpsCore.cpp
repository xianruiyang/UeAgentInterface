// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_NodeGraphOps.h"
#include "UeAgentHttpServer_NodeGraphOpsInternal.h"

#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "Server/UeAgentHttpServer.h"

#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ControlRigBlueprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "GraphEditorSettings.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraphNode_Knot.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "ScopedTransaction.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"


namespace UeAgentNodeGraphOps
{
	FGraphRef::FGraphRef()
		: Graph(nullptr)
		, bSupported(true)
	{
	}

	namespace Internal
	{
		FGraphRef::FGraphRef()
			: Graph(nullptr)
			, bSupported(true)
		{
		}

		FUniversalLayoutNodeRef::FUniversalLayoutNodeRef()
			: GraphNode(nullptr)
			, MaterialExpression(nullptr)
			, bVirtual(false)
		{
		}

		FUniversalLayoutModel::FUniversalLayoutModel()
			: WritableNodeCount(0)
			, CollapsedRerouteNodeCount(0)
			, CollapsedRerouteEdgeCount(0)
		{
		}

		FSharedGraphGeometryStats::FSharedGraphGeometryStats()
			: bUsedRealGeometry(false)
			, bOpenedEditorForGeometry(false)
			, bUsedOffscreenGraphEditor(false)
			, GeometrySource(TEXT("real_geometry_required"))
			, NodeCount(0)
			, MeasuredNodeCount(0)
			, PinWidgetCount(0)
			, PinGeometryAnchorCount(0)
			, VisibleMissingPinAnchorCount(0)
			, MissingPinAnchorCount(0)
			, FocusedGeometryNodeCount(0)
			, bFocusedGeometryBudgetExhausted(false)
			, bRestoredGraphViewAfterCapture(false)
			, RestoredViewLocation(FVector2f::ZeroVector)
			, RestoredZoomAmount(1.0f)
			, GeometryMeasurementZoomAmount(1.0f)
		{
		}

		FRootMaterialPropertySpec::FRootMaterialPropertySpec()
			: Name(TEXT(""))
			, Property(MP_BaseColor)
		{
		}

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

		bool SaveAssetPackage(UObject* Asset, FString& OutError)
		{
			if (!Asset)
			{
				OutError = TEXT("asset_not_found");
				return false;
			}
			UPackage* Package = Asset->GetOutermost();
			if (!Package)
			{
				OutError = TEXT("package_not_found");
				return false;
			}
			TArray<UPackage*> PackagesToSave;
			PackagesToSave.Add(Package);
			if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
			{
				OutError = TEXT("save_package_failed");
				return false;
			}
			return true;
		}

		bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, TSharedPtr<FJsonObject>& OutObj)
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (Obj.IsValid() && Obj->TryGetObjectField(Field, Found) && Found && Found->IsValid())
			{
				OutObj = *Found;
				return true;
			}
			return false;
		}

		bool TryGetStringFromEither(const TSharedPtr<FJsonObject>& Primary, const TSharedPtr<FJsonObject>& Secondary, const FString& Field, FString& OutValue)
		{
			return (Primary.IsValid() && Primary->TryGetStringField(Field, OutValue))
				|| (Secondary.IsValid() && Secondary->TryGetStringField(Field, OutValue));
		}

		bool TryGetBoolFromEither(const TSharedPtr<FJsonObject>& Primary, const TSharedPtr<FJsonObject>& Secondary, const FString& Field, bool& OutValue)
		{
			return (Primary.IsValid() && Primary->TryGetBoolField(Field, OutValue))
				|| (Secondary.IsValid() && Secondary->TryGetBoolField(Field, OutValue));
		}

		bool TryGetNumberFromEither(const TSharedPtr<FJsonObject>& Primary, const TSharedPtr<FJsonObject>& Secondary, const FString& Field, double& OutValue)
		{
			return (Primary.IsValid() && Primary->TryGetNumberField(Field, OutValue))
				|| (Secondary.IsValid() && Secondary->TryGetNumberField(Field, OutValue));
		}

		bool TryGetLayoutBool(const TSharedPtr<FJsonObject>& Params, const FString& Field, bool& OutValue)
		{
			TSharedPtr<FJsonObject> LayoutOptionsObj;
			TryGetObjectField(Params, TEXT("layout_options"), LayoutOptionsObj);
			return TryGetBoolFromEither(LayoutOptionsObj, Params, Field, OutValue);
		}

		bool HasRemovedDryRunParameter(const TSharedPtr<FJsonObject>& Params)
		{
			if (!Params.IsValid())
			{
				return false;
			}
			if (Params->HasField(TEXT("dry_run")))
			{
				return true;
			}
			TSharedPtr<FJsonObject> LayoutOptionsObj;
			return TryGetObjectField(Params, TEXT("layout_options"), LayoutOptionsObj)
				&& LayoutOptionsObj.IsValid()
				&& LayoutOptionsObj->HasField(TEXT("dry_run"));
		}

		bool TryComputeGraphEditorAccessOffsetX(double& OutOffset)
		{
			OutOffset = 0.0;
			const UGraphEditorSettings* Settings = GetDefault<UGraphEditorSettings>();
			if (!Settings)
			{
				return false;
			}
			const float ReferenceDeltaX = FMath::Max(
				1.0f,
				FMath::Abs(Settings->PaddingTowardsNodeEdge) * 2.0f
				+ FMath::Abs(Settings->PaddingLeftOfOutput)
				+ FMath::Abs(Settings->PaddingRightOfInput));
			const FVector2f Tangent = Settings->ComputeSplineTangent(
				FVector2f::ZeroVector,
				FVector2f(ReferenceDeltaX, 0.0f));
			const double Offset = FMath::Abs(static_cast<double>(Tangent.X));
			if (!FMath::IsFinite(Offset) || Offset <= 0.0)
			{
				return false;
			}
			OutOffset = Offset;
			return true;
		}

		bool ParseSharedLayoutOptions(const TSharedPtr<FJsonObject>& Params, UeAgentBlueprintAutoLayout::FOptions& OutOptions, FString& OutError)
		{
			TSharedPtr<FJsonObject> LayoutOptionsObj;
			TryGetObjectField(Params, TEXT("layout_options"), LayoutOptionsObj);

			UeAgentBlueprintAutoLayout::FOptions Options;
			double Value = 0.0;
			if (TryGetNumberFromEither(LayoutOptionsObj, Params, TEXT("origin_x"), Value))
			{
				Options.OriginX = Value;
			}
			if (TryGetNumberFromEither(LayoutOptionsObj, Params, TEXT("origin_y"), Value))
			{
				Options.OriginY = Value;
			}
			if (TryGetNumberFromEither(LayoutOptionsObj, Params, TEXT("horizontal_spacing"), Value))
			{
				Options.HorizontalSpacing = FMath::Max(120.0, Value);
			}
			if (TryGetNumberFromEither(LayoutOptionsObj, Params, TEXT("vertical_spacing"), Value))
			{
				Options.VerticalSpacing = FMath::Max(48.0, Value);
			}
			if (TryGetNumberFromEither(LayoutOptionsObj, Params, TEXT("node_padding"), Value))
			{
				Options.NodePadding = FMath::Max(0.0, Value);
			}

			double GraphEditorAccessOffsetX = 0.0;
			if (!TryComputeGraphEditorAccessOffsetX(GraphEditorAccessOffsetX))
			{
				OutError = TEXT("graph_editor_access_offset_unavailable");
				return false;
			}
			Options.OutputAccessOffsetX = GraphEditorAccessOffsetX;
			Options.InputAccessOffsetX = GraphEditorAccessOffsetX;
			Options.AccessOffsetSource = TEXT("graph_editor_settings_compute_spline_tangent");

			Options.bUseRealGeometry = true;
			Options.MissingPinAnchorCount = 0;
			OutOptions = Options;
			OutError.Reset();
			return true;
		}

		void InitializePendingRealPins(
			UeAgentBlueprintAutoLayout::FNode& Node,
			const TArray<FString>& InputPins,
			const TArray<FString>& OutputPins)
		{
			Node.InputPinNames = InputPins;
			Node.OutputPinNames = OutputPins;
			Node.InputPinAnchors.Init(FVector2D::ZeroVector, InputPins.Num());
			Node.OutputPinAnchors.Init(FVector2D::ZeroVector, OutputPins.Num());
			Node.InputPinAnchorValid.Init(0, InputPins.Num());
			Node.OutputPinAnchorValid.Init(0, OutputPins.Num());
		}

		FString PinNameOrFallback(const FString& Name, const FString& Prefix, const int32 Index)
		{
			FString Trimmed = Name;
			Trimmed.TrimStartAndEndInline();
			return Trimmed.IsEmpty() ? FString::Printf(TEXT("%s%d"), *Prefix, Index) : Trimmed;
		}

		FString AdapterForAsset(const UObject* Asset)
		{
			if (!Asset)
			{
				return TEXT("unknown");
			}
			if (Asset->IsA<UWidgetBlueprint>())
			{
				return TEXT("umg_k2");
			}
			if (Asset->IsA<UAnimBlueprint>())
			{
				return TEXT("anim_blueprint");
			}
			if (Asset->IsA<UControlRigBlueprint>())
			{
				return TEXT("generic_edgraph");
			}
			if (Asset->IsA<UBlueprint>())
			{
				return TEXT("k2_blueprint");
			}
			if (Asset->IsA<UMaterial>())
			{
				return TEXT("material_graph");
			}
			if (Asset->IsA<UMaterialFunctionInterface>())
			{
				return TEXT("material_function_graph");
			}
			return TEXT("generic_edgraph");
		}

		void CopyFieldsToTopLevel(const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Target)
		{
			if (!Source.IsValid() || !Target.IsValid())
			{
				return;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
			{
				Target->SetField(Pair.Key, Pair.Value);
			}
		}

		void ConfigureIncludeFlagsFromKinds(const TArray<TSharedPtr<FJsonValue>>& KindValues, const TSharedPtr<FJsonObject>& Target)
		{
			bool bIncludeUbergraphs = false;
			bool bIncludeFunctionGraphs = false;
			bool bIncludeMacroGraphs = false;
			bool bIncludeDelegateGraphs = false;
			bool bIncludeNestedGraphs = false;

			for (const TSharedPtr<FJsonValue>& Value : KindValues)
			{
				if (!Value.IsValid())
				{
					continue;
				}
				FString Kind = Value->AsString().ToLower();
				Kind.TrimStartAndEndInline();
				if (Kind == TEXT("event_graph") || Kind == TEXT("ubergraph") || Kind == TEXT("event"))
				{
					bIncludeUbergraphs = true;
				}
				else if (Kind == TEXT("function_graph") || Kind == TEXT("function"))
				{
					bIncludeFunctionGraphs = true;
				}
				else if (Kind == TEXT("macro_graph") || Kind == TEXT("macro"))
				{
					bIncludeMacroGraphs = true;
				}
				else if (Kind == TEXT("delegate_graph") || Kind == TEXT("delegate"))
				{
					bIncludeDelegateGraphs = true;
				}
				else if (Kind == TEXT("nested_graph") || Kind == TEXT("nested"))
				{
					bIncludeNestedGraphs = true;
				}
			}

			Target->SetBoolField(TEXT("include_ubergraphs"), bIncludeUbergraphs);
			Target->SetBoolField(TEXT("include_function_graphs"), bIncludeFunctionGraphs);
			Target->SetBoolField(TEXT("include_macro_graphs"), bIncludeMacroGraphs);
			Target->SetBoolField(TEXT("include_delegate_graphs"), bIncludeDelegateGraphs);
			Target->SetBoolField(TEXT("include_nested_graphs"), bIncludeNestedGraphs);
		}

		TSharedPtr<FJsonObject> BuildBlueprintLayoutParams(const TSharedPtr<FJsonObject>& Params)
		{
			TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
			CopyFieldsToTopLevel(Params, Target);

			TSharedPtr<FJsonObject> LayoutOptions;
			if (TryGetObjectField(Params, TEXT("layout_options"), LayoutOptions))
			{
				CopyFieldsToTopLevel(LayoutOptions, Target);
			}

			TSharedPtr<FJsonObject> Selector;
			if (!TryGetObjectField(Params, TEXT("graph_selector"), Selector))
			{
				return Target;
			}

			FString Mode;
			TryGetStringFromEither(Selector, nullptr, TEXT("mode"), Mode);
			if (Mode.IsEmpty())
			{
				TryGetStringFromEither(Selector, nullptr, TEXT("scope"), Mode);
			}
			Mode = Mode.ToLower();

			FString GraphPath;
			if (TryGetStringFromEither(Selector, nullptr, TEXT("graph_path"), GraphPath) && !GraphPath.TrimStartAndEnd().IsEmpty())
			{
				Target->SetStringField(TEXT("graph_path"), GraphPath);
			}
			FString GraphName;
			if (TryGetStringFromEither(Selector, nullptr, TEXT("graph_name"), GraphName) && !GraphName.TrimStartAndEnd().IsEmpty())
			{
				Target->SetStringField(TEXT("graph_name"), GraphName);
			}

			const TArray<TSharedPtr<FJsonValue>>* GraphNames = nullptr;
			if (Selector->TryGetArrayField(TEXT("graph_names"), GraphNames) && GraphNames)
			{
				Target->SetArrayField(TEXT("graph_names"), *GraphNames);
			}
			const TArray<TSharedPtr<FJsonValue>>* GraphPaths = nullptr;
			if (Selector->TryGetArrayField(TEXT("graph_paths"), GraphPaths) && GraphPaths)
			{
				TArray<TSharedPtr<FJsonValue>> GraphRefs;
				if (GraphNames)
				{
					GraphRefs = *GraphNames;
				}
				for (const TSharedPtr<FJsonValue>& Value : *GraphPaths)
				{
					GraphRefs.Add(Value);
				}
				Target->SetArrayField(TEXT("graph_names"), GraphRefs);
			}
			const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
			if (Selector->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs)
			{
				Target->SetArrayField(TEXT("graphs"), *Graphs);
			}

			const TArray<TSharedPtr<FJsonValue>>* GraphKinds = nullptr;
			if (Selector->TryGetArrayField(TEXT("graph_kinds"), GraphKinds) && GraphKinds)
			{
				ConfigureIncludeFlagsFromKinds(*GraphKinds, Target);
			}

			bool bIncludeNested = false;
			if (Selector->TryGetBoolField(TEXT("include_nested_graphs"), bIncludeNested))
			{
				Target->SetBoolField(TEXT("include_nested_graphs"), bIncludeNested);
			}
			bool bSkipEmpty = false;
			if (Selector->TryGetBoolField(TEXT("skip_empty_graphs"), bSkipEmpty))
			{
				Target->SetBoolField(TEXT("skip_empty_graphs"), bSkipEmpty);
			}

			if (Mode == TEXT("all_supported"))
			{
				Target->SetBoolField(TEXT("include_ubergraphs"), true);
				Target->SetBoolField(TEXT("include_function_graphs"), true);
				Target->SetBoolField(TEXT("include_macro_graphs"), true);
				Target->SetBoolField(TEXT("include_delegate_graphs"), true);
			}
			return Target;
		}

		FString GetMaterialInputPinName(UMaterialExpression* Expression, const int32 InputIndex, const FExpressionInput* Input)
		{
			FString InputName;
			if (const UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				InputName = FunctionCall->GetInputNameWithType(InputIndex, false).ToString();
			}
			else if (Expression)
			{
				InputName = Expression->GetInputName(InputIndex).ToString();
			}
			if (InputName.IsEmpty() && Input)
			{
				InputName = Input->InputName.ToString();
			}
			return PinNameOrFallback(InputName, TEXT("Input_"), InputIndex);
		}

		FString GetMaterialOutputPinName(UMaterialExpression* Expression, const int32 OutputIndex)
		{
			FString OutputName;
			if (Expression)
			{
				const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
				if (Outputs.IsValidIndex(OutputIndex))
				{
					OutputName = Outputs[OutputIndex].OutputName.ToString();
				}
			}
			return PinNameOrFallback(OutputName, TEXT("Output_"), FMath::Max(0, OutputIndex));
		}

		TArray<FString> GetMaterialInputPinNames(UMaterialExpression* Expression)
		{
			TArray<FString> Pins;
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				Pins.Add(GetMaterialInputPinName(Expression, It.Index, It.Input));
			}
			return Pins;
		}

		TArray<FString> GetMaterialOutputPinNames(UMaterialExpression* Expression)
		{
			TArray<FString> Pins;
			if (!Expression)
			{
				return Pins;
			}
			const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
			for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
			{
				Pins.Add(GetMaterialOutputPinName(Expression, OutputIndex));
			}
			if (Pins.IsEmpty())
			{
				Pins.Add(TEXT("Output_0"));
			}
			return Pins;
		}

		int32 GetMaterialInputPinSlotIndex(UMaterialExpression* Expression, const int32 InputIndex)
		{
			int32 Slot = 0;
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				if (It.Index == InputIndex)
				{
					return Slot;
				}
				++Slot;
			}
			return INDEX_NONE;
		}

		int32 GetMaterialOutputPinSlotIndex(UMaterialExpression* Expression, const int32 OutputIndex)
		{
			if (!Expression)
			{
				return INDEX_NONE;
			}
			const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
			if (Outputs.IsValidIndex(OutputIndex))
			{
				return OutputIndex;
			}
			return Outputs.IsEmpty() ? 0 : 0;
		}

		bool HasLayoutNumberField(const TSharedPtr<FJsonObject>& Params, const FString& Field)
		{
			TSharedPtr<FJsonObject> LayoutOptionsObj;
			TryGetObjectField(Params, TEXT("layout_options"), LayoutOptionsObj);
			double Ignored = 0.0;
			return TryGetNumberFromEither(LayoutOptionsObj, Params, Field, Ignored);
		}

		void ApplyDefaultOriginFromWritableNodes(
			const FUniversalLayoutModel& Model,
			const TSharedPtr<FJsonObject>& Params,
			UeAgentBlueprintAutoLayout::FOptions& Options)
		{
			const bool bHasOriginX = HasLayoutNumberField(Params, TEXT("origin_x"));
			const bool bHasOriginY = HasLayoutNumberField(Params, TEXT("origin_y"));
			if (bHasOriginX && bHasOriginY)
			{
				return;
			}

			bool bHasAny = false;
			double MinX = 0.0;
			double MinY = 0.0;
			for (int32 Index = 0; Index < Model.NodeRefs.Num(); ++Index)
			{
				if (!Model.Nodes.IsValidIndex(Index) || Model.NodeRefs[Index].bVirtual)
				{
					continue;
				}
				const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[Index];
				if (!bHasAny)
				{
					MinX = Node.OldX;
					MinY = Node.OldY;
					bHasAny = true;
					continue;
				}
				MinX = FMath::Min(MinX, Node.OldX);
				MinY = FMath::Min(MinY, Node.OldY);
			}
			if (bHasAny)
			{
				if (!bHasOriginX)
				{
					Options.OriginX = MinX;
				}
				if (!bHasOriginY)
				{
					Options.OriginY = MinY;
				}
			}
		}

		bool IsFiniteLayoutPoint(const FVector2D& Point);

		bool TryGetMeasuredPinAnchor(
			const UeAgentBlueprintAutoLayout::FNode& Node,
			const EEdGraphPinDirection Direction,
			const int32 PinIndex,
			FVector2D& OutAnchor);

		TSharedPtr<FJsonObject> SharedLayoutNodeToJson(
			const UeAgentBlueprintAutoLayout::FNode& Node,
			const FUniversalLayoutNodeRef& Ref)
		{
			auto PinsToJson = [&Node](const bool bInput)
			{
				TArray<TSharedPtr<FJsonValue>> PinsJson;
				const TArray<FString>& Names = bInput ? Node.InputPinNames : Node.OutputPinNames;
				const TArray<FVector2D>& Anchors = bInput ? Node.InputPinAnchors : Node.OutputPinAnchors;
				const TArray<uint8>& Valid = bInput ? Node.InputPinAnchorValid : Node.OutputPinAnchorValid;
				for (int32 PinIndex = 0; PinIndex < Names.Num(); ++PinIndex)
				{
					TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
					PinObj->SetNumberField(TEXT("index"), PinIndex);
					PinObj->SetStringField(TEXT("name"), Names[PinIndex]);
					const bool bValid = Anchors.IsValidIndex(PinIndex)
						&& Valid.IsValidIndex(PinIndex)
						&& Valid[PinIndex] != 0
						&& IsFiniteLayoutPoint(Anchors[PinIndex]);
					PinObj->SetBoolField(TEXT("anchor_valid"), bValid);
					if (bValid)
					{
						const FVector2D Anchor = Anchors[PinIndex];
						PinObj->SetNumberField(TEXT("anchor_x"), Anchor.X);
						PinObj->SetNumberField(TEXT("anchor_y"), Anchor.Y);
						PinObj->SetNumberField(TEXT("old_graph_x"), Node.OldX + Anchor.X);
						PinObj->SetNumberField(TEXT("old_graph_y"), Node.OldY + Anchor.Y);
						PinObj->SetNumberField(TEXT("new_graph_x"), Node.NewX + Anchor.X);
						PinObj->SetNumberField(TEXT("new_graph_y"), Node.NewY + Anchor.Y);
					}
					PinsJson.Add(MakeShared<FJsonValueObject>(PinObj));
				}
				return PinsJson;
			};

			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("node_guid"), Node.Guid);
			NodeObj->SetStringField(TEXT("node_name"), Node.Title);
			NodeObj->SetStringField(TEXT("node_class"), Node.ClassName);
			if (Ref.GraphNode)
			{
				NodeObj->SetStringField(TEXT("graph_node_object_name"), Ref.GraphNode->GetName());
				NodeObj->SetStringField(TEXT("graph_node_path"), Ref.GraphNode->GetPathName());
				NodeObj->SetStringField(TEXT("full_node_title"), Ref.GraphNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				if (const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Ref.GraphNode))
				{
					NodeObj->SetBoolField(TEXT("material_is_preview_expression"), MaterialNode->bIsPreviewExpression);
					if (const UMaterialExpression* Expression = MaterialNode->MaterialExpression)
					{
						NodeObj->SetStringField(TEXT("material_expression_class"), Expression->GetClass()->GetName());
						NodeObj->SetBoolField(TEXT("material_expression_hide_preview_window"), Expression->bHidePreviewWindow);
						NodeObj->SetBoolField(TEXT("material_expression_collapsed"), Expression->bCollapsed);
					}
				}
			}
			NodeObj->SetBoolField(TEXT("virtual_node"), Ref.bVirtual);
			NodeObj->SetNumberField(TEXT("width"), Node.Width);
			NodeObj->SetNumberField(TEXT("height"), Node.Height);
			NodeObj->SetNumberField(TEXT("old_x"), Node.OldX);
			NodeObj->SetNumberField(TEXT("old_y"), Node.OldY);
			NodeObj->SetNumberField(TEXT("new_x"), FMath::RoundToInt(Node.NewX));
			NodeObj->SetNumberField(TEXT("new_y"), FMath::RoundToInt(Node.NewY));
			NodeObj->SetNumberField(TEXT("rank"), Node.Rank);
			NodeObj->SetNumberField(TEXT("order"), Node.Order);
			NodeObj->SetArrayField(TEXT("input_pins"), PinsToJson(true));
			NodeObj->SetArrayField(TEXT("output_pins"), PinsToJson(false));
			return NodeObj;
		}

		TArray<TSharedPtr<FJsonValue>> SharedLayoutEdgesToJson(const FUniversalLayoutModel& Model)
		{
			TArray<TSharedPtr<FJsonValue>> EdgesJson;
			for (int32 EdgeIndex = 0; EdgeIndex < Model.Edges.Num(); ++EdgeIndex)
			{
				const UeAgentBlueprintAutoLayout::FEdge& Edge = Model.Edges[EdgeIndex];
				TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
				EdgeObj->SetNumberField(TEXT("index"), EdgeIndex);
				EdgeObj->SetNumberField(TEXT("source"), Edge.Source);
				EdgeObj->SetNumberField(TEXT("target"), Edge.Target);
				EdgeObj->SetStringField(TEXT("source_pin"), Edge.SourcePin);
				EdgeObj->SetStringField(TEXT("target_pin"), Edge.TargetPin);
				EdgeObj->SetNumberField(TEXT("source_pin_index"), Edge.SourcePinIndex);
				EdgeObj->SetNumberField(TEXT("target_pin_index"), Edge.TargetPinIndex);
				EdgeObj->SetBoolField(TEXT("exec"), Edge.bExec);
				if (Model.Nodes.IsValidIndex(Edge.Source))
				{
					EdgeObj->SetStringField(TEXT("source_node_name"), Model.Nodes[Edge.Source].Title);
				}
				if (Model.Nodes.IsValidIndex(Edge.Target))
				{
					EdgeObj->SetStringField(TEXT("target_node_name"), Model.Nodes[Edge.Target].Title);
				}

				FVector2D SourceAnchor = FVector2D::ZeroVector;
				FVector2D TargetAnchor = FVector2D::ZeroVector;
				const bool bHasSource = Model.Nodes.IsValidIndex(Edge.Source)
					&& TryGetMeasuredPinAnchor(Model.Nodes[Edge.Source], EGPD_Output, Edge.SourcePinIndex, SourceAnchor);
				const bool bHasTarget = Model.Nodes.IsValidIndex(Edge.Target)
					&& TryGetMeasuredPinAnchor(Model.Nodes[Edge.Target], EGPD_Input, Edge.TargetPinIndex, TargetAnchor);
				EdgeObj->SetBoolField(TEXT("source_anchor_valid"), bHasSource);
				EdgeObj->SetBoolField(TEXT("target_anchor_valid"), bHasTarget);
				if (bHasSource && bHasTarget)
				{
					const UeAgentBlueprintAutoLayout::FNode& SourceNode = Model.Nodes[Edge.Source];
					const UeAgentBlueprintAutoLayout::FNode& TargetNode = Model.Nodes[Edge.Target];
					const FVector2D OldSource(SourceNode.OldX + SourceAnchor.X, SourceNode.OldY + SourceAnchor.Y);
					const FVector2D OldTarget(TargetNode.OldX + TargetAnchor.X, TargetNode.OldY + TargetAnchor.Y);
					const FVector2D NewSource(SourceNode.NewX + SourceAnchor.X, SourceNode.NewY + SourceAnchor.Y);
					const FVector2D NewTarget(TargetNode.NewX + TargetAnchor.X, TargetNode.NewY + TargetAnchor.Y);
					EdgeObj->SetNumberField(TEXT("old_source_x"), OldSource.X);
					EdgeObj->SetNumberField(TEXT("old_source_y"), OldSource.Y);
					EdgeObj->SetNumberField(TEXT("old_target_x"), OldTarget.X);
					EdgeObj->SetNumberField(TEXT("old_target_y"), OldTarget.Y);
					EdgeObj->SetNumberField(TEXT("old_pin_y_delta"), OldSource.Y - OldTarget.Y);
					EdgeObj->SetNumberField(TEXT("new_source_x"), NewSource.X);
					EdgeObj->SetNumberField(TEXT("new_source_y"), NewSource.Y);
					EdgeObj->SetNumberField(TEXT("new_target_x"), NewTarget.X);
					EdgeObj->SetNumberField(TEXT("new_target_y"), NewTarget.Y);
					EdgeObj->SetNumberField(TEXT("new_pin_y_delta"), NewSource.Y - NewTarget.Y);
					EdgeObj->SetNumberField(TEXT("new_pin_y_residual"), FMath::Abs(NewSource.Y - NewTarget.Y));
				}
				EdgesJson.Add(MakeShared<FJsonValueObject>(EdgeObj));
			}
			return EdgesJson;
		}

		TArray<TSharedPtr<FJsonValue>> SharedReroutePlansToJson(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const UeAgentBlueprintAutoLayout::FReroutePlan& Plan : Plans)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("edge_index"), Plan.EdgeIndex);
				Obj->SetStringField(TEXT("reason"), Plan.Reason);
				Obj->SetStringField(TEXT("bus_id"), Plan.BusId);
				Obj->SetNumberField(TEXT("bus_lane_index"), Plan.BusLaneIndex);
				Obj->SetNumberField(TEXT("branch_lane_index"), Plan.BranchLaneIndex);
				Obj->SetNumberField(TEXT("attachment_merge_group"), Plan.AttachmentMergeGroup);
				TArray<TSharedPtr<FJsonValue>> Points;
				for (const FVector2D& Point : Plan.Points)
				{
					TSharedPtr<FJsonObject> PointObj = MakeShared<FJsonObject>();
					PointObj->SetNumberField(TEXT("x"), Point.X);
					PointObj->SetNumberField(TEXT("y"), Point.Y);
					Points.Add(MakeShared<FJsonValueObject>(PointObj));
				}
				Obj->SetArrayField(TEXT("points"), Points);
				TArray<TSharedPtr<FJsonValue>> KnotPoints;
				for (const FVector2D& Point : Plan.KnotPoints)
				{
					TSharedPtr<FJsonObject> PointObj = MakeShared<FJsonObject>();
					PointObj->SetNumberField(TEXT("x"), Point.X);
					PointObj->SetNumberField(TEXT("y"), Point.Y);
					KnotPoints.Add(MakeShared<FJsonValueObject>(PointObj));
				}
				Obj->SetArrayField(TEXT("knot_points"), KnotPoints);
				TArray<TSharedPtr<FJsonValue>> Roles;
				for (const UeAgentBlueprintAutoLayout::EReroutePointRole Role : Plan.PointRoles)
				{
					Roles.Add(MakeShared<FJsonValueString>(UeAgentBlueprintAutoLayout::ReroutePointRoleToString(Role)));
				}
				Obj->SetArrayField(TEXT("point_roles"), Roles);
				Items.Add(MakeShared<FJsonValueObject>(Obj));
			}
			return Items;
		}

		void AddSharedLayoutRunFields(const TSharedPtr<FJsonObject>& OutData, const UeAgentBlueprintAutoLayout::FRunResult& RunResult)
		{
			OutData->SetStringField(TEXT("layout_strategy"), RunResult.Strategy);
			OutData->SetStringField(TEXT("node_graph_layout_strategy"), TEXT("shared_blueprint_layout_core"));
			OutData->SetStringField(TEXT("layout_framework"), TEXT("clean_semantic_island_multiport_layout"));
			OutData->SetBoolField(TEXT("layout_design_complete"), RunResult.bDesignComplete);
			OutData->SetNumberField(TEXT("layout_design_feature_count"), RunResult.DesignFeatureCount);
			OutData->SetNumberField(TEXT("layout_design_feature_complete_count"), RunResult.DesignFeatureCompleteCount);
			OutData->SetNumberField(TEXT("layout_design_feature_partial_count"), RunResult.DesignFeaturePartialCount);
			OutData->SetNumberField(TEXT("layout_design_feature_missing_count"), RunResult.DesignFeatureMissingCount);
			OutData->SetArrayField(TEXT("layout_design_feature_coverage"), RunResult.DesignFeatureCoverage);
			OutData->SetNumberField(TEXT("reroute_plan_count"), RunResult.ReroutePlans.Num());
			OutData->SetNumberField(TEXT("local_bus_edge_count"), RunResult.LocalBusEdgeCount);
			OutData->SetNumberField(TEXT("external_bus_edge_count"), RunResult.ExternalBusEdgeCount);
			OutData->SetNumberField(TEXT("route_through_island_count"), RunResult.DesignDiagnostics.RouteThroughIslandCount);
			OutData->SetNumberField(TEXT("route_through_hard_occupancy_count"), RunResult.DesignDiagnostics.RouteThroughHardOccupancyCount);
			OutData->SetNumberField(TEXT("input_chain_count"), RunResult.DesignDiagnostics.InputChainCount);
			OutData->SetNumberField(TEXT("boundary_bus_count"), RunResult.DesignDiagnostics.BoundaryBusCount);
			OutData->SetNumberField(TEXT("island_internal_multi_column_bus_count"), RunResult.DesignDiagnostics.IslandInternalMultiColumnBusCount);
			OutData->SetNumberField(TEXT("island_internal_same_column_collision_bus_count"), RunResult.DesignDiagnostics.IslandInternalSameColumnCollisionBusCount);
			OutData->SetObjectField(TEXT("metrics_before"), UeAgentBlueprintAutoLayout::MetricsToJson(RunResult.MetricsBefore));
			OutData->SetObjectField(TEXT("metrics_after"), UeAgentBlueprintAutoLayout::MetricsToJson(RunResult.MetricsAfter));
			OutData->SetObjectField(TEXT("metrics_after_reroute_plan"), UeAgentBlueprintAutoLayout::MetricsToJson(RunResult.MetricsAfterReroute));
			OutData->SetArrayField(TEXT("reroute_plan"), SharedReroutePlansToJson(RunResult.ReroutePlans));
			OutData->SetArrayField(TEXT("lane_assignments"), RunResult.LaneAssignmentDiagnostics);
			OutData->SetArrayField(TEXT("rejection_diagnostics"), RunResult.RejectionDiagnostics);
		}

		void AddSharedGeometryFields(const TSharedPtr<FJsonObject>& OutData, const FSharedGraphGeometryStats& GeometryStats)
		{
			OutData->SetBoolField(TEXT("use_real_geometry"), true);
			OutData->SetBoolField(TEXT("used_real_geometry"), GeometryStats.bUsedRealGeometry);
			OutData->SetStringField(TEXT("geometry_source"), GeometryStats.GeometrySource);
			OutData->SetBoolField(TEXT("opened_editor_for_geometry"), GeometryStats.bOpenedEditorForGeometry);
			OutData->SetBoolField(TEXT("geometry_used_offscreen_graph_editor"), GeometryStats.bUsedOffscreenGraphEditor);
			OutData->SetNumberField(TEXT("geometry_node_count"), GeometryStats.NodeCount);
			OutData->SetNumberField(TEXT("measured_node_count"), GeometryStats.MeasuredNodeCount);
			OutData->SetNumberField(TEXT("missing_measured_node_geometry_count"), FMath::Max(0, GeometryStats.NodeCount - GeometryStats.MeasuredNodeCount));
			OutData->SetNumberField(TEXT("pin_widget_count"), GeometryStats.PinWidgetCount);
			OutData->SetNumberField(TEXT("pin_geometry_anchor_count"), GeometryStats.PinGeometryAnchorCount);
			OutData->SetNumberField(TEXT("missing_pin_anchor_count"), GeometryStats.MissingPinAnchorCount);
			OutData->SetNumberField(TEXT("missing_visible_pin_anchor_count"), GeometryStats.VisibleMissingPinAnchorCount);
			OutData->SetNumberField(TEXT("focused_geometry_node_count"), GeometryStats.FocusedGeometryNodeCount);
			OutData->SetBoolField(TEXT("focused_geometry_budget_exhausted"), GeometryStats.bFocusedGeometryBudgetExhausted);
			OutData->SetBoolField(TEXT("restored_graph_view_after_capture"), GeometryStats.bRestoredGraphViewAfterCapture);
			OutData->SetNumberField(TEXT("geometry_measurement_zoom"), GeometryStats.GeometryMeasurementZoomAmount);
			if (!GeometryStats.Error.IsEmpty())
			{
				OutData->SetStringField(TEXT("geometry_error"), GeometryStats.Error);
			}
		}

		bool OpenEditorForGeometryIfNeeded(UObject* Asset, bool& bOpenedEditor);
	}
}
