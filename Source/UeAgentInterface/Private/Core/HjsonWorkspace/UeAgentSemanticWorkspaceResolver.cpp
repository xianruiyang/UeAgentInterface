#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"

#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		const FString ManifestPath =
			TEXT(".uai/workspace.hjson");
		const FString EmbeddedPath =
			TEXT("workspace.hjson");

		struct FInventoryRow
		{
			FString UnitId;
			ESemanticUnitKind UnitKind =
				ESemanticUnitKind::OpaqueExtension;
			FString LogicalPath;
			FString ContentHash;
		};

		struct FParsedSource
		{
			FHjsonDocumentPtr Envelope;
			FHjsonDocumentPtr ManifestDocument;
			TArray<FHjsonDocumentPtr> UnitDocuments;
			const FHjsonNode* Manifest = nullptr;
			TArray<const FHjsonNode*> UnitRows;
			TArray<FString> ManagedPaths;
		};

		bool Fail(
			FWorkspaceError& OutError,
			const FString& Code,
			const FString& SourceUnit = FString(),
			const FString& JsonPointer = TEXT("/"))
		{
			OutError.Set(
				Code,
				SourceUnit,
				JsonPointer);
			return false;
		}

		bool FailResource(
			FWorkspaceError& OutError,
			const FUeAgentResourceBudgetExceeded& Error)
		{
			OutError.Set(
				Error.Code,
				Error.Path,
				TEXT("/"),
				Error.Observed,
				Error.bStoppedEarly);
			return false;
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			std::initializer_list<TPair<
				EUeAgentResourceDimension,
				int64>> Charges,
			const FString& Path,
			FWorkspaceError& OutError)
		{
			FUeAgentResourceBudgetExceeded ResourceError;
			TMap<EUeAgentResourceDimension, int64> Values;
			for (const TPair<
				EUeAgentResourceDimension,
				int64>& ChargeValue : Charges)
			{
				Values.Add(
					ChargeValue.Key,
					ChargeValue.Value);
			}
			return Resources.TryChargeMany(
					Values,
					Path,
					ResourceError)
				|| FailResource(OutError, ResourceError);
		}

		FString Sha256(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			const uint8 Empty = 0;
			SHA256(
				Bytes.IsEmpty() ? &Empty : Bytes.GetData(),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString Sha256Text(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
			return Sha256(Bytes);
		}

		const FHjsonNode* RequiredNode(
			const FHjsonNode& Object,
			const FString& Key,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Result = Object.Get(Key);
			if (Result == nullptr)
			{
				Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_field_missing"),
					Object.GetProvenance().GetSourceUnit(),
					TEXT("/") + Key);
			}
			return Result;
		}

		bool RequiredString(
			const FHjsonNode& Object,
			const FString& Key,
			FString& OutValue,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Node =
				RequiredNode(Object, Key, OutError);
			if (Node == nullptr)
			{
				return false;
			}
			if (Node->GetKind() != EHjsonNodeKind::String)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_field_invalid"),
					Node->GetProvenance().GetSourceUnit(),
					Node->GetProvenance().GetJsonPointer());
			}
			OutValue = Node->GetText().TrimStartAndEnd();
			if (OutValue.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_field_invalid"),
					Node->GetProvenance().GetSourceUnit(),
					Node->GetProvenance().GetJsonPointer());
			}
			return true;
		}

		bool OptionalString(
			const FHjsonNode& Object,
			const FString& Key,
			FString& OutValue,
			FWorkspaceError& OutError)
		{
			OutValue.Reset();
			const FHjsonNode* Node = Object.Get(Key);
			if (Node == nullptr)
			{
				return true;
			}
			if (Node->GetKind() != EHjsonNodeKind::String)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_field_invalid"),
					Node->GetProvenance().GetSourceUnit(),
					Node->GetProvenance().GetJsonPointer());
			}
			OutValue = Node->GetText().TrimStartAndEnd();
			return true;
		}

		bool RequiredInteger(
			const FHjsonNode& Object,
			const FString& Key,
			int64 Minimum,
			int64& OutValue,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Node =
				RequiredNode(Object, Key, OutError);
			if (Node == nullptr)
			{
				return false;
			}
			if (Node->GetKind() != EHjsonNodeKind::Integer
				|| !LexTryParseString(
					OutValue,
					*Node->GetLexeme())
				|| OutValue < Minimum)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_field_invalid"),
					Node->GetProvenance().GetSourceUnit(),
					Node->GetProvenance().GetJsonPointer());
			}
			return true;
		}

		bool ReadStringArray(
			const FHjsonNode& Object,
			const FString& Key,
			bool bRequired,
			TArray<FString>& OutValues,
			FWorkspaceError& OutError)
		{
			OutValues.Reset();
			const FHjsonNode* Node = Object.Get(Key);
			if (Node == nullptr)
			{
				return !bRequired
					|| Fail(
						OutError,
						TEXT(
							"semantic_workspace_manifest_field_missing"),
						Object.GetProvenance().GetSourceUnit(),
						TEXT("/") + Key);
			}
			if (Node->GetKind() != EHjsonNodeKind::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_field_invalid"),
					Node->GetProvenance().GetSourceUnit(),
					Node->GetProvenance().GetJsonPointer());
			}
			TSet<FString> Seen;
			for (const FHjsonNodeRef& Item : Node->GetItems())
			{
				if (Item->GetKind() != EHjsonNodeKind::String)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_manifest_field_invalid"),
						Item->GetProvenance().GetSourceUnit(),
						Item->GetProvenance().GetJsonPointer());
				}
				FString Value = Item->GetText().TrimStartAndEnd();
				if (Value.IsEmpty() || Seen.Contains(Value))
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_manifest_field_invalid"),
						Item->GetProvenance().GetSourceUnit(),
						Item->GetProvenance().GetJsonPointer());
				}
				Seen.Add(Value);
				OutValues.Add(MoveTemp(Value));
			}
			return true;
		}

		bool ParseUnitKind(
			const FString& Text,
			ESemanticUnitKind& OutKind)
		{
			for (uint8 Index = 0;
				Index <= static_cast<uint8>(
					ESemanticUnitKind::OpaqueExtension);
				++Index)
			{
				const ESemanticUnitKind Candidate =
					static_cast<ESemanticUnitKind>(Index);
				if (LexToString(Candidate) == Text)
				{
					OutKind = Candidate;
					return true;
				}
			}
			return false;
		}

		bool ParseMode(
			const FString& Text,
			ESemanticWorkspaceMode& OutMode)
		{
			for (uint8 Index = 0;
				Index <= static_cast<uint8>(
					ESemanticWorkspaceMode::EditBundle);
				++Index)
			{
				const ESemanticWorkspaceMode Candidate =
					static_cast<ESemanticWorkspaceMode>(Index);
				if (LexToString(Candidate) == Text)
				{
					OutMode = Candidate;
					return true;
				}
			}
			return false;
		}

		bool ParseStructuralEditKind(
			const FString& Text,
			EStructuralEditKind& OutKind)
		{
			for (uint8 Index = 0;
				Index <= static_cast<uint8>(
					EStructuralEditKind::SetOrder);
				++Index)
			{
				const EStructuralEditKind Candidate =
					static_cast<EStructuralEditKind>(Index);
				if (LexToString(Candidate) == Text)
				{
					OutKind = Candidate;
					return true;
				}
			}
			return false;
		}

		bool FailTypedValue(
			FWorkspaceError& OutError,
			const FHjsonNode& Node,
			const FString& Code =
				TEXT("semantic_workspace_typed_value_invalid"))
		{
			return Fail(
				OutError,
				Code,
				Node.GetProvenance().GetSourceUnit(),
				Node.GetProvenance().GetJsonPointer());
		}

		bool ReadTypedString(
			const FHjsonNode& Object,
			const FString& Key,
			FString& OutValue,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Node = Object.Get(Key);
			if (!Node || Node->GetKind() != EHjsonNodeKind::String)
			{
				return FailTypedValue(
					OutError,
					Node ? *Node : Object);
			}
			OutValue = Node->GetText().TrimStartAndEnd();
			return !OutValue.IsEmpty()
				|| FailTypedValue(OutError, *Node);
		}

		bool ReadTypedStringArray(
			const FHjsonNode& Object,
			const FString& Key,
			TArray<FString>& OutValues,
			FWorkspaceError& OutError)
		{
			OutValues.Reset();
			const FHjsonNode* Node = Object.Get(Key);
			if (!Node || Node->GetKind() != EHjsonNodeKind::Array)
			{
				return FailTypedValue(
					OutError,
					Node ? *Node : Object);
			}
			for (const FHjsonNodeRef& Item : Node->GetItems())
			{
				if (Item->GetKind() != EHjsonNodeKind::String)
				{
					return FailTypedValue(OutError, Item.Get());
				}
				OutValues.Add(Item->GetText().TrimStartAndEnd());
			}
			return true;
		}

		bool ParseTypedTemporal(
			const FHjsonNode& Object,
			const bool bRequireMarker,
			FUeAgentTemporalPoint& OutValue,
			FWorkspaceError& OutError)
		{
			if (Object.GetKind() != EHjsonNodeKind::Object)
			{
				return FailTypedValue(OutError, Object);
			}
			if (bRequireMarker)
			{
				FString Marker;
				if (!ReadTypedString(
						Object,
						TEXT("$uai"),
						Marker,
						OutError)
					|| Marker != TEXT("temporal"))
				{
					return false;
				}
			}
			FString ClockId;
			FString Tick;
			FString RateNumerator;
			FString RateDenominator;
			FString State;
			if (!ReadTypedString(
					Object,
					TEXT("clock_id"),
					ClockId,
					OutError)
				|| !ReadTypedString(
					Object,
					TEXT("tick"),
					Tick,
					OutError)
				|| !ReadTypedString(
					Object,
					TEXT("rate_numerator"),
					RateNumerator,
					OutError)
				|| !ReadTypedString(
					Object,
					TEXT("rate_denominator"),
					RateDenominator,
					OutError)
				|| !ReadTypedString(
					Object,
					TEXT("state"),
					State,
					OutError))
			{
				return false;
			}
			OutValue = FUeAgentTemporalPoint(
				MoveTemp(ClockId),
				MoveTemp(Tick),
				MoveTemp(RateNumerator),
				MoveTemp(RateDenominator),
				MoveTemp(State));
			return OutValue.IsValid()
				|| FailTypedValue(OutError, Object);
		}

		bool ParseSpatialQuantityKind(
			const FString& Text,
			EUeAgentSpatialQuantityKind& OutKind)
		{
			if (Text == TEXT("point"))
			{
				OutKind = EUeAgentSpatialQuantityKind::Point;
				return true;
			}
			if (Text == TEXT("direction"))
			{
				OutKind = EUeAgentSpatialQuantityKind::Direction;
				return true;
			}
			if (Text == TEXT("normal"))
			{
				OutKind = EUeAgentSpatialQuantityKind::Normal;
				return true;
			}
			if (Text == TEXT("transform"))
			{
				OutKind = EUeAgentSpatialQuantityKind::Transform;
				return true;
			}
			return false;
		}

		bool ParseTypedSpatial(
			const FHjsonNode& Object,
			FUeAgentSpatialValue& OutValue,
			FWorkspaceError& OutError)
		{
			if (Object.GetKind() != EHjsonNodeKind::Object)
			{
				return FailTypedValue(OutError, Object);
			}
			FString Marker;
			FString Subject;
			FString QuantityText;
			if (!ReadTypedString(
					Object,
					TEXT("$uai"),
					Marker,
					OutError)
				|| Marker != TEXT("spatial")
				|| !ReadTypedString(
					Object,
					TEXT("subject"),
					Subject,
					OutError)
				|| !ReadTypedString(
					Object,
					TEXT("quantity_kind"),
					QuantityText,
					OutError))
			{
				return false;
			}
			EUeAgentSpatialQuantityKind QuantityKind;
			if (!ParseSpatialQuantityKind(
					QuantityText,
					QuantityKind))
			{
				return FailTypedValue(OutError, Object);
			}
			const FHjsonNode* FrameNode = Object.Get(TEXT("frame"));
			if (!FrameNode
				|| FrameNode->GetKind() != EHjsonNodeKind::Object)
			{
				return FailTypedValue(
					OutError,
					FrameNode ? *FrameNode : Object);
			}
			FString FrameId;
			FString BasisFrameId;
			FString Unit;
			FString BasisEvidence;
			if (!ReadTypedString(
					*FrameNode,
					TEXT("frame_id"),
					FrameId,
					OutError)
				|| !ReadTypedString(
					*FrameNode,
					TEXT("basis_frame_id"),
					BasisFrameId,
					OutError)
				|| !ReadTypedString(
					*FrameNode,
					TEXT("unit"),
					Unit,
					OutError)
				|| !ReadTypedString(
					*FrameNode,
					TEXT("basis_evidence"),
					BasisEvidence,
					OutError))
			{
				return false;
			}
			const FHjsonNode* DimensionNode =
				FrameNode->Get(TEXT("dimension"));
			int64 Dimension = 0;
			if (!DimensionNode
				|| DimensionNode->GetKind() != EHjsonNodeKind::Integer
				|| !LexTryParseString(
					Dimension,
					*DimensionNode->GetLexeme())
				|| Dimension < 1
				|| Dimension > MAX_int32)
			{
				return FailTypedValue(
					OutError,
					DimensionNode ? *DimensionNode : *FrameNode);
			}
			const FHjsonNode* BasisNode = FrameNode->Get(TEXT("basis"));
			TArray<TArray<FString>> Basis;
			if (!BasisNode
				|| BasisNode->GetKind() != EHjsonNodeKind::Array)
			{
				return FailTypedValue(
					OutError,
					BasisNode ? *BasisNode : *FrameNode);
			}
			for (const FHjsonNodeRef& RowNode : BasisNode->GetItems())
			{
				if (RowNode->GetKind() != EHjsonNodeKind::Array)
				{
					return FailTypedValue(OutError, RowNode.Get());
				}
				TArray<FString> Row;
				for (const FHjsonNodeRef& Cell : RowNode->GetItems())
				{
					if (Cell->GetKind() != EHjsonNodeKind::String)
					{
						return FailTypedValue(OutError, Cell.Get());
					}
					Row.Add(Cell->GetText().TrimStartAndEnd());
				}
				Basis.Add(MoveTemp(Row));
			}
			TArray<FString> Origin;
			TArray<FString> Components;
			TArray<FString> ConversionEvidence;
			if (!ReadTypedStringArray(
					*FrameNode,
					TEXT("origin"),
					Origin,
					OutError)
				|| !ReadTypedStringArray(
					Object,
					TEXT("components"),
					Components,
					OutError)
				|| !ReadTypedStringArray(
					Object,
					TEXT("conversion_evidence"),
					ConversionEvidence,
					OutError))
			{
				return false;
			}
			const FHjsonNode* FrameObserved =
				FrameNode->Get(TEXT("observed_at"));
			const FHjsonNode* ValueObserved =
				Object.Get(TEXT("observed_at"));
			FUeAgentTemporalPoint FrameTime(TEXT("invalid"), TEXT("0"));
			FUeAgentTemporalPoint ValueTime(TEXT("invalid"), TEXT("0"));
			if (!FrameObserved
				|| !ValueObserved
				|| !ParseTypedTemporal(
					*FrameObserved,
					false,
					FrameTime,
					OutError)
				|| !ParseTypedTemporal(
					*ValueObserved,
					false,
					ValueTime,
					OutError))
			{
				return false;
			}
			FUeAgentSpatialFrame Frame(
				MoveTemp(FrameId),
				MoveTemp(BasisFrameId),
				static_cast<int32>(Dimension),
				MoveTemp(Basis),
				MoveTemp(Origin),
				MoveTemp(Unit),
				MoveTemp(BasisEvidence),
				MoveTemp(FrameTime));
			if (!Frame.IsValid())
			{
				return FailTypedValue(OutError, *FrameNode);
			}
			OutValue = FUeAgentSpatialValue(
				MoveTemp(Subject),
				QuantityKind,
				MoveTemp(Frame),
				MoveTemp(Components),
				MoveTemp(ValueTime),
				MoveTemp(ConversionEvidence));
			return OutValue.IsValid()
				|| FailTypedValue(OutError, Object);
		}

		bool TryTypedCanonicalValue(
			const FHjsonNode& Root,
			FUeAgentResourceTracker& Resources,
			bool& bOutHandled,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			bOutHandled = false;
			if (Root.GetKind() != EHjsonNodeKind::Object)
			{
				return true;
			}
			const FHjsonNode* MarkerNode = Root.Get(TEXT("$uai"));
			if (!MarkerNode)
			{
				return true;
			}
			bOutHandled = true;
			if (MarkerNode->GetKind() != EHjsonNodeKind::String)
			{
				return FailTypedValue(OutError, *MarkerNode);
			}
			FUeAgentStateSemanticError StateError;
			if (MarkerNode->GetText() == TEXT("decimal"))
			{
				FString Lexeme;
				if (!ReadTypedString(
						Root,
						TEXT("value"),
						Lexeme,
						OutError))
				{
					return false;
				}
				if (!FUeAgentCanonicalValue::TryDecimal(
						Lexeme,
						Resources,
						OutValue,
						StateError))
				{
					return Fail(
						OutError,
						StateError.Code,
						Root.GetProvenance().GetSourceUnit(),
						Root.GetProvenance().GetJsonPointer());
				}
				return true;
			}
			if (MarkerNode->GetText() == TEXT("temporal"))
			{
				FUeAgentTemporalPoint Temporal(TEXT("invalid"), TEXT("0"));
				if (!ParseTypedTemporal(
						Root,
						true,
						Temporal,
						OutError))
				{
					return false;
				}
				if (!FUeAgentCanonicalValue::TryTemporal(
						Temporal,
						Resources,
						OutValue,
						StateError))
				{
					return Fail(
						OutError,
						StateError.Code,
						Root.GetProvenance().GetSourceUnit(),
						Root.GetProvenance().GetJsonPointer());
				}
				return true;
			}
			if (MarkerNode->GetText() == TEXT("spatial"))
			{
				FUeAgentSpatialValue Spatial(
					TEXT("invalid"),
					EUeAgentSpatialQuantityKind::Point,
					FUeAgentSpatialFrame(
						TEXT("invalid"),
						TEXT("invalid"),
						1,
						{{TEXT("1")}},
						{TEXT("0")},
						TEXT("invalid"),
						TEXT("invalid"),
						FUeAgentTemporalPoint(
							TEXT("invalid"),
							TEXT("0"))),
					{TEXT("0")},
					FUeAgentTemporalPoint(TEXT("invalid"), TEXT("0")));
				if (!ParseTypedSpatial(Root, Spatial, OutError))
				{
					return false;
				}
				if (!FUeAgentCanonicalValue::TrySpatial(
						Spatial,
						Resources,
						OutValue,
						StateError))
				{
					return Fail(
						OutError,
						StateError.Code,
						Root.GetProvenance().GetSourceUnit(),
						Root.GetProvenance().GetJsonPointer());
				}
				return true;
			}
			return FailTypedValue(
				OutError,
				*MarkerNode,
				TEXT("semantic_workspace_typed_value_kind_unsupported"));
		}

		bool TryCanonicalValue(
			const FHjsonNode& Root,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			struct FVisit
			{
				const FHjsonNode* Node = nullptr;
				bool bExpanded = false;
			};
			TArray<FVisit> Stack;
			TMap<const FHjsonNode*, FUeAgentCanonicalValuePtr>
				Converted;
			Stack.Add({&Root, false});
			while (!Stack.IsEmpty())
			{
				const FVisit Visit =
					Stack.Pop(EAllowShrinking::No);
				if (Converted.Contains(Visit.Node))
				{
					continue;
				}
				if (!Visit.bExpanded
					&& Visit.Node->GetKind()
						== EHjsonNodeKind::Object)
				{
					bool bHandled = false;
					FUeAgentCanonicalValuePtr TypedValue;
					if (!TryTypedCanonicalValue(
							*Visit.Node,
							Resources,
							bHandled,
							TypedValue,
							OutError))
					{
						return false;
					}
					if (bHandled)
					{
						Converted.Add(Visit.Node, TypedValue);
						continue;
					}
				}
				if (!Visit.bExpanded
					&& (Visit.Node->GetKind()
						== EHjsonNodeKind::Array
						|| Visit.Node->GetKind()
							== EHjsonNodeKind::Object))
				{
					Stack.Add({Visit.Node, true});
					if (Visit.Node->GetKind()
						== EHjsonNodeKind::Array)
					{
						const TArray<FHjsonNodeRef>& Items =
							Visit.Node->GetItems();
						for (int32 Index = Items.Num() - 1;
							Index >= 0;
							--Index)
						{
							Stack.Add(
								{&Items[Index].Get(), false});
						}
					}
					else
					{
						const TArray<TPair<
							FString,
							FHjsonNodeRef>>& Entries =
								Visit.Node->GetEntries();
						for (int32 Index = Entries.Num() - 1;
							Index >= 0;
							--Index)
						{
							Stack.Add(
								{&Entries[Index].Value.Get(), false});
						}
					}
					continue;
				}

				FUeAgentCanonicalValuePtr Value;
				FUeAgentStateSemanticError StateError;
				bool bCreated = false;
				switch (Visit.Node->GetKind())
				{
				case EHjsonNodeKind::Null:
					bCreated = FUeAgentCanonicalValue::TryNull(
						Resources,
						Value,
						StateError);
					break;
				case EHjsonNodeKind::Boolean:
					bCreated =
						FUeAgentCanonicalValue::TryBoolean(
							Visit.Node->GetBoolean(),
							Resources,
							Value,
							StateError);
					break;
				case EHjsonNodeKind::String:
					bCreated =
						FUeAgentCanonicalValue::TryString(
							Visit.Node->GetText(),
							Resources,
							Value,
							StateError);
					break;
				case EHjsonNodeKind::Integer:
					bCreated =
						FUeAgentCanonicalValue::TryInteger(
							Visit.Node->GetLexeme(),
							Resources,
							Value,
							StateError);
					break;
				case EHjsonNodeKind::Decimal:
					bCreated =
						FUeAgentCanonicalValue::TryDecimal(
							Visit.Node->GetLexeme(),
							Resources,
							Value,
							StateError);
					break;
				case EHjsonNodeKind::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const FHjsonNodeRef& Item :
						Visit.Node->GetItems())
					{
						Items.Add(
							Converted.FindChecked(
								&Item.Get()).ToSharedRef());
					}
					bCreated =
						FUeAgentCanonicalValue::TryArray(
							Items,
							Resources,
							Value,
							StateError);
					break;
				}
				case EHjsonNodeKind::Object:
				{
					TArray<TPair<
						FString,
						FUeAgentCanonicalValueRef>> Items;
					for (const TPair<
						FString,
						FHjsonNodeRef>& Item :
						Visit.Node->GetEntries())
					{
						Items.Emplace(
							Item.Key,
							Converted.FindChecked(
								&Item.Value.Get())
								.ToSharedRef());
					}
					bCreated =
						FUeAgentCanonicalValue::TryObject(
							Items,
							Resources,
							Value,
							StateError);
					break;
				}
				default:
					break;
				}
				if (!bCreated || !Value.IsValid())
				{
					OutError.Set(
						StateError.Code.IsEmpty()
							? TEXT(
								"semantic_workspace_unit_value_invalid")
							: StateError.Code,
						Visit.Node->GetProvenance().GetSourceUnit(),
						Visit.Node->GetProvenance().GetJsonPointer(),
						0,
						false);
					return false;
				}
				Converted.Add(Visit.Node, MoveTemp(Value));
			}
			OutValue = Converted.FindChecked(&Root);
			return OutValue.IsValid();
		}

		bool ParseSemanticUnit(
			const FHjsonNode& Row,
			FUeAgentResourceTracker& Resources,
			TOptional<FSemanticUnit>& OutUnit,
			FWorkspaceError& OutError)
		{
			OutUnit.Reset();
			if (Row.GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_unit_document_invalid"),
					Row.GetProvenance().GetSourceUnit(),
					Row.GetProvenance().GetJsonPointer());
			}
			FString UnitId;
			FString KindText;
			FString SemanticTypeRef;
			FString Role;
			FString OwnerRef;
			FString FieldSetRef;
			FString OrderingRef;
			FString LayoutRef;
			FString SourceProvenance;
			FString TargetIdentityKey;
			if (!RequiredString(
					Row,
					TEXT("unit_id"),
					UnitId,
					OutError)
				|| !RequiredString(
					Row,
					TEXT("unit_kind"),
					KindText,
					OutError)
				|| !RequiredString(
					Row,
					TEXT("semantic_type_ref"),
					SemanticTypeRef,
					OutError)
				|| !RequiredString(
					Row,
					TEXT("role"),
					Role,
					OutError)
				|| !OptionalString(
					Row,
					TEXT("owner_ref"),
					OwnerRef,
					OutError)
				|| !OptionalString(
					Row,
					TEXT("field_set_ref"),
					FieldSetRef,
					OutError)
				|| !OptionalString(
					Row,
					TEXT("ordering_ref"),
					OrderingRef,
					OutError)
				|| !OptionalString(
					Row,
					TEXT("layout_ref"),
					LayoutRef,
					OutError)
				|| !OptionalString(
					Row,
					TEXT("source_provenance"),
					SourceProvenance,
					OutError)
				|| !OptionalString(
					Row,
					TEXT("target_identity_key"),
					TargetIdentityKey,
					OutError))
			{
				return false;
			}
			ESemanticUnitKind UnitKind;
			if (!ParseUnitKind(KindText, UnitKind))
			{
				return Fail(
					OutError,
					TEXT("semantic_workspace_unit_kind_invalid"),
					UnitId,
					TEXT("/unit_kind"));
			}
			const FHjsonNode* ValuesNode = Row.Get(TEXT("values"));
			if (ValuesNode == nullptr
				|| ValuesNode->GetKind()
					!= EHjsonNodeKind::Object)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_unit_values_invalid"),
					UnitId,
					TEXT("/values"));
			}
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			for (const TPair<FString, FHjsonNodeRef>& Item :
				ValuesNode->GetEntries())
			{
				FUeAgentCanonicalValuePtr Value;
				if (!TryCanonicalValue(
						Item.Value.Get(),
						Resources,
						Value,
						OutError))
				{
					return false;
				}
				if (Values.Contains(Item.Key))
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_unit_values_invalid"),
						UnitId,
						Item.Value->GetProvenance()
							.GetJsonPointer());
				}
				Values.Add(Item.Key, Value.ToSharedRef());
			}
			TArray<FString> ItemRefs;
			TArray<FString> RelationRefs;
			TArray<FString> DependencyRefs;
			TArray<FString> ProfileVisibility;
			if (!ReadStringArray(
					Row,
					TEXT("item_refs"),
					false,
					ItemRefs,
					OutError)
				|| !ReadStringArray(
					Row,
					TEXT("relation_refs"),
					false,
					RelationRefs,
					OutError)
				|| !ReadStringArray(
					Row,
					TEXT("dependency_refs"),
					false,
					DependencyRefs,
					OutError)
				|| !ReadStringArray(
					Row,
					TEXT("profile_visibility"),
					false,
					ProfileVisibility,
					OutError))
			{
				return false;
			}
			OutUnit.Emplace(
				MoveTemp(UnitId),
				UnitKind,
				MoveTemp(OwnerRef),
				MoveTemp(SemanticTypeRef),
				MoveTemp(Role),
				MoveTemp(FieldSetRef),
				MoveTemp(Values),
				MoveTemp(ItemRefs),
				MoveTemp(RelationRefs),
				MoveTemp(OrderingRef),
				MoveTemp(LayoutRef),
				MoveTemp(DependencyRefs),
				MoveTemp(ProfileVisibility),
				MoveTemp(SourceProvenance),
				MoveTemp(TargetIdentityKey));
			if (!OutUnit->IsValid())
			{
				OutUnit.Reset();
				return Fail(
					OutError,
					TEXT("semantic_workspace_unit_invalid"),
					Row.GetProvenance().GetSourceUnit(),
					Row.GetProvenance().GetJsonPointer());
			}
			return true;
		}

		bool SafeRelativePath(
			FString Value,
			FString& OutPath)
		{
			Value.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Value.IsEmpty()
				|| Value.StartsWith(TEXT("/"))
				|| Value.Contains(TEXT(":")))
			{
				return false;
			}
			TArray<FString> Segments;
			Value.ParseIntoArray(Segments, TEXT("/"), false);
			if (Segments.IsEmpty())
			{
				return false;
			}
			for (const FString& Segment : Segments)
			{
				if (Segment.IsEmpty()
					|| Segment == TEXT(".")
					|| Segment == TEXT(".."))
				{
					return false;
				}
			}
			OutPath = FString::Join(Segments, TEXT("/"));
			return true;
		}

		bool IsReparsePoint(const FString& Path)
		{
			const ESymlinkResult Result =
				FPlatformFileManager::Get()
					.GetPlatformFile()
					.IsSymlink(*Path);
			return Result == ESymlinkResult::Symlink;
		}

		bool ReadDocument(
			const FString& Path,
			const FString& SourceUnit,
			FUeAgentResourceTracker& Resources,
			FHjsonDocumentPtr& OutDocument,
			FWorkspaceError& OutError)
		{
			if (IsReparsePoint(Path))
			{
				return Fail(
					OutError,
					TEXT(
						"workspace_reparse_point_rejected"),
					SourceUnit);
			}
			if (!Charge(
					Resources,
					{{EUeAgentResourceDimension::Files, 1}},
					TEXT("semantic_workspace:") + SourceUnit,
					OutError))
			{
				return false;
			}
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				return Fail(
					OutError,
					TEXT("workspace_source_read_failed"),
					SourceUnit);
			}
			return TryParseHjsonDocument(
				Text,
				SourceUnit,
				Resources,
				OutDocument,
				OutError);
		}

		bool ParseInventory(
			const FHjsonNode& Manifest,
			TArray<FInventoryRow>& OutRows,
			FWorkspaceError& OutError)
		{
			OutRows.Reset();
			const FHjsonNode* Inventory =
				Manifest.Get(TEXT("unit_inventory"));
			if (Inventory == nullptr
				|| Inventory->GetKind()
					!= EHjsonNodeKind::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_inventory_invalid"),
					Manifest.GetProvenance().GetSourceUnit(),
					TEXT("/unit_inventory"));
			}
			TSet<FString> UnitIds;
			TSet<FString> Paths;
			for (const FHjsonNodeRef& Item :
				Inventory->GetItems())
			{
				if (Item->GetKind()
					!= EHjsonNodeKind::Object)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_inventory_invalid"),
						Item->GetProvenance().GetSourceUnit(),
						Item->GetProvenance().GetJsonPointer());
				}
				FInventoryRow Row;
				FString KindText;
				if (!RequiredString(
						Item.Get(),
						TEXT("unit_id"),
						Row.UnitId,
						OutError)
					|| !RequiredString(
						Item.Get(),
						TEXT("unit_kind"),
						KindText,
						OutError)
					|| !RequiredString(
						Item.Get(),
						TEXT("logical_path"),
						Row.LogicalPath,
						OutError)
					|| !RequiredString(
						Item.Get(),
						TEXT("content_hash"),
						Row.ContentHash,
						OutError))
				{
					return false;
				}
				if (!ParseUnitKind(KindText, Row.UnitKind))
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_inventory_invalid"),
						Row.UnitId,
						TEXT("/unit_kind"));
				}
				if (UnitIds.Contains(Row.UnitId))
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_unit_duplicate"),
						Row.UnitId);
				}
				if (Paths.Contains(Row.LogicalPath))
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_inventory_path_duplicate"),
						Row.LogicalPath);
				}
				UnitIds.Add(Row.UnitId);
				Paths.Add(Row.LogicalPath);
				OutRows.Add(MoveTemp(Row));
			}
			return true;
		}

		const FHjsonNode* FindEntry(
			const FHjsonNode& Object,
			const FString& Key)
		{
			return Object.GetKind() == EHjsonNodeKind::Object
				? Object.Get(Key)
				: nullptr;
		}

		bool ReadDirectorySource(
			const FWorkspaceSource& Source,
			FUeAgentResourceTracker& Resources,
			FParsedSource& OutParsed,
			TArray<FInventoryRow>& OutInventory,
			FWorkspaceError& OutError)
		{
			FString Root =
				FPaths::ConvertRelativePathToFull(Source.GetPath());
			FPaths::NormalizeDirectoryName(Root);
			if (!IFileManager::Get().DirectoryExists(*Root))
			{
				return Fail(
					OutError,
					TEXT(
						"workspace_source_directory_missing"),
					Root);
			}
			if (IsReparsePoint(Root))
			{
				return Fail(
					OutError,
					TEXT(
						"workspace_reparse_point_rejected"),
					Root);
			}
			const FString FullManifest =
				FPaths::Combine(Root, ManifestPath);
			if (!IFileManager::Get().FileExists(*FullManifest))
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_missing"),
					ManifestPath);
			}
			if (!ReadDocument(
					FullManifest,
					ManifestPath,
					Resources,
					OutParsed.ManifestDocument,
					OutError)
				|| !OutParsed.ManifestDocument.IsValid())
			{
				return false;
			}
			OutParsed.Manifest =
				&OutParsed.ManifestDocument->GetRoot().Get();
			if (OutParsed.Manifest->GetKind()
				!= EHjsonNodeKind::Object
				|| !ParseInventory(
					*OutParsed.Manifest,
					OutInventory,
					OutError))
			{
				return false;
			}

			TSet<FString> ExpectedPaths;
			OutParsed.UnitDocuments.Reserve(
				OutInventory.Num());
			OutParsed.UnitRows.Reserve(OutInventory.Num());
			for (FInventoryRow& Row : OutInventory)
			{
				FString Relative;
				if (!SafeRelativePath(
						Row.LogicalPath,
						Relative))
				{
					return Fail(
						OutError,
						TEXT("workspace_path_invalid"),
						Row.LogicalPath);
				}
				Row.LogicalPath = Relative;
				ExpectedPaths.Add(Relative);
				const FString FullPath =
					FPaths::Combine(Root, Relative);
				if (!IFileManager::Get().FileExists(*FullPath))
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_unit_missing"),
						Relative);
				}
				FHjsonDocumentPtr Document;
				if (!ReadDocument(
						FullPath,
						Relative,
						Resources,
						Document,
						OutError)
					|| !Document.IsValid())
				{
					return false;
				}
				const FHjsonNode* Unit =
					FindEntry(
						Document->GetRoot().Get(),
						TEXT("unit"));
				if (Unit == nullptr
					|| Unit->GetKind()
						!= EHjsonNodeKind::Object)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_unit_document_invalid"),
						Relative);
				}
				OutParsed.UnitDocuments.Add(Document);
				OutParsed.UnitRows.Add(Unit);
				OutParsed.ManagedPaths.Add(Relative);
			}

			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(
				Files,
				*Root,
				TEXT("*.hjson"),
				true,
				false);
			FString RelativeBase = Root;
			if (!RelativeBase.EndsWith(TEXT("/")))
			{
				RelativeBase += TEXT("/");
			}
			for (FString File : Files)
			{
				if (IsReparsePoint(File))
				{
					return Fail(
						OutError,
						TEXT(
							"workspace_reparse_point_rejected"),
						File);
				}
				if (!FPaths::MakePathRelativeTo(
						File,
						*RelativeBase))
				{
					return Fail(
						OutError,
						TEXT("workspace_path_escape"),
						File);
				}
				FPaths::NormalizeFilename(File);
				if (File != ManifestPath
					&& !ExpectedPaths.Contains(File))
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_unit_extra"),
						File);
				}
			}
			return true;
		}

		bool ReadEmbeddedSource(
			const FWorkspaceSource& Source,
			FUeAgentResourceTracker& Resources,
			FParsedSource& OutParsed,
			TArray<FInventoryRow>& OutInventory,
			FWorkspaceError& OutError)
		{
			FString Text;
			FString SourceUnit = EmbeddedPath;
			if (Source.GetKind()
				== EWorkspaceSourceKind::Inline)
			{
				Text = Source.GetText();
				if (!Source.GetSourceUnit().IsEmpty())
				{
					SourceUnit = Source.GetSourceUnit();
				}
				if (!Charge(
						Resources,
						{{EUeAgentResourceDimension::Files, 1}},
						TEXT("semantic_workspace:") + SourceUnit,
						OutError))
				{
					return false;
				}
				if (!TryParseHjsonDocument(
						Text,
						SourceUnit,
						Resources,
						OutParsed.Envelope,
						OutError))
				{
					return false;
				}
			}
			else
			{
				const FString Path =
					FPaths::ConvertRelativePathToFull(
						Source.GetPath());
				if (!IFileManager::Get().FileExists(*Path))
				{
					return Fail(
						OutError,
						TEXT(
							"workspace_source_file_missing"),
						Path);
				}
				if (!ReadDocument(
						Path,
						SourceUnit,
						Resources,
						OutParsed.Envelope,
						OutError))
				{
					return false;
				}
			}
			if (!OutParsed.Envelope.IsValid())
			{
				return false;
			}
			const FHjsonNode& Root =
				OutParsed.Envelope->GetRoot().Get();
			OutParsed.Manifest =
				FindEntry(Root, TEXT("manifest"));
			const FHjsonNode* Units =
				FindEntry(Root, TEXT("units"));
			if (OutParsed.Manifest == nullptr
				|| OutParsed.Manifest->GetKind()
					!= EHjsonNodeKind::Object
				|| Units == nullptr
				|| Units->GetKind()
					!= EHjsonNodeKind::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_envelope_invalid"),
					SourceUnit);
			}
			if (!ParseInventory(
					*OutParsed.Manifest,
					OutInventory,
					OutError))
			{
				return false;
			}
			if (OutInventory.Num() != Units->GetItems().Num())
			{
				return Fail(
					OutError,
					OutInventory.Num()
							> Units->GetItems().Num()
						? TEXT(
							"semantic_workspace_unit_missing")
						: TEXT(
							"semantic_workspace_unit_extra"),
					SourceUnit);
			}
			for (int32 Index = 0;
				Index < OutInventory.Num();
				++Index)
			{
				const FString ExpectedPath =
					FString::Printf(
						TEXT("#/units/%d"),
						Index);
				if (OutInventory[Index].LogicalPath
						!= ExpectedPath
					|| Units->GetItems()[Index]->GetKind()
						!= EHjsonNodeKind::Object)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_embedded_inventory_invalid"),
						SourceUnit);
				}
				OutParsed.UnitRows.Add(
					&Units->GetItems()[Index].Get());
				OutParsed.ManagedPaths.Add(ExpectedPath);
			}
			return true;
		}

		bool ParseRevision(
			const FHjsonNode& Manifest,
			FUeAgentStateRevision& OutRevision,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Revision =
				Manifest.Get(TEXT("base_state_revision"));
			if (Revision == nullptr
				|| Revision->GetKind()
					!= EHjsonNodeKind::Object)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_field_invalid"),
					Manifest.GetProvenance().GetSourceUnit(),
					TEXT("/base_state_revision"));
			}
			FString SourceId;
			FString Generation;
			int64 Sequence = 0;
			if (!RequiredString(
					*Revision,
					TEXT("source_id"),
					SourceId,
					OutError)
				|| !RequiredString(
					*Revision,
					TEXT("generation"),
					Generation,
					OutError)
				|| !RequiredInteger(
					*Revision,
					TEXT("sequence"),
					0,
					Sequence,
					OutError))
			{
				return false;
			}
			OutRevision = FUeAgentStateRevision(
				MoveTemp(SourceId),
				MoveTemp(Generation),
				Sequence);
			return OutRevision.IsValid()
				|| Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_field_invalid"),
					Manifest.GetProvenance().GetSourceUnit(),
					TEXT("/base_state_revision"));
		}

		bool ParseStructuralEdits(
			const FHjsonNode& Manifest,
			const FAuthoringModel& Model,
			TArray<FStructuralEdit>& OutEdits,
			FWorkspaceError& OutError)
		{
			OutEdits.Reset();
			const FHjsonNode* Edits =
				Manifest.Get(TEXT("structural_edits"));
			if (Edits == nullptr
				|| Edits->GetKind()
					!= EHjsonNodeKind::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_structural_edits_invalid"),
					Manifest.GetProvenance().GetSourceUnit(),
					TEXT("/structural_edits"));
			}
			TSet<FString> EditIds;
			for (const FHjsonNodeRef& Row : Edits->GetItems())
			{
				if (Row->GetKind()
					!= EHjsonNodeKind::Object)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_structural_edit_invalid"),
						Row->GetProvenance().GetSourceUnit(),
						Row->GetProvenance().GetJsonPointer());
				}
				FString EditId;
				FString KindText;
				FString OwnerUnitId;
				FString SubjectUnitId;
				FString ReplacementUnitId;
				FString RelationId;
				TArray<FString> OrderedUnitIds;
				if (!RequiredString(
						Row.Get(),
						TEXT("edit_id"),
						EditId,
						OutError)
					|| !RequiredString(
						Row.Get(),
						TEXT("kind"),
						KindText,
						OutError)
					|| !RequiredString(
						Row.Get(),
						TEXT("owner_unit_id"),
						OwnerUnitId,
						OutError)
					|| !OptionalString(
						Row.Get(),
						TEXT("subject_unit_id"),
						SubjectUnitId,
						OutError)
					|| !OptionalString(
						Row.Get(),
						TEXT("replacement_unit_id"),
						ReplacementUnitId,
						OutError)
					|| !OptionalString(
						Row.Get(),
						TEXT("relation_id"),
						RelationId,
						OutError)
					|| !ReadStringArray(
						Row.Get(),
						TEXT("ordered_unit_ids"),
						false,
						OrderedUnitIds,
						OutError))
				{
					return false;
				}
				EStructuralEditKind Kind;
				if (!ParseStructuralEditKind(
						KindText,
						Kind)
					|| EditIds.Contains(EditId))
				{
					return Fail(
						OutError,
						EditIds.Contains(EditId)
							? TEXT(
								"semantic_workspace_structural_edit_duplicate")
							: TEXT(
								"semantic_workspace_structural_edit_invalid"),
						EditId);
				}
				FStructuralEdit Edit(
					EditId,
					Kind,
					OwnerUnitId,
					SubjectUnitId,
					ReplacementUnitId,
					RelationId,
					MoveTemp(OrderedUnitIds));
				if (!Edit.IsValid())
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_structural_edit_invalid"),
						EditId);
				}
				if (Model.FindUnit(OwnerUnitId) == nullptr)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_reference_unresolved"),
						EditId);
				}
				for (const FString& Reference : {
					SubjectUnitId,
					ReplacementUnitId})
				{
					if (!Reference.IsEmpty()
						&& Model.FindUnit(Reference) == nullptr)
					{
						return Fail(
							OutError,
							TEXT(
								"semantic_workspace_reference_unresolved"),
							EditId);
					}
				}
				for (const FString& Reference :
					Edit.GetOrderedUnitIds())
				{
					if (Model.FindUnit(Reference) == nullptr)
					{
						return Fail(
							OutError,
							TEXT(
								"semantic_workspace_reference_unresolved"),
							EditId);
					}
				}
				EditIds.Add(EditId);
				OutEdits.Add(MoveTemp(Edit));
			}
			return true;
		}
	}

	static bool TryResolveSemanticWorkspaceInternal(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		const bool bAllowManagedUnitEdits,
		FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError)
	{
		OutWorkspace.Reset();
		OutError.Reset();
		FParsedSource Parsed;
		TArray<FInventoryRow> Inventory;
		const bool bRead =
			Source.GetKind()
					== EWorkspaceSourceKind::Directory
				? ReadDirectorySource(
					Source,
					Resources,
					Parsed,
					Inventory,
					OutError)
				: ReadEmbeddedSource(
					Source,
					Resources,
					Parsed,
					Inventory,
					OutError);
		if (!bRead || Parsed.Manifest == nullptr)
		{
			return false;
		}

		FString Schema;
		FString ModeText;
		FString ContractId;
		FString ProfileId;
		FString BaseRevision;
		FString ExpectedFingerprint;
		int64 ContractVersion = 0;
		int64 LayoutVersion = 0;
		if (!RequiredString(
				*Parsed.Manifest,
				TEXT("workspace_schema"),
				Schema,
				OutError)
			|| !RequiredString(
				*Parsed.Manifest,
				TEXT("import_mode"),
				ModeText,
				OutError)
			|| !RequiredString(
				*Parsed.Manifest,
				TEXT("contract_id"),
				ContractId,
				OutError)
			|| !RequiredInteger(
				*Parsed.Manifest,
				TEXT("contract_version"),
				1,
				ContractVersion,
				OutError)
			|| !RequiredInteger(
				*Parsed.Manifest,
				TEXT("layout_version"),
				1,
				LayoutVersion,
				OutError)
			|| !RequiredString(
				*Parsed.Manifest,
				TEXT("profile_id"),
				ProfileId,
				OutError)
			|| !RequiredString(
				*Parsed.Manifest,
				TEXT("base_revision"),
				BaseRevision,
				OutError)
			|| !RequiredString(
				*Parsed.Manifest,
				TEXT("semantic_fingerprint"),
				ExpectedFingerprint,
				OutError))
		{
			return false;
		}
		if (Schema != GetSemanticWorkspaceSchemaId())
		{
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_schema_unsupported"),
				Parsed.Manifest->GetProvenance()
					.GetSourceUnit(),
				TEXT("/workspace_schema"));
		}
		ESemanticWorkspaceMode Mode;
		if (!ParseMode(ModeText, Mode))
		{
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_mode_unsupported"),
				Parsed.Manifest->GetProvenance()
					.GetSourceUnit(),
				TEXT("/import_mode"));
		}
		if (Mode != ExpectedMode)
		{
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_mode_mismatch"),
				Parsed.Manifest->GetProvenance()
					.GetSourceUnit(),
				TEXT("/import_mode"));
		}
		if (Inventory.Num() != Parsed.UnitRows.Num())
		{
			return Fail(
				OutError,
				Inventory.Num() > Parsed.UnitRows.Num()
					? TEXT(
						"semantic_workspace_unit_missing")
					: TEXT(
						"semantic_workspace_unit_extra"));
		}

		TArray<FSemanticUnit> Units;
		Units.Reserve(Parsed.UnitRows.Num());
		TSet<FString> SeenUnitIds;
		for (int32 Index = 0;
			Index < Parsed.UnitRows.Num();
			++Index)
		{
			TOptional<FSemanticUnit> Unit;
			if (!ParseSemanticUnit(
					*Parsed.UnitRows[Index],
					Resources,
					Unit,
					OutError)
				|| !Unit.IsSet())
			{
				return false;
			}
			const FInventoryRow& InventoryRow =
				Inventory[Index];
			if (SeenUnitIds.Contains(Unit->GetUnitId()))
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_unit_duplicate"),
					Unit->GetUnitId());
			}
			if (Unit->GetUnitId() != InventoryRow.UnitId)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_unit_extra"),
					Unit->GetUnitId());
			}
			if (Unit->GetUnitKind()
				!= InventoryRow.UnitKind)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_unit_kind_mismatch"),
					Unit->GetUnitId());
			}
			TArray<uint8> RowBytes;
			if (!TrySerializeSemanticUnitRow(
					Unit.GetValue(),
					RowBytes))
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_unit_serialize_failed"),
					Unit->GetUnitId());
			}
			if (!bAllowManagedUnitEdits
				&& Sha256(RowBytes) != InventoryRow.ContentHash)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_unit_hash_mismatch"),
					Unit->GetUnitId());
			}
			SeenUnitIds.Add(Unit->GetUnitId());
			Units.Add(MoveTemp(Unit.GetValue()));
		}

		TArray<FString> TargetUnitIds;
		if (!ReadStringArray(
				*Parsed.Manifest,
				TEXT("target_unit_ids"),
				true,
				TargetUnitIds,
				OutError))
		{
			return false;
		}
		FAuthoringModelPtr Model;
		if (!TryCompileAuthoringModel(
				MoveTemp(ContractId),
				static_cast<int32>(ContractVersion),
				MoveTemp(ProfileId),
				MoveTemp(BaseRevision),
				MoveTemp(Units),
				MoveTemp(TargetUnitIds),
				Resources,
				Model,
				OutError)
			|| !Model.IsValid())
		{
			if (OutError.GetCode()
				== TEXT(
					"unresolved_semantic_unit_reference")
				|| OutError.GetCode()
					== TEXT("missing_target_unit"))
			{
				OutError.Set(
					TEXT(
						"semantic_workspace_reference_unresolved"),
					OutError.GetSourceUnit());
			}
			return false;
		}
		if (!bAllowManagedUnitEdits
			&& Model->GetSemanticFingerprint() != ExpectedFingerprint)
		{
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_fingerprint_mismatch"),
				Parsed.Manifest->GetProvenance()
					.GetSourceUnit(),
				TEXT("/semantic_fingerprint"));
		}

		FUeAgentStateRevision BaseStateRevision(
			TEXT("invalid"),
			TEXT("invalid"),
			-1);
		if (!ParseRevision(
				*Parsed.Manifest,
				BaseStateRevision,
				OutError))
		{
			return false;
		}
		TArray<FStructuralEdit> StructuralEdits;
		if (!ParseStructuralEdits(
				*Parsed.Manifest,
				*Model,
				StructuralEdits,
				OutError))
		{
			return false;
		}
		FString SelectionFingerprint;
		FString ClosureFingerprint;
		TArray<FString> ClosureUnitIds;
		if (!OptionalString(
				*Parsed.Manifest,
				TEXT("selection_fingerprint"),
				SelectionFingerprint,
				OutError)
			|| !OptionalString(
				*Parsed.Manifest,
				TEXT("closure_fingerprint"),
				ClosureFingerprint,
				OutError)
			|| !ReadStringArray(
				*Parsed.Manifest,
				TEXT("closure_unit_ids"),
				false,
				ClosureUnitIds,
				OutError))
		{
			return false;
		}
		if (Mode == ESemanticWorkspaceMode::EditBundle)
		{
			TSet<FString> ClosureSet(ClosureUnitIds);
			if (SelectionFingerprint.IsEmpty()
				|| ClosureFingerprint.IsEmpty()
				|| ClosureSet.Num()
					!= Model->GetUnits().Num())
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_closure_incomplete"));
			}
			for (const FSemanticUnit& Unit :
				Model->GetUnits())
			{
				if (!ClosureSet.Contains(Unit.GetUnitId()))
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_closure_incomplete"),
						Unit.GetUnitId());
				}
			}
		}
		FString WorkspaceMaterial =
			LexToString(Mode) + TEXT("|")
			+ Model->GetSemanticFingerprint();
		for (const FString& Path : Parsed.ManagedPaths)
		{
			WorkspaceMaterial += TEXT("|") + Path;
		}
		for (const FInventoryRow& Row : Inventory)
		{
			WorkspaceMaterial += TEXT("|")
				+ Row.UnitId + TEXT("|")
				+ Row.ContentHash;
		}
		OutWorkspace =
			MakeShared<FResolvedSemanticWorkspace>(
				Source.GetKind(),
				Mode,
				Model.ToSharedRef(),
				MoveTemp(BaseStateRevision),
				MoveTemp(StructuralEdits),
				MoveTemp(Parsed.ManagedPaths),
				MoveTemp(SelectionFingerprint),
				MoveTemp(ClosureFingerprint),
				MoveTemp(ClosureUnitIds),
				Sha256Text(WorkspaceMaterial));
		if (!OutWorkspace->IsValid())
		{
			OutWorkspace.Reset();
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_invalid"));
		}
		return true;
	}

	bool TryResolveSemanticWorkspace(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError)
	{
		return TryResolveSemanticWorkspaceInternal(
			Source,
			ExpectedMode,
			false,
			Resources,
			OutWorkspace,
			OutError);
	}

	bool TryResolveEditedSemanticWorkspace(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError)
	{
		return TryResolveSemanticWorkspaceInternal(
			Source,
			ExpectedMode,
			true,
			Resources,
			OutWorkspace,
			OutError);
	}
}
