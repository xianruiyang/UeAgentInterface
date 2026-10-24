#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentHjsonDiagnostic.h"

#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;

	namespace
	{
		const FString ContentGraphWorkspaceSchema =
			TEXT("uai.content_graph.workspace/1");
		const FString ContentGraphWorkspaceManifest =
			TEXT(".uai/workspace.hjson");

		struct FSourceFileStamp
		{
			FString Path;
			int64 Size = -1;
			int64 TimestampTicks = 0;

			bool operator==(const FSourceFileStamp& Other) const
			{
				return Path == Other.Path
					&& Size == Other.Size
					&& TimestampTicks == Other.TimestampTicks;
			}
		};

		struct FSourceVersionSnapshot
		{
			TArray<FSourceFileStamp> Files;

			bool operator==(const FSourceVersionSnapshot& Other) const
			{
				return Files == Other.Files;
			}
		};

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Reason,
			FString SourceUnit,
			FString Pointer = TEXT("/"),
			TSharedPtr<FJsonValue> Actual = nullptr,
			TSharedPtr<FJsonValue> Expected = nullptr)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("workspace_load"));
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			if (Pointer != TEXT("/")) Diagnostic.Source.JsonPath = Pointer;
			OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit, Pointer);
			return false;
		}

		bool FailEvidence(FWorkspaceError& OutError, const TCHAR* Code, const TCHAR* Reason,
			const FString& SourceUnit, const FString& Expected, const FString& Actual, const FString& Pointer = TEXT("/"))
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_load"));
			Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			Diagnostic.Source.InternalJsonPath = Pointer;
			OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit, Pointer);
			return false;
		}

		bool FailSourceField(FWorkspaceError& OutError, const FString& Target,
			const TSharedPtr<FJsonObject>& Source, const TCHAR* Field, const TCHAR* Reason,
			const TCHAR* Expected, const TCHAR* Code = TEXT("content_graph_source_invalid"), bool bPublicActual = true)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_load"));
			Diagnostic.TargetPath = Target;
			Diagnostic.Source.JsonPath = FString(TEXT("/source/")) + Field;
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			const auto Actual = Source ? Source->TryGetField(Field) : nullptr;
			Diagnostic.ActualValuePresent = Actual.IsValid();
			if (Actual)
			{
				const FString Type = UeAgentJsonDiagnostics::JsonValueTypeToString(Actual);
				if (!bPublicActual)
				{
					Diagnostic.ActualEvidence = Actual;
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Type);
				}
				else if (Actual->Type == EJson::Array || Actual->Type == EJson::Object)
				{
					const auto Summary = MakeShared<FJsonObject>();
					Summary->SetStringField(TEXT("type"), Type);
					if (Actual->Type == EJson::Array) Summary->SetNumberField(TEXT("count"), Actual->AsArray().Num());
					else if (Actual->AsObject().IsValid()) Summary->SetNumberField(TEXT("count"), Actual->AsObject()->Values.Num());
					Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Summary);
					Diagnostic.ActualEvidence = Actual;
				}
				else Diagnostic.ActualValue = Actual;
			}
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}

		bool FailSourceShape(FWorkspaceError& OutError, const FString& Target, const FJsonObject& Source, const TCHAR* ValueField)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("content_graph_source_invalid"),
				FString::Printf(TEXT("An import source must contain exactly 'kind' and '%s'."), ValueField), TEXT("workspace_load"));
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.Source.JsonPath = TEXT("/source");
			TArray<TSharedPtr<FJsonValue>> ActualFields;
			for (const auto& Field : Source.Values) ActualFields.Add(MakeShared<FJsonValueString>(Field.Key));
			Diagnostic.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(ActualFields));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueString>(TEXT("kind")), MakeShared<FJsonValueString>(ValueField)});
			OutError.SetFailure(MoveTemp(Diagnostic), Target);
			return false;
		}

		bool FailPreparedField(FWorkspaceError& OutError, const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field, const TCHAR* Requirement, const FString& SourceUnit,
			const FString& ParentPointer = FString())
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("content_graph_normalized_source_invalid"),
				FString::Printf(TEXT("Prepared import metadata field '%s' %s."), Field, Requirement), TEXT("workspace_load"));
			Diagnostic.Source.InternalJsonPath = ParentPointer + TEXT("/") + Field;
			Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Requirement);
			Diagnostic.ActualEvidence = Object ? Object->TryGetField(Field) : nullptr;
			OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit);
			return false;
		}

		bool ReadPreparedString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
			FString& OutValue, bool bAllowEmpty, FWorkspaceError& OutError,
			const FString& SourceUnit, const FString& ParentPointer = FString())
		{
			if (Object->TryGetStringField(Field, OutValue) && (bAllowEmpty || !OutValue.IsEmpty())) return true;
			return FailPreparedField(OutError, Object, Field,
				bAllowEmpty ? TEXT("must be a string") : TEXT("must be a non-empty string"), SourceUnit, ParentPointer);
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			std::initializer_list<TPair<EUeAgentResourceDimension, int64>> Charges,
			const FString& Path,
			FWorkspaceError& OutError)
		{
			TMap<EUeAgentResourceDimension, int64> Values;
			for (const TPair<EUeAgentResourceDimension, int64>& Value : Charges)
			{
				Values.Add(Value.Key, Value.Value);
			}
			FUeAgentResourceChargeError ChargeError;
			if (Resources.TryChargeMany(Values, Path, ChargeError))
			{
				return true;
			}
			ChargeError.Source.InternalJsonPath = MoveTemp(ChargeError.Source.JsonPath);
			ChargeError.Source.JsonPath.Reset();
			OutError.SetFailure(MoveTemp(ChargeError), Path);
			return false;
		}

		FString Sha256(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			const uint8 Empty = 0;
			SHA256(
				Bytes.IsEmpty() ? &Empty : Bytes.GetData(),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			return TEXT("sha256:")
				+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
		}

		FString Sha256Text(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
			return Sha256(Bytes);
		}

		bool IsSha256(const FString& Value)
		{
			if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71)
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsHexDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
		}

		bool TryBuildSemanticFingerprint(
			const TArray<FContentGraphImportDocument>& Documents,
			const FString& EntryFile,
			FUeAgentResourceTracker& Resources,
			FString& OutFingerprint,
			FWorkspaceError& OutError,
			const FString& DocumentRoot = FString())
		{
			OutFingerprint.Reset();
			if (Documents.IsEmpty() || EntryFile.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("content_graph_semantic_source_invalid"), TEXT("An import source requires at least one document and an entry file."),
					EntryFile, TEXT("/"), MakeShared<FJsonValueNumber>(Documents.Num()));
			}
			TArray<TPair<FString, FString>> CanonicalDocuments;
			CanonicalDocuments.Reserve(Documents.Num());
			int32 EntryMatches = 0;
			FWorkspaceError Diagnostics;
			for (const FContentGraphImportDocument& SourceDocument : Documents)
			{
				FHjsonDocumentPtr Parsed;
				if (!TryParseHjsonDocument(
						SourceDocument.GetDocument(),
						DocumentRoot.IsEmpty() ? SourceDocument.GetRelativePath()
							: FPaths::Combine(DocumentRoot, SourceDocument.GetRelativePath()),
						Resources,
						Parsed,
						OutError)
					|| !Parsed.IsValid())
				{
					Diagnostics.Append(OutError);
					continue;
				}
				TArray<uint8> CanonicalUtf8;
				if (!TryWriteHjsonDocument(
						Parsed.ToSharedRef(),
						Resources,
						CanonicalUtf8,
						OutError))
				{
					Diagnostics.Append(OutError);
					continue;
				}
				const FString RelativeIdentity =
					SourceDocument.GetRelativePath().ToLower();
				CanonicalDocuments.Emplace(
					RelativeIdentity,
					Sha256(CanonicalUtf8));
				EntryMatches += SourceDocument.GetRelativePath().Equals(
					EntryFile,
					ESearchCase::IgnoreCase)
					? 1
					: 0;
			}
			if (Diagnostics.HasErrors())
			{
				Diagnostics.MarkUnchecked(TEXT("workspace_resolution"));
				OutError = MoveTemp(Diagnostics);
				return false;
			}
			if (EntryMatches != 1)
			{
				return Fail(
					OutError,
					TEXT("content_graph_semantic_entry_invalid"), TEXT("The entry file must identify exactly one source document."),
					EntryFile, TEXT("/"), MakeShared<FJsonValueNumber>(EntryMatches), MakeShared<FJsonValueNumber>(1));
			}
			if (CanonicalDocuments.Num() == 1)
			{
				OutFingerprint = CanonicalDocuments[0].Value;
				return true;
			}
			CanonicalDocuments.Sort([](
				const TPair<FString, FString>& Left,
				const TPair<FString, FString>& Right)
			{
				return Left.Key < Right.Key;
			});
			FString Material = TEXT("entry|") + EntryFile.ToLower();
			for (const TPair<FString, FString>& Document : CanonicalDocuments)
			{
				Material += TEXT("|") + Document.Key + TEXT("|") + Document.Value;
			}
			OutFingerprint = Sha256Text(Material);
			return true;
		}

		bool IsReparsePoint(const FString& Path)
		{
			return FPlatformFileManager::Get()
				.GetPlatformFile()
				.IsSymlink(*Path) == ESymlinkResult::Symlink;
		}

		FString NormalizedFullPath(const FString& Path, const bool bDirectory)
		{
			FString Result = FPaths::ConvertRelativePathToFull(Path);
			FPaths::CollapseRelativeDirectories(Result);
			if (bDirectory)
			{
				FPaths::NormalizeDirectoryName(Result);
			}
			else
			{
				FPaths::NormalizeFilename(Result);
			}
			FPaths::RemoveDuplicateSlashes(Result);
			return Result;
		}

		bool IsWithinRoot(const FString& Root, const FString& Path)
		{
			FString Prefix = Root;
			if (!Prefix.EndsWith(TEXT("/")))
			{
				Prefix += TEXT("/");
			}
			return Path.StartsWith(Prefix, ESearchCase::IgnoreCase);
		}

		bool RejectReparseAncestors(
			const FString& Root,
			const FString& File,
			FWorkspaceError& OutError)
		{
			if (IsReparsePoint(File))
			{
				return Fail(
					OutError,
					TEXT("workspace_reparse_point_rejected"), TEXT("The source file is a reparse point; workspace imports require regular files."),
					File);
			}
			FString Relative = File;
			FString RelativeBase = Root;
			if (!RelativeBase.EndsWith(TEXT("/")))
			{
				RelativeBase += TEXT("/");
			}
			if (!FPaths::MakePathRelativeTo(Relative, *RelativeBase))
			{
				return Fail(
					OutError,
					TEXT("workspace_path_escape"), TEXT("The source file cannot be resolved relative to the workspace directory."),
					File);
			}
			FPaths::NormalizeFilename(Relative);
			TArray<FString> Segments;
			Relative.ParseIntoArray(Segments, TEXT("/"), true);
			FString Current = Root;
			for (int32 Index = 0; Index + 1 < Segments.Num(); ++Index)
			{
				Current = FPaths::Combine(Current, Segments[Index]);
				if (IsReparsePoint(Current))
				{
					return Fail(
						OutError,
						TEXT("workspace_reparse_point_rejected"), TEXT("A source file ancestor is a reparse point; workspace imports require regular directories."),
						Current);
				}
			}
			return true;
		}

		bool AddFileStamp(
			const FString& File,
			FSourceVersionSnapshot& OutSnapshot,
			FWorkspaceError& OutError)
		{
			const int64 Size = IFileManager::Get().FileSize(*File);
			if (Size < 0)
			{
				return Fail(
					OutError,
					TEXT("workspace_source_read_failed"), TEXT("The source file size could not be read."),
					File);
			}
			OutSnapshot.Files.Add({
				File,
				Size,
				IFileManager::Get().GetTimeStamp(*File).GetTicks(),
			});
			return true;
		}

		bool CaptureDirectory(
			const FString& SourcePath,
			FSourceVersionSnapshot& OutSnapshot,
			FWorkspaceError& OutError)
		{
			const FString Root = NormalizedFullPath(SourcePath, true);
			if (!IFileManager::Get().DirectoryExists(*Root))
			{
				return Fail(
					OutError,
					TEXT("workspace_source_directory_missing"), TEXT("The workspace source directory does not exist."),
					Root, TEXT("/source/path"), MakeShared<FJsonValueString>(Root));
			}
			if (IsReparsePoint(Root))
			{
				return Fail(
					OutError,
					TEXT("workspace_reparse_point_rejected"), TEXT("The workspace directory is a reparse point."),
					Root, TEXT("/source/path"), MakeShared<FJsonValueString>(Root));
			}
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(
				Files,
				*Root,
				TEXT("*.hjson"),
				true,
				false);
			for (FString& File : Files)
			{
				File = NormalizedFullPath(File, false);
			}
			Files.Sort();
			for (const FString& File : Files)
			{
				if (!IsWithinRoot(Root, File))
				{
					return Fail(
						OutError,
						TEXT("workspace_path_escape"), TEXT("A discovered source file is outside the workspace directory."),
						File);
				}
				if (!RejectReparseAncestors(Root, File, OutError)
					|| !AddFileStamp(File, OutSnapshot, OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool CaptureSourceVersion(
			const FWorkspaceSource& Source,
			FSourceVersionSnapshot& OutSnapshot,
			FWorkspaceError& OutError)
		{
			OutSnapshot.Files.Reset();
			if (Source.GetKind() == EWorkspaceSourceKind::Inline)
			{
				return true;
			}
			if (Source.GetKind() == EWorkspaceSourceKind::Directory)
			{
				return CaptureDirectory(
					Source.GetPath(),
					OutSnapshot,
					OutError);
			}
			const FString File = NormalizedFullPath(Source.GetPath(), false);
			if (!IFileManager::Get().FileExists(*File))
			{
				return Fail(
					OutError,
					TEXT("workspace_source_file_missing"), TEXT("The workspace source file does not exist."),
					File);
			}
			if (IsReparsePoint(File))
			{
				return Fail(
					OutError,
					TEXT("workspace_reparse_point_rejected"), TEXT("The source file is a reparse point; workspace imports require regular files."),
					File);
			}
			return AddFileStamp(File, OutSnapshot, OutError);
		}

		bool TryNormalizeRelativePath(
			FString Value,
			FString& OutPath,
			FWorkspaceError& OutError,
			const FString& SourceUnit)
		{
			OutPath.Reset();
			const FString Original = Value;
			Value.TrimStartAndEndInline();
			FPaths::NormalizeFilename(Value);
			FPaths::RemoveDuplicateSlashes(Value);
			if (Value.IsEmpty()
				|| !FPaths::IsRelative(Value)
				|| Value.Contains(TEXT(":"))
				|| Value == TEXT("..")
				|| Value.StartsWith(TEXT("../"))
				|| Value.Contains(TEXT("/../"))
				|| Value.EndsWith(TEXT("/..")))
			{
				return Fail(
					OutError,
					TEXT("workspace_path_invalid"), TEXT("A workspace document path must be non-empty, relative, and remain inside the workspace."),
					SourceUnit, TEXT("/"), MakeShared<FJsonValueString>(Original));
			}
			FString Collapsed = Value;
			if (!FPaths::CollapseRelativeDirectories(Collapsed)
				|| Collapsed.IsEmpty()
				|| Collapsed.StartsWith(TEXT("../")))
			{
				return Fail(
					OutError,
					TEXT("workspace_path_invalid"), TEXT("The document path cannot be normalized to a location inside the workspace."),
					SourceUnit, TEXT("/"), MakeShared<FJsonValueString>(Original));
			}
			OutPath = MoveTemp(Collapsed);
			return true;
		}

		bool TryReadUtf8File(
			const FString& Path,
			FUeAgentResourceTracker& Resources,
			FString& OutText,
			TArray<uint8>& OutBytes,
			FWorkspaceError& OutError)
		{
			OutText.Reset();
			OutBytes.Reset();
			if (!FFileHelper::LoadFileToArray(OutBytes, *Path))
			{
				return Fail(
					OutError,
					TEXT("workspace_source_read_failed"), TEXT("The workspace source file could not be read."),
					Path);
			}
			if (!Charge(
					Resources,
					{
						{EUeAgentResourceDimension::InputBytes, OutBytes.Num()},
						{EUeAgentResourceDimension::StringBytes, OutBytes.Num()},
						{EUeAgentResourceDimension::Files, 1},
						{EUeAgentResourceDimension::WorkUnits, 1},
						{EUeAgentResourceDimension::EstimatedMemoryBytes, OutBytes.Num() * 2},
					},
					Path,
					OutError))
			{
				return false;
			}
			const FUTF8ToTCHAR Converted(
				reinterpret_cast<const ANSICHAR*>(OutBytes.GetData()),
				OutBytes.Num());
			OutText = FString(Converted.Length(), Converted.Get());
			if (!OutText.IsEmpty() && OutText[0] == 0xfeff)
			{
				OutText.RightChopInline(1, EAllowShrinking::No);
			}
			return true;
		}

		bool FailManifestField(FWorkspaceError& OutError, const FString& Reason, const FHjsonNode& Object,
			const FString& Field, const FString& Expected, bool bPublicActual = false)
		{
			OutError.SetFailure(HjsonFieldFailure(TEXT("content_graph_workspace_manifest_invalid"), Reason,
				TEXT("workspace_load"), Object, Field, MakeShared<FJsonValueString>(Expected), bPublicActual));
			return false;
		}

		bool RequiredString(
			const FHjsonNode& Object,
			const TCHAR* Field,
			FString& OutValue,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Value = Object.Get(Field);
			if (!Value || Value->GetKind() != EHjsonNodeKind::String)
			{
				return FailManifestField(OutError, FString::Printf(TEXT("Workspace metadata field '%s' must be a string."), Field),
					Object, Field, TEXT("string"));
			}
			OutValue = Value->GetText();
			OutValue.TrimStartAndEndInline();
			if (!OutValue.IsEmpty()) return true;
			return FailManifestField(OutError, FString::Printf(TEXT("Workspace metadata field '%s' must not be empty."), Field),
				Object, Field, TEXT("non-empty string"), true);
		}

		bool RequiredInteger(
			const FHjsonNode& Object,
			const TCHAR* Field,
			int64& OutValue,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Value = Object.Get(Field);
			if (Value
				&& Value->GetKind() == EHjsonNodeKind::Integer
				&& LexTryParseString(OutValue, *Value->GetLexeme())) return true;
			return FailManifestField(OutError, FString::Printf(TEXT("Workspace metadata field '%s' must be a signed 64-bit integer."), Field),
				Object, Field, TEXT("int64"));
		}

		bool TryParseManifest(
			const FString& ManifestText,
			const FString& ManifestPath,
			const FString& ExpectedTargetIdentity,
			FUeAgentResourceTracker& Resources,
			FString& OutEntryFile,
			FString& OutBaseRevision,
			TArray<TPair<FString, FString>>& OutFiles,
			bool& bOutSemanticWorkspace,
			FWorkspaceError& OutError)
		{
			OutEntryFile.Reset();
			OutBaseRevision.Reset();
			OutFiles.Reset();
			bOutSemanticWorkspace = false;
			FHjsonDocumentPtr Manifest;
			if (!TryParseHjsonDocument(
					ManifestText,
					ManifestPath,
					Resources,
					Manifest,
					OutError)
				|| !Manifest.IsValid()
				|| Manifest->GetRoot()->GetKind() != EHjsonNodeKind::Object)
			{
				if (OutError.GetCode().IsEmpty())
				{
					if (Manifest.IsValid())
						OutError.SetFailure(HjsonNodeFailure(TEXT("content_graph_workspace_manifest_invalid"),
							TEXT("The workspace metadata document must have an object root."), TEXT("workspace_load"),
							*Manifest->GetRoot(), MakeShared<FJsonValueString>(TEXT("object")), false));
					else
						OutError.SetSourceFailure(TEXT("content_graph_workspace_manifest_invalid"),
							TEXT("Workspace metadata parsing returned no document or diagnostic."), ManifestPath);
				}
				return false;
			}
			const FHjsonNode& Root = *Manifest->GetRoot();
			FString Schema;
			if (!RequiredString(Root, TEXT("workspace_schema"), Schema, OutError))
			{
				return false;
			}
			if (Schema == GetSemanticWorkspaceSchemaId())
			{
				bOutSemanticWorkspace = true;
				return true;
			}
			FString TargetIdentity;
			int64 FormatVersion = 0;
			if (!RequiredInteger(Root, TEXT("format_version"), FormatVersion, OutError)
				|| !RequiredString(Root, TEXT("target_identity"), TargetIdentity, OutError)
				|| !RequiredString(Root, TEXT("base_revision"), OutBaseRevision, OutError)
				|| !RequiredString(Root, TEXT("entry_file"), OutEntryFile, OutError))
			{
				return false;
			}
			if (Schema != ContentGraphWorkspaceSchema)
			{
				return Fail(
					OutError,
					TEXT("content_graph_workspace_schema_unsupported"), TEXT("The workspace metadata schema is unsupported."),
					ManifestPath, TEXT("/workspace_schema"), MakeShared<FJsonValueString>(Schema), MakeShared<FJsonValueString>(ContentGraphWorkspaceSchema));
			}
			if (FormatVersion != 1)
				return Fail(OutError, TEXT("content_graph_workspace_schema_unsupported"), TEXT("The workspace format version is unsupported."),
					ManifestPath, TEXT("/format_version"), MakeShared<FJsonValueNumber>(FormatVersion), MakeShared<FJsonValueNumber>(1));
			if (TargetIdentity != ExpectedTargetIdentity)
			{
				return FailEvidence(
					OutError,
					TEXT("content_graph_workspace_target_mismatch"), TEXT("The workspace belongs to a different target."),
					ManifestPath, ExpectedTargetIdentity, TargetIdentity, TEXT("/target_identity"));
			}
			if (!TryNormalizeRelativePath(
					OutEntryFile,
					OutEntryFile,
					OutError,
					ManifestPath))
			{
				return false;
			}
			const FHjsonNode* Files = Root.Get(TEXT("files"));
			const FHjsonNode* References = Root.Get(TEXT("references"));
			if (!Files || Files->GetKind() != EHjsonNodeKind::Array || Files->GetItems().IsEmpty())
			{
				return FailManifestField(OutError, TEXT("Workspace metadata requires a non-empty files array."),
					Root, TEXT("files"), TEXT("non-empty array"), true);
			}
			if (!References || References->GetKind() != EHjsonNodeKind::Array)
				return FailManifestField(OutError, TEXT("Workspace metadata requires a references array."), Root, TEXT("references"), TEXT("array"), true);
			TSet<FString> Identities;
			for (const FHjsonNodeRef& FileValue : Files->GetItems())
			{
				if (FileValue->GetKind() != EHjsonNodeKind::Object)
				{
					OutError.SetFailure(HjsonNodeFailure(TEXT("content_graph_workspace_manifest_invalid"),
						TEXT("Each workspace files entry must be an object."), TEXT("workspace_load"), FileValue.Get(),
						MakeShared<FJsonValueString>(TEXT("object"))));
					return false;
				}
				FString RelativePath;
				FString ContentHash;
				if (!RequiredString(*FileValue, TEXT("path"), RelativePath, OutError)
					|| !RequiredString(*FileValue, TEXT("content_hash"), ContentHash, OutError)
					|| !TryNormalizeRelativePath(
						RelativePath,
						RelativePath,
						OutError,
						ManifestPath))
				{
					return false;
				}
				const FString Identity = RelativePath.ToLower();
				if (Identities.Contains(Identity))
					return FailManifestField(OutError, TEXT("A document path is declared more than once."), *FileValue, TEXT("path"), TEXT("unique document path"), true);
				if (!ContentHash.StartsWith(TEXT("sha256:")) || ContentHash.Len() != 71)
					return FailEvidence(OutError, TEXT("content_graph_workspace_manifest_invalid"), TEXT("A workspace files entry has an invalid SHA-256 content digest."),
						ManifestPath, TEXT("sha256: followed by 64 hexadecimal characters"), ContentHash,
						FileValue->Get(TEXT("content_hash"))->GetProvenance().GetJsonPointer());
				if (!RelativePath.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase)
					|| RelativePath.Equals(
						ContentGraphWorkspaceManifest,
						ESearchCase::IgnoreCase))
				{
					return FailManifestField(OutError, TEXT("Workspace files must reference HJSON documents other than the metadata file itself."),
						*FileValue, TEXT("path"), TEXT("HJSON document other than the metadata file"), true);
				}
				Identities.Add(Identity);
				OutFiles.Emplace(MoveTemp(RelativePath), ContentHash.ToLower());
			}
			if (!Identities.Contains(OutEntryFile.ToLower()))
			{
				OutError.SetFailure(HjsonFieldFailure(TEXT("content_graph_workspace_entry_missing"),
					TEXT("The entry document is not declared in the workspace files list."), TEXT("workspace_load"),
					Root, TEXT("entry_file"), MakeShared<FJsonValueString>(TEXT("declared document path"))));
				return false;
			}
			for (const FHjsonNodeRef& Reference : References->GetItems())
			{
				if (Reference->GetKind() != EHjsonNodeKind::String)
				{
					OutError.SetFailure(HjsonNodeFailure(TEXT("content_graph_workspace_manifest_invalid"),
						TEXT("Each workspace reference must be a document path string."), TEXT("workspace_load"), Reference.Get(),
						MakeShared<FJsonValueString>(TEXT("string"))));
					return false;
				}
				FString RelativePath;
				if (!TryNormalizeRelativePath(
						Reference->GetText(),
						RelativePath,
						OutError,
						ManifestPath)) return false;
				if (!Identities.Contains(RelativePath.ToLower()))
				{
					OutError.SetFailure(HjsonNodeFailure(TEXT("content_graph_workspace_reference_unresolved"),
						TEXT("The workspace reference does not identify a declared document."), TEXT("workspace_load"), Reference.Get(),
						MakeShared<FJsonValueString>(TEXT("declared document path"))));
					return false;
				}
			}
			return true;
		}

		bool TryLoadDirectorySource(
			const FString& SourcePath,
			const FString& ExpectedTargetIdentity,
			FUeAgentResourceTracker& Resources,
			FResolvedContentGraphImportSourcePtr& OutSource,
			FWorkspaceError& OutError)
		{
			const FString Root = NormalizedFullPath(SourcePath, true);
			if (!FPaths::IsRelative(SourcePath) && !IFileManager::Get().DirectoryExists(*Root))
			{
				return Fail(
					OutError,
					TEXT("workspace_source_directory_missing"), TEXT("The workspace source directory does not exist."),
					Root);
			}
			if (FPaths::IsRelative(SourcePath))
			{
				return Fail(OutError, TEXT("workspace_path_invalid"), TEXT("The resolved workspace directory path must be absolute."), SourcePath);
			}
			const FString ManifestPath =
				FPaths::Combine(Root, ContentGraphWorkspaceManifest);
			if (!IFileManager::Get().FileExists(*ManifestPath))
			{
				return Fail(
					OutError,
					TEXT("content_graph_workspace_manifest_missing"), TEXT("The directory has no workspace metadata; export the workspace before importing it."),
					ManifestPath);
			}
			FSourceVersionSnapshot Before;
			if (!CaptureDirectory(Root, Before, OutError))
			{
				return false;
			}
			FString ManifestText;
			TArray<uint8> ManifestBytes;
			if (!RejectReparseAncestors(Root, ManifestPath, OutError)
				|| !TryReadUtf8File(
					ManifestPath,
					Resources,
					ManifestText,
					ManifestBytes,
					OutError))
			{
				return false;
			}
			FString EntryFile;
			FString BaseRevision;
			TArray<TPair<FString, FString>> DeclaredFiles;
			bool bSemanticWorkspace = false;
			if (!TryParseManifest(
					ManifestText,
					ManifestPath,
					ExpectedTargetIdentity,
					Resources,
					EntryFile,
					BaseRevision,
					DeclaredFiles,
					bSemanticWorkspace,
					OutError))
			{
				return false;
			}
			if (bSemanticWorkspace)
			{
				FSourceVersionSnapshot After;
				if (!CaptureDirectory(Root, After, OutError)) return false;
				if (!(Before == After))
				{
					return Fail(
						OutError,
						TEXT("workspace_source_version_changed"), TEXT("Workspace source files changed while they were being loaded."),
						Root);
				}
				OutSource = MakeShared<FResolvedContentGraphImportSource>(
					EWorkspaceSourceKind::Directory,
					Root,
					ExpectedTargetIdentity,
					FString(),
					FString(),
					FString(),
					FString(),
					TArray<FContentGraphImportDocument>(),
					true);
				return true;
			}
			TSet<FString> DeclaredIdentities;
			TArray<FContentGraphImportDocument> Documents;
			FString FingerprintMaterial = Sha256(ManifestBytes);
			for (const TPair<FString, FString>& Declared : DeclaredFiles)
			{
				const FString FullPath = NormalizedFullPath(
					FPaths::Combine(Root, Declared.Key),
					false);
				if (!IsWithinRoot(Root, FullPath))
					return Fail(OutError, TEXT("workspace_path_escape"), TEXT("A declared source file resolves outside the workspace directory."), FullPath);
				if (!RejectReparseAncestors(Root, FullPath, OutError))
				{
					return false;
				}
				FString Document;
				TArray<uint8> Bytes;
				if (!TryReadUtf8File(
						FullPath,
						Resources,
						Document,
						Bytes,
						OutError))
				{
					return false;
				}
				const FString ActualHash = Sha256(Bytes);
				if (ActualHash != Declared.Value)
				{
					return FailEvidence(
						OutError,
						TEXT("content_graph_workspace_digest_mismatch"), TEXT("The source document content does not match its recorded workspace digest."),
						FullPath, Declared.Value, ActualHash);
				}
				DeclaredIdentities.Add(Declared.Key.ToLower());
				FingerprintMaterial += TEXT("|") + Declared.Key
					+ TEXT("|") + ActualHash;
				Documents.Emplace(Declared.Key, MoveTemp(Document), ActualHash);
			}
			TArray<FString> ActualFiles;
			IFileManager::Get().FindFilesRecursive(
				ActualFiles,
				*Root,
				TEXT("*.hjson"),
				true,
				false);
			for (FString Actual : ActualFiles)
			{
				Actual = NormalizedFullPath(Actual, false);
				FString Relative = Actual;
				FString RelativeRoot = Root;
				if (!RelativeRoot.EndsWith(TEXT("/")))
				{
					RelativeRoot += TEXT("/");
				}
				if (!FPaths::MakePathRelativeTo(Relative, *RelativeRoot))
				{
					return Fail(OutError, TEXT("workspace_path_escape"), TEXT("A source file cannot be resolved relative to the workspace directory."), Actual);
				}
				FPaths::NormalizeFilename(Relative);
				if (!Relative.Equals(
						ContentGraphWorkspaceManifest,
						ESearchCase::IgnoreCase)
					&& !DeclaredIdentities.Contains(Relative.ToLower()))
				{
					return Fail(
						OutError,
						TEXT("content_graph_workspace_file_undeclared"), TEXT("An HJSON file is present but is not declared by the workspace."),
						Actual);
				}
			}
			FSourceVersionSnapshot After;
			if (!CaptureDirectory(Root, After, OutError)) return false;
			if (!(Before == After))
			{
				return Fail(
					OutError,
					TEXT("workspace_source_version_changed"), TEXT("Workspace source files changed while they were being loaded."),
					Root);
			}
			FString SemanticFingerprint;
			if (!TryBuildSemanticFingerprint(
					Documents,
					EntryFile,
					Resources,
					SemanticFingerprint,
					OutError, Root))
			{
				return false;
			}
			OutSource = MakeShared<FResolvedContentGraphImportSource>(
				EWorkspaceSourceKind::Directory,
				Root,
				ExpectedTargetIdentity,
				MoveTemp(BaseRevision),
				MoveTemp(EntryFile),
				Sha256Text(FingerprintMaterial),
				MoveTemp(SemanticFingerprint),
				MoveTemp(Documents));
			return true;
		}

		bool TryLoadSingleFileSource(
			const FString& SourcePath,
			const FString& ExpectedTargetIdentity,
			FUeAgentResourceTracker& Resources,
			FResolvedContentGraphImportSourcePtr& OutSource,
			FWorkspaceError& OutError)
		{
			if (FPaths::IsRelative(SourcePath))
			{
				return Fail(OutError, TEXT("workspace_path_invalid"), TEXT("The resolved source file path must be absolute."), SourcePath);
			}
			const FString Path = NormalizedFullPath(SourcePath, false);
			FSourceVersionSnapshot Before;
			if (!CaptureSourceVersion(
					FWorkspaceSource::SingleFile(Path),
					Before,
					OutError))
			{
				return false;
			}
			FString Document;
			TArray<uint8> Bytes;
			if (!TryReadUtf8File(Path, Resources, Document, Bytes, OutError))
			{
				return false;
			}
			FSourceVersionSnapshot After;
			if (!CaptureSourceVersion(
					FWorkspaceSource::SingleFile(Path),
					After,
					OutError)) return false;
			if (!(Before == After))
			{
				return Fail(
					OutError,
					TEXT("workspace_source_version_changed"), TEXT("The source file changed while it was being read."),
					Path);
			}
			const FString Hash = Sha256(Bytes);
			TArray<FContentGraphImportDocument> Documents{
				FContentGraphImportDocument(
					FPaths::GetCleanFilename(Path),
					MoveTemp(Document),
					Hash),
			};
			FString SemanticFingerprint;
			if (!TryBuildSemanticFingerprint(
					Documents,
					FPaths::GetCleanFilename(Path),
					Resources,
					SemanticFingerprint,
					OutError, FPaths::GetPath(Path)))
			{
				return false;
			}
			OutSource = MakeShared<FResolvedContentGraphImportSource>(
				EWorkspaceSourceKind::SingleFile,
				Path,
				ExpectedTargetIdentity,
				FString(),
				FPaths::GetCleanFilename(Path),
				Hash,
				MoveTemp(SemanticFingerprint),
				MoveTemp(Documents));
			return true;
		}
	}

	FContentGraphImportDocument::FContentGraphImportDocument(
		FString InRelativePath,
		FString InDocument,
		FString InContentHash)
		: RelativePath(MoveTemp(InRelativePath))
		, Document(MoveTemp(InDocument))
		, ContentHash(MoveTemp(InContentHash))
	{
	}

	const FString& FContentGraphImportDocument::GetRelativePath() const
	{
		return RelativePath;
	}

	const FString& FContentGraphImportDocument::GetDocument() const
	{
		return Document;
	}

	const FString& FContentGraphImportDocument::GetContentHash() const
	{
		return ContentHash;
	}

	FResolvedContentGraphImportSource::FResolvedContentGraphImportSource(
		const EWorkspaceSourceKind InKind,
		FString InSourceUnit,
		FString InTargetIdentity,
		FString InBaseRevision,
		FString InEntryFile,
		FString InSourceFingerprint,
		FString InSemanticFingerprint,
		TArray<FContentGraphImportDocument> InDocuments,
		const bool bInSemanticWorkspaceDirectory)
		: Kind(InKind)
		, SourceUnit(MoveTemp(InSourceUnit))
		, TargetIdentity(MoveTemp(InTargetIdentity))
		, BaseRevision(MoveTemp(InBaseRevision))
		, EntryFile(MoveTemp(InEntryFile))
		, SourceFingerprint(MoveTemp(InSourceFingerprint))
		, SemanticFingerprint(MoveTemp(InSemanticFingerprint))
		, Documents(MoveTemp(InDocuments))
		, bSemanticWorkspaceDirectory(bInSemanticWorkspaceDirectory)
	{
	}

	EWorkspaceSourceKind FResolvedContentGraphImportSource::GetKind() const
	{
		return Kind;
	}

	const FString& FResolvedContentGraphImportSource::GetSourceUnit() const
	{
		return SourceUnit;
	}

	const FString& FResolvedContentGraphImportSource::GetTargetIdentity() const
	{
		return TargetIdentity;
	}

	const FString& FResolvedContentGraphImportSource::GetBaseRevision() const
	{
		return BaseRevision;
	}

	const FString& FResolvedContentGraphImportSource::GetEntryFile() const
	{
		return EntryFile;
	}

	const FString& FResolvedContentGraphImportSource::GetSourceFingerprint() const
	{
		return SourceFingerprint;
	}

	const FString& FResolvedContentGraphImportSource::GetSemanticFingerprint() const
	{
		return SemanticFingerprint;
	}

	const TArray<FContentGraphImportDocument>&
	FResolvedContentGraphImportSource::GetDocuments() const
	{
		return Documents;
	}

	const FContentGraphImportDocument*
	FResolvedContentGraphImportSource::GetEntryDocument() const
	{
		return Documents.FindByPredicate(
			[this](const FContentGraphImportDocument& Document)
			{
				return Document.GetRelativePath().Equals(
					EntryFile,
					ESearchCase::IgnoreCase);
			});
	}

	bool FResolvedContentGraphImportSource::IsSemanticWorkspaceDirectory() const
	{
		return bSemanticWorkspaceDirectory;
	}

	TSharedRef<FJsonObject>
	FResolvedContentGraphImportSource::MakeNormalizedSourceObject() const
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (bSemanticWorkspaceDirectory)
		{
			Result->SetStringField(TEXT("kind"), TEXT("semantic_workspace"));
			Result->SetStringField(TEXT("workspace_root"), SourceUnit);
			Result->SetStringField(TEXT("target_identity"), TargetIdentity);
			Result->SetStringField(TEXT("base_revision"), BaseRevision);
			Result->SetStringField(
				TEXT("workspace_fingerprint"), SourceFingerprint);
			Result->SetStringField(
				TEXT("semantic_fingerprint"), SemanticFingerprint);
			return Result;
		}
		Result->SetStringField(TEXT("kind"), TEXT("resolved"));
		Result->SetStringField(
			TEXT("carrier"),
			Kind == EWorkspaceSourceKind::Directory
				? TEXT("directory")
				: Kind == EWorkspaceSourceKind::SingleFile
					? TEXT("single_file")
					: TEXT("inline"));
		Result->SetStringField(TEXT("source_unit"), SourceUnit);
		Result->SetStringField(TEXT("target_identity"), TargetIdentity);
		Result->SetStringField(TEXT("base_revision"), BaseRevision);
		Result->SetStringField(TEXT("entry_file"), EntryFile);
		Result->SetStringField(TEXT("source_fingerprint"), SourceFingerprint);
		Result->SetStringField(TEXT("semantic_fingerprint"), SemanticFingerprint);
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FContentGraphImportDocument& Document : Documents)
		{
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("path"), Document.GetRelativePath());
			Value->SetStringField(TEXT("document"), Document.GetDocument());
			Value->SetStringField(TEXT("content_hash"), Document.GetContentHash());
			Values.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("documents"), MoveTemp(Values));
		return Result;
	}

	const FString& GetContentGraphImportSourceSchemaJson()
	{
		static const FString Schema = TEXT(
			"{\"oneOf\":["
			"{\"additionalProperties\":false,\"properties\":{"
			"\"document\":{\"minLength\":1,\"type\":\"string\"},"
			"\"kind\":{\"const\":\"inline\"}},"
			"\"required\":[\"kind\",\"document\"],\"type\":\"object\"},"
			"{\"additionalProperties\":false,\"properties\":{"
			"\"kind\":{\"const\":\"single_file\"},"
			"\"path\":{\"minLength\":1,\"type\":\"string\"}},"
			"\"required\":[\"kind\",\"path\"],\"type\":\"object\"},"
			"{\"additionalProperties\":false,\"properties\":{"
			"\"kind\":{\"const\":\"directory\"},"
			"\"path\":{\"minLength\":1,\"type\":\"string\"}},"
			"\"required\":[\"kind\",\"path\"],\"type\":\"object\"}"
			"]}");
		return Schema;
	}

	bool TryLoadContentGraphImportSource(
		const TSharedPtr<FJsonObject>& SourceObject,
		const FString& ExpectedTargetIdentity,
		FUeAgentResourceTracker& Resources,
		FResolvedContentGraphImportSourcePtr& OutSource,
		FWorkspaceError& OutError)
	{
		OutSource.Reset();
		OutError.Reset();
		if (!SourceObject.IsValid())
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("content_graph_source_invalid"),
				TEXT("The import source object is unavailable to the source loader."), TEXT("workspace_load"));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = TEXT("/source");
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
			Diagnostic.ActualValuePresent = false;
			OutError.SetFailure(MoveTemp(Diagnostic), ExpectedTargetIdentity);
			return false;
		}
		if (ExpectedTargetIdentity.IsEmpty())
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("content_graph_source_invalid"), TEXT("The import target must not be empty."), TEXT("workspace_load"));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = TEXT("/target");
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty target identity"));
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(ExpectedTargetIdentity);
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}
		FString Kind;
		if (!SourceObject->TryGetStringField(TEXT("kind"), Kind))
		{
			return FailSourceField(OutError, ExpectedTargetIdentity, SourceObject, TEXT("kind"),
				TEXT("The import source requires a string kind field."), TEXT("inline, single_file, or directory"));
		}
		Kind.TrimStartAndEndInline();
		if (Kind == TEXT("inline"))
		{
			FString Document;
			if (SourceObject->Values.Num() != 2)
				return FailSourceShape(OutError, ExpectedTargetIdentity, *SourceObject, TEXT("document"));
			if (!SourceObject->TryGetStringField(TEXT("document"), Document)
				|| Document.IsEmpty())
			{
				return FailSourceField(OutError, ExpectedTargetIdentity, SourceObject, TEXT("document"),
					TEXT("An inline source requires non-empty document text."), TEXT("non-empty string"));
			}
			FTCHARToUTF8 Utf8(*Document);
			if (!Charge(
					Resources,
					{
						{EUeAgentResourceDimension::InputBytes, Utf8.Length()},
						{EUeAgentResourceDimension::StringBytes, Utf8.Length()},
						{EUeAgentResourceDimension::WorkUnits, 1},
						{EUeAgentResourceDimension::EstimatedMemoryBytes, Utf8.Length() * 2},
					},
					TEXT("inline:content-graph-import"),
					OutError))
			{
				return false;
			}
			const FString Hash = Sha256Text(Document);
			TArray<FContentGraphImportDocument> Documents{
				FContentGraphImportDocument(
					TEXT("inline.hjson"),
					MoveTemp(Document),
					Hash),
			};
			FString SemanticFingerprint;
			if (!TryBuildSemanticFingerprint(
					Documents,
					TEXT("inline.hjson"),
					Resources,
					SemanticFingerprint,
					OutError))
			{
				return false;
			}
			OutSource = MakeShared<FResolvedContentGraphImportSource>(
				EWorkspaceSourceKind::Inline,
				TEXT("inline:content-graph-import"),
				ExpectedTargetIdentity,
				FString(),
				TEXT("inline.hjson"),
				Hash,
				MoveTemp(SemanticFingerprint),
				MoveTemp(Documents));
			return true;
		}
		if (Kind != TEXT("single_file") && Kind != TEXT("directory"))
			return FailSourceField(OutError, ExpectedTargetIdentity, SourceObject, TEXT("kind"),
				TEXT("The source kind must be inline, single_file, or directory."), TEXT("inline, single_file, or directory"),
				TEXT("content_graph_source_kind_unsupported"));
		FString Path;
		if (SourceObject->Values.Num() != 2)
			return FailSourceShape(OutError, ExpectedTargetIdentity, *SourceObject, TEXT("path"));
		if (!SourceObject->TryGetStringField(TEXT("path"), Path)
			|| Path.TrimStartAndEnd().IsEmpty())
		{
			return FailSourceField(OutError, ExpectedTargetIdentity, SourceObject, TEXT("path"),
				TEXT("A file or directory source requires a non-empty path."), TEXT("non-empty path string"), TEXT("content_graph_source_invalid"), false);
		}
		return Kind == TEXT("single_file")
			? TryLoadSingleFileSource(
				Path,
				ExpectedTargetIdentity,
				Resources,
				OutSource,
				OutError)
			: TryLoadDirectorySource(
					Path,
					ExpectedTargetIdentity,
					Resources,
					OutSource,
					OutError);
	}

	bool TryRecoverContentGraphImportMachineContext(
		const TSharedPtr<FJsonObject>& SourceObject,
		const FString& ExpectedTargetIdentity,
		FUeAgentResourceTracker& Resources,
		FResolvedContentGraphImportSourcePtr& OutResolvedSource,
		FString& OutExpectedRevision,
		FString& OutAuthorizationId,
		FWorkspaceError& OutError)
	{
		OutResolvedSource.Reset();
		OutExpectedRevision.Reset();
		OutAuthorizationId.Reset();
		OutError.Reset();

		if (!TryLoadContentGraphImportSource(
			SourceObject,
			ExpectedTargetIdentity,
			Resources,
			OutResolvedSource,
			OutError)
			|| !OutResolvedSource.IsValid())
		{
			return false;
		}
		FString WorkspaceFingerprint;
		if (OutResolvedSource->GetKind() != EWorkspaceSourceKind::Directory)
		{
			OutExpectedRevision = TEXT("missing");
			WorkspaceFingerprint =
				OutResolvedSource->GetSourceFingerprint()
				+ TEXT("|")
				+ OutResolvedSource->GetSemanticFingerprint();
		}
		else if (OutResolvedSource->IsSemanticWorkspaceDirectory())
		{
			const FString WorkspaceRoot = OutResolvedSource->GetSourceUnit();
			FResolvedSemanticWorkspacePtr SemanticWorkspace;
			if (!TryLoadHjsonImportSource(
					FWorkspaceSource::Directory(
						WorkspaceRoot),
					ESemanticWorkspaceMode::State,
					Resources,
					SemanticWorkspace,
					OutError)
				|| !SemanticWorkspace.IsValid())
			{
				return false;
			}
			OutExpectedRevision =
				SemanticWorkspace->GetModel()->GetBaseRevision();
			WorkspaceFingerprint =
				SemanticWorkspace->GetWorkspaceFingerprint();
			OutResolvedSource = MakeShared<FResolvedContentGraphImportSource>(
				EWorkspaceSourceKind::Directory,
				WorkspaceRoot,
				ExpectedTargetIdentity,
				OutExpectedRevision,
				FString(),
				WorkspaceFingerprint,
				SemanticWorkspace->GetModel()->GetSemanticFingerprint(),
				TArray<FContentGraphImportDocument>(),
				true);
		}
		else
		{
			OutExpectedRevision = OutResolvedSource->GetBaseRevision();
			WorkspaceFingerprint =
				OutResolvedSource->GetSourceFingerprint()
				+ TEXT("|")
				+ OutResolvedSource->GetSemanticFingerprint();
		}

		if (OutExpectedRevision.IsEmpty()
			|| WorkspaceFingerprint.IsEmpty())
		{
			OutExpectedRevision.Reset();
			return Fail(
				OutError,
				TEXT("content_graph_workspace_machine_context_unavailable"), TEXT("The workspace is missing the target revision or source fingerprint required to prepare the import."),
				OutResolvedSource->GetSourceUnit());
		}
		OutAuthorizationId = Sha256Text(
			TEXT("content_graph.import_workspace|")
			+ ExpectedTargetIdentity
			+ TEXT("|")
			+ OutExpectedRevision
			+ TEXT("|")
			+ WorkspaceFingerprint);
		return true;
	}

	bool TryLoadNormalizedContentGraphImportSource(
		const TSharedPtr<FJsonObject>& SourceObject,
		const FString& ExpectedTargetIdentity,
		const FString& ExpectedRevision,
		FUeAgentResourceTracker& Resources,
		FResolvedContentGraphImportSourcePtr& OutSource,
		FWorkspaceError& OutError)
	{
		OutSource.Reset();
		OutError.Reset();
		if (!SourceObject.IsValid())
			return Fail(OutError, TEXT("content_graph_normalized_source_invalid"), TEXT("The prepared import source object is missing."), ExpectedTargetIdentity);
		if (ExpectedTargetIdentity.IsEmpty())
			return Fail(OutError, TEXT("content_graph_normalized_source_invalid"), TEXT("The prepared import has no target identity."), FString());
		if (ExpectedRevision.IsEmpty())
			return Fail(OutError, TEXT("content_graph_normalized_source_invalid"), TEXT("The prepared import has no expected target revision."), ExpectedTargetIdentity);

		FString Kind;
		FString Carrier;
		FString SourceUnit;
		FString TargetIdentity;
		FString BaseRevision;
		FString EntryFile;
		FString SourceFingerprint;
		FString SemanticFingerprint;
		const TArray<TSharedPtr<FJsonValue>>* DocumentValues = nullptr;
		if (!ReadPreparedString(SourceObject, TEXT("kind"), Kind, true, OutError, ExpectedTargetIdentity)) return false;
		if (Kind == TEXT("semantic_workspace"))
		{
			FString WorkspaceRoot;
			FString WorkspaceFingerprint;
			FString ExpectedSemanticFingerprint;
			if (!ReadPreparedString(SourceObject, TEXT("workspace_root"), WorkspaceRoot, false, OutError, FString())
				|| !ReadPreparedString(SourceObject, TEXT("target_identity"), TargetIdentity, true, OutError, WorkspaceRoot)
				|| !ReadPreparedString(SourceObject, TEXT("base_revision"), BaseRevision, false, OutError, WorkspaceRoot)
				|| !ReadPreparedString(SourceObject, TEXT("workspace_fingerprint"), WorkspaceFingerprint, false, OutError, WorkspaceRoot)
				|| !ReadPreparedString(SourceObject, TEXT("semantic_fingerprint"), ExpectedSemanticFingerprint, false, OutError, WorkspaceRoot)) return false;
			if (SourceObject->Values.Num() != 6)
				return Fail(OutError, TEXT("content_graph_normalized_source_invalid"),
					TEXT("The prepared workspace source contains fields beyond kind, workspace_root, target_identity, base_revision, workspace_fingerprint and semantic_fingerprint."),
					WorkspaceRoot, TEXT("/"), MakeShared<FJsonValueNumber>(SourceObject->Values.Num()), MakeShared<FJsonValueNumber>(6));
			if (TargetIdentity != ExpectedTargetIdentity)
			{
				return FailEvidence(
					OutError,
					TEXT("content_graph_normalized_target_identity_mismatch"), TEXT("The prepared workspace source belongs to a different target."),
					WorkspaceRoot, ExpectedTargetIdentity, TargetIdentity);
			}
			if (BaseRevision != ExpectedRevision)
			{
				return FailEvidence(
					OutError,
					TEXT("content_graph_normalized_base_revision_mismatch"), TEXT("The prepared workspace target revision differs from the authorized import revision."),
					WorkspaceRoot, ExpectedRevision, BaseRevision);
			}
			const TSharedRef<FJsonObject> DirectorySource =
				MakeShared<FJsonObject>();
			DirectorySource->SetStringField(TEXT("kind"), TEXT("directory"));
			DirectorySource->SetStringField(TEXT("path"), WorkspaceRoot);
			FResolvedContentGraphImportSourcePtr ReloadedSource;
			if (!TryLoadContentGraphImportSource(
					DirectorySource,
					ExpectedTargetIdentity,
					Resources,
					ReloadedSource,
					OutError)
				|| !ReloadedSource.IsValid()
				|| !ReloadedSource->IsSemanticWorkspaceDirectory())
			{
				if (OutError.GetCode().IsEmpty())
				{
					OutError.SetSourceFailure(
						TEXT("content_graph_normalized_source_invalid"),
						TEXT("The reloaded source is not a semantic workspace directory."),
						WorkspaceRoot);
				}
				return false;
			}
			FResolvedSemanticWorkspacePtr SemanticWorkspace;
			if (!TryLoadHjsonImportSource(
					FWorkspaceSource::Directory(WorkspaceRoot),
					ESemanticWorkspaceMode::State,
					Resources,
					SemanticWorkspace,
					OutError)
				|| !SemanticWorkspace.IsValid())
			{
				return false;
			}
			if (SemanticWorkspace->GetModel()->GetBaseRevision() != BaseRevision)
			{
				return FailEvidence(
					OutError,
					TEXT("content_graph_normalized_base_revision_mismatch"), TEXT("The workspace target revision changed after import preparation."),
					WorkspaceRoot, BaseRevision, SemanticWorkspace->GetModel()->GetBaseRevision());
			}
			if (SemanticWorkspace->GetWorkspaceFingerprint() != WorkspaceFingerprint)
			{
				return FailEvidence(
					OutError,
					TEXT("content_graph_normalized_source_fingerprint_mismatch"), TEXT("The workspace files changed after import preparation."),
					WorkspaceRoot, WorkspaceFingerprint, SemanticWorkspace->GetWorkspaceFingerprint());
			}
			if (SemanticWorkspace->GetModel()->GetSemanticFingerprint() != ExpectedSemanticFingerprint)
				return FailEvidence(OutError, TEXT("content_graph_normalized_source_fingerprint_mismatch"),
					TEXT("The workspace semantic content changed after import preparation."), WorkspaceRoot,
					ExpectedSemanticFingerprint, SemanticWorkspace->GetModel()->GetSemanticFingerprint());
			OutSource = MakeShared<FResolvedContentGraphImportSource>(
				EWorkspaceSourceKind::Directory,
				WorkspaceRoot,
				ExpectedTargetIdentity,
				BaseRevision,
				FString(),
				WorkspaceFingerprint,
				ExpectedSemanticFingerprint,
				TArray<FContentGraphImportDocument>(),
				true);
			return true;
		}
		if (Kind != TEXT("resolved"))
			return FailPreparedField(OutError, SourceObject, TEXT("kind"), TEXT("must be semantic_workspace or resolved"), FString());
		if (!ReadPreparedString(SourceObject, TEXT("source_unit"), SourceUnit, false, OutError, FString())
			|| !ReadPreparedString(SourceObject, TEXT("carrier"), Carrier, true, OutError, SourceUnit)
			|| !ReadPreparedString(SourceObject, TEXT("target_identity"), TargetIdentity, true, OutError, SourceUnit)
			|| !ReadPreparedString(SourceObject, TEXT("base_revision"), BaseRevision, true, OutError, SourceUnit)
			|| !ReadPreparedString(SourceObject, TEXT("entry_file"), EntryFile, false, OutError, SourceUnit)
			|| !ReadPreparedString(SourceObject, TEXT("source_fingerprint"), SourceFingerprint, true, OutError, SourceUnit)
			|| !ReadPreparedString(SourceObject, TEXT("semantic_fingerprint"), SemanticFingerprint, true, OutError, SourceUnit)) return false;
		if (!IsSha256(SourceFingerprint))
			return FailPreparedField(OutError, SourceObject, TEXT("source_fingerprint"), TEXT("must be a SHA-256 digest"), SourceUnit);
		if (!IsSha256(SemanticFingerprint))
			return FailPreparedField(OutError, SourceObject, TEXT("semantic_fingerprint"), TEXT("must be a SHA-256 digest"), SourceUnit);
		if (!SourceObject->TryGetArrayField(TEXT("documents"), DocumentValues) || !DocumentValues || DocumentValues->IsEmpty())
			return FailPreparedField(OutError, SourceObject, TEXT("documents"), TEXT("must be a non-empty array"), SourceUnit);
		if (TargetIdentity != ExpectedTargetIdentity)
		{
			return FailEvidence(OutError, TEXT("content_graph_normalized_target_identity_mismatch"), TEXT("The resolved import source belongs to a different target."), SourceUnit, ExpectedTargetIdentity, TargetIdentity);
		}

		EWorkspaceSourceKind SourceKind = EWorkspaceSourceKind::Inline;
		if (Carrier == TEXT("directory"))
		{
			SourceKind = EWorkspaceSourceKind::Directory;
			if (BaseRevision != ExpectedRevision)
			{
				return FailEvidence(OutError, TEXT("content_graph_normalized_base_revision_mismatch"), TEXT("The resolved workspace revision differs from the authorized import revision."), SourceUnit, ExpectedRevision, BaseRevision);
			}
		}
		else if (Carrier == TEXT("single_file"))
		{
			SourceKind = EWorkspaceSourceKind::SingleFile;
		}
		else if (Carrier != TEXT("inline"))
		{
			return FailPreparedField(OutError, SourceObject, TEXT("carrier"), TEXT("must be inline, single_file, or directory"), SourceUnit);
		}
		if (SourceKind != EWorkspaceSourceKind::Directory && !BaseRevision.IsEmpty())
		{
			return FailEvidence(OutError, TEXT("content_graph_normalized_source_invalid"), TEXT("Only a directory source may contain a workspace base revision."), SourceUnit, FString(), BaseRevision);
		}
		if (SourceKind != EWorkspaceSourceKind::Directory && DocumentValues->Num() != 1)
		{
			return Fail(OutError, TEXT("content_graph_normalized_source_invalid"), TEXT("An inline or single-file source must contain exactly one document."), SourceUnit,
				TEXT("/"), MakeShared<FJsonValueNumber>(DocumentValues->Num()), MakeShared<FJsonValueNumber>(1));
		}

		FString NormalizedEntryFile;
		if (!TryNormalizeRelativePath(EntryFile, NormalizedEntryFile, OutError, SourceUnit))
		{
			return false;
		}
		TSet<FString> DocumentIdentities;
		TArray<FContentGraphImportDocument> Documents;
		Documents.Reserve(DocumentValues->Num());
		int32 EntryMatches = 0;
		for (int32 DocumentIndex = 0; DocumentIndex < DocumentValues->Num(); ++DocumentIndex)
		{
			const auto& DocumentValue = (*DocumentValues)[DocumentIndex];
			const FString DocumentPointer = FString::Printf(TEXT("/documents/%d"), DocumentIndex);
			const TSharedPtr<FJsonObject> DocumentObject =
				DocumentValue.IsValid() && DocumentValue->Type == EJson::Object ? DocumentValue->AsObject() : nullptr;
			FString RelativePath;
			FString Document;
			FString ContentHash;
			if (!DocumentObject.IsValid())
			{
				auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("content_graph_normalized_source_invalid"),
					FString::Printf(TEXT("Prepared import document at index %d must be an object."), DocumentIndex), TEXT("workspace_load"));
				Diagnostic.Source.InternalJsonPath = DocumentPointer;
				Diagnostic.ActualEvidence = DocumentValue;
				OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit);
				return false;
			}
			if (!ReadPreparedString(DocumentObject, TEXT("path"), RelativePath, true, OutError, SourceUnit, DocumentPointer)
				|| !ReadPreparedString(DocumentObject, TEXT("document"), Document, false, OutError, SourceUnit, DocumentPointer)
				|| !ReadPreparedString(DocumentObject, TEXT("content_hash"), ContentHash, true, OutError, SourceUnit, DocumentPointer)) return false;
			if (!IsSha256(ContentHash))
				return FailPreparedField(OutError, DocumentObject, TEXT("content_hash"), TEXT("must be a SHA-256 digest"), SourceUnit, DocumentPointer);
			FString NormalizedRelativePath;
			if (!TryNormalizeRelativePath(RelativePath, NormalizedRelativePath, OutError, SourceUnit))
			{
				return false;
			}
			const FString DocumentIdentity = NormalizedRelativePath.ToLower();
			if (DocumentIdentities.Contains(DocumentIdentity))
			{
				return Fail(OutError, TEXT("content_graph_normalized_document_ambiguous"), TEXT("More than one source document resolves to this path."), NormalizedRelativePath);
			}
			DocumentIdentities.Add(DocumentIdentity);
			FTCHARToUTF8 Utf8(*Document);
			if (!Charge(
				Resources,
				{
					{EUeAgentResourceDimension::InputBytes, Utf8.Length()},
					{EUeAgentResourceDimension::StringBytes, Utf8.Length()},
					{EUeAgentResourceDimension::Files, 1},
					{EUeAgentResourceDimension::Items, 1},
					{EUeAgentResourceDimension::WorkUnits, 1},
					{EUeAgentResourceDimension::EstimatedMemoryBytes, Utf8.Length() * 2},
				},
				SourceUnit + TEXT(":") + NormalizedRelativePath,
				OutError))
			{
				return false;
			}
			const FString ActualHash = Sha256Text(Document);
			if (ActualHash != ContentHash)
			{
				return FailEvidence(OutError, TEXT("content_graph_normalized_document_digest_mismatch"), TEXT("The resolved document text does not match its recorded digest."), NormalizedRelativePath, ContentHash, ActualHash);
			}
			EntryMatches += NormalizedRelativePath.Equals(
				NormalizedEntryFile, ESearchCase::IgnoreCase) ? 1 : 0;
			Documents.Emplace(
				MoveTemp(NormalizedRelativePath),
				MoveTemp(Document),
				MoveTemp(ContentHash));
		}
		if (EntryMatches != 1)
		{
			return Fail(OutError, TEXT("content_graph_normalized_entry_document_missing"), TEXT("The entry file must identify exactly one resolved document."), NormalizedEntryFile,
				TEXT("/"), MakeShared<FJsonValueNumber>(EntryMatches), MakeShared<FJsonValueNumber>(1));
		}
		if (SourceKind != EWorkspaceSourceKind::Directory
			&& SourceFingerprint != Documents[0].GetContentHash())
		{
			return FailEvidence(OutError, TEXT("content_graph_normalized_source_fingerprint_mismatch"), TEXT("The single-document source fingerprint differs from the document digest."), SourceUnit, SourceFingerprint, Documents[0].GetContentHash());
		}
		FString ActualSemanticFingerprint;
		const FString DocumentRoot = SourceKind == EWorkspaceSourceKind::Directory ? SourceUnit
			: SourceKind == EWorkspaceSourceKind::SingleFile ? FPaths::GetPath(SourceUnit) : FString();
		if (!TryBuildSemanticFingerprint(
			Documents, NormalizedEntryFile, Resources, ActualSemanticFingerprint, OutError, DocumentRoot))
		{
			return false;
		}
		if (ActualSemanticFingerprint != SemanticFingerprint)
		{
			return FailEvidence(OutError, TEXT("content_graph_normalized_semantic_fingerprint_mismatch"), TEXT("The resolved source semantic content differs from its prepared fingerprint."), SourceUnit, SemanticFingerprint, ActualSemanticFingerprint);
		}

		OutSource = MakeShared<FResolvedContentGraphImportSource>(
			SourceKind,
			MoveTemp(SourceUnit),
			MoveTemp(TargetIdentity),
			MoveTemp(BaseRevision),
			MoveTemp(NormalizedEntryFile),
			MoveTemp(SourceFingerprint),
			MoveTemp(SemanticFingerprint),
			MoveTemp(Documents));
		return true;
	}

	bool TryLoadHjsonImportSource(
		const FWorkspaceSource& Source,
		const ESemanticWorkspaceMode ExpectedMode,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError)
	{
		OutWorkspace.Reset();
		OutError.Reset();
		FSourceVersionSnapshot Before;
		if (!CaptureSourceVersion(Source, Before, OutError))
		{
			return false;
		}
		if (!TryResolveEditedSemanticWorkspace(
				Source,
				ExpectedMode,
				Resources,
				OutWorkspace,
				OutError)
			|| !OutWorkspace.IsValid())
		{
			OutWorkspace.Reset();
			return false;
		}
		FSourceVersionSnapshot After;
		FWorkspaceError VersionError;
		if (!CaptureSourceVersion(Source, After, VersionError))
		{
			OutWorkspace.Reset();
			OutError = MoveTemp(VersionError);
			return false;
		}
		if (!(Before == After))
		{
			OutWorkspace.Reset();
			OutError.SetSourceFailure(
				TEXT("workspace_source_version_changed"),
				TEXT("Workspace source files changed while the import was being resolved."), Source.GetPath());
			return false;
		}
		return true;
	}
}
