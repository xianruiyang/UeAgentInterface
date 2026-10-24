#include "Core/Contract/UeAgentContractJson.h"

#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::Contract::Json
{
	namespace
	{
		bool EncodeJsonString(
			const FString& Value,
			FString& OutText)
		{
			TArray<TSharedPtr<FJsonValue>> Wrapper;
			Wrapper.Add(MakeShared<FJsonValueString>(Value));
			FString Encoded;
			const TSharedRef<
				TJsonWriter<
					TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<
					TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(
						&Encoded);
			if (!FJsonSerializer::Serialize(Wrapper, Writer)
				|| Encoded.Len() < 2)
			{
				return false;
			}
			OutText = Encoded.Mid(1, Encoded.Len() - 2);
			return true;
		}

		FDiagnostic EncodingFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Path)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("contract_json_encode"));
			Error.Source.JsonPath = Path;
			return Error;
		}

		FString ChildPath(const FString& Path, FString Token)
		{
			Token.ReplaceInline(TEXT("~"), TEXT("~0"));
			Token.ReplaceInline(TEXT("/"), TEXT("~1"));
			return (Path == TEXT("/") ? FString() : Path) + TEXT("/") + Token;
		}

		bool WriteCanonical(
			const TSharedPtr<FJsonValue>& Value,
			FString& OutText,
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& Path)
		{
			if (!Value.IsValid())
			{
				OutError = EncodingFailure(TEXT("contract_json_value_invalid"), TEXT("The JSON value is missing."), Path);
				OutError.ActualValuePresent = false;
				return false;
			}
			switch (Value->Type)
			{
			case EJson::Null:
				OutText += TEXT("null");
				return true;
			case EJson::Boolean:
			{
				bool Boolean = false;
				if (!Value->TryGetBool(Boolean))
				{
					OutError = EncodingFailure(TEXT("contract_json_boolean_invalid"), TEXT("The JSON boolean could not be read."), Path);
					return false;
				}
				OutText += Boolean ? TEXT("true") : TEXT("false");
				return true;
			}
			case EJson::Number:
			{
				FString Number;
				if (!Value->TryGetString(Number)
					|| Number.IsEmpty())
				{
					OutError = EncodingFailure(TEXT("contract_json_number_invalid"), TEXT("The JSON number has no serializable numeric representation."), Path);
					return false;
				}
				OutText += Number;
				return true;
			}
			case EJson::String:
			{
				FString Text;
				FString Encoded;
				if (!Value->TryGetString(Text)
					|| !EncodeJsonString(Text, Encoded))
				{
					OutError = EncodingFailure(TEXT("contract_json_string_invalid"), TEXT("The JSON string could not be read or encoded."), Path);
					return false;
				}
				OutText += Encoded;
				return true;
			}
			case EJson::Array:
			{
				OutText += TEXT("[");
				const TArray<TSharedPtr<FJsonValue>>& Values =
					Value->AsArray();
				for (int32 Index = 0; Index < Values.Num(); ++Index)
				{
					if (Index > 0)
					{
						OutText += TEXT(",");
					}
					if (!WriteCanonical(
						Values[Index],
						OutText,
						OutError, ChildPath(Path, FString::FromInt(Index))))
					{
						return false;
					}
				}
				OutText += TEXT("]");
				return true;
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> Object =
					Value->AsObject();
				if (!Object.IsValid())
				{
					OutError = EncodingFailure(TEXT("contract_json_object_invalid"), TEXT("The JSON object value has no object storage."), Path);
					return false;
				}
				TArray<FString> Keys;
				Object->Values.GetKeys(Keys);
				Keys.Sort();
				OutText += TEXT("{");
				for (int32 Index = 0; Index < Keys.Num(); ++Index)
				{
					if (Index > 0)
					{
						OutText += TEXT(",");
					}
					FString EncodedKey;
					if (!EncodeJsonString(
						Keys[Index],
						EncodedKey))
					{
						OutError = EncodingFailure(TEXT("contract_json_key_invalid"), TEXT("The JSON object key could not be encoded."), ChildPath(Path, Keys[Index]));
						return false;
					}
					OutText += EncodedKey + TEXT(":");
					if (!WriteCanonical(
						Object->Values.FindRef(Keys[Index]),
						OutText,
						OutError, ChildPath(Path, Keys[Index])))
					{
						return false;
					}
				}
				OutText += TEXT("}");
				return true;
			}
			default:
				OutError = EncodingFailure(TEXT("contract_json_type_unsupported"), TEXT("The JSON value has an unsupported type tag."), Path);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Value->Type));
				return false;
			}
		}
	}

	bool TryParseObject(
		const FString& Text,
		TSharedPtr<FJsonObject>& OutObject,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutObject.Reset();
		OutError.Reset();
		if (Text.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("contract_json_empty"), TEXT("A non-empty JSON object document is required."), TEXT("contract_json_parse"));
			OutError.Source.JsonPath = TEXT("/");
			OutError.ActualValue = MakeShared<FJsonValueString>(Text);
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(
				Reader,
				OutObject,
				FJsonSerializer::EFlags::StoreNumbersAsStrings)
			|| !OutObject.IsValid())
		{
			OutError = FDiagnostic::Failure(TEXT("contract_json_object_invalid"),
				Reader->GetErrorMessage().IsEmpty() ? TEXT("Expected a JSON object at the document root.") : Reader->GetErrorMessage(), TEXT("contract_json_parse"));
			OutError.Source.Line = Reader->GetLineNumber();
			OutError.Source.Column = Reader->GetCharacterNumber();
			return false;
		}
		return true;
	}

	bool TryParseObject(
		const TArray<uint8>& Utf8Bytes,
		TSharedPtr<FJsonObject>& OutObject,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (Utf8Bytes.IsEmpty())
		{
			OutObject.Reset();
			OutError = FDiagnostic::Failure(TEXT("contract_json_empty"), TEXT("A non-empty UTF-8 JSON object document is required."), TEXT("contract_json_parse"));
			OutError.Source.JsonPath = TEXT("/");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			return false;
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(
				Utf8Bytes.GetData()),
			Utf8Bytes.Num());
		return TryParseObject(
			FString(Converted.Length(), Converted.Get()),
			OutObject,
			OutError);
	}

	bool TryCanonicalize(
		const TSharedPtr<FJsonValue>& Value,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutText.Reset();
		OutError.Reset();
		return WriteCanonical(Value, OutText, OutError, TEXT("/"));
	}

	bool TryCanonicalize(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object.IsValid())
		{
			OutText.Reset();
			OutError = EncodingFailure(TEXT("contract_json_object_invalid"), TEXT("A JSON object is required for serialization."), TEXT("/"));
			OutError.ActualValuePresent = false;
			return false;
		}
		return TryCanonicalize(
			MakeShared<FJsonValueObject>(Object),
			OutText,
			OutError);
	}

	bool TryCanonicalUtf8(
		const TSharedPtr<FJsonObject>& Object,
		TArray<uint8>& OutBytes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FString Canonical;
		if (!TryCanonicalize(Object, Canonical, OutError))
		{
			OutBytes.Reset();
			return false;
		}
		const FTCHARToUTF8 Converted(*Canonical);
		OutBytes.Reset(Converted.Length());
		OutBytes.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		return true;
	}

	FString Sha256Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		SHA256(
			reinterpret_cast<const unsigned char*>(
				Converted.Get()),
			Converted.Length(),
			Digest);
		FString Hex;
		Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
		for (const uint8 Byte : Digest)
		{
			Hex += FString::Printf(TEXT("%02x"), Byte);
		}
		return TEXT("sha256:") + Hex;
	}
}
