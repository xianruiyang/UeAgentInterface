// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

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
	using UE::UAI::Contract::FDiagnostic;

	FDiagnostic WriterFailure(const FString& Code, const FString& Reason,
		const FString& Path = FString(), const TSharedPtr<FJsonValue>& Actual = nullptr,
		const TSharedPtr<FJsonValue>& Expected = nullptr)
	{
		FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("export_writer"));
		Error.SourceKind = TEXT("native_result");
		Error.PropertyPath = Path;
		Error.SemanticPointer = Path;
		Error.Source.JsonPath = Path;
		Error.ActualValue = Actual;
		Error.ExpectedValue = Expected;
		return Error;
	}

	void SetFailure(FWorkspaceResult& OutResult, const UE::UAI::Contract::FDiagnostic& Diagnostic,
        const FString& FileId = FString(), const FString& FieldId = FString())
    {
        OutResult.Status = TEXT("failed");
        OutResult.Diagnostic = Diagnostic;
        OutResult.FileId = FileId;
        OutResult.FieldId = FieldId;
        if (OutResult.Diagnostic.Source.SourceUnit.IsEmpty()) OutResult.Diagnostic.Source.SourceUnit = FileId;
        if (OutResult.Diagnostic.PropertyPath.IsEmpty()) OutResult.Diagnostic.PropertyPath = FieldId;
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
		if (!Text.IsEmpty() && !Text.EndsWith(TEXT("\n"))) ++Result.LineCount;
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

	bool ValidateContractPointer(
		const UeAgentExportContract::FFieldDescriptor& ContractField,
		const FString& ContractPointer,
		const UeAgentSnapshotProjector::FProjectedField& ProjectedField,
		FDiagnostic& OutError)
	{
		if (!ContractPointer.Contains(TEXT("*")))
		{
			if (!ProjectedField.bCompoundContainerFallback && ContractPointer == ProjectedField.JsonPointer) return true;
			OutError = WriterFailure(TEXT("export_projected_field_contract_mismatch"),
				ProjectedField.bCompoundContainerFallback
					? TEXT("A container fallback cannot replace a field without a wildcard output path.")
					: TEXT("The projected field pointer differs from its contract output pointer."),
				ProjectedField.JsonPointer, MakeShared<FJsonValueString>(ProjectedField.JsonPointer),
				MakeShared<FJsonValueString>(ContractPointer));
			return false;
		}
		if (!ProjectedField.bCompoundContainerFallback)
		{
			if (UeAgentCompoundProjection::MatchesExpandedJsonPointer(ContractPointer, ProjectedField.JsonPointer)) return true;
			OutError = WriterFailure(TEXT("export_projected_field_contract_mismatch"),
				TEXT("The projected field pointer does not match its contract output pattern."),
				ProjectedField.JsonPointer, MakeShared<FJsonValueString>(ProjectedField.JsonPointer),
				MakeShared<FJsonValueString>(ContractPointer));
			return false;
		}

		UeAgentCompoundProjection::FJsonPointerPatternInfo PatternInfo;
		UE::UAI::Contract::FDiagnostic PatternError;
		if (!UeAgentCompoundProjection::InspectJsonPointerPattern(
			ContractPointer, PatternInfo, PatternError))
		{
			OutError = PatternError.IsEmpty()
				? WriterFailure(TEXT("export_projected_field_contract_mismatch"),
					TEXT("The contract output pointer has an invalid wildcard pattern."), ProjectedField.JsonPointer,
					MakeShared<FJsonValueString>(ContractPointer), MakeShared<FJsonValueString>(TEXT("a valid output pointer pattern")))
				: PatternError;
			return false;
		}
		if (ProjectedField.JsonPointer != PatternInfo.EmptyContainerPointer)
		{
			OutError = WriterFailure(TEXT("export_projected_field_contract_mismatch"),
				TEXT("The projected container fallback is at the wrong output pointer."),
				ProjectedField.JsonPointer, MakeShared<FJsonValueString>(ProjectedField.JsonPointer),
				MakeShared<FJsonValueString>(PatternInfo.EmptyContainerPointer));
			return false;
		}
		if (!ProjectedField.Value.IsValid())
		{
			OutError = WriterFailure(TEXT("export_projected_field_contract_mismatch"),
				TEXT("The projected container fallback has no value."), ProjectedField.JsonPointer,
				nullptr, MakeShared<FJsonValueString>(TEXT("a value allowed by the field's empty policy")));
			OutError.ActualValuePresent = false;
			return false;
		}
		if (ContractField.EmptyPolicy == TEXT("emit_null"))
		{
			if (ProjectedField.Value->Type == EJson::Null) return true;
			OutError = WriterFailure(TEXT("export_projected_field_contract_mismatch"),
				TEXT("The contract requires a null value for this empty container."),
				ProjectedField.JsonPointer, ProjectedField.Value, MakeShared<FJsonValueNull>());
			return false;
		}
		const TSharedPtr<FJsonObject> EmptyObject = ProjectedField.Value->Type == EJson::Object
			? ProjectedField.Value->AsObject()
			: nullptr;
		if (ContractField.EmptyPolicy == TEXT("emit_empty") && EmptyObject.IsValid() && EmptyObject->Values.IsEmpty()) return true;
		OutError = WriterFailure(TEXT("export_projected_field_contract_mismatch"),
			ContractField.EmptyPolicy == TEXT("emit_empty")
				? TEXT("The contract requires an empty object for this container fallback.")
				: TEXT("The contract's empty policy does not allow a container fallback."),
			ProjectedField.JsonPointer, ProjectedField.Value,
			MakeShared<FJsonValueString>(ContractField.EmptyPolicy == TEXT("emit_empty") ? TEXT("an empty object") : TEXT("no container fallback")));
		return false;
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

	bool AddPreferredFieldOrder(
		const FString& JsonPointer,
		FUeAgentStructuredHjsonWriteOptions& OutOptions,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (JsonPointer.IsEmpty()) return true;
		if (!JsonPointer.StartsWith(TEXT("/")))
		{
			OutError = WriterFailure(TEXT("json_pointer_must_start_with_slash"),
				TEXT("A projected field's JSON pointer must start with '/'."), TEXT("/json_pointer"),
				MakeShared<FJsonValueString>(JsonPointer), MakeShared<FJsonValueString>(TEXT("a JSON pointer starting with /")));
			return false;
		}
		TArray<FString> EncodedSegments;
		JsonPointer.Mid(1).ParseIntoArray(EncodedSegments, TEXT("/"), false);
		FString ParentPointer = TEXT("/");
		for (int32 SegmentIndex = 0; SegmentIndex < EncodedSegments.Num(); ++SegmentIndex)
		{
			const FString& Encoded = EncodedSegments[SegmentIndex];
			FString Decoded;
			if (!DecodeJsonPointerSegment(Encoded, Decoded) || Decoded.IsEmpty())
			{
				OutError = WriterFailure(TEXT("json_pointer_segment_invalid"),
					TEXT("A projected field pointer contains an empty segment or an invalid '~' escape."),
					FString::Printf(TEXT("/json_pointer/segments/%d"), SegmentIndex),
					MakeShared<FJsonValueString>(Encoded), MakeShared<FJsonValueString>(TEXT("a nonempty segment using only ~0 and ~1 escapes")));
				return false;
			}
			TArray<FString>& Preferred = OutOptions.PreferredKeyOrderByJsonPointer.FindOrAdd(ParentPointer);
			Preferred.AddUnique(Decoded);
			ParentPointer = ParentPointer == TEXT("/")
				? TEXT("/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Decoded)
				: ParentPointer + TEXT("/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Decoded);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = WriterFailure(TEXT("export_writer_field_value_invalid"), TEXT("The projected field has no value."),
				Path, nullptr, MakeShared<FJsonValueString>(TEXT("a JSON value")));
			OutError.ActualValuePresent = false;
			return false;
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (!ValidateFieldPolicy(
					Values[Index], Policy, ProfileId, bAllowsPropertyPath, bInsideCanonicalValue,
					FString::Printf(TEXT("%s/%d"), *Path, Index), OutError))
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
			OutError = WriterFailure(TEXT("export_writer_field_object_invalid"), TEXT("The projected object value has no object data."),
				Path, nullptr, MakeShared<FJsonValueString>(TEXT("a JSON object")));
			OutError.ActualValuePresent = false;
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			const FString ChildPath = Path + TEXT("/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key);
			if (!bInsideCanonicalValue)
			{
				if (IsRemovedField(Policy, Pair.Key))
				{
					OutError = WriterFailure(TEXT("export_writer_removed_field_present"),
						TEXT("The projected document contains a field excluded by the export writer policy."), ChildPath,
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
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
						OutError = WriterFailure(TEXT("export_writer_value_text_profile_violation"),
							TEXT("This export profile requires value_text to be omitted when a structured value is present."),
							ChildPath, MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
						OutError.InternalContext = ProfileId;
						return false;
					}
				}
				if (Pair.Key.Equals(TEXT("property_path"), ESearchCase::IgnoreCase) && !bAllowsPropertyPath)
				{
					OutError = WriterFailure(TEXT("export_writer_property_path_not_editable"),
						TEXT("A property_path is only allowed for an editable field or an identity/reference field."),
						ChildPath, MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					return false;
				}
			}
			const bool bChildInsideCanonicalValue = bInsideCanonicalValue
				|| Pair.Key.Equals(TEXT("value"), ESearchCase::IgnoreCase);
			if (!ValidateFieldPolicy(
				Pair.Value, Policy, ProfileId, bAllowsPropertyPath, bChildInsideCanonicalValue,
				ChildPath, OutError))
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
		UE::UAI::Contract::FDiagnostic ContractError;
		if (!UeAgentExportContract::ValidateContract(Contract, ContractError))
		{
			SetFailure(OutResult, ContractError);
			return false;
		}
		if (!Projection.IsProjected())
		{
			SetFailure(OutResult, Projection.Diagnostic.IsEmpty()
				? WriterFailure(TEXT("export_projection_unavailable"), TEXT("Snapshot projection did not produce a successful export projection."))
				: Projection.Diagnostic);
			return false;
		}
		if (Projection.ContractId != Contract.ContractId
			|| Projection.ContractVersion != Contract.ContractVersion)
		{
			FDiagnostic Error = WriterFailure(TEXT("export_projection_contract_mismatch"),
				Projection.ContractId != Contract.ContractId
					? TEXT("The projection was produced for a different export contract.")
					: TEXT("The projection was produced for a different version of the export contract."));
			Error.ActualEvidence = MakeShared<FJsonValueString>(Projection.ContractId + TEXT(":") + Projection.ContractVersion);
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Contract.ContractId + TEXT(":") + Contract.ContractVersion);
			SetFailure(OutResult, Error);
			return false;
		}
		OutProfile = Contract.FindProfile(Projection.ProfileId);
		if (!OutProfile)
		{
			FDiagnostic Error = WriterFailure(TEXT("export_projection_profile_mismatch"),
				TEXT("The projection uses a profile that is not declared by the export contract."), TEXT("/profile"));
			Error.InternalContext = Projection.ProfileId;
			SetFailure(OutResult, Error);
			return false;
		}
		return true;
	}
}

namespace UeAgentExportWorkspaceWriter
{
	FTextMetrics MeasureUtf8Text(const TConstArrayView<uint8> Utf8Text)
	{
		if (Utf8Text.IsEmpty())
		{
			return {};
		}
		const FUTF8ToTCHAR Decoded(
			reinterpret_cast<const ANSICHAR*>(Utf8Text.GetData()),
			Utf8Text.Num());
		// The input is the actual UTF-8 formatter/file body. Its byte length is
		// already known: do not allocate a second FString and encode it again.
		FTextMetrics Result;
		Result.CharacterCount = Decoded.Length();
		Result.Utf8Bytes = Utf8Text.Num();
		for (int32 Index = 0; Index < Decoded.Length(); ++Index)
			if (Decoded.Get()[Index] == TEXT('\n')) ++Result.LineCount;
		if (Decoded.Length() && Decoded.Get()[Decoded.Length() - 1] != TEXT('\n')) ++Result.LineCount;
		Result.TokenProxy = (Result.CharacterCount + 3) / 4;
		return Result;
	}

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
		return Status == TEXT("planned") && Diagnostic.IsEmpty();
	}

	bool FWorkspaceResult::IsWritten() const
	{
		return Status == TEXT("written") && Diagnostic.IsEmpty()
			&& WrittenFiles.Num() == Files.Num();
	}

	TSharedRef<FJsonObject> MakePublicWorkspaceSuccessResult(TConstArrayView<UE::UAI::Contract::FDiagnostic> Warnings)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		if (!Warnings.IsEmpty()) {
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const auto& Warning : Warnings) Values.Add(MakeShared<FJsonValueObject>(Warning.ToPublicJson()));
			Result->SetArrayField(TEXT("warnings"), Values);
		}
		return Result;
	}

	TSharedRef<FJsonObject> MakePublicWorkspaceSuccessResult(const TSharedPtr<FJsonObject>& PublisherReceipt)
	{
		TSharedRef<FJsonObject> Result = MakePublicWorkspaceSuccessResult();
		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		if (PublisherReceipt && PublisherReceipt->TryGetArrayField(TEXT("warnings"), Warnings) && !Warnings->IsEmpty())
			Result->SetArrayField(TEXT("warnings"), *Warnings);
		return Result;
	}

	TSharedRef<FJsonObject> FWorkspaceResult::ToPublicJsonObject() const
	{
		if (IsWritten())
		{
			return MakePublicWorkspaceSuccessResult(PublisherReceipt);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), Status);
		if (!Diagnostic.IsEmpty()) Result->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
		return Result;
	}

	TSharedRef<FJsonObject> FWorkspaceResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("uai.export_workspace_result.v1"));
		Result->SetStringField(TEXT("status"), Status);
		Result->SetStringField(TEXT("contract_id"), ContractId);
		Result->SetStringField(TEXT("contract_version"), ContractVersion);
		Result->SetStringField(TEXT("profile_id"), ProfileId);
		if (!ExpectedRevision.IsEmpty())
		{
			Result->SetStringField(TEXT("expected_revision"), ExpectedRevision);
		}
		Result->SetStringField(TEXT("workspace_root"), WorkspaceRoot);
		if (!EntryFile.IsEmpty())
		{
			Result->SetStringField(TEXT("entry_file"), EntryFile);
		}
		Result->SetObjectField(TEXT("metrics"), Metrics.ToJsonObject());
		Result->SetObjectField(
			TEXT("model_metrics"), ModelMetrics.ToJsonObject());
		if (PublisherReceipt.IsValid())
		{
			Result->SetObjectField(TEXT("publisher"), PublisherReceipt.ToSharedRef());
		}
		if (!Diagnostic.IsEmpty())
		{
			Result->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
			if (!FileId.IsEmpty()) Result->SetStringField(TEXT("file_id"), FileId);
			if (!FieldId.IsEmpty()) Result->SetStringField(TEXT("field_id"), FieldId);
		}
		Result->SetNumberField(TEXT("file_count"), Files.Num());
		Result->SetNumberField(
			TEXT("written_file_count"), WrittenFiles.Num());
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
		if (OutResult.WorkspaceRoot.IsEmpty())
		{
			SetFailure(OutResult, WriterFailure(TEXT("export_workspace_root_invalid"),
				TEXT("The export workspace root does not resolve to a nonempty path."), TEXT("/workspace_root"),
				MakeShared<FJsonValueString>(WorkspaceRoot), MakeShared<FJsonValueString>(TEXT("a nonempty workspace path"))));
			return false;
		}

		TSet<FString> SeenFileIds;
		TSet<FString> SeenProjectedFieldIds;
		TSet<FString> SeenContractFieldIds;
		TArray<FString> RelativePaths;
		for (const UeAgentSnapshotProjector::FProjectedFile& ProjectedFile : Projection.Files)
		{
			const UeAgentExportContract::FFileDescriptor* ContractFile = Contract.FindFile(ProjectedFile.FileId);
			FDiagnostic FileError;
			const FString FileCode = TEXT("export_projected_file_contract_mismatch");
			if (!ContractFile)
				FileError = WriterFailure(FileCode, TEXT("The projected file is not declared by the export contract."));
			else if (!ContractFile->IsEnabledForProfile(Profile->ProfileId))
				FileError = WriterFailure(FileCode, TEXT("The projected file is disabled for the selected export profile."));
			else if (SeenFileIds.Contains(ProjectedFile.FileId))
				FileError = WriterFailure(FileCode, TEXT("The projection contains this file more than once."));
			else if (ContractFile->Path != ProjectedFile.Path)
				FileError = WriterFailure(FileCode, TEXT("The projected file path differs from the contract path."), TEXT("/path"),
					MakeShared<FJsonValueString>(ProjectedFile.Path), MakeShared<FJsonValueString>(ContractFile->Path));
			else if (ContractFile->Role != ProjectedFile.Role)
				FileError = WriterFailure(FileCode, TEXT("The projected file role differs from the contract role."), TEXT("/role"),
					MakeShared<FJsonValueString>(ProjectedFile.Role), MakeShared<FJsonValueString>(ContractFile->Role));
			else if (ContractFile->bRequired != ProjectedFile.bRequired)
				FileError = WriterFailure(FileCode, TEXT("The projected file required flag differs from the contract."), TEXT("/required"),
					MakeShared<FJsonValueBoolean>(ProjectedFile.bRequired), MakeShared<FJsonValueBoolean>(ContractFile->bRequired));
			if (!FileError.IsEmpty())
			{
				FileError.TargetPath = ProjectedFile.Path;
				SetFailure(OutResult, FileError, ProjectedFile.FileId);
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
				FDiagnostic FieldError;
				const FString FieldCode = TEXT("export_projected_field_contract_mismatch");
				if (!ContractField)
					FieldError = WriterFailure(FieldCode, TEXT("The projected field is not declared by the export contract."), ProjectedField.JsonPointer);
				else if (!UeAgentExportContract::SplitOutputPath(ContractField->OutputPath, ContractFilePath, ContractPointer))
					FieldError = WriterFailure(FieldCode, TEXT("The contract field output path cannot be split into a file and JSON pointer."),
						ProjectedField.JsonPointer, MakeShared<FJsonValueString>(ContractField->OutputPath),
						MakeShared<FJsonValueString>(TEXT("file#json-pointer")));
				else if (ContractFilePath != ProjectedFile.Path)
					FieldError = WriterFailure(FieldCode, TEXT("The projected field belongs to a different output file."),
						ProjectedField.JsonPointer, MakeShared<FJsonValueString>(ProjectedFile.Path), MakeShared<FJsonValueString>(ContractFilePath));
				else if (!ValidateContractPointer(*ContractField, ContractPointer, ProjectedField, FieldError))
				{
					// The pointer validator owns the exact failure and any lower diagnostic.
				}
				else if (ContractField->Role != ProjectedField.Role)
					FieldError = WriterFailure(FieldCode, TEXT("The projected field role differs from the contract role."),
						ProjectedField.JsonPointer, MakeShared<FJsonValueString>(ProjectedField.Role), MakeShared<FJsonValueString>(ContractField->Role));
				else if (ContractField->bEditable != ProjectedField.bEditable)
					FieldError = WriterFailure(FieldCode, TEXT("The projected field editable flag differs from the contract."),
						ProjectedField.JsonPointer, MakeShared<FJsonValueBoolean>(ProjectedField.bEditable), MakeShared<FJsonValueBoolean>(ContractField->bEditable));
				else if (!Profile->IncludesRole(ContractField->Role))
					FieldError = WriterFailure(FieldCode, TEXT("The projected field role is excluded by the selected export profile."),
						ProjectedField.JsonPointer, MakeShared<FJsonValueString>(ContractField->Role), MakeShared<FJsonValueString>(TEXT("a role included in the selected profile")));
				else if (FileProjectedFieldIds.Contains(ProjectedField.FieldId) || SeenProjectedFieldIds.Contains(ProjectedField.FieldId))
					FieldError = WriterFailure(FieldCode, TEXT("The projection contains this field identifier more than once."), ProjectedField.JsonPointer);
				if (!FieldError.IsEmpty())
				{
					if (FieldError.TargetPath.IsEmpty()) FieldError.TargetPath = ProjectedFile.Path;
					SetFailure(OutResult, FieldError, ProjectedFile.FileId, ProjectedField.FieldId);
					return false;
				}
				UE::UAI::Contract::FDiagnostic PolicyError;
				if (!ValidateFieldPolicy(
					ProjectedField.Value,
					Contract.WriterPolicy,
					Profile->ProfileId,
					ProjectedField.bEditable
						|| ProjectedField.Role == TEXT("identity")
						|| ProjectedField.Role == TEXT("reference"),
					IsCanonicalValueProjector(ContractField->Source.ProjectorId),
					ProjectedField.JsonPointer,
					PolicyError))
				{
					SetFailure(OutResult, PolicyError, ProjectedFile.FileId, ProjectedField.FieldId);
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
					FDiagnostic Error = WriterFailure(TEXT("export_required_field_missing"),
						TEXT("The projection is missing a field required by the export contract."), ContractPointer,
						nullptr, MakeShared<FJsonValueString>(TEXT("a projected field")));
					Error.ActualValuePresent = false;
					Error.TargetPath = ContractFilePath;
					SetFailure(OutResult, Error, ProjectedFile.FileId, ContractField.FieldId);
					return false;
				}
			}

			TSharedPtr<FJsonObject> RebuiltRoot;
			UE::UAI::Contract::FDiagnostic RebuildError;
			if (!UeAgentSnapshotProjector::BuildProjectedFileRoot(ProjectedFile, RebuiltRoot, RebuildError))
			{
				SetFailure(OutResult, RebuildError, ProjectedFile.FileId);
				return false;
			}
			if (!ProjectedFile.Root.IsValid())
			{
				FDiagnostic Error = WriterFailure(TEXT("export_document_not_from_projected_fields"),
					TEXT("The projected file has no document object."), FString(), nullptr,
					MakeShared<FJsonValueString>(TEXT("the document built from the projected fields")));
				Error.ActualValuePresent = false;
				Error.TargetPath = ProjectedFile.Path;
				SetFailure(OutResult, Error, ProjectedFile.FileId);
				return false;
			}
			const FString ActualText = UeAgentStructuredAuthoringIO::WriteObject(ProjectedFile.Root.ToSharedRef());
			const FString ExpectedText = UeAgentStructuredAuthoringIO::WriteObject(RebuiltRoot.ToSharedRef());
			if (ActualText != ExpectedText)
			{
				const auto Difference = UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(
					MakeShared<FJsonValueObject>(RebuiltRoot), MakeShared<FJsonValueObject>(ProjectedFile.Root));
				FDiagnostic Error = WriterFailure(TEXT("export_document_not_from_projected_fields"),
					Difference.bEquivalent
						? TEXT("The projected document has equivalent JSON values but differs from the required serialized field output.")
						: TEXT("The projected document differs from the document rebuilt from its fields."));
				Error.TargetPath = ProjectedFile.Path;
				if (!Difference.bEquivalent)
				{
					Difference.ApplyTo(Error);
					Error.PropertyPath = Difference.Path;
					Error.SemanticPointer = Difference.Path;
				}
				else
				{
					int32 Offset = 0;
					while (Offset < ActualText.Len() && Offset < ExpectedText.Len() && ActualText[Offset] == ExpectedText[Offset]) ++Offset;
					Error.PropertyPath = FString::Printf(TEXT("/serialized_text/%d"), Offset);
					Error.ActualValue = MakeShared<FJsonValueString>(ActualText.Mid(Offset, 64));
					Error.ExpectedValue = MakeShared<FJsonValueString>(ExpectedText.Mid(Offset, 64));
				}
				SetFailure(OutResult, Error, ProjectedFile.FileId);
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
					SetFailure(OutResult, RebuildError, ProjectedFile.FileId, ProjectedField.FieldId);
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
				FDiagnostic Error = WriterFailure(TEXT("export_required_file_missing"),
					TEXT("The projection is missing an output file required by the export contract."), FString(),
					nullptr, MakeShared<FJsonValueString>(TEXT("a projected file")));
				Error.ActualValuePresent = false;
				Error.TargetPath = ContractFile.Path;
				SetFailure(OutResult, Error, ContractFile.FileId);
				return false;
			}
		}
		for (const UeAgentExportContract::FFieldDescriptor& ContractField : Contract.Fields)
		{
			if (ContractField.bRequired && Profile->IncludesRole(ContractField.Role)
				&& !SeenContractFieldIds.Contains(ContractField.FieldId))
			{
				FString RequiredFile;
				FString RequiredPointer;
				UeAgentExportContract::SplitOutputPath(ContractField.OutputPath, RequiredFile, RequiredPointer);
				FDiagnostic Error = WriterFailure(TEXT("export_required_field_missing"),
					TEXT("The projection is missing a field required by the export contract."), RequiredPointer,
					nullptr, MakeShared<FJsonValueString>(TEXT("a projected field")));
				Error.ActualValuePresent = false;
				Error.TargetPath = RequiredFile;
				SetFailure(OutResult, Error, FString(), ContractField.FieldId);
				return false;
			}
		}
		OutResult.Metrics.TokenProxy = (OutResult.Metrics.CharacterCount + 3) / 4;
		OutResult.ModelMetrics = OutResult.Metrics;

		UE::UAI::Contract::FDiagnostic PathError;
		if (!UeAgentAuthoringWorkspacePaths::DetectCaseCollisions(
			OutResult.WorkspaceRoot, RelativePaths, PathError))
		{
			SetFailure(OutResult, PathError);
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
				SetFailure(OutResult, PathError, Planned.FileId);
				return false;
			}
			Planned.ResolvedPath = Resolved.AbsolutePath;
		}

		OutResult.Status = TEXT("planned");
		if (!OutResult.Files.IsEmpty())
		{
			OutResult.EntryFile = OutResult.Files[0].RelativePath;
		}
		OutResult.Diagnostic.Reset();
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
		if (OutResult.WorkspaceRoot.IsEmpty()
			|| Policy.ContractId.IsEmpty()
			|| Policy.ContractVersion.IsEmpty()
			|| Policy.ProfileId.IsEmpty()
			|| Policy.EntryFile.IsEmpty()
			|| Fragments.IsEmpty())
		{
			const FString Missing = OutResult.WorkspaceRoot.IsEmpty() ? TEXT("workspace_root")
				: Policy.ContractId.IsEmpty() ? TEXT("contract_id")
				: Policy.ContractVersion.IsEmpty() ? TEXT("contract_version")
				: Policy.ProfileId.IsEmpty() ? TEXT("profile_id")
				: Policy.EntryFile.IsEmpty() ? TEXT("entry_file") : TEXT("fragments");
			FDiagnostic Error = WriterFailure(TEXT("export_workspace_plan_invalid"),
				FString::Printf(TEXT("The compound export plan requires a nonempty %s."), *Missing), TEXT("/") + Missing);
			if (Missing == TEXT("fragments"))
			{
				Error.ActualValue = MakeShared<FJsonValueNumber>(Fragments.Num());
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one fragment"));
			}
			else
			{
				Error.ActualValue = MakeShared<FJsonValueString>(FString());
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a nonempty value"));
			}
			SetFailure(OutResult, Error);
			return false;
		}

		TSet<FString> SeenFragmentIds;
		TArray<FString> RelativePaths;
		TMap<FString, int32> RoleCounts;
		bool bHasCharacterMetrics = false;
		for (int32 FragmentIndex = 0; FragmentIndex < Fragments.Num(); ++FragmentIndex)
		{
			const FWorkspaceFragment& Fragment = Fragments[FragmentIndex];
			const FString CanonicalFragmentId = Fragment.FragmentId.ToLower();
			if (CanonicalFragmentId.IsEmpty() || SeenFragmentIds.Contains(CanonicalFragmentId) || Fragment.Files.IsEmpty())
			{
				FDiagnostic Error = WriterFailure(TEXT("export_workspace_fragment_invalid"),
					CanonicalFragmentId.IsEmpty() ? TEXT("The export fragment has no identifier.")
						: SeenFragmentIds.Contains(CanonicalFragmentId) ? TEXT("The export fragment identifier is duplicated (case-insensitive).")
						: TEXT("The export fragment contains no files."),
					FString::Printf(TEXT("/fragments/%d/%s"), FragmentIndex,
						CanonicalFragmentId.IsEmpty() || SeenFragmentIds.Contains(CanonicalFragmentId) ? TEXT("fragment_id") : TEXT("files")));
				Error.InternalContext = Fragment.FragmentId;
				Error.TargetPath = Fragment.RelativeRoot;
				if (CanonicalFragmentId.IsEmpty())
				{
					Error.ActualValue = MakeShared<FJsonValueString>(FString());
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a nonempty fragment identifier"));
				}
				else if (!SeenFragmentIds.Contains(CanonicalFragmentId))
				{
					Error.ActualValue = MakeShared<FJsonValueNumber>(Fragment.Files.Num());
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one file"));
				}
				SetFailure(OutResult, Error);
				return false;
			}
			SeenFragmentIds.Add(CanonicalFragmentId);
			for (const FPlannedFile& FragmentFile : Fragment.Files)
			{
				FPlannedFile Planned = FragmentFile;
				if (Planned.Metrics.CharacterCount == 0
					&& !Planned.HjsonText.IsEmpty())
				{
					Planned.Metrics = MeasureText(Planned.HjsonText);
				}
				const FString CandidatePath = Fragment.RelativeRoot.IsEmpty()
					? FragmentFile.RelativePath
					: FPaths::Combine(Fragment.RelativeRoot, FragmentFile.RelativePath);
				UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
				ResolveOptions.bRequireHjson = true;
				UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
				UE::UAI::Contract::FDiagnostic PathError;
				if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
					OutResult.WorkspaceRoot, CandidatePath, ResolveOptions, Resolved, PathError))
				{
					SetFailure(OutResult, PathError, Planned.FileId);
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

		UE::UAI::Contract::FDiagnostic PathError;
		if (!UeAgentAuthoringWorkspacePaths::DetectCaseCollisions(
			OutResult.WorkspaceRoot, RelativePaths, PathError))
		{
			SetFailure(OutResult, PathError);
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
					OutResult.WorkspaceRoot, Reference, ResolveOptions, ResolvedReference, PathError))
				{
					SetFailure(OutResult, PathError, File.FileId);
					return false;
				}
				if (!CanonicalPaths.Contains(ResolvedReference.RelativePath.ToLower()))
				{
					FDiagnostic Error = WriterFailure(TEXT("export_workspace_reference_unresolved"),
						TEXT("The exported file references a file that is absent from the workspace plan."), TEXT("/references"),
						MakeShared<FJsonValueString>(Reference), MakeShared<FJsonValueString>(TEXT("a file included in the workspace plan")));
					Error.TargetPath = File.RelativePath;
					SetFailure(OutResult, Error, File.FileId);
					return false;
				}
			}
		}

		TSet<FString> DeclaredRoles;
		for (int32 RuleIndex = 0; RuleIndex < Policy.RoleCardinalities.Num(); ++RuleIndex)
		{
			const FRoleCardinality& Cardinality = Policy.RoleCardinalities[RuleIndex];
			if (Cardinality.Role.IsEmpty()
				|| Cardinality.Minimum < 0
				|| (Cardinality.Maximum != INDEX_NONE && Cardinality.Maximum < Cardinality.Minimum)
				|| DeclaredRoles.Contains(Cardinality.Role))
			{
				FDiagnostic Error = WriterFailure(TEXT("export_workspace_role_cardinality_invalid"),
					Cardinality.Role.IsEmpty() ? TEXT("An export role cardinality rule has no role.")
						: Cardinality.Minimum < 0 ? TEXT("An export role minimum cannot be negative.")
						: Cardinality.Maximum != INDEX_NONE && Cardinality.Maximum < Cardinality.Minimum
							? TEXT("An export role maximum cannot be below its minimum.")
							: TEXT("The export role has more than one cardinality rule."),
					FString::Printf(TEXT("/role_cardinalities/%d/%s"), RuleIndex,
						Cardinality.Role.IsEmpty() ? TEXT("role") : Cardinality.Minimum < 0 ? TEXT("minimum")
							: Cardinality.Maximum != INDEX_NONE && Cardinality.Maximum < Cardinality.Minimum ? TEXT("maximum") : TEXT("role")));
				if (Cardinality.Role.IsEmpty())
				{
					Error.ActualValue = MakeShared<FJsonValueString>(FString());
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a nonempty role"));
				}
				else if (Cardinality.Minimum < 0)
				{
					Error.ActualValue = MakeShared<FJsonValueNumber>(Cardinality.Minimum);
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("minimum >= 0"));
				}
				else if (Cardinality.Maximum != INDEX_NONE && Cardinality.Maximum < Cardinality.Minimum)
				{
					Error.ActualValue = MakeShared<FJsonValueNumber>(Cardinality.Maximum);
					Error.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("maximum >= %d, or -1 for unbounded"), Cardinality.Minimum));
				}
				else
				{
					Error.ActualValue = MakeShared<FJsonValueString>(Cardinality.Role);
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a unique role"));
				}
				SetFailure(OutResult, Error);
				return false;
			}
			DeclaredRoles.Add(Cardinality.Role);
			const int32 Count = RoleCounts.FindRef(Cardinality.Role);
			if (Count < Cardinality.Minimum
				|| (Cardinality.Maximum != INDEX_NONE && Count > Cardinality.Maximum))
			{
				TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
				Bounds->SetNumberField(TEXT("minimum"), Cardinality.Minimum);
				if (Cardinality.Maximum != INDEX_NONE) Bounds->SetNumberField(TEXT("maximum"), Cardinality.Maximum);
				SetFailure(OutResult, WriterFailure(TEXT("export_workspace_role_cardinality_invalid"),
					TEXT("The number of exported files for this role is outside its allowed range."),
					TEXT("/roles/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Cardinality.Role),
					MakeShared<FJsonValueNumber>(Count), MakeShared<FJsonValueObject>(Bounds)));
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
					FDiagnostic Error = WriterFailure(TEXT("export_workspace_role_cardinality_invalid"),
						!Rule ? TEXT("The exported file uses a role with no declared cardinality rule.")
							: TEXT("The exported file path does not match its role's required path pattern."),
						!Rule ? TEXT("/role") : TEXT("/path"),
						MakeShared<FJsonValueString>(!Rule ? File.Role : File.RelativePath),
						MakeShared<FJsonValueString>(!Rule ? TEXT("a declared role") : Rule->PathPattern));
					Error.TargetPath = File.RelativePath;
					SetFailure(OutResult, Error, File.FileId);
					return false;
				}
			}
		}

		UeAgentAuthoringWorkspacePaths::FResolveOptions EntryResolveOptions;
		EntryResolveOptions.bRequireHjson = true;
		UeAgentAuthoringWorkspacePaths::FResolvedPath ResolvedEntry;
		if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
			OutResult.WorkspaceRoot, Policy.EntryFile, EntryResolveOptions, ResolvedEntry, PathError))
		{
			SetFailure(OutResult, PathError);
			return false;
		}
		if (!CanonicalPaths.Contains(ResolvedEntry.RelativePath.ToLower()))
		{
			SetFailure(OutResult, WriterFailure(TEXT("export_workspace_entry_missing"),
				TEXT("The export entry file is absent from the workspace plan."), TEXT("/entry_file"),
				MakeShared<FJsonValueString>(Policy.EntryFile), MakeShared<FJsonValueString>(TEXT("a file included in the workspace plan"))));
			return false;
		}
		OutResult.EntryFile = ResolvedEntry.RelativePath;

		if (bHasCharacterMetrics)
		{
			OutResult.Metrics.TokenProxy = (OutResult.Metrics.CharacterCount + 3) / 4;
		}
		OutResult.ModelMetrics = OutResult.Metrics;
		OutResult.Status = TEXT("planned");
		OutResult.Diagnostic.Reset();
		return true;
	}

	bool PublishWorkspacePlan(FWorkspaceResult& InOutResult)
	{
		if (!InOutResult.IsPlanned())
		{
			if (!InOutResult.Diagnostic.IsEmpty())
			{
				InOutResult.Status = TEXT("failed");
				return false;
			}
			SetFailure(InOutResult, WriterFailure(TEXT("export_publish_plan_invalid"),
				TEXT("Only a successfully planned export workspace can be published."), TEXT("/status"),
				MakeShared<FJsonValueString>(InOutResult.Status), MakeShared<FJsonValueString>(TEXT("planned"))));
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
		UE::UAI::Contract::FDiagnostic PublishError;
		if (!UeAgentExportPublisher::PublishWorkspace(
			InOutResult.WorkspaceRoot,
			PublishFiles,
			PublishContext,
			PublishResult,
			PublishError))
		{
			SetFailure(InOutResult, PublishError);
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
			FDiagnostic Error = WriterFailure(TEXT("export_single_file_contract_invalid"),
				TEXT("A single-file export requires exactly one planned output file."), TEXT("/files"),
				MakeShared<FJsonValueNumber>(OutResult.Files.Num()), MakeShared<FJsonValueNumber>(1));
			Error.InternalContext = Contract.ContractId;
			SetFailure(OutResult, Error);
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
		UE::UAI::Contract::FDiagnostic PublishError;
		if (!UeAgentExportPublisher::PublishSingleFile(
			ResolvedFile,
			PublishFile,
			PublishContext,
			PublishResult,
			PublishError))
		{
			SetFailure(OutResult, PublishError, File.FileId);
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
