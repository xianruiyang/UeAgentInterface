#include "Utilities/UeAgentAuthoringFileIO.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/UeAgentAuthoringFieldPolicy.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

namespace
{
	bool ResolveAuthoringPath(
		const FString& FilePathInput,
		const FString& WorkspaceRootInput,
		FString& OutResolvedPath,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutResolvedPath.Reset();
		OutError.Reset();

		FString TrimmedFilePath = FilePathInput;
		TrimmedFilePath.TrimStartAndEndInline();
		if (TrimmedFilePath.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("missing_output_file"),
				TEXT("Saving the authoring document requires a non-empty output file path."), TEXT("authoring_io"));
			OutError.ActualValue = MakeShared<FJsonValueString>(FilePathInput);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty HJSON file path"));
			return false;
		}

		if (!Object.IsValid() && Options.bRequireObject)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_object_invalid"),
				TEXT("The authoring document to save is missing its required object."), TEXT("authoring_io"));
			OutError.TargetPath = FilePathInput;
			OutError.SourceKind = TEXT("native_result");
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
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_object_invalid"),
				TEXT("The authoring document could not be copied for serialization."), TEXT("authoring_io"));
			OutError.TargetPath = FilePathInput;
			OutError.SourceKind = TEXT("native_result");
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
		UE::UAI::Contract::FDiagnostic* OutError)
	{
		FString ResolvedPath;
		UE::UAI::Contract::FDiagnostic Error;
		const bool bResult = SaveObject(FilePathInput, Object, Options, ResolvedPath, Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return bResult;
	}

	static bool LoadParsedDocument(
		const FString& FilePathInput,
		FUeAgentStructuredHjsonDocument& OutDocument,
		const FLoadOptions& Options,
		FString& OutResolvedPath,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutDocument = FUeAgentStructuredHjsonDocument();
		OutResolvedPath.Reset();
		OutError.Reset();

		FString TrimmedFilePath = FilePathInput;
		TrimmedFilePath.TrimStartAndEndInline();
		if (TrimmedFilePath.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("missing_input_file"),
				TEXT("Loading an authoring document requires a non-empty input file path."), TEXT("authoring_io"));
			OutError.ActualValue = MakeShared<FJsonValueString>(FilePathInput);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty HJSON file path"));
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

		if (!UeAgentStructuredAuthoringIO::LoadDocument(ResolvedPath, OutDocument, OutError))
		{
			OutDocument.Root.Reset();
			return false;
		}

		OutResolvedPath = ResolvedPath;
		return true;
	}

	bool LoadDocument(
		const FString& FilePathInput, FUeAgentStructuredHjsonDocument& OutDocument,
		FString& OutResolvedPath, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return LoadParsedDocument(FilePathInput, OutDocument, FLoadOptions(), OutResolvedPath, OutError);
	}

	bool LoadObject(
		const FString& FilePathInput, TSharedPtr<FJsonObject>& OutObject,
		const FLoadOptions& Options, FString& OutResolvedPath,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutObject.Reset();
		OutResolvedPath.Reset();
		FUeAgentStructuredHjsonDocument Document;
		FString ResolvedPath;
		if (!LoadParsedDocument(FilePathInput, Document, Options, ResolvedPath, OutError)) return false;
		OutObject = Document.Root;
		if (OutObject.IsValid() && Options.bRehydrateGroupedDetails
			&& !UeAgentDetailsAuthoring::RehydrateGroupedDetailsInObjectTree(ResolvedPath, OutObject, OutError))
			return false;
		OutResolvedPath = ResolvedPath;
		return true;
	}

	bool LoadObject(
		const FString& FilePathInput,
		TSharedPtr<FJsonObject>& OutObject,
		const FLoadOptions& Options,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FString ResolvedPath;
		return LoadObject(FilePathInput, OutObject, Options, ResolvedPath, OutError);
	}
}
