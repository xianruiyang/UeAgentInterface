#include "Utilities/UeAgentFolderDetailsExport.h"

#include "Misc/Paths.h"
#include "Utilities/UeAgentDetailsAuthoring.h"

namespace UeAgentFolderDetailsExport
{
	FString MakeDetailsStem(const FString& InValue)
	{
		FString Stem = InValue;
		Stem.ReplaceInline(TEXT("/"), TEXT("_"));
		Stem.ReplaceInline(TEXT("\\"), TEXT("_"));
		Stem.ReplaceInline(TEXT(":"), TEXT("_"));
		Stem.ReplaceInline(TEXT("."), TEXT("_"));
		Stem.ReplaceInline(TEXT(" "), TEXT("_"));
		Stem.ReplaceInline(TEXT("\""), TEXT("_"));
		Stem.ReplaceInline(TEXT("'"), TEXT("_"));
		Stem.TrimStartAndEndInline();
		return Stem.IsEmpty() ? FString(TEXT("owner")) : Stem;
	}

	bool AttachOwnerDetails(
		UObject* Owner,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& Category,
		const TSharedPtr<FJsonObject>& EntryObject,
		FString& OutError,
		const FString& RefFieldName)
	{
		if (!Owner || !EntryObject.IsValid())
		{
			OutError = TEXT("details_owner_invalid");
			return false;
		}

		const FString DetailsRoot = FPaths::Combine(EntryDirectory, TEXT("details"), Category, MakeDetailsStem(OwnerId));
		UeAgentDetailsAuthoring::FOwnerDetailsExportRequest Request =
			UeAgentDetailsAuthoring::MakeOwnerDetailsExportRequest(
				Owner,
				OwnerKind,
				OwnerId,
				DetailsRoot,
				SourceCommand,
				FPaths::Combine(TEXT("details"), Category, MakeDetailsStem(OwnerId), TEXT("index.json")),
				RefFieldName);

		TSharedPtr<FJsonObject> DetailsSummary;
		if (!UeAgentDetailsAuthoring::ExportOwnerDetails(Request, DetailsSummary, OutError))
		{
			return false;
		}

		EntryObject->SetObjectField(RefFieldName, DetailsSummary);
		EntryObject->RemoveField(TEXT("properties"));
		EntryObject->RemoveField(TEXT("raw_properties"));
		EntryObject->RemoveField(TEXT("readonly_properties"));
		EntryObject->RemoveField(TEXT("missing_properties"));
		EntryObject->RemoveField(TEXT("unsupported_properties"));
		EntryObject->RemoveField(TEXT("raw_properties_count"));
		return true;
	}
}
