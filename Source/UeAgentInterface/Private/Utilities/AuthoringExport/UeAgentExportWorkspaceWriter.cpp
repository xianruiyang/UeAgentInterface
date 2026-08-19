// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"
#include "Utilities/AuthoringExport/UeAgentExportPublisher.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"

namespace
{
	using namespace UeAgentExportWorkspaceWriter;

	void SetFailure(
		FWorkspaceResult& OutResult,
		const FString& ErrorCode,
		const FString& ErrorDetail = FString(),
		const FString& FileId = FString(),
		const FString& FieldId = FString())
	{
		OutResult.Status = TEXT("failed");
		OutResult.ErrorCode = ErrorCode;
		OutResult.ErrorDetail = ErrorDetail.IsEmpty() ? ErrorCode : ErrorDetail;
		OutResult.FileId = FileId;
		OutResult.FieldId = FieldId;
	}

	FTextMetrics MeasureText(const FString& Text)
	{
		FTextMetrics Result;
		Result.CharacterCount = Text.Len();
		const FTCHARToUTF8 Utf8(*Text);
		Result.Utf8Bytes = Utf8.Length();
		for (const TCHAR Character : Text)
		{
			if (Character == TEXT('\n')) ++Result.LineCount;
		}
		if (!Text.IsEmpty() && Result.LineCount == 0) Result.LineCount = 1;
		Result.TokenProxy = (Result.CharacterCount + 3) / 4;
		return Result;
	}

	bool MatchesRolePathPattern(const FString& RelativePath, const FString& Pattern)
	{
		if (Pattern.IsEmpty()) return true;
		TArray<FString> PathSegments;
		TArray<FString> PatternSegments;
		RelativePath.ParseIntoArray(PathSegments, TEXT("/"), true);
		Pattern.ParseIntoArray(PatternSegments, TEXT("/"), true);
		if (PathSegments.Num() != PatternSegments.Num()) return false;
		for (int32 Index = 0; Index < PathSegments.Num(); ++Index)
		{
			const FString& PatternSegment = PatternSegments[Index];
			const bool bPlaceholder = PatternSegment.Len() > 2
				&& PatternSegment.StartsWith(TEXT("{"))
				&& PatternSegment.EndsWith(TEXT("}"));
			if ((!bPlaceholder && PathSegments[Index] != PatternSegment)
				|| (bPlaceholder && PathSegments[Index].IsEmpty()))
			{
				return false;
			}
		}
		return true;
	}

	bool IsCanonicalValueProjector(const FString& ProjectorId)
	{
		return ProjectorId == TEXT("uai.export.semantic.value")
			|| ProjectorId == TEXT("uai.export.semantic.values");
	}

	bool MatchesContractPointer(
		const UeAgentExportContract::FFieldDescriptor& ContractField,
		const FString& ContractPointer,
		const UeAgentSnapshotProjector::FProjectedField& ProjectedField)
	{
		if (!ContractPointer.Contains(TEXT("*")))
		{
			return !ProjectedField.bCompoundContainerFallback
				&& ContractPointer == ProjectedField.JsonPointer;
		}
		if (!ProjectedField.bCompoundContainerFallback)
		{
			return UeAgentCompoundProjection::MatchesExpandedJsonPointer(
				ContractPointer, ProjectedField.JsonPointer);
		}

		UeAgentCompoundProjection::FJsonPointerPatternInfo PatternInfo;
		FString PatternError;
		if (!UeAgentCompoundProjection::InspectJsonPointerPattern(
			ContractPointer, PatternInfo, PatternError)
			|| ProjectedField.JsonPointer != PatternInfo.EmptyContainerPointer
			|| !ProjectedField.Value.IsValid())
		{
			return false;
		}
		if (ContractField.EmptyPolicy == TEXT("emit_null"))
		{
			return ProjectedField.Value->Type == EJson::Null;
		}
		const TSharedPtr<FJsonObject> EmptyObject = ProjectedField.Value->Type == EJson::Object
			? ProjectedField.Value->AsObject()
			: nullptr;
		return ContractField.EmptyPolicy == TEXT("emit_empty")
			&& EmptyObject.IsValid()
			&& EmptyObject->Values.IsEmpty();
	}

	bool DecodeJsonPointerSegment(const FString& Encoded, FString& OutDecoded)
	{
		OutDecoded.Reset();
		for (int32 Index = 0; Index < Encoded.Len(); ++Index)
		{
			if (Encoded[Index] != TEXT('~'))
			{
				OutDecoded.AppendChar(Encoded[Index]);
				continue;
			}
			if (++Index >= Encoded.Len()) return false;
			if (Encoded[Index] == TEXT('0')) OutDecoded.AppendChar(TEXT('~'));
			else if (Encoded[Index] == TEXT('1')) OutDecoded.AppendChar(TEXT('/'));
			else return false;
		}
		return true;
	}

	FString EscapeJsonPointerSegment(FString Segment)
	{
		Segment.ReplaceInline(TEXT("~"), TEXT("~0"), ESearchCase::CaseSensitive);
		Segment.ReplaceInline(TEXT("/"), TEXT("~1"), ESearchCase::CaseSensitive);
		return Segment;
	}

	bool AddPreferredFieldOrder(
		const FString& JsonPointer,
		FUeAgentStructuredHjsonWriteOptions& OutOptions,
		FString& OutError)
	{
		if (JsonPointer.IsEmpty()) return true;
		if (!JsonPointer.StartsWith(TEXT("/")))
		{
			OutError = TEXT("json_pointer_must_start_with_slash");
			return false;
		}
		TArray<FString> EncodedSegments;
		JsonPointer.Mid(1).ParseIntoArray(EncodedSegments, TEXT("/"), false);
		FString ParentPointer = TEXT("/");
		for (const FString& Encoded : EncodedSegments)
		{
			FString Decoded;
			if (!DecodeJsonPointerSegment(Encoded, Decoded) || Decoded.IsEmpty())
			{
				OutError = TEXT("json_pointer_segment_invalid");
				return false;
			}
			TArray<FString>& Preferred = OutOptions.PreferredKeyOrderByJsonPointer.FindOrAdd(ParentPointer);
			Preferred.AddUnique(Decoded);
			ParentPointer = ParentPointer == TEXT("/")
				? TEXT("/") + EscapeJsonPointerSegment(Decoded)
				: ParentPointer + TEXT("/") + EscapeJsonPointerSegment(Decoded);
		}
		return true;
	}

	bool IsRemovedField(
		const UeAgentExportContract::FWriterPolicy& Policy,
		const FString& FieldName)
	{
		return Policy.RemoveFields.ContainsByPredicate([&FieldName](const FString& Candidate)
		{
			return Candidate.Equals(FieldName, ESearchCase::IgnoreCase);
		});
	}

	bool ValidateFieldPolicy(
		const TSharedPtr<FJsonValue>& Value,
		const UeAgentExportContract::FWriterPolicy& Policy,
		const FString& ProfileId,
		bool bAllowsPropertyPath,
		bool bInsideCanonicalValue,
		const FString& Path,
		FString& OutErrorCode,
		FString& OutErrorDetail)
	{
		if (!Value.IsValid())
		{
			OutErrorCode = TEXT("export_writer_field_value_invalid");
			OutErrorDetail = Path;
			return false;
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (!ValidateFieldPolicy(
					Values[Index], Policy, ProfileId, bAllowsPropertyPath, bInsideCanonicalValue,
					FString::Printf(TEXT("%s/%d"), *Path, Index), OutErrorCode, OutErrorDetail))
				{
					return false;
				}
			}
			return true;
		}
		if (Value->Type != EJson::Object) return true;

		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid())
		{
			OutErrorCode = TEXT("export_writer_field_object_invalid");
			OutErrorDetail = Path;
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			const FString ChildPath = Path + TEXT("/") + Pair.Key;
			if (!bInsideCanonicalValue)
			{
				if (IsRemovedField(Policy, Pair.Key))
				{
					OutErrorCode = TEXT("export_writer_removed_field_present");
					OutErrorDetail = ChildPath;
					return false;
				}
				if (Pair.Key.Equals(TEXT("value_text"), ESearchCase::IgnoreCase)
					&& !Policy.PreservesValueText(ProfileId))
				{
					const TSharedPtr<FJsonValue>* StructuredValue = Object->Values.Find(TEXT("value"));
					const bool bHasStructuredValue = StructuredValue && StructuredValue->IsValid()
						&& (*StructuredValue)->Type != EJson::None
						&& (*StructuredValue)->Type != EJson::Null;
					if (bHasStructuredValue)
					{
						OutErrorCode = TEXT("export_writer_value_text_profile_violation");
						OutErrorDetail = ChildPath;
						return false;
					}
				}
				if (Pair.Key.Equals(TEXT("property_path"), ESearchCase::IgnoreCase) && !bAllowsPropertyPath)
				{
					OutErrorCode = TEXT("export_writer_property_path_not_editable");
					OutErrorDetail = ChildPath;
					return false;
				}
			}
			const bool bChildInsideCanonicalValue = bInsideCanonicalValue
				|| Pair.Key.Equals(TEXT("value"), ESearchCase::IgnoreCase);
			if (!ValidateFieldPolicy(
				Pair.Value, Policy, ProfileId, bAllowsPropertyPath, bChildInsideCanonicalValue,
				ChildPath, OutErrorCode, OutErrorDetail))
			{
				return false;
			}
		}
		return true;
	}

	bool ValidateProjectionHeader(
		const UeAgentExportContract::FContract& Contract,
		const UeAgentSnapshotProjector::FProjectionResult& Projection,
		FWorkspaceResult& OutResult,
		const UeAgentExportContract::FProfileDescriptor*& OutProfile)
	{
		FString ContractError;
		if (!UeAgentExportContract::ValidateContract(Contract, ContractError))
		{
			SetFailure(OutResult, TEXT("export_contract_invalid"), ContractError);
			return false;
		}
		if (!Projection.IsProjected())
		{
			SetFailure(OutResult, TEXT("export_projection_not_ready"), Projection.ErrorCode);
			return false;
		}
		if (Projection.ContractId != Contract.ContractId
			|| Projection.ContractVersion != Contract.ContractVersion)
		{
			SetFailure(OutResult, TEXT("export_projection_contract_mismatch"));
			return false;
		}
		OutProfile = Contract.FindProfile(Projection.ProfileId);
		if (!OutProfile)
		{
			SetFailure(OutResult, TEXT("export_projection_profile_mismatch"), Projection.ProfileId);
			return false;
		}
		return true;
	}
}

namespace UeAgentExportWorkspaceWriter
{
	void FTextMetrics::Add(const FTextMetrics& Other)
	{
		CharacterCount += Other.CharacterCount;
		Utf8Bytes += Other.Utf8Bytes;
		LineCount += Other.LineCount;
		TokenProxy += Other.TokenProxy;
	}

	TSharedRef<FJsonObject> FTextMetrics::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("character_count"), CharacterCount);
		Result->SetNumberField(TEXT("utf8_bytes"), Utf8Bytes);
		Result->SetNumberField(TEXT("line_count"), LineCount);
		Result->SetNumberField(TEXT("token_proxy"), TokenProxy);
		Result->SetStringField(TEXT("token_metric"), TEXT("ceil_character_count_div_4"));
		return Result;
	}

	TSharedRef<FJsonObject> FPlannedFile::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("file_id"), FileId);
		Result->SetStringField(TEXT("relative_path"), RelativePath);
		Result->SetStringField(TEXT("resolved_path"), ResolvedPath);
		Result->SetStringField(TEXT("role"), Role);
		Result->SetObjectField(TEXT("metrics"), Metrics.ToJsonObject());
		TArray<TSharedPtr<FJsonValue>> ReferenceValues;
		for (const FString& Reference : ReferencedRelativePaths)
		{
			ReferenceValues.Add(MakeShared<FJsonValueString>(Reference));
		}
		Result->SetArrayField(TEXT("references"), ReferenceValues);
		return Result;
	}

	bool FWorkspaceResult::IsPlanned() const
	{
		return Status == TEXT("planned") && ErrorCode.IsEmpty();
	}

	bool FWorkspaceResult::IsWritten() const
	{
		return Status == TEXT("written") && ErrorCode.IsEmpty()
			&& WrittenFiles.Num() == Files.Num();
	}

	TSharedRef<FJsonObject> FWorkspaceResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("uai.export_workspace_result.v1"));
		Result->SetStringField(TEXT("status"), Status);
		Result->SetStringField(TEXT("contract_id"), ContractId);
		Result->SetStringField(TEXT("contract_version"), ContractVersion);
		Result->SetStringField(TEXT("profile_id"), ProfileId);
		Result->SetStringField(TEXT("workspace_root"), WorkspaceRoot);
		if (!EntryFile.IsEmpty())
		{
			Result->SetStringField(TEXT("entry_file"), EntryFile);
		}
		Result->SetNumberField(TEXT("token_budget"), TokenBudget);
		Result->SetObjectField(TEXT("metrics"), Metrics.ToJsonObject());
		if (PublisherReceipt.IsValid())
		{
			Result->SetObjectField(TEXT("publisher"), PublisherReceipt.ToSharedRef());
		}
		if (!ErrorCode.IsEmpty())
		{
			Result->SetStringField(TEXT("error_code"), ErrorCode);
			Result->SetStringField(TEXT("error_detail"), ErrorDetail);
			if (!FileId.IsEmpty()) Result->SetStringField(TEXT("file_id"), FileId);
			if (!FieldId.IsEmpty()) Result->SetStringField(TEXT("field_id"), FieldId);
		}
		TArray<TSharedPtr<FJsonValue>> FileValues;
		for (const FPlannedFile& File : Files)
		{
			FileValues.Add(MakeShared<FJsonValueObject>(File.ToJsonObject()));
		}
		Result->SetArrayField(TEXT("files"), FileValues);
		TArray<TSharedPtr<FJsonValue>> WrittenValues;
		for (const FString& File : WrittenFiles)
		{
			WrittenValues.Add(MakeShared<FJsonValueString>(File));
		}
		Result->SetArrayField(TEXT("written_files"), WrittenValues);
		TArray<TSharedPtr<FJsonValue>> WrittenRelativeValues;
		for (const FString& File : WrittenRelativeFiles)
		{
			WrittenRelativeValues.Add(MakeShared<FJsonValueString>(File));
		}
		Result->SetArrayField(TEXT("written_relative_files"), WrittenRelativeValues);
		return Result;
	}

	bool BuildWorkspacePlan(
		const UeAgentExportContract::FContract& Contract,
		const UeAgentSnapshotProjector::FProjectionResult& Projection,
		const FString& WorkspaceRoot,
		FWorkspaceResult& OutResult)
	{
		OutResult = FWorkspaceResult();
		OutResult.ContractId = Contract.ContractId;
		OutResult.ContractVersion = Contract.ContractVersion;
		OutResult.ProfileId = Projection.ProfileId;
		OutResult.WorkspaceRoot = UeAgentAuthoringWorkspacePaths::NormalizeAbsolutePath(WorkspaceRoot);
		OutResult.Status = TEXT("failed");

		const UeAgentExportContract::FProfileDescriptor* Profile = nullptr;
		if (!ValidateProjectionHeader(Contract, Projection, OutResult, Profile)) return false;
		OutResult.TokenBudget = Profile->TokenBudget;
		if (OutResult.WorkspaceRoot.IsEmpty())
		{
			SetFailure(OutResult, TEXT("export_workspace_root_invalid"));
			return false;
		}

		TSet<FString> SeenFileIds;
		TSet<FString> SeenProjectedFieldIds;
		TSet<FString> SeenContractFieldIds;
		TArray<FString> RelativePaths;
		for (const UeAgentSnapshotProjector::FProjectedFile& ProjectedFile : Projection.Files)
		{
			const UeAgentExportContract::FFileDescriptor* ContractFile = Contract.FindFile(ProjectedFile.FileId);
			if (!ContractFile || !ContractFile->IsEnabledForProfile(Profile->ProfileId)
				|| SeenFileIds.Contains(ProjectedFile.FileId)
				|| ContractFile->Path != ProjectedFile.Path
				|| ContractFile->Role != ProjectedFile.Role
				|| ContractFile->bRequired != ProjectedFile.bRequired)
			{
				SetFailure(OutResult, TEXT("export_projected_file_contract_mismatch"), ProjectedFile.Path, ProjectedFile.FileId);
				return false;
			}
			SeenFileIds.Add(ProjectedFile.FileId);
			RelativePaths.Add(ProjectedFile.Path);

			TSet<FString> FileProjectedFieldIds;
			TSet<FString> FileContractFieldIds;
			for (const UeAgentSnapshotProjector::FProjectedField& ProjectedField : ProjectedFile.Fields)
			{
				const FString ContractFieldId = ProjectedField.ContractFieldId.IsEmpty()
					? ProjectedField.FieldId
					: ProjectedField.ContractFieldId;
				const UeAgentExportContract::FFieldDescriptor* ContractField = Contract.FindField(ContractFieldId);
				FString ContractFilePath;
				FString ContractPointer;
				if (!ContractField
					|| !UeAgentExportContract::SplitOutputPath(ContractField->OutputPath, ContractFilePath, ContractPointer)
					|| ContractFilePath != ProjectedFile.Path
					|| !MatchesContractPointer(*ContractField, ContractPointer, ProjectedField)
					|| ContractField->Role != ProjectedField.Role
					|| ContractField->bEditable != ProjectedField.bEditable
					|| !Profile->IncludesRole(ContractField->Role)
					|| FileProjectedFieldIds.Contains(ProjectedField.FieldId)
					|| SeenProjectedFieldIds.Contains(ProjectedField.FieldId))
				{
					SetFailure(OutResult, TEXT("export_projected_field_contract_mismatch"),
						ProjectedField.JsonPointer, ProjectedFile.FileId, ProjectedField.FieldId);
					return false;
				}
				FString PolicyErrorCode;
				FString PolicyErrorDetail;
				if (!ValidateFieldPolicy(
					ProjectedField.Value,
					Contract.WriterPolicy,
					Profile->ProfileId,
					ProjectedField.bEditable
						|| ProjectedField.Role == TEXT("identity")
						|| ProjectedField.Role == TEXT("reference"),
					IsCanonicalValueProjector(ContractField->Source.ProjectorId),
					ProjectedField.JsonPointer,
					PolicyErrorCode,
					PolicyErrorDetail))
				{
					SetFailure(OutResult, PolicyErrorCode, PolicyErrorDetail, ProjectedFile.FileId, ProjectedField.FieldId);
					return false;
				}
				FileProjectedFieldIds.Add(ProjectedField.FieldId);
				SeenProjectedFieldIds.Add(ProjectedField.FieldId);
				FileContractFieldIds.Add(ContractFieldId);
				SeenContractFieldIds.Add(ContractFieldId);
			}

			for (const UeAgentExportContract::FFieldDescriptor& ContractField : Contract.Fields)
			{
				FString ContractFilePath;
				FString ContractPointer;
				if (ContractField.bRequired && Profile->IncludesRole(ContractField.Role)
					&& UeAgentExportContract::SplitOutputPath(ContractField.OutputPath, ContractFilePath, ContractPointer)
					&& ContractFilePath == ProjectedFile.Path
					&& !FileContractFieldIds.Contains(ContractField.FieldId))
				{
					SetFailure(OutResult, TEXT("export_required_field_missing"), ContractField.OutputPath,
						ProjectedFile.FileId, ContractField.FieldId);
					return false;
				}
			}

			TSharedPtr<FJsonObject> RebuiltRoot;
			FString RebuildError;
			if (!UeAgentSnapshotProjector::BuildProjectedFileRoot(ProjectedFile, RebuiltRoot, RebuildError))
			{
				SetFailure(OutResult, TEXT("export_document_rebuild_failed"), RebuildError, ProjectedFile.FileId);
				return false;
			}
			if (!ProjectedFile.Root.IsValid()
				|| UeAgentStructuredAuthoringIO::WriteObject(ProjectedFile.Root.ToSharedRef())
					!= UeAgentStructuredAuthoringIO::WriteObject(RebuiltRoot.ToSharedRef()))
			{
				SetFailure(OutResult, TEXT("export_document_not_from_projected_fields"), ProjectedFile.Path, ProjectedFile.FileId);
				return false;
			}

			FPlannedFile& Planned = OutResult.Files.AddDefaulted_GetRef();
			Planned.FileId = ProjectedFile.FileId;
			Planned.RelativePath = ProjectedFile.Path;
			Planned.Role = ProjectedFile.Role;
			Planned.Root = RebuiltRoot;
			for (const UeAgentSnapshotProjector::FProjectedField& ProjectedField : ProjectedFile.Fields)
			{
				if (!AddPreferredFieldOrder(ProjectedField.JsonPointer, Planned.WriteOptions, RebuildError))
				{
					SetFailure(OutResult, TEXT("export_field_order_invalid"), RebuildError,
						ProjectedFile.FileId, ProjectedField.FieldId);
					return false;
				}
			}
			Planned.HjsonText = UeAgentStructuredAuthoringIO::WriteObject(
				RebuiltRoot.ToSharedRef(), Planned.WriteOptions) + LINE_TERMINATOR;
			Planned.Metrics = MeasureText(Planned.HjsonText);
			OutResult.Metrics.Add(Planned.Metrics);
		}

		for (const UeAgentExportContract::FFileDescriptor& ContractFile : Contract.Files)
		{
			if (ContractFile.bRequired && ContractFile.IsEnabledForProfile(Profile->ProfileId)
				&& !SeenFileIds.Contains(ContractFile.FileId))
			{
				SetFailure(OutResult, TEXT("export_required_file_missing"), ContractFile.Path, ContractFile.FileId);
				return false;
			}
		}
		for (const UeAgentExportContract::FFieldDescriptor& ContractField : Contract.Fields)
		{
			if (ContractField.bRequired && Profile->IncludesRole(ContractField.Role)
				&& !SeenContractFieldIds.Contains(ContractField.FieldId))
			{
				SetFailure(OutResult, TEXT("export_required_field_missing"), ContractField.OutputPath,
					FString(), ContractField.FieldId);
				return false;
			}
		}
		OutResult.Metrics.TokenProxy = (OutResult.Metrics.CharacterCount + 3) / 4;

		FString PathError;
		if (!UeAgentAuthoringWorkspacePaths::DetectCaseCollisions(
			OutResult.WorkspaceRoot, RelativePaths, PathError))
		{
			SetFailure(OutResult, TEXT("export_workspace_path_conflict"), PathError);
			return false;
		}
		for (FPlannedFile& Planned : OutResult.Files)
		{
			UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
			ResolveOptions.bRequireHjson = true;
			UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
			if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
				OutResult.WorkspaceRoot, Planned.RelativePath, ResolveOptions, Resolved, PathError))
			{
				SetFailure(OutResult, TEXT("export_workspace_path_invalid"), PathError, Planned.FileId);
				return false;
			}
			Planned.ResolvedPath = Resolved.AbsolutePath;
		}

		OutResult.Status = TEXT("planned");
		if (!OutResult.Files.IsEmpty())
		{
			OutResult.EntryFile = OutResult.Files[0].RelativePath;
		}
		OutResult.ErrorCode.Reset();
		OutResult.ErrorDetail.Reset();
		OutResult.FileId.Reset();
		OutResult.FieldId.Reset();
		return true;
	}

	bool BuildCompoundWorkspacePlan(
		const FString& WorkspaceRoot,
		const TArray<FWorkspaceFragment>& Fragments,
		const FCompoundPlanPolicy& Policy,
		FWorkspaceResult& OutResult)
	{
		OutResult = FWorkspaceResult();
		OutResult.Status = TEXT("failed");
		OutResult.ContractId = Policy.ContractId;
		OutResult.ContractVersion = Policy.ContractVersion;
		OutResult.ProfileId = Policy.ProfileId;
		OutResult.WorkspaceRoot = UeAgentAuthoringWorkspacePaths::NormalizeAbsolutePath(WorkspaceRoot);
		OutResult.EntryFile = Policy.EntryFile;
		OutResult.TokenBudget = Policy.TokenBudget;
		if (OutResult.WorkspaceRoot.IsEmpty()
			|| Policy.ContractId.IsEmpty()
			|| Policy.ContractVersion.IsEmpty()
			|| Policy.ProfileId.IsEmpty()
			|| Policy.EntryFile.IsEmpty()
			|| Policy.TokenBudget <= 0
			|| Fragments.IsEmpty())
		{
			SetFailure(OutResult, TEXT("export_workspace_plan_invalid"), TEXT("compound_plan_header_invalid"));
			return false;
		}

		TSet<FString> SeenFragmentIds;
		TArray<FString> RelativePaths;
		TMap<FString, int32> RoleCounts;
		bool bHasCharacterMetrics = false;
		for (const FWorkspaceFragment& Fragment : Fragments)
		{
			const FString CanonicalFragmentId = Fragment.FragmentId.ToLower();
			if (CanonicalFragmentId.IsEmpty() || SeenFragmentIds.Contains(CanonicalFragmentId) || Fragment.Files.IsEmpty())
			{
				SetFailure(
					OutResult,
					TEXT("export_workspace_fragment_invalid"),
					FString::Printf(TEXT("fragment_id=%s"), *Fragment.FragmentId));
				return false;
			}
			SeenFragmentIds.Add(CanonicalFragmentId);
			for (const FPlannedFile& FragmentFile : Fragment.Files)
			{
				FPlannedFile Planned = FragmentFile;
				const FString CandidatePath = Fragment.RelativeRoot.IsEmpty()
					? FragmentFile.RelativePath
					: FPaths::Combine(Fragment.RelativeRoot, FragmentFile.RelativePath);
				UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
				ResolveOptions.bRequireHjson = true;
				UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
				FString PathError;
				if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
					OutResult.WorkspaceRoot, CandidatePath, ResolveOptions, Resolved, PathError))
				{
					SetFailure(OutResult, TEXT("export_workspace_path_invalid"), PathError, Planned.FileId);
					return false;
				}
				Planned.RelativePath = Resolved.RelativePath;
				Planned.ResolvedPath = Resolved.AbsolutePath;
				for (FString& Reference : Planned.ReferencedRelativePaths)
				{
					if (!Fragment.RelativeRoot.IsEmpty())
					{
						Reference = FPaths::Combine(Fragment.RelativeRoot, Reference);
					}
					Reference.ReplaceInline(TEXT("\\"), TEXT("/"));
				}
				RelativePaths.Add(Planned.RelativePath);
				RoleCounts.FindOrAdd(Planned.Role) += 1;
				OutResult.Metrics.Add(Planned.Metrics);
				bHasCharacterMetrics |= Planned.Metrics.CharacterCount > 0;
				OutResult.Files.Add(MoveTemp(Planned));
			}
		}

		FString PathError;
		if (!UeAgentAuthoringWorkspacePaths::DetectCaseCollisions(
			OutResult.WorkspaceRoot, RelativePaths, PathError))
		{
			SetFailure(OutResult, TEXT("export_workspace_path_case_collision"), PathError);
			return false;
		}

		TSet<FString> CanonicalPaths;
		for (const FString& RelativePath : RelativePaths)
		{
			CanonicalPaths.Add(RelativePath.ToLower());
		}
		for (const FPlannedFile& File : OutResult.Files)
		{
			for (const FString& Reference : File.ReferencedRelativePaths)
			{
				UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
				ResolveOptions.bRequireHjson = true;
				UeAgentAuthoringWorkspacePaths::FResolvedPath ResolvedReference;
				if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
					OutResult.WorkspaceRoot, Reference, ResolveOptions, ResolvedReference, PathError)
					|| !CanonicalPaths.Contains(ResolvedReference.RelativePath.ToLower()))
				{
					SetFailure(
						OutResult,
						TEXT("export_workspace_reference_unresolved"),
						FString::Printf(TEXT("source=%s reference=%s"), *File.RelativePath, *Reference),
						File.FileId);
					return false;
				}
			}
		}

		TSet<FString> DeclaredRoles;
		for (const FRoleCardinality& Cardinality : Policy.RoleCardinalities)
		{
			if (Cardinality.Role.IsEmpty()
				|| Cardinality.Minimum < 0
				|| (Cardinality.Maximum != INDEX_NONE && Cardinality.Maximum < Cardinality.Minimum)
				|| DeclaredRoles.Contains(Cardinality.Role))
			{
				SetFailure(OutResult, TEXT("export_workspace_role_cardinality_invalid"), Cardinality.Role);
				return false;
			}
			DeclaredRoles.Add(Cardinality.Role);
			const int32 Count = RoleCounts.FindRef(Cardinality.Role);
			if (Count < Cardinality.Minimum
				|| (Cardinality.Maximum != INDEX_NONE && Count > Cardinality.Maximum))
			{
				SetFailure(
					OutResult,
					TEXT("export_workspace_role_cardinality_invalid"),
					FString::Printf(
						TEXT("role=%s count=%d minimum=%d maximum=%d"),
						*Cardinality.Role,
						Count,
						Cardinality.Minimum,
						Cardinality.Maximum));
				return false;
			}
		}
		if (!DeclaredRoles.IsEmpty())
		{
			for (const FPlannedFile& File : OutResult.Files)
			{
				const FRoleCardinality* Rule = Policy.RoleCardinalities.FindByPredicate(
					[&File](const FRoleCardinality& Candidate)
					{
						return Candidate.Role == File.Role;
					});
				if (!Rule || !MatchesRolePathPattern(File.RelativePath, Rule->PathPattern))
				{
					SetFailure(OutResult, TEXT("export_workspace_role_cardinality_invalid"),
						FString::Printf(TEXT("role=%s path=%s"), *File.Role, *File.RelativePath));
					return false;
				}
			}
		}

		UeAgentAuthoringWorkspacePaths::FResolveOptions EntryResolveOptions;
		EntryResolveOptions.bRequireHjson = true;
		UeAgentAuthoringWorkspacePaths::FResolvedPath ResolvedEntry;
		if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
			OutResult.WorkspaceRoot, Policy.EntryFile, EntryResolveOptions, ResolvedEntry, PathError)
			|| !CanonicalPaths.Contains(ResolvedEntry.RelativePath.ToLower()))
		{
			SetFailure(OutResult, TEXT("export_workspace_entry_missing"), Policy.EntryFile);
			return false;
		}
		OutResult.EntryFile = ResolvedEntry.RelativePath;

		if (bHasCharacterMetrics)
		{
			OutResult.Metrics.TokenProxy = (OutResult.Metrics.CharacterCount + 3) / 4;
		}
		OutResult.Status = TEXT("planned");
		OutResult.ErrorCode.Reset();
		OutResult.ErrorDetail.Reset();
		return true;
	}

	bool PublishWorkspacePlan(FWorkspaceResult& InOutResult)
	{
		if (!InOutResult.IsPlanned())
		{
			SetFailure(InOutResult, TEXT("export_publish_plan_invalid"));
			return false;
		}
		TArray<UeAgentExportPublisher::FFilePlan> PublishFiles;
		for (const FPlannedFile& File : InOutResult.Files)
		{
			UeAgentExportPublisher::FFilePlan& PublishFile = PublishFiles.AddDefaulted_GetRef();
			PublishFile.RelativePath = File.RelativePath;
			PublishFile.Root = File.Root;
			PublishFile.WriteOptions = File.WriteOptions;
		}
		UeAgentExportPublisher::FPublishResult PublishResult;
		UeAgentExportPublisher::FPublishContext PublishContext;
		PublishContext.ContractId = InOutResult.ContractId;
		PublishContext.ContractVersion = InOutResult.ContractVersion;
		PublishContext.ProfileId = InOutResult.ProfileId;
		FString PublishErrorCode;
		FString PublishErrorDetail;
		if (!UeAgentExportPublisher::PublishWorkspace(
			InOutResult.WorkspaceRoot,
			PublishFiles,
			PublishContext,
			PublishResult,
			PublishErrorCode,
			PublishErrorDetail))
		{
			SetFailure(InOutResult, PublishErrorCode, PublishErrorDetail);
			return false;
		}
		InOutResult.PublisherReceipt = PublishResult.Receipt;
		InOutResult.WrittenFiles = MoveTemp(PublishResult.WrittenFiles);
		InOutResult.WrittenRelativeFiles = MoveTemp(PublishResult.WrittenRelativeFiles);
		InOutResult.Status = TEXT("written");
		return true;
	}

	bool WriteWorkspace(
		const UeAgentExportContract::FContract& Contract,
		const UeAgentSnapshotProjector::FProjectionResult& Projection,
		const FString& WorkspaceRoot,
		FWorkspaceResult& OutResult)
	{
		if (!BuildWorkspacePlan(Contract, Projection, WorkspaceRoot, OutResult)) return false;
		return PublishWorkspacePlan(OutResult);
	}

	bool WriteSingleFile(
		const UeAgentExportContract::FContract& Contract,
		const UeAgentSnapshotProjector::FProjectionResult& Projection,
		const FString& OutputFile,
		FWorkspaceResult& OutResult)
	{
		const FString ResolvedFile = UeAgentStructuredAuthoringIO::NormalizeAuthoringPath(OutputFile);
		const FString Parent = FPaths::GetPath(ResolvedFile);
		if (!BuildWorkspacePlan(Contract, Projection, Parent, OutResult)) return false;
		if (OutResult.Files.Num() != 1)
		{
			SetFailure(OutResult, TEXT("export_single_file_contract_invalid"), FString::Printf(
				TEXT("contract_id=%s enabled_files=%d"), *Contract.ContractId, OutResult.Files.Num()));
			return false;
		}

		FPlannedFile& File = OutResult.Files[0];
		UeAgentExportPublisher::FFilePlan PublishFile;
		PublishFile.RelativePath = FPaths::GetCleanFilename(ResolvedFile);
		PublishFile.Root = File.Root;
		PublishFile.WriteOptions = File.WriteOptions;
		UeAgentExportPublisher::FPublishResult PublishResult;
		UeAgentExportPublisher::FPublishContext PublishContext;
		PublishContext.ContractId = OutResult.ContractId;
		PublishContext.ContractVersion = OutResult.ContractVersion;
		PublishContext.ProfileId = OutResult.ProfileId;
		FString PublishErrorCode;
		FString PublishErrorDetail;
		if (!UeAgentExportPublisher::PublishSingleFile(
			ResolvedFile,
			PublishFile,
			PublishContext,
			PublishResult,
			PublishErrorCode,
			PublishErrorDetail))
		{
			SetFailure(OutResult, PublishErrorCode, PublishErrorDetail, File.FileId);
			return false;
		}
		File.ResolvedPath = ResolvedFile;
		OutResult.WorkspaceRoot = Parent;
		OutResult.EntryFile = FPaths::GetCleanFilename(ResolvedFile);
		OutResult.PublisherReceipt = PublishResult.Receipt;
		OutResult.WrittenFiles = MoveTemp(PublishResult.WrittenFiles);
		OutResult.WrittenRelativeFiles = MoveTemp(PublishResult.WrittenRelativeFiles);
		OutResult.Status = TEXT("written");
		return true;
	}
}
