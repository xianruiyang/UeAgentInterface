#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"

#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Containers/StringConv.h"
#include "Dom/JsonValue.h"
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
			FString SourceUnit)
		{
			OutError.Set(MoveTemp(Code), MoveTemp(SourceUnit));
			return false;
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
			FUeAgentResourceBudgetExceeded BudgetError;
			if (Resources.TryChargeMany(Values, Path, BudgetError))
			{
				return true;
			}
			OutError.Set(
				BudgetError.Code,
				BudgetError.Path,
				TEXT("/"),
				BudgetError.Observed,
				BudgetError.bStoppedEarly);
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

		bool TryBuildSemanticFingerprint(
			const TArray<FContentGraphImportDocument>& Documents,
			const FString& EntryFile,
			FUeAgentResourceTracker& Resources,
			FString& OutFingerprint,
			FWorkspaceError& OutError)
		{
			OutFingerprint.Reset();
			if (Documents.IsEmpty() || EntryFile.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("content_graph_semantic_source_invalid"),
					EntryFile);
			}
			TArray<TPair<FString, FString>> CanonicalDocuments;
			CanonicalDocuments.Reserve(Documents.Num());
			int32 EntryMatches = 0;
			for (const FContentGraphImportDocument& SourceDocument : Documents)
			{
				FHjsonDocumentPtr Parsed;
				if (!TryParseHjsonDocument(
						SourceDocument.GetDocument(),
						SourceDocument.GetRelativePath(),
						Resources,
						Parsed,
						OutError)
					|| !Parsed.IsValid())
				{
					return false;
				}
				TArray<uint8> CanonicalUtf8;
				if (!TryWriteHjsonDocument(
						Parsed.ToSharedRef(),
						Resources,
						CanonicalUtf8,
						OutError))
				{
					return false;
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
			if (EntryMatches != 1)
			{
				return Fail(
					OutError,
					TEXT("content_graph_semantic_entry_invalid"),
					EntryFile);
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
					TEXT("workspace_reparse_point_rejected"),
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
					TEXT("workspace_path_escape"),
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
						TEXT("workspace_reparse_point_rejected"),
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
					TEXT("workspace_source_read_failed"),
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
					TEXT("workspace_source_directory_missing"),
					Root);
			}
			if (IsReparsePoint(Root))
			{
				return Fail(
					OutError,
					TEXT("workspace_reparse_point_rejected"),
					Root);
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
						TEXT("workspace_path_escape"),
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
					TEXT("workspace_source_file_missing"),
					File);
			}
			if (IsReparsePoint(File))
			{
				return Fail(
					OutError,
					TEXT("workspace_reparse_point_rejected"),
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
					TEXT("workspace_path_invalid"),
					SourceUnit);
			}
			FString Collapsed = Value;
			if (!FPaths::CollapseRelativeDirectories(Collapsed)
				|| Collapsed.IsEmpty()
				|| Collapsed.StartsWith(TEXT("../")))
			{
				return Fail(
					OutError,
					TEXT("workspace_path_invalid"),
					SourceUnit);
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
					TEXT("workspace_source_read_failed"),
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

		bool RequiredString(
			const FHjsonNode& Object,
			const TCHAR* Field,
			FString& OutValue,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Value = Object.Get(Field);
			if (!Value || Value->GetKind() != EHjsonNodeKind::String)
			{
				return Fail(
					OutError,
					TEXT("content_graph_workspace_manifest_invalid"),
					Object.GetProvenance().GetSourceUnit());
			}
			OutValue = Value->GetText();
			OutValue.TrimStartAndEndInline();
			return !OutValue.IsEmpty() || Fail(
				OutError,
				TEXT("content_graph_workspace_manifest_invalid"),
				Object.GetProvenance().GetSourceUnit());
		}

		bool RequiredInteger(
			const FHjsonNode& Object,
			const TCHAR* Field,
			int64& OutValue,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Value = Object.Get(Field);
			return Value
				&& Value->GetKind() == EHjsonNodeKind::Integer
				&& LexTryParseString(OutValue, *Value->GetLexeme())
				|| Fail(
					OutError,
					TEXT("content_graph_workspace_manifest_invalid"),
					Object.GetProvenance().GetSourceUnit());
		}

		bool TryParseManifest(
			const FString& ManifestText,
			const FString& ManifestPath,
			const FString& ExpectedTargetIdentity,
			FUeAgentResourceTracker& Resources,
			FString& OutEntryFile,
			FString& OutBaseRevision,
			TArray<TPair<FString, FString>>& OutFiles,
			FWorkspaceError& OutError)
		{
			OutEntryFile.Reset();
			OutBaseRevision.Reset();
			OutFiles.Reset();
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
					OutError.Set(
						TEXT("content_graph_workspace_manifest_invalid"),
						ManifestPath);
				}
				return false;
			}
			const FHjsonNode& Root = *Manifest->GetRoot();
			FString Schema;
			FString TargetIdentity;
			int64 FormatVersion = 0;
			if (!RequiredString(Root, TEXT("workspace_schema"), Schema, OutError)
				|| !RequiredInteger(Root, TEXT("format_version"), FormatVersion, OutError)
				|| !RequiredString(Root, TEXT("target_identity"), TargetIdentity, OutError)
				|| !RequiredString(Root, TEXT("base_revision"), OutBaseRevision, OutError)
				|| !RequiredString(Root, TEXT("entry_file"), OutEntryFile, OutError))
			{
				return false;
			}
			if (Schema != ContentGraphWorkspaceSchema || FormatVersion != 1)
			{
				return Fail(
					OutError,
					TEXT("content_graph_workspace_schema_unsupported"),
					ManifestPath);
			}
			if (TargetIdentity != ExpectedTargetIdentity)
			{
				return Fail(
					OutError,
					TEXT("content_graph_workspace_target_mismatch"),
					TargetIdentity);
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
			if (!Files || Files->GetKind() != EHjsonNodeKind::Array
				|| Files->GetItems().IsEmpty()
				|| !References || References->GetKind() != EHjsonNodeKind::Array)
			{
				return Fail(
					OutError,
					TEXT("content_graph_workspace_manifest_invalid"),
					ManifestPath);
			}
			TSet<FString> Identities;
			for (const FHjsonNodeRef& FileValue : Files->GetItems())
			{
				if (FileValue->GetKind() != EHjsonNodeKind::Object)
				{
					return Fail(
						OutError,
						TEXT("content_graph_workspace_manifest_invalid"),
						ManifestPath);
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
				if (Identities.Contains(Identity)
					|| !ContentHash.StartsWith(TEXT("sha256:"))
					|| ContentHash.Len() != 71
					|| !RelativePath.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase)
					|| RelativePath.Equals(
						ContentGraphWorkspaceManifest,
						ESearchCase::IgnoreCase))
				{
					return Fail(
						OutError,
						TEXT("content_graph_workspace_manifest_invalid"),
						RelativePath);
				}
				Identities.Add(Identity);
				OutFiles.Emplace(MoveTemp(RelativePath), ContentHash.ToLower());
			}
			if (!Identities.Contains(OutEntryFile.ToLower()))
			{
				return Fail(
					OutError,
					TEXT("content_graph_workspace_entry_missing"),
					OutEntryFile);
			}
			for (const FHjsonNodeRef& Reference : References->GetItems())
			{
				if (Reference->GetKind() != EHjsonNodeKind::String)
				{
					return Fail(
						OutError,
						TEXT("content_graph_workspace_manifest_invalid"),
						ManifestPath);
				}
				FString RelativePath;
				if (!TryNormalizeRelativePath(
						Reference->GetText(),
						RelativePath,
						OutError,
						ManifestPath)
					|| !Identities.Contains(RelativePath.ToLower()))
				{
					return Fail(
						OutError,
						TEXT("content_graph_workspace_reference_unresolved"),
						RelativePath);
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
					TEXT("workspace_source_directory_missing"),
					Root);
			}
			if (FPaths::IsRelative(SourcePath))
			{
				return Fail(OutError, TEXT("workspace_path_invalid"), SourcePath);
			}
			const FString ManifestPath =
				FPaths::Combine(Root, ContentGraphWorkspaceManifest);
			if (!IFileManager::Get().FileExists(*ManifestPath))
			{
				return Fail(
					OutError,
					TEXT("content_graph_workspace_manifest_missing"),
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
			if (!TryParseManifest(
					ManifestText,
					ManifestPath,
					ExpectedTargetIdentity,
					Resources,
					EntryFile,
					BaseRevision,
					DeclaredFiles,
					OutError))
			{
				return false;
			}
			TSet<FString> DeclaredIdentities;
			TArray<FContentGraphImportDocument> Documents;
			FString FingerprintMaterial = Sha256(ManifestBytes);
			for (const TPair<FString, FString>& Declared : DeclaredFiles)
			{
				const FString FullPath = NormalizedFullPath(
					FPaths::Combine(Root, Declared.Key),
					false);
				if (!IsWithinRoot(Root, FullPath)
					|| !RejectReparseAncestors(Root, FullPath, OutError))
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
					return Fail(
						OutError,
						TEXT("content_graph_workspace_digest_mismatch"),
						Declared.Key);
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
					return Fail(OutError, TEXT("workspace_path_escape"), Actual);
				}
				FPaths::NormalizeFilename(Relative);
				if (!Relative.Equals(
						ContentGraphWorkspaceManifest,
						ESearchCase::IgnoreCase)
					&& !DeclaredIdentities.Contains(Relative.ToLower()))
				{
					return Fail(
						OutError,
						TEXT("content_graph_workspace_file_undeclared"),
						Relative);
				}
			}
			FSourceVersionSnapshot After;
			if (!CaptureDirectory(Root, After, OutError) || !(Before == After))
			{
				return Fail(
					OutError,
					TEXT("workspace_source_version_changed"),
					Root);
			}
			FString SemanticFingerprint;
			if (!TryBuildSemanticFingerprint(
					Documents,
					EntryFile,
					Resources,
					SemanticFingerprint,
					OutError))
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
				return Fail(OutError, TEXT("workspace_path_invalid"), SourcePath);
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
					OutError)
				|| !(Before == After))
			{
				return Fail(
					OutError,
					TEXT("workspace_source_version_changed"),
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
					OutError))
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
		TArray<FContentGraphImportDocument> InDocuments)
		: Kind(InKind)
		, SourceUnit(MoveTemp(InSourceUnit))
		, TargetIdentity(MoveTemp(InTargetIdentity))
		, BaseRevision(MoveTemp(InBaseRevision))
		, EntryFile(MoveTemp(InEntryFile))
		, SourceFingerprint(MoveTemp(InSourceFingerprint))
		, SemanticFingerprint(MoveTemp(InSemanticFingerprint))
		, Documents(MoveTemp(InDocuments))
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

	TSharedRef<FJsonObject>
	FResolvedContentGraphImportSource::MakeNormalizedSourceObject() const
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
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
		if (!SourceObject.IsValid() || ExpectedTargetIdentity.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("content_graph_source_invalid"),
				ExpectedTargetIdentity);
		}
		FString Kind;
		if (!SourceObject->TryGetStringField(TEXT("kind"), Kind))
		{
			return Fail(
				OutError,
				TEXT("content_graph_source_invalid"),
				ExpectedTargetIdentity);
		}
		Kind.TrimStartAndEndInline();
		if (Kind == TEXT("inline"))
		{
			FString Document;
			if (SourceObject->Values.Num() != 2
				|| !SourceObject->TryGetStringField(TEXT("document"), Document)
				|| Document.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("content_graph_source_invalid"),
					ExpectedTargetIdentity);
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
		FString Path;
		if (SourceObject->Values.Num() != 2
			|| !SourceObject->TryGetStringField(TEXT("path"), Path)
			|| Path.TrimStartAndEnd().IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("content_graph_source_invalid"),
				ExpectedTargetIdentity);
		}
		return Kind == TEXT("single_file")
			? TryLoadSingleFileSource(
				Path,
				ExpectedTargetIdentity,
				Resources,
				OutSource,
				OutError)
			: Kind == TEXT("directory")
				? TryLoadDirectorySource(
					Path,
					ExpectedTargetIdentity,
					Resources,
					OutSource,
					OutError)
				: Fail(
					OutError,
					TEXT("content_graph_source_kind_unsupported"),
					Kind);
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
		if (!TryResolveSemanticWorkspace(
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
		if (!CaptureSourceVersion(Source, After, VersionError)
			|| !(Before == After))
		{
			OutWorkspace.Reset();
			OutError.Set(
				TEXT("workspace_source_version_changed"),
				VersionError.GetSourceUnit().IsEmpty()
					? Source.GetPath()
					: VersionError.GetSourceUnit());
			return false;
		}
		return true;
	}
}
