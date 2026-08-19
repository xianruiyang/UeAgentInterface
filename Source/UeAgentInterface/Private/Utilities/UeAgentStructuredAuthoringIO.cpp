#include "Utilities/UeAgentStructuredAuthoringIO.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"

namespace
{
	bool RequireHjsonFilePath(const FString& FilePath, FString* OutError)
	{
		if (!FPaths::GetExtension(FilePath, false).Equals(TEXT("hjson"), ESearchCase::IgnoreCase))
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("hjson_file_required:%s"), *FilePath);
			}
			return false;
		}
		return true;
	}

	FString MakeIoError(
		const FString& Code,
		const FString& Phase,
		const FString& FilePath,
		const FString& Message)
	{
		FUeAgentHjsonDiagnostic Diagnostic;
		Diagnostic.Phase = Phase;
		Diagnostic.Code = Code;
		Diagnostic.SourceFile = FilePath;
		Diagnostic.Message = Message;
		return Diagnostic.ToString();
	}

	bool SaveUtf8File(const FString& Path, const FString& Text)
	{
		TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*Path));
		if (!Writer)
		{
			return false;
		}
		FTCHARToUTF8 Utf8(*Text);
		Writer->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
		Writer->Flush();
		const bool bSucceeded = !Writer->IsError() && Writer->Close();
		Writer.Reset();
		return bSucceeded;
	}
}

namespace UeAgentStructuredAuthoringIO
{
	FString NormalizeAuthoringPath(const FString& FilePathInput)
	{
		FString Path = FilePathInput;
		Path.TrimStartAndEndInline();
		if (FPaths::IsRelative(Path))
		{
			Path = FPaths::Combine(FPaths::ProjectDir(), Path);
		}
		return UeAgentAuthoringWorkspacePaths::NormalizeAbsolutePath(Path);
	}

	FString WriteObject(const TSharedRef<FJsonObject>& Object)
	{
		return FUeAgentStructuredHjsonFormatter::WriteObject(Object);
	}

	FString WriteObject(
		const TSharedRef<FJsonObject>& Object,
		const FUeAgentStructuredHjsonWriteOptions& Options)
	{
		return FUeAgentStructuredHjsonFormatter::WriteObject(Object, Options);
	}

	bool SaveObject(const FString& FilePath, const TSharedPtr<FJsonObject>& Object, FString* OutError)
	{
		return SaveObject(FilePath, Object, FUeAgentStructuredHjsonWriteOptions(), OutError);
	}

	bool SaveObject(
		const FString& FilePath,
		const TSharedPtr<FJsonObject>& Object,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FString* OutError)
	{
		if (!Object.IsValid())
		{
			if (OutError)
			{
				*OutError = TEXT("hjson_object_invalid");
			}
			return false;
		}

		const FString ResolvedFilePath = NormalizeAuthoringPath(FilePath);
		if (!RequireHjsonFilePath(ResolvedFilePath, OutError))
		{
			return false;
		}

		FUeAgentStructuredResourceTracker ResourceTracker(
			Options.ResourceBudget);
		FString Text;
		FString WriteError;
		if (!ResourceTracker.ChargeOutputChars(
				1,
				TEXT("/"),
				WriteError)
			|| !ResourceTracker.ChargeWorkUnits(
				1,
				TEXT("/"),
				WriteError)
			|| !ResourceTracker.ChargeEstimatedMemoryBytes(
				sizeof(TCHAR),
				TEXT("/"),
				WriteError)
			|| !FUeAgentStructuredHjsonFormatter::TryWriteObject(
				Object.ToSharedRef(),
				Options,
				ResourceTracker,
				Text,
				WriteError))
		{
			if (OutError)
			{
				*OutError = WriteError;
			}
			return false;
		}
		Text += TEXT("\n");

		FUeAgentStructuredHjsonDocument Validation;
		FString ValidationError;
		if (!FUeAgentStructuredHjsonParser::ParseDocument(
				Text,
				ResolvedFilePath,
				ResourceTracker,
				false,
				Validation,
				ValidationError))
		{
			if (OutError)
			{
				*OutError = ValidationError;
			}
			return false;
		}

		const int64 Utf8Length =
			FTCHARToUTF8_Convert::ConvertedLength(*Text, Text.Len());
		if (!ResourceTracker.ChargeEstimatedMemoryBytes(
				Utf8Length,
				TEXT("/"),
				WriteError)
			|| !ResourceTracker.ChargeWorkUnits(
				FMath::Max<int64>(1, Utf8Length / 65536ll),
				TEXT("/"),
				WriteError)
			|| !ResourceTracker.Checkpoint(TEXT("/"), WriteError))
		{
			if (OutError)
			{
				*OutError = WriteError;
			}
			return false;
		}

		const FString Directory = FPaths::GetPath(ResolvedFilePath);
		const bool bDirectoryExisted =
			Directory.IsEmpty() || FPaths::DirectoryExists(Directory);
		if (!Directory.IsEmpty()
			&& !bDirectoryExisted
			&& !IFileManager::Get().MakeDirectory(*Directory, true))
		{
			if (OutError)
			{
				*OutError = MakeIoError(
					TEXT("hjson_io_create_directory_failed"),
					TEXT("create_directory"),
					Directory,
					TEXT("failed to create authoring output directory"));
			}
			return false;
		}

		const FString TemporaryPath = FString::Printf(
			TEXT("%s.tmp.%s"),
			*ResolvedFilePath,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		if (!SaveUtf8File(TemporaryPath, Text))
		{
			IFileManager::Get().Delete(*TemporaryPath, false, true, true);
			if (!bDirectoryExisted && !Directory.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Directory, false, false);
			}
			if (OutError)
			{
				*OutError = MakeIoError(
					TEXT("hjson_io_write_failed"),
					TEXT("write"),
					ResolvedFilePath,
					TEXT("failed to write and flush temporary authoring file"));
			}
			return false;
		}

		if (!ResourceTracker.Checkpoint(TEXT("/"), WriteError))
		{
			IFileManager::Get().Delete(*TemporaryPath, false, true, true);
			if (!bDirectoryExisted && !Directory.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Directory, false, false);
			}
			if (OutError)
			{
				*OutError = WriteError;
			}
			return false;
		}

		if (!IFileManager::Get().Move(*ResolvedFilePath, *TemporaryPath, true, false, false, true))
		{
			IFileManager::Get().Delete(*TemporaryPath, false, true, true);
			if (!bDirectoryExisted && !Directory.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Directory, false, false);
			}
			if (OutError)
			{
				*OutError = MakeIoError(
					TEXT("hjson_io_replace_failed"),
					TEXT("replace"),
					ResolvedFilePath,
					TEXT("failed to atomically replace final authoring file"));
			}
			return false;
		}
		return true;
	}

	bool LoadObject(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		FUeAgentStructuredHjsonDocument Document;
		if (!LoadDocument(FilePath, Document, OutError))
		{
			OutObject.Reset();
			return false;
		}
		OutObject = Document.Root;
		return true;
	}

	bool LoadDocument(
		const FString& FilePath,
		FUeAgentStructuredHjsonDocument& OutDocument,
		FString& OutError)
	{
		return LoadDocument(
			FilePath,
			FUeAgentStructuredResourceBudget(),
			OutDocument,
			OutError);
	}

	bool LoadDocument(
		const FString& FilePath,
		const FUeAgentStructuredResourceBudget& ResourceBudget,
		FUeAgentStructuredHjsonDocument& OutDocument,
		FString& OutError)
	{
		OutDocument = FUeAgentStructuredHjsonDocument();
		OutError.Reset();
		const FString ResolvedFilePath = NormalizeAuthoringPath(FilePath);
		if (!RequireHjsonFilePath(ResolvedFilePath, &OutError))
		{
			return false;
		}

		TUniquePtr<FArchive> Reader(
			IFileManager::Get().CreateFileReader(*ResolvedFilePath));
		if (!Reader)
		{
			const bool bExists = FPaths::FileExists(ResolvedFilePath);
			OutError = MakeIoError(
				bExists
					? TEXT("hjson_io_read_failed")
					: TEXT("hjson_io_not_found"),
				TEXT("read"),
				ResolvedFilePath,
				bExists
					? TEXT("failed to open authoring file")
					: TEXT("authoring file not found"));
			return false;
		}

		const int64 FileBytes = Reader->TotalSize();
		if (FileBytes < 0)
		{
			OutError = MakeIoError(
				TEXT("hjson_io_size_failed"),
				TEXT("read"),
				ResolvedFilePath,
				TEXT("failed to determine authoring file size"));
			return false;
		}

		FUeAgentStructuredResourceTracker ResourceTracker(ResourceBudget);
		const int64 EstimatedReadMemory =
			FileBytes > (MAX_int64 - sizeof(FString)) / 3
				? MAX_int64
				: FileBytes * 3 + sizeof(FString);
		if (!ResourceTracker.ChargeInputBytes(
				FileBytes,
				TEXT("/"),
				OutError)
			|| !ResourceTracker.ChargeWorkUnits(
				FMath::Max<int64>(1, FileBytes / 65536ll),
				TEXT("/"),
				OutError)
			|| !ResourceTracker.ChargeEstimatedMemoryBytes(
				EstimatedReadMemory,
				TEXT("/"),
				OutError))
		{
			return false;
		}

		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Reader))
		{
			OutError = MakeIoError(
				TEXT("hjson_io_read_failed"),
				TEXT("read"),
				ResolvedFilePath,
				TEXT("failed to read authoring file"));
			return false;
		}
		Reader.Reset();

		return FUeAgentStructuredHjsonParser::ParseDocument(
			Text,
			ResolvedFilePath,
			ResourceTracker,
			true,
			OutDocument,
			OutError);
	}

	bool LoadOptionalObject(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		OutObject.Reset();
		OutError.Reset();
		const FString ResolvedFilePath = NormalizeAuthoringPath(FilePath);
		if (!RequireHjsonFilePath(ResolvedFilePath, &OutError))
		{
			return false;
		}
		if (!FPaths::FileExists(ResolvedFilePath))
		{
			return true;
		}
		return LoadObject(ResolvedFilePath, OutObject, OutError);
	}
}
