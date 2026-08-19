// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/UeAgentSpaceContract.h"

#include "Dom/JsonObject.h"

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
			FString& OutError)
		{
			if (!Contract->HasField(Field))
			{
				OutError = FString::Printf(TEXT("%s_field_missing:%s"), *ErrorPrefix, Field);
				return false;
			}
			if (!Contract->TryGetStringField(Field, OutValue) || OutValue.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s_field_invalid:%s"), *ErrorPrefix, Field);
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
		FString& OutError)
	{
		OutError.Reset();
		if (!Contract.IsValid())
		{
			OutError = ErrorPrefix + TEXT("_missing");
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Contract->Values)
		{
			if (!AllowedFields.Contains(Field.Key))
			{
				OutError = FString::Printf(TEXT("%s_field_unknown:%s"), *ErrorPrefix, *Field.Key);
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
			OutError = ErrorPrefix + TEXT("_subject_mismatch");
			return false;
		}
		if (Dimension != TEXT("3d"))
		{
			OutError = ErrorPrefix + TEXT("_dimension_mismatch");
			return false;
		}
		if (WriteFrame != ExpectedFrame || ReadFrame != ExpectedFrame || VerifyFrame != ExpectedFrame)
		{
			OutError = ErrorPrefix + TEXT("_frame_mismatch");
			return false;
		}
		return true;
	}
}
