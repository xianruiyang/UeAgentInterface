#include "Utilities/UeAgentFolderDetailsExport.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/SecureHash.h"
#include "Misc/Paths.h"
#include "Utilities/UeAgentDetailsAuthoring.h"

namespace UeAgentFolderDetailsExport
{
	namespace
	{
		void AddWrittenFile(TArray<FString>* OutWrittenFiles, const FString& FilePath)
		{
			if (!OutWrittenFiles || FilePath.IsEmpty())
			{
				return;
			}

			FString NormalizedPath = FilePath;
			FPaths::NormalizeFilename(NormalizedPath);
			OutWrittenFiles->AddUnique(NormalizedPath);
		}

		void AppendDetailsWrittenFiles(
			const FString& EntryDirectory,
			const FString& DetailsRoot,
			const TSharedPtr<FJsonObject>& DetailsSummary,
			TArray<FString>* OutWrittenFiles)
		{
			if (!OutWrittenFiles || !DetailsSummary.IsValid())
			{
				return;
			}

			FString IndexFile;
			if (DetailsSummary->TryGetStringField(TEXT("index_file"), IndexFile) && !IndexFile.IsEmpty())
			{
				AddWrittenFile(OutWrittenFiles, FPaths::Combine(EntryDirectory, IndexFile));
			}

			const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
			if (DetailsSummary->TryGetArrayField(TEXT("groups"), Groups) && Groups)
			{
				for (const TSharedPtr<FJsonValue>& GroupValue : *Groups)
				{
					const TSharedPtr<FJsonObject> GroupObject = GroupValue.IsValid() ? GroupValue->AsObject() : nullptr;
					if (!GroupObject.IsValid())
					{
						continue;
					}

					FString GroupFile;
					if (GroupObject->TryGetStringField(TEXT("file"), GroupFile) && !GroupFile.IsEmpty())
					{
						AddWrittenFile(OutWrittenFiles, FPaths::Combine(DetailsRoot, GroupFile));
					}
				}
			}

			TArray<FString> CandidateFiles;
			IFileManager::Get().FindFilesRecursive(
				CandidateFiles,
				*FPaths::Combine(DetailsRoot, TEXT("candidates")),
				TEXT("*.hjson"),
				true,
				false,
				false);
			for (const FString& CandidateFile : CandidateFiles)
			{
				AddWrittenFile(OutWrittenFiles, CandidateFile);
			}
		}
	}

	FString MakeDetailsStem(const FString& InValue)
	{
		FString Stem = FPaths::MakeValidFileName(InValue, TEXT('_'));
		Stem.ReplaceInline(TEXT("/"), TEXT("_"));
		Stem.ReplaceInline(TEXT("\\"), TEXT("_"));
		Stem.ReplaceInline(TEXT(":"), TEXT("_"));
		Stem.ReplaceInline(TEXT("."), TEXT("_"));
		Stem.ReplaceInline(TEXT("|"), TEXT("_"));
		Stem.ReplaceInline(TEXT("<"), TEXT("_"));
		Stem.ReplaceInline(TEXT(">"), TEXT("_"));
		Stem.ReplaceInline(TEXT("?"), TEXT("_"));
		Stem.ReplaceInline(TEXT("*"), TEXT("_"));
		Stem.ReplaceInline(TEXT(" "), TEXT("_"));
		Stem.ReplaceInline(TEXT("\""), TEXT("_"));
		Stem.ReplaceInline(TEXT("'"), TEXT("_"));
		Stem.TrimStartAndEndInline();
		if (Stem.IsEmpty())
		{
			Stem = TEXT("owner");
		}
		const FString IdentityHash = FMD5::HashAnsiString(*InValue).Left(8).ToLower();
		return Stem.Left(80) + TEXT("_") + IdentityHash;
	}

	static FString MakeDetailsRelativeRoot(const FString& RefFieldName, const FString& OwnerId, bool bUseOwnerScope)
	{
		const FString RootName = RefFieldName.IsEmpty() ? FString(TEXT("details")) : RefFieldName;
		return bUseOwnerScope ? FPaths::Combine(RootName, MakeDetailsStem(OwnerId)) : RootName;
	}

	static TSharedPtr<FJsonObject> BuildPropertiesContainer(const TArray<TSharedPtr<FJsonValue>>& Properties)
	{
		TSharedPtr<FJsonObject> PropertiesJson = MakeShared<FJsonObject>();
		PropertiesJson->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		PropertiesJson->SetStringField(TEXT("profile"), TEXT("authoring"));
		PropertiesJson->SetNumberField(TEXT("property_count"), Properties.Num());
		PropertiesJson->SetBoolField(TEXT("truncated"), false);
		PropertiesJson->SetArrayField(TEXT("properties"), Properties);
		return PropertiesJson;
	}

	bool AttachOwnerDetails(
		UObject* Owner,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& Category,
		const TSharedPtr<FJsonObject>& EntryObject,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FOwnerDetailsAttachOptions& Options)
	{
		if (!Owner || !EntryObject.IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("details_owner_invalid"),
				!Owner ? TEXT("The details owner object is null.") : TEXT("The entry object that receives the details is null."), TEXT("details_export"));
			OutError.CandidateName = OwnerId;
			OutError.TargetPath = Owner ? Owner->GetPathName() : FString();
			OutError.PropertyPath = !Owner ? TEXT("owner") : TEXT("entry");
			OutError.ActualValuePresent = false;
			return false;
		}

		const FString ResolvedOwnerId = OwnerId.IsEmpty() ? Owner->GetName() : OwnerId;
		const FString RefFieldName = Options.RefFieldName.IsEmpty() ? FString(TEXT("details")) : Options.RefFieldName;
		const FString DetailsRelativeRoot = MakeDetailsRelativeRoot(RefFieldName, ResolvedOwnerId, Options.bUseOwnerScope);
		const FString DetailsRoot = FPaths::Combine(EntryDirectory, DetailsRelativeRoot);
		UeAgentDetailsAuthoring::FOwnerDetailsExportRequest Request =
			UeAgentDetailsAuthoring::MakeOwnerDetailsExportRequest(
				Owner,
				OwnerKind,
				ResolvedOwnerId,
				DetailsRoot,
				SourceCommand,
				FPaths::Combine(DetailsRelativeRoot, TEXT("index.hjson")),
				RefFieldName);

		TSharedPtr<FJsonObject> DetailsSummary;
		if (!UeAgentDetailsAuthoring::ExportOwnerDetails(Request, DetailsSummary, OutError))
		{
			return false;
		}
		if (Options.PostExportDetails
			&& !Options.PostExportDetails(Request, DetailsSummary, OutError))
		{
			return false;
		}
		AppendDetailsWrittenFiles(EntryDirectory, DetailsRoot, DetailsSummary, Options.OutWrittenFiles);
		if (!Category.IsEmpty())
		{
			DetailsSummary->SetStringField(TEXT("category"), Category);
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

	bool AttachOwnerDetails(
		UObject* Owner,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& Category,
		const TSharedPtr<FJsonObject>& EntryObject,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& RefFieldName)
	{
		FOwnerDetailsAttachOptions Options;
		Options.RefFieldName = RefFieldName;
		Options.bUseOwnerScope = false;
		return AttachOwnerDetails(Owner, EntryDirectory, SourceCommand, OwnerKind, OwnerId, Category, EntryObject, OutError, Options);
	}

	bool AttachPropertyArrayDetails(
		const TArray<TSharedPtr<FJsonValue>>& Properties,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& OwnerName,
		const FString& OwnerPath,
		const FString& OwnerClassPath,
		const FString& Category,
		const TSharedPtr<FJsonObject>& EntryObject,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FPropertyArrayDetailsAttachOptions& Options)
	{
		if (!EntryObject.IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("details_entry_object_invalid"), TEXT("The entry object that receives the property details is null."), TEXT("details_export"));
			OutError.CandidateName = OwnerId;
			OutError.TargetPath = OwnerPath;
			OutError.PropertyPath = TEXT("entry");
			OutError.ActualValuePresent = false;
			return false;
		}

		const FString ResolvedOwnerId = OwnerId.IsEmpty() ? FString(TEXT("owner")) : OwnerId;
		UeAgentDetailsAuthoring::FOwnerInfo OwnerInfo;
		OwnerInfo.OwnerKind = OwnerKind;
		OwnerInfo.OwnerId = ResolvedOwnerId;
		OwnerInfo.OwnerName = OwnerName.IsEmpty() ? ResolvedOwnerId : OwnerName;
		OwnerInfo.OwnerPath = OwnerPath;
		OwnerInfo.OwnerClassPath = OwnerClassPath;
		OwnerInfo.SourceCommand = SourceCommand;

		const FString RefFieldName = Options.RefFieldName.IsEmpty() ? FString(TEXT("details")) : Options.RefFieldName;
		const FString DetailsRelativeRoot = MakeDetailsRelativeRoot(RefFieldName, ResolvedOwnerId, Options.bUseOwnerScope);
		const FString DetailsRoot = FPaths::Combine(EntryDirectory, DetailsRelativeRoot);
		const FString IndexFile = FPaths::Combine(DetailsRelativeRoot, TEXT("index.hjson"));

		TSharedPtr<FJsonObject> DetailsSummary;
		if (!UeAgentDetailsAuthoring::WriteGroupedDetails(
			DetailsRoot,
			OwnerInfo,
			BuildPropertiesContainer(Properties),
			DetailsSummary,
			OutError,
			IndexFile))
		{
			return false;
		}

		DetailsSummary->SetStringField(TEXT("index_file"), IndexFile.Replace(TEXT("\\"), TEXT("/")));
		DetailsSummary->SetStringField(TEXT("ref_field"), RefFieldName);
		DetailsSummary->SetStringField(TEXT("owner_kind"), OwnerInfo.OwnerKind);
		DetailsSummary->SetStringField(TEXT("owner_id"), OwnerInfo.OwnerId);
		if (!Category.IsEmpty())
		{
			DetailsSummary->SetStringField(TEXT("category"), Category);
		}

		AppendDetailsWrittenFiles(EntryDirectory, DetailsRoot, DetailsSummary, Options.OutWrittenFiles);
		EntryObject->RemoveField(Options.PropertiesFieldName.IsEmpty() ? FString(TEXT("properties")) : Options.PropertiesFieldName);
		if (!Options.CountFieldName.IsEmpty())
		{
			EntryObject->RemoveField(Options.CountFieldName);
		}
		EntryObject->SetObjectField(RefFieldName, DetailsSummary);
		return true;
	}
}
