#include "Utilities/UeAgentAuthoringJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"

namespace UeAgentAuthoringJson
{
	TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<FString> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> MakeSelector(const FString& Key, const FString& Value)
	{
		TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
		Selector->SetStringField(Key, Value);
		return Selector;
	}

	TSharedPtr<FJsonObject> MakeAvailability(const bool bEditable, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Availability = MakeShared<FJsonObject>();
		Availability->SetBoolField(TEXT("editable"), bEditable);
		if (!Reason.IsEmpty())
		{
			Availability->SetStringField(TEXT("reason"), Reason);
		}
		return Availability;
	}

	TArray<TSharedPtr<FJsonValue>> MakeEnumCandidates(const UEnum* Enum)
	{
		TArray<TSharedPtr<FJsonValue>> Candidates;
		if (!Enum)
		{
			return Candidates;
		}
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			if (Enum->HasMetaData(TEXT("Hidden"), Index))
			{
				continue;
			}
			const FString Name = Enum->GetNameStringByIndex(Index);
			if (Name.EndsWith(TEXT("_MAX")))
			{
				continue;
			}
			TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), Name);
			Candidate->SetNumberField(TEXT("value"), static_cast<double>(Enum->GetValueByIndex(Index)));
			const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
			if (!DisplayName.IsEmpty())
			{
				Candidate->SetStringField(TEXT("display_name"), DisplayName);
			}
			const FString Tooltip = Enum->GetMetaData(TEXT("ToolTip"), Index);
			if (!Tooltip.IsEmpty())
			{
				Candidate->SetStringField(TEXT("tooltip"), Tooltip);
			}
			Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
		}
		return Candidates;
	}

	void ApplyAuthoringMetadata(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& SelectorKey,
		const FString& SelectorValue,
		const FString& DisplayName,
		const FString& CategoryPath,
		const FString& ValueKind,
		const bool bEditable)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		Obj->SetObjectField(TEXT("selector"), MakeSelector(SelectorKey, SelectorValue));
		if (!DisplayName.IsEmpty())
		{
			Obj->SetStringField(TEXT("display_name"), DisplayName);
		}
		if (!CategoryPath.IsEmpty())
		{
			Obj->SetStringField(TEXT("category_path"), CategoryPath);
		}
		Obj->SetStringField(TEXT("value_kind"), ValueKind);
		Obj->SetObjectField(TEXT("availability"), MakeAvailability(bEditable));
	}

	void ApplyPropertyMetadata(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& PropertyPath,
		const FString& DisplayName,
		const FString& CategoryPath,
		const FString& ValueKind)
	{
		ApplyAuthoringMetadata(
			Obj,
			TEXT("property_path"),
			PropertyPath,
			DisplayName.IsEmpty() ? PropertyPath : DisplayName,
			CategoryPath,
			ValueKind);
	}

}
