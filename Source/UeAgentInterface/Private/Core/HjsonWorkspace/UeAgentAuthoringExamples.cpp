#include "Core/HjsonWorkspace/UeAgentAuthoringExamples.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringContentLayout.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/UeAgentSplineValue.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Components/SplineComponent.h"
#include "Curves/SimpleCurve.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentReadableName.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "Async/ParallelFor.h"
#include "Async/TaskGraphInterfaces.h"

namespace UE::UAI::HjsonWorkspace
{
	using namespace StateSemantic;
	// Only compact guide instructions and immutable published bytes survive a
	// call. The document identity is weak; no canonical subtree or UObject is held.
	struct FAuthoringExamplePublicationProof
	{
		struct FLookup { FString Ref, Name, Result; };
		static int64 LookupBytes(const TArray<FLookup>& Values)
		{
			int64 Bytes = Values.GetAllocatedSize();
			for (const auto& Value : Values)
				Bytes += Value.Ref.GetAllocatedSize() + Value.Name.GetAllocatedSize() + Value.Result.GetAllocatedSize();
			return Bytes;
		}
		struct FLookupGroup {
			TArray<FLookup> Values;
			int64 Charge() const { return sizeof(FLookupGroup) + 64 + LookupBytes(Values); }
		};
		struct FEntry { FString Pointer; int32 Lookup = INDEX_NONE; };
		struct FEntryGroup {
			TSharedPtr<const FLookupGroup> Lookups;
			TArray<FEntry> Values;
			uint32 Key = 0;
			int64 Charge() const {
				int64 Bytes = sizeof(FEntryGroup) + 64 + Values.GetAllocatedSize();
				for (const auto& Value : Values) Bytes += Value.Pointer.GetAllocatedSize();
				return Bytes;
			}
		};
		struct FDocument
		{
			TWeakPtr<const FUeAgentCanonicalValue> Identity;
			FString SourceContentHash;
			TArray<FLookup> Lookups;
			TSharedPtr<const FLookupGroup> LookupGroup;
			const TArray<FLookup>& ReadLookups() const { return LookupGroup ? LookupGroup->Values : Lookups; }
			TArray<FEntry> Entries;
			TSharedPtr<const FEntryGroup> EntryGroup;
			const TArray<FEntry>& ReadEntries() const { return EntryGroup ? EntryGroup->Values : Entries; }
			// Composition already owns the actual immutable file plan/bytes. Keep
			// its exact lookup identity here, not a second guide byte owner/charge.
			FString FilePath, FileHash;
			int64 Charge() const
			{
				// Shared lookup/instruction storage is charged once by the publication owner.
				int64 Bytes = sizeof(FDocument) + LookupBytes(Lookups) + Entries.GetAllocatedSize();
				for (const auto& Entry : Entries) Bytes += Entry.Pointer.GetAllocatedSize();
				Bytes += SourceContentHash.GetAllocatedSize() + FilePath.GetAllocatedSize() + FileHash.GetAllocatedSize();
				return Bytes;
			}
		};
		TMap<FString, TSharedPtr<const FDocument>> Documents;
	};
	namespace
	{
		using FValue = TSharedPtr<FJsonValue>;
		using FObject = TSharedPtr<FJsonObject>;
		FValue Text(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
		FValue Number(double Value) { return MakeShared<FJsonValueNumber>(Value); }
		FValue Bool(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
		FValue Object(std::initializer_list<TPair<FString, FValue>> Fields)
		{
			const auto Result = MakeShared<FJsonObject>();
			for (const auto& Field : Fields) Result->SetField(Field.Key, Field.Value);
			return MakeShared<FJsonValueObject>(Result);
		}
		FValue Array(std::initializer_list<FValue> Values) { return MakeShared<FJsonValueArray>(TArray<FValue>(Values)); }
		FObject GetObject(const FObject& Object, const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject>* Value = nullptr;
			return Object && Object->TryGetObjectField(Field, Value) ? *Value : nullptr;
		}
		FString GetString(const FObject& Object, const TCHAR* Field)
		{
			FString Value; if (Object) Object->TryGetStringField(Field, Value); return Value;
		}
		FString Pointer(const FString& Value) { return Value.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); }
		FUeAgentCanonicalValuePtr Member(const FUeAgentCanonicalValuePtr& Value, const FString& Name)
		{
			if (Value && Value->GetKind() == EUeAgentValueKind::Object)
				for (const auto& Field : Value->GetObjectItems())
					if (Field.Key.Equals(Name, ESearchCase::CaseSensitive)) return Field.Value;
			return nullptr;
		}
		FString CanonicalText(const FUeAgentCanonicalValuePtr& Value)
		{ return Value && Value->GetKind() == EUeAgentValueKind::String ? Value->GetText() : FString(); }
		FString PropertyGuideReference(const FUeAgentCanonicalValuePtr& Value)
		{
			return Member(Value, TEXT("value")) ? CanonicalText(Member(Value, TEXT("definition"))) : FString();
		}
		bool IsGuideDocumentChild(const FString& Key)
		{
			return Key != TEXT("definitions") && Key != TEXT("value") && Key != TEXT("content_ref");
		}
		FValue Strings(const TArray<FString>& Values)
		{
			TArray<FValue> Items; for (const auto& Value : Values) Items.Add(Text(Value));
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}
		FValue Clone(const FValue& Value)
		{
			if (!Value) return nullptr;
			if (Value->Type == EJson::Object)
			{
				const auto Result = MakeShared<FJsonObject>();
				for (const auto& Field : Value->AsObject()->Values) Result->SetField(Field.Key, Clone(Field.Value));
				return MakeShared<FJsonValueObject>(Result);
			}
			if (Value->Type == EJson::Array)
			{
				TArray<FValue> Items; for (const auto& Item : Value->AsArray()) Items.Add(Clone(Item));
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			return Value;
		}

		struct FTypeGenerator
		{
			const FAuthoringExampleDefinitionResolver& Resolve;
			TSet<FString> Active;
			TArray<FString> Notes;

			FObject Descriptor(const FValue& Value)
			{
				if (!Value) return nullptr;
				if (Value->Type == EJson::Object) return Value->AsObject();
				return Value->Type == EJson::String ? Resolve(Value->AsString()) : nullptr;
			}
			FValue GenerateReference(const FValue& DescriptorValue)
			{
				if (DescriptorValue && DescriptorValue->Type == EJson::String)
				{
					const FString Ref = DescriptorValue->AsString();
					if (Active.Contains(Ref))
					{
						Notes.AddUnique(TEXT("Recursive members use their referenced definition; expand only the branch being authored."));
						return Text(TEXT("<value from ") + Ref + TEXT(">"));
					}
					Active.Add(Ref); const auto Result = Generate(Descriptor(DescriptorValue)); Active.Remove(Ref); return Result;
				}
				return Generate(Descriptor(DescriptorValue));
			}
			FValue Generate(const FObject& Definition)
			{
				if (!Definition) return Text(TEXT("<value from the declared element type>"));
				FObject Type = GetObject(Definition, TEXT("type"));
				if (!Type && Definition->HasField(TEXT("kind"))) Type = Definition;
				FObject Schema = Descriptor(Definition->TryGetField(TEXT("value_definition")));
				if (Schema && Schema->HasField(TEXT("value_definition"))) Schema = Descriptor(Schema->TryGetField(TEXT("value_definition")));
				const FString Kind = GetString(Type, TEXT("kind"));
				FString Builtin = GetString(Definition, TEXT("builtin"));
				if (Builtin.IsEmpty()) Builtin = GetString(Schema, TEXT("builtin"));
				if (Builtin == TEXT("owned_object"))
				{
					Notes.AddUnique(TEXT("Use asset.owned_candidates at this property's native path; replace <candidate name> with the returned name and optional select. New items omit instance; retained items keep it."));
					return Object({{TEXT("owned_object"), Object({{TEXT("name"), Text(TEXT("<candidate name>"))}})}});
				}
				if (Kind == TEXT("array") || Kind == TEXT("set") || Kind == TEXT("fixed_array"))
				{
					FValue Element;
					if (GetString(Schema, TEXT("element_kind")) == TEXT("owned_object"))
						Element = Generate(Object({{TEXT("builtin"), Text(TEXT("owned_object"))}})->AsObject());
					else if (Schema && Schema->HasField(TEXT("element_definition"))) Element = GenerateReference(Schema->TryGetField(TEXT("element_definition")));
					else Element = Generate(GetObject(Type, TEXT("element_type")));
					if (Kind == TEXT("set") && GetString(Type, TEXT("hash_source")) == TEXT("from_required_null_creates")
						&& Element->Type == EJson::Object) Element->AsObject()->SetField(TEXT("from"), MakeShared<FJsonValueNull>());
					TArray<FValue> Items;
					int32 Count = 1;
					if (Kind == TEXT("fixed_array")) Type->TryGetNumberField(TEXT("fixed_length"), Count);
					for (int32 Index = 0; Index < Count; ++Index) Items.Add(Clone(Element));
					return MakeShared<FJsonValueArray>(MoveTemp(Items));
				}
				if (Kind == TEXT("map"))
				{
					auto Entry = Object({{TEXT("key"), Generate(GetObject(Type, TEXT("key_type")))},
						{TEXT("value"), Generate(GetObject(Type, TEXT("mapped_type")))}});
					if (GetString(Type, TEXT("hash_source")) == TEXT("from_required_null_creates"))
						Entry->AsObject()->SetField(TEXT("from"), MakeShared<FJsonValueNull>());
					return Object({{TEXT("entries"), Array({Entry})}});
				}
				if (Kind == TEXT("optional")) return Object({{TEXT("is_set"), Bool(true)}, {TEXT("value"), Generate(GetObject(Type, TEXT("inner_type")))}});
				if (Kind == TEXT("bool")) return Bool(false);
				if (Kind.StartsWith(TEXT("int")) || Kind.StartsWith(TEXT("uint")) || Kind == TEXT("float") || Kind == TEXT("double"))
				{
					double Value = 0.; const auto Constraints = GetObject(Definition, TEXT("constraints"));
					if (Constraints)
					{
						const FString Min = GetString(Constraints, TEXT("clamp_min")), Max = GetString(Constraints, TEXT("clamp_max"));
						if (!Min.IsEmpty()) Value = FMath::Max(Value, FCString::Atod(*Min));
						if (!Max.IsEmpty()) Value = FMath::Min(Value, FCString::Atod(*Max));
					}
					return Number(Value);
				}
				if (Kind == TEXT("enum") || Kind == TEXT("bitmask"))
				{
					const TArray<FValue>* Candidates = nullptr;
					const auto* Enum = Type ? FindObject<UEnum>(nullptr, *GetString(Type, TEXT("enum_path"))) : nullptr;
					const auto EnumDefinition = Enum && Kind != TEXT("bitmask") ? UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(Enum) : nullptr;
					FValue Choice = Text(TEXT("<name from this definition's enum candidates>"));
					if ((Definition->TryGetArrayField(TEXT("candidates"), Candidates)
						|| (EnumDefinition && EnumDefinition->TryGetArrayField(TEXT("candidates"), Candidates))) && !Candidates->IsEmpty())
					{
						const auto& First = (*Candidates)[0];
						if (First->Type == EJson::String) Choice = First;
						else if (First->Type == EJson::Object) Choice = Text(GetString(First->AsObject(), TEXT("name")));
						else if (First->Type == EJson::Array && !First->AsArray().IsEmpty()) Choice = First->AsArray()[0];
					}
					return Kind == TEXT("bitmask") ? Array({Choice}) : Choice;
				}
				if (Kind == TEXT("name") || Kind == TEXT("string")) return Text(TEXT("Example"));
				if (Kind == TEXT("text") || Builtin == TEXT("text")) return Object({{TEXT("source"), Text(TEXT("Example"))}});
				if (Kind.EndsWith(TEXT("_ref")))
				{
					Notes.AddUnique(TEXT("Replace <reference> with a public reference valid for the declared class/type; null clears a nullable reference."));
					return Text(TEXT("<reference>"));
				}
				if (Kind == TEXT("vector2")) return Array({Number(0), Number(0)});
				if (Kind == TEXT("vector3") || Kind == TEXT("rotator")) return Array({Number(0), Number(0), Number(0)});
				if (Kind == TEXT("vector4")) return Array({Number(0), Number(0), Number(0), Number(0)});
				if (Kind == TEXT("quat") || Kind == TEXT("linear_color")) return Array({Number(0), Number(0), Number(0), Number(1)});
				if (Kind == TEXT("color8")) return Array({Number(0), Number(0), Number(0), Number(255)});
				if (Kind == TEXT("transform")) return Object({{TEXT("translation"), Array({Number(0), Number(0), Number(0)})},
					{TEXT("rotation"), Array({Number(0), Number(0), Number(0)})}, {TEXT("scale"), Array({Number(1), Number(1), Number(1)})}});
				if (Kind == TEXT("guid")) return Text(TEXT("00000000-0000-0000-0000-000000000000"));
				if (Kind == TEXT("datetime") || Kind == TEXT("timespan")) return Object({{TEXT("ticks"), Text(TEXT("0"))}});
				if (Kind == TEXT("gameplay_tag")) return Text(TEXT("<registered gameplay tag>"));
				if (Kind == TEXT("gameplay_tags")) return Array({Text(TEXT("<registered gameplay tag>"))});
				if (Kind == TEXT("instanced_struct"))
					return Object({{TEXT("struct_path"), Text(TEXT("<permitted struct path>"))}, {TEXT("fields"), Object({})}});
				if (Kind == TEXT("property_bag") || Builtin == TEXT("property_bag"))
					return Object({{TEXT("schema"), Array({Object({{TEXT("name"), Text(TEXT("Example"))}, {TEXT("kind"), Text(TEXT("float"))}})})},
						{TEXT("fields"), Object({{TEXT("Example"), Number(0)}})}});
				if (Kind == TEXT("pcg_selector"))
				{
					Notes.AddUnique(TEXT("Replace Example with the intended attribute name or a supported native selector expression."));
					return Text(TEXT("Example"));
				}
				if (Kind == TEXT("spline"))
				{
					FSplineCurves Curves;
					Curves.Position.Points.Add(FInterpCurvePointVector(0.f, FVector::ZeroVector));
					Curves.Position.Points[0].InterpMode = CIM_Linear;
					return MakeShared<FJsonValueObject>(UeAgentSplineValue::Encode(Curves));
				}
				// Logical codecs can already publish native templates (curves are one consumer).
				const auto Contract = GetObject(Schema, TEXT("contract"));
				const auto Templates = GetObject(Contract ? Contract : Schema, TEXT("value_templates"));
				if (Templates && !Templates->Values.IsEmpty())
				{
					TArray<FString> Names; Templates->Values.GetKeys(Names); Names.Sort();
					auto Example = Clone(Templates->TryGetField(Names.Contains(TEXT("embedded")) ? TEXT("embedded") : Names[0]));
					if (Example && Example->Type == EJson::Object)
						if (const auto Channels = GetObject(Example->AsObject(), TEXT("channels")))
							if (const auto Key = GetObject(Contract ? Contract : Schema, TEXT("key_defaults")))
								for (const auto& Channel : Channels->Values)
									if (Channel.Value->Type == EJson::Object)
										Channel.Value->AsObject()->SetArrayField(TEXT("keys"), {Clone(MakeShared<FJsonValueObject>(Key))});
					return Example;
				}
				if (GetString(Schema, TEXT("type")) == TEXT("RichCurve"))
				{
					FRichCurve Curve; Curve.AddKey(0.f, 0.f);
					return MakeShared<FJsonValueObject>(UeAgentCurveJson::BuildFloatCurveJson(Curve, {}, {}, true, {}));
				}
				if (GetString(Schema, TEXT("type")) == TEXT("SimpleCurve"))
				{
					FSimpleCurve Curve; Curve.AddKey(0.f, 0.f);
					return MakeShared<FJsonValueObject>(UeAgentCurveJson::BuildSimpleCurveJson(Curve, {}, {}, true, {}));
				}
				FObject Fields = GetObject(Schema, TEXT("fields"));
				if (!Fields) Fields = GetObject(Definition, TEXT("fields"));
				// Empty set/map/optional children may have no value-dependent schema.
				// Their already-loaded native reflection is the same type authority.
				const FString StructPath = GetString(Type, TEXT("struct_path"));
				if (!Fields && Kind == TEXT("struct") && !Active.Contains(StructPath))
				{
					if (const auto* Struct = FindObject<UScriptStruct>(nullptr, *StructPath))
					{
						Fields = MakeShared<FJsonObject>();
						for (TFieldIterator<FProperty> It(Struct); It; ++It)
						{
							if (!UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(*It, Struct)) continue;
							const auto Child = MakeShared<FJsonObject>();
							Child->SetObjectField(TEXT("type"), UeAgentPropertyValueCodec::BuildTypeDescriptor(*It));
							Fields->SetObjectField(It->GetName(), Child);
						}
					}
				}
				if (Fields)
				{
					TArray<FString> Names; Fields->Values.GetKeys(Names); Names.Sort();
					const auto Members = MakeShared<FJsonObject>();
					// Sparse native structs retain native defaults. One member demonstrates
					// the recursive shape without expanding hundreds of unrelated settings.
					const bool WasActive = Active.Contains(StructPath);
					if (!StructPath.IsEmpty()) Active.Add(StructPath);
					for (const auto& Name : Names)
					{
						const auto Child = Descriptor(Fields->TryGetField(Name));
						bool Editable = true; if (Child) Child->TryGetBoolField(TEXT("editable"), Editable);
						if (!Editable) continue;
						Members->SetField(Name, GenerateReference(Fields->TryGetField(Name))); break;
					}
					if (!WasActive) Active.Remove(StructPath);
					Notes.AddUnique(TEXT("This sparse struct example selects one editable member. Other members and their types remain in definitions; omitted members use the native creation/default or existing partial-edit semantics."));
					return Object({{TEXT("fields"), MakeShared<FJsonValueObject>(Members)}});
				}
				Notes.AddUnique(TEXT("This type needs its declared native/logical contract; placeholders are instructions to fill, not importable literal values."));
				return Text(TEXT("<value according to ") + (Kind.IsEmpty() ? GetString(Type, TEXT("type_id")) : Kind) + TEXT(" definition>"));
			}
		};

		FObject Recipe(const FAuthoringExample& Example)
		{
			const auto Result = MakeShared<FJsonObject>();
			Result->SetField(TEXT("example"), Example.Value);
			if (!Example.Notes.IsEmpty()) Result->SetField(TEXT("notes"), Strings(Example.Notes));
			return Result;
		}
	}

	TSharedRef<FJsonObject> BuildAuthoringTypeExamples(const TSharedRef<FJsonObject>& Definition,
		const FAuthoringExampleDefinitionResolver& ResolveDefinition)
	{
		FTypeGenerator Generator{ResolveDefinition, {}, {}};
		const auto Result = MakeShared<FJsonObject>();
		if (const auto Type = Definition->TryGetField(TEXT("type"))) Result->SetField(TEXT("type"), Clone(Type));
		else if (Definition->HasField(TEXT("kind"))) Result->SetObjectField(TEXT("type"), Definition);
		Result->SetField(TEXT("example"), Generator.Generate(Definition));
		if (!Generator.Notes.IsEmpty()) Result->SetField(TEXT("notes"), Strings(Generator.Notes));
		return Result;
	}

	TArray<FAuthoringExample> BuildKeyedItemExamples(const FKeyedItemProjectionContract& Contract,
		const TSharedPtr<FJsonObject>& RecordSchema)
	{
		if (!Contract.bAllowNewItems) return {{TEXT("existing_items"), Object({}),
			{TEXT("This collection's members are native-defined. Edit existing entries; new parent items create their own native children.")}}};
		if (!Contract.CreationExamples.IsEmpty()) return Contract.CreationExamples;
		if (auto DomainExamples = BuildDomainKeyedItemExamples(Contract); !DomainExamples.IsEmpty()) return DomainExamples;
		FValue Row = Object({});
		TArray<FString> Notes;
		if (!Contract.RecordSchemaField.IsEmpty())
		{
			Row->AsObject()->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
			Notes.Add(TEXT("Native row_schema.properties supplies omitted creation defaults. Add the desired typed property rows under properties; the outer key is the new row name."));
		}
		else if (!Contract.bIdentityFromMapKey && Contract.bRequiresCreationCandidate)
		{
			Row->AsObject()->SetStringField(TEXT("name"), TEXT("<candidate name>"));
			Notes.Add(TEXT("Query candidates for this target and collection context. Copy the returned name and optional select. The outer key names the new instance; placement and initial properties belong to the row, not the selector."));
		}
		else if (RecordSchema)
		{
			// A native typed-record schema already supplies valid creation defaults.
			if (const auto Properties = GetObject(RecordSchema, TEXT("properties")))
				Row->AsObject()->SetField(TEXT("properties"), Clone(MakeShared<FJsonValueObject>(Properties)));
		}
		else
		{
			Notes.Add(TEXT("Use this collection's declared fields below; context-dependent required values must be supplied before import."));
			TArray<FString> Fields = Contract.VisibleFieldPolicy.VisibleTopLevelFields.Array(); Fields.Sort();
			Notes.Add(TEXT("Declared public fields: ") + FString::Join(Fields, TEXT(", ")));
		}
		return {{TEXT("new_item"), Object({{TEXT("NewItem"), Row}}), Notes}};
	}

	void FAuthoringExampleCatalog::CollectPropertyGuideReferences(const FUeAgentCanonicalValueRef& Document, TSet<FString>& OutReferences)
	{
		if (Document->GetKind() != EUeAgentValueKind::Object) return;
		const FString Reference = PropertyGuideReference(Document);
		if (!Reference.IsEmpty()) OutReferences.Add(Reference);
		for (const auto& Field : Document->GetObjectItems())
			if (IsGuideDocumentChild(Field.Key)) CollectPropertyGuideReferences(Field.Value, OutReferences);
	}

	void FAuthoringExampleCatalog::AddDefinitions(const FString& Path, const TSharedRef<FJsonObject>& Catalog, const TSharedPtr<FJsonObject>& Enums)
	{
		const auto Local = MakeShared<FJsonObject>();
		for (const auto& Entry : Catalog->Values)
		{
			if (!Entry.Value || Entry.Value->Type != EJson::Object) continue;
			const auto Definition = Clone(Entry.Value)->AsObject();
			if (Enums && Enums->HasField(Entry.Key)) Definition->SetField(TEXT("candidates"), Enums->TryGetField(Entry.Key));
			Local->SetObjectField(Entry.Key, Definition);
			const FString Ref = GetString(Definition, TEXT("ref"));
			if (!Ref.IsEmpty()) Definitions.Add(Ref, Definition);
		}
		DefinitionCatalogs.Add(FPaths::GetPath(Path) + TEXT("/"), Local);
	}

	void FAuthoringExampleCatalog::AddPropertyDocument(const FString& Path, TArray<FPropertyInstruction>&& Properties)
	{
		// Preserve the original canonical category/field encounter order without
		// constructing the former nested JSON/canonical placeholder document.
		Properties.Sort([](const auto& A, const auto& B) {
			const int32 Category = A.Category.Compare(B.Category, ESearchCase::CaseSensitive);
			return Category != 0 ? Category < 0 : A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0;
		});
		Documents.Add({Path, {}, {}, {}, nullptr, MoveTemp(Properties)});
	}

	void FAuthoringExampleCatalog::AddDocument(const FString& Path, const FUeAgentCanonicalValueRef& Document,
		const FString& SemanticType, const FString& Role, const FString& Field)
	{
		if (!Path.StartsWith(TEXT(".uai/"))) Documents.Add({Path, SemanticType, Role, Field, Document});
	}
	void FAuthoringExampleCatalog::AddRule(FAuthoringDocumentExampleRule Rule) { Rules.Add(MoveTemp(Rule)); }
	void FAuthoringExampleCatalog::AddCollection(const FString& Path, const FUeAgentCanonicalValueRef& Document,
		const FKeyedItemProjectionContract& Contract)
	{
		Collections.Add({Path, Document, MakeShared<FKeyedItemProjectionContract>(Contract)});
	}

	bool FAuthoringExampleCatalog::Publish(TArray<FExportFilePlan>& Files, FWorkspaceError& OutError,
		TFunctionRef<FString(const TArray<uint8>&)> Hash,
		const TSharedPtr<const FAuthoringExamplePublicationProof>& Previous,
		TSharedPtr<const FAuthoringExamplePublicationProof>* OutProof,
		const TMap<FString, const FExportFilePlan*>* PreviousFiles,
		const TSharedPtr<FJsonObject>& PreviousIndex, const TSet<FString>* ReplacedDocuments,
		const TMap<FString, FUeAgentCanonicalValueRef>* PreviousGuides,
		const TMap<FString, TArray<FString>>* ReplacedPointers,
		TMap<FString, FUeAgentCanonicalValueRef>* ProducedGuides)
	{
		const double PublicationStart = FPlatformTime::Seconds();
		if (OutProof) OutProof->Reset();
		using FGuideProof = FAuthoringExamplePublicationProof::FDocument;
		TMap<FString, TSharedPtr<const FGuideProof>> ReusedGuides;
		TMap<FString, TSharedPtr<const FGuideProof>> ReusedEmptyDocuments;
		TMap<FString, TSharedPtr<FGuideProof>> CandidateGuides;
		const bool bTrackGuideExclusivity = Previous.IsValid() || OutProof != nullptr;
		TSet<FString> NonExclusiveGuides;
		FGuideProof* Recording = nullptr;
		TMap<FString, int32> RecordedLookups;
		FString CurrentDocument;
		int32 ReusedGuideCount = 0;
		int32 ReusedEmptyCount = 0, VisitedDocuments = 0, GeneratedGuideFiles = 0;
		int64 VisitedNodes = 0, SetGuideCalls = 0, GuidePathComparisons = 0, DirectPropertyInstructions = 0;
		int64 CandidateInstructionBytes = 0;
		const auto Index = MakeShared<FJsonObject>();
		Index->SetBoolField(TEXT("read_only"), true);
		Index->SetStringField(TEXT("usage"), TEXT("Read only the guide for the file being edited. Copy example values into business HJSON, replacing <...> using current queries/definitions. Examples are not defaults or required fields. Preserve retained identities; new items omit captured instance/definition IDs."));
		const auto FileIndex = MakeShared<FJsonObject>();
		if (PreviousIndex) if (const auto Before = GetObject(PreviousIndex, TEXT("files")))
			for (const auto& File : Before->Values)
				if (!ReplacedDocuments || !ReplacedDocuments->Contains(File.Key)) FileIndex->SetField(File.Key, File.Value);
		auto Resources = MakeSemanticWorkspaceResourceTracker();
		using FGuideEntries = TArray<TPair<FString, FUeAgentCanonicalValueRef>>;
		TMap<FString, FGuideEntries> Guides;
		// Hashes select candidates only. Exact path comparison and encounter order
		// remain authoritative, including case-only paths and hash collisions.
		using FGuideEntryIndex = TMap<uint32, TArray<int32, TInlineAllocator<1>>>;
		TMap<FString, FGuideEntryIndex> GuideEntryIndices;
		TMap<FString, FUeAgentCanonicalValueRef> DocumentValues;
		// These are the formatter's already calculated hashes for actual output
		// documents. Do not serialize or hash another representation for guides.
		TMap<FString, FString> SourceHashes;
		if (bTrackGuideExclusivity)
			for (const auto& File : Files) SourceHashes.Add(File.GetRelativePath(), File.GetContentHash());
		const auto FindPreviousGuide = [&](const FGuideProof& Proof) -> const FExportFilePlan* {
			const auto* Found = PreviousFiles && !Proof.FilePath.IsEmpty() ? PreviousFiles->Find(Proof.FilePath) : nullptr;
			return Found && *Found && (*Found)->GetContentHash().Equals(Proof.FileHash, ESearchCase::CaseSensitive) ? *Found : nullptr;
		};
		for (const auto& Document : Documents) {
			if (bTrackGuideExclusivity && DocumentValues.Contains(Document.Path)) NonExclusiveGuides.Add(Document.Path);
			if (Document.Value) DocumentValues.Add(Document.Path, Document.Value.ToSharedRef());
		}
		auto SetCanonicalGuide = [&](const FString& File, const FString& Path, const FUeAgentCanonicalValueRef& Value, bool bTypeEntry = false)
		{
			++SetGuideCalls;
			if (bTrackGuideExclusivity) {
				if (!bTypeEntry || File != CurrentDocument) NonExclusiveGuides.Add(File);
				if (File != CurrentDocument) NonExclusiveGuides.Add(CurrentDocument);
			}
			// A later rule/collection/other document may contribute to the same
			// file. Expand its compact original entries before applying the write,
			// preserving the old encounter/overwrite order without parsing bytes.
			if (const auto* Reused = ReusedGuides.Find(File))
			{
				auto& Restored = Guides.FindOrAdd(File);
				for (const auto& Entry : (*Reused)->ReadEntries())
				{
					GuideEntryIndices.FindOrAdd(File).FindOrAdd(GetTypeHash(Entry.Pointer)).Add(Restored.Num());
					FUeAgentCanonicalValuePtr TextValue; FUeAgentStateSemanticError Error;
					if (!FUeAgentCanonicalValue::TryString((*Reused)->ReadLookups()[Entry.Lookup].Result, Resources, TextValue, Error)) {
						OutError.SetFailure(Error, File); return;
					}
					Restored.Emplace(Entry.Pointer, TextValue.ToSharedRef());
				}
				ReusedGuides.Remove(File);
			}
			auto& Entries = Guides.FindOrAdd(File);
			auto& Positions = GuideEntryIndices.FindOrAdd(File).FindOrAdd(GetTypeHash(Path));
			for (const int32 Position : Positions)
			{
				++GuidePathComparisons;
				if (Entries[Position].Key.Equals(Path, ESearchCase::CaseSensitive))
				{ Entries[Position].Value = Value; return; }
			}
			Positions.Add(Entries.Num()); Entries.Emplace(Path, Value);
		};
		auto SetGuide = [&](const FString& File, const FString& Path, const FValue& Value, bool bTypeEntry = false) {
			FUeAgentCanonicalValuePtr Canonical; Contract::FDiagnostic Error;
			if (!CanonicalJson::TryFromJsonValue(Value, Resources, Canonical, Error)) { OutError.SetFailure(Error, File); return; }
			SetCanonicalGuide(File, Path, Canonical.ToSharedRef(), bTypeEntry);
		};

		// Navigate exactly the content references emitted by the existing organizer.
		auto FollowContent = [&](FUeAgentCanonicalValuePtr& Value, FString& File, FString& PointerPath)
		{
			TSet<FString> Seen;
			while (Value && Value->GetKind() == EUeAgentValueKind::Object)
			{
				const FString Target = CanonicalText(Member(Value, TEXT("content_ref")));
				const auto* Content = DocumentValues.Find(Target);
				if (!Content || Seen.Contains(Target)) break;
				Seen.Add(Target); File = Target; PointerPath = TEXT("/value");
				Value = Member(*Content, TEXT("value"));
			}
		};
		TMap<FString, FString> SharedRecipes;
		// A definition and the resolver's document scope are immutable throughout
		// this publication. Repeated fields of that type otherwise rebuild and
		// serialize the very same recipe before SharedRecipes can deduplicate it.
		struct FResolvedRecipe { FString Path; FUeAgentCanonicalValuePtr GuideText; };
		TMap<const FJsonObject*, TMap<FString, FResolvedRecipe>> GlobalTypeRecipes;
		int32 RecipeTextValues = 0;
		int32 ReusedTypeRecipes = 0;
		int32 ReusedGlobalRecipes = 0;
		TSet<FString> UsedPaths;
		// A scope is the COMPLETE ordered fallback chain, not only the longest
		// catalog. A missing alias in that catalog must still consult its parents.
		struct FLookupScope {
			TArray<FObject> Catalogs;
			TMap<FString, FObject> Definitions;
			TMap<FString, TMap<FString, FResolvedRecipe>> Recipes;
			TMap<const FJsonObject*, TMap<FString, FResolvedRecipe>> Types;
			TMap<const FAuthoringExamplePublicationProof::FLookupGroup*, bool> Groups;
		};
		TArray<TPair<FString, FObject>> Catalogs;
		for (const auto& Catalog : DefinitionCatalogs) Catalogs.Emplace(Catalog.Key, Catalog.Value);
		Catalogs.StableSort([](const auto& A, const auto& B) { return A.Key.Len() > B.Key.Len(); });
		TMap<FString, TSharedPtr<FLookupScope>> Scopes, DirectoryScopes;
		TSharedPtr<FLookupScope> CurrentScope;
		int32 ReusedLookupGroups = 0, ReusedScopedLookups = 0;
		auto SelectScope = [&]() {
			const FString Directory = FPaths::GetPath(CurrentDocument);
			if (const auto* Existing = DirectoryScopes.Find(Directory)) { CurrentScope = *Existing; return; }
			FString Key; TArray<FObject> Chain;
			for (int32 I = 0; I < Catalogs.Num(); ++I) if (CurrentDocument.StartsWith(Catalogs[I].Key)) {
				Key.AppendInt(I); Key.AppendChar(TEXT(';')); Chain.Add(Catalogs[I].Value);
			}
			auto& Scope = Scopes.FindOrAdd(Key);
			if (!Scope) { Scope = MakeShared<FLookupScope>(); Scope->Catalogs = MoveTemp(Chain); }
			CurrentScope = Scope; DirectoryScopes.Add(Directory, Scope);
		};
		bool bScopedRecipeResolution = false;
		const auto Resolve = [&](const FString& Ref) -> FObject
		{
			if (const auto* Found = Definitions.Find(Ref)) return *Found;
			// Human aliases are scoped to the owning catalog, unlike nested public refs.
			bScopedRecipeResolution = true;
			if (const auto* Existing = CurrentScope->Definitions.Find(Ref)) return *Existing;
			FObject Found;
			for (const auto& Catalog : CurrentScope->Catalogs)
				if ((Found = GetObject(Catalog, *Ref))) break;
			CurrentScope->Definitions.Add(Ref, Found); return Found;
		};
		auto PublishObject = [&](const FString& Path, const TSharedRef<FJsonObject>& Value) -> bool
		{
			TArray<uint8> Bytes;
			if (!TrySerializeSemanticHjsonObject(Value, Bytes, OutError, Path)) return false;
			Files.Emplace(Path, Bytes, Hash(Bytes));
			return true;
		};
		auto Share = [&](const FString& Name, const TSharedRef<FJsonObject>& Value, FString& OutPath) -> bool
		{
			TArray<uint8> Bytes;
			if (!TrySerializeSemanticHjsonObject(Value, Bytes, OutError, CurrentDocument)) return false;
			const FString Identity = Hash(Bytes);
			if (const auto* Found = SharedRecipes.Find(Identity)) { OutPath = *Found; return true; }
			const FString Stem = TEXT("examples/types/") + MakeAuthoringContentPathSegment(Name);
			// These are generated reading aids, reached through the current guide.
			// Their already-computed recipe identity is stable across encounter order;
			// sequential # aliases otherwise rewrite unrelated guides after an edit.
			OutPath = AllocateReadableName(Stem + TEXT("--") + Identity.Right(24),
				[&](const FString& Candidate) { return UsedPaths.Contains(Candidate.ToLower()); }) + TEXT(".hjson");
			UsedPaths.Add(OutPath.LeftChop(6).ToLower());
			SharedRecipes.Add(Identity, OutPath);
			Files.Emplace(OutPath, Bytes, Identity);
			return true;
		};
		// Evaluate the authoritative generator in the current lookup scope. A
		// cached guide proves only its pointers and recipe *paths*, never native
		// type metadata or the current contents of a shared recipe.
		auto ResolveRecipe = [&](const FString& Ref, const FString& RecipeName, const FResolvedRecipe*& Result) -> bool
		{
			auto& ScopedResults = CurrentScope->Recipes.FindOrAdd(Ref);
			if (const auto* Existing = ScopedResults.Find(RecipeName)) {
				Result = Existing; ++ReusedScopedLookups; return true;
			}
			const auto Remember = [&](FResolvedRecipe Value) { Result = &ScopedResults.Add(RecipeName, MoveTemp(Value)); return true; };
			const auto Definition = Resolve(Ref);
			if (!Definition) return Remember({TEXT("@missing"), {}});
			bool Editable = true; Definition->TryGetBoolField(TEXT("editable"), Editable);
			if (!Editable) return Remember({TEXT("@readonly"), {}});
			auto& GlobalRecipes = GlobalTypeRecipes.FindOrAdd(Definition.Get());
			if (const auto* Existing = GlobalRecipes.Find(RecipeName)) {
				++ReusedTypeRecipes; ++ReusedGlobalRecipes; return Remember(*Existing);
			}
			auto& TypeRecipes = CurrentScope->Types.FindOrAdd(Definition.Get());
			if (const auto* Existing = TypeRecipes.Find(RecipeName)) { ++ReusedTypeRecipes; return Remember(*Existing); }
			FResolvedRecipe Value;
			bScopedRecipeResolution = false;
			if (!Share(RecipeName, BuildAuthoringTypeExamples(Definition.ToSharedRef(), Resolve), Value.Path)) return false;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(Value.Path, Resources, Value.GuideText, Error)) {
				OutError.SetFailure(Error, CurrentDocument); return false;
			}
			++RecipeTextValues;
			TypeRecipes.Add(RecipeName, Value);
			if (!bScopedRecipeResolution) GlobalRecipes.Add(RecipeName, Value);
			return Remember(MoveTemp(Value));
		};

		using FLookupGroup = FAuthoringExamplePublicationProof::FLookupGroup;
		using FEntryGroup = FAuthoringExamplePublicationProof::FEntryGroup;
		TMap<uint32, TArray<TSharedPtr<const FLookupGroup>>> SealedGroups;
		TMap<uint32, TArray<TSharedPtr<const FEntryGroup>>> SealedEntries;
		TSet<const FEntryGroup*> CandidateEntryGroups;
		if (Previous) for (const auto& Pair : Previous->Documents)
			if (const auto& Group = Pair.Value->EntryGroup) SealedEntries.FindOrAdd(Group->Key).AddUnique(Group);
		auto SealEntries = [&](FGuideProof& Proof) {
			if (Proof.Entries.IsEmpty()) return;
			uint32 Key = PointerHash(Proof.LookupGroup.Get());
			int64 MutableBytes = Proof.Entries.GetAllocatedSize();
			for (const auto& Entry : Proof.Entries) {
				Key = HashCombineFast(Key, HashCombineFast(GetTypeHash(Entry.Pointer), GetTypeHash(Entry.Lookup)));
				MutableBytes += Entry.Pointer.GetAllocatedSize();
			}
			auto& Candidates = SealedEntries.FindOrAdd(Key);
			for (const auto& Group : Candidates) {
				bool bSame = Group->Lookups == Proof.LookupGroup && Group->Values.Num() == Proof.Entries.Num();
				for (int32 I = 0; bSame && I < Proof.Entries.Num(); ++I)
					bSame = Group->Values[I].Lookup == Proof.Entries[I].Lookup
						&& Group->Values[I].Pointer.Equals(Proof.Entries[I].Pointer, ESearchCase::CaseSensitive);
				if (bSame) { Proof.EntryGroup = Group; break; }
			}
			CandidateInstructionBytes -= MutableBytes;
			if (!Proof.EntryGroup) {
				auto Group = MakeShared<FEntryGroup>(); Group->Lookups = Proof.LookupGroup;
				Group->Values = MoveTemp(Proof.Entries); Group->Key = Key;
				Proof.EntryGroup = Group; Candidates.Add(MoveTemp(Group));
			} else Proof.Entries.Empty();
			if (!CandidateEntryGroups.Contains(Proof.EntryGroup.Get())) {
				CandidateEntryGroups.Add(Proof.EntryGroup.Get());
				CandidateInstructionBytes += Proof.EntryGroup->Charge();
			}
		};
		auto SealLookups = [&](FGuideProof& Proof) {
			uint32 Key = 0;
			for (const auto& L : Proof.Lookups) Key = HashCombineFast(Key, HashCombineFast(GetTypeHash(L.Ref), HashCombineFast(GetTypeHash(L.Name), GetTypeHash(L.Result))));
			auto& Candidates = SealedGroups.FindOrAdd(Key);
			for (const auto& Group : Candidates) {
				bool bEqual = Group->Values.Num() == Proof.Lookups.Num();
				for (int32 I = 0; bEqual && I < Proof.Lookups.Num(); ++I) {
					const auto& A = Group->Values[I]; const auto& B = Proof.Lookups[I];
					bEqual = A.Ref.Equals(B.Ref, ESearchCase::CaseSensitive) && A.Name.Equals(B.Name, ESearchCase::CaseSensitive)
						&& A.Result.Equals(B.Result, ESearchCase::CaseSensitive);
				}
				if (bEqual) {
					CandidateInstructionBytes -= FAuthoringExamplePublicationProof::LookupBytes(Proof.Lookups);
					Proof.LookupGroup = Group; Proof.Lookups.Empty(); return;
				}
			}
			auto Group = MakeShared<FLookupGroup>(); Group->Values = MoveTemp(Proof.Lookups);
			CandidateInstructionBytes += sizeof(FLookupGroup) + 64;
			Proof.LookupGroup = Group; Candidates.Add(MoveTemp(Group));
		};
		const double DocumentsStart = FPlatformTime::Seconds();
		for (const auto& Document : Documents)
		{
			CurrentDocument = Document.Path;
			SelectScope();
			Recording = nullptr;
			RecordedLookups.Reset();
			bool bReused = false;
			if (Previous && !Guides.Contains(Document.Path) && !NonExclusiveGuides.Contains(Document.Path))
				if (const auto* Prior = Previous->Documents.Find(Document.Path);
					Prior && ((Document.Value && (*Prior)->Identity.Pin().Get() == Document.Value.Get())
						|| (!(*Prior)->SourceContentHash.IsEmpty()
							&& SourceHashes.FindRef(Document.Path).Equals((*Prior)->SourceContentHash, ESearchCase::CaseSensitive))))
				{
					bool bSame = true;
					const auto* Group = (*Prior)->LookupGroup.Get();
					if (const bool* Verified = Group ? CurrentScope->Groups.Find(Group) : nullptr) {
						bSame = *Verified; ++ReusedLookupGroups;
					} else {
						for (const auto& Lookup : (*Prior)->ReadLookups()) {
							const FResolvedRecipe* Actual = nullptr;
							if (!ResolveRecipe(Lookup.Ref, Lookup.Name, Actual)) return false;
							if (!Actual->Path.Equals(Lookup.Result, ESearchCase::CaseSensitive)) { bSame = false; break; }
						}
						if (Group) CurrentScope->Groups.Add(Group, bSame);
					}
					if (bSame && FindPreviousGuide(**Prior)) {
						ReusedGuides.Add(Document.Path, *Prior); bReused = true; ++ReusedGuideCount;
					} else if (bSame && (*Prior)->FilePath.IsEmpty() && (*Prior)->ReadEntries().IsEmpty()) {
						ReusedEmptyDocuments.Add(Document.Path, *Prior); bReused = true; ++ReusedEmptyCount;
					}
				}
			if (!bReused && OutProof && !NonExclusiveGuides.Contains(Document.Path)
				&& CandidateGuides.Num() < 8192 && CandidateInstructionBytes < 32 * 1024 * 1024)
			{
				auto Candidate = MakeShared<FGuideProof>();
				Candidate->Identity = Document.Value;
				Candidate->SourceContentHash = SourceHashes.FindRef(Document.Path);
				CandidateInstructionBytes += sizeof(FGuideProof) + Document.Path.GetAllocatedSize() + 128;
				Recording = &Candidate.Get(); CandidateGuides.Add(Document.Path, MoveTemp(Candidate));
			}
			auto PropertyGuide = [&](const FString& Ref, const FString& TypeName, const FString& Path,
				FUeAgentCanonicalValuePtr ValueToEdit, bool* bSkipChildren = nullptr) -> bool
			{
				if (!Ref.IsEmpty())
				{
					const FString& RecipeName = TypeName.IsEmpty() ? Ref : TypeName;
					const FResolvedRecipe* RecipeResult = nullptr;
					if (!ResolveRecipe(Ref, RecipeName, RecipeResult)) return false;
					// Borrow until this immediate consumer; no resolver/table mutation occurs below.
					const FString& ExamplePath = RecipeResult->Path;
					int32 LookupIndex = INDEX_NONE;
					if (Recording)
					{
						const FString Key = FString::FromInt(Ref.Len()) + TEXT(":") + Ref + RecipeName;
						if (const int32* Existing = RecordedLookups.Find(Key)) LookupIndex = *Existing;
						else {
							const int64 Before = Recording->Lookups.GetAllocatedSize();
							LookupIndex = Recording->Lookups.Add({Ref, RecipeName, ExamplePath}); RecordedLookups.Add(Key, LookupIndex);
							const auto& Added = Recording->Lookups.Last();
							CandidateInstructionBytes += Recording->Lookups.GetAllocatedSize() - Before
								+ Added.Ref.GetAllocatedSize() + Added.Name.GetAllocatedSize() + Added.Result.GetAllocatedSize();
						}
					}
					if (ExamplePath == TEXT("@readonly")) { if (bSkipChildren) *bSkipChildren = true; return true; }
					if (ExamplePath != TEXT("@missing"))
					{
						const auto PublishAt = [&](const FString& EditFile, const FString& EditPointer) {
							SetCanonicalGuide(EditFile, EditPointer, RecipeResult->GuideText.ToSharedRef(), true);
							if (Recording) {
								const int64 Before = Recording->Entries.GetAllocatedSize();
								Recording->Entries.Add({EditPointer, LookupIndex});
								CandidateInstructionBytes += Recording->Entries.GetAllocatedSize() - Before + Recording->Entries.Last().Pointer.GetAllocatedSize();
							}
						};
						if (!ValueToEdit) PublishAt(Document.Path, Path + TEXT("/value"));
						else {
							FString EditFile = Document.Path, EditPointer = Path + TEXT("/value");
							// Preserve real content traversal, including unresolved links.
						if (bTrackGuideExclusivity && Member(ValueToEdit, TEXT("content_ref"))) NonExclusiveGuides.Add(Document.Path);
							FollowContent(ValueToEdit, EditFile, EditPointer); PublishAt(EditFile, EditPointer);
						}
					}
					if (Recording && CandidateInstructionBytes >= 32 * 1024 * 1024) {
						// This unfinished document owns no sealed groups yet. Retiring
						// it releases these actual instructions, so later documents do
						// not inherit a permanent charge for already-freed backing.
						CandidateInstructionBytes -= Recording->Charge() + Document.Path.GetAllocatedSize() + 128;
						Recording = nullptr; CandidateGuides.Remove(Document.Path); RecordedLookups.Reset();
					}
				}
				return true;
			};
			TFunction<bool(const FUeAgentCanonicalValuePtr&, const FString&)> Visit;
			Visit = [&](const FUeAgentCanonicalValuePtr& Value, const FString& Path) -> bool
			{
				++VisitedNodes;
				if (!Value || Value->GetKind() != EUeAgentValueKind::Object) return true;
				const FString Ref = PropertyGuideReference(Value);
				if (!Ref.IsEmpty()) {
					bool bSkipChildren = false;
					if (!PropertyGuide(Ref, CanonicalText(Member(Value, TEXT("type"))), Path, Member(Value, TEXT("value")), &bSkipChildren)) return false;
					if (bSkipChildren) return true;
				}
				if (Member(Value, TEXT("nodes")) && Member(Value, TEXT("connections")))
				{
					SetGuide(Document.Path, Path + TEXT("/nodes:new_node"), MakeShared<FJsonValueObject>(Recipe({TEXT("new_node"),
						UE::UAI::HjsonWorkspace::Object({{TEXT("NewNode"), UE::UAI::HjsonWorkspace::Object({{TEXT("candidate"), UE::UAI::HjsonWorkspace::Object({{TEXT("name"), Text(TEXT("<candidate name>"))}})}})}}),
						{TEXT("Use the graph's documented creation candidates; use its candidate query when provided. Copy candidate_context beside candidate only when returned. Re-exported pin aliases define connection endpoints; properties_ref is not a creation input.")}})));
				}
				for (const auto& Field : Value->GetObjectItems())
				{
					// Never infer a creation shape from current instances, nor traverse data arrays.
					if (!IsGuideDocumentChild(Field.Key)) continue;
					if (!Visit(Field.Value, Path + TEXT("/") + Pointer(Field.Key))) return false;
				}
				return true;
			};
			if (!bReused) {
				++VisitedDocuments;
				if (Document.Value && !Visit(Document.Value, FString())) return false;
				for (const auto& Property : Document.Properties) {
					++DirectPropertyInstructions;
					if (!PropertyGuide(Property.Definition, Property.TypeName, Property.Pointer, nullptr)) return false;
				}
			}
			Recording = nullptr;
			if (auto* Candidate = CandidateGuides.Find(Document.Path)) { SealLookups(**Candidate); SealEntries(**Candidate); }
			for (const auto& Rule : Rules)
				for (const auto& Example : Rule(Document.SemanticType, Document.Role, Document.Field))
					SetGuide(Document.Path, Example.Name, MakeShared<FJsonValueObject>(Recipe(Example)));
		}
		const double CollectionsStart = FPlatformTime::Seconds();
		CurrentDocument.Reset();
		TFunction<void(FUeAgentCanonicalValuePtr, FString, FString, const FKeyedItemProjectionContract&, const FObject&)> VisitCollection;
		VisitCollection = [&](FUeAgentCanonicalValuePtr Value, FString File, FString Path, const FKeyedItemProjectionContract& Contract, const FObject& Schema)
		{
			FollowContent(Value, File, Path);
			if (!Value || Value->GetKind() != EUeAgentValueKind::Object) return;
			for (const auto& Example : BuildKeyedItemExamples(Contract, Schema))
				SetGuide(File, Path + TEXT(":") + Example.Name, MakeShared<FJsonValueObject>(Recipe(Example)));
			// Leaf membership is irrelevant to this consumer: the only operation
			// inside the item loop is visiting declared child collections.
			if (Contract.CollectionFieldContracts.IsEmpty()) return;
			for (const auto& Item : Value->GetObjectItems())
			{
				FString ItemFile = File, ItemPointer = Path + TEXT("/") + Pointer(Item.Key);
				FUeAgentCanonicalValuePtr Record = Item.Value; FollowContent(Record, ItemFile, ItemPointer);
				if (!Record || Record->GetKind() != EUeAgentValueKind::Object) continue;
				for (const auto& Child : Contract.CollectionFieldContracts)
					if (const auto ChildValue = Member(Record, Child.Key))
						VisitCollection(ChildValue, ItemFile, ItemPointer + TEXT("/") + Pointer(Child.Key), *Child.Value, nullptr);
			}
		};
		for (const auto& Collection : Collections)
		{
			FObject Schema;
			if (const auto Value = Member(Collection.Document, Collection.Contract->RecordSchemaField))
			{
				FValue Json; UE::UAI::Contract::FDiagnostic Error;
				if (!CanonicalJson::TryToJsonValue(*Value, Json, Error))
				{ OutError.SetFailure(Error, Collection.Path); return false; }
				if (Json && Json->Type == EJson::Object) Schema = Json->AsObject();
			}
			VisitCollection(Member(Collection.Document, Collection.Contract->RootField), Collection.Path,
				TEXT("/") + Pointer(Collection.Contract->RootField), *Collection.Contract,
				Schema);
		}
		const double FilesStart = FPlatformTime::Seconds();
		// A packed public document may contain many untouched rows. Preserve
		// their existing guide entries rather than interpreting their values.
		if (PreviousGuides) for (const auto& Guide : *PreviousGuides) {
			const auto At = Member(Guide.Value, TEXT("at"));
			if (!At || At->GetKind() != EUeAgentValueKind::Object) continue;
			const auto* Pointers = ReplacedPointers ? ReplacedPointers->Find(Guide.Key) : nullptr;
			for (const auto& Entry : At->GetObjectItems()) {
				bool bReplaced = false;
				if (Pointers) for (const auto& Pointer : *Pointers)
					bReplaced |= Pointer == TEXT("/") || Entry.Key == Pointer || Entry.Key.StartsWith(Pointer + TEXT("/"), ESearchCase::CaseSensitive)
						|| Entry.Key.StartsWith(Pointer + TEXT(":"), ESearchCase::CaseSensitive);
				if (bReplaced) continue;
				const auto* Existing = Guides.Find(Guide.Key);
				if (Existing && Existing->ContainsByPredicate([&](const auto& Field) { return Field.Key == Entry.Key; })) continue;
				SetCanonicalGuide(Guide.Key, Entry.Key, Entry.Value);
			}
		}
		if (OutError.HasErrors()) return false;
		TArray<FString> GuidePaths; Guides.GetKeys(GuidePaths);
		for (const auto& Reused : ReusedGuides) GuidePaths.Add(Reused.Key);
		GuidePaths.Sort();
		struct FGuideEncoding {
			const FString* SourcePath = nullptr; FString GuidePath;
			FUeAgentCanonicalValuePtr Value;
			const FGuideEntries* BorrowedEntries = nullptr;
			TArray<uint8> Bytes; TSharedPtr<FWorkspaceError> Error;
			double WorkSeconds = 0; bool bSucceeded = false;
		};
		TArray<FGuideEncoding> Encoding;
		const int32 EncodingWidth = FMath::Max(1, FTaskGraphInterface::Get().GetNumWorkerThreads() + 1);
		int32 EncodingBatches = 0; double EncodingWall = 0, EncodingWork = 0;
		int64 GuideAtKeys = 0;
		int32 BorrowedRoots = 0, CanonicalRoots = 0;
		FUeAgentCanonicalValuePtr BorrowedReadOnly;
		const FString AtKey = TEXT("at"), EditFileKey = TEXT("edit_file"), ReadOnlyKey = TEXT("read_only");
		const auto FlushEncoding = [&]() {
			if (Encoding.IsEmpty()) return true;
			++EncodingBatches; const double Started = FPlatformTime::Seconds();
			ParallelFor(Encoding.Num(), [&](int32 Index) {
				auto& Work = Encoding[Index]; FWorkspaceError Failure;
				const double WorkStarted = FPlatformTime::Seconds();
				// Borrow this frozen canonical root; no shared-reference copies,
				// catalog mutation, hash callback or filesystem work runs here.
				// The original {at, edit_file, read_only} root has no direct
				// array or 32 object fields: fragment retention has no candidate,
				// so workers do not mutate shared canonical weak-reference counts.
				if (Work.BorrowedEntries) {
					FUeAgentStructuredHjsonObjectView At;
					At.Entries = Work.BorrowedEntries;
					const FUeAgentStructuredHjsonObjectFieldView RootFields[] {
						{AtKey, At}, {EditFileKey, *Work.SourcePath}, {ReadOnlyKey, *BorrowedReadOnly}};
					const FUeAgentStructuredHjsonObjectView Root{MakeArrayView(RootFields)};
					Work.bSucceeded = TrySerializeSemanticHjsonObject(Root, Work.Bytes, Failure, Work.GuidePath);
				} else Work.bSucceeded = TrySerializeSemanticHjsonObject(*Work.Value, Work.Bytes, Failure, Work.GuidePath);
				Work.WorkSeconds = FPlatformTime::Seconds() - WorkStarted;
				if (!Work.bSucceeded) Work.Error = MakeShared<FWorkspaceError>(MoveTemp(Failure));
			});
			EncodingWall += FPlatformTime::Seconds() - Started;
			for (auto& Work : Encoding) {
				EncodingWork += Work.WorkSeconds;
				OutError.Reset(); // Original serializer's per-document boundary.
				if (!Work.bSucceeded) { OutError = *Work.Error; return false; }
				const FString ContentHash = Hash(Work.Bytes);
				Files.Emplace(Work.GuidePath, MoveTemp(Work.Bytes), ContentHash);
				if (auto* Candidate = CandidateGuides.Find(*Work.SourcePath)) {
					(*Candidate)->FilePath = Files.Last().GetRelativePath();
					(*Candidate)->FileHash = Files.Last().GetContentHash();
				}
				FileIndex->SetStringField(*Work.SourcePath, Work.GuidePath);
			}
			Encoding.Reset(); return true;
		};
		for (const auto& Path : GuidePaths)
		{
			if (const auto* Reused = ReusedGuides.Find(Path))
			{
				if (!FlushEncoding()) return false;
				Files.Add(*FindPreviousGuide(**Reused));
				FileIndex->SetStringField(Path, (*Reused)->FilePath);
				continue;
			}
			const auto& Entries = Guides.FindChecked(Path);
			if (Entries.IsEmpty()) continue;
			GuideAtKeys += Entries.Num();
			++GeneratedGuideFiles;
			const FString GuidePath = TEXT("examples/files/") + Path;
			if (!ProducedGuides) {
				FUeAgentStateSemanticError StateError;
				// The same member errors, without constructing a second owning
				// tree solely for its serializer. Fixed root literals are unique;
				// this publication's tracker is deliberately uncounted/unlimited.
				if (!FUeAgentCanonicalValue::TryValidateObjectMembers(Entries, Resources, StateError)
					|| (!BorrowedReadOnly && !FUeAgentCanonicalValue::TryBoolean(true, Resources, BorrowedReadOnly, StateError))) {
					if (!FlushEncoding()) return false;
					OutError.SetFailure(StateError, GuidePath); return false;
				}
				auto& Work = Encoding.AddDefaulted_GetRef();
				Work.SourcePath = &Path; Work.GuidePath = GuidePath; Work.BorrowedEntries = &Entries; ++BorrowedRoots;
				if (Encoding.Num() == EncodingWidth && !FlushEncoding()) return false;
				continue;
			}
			UE::UAI::Contract::FDiagnostic Error;
			FUeAgentCanonicalValuePtr At, ReadOnly, EditFile, Guide;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryObject(Entries, Resources, At, StateError)
				|| !FUeAgentCanonicalValue::TryBoolean(true, Resources, ReadOnly, StateError)
				|| !FUeAgentCanonicalValue::TryString(Path, Resources, EditFile, StateError)
				|| !FUeAgentCanonicalValue::TryObject({{TEXT("read_only"), ReadOnly.ToSharedRef()},
					{TEXT("edit_file"), EditFile.ToSharedRef()}, {TEXT("at"), At.ToSharedRef()}}, Resources, Guide, StateError))
			{
				// Earlier documents retain their original serialization-error
				// priority before this later constructor diagnostic is returned.
				if (!FlushEncoding()) return false;
				OutError.SetFailure(StateError, GuidePath); return false;
			}
			ProducedGuides->Add(Path, Guide.ToSharedRef()); ++CanonicalRoots;
			auto& Work = Encoding.AddDefaulted_GetRef();
			Work.SourcePath = &Path; Work.GuidePath = GuidePath; Work.Value = MoveTemp(Guide);
			if (Encoding.Num() == EncodingWidth && !FlushEncoding()) return false;
		}
		if (!FlushEncoding()) return false;
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=example_encoding_tasks batches=%d width=%d task_wall_s=%.6f worker_elapsed_sum_s=%.6f catalog_and_hash_callback=original_order fields_wrapper_copies=0 guide_at_keys=%lld exact_duplicate_key_copies=0 borrowed_roots=%d canonical_roots=%d"),
			EncodingBatches, EncodingWidth, EncodingWall, EncodingWork, GuideAtKeys, BorrowedRoots, CanonicalRoots);
		Index->SetObjectField(TEXT("files"), FileIndex);
		if (!PublishObject(TEXT("examples/index.hjson"), Index)) return false;
		const double ProofStart = FPlatformTime::Seconds();
		int64 ProofBytes = 0;
		int32 ProofDocuments = 0, EmptyProofDocuments = 0, ChargedGroupCount = 0, ChargedEntryGroupCount = 0;
		if (OutProof)
		{
			auto Next = MakeShared<FAuthoringExamplePublicationProof>();
			TSet<const FLookupGroup*> ChargedGroups;
			TSet<const FEntryGroup*> ChargedEntries;
			auto Admit = [&](const FString& Path, const TSharedPtr<const FGuideProof>& Proof) {
				if (NonExclusiveGuides.Contains(Path) || (Proof->FilePath.IsEmpty() && !Proof->ReadEntries().IsEmpty())
					|| Next->Documents.Contains(Path) || Next->Documents.Num() >= 8192) return;
				const auto* Group = Proof->LookupGroup.Get();
				const bool bNewGroup = Group && !ChargedGroups.Contains(Group);
				const auto* Entries = Proof->EntryGroup.Get();
				const bool bNewEntries = Entries && !ChargedEntries.Contains(Entries);
				const int64 Charge = Proof->Charge() + Path.GetAllocatedSize() + 128
					+ (bNewGroup ? Group->Charge() + 64 : 0) + (bNewEntries ? Entries->Charge() + 64 : 0);
				if (ProofBytes + Charge > 32 * 1024 * 1024) return;
				ProofBytes += Charge; Next->Documents.Add(Path, Proof);
				if (bNewGroup) ChargedGroups.Add(Group);
				if (bNewEntries) ChargedEntries.Add(Entries);
				if (Proof->FilePath.IsEmpty()) ++EmptyProofDocuments;
			};
			for (const auto& Path : GuidePaths)
				if (const auto* Reused = ReusedGuides.Find(Path)) Admit(Path, *Reused);
				else if (const auto* Candidate = CandidateGuides.Find(Path)) Admit(Path, *Candidate);
			// Empty Visit results carry their real lookup dependencies too. Rules
			// and collections still run each time and can disqualify exclusivity.
			for (const auto& Document : Documents)
				if (const auto* Reused = ReusedEmptyDocuments.Find(Document.Path)) Admit(Document.Path, *Reused);
				else if (const auto* Candidate = CandidateGuides.Find(Document.Path);
					Candidate && (*Candidate)->FilePath.IsEmpty()) Admit(Document.Path, *Candidate);
			ProofDocuments = Next->Documents.Num();
			ChargedGroupCount = ChargedGroups.Num();
			ChargedEntryGroupCount = ChargedEntries.Num();
			*OutProof = MoveTemp(Next);
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=example_recipes reused=%d global_reused=%d definitions=%d reused_guide_blocks=%d retained_guide_blocks=%d guide_proof_bytes=%lld lookup_scopes=%d lookup_group_hits=%d scoped_lookup_hits=%d instruction_groups=%d guide_paths=%d generated_guides=%d recipe_text_values=%d per_instruction_text_factory_calls=0"),
			ReusedTypeRecipes, ReusedGlobalRecipes, GlobalTypeRecipes.Num(), ReusedGuideCount, ProofDocuments, ProofBytes,
			Scopes.Num(), ReusedLookupGroups, ReusedScopedLookups, ChargedEntryGroupCount, GuidePaths.Num(), GeneratedGuideFiles, RecipeTextValues);
		const double PublicationEnd = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=example_publication setup_s=%.6f documents_s=%.6f collections_s=%.6f files_s=%.6f proof_s=%.6f total_s=%.6f documents=%d visited_documents=%d visited_nodes=%lld reused_empty=%d retained_empty=%d lookup_groups=%d set_guide=%lld path_comparisons=%lld"),
			DocumentsStart - PublicationStart, CollectionsStart - DocumentsStart, FilesStart - CollectionsStart,
			ProofStart - FilesStart, PublicationEnd - ProofStart, PublicationEnd - PublicationStart,
			Documents.Num(), VisitedDocuments, VisitedNodes, ReusedEmptyCount, EmptyProofDocuments, ChargedGroupCount, SetGuideCalls, GuidePathComparisons);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=example_property_handoff direct_instructions=%lld fake_property_documents=0 guide_entry_json_roundtrip=false"), DirectPropertyInstructions);
		return true;
	}
}
