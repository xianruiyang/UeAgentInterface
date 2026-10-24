#include "Utilities/UeAgentAuthoringFieldPolicy.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentAuthoringFieldPolicy
{
	namespace
	{
		bool IsDebugOrReadbackDocument(const TSharedPtr<FJsonObject>& Object)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			for (const FString& FieldName : { FString(TEXT("profile")), FString(TEXT("kind")), FString(TEXT("schema")) })
			{
				FString Value;
				if (Object->TryGetStringField(FieldName, Value))
				{
					Value.ToLowerInline();
					if (Value.Contains(TEXT("debug")) || Value.Contains(TEXT("readback")))
					{
						return true;
					}
				}
			}
			return false;
		}

		bool IsMetadataDescriptor(const TSharedPtr<FJsonObject>& Object)
		{
			if (!Object.IsValid())
			{
				return false;
			}

			if (Object->HasField(TEXT("property_path")))
			{
				return true;
			}

			const TSharedPtr<FJsonObject>* Selector = nullptr;
			const TSharedPtr<FJsonObject>* Availability = nullptr;
			return Object->TryGetObjectField(TEXT("selector"), Selector)
				&& Selector && Selector->IsValid()
				&& Object->TryGetObjectField(TEXT("availability"), Availability)
				&& Availability && Availability->IsValid()
				&& Object->HasField(TEXT("kind"));
		}

		void RemoveRedundantTextValue(
			const TSharedPtr<FJsonObject>& Object,
			const FString& ValueField,
			const FString& TextField)
		{
			if (!Object.IsValid() || !Object->HasField(TextField))
			{
				return;
			}

			const TSharedPtr<FJsonValue> Value = Object->TryGetField(ValueField);
			if (!Value.IsValid() || Value->Type == EJson::Null || Value->Type == EJson::None)
			{
				return;
			}

			Object->RemoveField(TextField);
		}

		void NormalizeRecursive(const TSharedPtr<FJsonObject>& Object, const bool bPreserveTextValues)
		{
			if (!Object.IsValid())
			{
				return;
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (!Pair.Value.IsValid())
				{
					continue;
				}
				if (Pair.Value->Type == EJson::Object)
				{
					NormalizeRecursive(Pair.Value->AsObject(), bPreserveTextValues);
				}
				else if (Pair.Value->Type == EJson::Array)
				{
					for (const TSharedPtr<FJsonValue>& Item : Pair.Value->AsArray())
					{
						if (Item.IsValid() && Item->Type == EJson::Object)
						{
							NormalizeRecursive(Item->AsObject(), bPreserveTextValues);
						}
					}
				}
			}

			if (IsMetadataDescriptor(Object))
			{
				Object->RemoveField(TEXT("control_type"));
				Object->RemoveField(TEXT("operations"));
				Object->RemoveField(TEXT("category_path"));
				if (!bPreserveTextValues && Object->HasField(TEXT("property_path")))
				{
					Object->RemoveField(TEXT("selector"));
					Object->RemoveField(TEXT("property_name"));
					Object->RemoveField(TEXT("authored_name"));
				}
			}

			if (!bPreserveTextValues)
			{
				RemoveRedundantTextValue(Object, TEXT("value"), TEXT("value_text"));
				RemoveRedundantTextValue(Object, TEXT("default_value"), TEXT("default_value_text"));
			}
		}
	}

	void NormalizeAuthoringObjectTree(const TSharedPtr<FJsonObject>& Object)
	{
		NormalizeRecursive(Object, IsDebugOrReadbackDocument(Object));
	}
}
