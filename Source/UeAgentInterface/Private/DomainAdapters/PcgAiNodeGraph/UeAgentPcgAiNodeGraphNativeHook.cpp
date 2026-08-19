#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphNativeHook.h"

#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQueryGraph.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGDataAsset.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "SmartObjectDefinition.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "Components/StateTreeComponentSchema.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StructUtils/PropertyBag.h"

#include <openssl/sha.h>

namespace UE::UAI::DomainAdapters::PcgAiNodeGraph
{
	namespace
	{
		FString ToGuidString(const FGuid& Guid)
		{
			return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
		}

		FString HashCanonicalText(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text, Text.Len());
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			const uint8* Bytes = Utf8.Length() > 0
				? reinterpret_cast<const uint8*>(Utf8.Get())
				: &Empty;
			if (!SHA256(Bytes, static_cast<size_t>(Utf8.Length()),
				Signature.Signature))
			{
				return FString();
			}
			return TEXT("sha256:") + Signature.ToString().ToLower();
		}

		FString HashCanonicalLines(TArray<FString> Lines)
		{
			Lines.Sort();
			return HashCanonicalText(FString::Join(Lines, TEXT("\n")));
		}

		FString MakeEvidence(
			const TCHAR* Stage,
			const TCHAR* Domain,
			const FString& Revision)
		{
			return FString::Printf(
				TEXT("pcg-ai-node-graph:%s:%s:%s"),
				Stage, Domain, *Revision);
		}

		FString MakeEdGraphNodeItemKey(const UEdGraphNode& Node)
		{
			const FString Identity = Node.NodeGuid.IsValid()
				? ToGuidString(Node.NodeGuid)
				: Node.GetPathName(Node.GetGraph());
			return TEXT("node:") + Identity;
		}

		FString MakeEdGraphPinKey(
			const FString& OwnerItemKey,
			const UEdGraphPin& Pin,
			const int32 PinIndex)
		{
			const FString Direction = Pin.Direction == EGPD_Output
				? TEXT("out") : TEXT("in");
			const FString NativeIdentity = Pin.PinId.IsValid()
				? ToGuidString(Pin.PinId)
				: FString::Printf(TEXT("%s:%d"), *Pin.PinName.ToString(), PinIndex);
			return FString::Printf(
				TEXT("%s:%s:%s"),
				*OwnerItemKey, *Direction, *NativeIdentity);
		}

		FString GetEdGraphPayloadType(const UEdGraphNode& Node)
		{
			const UAIGraphNode* AiNode = Cast<UAIGraphNode>(&Node);
			const UObject* NodeInstance = AiNode ? AiNode->NodeInstance.Get() : nullptr;
			if (const UEnvQueryOption* Option = Cast<UEnvQueryOption>(NodeInstance))
			{
				if (Option->Generator)
				{
					return Option->Generator->GetClass()->GetPathName();
				}
			}
			return NodeInstance
				? NodeInstance->GetClass()->GetPathName()
				: Node.GetClass()->GetPathName();
		}

		bool CaptureEdGraphTopology(
			const FString& TargetKind,
			const UObject& Target,
			const UEdGraph& Graph,
			FPcgAiNodeGraphTopology& OutTopology,
			FString& OutRevision,
			FPcgAiNodeGraphAdapterError& OutError)
		{
			TArray<FPcgAiNodeGraphItemIdentity> Items;
			TArray<FPcgAiNodeGraphPinIdentity> Pins;
			TArray<FPcgAiNodeGraphRelationIdentity> Relations;
			TMap<const UEdGraphNode*, FString> ItemKeys;
			TMap<const UEdGraphPin*, FString> PinKeys;

			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (!Node)
				{
					continue;
				}
				const FString ItemKey = MakeEdGraphNodeItemKey(*Node);
				const FString NativeIdentity = Node->NodeGuid.IsValid()
					? ToGuidString(Node->NodeGuid)
					: Node->GetPathName(&Graph);
				const FString BuiltinRole = Node->GetClass()->GetName().EndsWith(TEXT("_Root"))
					? TEXT("root") : FString();
				Items.Emplace(
					ItemKey,
					NativeIdentity,
					BuiltinRole,
					Node->GetClass()->GetPathName(),
					GetEdGraphPayloadType(*Node));
				ItemKeys.Add(Node, ItemKey);
			}

			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				const FString* OwnerItemKey = Node ? ItemKeys.Find(Node) : nullptr;
				if (!OwnerItemKey)
				{
					continue;
				}
				for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
				{
					const UEdGraphPin* Pin = Node->Pins[PinIndex];
					if (!Pin || (Pin->Direction != EGPD_Input
						&& Pin->Direction != EGPD_Output))
					{
						continue;
					}
					const FString PinKey = MakeEdGraphPinKey(
						*OwnerItemKey, *Pin, PinIndex);
					const FString NativeLabel = Pin->PinName.IsNone()
						? FString::Printf(TEXT("pin_%d"), PinIndex)
						: Pin->PinName.ToString();
					const FString NativeType = Pin->PinType.PinCategory.IsNone()
						? TEXT("ed_graph_pin")
						: Pin->PinType.PinCategory.ToString();
					Pins.Emplace(
						PinKey,
						*OwnerItemKey,
						NativeLabel,
						Pin->Direction == EGPD_Output
							? EPcgAiNodeGraphPinDirection::Output
							: EPcgAiNodeGraphPinDirection::Input,
						NativeType);
					PinKeys.Add(Pin, PinKey);
				}
			}

			for (const UEdGraphNode* Node : Graph.Nodes)
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
					const FString* FromPinKey = PinKeys.Find(Pin);
					if (!FromPinKey)
					{
						continue;
					}
					for (int32 LinkIndex = 0; LinkIndex < Pin->LinkedTo.Num(); ++LinkIndex)
					{
						const UEdGraphPin* LinkedPin = Pin->LinkedTo[LinkIndex];
						const FString* ToPinKey = LinkedPin
							? PinKeys.Find(LinkedPin) : nullptr;
						if (!ToPinKey || LinkedPin->Direction != EGPD_Input)
						{
							continue;
						}
						const FString RelationKey = FString::Printf(
							TEXT("link:%s->%s"), **FromPinKey, **ToPinKey);
						Relations.Emplace(
							RelationKey,
							TEXT("graph_link"),
							FString::Printf(
								TEXT("%s:%s"),
								*ToGuidString(Pin->PinId),
								*ToGuidString(LinkedPin->PinId)),
							*FromPinKey,
							*ToPinKey,
							LinkIndex);
					}
				}
			}

			Items.Sort([](const FPcgAiNodeGraphItemIdentity& A,
				const FPcgAiNodeGraphItemIdentity& B)
			{
				return A.GetItemKey() < B.GetItemKey();
			});
			Pins.Sort([](const FPcgAiNodeGraphPinIdentity& A,
				const FPcgAiNodeGraphPinIdentity& B)
			{
				return A.GetPinKey() < B.GetPinKey();
			});
			Relations.Sort([](const FPcgAiNodeGraphRelationIdentity& A,
				const FPcgAiNodeGraphRelationIdentity& B)
			{
				return A.GetRelationKey() < B.GetRelationKey();
			});

			TArray<FString> RevisionLines;
			for (const FPcgAiNodeGraphItemIdentity& Item : Items)
			{
				RevisionLines.Add(FString::Printf(
					TEXT("item|%s|%s|%s|%s|%s"),
					*Item.GetItemKey(), *Item.GetNativeIdentity(),
					*Item.GetStableBuiltinRole(),
					*Item.GetNativeCarrierTypeIdentity(),
					*Item.GetNativeTypeIdentity()));
			}
			for (const FPcgAiNodeGraphPinIdentity& Pin : Pins)
			{
				RevisionLines.Add(FString::Printf(
					TEXT("pin|%s|%s|%s|%d|%s"),
					*Pin.GetPinKey(), *Pin.GetOwnerItemKey(),
					*Pin.GetNativeLabel(), static_cast<int32>(Pin.GetDirection()),
					*Pin.GetNativeTypeIdentity()));
			}
			for (const FPcgAiNodeGraphRelationIdentity& Relation : Relations)
			{
				RevisionLines.Add(FString::Printf(
					TEXT("relation|%s|%s|%s|%s|%s|%d"),
					*Relation.GetRelationKey(), *Relation.GetRelationKind(),
					*Relation.GetNativeRelationIdentity(),
					*Relation.GetFromIdentity(), *Relation.GetToIdentity(),
					Relation.GetOrderIndex()));
			}
			OutRevision = HashCanonicalLines(MoveTemp(RevisionLines));
			OutTopology = FPcgAiNodeGraphTopology(
				TargetKind,
				Target.GetPathName(),
				Graph.GetPathName(&Target),
				MoveTemp(Items),
				MoveTemp(Pins),
				MoveTemp(Relations));
			return TryValidatePcgAiNodeGraphTopology(OutTopology, OutError);
		}

		FString PropertyBagTypeName(const EPropertyBagPropertyType Type)
		{
			switch (Type)
			{
			case EPropertyBagPropertyType::Double:
				return TEXT("double");
			case EPropertyBagPropertyType::Float:
				return TEXT("float");
			case EPropertyBagPropertyType::Int32:
				return TEXT("int32");
			case EPropertyBagPropertyType::Int64:
				return TEXT("int64");
			case EPropertyBagPropertyType::Bool:
				return TEXT("bool");
			case EPropertyBagPropertyType::Name:
				return TEXT("name");
			case EPropertyBagPropertyType::String:
				return TEXT("string");
			default:
				return FString::Printf(TEXT("property_bag_type_%d"),
					static_cast<int32>(Type));
			}
		}

		bool CapturePcgGraph(
			UPCGGraph& Graph,
			FPcgAiNodeGraphTopology& OutTopology,
			TArray<FPcgAiNodeGraphParameterIdentity>& OutParameters,
			FString& OutRevision,
			FPcgAiNodeGraphAdapterError& OutError)
		{
			TArray<UPCGNode*> Nodes;
			if (UPCGNode* InputNode = Graph.GetInputNode())
			{
				Nodes.Add(InputNode);
			}
			Nodes.Append(Graph.GetNodes());
			if (UPCGNode* OutputNode = Graph.GetOutputNode())
			{
				Nodes.Add(OutputNode);
			}

			TArray<FPcgAiNodeGraphItemIdentity> Items;
			TArray<FPcgAiNodeGraphPinIdentity> Pins;
			TArray<FPcgAiNodeGraphRelationIdentity> Relations;
			TMap<const UPCGNode*, FString> ItemKeys;
			TMap<const UPCGPin*, FString> PinKeys;

			for (UPCGNode* Node : Nodes)
			{
				if (!Node)
				{
					continue;
				}
				FString BuiltinRole;
				FString ItemKey;
				if (Node == Graph.GetInputNode())
				{
					BuiltinRole = TEXT("input");
					ItemKey = TEXT("pcg.input");
				}
				else if (Node == Graph.GetOutputNode())
				{
					BuiltinRole = TEXT("output");
					ItemKey = TEXT("pcg.output");
				}
				else
				{
					ItemKey = Node->HasAuthoredTitle()
						? TEXT("pcg_node:")
							+ Node->GetAuthoredTitleName().ToString()
						: TEXT("pcg.node:") + Node->GetPathName(&Graph);
				}
				const UPCGSettings* Settings = Node->GetSettings();
				Items.Emplace(
					ItemKey,
					Node->GetPathName(&Graph),
					BuiltinRole,
					Node->GetClass()->GetPathName(),
					Settings ? Settings->GetClass()->GetPathName()
						: Node->GetClass()->GetPathName());
				ItemKeys.Add(Node, ItemKey);
			}

			auto AddPins = [&Pins, &PinKeys, &ItemKeys](
				const UPCGNode& Node,
				const TArray<TObjectPtr<UPCGPin>>& NativePins,
				const EPcgAiNodeGraphPinDirection Direction)
			{
				const FString* ItemKey = ItemKeys.Find(&Node);
				if (!ItemKey)
				{
					return;
				}
				for (int32 Index = 0; Index < NativePins.Num(); ++Index)
				{
					const UPCGPin* Pin = NativePins[Index];
					if (!Pin)
					{
						continue;
					}
					const FString Label = Pin->Properties.Label.IsNone()
						? FString::Printf(TEXT("pin_%d"), Index)
						: Pin->Properties.Label.ToString();
					const FString PinKey = FString::Printf(
						TEXT("%s:%s:%s:%d"),
						**ItemKey,
						Direction == EPcgAiNodeGraphPinDirection::Output
							? TEXT("out") : TEXT("in"),
						*Label,
						Index);
					const FString TypeName = StaticEnum<EPCGDataType>()
						->GetNameStringByValue(
							static_cast<int64>(Pin->Properties.AllowedTypes));
					Pins.Emplace(
						PinKey,
						*ItemKey,
						Label,
						Direction,
						TypeName.IsEmpty() ? TEXT("pcg_data") : TypeName);
					PinKeys.Add(Pin, PinKey);
				}
			};

			for (const UPCGNode* Node : Nodes)
			{
				if (!Node)
				{
					continue;
				}
				AddPins(*Node, Node->GetInputPins(),
					EPcgAiNodeGraphPinDirection::Input);
				AddPins(*Node, Node->GetOutputPins(),
					EPcgAiNodeGraphPinDirection::Output);
			}

			for (const UPCGNode* Node : Nodes)
			{
				if (!Node)
				{
					continue;
				}
				for (const UPCGPin* FromPin : Node->GetOutputPins())
				{
					const FString* FromPinKey = FromPin ? PinKeys.Find(FromPin) : nullptr;
					if (!FromPinKey)
					{
						continue;
					}
					for (const UPCGEdge* Edge : FromPin->Edges)
					{
						if (!Edge || Edge->InputPin != FromPin || !Edge->OutputPin)
						{
							continue;
						}
						const FString* ToPinKey = PinKeys.Find(Edge->OutputPin);
						if (!ToPinKey)
						{
							continue;
						}
						Relations.Emplace(
							TEXT("edge:") + Edge->GetPathName(&Graph),
							TEXT("graph_link"),
							Edge->GetPathName(&Graph),
							*FromPinKey,
							*ToPinKey,
							INDEX_NONE);
					}
				}
			}

			OutParameters.Reset();
			const FInstancedPropertyBag* Bag = Graph.GetUserParametersStruct();
			if (Bag && Bag->GetPropertyBagStruct())
			{
				for (const FPropertyBagPropertyDesc& Desc
					: Bag->GetPropertyBagStruct()->GetPropertyDescs())
				{
					OutParameters.Emplace(
						Graph.GetPathName(),
						Desc.ID,
						Desc.Name,
						PropertyBagTypeName(Desc.ValueType));
				}
			}

			Items.Sort([](const FPcgAiNodeGraphItemIdentity& A,
				const FPcgAiNodeGraphItemIdentity& B)
			{
				return A.GetItemKey() < B.GetItemKey();
			});
			Pins.Sort([](const FPcgAiNodeGraphPinIdentity& A,
				const FPcgAiNodeGraphPinIdentity& B)
			{
				return A.GetPinKey() < B.GetPinKey();
			});
			Relations.Sort([](const FPcgAiNodeGraphRelationIdentity& A,
				const FPcgAiNodeGraphRelationIdentity& B)
			{
				return A.GetRelationKey() < B.GetRelationKey();
			});
			OutParameters.Sort([](const FPcgAiNodeGraphParameterIdentity& A,
				const FPcgAiNodeGraphParameterIdentity& B)
			{
				return A.GetPropertyName().LexicalLess(B.GetPropertyName());
			});

			TArray<FString> RevisionLines;
			for (const FPcgAiNodeGraphItemIdentity& Item : Items)
			{
				RevisionLines.Add(FString::Printf(
					TEXT("item|%s|%s|%s|%s"),
					*Item.GetItemKey(), *Item.GetNativeIdentity(),
					*Item.GetStableBuiltinRole(), *Item.GetNativeTypeIdentity()));
			}
			for (const FPcgAiNodeGraphPinIdentity& Pin : Pins)
			{
				RevisionLines.Add(FString::Printf(
					TEXT("pin|%s|%s|%s|%d|%s"),
					*Pin.GetPinKey(), *Pin.GetOwnerItemKey(),
					*Pin.GetNativeLabel(), static_cast<int32>(Pin.GetDirection()),
					*Pin.GetNativeTypeIdentity()));
			}
			for (const FPcgAiNodeGraphRelationIdentity& Relation : Relations)
			{
				RevisionLines.Add(FString::Printf(
					TEXT("relation|%s|%s|%s|%s"),
					*Relation.GetRelationKey(), *Relation.GetNativeRelationIdentity(),
					*Relation.GetFromIdentity(), *Relation.GetToIdentity()));
			}
			for (const FPcgAiNodeGraphParameterIdentity& Parameter : OutParameters)
			{
				RevisionLines.Add(FString::Printf(
					TEXT("parameter|%s|%s|%s"),
					*ToGuidString(Parameter.GetPropertyGuid()),
					*Parameter.GetPropertyName().ToString(),
					*Parameter.GetPropertyType()));
			}
			OutRevision = HashCanonicalLines(MoveTemp(RevisionLines));
			OutTopology = FPcgAiNodeGraphTopology(
				TEXT("pcg_graph"),
				Graph.GetPathName(),
				Graph.GetPathName(),
				MoveTemp(Items),
				MoveTemp(Pins),
				MoveTemp(Relations));
			if (!TryValidatePcgAiNodeGraphTopology(OutTopology, OutError))
			{
				return false;
			}
			for (const FPcgAiNodeGraphParameterIdentity& Parameter : OutParameters)
			{
				if (!TryValidatePcgAiNodeGraphParameterIdentity(Parameter, OutError))
				{
					return false;
				}
			}
			return true;
		}
	}

	bool FPcgAiNodeGraphNativeHook::TryCaptureOwnedAssetObject(
		UObject& Outer,
		const FString& AssetKind,
		UClass& AssetClass,
		const FName ObjectName,
		UObject& Object,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphAssetIdentity& OutIdentity,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutApplyReceipt.SetEvidenceId(FString());
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutIdentity = FPcgAiNodeGraphAssetIdentity();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		if (AssetKind.IsEmpty() || ObjectName.IsNone()
			|| AssetClass.HasAnyClassFlags(CLASS_Abstract)
			|| Object.GetOuter() != &Outer
			|| Object.GetFName() != ObjectName
			|| Object.GetClass() != &AssetClass)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_asset_create_input_invalid"));
			return false;
		}
		const FString ApplyRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s"), *Object.GetPathName(), *AssetClass.GetPathName()));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(
			TEXT("apply"), TEXT("asset"), ApplyRevision));
#if WITH_EDITOR
		Object.PostEditChange();
#endif
		const bool bRefreshed = Object.GetOuter() == &Outer
			&& Object.GetFName() == ObjectName
			&& Object.GetClass() == &AssetClass;
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Refresh;
		OutBarrierReceipt.bComplete = bRefreshed;
		OutBarrierReceipt.ErrorCount = bRefreshed ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("asset"), ApplyRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("asset"), ApplyRevision);
		if (!bRefreshed)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_asset_refresh_incomplete"));
			return false;
		}
		OutRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s"), *AssetKind,
			*Object.GetPathName(), *Object.GetClass()->GetPathName()));
		OutIdentity = FPcgAiNodeGraphAssetIdentity(
			AssetKind,
			Object.GetPathName(),
			Object.GetClass()->GetPathName());
		if (!TryValidatePcgAiNodeGraphAssetIdentity(OutIdentity, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(TEXT("readback"), TEXT("asset"), OutRevision);
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TryCreateAndCaptureOwnedAssetObject(
		UObject& Outer,
		const FString& AssetKind,
		UClass& AssetClass,
		const FName ObjectName,
		UObject*& OutObject,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphAssetIdentity& OutIdentity,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutObject = nullptr;
		OutError.Reset();
		if (AssetKind.IsEmpty() || ObjectName.IsNone()
			|| AssetClass.HasAnyClassFlags(CLASS_Abstract))
		{
			OutError.Set(TEXT("pcg_ai_node_graph_asset_create_input_invalid"));
			return false;
		}
		OutObject = NewObject<UObject>(
			&Outer, &AssetClass, ObjectName, RF_Transactional);
		if (!OutObject || OutObject->GetClass() != &AssetClass)
		{
			OutObject = nullptr;
			OutError.Set(TEXT("pcg_ai_node_graph_asset_create_failed"));
			return false;
		}
		if (!TryCaptureOwnedAssetObject(
				Outer, AssetKind, AssetClass, ObjectName, *OutObject,
				OutApplyReceipt, OutBarrierReceipt, OutIdentity,
				OutRevision, OutEvidenceId, OutError))
		{
			OutObject = nullptr;
			return false;
		}
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TryCompileAndCaptureBehaviorTree(
		UBehaviorTree& Tree,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutApplyReceipt.SetEvidenceId(FString());
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutTopology = FPcgAiNodeGraphTopology();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph);
		if (!Graph)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_behavior_tree_graph_missing"));
			return false;
		}
		const FString Before = HashCanonicalText(Tree.GetPathName() + TEXT("|before_compile"));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(TEXT("apply"), TEXT("behavior_tree"), Before));
		Graph->UpdateAsset();
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Compile;
		OutBarrierReceipt.bComplete = Tree.RootNode != nullptr;
		OutBarrierReceipt.ErrorCount = OutBarrierReceipt.bComplete ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("behavior_tree"), Before);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("behavior_tree"), Before);
		if (!OutBarrierReceipt.bComplete)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_behavior_tree_compile_incomplete"));
			return false;
		}
		if (!CaptureEdGraphTopology(
			TEXT("behavior_tree"), Tree, *Graph,
			OutTopology, OutRevision, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(TEXT("readback"), TEXT("behavior_tree"), OutRevision);
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TryAddAndCaptureBlackboardEntry(
		UBlackboardData& Blackboard,
		const FName EntryName,
		UClass& KeyTypeClass,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphBlackboardEntryIdentity& OutIdentity,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutApplyReceipt.SetEvidenceId(FString());
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutIdentity = FPcgAiNodeGraphBlackboardEntryIdentity();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		if (EntryName.IsNone()
			|| !KeyTypeClass.IsChildOf(UBlackboardKeyType::StaticClass())
			|| KeyTypeClass.HasAnyClassFlags(CLASS_Abstract))
		{
			OutError.Set(TEXT("pcg_ai_node_graph_blackboard_entry_input_invalid"));
			return false;
		}
		if (Blackboard.Keys.ContainsByPredicate(
			[EntryName](const FBlackboardEntry& Entry)
			{
				return Entry.EntryName == EntryName;
			}))
		{
			OutError.Set(TEXT("pcg_ai_node_graph_blackboard_entry_duplicate"));
			return false;
		}
		FBlackboardEntry Entry;
		Entry.EntryName = EntryName;
		Entry.KeyType = NewObject<UBlackboardKeyType>(
			&Blackboard, &KeyTypeClass, NAME_None, RF_Transactional);
		if (!Entry.KeyType)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_blackboard_key_create_failed"));
			return false;
		}
		Blackboard.Keys.Add(Entry);
		const FString ApplyRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s"), *Blackboard.GetPathName(),
			*EntryName.ToString(), *KeyTypeClass.GetPathName()));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(
			TEXT("apply"), TEXT("blackboard"), ApplyRevision));
		Blackboard.UpdateParentKeys();
		Blackboard.UpdateKeyIDs();
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Refresh;
		OutBarrierReceipt.bComplete =
			Blackboard.GetKeyID(EntryName) != FBlackboard::InvalidKey;
		OutBarrierReceipt.ErrorCount = OutBarrierReceipt.bComplete ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("blackboard"), ApplyRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("blackboard"), ApplyRevision);
		if (!OutBarrierReceipt.bComplete)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_blackboard_refresh_incomplete"));
			return false;
		}

		TArray<FString> RevisionLines;
		for (const FBlackboardEntry& NativeEntry : Blackboard.Keys)
		{
			RevisionLines.Add(FString::Printf(
				TEXT("%s|%s"),
				*NativeEntry.EntryName.ToString(),
				NativeEntry.KeyType
					? *NativeEntry.KeyType->GetClass()->GetPathName()
					: TEXT("null")));
		}
		OutRevision = HashCanonicalLines(MoveTemp(RevisionLines));
		const FBlackboardEntry* ReadbackEntry = Blackboard.Keys.FindByPredicate(
			[EntryName](const FBlackboardEntry& Candidate)
			{
				return Candidate.EntryName == EntryName;
			});
		if (!ReadbackEntry || !ReadbackEntry->KeyType)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_blackboard_readback_failed"));
			return false;
		}
		OutIdentity = FPcgAiNodeGraphBlackboardEntryIdentity(
			Blackboard.GetPathName(),
			EntryName.ToString(),
			ReadbackEntry->KeyType->GetClass()->GetPathName(),
			EPcgAiNodeGraphEntryOwnership::Local);
		if (!TryValidatePcgAiNodeGraphBlackboardEntryIdentity(OutIdentity, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(TEXT("readback"), TEXT("blackboard"), OutRevision);
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TryCompileAndCaptureEqs(
		UEnvQuery& Query,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutApplyReceipt.SetEvidenceId(FString());
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutTopology = FPcgAiNodeGraphTopology();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		UEnvironmentQueryGraph* Graph = Cast<UEnvironmentQueryGraph>(Query.EdGraph);
		if (!Graph)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_eqs_graph_missing"));
			return false;
		}
		const FString Before = HashCanonicalText(Query.GetPathName() + TEXT("|before_compile"));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(TEXT("apply"), TEXT("eqs"), Before));
		Graph->UpdateAsset();
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Compile;
		OutBarrierReceipt.bComplete = Graph->Nodes.ContainsByPredicate(
			[](const UEdGraphNode* Node)
			{
				const UAIGraphNode* AiNode = Cast<UAIGraphNode>(Node);
				const UEnvQueryOption* Option = AiNode
					? Cast<UEnvQueryOption>(AiNode->NodeInstance.Get())
					: nullptr;
				return Option && Option->Generator;
			});
		OutBarrierReceipt.ErrorCount = OutBarrierReceipt.bComplete ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("eqs"), Before);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("eqs"), Before);
		if (!OutBarrierReceipt.bComplete)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_eqs_compile_incomplete"));
			return false;
		}
		if (!CaptureEdGraphTopology(
			TEXT("eqs"), Query, *Graph,
			OutTopology, OutRevision, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(TEXT("readback"), TEXT("eqs"), OutRevision);
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TryEnsureCompileAndCaptureStateTree(
		UStateTree& Tree,
		const FName RootStateName,
		const FName ChildStateName,
		const FGuid& TransitionGuid,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutApplyReceipt.SetEvidenceId(FString());
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutTopology = FPcgAiNodeGraphTopology();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		if (RootStateName.IsNone() || ChildStateName.IsNone()
			|| !TransitionGuid.IsValid())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_state_tree_root_name_missing"));
			return false;
		}
#if WITH_EDITORONLY_DATA
		UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(Tree.EditorData);
		if (!EditorData)
		{
			EditorData = NewObject<UStateTreeEditorData>(
				&Tree, UStateTreeEditorData::StaticClass(), NAME_None, RF_Transactional);
			Tree.EditorData = EditorData;
		}
		if (!EditorData->Schema)
		{
			EditorData->Schema = NewObject<UStateTreeComponentSchema>(
				EditorData, UStateTreeComponentSchema::StaticClass(),
				NAME_None, RF_Transactional);
		}
		UStateTreeState* RootState = nullptr;
		if (EditorData->SubTrees.IsEmpty())
		{
			UStateTreeState& Root = EditorData->AddRootState();
			Root.ID = FGuid::NewGuid();
			Root.Name = RootStateName;
			RootState = &Root;
		}
		else
		{
			UStateTreeState* Root = EditorData->SubTrees[0];
			if (!Root)
			{
				OutError.Set(TEXT("pcg_ai_node_graph_state_tree_root_invalid"));
				return false;
			}
			if (!Root->ID.IsValid())
			{
				Root->ID = FGuid::NewGuid();
			}
			Root->Name = RootStateName;
			RootState = Root;
		}
		UStateTreeState* ChildState = nullptr;
		for (UStateTreeState* State : RootState->Children)
		{
			if (State && State->Name == ChildStateName)
			{
				ChildState = State;
				break;
			}
		}
		if (!ChildState)
		{
			ChildState = &RootState->AddChildState(ChildStateName);
			ChildState->ID = FGuid::NewGuid();
		}
		else if (!ChildState->ID.IsValid())
		{
			ChildState->ID = FGuid::NewGuid();
		}
		FStateTreeTransition* NativeTransition = RootState->Transitions.FindByPredicate(
			[TransitionGuid](const FStateTreeTransition& Transition)
			{
				return Transition.ID == TransitionGuid;
			});
		if (!NativeTransition)
		{
			NativeTransition = &RootState->AddTransition(
				EStateTreeTransitionTrigger::OnStateCompleted,
				EStateTreeTransitionType::GotoState,
				ChildState);
			NativeTransition->ID = TransitionGuid;
		}
		const FString ApplyRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s|%s"), *Tree.GetPathName(),
			*RootStateName.ToString(), *ChildStateName.ToString(),
			*ToGuidString(TransitionGuid)));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(
			TEXT("apply"), TEXT("state_tree"), ApplyRevision));
		return TryCompileAndCaptureStateTree(
			Tree, OutApplyReceipt, OutBarrierReceipt, OutTopology,
			OutRevision, OutEvidenceId, OutError);
#else
		OutError.Set(TEXT("pcg_ai_node_graph_state_tree_editor_data_unavailable"));
		return false;
#endif
	}

	bool FPcgAiNodeGraphNativeHook::TryCompileAndCaptureStateTree(
		UStateTree& Tree,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutTopology = FPcgAiNodeGraphTopology();
		OutRevision.Reset();
		OutEvidenceId.Reset();
#if WITH_EDITORONLY_DATA
		UStateTreeEditorData* const EditorData =
			Cast<UStateTreeEditorData>(Tree.EditorData);
		if (!EditorData)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_state_tree_editor_data_unavailable"));
			return false;
		}
		const FString ApplyRevision = HashCanonicalText(
			Tree.GetPathName() + TEXT("|compile_current"));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(
			TEXT("apply"), TEXT("state_tree"), ApplyRevision));
		FStateTreeCompilerLog CompileLog;
		const bool bCompiled = UStateTreeEditingSubsystem::CompileStateTree(
			&Tree, CompileLog);
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Compile;
		OutBarrierReceipt.bComplete = bCompiled && Tree.IsReadyToRun();
		OutBarrierReceipt.ErrorCount = OutBarrierReceipt.bComplete ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("state_tree"), ApplyRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("state_tree"), ApplyRevision);
		if (!OutBarrierReceipt.bComplete)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_state_tree_compile_incomplete"));
			return false;
		}

		TArray<FPcgAiNodeGraphItemIdentity> Items;
		TArray<FPcgAiNodeGraphRelationIdentity> Relations;
		TMap<FGuid, FString> ItemByStateId;
		TArray<const UStateTreeState*> CapturedStates;
		struct FPendingState
		{
			const UStateTreeState* State = nullptr;
			FString ParentItemKey;
			int32 OrderIndex = INDEX_NONE;
		};
		TArray<FPendingState> Pending;
		for (int32 Index = EditorData->SubTrees.Num() - 1; Index >= 0; --Index)
		{
			Pending.Add({EditorData->SubTrees[Index], FString(), Index});
		}
		while (!Pending.IsEmpty())
		{
			const FPendingState Current = Pending.Pop(EAllowShrinking::No);
			if (!Current.State)
			{
				continue;
			}
			const FString NativeIdentity = Current.State->ID.IsValid()
				? ToGuidString(Current.State->ID)
				: Current.State->GetPathName(EditorData);
			const FString ItemKey = TEXT("state:") + NativeIdentity;
			Items.Emplace(
				ItemKey,
				NativeIdentity,
				Current.ParentItemKey.IsEmpty() ? TEXT("root") : FString(),
				Current.State->GetClass()->GetPathName(),
				TEXT("state_tree_state"));
			if (Current.State->ID.IsValid())
			{
				ItemByStateId.Add(Current.State->ID, ItemKey);
			}
			CapturedStates.Add(Current.State);
			if (!Current.ParentItemKey.IsEmpty())
			{
				Relations.Emplace(
					FString::Printf(TEXT("child:%s->%s"),
						*Current.ParentItemKey, *ItemKey),
					TEXT("hierarchy_child"),
					FString(),
					Current.ParentItemKey,
					ItemKey,
					Current.OrderIndex);
			}
			for (int32 ChildIndex = Current.State->Children.Num() - 1;
				ChildIndex >= 0; --ChildIndex)
			{
				Pending.Add({Current.State->Children[ChildIndex], ItemKey, ChildIndex});
			}
		}
		for (const UStateTreeState* State : CapturedStates)
		{
			if (!State || !State->ID.IsValid())
			{
				continue;
			}
			const FString* FromItemKey = ItemByStateId.Find(State->ID);
			if (!FromItemKey)
			{
				continue;
			}
			for (int32 TransitionIndex = 0;
				TransitionIndex < State->Transitions.Num(); ++TransitionIndex)
			{
				const FStateTreeTransition& Transition =
					State->Transitions[TransitionIndex];
				const FString* ToItemKey = ItemByStateId.Find(Transition.State.ID);
				if (!Transition.ID.IsValid() || !ToItemKey)
				{
					continue;
				}
				const FString TransitionIdentity = ToGuidString(Transition.ID);
				Relations.Emplace(
					TEXT("transition:") + TransitionIdentity,
					TEXT("state_transition"),
					TransitionIdentity,
					*FromItemKey,
					*ToItemKey,
					TransitionIndex);
			}
		}

		Items.Sort([](const FPcgAiNodeGraphItemIdentity& A,
			const FPcgAiNodeGraphItemIdentity& B)
		{
			return A.GetItemKey() < B.GetItemKey();
		});
		Relations.Sort([](const FPcgAiNodeGraphRelationIdentity& A,
			const FPcgAiNodeGraphRelationIdentity& B)
		{
			return A.GetRelationKey() < B.GetRelationKey();
		});
		TArray<FString> RevisionLines;
		for (const FPcgAiNodeGraphItemIdentity& Item : Items)
		{
			RevisionLines.Add(FString::Printf(TEXT("%s|%s|%s"),
				*Item.GetItemKey(), *Item.GetNativeIdentity(),
				*Item.GetNativeTypeIdentity()));
		}
		for (const FPcgAiNodeGraphRelationIdentity& Relation : Relations)
		{
			RevisionLines.Add(FString::Printf(TEXT("%s|%s|%s|%d"),
				*Relation.GetRelationKey(), *Relation.GetFromIdentity(),
				*Relation.GetToIdentity(), Relation.GetOrderIndex()));
		}
		OutRevision = HashCanonicalLines(MoveTemp(RevisionLines));
		OutTopology = FPcgAiNodeGraphTopology(
			TEXT("state_tree"),
			Tree.GetPathName(),
			EditorData->GetPathName(&Tree),
			MoveTemp(Items),
			{},
			MoveTemp(Relations));
		if (!TryValidatePcgAiNodeGraphTopology(OutTopology, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(TEXT("readback"), TEXT("state_tree"), OutRevision);
		return true;
#else
		OutError.Set(TEXT("pcg_ai_node_graph_state_tree_editor_data_unavailable"));
		return false;
#endif
	}

	bool FPcgAiNodeGraphNativeHook::TryAddValidateAndCaptureSmartObjectSlot(
		USmartObjectDefinition& Definition,
		const FGuid& SlotGuid,
		UClass& BehaviorDefinitionClass,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutApplyReceipt.SetEvidenceId(FString());
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutTopology = FPcgAiNodeGraphTopology();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		if (!SlotGuid.IsValid()
			|| !BehaviorDefinitionClass.IsChildOf(
				USmartObjectBehaviorDefinition::StaticClass())
			|| BehaviorDefinitionClass.HasAnyClassFlags(CLASS_Abstract))
		{
			OutError.Set(TEXT("pcg_ai_node_graph_smart_object_slot_input_invalid"));
			return false;
		}
		if (Definition.FindSlotByID(SlotGuid) != INDEX_NONE)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_smart_object_slot_duplicate"));
			return false;
		}
		FSmartObjectSlotDefinition& Slot = Definition.DebugAddSlot();
#if WITH_EDITORONLY_DATA
		Slot.ID = SlotGuid;
#endif
		USmartObjectBehaviorDefinition* Behavior =
			NewObject<USmartObjectBehaviorDefinition>(
				&Definition, &BehaviorDefinitionClass,
				NAME_None, RF_Transactional);
		if (!Behavior)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_smart_object_behavior_create_failed"));
			return false;
		}
		Slot.BehaviorDefinitions.Add(Behavior);
		const FString ApplyRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s"), *Definition.GetPathName(),
			*ToGuidString(SlotGuid), *BehaviorDefinitionClass.GetPathName()));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(
			TEXT("apply"), TEXT("smart_object"), ApplyRevision));
		TArray<TPair<EMessageSeverity::Type, FText>> ValidationMessages;
		const bool bValidated = Definition.Validate(&ValidationMessages);
		int32 ErrorCount = 0;
		for (const TPair<EMessageSeverity::Type, FText>& Message
			: ValidationMessages)
		{
			if (Message.Key == EMessageSeverity::Error)
			{
				++ErrorCount;
			}
		}
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Validate;
		OutBarrierReceipt.bComplete = bValidated && ErrorCount == 0;
		OutBarrierReceipt.ErrorCount = ErrorCount;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("smart_object"), ApplyRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("smart_object"), ApplyRevision);
		if (!OutBarrierReceipt.bComplete)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_smart_object_validate_incomplete"));
			return false;
		}

		TArray<FPcgAiNodeGraphItemIdentity> Items;
		TArray<FPcgAiNodeGraphRelationIdentity> Relations;
		const FString DefinitionItemKey = TEXT("smart_object.definition");
		Items.Emplace(
			DefinitionItemKey,
			Definition.GetPathName(),
			TEXT("definition"),
			Definition.GetClass()->GetPathName(),
			Definition.GetClass()->GetPathName());
		const TConstArrayView<FSmartObjectSlotDefinition> Slots = Definition.GetSlots();
		for (int32 Index = 0; Index < Slots.Num(); ++Index)
		{
			const FSmartObjectSlotDefinition& NativeSlot = Slots[Index];
#if WITH_EDITORONLY_DATA
			const FString NativeIdentity = NativeSlot.ID.IsValid()
				? ToGuidString(NativeSlot.ID)
				: FString::Printf(TEXT("slot_%d"), Index);
#else
			const FString NativeIdentity = FString::Printf(TEXT("slot_%d"), Index);
#endif
			const FString ItemKey = TEXT("smart_object.slot:") + NativeIdentity;
			const USmartObjectBehaviorDefinition* NativeBehavior =
				NativeSlot.BehaviorDefinitions.IsEmpty()
					? nullptr : NativeSlot.BehaviorDefinitions[0].Get();
			Items.Emplace(
				ItemKey,
				NativeIdentity,
				FString(),
				FSmartObjectSlotDefinition::StaticStruct()->GetPathName(),
				NativeBehavior
					? NativeBehavior->GetClass()->GetPathName()
					: TEXT("smart_object_slot"));
			Relations.Emplace(
				TEXT("slot:") + NativeIdentity,
				TEXT("ordered_attachment"),
				NativeIdentity,
				DefinitionItemKey,
				ItemKey,
				Index);
		}
		TArray<FString> RevisionLines;
		for (const FPcgAiNodeGraphItemIdentity& Item : Items)
		{
			RevisionLines.Add(FString::Printf(TEXT("%s|%s|%s"),
				*Item.GetItemKey(), *Item.GetNativeIdentity(),
				*Item.GetNativeTypeIdentity()));
		}
		OutRevision = HashCanonicalLines(MoveTemp(RevisionLines));
		OutTopology = FPcgAiNodeGraphTopology(
			TEXT("smart_object_definition"),
			Definition.GetPathName(),
			Definition.GetPathName(),
			MoveTemp(Items),
			{},
			MoveTemp(Relations));
		if (!TryValidatePcgAiNodeGraphTopology(OutTopology, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(TEXT("readback"), TEXT("smart_object"), OutRevision);
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TryAddPcgNode(
		UPCGGraph& Graph,
		UClass& SettingsClass,
		UPCGNode*& OutNode,
		FPcgAiNodeGraphNativeMutationReceipt& OutReceipt,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutNode = nullptr;
		OutReceipt.SetEvidenceId(FString());
		if (!SettingsClass.IsChildOf(UPCGSettings::StaticClass())
			|| SettingsClass.HasAnyClassFlags(CLASS_Abstract))
		{
			OutError.Set(TEXT("pcg_ai_node_graph_pcg_settings_class_invalid"));
			return false;
		}
		UPCGSettings* DefaultSettings = nullptr;
		OutNode = Graph.AddNodeOfType(&SettingsClass, DefaultSettings);
		if (!OutNode || !DefaultSettings)
		{
			OutNode = nullptr;
			OutError.Set(TEXT("pcg_ai_node_graph_pcg_node_add_failed"));
			return false;
		}
		OutNode->UpdateAfterSettingsChangeDuringCreation();
		const FString Revision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s"), *OutNode->GetPathName(&Graph),
			*DefaultSettings->GetClass()->GetPathName()));
		OutReceipt.SetEvidenceId(MakeEvidence(TEXT("apply"), TEXT("pcg_node"), Revision));
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TryConnectPcgNodes(
		UPCGGraph& Graph,
		UPCGNode& FromNode,
		const FName FromPinLabel,
		UPCGNode& ToNode,
		const FName ToPinLabel,
		FPcgAiNodeGraphNativeMutationReceipt& OutReceipt,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutReceipt.SetEvidenceId(FString());
		if (FromPinLabel.IsNone() || ToPinLabel.IsNone())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_pcg_edge_pin_missing"));
			return false;
		}
		const UPCGPin* FromPin = FromNode.GetOutputPin(FromPinLabel);
		const UPCGPin* ToPin = ToNode.GetInputPin(ToPinLabel);
		if (!FromPin || !ToPin)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_pcg_edge_pin_unresolved"));
			return false;
		}
		Graph.AddLabeledEdge(&FromNode, FromPinLabel, &ToNode, ToPinLabel);
		const bool bConnected = FromPin->Edges.ContainsByPredicate(
			[FromPin, ToPin](const UPCGEdge* Edge)
			{
				return Edge && Edge->InputPin == FromPin
					&& Edge->OutputPin == ToPin;
			});
		if (!bConnected)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_pcg_edge_add_failed"));
			return false;
		}
		const FString Revision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s|%s"),
			*FromNode.GetPathName(&Graph), *FromPinLabel.ToString(),
			*ToNode.GetPathName(&Graph), *ToPinLabel.ToString()));
		OutReceipt.SetEvidenceId(MakeEvidence(TEXT("apply"), TEXT("pcg_edge"), Revision));
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TrySetPcgDoubleParameter(
		UPCGGraph& Graph,
		const FGuid& PropertyGuid,
		const FName PropertyName,
		const double Value,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphParameterIdentity& OutIdentity,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutApplyReceipt.SetEvidenceId(FString());
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutIdentity = FPcgAiNodeGraphParameterIdentity();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		if (!PropertyGuid.IsValid() || PropertyName.IsNone())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_pcg_parameter_identity_invalid"));
			return false;
		}
		bool bIdentityAccepted = false;
		bool bSet = false;
		Graph.UpdateUserParametersStruct(
			[&](FInstancedPropertyBag& Bag)
			{
				const FPropertyBagPropertyDesc* const ExistingById =
					Bag.FindPropertyDescByID(PropertyGuid);
				const FPropertyBagPropertyDesc* const ExistingByName =
					Bag.FindPropertyDescByName(PropertyName);
				if (ExistingById)
				{
					bIdentityAccepted = ExistingById->Name == PropertyName
						&& ExistingById->ValueType
							== EPropertyBagPropertyType::Double
						&& (!ExistingByName || ExistingByName->ID == PropertyGuid);
				}
				else if (!ExistingByName)
				{
					FPropertyBagPropertyDesc Desc(
						PropertyName, EPropertyBagPropertyType::Double);
					Desc.ID = PropertyGuid;
					bIdentityAccepted = Bag.AddProperties({Desc})
						== EPropertyBagAlterationResult::Success;
				}
				bSet = bIdentityAccepted
					&& Bag.SetValueDouble(PropertyName, Value)
					== EPropertyBagResult::Success;
			});
		if (!bIdentityAccepted || !bSet)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_pcg_parameter_write_failed"));
			return false;
		}
		const FString ApplyRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s|%.17g"),
			*Graph.GetPathName(), *ToGuidString(PropertyGuid),
			*PropertyName.ToString(), Value));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(
			TEXT("apply"), TEXT("pcg_parameter"), ApplyRevision));
		const FInstancedPropertyBag* ReadbackBag = Graph.GetUserParametersStruct();
		const FPropertyBagPropertyDesc* ReadbackDesc = ReadbackBag
			? ReadbackBag->FindPropertyDescByID(PropertyGuid) : nullptr;
		double ReadbackDouble = 0.0;
		bool bValueRead = false;
		if (ReadbackBag)
		{
			const TValueOrError<double, EPropertyBagResult> ReadbackValue =
				ReadbackBag->GetValueDouble(PropertyName);
			if (ReadbackValue.IsValid())
			{
				ReadbackDouble = ReadbackValue.GetValue();
				bValueRead = true;
			}
		}
		const bool bRefreshed = ReadbackDesc
			&& ReadbackDesc->Name == PropertyName
			&& ReadbackDesc->ValueType == EPropertyBagPropertyType::Double
			&& bValueRead
			&& FMath::IsNearlyEqual(ReadbackDouble, Value);
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Refresh;
		OutBarrierReceipt.bComplete = bRefreshed;
		OutBarrierReceipt.ErrorCount = bRefreshed ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("pcg_parameter"), ApplyRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("pcg_parameter"), ApplyRevision);
		if (!bRefreshed)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_pcg_parameter_refresh_incomplete"));
			return false;
		}
		OutRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s|%s|%.17g"),
			*Graph.GetPathName(), *ToGuidString(ReadbackDesc->ID),
			*ReadbackDesc->Name.ToString(),
			*PropertyBagTypeName(ReadbackDesc->ValueType),
			ReadbackDouble));
		OutIdentity = FPcgAiNodeGraphParameterIdentity(
			Graph.GetPathName(),
			ReadbackDesc->ID,
			ReadbackDesc->Name,
			PropertyBagTypeName(ReadbackDesc->ValueType));
		if (!TryValidatePcgAiNodeGraphParameterIdentity(OutIdentity, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(
			TEXT("readback"), TEXT("pcg_parameter"), OutRevision);
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TryCompileAndCapturePcgGraph(
		UPCGGraph& Graph,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphTopology& OutTopology,
		TArray<FPcgAiNodeGraphParameterIdentity>& OutParameters,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutTopology = FPcgAiNodeGraphTopology();
		OutParameters.Reset();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		const bool bCompiled = Graph.Recompile();
		const FString BarrierRevision = HashCanonicalText(
			Graph.GetPathName() + TEXT("|compile"));
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Compile;
		OutBarrierReceipt.bComplete = bCompiled;
		OutBarrierReceipt.ErrorCount = bCompiled ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("pcg_graph"), BarrierRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("pcg_graph"), BarrierRevision);
		if (!bCompiled)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_pcg_compile_incomplete"));
			return false;
		}
		if (!CapturePcgGraph(
			Graph, OutTopology, OutParameters, OutRevision, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(TEXT("readback"), TEXT("pcg_graph"), OutRevision);
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TryBindAndCapturePcgComponent(
		UPCGComponent& Component,
		UPCGGraphInterface& Graph,
		const FString& GenerationPolicy,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphComponentBindingIdentity& OutIdentity,
		FString& OutRevision,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutApplyReceipt.SetEvidenceId(FString());
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutIdentity = FPcgAiNodeGraphComponentBindingIdentity();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		EPCGComponentGenerationTrigger NativePolicy;
		if (GenerationPolicy == TEXT("generate_on_demand"))
		{
			NativePolicy = EPCGComponentGenerationTrigger::GenerateOnDemand;
		}
		else if (GenerationPolicy == TEXT("generate_on_load"))
		{
			NativePolicy = EPCGComponentGenerationTrigger::GenerateOnLoad;
		}
		else if (GenerationPolicy == TEXT("generate_at_runtime"))
		{
			NativePolicy = EPCGComponentGenerationTrigger::GenerateAtRuntime;
		}
		else
		{
			OutError.Set(TEXT("pcg_ai_node_graph_component_generation_policy_invalid"));
			return false;
		}
		AActor* Owner = Component.GetOwner();
		if (!Owner || !Graph.GetGraph())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_component_binding_target_invalid"));
			return false;
		}
		Component.GenerationTrigger = NativePolicy;
		Component.SetGraph(&Graph);
		const FString ApplyRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s"), *Component.GetPathName(),
			*Graph.GetPathName(), *GenerationPolicy));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(
			TEXT("apply"), TEXT("pcg_component"), ApplyRevision));
		const bool bRefreshed = Component.GetGraph() == Graph.GetGraph()
			&& Component.GenerationTrigger == NativePolicy;
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Refresh;
		OutBarrierReceipt.bComplete = bRefreshed;
		OutBarrierReceipt.ErrorCount = bRefreshed ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("pcg_component"), ApplyRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("pcg_component"), ApplyRevision);
		if (!bRefreshed)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_component_refresh_incomplete"));
			return false;
		}
		OutRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s|%s"), *Owner->GetPathName(),
			*Component.GetPathName(), *Component.GetGraph()->GetPathName(),
			*GenerationPolicy));
		OutIdentity = FPcgAiNodeGraphComponentBindingIdentity(
			Owner->GetPathName(),
			Component.GetPathName(),
			Component.GetGraph()->GetPathName(),
			GenerationPolicy);
		if (!TryValidatePcgAiNodeGraphComponentBindingIdentity(OutIdentity, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(TEXT("readback"), TEXT("pcg_component"), OutRevision);
		return true;
	}

	bool FPcgAiNodeGraphNativeHook::TrySetAndCapturePcgDataAsset(
		UPCGDataAsset& DataAsset,
		UPCGData& Data,
		const FSoftObjectPath& SourceIdentity,
		FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
		FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
		FPcgAiNodeGraphDataAssetIdentity& OutIdentity,
		FString& OutEvidenceId,
		FPcgAiNodeGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutApplyReceipt.SetEvidenceId(FString());
		OutBarrierReceipt = FPcgAiNodeGraphBarrierReceipt();
		OutIdentity = FPcgAiNodeGraphDataAssetIdentity();
		OutEvidenceId.Reset();
		if (!SourceIdentity.IsValid())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_data_asset_source_invalid"));
			return false;
		}
		DataAsset.Data.Reset();
		FPCGTaggedData TaggedData;
		TaggedData.Data = &Data;
		DataAsset.Data.TaggedData.Add(MoveTemp(TaggedData));
#if WITH_EDITORONLY_DATA
		DataAsset.ObjectPath = SourceIdentity;
#endif
		const FString ApplyRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s"), *DataAsset.GetPathName(),
			*Data.GetClass()->GetPathName(), *SourceIdentity.ToString()));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(
			TEXT("apply"), TEXT("pcg_data_asset"), ApplyRevision));
		DataAsset.Data.ComputeCrcs(true);
		const bool bValidated = DataAsset.Data.TaggedData.Num() == 1
			&& DataAsset.Data.TaggedData[0].Data.Get() == &Data
#if WITH_EDITORONLY_DATA
			&& DataAsset.ObjectPath == SourceIdentity
#endif
			;
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Validate;
		OutBarrierReceipt.bComplete = bValidated;
		OutBarrierReceipt.ErrorCount = bValidated ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("pcg_data_asset"), ApplyRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("pcg_data_asset"), ApplyRevision);
		if (!bValidated)
		{
			OutError.Set(TEXT("pcg_ai_node_graph_data_asset_validate_incomplete"));
			return false;
		}
		const UPCGData* ReadbackData = DataAsset.Data.TaggedData[0].Data.Get();
		const FString ContentRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s"), *DataAsset.GetPathName(),
			ReadbackData ? *ReadbackData->GetClass()->GetPathName() : TEXT("null"),
			*SourceIdentity.ToString()));
		OutIdentity = FPcgAiNodeGraphDataAssetIdentity(
			DataAsset.GetPathName(),
			ReadbackData ? ReadbackData->GetClass()->GetPathName() : FString(),
			SourceIdentity.ToString(),
			ContentRevision);
		if (!TryValidatePcgAiNodeGraphDataAssetIdentity(OutIdentity, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(
			TEXT("readback"), TEXT("pcg_data_asset"), ContentRevision);
		return true;
	}
}
