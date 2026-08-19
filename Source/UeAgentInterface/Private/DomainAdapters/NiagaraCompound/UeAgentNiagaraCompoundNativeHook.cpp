#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundNativeHook.h"

#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundDiscovery.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Domains/Niagara/UeAgentNiagaraEmitterCompileContext.h"
#include "Domains/Niagara/UeAgentNiagaraStackValidation.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"

namespace UE::UAI::DomainAdapters::NiagaraCompound
{
	namespace
	{
		using namespace ContextResource;
		using namespace Discovery;
		using namespace GenericEffect;
		using namespace GenericRead;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace StateSemantic;
		using namespace UeAgentNiagaraStageModuleAuthoring;

		const FString CatalogKind(TEXT("uai_niagara_compound"));
		const FString CatalogSourceId(TEXT("uai-niagara-compound-catalog"));
		const FString ReadPortId(TEXT("uai-niagara-compound-read"));

		class FCanonicalStateHasher final
		{
		public:
			void MixByte(const uint8 Byte)
			{
				Hash ^= Byte;
				Hash *= 1099511628211ull;
			}

			void MixInt64(const int64 Value)
			{
				const uint64 Bits = static_cast<uint64>(Value);
				for (int32 Offset = 0; Offset < 64; Offset += 8)
				{
					MixByte(
						static_cast<uint8>(Bits >> Offset));
				}
			}

			void MixString(const FString& Value)
			{
				const FTCHARToUTF8 Utf8(*Value);
				MixInt64(Utf8.Length());
				for (int32 Index = 0;
					Index < Utf8.Length();
					++Index)
				{
					MixByte(
						static_cast<uint8>(Utf8.Get()[Index]));
				}
			}

			int64 GetSequence() const
			{
				const int64 Sequence = static_cast<int64>(
					Hash & 0x001fffffffffffffull);
				return FMath::Max<int64>(1, Sequence);
			}

		private:
			uint64 Hash = 1469598103934665603ull;
		};

		struct FCanonicalHashItem
		{
			FUeAgentCanonicalValuePtr Value;
			FString ObjectKey;
			bool bIsObjectKey = false;

			static FCanonicalHashItem ForValue(
				const FUeAgentCanonicalValueRef& InValue)
			{
				FCanonicalHashItem Item;
				Item.Value = InValue;
				return Item;
			}

			static FCanonicalHashItem ForObjectKey(
				const FString& InObjectKey)
			{
				FCanonicalHashItem Item;
				Item.ObjectKey = InObjectKey;
				Item.bIsObjectKey = true;
				return Item;
			}
		};

		int64 CanonicalStateRevisionSequence(
			const FUeAgentCanonicalIdentity& Identity,
			const FUeAgentCanonicalValueRef& Root)
		{
			FCanonicalStateHasher Hasher;
			Hasher.MixString(Identity.GetCanonicalKey());
			TArray<FCanonicalHashItem> Stack;
			Stack.Add(FCanonicalHashItem::ForValue(Root));
			while (!Stack.IsEmpty())
			{
				FCanonicalHashItem Item =
					Stack.Pop(EAllowShrinking::No);
				if (Item.bIsObjectKey)
				{
					Hasher.MixByte(0xf1);
					Hasher.MixString(Item.ObjectKey);
					continue;
				}
				if (!Item.Value.IsValid())
				{
					Hasher.MixByte(0xff);
					continue;
				}
				const FUeAgentCanonicalValue& Value = *Item.Value;
				Hasher.MixByte(0xf0);
				Hasher.MixByte(
					static_cast<uint8>(Value.GetKind()));
				switch (Value.GetKind())
				{
				case EUeAgentValueKind::Null:
					break;
				case EUeAgentValueKind::Boolean:
					Hasher.MixByte(Value.GetBoolean() ? 1 : 0);
					break;
				case EUeAgentValueKind::String:
					Hasher.MixString(Value.GetText());
					break;
				case EUeAgentValueKind::Integer:
				case EUeAgentValueKind::Decimal:
					Hasher.MixString(Value.GetLexeme());
					break;
				case EUeAgentValueKind::Array:
				{
					const TArray<FUeAgentCanonicalValueRef>& Items =
						Value.GetArrayItems();
					Hasher.MixInt64(Items.Num());
					for (int32 Index = Items.Num() - 1;
						Index >= 0;
						--Index)
					{
						Stack.Add(
							FCanonicalHashItem::ForValue(
								Items[Index]));
					}
					break;
				}
				case EUeAgentValueKind::Object:
				{
					const TArray<TPair<
						FString,
						FUeAgentCanonicalValueRef>>& Items =
							Value.GetObjectItems();
					Hasher.MixInt64(Items.Num());
					TArray<int32> Order;
					Order.Reserve(Items.Num());
					for (int32 Index = 0;
						Index < Items.Num();
						++Index)
					{
						Order.Add(Index);
					}
					Order.Sort(
						[&Items](const int32 Left, const int32 Right)
						{
							return Items[Left].Key
								< Items[Right].Key;
						});
					for (int32 OrderIndex = Order.Num() - 1;
						OrderIndex >= 0;
						--OrderIndex)
					{
						const TPair<
							FString,
							FUeAgentCanonicalValueRef>& Field =
								Items[Order[OrderIndex]];
						Stack.Add(
							FCanonicalHashItem::ForValue(
								Field.Value));
						Stack.Add(
							FCanonicalHashItem::ForObjectKey(
								Field.Key));
					}
					break;
				}
				case EUeAgentValueKind::Temporal:
					if (Value.GetTemporal().IsValid())
					{
						const FUeAgentTemporalPoint& Temporal =
							*Value.GetTemporal();
						Hasher.MixString(Temporal.GetClockId());
						Hasher.MixString(Temporal.GetTick());
						Hasher.MixString(
							Temporal.GetRateNumerator());
						Hasher.MixString(
							Temporal.GetRateDenominator());
						Hasher.MixString(Temporal.GetState());
					}
					break;
				case EUeAgentValueKind::Spatial:
					if (Value.GetSpatial().IsValid())
					{
						const FUeAgentSpatialValue& Spatial =
							*Value.GetSpatial();
						Hasher.MixString(Spatial.GetSubject());
						Hasher.MixByte(
							static_cast<uint8>(
								Spatial.GetQuantityKind()));
						Hasher.MixString(
							Spatial.GetFrame().GetFrameId());
						Hasher.MixString(
							Spatial.GetFrame().GetBasisFrameId());
						Hasher.MixInt64(
							Spatial.GetFrame().GetDimension());
						Hasher.MixString(
							Spatial.GetFrame().GetUnit());
						for (const TArray<FString>& Row :
							Spatial.GetFrame().GetBasis())
						{
							Hasher.MixInt64(Row.Num());
							for (const FString& Component : Row)
							{
								Hasher.MixString(Component);
							}
						}
						for (const FString& Component :
							Spatial.GetFrame().GetOrigin())
						{
							Hasher.MixString(Component);
						}
						Hasher.MixString(
							Spatial.GetFrame().GetBasisEvidence());
						for (const FString& Component :
							Spatial.GetComponents())
						{
							Hasher.MixString(Component);
						}
						for (const FString& Evidence :
							Spatial.GetConversionEvidence())
						{
							Hasher.MixString(Evidence);
						}
					}
					break;
				default:
					break;
				}
			}
			return Hasher.GetSequence();
		}

		FString CanonicalNiagaraTypeId(
			const FNiagaraTypeDefinition& Type)
		{
			if (Type == FNiagaraTypeDefinition::GetBoolDef())
			{
				return TEXT("bool");
			}
			if (Type == FNiagaraTypeDefinition::GetFloatDef())
			{
				return TEXT("float");
			}
			if (Type == FNiagaraTypeDefinition::GetIntDef())
			{
				return TEXT("int32");
			}
			if (const UStruct* Struct = Type.GetStruct())
			{
				return Struct->GetPathName();
			}
			if (const UClass* Class = Type.GetClass())
			{
				return Class->GetPathName();
			}
			return TEXT("unknown");
		}

		bool TryString(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
					Text,
					Resources,
					Value,
					Error))
			{
				OutError = {
					Error.Code,
					TEXT("niagara_compound_native_capture"),
					Error.Detail,
				};
				return false;
			}
			OutValue = MoveTemp(Value);
			return true;
		}

		bool TryArray(
			const TArray<FUeAgentCanonicalValueRef>& Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError,
			const FString& Subject)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					Error))
			{
				OutError = {
					Error.Code,
					Subject,
					Error.Detail,
				};
				return false;
			}
			return true;
		}

		bool TryObject(
			const TArray<
				TPair<FString, FUeAgentCanonicalValueRef>>& Fields,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError,
			const FString& Subject)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(
					Fields,
					Resources,
					OutValue,
					Error))
			{
				OutError = {
					Error.Code,
					Subject,
					Error.Detail,
				};
				return false;
			}
			return true;
		}

		bool TryMakeCommonFields(
			const FResolvedModuleInput& Input,
			const FString& ModeText,
			FUeAgentResourceTracker& Resources,
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>& OutFields,
			FUeAgentGenericReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Kind;
			FUeAgentCanonicalValuePtr Name;
			FUeAgentCanonicalValuePtr Type;
			FUeAgentCanonicalValuePtr Mode;
			const FString TypeId = CanonicalNiagaraTypeId(
				Input.InputVariable.GetType());
			if (!TryString(
					TEXT("niagara_input"),
					Resources,
					Kind,
					OutError)
				|| !TryString(
					Input.FullInputName,
					Resources,
					Name,
					OutError)
				|| !TryString(
					TypeId.IsEmpty() ? TEXT("unknown") : TypeId,
					Resources,
					Type,
					OutError)
				|| !TryString(
					ModeText,
					Resources,
					Mode,
					OutError))
			{
				return false;
			}
			OutFields = {
				{TEXT("kind"), Kind.ToSharedRef()},
				{TEXT("input_name"), Name.ToSharedRef()},
				{TEXT("type_id"), Type.ToSharedRef()},
				{TEXT("mode"), Mode.ToSharedRef()},
			};
			return true;
		}

		bool TryMakeLiteralValue(
			const FResolvedModuleInput& Input,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError)
		{
			if (!Input.VisiblePin)
			{
				OutError = {
					TEXT("niagara_compound_input_pin_missing"),
					Input.FullInputName,
					TEXT("literal input has no readable pin"),
				};
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr LiteralValue;
			if (!TryMakeCommonFields(
					Input,
					TEXT("literal"),
					Resources,
					Fields,
					OutError))
			{
				return false;
			}
			const FString TypeId = CanonicalNiagaraTypeId(
				Input.InputVariable.GetType());
			FString CanonicalLiteral;
			bool bLiteralValid = false;
			if (TypeId == TEXT("float"))
			{
				double Number = 0.0;
				bLiteralValid = LexTryParseString(
						Number, *Input.VisiblePin->DefaultValue)
					&& FMath::IsFinite(Number);
				if (bLiteralValid)
				{
					CanonicalLiteral = FString::SanitizeFloat(Number);
				}
			}
			else if (TypeId == TEXT("int32"))
			{
				int32 Integer = 0;
				bLiteralValid = LexTryParseString(
					Integer, *Input.VisiblePin->DefaultValue);
				if (bLiteralValid)
				{
					CanonicalLiteral = FString::FromInt(Integer);
				}
			}
			else if (TypeId == TEXT("bool"))
			{
				FString Text = Input.VisiblePin->DefaultValue.TrimStartAndEnd();
				Text.ToLowerInline();
				bLiteralValid = Text == TEXT("true") || Text == TEXT("false");
				CanonicalLiteral = MoveTemp(Text);
			}
			else
			{
				CanonicalLiteral = Input.VisiblePin->DefaultValue;
				bLiteralValid = true;
			}
			if (!bLiteralValid
				|| !TryString(
					CanonicalLiteral, Resources, LiteralValue, OutError)
				|| !LiteralValue.IsValid())
			{
				if (OutError.Code.IsEmpty())
				{
					OutError = {
						TEXT("niagara_compound_literal_capture_invalid"),
						Input.FullInputName,
						Input.VisiblePin->DefaultValue,
					};
				}
				return false;
			}
			Fields.Emplace(
				TEXT("literal"),
				LiteralValue.ToSharedRef());
			return TryObject(
				Fields,
				Resources,
				OutValue,
				OutError,
				Input.FullInputName);
		}

		bool TryMakeLinkedValue(
			const FResolvedModuleInput& Input,
			const FString& LinkedParameter,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr Linked;
			if (!TryMakeCommonFields(
					Input,
					TEXT("linked_parameter"),
					Resources,
					Fields,
					OutError)
				|| !TryString(
					LinkedParameter,
					Resources,
					Linked,
					OutError))
			{
				return false;
			}
			Fields.Emplace(
				TEXT("linked_parameter"),
				Linked.ToSharedRef());
			return TryObject(
				Fields,
				Resources,
				OutValue,
				OutError,
				Input.FullInputName);
		}

		bool TryMakeDynamicValue(
			const FResolvedModuleInput& Input,
			const UNiagaraNodeFunctionCall& Dynamic,
			const TArray<FUeAgentCanonicalValueRef>& Children,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr Script;
			FUeAgentCanonicalValuePtr Inputs;
			if (!TryMakeCommonFields(
					Input,
					TEXT("dynamic_input"),
					Resources,
					Fields,
					OutError)
				|| !TryString(
					GetModuleScriptPath(Dynamic),
					Resources,
					Script,
					OutError)
				|| !TryArray(
					Children,
					Resources,
					Inputs,
					OutError,
					Input.FullInputName))
			{
				return false;
			}
			Fields.Emplace(
				TEXT("dynamic_script_identity"),
				Script.ToSharedRef());
			Fields.Emplace(TEXT("inputs"), Inputs.ToSharedRef());
			return TryObject(
				Fields,
				Resources,
				OutValue,
				OutError,
				Input.FullInputName);
		}

		bool TryReadLinkedParameter(
			const FResolvedModuleInput& Input,
			FString& OutLinkedParameter,
			FString& OutError)
		{
			OutLinkedParameter.Reset();
			if (!Input.VisiblePin)
			{
				return false;
			}
			TSharedPtr<FJsonObject> Payload;
			bool bHasValue = false;
			if (!BuildInputValueForPin(
					*Input.VisiblePin,
					Input.InputVariable.GetType(),
					Payload,
					bHasValue,
					OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Value = nullptr;
			FString Source;
			return bHasValue
				&& Payload.IsValid()
				&& Payload->TryGetObjectField(TEXT("value"), Value)
				&& Value
				&& Value->IsValid()
				&& (*Value)->TryGetStringField(TEXT("source"), Source)
				&& Source.Equals(
					TEXT("linked_input"),
					ESearchCase::IgnoreCase)
				&& (*Value)->TryGetStringField(
					TEXT("linked_parameter"),
					OutLinkedParameter)
				&& !OutLinkedParameter.TrimStartAndEnd().IsEmpty();
		}

		bool TryCaptureCanonicalInput(
			UNiagaraNodeFunctionCall& RootOwner,
			const FResolvedModuleInput& RootInput,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError)
		{
			struct FNode
			{
				UNiagaraNodeFunctionCall* Owner = nullptr;
				FResolvedModuleInput Input;
				UNiagaraNodeFunctionCall* Dynamic = nullptr;
				int32 ParentIndex = INDEX_NONE;
				TArray<int32> ChildIndices;
				FUeAgentCanonicalValuePtr Value;
			};

			TArray<FNode> Nodes;
			Nodes.Add(
				{
					&RootOwner,
					RootInput,
					nullptr,
					INDEX_NONE,
					{},
					nullptr,
				});
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				FNode& Node = Nodes[Index];
				if (!Node.Owner
					|| !Node.Input.InputVariable.IsValid())
				{
					OutError = {
						TEXT("niagara_compound_capture_node_invalid"),
						Node.Input.FullInputName,
						TEXT("native input traversal lost its owner"),
					};
					return false;
				}
				if (!Node.Input.VisiblePin)
				{
					Node.Input.VisiblePin =
						FindResolvedInputValuePin(
							*Node.Owner,
							Node.Input);
				}
				FString ResolvedName;
				Node.Dynamic = FindDynamicInputForModuleInput(
					*Node.Owner,
					Node.Input.FullInputName,
					ResolvedName);
				if (!Node.Dynamic)
				{
					continue;
				}
				for (int32 Parent = Node.ParentIndex;
					Parent != INDEX_NONE;
					Parent = Nodes[Parent].ParentIndex)
				{
					if (Nodes[Parent].Dynamic == Node.Dynamic)
					{
						OutError = {
							TEXT(
								"niagara_compound_dynamic_cycle_detected"),
							Node.Input.FullInputName,
							Node.Dynamic->GetPathName(),
						};
						return false;
					}
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryChargeMany(
						{
							{
								EUeAgentResourceDimension::Nodes,
								1,
							},
							{
								EUeAgentResourceDimension::WorkUnits,
								1,
							},
						},
						Node.Input.FullInputName,
						BudgetError))
				{
					OutError = {
						BudgetError.Code,
						Node.Input.FullInputName,
						BudgetError.Path,
					};
					return false;
				}
				TArray<FResolvedModuleInput> Inputs;
				UNiagaraNodeFunctionCall* const DynamicNode =
					Node.Dynamic;
				BuildResolvedModuleInputs(*DynamicNode, Inputs);
				for (const FResolvedModuleInput& Input : Inputs)
				{
					if (!Input.InputVariable.IsValid())
					{
						continue;
					}
					FString ChildName;
					UNiagaraNodeFunctionCall* ChildDynamic =
						FindDynamicInputForModuleInput(
							*DynamicNode,
							Input.FullInputName,
							ChildName);
					if (!ChildDynamic && !Input.VisiblePin)
					{
						continue;
					}
					const int32 ChildIndex = Nodes.Add(
						{
							DynamicNode,
							Input,
							nullptr,
							Index,
							{},
							nullptr,
						});
					Nodes[Index].ChildIndices.Add(ChildIndex);
				}
			}

			for (int32 Index = Nodes.Num() - 1;
				Index >= 0;
				--Index)
			{
				FNode& Node = Nodes[Index];
				if (Node.Dynamic)
				{
					TArray<FUeAgentCanonicalValueRef> Children;
					Children.Reserve(Node.ChildIndices.Num());
					for (const int32 ChildIndex : Node.ChildIndices)
					{
						if (!Nodes.IsValidIndex(ChildIndex)
							|| !Nodes[ChildIndex].Value.IsValid())
						{
							OutError = {
								TEXT(
									"niagara_compound_capture_child_invalid"),
								Node.Input.FullInputName,
								FString::FromInt(ChildIndex),
							};
							return false;
						}
						Children.Add(
							Nodes[ChildIndex].Value.ToSharedRef());
					}
					if (!TryMakeDynamicValue(
							Node.Input,
							*Node.Dynamic,
							Children,
							Resources,
							Node.Value,
							OutError))
					{
						return false;
					}
					continue;
				}
				if (Node.Input.InputVariable
						.GetType()
						.IsDataInterface())
				{
					OutError = {
						TEXT(
							"niagara_compound_data_interface_capture_not_bound"),
						Node.Input.FullInputName,
						TEXT(
							"data-interface canonical projection is not bound"),
					};
					return false;
				}
				FString LinkedParameter;
				FString NativeError;
				if (Node.Input.VisiblePin
					&& !Node.Input.VisiblePin->LinkedTo.IsEmpty())
				{
					if (!TryReadLinkedParameter(
							Node.Input,
							LinkedParameter,
							NativeError))
					{
						OutError = {
							TEXT(
								"niagara_compound_link_capture_failed"),
							Node.Input.FullInputName,
							NativeError,
						};
						return false;
					}
					if (!TryMakeLinkedValue(
							Node.Input,
							LinkedParameter,
							Resources,
							Node.Value,
							OutError))
					{
						return false;
					}
				}
				else if (!TryMakeLiteralValue(
						Node.Input,
						Resources,
						Node.Value,
						OutError))
				{
					return false;
				}
			}
			OutValue = Nodes[0].Value;
			return OutValue.IsValid();
		}

		const FUeAgentCanonicalValueRef* FindField(
			const FUeAgentCanonicalValue& Value,
			const FString& Name)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			const TPair<FString, FUeAgentCanonicalValueRef>* Found =
				Value.GetObjectItems().FindByPredicate(
					[&Name](
						const TPair<
							FString,
							FUeAgentCanonicalValueRef>& Pair)
					{
						return Pair.Key == Name;
					});
			return Found ? &Found->Value : nullptr;
		}

		bool TryBuildLiteralPayload(
			const FUeAgentCanonicalValue& Value,
			TSharedPtr<const FJsonObject>& OutPayload,
			FString& OutError)
		{
			OutPayload.Reset();
			const FUeAgentCanonicalValueRef* Mode =
				FindField(Value, TEXT("mode"));
			const FUeAgentCanonicalValueRef* Literal =
				FindField(Value, TEXT("literal"));
			const FUeAgentCanonicalValueRef* Type =
				FindField(Value, TEXT("type_id"));
			if (!Mode
				|| (*Mode)->GetKind() != EUeAgentValueKind::String
				|| (*Mode)->GetText() != TEXT("literal")
				|| !Literal
				|| !Type
				|| (*Type)->GetKind() != EUeAgentValueKind::String)
			{
				OutError =
					TEXT("niagara_compound_literal_type_unsupported");
				return false;
			}
			TSharedRef<FJsonObject> Payload =
				MakeShared<FJsonObject>();
			FString LiteralText;
			switch ((*Literal)->GetKind())
			{
			case EUeAgentValueKind::String:
				LiteralText = (*Literal)->GetText();
				break;
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal:
				LiteralText = (*Literal)->GetLexeme();
				break;
			case EUeAgentValueKind::Boolean:
				LiteralText = (*Literal)->GetBoolean()
					? TEXT("true")
					: TEXT("false");
				break;
			default:
				OutError =
					TEXT("niagara_compound_literal_type_unsupported");
				return false;
			}
			if ((*Type)->GetText() == TEXT("float"))
			{
				double Number = 0.0;
				if (!LexTryParseString(
						Number,
						*LiteralText)
					|| !FMath::IsFinite(Number))
				{
					OutError =
						TEXT("niagara_compound_literal_invalid");
					return false;
				}
				Payload->SetStringField(
					TEXT("value_kind"),
					TEXT("float"));
				Payload->SetNumberField(TEXT("value"), Number);
			}
			else if ((*Type)->GetText() == TEXT("bool"))
			{
				FString Text = LiteralText.TrimStartAndEnd();
				Text.ToLowerInline();
				if (Text != TEXT("true") && Text != TEXT("false"))
				{
					OutError =
						TEXT("niagara_compound_literal_invalid");
					return false;
				}
				Payload->SetStringField(
					TEXT("value_kind"),
					TEXT("bool"));
				Payload->SetBoolField(
					TEXT("value"),
					Text == TEXT("true"));
			}
			else if ((*Type)->GetText() == TEXT("int32"))
			{
				int32 Integer = 0;
				if (!LexTryParseString(Integer, *LiteralText))
				{
					OutError =
						TEXT("niagara_compound_literal_invalid");
					return false;
				}
				Payload->SetStringField(
					TEXT("value_kind"),
					TEXT("int"));
				Payload->SetNumberField(TEXT("value"), Integer);
			}
			else
			{
				OutError =
					TEXT("niagara_compound_literal_type_unsupported");
				return false;
			}
			OutPayload = Payload;
			return true;
		}

		bool TryReadRequiredString(
			const FUeAgentCanonicalValue& Value,
			const FString& Name,
			FString& OutText,
			FString& OutError)
		{
			const FUeAgentCanonicalValueRef* Field =
				FindField(Value, Name);
			if (!Field
				|| (*Field)->GetKind()
					!= EUeAgentValueKind::String)
			{
				OutError =
					TEXT("niagara_compound_field_invalid:")
					+ Name;
				return false;
			}
			OutText = (*Field)->GetText().TrimStartAndEnd();
			if (OutText.IsEmpty())
			{
				OutError =
					TEXT("niagara_compound_field_invalid:")
					+ Name;
				return false;
			}
			return true;
		}

		bool TryApplyCanonicalInput(
			UNiagaraNodeFunctionCall& RootOwner,
			const FString& RootInputName,
			const FUeAgentCanonicalValueRef& RootValue,
			FUeAgentResourceTracker& Resources,
			bool& bOutChanged,
			FString& OutError)
		{
			struct FFrame
			{
				UNiagaraNodeFunctionCall* Owner = nullptr;
				FString InputName;
				FUeAgentCanonicalValueRef Value;
				FString Path;
			};

			bOutChanged = false;
			OutError.Reset();
			TArray<FFrame> Stack;
			Stack.Add(
				{
					&RootOwner,
					RootInputName,
					RootValue,
					TEXT("/niagara_input"),
				});
			while (!Stack.IsEmpty())
			{
				FFrame Frame = Stack.Pop(EAllowShrinking::No);
				if (!Frame.Owner)
				{
					OutError =
						TEXT("niagara_compound_apply_owner_missing:")
						+ Frame.Path;
					return false;
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryChargeMany(
						{
							{
								EUeAgentResourceDimension::Nodes,
								1,
							},
							{
								EUeAgentResourceDimension::WorkUnits,
								1,
							},
						},
						Frame.Path,
						BudgetError))
				{
					OutError = BudgetError.Code
						+ TEXT(":")
						+ BudgetError.Path;
					return false;
				}
				FString InputName;
				FString TypeId;
				FString Mode;
				if (!TryReadRequiredString(
						*Frame.Value,
						TEXT("input_name"),
						InputName,
						OutError)
					|| !TryReadRequiredString(
						*Frame.Value,
						TEXT("type_id"),
						TypeId,
						OutError)
					|| !TryReadRequiredString(
						*Frame.Value,
						TEXT("mode"),
						Mode,
						OutError))
				{
					OutError += TEXT(":") + Frame.Path;
					return false;
				}
				TArray<FResolvedModuleInput> ResolvedInputs;
				BuildResolvedModuleInputs(
					*Frame.Owner,
					ResolvedInputs);
				FString ResolvedInputName;
				const FResolvedModuleInput* ResolvedInput =
					ResolveModuleInputByName(
						ResolvedInputs,
						Frame.InputName.IsEmpty()
							? InputName
							: Frame.InputName,
						ResolvedInputName);
				if (!ResolvedInput
					|| !ResolvedInput->InputVariable.IsValid())
				{
					OutError =
						TEXT("niagara_compound_input_not_found:")
						+ Frame.Path
						+ TEXT(":")
						+ InputName;
					return false;
				}
				if (!InputName.Equals(
						ResolvedInput->FullInputName,
						ESearchCase::CaseSensitive)
					|| TypeId
						!= CanonicalNiagaraTypeId(
							ResolvedInput
								->InputVariable
								.GetType()))
				{
					OutError =
						TEXT("niagara_compound_input_type_mismatch:")
						+ Frame.Path;
					return false;
				}

				bool bChanged = false;
				FString NativeError;
				const FString TargetInputName =
					ResolvedInput->FullInputName;
				FString AppliedInputName;
				if (Mode == TEXT("literal"))
				{
					TSharedPtr<const FJsonObject> Payload;
					if (!TryBuildLiteralPayload(
							*Frame.Value,
							Payload,
							NativeError)
						|| !SetLiteralInputValue(
							*Frame.Owner,
							TargetInputName,
							Payload,
							AppliedInputName,
							bChanged,
							NativeError))
					{
						OutError =
							TEXT(
								"niagara_compound_literal_apply_failed:")
							+ Frame.Path
							+ TEXT(":")
							+ NativeError;
						return false;
					}
				}
				else if (Mode == TEXT("linked_parameter"))
				{
					FString LinkedParameter;
					if (!TryReadRequiredString(
							*Frame.Value,
							TEXT("linked_parameter"),
							LinkedParameter,
							NativeError)
						|| !SetLinkedInputValue(
							*Frame.Owner,
							TargetInputName,
							LinkedParameter,
							AppliedInputName,
							bChanged,
							NativeError))
					{
						OutError =
							TEXT(
								"niagara_compound_link_apply_failed:")
							+ Frame.Path
							+ TEXT(":")
							+ NativeError;
						return false;
					}
				}
				else if (Mode == TEXT("dynamic_input"))
				{
					FString ScriptIdentity;
					const FUeAgentCanonicalValueRef* Inputs =
						FindField(*Frame.Value, TEXT("inputs"));
					if (!TryReadRequiredString(
							*Frame.Value,
							TEXT("dynamic_script_identity"),
							ScriptIdentity,
							NativeError)
						|| !Inputs
						|| (*Inputs)->GetKind()
							!= EUeAgentValueKind::Array)
					{
						OutError =
							TEXT(
								"niagara_compound_dynamic_value_invalid:")
							+ Frame.Path
							+ TEXT(":")
							+ NativeError;
						return false;
					}
					UNiagaraScript* Script =
						LoadObject<UNiagaraScript>(
							nullptr,
							*ScriptIdentity);
					if (!IsValid(Script)
						|| Script->GetUsage()
							!= ENiagaraScriptUsage::DynamicInput)
					{
						OutError =
							TEXT(
								"niagara_compound_dynamic_script_invalid:")
							+ ScriptIdentity;
						return false;
					}
					UNiagaraNodeFunctionCall* Dynamic = nullptr;
					if (!SetDynamicInput(
							*Frame.Owner,
							TargetInputName,
							*Script,
							Script->GetName(),
							Dynamic,
							AppliedInputName,
							NativeError)
						|| !Dynamic)
					{
						OutError =
							TEXT(
								"niagara_compound_dynamic_apply_failed:")
							+ Frame.Path
							+ TEXT(":")
							+ NativeError;
						return false;
					}
					FString InitializationSummary;
					if (!InitializeDynamicInputDataInterfaces(
							*Dynamic,
							InitializationSummary,
							NativeError))
					{
						OutError =
							TEXT(
								"niagara_compound_dynamic_initialize_failed:")
							+ Frame.Path
							+ TEXT(":")
							+ NativeError;
						return false;
					}
					const TArray<FUeAgentCanonicalValueRef>& Children =
						(*Inputs)->GetArrayItems();
					for (int32 Index = Children.Num() - 1;
						Index >= 0;
						--Index)
					{
						FString ChildName;
						if (!TryReadRequiredString(
								*Children[Index],
								TEXT("input_name"),
								ChildName,
								NativeError))
						{
							OutError =
								TEXT(
									"niagara_compound_dynamic_child_invalid:")
								+ Frame.Path
								+ TEXT(":")
								+ NativeError;
							return false;
						}
						Stack.Add(
							{
								Dynamic,
								ChildName,
								Children[Index],
								FString::Printf(
									TEXT("%s/inputs/%d"),
									*Frame.Path,
									Index),
							});
					}
					bChanged = true;
				}
				else if (Mode == TEXT("data_interface"))
				{
					OutError =
						TEXT(
							"niagara_compound_data_interface_apply_not_bound:")
						+ Frame.Path;
					return false;
				}
				else
				{
					OutError =
						TEXT("niagara_compound_mode_invalid:")
						+ Frame.Path;
					return false;
				}
				bOutChanged = bOutChanged || bChanged;
			}
			return true;
		}

		const FUeAgentGenericEffectBinding* FindBinding(
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			const FString& IdentityKey)
		{
			for (const FUeAgentGenericEffectBinding& Binding : Bindings)
			{
				if (Binding.GetCandidateBinding()
						.GetSummary()
						.GetIdentity()
						.GetCanonicalKey()
					== IdentityKey)
				{
					return &Binding;
				}
			}
			return nullptr;
		}

		bool TryCompileNiagaraAsset(
			UObject& Asset,
			UeAgentNiagaraCompileValidation::FResult& OutCompile,
			UeAgentNiagaraStackValidation::FResult& OutStack,
			FString& OutError)
		{
			OutCompile =
				UeAgentNiagaraCompileValidation::FResult();
			OutStack =
				UeAgentNiagaraStackValidation::FResult();
			if (UNiagaraSystem* System =
					Cast<UNiagaraSystem>(&Asset))
			{
				System->RequestCompile(true);
				System->WaitForCompilationComplete(true, false);
				System->PollForCompilationComplete(true);
				if (System->HasOutstandingCompilationRequests(true))
				{
					OutError =
						TEXT("niagara_compile_incomplete");
					return false;
				}
				System->CacheFromCompiledData();
				OutCompile =
					UeAgentNiagaraCompileValidation::InspectAsset(
						System);
			}
			else if (UNiagaraEmitter* Emitter =
					Cast<UNiagaraEmitter>(&Asset))
			{
				UeAgentNiagaraEmitterCompileContext::FResult Result;
				if (!UeAgentNiagaraEmitterCompileContext::Compile(
						*Emitter,
						Emitter
							->GetExposedVersion()
							.VersionGuid,
						true,
						Result,
						OutError))
				{
					return false;
				}
				OutCompile = Result.Validation;
			}
			else if (UNiagaraScript* Script =
					Cast<UNiagaraScript>(&Asset))
			{
				Script->RequestCompile(
					Script->GetExposedVersion().VersionGuid,
					true);
				OutCompile =
					UeAgentNiagaraCompileValidation::InspectAsset(
						Script);
			}
			else
			{
				OutError =
					TEXT("niagara_compile_target_invalid");
				return false;
			}
			if (!OutCompile.bCompilationComplete)
			{
				OutError = TEXT("niagara_compile_incomplete");
				return false;
			}
			OutStack =
				UeAgentNiagaraStackValidation::InspectAsset(&Asset);
			if (!OutStack.bInspectionComplete)
			{
				OutError = OutStack.FirstError.IsEmpty()
					? TEXT("niagara_stack_inspection_failed")
					: OutStack.FirstError;
				return false;
			}
			return true;
		}
	}

	class FUeAgentNiagaraCompoundNativeHook::FState final
	{
	public:
		struct FAppliedChange
		{
			TWeakObjectPtr<UNiagaraNodeFunctionCall> Module;
			FString InputName;
			FUeAgentCanonicalValuePtr BeforeValue;
		};

		struct FOperation
		{
			int64 SubjectGeneration = -1;
			TArray<FAppliedChange> AppliedChanges;
			TArray<TWeakObjectPtr<UObject>> Assets;
		};

		FCriticalSection Mutex;
		TMap<FString, FOperation> AppliedByOperation;
		TMap<int64, FString> OperationByGeneration;
		int64 NextGeneration = 1;
		int64 NextCaptureSequence = 1;
	};

	const FString& GetNiagaraCompoundCatalogKind()
	{
		return CatalogKind;
	}

	const FString& GetNiagaraCompoundCatalogSourceId()
	{
		return CatalogSourceId;
	}

	const FString& GetNiagaraCompoundReadPortId()
	{
		return ReadPortId;
	}

	Contract::FUeAgentDescriptorIdentity
	GetNiagaraCompoundOperationDescriptorIdentity()
	{
		return GetNiagaraCompoundOperationDescriptorRegistry()
			->GetDescriptors()[0].GetIdentity();
	}

	TSharedRef<const Contract::FUeAgentDescriptorRegistry>
	GetNiagaraCompoundOperationDescriptorRegistry()
	{
		static const TSharedRef<
			const Contract::FUeAgentDescriptorRegistry>
			Registry = []()
			{
				using namespace Contract;
				TSharedPtr<const FUeAgentDescriptorRegistry>
					Result;
				FString Error;
				const bool bCreated =
					FUeAgentDescriptorRegistry::TryCreate(
						{
							FUeAgentOperationDescriptor(
								FUeAgentDescriptorIdentity(
									TEXT(
										"uai.operation.niagara.compound"),
									1,
									TEXT(
										"sha256:29db469e82374cc95b6dddef44ad78597189ed614015e82af0e49664898827fc")),
								TEXT(
									"niagara.compound.apply"),
								TEXT("niagara"),
								EUeAgentDescriptorOperationKind::
									Operation,
								FUeAgentExecutionContextPolicy(
									{
										EUeAgentStatePlane::
											Persisted,
									},
									{},
									EUeAgentContextResolutionPolicy::
										ExplicitOnly,
									EUeAgentTransientContextPolicy::
										ReadOnly),
								FUeAgentContractValue(
									TEXT(
										"{\"type\":\"object\"}")),
								FUeAgentContractValue(
									TEXT(
										"{\"type\":\"object\"}"))),
						},
						Result,
						Error);
				checkf(
					bCreated && Result.IsValid(),
					TEXT(
						"Niagara compound descriptor "
						"registry invalid: %s"),
					*Error);
				return Result.ToSharedRef();
			}();
		return Registry;
	}

	FUeAgentNiagaraCompoundNativeHook::
		FUeAgentNiagaraCompoundNativeHook(FString InGeneration)
		: Generation(MoveTemp(InGeneration))
		, State(MakeShared<FState, ESPMode::ThreadSafe>())
	{
		Generation.TrimStartAndEndInline();
	}

	FUeAgentNiagaraCompoundNativeHook::
		~FUeAgentNiagaraCompoundNativeHook() = default;

	bool FUeAgentNiagaraCompoundNativeHook::IsAvailable() const
	{
		return !Generation.IsEmpty();
	}

	bool FUeAgentNiagaraCompoundNativeHook::TryCapture(
		const Discovery::FUeAgentStateReadRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		const ContextResource::FUeAgentOperationDeadline& Deadline,
		const ContextResource::FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const GenericRead::FUeAgentUeReadCapture>& OutCapture,
		GenericRead::FUeAgentGenericReadError& OutError)
	{
		OutCapture.Reset();
		OutError = {};
		FUeAgentCancellationRequested Cancelled;
		if (!Cancellation.TryCheckpoint(Cancelled))
		{
			OutError = {
				Cancelled.Code,
				Request.GetBinding().GetBindingId(),
				Cancelled.Reason,
			};
			return false;
		}
		FUeAgentOperationDeadlineExceeded Expired;
		if (!Deadline.TryCheckpoint(
				TEXT("niagara_compound_native.capture"),
				Expired))
		{
			OutError = {
				Expired.Code,
				Request.GetBinding().GetBindingId(),
				Expired.Stage,
			};
			return false;
		}
		FUeAgentResolvedNiagaraCompoundInput Resolved;
		FString ErrorCode;
		FString Detail;
		if (!TryResolveNiagaraCompoundInput(
				Request.GetBinding(),
				Generation,
				Resolved,
				ErrorCode,
				Detail))
		{
			OutError = {
				ErrorCode,
				Request.GetBinding().GetBindingId(),
				Detail,
			};
			return false;
		}
		if (!Resolved.Asset
			|| !Resolved.Module
			|| !Resolved.Input.InputVariable.IsValid()
			|| (Request.GetStatePlane()
					== EUeAgentStatePlane::Persisted
				&& Resolved.Asset->GetOutermost()->IsDirty()))
		{
			OutError = {
				TEXT(
					"niagara_compound_persisted_state_unavailable"),
				Request.GetBinding()
					.GetSummary()
					.GetIdentity()
					.GetOwnerId(),
				TEXT(
					"the Niagara asset is unavailable or dirty"),
			};
			return false;
		}
		FUeAgentCanonicalValuePtr Value;
		if (!TryCaptureCanonicalInput(
				*Resolved.Module,
				Resolved.Input,
				Resources,
				Value,
				OutError))
		{
			return false;
		}
		int64 CaptureSequence = 0;
		{
			FScopeLock Lock(&State->Mutex);
			CaptureSequence = State->NextCaptureSequence++;
		}
		const int64 StateRevisionSequence =
			CanonicalStateRevisionSequence(
				Request.GetBinding()
					.GetSummary()
					.GetIdentity(),
				Value.ToSharedRef());
		const FUeAgentStateRevision Revision(
			ReadPortId,
			Generation,
			StateRevisionSequence);
		TArray<FUeAgentUeReadEntry> Entries;
		Entries.Emplace(
			Request.GetBinding()
				.GetSummary()
				.GetIdentity(),
			FUeAgentUeTypeBinding(
				TEXT("UE.NiagaraInput:compound"),
				FUeAgentCanonicalType(
					TEXT("uai.niagara.input.v1"),
					EUeAgentValueKind::Object,
					false,
					TEXT("object")),
				TEXT("niagara-native-graph-input")),
			Value.ToSharedRef(),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::DomainNative,
				TEXT("Niagara.Input"),
				TEXT("niagara-native-input-default"),
				Value.ToSharedRef()),
			FUeAgentStateDetails(
				Resolved.Input.FullInputName,
				TEXT("Fresh native Niagara graph capture"),
				{TEXT("Niagara"), TEXT("Inputs")},
				true,
				TEXT("available"),
				{
					{
						TEXT("module_node_guid"),
						Resolved.Selector.ModuleNodeGuid,
					},
					{
						TEXT("stage_key"),
						Resolved.Selector.StageKey,
					},
				}),
			Revision);
		const FString EvidenceId = FString::Printf(
			TEXT("niagara-compound-native:%s:%lld"),
			*Request.GetBinding()
				.GetSummary()
				.GetIdentity()
				.GetCanonicalKey(),
			CaptureSequence);
		OutCapture = MakeShared<FUeAgentUeReadCapture>(
			TEXT("snapshot:") + EvidenceId,
			Request.GetStatePlane(),
			Revision,
			FUeAgentTemporalPoint(
				TEXT("editor-session"),
				FString::Printf(
					TEXT("%lld"),
					CaptureSequence),
				TEXT("1"),
				TEXT("1"),
				TEXT("captured")),
			EvidenceId,
			true,
			MoveTemp(Entries));
		return true;
	}

	bool FUeAgentNiagaraCompoundNativeHook::TryApply(
		const ExecutionMutation::FExecutionMutationRequest& Request,
		const ExecutionMutation::FLockFenceLease& Lease,
		const TArray<GenericEffect::FUeAgentGenericEffectBinding>& Bindings,
		ContextResource::FUeAgentResourceTracker& Resources,
		GenericEffect::FUeAgentGenericEffectApplyResult& OutResult,
		ExecutionMutation::FExecutionMutationError& OutError)
	{
		OutResult = FUeAgentGenericEffectApplyResult();
		OutError = FExecutionMutationError();
		{
			FScopeLock Lock(&State->Mutex);
			if (State->AppliedByOperation.Contains(
					Request.GetOperationId()))
			{
				OutError = FExecutionMutationError(
					TEXT(
						"niagara_compound_operation_already_applied"),
					Request.GetOperationId());
				return false;
			}
		}
		TArray<FState::FAppliedChange> AppliedItems;
		TArray<UObject*> Assets;
		TArray<FString> MutatedKeys;
		auto RestoreApplied =
			[&Resources](
				TArray<FState::FAppliedChange>& Items,
				FString& OutRestoreError)
			{
				for (int32 Index = Items.Num() - 1;
					Index >= 0;
					--Index)
				{
					FState::FAppliedChange& Item = Items[Index];
					UNiagaraNodeFunctionCall* Module =
						Item.Module.Get();
					bool bChanged = false;
					if (!Module
						|| !Item.BeforeValue.IsValid()
						|| !TryApplyCanonicalInput(
							*Module,
							Item.InputName,
							Item.BeforeValue.ToSharedRef(),
							Resources,
							bChanged,
							OutRestoreError))
					{
						if (OutRestoreError.IsEmpty())
						{
							OutRestoreError =
								TEXT(
									"niagara_compound_restore_target_lost:")
								+ Item.InputName;
						}
						return false;
					}
				}
				return true;
			};
		auto FailWithRestore =
			[&AppliedItems, &RestoreApplied, &OutError](
				const FString& Code,
				const FString& Detail)
			{
				FString RestoreError;
				const bool bRestored =
					AppliedItems.IsEmpty()
					|| RestoreApplied(AppliedItems, RestoreError);
				OutError = FExecutionMutationError(
					bRestored
						? Code
						: TEXT(
							"niagara_compound_apply_rollback_failed"),
					Detail
					+ (bRestored
						? FString()
						: TEXT(":restore=") + RestoreError));
				return false;
			};
		for (const FPlannedChange& Change :
			Request.GetPlan()->GetChangeSet().GetChanges())
		{
			const FString IdentityKey =
				Change.GetIdentity().GetCanonicalKey();
			const FUeAgentGenericEffectBinding* Binding =
				FindBinding(Bindings, IdentityKey);
			if (!Binding
				|| !Lease.GetLockKeys().Contains(IdentityKey)
				|| !Change.GetAfter().IsValid())
			{
				return FailWithRestore(
					TEXT("niagara_compound_effect_binding_missing"),
					IdentityKey);
			}
			FUeAgentResolvedNiagaraCompoundInput Resolved;
			FString ErrorCode;
			FString Detail;
			if (!TryResolveNiagaraCompoundInput(
					Binding->GetCandidateBinding(),
					Generation,
					Resolved,
					ErrorCode,
					Detail)
				|| !Resolved.Module
				|| !Resolved.Asset)
			{
				return FailWithRestore(
					ErrorCode.IsEmpty()
						? TEXT(
							"niagara_compound_input_pin_missing")
						: ErrorCode,
					Detail);
			}
			FUeAgentCanonicalValuePtr NativeBefore;
			FUeAgentGenericReadError CaptureError;
			if (!TryCaptureCanonicalInput(
					*Resolved.Module,
					Resolved.Input,
					Resources,
					NativeBefore,
					CaptureError))
			{
				return FailWithRestore(
					CaptureError.Code,
					CaptureError.SubjectId
					+ TEXT(":")
					+ CaptureError.Detail);
			}
			FState::FAppliedChange Applied;
			Applied.Module = Resolved.Module;
			Applied.InputName = Resolved.Input.FullInputName;
			Applied.BeforeValue = MoveTemp(NativeBefore);
			AppliedItems.Add(Applied);
			bool bChanged = false;
			FString NativeError;
			if (!TryApplyCanonicalInput(
					*Resolved.Module,
					Resolved.Input.FullInputName,
					Change.GetAfter().ToSharedRef(),
					Resources,
					bChanged,
					NativeError))
			{
				return FailWithRestore(
					TEXT("niagara_compound_native_apply_failed"),
					NativeError);
			}
			if (bChanged)
			{
				MutatedKeys.Add(IdentityKey);
			}
			Assets.AddUnique(Resolved.Asset);
		}
		int64 SubjectGeneration = 0;
		bool bOperationAlreadyApplied = false;
		{
			FScopeLock Lock(&State->Mutex);
			if (State->AppliedByOperation.Contains(
					Request.GetOperationId()))
			{
				bOperationAlreadyApplied = true;
			}
			else
			{
				SubjectGeneration = State->NextGeneration++;
				FState::FOperation Operation;
				Operation.SubjectGeneration = SubjectGeneration;
				Operation.AppliedChanges = MoveTemp(AppliedItems);
				for (UObject* Asset : Assets)
				{
					Operation.Assets.Add(Asset);
				}
				State->AppliedByOperation.Add(
					Request.GetOperationId(),
					MoveTemp(Operation));
				State->OperationByGeneration.Add(
					SubjectGeneration,
					Request.GetOperationId());
			}
		}
		if (bOperationAlreadyApplied)
		{
			return FailWithRestore(
				TEXT("niagara_compound_operation_already_applied"),
				Request.GetOperationId());
		}
		const bool bDidMutationStart = !MutatedKeys.IsEmpty();
		OutResult = FUeAgentGenericEffectApplyResult(
			SubjectGeneration,
			MoveTemp(MutatedKeys),
			{TEXT("niagara.compile"), TEXT("niagara.stack")},
			true,
			Request.GetOperationId(),
			bDidMutationStart);
		return OutResult.IsValid();
	}

	bool FUeAgentNiagaraCompoundNativeHook::TryRollback(
		const ExecutionMutation::FExecutionMutationRequest& Request,
		const GenericEffect::FUeAgentGenericEffectApplyResult& Applied,
		ContextResource::FUeAgentResourceTracker& Resources,
		bool& bOutConfirmed,
		ExecutionMutation::FExecutionMutationError& OutError)
	{
		bOutConfirmed = false;
		OutError = FExecutionMutationError();
		FState::FOperation Operation;
		{
			FScopeLock Lock(&State->Mutex);
			FState::FOperation* Found =
				State->AppliedByOperation.Find(
					Applied.GetRollbackToken());
			if (!Found)
			{
				OutError = FExecutionMutationError(
					TEXT(
						"niagara_compound_rollback_state_missing"),
					Applied.GetRollbackToken());
				return false;
			}
			Operation = *Found;
		}
		for (int32 Index = Operation.AppliedChanges.Num() - 1;
			Index >= 0;
			--Index)
		{
			FState::FAppliedChange& Item =
				Operation.AppliedChanges[Index];
			UNiagaraNodeFunctionCall* Module = Item.Module.Get();
			if (!Module || !Item.BeforeValue.IsValid())
			{
				OutError = FExecutionMutationError(
					TEXT(
						"niagara_compound_rollback_target_lost"),
					Item.InputName);
				return false;
			}
			bool bChanged = false;
			FString NativeError;
			if (!TryApplyCanonicalInput(
					*Module,
					Item.InputName,
					Item.BeforeValue.ToSharedRef(),
					Resources,
					bChanged,
					NativeError))
			{
				OutError = FExecutionMutationError(
					TEXT(
						"niagara_compound_native_rollback_failed"),
					NativeError);
				return false;
			}
		}
		{
			FScopeLock Lock(&State->Mutex);
			State->AppliedByOperation.Remove(
				Applied.GetRollbackToken());
			State->OperationByGeneration.Remove(
				Operation.SubjectGeneration);
		}
		bOutConfirmed = true;
		return true;
	}

	bool FUeAgentNiagaraCompoundNativeHook::TryCompileAndSettle(
		const ExecutionMutation::FBarrierRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		FUeAgentNiagaraCompileSettleReceipt& OutReceipt,
		ExecutionMutation::FExecutionMutationError& OutError)
	{
		OutReceipt = FUeAgentNiagaraCompileSettleReceipt();
		OutError = FExecutionMutationError();
		FState::FOperation Operation;
		{
			FScopeLock Lock(&State->Mutex);
			const FString* OperationId =
				State->OperationByGeneration.Find(
					Request.GetSubjectGeneration());
			const FState::FOperation* Found = OperationId
				? State->AppliedByOperation.Find(*OperationId)
				: nullptr;
			if (!Found)
			{
				OutError = FExecutionMutationError(
					TEXT(
						"niagara_compound_settle_generation_missing"),
					FString::Printf(
						TEXT("%lld"),
						Request.GetSubjectGeneration()));
				return false;
			}
			Operation = *Found;
		}

		int32 CompileErrorCount = 0;
		int32 CompileWarningCount = 0;
		int32 StackIssueCount = 0;
		int32 StackWarningCount = 0;
		bool bCompilationComplete = true;
		TArray<FString> CompiledTargets;
		TArray<FString> AssetPaths;
		for (const TWeakObjectPtr<UObject>& WeakAsset :
			Operation.Assets)
		{
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				OutError = FExecutionMutationError(
					TEXT(
						"niagara_compound_settle_target_lost"),
					FString::Printf(
						TEXT("%lld"),
						Request.GetSubjectGeneration()));
				return false;
			}
			FUeAgentResourceBudgetExceeded BudgetError;
			if (!Resources.TryChargeMany(
					{
						{
							EUeAgentResourceDimension::Items,
							1,
						},
						{
							EUeAgentResourceDimension::WorkUnits,
							1,
						},
					},
					Asset->GetPathName(),
					BudgetError))
			{
				OutError = FExecutionMutationError(
					BudgetError.Code,
					BudgetError.Path);
				return false;
			}
			UeAgentNiagaraCompileValidation::FResult Compile;
			UeAgentNiagaraStackValidation::FResult Stack;
			FString NativeError;
			if (!TryCompileNiagaraAsset(
					*Asset,
					Compile,
					Stack,
					NativeError))
			{
				OutError = FExecutionMutationError(
					NativeError.IsEmpty()
						? TEXT(
							"niagara_compound_settle_failed")
						: NativeError,
					Asset->GetPathName());
				return false;
			}
			bCompilationComplete =
				bCompilationComplete
				&& Compile.bCompilationComplete;
			CompileErrorCount += Compile.ErrorScriptCount;
			CompileWarningCount += Compile.WarningScriptCount;
			StackIssueCount += Stack.ErrorCount;
			StackWarningCount += Stack.WarningCount;
			for (const FString& Target : Compile.CompiledTargets)
			{
				CompiledTargets.AddUnique(Target);
			}
			AssetPaths.AddUnique(Asset->GetPathName());
		}
		CompiledTargets.Sort();
		AssetPaths.Sort();

		int64 BarrierSequence = 0;
		{
			FScopeLock Lock(&State->Mutex);
			BarrierSequence = State->NextCaptureSequence++;
		}
		const FString PostBarrierRevision = FString::Printf(
			TEXT("%s:%lld:%lld"),
			*Generation,
			Request.GetSubjectGeneration(),
			BarrierSequence);
		const FString EvidenceId = FString::Printf(
			TEXT("niagara-compound-settle:%lld:%u:%lld"),
			Request.GetSubjectGeneration(),
			GetTypeHash(FString::Join(AssetPaths, TEXT("|"))),
			BarrierSequence);
		OutReceipt = FUeAgentNiagaraCompileSettleReceipt(
			Request.GetSubjectGeneration(),
			bCompilationComplete,
			CompileErrorCount,
			StackIssueCount,
			CompileWarningCount,
			StackWarningCount,
			MoveTemp(CompiledTargets),
			{TEXT("niagara.compile"), TEXT("niagara.stack")},
			PostBarrierRevision,
			EvidenceId);
		if (!OutReceipt.IsValid())
		{
			OutError = FExecutionMutationError(
				TEXT(
					"niagara_compound_settle_receipt_invalid"),
				EvidenceId);
			return false;
		}
		return true;
	}

	void FUeAgentNiagaraCompoundNativeHook::
		ReleaseVerifiedOperation(const FString& OperationId)
	{
		FScopeLock Lock(&State->Mutex);
		const FState::FOperation* Found =
			State->AppliedByOperation.Find(OperationId);
		if (Found)
		{
			State->OperationByGeneration.Remove(
				Found->SubjectGeneration);
			State->AppliedByOperation.Remove(OperationId);
		}
	}
}
