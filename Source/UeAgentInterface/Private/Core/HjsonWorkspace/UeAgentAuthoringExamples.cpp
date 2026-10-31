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
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

namespace UE::UAI::HjsonWorkspace
{
	using namespace StateSemantic;
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
		TFunctionRef<FString(const TArray<uint8>&)> Hash)
	{
		const auto Index = MakeShared<FJsonObject>();
		Index->SetBoolField(TEXT("read_only"), true);
		Index->SetStringField(TEXT("usage"), TEXT("Read only the guide for the file being edited. Copy example values into business HJSON, replacing <...> using current queries/definitions. Examples are not defaults or required fields. Preserve retained identities; new items omit captured instance/definition IDs."));
		const auto FileIndex = MakeShared<FJsonObject>();
		using FGuideEntries = TArray<TPair<FString, FValue>>;
		TMap<FString, FGuideEntries> Guides;
		TMap<FString, FUeAgentCanonicalValueRef> DocumentValues;
		for (const auto& Document : Documents) DocumentValues.Add(Document.Path, Document.Value);
		auto SetGuide = [&](const FString& File, const FString& Path, const FValue& Value)
		{
			auto& Entries = Guides.FindOrAdd(File);
			if (auto* Found = Entries.FindByPredicate([&](const auto& Entry) { return Entry.Key.Equals(Path, ESearchCase::CaseSensitive); }))
				Found->Value = Value;
			else Entries.Emplace(Path, Value);
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
		TMap<const FJsonObject*, TMap<FString, FString>> ResolvedTypeRecipes;
		int32 ReusedTypeRecipes = 0;
		TSet<FString> UsedPaths;
		FString CurrentDocument;
		const auto Resolve = [&](const FString& Ref) -> FObject
		{
			if (const auto* Found = Definitions.Find(Ref)) return *Found;
			// Human aliases are scoped to the owning catalog, unlike nested public refs.
			FObject Best; int32 Longest = -1;
			for (const auto& Catalog : DefinitionCatalogs)
				if (CurrentDocument.StartsWith(Catalog.Key) && Catalog.Key.Len() > Longest)
					if (auto Value = GetObject(Catalog.Value, *Ref)) { Best = Value; Longest = Catalog.Key.Len(); }
			return Best;
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
			OutPath = AllocateReadableName(Stem, [&](const FString& Candidate) { return UsedPaths.Contains(Candidate.ToLower()); }) + TEXT(".hjson");
			UsedPaths.Add(OutPath.LeftChop(6).ToLower());
			SharedRecipes.Add(Identity, OutPath);
			Files.Emplace(OutPath, Bytes, Identity);
			return true;
		};
		for (const auto& Document : Documents)
		{
			CurrentDocument = Document.Path;
			TFunction<bool(const FUeAgentCanonicalValuePtr&, const FString&)> Visit;
			Visit = [&](const FUeAgentCanonicalValuePtr& Value, const FString& Path) -> bool
			{
				if (!Value || Value->GetKind() != EUeAgentValueKind::Object) return true;
				const FString Ref = CanonicalText(Member(Value, TEXT("definition")));
				if (Member(Value, TEXT("value")) && !Ref.IsEmpty())
				{
					if (const auto Definition = Resolve(Ref))
					{
						bool Editable = true; Definition->TryGetBoolField(TEXT("editable"), Editable);
						if (!Editable) return true;
						FString ExamplePath;
						const FString TypeName = CanonicalText(Member(Value, TEXT("type")));
						const FString RecipeName = TypeName.IsEmpty() ? Ref : TypeName;
						const FString ScopeKey = FString::FromInt(CurrentDocument.Len()) + TEXT(":") + CurrentDocument + RecipeName;
						auto& TypeRecipes = ResolvedTypeRecipes.FindOrAdd(Definition.Get());
						if (const FString* Existing = TypeRecipes.Find(ScopeKey)) { ExamplePath = *Existing; ++ReusedTypeRecipes; }
						else {
							if (!Share(RecipeName, BuildAuthoringTypeExamples(Definition.ToSharedRef(), Resolve), ExamplePath)) return false;
							TypeRecipes.Add(ScopeKey, ExamplePath);
						}
						FString EditFile = Document.Path, EditPointer = Path + TEXT("/value");
						auto ValueToEdit = Member(Value, TEXT("value"));
						FollowContent(ValueToEdit, EditFile, EditPointer);
						SetGuide(EditFile, EditPointer, Text(ExamplePath));
					}
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
					if (Field.Key == TEXT("definitions") || Field.Key == TEXT("value") || Field.Key == TEXT("content_ref")) continue;
					if (!Visit(Field.Value, Path + TEXT("/") + Pointer(Field.Key))) return false;
				}
				return true;
			};
			if (!Visit(Document.Value, FString())) return false;
			for (const auto& Rule : Rules)
				for (const auto& Example : Rule(Document.SemanticType, Document.Role, Document.Field))
					SetGuide(Document.Path, Example.Name, MakeShared<FJsonValueObject>(Recipe(Example)));
		}
		TFunction<void(FUeAgentCanonicalValuePtr, FString, FString, const FKeyedItemProjectionContract&, const FObject&)> VisitCollection;
		VisitCollection = [&](FUeAgentCanonicalValuePtr Value, FString File, FString Path, const FKeyedItemProjectionContract& Contract, const FObject& Schema)
		{
			FollowContent(Value, File, Path);
			if (!Value || Value->GetKind() != EUeAgentValueKind::Object) return;
			for (const auto& Example : BuildKeyedItemExamples(Contract, Schema))
				SetGuide(File, Path + TEXT(":") + Example.Name, MakeShared<FJsonValueObject>(Recipe(Example)));
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
		auto Resources = MakeSemanticWorkspaceResourceTracker();
		TArray<FString> GuidePaths; Guides.GetKeys(GuidePaths); GuidePaths.Sort();
		for (const auto& Path : GuidePaths)
		{
			const auto& Entries = Guides.FindChecked(Path);
			if (Entries.IsEmpty()) continue;
			const FString GuidePath = TEXT("examples/files/") + Path;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			UE::UAI::Contract::FDiagnostic Error;
			for (const auto& Entry : Entries)
			{
				FUeAgentCanonicalValuePtr Value;
				if (!CanonicalJson::TryFromJsonValue(Entry.Value, Resources, Value, Error))
				{ OutError.SetFailure(Error, GuidePath); return false; }
				Fields.Emplace(Entry.Key, Value.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr At, ReadOnly, EditFile, Guide;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, At, StateError)
				|| !FUeAgentCanonicalValue::TryBoolean(true, Resources, ReadOnly, StateError)
				|| !FUeAgentCanonicalValue::TryString(Path, Resources, EditFile, StateError)
				|| !FUeAgentCanonicalValue::TryObject({{TEXT("read_only"), ReadOnly.ToSharedRef()},
					{TEXT("edit_file"), EditFile.ToSharedRef()}, {TEXT("at"), At.ToSharedRef()}}, Resources, Guide, StateError))
			{ OutError.SetFailure(StateError, GuidePath); return false; }
			TArray<uint8> Bytes;
			if (!TrySerializeSemanticHjsonObject(*Guide, Bytes, OutError, GuidePath)) return false;
			Files.Emplace(GuidePath, Bytes, Hash(Bytes));
			FileIndex->SetStringField(Path, GuidePath);
		}
		Index->SetObjectField(TEXT("files"), FileIndex);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=example_recipes reused=%d definitions=%d"), ReusedTypeRecipes, ResolvedTypeRecipes.Num());
		return PublishObject(TEXT("examples/index.hjson"), Index);
	}
}
