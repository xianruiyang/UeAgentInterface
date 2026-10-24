#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphNativeHook.h"

#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Logging/TokenizedMessage.h"
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
#include "UObject/Class.h"

#include <openssl/sha.h>

namespace UE::UAI::DomainAdapters::PcgAiNodeGraph
{
	namespace
	{
		Contract::FDiagnostic NativeGraphFact(const TCHAR* Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_adapter"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.TargetPath = Target;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FString GenerationPolicyName(const EPCGComponentGenerationTrigger Policy)
		{
			switch (Policy)
			{
			case EPCGComponentGenerationTrigger::GenerateOnDemand: return TEXT("generate_on_demand");
			case EPCGComponentGenerationTrigger::GenerateOnLoad: return TEXT("generate_on_load");
			case EPCGComponentGenerationTrigger::GenerateAtRuntime: return TEXT("generate_at_runtime");
			default: return FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Policy));
			}
		}

		TSharedPtr<FJsonValue> NativeDoubleFact(const double Value)
		{
			if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
			return MakeShared<FJsonValueString>(FString::Printf(TEXT("non-finite double: %.17g"), Value));
		}

		struct FDiagnosticStateTreeCompilerLog final : FStateTreeCompilerLog
		{
			const TArray<FStateTreeCompilerLogMessage>& GetMessages() const { return Messages; }
		};

		void AppendStateTreeCompilerDiagnostics(const FDiagnosticStateTreeCompilerLog& Log,
			const FString& Target, Contract::FDiagnostic& OutError)
		{
			int32 MessageIndex = 0;
			for (const FStateTreeCompilerLogMessage& SourceMessage : Log.GetMessages())
			{
				auto Diagnostic = NativeGraphFact(TEXT("pcg_ai_node_graph_state_tree_compiler_message"),
					SourceMessage.Message.IsEmpty() ? TEXT("The StateTree compiler recorded an empty message.") : SourceMessage.Message,
					FString::Printf(TEXT("/state_tree/compiler/messages/%d"), MessageIndex++), nullptr, nullptr, Target);
				if (SourceMessage.Severity == EMessageSeverity::Warning
					|| SourceMessage.Severity == EMessageSeverity::PerformanceWarning)
					Diagnostic.Severity = Contract::ESeverity::Warning;
				else if (SourceMessage.Severity == EMessageSeverity::Info)
					Diagnostic.Severity = Contract::ESeverity::Info;
				const UStateTreeState* State = SourceMessage.State.Get();
				if (State) Diagnostic.CandidateName = State->Name.ToString();
				const auto PrivateContext = MakeShared<FJsonObject>();
				PrivateContext->SetNumberField(TEXT("native_severity"), SourceMessage.Severity);
				if (State) PrivateContext->SetStringField(TEXT("state_guid"), State->ID.ToString());
				if (SourceMessage.Item.ID.IsValid())
				{
					const auto Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), SourceMessage.Item.Name.ToString());
					Item->SetStringField(TEXT("kind"), UEnum::GetValueAsString(SourceMessage.Item.DataSource));
					Diagnostic.ActualValuePresent = true;
					Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Item);
					PrivateContext->SetStringField(TEXT("item_guid"), SourceMessage.Item.ID.ToString());
				}
				Diagnostic.ActualEvidence = MakeShared<FJsonValueObject>(PrivateContext);
				OutError.Append(Diagnostic);
			}
		}

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
			if (AssetKind.IsEmpty())
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_create_input_invalid"), TEXT("Native asset creation requires a non-empty asset kind."), TEXT("/asset/kind"),
					MakeShared<FJsonValueString>(AssetKind), MakeShared<FJsonValueString>(TEXT("non-empty string")), Object.GetPathName());
			}
			else if (ObjectName.IsNone())
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_create_input_invalid"), TEXT("Native asset creation requires a non-empty object name."), TEXT("/asset/name"),
					MakeShared<FJsonValueString>(ObjectName.ToString()), MakeShared<FJsonValueString>(TEXT("non-empty object name")), Object.GetPathName());
			}
			else if (AssetClass.HasAnyClassFlags(CLASS_Abstract))
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_create_input_invalid"), TEXT("An abstract native asset class cannot be instantiated."), TEXT("/asset/class_abstract"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Object.GetPathName());
				OutError.CandidateName = AssetClass.GetPathName();
			}
			else 
			{
				if (Object.GetOuter() != &Outer)
				{
					OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_create_input_invalid"), TEXT("The native asset belongs to a different outer object."), TEXT("/asset/outer"),
						MakeShared<FJsonValueString>(Object.GetOuter() ? Object.GetOuter()->GetPathName() : FString()), MakeShared<FJsonValueString>(Outer.GetPathName()), Object.GetPathName());
				}
				else if (Object.GetFName() != ObjectName)
				{
					OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_create_input_invalid"), TEXT("The native asset name differs from the requested name."), TEXT("/asset/name"),
						MakeShared<FJsonValueString>(Object.GetFName().ToString()), MakeShared<FJsonValueString>(ObjectName.ToString()), Object.GetPathName());
				}
				else 
				{
					OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_create_input_invalid"), TEXT("The native asset class differs from the requested class."), TEXT("/asset/class"),
						MakeShared<FJsonValueString>(Object.GetClass()->GetPathName()), MakeShared<FJsonValueString>(AssetClass.GetPathName()), Object.GetPathName());
				}
			}
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
			if (Object.GetOuter() != &Outer)
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_refresh_incomplete"), TEXT("The native asset belongs to a different outer object."), TEXT("/asset/outer"),
					MakeShared<FJsonValueString>(Object.GetOuter() ? Object.GetOuter()->GetPathName() : FString()), MakeShared<FJsonValueString>(Outer.GetPathName()), Object.GetPathName());
			}
			else if (Object.GetFName() != ObjectName)
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_refresh_incomplete"), TEXT("The native asset name differs from the requested name."), TEXT("/asset/name"),
					MakeShared<FJsonValueString>(Object.GetFName().ToString()), MakeShared<FJsonValueString>(ObjectName.ToString()), Object.GetPathName());
			}
			else 
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_refresh_incomplete"), TEXT("The native asset class differs from the requested class."), TEXT("/asset/class"),
					MakeShared<FJsonValueString>(Object.GetClass()->GetPathName()), MakeShared<FJsonValueString>(AssetClass.GetPathName()), Object.GetPathName());
			}
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
			if (AssetKind.IsEmpty())
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_create_input_invalid"), TEXT("Native asset creation requires a non-empty asset kind."), TEXT("/asset/kind"),
					MakeShared<FJsonValueString>(AssetKind), MakeShared<FJsonValueString>(TEXT("non-empty string")), Outer.GetPathName());
			}
			else if (ObjectName.IsNone())
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_create_input_invalid"), TEXT("Native asset creation requires a non-empty object name."), TEXT("/asset/name"),
					MakeShared<FJsonValueString>(ObjectName.ToString()), MakeShared<FJsonValueString>(TEXT("non-empty object name")), Outer.GetPathName());
			}
			else 
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_create_input_invalid"), TEXT("An abstract native asset class cannot be instantiated."), TEXT("/asset/class_abstract"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Outer.GetPathName());
				OutError.CandidateName = AssetClass.GetPathName();
			}
			return false;
		}
		OutObject = NewObject<UObject>(
			&Outer, &AssetClass, ObjectName, RF_Transactional);
		if (!OutObject || OutObject->GetClass() != &AssetClass)
		{
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_asset_create_failed"), !OutObject ? TEXT("The native asset object could not be created.") : TEXT("The created native asset has an unexpected class."), !OutObject ? TEXT("/asset/object_available") : TEXT("/asset/class"),
				!OutObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(OutObject->GetClass()->GetPathName())), !OutObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(AssetClass.GetPathName())), OutObject ? OutObject->GetPathName() : Outer.GetPathName());
			OutError.CandidateName = ObjectName.ToString();
			OutObject = nullptr;
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
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_behavior_tree_graph_missing"), Tree.BTGraph ? TEXT("The Behavior Tree graph has an unexpected class.") : TEXT("The Behavior Tree asset has no editor graph."), TEXT("/behavior_tree/graph_class"),
				Tree.BTGraph ? MakeShared<FJsonValueString>(Tree.BTGraph->GetClass()->GetPathName()) : TSharedPtr<FJsonValueString>(), MakeShared<FJsonValueString>(UBehaviorTreeGraph::StaticClass()->GetPathName()), Tree.GetPathName());
			return false;
		}
		const FString Before = HashCanonicalText(Tree.GetPathName() + TEXT("|before_compile"));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(TEXT("apply"), TEXT("behavior_tree"), Before));
		Graph->UpdateAsset();
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Compile;
		// UpdateAsset synchronously settles the editor graph, including disconnected
		// drafts. A runtime root is required to run the tree, not to save its edits.
		OutBarrierReceipt.bComplete = true;
		OutBarrierReceipt.ErrorCount = 0;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("behavior_tree"), Before);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("behavior_tree"), Before);
		if (!CaptureEdGraphTopology(
			TEXT("behavior_tree"), Tree, *Graph,
			OutTopology, OutRevision, OutError))
		{
			return false;
		}
		OutEvidenceId = MakeEvidence(TEXT("readback"), TEXT("behavior_tree"), OutRevision);
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
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_eqs_graph_missing"), Query.EdGraph ? TEXT("The EQS graph has an unexpected class.") : TEXT("The EQS asset has no editor graph."), TEXT("/eqs/graph_class"),
				Query.EdGraph ? MakeShared<FJsonValueString>(Query.EdGraph->GetClass()->GetPathName()) : TSharedPtr<FJsonValueString>(), MakeShared<FJsonValueString>(UEnvironmentQueryGraph::StaticClass()->GetPathName()), Query.GetPathName());
			return false;
		}
		const FString Before = HashCanonicalText(Query.GetPathName() + TEXT("|before_compile"));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(TEXT("apply"), TEXT("eqs"), Before));
		Graph->UpdateAsset();
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Compile;
		// Empty runtime options are a valid projection of an unfinished editor graph.
		// UpdateAsset has completed even when the query is not ready to execute.
		OutBarrierReceipt.bComplete = true;
		OutBarrierReceipt.ErrorCount = 0;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("eqs"), Before);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("eqs"), Before);
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
			if (RootStateName.IsNone())
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_state_tree_root_name_missing"), TEXT("The StateTree root state requires a name."), TEXT("/state_tree/root_name"),
					MakeShared<FJsonValueString>(RootStateName.ToString()), MakeShared<FJsonValueString>(TEXT("non-empty state name")), Tree.GetPathName());
			}
			else if (ChildStateName.IsNone())
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_state_tree_root_name_missing"), TEXT("The StateTree child state requires a name."), TEXT("/state_tree/child_name"),
					MakeShared<FJsonValueString>(ChildStateName.ToString()), MakeShared<FJsonValueString>(TEXT("non-empty state name")), Tree.GetPathName());
			}
			else 
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_state_tree_root_name_missing"), TEXT("The StateTree transition GUID is invalid."), TEXT("/state_tree/transition_guid_valid"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Tree.GetPathName());
				OutError.InternalContext = ToGuidString(TransitionGuid);
			}
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
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_state_tree_root_invalid"), TEXT("The existing StateTree root entry has no state object."), TEXT("/state_tree/roots/0/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Tree.GetPathName());
				OutError.CandidateName = RootStateName.ToString();
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
		OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_state_tree_editor_data_unavailable"), TEXT("StateTree editor data is unavailable in this build."), TEXT("/state_tree/editor_data_supported"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Tree.GetPathName());
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
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_state_tree_editor_data_unavailable"), Tree.EditorData ? TEXT("The StateTree editor data has an unexpected class.") : TEXT("The StateTree asset has no editor data."), TEXT("/state_tree/editor_data_class"),
				Tree.EditorData ? MakeShared<FJsonValueString>(Tree.EditorData->GetClass()->GetPathName()) : TSharedPtr<FJsonValueString>(), MakeShared<FJsonValueString>(UStateTreeEditorData::StaticClass()->GetPathName()), Tree.GetPathName());
			return false;
		}
		const FString ApplyRevision = HashCanonicalText(
			Tree.GetPathName() + TEXT("|compile_current"));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(
			TEXT("apply"), TEXT("state_tree"), ApplyRevision));
		FDiagnosticStateTreeCompilerLog CompileLog;
		const bool bCompiled = UStateTreeEditingSubsystem::CompileStateTree(
			&Tree, CompileLog);
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Compile;
		// The native compiler result is authoritative; runtime readiness also depends
		// on executable states and must not reject an otherwise compiled editor draft.
		OutBarrierReceipt.bComplete = bCompiled;
		OutBarrierReceipt.ErrorCount = OutBarrierReceipt.bComplete ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("state_tree"), ApplyRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("state_tree"), ApplyRevision);
		if (!OutBarrierReceipt.bComplete)
		{
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_state_tree_compile_incomplete"),
				TEXT("The StateTree compiler rejected the asset."),
				TEXT("/state_tree/compile_succeeded"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Tree.GetPathName());
			AppendStateTreeCompilerDiagnostics(CompileLog, Tree.GetPathName(), OutError);
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
		OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_state_tree_editor_data_unavailable"), TEXT("StateTree editor data is unavailable in this build."), TEXT("/state_tree/editor_data_supported"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Tree.GetPathName());
		return false;
#endif
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
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_settings_class_invalid"), !SettingsClass.IsChildOf(UPCGSettings::StaticClass()) ? TEXT("The PCG node settings class is not derived from PCGSettings.") : TEXT("An abstract PCG settings class cannot be instantiated."), !SettingsClass.IsChildOf(UPCGSettings::StaticClass()) ? TEXT("/pcg_node/settings_class") : TEXT("/pcg_node/settings_class_abstract"),
				!SettingsClass.IsChildOf(UPCGSettings::StaticClass()) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(SettingsClass.GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)), !SettingsClass.IsChildOf(UPCGSettings::StaticClass()) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("subclass of /Script/PCG.PCGSettings"))) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)), Graph.GetPathName());
			OutError.CandidateName = SettingsClass.GetPathName();
			return false;
		}
		UPCGSettings* DefaultSettings = nullptr;
		OutNode = Graph.AddNodeOfType(&SettingsClass, DefaultSettings);
		if (!OutNode || !DefaultSettings)
		{
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_node_add_failed"), !OutNode ? TEXT("The PCG graph did not create the requested node.") : TEXT("The created PCG node has no default settings object."), !OutNode ? TEXT("/pcg_node/node_created") : TEXT("/pcg_node/default_settings_created"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetPathName());
			OutError.CandidateName = SettingsClass.GetPathName();
			OutNode = nullptr;
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
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_edge_pin_missing"), FromPinLabel.IsNone() ? TEXT("The PCG edge requires a source output pin label.") : TEXT("The PCG edge requires a destination input pin label."), FromPinLabel.IsNone() ? TEXT("/pcg_edge/from_pin") : TEXT("/pcg_edge/to_pin"),
				MakeShared<FJsonValueString>(FromPinLabel.IsNone() ? FromPinLabel.ToString() : ToPinLabel.ToString()), MakeShared<FJsonValueString>(TEXT("non-empty pin label")), Graph.GetPathName());
			return false;
		}
		const UPCGPin* FromPin = FromNode.GetOutputPin(FromPinLabel);
		const UPCGPin* ToPin = ToNode.GetInputPin(ToPinLabel);
		if (!FromPin || !ToPin)
		{
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_edge_pin_unresolved"), !FromPin ? TEXT("The PCG edge source output pin does not resolve.") : TEXT("The PCG edge destination input pin does not resolve."), !FromPin ? TEXT("/pcg_edge/from_pin") : TEXT("/pcg_edge/to_pin"),
				MakeShared<FJsonValueString>(!FromPin ? FromPinLabel.ToString() : ToPinLabel.ToString()), MakeShared<FJsonValueString>(!FromPin ? TEXT("existing output pin") : TEXT("existing input pin")), Graph.GetPathName());
			OutError.InternalContext = FromNode.GetPathName(&Graph) + TEXT(" -> ") + ToNode.GetPathName(&Graph);
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
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_edge_add_failed"), TEXT("The PCG edge was not present in native readback after adding it."), TEXT("/pcg_edge/connected"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetPathName());
			OutError.CandidateName = FromPinLabel.ToString() + TEXT(" -> ") + ToPinLabel.ToString();
			OutError.InternalContext = FromNode.GetPathName(&Graph) + TEXT(" -> ") + ToNode.GetPathName(&Graph);
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
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_identity_invalid"),
				!PropertyGuid.IsValid() ? TEXT("The PCG parameter property GUID is invalid.") : TEXT("The PCG parameter requires a property name."),
				!PropertyGuid.IsValid() ? TEXT("/pcg_parameter/property_guid_valid") : TEXT("/pcg_parameter/name_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetPathName());
			OutError.CandidateName = PropertyName.IsNone() ? FString() : PropertyName.ToString();
			OutError.InternalContext = ToGuidString(PropertyGuid);
			return false;
		}
		bool bIdentityAccepted = false;
		bool bSet = false;
		bool bBagVisited = false;
		bool bParameterCreated = false;
		Graph.UpdateUserParametersStruct(
			[&](FInstancedPropertyBag& Bag)
			{
				bBagVisited = true;
				const FPropertyBagPropertyDesc* const ExistingById = Bag.FindPropertyDescByID(PropertyGuid);
				const FPropertyBagPropertyDesc* const ExistingByName = Bag.FindPropertyDescByName(PropertyName);
				if (ExistingById)
				{
					bIdentityAccepted = ExistingById->Name == PropertyName
						&& ExistingById->ValueType == EPropertyBagPropertyType::Double
						&& (!ExistingByName || ExistingByName->ID == PropertyGuid);
					if (!bIdentityAccepted)
					{
						if (ExistingById->Name != PropertyName)
						{
							OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_write_failed"),
								TEXT("The PCG property GUID resolves to a different parameter name."), TEXT("/pcg_parameter/name"),
								MakeShared<FJsonValueString>(ExistingById->Name.ToString()),
								MakeShared<FJsonValueString>(PropertyName.ToString()), Graph.GetPathName());
						}
						else if (ExistingById->ValueType != EPropertyBagPropertyType::Double)
						{
							OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_write_failed"),
								TEXT("The PCG parameter has a different property type."), TEXT("/pcg_parameter/type"),
								MakeShared<FJsonValueString>(PropertyBagTypeName(ExistingById->ValueType)),
								MakeShared<FJsonValueString>(PropertyBagTypeName(EPropertyBagPropertyType::Double)), Graph.GetPathName());
						}
						else
						{
							OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_write_failed"),
								TEXT("The PCG parameter name is bound to a different property GUID."), TEXT("/pcg_parameter/property_identity_matches"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetPathName());
							OutError.ActualEvidence = MakeShared<FJsonValueString>(ToGuidString(ExistingByName->ID));
							OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ToGuidString(PropertyGuid));
						}
					}
				}
				else if (!ExistingByName)
				{
					FPropertyBagPropertyDesc Desc(PropertyName, EPropertyBagPropertyType::Double);
					Desc.ID = PropertyGuid;
					const EPropertyBagAlterationResult AddResult = Bag.AddProperties({Desc});
					bIdentityAccepted = AddResult == EPropertyBagAlterationResult::Success;
					bParameterCreated = bIdentityAccepted;
					if (!bIdentityAccepted)
					{
						OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_write_failed"),
							TEXT("The PCG parameter bag rejected the new double property."), TEXT("/pcg_parameter/add_result"),
							MakeShared<FJsonValueString>(UEnum::GetValueAsString(AddResult)),
							MakeShared<FJsonValueString>(UEnum::GetValueAsString(EPropertyBagAlterationResult::Success)), Graph.GetPathName());
					}
				}
				else
				{
					OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_write_failed"),
						TEXT("An existing PCG parameter name is already bound to another property GUID."), TEXT("/pcg_parameter/property_identity_matches"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetPathName());
					OutError.ActualEvidence = MakeShared<FJsonValueString>(ToGuidString(ExistingByName->ID));
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ToGuidString(PropertyGuid));
				}
				if (bIdentityAccepted)
				{
					const EPropertyBagResult SetResult = Bag.SetValueDouble(PropertyName, Value);
					bSet = SetResult == EPropertyBagResult::Success;
					if (!bSet)
					{
						OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_write_failed"),
							TEXT("The PCG parameter bag rejected the double value."), TEXT("/pcg_parameter/set_result"),
							MakeShared<FJsonValueString>(UEnum::GetValueAsString(SetResult)),
							MakeShared<FJsonValueString>(UEnum::GetValueAsString(EPropertyBagResult::Success)), Graph.GetPathName());
						OutError.ProposedValue = NativeDoubleFact(Value);
					}
				}
			});
		if (!bIdentityAccepted || !bSet)
		{
			if (OutError.IsEmpty())
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_write_failed"),
					!bBagVisited ? TEXT("The PCG graph did not expose an editable parameter bag.") : TEXT("The PCG parameter update did not complete."),
					!bBagVisited ? TEXT("/pcg_parameter/bag_available") : TEXT("/pcg_parameter/write_completed"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetPathName());
			}
			OutError.CandidateName = PropertyName.ToString();
			if (bParameterCreated) OutError.MutationStarted = true;
			return false;
		}
		const FString ApplyRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s|%.17g"), *Graph.GetPathName(), *ToGuidString(PropertyGuid), *PropertyName.ToString(), Value));
		OutApplyReceipt.SetEvidenceId(MakeEvidence(TEXT("apply"), TEXT("pcg_parameter"), ApplyRevision));
		const FInstancedPropertyBag* ReadbackBag = Graph.GetUserParametersStruct();
		const FPropertyBagPropertyDesc* ReadbackDesc = ReadbackBag ? ReadbackBag->FindPropertyDescByID(PropertyGuid) : nullptr;
		double ReadbackDouble = 0.0;
		bool bValueRead = false;
		TOptional<EPropertyBagResult> ReadbackError;
		if (ReadbackBag)
		{
			const TValueOrError<double, EPropertyBagResult> ReadbackValue = ReadbackBag->GetValueDouble(PropertyName);
			if (ReadbackValue.IsValid())
			{
				ReadbackDouble = ReadbackValue.GetValue();
				bValueRead = true;
			}
			else if (const EPropertyBagResult* Error = ReadbackValue.TryGetError())
			{
				ReadbackError = *Error;
			}
		}
		const bool bRefreshed = ReadbackDesc && ReadbackDesc->Name == PropertyName
			&& ReadbackDesc->ValueType == EPropertyBagPropertyType::Double
			&& bValueRead && FMath::IsNearlyEqual(ReadbackDouble, Value);
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Refresh;
		OutBarrierReceipt.bComplete = bRefreshed;
		OutBarrierReceipt.ErrorCount = bRefreshed ? 0 : 1;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(TEXT("barrier"), TEXT("pcg_parameter"), ApplyRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(TEXT("settle"), TEXT("pcg_parameter"), ApplyRevision);
		if (!bRefreshed)
		{
			if (!ReadbackBag || !ReadbackDesc)
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_refresh_incomplete"),
					!ReadbackBag ? TEXT("The PCG parameter bag is unavailable during readback.") : TEXT("The PCG property GUID does not resolve during readback."),
					!ReadbackBag ? TEXT("/pcg_parameter/readback_bag_available") : TEXT("/pcg_parameter/property_guid_resolved"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetPathName());
				OutError.InternalContext = ToGuidString(PropertyGuid);
			}
			else if (ReadbackDesc->Name != PropertyName)
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_refresh_incomplete"),
					TEXT("The PCG parameter readback name differs from the requested name."), TEXT("/pcg_parameter/name"),
					MakeShared<FJsonValueString>(ReadbackDesc->Name.ToString()), MakeShared<FJsonValueString>(PropertyName.ToString()), Graph.GetPathName());
			}
			else if (ReadbackDesc->ValueType != EPropertyBagPropertyType::Double)
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_refresh_incomplete"),
					TEXT("The PCG parameter readback has a different property type."), TEXT("/pcg_parameter/type"),
					MakeShared<FJsonValueString>(PropertyBagTypeName(ReadbackDesc->ValueType)),
					MakeShared<FJsonValueString>(PropertyBagTypeName(EPropertyBagPropertyType::Double)), Graph.GetPathName());
			}
			else if (!bValueRead)
			{
				if (ReadbackError.IsSet())
				{
					OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_refresh_incomplete"),
						TEXT("The PCG parameter bag could not read the double value."), TEXT("/pcg_parameter/read_result"),
						MakeShared<FJsonValueString>(UEnum::GetValueAsString(ReadbackError.GetValue())),
						MakeShared<FJsonValueString>(UEnum::GetValueAsString(EPropertyBagResult::Success)), Graph.GetPathName());
				}
				else
				{
					OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_refresh_incomplete"),
						TEXT("The PCG parameter readback returned neither a value nor an error."), TEXT("/pcg_parameter/readback_value_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetPathName());
				}
			}
			else
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_pcg_parameter_refresh_incomplete"),
					TEXT("The PCG double parameter readback is not nearly equal to the requested value."), TEXT("/pcg_parameter/value"),
					NativeDoubleFact(ReadbackDouble), NativeDoubleFact(Value), Graph.GetPathName());
			}
			OutError.CandidateName = PropertyName.ToString();
			OutError.MutationStarted = true;
			return false;
		}
		OutRevision = HashCanonicalText(FString::Printf(
			TEXT("%s|%s|%s|%s|%.17g"), *Graph.GetPathName(), *ToGuidString(ReadbackDesc->ID),
			*ReadbackDesc->Name.ToString(), *PropertyBagTypeName(ReadbackDesc->ValueType), ReadbackDouble));
		OutIdentity = FPcgAiNodeGraphParameterIdentity(Graph.GetPathName(), ReadbackDesc->ID, ReadbackDesc->Name, PropertyBagTypeName(ReadbackDesc->ValueType));
		if (!TryValidatePcgAiNodeGraphParameterIdentity(OutIdentity, OutError)) return false;
		OutEvidenceId = MakeEvidence(TEXT("readback"), TEXT("pcg_parameter"), OutRevision);
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
		// Recompile returns whether compiled tasks changed, not compilation success.
		// Its synchronous completion settles this barrier; capture validates the graph below.
		Graph.Recompile();
		const FString BarrierRevision = HashCanonicalText(
			Graph.GetPathName() + TEXT("|compile"));
		OutBarrierReceipt.BarrierKind = EPcgAiNodeGraphBarrierKind::Compile;
		OutBarrierReceipt.bComplete = true;
		OutBarrierReceipt.ErrorCount = 0;
		OutBarrierReceipt.BarrierEvidenceId = MakeEvidence(
			TEXT("barrier"), TEXT("pcg_graph"), BarrierRevision);
		OutBarrierReceipt.SettleEvidenceId = MakeEvidence(
			TEXT("settle"), TEXT("pcg_graph"), BarrierRevision);
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
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_component_generation_policy_invalid"), TEXT("The PCG component generation policy is unsupported."), TEXT("/component_binding/generation_policy"),
				MakeShared<FJsonValueString>(GenerationPolicy), MakeShared<FJsonValueString>(TEXT("generate_on_demand | generate_on_load | generate_at_runtime")), Component.GetPathName());
			return false;
		}
		AActor* Owner = Component.GetOwner();
		if (!Owner || !Graph.GetGraph())
		{
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_component_binding_target_invalid"), !Owner ? TEXT("The PCG component has no owning actor.") : TEXT("The selected PCG graph interface has no graph."), !Owner ? TEXT("/component_binding/owner_available") : TEXT("/component_binding/graph_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Component.GetPathName());
			if (Owner) OutError.CandidateName = Graph.GetPathName();
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
			if (Component.GetGraph() != Graph.GetGraph())
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_component_refresh_incomplete"), TEXT("The PCG component readback graph differs from the assigned graph."), TEXT("/component_binding/graph"),
					Component.GetGraph() ? MakeShared<FJsonValueString>(Component.GetGraph()->GetPathName()) : TSharedPtr<FJsonValueString>(), MakeShared<FJsonValueString>(Graph.GetGraph()->GetPathName()), Component.GetPathName());
			}
			else
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_component_refresh_incomplete"), TEXT("The PCG component readback generation policy differs from the assigned policy."), TEXT("/component_binding/generation_policy"),
					MakeShared<FJsonValueString>(GenerationPolicyName(Component.GenerationTrigger)), MakeShared<FJsonValueString>(GenerationPolicy), Component.GetPathName());
			}
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
			OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_data_asset_source_invalid"), TEXT("The PCG data asset source is not a valid soft object reference."), TEXT("/data_asset/source"),
				MakeShared<FJsonValueString>(SourceIdentity.ToString()), MakeShared<FJsonValueString>(TEXT("valid soft object reference")), DataAsset.GetPathName());
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
			if (DataAsset.Data.TaggedData.Num() != 1)
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_data_asset_validate_incomplete"), TEXT("The PCG data asset readback must contain exactly one tagged data entry."), TEXT("/data_asset/tagged_data/count"),
					MakeShared<FJsonValueNumber>(DataAsset.Data.TaggedData.Num()), MakeShared<FJsonValueNumber>(1), DataAsset.GetPathName());
			}
			else if (DataAsset.Data.TaggedData[0].Data.Get() != &Data)
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_data_asset_validate_incomplete"), TEXT("The PCG data asset readback does not refer to the assigned data object."), TEXT("/data_asset/tagged_data/0/data_matches_source"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), DataAsset.GetPathName());
				const auto PrivateObjects = MakeShared<FJsonObject>();
				const UPCGData* Readback = DataAsset.Data.TaggedData[0].Data.Get();
				PrivateObjects->SetStringField(TEXT("actual"), Readback ? Readback->GetPathName() : FString());
				PrivateObjects->SetStringField(TEXT("expected"), Data.GetPathName());
				OutError.ActualEvidence = MakeShared<FJsonValueObject>(PrivateObjects);
			}
			#if WITH_EDITORONLY_DATA
			else
			{
				OutError = NativeGraphFact(TEXT("pcg_ai_node_graph_data_asset_validate_incomplete"), TEXT("The PCG data asset readback source reference differs from the assigned source."), TEXT("/data_asset/source"),
					MakeShared<FJsonValueString>(DataAsset.ObjectPath.ToString()), MakeShared<FJsonValueString>(SourceIdentity.ToString()), DataAsset.GetPathName());
			}
			#endif
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
