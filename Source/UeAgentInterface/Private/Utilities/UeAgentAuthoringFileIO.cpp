#include "Utilities/UeAgentAuthoringFileIO.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/UeAgentAuthoringFieldPolicy.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"

namespace
{
	bool ResolveAuthoringPath(
		const FString& FilePathInput,
		const FString& WorkspaceRootInput,
		FString& OutResolvedPath,
		FString& OutError)
	{
		FString WorkspaceRoot = WorkspaceRootInput;
		WorkspaceRoot.TrimStartAndEndInline();
		if (WorkspaceRoot.IsEmpty())
		{
			WorkspaceRoot = FPaths::ProjectDir();
		}
		else if (FPaths::IsRelative(WorkspaceRoot))
		{
			WorkspaceRoot = FPaths::Combine(FPaths::ProjectDir(), WorkspaceRoot);
		}

		UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
		ResolveOptions.bRequireHjson = true;
		UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
		if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
			WorkspaceRoot,
			FilePathInput,
			ResolveOptions,
			Resolved,
			OutError))
		{
			return false;
		}
		OutResolvedPath = Resolved.AbsolutePath;
		return true;
	}
}

namespace UeAgentAuthoringFileIO
{
	bool SaveObject(
		const FString& FilePathInput,
		const TSharedPtr<FJsonObject>& Object,
		const FSaveOptions& Options,
		FString& OutResolvedPath,
		FString& OutError)
	{
		OutResolvedPath.Reset();
		OutError.Reset();

		FString TrimmedFilePath = FilePathInput;
		TrimmedFilePath.TrimStartAndEndInline();
		if (TrimmedFilePath.IsEmpty())
		{
			OutError = TEXT("missing_output_file");
			return false;
		}

		if (!Object.IsValid() && Options.bRequireObject)
		{
			OutError = TEXT("hjson_object_invalid");
			return false;
		}

		FString ResolvedPath;
		if (!ResolveAuthoringPath(TrimmedFilePath, Options.WorkspaceRoot, ResolvedPath, OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> ObjectToSave = Object.IsValid()
			? UeAgentDetailsAuthoring::CloneObjectTree(Object)
			: MakeShared<FJsonObject>();

		if (!ObjectToSave.IsValid())
		{
			OutError = TEXT("hjson_object_invalid");
			return false;
		}

		if (Options.bExternalizeInlineDetails)
		{
			if (!UeAgentDetailsAuthoring::ExternalizeInlineDetailsInObjectTree(
				ResolvedPath,
				ObjectToSave,
				Options.SourceCommand,
				OutError,
				Options.bScopeInlineDetailsToEntryFile))
			{
				return false;
			}
		}

		if (Options.bNormalizeAuthoringFields)
		{
			UeAgentAuthoringFieldPolicy::NormalizeAuthoringObjectTree(ObjectToSave);
		}

		if (Options.bRemoveDiagnosticAuthoringFields)
		{
			UeAgentDetailsAuthoring::RemoveDiagnosticAuthoringFieldsInObjectTree(ObjectToSave);
		}

		if (!UeAgentStructuredAuthoringIO::SaveObject(ResolvedPath, ObjectToSave, &OutError))
		{
			return false;
		}

		OutResolvedPath = ResolvedPath;
		if (Options.bRecordResolvedPathInWrittenFiles && Options.OutWrittenFiles)
		{
			Options.OutWrittenFiles->Add(ResolvedPath);
		}
		return true;
	}

	bool SaveObject(
		const FString& FilePathInput,
		const TSharedPtr<FJsonObject>& Object,
		const FSaveOptions& Options,
		FString* OutError)
	{
		FString ResolvedPath;
		FString Error;
		const bool bResult = SaveObject(FilePathInput, Object, Options, ResolvedPath, Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return bResult;
	}

	bool LoadObject(
		const FString& FilePathInput,
		TSharedPtr<FJsonObject>& OutObject,
		const FLoadOptions& Options,
		FString& OutResolvedPath,
		FString& OutError)
	{
		OutObject.Reset();
		OutResolvedPath.Reset();
		OutError.Reset();

		FString TrimmedFilePath = FilePathInput;
		TrimmedFilePath.TrimStartAndEndInline();
		if (TrimmedFilePath.IsEmpty())
		{
			OutError = TEXT("missing_input_file");
			return false;
		}

		FString ResolvedPath;
		if (!ResolveAuthoringPath(TrimmedFilePath, Options.WorkspaceRoot, ResolvedPath, OutError))
		{
			return false;
		}
		if (Options.bOptional && !IFileManager::Get().FileExists(*ResolvedPath))
		{
			OutResolvedPath = ResolvedPath;
			return true;
		}

		if (!UeAgentStructuredAuthoringIO::LoadObject(ResolvedPath, OutObject, OutError))
		{
			return false;
		}

		if (Options.bRehydrateGroupedDetails)
		{
			if (!UeAgentDetailsAuthoring::RehydrateGroupedDetailsInObjectTree(ResolvedPath, OutObject, OutError))
			{
				return false;
			}
		}

		OutResolvedPath = ResolvedPath;
		return true;
	}

	bool LoadObject(
		const FString& FilePathInput,
		TSharedPtr<FJsonObject>& OutObject,
		const FLoadOptions& Options,
		FString& OutError)
	{
		FString ResolvedPath;
		return LoadObject(FilePathInput, OutObject, Options, ResolvedPath, OutError);
	}
}
