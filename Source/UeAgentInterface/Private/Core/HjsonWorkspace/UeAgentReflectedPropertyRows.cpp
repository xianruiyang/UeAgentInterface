#include "Core/HjsonWorkspace/UeAgentReflectedPropertyRows.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::HjsonWorkspace
{
using namespace StateSemantic;
namespace
{
	void AppendRowFailure(UE::UAI::Contract::FDiagnostic& Failures, const TCHAR* Code,
		const FString& Reason, const FString& Path, const FString& Expected,
		const FUeAgentCanonicalValue* Actual, bool bNative = false)
	{
		TSharedPtr<FJsonValue> Json; UE::UAI::Contract::FDiagnostic Error;
		if (Actual && !CanonicalJson::TryToJsonValue(*Actual, Json, Error))
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Path);
		auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path,
			Expected.IsEmpty() ? TSharedPtr<FJsonValue>() : MakeShared<FJsonValueString>(Expected), Json,
			TEXT("input_decode"), Reason);
		if (Actual) Diagnostic.ActualValuePresent = true;
		if (bNative) Diagnostic.SourceKind = TEXT("native_result");
		if (!Error.IsEmpty()) Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(MoveTemp(Error));
		Failures.Append(Diagnostic);
	}
}
		FString ReadablePropertyCategory(const TArray<FString>& CategoryPath)
		{
			TArray<FString> Segments;
			for (FString Segment : CategoryPath)
			{
				Segment.TrimStartAndEndInline();
				if (!Segment.IsEmpty())
				{
					Segments.Add(MoveTemp(Segment));
				}
			}
			return Segments.IsEmpty()
				? FString(TEXT("General"))
				: FString::Join(Segments, TEXT("|"));
		}

		FString ShortReadableTypeName(FString Value)
		{
			Value.TrimStartAndEndInline();
			const int32 Dot = Value.Find(TEXT("."), ESearchCase::CaseSensitive,
				ESearchDir::FromEnd);
			const int32 Slash = Value.Find(TEXT("/"), ESearchCase::CaseSensitive,
				ESearchDir::FromEnd);
			const int32 Separator = FMath::Max(Dot, Slash);
			if (Separator != INDEX_NONE)
			{
				Value.RightChopInline(Separator + 1);
			}
			return Value;
		}


FString ReadablePropertyTypeName(const FUeAgentCanonicalValue& Type)
{
				TFunction<FString(const FUeAgentCanonicalValue&)> FormatType;
				FormatType = [&FormatType](
					const FUeAgentCanonicalValue& Descriptor) -> FString
				{
					if (Descriptor.GetKind() != EUeAgentValueKind::Object)
					{
						return FString();
					}
					FString Kind;
					FString TypeId;
					FString EnumPath;
					FString StructPath;
					FString FixedLength;
					const FUeAgentCanonicalValue* ElementType = nullptr;
					const FUeAgentCanonicalValue* KeyType = nullptr;
					const FUeAgentCanonicalValue* MappedType = nullptr;
					const FUeAgentCanonicalValue* InnerType = nullptr;
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
						: Descriptor.GetObjectItems())
					{
						if (Field.Value->GetKind() == EUeAgentValueKind::String)
						{
							if (Field.Key == TEXT("kind")) Kind = Field.Value->GetText();
							else if (Field.Key == TEXT("type_id")) TypeId = Field.Value->GetText();
							else if (Field.Key == TEXT("enum_path")) EnumPath = Field.Value->GetText();
							else if (Field.Key == TEXT("struct_path")) StructPath = Field.Value->GetText();
						}
						else if ((Field.Value->GetKind() == EUeAgentValueKind::Integer
								|| Field.Value->GetKind() == EUeAgentValueKind::Decimal)
							&& Field.Key == TEXT("fixed_length"))
						{
							FixedLength = Field.Value->GetLexeme();
							while (FixedLength.EndsWith(TEXT("0"))
								&& FixedLength.Contains(TEXT(".")))
							{
								FixedLength.LeftChopInline(1);
							}
							if (FixedLength.EndsWith(TEXT("."))) FixedLength.LeftChopInline(1);
						}
						else if (Field.Value->GetKind() == EUeAgentValueKind::Object)
						{
							if (Field.Key == TEXT("element_type")) ElementType = &Field.Value.Get();
							else if (Field.Key == TEXT("key_type")) KeyType = &Field.Value.Get();
							else if (Field.Key == TEXT("mapped_type")) MappedType = &Field.Value.Get();
							else if (Field.Key == TEXT("inner_type")) InnerType = &Field.Value.Get();
						}
					}
					if (!EnumPath.IsEmpty())
						return Kind.Equals(TEXT("bitmask"), ESearchCase::IgnoreCase)
							? TEXT("bitmask<") + EnumPath + TEXT(">") : EnumPath;
					if (Kind.Equals(TEXT("boolean"), ESearchCase::IgnoreCase)
						|| Kind.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
					{
						return TEXT("bool");
					}
					if ((Kind.Equals(TEXT("enum"), ESearchCase::IgnoreCase)
						|| Kind.Equals(TEXT("bitmask"), ESearchCase::IgnoreCase))
						&& !TypeId.IsEmpty())
					{
						return Kind.Equals(TEXT("bitmask"), ESearchCase::IgnoreCase)
							? TEXT("bitmask<") + TypeId + TEXT(">") : TypeId;
					}
					if ((Kind.Equals(TEXT("struct"), ESearchCase::IgnoreCase)
						|| Kind.Equals(TEXT("pcg_selector"), ESearchCase::IgnoreCase))
						&& !StructPath.IsEmpty())
					{
						return Kind.Equals(TEXT("pcg_selector"), ESearchCase::IgnoreCase)
							? TEXT("pcg_selector<") + StructPath + TEXT(">") : StructPath;
					}
					if (Kind.Equals(TEXT("array"), ESearchCase::IgnoreCase)
						&& ElementType != nullptr)
					{
						return FormatType(*ElementType) + TEXT("[]");
					}
					if (Kind.Equals(TEXT("set"), ESearchCase::IgnoreCase)
						&& ElementType != nullptr)
					{
						return TEXT("set<") + FormatType(*ElementType) + TEXT(">");
					}
					if (Kind.Equals(TEXT("map"), ESearchCase::IgnoreCase)
						&& KeyType != nullptr && MappedType != nullptr)
					{
						return TEXT("map<") + FormatType(*KeyType) + TEXT(",")
							+ FormatType(*MappedType) + TEXT(">");
					}
					if (Kind.Equals(TEXT("optional"), ESearchCase::IgnoreCase)
						&& InnerType != nullptr)
					{
						return FormatType(*InnerType) + TEXT("?");
					}
					if (Kind.Equals(TEXT("fixed_array"), ESearchCase::IgnoreCase)
						&& ElementType != nullptr && !FixedLength.IsEmpty())
					{
						return FormatType(*ElementType) + TEXT("[") + FixedLength + TEXT("]");
					}
					if (!Kind.IsEmpty()) return Kind.ToLower();
					if (!TypeId.IsEmpty()) return TypeId;
					return FString();
				};

return FormatType(Type);
}

bool TryDescribeReadableProperty(const FUeAgentCanonicalValue& Descriptor, const FUeAgentCanonicalValue& Type,
	FReadablePropertySchema& OutSchema)
{
	if (Descriptor.GetKind() != EUeAgentValueKind::Object) return false;
	TArray<FString> Categories;
	bool bHasCategory = false;
	for (const auto& Field : Descriptor.GetObjectItems())
	{
		if (Field.Key == TEXT("property_name") && Field.Value->GetKind() == EUeAgentValueKind::String) OutSchema.Name = Field.Value->GetText();
		if (Field.Key == TEXT("category_path") && Field.Value->GetKind() == EUeAgentValueKind::Array)
		{
			bHasCategory = true;
			for (const auto& Part : Field.Value->GetArrayItems())
			{
				if (Part->GetKind() != EUeAgentValueKind::String) return false;
				Categories.Add(Part->GetText());
			}
		}
	}
	OutSchema.Category = ReadablePropertyCategory(Categories);
	OutSchema.Type = ReadablePropertyTypeName(Type);
	return bHasCategory && !OutSchema.Name.IsEmpty() && !OutSchema.Type.IsEmpty();
}

bool TryReadReadablePropertyRow(const FUeAgentCanonicalValue& Descriptor, const FReadablePropertySchema& Schema,
	FUeAgentCanonicalValuePtr& OutValue, FUeAgentStateSemanticError& OutError, const FString& Path)
{
	OutValue.Reset();
	OutError = FUeAgentStateSemanticError();
	const auto Report = [&](const TCHAR* Code, const FString& Reason, const FString& Pointer,
		const FString& Expected, const FUeAgentCanonicalValue* Actual, bool bNative = false)
	{ AppendRowFailure(OutError, Code, Reason, Pointer, Expected, Actual, bNative); };
	if (Descriptor.GetKind() != EUeAgentValueKind::Object) {
		Report(TEXT("hjson_initial_property_invalid"), TEXT("A property row must be an object."), Path, TEXT("object"), &Descriptor);
		return false;
	}
	const FUeAgentCanonicalValue* Type = nullptr;
	const FUeAgentCanonicalValue* Definition = nullptr;
	for (const auto& Member : Descriptor.GetObjectItems()) {
		if (Member.Key == TEXT("type")) Type = &Member.Value.Get();
		else if (Member.Key == TEXT("value")) OutValue = Member.Value;
		else if (Member.Key == TEXT("definition")) Definition = &Member.Value.Get();
		else Report(TEXT("hjson_initial_property_invalid"), TEXT("This field is not supported in a property row."),
			Path + TEXT("/") + Member.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), TEXT("type, value, definition"), nullptr);
	}
	if (!Type || Type->GetKind() != EUeAgentValueKind::String)
		Report(TEXT("semantic_workspace_property_type_missing"), Type ? TEXT("Property type must be a string.") : TEXT("Property type is required."), Path + TEXT("/type"), Schema.Type, Type);
	else if (!Type->GetText().TrimStartAndEnd().Equals(Schema.Type.TrimStartAndEnd(), ESearchCase::CaseSensitive))
		Report(TEXT("semantic_workspace_property_type_mismatch"), TEXT("Property type does not match its schema."), Path + TEXT("/type"), Schema.Type, Type);
	if (!OutValue)
		Report(TEXT("hjson_initial_property_value_missing"), TEXT("Property value is required."), Path + TEXT("/value"), FString(), nullptr);
	if (Definition && (Definition->GetKind() != EUeAgentValueKind::String
		|| (!Schema.Definition.IsEmpty() && Definition->GetText() != Schema.Definition)))
		Report(TEXT("semantic_workspace_property_definition_mismatch"), TEXT("Property definition must be a string matching its schema definition."), Path + TEXT("/definition"), Schema.Definition, Definition);
	if (Schema.Type.IsEmpty())
		Report(TEXT("hjson_import_property_type_missing"), TEXT("The native property schema did not provide a type."), Path + TEXT("/type"), FString(), nullptr, true);
	return OutError.IsEmpty();
}

bool TryReadReadablePropertyRows(const FUeAgentCanonicalValue& Categories,
	const TArray<FReadablePropertySchema>& Schema, TArray<FResolvedReadableProperty>& OutRows,
	FUeAgentStateSemanticError& OutError)
{
	OutRows.Reset();
	OutError = FUeAgentStateSemanticError();
	UeAgentAuthoringDiagnostics::FDiagnostic Failures;
	auto Segment = [](const FString& Key) { return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); };
	auto Report = [&](const TCHAR* Code, const FString& Reason, const FString& Path, const FString& Expected,
		const FUeAgentCanonicalValue* Actual, bool bNative = false)
	{
		AppendRowFailure(Failures, Code, Reason, Path, Expected, Actual, bNative);
	};
	if (Categories.GetKind() != EUeAgentValueKind::Object)
		Report(TEXT("hjson_initial_categories_invalid"), TEXT("Property categories must be an object."), TEXT("/"), TEXT("object"), &Categories);
	else for (const auto& Category : Categories.GetObjectItems())
	{
		const FString CategoryPath = TEXT("/") + Segment(Category.Key);
		if (Category.Value->GetKind() != EUeAgentValueKind::Object)
		{ Report(TEXT("hjson_initial_category_invalid"), TEXT("A property category must contain an object of property rows."), CategoryPath, TEXT("object"), &Category.Value.Get()); continue; }
		TSet<int32> Seen;
		for (const auto& Field : Category.Value->GetObjectItems())
		{
			const FString Path = CategoryPath + TEXT("/") + Segment(Field.Key);
			int32 Found = INDEX_NONE; bool bAmbiguous = false;
			for (int32 Index = 0; Index < Schema.Num(); ++Index)
				if (Schema[Index].Name.Equals(Field.Key, ESearchCase::CaseSensitive)
					&& Schema[Index].Category.Equals(Category.Key, ESearchCase::CaseSensitive))
				{ bAmbiguous |= Found != INDEX_NONE; Found = Index; }
			if (bAmbiguous)
			{ Report(TEXT("hjson_initial_property_ambiguous"), TEXT("This property name matches multiple schema entries in the category."), Path, FString(), nullptr); continue; }
			if (Found == INDEX_NONE)
			{ Report(TEXT("hjson_initial_property_unknown"), TEXT("This property is not declared in the category's schema."), Path, FString(), nullptr); continue; }
			if (Seen.Contains(Found))
			{ Report(TEXT("hjson_initial_property_path_duplicate"), TEXT("This property schema entry has already been supplied."), Path, FString(), nullptr); continue; }
			Seen.Add(Found);
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError RowError;
			if (TryReadReadablePropertyRow(Field.Value.Get(), Schema[Found], Value, RowError, Path)) OutRows.Add({Found, Value});
			else Failures.Append(RowError);
		}
	}
	if (!Failures.Code.IsEmpty())
	{ OutError = MoveTemp(Failures); return false; }
	return true;
}

bool TryAddReadablePropertyRow(const TSharedRef<FJsonObject>& Categories,
	const FReadablePropertySchema& Schema, const TSharedPtr<FJsonValue>& Value)
{
	if (Schema.Name.IsEmpty() || Schema.Type.IsEmpty() || !Value) return false;
	const TSharedPtr<FJsonObject>* Existing = nullptr;
	TSharedPtr<FJsonObject> Category;
	if (Categories->TryGetObjectField(Schema.Category, Existing)) Category = *Existing;
	else { Category = MakeShared<FJsonObject>(); Categories->SetObjectField(Schema.Category, Category); }
	if (!Category || Category->HasField(Schema.Name)) return false;
	const auto Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("type"), Schema.Type);
	if (!Schema.Definition.IsEmpty()) Row->SetStringField(TEXT("definition"), Schema.Definition);
	Row->SetField(TEXT("value"), Value);
	Category->SetObjectField(Schema.Name, Row);
	return true;
}
}
