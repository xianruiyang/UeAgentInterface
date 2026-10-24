#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include <initializer_list>

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob::CaptureDiagnostics
{
	inline TSharedPtr<FJsonValue> Facts(std::initializer_list<TPair<FString, TSharedPtr<FJsonValue>>> Fields)
	{
		const auto Object = MakeShared<FJsonObject>();
		for (const auto& Field : Fields)
			if (Field.Value.IsValid()) Object->SetField(Field.Key, Field.Value);
		return MakeShared<FJsonValueObject>(Object);
	}

	inline TSharedPtr<FJsonValue> Number(double Value)
	{
		if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
		return MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value));
	}

	inline TSharedPtr<FJsonValue> Size(const FIntPoint& Value)
	{
		return Facts({ { TEXT("width"), Number(Value.X) }, { TEXT("height"), Number(Value.Y) } });
	}

	inline Contract::FDiagnostic Native(const FString& Code, const FString& Reason,
		const FString& Stage, const FString& Field, const TSharedPtr<FJsonValue>& Actual,
		const TSharedPtr<FJsonValue>& Expected)
	{
		auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, Stage, Reason);
		Error.SourceKind = TEXT("native_result");
		Error.bRetrySafe = false;
		Error.Source = {};
		Error.SemanticPointer.Reset();
		Error.ActualValuePresent = Actual.IsValid();
		Error.ExpectedValuePresent = Expected.IsValid();
		return Error;
	}

	inline Contract::FDiagnostic Input(const FString& Code, const FString& Reason,
		const FString& Stage, const FString& Pointer, const TSharedPtr<FJsonValue>& Actual,
		const TSharedPtr<FJsonValue>& Expected)
	{
		auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer, Expected, Actual, Stage, Reason);
		Error.SourceKind = TEXT("command_input");
		Error.bRetrySafe = false;
		Error.Source.bCommandInput = true;
		Error.ActualValuePresent = Actual.IsValid();
		Error.ExpectedValuePresent = Expected.IsValid();
		return Error;
	}

	// This supplements an already-produced parameter error; causes and facts remain intact.
	inline void LocateInput(Contract::FDiagnostic& Error, const FString& Pointer)
	{
		if (Error.SourceKind != TEXT("native_result") && Error.Source.SourceFile.IsEmpty()
			&& Error.Source.JsonPath.IsEmpty() && Error.SemanticPointer.IsEmpty())
		{
			Error.SourceKind = TEXT("command_input");
			Error.Source.bCommandInput = true;
			Error.Source.JsonPath = Pointer;
			Error.SemanticPointer = Pointer;
		}
	}
}
