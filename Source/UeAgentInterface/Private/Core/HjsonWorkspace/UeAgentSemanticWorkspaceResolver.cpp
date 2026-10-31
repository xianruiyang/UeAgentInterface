#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentHjsonDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	struct FSemanticWorkspaceReadState
	{
		struct FDocument
		{
			FString SourceUnit;
			FString Text;
			FHjsonDocumentPtr Parsed;
			TMap<EUeAgentResourceDimension, int64> ParseCharges;
			bool bExactParseCharges = false;
		};
		FSemanticWorkspaceReadState* Previous = nullptr;
		TMap<FString, FDocument> Documents;
		int32 ReusedPublicSyntax = 0;
		TMap<FString, FString> HashOnlyDocuments;
		TMap<FString, FString> ResolvedHashOnlyDocuments;
		// Only one completed resolution is retained, and only for this synchronous
		// import. Reuse is conditional on rereading every source byte, not timestamps.
		FString ResolvedRoot;
		ESemanticWorkspaceMode ResolvedMode = ESemanticWorkspaceMode::State;
		bool bResolvedManagedEdits = false;
		FResolvedSemanticWorkspacePtr ResolvedWorkspace;
		FWorkspaceError ResolvedDiagnostics;
		TMap<FString, FHjsonDocumentPtr> ResolvedDocuments;
	};
	static thread_local FSemanticWorkspaceReadState* ActiveReadState = nullptr;
	struct FCompletedDocumentRead
	{
		FString SourceUnit, ContentHash;
		FHjsonDocumentPtr Parsed;
		TMap<EUeAgentResourceDimension, int64> ParseCharges;
	};
	// One successful resolver's public documents. Private baseline envelopes
	// keep their existing bounded typed/row proofs instead of retaining a second AST.
	static thread_local TMap<FString, FCompletedDocumentRead> PreviousDocumentReads;

	FSemanticWorkspaceReadScope::FSemanticWorkspaceReadScope()
		: State(MakeUnique<FSemanticWorkspaceReadState>())
	{
		State->Previous = ActiveReadState;
		ActiveReadState = State.Get();
	}

	FSemanticWorkspaceReadScope::~FSemanticWorkspaceReadScope()
	{
		ActiveReadState = State->Previous;
	}

	namespace
	{
		const FString ManifestPath =
			TEXT(".uai/workspace.hjson");
		const FString EmbeddedPath =
			TEXT("workspace.hjson");

		struct FInventoryRow
		{
			FString UnitId;
			FString PublicRef;
			ESemanticUnitKind UnitKind =
				ESemanticUnitKind::OpaqueExtension;
			FString LogicalPath;
			FString DocumentPath;
			FString UnitPointer;
			FString ContentHash;
			FString EditHash;
			FString EnvelopeRef;
			TArray<FString> ContentFiles;
			const FHjsonNode* SourceNode = nullptr;
			const FHjsonNode* Envelope = nullptr;
			FHjsonNodePtr RetainedSourceNode;
			FString SourceRowHash;
		};

		struct FBaselineParseProof
		{
			FString ContentHash;
			TMap<EUeAgentResourceDimension, int64> Charges;
			bool bExact = false;
		};
		struct FParsedSource
		{
			FHjsonDocumentPtr Envelope;
			FHjsonDocumentPtr ManifestDocument;
			TArray<FHjsonDocumentPtr> UnitDocuments;
			const FHjsonNode* Manifest = nullptr;
			TArray<const FHjsonNode*> UnitRows;
			TArray<FHjsonNodeRef> AssembledRows;
			TArray<const FHjsonNode*> UnitEnvelopes;
			TArray<FString> ManagedPaths;
			TSet<FString> ContentReferenceUnits;
			TMap<FString, FBaselineParseProof> BaselineProofs;
			TMap<FString, FString> InventoryHashes;
		};

		// A proof is produced by successful resolution, never by the export model.
		// It retains typed rows and their small source subtrees, not the manifest AST.
		struct FResolvedRowProof
		{
			FString InputHash;
			TOptional<FSemanticUnit> Unit;
			TOptional<FSemanticUnit> Baseline;
			TOptional<FSourceProvenance> Source;
			FHjsonDocumentPtr SourceDocument;
			TSet<FString> KeyedRoots;
			TMap<EUeAgentResourceDimension, int64> Charges;
			FWorkspaceError Diagnostics;
			bool bEdited = false;
		};
		struct FResolvedRowsProof
		{
			FString Root;
			FString ReferenceMapHash;
			ESemanticWorkspaceMode Mode = ESemanticWorkspaceMode::State;
			bool bManagedEdits = false;
			TMap<FString, FResolvedRowProof> Rows;
			TMap<FString, FBaselineParseProof> Baselines;
		};
		static thread_local TSharedPtr<const FResolvedRowsProof> PreviousResolvedRows;
		struct FManifestIndexProof
		{
			FString Root;
			FString ContentHash;
			FString ReferenceMapHash;
			FHjsonDocumentPtr Header;
			TArray<uint8> Utf8;
			TArray<FInventoryRow> Inventory;
			TMap<EUeAgentResourceDimension, int64> ParseCharges;
		};
		static thread_local TSharedPtr<const FManifestIndexProof> PreviousManifestIndex;
		struct FInventoryDocumentProof
		{
			FString ContentHash;
			TArray<FInventoryRow> Rows;
			TMap<EUeAgentResourceDimension, int64> ParseCharges;
		};
		static thread_local FString InventoryProofRoot;
		static thread_local TMap<FString, TSharedPtr<const FInventoryDocumentProof>> InventoryProofs;

		bool Fail(
			FWorkspaceError& OutError,
			const FString& Code,
			const FString& Reason,
			const FString& SourceUnit = FString(),
			const FString& JsonPointer = TEXT("/"),
			TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_resolve"));
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit, JsonPointer);
			if (ActiveReadState)
				if (const auto* Document = ActiveReadState->Documents.Find(SourceUnit); Document && Document->Parsed)
					if (const auto* Node = Document->Parsed->Find(JsonPointer)) OutError.Locate(Node->GetProvenance());
			return false;
		}

		bool FailResource(
			FWorkspaceError& OutError,
			const FUeAgentResourceChargeError& Error)
		{
			OutError.SetFailure(Error);
			return false;
		}

		bool FailAtNode(FWorkspaceError& OutError, FUeAgentStateSemanticError Error, const FHjsonNode& Node)
		{
			const auto& Source = Node.GetProvenance();
			OutError.SetFailure(MoveTemp(Error), Source.GetSourceUnit(), Source.GetJsonPointer());
			OutError.Locate(Source);
			if (ActiveReadState)
				if (const auto* Document = ActiveReadState->Documents.Find(Source.GetSourceUnit()); Document && Document->Parsed)
					OutError.Locate(*Document->Parsed);
			return false;
		}

		bool FailNode(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FHjsonNode& Node, TSharedPtr<FJsonValue> Expected = nullptr, bool bPublicActual = true)
		{
			return FailAtNode(OutError, HjsonNodeFailure(Code, Reason, TEXT("workspace_resolve"),
				Node, MoveTemp(Expected), bPublicActual), Node);
		}

		bool FailField(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FHjsonNode& Object, const FString& Field, TSharedPtr<FJsonValue> Expected = nullptr, bool bPublicActual = true)
		{
			const auto* Node = Object.Get(Field);
			return FailAtNode(OutError, HjsonFieldFailure(Code, Reason, TEXT("workspace_resolve"),
				Object, Field, MoveTemp(Expected), bPublicActual), Node ? *Node : Object);
		}

		bool FailCapturedField(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FHjsonNode& Object, const FString& Field, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			auto Diagnostic = HjsonFieldFailure(Code, Reason, TEXT("workspace_resolve"), Object, Field, MoveTemp(Expected), false);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.Source.SourceFile.Reset();
			Diagnostic.Source.JsonPath.Reset();
			Diagnostic.Source.Line = Diagnostic.Source.Column = 0;
			Diagnostic.PropertyPath = Field;
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}

		struct FNodeDifference
		{
			const FHjsonNode* Actual = nullptr;
			const FHjsonNode* Expected = nullptr;
			bool bCount = false;
		};

		bool EquivalentNode(const FHjsonNode& Left, const FHjsonNode& Right, FNodeDifference* Difference = nullptr)
		{
			const auto Reject = [&](const FHjsonNode* Expected = nullptr, bool bCount = false)
			{
				if (Difference) *Difference = {&Left, Expected, bCount};
				return false;
			};
			if (Left.GetKind() != Right.GetKind())
			{
				return Reject(&Right);
			}
			switch (Left.GetKind())
			{
			case EHjsonNodeKind::Null:
				return true;
			case EHjsonNodeKind::Boolean:
				return Left.GetBoolean() == Right.GetBoolean() || Reject(&Right);
			case EHjsonNodeKind::String:
				return Left.GetText() == Right.GetText() || Reject(&Right);
			case EHjsonNodeKind::Integer:
			case EHjsonNodeKind::Decimal:
				return Left.GetLexeme() == Right.GetLexeme() || Reject(&Right);
			case EHjsonNodeKind::Array:
			{
				const TArray<FHjsonNodeRef>& L = Left.GetItems();
				const TArray<FHjsonNodeRef>& R = Right.GetItems();
				if (L.Num() != R.Num())
				{
					return Reject(&Right, true);
				}
				for (int32 Index = 0; Index < L.Num(); ++Index)
				{
					if (!EquivalentNode(L[Index].Get(), R[Index].Get(), Difference))
					{
						return false;
					}
				}
				return true;
			}
			case EHjsonNodeKind::Object:
			{
				const TArray<TPair<FString, FHjsonNodeRef>>& L =
					Left.GetEntries();
				const TArray<TPair<FString, FHjsonNodeRef>>& R =
					Right.GetEntries();
				if (L.Num() != R.Num())
				{
					return Reject(&Right, true);
				}
				for (const TPair<FString, FHjsonNodeRef>& Entry : L)
				{
					const FHjsonNode* Other = Right.Get(Entry.Key);
					if (!Other)
					{
						if (Difference) *Difference = {&Entry.Value.Get(), nullptr, false};
						return false;
					}
					if (!EquivalentNode(Entry.Value.Get(), *Other, Difference))
					{
						return false;
					}
				}
				return true;
			}
			default:
				return Reject(&Right);
			}
		}

		bool FailMetadataDifference(FWorkspaceError& OutError, const FString& Reason, const FNodeDifference& Difference)
		{
			auto Diagnostic = HjsonNodeFailure(TEXT("semantic_workspace_envelope_mismatch"), Reason,
				TEXT("workspace_resolve"), *Difference.Actual, nullptr, false);
			Diagnostic.ExpectedEvidence = HjsonDiagnosticValue(Difference.Expected);
			Diagnostic.ExpectedValuePresent = Difference.Expected != nullptr;
			if (Difference.bCount)
			{
				const auto Count = [](const FHjsonNode& Node)
				{ return Node.GetKind() == EHjsonNodeKind::Array ? Node.GetItems().Num() : Node.GetEntries().Num(); };
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Count(*Difference.Actual));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(Count(*Difference.Expected));
			}
			return FailAtNode(OutError, MoveTemp(Diagnostic), *Difference.Actual);
		}

		const FHjsonNode* ResolveEnvelopeField(
			const FHjsonNode& Row,
			const FHjsonNode* Envelope,
			const FString& Key,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* RowValue = Row.Get(Key);
			const FHjsonNode* EnvelopeValue =
				Envelope == nullptr ? nullptr : Envelope->Get(Key);
			FNodeDifference Difference;
			if (RowValue != nullptr && EnvelopeValue != nullptr
				&& !EquivalentNode(*RowValue, *EnvelopeValue, &Difference))
			{
				FailMetadataDifference(OutError, FString::Printf(TEXT("Metadata field '%s' differs from its captured workspace value."), *Key), Difference);
				return nullptr;
			}
			return RowValue != nullptr ? RowValue : EnvelopeValue;
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			std::initializer_list<TPair<
				EUeAgentResourceDimension,
				int64>> Charges,
			const FString& Path,
			FWorkspaceError& OutError)
		{
			FUeAgentResourceChargeError ResourceError;
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

		FString Sha256(const uint8* Bytes, const int64 Num)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			const uint8 Empty = 0;
			SHA256(
				Num == 0 ? &Empty : Bytes,
				static_cast<size_t>(Num),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString Sha256(const TArray<uint8>& Bytes)
		{
			return Sha256(Bytes.GetData(), Bytes.Num());
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

		FString HashReferenceMap(const TArray<FInventoryRow>& Inventory)
		{
			TArray<const FInventoryRow*> Sorted;
			for (const auto& Row : Inventory) Sorted.Add(&Row);
			Sorted.Sort([](const FInventoryRow& A, const FInventoryRow& B) { return A.PublicRef < B.PublicRef; });
			FString Material;
			for (const auto* Row : Sorted)
				Material += FString::Printf(TEXT("%d:%s%d:%s"), Row->PublicRef.Len(), *Row->PublicRef, Row->UnitId.Len(), *Row->UnitId);
			return Sha256Text(Material);
		}

		bool TryCanonicalValue(const FHjsonNode& Root, FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError,
			const TMap<FString, FString>* PublicRefToUnitId = nullptr,
			bool bInterpretTypedValues = true);

		bool TrySerializeEditNode(
			const FHjsonNode& Node,
			FUeAgentResourceTracker& Resources,
			FWorkspaceError& OutError,
			TArray<uint8>& OutBytes)
		{
			OutBytes.Reset();
			FUeAgentCanonicalValuePtr Value;
			if (!TryCanonicalValue(Node, Resources, Value, OutError, nullptr, false))
			{
				return false;
			}
			if (!Value.IsValid()) {
				OutError.SetSourceFailure(TEXT("semantic_edit_value_missing"), TEXT("The edit node has no canonical value to serialize."),
					Node.GetProvenance().GetSourceUnit(), Node.GetProvenance().GetJsonPointer());
				OutError.Locate(Node.GetProvenance()); return false;
			}
			if (!TrySerializeSemanticHjsonObject(*Value, OutBytes, OutError, Node.GetProvenance().GetSourceUnit())) {
				OutError.Locate(Node.GetProvenance()); return false;
			}
			return true;
		}

		const FHjsonNode* RequiredNode(
			const FHjsonNode& Object,
			const FString& Key,
			FWorkspaceError& OutError, const FString& ExpectedType = FString())
		{
			const FHjsonNode* Result = Object.Get(Key);
			if (Result == nullptr)
			{
				FailField(OutError, TEXT("semantic_workspace_manifest_field_missing"),
					FString::Printf(TEXT("Required workspace metadata field '%s' is missing."), *Key), Object, Key,
					ExpectedType.IsEmpty() ? TSharedPtr<FJsonValue>() : MakeShared<FJsonValueString>(ExpectedType), false);
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
				RequiredNode(Object, Key, OutError, TEXT("string"));
			if (Node == nullptr)
			{
				return false;
			}
			if (Node->GetKind() != EHjsonNodeKind::String)
			{
				return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					FString::Printf(TEXT("Workspace metadata field '%s' must be a string."), *Key), *Node,
					MakeShared<FJsonValueString>(TEXT("string")), false);
			}
			OutValue = Node->GetText().TrimStartAndEnd();
			if (OutValue.IsEmpty())
			{
				return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					FString::Printf(TEXT("Workspace metadata field '%s' must not be empty."), *Key), *Node,
					MakeShared<FJsonValueString>(TEXT("non-empty string")));
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
				return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					FString::Printf(TEXT("Optional workspace metadata field '%s' must be a string when present."), *Key), *Node,
					MakeShared<FJsonValueString>(TEXT("string")), false);
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
				RequiredNode(Object, Key, OutError, TEXT("integer"));
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
				return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					FString::Printf(TEXT("Workspace metadata field '%s' must be a signed 64-bit integer at least %lld."), *Key, Minimum), *Node,
					MakeShared<FJsonValueString>(FString::Printf(TEXT("int64 >= %lld"), Minimum)), false);
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
				if (!bRequired) return true;
				RequiredNode(Object, Key, OutError, TEXT("array"));
				return false;
			}
			if (Node->GetKind() != EHjsonNodeKind::Array)
			{
				return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					TEXT("The workspace metadata field must be an array."), *Node, MakeShared<FJsonValueString>(TEXT("array")), false);
			}
			TSet<FString> Seen;
			for (const FHjsonNodeRef& Item : Node->GetItems())
			{
				if (Item->GetKind() != EHjsonNodeKind::String)
				{
					return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
						TEXT("Each entry in this workspace metadata array must be a string."), Item.Get(), MakeShared<FJsonValueString>(TEXT("string")), false);
				}
				FString Value = Item->GetText().TrimStartAndEnd();
				if (Value.IsEmpty() || Seen.Contains(Value))
				{
					return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
						Value.IsEmpty() ? TEXT("Workspace metadata array entries must not be empty.")
							: TEXT("This workspace metadata array entry duplicates an earlier entry."), Item.Get(),
						MakeShared<FJsonValueString>(TEXT("non-empty unique string")), false);
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

		TSharedPtr<FJsonValue> UnitKindChoices()
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (uint8 Index = 0; Index <= static_cast<uint8>(ESemanticUnitKind::OpaqueExtension); ++Index)
				Values.Add(MakeShared<FJsonValueString>(LexToString(static_cast<ESemanticUnitKind>(Index))));
			return MakeShared<FJsonValueArray>(MoveTemp(Values));
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
			const FString& Reason,
			TSharedPtr<FJsonValue> Expected = nullptr,
			const FString& Code =
				TEXT("semantic_workspace_typed_value_invalid"))
		{
			return FailNode(OutError, Code, Reason, Node, MoveTemp(Expected));
		}

		bool FailTypedValidation(FWorkspaceError& OutError, Contract::FDiagnostic Error, const FHjsonNode& Root)
		{
			const FString& Path = Error.PropertyPath.IsEmpty() ? Error.SemanticPointer : Error.PropertyPath;
			FString Pointer = Path.StartsWith(TEXT("/")) ? Path : TEXT("/") + Path;
			Pointer.ReplaceInline(TEXT("."), TEXT("/"));
			Pointer.ReplaceInline(TEXT("["), TEXT("/"));
			Pointer.ReplaceInline(TEXT("]"), TEXT(""));
			if (const auto* Node = Path.IsEmpty() ? nullptr : Root.Find(Pointer))
				return FailAtNode(OutError, MoveTemp(Error), *Node);
			Error.SourceKind = TEXT("related_input");
			return FailAtNode(OutError, MoveTemp(Error), Root);
		}

		bool ReadTypedString(
			const FHjsonNode& Object,
			const FString& Key,
			FString& OutValue,
			FWorkspaceError& OutError, bool bPublicActual = true)
		{
			const FHjsonNode* Node = Object.Get(Key);
			if (!Node || Node->GetKind() != EHjsonNodeKind::String)
			{
				return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					FString::Printf(TEXT("Typed value field '%s' must be a string."), *Key), Object, Key,
					MakeShared<FJsonValueString>(TEXT("string")), bPublicActual);
			}
			OutValue = Node->GetText().TrimStartAndEnd();
			return !OutValue.IsEmpty()
				|| FailNode(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					FString::Printf(TEXT("Typed value field '%s' must not be empty."), *Key), *Node,
					MakeShared<FJsonValueString>(TEXT("non-empty string")), bPublicActual);
		}

		bool ReadTypedStringArray(
			const FHjsonNode& Object,
			const FString& Key,
			TArray<FString>& OutValues,
			FWorkspaceError& OutError, bool bPublicActual = true)
		{
			OutValues.Reset();
			const FHjsonNode* Node = Object.Get(Key);
			if (!Node || Node->GetKind() != EHjsonNodeKind::Array)
			{
				return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					FString::Printf(TEXT("Typed value field '%s' must be an array."), *Key), Object, Key,
					MakeShared<FJsonValueString>(TEXT("array")), bPublicActual);
			}
			for (const FHjsonNodeRef& Item : Node->GetItems())
			{
				if (Item->GetKind() != EHjsonNodeKind::String)
				{
					return FailNode(OutError, TEXT("semantic_workspace_typed_value_invalid"),
						FString::Printf(TEXT("Each element of '%s' must be a string."), *Key), Item.Get(),
						MakeShared<FJsonValueString>(TEXT("string")), bPublicActual);
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
				return FailTypedValue(OutError, Object, TEXT("A temporal value must be an object."), MakeShared<FJsonValueString>(TEXT("object")));
			}
			if (bRequireMarker)
			{
				FString Marker;
				if (!ReadTypedString(
						Object,
						TEXT("$uai"),
						Marker,
						OutError))
				{
					return false;
				}
				if (Marker != TEXT("temporal")) return FailTypedValue(OutError, *Object.Get(TEXT("$uai")),
					TEXT("A temporal value requires $uai to be temporal."), MakeShared<FJsonValueString>(TEXT("temporal")));
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
					OutError, false)
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
				|| FailTypedValidation(OutError, OutValue.GetValidationDiagnostic(), Object);
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
				return FailTypedValue(OutError, Object, TEXT("A spatial value must be an object."), MakeShared<FJsonValueString>(TEXT("object")));
			}
			FString Marker;
			FString Subject;
			FString QuantityText;
			if (!ReadTypedString(
					Object,
					TEXT("$uai"),
					Marker,
					OutError)
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
			if (Marker != TEXT("spatial")) return FailTypedValue(OutError, *Object.Get(TEXT("$uai")),
				TEXT("A spatial value requires $uai to be spatial."), MakeShared<FJsonValueString>(TEXT("spatial")));
			if (!ParseSpatialQuantityKind(
					QuantityText,
					QuantityKind))
			{
				return FailTypedValue(OutError, *Object.Get(TEXT("quantity_kind")),
					TEXT("The spatial quantity_kind must be point, direction, normal, or transform."),
					MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
						MakeShared<FJsonValueString>(TEXT("point")), MakeShared<FJsonValueString>(TEXT("direction")),
						MakeShared<FJsonValueString>(TEXT("normal")), MakeShared<FJsonValueString>(TEXT("transform"))}));
			}
			const FHjsonNode* FrameNode = Object.Get(TEXT("frame"));
			if (!FrameNode
				|| FrameNode->GetKind() != EHjsonNodeKind::Object)
			{
				return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					TEXT("A spatial value requires a frame object."), Object, TEXT("frame"), MakeShared<FJsonValueString>(TEXT("object")));
			}
			FString FrameId;
			FString BasisFrameId;
			FString Unit;
			FString BasisEvidence;
			if (!ReadTypedString(
					*FrameNode,
					TEXT("frame_id"),
					FrameId,
					OutError, false)
				|| !ReadTypedString(
					*FrameNode,
					TEXT("basis_frame_id"),
					BasisFrameId,
					OutError, false)
				|| !ReadTypedString(
					*FrameNode,
					TEXT("unit"),
					Unit,
					OutError)
				|| !ReadTypedString(
					*FrameNode,
					TEXT("basis_evidence"),
					BasisEvidence,
					OutError, false))
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
				return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					TEXT("The spatial frame dimension must be an integer from 1 through 2147483647."), *FrameNode, TEXT("dimension"),
					MakeShared<FJsonValueString>(TEXT("integer from 1 through 2147483647")));
			}
			const FHjsonNode* BasisNode = FrameNode->Get(TEXT("basis"));
			TArray<TArray<FString>> Basis;
			if (!BasisNode
				|| BasisNode->GetKind() != EHjsonNodeKind::Array)
			{
				return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					TEXT("The spatial frame basis must be an array of rows."), *FrameNode, TEXT("basis"), MakeShared<FJsonValueString>(TEXT("array")));
			}
			for (const FHjsonNodeRef& RowNode : BasisNode->GetItems())
			{
				if (RowNode->GetKind() != EHjsonNodeKind::Array)
				{
					return FailTypedValue(OutError, RowNode.Get(), TEXT("Each spatial frame basis row must be an array."), MakeShared<FJsonValueString>(TEXT("array")));
				}
				TArray<FString> Row;
				for (const FHjsonNodeRef& Cell : RowNode->GetItems())
				{
					if (Cell->GetKind() != EHjsonNodeKind::String)
					{
						return FailTypedValue(OutError, Cell.Get(), TEXT("Each spatial frame basis cell must be a numeric string."), MakeShared<FJsonValueString>(TEXT("numeric string")));
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
					OutError, false))
			{
				return false;
			}
			const FHjsonNode* FrameObserved =
				FrameNode->Get(TEXT("observed_at"));
			const FHjsonNode* ValueObserved =
				Object.Get(TEXT("observed_at"));
			FUeAgentTemporalPoint FrameTime(TEXT("invalid"), TEXT("0"));
			FUeAgentTemporalPoint ValueTime(TEXT("invalid"), TEXT("0"));
			if (!FrameObserved) return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
				TEXT("The spatial frame requires observed_at."), *FrameNode, TEXT("observed_at"), MakeShared<FJsonValueString>(TEXT("temporal object")));
			if (!ValueObserved) return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
				TEXT("The spatial value requires observed_at."), Object, TEXT("observed_at"), MakeShared<FJsonValueString>(TEXT("temporal object")));
			if (!ParseTypedTemporal(
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
				return FailTypedValidation(OutError, Frame.GetValidationDiagnostic(), *FrameNode);
			}
			OutValue = FUeAgentSpatialValue(
				MoveTemp(Subject),
				QuantityKind,
				MoveTemp(Frame),
				MoveTemp(Components),
				MoveTemp(ValueTime),
				MoveTemp(ConversionEvidence));
			return OutValue.IsValid()
				|| FailTypedValidation(OutError, OutValue.GetValidationDiagnostic(), Object);
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
				return FailTypedValue(OutError, *MarkerNode, TEXT("The $uai typed-value marker must be a string."), MakeShared<FJsonValueString>(TEXT("string")));
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
					return FailAtNode(OutError, MoveTemp(StateError), Root);
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
					return FailAtNode(OutError, MoveTemp(StateError), Root);
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
					return FailAtNode(OutError, MoveTemp(StateError), Root);
				}
				return true;
			}
			return FailTypedValue(
				OutError,
				*MarkerNode,
				TEXT("The $uai marker must be decimal, temporal, or spatial."),
				MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(TEXT("decimal")),
					MakeShared<FJsonValueString>(TEXT("temporal")), MakeShared<FJsonValueString>(TEXT("spatial"))}),
				TEXT("semantic_workspace_typed_value_kind_unsupported"));
		}

		bool TryCanonicalValue(
			const FHjsonNode& Root,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError,
			const TMap<FString, FString>* PublicRefToUnitId,
			const bool bInterpretTypedValues)
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
				if (bInterpretTypedValues && !Visit.bExpanded
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
				{
					const FString* Internal = PublicRefToUnitId == nullptr
						? nullptr
						: PublicRefToUnitId->Find(Visit.Node->GetText());
					bCreated =
						FUeAgentCanonicalValue::TryString(
							Internal == nullptr
								? Visit.Node->GetText() : *Internal,
							Resources,
							Value,
							StateError);
					break;
				}
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
						const FString* InternalKey =
							PublicRefToUnitId == nullptr
								? nullptr
								: PublicRefToUnitId->Find(Item.Key);
						Items.Emplace(
							InternalKey == nullptr ? Item.Key : *InternalKey,
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
					if (StateError.IsEmpty()) StateError = Contract::FDiagnostic::Failure(
						TEXT("semantic_workspace_unit_value_invalid"),
						TEXT("The HJSON node could not be represented as a semantic value."), TEXT("workspace_resolve"));
					return FailAtNode(OutError, MoveTemp(StateError), *Visit.Node);
				}
				Converted.Add(Visit.Node, MoveTemp(Value));
			}
			OutValue = Converted.FindChecked(&Root);
			return OutValue.IsValid();
		}

		bool ParseSemanticUnit(
			const FHjsonNode& Row,
			const FHjsonNode* Envelope,
			const FString& InventoryUnitId,
			const FString& InventoryKind,
			const TMap<FString, FString>& PublicRefToUnitId,
			FUeAgentResourceTracker& Resources,
			TOptional<FSemanticUnit>& OutUnit,
			TOptional<bool>& OutManagedSemanticChanged,
			TSet<FString>& OutResolvedKeyedOperationRoots,
			FHjsonNodePtr& OutValueSources,
			FWorkspaceError& OutError)
		{
			OutManagedSemanticChanged.Reset();
			OutResolvedKeyedOperationRoots.Reset();
			OutUnit.Reset();
			OutValueSources.Reset();
			if (Row.GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_unit_document_invalid"), TEXT("A semantic unit document row must be an object."),
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
				auto RequiredResolvedString =
				[&](const FString& Key, FString& Value)
			{
				const FHjsonNode* Node = ResolveEnvelopeField(
					Row, Envelope, Key, OutError);
				if (Node == nullptr)
				{
					if (!OutError.HasErrors()) FailField(OutError, TEXT("semantic_workspace_manifest_field_missing"),
						FString::Printf(TEXT("Required semantic metadata field '%s' is missing."), *Key),
						Row, Key, MakeShared<FJsonValueString>(TEXT("string")), false);
					return false;
				}
				return RequiredString(Row.Get(Key) ? Row : *Envelope, Key, Value, OutError);
			};
			auto OptionalResolvedString =
				[&](const FString& Key, FString& Value)
			{
				const FHjsonNode* Node = ResolveEnvelopeField(
					Row, Envelope, Key, OutError);
				if (Node == nullptr)
				{
					return OutError.GetCode().IsEmpty();
				}
				return OptionalString(Row.Get(Key) ? Row : *Envelope, Key, Value, OutError);
			};
			auto ReadResolvedArray =
				[&](const FString& Key, TArray<FString>& Values)
			{
				const FHjsonNode* RowValue = Row.Get(Key);
				const FHjsonNode* EnvelopeValue =
					Envelope == nullptr ? nullptr : Envelope->Get(Key);
				FNodeDifference Difference;
				if (RowValue != nullptr && EnvelopeValue != nullptr
					&& !EquivalentNode(*RowValue, *EnvelopeValue, &Difference))
				{
					return FailMetadataDifference(OutError,
						FString::Printf(TEXT("Metadata array '%s' differs from its captured workspace value."), *Key), Difference);
				}
				if (RowValue != nullptr)
				{
					return ReadStringArray(Row, Key, false, Values, OutError);
				}
				if (EnvelopeValue != nullptr)
				{
					return ReadStringArray(
						*Envelope, Key, false, Values, OutError);
				}
				return true;
			};
			const FHjsonNode* KindNode = Row.Get(TEXT("unit_kind"));
			if (KindNode != nullptr)
			{
				if (KindNode->GetKind() != EHjsonNodeKind::String)
				{
					return FailNode(OutError, TEXT("semantic_workspace_unit_document_invalid"),
						TEXT("The semantic unit_kind field must be a string."), *KindNode, MakeShared<FJsonValueString>(TEXT("string")));
				}
				KindText = KindNode->GetText().TrimStartAndEnd();
				if (!InventoryKind.IsEmpty()
					&& KindText != InventoryKind)
				{
					return FailNode(OutError, TEXT("semantic_workspace_envelope_mismatch"),
						TEXT("The authored unit kind differs from the workspace inventory."), *KindNode, MakeShared<FJsonValueString>(InventoryKind));
				}
			}
			else
			{
				KindText = InventoryKind;
			}
			const FHjsonNode* UnitIdNode = Row.Get(TEXT("unit_id"));
			if (UnitIdNode == nullptr)
			{
				UnitId = InventoryUnitId;
			}
			else if (UnitIdNode->GetKind() != EHjsonNodeKind::String)
			{
				return FailNode(OutError, TEXT("semantic_workspace_unit_document_invalid"), TEXT("The semantic unit_id field must be a string."),
					*UnitIdNode, MakeShared<FJsonValueString>(TEXT("string")), false);
			}
			else
			{
				UnitId = UnitIdNode->GetText().TrimStartAndEnd();
				if (!InventoryUnitId.IsEmpty() && UnitId != InventoryUnitId)
				{
					auto Diagnostic = HjsonNodeFailure(TEXT("semantic_workspace_envelope_mismatch"),
						TEXT("The authored unit identity differs from the workspace inventory."), TEXT("workspace_resolve"), *UnitIdNode, nullptr, false);
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(InventoryUnitId);
					return FailAtNode(OutError, MoveTemp(Diagnostic), *UnitIdNode);
				}
			}
			if (UnitId.IsEmpty()) return FailField(OutError, TEXT("semantic_workspace_unit_document_invalid"),
				TEXT("The semantic unit identity is empty in both the document and its inventory."), Row, TEXT("unit_id"),
				MakeShared<FJsonValueString>(TEXT("non-empty string")), false);
			if (!RequiredResolvedString(TEXT("semantic_type_ref"), SemanticTypeRef)
				|| !RequiredResolvedString(TEXT("role"), Role)
				|| !OptionalResolvedString(TEXT("owner_ref"), OwnerRef)
				|| !OptionalResolvedString(TEXT("field_set_ref"), FieldSetRef)
				|| !OptionalResolvedString(TEXT("ordering_ref"), OrderingRef)
				|| !OptionalResolvedString(TEXT("layout_ref"), LayoutRef)
				|| !OptionalResolvedString(TEXT("source_provenance"), SourceProvenance)
				|| !OptionalResolvedString(TEXT("target_identity_key"), TargetIdentityKey))
			{
				return false;
			}
			ESemanticUnitKind UnitKind;
			if (!ParseUnitKind(KindText, UnitKind))
			{
				auto Diagnostic = HjsonFieldFailure(TEXT("semantic_workspace_unit_kind_invalid"),
					TEXT("The semantic unit kind is unsupported."), TEXT("workspace_resolve"), KindNode ? Row : (Envelope ? *Envelope : Row),
					TEXT("unit_kind"), UnitKindChoices());
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(KindText);
				Diagnostic.ActualValuePresent = true;
				return FailAtNode(OutError, MoveTemp(Diagnostic), KindNode ? *KindNode : Row);
			}
			const FHjsonNode* Definition = Row.Get(TEXT("definition"));
			const bool bKeyedProperty = UnitKind == ESemanticUnitKind::PropertySet
				&& Row.Get(TEXT("values")) == nullptr && Envelope && Envelope->Get(TEXT("captured_type"));
			if (!bKeyedProperty
				&& (Definition || (UnitKind == ESemanticUnitKind::ItemState && !FieldSetRef.IsEmpty())))
			{
				const FString* ResolvedDefinition = Definition && Definition->GetKind() == EHjsonNodeKind::String
					? PublicRefToUnitId.Find(Definition->GetText()) : nullptr;
				if (UnitKind != ESemanticUnitKind::ItemState || FieldSetRef.IsEmpty() || !Definition
					|| Definition->GetKind() != EHjsonNodeKind::String
					|| (ResolvedDefinition ? *ResolvedDefinition : Definition->GetText()) != FieldSetRef)
					return Fail(OutError, TEXT("semantic_workspace_item_definition_read_only"), TEXT("The item's definition reference must retain its captured field-set definition."),
						Row.GetProvenance().GetSourceUnit(), TEXT("/definition"));
			}
			const FHjsonNode* ValuesNode = Row.Get(TEXT("values"));
			const FHjsonNode* CapturedValues =
				Envelope == nullptr ? nullptr : Envelope->Get(TEXT("captured_values"));
			const bool bGraphProjection =
				UnitKind == ESemanticUnitKind::StructureState
				&& ValuesNode == nullptr
				&& IsGraphAuthoringStructureType(SemanticTypeRef,
					CapturedValues && CapturedValues->Get(TEXT("nodes")) && CapturedValues->Get(TEXT("edges")));
			// Decomposed document collections carry semantic-id#document-path.
			// Their native capture and contract belong to composition, not to a
			// global root-name match (e.g. Sequence and UMG both have "bindings").
			const bool bDocumentCollectionProjection = UnitKind == ESemanticUnitKind::StructureState
				&& ValuesNode == nullptr && SemanticTypeRef.Contains(TEXT("#"));
			TArray<const FKeyedItemProjectionContract*> KeyedItemContracts;
			for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
				if (!bGraphProjection && !bDocumentCollectionProjection && (Row.Get(Contract.RootField) != nullptr
					|| (CapturedValues && CapturedValues->Get(Contract.RootField))))
				{
					KeyedItemContracts.Add(&Contract);
				}
			const bool bKeyedItemProjection =
				UnitKind == ESemanticUnitKind::StructureState
				&& ValuesNode == nullptr
				&& !KeyedItemContracts.IsEmpty();
			const bool bActorComponentItemProjection =
				UnitKind == ESemanticUnitKind::ItemState
				&& Role == TEXT("actor_component_hierarchy")
				&& (SemanticTypeRef.EndsWith(TEXT(".actor.target"))
					|| SemanticTypeRef.EndsWith(TEXT(".component.target")));
			if (!bGraphProjection && !bKeyedProperty
				&& !bKeyedItemProjection && !bActorComponentItemProjection
				&& ValuesNode == nullptr)
			{
				ValuesNode = CapturedValues;
			}
			if (!bGraphProjection && !bKeyedProperty && !bKeyedItemProjection
				&& !bActorComponentItemProjection
				&& (ValuesNode == nullptr
				|| ValuesNode->GetKind() != EHjsonNodeKind::Object))
			{
				return FailField(OutError, TEXT("semantic_workspace_unit_values_invalid"),
					TEXT("The semantic unit requires a values object or a captured values object."),
					Row.Get(TEXT("values")) || !Envelope ? Row : *Envelope,
					Row.Get(TEXT("values")) || !Envelope ? TEXT("values") : TEXT("captured_values"),
					MakeShared<FJsonValueString>(TEXT("object")), false);
			}
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			// Inspect only structural owner records, never reflected business values.
			// Keep the original nodes until every independent seed has been checked.
			if (UnitKind == ESemanticUnitKind::StructureState)
			{
				FWorkspaceError SeedErrors;
				auto CheckPayload = [&SeedErrors](const FHjsonNode* Owner, const TCHAR* Field)
				{
					const FHjsonNode* Payload = Owner ? Owner->Get(Field) : nullptr;
					if (!Payload) return;
					FWorkspaceError Error;
					if (!UeAgentDetailsAuthoringService::ValidatePropertyPayload(*Payload, Error)) SeedErrors.Append(Error);
				};
				auto CheckOwner = [&CheckPayload](const FHjsonNode& Owner)
				{
					CheckPayload(&Owner, TEXT("properties"));
					CheckPayload(&Owner, TEXT("property_changes"));
					CheckPayload(Owner.Get(TEXT("slot")), TEXT("properties"));
					if (const FHjsonNode* Renderers = Owner.Get(TEXT("renderers")); Renderers && Renderers->GetKind() == EHjsonNodeKind::Object)
						for (const auto& Renderer : Renderers->GetEntries()) CheckPayload(&Renderer.Value.Get(), TEXT("properties"));
				};
				auto CheckCollection = [&CheckOwner](const FHjsonNode* Collection)
				{
					if (!Collection || Collection->GetKind() != EHjsonNodeKind::Object) return;
					for (const auto& Entry : Collection->GetEntries()) CheckOwner(Entry.Value.Get());
				};
				auto CheckStructure = [&CheckCollection](const FHjsonNode* Structure)
				{
					if (!Structure || Structure->GetKind() != EHjsonNodeKind::Object) return;
					for (const TCHAR* Field : {TEXT("nodes"), TEXT("widgets"), TEXT("items"), TEXT("goals")})
						CheckCollection(Structure->Get(Field));
					const FHjsonNode* Emitters = Structure->Get(TEXT("emitters"));
					CheckCollection(Emitters);
					if (Emitters && Emitters->GetKind() == EHjsonNodeKind::Object)
						for (const auto& Entry : Emitters->GetEntries()) CheckCollection(Entry.Value->Get(TEXT("renderers")));
					const FHjsonNode* Tree = Structure->Get(TEXT("widget_tree"));
					if (Tree) CheckCollection(Tree->Get(TEXT("widgets")));
					const FHjsonNode* Graphs = Structure->Get(TEXT("graphs"));
					if (Graphs && Graphs->GetKind() == EHjsonNodeKind::Object)
						for (const auto& Graph : Graphs->GetEntries()) CheckCollection(Graph.Value->Get(TEXT("nodes")));
				};
				if (bKeyedItemProjection)
				{
					for (const auto* Contract : KeyedItemContracts)
					{
						// Typed record properties belong to their row schema. They are
						// not reflected UObject property documents or creation seeds.
						if (Contract->RecordSchemaField.IsEmpty() && Contract->bInterpretSemanticValues)
							CheckCollection(Row.Get(Contract->RootField));
					}
				}
				else
				{
					const FHjsonNode* Structure = bDocumentCollectionProjection || bGraphProjection ? &Row : ValuesNode;
					if (!Structure) Structure = &Row;
					CheckStructure(Structure);
					CheckStructure(Structure->Get(TEXT("provider_extension")));
				}
				if (SeedErrors.HasErrors()) { OutError = MoveTemp(SeedErrors); return false; }
			}
			if (bDocumentCollectionProjection)
			{
				for (const auto& Field : CapturedValues->GetEntries())
				{
					const FHjsonNode* Edited = Row.Get(Field.Key);
					FUeAgentCanonicalValuePtr Value;
					if (!TryCanonicalValue(Edited ? *Edited : Field.Value.Get(), Resources, Value,
						OutError, &PublicRefToUnitId)) return false;
					Values.Add(Field.Key, Value.ToSharedRef());
				}
			}
			else if (bGraphProjection)
			{
				if (CapturedValues == nullptr
					|| CapturedValues->GetKind() != EHjsonNodeKind::Object)
				{
					return FailCapturedField(OutError, TEXT("semantic_workspace_graph_capture_missing"),
						TEXT("The graph has no captured values object; export the workspace again."), Envelope ? *Envelope : Row,
						TEXT("captured_values"), MakeShared<FJsonValueString>(TEXT("object")));
				}
				FUeAgentCanonicalValuePtr CapturedRoot;
				FUeAgentCanonicalValuePtr VisibleRoot;
				if (!TryCanonicalValue(
						*CapturedValues, Resources, CapturedRoot, OutError)
					|| !TryCanonicalValue(
						Row, Resources, VisibleRoot, OutError,
						&PublicRefToUnitId)
					|| !TryResolveGraphAuthoringProjection(
						*VisibleRoot,
						*CapturedRoot,
						Row.GetProvenance().GetSourceUnit(),
						Resources,
						Values,
						OutError, &Row, &OutValueSources))
				{
					return false;
				}
			}
			else if (bKeyedItemProjection)
			{
				auto IsCollectionRoot = [&](const FString& Name) {
					return KeyedItemContracts.ContainsByPredicate([&](const auto* Contract) { return Contract->RootField == Name; });
				};
				auto IsDeclaredRootField = [&](const FString& Name) {
					return KeyedItemContracts.ContainsByPredicate([&](const auto* Contract) {
						return Contract->RootField == Name || Contract->RecordSchemaField == Name
							|| Contract->IsReadOnlyRootField(Name) || Contract->IsEditableRootField(Name);
					});
				};
				auto ResolveCollection = [&](const FKeyedItemProjectionContract& Contract, FWorkspaceError& OutError) -> bool {
					const FHjsonNode* CapturedItems = CapturedValues == nullptr
						? nullptr : CapturedValues->Get(Contract.RootField);
					const FHjsonNode* VisibleItems = Row.Get(Contract.RootField);
					if (CapturedItems == nullptr)
					{
						return FailCapturedField(OutError, TEXT("semantic_workspace_keyed_item_capture_missing"),
							TEXT("The collection has no captured items; export the workspace again."),
							CapturedValues ? *CapturedValues : (Envelope ? *Envelope : Row),
							CapturedValues ? Contract.RootField : TEXT("captured_values"));
					}
					FUeAgentCanonicalValuePtr Captured;
					FUeAgentCanonicalValuePtr Visible;
					FUeAgentCanonicalValuePtr Resolved;
					FUeAgentCanonicalValuePtr RecordSchema;
					bool bSemanticChanged = false;
					bool bRootChanged = false;
					for (const auto& Field : Row.GetEntries())
						if (Contract.IsEditableRootField(Field.Key) && !CapturedValues->Get(Field.Key))
							return FailCapturedField(OutError, TEXT("keyed_item_root_field_not_captured"),
								FString::Printf(TEXT("Editable root field '%s' has no captured baseline."), *Field.Key), *CapturedValues, Field.Key);
					if (!Contract.ReadOnlyRootFields.IsEmpty() || !Contract.EditableRootFields.IsEmpty())
						for (const auto& Field : Row.GetEntries())
							if (!IsDeclaredRootField(Field.Key))
							{
								auto Diagnostic = HjsonNodeFailure(TEXT("keyed_item_root_field_unknown"),
									FString::Printf(TEXT("Root field '%s' is not declared by this collection contract."), *Field.Key),
									TEXT("workspace_resolve"), Field.Value.Get(), nullptr, false);
								Diagnostic.ActualValue = MakeShared<FJsonValueString>(Field.Key);
								return FailAtNode(OutError, MoveTemp(Diagnostic), Field.Value.Get());
							}
					if (!Contract.RecordSchemaField.IsEmpty())
					{
						const FHjsonNode* CapturedSchema = CapturedValues->Get(Contract.RecordSchemaField);
						const FHjsonNode* VisibleSchema = Row.Get(Contract.RecordSchemaField);
						FUeAgentCanonicalValuePtr VisibleSchemaValue;
						FUeAgentSemanticComparison SchemaComparison;
						FUeAgentStateSemanticError SchemaError;
						if (!CapturedSchema) return FailCapturedField(OutError, TEXT("keyed_record_schema_read_only"),
							TEXT("The record schema has no captured baseline; export the workspace again."), *CapturedValues, Contract.RecordSchemaField);
						if (!VisibleSchema) return FailField(OutError, TEXT("keyed_record_schema_read_only"),
							TEXT("The required read-only record schema was removed."), Row, Contract.RecordSchemaField);
						if (!TryCanonicalValue(*CapturedSchema, Resources, RecordSchema, OutError)
							|| !TryCanonicalValue(*VisibleSchema, Resources, VisibleSchemaValue, OutError)) return false;
						if (!TryCompareValues(RecordSchema.ToSharedRef(), VisibleSchemaValue.ToSharedRef(),
							VisibleSchema->GetProvenance().GetJsonPointer(), Resources, SchemaComparison, SchemaError))
							return FailAtNode(OutError, MoveTemp(SchemaError), *VisibleSchema);
						if (!SchemaComparison.IsEquivalent())
							return FailAtNode(OutError, MakeComparisonDiagnostic(SchemaComparison, TEXT("keyed_record_schema_read_only"),
								TEXT("The record schema is read-only and differs from its captured value."), TEXT("workspace_resolve")), *VisibleSchema);
					}
					for (const auto& Field : CapturedValues->GetEntries())
					{
						if (IsCollectionRoot(Field.Key)) continue;
						FUeAgentCanonicalValuePtr Value;
						if (!TryCanonicalValue(*Field.Value, Resources, Value, OutError, nullptr, Contract.bInterpretSemanticValues)) return false;
						if (Contract.IsReadOnlyRootField(Field.Key))
						{
							const FHjsonNode* VisibleField = Row.Get(Field.Key);
							FUeAgentCanonicalValuePtr VisibleValue;
							FUeAgentSemanticComparison Comparison;
							FUeAgentStateSemanticError Error;
							if (!VisibleField)
								return FailField(OutError, TEXT("keyed_item_root_field_read_only"),
									FString::Printf(TEXT("Required read-only root field '%s' was removed."), *Field.Key), Row, Field.Key);
							if (!TryCanonicalValue(*VisibleField, Resources, VisibleValue, OutError, nullptr, Contract.bInterpretSemanticValues)) return false;
							if (!TryCompareValues(Value.ToSharedRef(), VisibleValue.ToSharedRef(),
								VisibleField->GetProvenance().GetJsonPointer(), Resources, Comparison, Error))
							{
								return FailAtNode(OutError, MoveTemp(Error), *VisibleField);
							}
							if (!Comparison.IsEquivalent())
								return FailAtNode(OutError, MakeComparisonDiagnostic(Comparison, TEXT("keyed_item_root_field_read_only"),
									TEXT("A read-only root field differs from its captured value."), TEXT("workspace_resolve")), *VisibleField);
						}
						if (Contract.IsEditableRootField(Field.Key))
						{
							if (const FHjsonNode* VisibleField = Row.Get(Field.Key))
							{
								FUeAgentCanonicalValuePtr VisibleValue;
								FUeAgentSemanticComparison Comparison;
								FUeAgentStateSemanticError Error;
								if (!TryCanonicalValue(*VisibleField, Resources, VisibleValue, OutError, nullptr, Contract.bInterpretSemanticValues)) return false;
								if (!TryCompareValues(Value.ToSharedRef(), VisibleValue.ToSharedRef(),
									VisibleField->GetProvenance().GetJsonPointer(), Resources, Comparison, Error))
								{
									return FailAtNode(OutError, MoveTemp(Error), *VisibleField);
								}
								bRootChanged |= !Comparison.IsEquivalent();
								Value = MoveTemp(VisibleValue);
							}
						}
						Values.Add(Field.Key, Value.ToSharedRef());
					}
					if (!TryCanonicalValue(*CapturedItems, Resources, Captured, OutError, nullptr, Contract.bInterpretSemanticValues)) return false;
					// Absent fields retain captured state. Only an explicitly supplied
					// collection participates in desired-state item removal/creation.
					if (!VisibleItems) Resolved = Captured;
					else if (VisibleItems == nullptr || !TryCanonicalValue(
							*VisibleItems, Resources, Visible, OutError,
							Contract.bInterpretSemanticValues ? &PublicRefToUnitId : nullptr, Contract.bInterpretSemanticValues)
						|| !TryResolveKeyedItemDesiredState(
							*Visible, *Captured, Row.GetProvenance().GetSourceUnit(),
							Contract, RecordSchema.Get(), Resources, Resolved, bSemanticChanged, OutError, nullptr,
							EKeyedItemInputKind::VisibleDesiredState, nullptr, VisibleItems))
					{
						return false;
					}
					Values.Add(Contract.RootField, Resolved.ToSharedRef());
					if (VisibleItems && (!Contract.bIdentityFromMapKey || Contract.bEmitOperationRecords))
						OutResolvedKeyedOperationRoots.Add(Contract.RootField);
					OutManagedSemanticChanged = OutManagedSemanticChanged.Get(false) || bSemanticChanged || bRootChanged;
					return true;
					};
				FWorkspaceError CollectionErrors;
				for (const auto* Contract : KeyedItemContracts)
				{
					FWorkspaceError Error;
					if (!ResolveCollection(*Contract, Error))
					{
						const FHjsonNode* Source = Row.Get(Error.GetSourceUnit());
						if (!Source) Source = Row.Get(Contract->RootField);
						Error.Locate(Source ? Source->GetProvenance() : Row.GetProvenance());
						CollectionErrors.Append(Error);
					}
				}
				if (CollectionErrors.HasErrors()) { OutError = MoveTemp(CollectionErrors); return false; }
			}
			else if (bActorComponentItemProjection)
			{
				if (CapturedValues == nullptr
					|| CapturedValues->GetKind() != EHjsonNodeKind::Object)
				{
					return FailCapturedField(OutError, TEXT("semantic_workspace_captured_values_missing"),
						TEXT("The actor or component has no captured values object; export the workspace again."), Envelope ? *Envelope : Row,
						TEXT("captured_values"), MakeShared<FJsonValueString>(TEXT("object")));
				}
				FUeAgentCanonicalValuePtr Captured;
				FUeAgentCanonicalValuePtr Visible;
				FUeAgentCanonicalValuePtr Resolved;
				bool bSemanticChanged = false;
				if (!TryCanonicalValue(*CapturedValues, Resources, Captured, OutError)
					|| !TryCanonicalValue(
						Row, Resources, Visible, OutError,
						&PublicRefToUnitId)
					|| !TryResolveCapturedItemDesiredState(
						*Visible, *Captured, Row.GetProvenance().GetSourceUnit(),
						GetActorComponentHierarchyItemVisibleFieldPolicy(),
						Resources, Resolved, bSemanticChanged, OutError))
				{
					return false;
				}
				for (const auto& Field : Resolved->GetObjectItems())
				{
					Values.Add(Field.Key, Field.Value);
				}
				OutManagedSemanticChanged = bSemanticChanged;
			}
			else if (bKeyedProperty)
			{
				const FHjsonNode* CapturedType =
					Envelope == nullptr ? nullptr : Envelope->Get(TEXT("captured_type"));
				if (CapturedType == nullptr
					|| CapturedType->GetKind() != EHjsonNodeKind::String)
				{
					return FailCapturedField(OutError, TEXT("semantic_workspace_property_type_missing"),
						TEXT("The property has no captured type string; export the workspace again."), Envelope ? *Envelope : Row,
						TEXT("captured_type"), MakeShared<FJsonValueString>(TEXT("string")));
				}
				if (CapturedValues == nullptr
					|| CapturedValues->GetKind() != EHjsonNodeKind::Object)
				{
					return FailCapturedField(OutError, TEXT("semantic_workspace_captured_values_missing"),
						TEXT("The property has no captured values object; export the workspace again."), Envelope ? *Envelope : Row,
						TEXT("captured_values"), MakeShared<FJsonValueString>(TEXT("object")));
				}
				TArray<FString> TemplateFields;
				for (const auto& Field : CapturedValues->GetEntries())
					TemplateFields.Add(Field.Key);
				if (!TryResolveKeyedPropertyValues(Row, CapturedType->GetText(), TemplateFields,
					Resources, Values, OutError, &PublicRefToUnitId, &OutValueSources)) return false;
				// The optional catalog reference cannot select a different property's schema.
				const FHjsonNode* CapturedDefinition = Envelope->Get(TEXT("captured_definition"));
				if (Definition && (Definition->GetKind() != EHjsonNodeKind::String
					|| (CapturedDefinition && Definition->GetText() != CapturedDefinition->GetText())))
					return FailNode(OutError, TEXT("semantic_workspace_property_definition_mismatch"),
						TEXT("The authored property definition differs from the captured definition."), *Definition,
						CapturedDefinition ? HjsonDiagnosticValue(CapturedDefinition) : TSharedPtr<FJsonValue>());
			}
			if (!bDocumentCollectionProjection && !bGraphProjection && !bKeyedItemProjection
				&& !bActorComponentItemProjection
				&& !bKeyedProperty)
			{
				for (const TPair<FString, FHjsonNodeRef>& Item : ValuesNode->GetEntries())
				{
					FUeAgentCanonicalValuePtr Value;
					if (!TryCanonicalValue(
							Item.Value.Get(), Resources, Value, OutError,
							&PublicRefToUnitId))
					{
						return false;
					}
					if (Values.Contains(Item.Key))
					{
						auto Diagnostic = HjsonNodeFailure(TEXT("semantic_workspace_unit_values_invalid"),
							TEXT("The semantic values contain a duplicate field name."), TEXT("workspace_resolve"), Item.Value.Get());
						Diagnostic.ActualValue = MakeShared<FJsonValueString>(Item.Key);
						return FailAtNode(OutError, MoveTemp(Diagnostic), Item.Value.Get());
					}
					Values.Add(Item.Key, Value.ToSharedRef());
				}
			}
			TArray<FString> ItemRefs;
			TArray<FString> RelationRefs;
			TArray<FString> DependencyRefs;
			TArray<FString> ProfileVisibility;
			if (!ReadResolvedArray(TEXT("item_refs"), ItemRefs)
				|| !ReadResolvedArray(TEXT("relation_refs"), RelationRefs)
				|| !ReadResolvedArray(TEXT("dependency_refs"), DependencyRefs)
				|| !ReadResolvedArray(TEXT("profile_visibility"), ProfileVisibility))
			{
				return false;
			}
			auto ResolvePublicRefs = [&PublicRefToUnitId](TArray<FString>& Values)
			{
				for (FString& Value : Values)
				{
					if (const FString* Internal = PublicRefToUnitId.Find(Value))
					{
						Value = *Internal;
					}
				}
			};
			ResolvePublicRefs(ItemRefs);
			ResolvePublicRefs(RelationRefs);
			ResolvePublicRefs(DependencyRefs);
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
				auto Diagnostic = OutUnit->GetValidationDiagnostic();
				OutUnit.Reset();
				return FailAtNode(OutError, MoveTemp(Diagnostic), Row);
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
			FWorkspaceError& OutError, FString* OutContentHash = nullptr)
		{
			if (IsReparsePoint(Path))
			{
				return Fail(
					OutError,
					TEXT(
						"workspace_reparse_point_rejected"), TEXT("The document or one of its ancestor directories is a reparse point."),
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
					TEXT("workspace_source_read_failed"), TEXT("The semantic workspace document could not be read."),
					SourceUnit);
			}
			FString CachePath;
			if (OutContentHash) *OutContentHash = Sha256Text(Text);
			if (ActiveReadState)
			{
				CachePath = FPaths::ConvertRelativePathToFull(Path);
				FPaths::NormalizeFilename(CachePath);
				const auto* Cached = ActiveReadState->Documents.Find(CachePath);
				if (Cached && Cached->SourceUnit.Equals(SourceUnit, ESearchCase::CaseSensitive)
					&& (SourceUnit != ManifestPath || Cached->Parsed->GetRoot()->Get(TEXT("unit_inventory"))
						|| Cached->Parsed->GetRoot()->Get(TEXT("unit_inventory_files")))
					&& Cached->Text.Equals(Text, ESearchCase::CaseSensitive))
				{
					OutDocument = Cached->Parsed;
					OutError.Reset();
					return true;
				}
			}
			const auto BeforeParse = Resources.Snapshot();
			TMap<EUeAgentResourceDimension, int64> ParseCharges;
			bool bExactParseCharges = true;
			const auto* PreviousDocument = ActiveReadState ? PreviousDocumentReads.Find(CachePath) : nullptr;
			const bool bReuseDocument = PreviousDocument && PreviousDocument->SourceUnit == SourceUnit
				&& PreviousDocument->ContentHash == Sha256Text(Text);
			if (bReuseDocument) {
				++ActiveReadState->ReusedPublicSyntax;
				FUeAgentResourceChargeError ResourceError;
				if (!Resources.TryChargeMany(PreviousDocument->ParseCharges, TEXT("semantic_workspace:actual_public_document"), ResourceError))
					return FailResource(OutError, ResourceError);
				OutDocument = PreviousDocument->Parsed;
				ParseCharges = PreviousDocument->ParseCharges;
			} else if (!TryParseHjsonDocument(
				Text,
				SourceUnit,
				Resources,
				OutDocument,
				OutError)) return false;
			if (!bReuseDocument) {
				const auto After = Resources.Snapshot();
				for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D) {
					const auto Dimension = static_cast<EUeAgentResourceDimension>(D);
					const int64 A = BeforeParse.Consumed(Dimension), B = After.Consumed(Dimension);
					if (B == MAX_int64 || B < A) { bExactParseCharges = false; break; }
					if (B > A) ParseCharges.Add(Dimension, B - A);
				}
			}
			if (ActiveReadState)
			{
				auto& Cached = ActiveReadState->Documents.FindOrAdd(CachePath);
				Cached.SourceUnit = SourceUnit;
				Cached.Text = MoveTemp(Text);
				Cached.Parsed = OutDocument;
				Cached.ParseCharges = MoveTemp(ParseCharges);
				Cached.bExactParseCharges = bExactParseCharges;
			}
			return true;
		}

		bool AddInventoryIdentity(const FInventoryRow& Row, TSet<FString>& UnitIds,
			TSet<FString>& PublicRefs, TSet<FString>& LogicalPaths, FWorkspaceError& OutError)
		{
			if (UnitIds.Contains(Row.UnitId))
				return FailField(OutError, TEXT("semantic_workspace_unit_duplicate"),
					TEXT("The workspace inventory contains a duplicate unit identity."), *Row.SourceNode, TEXT("unit_id"),
					MakeShared<FJsonValueString>(TEXT("unique unit identity")), false);
			if (PublicRefs.Contains(Row.PublicRef))
				return FailField(OutError, TEXT("semantic_workspace_public_ref_duplicate"),
					TEXT("The workspace inventory contains a duplicate public reference."), *Row.SourceNode, TEXT("public_ref"),
					MakeShared<FJsonValueString>(TEXT("unique public reference")));
			if (LogicalPaths.Contains(Row.LogicalPath))
				return FailField(OutError, TEXT("semantic_workspace_inventory_path_duplicate"),
					TEXT("The workspace inventory contains a duplicate logical path."), *Row.SourceNode, TEXT("logical_path"),
					MakeShared<FJsonValueString>(TEXT("unique logical path")));
			UnitIds.Add(Row.UnitId); PublicRefs.Add(Row.PublicRef); LogicalPaths.Add(Row.LogicalPath);
			return true;
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
				return FailField(OutError, TEXT("semantic_workspace_inventory_invalid"),
					TEXT("Workspace metadata requires a unit_inventory array."), Manifest, TEXT("unit_inventory"), MakeShared<FJsonValueString>(TEXT("array")), false);
			}
			FWorkspaceError Diagnostics;
			TSet<FString> UnitIds;
			TSet<FString> PublicRefs;
			TSet<FString> LogicalPaths;
			for (const FHjsonNodeRef& Item :
				Inventory->GetItems())
			{
				OutError.Reset();
				auto ReadItem = [&]() -> bool
				{
					if (Item->GetKind()
						!= EHjsonNodeKind::Object)
					{
						return FailNode(OutError, TEXT("semantic_workspace_inventory_invalid"),
							TEXT("Each workspace inventory entry must be an object."), Item.Get(), MakeShared<FJsonValueString>(TEXT("object")), false);
					}
					FInventoryRow Row;
					Row.SourceNode = &Item.Get();
					FString KindText;
					if (!RequiredString(
							Item.Get(),
							TEXT("unit_id"),
							Row.UnitId,
							OutError)
						|| !RequiredString(
							Item.Get(),
							TEXT("public_ref"),
							Row.PublicRef,
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
						|| !OptionalString(
							Item.Get(),
							TEXT("document_path"),
							Row.DocumentPath,
							OutError)
						|| !OptionalString(
							Item.Get(),
							TEXT("unit_pointer"),
							Row.UnitPointer,
							OutError)
						|| !RequiredString(
							Item.Get(),
							TEXT("content_hash"),
							Row.ContentHash,
							OutError)
						|| !OptionalString(
							Item.Get(),
							TEXT("edit_hash"),
							Row.EditHash,
							OutError))
					{
						return false;
					}
					if (Row.DocumentPath.IsEmpty())
					{
						Row.DocumentPath = Row.LogicalPath;
					}
					if (Row.DocumentPath.IsEmpty())
					{
						return FailField(OutError, TEXT("semantic_workspace_inventory_invalid"),
							TEXT("The inventory entry has neither a document path nor a logical path."), Item.Get(),
							TEXT("document_path"), MakeShared<FJsonValueString>(TEXT("non-empty logical document path")));
					}
					if (Row.UnitPointer.IsEmpty())
					{
						Row.UnitPointer = TEXT("/unit");
					}
					if (const FHjsonNode* ContentFiles = Item->Get(TEXT("content_files")))
					{
						if (ContentFiles->GetKind() != EHjsonNodeKind::Array)
							return FailNode(OutError, TEXT("semantic_workspace_content_files_invalid"),
								TEXT("The inventory content_files field must be an array."), *ContentFiles, MakeShared<FJsonValueString>(TEXT("array")), false);
						for (const auto& File : ContentFiles->GetItems())
						{
							if (File->GetKind() != EHjsonNodeKind::String)
								return FailNode(OutError, TEXT("semantic_workspace_content_files_invalid"),
									TEXT("Each inventory content_files entry must be a string."), File.Get(), MakeShared<FJsonValueString>(TEXT("string")), false);
							Row.ContentFiles.Add(File->GetText());
						}
					}
					Row.Envelope = Item->Get(TEXT("envelope"));
					if (!OptionalString(Item.Get(), TEXT("envelope_ref"), Row.EnvelopeRef, OutError)) return false;
					if (!Row.EnvelopeRef.IsEmpty())
					{
						FString Safe;
						if (Row.Envelope || !SafeRelativePath(Row.EnvelopeRef, Safe)
							|| !Safe.StartsWith(TEXT(".uai/baselines/"), ESearchCase::CaseSensitive)
							|| !Safe.EndsWith(TEXT(".hjson"), ESearchCase::CaseSensitive))
							return FailField(OutError, TEXT("semantic_workspace_envelope_ref_invalid"),
								TEXT("A captured envelope must be inline or reference a managed baseline document, exclusively."),
								Item.Get(), TEXT("envelope_ref"));
						Row.EnvelopeRef = MoveTemp(Safe);
					}
					if (Row.Envelope != nullptr
						&& Row.Envelope->GetKind() != EHjsonNodeKind::Object)
					{
						return FailNode(OutError, TEXT("semantic_workspace_inventory_invalid"),
							TEXT("The captured inventory envelope must be an object."), *Row.Envelope, MakeShared<FJsonValueString>(TEXT("object")), false);
					}
					if (!ParseUnitKind(KindText, Row.UnitKind))
					{
						return FailField(OutError, TEXT("semantic_workspace_inventory_invalid"),
							TEXT("The inventory unit_kind is unsupported."), Item.Get(), TEXT("unit_kind"), UnitKindChoices());
					}
					if (!AddInventoryIdentity(Row, UnitIds, PublicRefs, LogicalPaths, OutError)) return false;
					OutRows.Add(MoveTemp(Row));
					return true;
				};
				if (!ReadItem()) Diagnostics.Append(OutError);
			}
			OutError = MoveTemp(Diagnostics);
			return !OutError.HasErrors();
		}

		const FHjsonNode* FindEntry(
			const FHjsonNode& Object,
			const FString& Key)
		{
			return Object.GetKind() == EHjsonNodeKind::Object
				? Object.Get(Key)
				: nullptr;
		}

		bool MaterializeInventoryRow(FInventoryRow& Row, const FManifestIndexProof& Proof,
			FUeAgentResourceTracker& Resources, FWorkspaceError& OutError)
		{
			if (!Row.EnvelopeRef.IsEmpty()) return true; // The complete thin row is already retained.
			const FSourceProvenance Origin = Row.SourceNode->GetProvenance();
			const int64 Start = Origin.GetByteStart(), End = Origin.GetByteEnd();
			if (Start < 0 || End <= Start || End > Proof.Utf8.Num()) return false;
			FString Text;
			FFileHelper::BufferToString(Text, Proof.Utf8.GetData() + Start, static_cast<int32>(End - Start));
			FHjsonDocumentPtr Document;
			if (!TryParseHjsonDocument(Text, ManifestPath, Resources, Document, OutError)) return false;
			// Restore exact source locations after parsing just this original span.
			// Use an iterative copy so nesting cannot turn this optimization into a
			// recursive stack limit absent from the normal parser.
			TArray<TPair<const FHjsonNode*, bool>> Stack;
			TMap<const FHjsonNode*, FHjsonNodeRef> Rebased;
			Stack.Emplace(&Document->GetRoot().Get(), false);
			while (!Stack.IsEmpty())
			{
				const auto Task = Stack.Pop(EAllowShrinking::No);
				const FHjsonNode& Node = *Task.Key;
				if (!Task.Value)
				{
					Stack.Emplace(Task.Key, true);
					for (const auto& Child : Node.GetItems()) Stack.Emplace(&Child.Get(), false);
					for (const auto& Child : Node.GetEntries()) Stack.Emplace(&Child.Value.Get(), false);
					continue;
				}
				TArray<FHjsonNodeRef> Items;
				TArray<TPair<FString, FHjsonNodeRef>> Entries;
				for (const auto& Child : Node.GetItems()) Items.Add(Rebased.FindChecked(&Child.Get()));
				for (const auto& Child : Node.GetEntries()) Entries.Emplace(Child.Key, Rebased.FindChecked(&Child.Value.Get()));
				const auto& Location = Node.GetProvenance();
				if (!Charge(Resources, {{EUeAgentResourceDimension::WorkUnits, 1},
					{EUeAgentResourceDimension::EstimatedMemoryBytes, sizeof(FHjsonNode)}},
					TEXT("semantic_workspace:row_source"), OutError)) return false;
				const FString Pointer = Location.GetJsonPointer() == TEXT("/")
					? Origin.GetJsonPointer() : Origin.GetJsonPointer() + Location.GetJsonPointer();
				FSourceProvenance Source(Origin.GetSourceUnit(), Pointer,
					Origin.GetLine() + Location.GetLine() - 1,
					Location.GetLine() == 1 ? Origin.GetColumn() + Location.GetColumn() - 1 : Location.GetColumn(),
					Start + Location.GetByteStart(), Start + Location.GetByteEnd(), Location.GetOriginalKey());
				Rebased.Add(Task.Key, MakeShared<FHjsonNode>(Node.GetKind(), MoveTemp(Source),
					Node.GetLexeme(), Node.GetText(), Node.GetBoolean(), MoveTemp(Items), MoveTemp(Entries)));
			}
			Row.RetainedSourceNode = Rebased.FindChecked(&Document->GetRoot().Get());
			Row.SourceNode = Row.RetainedSourceNode.Get();
			Row.Envelope = Row.SourceNode->Get(TEXT("envelope"));
			return true;
		}

		TSharedPtr<const FInventoryDocumentProof> MakeInventoryDocumentProof(const FString& Root,
			const FString& Path, const FString& ContentHash, const FHjsonDocument& Document,
			const TArray<uint8>& Utf8, const TMap<EUeAgentResourceDimension, int64>& ParseCharges,
			FWorkspaceError& OutError)
		{
			auto Proof = MakeShared<FInventoryDocumentProof>();
			if (!ParseInventory(*Document.GetRoot(), Proof->Rows, OutError)) return {};
			Proof->ContentHash = ContentHash; Proof->ParseCharges = ParseCharges;
			const auto& Nodes = Document.GetRoot()->Get(TEXT("unit_inventory"))->GetItems();
			for (int32 Index = 0; Index < Proof->Rows.Num(); ++Index) {
				auto& Row = Proof->Rows[Index];
				const auto& Span = Row.SourceNode->GetProvenance();
				if (Span.GetByteStart() < 0 || Span.GetByteEnd() <= Span.GetByteStart() || Span.GetByteEnd() > Utf8.Num()) {
					Fail(OutError, TEXT("semantic_workspace_inventory_source_invalid"),
						TEXT("The parsed inventory row has no valid source byte range."), Path); return {};
				}
				Row.SourceRowHash = Sha256(Utf8.GetData() + Span.GetByteStart(), Span.GetByteEnd() - Span.GetByteStart());
				Row.RetainedSourceNode = Nodes[Index];
			}
			if (InventoryProofRoot != Root) { InventoryProofs.Reset(); InventoryProofRoot = Root; PreviousManifestIndex.Reset(); }
			int64 Rows = Proof->Rows.Num();
			for (const auto& Existing : InventoryProofs) if (Existing.Key != Path) Rows += Existing.Value->Rows.Num();
			if (Rows > 200000 || (!InventoryProofs.Contains(Path) && InventoryProofs.Num() >= 256)) InventoryProofs.Reset();
			if (Proof->Rows.Num() <= 200000) InventoryProofs.Add(Path, Proof);
			return Proof;
		}

		bool ReadInventoryDocuments(const FString& Root, const FHjsonNode& Manifest,
			FUeAgentResourceTracker& Resources, FParsedSource& Parsed, TArray<FInventoryRow>& Rows,
			FWorkspaceError& OutError)
		{
			const auto* Files = Manifest.Get(TEXT("unit_inventory_files"));
			if (!Files || Files->GetKind() != EHjsonNodeKind::Array || Manifest.Get(TEXT("unit_inventory")))
				return FailField(OutError, TEXT("semantic_workspace_inventory_invalid"),
					TEXT("Workspace metadata requires inline unit_inventory or unit_inventory_files, exclusively."), Manifest, TEXT("unit_inventory_files"));
			TSet<FString> Paths, UnitIds, PublicRefs, LogicalPaths;
			int32 ReusedFiles = 0;
			for (const auto& File : Files->GetItems()) {
				FString Path;
				if (File->GetKind() != EHjsonNodeKind::String || !SafeRelativePath(File->GetText(), Path)
					|| !Path.StartsWith(TEXT(".uai/inventory/"), ESearchCase::CaseSensitive)
					|| !Path.EndsWith(TEXT(".hjson"), ESearchCase::CaseSensitive) || Paths.Contains(Path))
					return FailNode(OutError, TEXT("semantic_workspace_inventory_ref_invalid"),
						TEXT("Inventory files must be unique managed relative paths under .uai/inventory/."), *File);
				Paths.Add(Path);
				const FString FullPath = FPaths::Combine(Root, Path);
				if (IsReparsePoint(FullPath)) return Fail(OutError, TEXT("workspace_reparse_point_rejected"),
					TEXT("The inventory document or an ancestor is a reparse point."), Path);
				FString Text;
				if (!FFileHelper::LoadFileToString(Text, *FullPath)) return Fail(OutError,
					TEXT("workspace_source_read_failed"), TEXT("The inventory document could not be read."), Path);
				const FString Hash = Sha256Text(Text);
				auto Proof = InventoryProofRoot == Root ? InventoryProofs.FindRef(Path) : nullptr;
				if (Proof && Proof->ContentHash == Hash) {
					if (!Charge(Resources, {{EUeAgentResourceDimension::Files, 1}}, TEXT("semantic_workspace:") + Path, OutError)) return false;
					FUeAgentResourceChargeError Error;
					if (!Resources.TryChargeMany(Proof->ParseCharges, TEXT("semantic_workspace:validated_inventory"), Error)) return FailResource(OutError, Error);
					++ReusedFiles;
				} else {
					const auto Before = Resources.Snapshot();
					FHjsonDocumentPtr Document; FString ParsedHash;
					if (!ReadDocument(FullPath, Path, Resources, Document, OutError, &ParsedHash)) return false;
					if (ParsedHash != Hash) return Fail(OutError, TEXT("workspace_source_changed_during_read"),
						TEXT("The inventory document changed while this workspace was being resolved."), Path);
					TMap<EUeAgentResourceDimension, int64> Charges;
					const auto After = Resources.Snapshot();
					for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D) {
						const auto Dimension = static_cast<EUeAgentResourceDimension>(D);
						const int64 Delta = After.Consumed(Dimension) - Before.Consumed(Dimension);
						if (Dimension != EUeAgentResourceDimension::Files && Delta > 0) Charges.Add(Dimension, Delta);
					}
					const FTCHARToUTF8 Converted(*Text); TArray<uint8> Utf8;
					Utf8.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
					Proof = MakeInventoryDocumentProof(Root, Path, Hash, *Document, Utf8, Charges, OutError);
					if (!Proof) return false;
				}
				for (const auto& Row : Proof->Rows) {
					if (!AddInventoryIdentity(Row, UnitIds, PublicRefs, LogicalPaths, OutError)) return false;
					Rows.Add(Row);
				}
				Parsed.ManagedPaths.Add(Path);
				Parsed.InventoryHashes.Add(Path, Hash);
				if (ActiveReadState) ActiveReadState->HashOnlyDocuments.Add(FullPath, Hash);
			}
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=inventory_documents files=%d reused_syntax=%d rows=%d"),
				Paths.Num(), ReusedFiles, Rows.Num());
			return true;
		}

		bool ReadDirectorySource(
			const FWorkspaceSource& Source,
			FUeAgentResourceTracker& Resources,
			FParsedSource& OutParsed,
			TArray<FInventoryRow>& OutInventory,
			FWorkspaceError& OutError,
			const TOptional<ESemanticWorkspaceMode> ExpectedMode = {},
			const bool bAllowManagedUnitEdits = false)
		{
			const double DirectoryStarted = FPlatformTime::Seconds();
			FString Root =
				FPaths::ConvertRelativePathToFull(Source.GetPath());
			FPaths::NormalizeDirectoryName(Root);
			if (!IFileManager::Get().DirectoryExists(*Root))
			{
				return Fail(
					OutError,
					TEXT(
						"workspace_source_directory_missing"), TEXT("The semantic workspace directory does not exist."),
					Root);
			}
			if (IsReparsePoint(Root))
			{
				return Fail(
					OutError,
					TEXT(
						"workspace_reparse_point_rejected"), TEXT("The semantic workspace directory is a reparse point."),
					Root);
			}
			const FString FullManifest =
				FPaths::Combine(Root, ManifestPath);
			if (!IFileManager::Get().FileExists(*FullManifest))
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_missing"), TEXT("The directory has no semantic workspace metadata; export the workspace again."),
					ManifestPath);
			}
			TSharedPtr<const FManifestIndexProof> ManifestIndex;
			if (ExpectedMode.IsSet() && ActiveReadState && PreviousManifestIndex && PreviousResolvedRows
				&& PreviousManifestIndex->Root == Root && PreviousResolvedRows->Root == Root
				&& PreviousResolvedRows->Mode == ExpectedMode.GetValue()
				&& PreviousResolvedRows->bManagedEdits == bAllowManagedUnitEdits
				&& PreviousResolvedRows->ReferenceMapHash == PreviousManifestIndex->ReferenceMapHash
				&& !IsReparsePoint(FullManifest))
			{
				TArray<uint8> ActualBytes;
				if (FFileHelper::LoadFileToArray(ActualBytes, *FullManifest)
					&& Sha256(ActualBytes) == PreviousManifestIndex->ContentHash)
				{
					if (!Charge(Resources, {{EUeAgentResourceDimension::Files, 1}}, TEXT("semantic_workspace:manifest"), OutError)) return false;
					FUeAgentResourceChargeError ResourceError;
					if (!Resources.TryChargeMany(PreviousManifestIndex->ParseCharges, TEXT("semantic_workspace:validated_manifest"), ResourceError))
						return FailResource(OutError, ResourceError);
					ManifestIndex = PreviousManifestIndex;
					OutParsed.ManifestDocument = ManifestIndex->Header;
					auto& Cached = ActiveReadState->Documents.FindOrAdd(FullManifest);
					Cached.SourceUnit = ManifestPath;
					FFileHelper::BufferToString(Cached.Text, ActualBytes.GetData(), ActualBytes.Num());
					Cached.Parsed = OutParsed.ManifestDocument;
					OutInventory = ManifestIndex->Inventory;
				}
			}
			if (!ManifestIndex && (!ReadDocument(
					FullManifest,
					ManifestPath,
					Resources,
					OutParsed.ManifestDocument,
					OutError)
				|| !OutParsed.ManifestDocument.IsValid()))
			{
				return false;
			}
			OutParsed.Manifest =
				&OutParsed.ManifestDocument->GetRoot().Get();
			FWorkspaceError Diagnostics;
			if (OutParsed.Manifest->GetKind() != EHjsonNodeKind::Object)
				return Fail(OutError, TEXT("semantic_workspace_inventory_invalid"), TEXT("The workspace metadata root must be an object."), ManifestPath);
			const bool bInventoryDocuments = OutParsed.Manifest->Get(TEXT("unit_inventory_files")) != nullptr;
			if (!ManifestIndex && !(bInventoryDocuments
				? ReadInventoryDocuments(Root, *OutParsed.Manifest, Resources, OutParsed, OutInventory, OutError)
				: ParseInventory(*OutParsed.Manifest, OutInventory, OutError)))
			{
				Diagnostics.Append(OutError);
				Diagnostics.MarkUnchecked(TEXT("inventory_units"), FString(), TEXT("invalid_inventory_rows"));
			}
			const bool bReusePriorRows = ManifestIndex || (bInventoryDocuments && ExpectedMode.IsSet() && ActiveReadState
				&& PreviousResolvedRows && PreviousResolvedRows->Root == Root
				&& PreviousResolvedRows->Mode == ExpectedMode.GetValue()
				&& PreviousResolvedRows->bManagedEdits == bAllowManagedUnitEdits
				&& PreviousResolvedRows->ReferenceMapHash == HashReferenceMap(OutInventory));
			const double InventoryReadFinished = FPlatformTime::Seconds();

			TSet<FString> ManagedDocumentPaths;
			TSet<FString> DocumentPointers;
			TMap<FString, FHjsonDocumentPtr> Documents;
			TMap<FString, FHjsonDocumentPtr> ContentDocuments;
			TMap<FString, bool> DocumentExistence;
			TSet<FString> FailedDocuments;
			TSet<FString> UniqueSourcePaths;
			for (const FInventoryRow& Row : OutInventory)
			{
				UniqueSourcePaths.Add(Row.DocumentPath);
			}
			TArray<FString> SourcePaths = UniqueSourcePaths.Array();
			SourcePaths.Sort();
			for (const FString& Path : SourcePaths)
			{
				FString SafePath;
				FWorkspaceError Error;
				FHjsonDocumentPtr Document;
				if (!SafeRelativePath(Path, SafePath))
				{
					Fail(Error, TEXT("workspace_path_invalid"), TEXT("A managed document path must be relative and remain inside the workspace."),
						FString(), TEXT("/"), MakeShared<FJsonValueString>(Path));
					Diagnostics.Append(Error);
					continue;
				}
				if (!ReadDocument(FPaths::Combine(Root, SafePath), SafePath, Resources, Document, Error))
				{
					Diagnostics.Append(Error);
					FailedDocuments.Add(SafePath);
					continue;
				}
				Documents.Add(SafePath, Document);
				ContentDocuments.Add(SafePath, Document);
				OutParsed.UnitDocuments.Add(Document);
			}

			OutParsed.UnitDocuments.Reserve(
				OutInventory.Num());
			const double DocumentsReadFinished = FPlatformTime::Seconds();
			double BaselineParseSeconds = 0, BaselineReadSeconds = 0;
			double RowPathsSeconds = 0, RowExistsSeconds = 0, RowPointerSeconds = 0, RowProofSeconds = 0;
			OutParsed.UnitRows.Reserve(OutInventory.Num());
			TMap<FString, FString> CurrentDocumentHashes;
			TMap<FString, FHjsonDocumentPtr> BaselineDocuments;
			auto LoadBaseline = [&](const FString& Path) -> bool
			{
				if (BaselineDocuments.Contains(Path)) return true;
				const double Started = FPlatformTime::Seconds();
				FHjsonDocumentPtr Document;
				FString ParsedHash;
				const auto Before = Resources.Snapshot();
				if (!ReadDocument(FPaths::Combine(Root, Path), Path, Resources, Document, OutError, &ParsedHash)) return false;
				if (ParsedHash != OutParsed.BaselineProofs.FindChecked(Path).ContentHash)
					return Fail(OutError, TEXT("workspace_source_changed"),
						TEXT("The captured baseline document changed while this workspace was being resolved."), Path);
				if (Document->GetRoot()->GetKind() != EHjsonNodeKind::Object)
					return FailNode(OutError, TEXT("semantic_workspace_envelope_ref_invalid"),
						TEXT("A baseline document must contain envelopes keyed by unit identity."), *Document->GetRoot(),
						MakeShared<FJsonValueString>(TEXT("object")), false);
				auto& Proof = OutParsed.BaselineProofs.FindChecked(Path);
				Proof.Charges.Reset(); Proof.bExact = true;
				const auto After = Resources.Snapshot();
				for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D)
				{
					const auto Dimension = static_cast<EUeAgentResourceDimension>(D);
					const int64 A = Before.Consumed(Dimension), B = After.Consumed(Dimension);
					if (B == MAX_int64 || B < A) { Proof.bExact = false; break; }
					if (B > A) Proof.Charges.Add(Dimension, B - A);
				}
				BaselineDocuments.Add(Path, Document);
				OutParsed.UnitDocuments.Add(Document);
				BaselineParseSeconds += FPlatformTime::Seconds() - Started;
				return true;
			};
			int32 ReusedManifestRows = 0;
			for (FInventoryRow& Row : OutInventory)
			{
				OutError.Reset();
				auto ReadRow = [&]() -> bool
				{
					const double PathsStarted = FPlatformTime::Seconds();
					double ThisBaselineRead = 0;
					FString Relative;
					if (!SafeRelativePath(
							Row.DocumentPath,
							Relative))
					{
						const auto* DocumentPath = Row.SourceNode->Get(TEXT("document_path"));
						const FString Field = DocumentPath && !DocumentPath->GetText().TrimStartAndEnd().IsEmpty() ? TEXT("document_path") : TEXT("logical_path");
						return FailField(OutError, TEXT("workspace_path_invalid"),
							TEXT("The inventory document path must be relative and remain inside the workspace."), *Row.SourceNode, Field,
							MakeShared<FJsonValueString>(TEXT("relative workspace document path")), false);
					}
					Row.DocumentPath = Relative;
					if (FailedDocuments.Contains(Relative)) return false;
					if (!Row.EnvelopeRef.IsEmpty() && !OutParsed.BaselineProofs.Contains(Row.EnvelopeRef))
					{
						const double Started = FPlatformTime::Seconds();
						const FString Path = FPaths::Combine(Root, Row.EnvelopeRef);
						if (IsReparsePoint(Path)) return Fail(OutError, TEXT("workspace_reparse_point_rejected"),
							TEXT("The baseline document or an ancestor is a reparse point."), Row.EnvelopeRef);
						if (!Charge(Resources, {{EUeAgentResourceDimension::Files, 1}},
							TEXT("semantic_workspace:") + Row.EnvelopeRef, OutError)) return false;
						FString Text;
						if (!FFileHelper::LoadFileToString(Text, *Path)) return Fail(OutError,
							TEXT("workspace_source_read_failed"), TEXT("The captured baseline document could not be read."), Row.EnvelopeRef);
						FBaselineParseProof Proof; Proof.ContentHash = Sha256Text(Text);
						if (ActiveReadState) ActiveReadState->HashOnlyDocuments.Add(Path, Proof.ContentHash);
						OutParsed.BaselineProofs.Add(Row.EnvelopeRef, MoveTemp(Proof));
						ManagedDocumentPaths.Add(Row.EnvelopeRef);
						OutParsed.ManagedPaths.Add(Row.EnvelopeRef);
						ThisBaselineRead = FPlatformTime::Seconds() - Started; BaselineReadSeconds += ThisBaselineRead;
					}
					for (int32 Index = 0; Index < Row.ContentFiles.Num(); ++Index)
					{
						const FString& ContentPath = Row.ContentFiles[Index];
						FString SafePath;
						if (!SafeRelativePath(ContentPath, SafePath))
							return FailNode(OutError, TEXT("workspace_path_invalid"),
								TEXT("The content file path must be relative and remain inside the workspace."),
								Row.SourceNode->Get(TEXT("content_files"))->GetItems()[Index].Get(),
								MakeShared<FJsonValueString>(TEXT("relative workspace document path")), false);
						if (!ManagedDocumentPaths.Contains(SafePath))
						{
							ManagedDocumentPaths.Add(SafePath);
							OutParsed.ManagedPaths.Add(SafePath);
						}
					}
					if (!Row.LogicalPath.IsEmpty())
					{
						FString Logical;
						if (!SafeRelativePath(Row.LogicalPath, Logical))
						{
							return FailField(OutError, TEXT("workspace_path_invalid"),
								TEXT("The inventory logical path must be relative and remain inside the workspace."), *Row.SourceNode, TEXT("logical_path"),
								MakeShared<FJsonValueString>(TEXT("relative workspace document path")), false);
						}
						Row.LogicalPath = Logical;
					}
					RowPathsSeconds += FPlatformTime::Seconds() - PathsStarted - ThisBaselineRead;
					if (DocumentPointers.Contains(Row.DocumentPath + TEXT("|") + Row.UnitPointer))
					{
						auto Diagnostic = HjsonNodeFailure(TEXT("semantic_workspace_unit_pointer_duplicate"),
							TEXT("More than one inventory unit points to the same document node."), TEXT("workspace_resolve"), *Row.SourceNode, nullptr, false);
						Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(2);
						Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
						Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Row.DocumentPath + TEXT("|") + Row.UnitPointer);
						return FailAtNode(OutError, MoveTemp(Diagnostic), *Row.SourceNode);
					}
					DocumentPointers.Add(Row.DocumentPath + TEXT("|") + Row.UnitPointer);
					const FString FullPath =
						FPaths::Combine(Root, Relative);
					const double ExistsStarted = FPlatformTime::Seconds();
					const bool* Exists = DocumentExistence.Find(FullPath);
					if (!Exists) Exists = &DocumentExistence.Add(FullPath, IFileManager::Get().FileExists(*FullPath));
					const bool bDocumentExists = *Exists;
					RowExistsSeconds += FPlatformTime::Seconds() - ExistsStarted;
					if (!bDocumentExists)
					{
						const auto* DocumentPath = Row.SourceNode->Get(TEXT("document_path"));
						const auto* PathNode = DocumentPath && !DocumentPath->GetText().TrimStartAndEnd().IsEmpty()
							? DocumentPath : Row.SourceNode->Get(TEXT("logical_path"));
						return FailNode(OutError, TEXT("semantic_workspace_unit_missing"),
							TEXT("A document declared by the workspace inventory does not exist."), *PathNode,
							MakeShared<FJsonValueString>(TEXT("existing workspace document")));
					}
					FHjsonDocumentPtr* ExistingDocument = Documents.Find(Row.DocumentPath);
					if (ExistingDocument == nullptr)
					{
						FHjsonDocumentPtr Document;
						if (!ReadDocument(
								FullPath,
								Row.DocumentPath,
								Resources,
								Document,
								OutError)
							|| !Document.IsValid())
						{
							return false;
						}
						Documents.Add(Row.DocumentPath, Document);
						OutParsed.UnitDocuments.Add(Document);
						ExistingDocument = Documents.Find(Row.DocumentPath);
					}
					bool bReuseResolvedRow = false;
					const double ProofStarted = FPlatformTime::Seconds();
					if (bReusePriorRows)
					{
						const auto* Previous = PreviousResolvedRows->Rows.Find(Row.UnitId);
						const auto* Current = ActiveReadState->Documents.Find(FullPath);
						if (Previous && Current && Row.ContentFiles.IsEmpty())
						{
							FString* Hash = CurrentDocumentHashes.Find(Row.DocumentPath);
							if (!Hash) Hash = &CurrentDocumentHashes.Add(Row.DocumentPath, Sha256Text(Current->Text));
							const auto& Span = Row.SourceNode->GetProvenance();
							const FString RowHash = !Row.SourceRowHash.IsEmpty() ? Row.SourceRowHash
								: Sha256(ManifestIndex->Utf8.GetData() + Span.GetByteStart(), Span.GetByteEnd() - Span.GetByteStart());
							const FString InputHash = RowHash + *Hash
								+ (Row.EnvelopeRef.IsEmpty() ? FString() : OutParsed.BaselineProofs.FindChecked(Row.EnvelopeRef).ContentHash);
							bReuseResolvedRow = Previous->InputHash == InputHash;
						}
						if (!bReuseResolvedRow && ManifestIndex && !MaterializeInventoryRow(Row, *ManifestIndex, Resources, OutError)) return false;
						if (bReuseResolvedRow) ++ReusedManifestRows;
					}
					RowProofSeconds += FPlatformTime::Seconds() - ProofStarted;
					if (!bReuseResolvedRow && !Row.EnvelopeRef.IsEmpty())
					{
						if (!LoadBaseline(Row.EnvelopeRef)) return false;
						Row.Envelope = BaselineDocuments.FindChecked(Row.EnvelopeRef)->GetRoot()->Get(Row.UnitId);
						if (!Row.Envelope || Row.Envelope->GetKind() != EHjsonNodeKind::Object)
							return FailField(OutError, TEXT("semantic_workspace_envelope_missing"),
								TEXT("The referenced baseline document has no object envelope for this unit identity."),
								*Row.SourceNode, TEXT("envelope_ref"));
					}
					const double PointerStarted = FPlatformTime::Seconds();
					const FHjsonNode* Unit = (*ExistingDocument)->Find(Row.UnitPointer);
					RowPointerSeconds += FPlatformTime::Seconds() - PointerStarted;
					if (Unit == nullptr
						|| Unit->GetKind()
							!= EHjsonNodeKind::Object)
					{
						if (Unit) return FailNode(OutError, TEXT("semantic_workspace_unit_document_invalid"),
							TEXT("The inventory unit pointer does not identify an object in its document."), *Unit,
							MakeShared<FJsonValueString>(TEXT("object")));
						auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_unit_document_invalid"),
							TEXT("The inventory unit pointer does not identify an object in its document."), TEXT("workspace_resolve"));
						Diagnostic.ActualValuePresent = false;
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
						Diagnostic.Source.JsonPath = Row.UnitPointer;
						OutError.SetFailure(MoveTemp(Diagnostic), Row.DocumentPath, Row.UnitPointer);
						OutError.Locate(*(*ExistingDocument)->GetRoot());
						return false;
					}
					if (!bReuseResolvedRow)
					{
						TSet<FString> Expanding;
						TFunction<bool(const FHjsonNodeRef&, FHjsonNodePtr&)> Expand;
						Expand = [&](const FHjsonNodeRef& Node, FHjsonNodePtr& Out) -> bool
						{
							if (Node->GetKind() == EHjsonNodeKind::Object)
							{
								const FHjsonNode* Reference = Node->Get(TEXT("content_ref"));
								if (Reference && Node->GetEntries().Num() == 1
									&& Reference->GetKind() == EHjsonNodeKind::String)
								{
									OutParsed.ContentReferenceUnits.Add(Row.UnitId);
									FString Path;
									if (!SafeRelativePath(Reference->GetText(), Path))
										return FailNode(OutError, TEXT("workspace_path_invalid"),
											TEXT("The content reference must be relative and remain inside the workspace."), *Reference,
											MakeShared<FJsonValueString>(TEXT("relative workspace document path")), false);
									if (!ManagedDocumentPaths.Contains(Path))
									{
										ManagedDocumentPaths.Add(Path);
										OutParsed.ManagedPaths.Add(Path);
									}
									if (Expanding.Contains(Path))
										return FailNode(OutError, TEXT("semantic_workspace_content_reference_cycle"),
											TEXT("This content reference forms a cycle."), *Reference, MakeShared<FJsonValueString>(TEXT("acyclic document reference")));
									if (FailedDocuments.Contains(Path)) return false;
									auto* ContentDocument = ContentDocuments.Find(Path);
									if (!ContentDocument)
									{
										FHjsonDocumentPtr ParsedContent;
										if (!ReadDocument(FPaths::Combine(Root, Path), Path, Resources, ParsedContent, OutError))
										{
											FailedDocuments.Add(Path);
											return false;
										}
										ContentDocuments.Add(Path, ParsedContent);
										OutParsed.UnitDocuments.Add(ParsedContent);
										ContentDocument = ContentDocuments.Find(Path);
									}
									const auto& ContentRoot = (*ContentDocument)->GetRoot();
									const auto* Content = ContentRoot->GetEntries().FindByPredicate([](const auto& Field) {
										return Field.Key == TEXT("value"); });
									if (ContentRoot->GetKind() != EHjsonNodeKind::Object)
										return FailNode(OutError, TEXT("semantic_workspace_content_value_missing"),
											TEXT("The referenced content document must be an object containing value."), *ContentRoot,
											MakeShared<FJsonValueString>(TEXT("object")));
									if (!Content) return FailField(OutError, TEXT("semantic_workspace_content_value_missing"),
										TEXT("The referenced content document requires a value field."), *ContentRoot, TEXT("value"));
									Expanding.Add(Path);
									const bool bExpanded = Expand(Content->Value, Out);
									Expanding.Remove(Path);
									return bExpanded;
								}
								bool bChildrenValid = true;
								bool bChildrenChanged = false;
								TArray<TPair<FString, FHjsonNodeRef>> Entries;
								for (int32 EntryIndex = 0; EntryIndex < Node->GetEntries().Num(); ++EntryIndex)
								{
									const auto& Field = Node->GetEntries()[EntryIndex];
									FHjsonNodePtr Value;
									if (!Expand(Field.Value, Value))
								{
									Diagnostics.Append(OutError);
									OutError.Reset();
									bChildrenValid = false;
									continue;
								}
									if (Value.Get() != &Field.Value.Get())
									{
										if (!bChildrenChanged) Entries = Node->GetEntries();
										bChildrenChanged = true;
										Entries[EntryIndex].Value = Value.ToSharedRef();
									}
								}
								if (!bChildrenValid) return false;
								if (bChildrenChanged)
									Out = MakeShared<FHjsonNode>(EHjsonNodeKind::Object, Node->GetProvenance(),
										FString(), FString(), false, TArray<FHjsonNodeRef>{}, MoveTemp(Entries));
								else Out = Node;
							}
							else if (Node->GetKind() == EHjsonNodeKind::Array)
							{
								bool bChildrenValid = true;
								bool bChildrenChanged = false;
								TArray<FHjsonNodeRef> Items;
								for (int32 ItemIndex = 0; ItemIndex < Node->GetItems().Num(); ++ItemIndex)
								{
									const auto& Item = Node->GetItems()[ItemIndex];
									FHjsonNodePtr Value;
									if (!Expand(Item, Value))
								{
									Diagnostics.Append(OutError);
									OutError.Reset();
									bChildrenValid = false;
									continue;
								}
									if (Value.Get() != &Item.Get())
									{
										if (!bChildrenChanged) Items = Node->GetItems();
										bChildrenChanged = true;
										Items[ItemIndex] = Value.ToSharedRef();
									}
								}
								if (!bChildrenValid) return false;
								if (bChildrenChanged)
									Out = MakeShared<FHjsonNode>(EHjsonNodeKind::Array, Node->GetProvenance(),
										FString(), FString(), false, MoveTemp(Items), TArray<TPair<FString, FHjsonNodeRef>>{});
								else Out = Node;
							}
							else Out = Node; // Preserve scalar lexemes, including exact integers.
							return true;
					};
					FHjsonNodePtr Expanded;
					const FHjsonNodeRef Original = MakeShared<FHjsonNode>(*Unit);
					if (!Expand(Original, Expanded)) return false;
					OutParsed.AssembledRows.Add(Expanded.ToSharedRef());
					Unit = Expanded.Get();
				}
				OutParsed.UnitRows.Add(Unit);
				OutParsed.UnitEnvelopes.Add(Row.Envelope);
				if (!ManagedDocumentPaths.Contains(Row.DocumentPath))
				{
					ManagedDocumentPaths.Add(Row.DocumentPath);
					OutParsed.ManagedPaths.Add(Row.DocumentPath);
				}

				return true;
				};
				if (!ReadRow())
				{
					Diagnostics.Append(OutError);
					Diagnostics.MarkUnchecked(TEXT("unit_semantics"), Row.DocumentPath, TEXT("unit_or_dependency_invalid"));
				}
			}
			int32 ReusedBaselineSyntax = 0;
			const double RowsReadFinished = FPlatformTime::Seconds();
			if (!Diagnostics.HasErrors()) for (auto& Baseline : OutParsed.BaselineProofs)
			{
				if (BaselineDocuments.Contains(Baseline.Key)) continue;
				const auto* Prior = bReusePriorRows && PreviousResolvedRows
					? PreviousResolvedRows->Baselines.Find(Baseline.Key) : nullptr;
				if (Prior && Prior->bExact && Prior->ContentHash == Baseline.Value.ContentHash)
				{
					FUeAgentResourceChargeError ResourceError;
					if (!Resources.TryChargeMany(Prior->Charges, TEXT("semantic_workspace:validated_baseline"), ResourceError))
						return FailResource(OutError, ResourceError);
					Baseline.Value = *Prior; ++ReusedBaselineSyntax;
				}
				else if (!LoadBaseline(Baseline.Key)) return false;
			}
			if (!OutParsed.BaselineProofs.IsEmpty())
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=baseline_documents files=%d parsed_files=%d reused_syntax=%d"),
					OutParsed.BaselineProofs.Num(), BaselineDocuments.Num(), ReusedBaselineSyntax);
			for (const TPair<FString, FHjsonDocumentPtr>& DocumentPair : Documents)
			{
				OutError.Reset();
				auto ReadInterface = [&]() -> bool
				{
					const FHjsonNode* InterfaceRef =
						DocumentPair.Value->Find(TEXT("/interface_ref"));
					if (InterfaceRef == nullptr)
					{
						return true;
					}
					if (InterfaceRef->GetKind() != EHjsonNodeKind::String)
					{
						return Fail(
							OutError,
							TEXT("semantic_workspace_interface_ref_invalid"), TEXT("The interface_ref field must be a string."),
							DocumentPair.Key,
							TEXT("/interface_ref"));
					}
					const FString InterfaceFileName = InterfaceRef->GetText();
					if (InterfaceFileName.IsEmpty()
						|| FPaths::GetCleanFilename(InterfaceFileName)
							!= InterfaceFileName
						|| !InterfaceFileName.EndsWith(
							TEXT(".interfaces.hjson"),
							ESearchCase::IgnoreCase))
					{
						return Fail(
							OutError,
							TEXT("semantic_workspace_interface_ref_invalid"), TEXT("The interface reference must be a filename ending in .interfaces.hjson, without directory components."),
							DocumentPair.Key,
							TEXT("/interface_ref"));
					}
					FString InterfacePath;
					if (!SafeRelativePath(
							FPaths::Combine(
								FPaths::GetPath(DocumentPair.Key),
								InterfaceFileName),
							InterfacePath))
					{
						return Fail(
							OutError,
							TEXT("semantic_workspace_interface_ref_invalid"), TEXT("The interface reference cannot be resolved inside the workspace."),
							DocumentPair.Key,
							TEXT("/interface_ref"));
					}
					if (!IFileManager::Get().FileExists(
							*FPaths::Combine(Root, InterfacePath)))
					{
						return Fail(
							OutError,
							TEXT("semantic_workspace_interface_missing"), TEXT("The referenced interface document does not exist."),
							InterfacePath);
					}
					if (!ManagedDocumentPaths.Contains(InterfacePath))
					{
						ManagedDocumentPaths.Add(InterfacePath);
						OutParsed.ManagedPaths.Add(InterfacePath);
					}
					return true;
				};
				if (!ReadInterface()) Diagnostics.Append(OutError);
			}
			if (Diagnostics.HasErrors())
			{
				Diagnostics.MarkUnchecked(TEXT("workspace_semantics"), FString(), TEXT("invalid_units"));
				OutError = MoveTemp(Diagnostics);
				return false;
			}
			if (bReusePriorRows)
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=manifest_index reused_rows=%d parsed_rows=%d"),
					ReusedManifestRows, OutInventory.Num() - ReusedManifestRows);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=directory_source rows=%d documents=%d reused_public_syntax=%d inventory_s=%.6f documents_s=%.6f rows_s=%.6f row_paths_s=%.6f row_exists_s=%.6f row_pointer_s=%.6f row_proof_s=%.6f baseline_read_s=%.6f baseline_parse_s=%.6f total_s=%.6f"),
				OutInventory.Num(), Documents.Num(), ActiveReadState ? ActiveReadState->ReusedPublicSyntax : 0, InventoryReadFinished - DirectoryStarted,
				DocumentsReadFinished - InventoryReadFinished, RowsReadFinished - DocumentsReadFinished,
				RowPathsSeconds, RowExistsSeconds, RowPointerSeconds, RowProofSeconds, BaselineReadSeconds, BaselineParseSeconds,
				FPlatformTime::Seconds() - DirectoryStarted);
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
							"workspace_source_file_missing"), TEXT("The embedded semantic workspace file does not exist."),
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
						"semantic_workspace_envelope_invalid"), TEXT("The embedded workspace requires a manifest object and a units array."),
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
							"semantic_workspace_unit_extra"), TEXT("The embedded unit count differs from the workspace inventory count."),
					SourceUnit, Units->GetProvenance().GetJsonPointer(),
					MakeShared<FJsonValueNumber>(Units->GetItems().Num()), MakeShared<FJsonValueNumber>(OutInventory.Num()));
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
							"semantic_workspace_embedded_inventory_invalid"), TEXT("An embedded unit must be an object at the array position declared by its inventory logical path."),
						SourceUnit);
				}
				OutParsed.UnitRows.Add(
					&Units->GetItems()[Index].Get());
				OutParsed.UnitEnvelopes.Add(nullptr);
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
				return FailField(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					TEXT("Workspace metadata requires a base_state_revision object."), Manifest, TEXT("base_state_revision"),
					MakeShared<FJsonValueString>(TEXT("object")), false);
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
				|| FailTypedValidation(OutError, Contract::FDiagnostic::WrapFailure(TEXT("semantic_workspace_manifest_field_invalid"),
					OutRevision.GetValidationDiagnostic()), *Revision);
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
				return FailField(OutError, TEXT("semantic_workspace_structural_edits_invalid"),
					TEXT("Workspace metadata requires a structural_edits array."), Manifest, TEXT("structural_edits"),
					MakeShared<FJsonValueString>(TEXT("array")), false);
			}
			FWorkspaceError Diagnostics;
			TSet<FString> EditIds;
			for (const FHjsonNodeRef& Row : Edits->GetItems())
			{
				OutError.Reset();
				auto ParseEdit = [&]() -> bool
				{
				if (Row->GetKind()
					!= EHjsonNodeKind::Object)
				{
					return FailNode(OutError, TEXT("semantic_workspace_structural_edit_invalid"),
						TEXT("Each structural edit must be an object."), Row.Get(), MakeShared<FJsonValueString>(TEXT("object")), false);
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
				if (EditIds.Contains(EditId))
				{
					return FailField(OutError, TEXT("semantic_workspace_structural_edit_duplicate"),
						TEXT("This structural edit identity occurs more than once."), Row.Get(), TEXT("edit_id"),
						MakeShared<FJsonValueString>(TEXT("unique edit identity")), false);
				}
				EStructuralEditKind Kind;
				if (!ParseStructuralEditKind(KindText, Kind))
				{
					TArray<TSharedPtr<FJsonValue>> Allowed;
					for (uint8 Index = 0; Index <= static_cast<uint8>(EStructuralEditKind::SetOrder); ++Index)
						Allowed.Add(MakeShared<FJsonValueString>(LexToString(static_cast<EStructuralEditKind>(Index))));
					return FailField(OutError, TEXT("semantic_workspace_structural_edit_invalid"),
						TEXT("The structural edit kind is unsupported."), Row.Get(), TEXT("kind"), MakeShared<FJsonValueArray>(MoveTemp(Allowed)));
				}
				FStructuralEdit Edit(
					EditId,
					Kind,
					OwnerUnitId,
					SubjectUnitId,
					ReplacementUnitId,
					RelationId,
					MoveTemp(OrderedUnitIds), Row->GetProvenance());
				if (!Edit.IsValid())
				{
					return FailTypedValidation(OutError, Contract::FDiagnostic::WrapFailure(TEXT("semantic_workspace_structural_edit_invalid"),
						Edit.GetValidationDiagnostic()), Row.Get());
				}
				if (Model.FindUnit(OwnerUnitId) == nullptr)
				{
					return FailField(OutError, TEXT("semantic_workspace_reference_unresolved"),
						TEXT("The structural edit owner is absent from the workspace."), Row.Get(), TEXT("owner_unit_id"),
						MakeShared<FJsonValueString>(TEXT("existing workspace unit")), false);
				}
				for (const auto& Reference : {TPair<FString, FString>(TEXT("subject_unit_id"), SubjectUnitId),
					TPair<FString, FString>(TEXT("replacement_unit_id"), ReplacementUnitId)})
				{
					if (!Reference.Value.IsEmpty()
						&& Model.FindUnit(Reference.Value) == nullptr)
					{
						return FailField(OutError, TEXT("semantic_workspace_reference_unresolved"),
							FString::Printf(TEXT("Structural edit field '%s' refers to a unit absent from the workspace."), *Reference.Key),
							Row.Get(), Reference.Key, MakeShared<FJsonValueString>(TEXT("existing workspace unit")), false);
					}
				}
				for (int32 Index = 0; Index < Edit.GetOrderedUnitIds().Num(); ++Index)
				{
					const FString& Reference = Edit.GetOrderedUnitIds()[Index];
					if (Model.FindUnit(Reference) == nullptr)
					{
						const auto* Entries = Row->Get(TEXT("ordered_unit_ids"));
						const auto* Entry = Entries && Entries->GetItems().IsValidIndex(Index) ? &Entries->GetItems()[Index].Get() : nullptr;
						if (Entry) return FailNode(OutError, TEXT("semantic_workspace_reference_unresolved"),
							TEXT("This ordered unit referenced by the structural edit is absent from the workspace."), *Entry,
							MakeShared<FJsonValueString>(TEXT("existing workspace unit")), false);
						return FailField(OutError, TEXT("semantic_workspace_reference_unresolved"),
							TEXT("An ordered unit referenced by the structural edit is absent from the workspace."), Row.Get(), TEXT("ordered_unit_ids"), nullptr, false);
					}
				}
				EditIds.Add(EditId);
				OutEdits.Add(MoveTemp(Edit));
				return true;
				};
				if (!ParseEdit())
				{
					OutError.Locate(Row->GetProvenance());
					Diagnostics.Append(OutError);
					Diagnostics.MarkUnchecked(TEXT("structural_edit"), Row->GetProvenance().GetSourceUnit(), TEXT("edit_invalid"));
				}
			}
			OutError = MoveTemp(Diagnostics);
			return !OutError.HasErrors();
		}
	}

	void RememberParsedSemanticManifest(const FString& WorkspaceRoot, const FString& ContentHash,
		const FHjsonDocument& Document, const TArray<uint8>& Utf8,
		const TMap<EUeAgentResourceDimension, int64>& ParseCharges)
	{
		FString Root = FPaths::ConvertRelativePathToFull(WorkspaceRoot);
		FPaths::NormalizeDirectoryName(Root);
		if (Document.GetSourceUnit().StartsWith(TEXT(".uai/inventory/"), ESearchCase::CaseSensitive)) {
			FWorkspaceError Error;
			MakeInventoryDocumentProof(Root, Document.GetSourceUnit(), ContentHash, Document, Utf8, ParseCharges, Error);
			return;
		}
		if (!PreviousResolvedRows || PreviousResolvedRows->Root != Root
			|| Utf8.Num() > 512LL * 1024 * 1024) return;
		const auto* Schema = Document.GetRoot()->Get(TEXT("workspace_schema"));
		const auto* Rows = Document.GetRoot()->Get(TEXT("unit_inventory"));
		if (!Schema || Schema->GetText() != GetSemanticWorkspaceSchemaId() || !Rows
			|| Rows->GetKind() != EHjsonNodeKind::Array || Rows->GetItems().Num() > 200000) return;
		auto Proof = MakeShared<FManifestIndexProof>();
		FWorkspaceError Error;
		if (!ParseInventory(*Document.GetRoot(), Proof->Inventory, Error)) return;
		Proof->ReferenceMapHash = HashReferenceMap(Proof->Inventory);
		if (Proof->ReferenceMapHash != PreviousResolvedRows->ReferenceMapHash) return;
		Proof->Root = MoveTemp(Root);
		Proof->ContentHash = ContentHash;
		Proof->Utf8 = Utf8;
		Proof->ParseCharges = ParseCharges;
		TArray<TPair<FString, FHjsonNodeRef>> HeaderFields;
		for (const auto& Field : Document.GetRoot()->GetEntries())
			if (Field.Key != TEXT("unit_inventory")) HeaderFields.Add(Field);
		Proof->Header = MakeShared<FHjsonDocument>(Document.GetSourceUnit(), MakeShared<FHjsonNode>(
			EHjsonNodeKind::Object, Document.GetRoot()->GetProvenance(), FString(), FString(), false,
			TArray<FHjsonNodeRef>{}, MoveTemp(HeaderFields)));
		for (auto& Row : Proof->Inventory)
		{
			const auto& Span = Row.SourceNode->GetProvenance();
			if (Span.GetByteStart() < 0 || Span.GetByteEnd() <= Span.GetByteStart() || Span.GetByteEnd() > Utf8.Num()) return;
			// Keep only the small index/diagnostic fields. Large captured envelopes
			// remain exact UTF-8 spans and are parsed lazily for changed rows.
			TArray<TPair<FString, FHjsonNodeRef>> Fields;
			for (const auto& Field : Row.SourceNode->GetEntries())
				if (Field.Key != TEXT("envelope")) Fields.Add(Field);
			Row.RetainedSourceNode = MakeShared<FHjsonNode>(EHjsonNodeKind::Object, Span,
				FString(), FString(), false, TArray<FHjsonNodeRef>{}, MoveTemp(Fields));
			Row.SourceNode = Row.RetainedSourceNode.Get();
			Row.Envelope = nullptr;
		}
		PreviousManifestIndex = MoveTemp(Proof);
	}

	bool TryCollectSemanticWorkspaceManagedPaths(
		const FString& WorkspaceRoot, FUeAgentResourceTracker& Resources,
		TArray<FString>& OutPaths, FWorkspaceError& OutError)
	{
		FParsedSource Parsed;
		TArray<FInventoryRow> Inventory;
		if (!ReadDirectorySource(FWorkspaceSource::Directory(WorkspaceRoot), Resources, Parsed, Inventory, OutError))
			return false;
		OutPaths = MoveTemp(Parsed.ManagedPaths);
		OutPaths.AddUnique(ManifestPath);
		return true;
	}

	static bool TryResolveSemanticWorkspaceInternal(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		const bool bAllowManagedUnitEdits,
		FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError)
	{
		const double ResolveStarted = FPlatformTime::Seconds();
		OutWorkspace.Reset();
		OutError.Reset();
		FString ResolutionRoot;
		if (ActiveReadState && Source.GetKind() == EWorkspaceSourceKind::Directory)
		{
			ResolutionRoot = FPaths::ConvertRelativePathToFull(Source.GetPath());
			FPaths::NormalizeDirectoryName(ResolutionRoot);
			auto& Cached = *ActiveReadState;
			bool bReusable = Cached.ResolvedWorkspace.IsValid()
				&& Cached.ResolvedRoot.Equals(ResolutionRoot, ESearchCase::CaseSensitive)
				&& Cached.ResolvedMode == ExpectedMode
				&& Cached.bResolvedManagedEdits == bAllowManagedUnitEdits
				&& Cached.ResolvedDocuments.Contains(FPaths::Combine(ResolutionRoot, ManifestPath))
				&& IFileManager::Get().DirectoryExists(*ResolutionRoot)
				&& !IsReparsePoint(ResolutionRoot);
			if (bReusable)
			{
				for (const auto& Document : Cached.ResolvedDocuments)
				{
					const auto* Parsed = Cached.Documents.Find(Document.Key);
					FString CurrentText;
					if (!Parsed || Parsed->Parsed != Document.Value || IsReparsePoint(Document.Key)
						|| !FFileHelper::LoadFileToString(CurrentText, *Document.Key)
						|| !Parsed->Text.Equals(CurrentText, ESearchCase::CaseSensitive))
					{
						bReusable = false;
						break;
					}
				}
			}
			if (bReusable)
			{
				for (const auto& Document : Cached.ResolvedHashOnlyDocuments)
				{
					FString Text;
					if (IsReparsePoint(Document.Key) || !FFileHelper::LoadFileToString(Text, *Document.Key)
						|| Sha256Text(Text) != Document.Value) { bReusable = false; break; }
				}
			}
			if (bReusable)
			{
				// Interface references are checked for existence by ReadDirectorySource
				// without parsing their contents. Preserve that input as well.
				for (const FString& Path : Cached.ResolvedWorkspace->GetManagedUnitPaths())
				{
					if (!IFileManager::Get().FileExists(*FPaths::Combine(ResolutionRoot, Path)))
					{
						bReusable = false;
						break;
					}
				}
			}
			if (bReusable)
			{
				if (!Charge(Resources, {{EUeAgentResourceDimension::Files,
					Cached.ResolvedDocuments.Num() + Cached.ResolvedHashOnlyDocuments.Num()}},
					TEXT("semantic_workspace:validated_resolution"), OutError)) return false;
				OutWorkspace = Cached.ResolvedWorkspace;
				OutError = Cached.ResolvedDiagnostics;
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=semantic_resolve cached=true files=%d total_s=%.6f"),
					Cached.ResolvedDocuments.Num(), FPlatformTime::Seconds() - ResolveStarted);
				return true;
			}
		}
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
					OutError, ExpectedMode, bAllowManagedUnitEdits)
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
		const double SourceReadFinished = FPlatformTime::Seconds();

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
					"semantic_workspace_schema_unsupported"), TEXT("The semantic workspace schema is unsupported."),
				Parsed.Manifest->GetProvenance()
					.GetSourceUnit(),
				Parsed.Manifest->Get(TEXT("workspace_schema"))->GetProvenance().GetJsonPointer(),
				MakeShared<FJsonValueString>(Schema), MakeShared<FJsonValueString>(GetSemanticWorkspaceSchemaId()));
		}
		ESemanticWorkspaceMode Mode;
		if (!ParseMode(ModeText, Mode))
		{
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_mode_unsupported"), TEXT("The semantic workspace import mode is unsupported."),
				Parsed.Manifest->GetProvenance()
					.GetSourceUnit(),
				Parsed.Manifest->Get(TEXT("import_mode"))->GetProvenance().GetJsonPointer(), MakeShared<FJsonValueString>(ModeText),
				MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(TEXT("state")),
					MakeShared<FJsonValueString>(TEXT("seed")), MakeShared<FJsonValueString>(TEXT("edit_bundle"))}));
		}
		if (Mode != ExpectedMode)
		{
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_mode_mismatch"), TEXT("The workspace import mode differs from the mode required by this operation."),
				Parsed.Manifest->GetProvenance()
					.GetSourceUnit(),
				Parsed.Manifest->Get(TEXT("import_mode"))->GetProvenance().GetJsonPointer(),
				MakeShared<FJsonValueString>(ModeText), MakeShared<FJsonValueString>(LexToString(ExpectedMode)));
		}
		if (Inventory.Num() != Parsed.UnitRows.Num())
		{
			return Fail(
				OutError,
				Inventory.Num() > Parsed.UnitRows.Num()
					? TEXT(
						"semantic_workspace_unit_missing")
					: TEXT(
						"semantic_workspace_unit_extra"), TEXT("The resolved document count differs from the workspace inventory count."),
				Parsed.Manifest->GetProvenance().GetSourceUnit(), Parsed.Manifest->GetProvenance().GetJsonPointer(),
				MakeShared<FJsonValueNumber>(Parsed.UnitRows.Num()), MakeShared<FJsonValueNumber>(Inventory.Num()));
		}

		TArray<FSemanticUnit> Units;
		Units.Reserve(Parsed.UnitRows.Num());
		TArray<FString> EditedUnitIds;
		TMap<FString, TSet<FString>> ResolvedKeyedOperationRoots;
		TMap<FString, FSourceProvenance> UnitSources;
		TMap<FString, FHjsonDocumentRef> UnitSourceDocuments;
		TMap<FString, FSemanticUnit> BaselineUnits;
		TSet<FString> SeenUnitIds;
		TMap<FString, FString> PublicRefToUnitId;
		for (const FInventoryRow& Row : Inventory)
		{
			PublicRefToUnitId.Add(Row.PublicRef, Row.UnitId);
		}
		TSharedPtr<FResolvedRowsProof> NextRows;
		TSharedPtr<const FResolvedRowsProof> PriorRows;
		TArray<uint8> ManifestUtf8;
		TMap<FString, FString> DocumentHashes;
		int32 ReusedSemanticRows = 0;
		if (ActiveReadState && !ResolutionRoot.IsEmpty() && Inventory.Num() <= 200000)
		{
			int64 SourceCharacters = 0;
				for (const auto& Document : ActiveReadState->Documents)
					if (!Parsed.BaselineProofs.Contains(Document.Value.SourceUnit)) SourceCharacters += Document.Value.Text.Len();
			const auto* Manifest = ActiveReadState->Documents.Find(FPaths::Combine(ResolutionRoot, ManifestPath));
			if (Manifest && SourceCharacters <= 256LL * 1024 * 1024)
			{
				const FTCHARToUTF8 Utf8(*Manifest->Text);
				ManifestUtf8.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
				NextRows = MakeShared<FResolvedRowsProof>();
				NextRows->Root = ResolutionRoot;
				NextRows->Mode = ExpectedMode;
				NextRows->bManagedEdits = bAllowManagedUnitEdits;
				NextRows->ReferenceMapHash = HashReferenceMap(Inventory);
				NextRows->Baselines = Parsed.BaselineProofs;
				if (PreviousResolvedRows && PreviousResolvedRows->Root == ResolutionRoot
					&& PreviousResolvedRows->Mode == ExpectedMode
					&& PreviousResolvedRows->bManagedEdits == bAllowManagedUnitEdits
					&& PreviousResolvedRows->ReferenceMapHash == NextRows->ReferenceMapHash)
					PriorRows = PreviousResolvedRows;
			}
		}
		FWorkspaceError UnitDiagnostics;
		for (int32 Index = 0;
			Index < Parsed.UnitRows.Num();
			++Index)
		{
			OutError.Reset();
			FString RowInputHash;
			const auto& ProofInventory = Inventory[Index];
			if (NextRows && ProofInventory.SourceNode && ProofInventory.ContentFiles.IsEmpty()
				&& !Parsed.ContentReferenceUnits.Contains(ProofInventory.UnitId))
			{
				const auto& Span = ProofInventory.SourceNode->GetProvenance();
				const int64 Start = Span.GetByteStart(), End = Span.GetByteEnd();
				const FString FullPath = FPaths::Combine(ResolutionRoot, ProofInventory.DocumentPath);
				const auto* Document = ActiveReadState->Documents.Find(FullPath);
				if (Document && (!ProofInventory.SourceRowHash.IsEmpty() || (Start >= 0 && End > Start && End <= ManifestUtf8.Num())))
				{
					FString* Hash = DocumentHashes.Find(ProofInventory.DocumentPath);
					if (!Hash) Hash = &DocumentHashes.Add(ProofInventory.DocumentPath, Sha256Text(Document->Text));
					RowInputHash = (!ProofInventory.SourceRowHash.IsEmpty() ? ProofInventory.SourceRowHash
						: Sha256(ManifestUtf8.GetData() + Start, End - Start)) + *Hash
						+ (ProofInventory.EnvelopeRef.IsEmpty() ? FString()
							: Parsed.BaselineProofs.FindChecked(ProofInventory.EnvelopeRef).ContentHash);
				}
			}
			const auto* Prior = PriorRows ? PriorRows->Rows.Find(ProofInventory.UnitId) : nullptr;
			if (Prior && !RowInputHash.IsEmpty() && Prior->InputHash == RowInputHash && Prior->Unit.IsSet())
			{
				FUeAgentResourceChargeError ResourceError;
				if (!Resources.TryChargeMany(Prior->Charges, TEXT("semantic_workspace:validated_row"), ResourceError))
					return FailResource(OutError, ResourceError);
				Units.Add(Prior->Unit.GetValue());
				SeenUnitIds.Add(ProofInventory.UnitId);
				if (Prior->Baseline.IsSet()) BaselineUnits.Add(ProofInventory.UnitId, Prior->Baseline.GetValue());
				if (Prior->Source.IsSet()) UnitSources.Add(ProofInventory.UnitId, Prior->Source.GetValue());
				if (Prior->SourceDocument) UnitSourceDocuments.Add(ProofInventory.UnitId, Prior->SourceDocument.ToSharedRef());
				if (!Prior->KeyedRoots.IsEmpty()) ResolvedKeyedOperationRoots.Add(ProofInventory.UnitId, Prior->KeyedRoots);
				if (Prior->bEdited) EditedUnitIds.Add(ProofInventory.UnitId);
				OutError = Prior->Diagnostics;
				NextRows->Rows.Add(ProofInventory.UnitId, *Prior);
				++ReusedSemanticRows;
				continue;
			}
			const auto UsageBefore = Resources.Snapshot();
			const int32 EditedBefore = EditedUnitIds.Num();
			auto ParseUnit = [&]() -> bool
			{
				const FHjsonNode* UnitEnvelope =
					Parsed.UnitEnvelopes.IsValidIndex(Index)
						? Parsed.UnitEnvelopes[Index]
						: nullptr;
				if (bAllowManagedUnitEdits && UnitEnvelope == nullptr)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_editable_envelope_"
							"reexport_required"), TEXT("The editable unit has no captured envelope; export the workspace again."),
						Inventory[Index].UnitId,
						TEXT("/unit_inventory/envelope"));
				}
				TOptional<FSemanticUnit> Unit;
				TOptional<bool> ManagedSemanticChanged;
				TSet<FString> UnitResolvedKeyedOperationRoots;
				FHjsonNodePtr UnitValueSources;
				if (!ParseSemanticUnit(
						*Parsed.UnitRows[Index],
						UnitEnvelope,
						Inventory[Index].UnitId,
						LexToString(Inventory[Index].UnitKind),
						PublicRefToUnitId,
						Resources,
						Unit,
						ManagedSemanticChanged,
						UnitResolvedKeyedOperationRoots,
						UnitValueSources,
						OutError)
					|| !Unit.IsSet())
				{
					return false;
				}
				const FInventoryRow& InventoryRow =
					Inventory[Index];
				if (Source.GetKind() == EWorkspaceSourceKind::Directory)
				{
					const FHjsonNode& Row = *Parsed.UnitRows[Index];
					UnitSources.Add(Unit->GetUnitId(), Row.GetProvenance());
					const FHjsonNode* Values = Row.Get(TEXT("values"));
					// A shallow root copy retains the parser's original child nodes and
					// provenance, including expanded public file references. No reparse.
					UnitSourceDocuments.Add(Unit->GetUnitId(), MakeShared<FHjsonDocument>(
						Row.GetProvenance().GetSourceUnit(), UnitValueSources.IsValid()
							? UnitValueSources.ToSharedRef() : MakeShared<FHjsonNode>(Values ? *Values : Row)));
				}
				if (!UnitResolvedKeyedOperationRoots.IsEmpty())
					ResolvedKeyedOperationRoots.Add(Unit->GetUnitId(), MoveTemp(UnitResolvedKeyedOperationRoots));
				if (bAllowManagedUnitEdits && InventoryRow.EditHash.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("semantic_workspace_edit_hash_reexport_required"), TEXT("The editable unit has no baseline edit digest; export the workspace again."),
						InventoryRow.UnitId,
						TEXT("/unit_inventory/edit_hash"));
				}
				if (SeenUnitIds.Contains(Unit->GetUnitId()))
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_unit_duplicate"),
						TEXT("More than one resolved document declares the same semantic unit."), TEXT("workspace_resolve"));
					Diagnostic.PropertyPath = TEXT("unit_id");
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Unit->GetUnitId());
					return FailAtNode(OutError, MoveTemp(Diagnostic), *Parsed.UnitRows[Index]);
				}
				if (Unit->GetUnitId() != InventoryRow.UnitId)
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_unit_extra"),
						TEXT("The resolved semantic unit identity differs from its inventory entry."), TEXT("workspace_resolve"));
					Diagnostic.PropertyPath = TEXT("unit_id");
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Unit->GetUnitId());
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(InventoryRow.UnitId);
					return FailAtNode(OutError, MoveTemp(Diagnostic), *Parsed.UnitRows[Index]);
				}
				if (Unit->GetUnitKind()
					!= InventoryRow.UnitKind)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_unit_kind_mismatch"), TEXT("The resolved semantic unit kind differs from its inventory entry."),
						Parsed.UnitRows[Index]->GetProvenance().GetSourceUnit(),
						Parsed.UnitRows[Index]->GetProvenance().GetJsonPointer(),
						MakeShared<FJsonValueString>(LexToString(Unit->GetUnitKind())), MakeShared<FJsonValueString>(LexToString(InventoryRow.UnitKind)));
				}
				TArray<uint8> RowBytes;
				if (!TrySerializeSemanticUnitRow(Unit.GetValue(), RowBytes, OutError, InventoryRow.DocumentPath)) return false;
				const FString ActualContentHash = Sha256(RowBytes);
				const bool bContentHashChanged = ActualContentHash != InventoryRow.ContentHash;
				// Reuse the writer's captured machine values, not the edited visible row.
				// A hash match binds the complete unit (including identity/definition refs).
				// Older envelopes may lack these values; unchanged rows remain usable.
				if (!bContentHashChanged) BaselineUnits.Add(Unit->GetUnitId(), Unit.GetValue());
				else if (const FHjsonNode* Captured = UnitEnvelope ? UnitEnvelope->Get(TEXT("captured_values")) : nullptr;
					Captured && Captured->GetKind() == EHjsonNodeKind::Object)
				{
					TMap<FString, FUeAgentCanonicalValueRef> Values;
					FWorkspaceError BaselineError;
					for (const auto& Field : Captured->GetEntries())
					{
						FUeAgentCanonicalValuePtr Value;
						if (!TryCanonicalValue(*Field.Value, Resources, Value, BaselineError))
						{ OutError = MoveTemp(BaselineError); return false; }
						Values.Add(Field.Key, Value.ToSharedRef());
					}
					{
						auto Baseline = Unit->WithValues(MoveTemp(Values));
						FString BaselineHash;
						if (!TryHashSemanticUnitRow(Baseline, BaselineHash, BaselineError, InventoryRow.DocumentPath))
						{ OutError = MoveTemp(BaselineError); return false; }
						if (BaselineHash == InventoryRow.ContentHash)
							BaselineUnits.Add(Unit->GetUnitId(), MoveTemp(Baseline));
					}
				}
				if (!bAllowManagedUnitEdits && bContentHashChanged)
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_unit_hash_mismatch"),
						TEXT("The semantic unit content differs from its recorded digest in a read-only workspace load."), TEXT("workspace_resolve"));
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(ActualContentHash);
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(InventoryRow.ContentHash);
					return FailAtNode(OutError, MoveTemp(Diagnostic), *Parsed.UnitRows[Index]);
				}
				TArray<uint8> EditBytes;
				if (bAllowManagedUnitEdits
					&& !TrySerializeEditNode(
						*Parsed.UnitRows[Index], Resources, OutError, EditBytes))
				{
					if (OutError.GetCode().IsEmpty())
					{
						return Fail(OutError,
							TEXT("semantic_workspace_edit_hash_failed"), TEXT("The edited semantic unit could not be serialized for change detection."), InventoryRow.UnitId);
					}
					return false;
				}
				if (bAllowManagedUnitEdits && EditBytes.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("semantic_workspace_edit_hash_failed"), TEXT("Serializing the edited semantic unit produced no bytes for change detection."),
						InventoryRow.UnitId);
				}
				if (bAllowManagedUnitEdits
					&& Sha256(EditBytes) != InventoryRow.EditHash)
				{
					if (!ManagedSemanticChanged.IsSet()
						|| ManagedSemanticChanged.GetValue())
					{
						EditedUnitIds.Add(Unit->GetUnitId());
					}
				}
				SeenUnitIds.Add(Unit->GetUnitId());
				Units.Add(MoveTemp(Unit.GetValue()));
				return true;
			};
			if (!ParseUnit())
			{
				OutError.Locate(Parsed.UnitRows[Index]->GetProvenance());
				UnitDiagnostics.Append(OutError);
				UnitDiagnostics.MarkUnchecked(TEXT("unit_semantics"), Inventory[Index].DocumentPath, TEXT("unit_structure_invalid"));
			}
			else if (NextRows && !RowInputHash.IsEmpty())
			{
				FResolvedRowProof Proof;
				Proof.InputHash = MoveTemp(RowInputHash);
				Proof.Unit = Units.Last(); // Before graph-layout derived values below.
				Proof.bEdited = EditedUnitIds.Num() != EditedBefore;
				Proof.Diagnostics = OutError;
				if (const auto* Value = BaselineUnits.Find(ProofInventory.UnitId)) Proof.Baseline = *Value;
				if (const auto* Value = UnitSources.Find(ProofInventory.UnitId)) Proof.Source = *Value;
				if (const auto* Value = UnitSourceDocuments.Find(ProofInventory.UnitId)) Proof.SourceDocument = *Value;
				if (const auto* Value = ResolvedKeyedOperationRoots.Find(ProofInventory.UnitId)) Proof.KeyedRoots = *Value;
				const auto UsageAfter = Resources.Snapshot();
				bool bExact = true;
				for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D)
				{
					const auto Dimension = static_cast<EUeAgentResourceDimension>(D);
					const int64 Before = UsageBefore.Consumed(Dimension), After = UsageAfter.Consumed(Dimension);
					if (After == MAX_int64 || After < Before) { bExact = false; break; }
					if (After > Before) Proof.Charges.Add(Dimension, After - Before);
				}
				if (bExact) NextRows->Rows.Add(ProofInventory.UnitId, MoveTemp(Proof));
			}
		}
		if (UnitDiagnostics.HasErrors())
		{
			UnitDiagnostics.MarkUnchecked(TEXT("model_references"), FString(), TEXT("invalid_units"));
			OutError = MoveTemp(UnitDiagnostics);
			return false;
		}

		// Graph layout is edited through the ordinary node pos. Hidden layout units
		// are derived from that same resolved value, never an independent edit source.
		if (bAllowManagedUnitEdits)
		{
			FWorkspaceError LayoutDiagnostics;
			TSet<FString> AmbiguousGraphs;
			TMap<FString, int32> UnitIndices;
			auto CollectLayoutError = [&](const FSemanticUnit& Unit)
			{
				if (const int32* Index = UnitIndices.Find(Unit.GetUnitId()))
					OutError.Locate(Parsed.UnitRows[*Index]->GetProvenance());
				LayoutDiagnostics.Append(OutError);
				OutError.Reset();
			};
			TMap<FString, TMap<FString, FUeAgentCanonicalValueRef>> NodesByGraph;
			for (int32 Index = 0; Index < Units.Num(); ++Index)
			{
				const FSemanticUnit& Unit = Units[Index];
				UnitIndices.Add(Unit.GetUnitId(), Index);
				if (Unit.GetUnitKind() != ESemanticUnitKind::StructureState) continue;
				const auto* Nodes = Unit.GetValues().Find(TEXT("nodes"));
				if (!Nodes || (*Nodes)->GetKind() != EUeAgentValueKind::Object || !Unit.GetValues().Contains(TEXT("edges"))) continue;
				auto& ById = NodesByGraph.FindOrAdd(Unit.GetUnitId());
				for (const auto& Node : (*Nodes)->GetObjectItems())
				{
					if (Node.Value->GetKind() != EUeAgentValueKind::Object) continue;
					for (const auto& Field : Node.Value->GetObjectItems())
						if (Field.Key == TEXT("id") && Field.Value->GetKind() == EUeAgentValueKind::String)
						{
							if (ById.Contains(Field.Value->GetText()))
							{
								Fail(OutError, TEXT("semantic_workspace_graph_layout_identity_ambiguous"), TEXT("More than one graph node declares the same layout identity."), Unit.GetUnitId());
								CollectLayoutError(Unit);
								AmbiguousGraphs.Add(Unit.GetUnitId());
								continue;
							}
							ById.Add(Field.Value->GetText(), Node.Value);
						}
				}
			}
			for (const FSemanticUnit& Node : Units)
			{
				if (Node.GetUnitKind() != ESemanticUnitKind::ItemState || Node.GetLayoutRef().IsEmpty()) continue;
				if (AmbiguousGraphs.Contains(Node.GetOwnerRef()))
				{
					LayoutDiagnostics.MarkUnchecked(TEXT("graph_layout"), Node.GetSourceProvenance(), TEXT("ambiguous_node_identity"));
					continue;
				}
				const auto* ById = NodesByGraph.Find(Node.GetOwnerRef());
				const auto* Identity = Node.GetValues().Find(TEXT("id"));
				if (!ById || !Identity || (*Identity)->GetKind() != EUeAgentValueKind::String) continue;
				const auto* Resolved = ById->Find((*Identity)->GetText());
				if (!Resolved) continue;
				TMap<FString, FUeAgentCanonicalValueRef> Fields;
				for (const auto& Field : (*Resolved)->GetObjectItems()) Fields.Add(Field);
				const auto* Operation = Fields.Find(TEXT("operation"));
				if (Operation && (*Operation)->GetKind() == EUeAgentValueKind::String && (*Operation)->GetText() == TEXT("remove")) continue;
				if (!TryNormalizeGraphAuthoringNodePosition(Fields, Node.GetUnitId(), Resources, OutError))
				{
					CollectLayoutError(Node);
					continue;
				}
				const auto* Position = Fields.Find(TEXT("pos"));
				if (!Position) continue;
				const int32* LayoutIndex = UnitIndices.Find(Node.GetLayoutRef());
				if (!LayoutIndex)
				{
					Fail(OutError, TEXT("semantic_workspace_graph_layout_missing"), TEXT("The graph node's referenced layout unit is missing."), Node.GetUnitId());
					CollectLayoutError(Node);
					continue;
				}
				const FSemanticUnit& Layout = Units[*LayoutIndex];
				if (Layout.GetUnitKind() != ESemanticUnitKind::LayoutState || Layout.GetOwnerRef() != Node.GetUnitId())
				{
					Fail(OutError, TEXT("semantic_workspace_graph_layout_owner_mismatch"), TEXT("The graph node's layout has the wrong unit kind or belongs to a different owner."), Node.GetUnitId());
					CollectLayoutError(Node);
					continue;
				}
				TMap<FString, FUeAgentCanonicalValueRef> Values;
				for (const auto& Field : (*Position)->GetObjectItems()) Values.Add(Field);
				Units[*LayoutIndex] = FSemanticUnit(Layout.GetUnitId(), Layout.GetUnitKind(), Layout.GetOwnerRef(),
					Layout.GetSemanticTypeRef(), Layout.GetRole(), Layout.GetFieldSetRef(), MoveTemp(Values),
					Layout.GetItemRefs(), Layout.GetRelationRefs(), Layout.GetOrderingRef(), Layout.GetLayoutRef(),
					Layout.GetDependencyRefs(), Layout.GetProfileVisibility(), Layout.GetSourceProvenance(),
					Layout.GetTargetIdentityKey(), Layout.GetCategoryPath());
			}
			if (LayoutDiagnostics.HasErrors())
			{
				LayoutDiagnostics.MarkUnchecked(TEXT("model_references"), FString(), TEXT("invalid_graph_layout"));
				OutError = MoveTemp(LayoutDiagnostics);
				return false;
			}
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
			OutError.LocateUnits(UnitSources);
			if (OutError.GetCode() == TEXT("unresolved_semantic_unit_reference")
				|| OutError.GetCode() == TEXT("missing_target_unit"))
				OutError.Recode(TEXT("semantic_workspace_reference_unresolved"), TEXT("The workspace reference does not resolve to a semantic unit."));
			return false;
		}
		if (!bAllowManagedUnitEdits
			&& Model->GetSemanticFingerprint() != ExpectedFingerprint)
		{
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_fingerprint_mismatch"), TEXT("The semantic workspace content differs from its recorded fingerprint."),
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
			if (SelectionFingerprint.IsEmpty() || ClosureFingerprint.IsEmpty())
			{
				const FString Field = SelectionFingerprint.IsEmpty() ? TEXT("selection_fingerprint") : TEXT("closure_fingerprint");
				return FailField(OutError, TEXT("semantic_workspace_closure_incomplete"),
					FString::Printf(TEXT("Edit bundle metadata requires a non-empty %s."), *Field), *Parsed.Manifest, Field,
					MakeShared<FJsonValueString>(TEXT("non-empty fingerprint")), false);
			}
			if (ClosureSet.Num() != Model->GetUnits().Num())
			{
				auto Diagnostic = HjsonFieldFailure(TEXT("semantic_workspace_closure_incomplete"),
					TEXT("The edit bundle closure count differs from the model unit count."), TEXT("workspace_resolve"),
					*Parsed.Manifest, TEXT("closure_unit_ids"), nullptr, false);
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(ClosureSet.Num());
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(Model->GetUnits().Num());
				return FailAtNode(OutError, MoveTemp(Diagnostic), *Parsed.Manifest);
			}
			for (const FSemanticUnit& Unit :
				Model->GetUnits())
			{
				if (!ClosureSet.Contains(Unit.GetUnitId()))
				{
					auto Diagnostic = HjsonFieldFailure(TEXT("semantic_workspace_closure_incomplete"),
						TEXT("The edit bundle closure omits a semantic unit required by the model."), TEXT("workspace_resolve"),
						*Parsed.Manifest, TEXT("closure_unit_ids"), nullptr, false);
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Unit.GetUnitId());
					return FailAtNode(OutError, MoveTemp(Diagnostic), *Parsed.Manifest);
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
		TArray<FString> BaselinePaths;
		Parsed.BaselineProofs.GetKeys(BaselinePaths);
		BaselinePaths.Sort();
		for (const FString& Path : BaselinePaths)
			WorkspaceMaterial += TEXT("|baseline:") + Path + TEXT("|")
				+ Parsed.BaselineProofs.FindChecked(Path).ContentHash;
		TArray<FString> InventoryPaths;
		Parsed.InventoryHashes.GetKeys(InventoryPaths); InventoryPaths.Sort();
		for (const FString& Path : InventoryPaths)
			WorkspaceMaterial += TEXT("|inventory:") + Path + TEXT("|") + Parsed.InventoryHashes.FindChecked(Path);
		TMap<FString, FString> BaselineUnitHashes;
		for (const FInventoryRow& Row : Inventory)
		{
			WorkspaceMaterial += TEXT("|")
				+ Row.UnitId + TEXT("|")
				+ Row.ContentHash;
			BaselineUnitHashes.Add(Row.UnitId, Row.ContentHash);
		}
		TOptional<TArray<FString>> ExplicitEditedUnitIds;
		if (bAllowManagedUnitEdits
			&& Source.GetKind() == EWorkspaceSourceKind::Directory)
		{
			ExplicitEditedUnitIds = MoveTemp(EditedUnitIds);
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
				Sha256Text(WorkspaceMaterial),
				MoveTemp(ExplicitEditedUnitIds), MoveTemp(ResolvedKeyedOperationRoots), MoveTemp(UnitSources), MoveTemp(BaselineUnitHashes),
				MoveTemp(UnitSourceDocuments), MoveTemp(BaselineUnits),
				Parsed.Manifest->Get(TEXT("structural_edits"))->GetProvenance());
		if (!OutWorkspace->IsValid())
		{
			auto Diagnostic = Contract::FDiagnostic::WrapFailure(TEXT("semantic_workspace_invalid"), OutWorkspace->GetValidationDiagnostic());
			OutWorkspace.Reset();
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}
		if (ActiveReadState && !ResolutionRoot.IsEmpty())
		{
			auto& Cached = *ActiveReadState;
			Cached.ResolvedRoot = ResolutionRoot;
			Cached.ResolvedMode = ExpectedMode;
			Cached.bResolvedManagedEdits = bAllowManagedUnitEdits;
			Cached.ResolvedWorkspace = OutWorkspace;
			Cached.ResolvedDiagnostics = OutError;
			Cached.ResolvedDocuments.Reset();
			Cached.ResolvedHashOnlyDocuments.Reset();
			const FString Prefix = ResolutionRoot + TEXT("/");
			for (const auto& Document : Cached.Documents)
			{
				if (Document.Key.StartsWith(Prefix, ESearchCase::CaseSensitive))
					Cached.ResolvedDocuments.Add(Document.Key, Document.Value.Parsed);
			}
			for (const auto& Document : Cached.HashOnlyDocuments)
				if (Document.Key.StartsWith(Prefix, ESearchCase::CaseSensitive)
					&& !Cached.ResolvedDocuments.Contains(Document.Key))
					Cached.ResolvedHashOnlyDocuments.Add(Document.Key, Document.Value);
		}
		if (NextRows) {
			TMap<FString, FCompletedDocumentRead> CompletedDocuments;
			if (ActiveReadState) for (const auto& Document : ActiveReadState->Documents) {
				if (!Document.Key.StartsWith(ResolutionRoot + TEXT("/")) || Document.Value.SourceUnit.StartsWith(TEXT(".uai/"))
					|| !Document.Value.bExactParseCharges) continue;
				CompletedDocuments.Add(Document.Key, {Document.Value.SourceUnit, Sha256Text(Document.Value.Text),
					Document.Value.Parsed, Document.Value.ParseCharges});
			}
			PreviousDocumentReads = MoveTemp(CompletedDocuments);
			PreviousResolvedRows = MoveTemp(NextRows);
		}
		else PreviousResolvedRows.Reset();
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=semantic_resolve cached=false units=%d reused_rows=%d source_read_s=%.6f semantic_s=%.6f total_s=%.6f"),
			OutWorkspace->GetModel()->GetUnits().Num(), ReusedSemanticRows, SourceReadFinished - ResolveStarted,
			FPlatformTime::Seconds() - SourceReadFinished, FPlatformTime::Seconds() - ResolveStarted);
		return true;
	}

	bool TryResolveKeyedPropertyValues(const FHjsonNode& Row, const FString& ExpectedType,
		const TArray<FString>& TemplateFields, FUeAgentResourceTracker& Resources,
		TMap<FString, FUeAgentCanonicalValueRef>& OutValues, FWorkspaceError& OutError,
		const TMap<FString, FString>* PublicRefs, FHjsonNodePtr* OutValueSources)
	{
		OutValues.Reset();
		if (OutValueSources) OutValueSources->Reset();
		const auto* Type = Row.Get(TEXT("type"));
		const auto* Value = Row.Get(TEXT("value"));
		if (!Type || Type->GetKind() != EHjsonNodeKind::String)
			return FailField(OutError, TEXT("semantic_workspace_property_type_missing"), TEXT("The property requires a string type field."),
				Row, TEXT("type"), MakeShared<FJsonValueString>(TEXT("string")));
		if (!Value)
			return FailField(OutError, TEXT("semantic_workspace_property_value_missing"), TEXT("The property requires a value field."), Row, TEXT("value"));
		if (!Type->GetText().TrimStartAndEnd().Equals(ExpectedType.TrimStartAndEnd(), ESearchCase::CaseSensitive))
			return FailNode(OutError, TEXT("semantic_workspace_property_type_mismatch"),
				TEXT("The authored property type differs from its declared type."), *Type, MakeShared<FJsonValueString>(ExpectedType));
		TMap<FString, FUeAgentCanonicalValueRef> Result;
		// The same owner that restores native aliases also binds those aliases
		// to their original parser nodes. Only the source tree is synthesized.
		const FHjsonNodeRef* ValueRef = nullptr;
		for (const auto& Field : Row.GetEntries())
			if (Field.Key == TEXT("value")) { ValueRef = &Field.Value; break; }
		TArray<TPair<FString, FHjsonNodeRef>> SourceFields;
		// Current exports use the native type. Legacy captured workspaces may still
		// carry a definition alias here; preserve their captured text encoding.
		const bool bTextType = ExpectedType == TEXT("text")
			|| ExpectedType.StartsWith(TEXT("text#"));
		if (bTextType && Value->GetKind() == EHjsonNodeKind::String)
		{
			FUeAgentCanonicalValuePtr Parsed;
			if (!TryCanonicalValue(*Value, Resources, Parsed, OutError, PublicRefs)) return false;
			// Pin defaults capture text in a direct value wrapper; native FText
			// properties use literal/localized. Preserve the captured owner shape.
			const FString TextField = TemplateFields.Num() == 1
				&& TemplateFields[0] == TEXT("value") ? TEXT("value") : TEXT("literal");
			Result.Add(TextField, Parsed.ToSharedRef());
			if (OutValueSources && ValueRef) SourceFields.Emplace(TextField, *ValueRef);
		}
		else if (!bTextType && TemplateFields.Num() == 1
			&& UeAgentAuthoringTypeDescriptors::IsDirectPropertyValueWrapper(TemplateFields[0])
			&& !Value->Get(TemplateFields[0])
			&& !(TemplateFields[0] == TEXT("path") && Value->Get(TEXT("owned_object"))))
		{
			// This is the captured PropertySet's outer values envelope, never a
			// recursive business object. Native descendants are restored by the
			// type-aware semantic overlay; this boundary consumes no nested names.
			FUeAgentCanonicalValuePtr Parsed;
			if (!TryCanonicalValue(*Value, Resources, Parsed, OutError, PublicRefs)) return false;
			Result.Add(TemplateFields[0], Parsed.ToSharedRef());
			if (OutValueSources && ValueRef) SourceFields.Emplace(TemplateFields[0], *ValueRef);
		}
		else
		{
			if (Value->GetKind() != EHjsonNodeKind::Object)
				return FailNode(OutError, TEXT("semantic_workspace_unit_values_invalid"),
					TEXT("A complex property value must be an object."), *Value, MakeShared<FJsonValueString>(TEXT("object")));
			for (const auto& Field : Value->GetEntries())
			{
				FUeAgentCanonicalValuePtr Parsed;
				if (Result.Contains(Field.Key))
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_unit_values_invalid"),
						TEXT("The semantic values contain a duplicate field name."), TEXT("workspace_resolve"));
					Diagnostic.CandidateName = Field.Key;
					return FailAtNode(OutError, MoveTemp(Diagnostic), *Field.Value);
				}
				if (!TryCanonicalValue(*Field.Value, Resources, Parsed, OutError, PublicRefs)) return false;
				Result.Add(Field.Key, Parsed.ToSharedRef());
				if (OutValueSources) SourceFields.Emplace(Field.Key, Field.Value);
			}
		}
		OutValues = MoveTemp(Result);
		if (OutValueSources)
		{
			// Existing document bindings may name /value; retain that alias too.
			if (ValueRef && !SourceFields.ContainsByPredicate([](const auto& Field) { return Field.Key == TEXT("value"); }))
				SourceFields.Emplace(TEXT("value"), *ValueRef);
			*OutValueSources = MakeShared<FHjsonNode>(EHjsonNodeKind::Object, Row.GetProvenance(),
				FString(), FString(), false, TArray<FHjsonNodeRef>(), MoveTemp(SourceFields));
		}
		return true;
	}

	bool TryProjectHjsonCanonicalValue(const FHjsonNode& Root, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError, bool bInterpretSemanticValues)
	{
		return TryCanonicalValue(Root, Resources, OutValue, OutError, nullptr, bInterpretSemanticValues);
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
