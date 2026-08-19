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

		bool WriteCanonical(
			const TSharedPtr<FJsonValue>& Value,
			FString& OutText,
			FString& OutError)
		{
			if (!Value.IsValid())
			{
				OutError = TEXT("contract_json_value_invalid");
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
					OutError =
						TEXT("contract_json_boolean_invalid");
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
					OutError =
						TEXT("contract_json_number_invalid");
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
					OutError =
						TEXT("contract_json_string_invalid");
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
						OutError))
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
					OutError =
						TEXT("contract_json_object_invalid");
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
						OutError =
							TEXT("contract_json_key_invalid");
						return false;
					}
					OutText += EncodedKey + TEXT(":");
					if (!WriteCanonical(
						Object->Values.FindRef(Keys[Index]),
						OutText,
						OutError))
					{
						return false;
					}
				}
				OutText += TEXT("}");
				return true;
			}
			default:
				OutError =
					TEXT("contract_json_type_unsupported");
				return false;
			}
		}
	}

	bool TryParseObject(
		const FString& Text,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError)
	{
		OutObject.Reset();
		OutError.Reset();
		if (Text.IsEmpty())
		{
			OutError = TEXT("contract_json_empty");
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
			OutError = TEXT("contract_json_object_invalid");
			return false;
		}
		return true;
	}

	bool TryParseObject(
		const TArray<uint8>& Utf8Bytes,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError)
	{
		if (Utf8Bytes.IsEmpty())
		{
			OutObject.Reset();
			OutError = TEXT("contract_json_empty");
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
		FString& OutError)
	{
		OutText.Reset();
		OutError.Reset();
		return WriteCanonical(Value, OutText, OutError);
	}

	bool TryCanonicalize(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutText,
		FString& OutError)
	{
		if (!Object.IsValid())
		{
			OutText.Reset();
			OutError = TEXT("contract_json_object_invalid");
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
		FString& OutError)
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
