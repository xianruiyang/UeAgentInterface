// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"

#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "ControlRigBlueprint.h"
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
	namespace
	{
		struct FGraphRef
		{
			UEdGraph* Graph = nullptr;
			FString GraphKind;
			bool bSupported = true;
			FString UnsupportedReason;
		};

		struct FUniversalLayoutNodeRef
		{
			UEdGraphNode* GraphNode = nullptr;
			UMaterialExpression* MaterialExpression = nullptr;
			bool bVirtual = false;
		};

		struct FUniversalLayoutModel
		{
			TArray<UeAgentBlueprintAutoLayout::FNode> Nodes;
			TArray<UeAgentBlueprintAutoLayout::FEdge> Edges;
			TArray<FUniversalLayoutNodeRef> NodeRefs;
			TMap<UEdGraphNode*, int32> GraphNodeToIndex;
			TMap<UMaterialExpression*, int32> ExpressionToIndex;
			int32 WritableNodeCount = 0;
			int32 CollapsedRerouteNodeCount = 0;
			int32 CollapsedRerouteEdgeCount = 0;
		};

		struct FSharedGraphGeometryStats
		{
			bool bUsedRealGeometry = false;
			bool bOpenedEditorForGeometry = false;
			bool bUsedOffscreenGraphEditor = false;
			FString GeometrySource = TEXT("real_geometry_required");
			FString Error;
			int32 NodeCount = 0;
			int32 MeasuredNodeCount = 0;
			int32 PinWidgetCount = 0;
			int32 PinGeometryAnchorCount = 0;
			int32 VisibleMissingPinAnchorCount = 0;
			int32 MissingPinAnchorCount = 0;
			int32 FocusedGeometryNodeCount = 0;
			bool bFocusedGeometryBudgetExhausted = false;
			bool bRestoredGraphViewAfterCapture = false;
			FVector2f RestoredViewLocation = FVector2f::ZeroVector;
			float RestoredZoomAmount = 1.0f;
			float GeometryMeasurementZoomAmount = 1.0f;
		};

		struct FRootMaterialPropertySpec
		{
			const TCHAR* Name = TEXT("");
			EMaterialProperty Property = MP_BaseColor;
		};

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

		static bool SaveAssetPackage(UObject* Asset, FString& OutError)
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

		static bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, TSharedPtr<FJsonObject>& OutObj)
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (Obj.IsValid() && Obj->TryGetObjectField(Field, Found) && Found && Found->IsValid())
			{
				OutObj = *Found;
				return true;
			}
			return false;
		}

		static bool TryGetStringFromEither(const TSharedPtr<FJsonObject>& Primary, const TSharedPtr<FJsonObject>& Secondary, const FString& Field, FString& OutValue)
		{
			return (Primary.IsValid() && Primary->TryGetStringField(Field, OutValue))
				|| (Secondary.IsValid() && Secondary->TryGetStringField(Field, OutValue));
		}

		static bool TryGetBoolFromEither(const TSharedPtr<FJsonObject>& Primary, const TSharedPtr<FJsonObject>& Secondary, const FString& Field, bool& OutValue)
		{
			return (Primary.IsValid() && Primary->TryGetBoolField(Field, OutValue))
				|| (Secondary.IsValid() && Secondary->TryGetBoolField(Field, OutValue));
		}

		static bool TryGetNumberFromEither(const TSharedPtr<FJsonObject>& Primary, const TSharedPtr<FJsonObject>& Secondary, const FString& Field, double& OutValue)
		{
			return (Primary.IsValid() && Primary->TryGetNumberField(Field, OutValue))
				|| (Secondary.IsValid() && Secondary->TryGetNumberField(Field, OutValue));
		}

		static bool TryGetLayoutBool(const TSharedPtr<FJsonObject>& Params, const FString& Field, bool& OutValue)
		{
			TSharedPtr<FJsonObject> LayoutOptionsObj;
			TryGetObjectField(Params, TEXT("layout_options"), LayoutOptionsObj);
			return TryGetBoolFromEither(LayoutOptionsObj, Params, Field, OutValue);
		}

		static bool HasRemovedDryRunParameter(const TSharedPtr<FJsonObject>& Params)
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

		static bool TryComputeGraphEditorAccessOffsetX(double& OutOffset)
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

		static bool ParseSharedLayoutOptions(const TSharedPtr<FJsonObject>& Params, UeAgentBlueprintAutoLayout::FOptions& OutOptions, FString& OutError)
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
			Options.PinDirectionBiasX = GraphEditorAccessOffsetX;
			Options.OutputAccessOffsetX = GraphEditorAccessOffsetX;
			Options.InputAccessOffsetX = GraphEditorAccessOffsetX;
			Options.AccessOffsetSource = TEXT("graph_editor_settings_compute_spline_tangent");

			Options.bUseRealGeometry = true;
			Options.MissingPinAnchorCount = 0;
			OutOptions = Options;
			OutError.Reset();
			return true;
		}

		static void InitializePendingRealPins(
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

		static FString PinNameOrFallback(const FString& Name, const FString& Prefix, const int32 Index)
		{
			FString Trimmed = Name;
			Trimmed.TrimStartAndEndInline();
			return Trimmed.IsEmpty() ? FString::Printf(TEXT("%s%d"), *Prefix, Index) : Trimmed;
		}

		static FString AdapterForAsset(const UObject* Asset)
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

		static bool IsGraphLikelyStateMachineTopLevel(const UEdGraph* Graph)
		{
			if (!Graph)
			{
				return false;
			}
			const FString ClassName = Graph->GetClass()->GetName();
			return ClassName.Contains(TEXT("AnimationStateMachineGraph"))
				|| ClassName.Contains(TEXT("StateMachineGraph"))
				|| ClassName.Contains(TEXT("BehaviorTree"))
				|| ClassName.Contains(TEXT("StateTree"));
		}

		static FString BlueprintGraphKind(const UBlueprint* Blueprint, const UEdGraph* Graph)
		{
			if (!Blueprint || !Graph)
			{
				return TEXT("unknown");
			}
			auto ContainsGraph = [Graph](const TArray<UEdGraph*>& Graphs) -> bool
			{
				for (const UEdGraph* Candidate : Graphs)
				{
					if (Candidate == Graph)
					{
						return true;
					}
				}
				return false;
			};
			if (ContainsGraph(Blueprint->UbergraphPages))
			{
				return TEXT("event_graph");
			}
			if (ContainsGraph(Blueprint->FunctionGraphs))
			{
				return TEXT("function_graph");
			}
			if (ContainsGraph(Blueprint->MacroGraphs))
			{
				return TEXT("macro_graph");
			}
			if (ContainsGraph(Blueprint->DelegateSignatureGraphs))
			{
				return TEXT("delegate_graph");
			}
			return TEXT("nested_graph");
		}

		static int32 CountGraphEdges(const UEdGraph* Graph)
		{
			if (!Graph)
			{
				return 0;
			}
			int32 Count = 0;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output)
					{
						continue;
					}
					for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode() && LinkedPin->GetOwningNode()->GetGraph() == Graph)
						{
							++Count;
						}
					}
				}
			}
			return Count;
		}

		static int32 CountSemanticNodes(const UEdGraph* Graph)
		{
			if (!Graph)
			{
				return 0;
			}
			int32 Count = 0;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && !Node->IsA<UEdGraphNode_Comment>())
				{
					++Count;
				}
			}
			return Count;
		}

		static TSharedPtr<FJsonObject> GraphRefToJson(const FGraphRef& GraphRef)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("graph_name"), GraphRef.Graph ? GraphRef.Graph->GetName() : TEXT(""));
			Item->SetStringField(TEXT("graph_path"), GraphRef.Graph ? GraphRef.Graph->GetPathName() : TEXT(""));
			Item->SetStringField(TEXT("graph_kind"), GraphRef.GraphKind);
			Item->SetStringField(TEXT("graph_class"), GraphRef.Graph ? GraphRef.Graph->GetClass()->GetPathName() : TEXT(""));
			Item->SetBoolField(TEXT("supported"), GraphRef.bSupported);
			if (!GraphRef.bSupported)
			{
				Item->SetStringField(TEXT("unsupported_reason"), GraphRef.UnsupportedReason);
			}
			Item->SetNumberField(TEXT("node_count"), CountSemanticNodes(GraphRef.Graph));
			Item->SetNumberField(TEXT("edge_count"), CountGraphEdges(GraphRef.Graph));
			return Item;
		}

		static void AddUniqueGraph(TArray<FGraphRef>& Graphs, TSet<UEdGraph*>& SeenGraphs, UEdGraph* Graph, const FString& Kind)
		{
			if (!Graph || SeenGraphs.Contains(Graph))
			{
				return;
			}
			SeenGraphs.Add(Graph);
			FGraphRef Ref;
			Ref.Graph = Graph;
			Ref.GraphKind = Kind;
			if (IsGraphLikelyStateMachineTopLevel(Graph))
			{
				Ref.bSupported = false;
				Ref.UnsupportedReason = TEXT("non_pin_node_graph_or_vertical_state_graph");
			}
			Graphs.Add(Ref);
		}

		static TArray<FGraphRef> CollectBlueprintGraphs(UBlueprint* Blueprint, const bool bIncludeNestedGraphs)
		{
			TArray<FGraphRef> Graphs;
			TSet<UEdGraph*> SeenGraphs;
			if (!Blueprint)
			{
				return Graphs;
			}
			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				AddUniqueGraph(Graphs, SeenGraphs, Graph, TEXT("event_graph"));
			}
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				AddUniqueGraph(Graphs, SeenGraphs, Graph, TEXT("function_graph"));
			}
			for (UEdGraph* Graph : Blueprint->MacroGraphs)
			{
				AddUniqueGraph(Graphs, SeenGraphs, Graph, TEXT("macro_graph"));
			}
			for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
			{
				AddUniqueGraph(Graphs, SeenGraphs, Graph, TEXT("delegate_graph"));
			}
			if (bIncludeNestedGraphs)
			{
				TArray<UEdGraph*> AllGraphs;
				Blueprint->GetAllGraphs(AllGraphs);
				for (UEdGraph* Graph : AllGraphs)
				{
					AddUniqueGraph(Graphs, SeenGraphs, Graph, BlueprintGraphKind(Blueprint, Graph));
				}
			}
			return Graphs;
		}

		static TArray<FGraphRef> CollectGenericEdGraphs(UObject* Asset)
		{
			TArray<FGraphRef> Graphs;
			TSet<UEdGraph*> SeenGraphs;
			if (!Asset)
			{
				return Graphs;
			}
			TArray<UObject*> ChildObjects;
			GetObjectsWithOuter(Asset, ChildObjects, true);
			for (UObject* Child : ChildObjects)
			{
				if (UEdGraph* Graph = Cast<UEdGraph>(Child))
				{
					AddUniqueGraph(Graphs, SeenGraphs, Graph, TEXT("edgraph"));
				}
			}
			return Graphs;
		}

		static void CopyFieldsToTopLevel(const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Target)
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

		static void ConfigureIncludeFlagsFromKinds(const TArray<TSharedPtr<FJsonValue>>& KindValues, const TSharedPtr<FJsonObject>& Target)
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

		static TSharedPtr<FJsonObject> BuildBlueprintLayoutParams(const TSharedPtr<FJsonObject>& Params)
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

		static const TArray<FRootMaterialPropertySpec>& GetRootMaterialPropertySpecs()
		{
			static const TArray<FRootMaterialPropertySpec> Specs = {
				{ TEXT("BaseColor"), MP_BaseColor },
				{ TEXT("EmissiveColor"), MP_EmissiveColor },
				{ TEXT("Opacity"), MP_Opacity },
				{ TEXT("OpacityMask"), MP_OpacityMask },
				{ TEXT("Metallic"), MP_Metallic },
				{ TEXT("Specular"), MP_Specular },
				{ TEXT("Roughness"), MP_Roughness },
				{ TEXT("Normal"), MP_Normal },
				{ TEXT("Tangent"), MP_Tangent },
				{ TEXT("WorldPositionOffset"), MP_WorldPositionOffset },
				{ TEXT("SubsurfaceColor"), MP_SubsurfaceColor },
				{ TEXT("AmbientOcclusion"), MP_AmbientOcclusion },
				{ TEXT("Refraction"), MP_Refraction },
				{ TEXT("PixelDepthOffset"), MP_PixelDepthOffset },
				{ TEXT("Anisotropy"), MP_Anisotropy }
			};
			return Specs;
		}

		static TArray<UMaterialExpression*> GetMaterialGraphExpressions(UObject* Asset)
		{
			TArray<UMaterialExpression*> Expressions;
			if (UMaterial* Material = Cast<UMaterial>(Asset))
			{
				for (UMaterialExpression* Expression : Material->GetExpressions())
				{
					if (Expression)
					{
						Expressions.Add(Expression);
					}
				}
			}
			else if (UMaterialFunctionInterface* Function = Cast<UMaterialFunctionInterface>(Asset))
			{
				for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
				{
					if (Expression)
					{
						Expressions.Add(Expression.Get());
					}
				}
			}
			return Expressions;
		}

		static int32 CountMaterialExpressionEdges(const TArray<UMaterialExpression*>& Expressions)
		{
			TSet<UMaterialExpression*> ExpressionSet(Expressions);
			int32 Count = 0;
			for (UMaterialExpression* Expression : Expressions)
			{
				if (!Expression)
				{
					continue;
				}
				for (FExpressionInputIterator It{ Expression }; It; ++It)
				{
					if (It->Expression && ExpressionSet.Contains(It->Expression))
					{
						++Count;
					}
				}
			}
			return Count;
		}

		static int32 CountMaterialRootEdges(UMaterial* Material, const TArray<UMaterialExpression*>& Expressions)
		{
			if (!Material)
			{
				return 0;
			}
			TSet<UMaterialExpression*> ExpressionSet(Expressions);
			int32 Count = 0;
			for (const FRootMaterialPropertySpec& PropertySpec : GetRootMaterialPropertySpecs())
			{
				FExpressionInput* Input = Material->GetExpressionInputForProperty(PropertySpec.Property);
				if (Input && Input->Expression && ExpressionSet.Contains(Input->Expression))
				{
					++Count;
				}
			}
			return Count;
		}

		static int32 CountMaterialLayoutEdges(UObject* Asset, const TArray<UMaterialExpression*>& Expressions)
		{
			return CountMaterialExpressionEdges(Expressions) + CountMaterialRootEdges(Cast<UMaterial>(Asset), Expressions);
		}

		static void AddMaterialGraphSummary(UObject* Asset, TArray<TSharedPtr<FJsonValue>>& Graphs)
		{
			TArray<UMaterialExpression*> Expressions;
			FString GraphName = TEXT("MaterialGraph");
			FString GraphKind = TEXT("material_graph");
			if (Asset->IsA<UMaterial>())
			{
				Expressions = GetMaterialGraphExpressions(Asset);
			}
			else if (Asset->IsA<UMaterialFunctionInterface>())
			{
				Expressions = GetMaterialGraphExpressions(Asset);
				GraphName = TEXT("MaterialFunctionGraph");
				GraphKind = TEXT("material_function_graph");
			}
			else
			{
				return;
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("graph_name"), GraphName);
			Item->SetStringField(TEXT("graph_path"), Asset->GetPathName() + TEXT(":") + GraphName);
			Item->SetStringField(TEXT("graph_kind"), GraphKind);
			Item->SetStringField(TEXT("graph_class"), TEXT("material_expression_graph"));
			Item->SetBoolField(TEXT("supported"), true);
			Item->SetStringField(TEXT("layout_adapter"), TEXT("shared_blueprint_layout_core"));
			Item->SetNumberField(TEXT("node_count"), Expressions.Num());
			Item->SetNumberField(TEXT("edge_count"), CountMaterialLayoutEdges(Asset, Expressions));
			Graphs.Add(MakeShared<FJsonValueObject>(Item));
		}

		static FString GetMaterialInputPinName(UMaterialExpression* Expression, const int32 InputIndex, const FExpressionInput* Input)
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

		static FString GetMaterialOutputPinName(UMaterialExpression* Expression, const int32 OutputIndex)
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

		static TArray<FString> GetMaterialInputPinNames(UMaterialExpression* Expression)
		{
			TArray<FString> Pins;
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				Pins.Add(GetMaterialInputPinName(Expression, It.Index, It.Input));
			}
			return Pins;
		}

		static TArray<FString> GetMaterialOutputPinNames(UMaterialExpression* Expression)
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

		static int32 GetMaterialInputPinSlotIndex(UMaterialExpression* Expression, const int32 InputIndex)
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

		static int32 GetMaterialOutputPinSlotIndex(UMaterialExpression* Expression, const int32 OutputIndex)
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

		static bool HasLayoutNumberField(const TSharedPtr<FJsonObject>& Params, const FString& Field)
		{
			TSharedPtr<FJsonObject> LayoutOptionsObj;
			TryGetObjectField(Params, TEXT("layout_options"), LayoutOptionsObj);
			double Ignored = 0.0;
			return TryGetNumberFromEither(LayoutOptionsObj, Params, Field, Ignored);
		}

		static void ApplyDefaultOriginFromWritableNodes(
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

		static bool IsFiniteLayoutPoint(const FVector2D& Point);

		static bool TryGetMeasuredPinAnchor(
			const UeAgentBlueprintAutoLayout::FNode& Node,
			const EEdGraphPinDirection Direction,
			const int32 PinIndex,
			FVector2D& OutAnchor);

		static TSharedPtr<FJsonObject> SharedLayoutNodeToJson(
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

		static TArray<TSharedPtr<FJsonValue>> SharedLayoutEdgesToJson(const FUniversalLayoutModel& Model)
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

		static TArray<TSharedPtr<FJsonValue>> SharedReroutePlansToJson(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans)
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

		static void AddSharedLayoutRunFields(const TSharedPtr<FJsonObject>& OutData, const UeAgentBlueprintAutoLayout::FRunResult& RunResult)
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
			OutData->SetNumberField(TEXT("final_visible_bus_lane_adjustment_count"), RunResult.DesignDiagnostics.FinalVisibleBusLaneAdjustmentCount);
			OutData->SetNumberField(TEXT("final_visible_bus_lane_violation_count"), RunResult.DesignDiagnostics.FinalVisibleBusLaneViolationCount);
			OutData->SetNumberField(TEXT("final_visible_branch_x_lane_adjustment_count"), RunResult.DesignDiagnostics.FinalVisibleBranchXLaneAdjustmentCount);
			OutData->SetNumberField(TEXT("final_visible_branch_x_lane_violation_count"), RunResult.DesignDiagnostics.FinalVisibleBranchXLaneViolationCount);
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

		static void AddSharedGeometryFields(const TSharedPtr<FJsonObject>& OutData, const FSharedGraphGeometryStats& GeometryStats)
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

		static bool OpenEditorForGeometryIfNeeded(UObject* Asset, bool& bOpenedEditor);
		static void PumpAndTickSlateForLayoutGeometry();

		static UMaterialGraph* CreateTransientMaterialFunctionGraphForLayout(UMaterialFunction* Function, FString& OutError)
		{
			if (!Function)
			{
				OutError = TEXT("material_function_required_for_transient_layout_graph");
				return nullptr;
			}

			UMaterial* PreviewMaterial = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transactional | RF_Transient);
			if (!PreviewMaterial)
			{
				OutError = TEXT("material_function_preview_material_create_failed");
				return nullptr;
			}
			PreviewMaterial->SetShadingModel(MSM_Unlit);
			PreviewMaterial->MaterialDomain = Function->PreviewMaterialDomain;

			UMaterialFunction* PreviewFunction = Cast<UMaterialFunction>(
				StaticDuplicateObject(
					Function,
					GetTransientPackage(),
					NAME_None,
					~RF_Standalone,
					Function->GetClass()));
			if (!PreviewFunction)
			{
				OutError = TEXT("material_function_preview_duplicate_failed");
				return nullptr;
			}
			PreviewFunction->ParentFunction = Function;
			PreviewFunction->SetFlags(RF_Transactional);

			UMaterialGraph* Graph = CastChecked<UMaterialGraph>(
				FBlueprintEditorUtils::CreateNewGraph(
					PreviewMaterial,
					NAME_None,
					UMaterialGraph::StaticClass(),
					UMaterialGraphSchema::StaticClass()));
			PreviewMaterial->MaterialGraph = Graph;
			Graph->Material = PreviewMaterial;
			Graph->MaterialFunction = PreviewFunction;
			Graph->OriginalMaterialFullName = Function->GetName();
			PreviewFunction->MaterialGraph = Graph;
			PreviewFunction->EditorMaterial = PreviewMaterial;

			PreviewMaterial->AssignExpressionCollection(PreviewFunction->GetExpressionCollection());
			PreviewMaterial->bEnableNewHLSLGenerator = PreviewFunction->IsUsingNewHLSLGenerator();
			for (UMaterialExpression* Expression : PreviewMaterial->GetExpressions())
			{
				if (Expression)
				{
					Expression->Function = nullptr;
					Expression->Material = PreviewMaterial;
				}
			}
			UMaterialExpressionFunctionOutput* PreviewOutput = nullptr;
			UMaterialExpressionFunctionOutput* FirstOutput = nullptr;
			for (int32 ExpressionIndex = PreviewMaterial->GetExpressions().Num() - 1; ExpressionIndex >= 0; --ExpressionIndex)
			{
				if (UMaterialExpressionFunctionOutput* FunctionOutput =
					Cast<UMaterialExpressionFunctionOutput>(PreviewMaterial->GetExpressions()[ExpressionIndex]))
				{
					FirstOutput = FunctionOutput;
					if (FunctionOutput->bLastPreviewed)
					{
						PreviewOutput = FunctionOutput;
					}
				}
			}
			if (!PreviewOutput)
			{
				PreviewOutput = FirstOutput;
			}
			const FGuid PreviewOutputGuid = PreviewOutput
				? PreviewOutput->MaterialExpressionGuid
				: FGuid();
			if (PreviewOutput)
			{
				PreviewOutput->bLastPreviewed = true;
			}
			Graph->RebuildGraph();
			if (PreviewOutput)
			{
				int32 FunctionOutputNodeCount = 0;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
					if (MaterialNode && Cast<UMaterialExpressionFunctionOutput>(MaterialNode->MaterialExpression))
					{
						++FunctionOutputNodeCount;
					}
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node))
					{
						const UMaterialExpression* NodeExpression = MaterialNode->MaterialExpression;
						const bool bMatchesPreviewOutput =
							NodeExpression == PreviewOutput
							|| (NodeExpression && PreviewOutputGuid.IsValid() && NodeExpression->MaterialExpressionGuid == PreviewOutputGuid)
							|| (FunctionOutputNodeCount == 1 && Cast<UMaterialExpressionFunctionOutput>(NodeExpression));
						MaterialNode->bIsPreviewExpression = bMatchesPreviewOutput;
					}
				}
			}
			return Graph;
		}

		static UMaterialGraph* ResolveMaterialSharedGraph(UObject* Asset, FString& OutError)
		{
			if (!Asset)
			{
				OutError = TEXT("asset_not_found");
				return nullptr;
			}
			UMaterialGraph* Graph = nullptr;
			bool bGraphAlreadyRebuiltWithPreviewState = false;
			if (UMaterial* Material = Cast<UMaterial>(Asset))
			{
#if WITH_EDITORONLY_DATA
				if (!Material->MaterialGraph)
				{
					Material->MaterialGraph = CastChecked<UMaterialGraph>(
						FBlueprintEditorUtils::CreateNewGraph(
							Material,
							NAME_None,
							UMaterialGraph::StaticClass(),
							UMaterialGraphSchema::StaticClass()));
				}
				Graph = Material->MaterialGraph;
				if (Graph && Graph == Material->MaterialGraph)
				{
					Graph->Material = Material;
					Graph->MaterialFunction = nullptr;
					Graph->OriginalMaterialFullName = Material->GetName();
				}
#endif
			}
			else if (Cast<UMaterialFunction>(Asset))
			{
#if WITH_EDITORONLY_DATA
				UMaterialFunction* Function = CastChecked<UMaterialFunction>(Asset);
				Graph = CreateTransientMaterialFunctionGraphForLayout(Function, OutError);
				if (!Graph)
				{
					return nullptr;
				}
				bGraphAlreadyRebuiltWithPreviewState = true;
#endif
			}
			if (!Graph && !Cast<UMaterialFunction>(Asset))
			{
				bool bOpenedEditor = false;
				OpenEditorForGeometryIfNeeded(Asset, bOpenedEditor);
				PumpAndTickSlateForLayoutGeometry();
				PumpAndTickSlateForLayoutGeometry();
				if (UMaterial* Material = Cast<UMaterial>(Asset))
				{
#if WITH_EDITORONLY_DATA
					if (!Material->MaterialGraph)
					{
						Material->MaterialGraph = CastChecked<UMaterialGraph>(
							FBlueprintEditorUtils::CreateNewGraph(
								Material,
								NAME_None,
								UMaterialGraph::StaticClass(),
								UMaterialGraphSchema::StaticClass()));
					}
					Graph = Material->MaterialGraph;
					if (Graph)
					{
						Graph->Material = Material;
						Graph->MaterialFunction = nullptr;
						Graph->OriginalMaterialFullName = Material->GetName();
					}
#endif
				}
			}
			if (!Graph)
			{
				OutError = TEXT("material_edgraph_not_found");
				return nullptr;
			}
			if (!bGraphAlreadyRebuiltWithPreviewState)
			{
				Graph->RebuildGraph();
			}
			return Graph;
		}

		static UEdGraph* ResolveGenericGraph(UObject* Asset, const FString& GraphRef)
		{
			const TArray<FGraphRef> Graphs = CollectGenericEdGraphs(Asset);
			for (const FGraphRef& Ref : Graphs)
			{
				if (!Ref.Graph)
				{
					continue;
				}
				if (Ref.Graph->GetName().Equals(GraphRef, ESearchCase::IgnoreCase)
					|| Ref.Graph->GetPathName().Equals(GraphRef, ESearchCase::IgnoreCase))
				{
					return Ref.Graph;
				}
			}
			if (Graphs.Num() == 1 && GraphRef.IsEmpty())
			{
				return Graphs[0].Graph;
			}
			return nullptr;
		}

		static bool IsExecLikeGraphPin(const UEdGraphPin* Pin)
		{
			if (!Pin)
			{
				return false;
			}
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->PinType.PinCategory == FName(TEXT("exec")))
			{
				return true;
			}
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				if (const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					if (ScriptStruct->IsChildOf(FRigVMExecutePin::StaticStruct()))
					{
						return true;
					}
				}
			}
			if (const URigVMEdGraphNode* RigNode = Cast<URigVMEdGraphNode>(Pin->GetOwningNode()))
			{
				if (const URigVMPin* ModelPin = RigNode->FindModelPinFromGraphPin(Pin))
				{
					return ModelPin->IsExecuteContext();
				}
			}
			return false;
		}

		static bool GetSharedReroutePassThroughPins(UEdGraphNode* Node, UEdGraphPin*& OutInputPin, UEdGraphPin*& OutOutputPin)
		{
			OutInputPin = nullptr;
			OutOutputPin = nullptr;
			if (!Node)
			{
				return false;
			}
			int32 InputPinIndex = INDEX_NONE;
			int32 OutputPinIndex = INDEX_NONE;
			if (Node->ShouldDrawNodeAsControlPointOnly(InputPinIndex, OutputPinIndex))
			{
				if (!Node->Pins.IsValidIndex(InputPinIndex) || !Node->Pins.IsValidIndex(OutputPinIndex))
				{
					return false;
				}
				OutInputPin = Node->Pins[InputPinIndex];
				OutOutputPin = Node->Pins[OutputPinIndex];
				return OutInputPin && OutOutputPin;
			}

			const URigVMEdGraphNode* RigNode = Cast<URigVMEdGraphNode>(Node);
			const FString NodeName = Node->GetName();
			const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			const FString ModelPath = RigNode ? RigNode->GetModelNodePath() : FString();
			const bool bLooksLikeRigVMReroute =
				NodeName.StartsWith(TEXT("RerouteNode_"))
				|| Title.StartsWith(TEXT("RerouteNode_"))
				|| ModelPath.StartsWith(TEXT("RerouteNode_"));
			if (!bLooksLikeRigVMReroute)
			{
				return false;
			}

			TArray<UEdGraphPin*> CandidateInputs;
			TArray<UEdGraphPin*> CandidateOutputs;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->ParentPin != nullptr)
				{
					continue;
				}
				if (Pin->Direction == EGPD_Input)
				{
					CandidateInputs.Add(Pin);
				}
				else if (Pin->Direction == EGPD_Output)
				{
					CandidateOutputs.Add(Pin);
				}
			}
			if (CandidateInputs.Num() != 1 || CandidateOutputs.Num() != 1)
			{
				return false;
			}
			OutInputPin = CandidateInputs[0];
			OutOutputPin = CandidateOutputs[0];
			const bool bValuePassThrough =
				OutInputPin->PinName == OutOutputPin->PinName
				|| OutInputPin->PinName == TEXT("Value")
				|| OutOutputPin->PinName == TEXT("Value");
			if (!bValuePassThrough || (OutInputPin->LinkedTo.Num() == 0 && OutOutputPin->LinkedTo.Num() == 0))
			{
				OutInputPin = nullptr;
				OutOutputPin = nullptr;
				return false;
			}
			return OutInputPin && OutOutputPin;
		}

		static bool IsSharedLayoutRerouteNode(UEdGraphNode* Node)
		{
			UEdGraphPin* InputPin = nullptr;
			UEdGraphPin* OutputPin = nullptr;
			return GetSharedReroutePassThroughPins(Node, InputPin, OutputPin);
		}

		static void CollectTerminalSharedLinkedPins(UEdGraphPin* LinkedPin, TArray<UEdGraphPin*>& OutPins, TSet<UEdGraphPin*>& VisitedPins)
		{
			if (!LinkedPin || VisitedPins.Contains(LinkedPin))
			{
				return;
			}
			VisitedPins.Add(LinkedPin);

			UEdGraphNode* Owner = LinkedPin->GetOwningNode();
			UEdGraphPin* RerouteInput = nullptr;
			UEdGraphPin* RerouteOutput = nullptr;
			if (!GetSharedReroutePassThroughPins(Owner, RerouteInput, RerouteOutput))
			{
				OutPins.Add(LinkedPin);
				return;
			}

			UEdGraphPin* NextPin = nullptr;
			if (LinkedPin == RerouteInput)
			{
				NextPin = RerouteOutput;
			}
			else if (LinkedPin == RerouteOutput)
			{
				NextPin = RerouteInput;
			}
			if (!NextPin)
			{
				OutPins.Add(LinkedPin);
				return;
			}
			for (UEdGraphPin* NextLinked : NextPin->LinkedTo)
			{
				CollectTerminalSharedLinkedPins(NextLinked, OutPins, VisitedPins);
			}
		}

		static bool IsVisibleSharedGraphPin(const UEdGraphPin* Pin)
		{
			if (!Pin)
			{
				return false;
			}
			return !Pin->bHidden || Pin->LinkedTo.Num() > 0 || IsExecLikeGraphPin(Pin);
		}

		static FString GraphPinNameString(const UEdGraphPin* Pin)
		{
			return PinNameOrFallback(Pin ? Pin->GetName() : FString(), TEXT("Pin_"), 0);
		}

		static TArray<UEdGraphPin*> GetSharedGraphPinsByDirection(UEdGraphNode* Node, EEdGraphPinDirection Direction)
		{
			TArray<UEdGraphPin*> OutPins;
			if (!Node)
			{
				return OutPins;
			}
			TSet<const UEdGraphPin*> SeenPins;
			TFunction<void(UEdGraphPin*)> VisitPin = [&](UEdGraphPin* Pin)
			{
				if (!Pin || SeenPins.Contains(Pin))
				{
					return;
				}
				SeenPins.Add(Pin);
				if (IsVisibleSharedGraphPin(Pin) && Pin->Direction == Direction)
				{
					OutPins.Add(Pin);
				}
				for (UEdGraphPin* SubPin : Pin->SubPins)
				{
					VisitPin(SubPin);
				}
			};
			for (UEdGraphPin* Pin : Node->Pins)
			{
				VisitPin(Pin);
			}
			return OutPins;
		}

		static int32 GetSharedGraphPinIndexInDirection(const UEdGraphPin* Pin)
		{
			if (!Pin)
			{
				return INDEX_NONE;
			}
			const UEdGraphNode* Node = Pin->GetOwningNode();
			if (!Node)
			{
				return INDEX_NONE;
			}
			const TArray<UEdGraphPin*> DirectionPins = GetSharedGraphPinsByDirection(const_cast<UEdGraphNode*>(Node), Pin->Direction);
			for (int32 Index = 0; Index < DirectionPins.Num(); ++Index)
			{
				if (DirectionPins[Index] == Pin)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}

		static bool IsFiniteLayoutPoint(const FVector2D& Point)
		{
			return FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y);
		}

		static bool IsUsableSlateGeometry(const FGeometry& Geometry)
		{
			const FVector2f Size = Geometry.GetLocalSize();
			const FVector2f AbsolutePosition = Geometry.GetAbsolutePosition();
			return Size.X > 0.5f
				&& Size.Y > 0.5f
				&& FMath::IsFinite(AbsolutePosition.X)
				&& FMath::IsFinite(AbsolutePosition.Y);
		}

		static bool TryGetUsableWidgetGeometry(const SWidget& Widget, FGeometry& OutGeometry)
		{
			const FGeometry TickGeometry = Widget.GetTickSpaceGeometry();
			if (IsUsableSlateGeometry(TickGeometry))
			{
				OutGeometry = TickGeometry;
				return true;
			}
			const FGeometry CachedGeometry = Widget.GetCachedGeometry();
			if (IsUsableSlateGeometry(CachedGeometry))
			{
				OutGeometry = CachedGeometry;
				return true;
			}
			return false;
		}

		static void SetMeasuredPinAnchor(
			UeAgentBlueprintAutoLayout::FNode& Node,
			const EEdGraphPinDirection Direction,
			const int32 PinIndex,
			const FVector2D& Anchor)
		{
			if (PinIndex == INDEX_NONE || !IsFiniteLayoutPoint(Anchor))
			{
				return;
			}
			if (Direction == EGPD_Input)
			{
				if (Node.InputPinAnchors.IsValidIndex(PinIndex) && Node.InputPinAnchorValid.IsValidIndex(PinIndex))
				{
					Node.InputPinAnchors[PinIndex] = Anchor;
					Node.InputPinAnchorValid[PinIndex] = 1;
				}
			}
			else if (Direction == EGPD_Output)
			{
				if (Node.OutputPinAnchors.IsValidIndex(PinIndex) && Node.OutputPinAnchorValid.IsValidIndex(PinIndex))
				{
					Node.OutputPinAnchors[PinIndex] = Anchor;
					Node.OutputPinAnchorValid[PinIndex] = 1;
				}
			}
		}

		static bool TryGetMeasuredPinAnchor(
			const UeAgentBlueprintAutoLayout::FNode& Node,
			const EEdGraphPinDirection Direction,
			const int32 PinIndex,
			FVector2D& OutAnchor)
		{
			if (PinIndex == INDEX_NONE)
			{
				return false;
			}
			if (Direction == EGPD_Input)
			{
				if (Node.InputPinAnchors.IsValidIndex(PinIndex)
					&& Node.InputPinAnchorValid.IsValidIndex(PinIndex)
					&& Node.InputPinAnchorValid[PinIndex] != 0)
				{
					OutAnchor = Node.InputPinAnchors[PinIndex];
					return true;
				}
			}
			else if (Direction == EGPD_Output)
			{
				if (Node.OutputPinAnchors.IsValidIndex(PinIndex)
					&& Node.OutputPinAnchorValid.IsValidIndex(PinIndex)
					&& Node.OutputPinAnchorValid[PinIndex] != 0)
				{
					OutAnchor = Node.OutputPinAnchors[PinIndex];
					return true;
				}
			}
			return false;
		}

		static int32 CountMeasuredPinAnchors(const UeAgentBlueprintAutoLayout::FNode& Node, const EEdGraphPinDirection Direction)
		{
			const TArray<uint8>& Valid = Direction == EGPD_Input ? Node.InputPinAnchorValid : Node.OutputPinAnchorValid;
			int32 Count = 0;
			for (const uint8 bValid : Valid)
			{
				if (bValid != 0)
				{
					++Count;
				}
			}
			return Count;
		}

		static void PumpAndTickSlateForLayoutGeometry()
		{
			if (!FSlateApplication::IsInitialized())
			{
				return;
			}
			FSlateApplication& SlateApp = FSlateApplication::Get();
			SlateApp.PumpMessages();
			if (!SlateApp.IsTicking())
			{
				SlateApp.Tick(ESlateTickType::All);
			}
		}

		static FVector2f SnapViewLocationToPixelGrid(const FVector2f& Location, const float ZoomAmount)
		{
			const float SafeZoom = FMath::Max(ZoomAmount, 0.01f);
			return FVector2f(
				FMath::RoundToFloat(Location.X * SafeZoom) / SafeZoom,
				FMath::RoundToFloat(Location.Y * SafeZoom) / SafeZoom);
		}

		static void FindArrangedDescendantGeometries(
			const SWidget& Widget,
			const FGeometry& WidgetGeometry,
			const TSet<TSharedRef<SWidget>>& WidgetsToFind,
			TMap<TSharedRef<SWidget>, FArrangedWidget>& OutResult)
		{
			FArrangedChildren ArrangedChildren(EVisibility::Visible);
			Widget.ArrangeChildren(WidgetGeometry, ArrangedChildren, true);
			for (int32 ChildIndex = 0; ChildIndex < ArrangedChildren.Num(); ++ChildIndex)
			{
				const FArrangedWidget& Child = ArrangedChildren[ChildIndex];
				if (WidgetsToFind.Contains(Child.Widget))
				{
					OutResult.Add(Child.Widget, Child);
				}
			}
			if (OutResult.Num() == WidgetsToFind.Num())
			{
				return;
			}
			for (int32 ChildIndex = 0; ChildIndex < ArrangedChildren.Num(); ++ChildIndex)
			{
				const FArrangedWidget& Child = ArrangedChildren[ChildIndex];
				FindArrangedDescendantGeometries(Child.Widget.Get(), Child.Geometry, WidgetsToFind, OutResult);
				if (OutResult.Num() == WidgetsToFind.Num())
				{
					return;
				}
			}
		}

		static FVector2f GetGraphPinConnectionPointAbsolute(
			const FGeometry& PinGeometry,
			const EEdGraphPinDirection Direction,
			const float ZoomAmount)
		{
			constexpr float StartFudgeX = 4.0f;
			constexpr float EndFudgeX = 4.0f;
			const FSlateBrush* ArrowImage = FAppStyle::GetBrush(TEXT("Graph.Arrow"));
			const float ArrowRadiusX = ArrowImage
				? ArrowImage->ImageSize.X * ZoomAmount * 0.5f
				: 0.0f;
			const FVector2f DrawSize = PinGeometry.GetDrawSize();
			const float CenterY = PinGeometry.AbsolutePosition.Y + DrawSize.Y * 0.5f;
			if (Direction == EGPD_Input)
			{
				return FVector2f(
					PinGeometry.AbsolutePosition.X - (ArrowRadiusX - EndFudgeX),
					CenterY);
			}
			return FVector2f(
				PinGeometry.AbsolutePosition.X + DrawSize.X - StartFudgeX,
				CenterY);
		}

		static bool TryGetCachedPinNodeOffsetY(const SGraphPin& PinWidget, double& OutNodeOffsetY)
		{
			// Match the data used by SGraphPanel's built-in alignment code:
			// NodePosY + SGraphPin::GetNodeOffset().Y. In minimized/headless paths the
			// widget may not have ticked yet, and UE leaves this cache at 0; treat that
			// as unavailable and fall back to arranged geometry.
			const FVector2f CachedPinNodeOffset = UE::Slate::CastToVector2f(PinWidget.GetNodeOffset());
			if (!FMath::IsFinite(CachedPinNodeOffset.Y) || FMath::Abs(CachedPinNodeOffset.Y) <= 0.5f)
			{
				return false;
			}
			OutNodeOffsetY = static_cast<double>(CachedPinNodeOffset.Y);
			return true;
		}

		static bool TryConvertAbsolutePinPointToNodeAnchor(
			SGraphPanel& GraphPanel,
			const SGraphNode& GraphNodeWidget,
			const FVector2D& NodeGraphPosition,
			const FVector2f& PinPointAbsolute,
			FVector2D& OutAnchor)
		{
			FGeometry NodeGeometry;
			if (TryGetUsableWidgetGeometry(GraphNodeWidget, NodeGeometry))
			{
				const float ZoomAmount = FMath::Max(GraphPanel.GetZoomAmount(), 0.01f);
				const FVector2f NodeAbsolute = NodeGeometry.GetAbsolutePosition();
				const FVector2f LocalGraphOffset = (PinPointAbsolute - NodeAbsolute) / ZoomAmount;
				OutAnchor = FVector2D(static_cast<double>(LocalGraphOffset.X), static_cast<double>(LocalGraphOffset.Y));
				if (IsFiniteLayoutPoint(OutAnchor))
				{
					return true;
				}
			}

			FGeometry PanelGeometry;
			if (!TryGetUsableWidgetGeometry(GraphPanel, PanelGeometry))
			{
				PanelGeometry = FGeometry::MakeRoot(FVector2f(4096.0f, 4096.0f), FSlateLayoutTransform(1.0f));
			}
			const FVector2f PinCenterPanel = PanelGeometry.AbsoluteToLocal(PinPointAbsolute);
			const FVector2f PinCenterGraph = GraphPanel.PanelCoordToGraphCoord(PinCenterPanel);
			OutAnchor = FVector2D(
				static_cast<double>(PinCenterGraph.X) - NodeGraphPosition.X,
				static_cast<double>(PinCenterGraph.Y) - NodeGraphPosition.Y);
			return IsFiniteLayoutPoint(OutAnchor);
		}

		static bool TryGetArrangedPinAnchor(
			SGraphPanel& GraphPanel,
			SGraphNode& GraphNodeWidget,
			const TSharedRef<SGraphPin>& PinWidget,
			const FVector2D& NodeGraphPosition,
			const EEdGraphPinDirection Direction,
			FVector2D& OutAnchor)
		{
			FGeometry PanelGeometry;
			if (!TryGetUsableWidgetGeometry(GraphPanel, PanelGeometry))
			{
				PanelGeometry = FGeometry::MakeRoot(FVector2f(4096.0f, 4096.0f), FSlateLayoutTransform(1.0f));
			}

			const float ZoomAmount = FMath::Max(GraphPanel.GetZoomAmount(), 0.01f);
			const float WidgetScale = FSlateApplication::IsInitialized()
				? FSlateApplication::Get().GetApplicationScale()
				: 1.0f;
			GraphNodeWidget.SlatePrepass(WidgetScale * ZoomAmount);

			TSet<TSharedRef<SWidget>> WidgetsToFind;
			WidgetsToFind.Add(PinWidget);

			TMap<TSharedRef<SWidget>, FArrangedWidget> ArrangedWidgets;
			FGeometry ArrangedNodeGeometry;
			bool bHasArrangedNodeGeometry = false;
			{
				const auto ViewOffset = GraphPanel.GetViewOffset();
				const FVector2f NodePanelOffset(
					GraphNodeWidget.GetPosition2f().X - static_cast<float>(ViewOffset.X),
					GraphNodeWidget.GetPosition2f().Y - static_cast<float>(ViewOffset.Y));
				const FArrangedWidget ArrangedNode = PanelGeometry.MakeChild(
					GraphNodeWidget.AsShared(),
					NodePanelOffset,
					GraphNodeWidget.GetDesiredSize(),
					ZoomAmount);
				ArrangedNodeGeometry = ArrangedNode.Geometry;
				bHasArrangedNodeGeometry = true;
				FindArrangedDescendantGeometries(GraphNodeWidget, ArrangedNode.Geometry, WidgetsToFind, ArrangedWidgets);
			}

			if (ArrangedWidgets.Num() < WidgetsToFind.Num())
			{
				FindArrangedDescendantGeometries(GraphPanel, PanelGeometry, WidgetsToFind, ArrangedWidgets);
			}

			const FArrangedWidget* PinGeometry = ArrangedWidgets.Find(PinWidget);
			FGeometry FallbackPinGeometry;
			if (!PinGeometry && TryGetUsableWidgetGeometry(PinWidget.Get(), FallbackPinGeometry))
			{
				ArrangedWidgets.Add(PinWidget, FArrangedWidget(PinWidget, FallbackPinGeometry));
				PinGeometry = ArrangedWidgets.Find(PinWidget);
			}
			if (!PinGeometry)
			{
				return false;
			}
			auto TryComputeArrangedNodeOffsetY = [&ArrangedNodeGeometry, bHasArrangedNodeGeometry](
				const FGeometry& Geometry,
				double& OutNodeOffsetY) -> bool
			{
				if (!bHasArrangedNodeGeometry)
				{
					return false;
				}
				const float PinScale = FMath::Max(Geometry.Scale, 0.01f);
				const float NodeScale = FMath::Max(ArrangedNodeGeometry.Scale, 0.01f);
				const FVector2f NodeOffset = Geometry.AbsolutePosition / PinScale - ArrangedNodeGeometry.AbsolutePosition / NodeScale;
				const float CenterY = NodeOffset.Y + Geometry.Size.Y * 0.5f;
				if (!FMath::IsFinite(CenterY))
				{
					return false;
				}
				OutNodeOffsetY = static_cast<double>(CenterY);
				return true;
			};

			const FVector2f PinPointAbsolute = GetGraphPinConnectionPointAbsolute(
				PinGeometry->Geometry,
				Direction,
				ZoomAmount);
			double PinNodeOffsetY = 0.0;
			if (!TryGetCachedPinNodeOffsetY(PinWidget.Get(), PinNodeOffsetY)
				&& !TryComputeArrangedNodeOffsetY(PinGeometry->Geometry, PinNodeOffsetY))
			{
				return false;
			}

			if (bHasArrangedNodeGeometry)
			{
				const FVector2f NodeAbsolute = ArrangedNodeGeometry.GetAbsolutePosition();
				const FVector2f LocalGraphOffset = (PinPointAbsolute - NodeAbsolute) / ZoomAmount;
				OutAnchor = FVector2D(static_cast<double>(LocalGraphOffset.X), PinNodeOffsetY);
				if (IsFiniteLayoutPoint(OutAnchor))
				{
					return true;
				}
			}

			FVector2D ConvertedAnchor;
			if (TryConvertAbsolutePinPointToNodeAnchor(
				GraphPanel,
				GraphNodeWidget,
				NodeGraphPosition,
				PinPointAbsolute,
				ConvertedAnchor))
			{
				ConvertedAnchor.Y = PinNodeOffsetY;
				OutAnchor = ConvertedAnchor;
				return true;
			}
			return false;
		}

		static bool TryGetTickSpacePinAnchor(
			SGraphPanel& GraphPanel,
			SGraphNode& GraphNodeWidget,
			const TSharedRef<SGraphPin>& PinWidget,
			const FVector2D& NodeGraphPosition,
			const EEdGraphPinDirection Direction,
			FVector2D& OutAnchor)
		{
			return TryGetArrangedPinAnchor(GraphPanel, GraphNodeWidget, PinWidget, NodeGraphPosition, Direction, OutAnchor);
		}

		static void CollectSharedGraphNodePins(UEdGraphNode* GraphNode, UeAgentBlueprintAutoLayout::FNode& OutNode)
		{
			TArray<FString> InputPins;
			TArray<FString> OutputPins;
			const TArray<UEdGraphPin*> Inputs = GetSharedGraphPinsByDirection(GraphNode, EGPD_Input);
			const TArray<UEdGraphPin*> Outputs = GetSharedGraphPinsByDirection(GraphNode, EGPD_Output);
			for (int32 Index = 0; Index < Inputs.Num(); ++Index)
			{
				InputPins.Add(PinNameOrFallback(GraphPinNameString(Inputs[Index]), TEXT("Input_"), Index));
				if (IsExecLikeGraphPin(Inputs[Index]))
				{
					OutNode.bHasExecPin = true;
				}
			}
			for (int32 Index = 0; Index < Outputs.Num(); ++Index)
			{
				OutputPins.Add(PinNameOrFallback(GraphPinNameString(Outputs[Index]), TEXT("Output_"), Index));
				if (IsExecLikeGraphPin(Outputs[Index]))
				{
					OutNode.bHasExecPin = true;
				}
			}
			InitializePendingRealPins(OutNode, InputPins, OutputPins);
		}

		static FUniversalLayoutModel BuildGenericSharedLayoutModel(UEdGraph* Graph)
		{
			FUniversalLayoutModel Model;
			if (!Graph)
			{
				return Model;
			}

			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				if (!GraphNode)
				{
					continue;
				}
				if (IsSharedLayoutRerouteNode(GraphNode))
				{
					++Model.CollapsedRerouteNodeCount;
					continue;
				}

				UeAgentBlueprintAutoLayout::FNode Node;
				Node.Index = Model.Nodes.Num();
				Node.Guid = GraphNode->NodeGuid.IsValid()
					? GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
					: GraphNode->GetPathName();
				Node.Title = GraphNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
				if (Node.Title.IsEmpty())
				{
					Node.Title = GraphNode->GetName();
				}
				Node.ClassName = GraphNode->GetClass()->GetName();
				Node.OldX = GraphNode->NodePosX;
				Node.OldY = GraphNode->NodePosY;
				Node.NewX = Node.OldX;
				Node.NewY = Node.OldY;
				if (const UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(GraphNode))
				{
					Node.bIsCommentNode = true;
					Node.CommentDepth = CommentNode->CommentDepth;
				}
				CollectSharedGraphNodePins(GraphNode, Node);

				Model.GraphNodeToIndex.Add(GraphNode, Node.Index);
				FUniversalLayoutNodeRef Ref;
				Ref.GraphNode = GraphNode;
				Model.NodeRefs.Add(Ref);
				Model.Nodes.Add(Node);
			}
			Model.WritableNodeCount = Model.Nodes.Num();

			TSet<FString> EdgeKeys;
			for (UEdGraphNode* SourceNode : Graph->Nodes)
			{
				const int32* SourceIndexPtr = Model.GraphNodeToIndex.Find(SourceNode);
				if (!SourceIndexPtr)
				{
					continue;
				}
				const TArray<UEdGraphPin*> SourcePins = GetSharedGraphPinsByDirection(SourceNode, EGPD_Output);
				for (UEdGraphPin* SourcePin : SourcePins)
				{
					for (UEdGraphPin* LinkedPin : SourcePin->LinkedTo)
					{
						TArray<UEdGraphPin*> TerminalPins;
						TSet<UEdGraphPin*> VisitedPins;
						CollectTerminalSharedLinkedPins(LinkedPin, TerminalPins, VisitedPins);
						for (UEdGraphPin* TerminalPin : TerminalPins)
						{
							if (!TerminalPin || TerminalPin->Direction != EGPD_Input)
							{
								continue;
							}
							UEdGraphNode* TargetNode = TerminalPin->GetOwningNode();
							const int32* TargetIndexPtr = Model.GraphNodeToIndex.Find(TargetNode);
							if (!TargetIndexPtr)
							{
								continue;
							}

							UeAgentBlueprintAutoLayout::FEdge Edge;
							Edge.Source = *SourceIndexPtr;
							Edge.Target = *TargetIndexPtr;
							Edge.SourcePin = GraphPinNameString(SourcePin);
							Edge.TargetPin = GraphPinNameString(TerminalPin);
							Edge.SourcePinIndex = GetSharedGraphPinIndexInDirection(SourcePin);
							Edge.TargetPinIndex = GetSharedGraphPinIndexInDirection(TerminalPin);
							if (Edge.SourcePinIndex == INDEX_NONE || Edge.TargetPinIndex == INDEX_NONE)
							{
								continue;
							}
							Edge.bExec = IsExecLikeGraphPin(SourcePin) || IsExecLikeGraphPin(TerminalPin);
							const FString EdgeKey = FString::Printf(
								TEXT("%d:%d:%s:%s:%d:%d"),
								Edge.Source,
								Edge.Target,
								*Edge.SourcePin,
								*Edge.TargetPin,
								Edge.SourcePinIndex,
								Edge.TargetPinIndex);
							if (!EdgeKeys.Contains(EdgeKey))
							{
								EdgeKeys.Add(EdgeKey);
								if (TerminalPin != LinkedPin)
								{
									++Model.CollapsedRerouteEdgeCount;
								}
								Model.Edges.Add(Edge);
							}
						}
					}
				}
			}

			return Model;
		}

		static bool OpenEditorForGeometryIfNeeded(UObject* Asset, bool& bOpenedEditor)
		{
			bOpenedEditor = false;
			if (!Asset || !GEditor)
			{
				return false;
			}
			if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				if (!AssetEditorSubsystem->FindEditorForAsset(Asset, false))
				{
					if (!AssetEditorSubsystem->OpenEditorForAsset(Asset))
					{
						return false;
					}
					bOpenedEditor = true;
				}
				PumpAndTickSlateForLayoutGeometry();
				PumpAndTickSlateForLayoutGeometry();
				return true;
			}
			return false;
		}

		static bool CaptureSharedGraphRealGeometry(
			UObject* Asset,
			UEdGraph* Graph,
			FUniversalLayoutModel& Model,
			FSharedGraphGeometryStats& OutStats,
			FString& OutError,
			const bool bOpenAssetEditorForGeometry)
		{
			OutStats = FSharedGraphGeometryStats();
			OutStats.NodeCount = Model.Nodes.Num();
			OutStats.GeometrySource = TEXT("slate_graph_editor_bounds_and_arranged_pin_geometry");
			OutStats.GeometryMeasurementZoomAmount = 1.0f;

			if (!Asset || !Graph)
			{
				OutError = TEXT("asset_or_graph_missing_for_real_geometry");
				OutStats.Error = OutError;
				return false;
			}
			if (!FSlateApplication::IsInitialized())
			{
				OutError = TEXT("slate_not_initialized_for_real_geometry");
				OutStats.Error = OutError;
				return false;
			}

			bool bOpenedAssetEditor = false;
			if (bOpenAssetEditorForGeometry)
			{
				OpenEditorForGeometryIfNeeded(Asset, bOpenedAssetEditor);
			}

			TSharedPtr<SGraphEditor> OwnedGeometryGraphEditor;
			FGraphAppearanceInfo AppearanceInfo;
			AppearanceInfo.CornerText = FText::FromString(TEXT("UAI real geometry probe"));
			OwnedGeometryGraphEditor = SNew(SGraphEditor)
				.IsEditable(false)
				.DisplayAsReadOnly(true)
				.ShowGraphStateOverlay(false)
				.Appearance(AppearanceInfo)
				.GraphToEdit(Graph);
			TSharedPtr<SGraphEditor> GraphEditor = OwnedGeometryGraphEditor;
			OutStats.bOpenedEditorForGeometry = bOpenedAssetEditor;
			OutStats.bUsedOffscreenGraphEditor = true;
			PumpAndTickSlateForLayoutGeometry();
			SGraphPanel* GraphPanel = GraphEditor->GetGraphPanel();
			if (!GraphPanel)
			{
				OutError = TEXT("graph_panel_not_found_for_real_geometry");
				OutStats.Error = OutError;
				return false;
			}

			FVector2f SavedViewLocation = FVector2f::ZeroVector;
			float SavedZoom = 1.0f;
			GraphEditor->GetViewLocation(SavedViewLocation, SavedZoom);
			OutStats.RestoredViewLocation = SavedViewLocation;
			OutStats.RestoredZoomAmount = SavedZoom;

			auto RestoreGraphView = [&]()
			{
				GraphEditor->SetViewLocation(SnapViewLocationToPixelGrid(SavedViewLocation, SavedZoom), SavedZoom);
				OutStats.bRestoredGraphViewAfterCapture = true;
				PumpAndTickSlateForLayoutGeometry();
			};

			auto RefreshGraphPanelLayout = [&](const int32 FrameCount)
			{
				for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
				{
					GraphPanel->Update();
					PumpAndTickSlateForLayoutGeometry();
					const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
					GraphEditor->SlatePrepass(WidgetScale);
					GraphPanel->SlatePrepass(WidgetScale);
				}
			};

			GraphEditor->SetViewLocation(SnapViewLocationToPixelGrid(SavedViewLocation, 1.0f), 1.0f);
			for (const FUniversalLayoutNodeRef& Ref : Model.NodeRefs)
			{
				if (Ref.GraphNode)
				{
					GraphPanel->RefreshNode(*Ref.GraphNode);
				}
			}
			RefreshGraphPanelLayout(3);

			for (UeAgentBlueprintAutoLayout::FNode& Node : Model.Nodes)
			{
				Node.InputPinAnchorValid.Init(0, Node.InputPinAnchors.Num());
				Node.OutputPinAnchorValid.Init(0, Node.OutputPinAnchors.Num());
				Node.bMeasuredGeometry = false;
			}

			TArray<TSet<int32>> UsedInputPinIndices;
			TArray<TSet<int32>> UsedOutputPinIndices;
			UsedInputPinIndices.SetNum(Model.Nodes.Num());
			UsedOutputPinIndices.SetNum(Model.Nodes.Num());
			for (const UeAgentBlueprintAutoLayout::FEdge& Edge : Model.Edges)
			{
				if (UsedOutputPinIndices.IsValidIndex(Edge.Source) && Edge.SourcePinIndex != INDEX_NONE)
				{
					UsedOutputPinIndices[Edge.Source].Add(Edge.SourcePinIndex);
				}
				if (UsedInputPinIndices.IsValidIndex(Edge.Target) && Edge.TargetPinIndex != INDEX_NONE)
				{
					UsedInputPinIndices[Edge.Target].Add(Edge.TargetPinIndex);
				}
			}
			auto CountMissingUsedPinAnchors = [&](
				const int32 NodeIndex,
				const EEdGraphPinDirection Direction) -> int32
			{
				if (!Model.Nodes.IsValidIndex(NodeIndex))
				{
					return 0;
				}
				const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[NodeIndex];
				const TSet<int32>& UsedPins = Direction == EGPD_Input ? UsedInputPinIndices[NodeIndex] : UsedOutputPinIndices[NodeIndex];
				int32 Missing = 0;
				for (const int32 PinIndex : UsedPins)
				{
					FVector2D Anchor = FVector2D::ZeroVector;
					if (!TryGetMeasuredPinAnchor(Node, Direction, PinIndex, Anchor))
					{
						++Missing;
					}
				}
				return Missing;
			};

			auto CaptureNodeSize = [&](UeAgentBlueprintAutoLayout::FNode& LayoutNode, UEdGraphNode* GraphNode) -> bool
			{
				if (!GraphNode)
				{
					return false;
				}
				struct FMeasuredNodeSize
				{
					bool bValid = false;
					double Width = 0.0;
					double Height = 0.0;
				};
				auto MakeMeasuredSize = [](const double Width, const double Height)
				{
					FMeasuredNodeSize Size;
					if (FMath::IsFinite(Width) && FMath::IsFinite(Height) && Width > 1.0 && Height > 1.0)
					{
						Size.bValid = true;
						Size.Width = Width;
						Size.Height = Height;
					}
					return Size;
				};
				FMeasuredNodeSize DesiredSize;
				TSharedPtr<SGraphNode> GraphNodeWidget = GraphPanel->GetNodeWidgetFromGuid(GraphNode->NodeGuid);
				if (GraphNodeWidget.IsValid())
				{
					const FVector2D Desired = GraphNodeWidget->GetDesiredSize();
					DesiredSize = MakeMeasuredSize(Desired.X, Desired.Y);
				}
				FMeasuredNodeSize BoundsSize;
				FSlateRect Bounds;
				if (GraphEditor->GetBoundsForNode(GraphNode, Bounds, 0.0f))
				{
					BoundsSize = MakeMeasuredSize(
						static_cast<double>(Bounds.Right - Bounds.Left),
						static_cast<double>(Bounds.Bottom - Bounds.Top));
				}
				const FMeasuredNodeSize ChosenSize = DesiredSize.bValid ? DesiredSize : BoundsSize;
				if (!ChosenSize.bValid)
				{
					LayoutNode.bMeasuredGeometry = false;
					return false;
				}
				LayoutNode.Width = FMath::Clamp(ChosenSize.Width, 1.0, 5000.0);
				LayoutNode.Height = FMath::Clamp(ChosenSize.Height, 1.0, 5000.0);
				LayoutNode.bMeasuredGeometry = true;
				return true;
			};

			auto CapturePinAnchorsForNode = [&](UeAgentBlueprintAutoLayout::FNode& LayoutNode, UEdGraphNode* GraphNode, const bool bCountPinWidgets)
			{
				if (!GraphNode)
				{
					return;
				}
				TSharedPtr<SGraphNode> GraphNodeWidget = GraphPanel->GetNodeWidgetFromGuid(GraphNode->NodeGuid);
				if (!GraphNodeWidget.IsValid())
				{
					return;
				}
				for (UEdGraphPin* Pin : GraphNode->Pins)
				{
					if (!IsVisibleSharedGraphPin(Pin) || (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output))
					{
						continue;
					}
					const int32 PinIndex = GetSharedGraphPinIndexInDirection(Pin);
					if (PinIndex == INDEX_NONE)
					{
						continue;
					}
					FVector2D ExistingAnchor = FVector2D::ZeroVector;
					if (TryGetMeasuredPinAnchor(LayoutNode, Pin->Direction, PinIndex, ExistingAnchor))
					{
						continue;
					}
					TSharedPtr<SGraphPin> PinWidget = GraphNodeWidget->FindWidgetForPin(Pin);
					if (!PinWidget.IsValid())
					{
						continue;
					}
					if (bCountPinWidgets)
					{
						++OutStats.PinWidgetCount;
					}
					const FVector2f WidgetNodeGraphPosition2f = GraphNodeWidget->GetPosition2f();
					const FVector2D WidgetNodeGraphPosition(
						static_cast<double>(WidgetNodeGraphPosition2f.X),
						static_cast<double>(WidgetNodeGraphPosition2f.Y));
					FVector2D Anchor = FVector2D::ZeroVector;
					if (TryGetTickSpacePinAnchor(*GraphPanel, *GraphNodeWidget, PinWidget.ToSharedRef(), WidgetNodeGraphPosition, Pin->Direction, Anchor))
					{
						SetMeasuredPinAnchor(LayoutNode, Pin->Direction, PinIndex, Anchor);
						++OutStats.PinGeometryAnchorCount;
					}
				}
			};

			for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
			{
				UEdGraphNode* GraphNode = Model.NodeRefs.IsValidIndex(NodeIndex) ? Model.NodeRefs[NodeIndex].GraphNode : nullptr;
				if (!GraphNode)
				{
					continue;
				}
				if (CaptureNodeSize(Model.Nodes[NodeIndex], GraphNode))
				{
					++OutStats.MeasuredNodeCount;
				}
				CapturePinAnchorsForNode(Model.Nodes[NodeIndex], GraphNode, true);
			}

			constexpr double MaxFocusedCaptureSeconds = 30.0;
			constexpr int32 MaxFocusedCapturePasses = 2;
			const double FocusedStartSeconds = FPlatformTime::Seconds();
			auto CountAllMissingUsedPinAnchors = [&]()
			{
				int32 Missing = 0;
				for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
				{
					Missing += CountMissingUsedPinAnchors(NodeIndex, EGPD_Input);
					Missing += CountMissingUsedPinAnchors(NodeIndex, EGPD_Output);
				}
				return Missing;
			};
			for (int32 PassIndex = 0; PassIndex < MaxFocusedCapturePasses; ++PassIndex)
			{
				const int32 MissingAtPassStart = CountAllMissingUsedPinAnchors();
				if (MissingAtPassStart <= 0)
				{
					break;
				}
				for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
				{
					if ((FPlatformTime::Seconds() - FocusedStartSeconds) > MaxFocusedCaptureSeconds)
					{
						OutStats.bFocusedGeometryBudgetExhausted = true;
						break;
					}
					UEdGraphNode* GraphNode = Model.NodeRefs.IsValidIndex(NodeIndex) ? Model.NodeRefs[NodeIndex].GraphNode : nullptr;
					if (!GraphNode)
					{
						continue;
					}
					const int32 MissingForNode =
						CountMissingUsedPinAnchors(NodeIndex, EGPD_Input)
						+ CountMissingUsedPinAnchors(NodeIndex, EGPD_Output);
					if (MissingForNode <= 0 && Model.Nodes[NodeIndex].bMeasuredGeometry)
					{
						continue;
					}
					const FVector2f FocusedViewLocation(
						FMath::RoundToFloat(static_cast<float>(Model.Nodes[NodeIndex].OldX - 320.0)),
						FMath::RoundToFloat(static_cast<float>(Model.Nodes[NodeIndex].OldY - 220.0)));
					GraphEditor->SetViewLocation(SnapViewLocationToPixelGrid(FocusedViewLocation, 1.0f), 1.0f);
					++OutStats.FocusedGeometryNodeCount;
					RefreshGraphPanelLayout(2);
					CaptureNodeSize(Model.Nodes[NodeIndex], GraphNode);
					CapturePinAnchorsForNode(Model.Nodes[NodeIndex], GraphNode, false);
				}
				if (OutStats.bFocusedGeometryBudgetExhausted || CountAllMissingUsedPinAnchors() >= MissingAtPassStart)
				{
					break;
				}
			}

			OutStats.MissingPinAnchorCount = 0;
			OutStats.VisibleMissingPinAnchorCount = 0;
			int32 MissingNodeGeometryCount = 0;
			TArray<FString> MissingNodeGeometrySamples;
			TArray<FString> MissingUsedPinAnchorSamples;
			for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
			{
				const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[NodeIndex];
				OutStats.VisibleMissingPinAnchorCount += Node.InputPinNames.Num() - CountMeasuredPinAnchors(Node, EGPD_Input);
				OutStats.VisibleMissingPinAnchorCount += Node.OutputPinNames.Num() - CountMeasuredPinAnchors(Node, EGPD_Output);
				if (!Node.bMeasuredGeometry)
				{
					++MissingNodeGeometryCount;
					if (MissingNodeGeometrySamples.Num() < 8)
					{
						UEdGraphNode* GraphNode = Model.NodeRefs.IsValidIndex(NodeIndex) ? Model.NodeRefs[NodeIndex].GraphNode : nullptr;
						const FString GraphNodeName = GraphNode ? GraphNode->GetName() : TEXT("<null>");
						const FString ClassName = GraphNode ? GraphNode->GetClass()->GetName() : TEXT("<null>");
						MissingNodeGeometrySamples.Add(FString::Printf(
							TEXT("%d:%s:%s:%s"),
							NodeIndex,
							*Node.Title,
							*ClassName,
							*GraphNodeName));
					}
				}
			}
			for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
			{
				OutStats.MissingPinAnchorCount += CountMissingUsedPinAnchors(NodeIndex, EGPD_Input);
				OutStats.MissingPinAnchorCount += CountMissingUsedPinAnchors(NodeIndex, EGPD_Output);
				const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[NodeIndex];
				auto AddMissingUsedPinSamples = [&](const EEdGraphPinDirection Direction)
				{
					const TSet<int32>& UsedPins = Direction == EGPD_Input ? UsedInputPinIndices[NodeIndex] : UsedOutputPinIndices[NodeIndex];
					const TArray<FString>& PinNames = Direction == EGPD_Input ? Node.InputPinNames : Node.OutputPinNames;
					for (const int32 PinIndex : UsedPins)
					{
						FVector2D Anchor = FVector2D::ZeroVector;
						if (TryGetMeasuredPinAnchor(Node, Direction, PinIndex, Anchor))
						{
							continue;
						}
						if (MissingUsedPinAnchorSamples.Num() >= 12)
						{
							return;
						}
						const FString PinName = PinNames.IsValidIndex(PinIndex) ? PinNames[PinIndex] : FString::Printf(TEXT("pin_%d"), PinIndex);
						MissingUsedPinAnchorSamples.Add(FString::Printf(
							TEXT("%d:%s:%s:%d:%s"),
							NodeIndex,
							*Node.Title,
							Direction == EGPD_Input ? TEXT("input") : TEXT("output"),
							PinIndex,
							*PinName));
					}
				};
				AddMissingUsedPinSamples(EGPD_Input);
				AddMissingUsedPinSamples(EGPD_Output);
			}
			OutStats.bUsedRealGeometry = MissingNodeGeometryCount == 0 && OutStats.MissingPinAnchorCount == 0;
			OutStats.GeometrySource = OutStats.bUsedRealGeometry
				? TEXT("slate_graph_editor_bounds_and_arranged_pin_geometry")
				: TEXT("incomplete_slate_real_pin_geometry");
			if (!OutStats.bUsedRealGeometry)
			{
				OutError = FString::Printf(
					TEXT("real_geometry_incomplete nodes=%d/%d missing_node_geometry=%d pin_widgets=%d pin_geometry=%d missing_used_pin_anchors=%d missing_visible_pin_anchors=%d"),
					OutStats.MeasuredNodeCount,
					OutStats.NodeCount,
					MissingNodeGeometryCount,
					OutStats.PinWidgetCount,
					OutStats.PinGeometryAnchorCount,
					OutStats.MissingPinAnchorCount,
					OutStats.VisibleMissingPinAnchorCount);
				if (OutStats.bFocusedGeometryBudgetExhausted)
				{
					OutError += FString::Printf(TEXT(" focused_capture_budget_exhausted focused_nodes=%d"), OutStats.FocusedGeometryNodeCount);
				}
				if (MissingNodeGeometrySamples.Num() > 0)
				{
					OutError += FString::Printf(TEXT(" missing_node_samples=[%s]"), *FString::Join(MissingNodeGeometrySamples, TEXT(";")));
				}
				if (MissingUsedPinAnchorSamples.Num() > 0)
				{
					OutError += FString::Printf(TEXT(" missing_used_pin_samples=[%s]"), *FString::Join(MissingUsedPinAnchorSamples, TEXT(";")));
				}
				OutStats.Error = OutError;
			}
			else
			{
				OutError.Reset();
			}
			RestoreGraphView();
			return OutStats.bUsedRealGeometry;
		}

		static bool RunSharedBlueprintLayoutCore(
			FUniversalLayoutModel& Model,
			const TSharedPtr<FJsonObject>& Params,
			UeAgentBlueprintAutoLayout::FOptions& OutOptions,
			UeAgentBlueprintAutoLayout::FRunResult& OutRunResult,
			FString& OutError)
		{
			if (!ParseSharedLayoutOptions(Params, OutOptions, OutError))
			{
				return false;
			}
			ApplyDefaultOriginFromWritableNodes(Model, Params, OutOptions);
			if (!UeAgentBlueprintAutoLayout::RunLayout(Model.Nodes, Model.Edges, OutOptions, OutRunResult))
			{
				OutError = TEXT("shared_blueprint_layout_core_failed");
				return false;
			}
			return true;
		}

		static bool ApplySharedGraphNodePosition(UEdGraphNode* GraphNode, const int32 NewX, const int32 NewY)
		{
			if (!GraphNode)
			{
				return false;
			}
			if (URigVMEdGraphNode* RigNode = Cast<URigVMEdGraphNode>(GraphNode))
			{
				URigVMController* Controller = RigNode->GetController();
				URigVMNode* ModelNode = RigNode->GetModelNode();
				if (Controller && ModelNode)
				{
					return Controller->SetNodePosition(ModelNode, FVector2D(NewX, NewY), true, false, false);
				}
			}
			GraphNode->Modify();
			GraphNode->NodePosX = NewX;
			GraphNode->NodePosY = NewY;
			return true;
		}

		static UEdGraphPin* FindSharedLayoutGraphPin(
			const FUniversalLayoutModel& Model,
			const UeAgentBlueprintAutoLayout::FEdge& Edge,
			const bool bSource)
		{
			const int32 NodeIndex = bSource ? Edge.Source : Edge.Target;
			if (!Model.NodeRefs.IsValidIndex(NodeIndex))
			{
				return nullptr;
			}
			UEdGraphNode* GraphNode = Model.NodeRefs[NodeIndex].GraphNode;
			if (!GraphNode)
			{
				return nullptr;
			}
			const EEdGraphPinDirection Direction = bSource ? EGPD_Output : EGPD_Input;
			const int32 PinIndex = bSource ? Edge.SourcePinIndex : Edge.TargetPinIndex;
			const FString& PinName = bSource ? Edge.SourcePin : Edge.TargetPin;
			const TArray<UEdGraphPin*> Pins = GetSharedGraphPinsByDirection(GraphNode, Direction);
			if (Pins.IsValidIndex(PinIndex))
			{
				return Pins[PinIndex];
			}
			for (UEdGraphPin* Pin : Pins)
			{
				if (Pin && GraphPinNameString(Pin) == PinName)
				{
					return Pin;
				}
			}
			return nullptr;
		}

		static bool TryGetRigVMPinPathsForEdge(
			const FUniversalLayoutModel& Model,
			const UeAgentBlueprintAutoLayout::FEdge& Edge,
			FString& OutSourcePath,
			FString& OutTargetPath)
		{
			UEdGraphPin* SourceGraphPin = FindSharedLayoutGraphPin(Model, Edge, true);
			UEdGraphPin* TargetGraphPin = FindSharedLayoutGraphPin(Model, Edge, false);
			if (!SourceGraphPin || !TargetGraphPin)
			{
				return false;
			}
			const URigVMEdGraphNode* SourceNode = Cast<URigVMEdGraphNode>(SourceGraphPin->GetOwningNode());
			const URigVMEdGraphNode* TargetNode = Cast<URigVMEdGraphNode>(TargetGraphPin->GetOwningNode());
			if (!SourceNode || !TargetNode)
			{
				return false;
			}
			const URigVMPin* SourceModelPin = SourceNode->FindModelPinFromGraphPin(SourceGraphPin);
			const URigVMPin* TargetModelPin = TargetNode->FindModelPinFromGraphPin(TargetGraphPin);
			if (!SourceModelPin || !TargetModelPin)
			{
				return false;
			}
			OutSourcePath = SourceModelPin->GetPinPath();
			OutTargetPath = TargetModelPin->GetPinPath();
			return !OutSourcePath.IsEmpty() && !OutTargetPath.IsEmpty();
		}

		static FString MakeRigVMLinkPath(const FString& SourcePath, const FString& TargetPath)
		{
			return FString::Printf(TEXT("%s -> %s"), *SourcePath, *TargetPath);
		}

		static int32 CountRigVMRerouteNodes(const URigVMEdGraph* RigGraph)
		{
			const URigVMGraph* ModelGraph = RigGraph ? RigGraph->GetModel() : nullptr;
			if (!ModelGraph)
			{
				return 0;
			}
			int32 Count = 0;
			for (URigVMNode* Node : ModelGraph->GetNodes())
			{
				if (Cast<URigVMRerouteNode>(Node))
				{
					++Count;
				}
			}
			return Count;
		}

		static bool RemoveRigVMRerouteNodesAndRestoreSemanticLinks(
			URigVMEdGraph* RigGraph,
			const TArray<TPair<FString, FString>>& EdgePinPaths,
			FString& OutError)
		{
			URigVMGraph* ModelGraph = RigGraph ? RigGraph->GetModel() : nullptr;
			URigVMController* Controller = RigGraph ? RigGraph->GetController() : nullptr;
			if (!ModelGraph || !Controller)
			{
				OutError = TEXT("rigvm_graph_or_controller_unavailable");
				return false;
			}

			TArray<URigVMRerouteNode*> RerouteNodes;
			for (URigVMNode* Node : ModelGraph->GetNodes())
			{
				if (URigVMRerouteNode* RerouteNode = Cast<URigVMRerouteNode>(Node))
				{
					RerouteNodes.Add(RerouteNode);
				}
			}
			for (URigVMRerouteNode* RerouteNode : RerouteNodes)
			{
				if (RerouteNode && !Controller->RemoveNode(RerouteNode, true, false))
				{
					OutError = FString::Printf(TEXT("rigvm_remove_reroute_failed node=%s"), *RerouteNode->GetName());
					return false;
				}
			}
			for (const TPair<FString, FString>& EdgePath : EdgePinPaths)
			{
				if (ModelGraph->FindLink(MakeRigVMLinkPath(EdgePath.Key, EdgePath.Value)))
				{
					continue;
				}
				if (!Controller->AddLink(EdgePath.Key, EdgePath.Value, true, false))
				{
					OutError = FString::Printf(TEXT("rigvm_restore_semantic_link_failed link=%s"), *MakeRigVMLinkPath(EdgePath.Key, EdgePath.Value));
					return false;
				}
			}
			return true;
		}

		static FVector2D GetRigVMRerouteNodeSpacerSize()
		{
			// Matches SGraphNodeKnot::NodeSpacerSize. Layout plans store the visible knot/control-point center.
			return FVector2D(42.0, 24.0);
		}

		static FVector2D ConvertLayoutKnotControlPointToRigVMNodePosition(const FVector2D& ControlPoint)
		{
			return ControlPoint - GetRigVMRerouteNodeSpacerSize() * 0.5;
		}

		struct FRigVMRerouteControlPointAlignment
		{
			TWeakObjectPtr<URigVMRerouteNode> RerouteNode;
			FVector2D DesiredControlPoint = FVector2D::ZeroVector;
		};

		static UEdGraphPin* FindRigVMRerouteVisibleGraphPin(URigVMEdGraphNode* EdNode)
		{
			if (!EdNode)
			{
				return nullptr;
			}
			int32 InputPinIndex = INDEX_NONE;
			int32 OutputPinIndex = INDEX_NONE;
			if (EdNode->ShouldDrawNodeAsControlPointOnly(InputPinIndex, OutputPinIndex)
				&& EdNode->Pins.IsValidIndex(OutputPinIndex))
			{
				return EdNode->Pins[OutputPinIndex];
			}
			for (UEdGraphPin* Pin : EdNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output && Pin->ParentPin == nullptr)
				{
					return Pin;
				}
			}
			return nullptr;
		}

		static bool TryGetPinImageCenterAnchor(
			SGraphPanel& GraphPanel,
			SGraphNode& GraphNodeWidget,
			const TSharedRef<SGraphPin>& PinWidget,
			FVector2D& OutAnchor)
		{
			FGeometry PanelGeometry;
			if (!TryGetUsableWidgetGeometry(GraphPanel, PanelGeometry))
			{
				PanelGeometry = FGeometry::MakeRoot(FVector2f(4096.0f, 4096.0f), FSlateLayoutTransform(1.0f));
			}

			const float ZoomAmount = FMath::Max(GraphPanel.GetZoomAmount(), 0.01f);
			const float WidgetScale = FSlateApplication::IsInitialized()
				? FSlateApplication::Get().GetApplicationScale()
				: 1.0f;
			GraphNodeWidget.SlatePrepass(WidgetScale * ZoomAmount);

			TSharedRef<SWidget> AnchorWidget = StaticCastSharedRef<SWidget>(PinWidget);
			if (TSharedPtr<SWidget> PinImage = PinWidget->GetPinImageWidget())
			{
				AnchorWidget = PinImage.ToSharedRef();
			}

			TSet<TSharedRef<SWidget>> WidgetsToFind;
			WidgetsToFind.Add(AnchorWidget);

			TMap<TSharedRef<SWidget>, FArrangedWidget> ArrangedWidgets;
			const auto ViewOffset = GraphPanel.GetViewOffset();
			const FVector2f NodePanelOffset(
				GraphNodeWidget.GetPosition2f().X - static_cast<float>(ViewOffset.X),
				GraphNodeWidget.GetPosition2f().Y - static_cast<float>(ViewOffset.Y));
			const FArrangedWidget ArrangedNode = PanelGeometry.MakeChild(
				GraphNodeWidget.AsShared(),
				NodePanelOffset,
				GraphNodeWidget.GetDesiredSize(),
				ZoomAmount);
			FindArrangedDescendantGeometries(GraphNodeWidget, ArrangedNode.Geometry, WidgetsToFind, ArrangedWidgets);
			if (ArrangedWidgets.Num() < WidgetsToFind.Num())
			{
				FindArrangedDescendantGeometries(GraphPanel, PanelGeometry, WidgetsToFind, ArrangedWidgets);
			}

			const FArrangedWidget* AnchorGeometry = ArrangedWidgets.Find(AnchorWidget);
			FGeometry FallbackAnchorGeometry;
			if (!AnchorGeometry && TryGetUsableWidgetGeometry(AnchorWidget.Get(), FallbackAnchorGeometry))
			{
				ArrangedWidgets.Add(AnchorWidget, FArrangedWidget(AnchorWidget, FallbackAnchorGeometry));
				AnchorGeometry = ArrangedWidgets.Find(AnchorWidget);
			}
			if (!AnchorGeometry)
			{
				return false;
			}

			const FVector2f AnchorCenterAbsolute =
				AnchorGeometry->Geometry.AbsolutePosition + AnchorGeometry->Geometry.GetDrawSize() * 0.5f;
			const FVector2f NodeAbsolute = ArrangedNode.Geometry.GetAbsolutePosition();
			const FVector2f LocalGraphOffset = (AnchorCenterAbsolute - NodeAbsolute) / ZoomAmount;
			OutAnchor = FVector2D(static_cast<double>(LocalGraphOffset.X), static_cast<double>(LocalGraphOffset.Y));
			return IsFiniteLayoutPoint(OutAnchor);
		}

		static bool TryMeasureRigVMRerouteControlPoint(
			URigVMEdGraph* RigGraph,
			SGraphPanel& GraphPanel,
			URigVMRerouteNode* RerouteNode,
			FVector2D& OutControlPoint)
		{
			if (!RigGraph || !RerouteNode)
			{
				return false;
			}
			URigVMEdGraphNode* EdNode = Cast<URigVMEdGraphNode>(
				RigGraph->FindNodeForModelNodeName(RerouteNode->GetFName()));
			if (!EdNode)
			{
				return false;
			}
			TSharedPtr<SGraphNode> GraphNodeWidget = GraphPanel.GetNodeWidgetFromGuid(EdNode->NodeGuid);
			if (!GraphNodeWidget.IsValid())
			{
				GraphPanel.RefreshNode(*EdNode);
				PumpAndTickSlateForLayoutGeometry();
				GraphNodeWidget = GraphPanel.GetNodeWidgetFromGuid(EdNode->NodeGuid);
			}
			if (!GraphNodeWidget.IsValid())
			{
				return false;
			}
			UEdGraphPin* VisiblePin = FindRigVMRerouteVisibleGraphPin(EdNode);
			if (!VisiblePin)
			{
				return false;
			}
			TSharedPtr<SGraphPin> PinWidget = GraphNodeWidget->FindWidgetForPin(VisiblePin);
			if (!PinWidget.IsValid())
			{
				return false;
			}
			FVector2D Anchor = FVector2D::ZeroVector;
			if (!TryGetPinImageCenterAnchor(GraphPanel, *GraphNodeWidget, PinWidget.ToSharedRef(), Anchor))
			{
				return false;
			}
			OutControlPoint = RerouteNode->GetPosition() + Anchor;
			return IsFiniteLayoutPoint(OutControlPoint);
		}

		static bool AlignRigVMRerouteControlPointsToSlateGeometry(
			URigVMEdGraph* RigGraph,
			URigVMController* Controller,
			const TArray<FRigVMRerouteControlPointAlignment>& Alignments,
			FString& OutError)
		{
			if (Alignments.Num() == 0)
			{
				return true;
			}
			if (!RigGraph || !Controller)
			{
				OutError = TEXT("rigvm_reroute_alignment_graph_or_controller_unavailable");
				return false;
			}
			if (!FSlateApplication::IsInitialized())
			{
				OutError = TEXT("rigvm_reroute_alignment_slate_not_initialized");
				return false;
			}

			FGraphAppearanceInfo AppearanceInfo;
			AppearanceInfo.CornerText = FText::FromString(TEXT("UAI RigVM reroute alignment probe"));
			TSharedPtr<SGraphEditor> GraphEditor = SNew(SGraphEditor)
				.IsEditable(false)
				.DisplayAsReadOnly(true)
				.ShowGraphStateOverlay(false)
				.Appearance(AppearanceInfo)
				.GraphToEdit(RigGraph);
			PumpAndTickSlateForLayoutGeometry();
			SGraphPanel* GraphPanel = GraphEditor.IsValid() ? GraphEditor->GetGraphPanel() : nullptr;
			if (!GraphPanel)
			{
				OutError = TEXT("rigvm_reroute_alignment_graph_panel_unavailable");
				return false;
			}

			auto RefreshLayout = [&]()
			{
				GraphPanel->Update();
				PumpAndTickSlateForLayoutGeometry();
				const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
				GraphEditor->SlatePrepass(WidgetScale);
				GraphPanel->SlatePrepass(WidgetScale);
			};

			for (const FRigVMRerouteControlPointAlignment& Alignment : Alignments)
			{
				URigVMRerouteNode* RerouteNode = Alignment.RerouteNode.Get();
				if (!RerouteNode)
				{
					OutError = TEXT("rigvm_reroute_alignment_node_lost");
					return false;
				}
				if (!IsFiniteLayoutPoint(Alignment.DesiredControlPoint))
				{
					OutError = FString::Printf(TEXT("rigvm_reroute_alignment_invalid_target node=%s"), *RerouteNode->GetName());
					return false;
				}

				URigVMEdGraphNode* EdNode = Cast<URigVMEdGraphNode>(
					RigGraph->FindNodeForModelNodeName(RerouteNode->GetFName()));
				if (!EdNode)
				{
					OutError = FString::Printf(TEXT("rigvm_reroute_alignment_ednode_not_found node=%s"), *RerouteNode->GetName());
					return false;
				}
				const FVector2D CurrentPosition = RerouteNode->GetPosition();
				GraphEditor->SetViewLocation(
					SnapViewLocationToPixelGrid(
						FVector2f(
							FMath::RoundToFloat(static_cast<float>(CurrentPosition.X - 256.0)),
							FMath::RoundToFloat(static_cast<float>(CurrentPosition.Y - 192.0))),
						1.0f),
					1.0f);
				GraphPanel->RefreshNode(*EdNode);
				RefreshLayout();
				RefreshLayout();

				FVector2D MeasuredControlPoint = FVector2D::ZeroVector;
				if (!TryMeasureRigVMRerouteControlPoint(RigGraph, *GraphPanel, RerouteNode, MeasuredControlPoint))
				{
					OutError = FString::Printf(TEXT("rigvm_reroute_alignment_measure_failed node=%s"), *RerouteNode->GetName());
					return false;
				}
				const FVector2D Delta = Alignment.DesiredControlPoint - MeasuredControlPoint;
				if (FMath::Abs(Delta.X) > 0.01 || FMath::Abs(Delta.Y) > 0.01)
				{
					if (!Controller->SetNodePosition(RerouteNode, CurrentPosition + Delta, true, true, false))
					{
						OutError = FString::Printf(TEXT("rigvm_reroute_alignment_move_failed node=%s"), *RerouteNode->GetName());
						return false;
					}
				}
			}

			double MaxResidual = 0.0;
			FString MaxResidualNodeName;
			FVector2D MaxResidualDesired = FVector2D::ZeroVector;
			FVector2D MaxResidualMeasured = FVector2D::ZeroVector;
			for (const FRigVMRerouteControlPointAlignment& Alignment : Alignments)
			{
				URigVMRerouteNode* RerouteNode = Alignment.RerouteNode.Get();
				if (!RerouteNode)
				{
					OutError = TEXT("rigvm_reroute_alignment_verify_node_lost");
					return false;
				}
				URigVMEdGraphNode* EdNode = Cast<URigVMEdGraphNode>(
					RigGraph->FindNodeForModelNodeName(RerouteNode->GetFName()));
				if (!EdNode)
				{
					OutError = FString::Printf(TEXT("rigvm_reroute_alignment_verify_ednode_not_found node=%s"), *RerouteNode->GetName());
					return false;
				}
				const FVector2D CurrentPosition = RerouteNode->GetPosition();
				GraphEditor->SetViewLocation(
					SnapViewLocationToPixelGrid(
						FVector2f(
							FMath::RoundToFloat(static_cast<float>(CurrentPosition.X - 256.0)),
							FMath::RoundToFloat(static_cast<float>(CurrentPosition.Y - 192.0))),
						1.0f),
					1.0f);
				GraphPanel->RefreshNode(*EdNode);
				RefreshLayout();

				FVector2D MeasuredControlPoint = FVector2D::ZeroVector;
				if (!TryMeasureRigVMRerouteControlPoint(RigGraph, *GraphPanel, RerouteNode, MeasuredControlPoint))
				{
					OutError = FString::Printf(TEXT("rigvm_reroute_alignment_verify_measure_failed node=%s"), *RerouteNode->GetName());
					return false;
				}
				const double Residual = FVector2D::Distance(Alignment.DesiredControlPoint, MeasuredControlPoint);
				if (Residual > MaxResidual)
				{
					MaxResidual = Residual;
					MaxResidualNodeName = RerouteNode->GetName();
					MaxResidualDesired = Alignment.DesiredControlPoint;
					MaxResidualMeasured = MeasuredControlPoint;
				}
			}
			if (MaxResidual > 0.5)
			{
				OutError = FString::Printf(
					TEXT("rigvm_reroute_alignment_residual_too_large max_residual=%.3f node=%s desired=(%.3f,%.3f) measured=(%.3f,%.3f)"),
					MaxResidual,
					*MaxResidualNodeName,
					MaxResidualDesired.X,
					MaxResidualDesired.Y,
					MaxResidualMeasured.X,
					MaxResidualMeasured.Y);
				return false;
			}
			return true;
		}

		struct FRigVMSharedReroutePoint
		{
			TWeakObjectPtr<URigVMRerouteNode> RerouteNode;
			FString ValuePinPath;
		};

		static FString MakeRigVMSharedRerouteKey(
			const UeAgentBlueprintAutoLayout::FReroutePlan& Plan,
			const FVector2D& Point)
		{
			return UeAgentBlueprintAutoLayout::MakeRerouteAttachmentPointMergeKey(
				Plan.AttachmentMergeGroup,
				Point);
		}

		static bool EnsureRigVMLink(
			URigVMGraph* ModelGraph,
			URigVMController* Controller,
			const FString& SourcePath,
			const FString& TargetPath,
			const TCHAR* Context,
			FString& OutError)
		{
			if (!ModelGraph || !Controller || SourcePath.IsEmpty() || TargetPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("rigvm_%s_link_context_invalid"), Context ? Context : TEXT("ensure"));
				return false;
			}
			if (ModelGraph->FindLink(MakeRigVMLinkPath(SourcePath, TargetPath)))
			{
				return true;
			}
			if (!Controller->AddLink(SourcePath, TargetPath, true, false))
			{
				OutError = FString::Printf(
					TEXT("rigvm_%s_add_link_failed link=%s"),
					Context ? Context : TEXT("ensure"),
					*MakeRigVMLinkPath(SourcePath, TargetPath));
				return false;
			}
			return true;
		}

		static bool BreakRigVMLinkIfPresent(
			URigVMGraph* ModelGraph,
			URigVMController* Controller,
			const FString& SourcePath,
			const FString& TargetPath,
			const TCHAR* Context,
			FString& OutError)
		{
			if (!ModelGraph || !Controller || SourcePath.IsEmpty() || TargetPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("rigvm_%s_link_context_invalid"), Context ? Context : TEXT("break"));
				return false;
			}
			if (!ModelGraph->FindLink(MakeRigVMLinkPath(SourcePath, TargetPath)))
			{
				return true;
			}
			if (!Controller->BreakLink(SourcePath, TargetPath, true, false))
			{
				OutError = FString::Printf(
					TEXT("rigvm_%s_break_link_failed link=%s"),
					Context ? Context : TEXT("break"),
					*MakeRigVMLinkPath(SourcePath, TargetPath));
				return false;
			}
			return true;
		}

		static int32 ApplyRigVMReroutePlans(
			URigVMEdGraph* RigGraph,
			const FUniversalLayoutModel& Model,
			const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans,
			const bool bReplaceExistingReroutes,
			FString& OutError)
		{
			URigVMGraph* ModelGraph = RigGraph ? RigGraph->GetModel() : nullptr;
			URigVMController* Controller = RigGraph ? RigGraph->GetController() : nullptr;
			if (!ModelGraph || !Controller)
			{
				OutError = TEXT("rigvm_graph_or_controller_unavailable");
				return 0;
			}

			TArray<TPair<FString, FString>> EdgePinPaths;
			EdgePinPaths.SetNum(Model.Edges.Num());
			for (int32 EdgeIndex = 0; EdgeIndex < Model.Edges.Num(); ++EdgeIndex)
			{
				FString SourcePath;
				FString TargetPath;
				if (!TryGetRigVMPinPathsForEdge(Model, Model.Edges[EdgeIndex], SourcePath, TargetPath))
				{
					OutError = FString::Printf(TEXT("rigvm_edge_pin_path_unavailable edge=%d"), EdgeIndex);
					return 0;
				}
				EdgePinPaths[EdgeIndex] = TPair<FString, FString>(SourcePath, TargetPath);
			}

			if (bReplaceExistingReroutes && !RemoveRigVMRerouteNodesAndRestoreSemanticLinks(RigGraph, EdgePinPaths, OutError))
			{
				return 0;
			}

			int32 CreatedCount = 0;
			TArray<FRigVMRerouteControlPointAlignment> CreatedAlignments;
			TMap<FString, FRigVMSharedReroutePoint> SharedRerouteByMergeKey;
			for (const UeAgentBlueprintAutoLayout::FReroutePlan& Plan : Plans)
			{
				if (Plan.EdgeIndex == INDEX_NONE || !EdgePinPaths.IsValidIndex(Plan.EdgeIndex))
				{
					continue;
				}
				const TArray<FVector2D>& WritePoints = UeAgentBlueprintAutoLayout::GetAuthoritativeReroutePoints(Plan);
				if (WritePoints.Num() == 0)
				{
					continue;
				}

				FString CurrentSourcePath = EdgePinPaths[Plan.EdgeIndex].Key;
				const FString TargetPath = EdgePinPaths[Plan.EdgeIndex].Value;
				if (!EnsureRigVMLink(ModelGraph, Controller, CurrentSourcePath, TargetPath, TEXT("restore_direct"), OutError))
				{
					return CreatedCount;
				}

				for (int32 PointIndex = 0; PointIndex < WritePoints.Num(); ++PointIndex)
				{
					const FVector2D& Point = WritePoints[PointIndex];
					const FString SharedKey = MakeRigVMSharedRerouteKey(Plan, Point);
					if (!SharedKey.IsEmpty())
					{
						if (FRigVMSharedReroutePoint* ExistingSharedPoint = SharedRerouteByMergeKey.Find(SharedKey))
						{
							URigVMRerouteNode* ExistingRerouteNode = ExistingSharedPoint->RerouteNode.Get();
							if (!ExistingRerouteNode || ExistingSharedPoint->ValuePinPath.IsEmpty())
							{
								OutError = FString::Printf(TEXT("rigvm_shared_reroute_lost key=%s"), *SharedKey);
								return CreatedCount;
							}
							if (!BreakRigVMLinkIfPresent(ModelGraph, Controller, CurrentSourcePath, TargetPath, TEXT("shared_reroute_direct"), OutError)
								|| !EnsureRigVMLink(ModelGraph, Controller, CurrentSourcePath, ExistingSharedPoint->ValuePinPath, TEXT("shared_reroute"), OutError)
								|| !EnsureRigVMLink(ModelGraph, Controller, ExistingSharedPoint->ValuePinPath, TargetPath, TEXT("shared_reroute_tail"), OutError))
							{
								return CreatedCount;
							}
							CurrentSourcePath = ExistingSharedPoint->ValuePinPath;
							continue;
						}
					}

					URigVMLink* Link = ModelGraph->FindLink(MakeRigVMLinkPath(CurrentSourcePath, TargetPath));
					if (!Link)
					{
						OutError = FString::Printf(TEXT("rigvm_reroute_link_not_found link=%s"), *MakeRigVMLinkPath(CurrentSourcePath, TargetPath));
						return CreatedCount;
					}
					const FVector2D RigVMNodePosition = ConvertLayoutKnotControlPointToRigVMNodePosition(Point);
					URigVMRerouteNode* RerouteNode = Controller->AddRerouteNodeOnLink(Link, RigVMNodePosition, FString(), true, false);
					if (!RerouteNode)
					{
						OutError = FString::Printf(TEXT("rigvm_add_reroute_failed link=%s"), *MakeRigVMLinkPath(CurrentSourcePath, TargetPath));
						return CreatedCount;
					}
					URigVMPin* ValuePin = RerouteNode->FindPin(TEXT("Value"));
					if (!ValuePin)
					{
						OutError = FString::Printf(TEXT("rigvm_reroute_value_pin_missing node=%s"), *RerouteNode->GetName());
						return CreatedCount;
					}
					FRigVMRerouteControlPointAlignment Alignment;
					Alignment.RerouteNode = RerouteNode;
					Alignment.DesiredControlPoint = Point;
					CreatedAlignments.Add(Alignment);
					CurrentSourcePath = ValuePin->GetPinPath();
					if (!SharedKey.IsEmpty())
					{
						FRigVMSharedReroutePoint SharedPoint;
						SharedPoint.RerouteNode = RerouteNode;
						SharedPoint.ValuePinPath = CurrentSourcePath;
						SharedRerouteByMergeKey.Add(SharedKey, SharedPoint);
					}
					++CreatedCount;
				}
				if (!EnsureRigVMLink(ModelGraph, Controller, CurrentSourcePath, TargetPath, TEXT("reroute_tail"), OutError))
				{
					return CreatedCount;
				}
			}
			if (!AlignRigVMRerouteControlPointsToSlateGeometry(RigGraph, Controller, CreatedAlignments, OutError))
			{
				return CreatedCount;
			}
			return CreatedCount;
		}

		static int32 CountSharedPlannedRerouteKnotNodes(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans)
		{
			int32 Count = 0;
			TSet<FString> SharedKeys;
			for (const UeAgentBlueprintAutoLayout::FReroutePlan& Plan : Plans)
			{
				const TArray<FVector2D>& WritePoints = UeAgentBlueprintAutoLayout::GetAuthoritativeReroutePoints(Plan);
				for (int32 PointIndex = 0; PointIndex < WritePoints.Num(); ++PointIndex)
				{
					const FString SharedKey = MakeRigVMSharedRerouteKey(Plan, WritePoints[PointIndex]);
					if (!SharedKey.IsEmpty())
					{
						if (SharedKeys.Contains(SharedKey))
						{
							continue;
						}
						SharedKeys.Add(SharedKey);
					}
					++Count;
				}
			}
			return Count;
		}

		static UMaterialExpression* FindOriginalFunctionExpressionForPreview(
			UMaterialFunction* OriginalFunction,
			const UMaterialExpression* PreviewExpression)
		{
			if (!OriginalFunction || !PreviewExpression)
			{
				return nullptr;
			}

			const FGuid PreviewGuid = PreviewExpression->MaterialExpressionGuid;
			UMaterialExpression* FirstClassMatch = nullptr;
			for (UMaterialExpression* OriginalExpression : OriginalFunction->GetExpressions())
			{
				if (!OriginalExpression)
				{
					continue;
				}
				if (OriginalExpression->GetClass() == PreviewExpression->GetClass() && !FirstClassMatch)
				{
					FirstClassMatch = OriginalExpression;
				}
				if (PreviewGuid.IsValid() && OriginalExpression->MaterialExpressionGuid == PreviewGuid)
				{
					if (OriginalExpression->GetClass() == PreviewExpression->GetClass())
					{
						return OriginalExpression;
					}
				}
			}

			return PreviewGuid.IsValid() ? nullptr : FirstClassMatch;
		}

		static bool BuildMaterialFunctionExpressionWritebackMap(
			UMaterialFunction* OriginalFunction,
			const FUniversalLayoutModel& Model,
			TMap<UEdGraphNode*, UMaterialExpression*>& OutWritebackMap,
			FString& OutError)
		{
			OutWritebackMap.Reset();
			if (!OriginalFunction)
			{
				return true;
			}

			for (const FUniversalLayoutNodeRef& Ref : Model.NodeRefs)
			{
				if (Ref.bVirtual || !Ref.GraphNode)
				{
					continue;
				}
				UMaterialGraphNode* MaterialGraphNode = Cast<UMaterialGraphNode>(Ref.GraphNode);
				if (!MaterialGraphNode || !MaterialGraphNode->MaterialExpression)
				{
					continue;
				}

				UMaterialExpression* OriginalExpression =
					FindOriginalFunctionExpressionForPreview(OriginalFunction, MaterialGraphNode->MaterialExpression);
				if (!OriginalExpression)
				{
					OutError = FString::Printf(
						TEXT("material_function_preview_expression_writeback_failed graph_node=%s expression=%s class=%s guid=%s"),
						*Ref.GraphNode->GetName(),
						*MaterialGraphNode->MaterialExpression->GetName(),
						*MaterialGraphNode->MaterialExpression->GetClass()->GetName(),
						*MaterialGraphNode->MaterialExpression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphens));
					return false;
				}
				OutWritebackMap.Add(Ref.GraphNode, OriginalExpression);
			}

			return true;
		}

		static bool LayoutMaterialExpressionGraph(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutData,
			FString& OutError)
		{
			UMaterial* Material = Cast<UMaterial>(Asset);
			UMaterialFunctionInterface* Function = Cast<UMaterialFunctionInterface>(Asset);
			if (!Material && !Function)
			{
				OutError = TEXT("material_graph_asset_required");
				return false;
			}

			bool bSaveAfterLayout = false;
			bool bCompileAfterLayout = false;
			bool bInsertRerouteNodes = false;
			bool bReplaceExistingRerouteNodes = false;
			TryGetBoolFromEither(Params, nullptr, TEXT("save_after_layout"), bSaveAfterLayout);
			TryGetBoolFromEither(Params, nullptr, TEXT("compile_after_layout"), bCompileAfterLayout);
			TryGetLayoutBool(Params, TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
			TryGetLayoutBool(Params, TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);

			UMaterialGraph* Graph = ResolveMaterialSharedGraph(Asset, OutError);
			if (!Graph)
			{
				return false;
			}
			FUniversalLayoutModel Model = BuildGenericSharedLayoutModel(Graph);
			FSharedGraphGeometryStats GeometryStats;
			if (!CaptureSharedGraphRealGeometry(Asset, Graph, Model, GeometryStats, OutError, false))
			{
				return false;
			}
			UeAgentBlueprintAutoLayout::FOptions Options;
			UeAgentBlueprintAutoLayout::FRunResult RunResult;
			if (!RunSharedBlueprintLayoutCore(Model, Params, Options, RunResult, OutError))
			{
				return false;
			}
			const int32 PlannedRerouteKnotNodeCount = CountSharedPlannedRerouteKnotNodes(RunResult.ReroutePlans);
			if ((bInsertRerouteNodes || bReplaceExistingRerouteNodes) && PlannedRerouteKnotNodeCount > 0)
			{
				OutError = TEXT("material_graph_reroute_writeback_unsupported_for_adapter");
				return false;
			}
			TMap<UEdGraphNode*, UMaterialExpression*> MaterialFunctionWritebackMap;
			UMaterialFunction* OriginalMaterialFunction = Cast<UMaterialFunction>(Function);
			if (OriginalMaterialFunction
				&& !BuildMaterialFunctionExpressionWritebackMap(
					OriginalMaterialFunction,
					Model,
					MaterialFunctionWritebackMap,
					OutError))
			{
				return false;
			}

			int32 MovedCount = 0;
			TArray<TSharedPtr<FJsonValue>> NodeResults;
			TSet<UEdGraphNode*> NodesNeedingWrite;
			const int32 NodeCount = FMath::Min(Model.Nodes.Num(), Model.NodeRefs.Num());
			for (int32 Index = 0; Index < NodeCount; ++Index)
			{
				UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[Index];
				const FUniversalLayoutNodeRef& Ref = Model.NodeRefs[Index];
				const int32 NewX = FMath::RoundToInt(Node.NewX);
				const int32 NewY = FMath::RoundToInt(Node.NewY);
				if (!Ref.bVirtual && Ref.GraphNode)
				{
					bool bNeedsWrite = Ref.GraphNode->NodePosX != NewX || Ref.GraphNode->NodePosY != NewY;
					if (UMaterialGraphNode* MaterialGraphNode = Cast<UMaterialGraphNode>(Ref.GraphNode))
					{
						if (UMaterialExpression* PreviewExpression = MaterialGraphNode->MaterialExpression)
						{
							bNeedsWrite |= PreviewExpression->MaterialExpressionEditorX != NewX
								|| PreviewExpression->MaterialExpressionEditorY != NewY;
						}
					}
					if (UMaterialExpression** OriginalExpressionPtr = MaterialFunctionWritebackMap.Find(Ref.GraphNode))
					{
						if (UMaterialExpression* OriginalExpression = *OriginalExpressionPtr)
						{
							bNeedsWrite |= OriginalExpression->MaterialExpressionEditorX != NewX
								|| OriginalExpression->MaterialExpressionEditorY != NewY;
						}
					}
					if (bNeedsWrite)
					{
						++MovedCount;
						NodesNeedingWrite.Add(Ref.GraphNode);
					}
				}
				NodeResults.Add(MakeShared<FJsonValueObject>(SharedLayoutNodeToJson(Node, Ref)));
			}

			bool bSaved = false;
			bool bCompiled = false;
			FString CompileSkippedReason;
			TUniquePtr<FScopedTransaction> LayoutTransaction;
			if (MovedCount > 0)
			{
				LayoutTransaction = MakeUnique<FScopedTransaction>(
					NSLOCTEXT("UeAgentInterface", "NodeGraphLayoutMaterialTransaction", "UeAgentInterface Node Graph Layout"));
				Graph->Modify();
				Asset->Modify();
				for (int32 Index = 0; Index < NodeCount; ++Index)
				{
					const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[Index];
					const FUniversalLayoutNodeRef& Ref = Model.NodeRefs[Index];
					if (!Ref.bVirtual && Ref.GraphNode)
					{
						if (!NodesNeedingWrite.Contains(Ref.GraphNode))
						{
							continue;
						}
						const int32 NewX = FMath::RoundToInt(Node.NewX);
						const int32 NewY = FMath::RoundToInt(Node.NewY);
						Ref.GraphNode->Modify();
						Ref.GraphNode->NodePosX = NewX;
						Ref.GraphNode->NodePosY = NewY;
						if (UMaterialGraphNode* MaterialGraphNode = Cast<UMaterialGraphNode>(Ref.GraphNode))
						{
							if (UMaterialExpression* PreviewExpression = MaterialGraphNode->MaterialExpression)
							{
								PreviewExpression->Modify();
								PreviewExpression->MaterialExpressionEditorX = NewX;
								PreviewExpression->MaterialExpressionEditorY = NewY;
							}
						}
						if (UMaterialExpression** OriginalExpressionPtr = MaterialFunctionWritebackMap.Find(Ref.GraphNode))
						{
							UMaterialExpression* OriginalExpression = *OriginalExpressionPtr;
							if (OriginalExpression)
							{
								OriginalExpression->Modify();
								OriginalExpression->MaterialExpressionEditorX = NewX;
								OriginalExpression->MaterialExpressionEditorY = NewY;
							}
						}
					}
				}
				if (!OriginalMaterialFunction)
				{
					Graph->LinkMaterialExpressionsFromGraph();
					if (Material && Graph->Material && Graph->Material != Material)
					{
						Material->AssignExpressionCollection(Graph->Material->GetExpressionCollection());
					}
				}
				Graph->NotifyGraphChanged();
				Asset->MarkPackageDirty();
				Asset->PostEditChange();
				if (bCompileAfterLayout)
				{
					if (Material)
					{
						UMaterialEditingLibrary::RecompileMaterial(Material);
						bCompiled = true;
					}
					else
					{
						CompileSkippedReason = TEXT("material_function_compile_not_supported_by_node_graph_layout");
					}
				}
				if (bSaveAfterLayout && !SaveAssetPackage(Asset, OutError))
				{
					if (LayoutTransaction)
					{
						LayoutTransaction->Cancel();
					}
					return false;
				}
				bSaved = bSaveAfterLayout;
			}

			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("adapter"), AdapterForAsset(Asset));
			OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
			OutData->SetStringField(TEXT("graph_path"), Graph->GetPathName());
			AddSharedLayoutRunFields(OutData, RunResult);
			AddSharedGeometryFields(OutData, GeometryStats);
			OutData->SetBoolField(TEXT("changed"), MovedCount > 0);
			OutData->SetBoolField(TEXT("saved"), bSaved);
			OutData->SetBoolField(TEXT("compiled"), bCompiled);
			if (!CompileSkippedReason.IsEmpty())
			{
				OutData->SetStringField(TEXT("compile_skipped_reason"), CompileSkippedReason);
			}
			OutData->SetNumberField(TEXT("output_access_offset_x"), Options.OutputAccessOffsetX);
			OutData->SetNumberField(TEXT("input_access_offset_x"), Options.InputAccessOffsetX);
			OutData->SetStringField(TEXT("access_offset_source"), Options.AccessOffsetSource);
			OutData->SetBoolField(TEXT("reroute_writeback_supported"), false);
			OutData->SetBoolField(TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
			OutData->SetBoolField(TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
			OutData->SetNumberField(TEXT("planned_reroute_knot_node_count"), PlannedRerouteKnotNodeCount);
			OutData->SetNumberField(TEXT("node_count"), Model.WritableNodeCount);
			OutData->SetNumberField(TEXT("virtual_node_count"), Model.Nodes.Num() - Model.WritableNodeCount);
			OutData->SetNumberField(TEXT("edge_count"), Model.Edges.Num());
			OutData->SetNumberField(TEXT("collapsed_reroute_node_count"), Model.CollapsedRerouteNodeCount);
			OutData->SetNumberField(TEXT("collapsed_reroute_edge_count"), Model.CollapsedRerouteEdgeCount);
			OutData->SetNumberField(TEXT("moved_node_count"), MovedCount);
			OutData->SetArrayField(TEXT("nodes"), NodeResults);
			OutData->SetArrayField(TEXT("edges"), SharedLayoutEdgesToJson(Model));
			return true;
		}

		static bool LayoutGenericEdGraph(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutData,
			FString& OutError)
		{
			FString GraphRef;
			TSharedPtr<FJsonObject> Selector;
			if (TryGetObjectField(Params, TEXT("graph_selector"), Selector))
			{
				TryGetStringFromEither(Selector, nullptr, TEXT("graph_path"), GraphRef);
				if (GraphRef.IsEmpty())
				{
					TryGetStringFromEither(Selector, nullptr, TEXT("graph_name"), GraphRef);
				}
			}
			if (GraphRef.IsEmpty())
			{
				TryGetStringFromEither(Params, nullptr, TEXT("graph_path"), GraphRef);
			}
			if (GraphRef.IsEmpty())
			{
				TryGetStringFromEither(Params, nullptr, TEXT("graph_name"), GraphRef);
			}

			UEdGraph* Graph = ResolveGenericGraph(Asset, GraphRef);
			if (!Graph)
			{
				OutError = TEXT("graph_not_found");
				return false;
			}
			if (IsGraphLikelyStateMachineTopLevel(Graph))
			{
				OutError = TEXT("unsupported_non_pin_node_graph_or_vertical_state_graph");
				return false;
			}

			bool bSaveAfterLayout = false;
			TryGetBoolFromEither(Params, nullptr, TEXT("save_after_layout"), bSaveAfterLayout);
			bool bInsertRerouteNodes = false;
			bool bReplaceExistingRerouteNodes = false;
			bool bAllowRemoveAllRerouteNodes = false;
			TryGetLayoutBool(Params, TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
			TryGetLayoutBool(Params, TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
			TryGetLayoutBool(Params, TEXT("allow_remove_all_reroute_nodes"), bAllowRemoveAllRerouteNodes);
			const bool bEffectiveReplaceExistingRerouteNodes = bReplaceExistingRerouteNodes || bInsertRerouteNodes;
			URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph);
			const bool bRerouteWritebackSupported = RigGraph != nullptr;

			FUniversalLayoutModel Model = BuildGenericSharedLayoutModel(Graph);
			FSharedGraphGeometryStats GeometryStats;
			if (!CaptureSharedGraphRealGeometry(Asset, Graph, Model, GeometryStats, OutError, true))
			{
				return false;
			}
			UeAgentBlueprintAutoLayout::FOptions Options;
			UeAgentBlueprintAutoLayout::FRunResult RunResult;
			if (!RunSharedBlueprintLayoutCore(Model, Params, Options, RunResult, OutError))
			{
				return false;
			}
			const int32 ExistingRerouteNodeCount = RigGraph ? CountRigVMRerouteNodes(RigGraph) : 0;
			const int32 PlannedRerouteKnotNodeCount = CountSharedPlannedRerouteKnotNodes(RunResult.ReroutePlans);
			if ((bInsertRerouteNodes || bReplaceExistingRerouteNodes)
				&& !bRerouteWritebackSupported
				&& PlannedRerouteKnotNodeCount > 0)
			{
				OutError = TEXT("generic_edgraph_reroute_writeback_unsupported_for_adapter");
				return false;
			}
			if (bEffectiveReplaceExistingRerouteNodes
				&& ExistingRerouteNodeCount > 0
				&& PlannedRerouteKnotNodeCount == 0
				&& !bAllowRemoveAllRerouteNodes)
			{
				OutError = FString::Printf(
					TEXT("reroute_delete_all_guard_blocked existing_reroute_nodes=%d planned_reroute_knots=0; pass allow_remove_all_reroute_nodes=true only when intentionally flattening every existing reroute"),
					ExistingRerouteNodeCount);
				return false;
			}

			int32 MovedCount = 0;
			TArray<TSharedPtr<FJsonValue>> NodeResults;
			const int32 NodeCount = FMath::Min(Model.Nodes.Num(), Model.NodeRefs.Num());
			for (int32 Index = 0; Index < NodeCount; ++Index)
			{
				UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[Index];
				const FUniversalLayoutNodeRef& Ref = Model.NodeRefs[Index];
				const int32 NewX = FMath::RoundToInt(Node.NewX);
				const int32 NewY = FMath::RoundToInt(Node.NewY);
				if (!Ref.bVirtual && Ref.GraphNode)
				{
					const int32 OldX = Ref.GraphNode->NodePosX;
					const int32 OldY = Ref.GraphNode->NodePosY;
					if (OldX != NewX || OldY != NewY)
					{
						++MovedCount;
					}
				}
				NodeResults.Add(MakeShared<FJsonValueObject>(SharedLayoutNodeToJson(Node, Ref)));
			}

			bool bSaved = false;
			int32 CreatedRerouteNodeCount = 0;
			const bool bNeedsRerouteWrite = bRerouteWritebackSupported
				&& bEffectiveReplaceExistingRerouteNodes
				&& (ExistingRerouteNodeCount > 0 || PlannedRerouteKnotNodeCount > 0);
			const bool bNeedsWrite = MovedCount > 0 || bNeedsRerouteWrite;
			TUniquePtr<FScopedTransaction> LayoutTransaction;
			if (bNeedsWrite)
			{
				LayoutTransaction = MakeUnique<FScopedTransaction>(
					NSLOCTEXT("UeAgentInterface", "NodeGraphLayoutGenericTransaction", "UeAgentInterface Node Graph Layout"));
				Graph->Modify();
				Asset->Modify();
				for (int32 Index = 0; Index < NodeCount; ++Index)
				{
					const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[Index];
					const FUniversalLayoutNodeRef& Ref = Model.NodeRefs[Index];
					if (!Ref.bVirtual && Ref.GraphNode)
					{
						const int32 NewX = FMath::RoundToInt(Node.NewX);
						const int32 NewY = FMath::RoundToInt(Node.NewY);
						if (Ref.GraphNode->NodePosX != NewX || Ref.GraphNode->NodePosY != NewY)
						{
							ApplySharedGraphNodePosition(Ref.GraphNode, NewX, NewY);
						}
					}
				}
				if (bNeedsRerouteWrite)
				{
					CreatedRerouteNodeCount = ApplyRigVMReroutePlans(
						RigGraph,
						Model,
						bInsertRerouteNodes ? RunResult.ReroutePlans : TArray<UeAgentBlueprintAutoLayout::FReroutePlan>(),
						bEffectiveReplaceExistingRerouteNodes,
						OutError);
					if (!OutError.IsEmpty())
					{
						if (LayoutTransaction)
						{
							LayoutTransaction->Cancel();
						}
						return false;
					}
				}
				Graph->NotifyGraphChanged();
				Asset->MarkPackageDirty();
				if (bSaveAfterLayout && !SaveAssetPackage(Asset, OutError))
				{
					if (LayoutTransaction)
					{
						LayoutTransaction->Cancel();
					}
					return false;
				}
				bSaved = bSaveAfterLayout;
			}

			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("adapter"), AdapterForAsset(Asset));
			OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
			OutData->SetStringField(TEXT("graph_path"), Graph->GetPathName());
			AddSharedLayoutRunFields(OutData, RunResult);
			AddSharedGeometryFields(OutData, GeometryStats);
			OutData->SetBoolField(TEXT("changed"), bNeedsWrite);
			OutData->SetBoolField(TEXT("saved"), bSaved);
			OutData->SetNumberField(TEXT("output_access_offset_x"), Options.OutputAccessOffsetX);
			OutData->SetNumberField(TEXT("input_access_offset_x"), Options.InputAccessOffsetX);
			OutData->SetStringField(TEXT("access_offset_source"), Options.AccessOffsetSource);
			OutData->SetBoolField(TEXT("reroute_writeback_supported"), bRerouteWritebackSupported);
			OutData->SetBoolField(TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
			OutData->SetBoolField(TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
			OutData->SetBoolField(TEXT("effective_replace_existing_reroute_nodes"), bEffectiveReplaceExistingRerouteNodes);
			OutData->SetBoolField(TEXT("allow_remove_all_reroute_nodes"), bAllowRemoveAllRerouteNodes);
			OutData->SetNumberField(TEXT("existing_reroute_node_count"), ExistingRerouteNodeCount);
			OutData->SetNumberField(TEXT("planned_reroute_knot_node_count"), PlannedRerouteKnotNodeCount);
			OutData->SetNumberField(TEXT("node_count"), Model.WritableNodeCount);
			OutData->SetNumberField(TEXT("virtual_node_count"), Model.Nodes.Num() - Model.WritableNodeCount);
			OutData->SetNumberField(TEXT("edge_count"), Model.Edges.Num());
			OutData->SetNumberField(TEXT("collapsed_reroute_node_count"), Model.CollapsedRerouteNodeCount);
			OutData->SetNumberField(TEXT("collapsed_reroute_edge_count"), Model.CollapsedRerouteEdgeCount);
			OutData->SetNumberField(TEXT("moved_node_count"), MovedCount);
			OutData->SetNumberField(TEXT("created_reroute_node_count"), CreatedRerouteNodeCount);
			OutData->SetArrayField(TEXT("nodes"), NodeResults);
			OutData->SetArrayField(TEXT("edges"), SharedLayoutEdgesToJson(Model));
			return true;
		}
	}
}

bool FUeAgentHttpServer::CmdNodeGraphList(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNodeGraphOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	FString RequestedAdapter = TEXT("auto");
	JsonTryGetString(Ctx.Params, TEXT("adapter"), RequestedAdapter);
	const FString Adapter = UeAgentNodeGraphOps::AdapterForAsset(Asset);

	bool bIncludeNestedGraphs = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_nested_graphs"), bIncludeNestedGraphs);
	TSharedPtr<FJsonObject> Selector;
	if (UeAgentNodeGraphOps::TryGetObjectField(Ctx.Params, TEXT("graph_selector"), Selector))
	{
		Selector->TryGetBoolField(TEXT("include_nested_graphs"), bIncludeNestedGraphs);
	}

	TArray<TSharedPtr<FJsonValue>> GraphsJson;
	if (Asset->IsA<UControlRigBlueprint>())
	{
		const TArray<UeAgentNodeGraphOps::FGraphRef> Graphs = UeAgentNodeGraphOps::CollectGenericEdGraphs(Asset);
		for (const UeAgentNodeGraphOps::FGraphRef& Graph : Graphs)
		{
			GraphsJson.Add(MakeShared<FJsonValueObject>(UeAgentNodeGraphOps::GraphRefToJson(Graph)));
		}
	}
	else if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
	{
		const TArray<UeAgentNodeGraphOps::FGraphRef> Graphs = UeAgentNodeGraphOps::CollectBlueprintGraphs(Blueprint, bIncludeNestedGraphs);
		for (const UeAgentNodeGraphOps::FGraphRef& Graph : Graphs)
		{
			GraphsJson.Add(MakeShared<FJsonValueObject>(UeAgentNodeGraphOps::GraphRefToJson(Graph)));
		}
	}
	else if (Asset->IsA<UMaterial>() || Asset->IsA<UMaterialFunctionInterface>())
	{
		UeAgentNodeGraphOps::AddMaterialGraphSummary(Asset, GraphsJson);
	}
	else
	{
		const TArray<UeAgentNodeGraphOps::FGraphRef> Graphs = UeAgentNodeGraphOps::CollectGenericEdGraphs(Asset);
		for (const UeAgentNodeGraphOps::FGraphRef& Graph : Graphs)
		{
			GraphsJson.Add(MakeShared<FJsonValueObject>(UeAgentNodeGraphOps::GraphRefToJson(Graph)));
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("requested_adapter"), RequestedAdapter);
	OutData->SetStringField(TEXT("adapter"), Adapter);
	OutData->SetArrayField(TEXT("graphs"), GraphsJson);
	OutData->SetNumberField(TEXT("graph_count"), GraphsJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNodeGraphLayout(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (UeAgentNodeGraphOps::HasRemovedDryRunParameter(Ctx.Params))
	{
		OutError = TEXT("dry_run_removed_node_graph_layout_always_writes_use_undo_to_revert");
		return false;
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNodeGraphOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	const FString Adapter = UeAgentNodeGraphOps::AdapterForAsset(Asset);
	if (Adapter == TEXT("generic_edgraph"))
	{
		return UeAgentNodeGraphOps::LayoutGenericEdGraph(Asset, Ctx.Params, OutData, OutError);
	}

	if (Asset->IsA<UBlueprint>())
	{
		TSharedPtr<FJsonObject> LegacyParams = UeAgentNodeGraphOps::BuildBlueprintLayoutParams(Ctx.Params);
		FUeAgentRequestContext LegacyCtx;
		LegacyCtx.RequestId = Ctx.RequestId + TEXT(":blueprint_layout_all_graphs");
		LegacyCtx.Command = TEXT("blueprint_layout_all_graphs");
		LegacyCtx.Params = LegacyParams;

		TSharedPtr<FJsonObject> LegacyData;
		FString LegacyError;
		if (!ExecuteBlueprintCommand(TEXT("blueprint_layout_all_graphs"), LegacyCtx, LegacyData, LegacyError))
		{
			OutData = LegacyData.IsValid() ? LegacyData : MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("adapter"), Adapter);
			OutData->SetStringField(TEXT("delegated_command"), TEXT("blueprint_layout_all_graphs"));
			OutError = LegacyError;
			return false;
		}

		OutData = LegacyData.IsValid() ? LegacyData : MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("adapter"), Adapter);
		OutData->SetStringField(TEXT("delegated_command"), TEXT("blueprint_layout_all_graphs"));
		OutData->SetStringField(TEXT("node_graph_layout_strategy"), TEXT("delegated_blueprint_layout_kernel"));
		return true;
	}

	if (Asset->IsA<UMaterial>() || Asset->IsA<UMaterialFunctionInterface>())
	{
		return UeAgentNodeGraphOps::LayoutMaterialExpressionGraph(Asset, Ctx.Params, OutData, OutError);
	}

	return UeAgentNodeGraphOps::LayoutGenericEdGraph(Asset, Ctx.Params, OutData, OutError);
}
