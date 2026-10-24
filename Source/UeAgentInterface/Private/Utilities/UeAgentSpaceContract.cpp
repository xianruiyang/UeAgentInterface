// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/UeAgentSpaceContract.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentSpaceContract
{
	namespace
	{
		const TSet<FString> AllowedFields = {
			TEXT("subject"),
			TEXT("dimension"),
			TEXT("Fsemantic"),
			TEXT("Fwrite"),
			TEXT("Fread"),
			TEXT("Fverify"),
			TEXT("conversion_chain"),
		};

		bool ReadRequiredString(
			const TSharedPtr<const FJsonObject>& Contract,
			const TCHAR* Field,
			const FString& ErrorPrefix,
			FString& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Contract->HasField(Field))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(ErrorPrefix + TEXT("_field_missing"), FString::Printf(TEXT("Space contract field '%s' is missing."), Field), TEXT("space_contract"));
                OutError.PropertyPath = Field;
				OutError.Source.JsonPath = TEXT("/") + FString(Field);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty string"));
				OutError.ActualValuePresent = false;
				return false;
			}
			if (!Contract->TryGetStringField(Field, OutValue) || OutValue.TrimStartAndEnd().IsEmpty())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(ErrorPrefix + TEXT("_field_invalid"), FString::Printf(TEXT("Space contract field '%s' is invalid."), Field), TEXT("space_contract"));
                OutError.PropertyPath = Field;
				OutError.Source.JsonPath = TEXT("/") + FString(Field);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty string"));
				OutError.ActualValue = Contract->Values.FindRef(Field);
				return false;
			}
			return true;
		}
	}

	TSharedRef<FJsonObject> Make3D(
		const FString& Subject,
		const FString& Frame,
		const FString& SemanticFrame,
		const FString& ConversionChain)
	{
		TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
		Contract->SetStringField(TEXT("subject"), Subject);
		Contract->SetStringField(TEXT("dimension"), TEXT("3d"));
		Contract->SetStringField(TEXT("Fsemantic"), SemanticFrame);
		Contract->SetStringField(TEXT("Fwrite"), Frame);
		Contract->SetStringField(TEXT("Fread"), Frame);
		Contract->SetStringField(TEXT("Fverify"), Frame);
		Contract->SetStringField(TEXT("conversion_chain"), ConversionChain);
		return Contract;
	}

	bool Validate3D(
		const TSharedPtr<const FJsonObject>& Contract,
		const FString& ExpectedSubject,
		const FString& ExpectedFrame,
		const FString& ErrorPrefix,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Contract.IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(ErrorPrefix + TEXT("_missing"), TEXT("The space contract object is missing."), TEXT("space_contract"));
			OutError.CandidateName = ExpectedSubject;
			OutError.ActualValuePresent = false;
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Contract->Values)
		{
			if (!AllowedFields.Contains(Field.Key))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(ErrorPrefix + TEXT("_field_unknown"), FString::Printf(TEXT("Space contract field '%s' is unknown."), *Field.Key), TEXT("space_contract"));
                OutError.PropertyPath = Field.Key;
				return false;
			}
		}

		FString Subject;
		FString Dimension;
		FString SemanticFrame;
		FString WriteFrame;
		FString ReadFrame;
		FString VerifyFrame;
		FString ConversionChain;
		if (!ReadRequiredString(Contract, TEXT("subject"), ErrorPrefix, Subject, OutError)
			|| !ReadRequiredString(Contract, TEXT("dimension"), ErrorPrefix, Dimension, OutError)
			|| !ReadRequiredString(Contract, TEXT("Fsemantic"), ErrorPrefix, SemanticFrame, OutError)
			|| !ReadRequiredString(Contract, TEXT("Fwrite"), ErrorPrefix, WriteFrame, OutError)
			|| !ReadRequiredString(Contract, TEXT("Fread"), ErrorPrefix, ReadFrame, OutError)
			|| !ReadRequiredString(Contract, TEXT("Fverify"), ErrorPrefix, VerifyFrame, OutError)
			|| !ReadRequiredString(Contract, TEXT("conversion_chain"), ErrorPrefix, ConversionChain, OutError))
		{
			return false;
		}

		if (Subject != ExpectedSubject)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(ErrorPrefix + TEXT("_subject_mismatch"), TEXT("The space contract names a different subject."), TEXT("space_contract"));
			OutError.PropertyPath = TEXT("subject");
			OutError.Source.JsonPath = TEXT("/subject");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedSubject);
			OutError.ActualValue = MakeShared<FJsonValueString>(Subject);
			return false;
		}
		if (Dimension != TEXT("3d"))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(ErrorPrefix + TEXT("_dimension_mismatch"), TEXT("The space contract must use three dimensions."), TEXT("space_contract"));
			OutError.PropertyPath = TEXT("dimension");
			OutError.Source.JsonPath = TEXT("/dimension");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("3d"));
			OutError.ActualValue = MakeShared<FJsonValueString>(Dimension);
			return false;
		}
		if (WriteFrame != ExpectedFrame || ReadFrame != ExpectedFrame || VerifyFrame != ExpectedFrame)
		{
			const TCHAR* Field = WriteFrame != ExpectedFrame ? TEXT("Fwrite") : ReadFrame != ExpectedFrame ? TEXT("Fread") : TEXT("Fverify");
			OutError = UE::UAI::Contract::FDiagnostic::Failure(ErrorPrefix + TEXT("_frame_mismatch"), TEXT("The space contract frame differs from the required frame."), TEXT("space_contract"));
			OutError.PropertyPath = Field;
			OutError.Source.JsonPath = TEXT("/") + FString(Field);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedFrame);
			OutError.ActualValue = Contract->Values.FindRef(Field);
			return false;
		}
		return true;
	}
}
