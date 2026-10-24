#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundNativeHook.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundDiscovery.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Domains/Niagara/UeAgentNiagaraEmitterCompileContext.h"
#include "Domains/Niagara/UeAgentNiagaraStackValidation.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/PackageName.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/Class.h"

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

		Contract::FDiagnostic NativeCompoundFact(const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& InputName = FString(), const FString& Stage = TEXT("niagara_capture"))
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, Stage);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.CandidateName = InputName;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		bool NativeCaptureFailure(Contract::FDiagnostic& Error, const FString& InputName, const FString& Path,
			const FString& Target = FString())
		{
			TFunction<void(Contract::FDiagnostic&)> Supplement = [&](Contract::FDiagnostic& Item)
			{
				if (Item.CandidateName.IsEmpty()) Item.CandidateName = InputName;
				if (Item.PropertyPath.IsEmpty()) Item.PropertyPath = Path;
				if (Item.SemanticPointer.IsEmpty()) Item.SemanticPointer = Path;
				if (Item.SourceKind.IsEmpty()) Item.SourceKind = TEXT("native_result");
				if (Item.TargetPath.IsEmpty()) Item.TargetPath = Target;
				for (auto& Child : Item.Diagnostics) Supplement(Child);
				if (Item.PublicCause)
				{
					auto Cause = MakeShared<Contract::FDiagnostic>(*Item.PublicCause);
					Supplement(*Cause);
					Item.PublicCause = MoveTemp(Cause);
				}
			};
			Supplement(Error);
			return false;
		}

		TSharedPtr<FJsonValue> NativeJsonFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid() || (Value->Type != EJson::Object && Value->Type != EJson::Array)) return Value;
			const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
			Shape->SetStringField(TEXT("kind"), Value->Type == EJson::Object ? TEXT("object") : TEXT("array"));
			Shape->SetNumberField(TEXT("count"), Value->Type == EJson::Object ? Value->AsObject()->Values.Num() : Value->AsArray().Num());
			return MakeShared<FJsonValueObject>(Shape);
		}

		TSharedPtr<FJsonValue> NativeCanonicalFact(const FUeAgentCanonicalValue* Value)
		{
			if (!Value) return nullptr;
			switch (Value->GetKind())
			{
			case EUeAgentValueKind::Null: return MakeShared<FJsonValueNull>();
			case EUeAgentValueKind::Boolean: return MakeShared<FJsonValueBoolean>(Value->GetBoolean());
			case EUeAgentValueKind::String: return MakeShared<FJsonValueString>(Value->GetText());
			default: break;
			}
			const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
			const TCHAR* Kind = TEXT("unknown");
			switch (Value->GetKind())
			{
			case EUeAgentValueKind::Null: Kind = TEXT("null"); break;
			case EUeAgentValueKind::Boolean: Kind = TEXT("boolean"); break;
			case EUeAgentValueKind::String: Kind = TEXT("string"); break;
			case EUeAgentValueKind::Integer: Kind = TEXT("integer"); break;
			case EUeAgentValueKind::Decimal: Kind = TEXT("decimal"); break;
			case EUeAgentValueKind::Array: Kind = TEXT("array"); break;
			case EUeAgentValueKind::Object: Kind = TEXT("object"); break;
			case EUeAgentValueKind::Temporal: Kind = TEXT("temporal"); break;
			case EUeAgentValueKind::Spatial: Kind = TEXT("spatial"); break;
			}
			Shape->SetStringField(TEXT("kind"), Kind);
			if (Value->GetKind() == EUeAgentValueKind::Integer || Value->GetKind() == EUeAgentValueKind::Decimal)
				Shape->SetStringField(TEXT("lexeme"), Value->GetLexeme());
			if (Value->GetKind() == EUeAgentValueKind::Array) Shape->SetNumberField(TEXT("count"), Value->GetArrayItems().Num());
			if (Value->GetKind() == EUeAgentValueKind::Object) Shape->SetNumberField(TEXT("count"), Value->GetObjectItems().Num());
			return MakeShared<FJsonValueObject>(Shape);
		}

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
			FUeAgentGenericReadError& OutError,
			const FString& Path)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
					Text,
					Resources,
					Value,
					Error))
			{
				OutError = MoveTemp(Error);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TEXT("niagara_compound_native_capture");
				return NativeCaptureFailure(OutError, FString(), Path);
			}
			OutValue = MoveTemp(Value);
			return true;
		}

		bool TryArray(
			const TArray<FUeAgentCanonicalValueRef>& Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError,
			const FString& Subject,
			const FString& Path)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					Error))
			{
				OutError = MoveTemp(Error);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
				return NativeCaptureFailure(OutError, Subject, Path);
			}
			return true;
		}

		bool TryObject(
			const TArray<
				TPair<FString, FUeAgentCanonicalValueRef>>& Fields,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError,
			const FString& Subject,
			const FString& Path)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(
					Fields,
					Resources,
					OutValue,
					Error))
			{
				OutError = MoveTemp(Error);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
				return NativeCaptureFailure(OutError, Subject, Path);
			}
			return true;
		}

		bool TryMakeCommonFields(
			const FResolvedModuleInput& Input,
			const FString& ModeText,
			FUeAgentResourceTracker& Resources,
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>& OutFields,
			FUeAgentGenericReadError& OutError,
			const FString& Path)
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
					OutError, Path + TEXT("/kind"))
				|| !TryString(
					Input.FullInputName,
					Resources,
					Name,
					OutError, Path + TEXT("/input_name"))
				|| !TryString(
					TypeId.IsEmpty() ? TEXT("unknown") : TypeId,
					Resources,
					Type,
					OutError, Path + TEXT("/type_id"))
				|| !TryString(
					ModeText,
					Resources,
					Mode,
					OutError, Path + TEXT("/mode")))
			{
				return NativeCaptureFailure(OutError, Input.FullInputName, Path);
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
			FUeAgentGenericReadError& OutError,
			const FString& Path)
		{
			if (!Input.VisiblePin)
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_input_pin_missing"),
					TEXT("The literal Niagara input has no readable native pin."), Path + TEXT("/literal/pin_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.FullInputName);
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr LiteralValue;
			if (!TryMakeCommonFields(
					Input,
					TEXT("literal"),
					Resources,
					Fields,
					OutError, Path))
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
			if (!bLiteralValid)
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_literal_capture_invalid"),
					TEXT("The native Niagara input default cannot be read as its declared scalar type."), Path + TEXT("/literal"),
					MakeShared<FJsonValueString>(Input.VisiblePin->DefaultValue),
					MakeShared<FJsonValueString>(TypeId == TEXT("float") ? TEXT("finite float")
						: (TypeId == TEXT("int32") ? TEXT("int32") : TEXT("true|false"))), Input.FullInputName);
				return false;
			}
			if (!TryString(CanonicalLiteral, Resources, LiteralValue, OutError, Path + TEXT("/literal")))
				return NativeCaptureFailure(OutError, Input.FullInputName, Path + TEXT("/literal"));
			if (!LiteralValue.IsValid())
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_literal_capture_invalid"),
					TEXT("Native literal conversion returned no canonical value."), Path + TEXT("/literal/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.FullInputName);
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
				Input.FullInputName, Path);
		}

		bool TryMakeLinkedValue(
			const FResolvedModuleInput& Input,
			const FString& LinkedParameter,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError,
			const FString& Path)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr Linked;
			if (!TryMakeCommonFields(
					Input,
					TEXT("linked_parameter"),
					Resources,
					Fields,
					OutError, Path)
				|| !TryString(
					LinkedParameter,
					Resources,
					Linked,
					OutError, Path + TEXT("/linked_parameter")))
			{
				return NativeCaptureFailure(OutError, Input.FullInputName, Path);
			}
			Fields.Emplace(
				TEXT("linked_parameter"),
				Linked.ToSharedRef());
			return TryObject(
				Fields,
				Resources,
				OutValue,
				OutError,
				Input.FullInputName, Path);
		}

		bool TryMakeDynamicValue(
			const FResolvedModuleInput& Input,
			const UNiagaraNodeFunctionCall& Dynamic,
			const TArray<FUeAgentCanonicalValueRef>& Children,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError,
			const FString& Path)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr Script;
			FUeAgentCanonicalValuePtr Inputs;
			if (!TryMakeCommonFields(
					Input,
					TEXT("dynamic_input"),
					Resources,
					Fields,
					OutError, Path)
				|| !TryString(
					GetModuleScriptPath(Dynamic),
					Resources,
					Script,
					OutError, Path + TEXT("/dynamic_script_identity"))
				|| !TryArray(
					Children,
					Resources,
					Inputs,
					OutError,
					Input.FullInputName, Path + TEXT("/inputs")))
			{
				return NativeCaptureFailure(OutError, Input.FullInputName, Path);
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
				Input.FullInputName, Path);
		}

		bool TryReadLinkedParameter(
			const FResolvedModuleInput& Input,
			FString& OutLinkedParameter,
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& Path)
		{
			OutLinkedParameter.Reset();
			if (!Input.VisiblePin)
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_input_pin_missing"),
					TEXT("The linked Niagara input has no readable native pin."), Path + TEXT("/linked_parameter/pin_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.FullInputName);
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
				return NativeCaptureFailure(OutError, Input.FullInputName, Path + TEXT("/linked_parameter"));
			}
			const TSharedPtr<FJsonObject>* Value = nullptr;
			FString Source;
			if (!bHasValue || !Payload.IsValid())
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_linked_value_missing"),
					!bHasValue ? TEXT("The linked Niagara pin returned no value.") : TEXT("The linked Niagara pin returned no value payload."),
					Path + (!bHasValue ? TEXT("/linked_parameter/has_value") : TEXT("/linked_parameter/payload_available")),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.FullInputName);
				return false;
			}
			if (!Payload->TryGetObjectField(TEXT("value"), Value) || !Value || !Value->IsValid())
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_linked_value_invalid"),
					TEXT("The linked Niagara pin value must be an object."), Path + TEXT("/linked_parameter/value"),
					NativeJsonFact(Payload->TryGetField(TEXT("value"))), MakeShared<FJsonValueString>(TEXT("object")), Input.FullInputName);
				return false;
			}
			if (!(*Value)->TryGetStringField(TEXT("source"), Source) || !Source.Equals(TEXT("linked_input"), ESearchCase::IgnoreCase))
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_linked_source_invalid"),
					TEXT("The linked Niagara pin value does not describe a linked input."), Path + TEXT("/linked_parameter/source"),
					NativeJsonFact((*Value)->TryGetField(TEXT("source"))), MakeShared<FJsonValueString>(TEXT("linked_input")), Input.FullInputName);
				return false;
			}
			if (!(*Value)->TryGetStringField(TEXT("linked_parameter"), OutLinkedParameter)
				|| OutLinkedParameter.TrimStartAndEnd().IsEmpty())
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_linked_parameter_invalid"),
					TEXT("The linked Niagara pin value requires a non-empty parameter name."), Path + TEXT("/linked_parameter"),
					NativeJsonFact((*Value)->TryGetField(TEXT("linked_parameter"))),
					MakeShared<FJsonValueString>(TEXT("non-empty parameter name")), Input.FullInputName);
				return false;
			}
			return true;
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
				FString Path;
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
					TEXT("/niagara_input"),
				});
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				FNode& Node = Nodes[Index];
				if (!Node.Owner
					|| !Node.Input.InputVariable.IsValid())
				{
					OutError = NativeCompoundFact(TEXT("niagara_compound_capture_node_invalid"),
						!Node.Owner ? TEXT("Native Niagara input traversal lost its owner node.")
							: TEXT("Native Niagara input traversal found an invalid input variable."),
						Node.Path + (!Node.Owner ? TEXT("/owner_available") : TEXT("/input_variable/valid")),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Node.Input.FullInputName);
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
				Node.Dynamic = FindDynamicInputForModuleInput(*Node.Owner, Node.Input.FullInputName, ResolvedName, OutError);
				if (OutError.IsValid()) return NativeCaptureFailure(OutError, Node.Input.FullInputName, Node.Path);
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
						OutError = NativeCompoundFact(TEXT("niagara_compound_dynamic_cycle_detected"),
							TEXT("The Niagara dynamic input recursively references an active ancestor node."), Node.Path + TEXT("/dynamic_cycle"),
							MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Node.Input.FullInputName);
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Node.Dynamic->GetPathName());
						return false;
					}
				}
				FUeAgentResourceChargeError ChargeError;
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
						ChargeError))
				{
					OutError = MoveTemp(ChargeError);
					return NativeCaptureFailure(OutError, Node.Input.FullInputName, Node.Path);
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
					const FString ChildPath = Nodes[Index].Path + TEXT("/inputs/")
						+ FString::FromInt(Nodes[Index].ChildIndices.Num());
					FString ChildName;
					UNiagaraNodeFunctionCall* ChildDynamic =
						FindDynamicInputForModuleInput(*DynamicNode, Input.FullInputName, ChildName, OutError);
					if (OutError.IsValid()) return NativeCaptureFailure(OutError, Input.FullInputName, ChildPath);
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
							ChildPath,
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
					for (int32 Position = 0; Position < Node.ChildIndices.Num(); ++Position)
					{
						const int32 ChildIndex = Node.ChildIndices[Position];
						if (!Nodes.IsValidIndex(ChildIndex)
							|| !Nodes[ChildIndex].Value.IsValid())
						{
							OutError = NativeCompoundFact(TEXT("niagara_compound_capture_child_invalid"),
								!Nodes.IsValidIndex(ChildIndex) ? TEXT("The native dynamic input references a missing captured child node.")
									: TEXT("The native dynamic input child has no captured canonical value."),
								Node.Path + TEXT("/inputs/") + FString::FromInt(Position) + TEXT("/available"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Node.Input.FullInputName);
							OutError.ActualEvidence = MakeShared<FJsonValueNumber>(ChildIndex);
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
							OutError, Node.Path))
					{
						return false;
					}
					continue;
				}
				if (Node.Input.InputVariable
						.GetType()
						.IsDataInterface())
				{
					OutError = NativeCompoundFact(TEXT("niagara_compound_data_interface_capture_not_bound"),
						TEXT("Canonical capture is not bound for this Niagara data interface type."), Node.Path + TEXT("/data_interface/type"),
						MakeShared<FJsonValueString>(CanonicalNiagaraTypeId(Node.Input.InputVariable.GetType())),
						MakeShared<FJsonValueString>(TEXT("data interface type with a canonical capture provider")), Node.Input.FullInputName);
					return false;
				}
				FString LinkedParameter;
				UE::UAI::Contract::FDiagnostic NativeError;
				if (Node.Input.VisiblePin
					&& !Node.Input.VisiblePin->LinkedTo.IsEmpty())
				{
					if (!TryReadLinkedParameter(
							Node.Input,
							LinkedParameter,
							NativeError, Node.Path))
					{
						OutError = MoveTemp(NativeError);
						return NativeCaptureFailure(OutError, Node.Input.FullInputName, Node.Path);
					}
					if (!TryMakeLinkedValue(
							Node.Input,
							LinkedParameter,
							Resources,
							Node.Value,
							OutError, Node.Path))
					{
						return false;
					}
				}
				else if (!TryMakeLiteralValue(
						Node.Input,
						Resources,
						Node.Value,
						OutError, Node.Path))
				{
					return false;
				}
			}
			OutValue = Nodes[0].Value;
			if (!OutValue.IsValid())
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_capture_value_missing"),
					TEXT("Native Niagara input capture returned no canonical root value."), TEXT("/niagara_input/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), RootInput.FullInputName);
				return false;
			}
			return true;
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
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& Path,
			const FString& InputName)
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
				if (!Mode || (*Mode)->GetKind() != EUeAgentValueKind::String || (*Mode)->GetText() != TEXT("literal"))
					OutError = NativeCompoundFact(TEXT("niagara_compound_literal_type_unsupported"),
						TEXT("Literal conversion requires the literal mode."), Path + TEXT("/mode"),
						NativeCanonicalFact(Mode ? &Mode->Get() : nullptr), MakeShared<FJsonValueString>(TEXT("literal")), InputName, TEXT("niagara_apply"));
				else if (!Literal)
					OutError = NativeCompoundFact(TEXT("niagara_compound_literal_type_unsupported"),
						TEXT("Literal conversion requires a literal value."), Path + TEXT("/literal"),
						nullptr, MakeShared<FJsonValueString>(TEXT("present scalar value")), InputName, TEXT("niagara_apply"));
				else
					OutError = NativeCompoundFact(TEXT("niagara_compound_literal_type_unsupported"),
						TEXT("Literal conversion requires a string type identifier."), Path + TEXT("/type_id"),
						NativeCanonicalFact(Type ? &Type->Get() : nullptr), MakeShared<FJsonValueString>(TEXT("string")), InputName, TEXT("niagara_apply"));
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
				OutError = NativeCompoundFact(TEXT("niagara_compound_literal_type_unsupported"),
					TEXT("The Niagara literal value must be a string, number or boolean."), Path + TEXT("/literal"),
					NativeCanonicalFact(&Literal->Get()), MakeShared<FJsonValueString>(TEXT("string|integer|decimal|boolean")), InputName, TEXT("niagara_apply"));
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
					OutError = NativeCompoundFact(TEXT("niagara_compound_literal_invalid"),
						TEXT("The Niagara literal cannot be parsed as a finite float."), Path + TEXT("/literal"), MakeShared<FJsonValueString>(LiteralText),
						MakeShared<FJsonValueString>(TEXT("finite float")), InputName, TEXT("niagara_apply"));
					return false;
				}
				Payload->SetStringField(
					TEXT("kind"),
					TEXT("float"));
				Payload->SetNumberField(TEXT("value"), Number);
			}
			else if ((*Type)->GetText() == TEXT("bool"))
			{
				FString Text = LiteralText.TrimStartAndEnd();
				Text.ToLowerInline();
				if (Text != TEXT("true") && Text != TEXT("false"))
				{
					OutError = NativeCompoundFact(TEXT("niagara_compound_literal_invalid"),
						TEXT("The Niagara literal cannot be parsed as a boolean."), Path + TEXT("/literal"), MakeShared<FJsonValueString>(LiteralText),
						MakeShared<FJsonValueString>(TEXT("true|false")), InputName, TEXT("niagara_apply"));
					return false;
				}
				Payload->SetStringField(
					TEXT("kind"),
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
					OutError = NativeCompoundFact(TEXT("niagara_compound_literal_invalid"),
						TEXT("The Niagara literal cannot be parsed as an int32."), Path + TEXT("/literal"), MakeShared<FJsonValueString>(LiteralText),
						MakeShared<FJsonValueString>(TEXT("int32")), InputName, TEXT("niagara_apply"));
					return false;
				}
				Payload->SetStringField(
					TEXT("kind"),
					TEXT("int"));
				Payload->SetNumberField(TEXT("value"), Integer);
			}
			else
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_literal_type_unsupported"),
					TEXT("This Niagara input type has no scalar literal payload conversion."), Path + TEXT("/type_id"),
					MakeShared<FJsonValueString>((*Type)->GetText()), MakeShared<FJsonValueString>(TEXT("float|bool|int32")), InputName, TEXT("niagara_apply"));
				return false;
			}
			OutPayload = Payload;
			return true;
		}

		bool TryReadRequiredString(
			const FUeAgentCanonicalValue& Value,
			const FString& Name,
			FString& OutText,
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& Path,
			const FString& InputName)
		{
			const FUeAgentCanonicalValueRef* Field = FindField(Value, Name);
			if (!Field || (*Field)->GetKind() != EUeAgentValueKind::String)
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_field_invalid"),
					Value.GetKind() != EUeAgentValueKind::Object ? TEXT("The Niagara input value must be an object.")
						: TEXT("The Niagara input field must be a string."),
					Value.GetKind() != EUeAgentValueKind::Object ? Path : Path + TEXT("/") + Name,
					NativeCanonicalFact(Value.GetKind() != EUeAgentValueKind::Object ? &Value : (Field ? &Field->Get() : nullptr)),
					MakeShared<FJsonValueString>(Value.GetKind() != EUeAgentValueKind::Object ? TEXT("object") : TEXT("string")),
					InputName, TEXT("niagara_apply"));
				return false;
			}
			OutText = (*Field)->GetText().TrimStartAndEnd();
			if (OutText.IsEmpty())
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_field_invalid"),
					TEXT("The Niagara input field requires a non-empty string."), Path + TEXT("/") + Name,
					NativeCanonicalFact(&Field->Get()), MakeShared<FJsonValueString>(TEXT("non-empty string")), InputName, TEXT("niagara_apply"));
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
			Stack.Add({&RootOwner, RootInputName, RootValue, TEXT("/niagara_input")});
			while (!Stack.IsEmpty())
			{
				FFrame Frame = Stack.Pop(EAllowShrinking::No);
				auto ApplyFact = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Suffix,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					return NativeCompoundFact(Code, Reason, Frame.Path + Suffix, MoveTemp(Actual), MoveTemp(Expected),
						Frame.InputName, TEXT("niagara_apply"));
				};
				if (!Frame.Owner)
				{
					OutError = ApplyFact(TEXT("niagara_compound_apply_owner_missing"),
						TEXT("The Niagara input owner is unavailable during native application."), TEXT("/owner_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				FUeAgentResourceChargeError ChargeError;
				if (!Resources.TryChargeMany(
					{{EUeAgentResourceDimension::Nodes, 1}, {EUeAgentResourceDimension::WorkUnits, 1}}, Frame.Path, ChargeError))
				{
					OutError = MoveTemp(ChargeError);
					return NativeCaptureFailure(OutError, Frame.InputName, Frame.Path);
				}
				FString InputName, TypeId, Mode;
				if (!TryReadRequiredString(*Frame.Value, TEXT("input_name"), InputName, OutError, Frame.Path, Frame.InputName)
					|| !TryReadRequiredString(*Frame.Value, TEXT("type_id"), TypeId, OutError, Frame.Path, Frame.InputName)
					|| !TryReadRequiredString(*Frame.Value, TEXT("mode"), Mode, OutError, Frame.Path, Frame.InputName))
					return false;
				TArray<FResolvedModuleInput> ResolvedInputs;
				BuildResolvedModuleInputs(*Frame.Owner, ResolvedInputs);
				FString ResolvedInputName;
				const FString RequestedInputName = Frame.InputName.IsEmpty() ? InputName : Frame.InputName;
				const FResolvedModuleInput* ResolvedInput = ResolveModuleInputByName(ResolvedInputs, RequestedInputName, ResolvedInputName);
				if (!ResolvedInput)
				{
					OutError = ApplyFact(TEXT("niagara_compound_input_not_found"),
						TEXT("The requested Niagara input name is absent from the native owner."), TEXT("/input_name"),
						MakeShared<FJsonValueString>(RequestedInputName), MakeShared<FJsonValueString>(TEXT("existing native input name")));
					return false;
				}
				if (!ResolvedInput->InputVariable.IsValid())
				{
					OutError = ApplyFact(TEXT("niagara_compound_input_not_found"),
						TEXT("The resolved Niagara input variable is not valid."), TEXT("/input_variable/valid"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (!InputName.Equals(ResolvedInput->FullInputName, ESearchCase::CaseSensitive))
				{
					OutError = ApplyFact(TEXT("niagara_compound_input_type_mismatch"),
						TEXT("The authored input name does not match the resolved Niagara input name."), TEXT("/input_name"),
						MakeShared<FJsonValueString>(InputName), MakeShared<FJsonValueString>(ResolvedInput->FullInputName));
					return false;
				}
				const FString NativeType = CanonicalNiagaraTypeId(ResolvedInput->InputVariable.GetType());
				if (TypeId != NativeType)
				{
					OutError = ApplyFact(TEXT("niagara_compound_input_type_mismatch"),
						TEXT("The authored Niagara input type does not match the native input type."), TEXT("/type_id"),
						MakeShared<FJsonValueString>(TypeId), MakeShared<FJsonValueString>(NativeType));
					return false;
				}
				bool bChanged = false;
				UE::UAI::Contract::FDiagnostic NativeError;
				const FString TargetInputName = ResolvedInput->FullInputName;
				FString AppliedInputName;
				if (Mode == TEXT("literal"))
				{
					TSharedPtr<const FJsonObject> Payload;
					if (!TryBuildLiteralPayload(*Frame.Value, Payload, NativeError, Frame.Path, TargetInputName)
						|| !SetLiteralInputValue(*Frame.Owner, TargetInputName, Payload, AppliedInputName, bChanged, NativeError))
					{
						OutError = MoveTemp(NativeError);
						return NativeCaptureFailure(OutError, TargetInputName, Frame.Path + TEXT("/literal"));
					}
				}
				else if (Mode == TEXT("linked_parameter"))
				{
					FString LinkedParameter;
					if (!TryReadRequiredString(*Frame.Value, TEXT("linked_parameter"), LinkedParameter, NativeError, Frame.Path, TargetInputName)
						|| !SetLinkedInputValue(*Frame.Owner, TargetInputName, LinkedParameter, AppliedInputName, bChanged, NativeError))
					{
						OutError = MoveTemp(NativeError);
						return NativeCaptureFailure(OutError, TargetInputName, Frame.Path + TEXT("/linked_parameter"));
					}
				}
				else if (Mode == TEXT("dynamic_input"))
				{
					FString ScriptIdentity;
					const FUeAgentCanonicalValueRef* Inputs = FindField(*Frame.Value, TEXT("inputs"));
					if (!TryReadRequiredString(*Frame.Value, TEXT("dynamic_script_identity"), ScriptIdentity, OutError, Frame.Path, TargetInputName))
						return false;
					if (!Inputs || (*Inputs)->GetKind() != EUeAgentValueKind::Array)
					{
						OutError = ApplyFact(TEXT("niagara_compound_dynamic_value_invalid"),
							TEXT("The Niagara dynamic input requires an input array."), TEXT("/inputs"),
							NativeCanonicalFact(Inputs ? &Inputs->Get() : nullptr), MakeShared<FJsonValueString>(TEXT("array")));
						return false;
					}
					UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptIdentity);
					if (!IsValid(Script))
					{
						OutError = ApplyFact(TEXT("niagara_compound_dynamic_script_invalid"),
							TEXT("The Niagara dynamic input script asset could not be loaded."), TEXT("/dynamic_script_identity"),
							MakeShared<FJsonValueString>(ScriptIdentity), MakeShared<FJsonValueString>(TEXT("loadable Niagara script asset")));
						return false;
					}
					if (Script->GetUsage() != ENiagaraScriptUsage::DynamicInput)
					{
						OutError = ApplyFact(TEXT("niagara_compound_dynamic_script_invalid"),
							TEXT("The selected Niagara script does not have DynamicInput usage."), TEXT("/dynamic_script_usage"),
							MakeShared<FJsonValueString>(UEnum::GetValueAsString(Script->GetUsage())),
							MakeShared<FJsonValueString>(UEnum::GetValueAsString(ENiagaraScriptUsage::DynamicInput)));
						OutError.TargetPath = ScriptIdentity;
						return false;
					}
					UNiagaraNodeFunctionCall* Dynamic = nullptr;
					if (!SetDynamicInput(*Frame.Owner, TargetInputName, *Script, Script->GetName(), Dynamic, AppliedInputName, NativeError))
					{
						OutError = MoveTemp(NativeError);
						return NativeCaptureFailure(OutError, TargetInputName, Frame.Path + TEXT("/dynamic_script_identity"));
					}
					if (!InitializeDynamicInputDataInterfaces(*Dynamic, NativeError))
					{
						OutError = MoveTemp(NativeError);
						return NativeCaptureFailure(OutError, TargetInputName, Frame.Path + TEXT("/dynamic_script_identity"));
					}
					const TArray<FUeAgentCanonicalValueRef>& Children = (*Inputs)->GetArrayItems();
					for (int32 Index = Children.Num() - 1; Index >= 0; --Index)
					{
						const FString ChildPath = FString::Printf(TEXT("%s/inputs/%d"), *Frame.Path, Index);
						FString ChildName;
						if (!TryReadRequiredString(*Children[Index], TEXT("input_name"), ChildName, OutError, ChildPath, TargetInputName))
							return false;
						Stack.Add({Dynamic, ChildName, Children[Index], ChildPath});
					}
					bChanged = true;
				}
				else if (Mode == TEXT("data_interface"))
				{
					OutError = ApplyFact(TEXT("niagara_compound_data_interface_apply_not_bound"),
						TEXT("Native application is not bound for this Niagara data interface type."), TEXT("/type_id"),
						MakeShared<FJsonValueString>(TypeId), MakeShared<FJsonValueString>(TEXT("data interface type with a native apply provider")));
					return false;
				}
				else
				{
					OutError = ApplyFact(TEXT("niagara_compound_mode_invalid"),
						TEXT("The Niagara input mode is not supported."), TEXT("/mode"),
						MakeShared<FJsonValueString>(Mode), MakeShared<FJsonValueString>(TEXT("literal|linked_parameter|dynamic_input|data_interface")));
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutCompile = UeAgentNiagaraCompileValidation::FResult();
			OutStack = UeAgentNiagaraStackValidation::FResult();
			const FString AssetPath = Asset.GetPathName();
			if (UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset))
			{
				System->RequestCompile(true);
				System->WaitForCompilationComplete(true, false);
				System->PollForCompilationComplete(true);
				if (System->HasOutstandingCompilationRequests(true))
				{
					OutError = NativeCompoundFact(TEXT("niagara_compile_incomplete"),
						TEXT("The Niagara system still has outstanding compilation requests after waiting."),
						TEXT("/compilation/outstanding_requests"), MakeShared<FJsonValueBoolean>(true),
						MakeShared<FJsonValueBoolean>(false), FString(), TEXT("niagara_compile"));
					OutError.TargetPath = AssetPath;
					return false;
				}
				System->CacheFromCompiledData();
				OutCompile = UeAgentNiagaraCompileValidation::InspectAsset(System);
			}
			else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset))
			{
				UeAgentNiagaraEmitterCompileContext::FResult Result;
				if (!UeAgentNiagaraEmitterCompileContext::Compile(
					*Emitter, Emitter->GetExposedVersion().VersionGuid, true, Result, OutError))
				{
					return NativeCaptureFailure(OutError, FString(), FString(), AssetPath);
				}
				OutCompile = Result.Validation;
			}
			else if (UNiagaraScript* Script = Cast<UNiagaraScript>(&Asset))
			{
				Script->RequestCompile(Script->GetExposedVersion().VersionGuid, true);
				OutCompile = UeAgentNiagaraCompileValidation::InspectAsset(Script);
			}
			else
			{
				OutError = NativeCompoundFact(TEXT("niagara_compile_target_invalid"),
					TEXT("The compilation target is not a Niagara system, emitter, or script."),
					TEXT("/compilation/target_class"), MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(TEXT("NiagaraSystem | NiagaraEmitter | NiagaraScript")),
					FString(), TEXT("niagara_compile"));
				OutError.TargetPath = AssetPath;
				return false;
			}
			if (!OutCompile.bCompilationComplete)
			{
				OutError = NativeCompoundFact(TEXT("niagara_compile_incomplete"),
					TEXT("Niagara compilation did not reach a complete state."),
					TEXT("/compilation/complete"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), FString(), TEXT("niagara_compile"));
				OutError.TargetPath = AssetPath;
				return false;
			}
			OutStack = UeAgentNiagaraStackValidation::InspectAsset(&Asset);
			if (!OutStack.bInspectionComplete)
			{
				OutError = OutStack.GetFailure();
				return NativeCaptureFailure(OutError, FString(), FString(), AssetPath);
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
			FString AssetPath;
			FUeAgentCanonicalValuePtr BeforeValue;
		};

		struct FAssetReference
		{
			TWeakObjectPtr<UObject> Asset;
			FString AssetPath;
		};

		struct FOperation
		{
			int64 SubjectGeneration = -1;
			TArray<FAppliedChange> AppliedChanges;
			TArray<FAssetReference> Assets;
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
				UE::UAI::Contract::FDiagnostic Error;
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
					*Error.ToPublicDetail());
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
			OutError = Cancelled;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetBinding().GetBindingId();
			return NativeCaptureFailure(OutError, FString(), TEXT("/niagara_input"),
				Request.GetBinding().GetSummary().GetIdentity().GetOwnerId());
		}
		FUeAgentResolvedNiagaraCompoundInput Resolved;
		UE::UAI::Contract::FDiagnostic ResolveError;
		if (!TryResolveNiagaraCompoundInput(
				Request.GetBinding(),
				Generation,
				Resolved, ResolveError))
		{
			OutError = ResolveError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetBinding().GetBindingId();
			return NativeCaptureFailure(OutError, FString(), TEXT("/niagara_input"),
				Request.GetBinding().GetSummary().GetIdentity().GetOwnerId());
		}
		const FString AssetPath = Request.GetBinding().GetSummary().GetIdentity().GetOwnerId();
		if (!Resolved.Asset || !Resolved.Module || !Resolved.Input.InputVariable.IsValid())
		{
			const bool bAssetMissing = !Resolved.Asset;
			const bool bModuleMissing = !Resolved.Module;
			OutError = NativeCompoundFact(TEXT("niagara_compound_persisted_state_unavailable"),
				bAssetMissing ? TEXT("The resolved Niagara asset is no longer available.")
					: (bModuleMissing ? TEXT("The resolved Niagara module is no longer available.")
						: TEXT("The resolved Niagara input has no valid variable type.")),
				bAssetMissing ? TEXT("/niagara_input/asset_available")
					: (bModuleMissing ? TEXT("/niagara_input/module_available") : TEXT("/niagara_input/variable_valid")),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
				Resolved.Input.FullInputName);
			OutError.TargetPath = AssetPath;
			return false;
		}
		if (Request.GetStatePlane() == EUeAgentStatePlane::Persisted
			&& Resolved.Asset->GetOutermost()->IsDirty())
		{
			OutError = NativeCompoundFact(TEXT("niagara_compound_persisted_state_unavailable"),
				TEXT("Persisted Niagara input state is unavailable because the asset package has unsaved changes."),
				TEXT("/niagara_input/package_dirty"), MakeShared<FJsonValueBoolean>(true),
				MakeShared<FJsonValueBoolean>(false), Resolved.Input.FullInputName);
			OutError.TargetPath = Resolved.Asset->GetPathName();
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
			return NativeCaptureFailure(OutError, Resolved.Input.FullInputName,
				TEXT("/niagara_input"), Resolved.Asset->GetPathName());
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
		auto OperationAlreadyApplied = [&Request]()
		{
			auto Error = NativeCompoundFact(TEXT("niagara_compound_operation_already_applied"),
				TEXT("This Niagara compound operation has already been applied."),
				TEXT("/operation/state"), MakeShared<FJsonValueString>(TEXT("already_applied")),
				MakeShared<FJsonValueString>(TEXT("not_applied")), FString(), TEXT("niagara_apply"));
			Error.ActualEvidence = MakeShared<FJsonValueString>(Request.GetOperationId());
			return Error;
		};
		{
			FScopeLock Lock(&State->Mutex);
			if (State->AppliedByOperation.Contains(Request.GetOperationId()))
			{
				OutError = OperationAlreadyApplied();
				return false;
			}
		}
		OutResult = FUeAgentGenericEffectApplyResult::NoMutation();
		TArray<FState::FAppliedChange> AppliedItems;
		TArray<UObject*> Assets;
		TArray<FString> MutatedKeys;
		auto RestoreApplied = [&Resources](TArray<FState::FAppliedChange>& Items,
			UE::UAI::Contract::FDiagnostic& OutRestoreError)
		{
			for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
			{
				FState::FAppliedChange& Item = Items[Index];
				UNiagaraNodeFunctionCall* Module = Item.Module.Get();
				if (!Module || !Item.BeforeValue.IsValid())
				{
					OutRestoreError = NativeCompoundFact(TEXT("niagara_compound_restore_target_lost"),
						!Module ? TEXT("The Niagara module needed to restore the input is no longer available.")
							: TEXT("The previous Niagara input value needed for restoration is unavailable."),
						!Module ? TEXT("/niagara_input/module_available") : TEXT("/niagara_input/before_value_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
						Item.InputName, TEXT("rollback"));
					OutRestoreError.TargetPath = Item.AssetPath;
					return false;
				}
				bool bChanged = false;
				if (!TryApplyCanonicalInput(*Module, Item.InputName, Item.BeforeValue.ToSharedRef(),
					Resources, bChanged, OutRestoreError))
				{
					return NativeCaptureFailure(OutRestoreError, Item.InputName, TEXT("/niagara_input"), Item.AssetPath);
				}
			}
			return true;
		};
		auto FailWithRestore = [&AppliedItems, &RestoreApplied, &OutError](UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			UE::UAI::Contract::FDiagnostic RestoreError;
			const bool bAttempted = !AppliedItems.IsEmpty();
			const bool bRestored = !bAttempted || RestoreApplied(AppliedItems, RestoreError);
			OutError = MoveTemp(Diagnostic);
			OutError.RollbackAttempted = bAttempted;
			OutError.RollbackSucceeded = bRestored;
			OutError.Append(RestoreError);
			return false;
		};
		for (const FPlannedChange& Change : Request.GetPlan()->GetChangeSet().GetChanges())
		{
			const FString IdentityKey = Change.GetIdentity().GetCanonicalKey();
			const FUeAgentGenericEffectBinding* Binding = FindBinding(Bindings, IdentityKey);
			if (!Binding || !Lease.GetLockKeys().Contains(IdentityKey) || !Change.GetAfter().IsValid())
			{
				const bool bBindingMissing = !Binding;
				const bool bLockMissing = !Lease.GetLockKeys().Contains(IdentityKey);
				auto Error = NativeCompoundFact(TEXT("niagara_compound_effect_binding_missing"),
					bBindingMissing ? TEXT("The planned Niagara input change has no effect binding.")
						: (bLockMissing ? TEXT("The Niagara input change is not covered by the acquired lock lease.")
							: TEXT("The planned Niagara input change has no replacement value.")),
					bBindingMissing ? TEXT("/change/effect_binding_available")
						: (bLockMissing ? TEXT("/change/lock_covered") : TEXT("/change/after_value_available")),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					FString(), TEXT("niagara_apply"));
				Error.TargetPath = Change.GetIdentity().GetOwnerId();
				Error.ActualEvidence = MakeShared<FJsonValueString>(IdentityKey);
				return FailWithRestore(MoveTemp(Error));
			}
			FUeAgentResolvedNiagaraCompoundInput Resolved;
			UE::UAI::Contract::FDiagnostic ResolveError;
			if (!TryResolveNiagaraCompoundInput(Binding->GetCandidateBinding(), Generation, Resolved, ResolveError))
			{
				NativeCaptureFailure(ResolveError, FString(), TEXT("/niagara_input"), Change.GetIdentity().GetOwnerId());
				return FailWithRestore(MoveTemp(ResolveError));
			}
			FUeAgentCanonicalValuePtr NativeBefore;
			FUeAgentGenericReadError CaptureError;
			if (!TryCaptureCanonicalInput(*Resolved.Module, Resolved.Input, Resources, NativeBefore, CaptureError))
			{
				NativeCaptureFailure(CaptureError, Resolved.Input.FullInputName, TEXT("/niagara_input"), Resolved.Asset->GetPathName());
				return FailWithRestore(MoveTemp(CaptureError));
			}
			FState::FAppliedChange Applied;
			Applied.Module = Resolved.Module;
			Applied.InputName = Resolved.Input.FullInputName;
			Applied.AssetPath = Resolved.Asset->GetPathName();
			Applied.BeforeValue = MoveTemp(NativeBefore);
			AppliedItems.Add(Applied);
			bool bChanged = false;
			UE::UAI::Contract::FDiagnostic NativeError;
			OutResult = FUeAgentGenericEffectApplyResult();
			if (!TryApplyCanonicalInput(*Resolved.Module, Resolved.Input.FullInputName,
				Change.GetAfter().ToSharedRef(), Resources, bChanged, NativeError))
			{
				NativeCaptureFailure(NativeError, Resolved.Input.FullInputName, TEXT("/niagara_input"), Resolved.Asset->GetPathName());
				return FailWithRestore(MoveTemp(NativeError));
			}
			if (bChanged) MutatedKeys.Add(IdentityKey);
			Assets.AddUnique(Resolved.Asset);
		}
		int64 SubjectGeneration = 0;
		bool bOperationAlreadyApplied = false;
		{
			FScopeLock Lock(&State->Mutex);
			if (State->AppliedByOperation.Contains(Request.GetOperationId()))
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
					FState::FAssetReference AssetReference;
					AssetReference.Asset = Asset;
					AssetReference.AssetPath = Asset->GetPathName();
					Operation.Assets.Add(MoveTemp(AssetReference));
				}
				State->AppliedByOperation.Add(Request.GetOperationId(), MoveTemp(Operation));
				State->OperationByGeneration.Add(SubjectGeneration, Request.GetOperationId());
			}
		}
		if (bOperationAlreadyApplied) return FailWithRestore(OperationAlreadyApplied());
		const bool bDidMutationStart = !MutatedKeys.IsEmpty();
		OutResult = FUeAgentGenericEffectApplyResult(SubjectGeneration, MoveTemp(MutatedKeys),
			{TEXT("niagara.compile"), TEXT("niagara.stack")}, true, Request.GetOperationId(), bDidMutationStart);
		if (!OutResult.IsValid())
		{
			OutError = OutResult.GetValidationDiagnostic();
			return false;
		}
		return true;
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
				OutError = NativeCompoundFact(TEXT("niagara_compound_rollback_state_missing"),
					TEXT("The saved Niagara compound operation needed for rollback is unavailable."),
					TEXT("/rollback/state_available"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), FString(), TEXT("rollback"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Applied.GetRollbackToken());
				OutError.RollbackAttempted = true;
				OutError.RollbackSucceeded = false;
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
				OutError = NativeCompoundFact(TEXT("niagara_compound_rollback_target_lost"),
					!Module ? TEXT("The Niagara module needed to roll back the input is no longer available.")
						: TEXT("The previous Niagara input value needed for rollback is unavailable."),
					!Module ? TEXT("/niagara_input/module_available") : TEXT("/niagara_input/before_value_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					Item.InputName, TEXT("rollback"));
				OutError.TargetPath = Item.AssetPath;
				OutError.RollbackAttempted = true;
				OutError.RollbackSucceeded = false;
				return false;
			}
			bool bChanged = false;
			UE::UAI::Contract::FDiagnostic NativeError;
			if (!TryApplyCanonicalInput(
					*Module,
					Item.InputName,
					Item.BeforeValue.ToSharedRef(),
					Resources,
					bChanged,
					NativeError))
			{
				OutError = NativeError;
				OutError.RollbackAttempted = true;
				OutError.RollbackSucceeded = false;
				return NativeCaptureFailure(OutError, Item.InputName, TEXT("/niagara_input"), Item.AssetPath);
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
				OutError = NativeCompoundFact(TEXT("niagara_compound_settle_generation_missing"),
					TEXT("No applied Niagara compound operation is registered for this settle request."),
					TEXT("/settle/operation_available"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), FString(), TEXT("niagara_settle"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Request.GetSubjectGeneration()));
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
		for (const FState::FAssetReference& AssetReference : Operation.Assets)
		{
			UObject* Asset = AssetReference.Asset.Get();
			if (!Asset)
			{
				OutError = NativeCompoundFact(TEXT("niagara_compound_settle_target_lost"),
					TEXT("The Niagara asset needed to compile and settle the applied operation is no longer available."),
					TEXT("/settle/asset_available"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), FString(), TEXT("niagara_settle"));
				OutError.TargetPath = AssetReference.AssetPath;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Request.GetSubjectGeneration()));
				return false;
			}
			FUeAgentResourceChargeError ChargeError;
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
					ChargeError))
			{
				OutError = ChargeError;
				return NativeCaptureFailure(OutError, FString(), FString(), AssetReference.AssetPath);
			}
			UeAgentNiagaraCompileValidation::FResult Compile;
			UeAgentNiagaraStackValidation::FResult Stack;
			UE::UAI::Contract::FDiagnostic NativeError;
			if (!TryCompileNiagaraAsset(
					*Asset,
					Compile,
					Stack,
					NativeError))
			{
				OutError = MoveTemp(NativeError);
				return NativeCaptureFailure(OutError, FString(), FString(), AssetReference.AssetPath);
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
			OutError = OutReceipt.GetValidationDiagnostic();
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
