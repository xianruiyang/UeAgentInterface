#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include <initializer_list>

namespace UE::UAI::DomainAdapters::PieRuntimeDebug::PieDiagnostics
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

	inline TSharedPtr<FJsonValue> CanonicalValueFact(const StateSemantic::FUeAgentCanonicalValue& Value)
	{
		using StateSemantic::EUeAgentValueKind;
		if (Value.GetKind() == EUeAgentValueKind::String) return MakeShared<FJsonValueString>(Value.GetText());
		if (Value.GetKind() == EUeAgentValueKind::Boolean) return MakeShared<FJsonValueBoolean>(Value.GetBoolean());
		if (Value.GetKind() == EUeAgentValueKind::Null) return MakeShared<FJsonValueNull>();
		const TCHAR* Kind = TEXT("unknown");
		switch (Value.GetKind())
		{
		case EUeAgentValueKind::Integer: Kind = TEXT("integer"); break;
		case EUeAgentValueKind::Decimal: Kind = TEXT("decimal"); break;
		case EUeAgentValueKind::Array: Kind = TEXT("array"); break;
		case EUeAgentValueKind::Object: Kind = TEXT("object"); break;
		case EUeAgentValueKind::Temporal: Kind = TEXT("temporal"); break;
		case EUeAgentValueKind::Spatial: Kind = TEXT("spatial"); break;
		default: break;
		}
		const auto Fact = MakeShared<FJsonObject>();
		Fact->SetStringField(TEXT("kind"), Kind);
		if (!Value.GetLexeme().IsEmpty()) Fact->SetStringField(TEXT("lexeme"), Value.GetLexeme());
		if (Value.GetKind() == EUeAgentValueKind::Array) Fact->SetNumberField(TEXT("length"), Value.GetArrayItems().Num());
		return MakeShared<FJsonValueObject>(Fact);
	}

	inline TSharedPtr<FJsonValue> CanonicalValueFact(const StateSemantic::FUeAgentCanonicalValueRef* Value)
	{
		return Value ? CanonicalValueFact(Value->Get()) : nullptr;
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


	// The caller owns the argument layout; lower diagnostics keep their facts and causes.
	inline void BindArgumentInput(Contract::FDiagnostic& Error, const FString& Prefix, bool bDescriptorField = false)
	{
		if (!Error.SourceKind.StartsWith(TEXT("native_")) && Error.Source.SourceFile.IsEmpty())
		{
			const FString Relative = bDescriptorField
				? (Error.PropertyPath.IsEmpty() ? FString() : TEXT("/") + Error.PropertyPath.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")))
				: !Error.Source.InternalJsonPath.IsEmpty() ? Error.Source.InternalJsonPath
				: !Error.Source.JsonPath.IsEmpty() ? Error.Source.JsonPath : Error.SemanticPointer;
			const FString Pointer = Prefix + (Relative.IsEmpty() || Relative == TEXT("/") ? FString() : Relative);
			Error.SourceKind = TEXT("command_input");
			Error.Source.bCommandInput = true;
			Error.Source.JsonPath = Pointer.IsEmpty() ? TEXT("/") : Pointer;
			Error.SemanticPointer = Error.Source.JsonPath;
		}
		for (auto& Child : Error.Diagnostics) BindArgumentInput(Child, Prefix, bDescriptorField);
		if (Error.PublicCause)
		{
			auto Cause = MakeShared<Contract::FDiagnostic>(*Error.PublicCause);
			BindArgumentInput(*Cause, Prefix, bDescriptorField);
			Error.PublicCause = MoveTemp(Cause);
		}
	}

	inline void RemapArgumentInput(Contract::FDiagnostic& Error, const TMap<FString, FString>& Sources)
	{
		if (Error.SourceKind == TEXT("command_input") && Error.Source.SourceFile.IsEmpty())
		{
			const FString Pointer = Error.SemanticPointer.IsEmpty() ? Error.Source.JsonPath : Error.SemanticPointer;
			const TPair<FString, FString>* Match = nullptr;
			for (const auto& Source : Sources)
				if ((Pointer == Source.Key || Pointer.StartsWith(Source.Key + TEXT("/")))
					&& (!Match || Source.Key.Len() > Match->Key.Len())) Match = &Source;
			if (Match)
			{
				Error.Source.JsonPath = Match->Value + Pointer.Mid(Match->Key.Len());
				Error.SemanticPointer = Error.Source.JsonPath;
			}
		}
		for (auto& Child : Error.Diagnostics) RemapArgumentInput(Child, Sources);
		if (Error.PublicCause)
		{
			auto Cause = MakeShared<Contract::FDiagnostic>(*Error.PublicCause);
			RemapArgumentInput(*Cause, Sources);
			Error.PublicCause = MoveTemp(Cause);
		}
	}

	inline void MarkNativeSource(Contract::FDiagnostic& Error)
	{
		Error.SourceKind = TEXT("native_result");
		static_cast<Contract::FPublicSourceLocation&>(Error.Source) = {};
		Error.Source.bCommandInput = false;
		Error.SemanticPointer.Reset();
		for (auto& Child : Error.Diagnostics) MarkNativeSource(Child);
		if (Error.PublicCause)
		{
			auto Cause = MakeShared<Contract::FDiagnostic>(*Error.PublicCause);
			MarkNativeSource(*Cause);
			Error.PublicCause = MoveTemp(Cause);
		}
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

}
