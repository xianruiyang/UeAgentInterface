#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphToken.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"

namespace UeAgentAuthoringDiagnostics
{
	namespace
	{
		thread_local FScopedWarnings* ActiveWarnings = nullptr;
		thread_local FScopedSourceLocations* ActiveSources = nullptr;
		void MarkWarning(FDiagnostic& Diagnostic)
		{
			Diagnostic.Severity = ESeverity::Warning;
			for (auto& Child : Diagnostic.Diagnostics) MarkWarning(Child);
		}
	}

	FScopedSourceLocations::FScopedSourceLocations(const FJsonObject* InRoot,
		TFunction<FSourceLocation(const FString&)> InResolve)
		: FScopedSourceLocations(InRoot, MoveTemp(InResolve), false) {}
	FScopedSourceLocations::FScopedSourceLocations(const FJsonObject* InRoot,
		TFunction<FSourceLocation(const FString&)> InResolve, bool bInCommandInput)
		: Parent(ActiveSources), Root(InRoot), bCommandInput(bInCommandInput), Resolve(MoveTemp(InResolve))
	{ ActiveSources = this; }
	FScopedSourceLocations::~FScopedSourceLocations() { ActiveSources = Parent; }
	bool FScopedSourceLocations::Locate(const FJsonObject& Object, const FString& Field, FSourceLocation& Out,
		const FString& RelativePointer)
	{
		const auto Token = [](const FString& Value) { return Value.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); };
		// Walk only on failure, not for each successful preflight. Match the actual
		// parsed object, never a guessed alias or a string extracted from an error.
		for (auto* Scope = ActiveSources; Scope; Scope = Scope->Parent)
		{
			if (!Scope->Root) continue;
			struct FPendingSource
			{
				TSharedPtr<FJsonValue> Value;
				FString Pointer;
				const FJsonObject* SourceObject;
				FString SourcePointer;
			};
			const auto HasSource = [](const FJsonObject& Candidate)
			{
				const TSharedPtr<FJsonObject>* Source = nullptr;
				return Candidate.TryGetObjectField(TEXT("authoring_source"), Source) && Source && Source->IsValid();
			};
			TArray<FPendingSource> Pending;
			FString Found;
			const FJsonObject* SourceObject = HasSource(*Scope->Root) ? Scope->Root : nullptr;
			FString SourcePointer;
			bool bFound = Scope->Root == &Object;
			if (!bFound)
				for (const auto& Child : Scope->Root->Values)
					if (Child.Key != TEXT("authoring_source"))
						Pending.Add({Child.Value, TEXT("/") + Token(Child.Key), SourceObject, FString()});
			while (!bFound && !Pending.IsEmpty())
			{
				const auto Item = Pending.Pop(EAllowShrinking::No);
				if (!Item.Value) continue;
				if (Item.Value->Type == EJson::Object)
				{
					const auto& ChildObject = Item.Value->AsObject();
					if (!ChildObject) continue;
					const FJsonObject* NearestSource = HasSource(*ChildObject) ? ChildObject.Get() : Item.SourceObject;
					const FString NearestPointer = NearestSource == ChildObject.Get() ? Item.Pointer : Item.SourcePointer;
					if (ChildObject.Get() == &Object)
					{
						Found = Item.Pointer;
						SourceObject = NearestSource;
						SourcePointer = NearestPointer;
						bFound = true;
						break;
					}
					for (const auto& Child : ChildObject->Values)
						if (Child.Key != TEXT("authoring_source"))
							Pending.Add({Child.Value, Item.Pointer + TEXT("/") + Token(Child.Key), NearestSource, NearestPointer});
				}
				else if (Item.Value->Type == EJson::Array)
					for (int32 Index = 0; Index < Item.Value->AsArray().Num(); ++Index)
						Pending.Add({Item.Value->AsArray()[Index], Item.Pointer + TEXT("/") + LexToString(Index), Item.SourceObject, Item.SourcePointer});
			}
			if (!bFound) continue;
			if (!RelativePointer.IsEmpty() && RelativePointer != TEXT("/")) Found += RelativePointer;
			else if (!Field.IsEmpty()) Found += TEXT("/") + Token(Field);
			if (SourceObject)
			{
				// Generated children (for example graph endpoints) inherit their
				// closest explicit provenance before an inline adapter can claim them.
				FDiagnostic Projected;
				Projected.Source.InternalJsonPath = Found.Mid(SourcePointer.Len());
				FString SourceField = Projected.Source.InternalJsonPath;
				SourceField.RemoveFromStart(TEXT("/"));
				int32 Slash = INDEX_NONE;
				if (SourceField.FindChar(TEXT('/'), Slash)) SourceField.LeftInline(Slash);
				SourceField.ReplaceInline(TEXT("~1"), TEXT("/"));
				SourceField.ReplaceInline(TEXT("~0"), TEXT("~"));
				LocateAuthoringSource(Projected, *SourceObject, SourceField);
				if (!Projected.Source.SourceFile.IsEmpty())
				{
					Out = MoveTemp(Projected.Source);
					return true;
				}
			}
			FSourceLocation Resolved = Scope->Resolve(Found);
			Resolved.bCommandInput |= Scope->bCommandInput;
			if (Resolved.SourceFile.IsEmpty() && !Resolved.bCommandInput) continue;
			Out = Resolved;
			return true;
		}
		return false;
	}

	TMap<FString, FSourceLocation> FScopedSourceLocations::CaptureProjection(const FJsonObject& Object)
	{
		struct FOrigin { FScopedSourceLocations* Scope; FString Pointer; };
		TMap<const FJsonValue*, FOrigin> Values;
		TMap<const FJsonObject*, FOrigin> Objects;
		const auto Token = [](const FString& Value) { return Value.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); };
		for (auto* Scope = ActiveSources; Scope; Scope = Scope->Parent)
		{
			if (!Scope->Root) continue;
			if (!Objects.Contains(Scope->Root)) Objects.Add(Scope->Root, {Scope, FString()});
			TArray<TPair<TSharedPtr<FJsonValue>, FString>> Pending;
			for (const auto& Field : Scope->Root->Values) Pending.Emplace(Field.Value, TEXT("/") + Token(Field.Key));
			while (!Pending.IsEmpty())
			{
				const auto Item = Pending.Pop(EAllowShrinking::No);
				if (!Item.Key) continue;
				if (!Values.Contains(Item.Key.Get())) Values.Add(Item.Key.Get(), {Scope, Item.Value});
				if (Item.Key->Type == EJson::Object && Item.Key->AsObject())
				{
					const auto& Child = Item.Key->AsObject();
					if (!Objects.Contains(Child.Get())) Objects.Add(Child.Get(), {Scope, Item.Value});
					for (const auto& Field : Child->Values) Pending.Emplace(Field.Value, Item.Value + TEXT("/") + Token(Field.Key));
				}
				else if (Item.Key->Type == EJson::Array)
					for (int32 Index = 0; Index < Item.Key->AsArray().Num(); ++Index)
						Pending.Emplace(Item.Key->AsArray()[Index], Item.Value + TEXT("/") + LexToString(Index));
			}
		}
		TMap<FString, FSourceLocation> Result;
		auto Add = [&Result](const FString& Pointer, const FOrigin* Origin)
		{
			if (!Origin) return;
			auto Source = Origin->Scope->Resolve(Origin->Pointer);
			Source.bCommandInput |= Origin->Scope->bCommandInput;
			if (!Source.SourceFile.IsEmpty() || Source.bCommandInput)
				Result.Add(Pointer.IsEmpty() ? TEXT("/") : Pointer, MoveTemp(Source));
		};
		Add(TEXT("/"), Objects.Find(&Object));
		TArray<TPair<TSharedPtr<FJsonValue>, FString>> Pending;
		for (const auto& Field : Object.Values) Pending.Emplace(Field.Value, TEXT("/") + Token(Field.Key));
		while (!Pending.IsEmpty())
		{
			const auto Item = Pending.Pop(EAllowShrinking::No);
			if (!Item.Key) continue;
			const FOrigin* Origin = Values.Find(Item.Key.Get());
			if (Item.Key->Type == EJson::Object && Item.Key->AsObject())
			{
				if (!Origin) Origin = Objects.Find(Item.Key->AsObject().Get());
				for (const auto& Field : Item.Key->AsObject()->Values) Pending.Emplace(Field.Value, Item.Value + TEXT("/") + Token(Field.Key));
			}
			else if (Item.Key->Type == EJson::Array)
				for (int32 Index = 0; Index < Item.Key->AsArray().Num(); ++Index)
					Pending.Emplace(Item.Key->AsArray()[Index], Item.Value + TEXT("/") + LexToString(Index));
			Add(Item.Value, Origin);
		}
		return Result;
	}

	FSourceLocation FScopedSourceLocations::ResolveProjection(
		const TMap<FString, FSourceLocation>& Sources, FString Pointer)
	{
		if (Pointer.IsEmpty()) Pointer = TEXT("/");
		for (;;)
		{
			if (const auto* Source = Sources.Find(Pointer)) return *Source;
			if (Pointer == TEXT("/")) return {};
			int32 Slash;
			if (!Pointer.FindLastChar(TEXT('/'), Slash)) return {};
			Pointer = Slash == 0 ? TEXT("/") : Pointer.Left(Slash);
		}
	}

	FScopedWarnings::FScopedWarnings(bool bForwardToParent)
		: Parent(ActiveWarnings), bForward(bForwardToParent) { ActiveWarnings = this; }
	FScopedWarnings::~FScopedWarnings() { ActiveWarnings = Parent; }
	void FScopedWarnings::Add(const FDiagnostic& Diagnostic)
	{
		if (Parent && bForward) { Parent->Add(Diagnostic); return; }
		auto Warning = Diagnostic;
		MarkWarning(Warning);
		Warnings.Add(MoveTemp(Warning));
	}
	FString FScopedWarnings::AppendToPayload(const FString& Payload) const
	{
		if (Warnings.IsEmpty()) return Payload;
		TSharedPtr<FJsonObject> Object;
		if (Payload.IsEmpty()) Object = MakeShared<FJsonObject>();
		else if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Payload), Object)
			|| !Object.IsValid()) return Payload; // The protocol reports malformed payloads.
		TArray<TSharedPtr<FJsonValue>> Combined;
		const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
		if (Object->TryGetArrayField(TEXT("warnings"), Existing)) Combined = *Existing;
		Combined.Append(UE::UAI::Contract::ToPublicDiagnostics(Warnings, true));
		Object->SetArrayField(TEXT("warnings"), Combined);
		FString Result;
		FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Result));
		return Result;
	}
	void ReportConsistencyWarning(const FDiagnostic& Diagnostic)
	{
		FDiagnostic Warning = Diagnostic;
		Warning.Severity = ESeverity::Warning;
		if (ActiveWarnings) ActiveWarnings->Add(Warning);
		else UE_LOG(LogTemp, Warning, TEXT("UAI consistency observation: %s"), *Warning.ToPublicDetail());
	}
	bool HasConsistencyWarnings() { return ActiveWarnings && ActiveWarnings->HasWarnings(); }
	void ReportConsistencyWarning(const FString& FallbackCode, FDiagnostic Diagnostic, const FString& Stage)
	{
		if (Diagnostic.IsEmpty())
		{
			Diagnostic = FDiagnostic::Failure(FallbackCode.IsEmpty() ? TEXT("consistency_diagnostic_missing") : FallbackCode,
				TEXT("The consistency check failed without reporting a diagnostic."), Stage);
			Diagnostic.SourceKind = TEXT("native_result");
		}
		if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = Stage;
		ReportConsistencyWarning(Diagnostic);
	}
	void LocateFailure(FDiagnostic& Diagnostic, const FJsonObject& Object,
		const FString& Stage, const FString& TargetPath, const FString& Field)
	{
		if (Diagnostic.IsEmpty()) Diagnostic = FDiagnostic::Failure(TEXT("authoring_diagnostic_missing"),
			TEXT("The failed authoring operation did not report its reason."), Stage);
		if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = Stage;
		if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = TargetPath;
		LocateAuthoringSource(Diagnostic, Object, Field);
	}

	void PrefixDocumentFailure(FDiagnostic& Diagnostic, const FString& Prefix, bool bAppendPropertyPath, bool bRelativeCommandInput)
	{
		auto PrefixPointer = [&Prefix, bAppendPropertyPath](const FString& Pointer, const FString& Property)
		{
			if (bAppendPropertyPath && !Property.IsEmpty())
				return Prefix + TEXT("/") + Property.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			return Prefix + (Pointer.IsEmpty() || Pointer == TEXT("/") ? FString() : Pointer);
		};
		if (Diagnostic.SourceKind != TEXT("native_result")
			&& (bRelativeCommandInput || Diagnostic.SourceKind != TEXT("command_input"))
			&& (Diagnostic.Source.SourceFile.IsEmpty() || Diagnostic.Source.SourceFile.StartsWith(TEXT("inline://"))))
		{
			Diagnostic.Source.JsonPath = PrefixPointer(Diagnostic.Source.JsonPath, Diagnostic.PropertyPath);
			if (!Diagnostic.Source.InternalJsonPath.IsEmpty())
				Diagnostic.Source.InternalJsonPath = PrefixPointer(Diagnostic.Source.InternalJsonPath, Diagnostic.PropertyPath);
		}
		if (Diagnostic.SourceKind != TEXT("native_result") && !Diagnostic.SemanticPointer.IsEmpty())
			Diagnostic.SemanticPointer = PrefixPointer(Diagnostic.SemanticPointer, FString());
		for (auto& Child : Diagnostic.Diagnostics) PrefixDocumentFailure(Child, Prefix, bAppendPropertyPath, bRelativeCommandInput);
		if (Diagnostic.PublicCause)
		{
			auto Cause = MakeShared<FDiagnostic>(*Diagnostic.PublicCause);
			PrefixDocumentFailure(*Cause, Prefix, bAppendPropertyPath, bRelativeCommandInput);
			Diagnostic.PublicCause = MoveTemp(Cause);
		}
	}

	void PrefixInternalDocumentFailure(FDiagnostic& Diagnostic, const FString& Prefix)
	{
		if (Diagnostic.SourceKind != TEXT("native_result") && Diagnostic.Source.SourceFile.IsEmpty())
		{
			const FString Pointer = !Diagnostic.Source.InternalJsonPath.IsEmpty() ? Diagnostic.Source.InternalJsonPath
				: !Diagnostic.SemanticPointer.IsEmpty() ? Diagnostic.SemanticPointer : Diagnostic.Source.JsonPath;
			Diagnostic.Source.InternalJsonPath = Prefix + (Pointer.IsEmpty() || Pointer == TEXT("/") ? FString() : Pointer);
		}
		for (auto& Child : Diagnostic.Diagnostics) PrefixInternalDocumentFailure(Child, Prefix);
		if (Diagnostic.PublicCause)
		{
			auto Cause = MakeShared<FDiagnostic>(*Diagnostic.PublicCause);
			PrefixInternalDocumentFailure(*Cause, Prefix);
			Diagnostic.PublicCause = MoveTemp(Cause);
		}
	}

	void SetRollbackFacts(FDiagnostic& Diagnostic, bool bSucceeded)
	{
		Diagnostic.RollbackAttempted = true;
		Diagnostic.RollbackSucceeded = bSucceeded;
		if (!bSucceeded) Diagnostic.bRetrySafe = false;
	}



	void MarkUnchecked(FDiagnostic& Diagnostic, const FString& InStage, const FJsonObject& Object, const FString& Reason)
	{
		FDiagnostic Location;
		UeAgentAuthoringDiagnostics::LocateAuthoringSource(Location, Object);
		auto Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("stage"), InStage);
		Item->SetStringField(TEXT("reason"), Reason);
		if (!Location.Source.SourceFile.IsEmpty()) Item->SetStringField(TEXT("source_file"), Location.Source.SourceFile);
		if (!Location.Source.JsonPath.IsEmpty()) Item->SetStringField(TEXT("json_pointer"), Location.Source.JsonPath);
		if (Location.Source.Line > 0) Item->SetNumberField(TEXT("line"), Location.Source.Line);
		Diagnostic.Unchecked.Add(MakeShared<FJsonValueObject>(Item));
	}





	void LocatePropertyValueSource(FDiagnostic& Diagnostic, const FJsonObject& Descriptor)
	{
		for (auto& Child : Diagnostic.Diagnostics) LocatePropertyValueSource(Child, Descriptor);
		if (Diagnostic.PublicCause)
		{
			auto Cause = MakeShared<FDiagnostic>(*Diagnostic.PublicCause);
			LocatePropertyValueSource(*Cause, Descriptor);
			Diagnostic.PublicCause = MoveTemp(Cause);
		}
		auto& Source = Diagnostic.Source;
		const TSharedPtr<FJsonObject>* Location = nullptr;
		if (!Descriptor.TryGetObjectField(TEXT("authoring_source"), Location) || !Location || !Location->IsValid()) return;
		if (Diagnostic.TargetPath.IsEmpty()) (*Location)->TryGetStringField(TEXT("target"), Diagnostic.TargetPath);
		if (!Source.SourceFile.IsEmpty() || Source.bCommandInput || Diagnostic.SourceKind == TEXT("native_result")
			|| Diagnostic.SourceKind == TEXT("command_input")) return;
		const FString RelativePointer = !Source.InternalJsonPath.IsEmpty() ? Source.InternalJsonPath
			: Diagnostic.SemanticPointer.IsEmpty() ? Source.JsonPath : Diagnostic.SemanticPointer;
		(*Location)->TryGetStringField(TEXT("source_file"), Source.SourceFile);
		(*Location)->TryGetStringField(TEXT("json_pointer"), Source.JsonPath);
		(*Location)->TryGetNumberField(TEXT("line"), Source.Line);
		(*Location)->TryGetNumberField(TEXT("column"), Source.Column);
		FString ValuePointer;
		if ((*Location)->TryGetStringField(TEXT("source_unit"), Source.SourceUnit)
			&& (*Location)->TryGetStringField(TEXT("value_internal_json_path"), ValuePointer))
			Source.InternalJsonPath = ValuePointer + (RelativePointer == TEXT("/") ? FString() : RelativePointer);
	}

	void LocateAuthoringSource(FDiagnostic& Diagnostic, const FJsonObject& Object, const FString& Field)
	{
		auto& Source = Diagnostic.Source;
		auto& PropertyPath = Diagnostic.PropertyPath;
		auto& Diagnostics = Diagnostic.Diagnostics;
		// Children retain their individual causes and any more precise provenance.
		for (auto& Child : Diagnostics)
            LocateAuthoringSource(Child, Object, Field);
		if (Diagnostic.PublicCause)
		{
			auto Cause = MakeShared<FDiagnostic>(*Diagnostic.PublicCause);
			LocateAuthoringSource(*Cause, Object, Field);
			Diagnostic.PublicCause = MoveTemp(Cause);
		}
		// Preserve the producer's file binding, including a containing edit. A later
		// adapter may operate on an inline document; only the workspace source map
		// may refine this binding through SourceUnit/InternalJsonPath.
		if (!Source.SourceFile.IsEmpty() || Diagnostic.SourceKind == TEXT("native_result")
			|| Diagnostic.SourceKind == TEXT("command_input")) return;
		const bool bInternalPointer = !Source.InternalJsonPath.IsEmpty();
		const FString RelativePointer = bInternalPointer ? Source.InternalJsonPath
			: Diagnostic.SemanticPointer.IsEmpty() ? Source.JsonPath : Diagnostic.SemanticPointer;
		const FString FieldPointer = TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		const bool bRelativeToObject = RelativePointer.StartsWith(TEXT("/")) && (Field.IsEmpty()
			|| RelativePointer == FieldPointer || RelativePointer.StartsWith(FieldPointer + TEXT("/")));
		const TSharedPtr<FJsonObject>* Location = nullptr;
		if (!Object.TryGetObjectField(TEXT("authoring_source"), Location)
			|| !Location || !Location->IsValid())
		{
			if (FScopedSourceLocations::Locate(Object, Field, Source, bRelativeToObject ? RelativePointer : FString()))
			{
				Diagnostic.SourceKind = Source.SourceFile.IsEmpty() ? TEXT("command_input")
					: Source.InternalJsonPath == Source.JsonPath ? TEXT("authored_value") : TEXT("related_input");
				if (!bInternalPointer && Diagnostic.SemanticPointer.IsEmpty() && bRelativeToObject) Diagnostic.SemanticPointer = RelativePointer;
				if (!bInternalPointer) PropertyPath.Reset();
			}
			return;
		}
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		const TSharedPtr<FJsonObject>* FieldLocation = nullptr;
		const FJsonObject* Selected = Location->Get();
		if (!Field.IsEmpty() && (*Location)->TryGetObjectField(TEXT("fields"), Fields)
			&& Fields && Fields->IsValid() && (*Fields)->TryGetObjectField(Field, FieldLocation)
			&& FieldLocation && FieldLocation->IsValid()) Selected = FieldLocation->Get();
		Selected->TryGetStringField(TEXT("source_file"), Source.SourceFile);
		Selected->TryGetStringField(TEXT("json_pointer"), Source.JsonPath);
		Selected->TryGetNumberField(TEXT("line"), Source.Line);
		Selected->TryGetNumberField(TEXT("column"), Source.Column);
		// The descriptor identifies the parsed object; the producer identifies the
		// failing field within it. Keep that lookup separate from the physical
		// container location until the workspace resolves the actual HJSON leaf.
		bool bMappedField = false;
		if (bRelativeToObject || !Field.IsEmpty())
		{
			FString UnitId, ObjectPointer;
			// An explicitly projected field can have a different semantic name or
			// nesting. Its source coordinate owns the suffix below that field.
			if (Selected != Location->Get() && Selected->TryGetStringField(TEXT("source_unit"), UnitId)
				&& Selected->TryGetStringField(TEXT("internal_json_path"), ObjectPointer))
			{
				Source.SourceUnit = UnitId;
				Source.InternalJsonPath = ObjectPointer + (bRelativeToObject ? RelativePointer.Mid(FieldPointer.Len()) : FString());
				bMappedField = true;
			}
			else if (bRelativeToObject && (*Location)->TryGetStringField(TEXT("source_unit"), UnitId)
				&& (*Location)->TryGetStringField(TEXT("internal_json_path"), ObjectPointer))
			{
				Source.SourceUnit = UnitId;
				Source.InternalJsonPath = ObjectPointer + (RelativePointer == TEXT("/") ? FString() : RelativePointer);
			}
		}
		if (!Source.SourceFile.IsEmpty())
		{
			const bool bNested = bRelativeToObject && RelativePointer != (Field.IsEmpty() ? TEXT("/") : FieldPointer);
Diagnostic.SourceKind = bMappedField || bNested || (!Field.IsEmpty() && Selected == Location->Get()) ? TEXT("related_input") : TEXT("authored_value");
			if (!bInternalPointer && Diagnostic.SemanticPointer.IsEmpty() && bRelativeToObject) Diagnostic.SemanticPointer = RelativePointer;
			if (!bInternalPointer) PropertyPath.Reset();
		}
	}







	FDiagnostic MakeValueFailure(const FString& Code, const FString& Path,
		const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual,
		const FString& Stage, const FString& PublicReason)
	{
		FDiagnostic Diagnostic = FDiagnostic::Failure(Code, PublicReason, Stage);
        Diagnostic.bRetrySafe = true;
		Diagnostic.PropertyPath = Path;
		if (Path.StartsWith(TEXT("/")))
		{
			Diagnostic.SemanticPointer = Path;
			Diagnostic.Source.JsonPath = Path;
		}
		Diagnostic.ExpectedValue = Expected;
		Diagnostic.ActualValue = Actual;
		return Diagnostic;
	}







	FDiagnostic MakeIssuesFailure(const FString& FallbackCode,
        const TArray<FDiagnostic>& Issues, const FString& Stage, const FString& TargetPath)
    {
        FDiagnostic Failures;
        for (auto Issue : Issues)
        {
            if (Issue.IsEmpty() || Issue.Severity == ESeverity::Info || Issue.Code == TEXT("json_unknown_field")) continue;
            if (Issue.Stage.IsEmpty()) Issue.Stage = Stage;
            if (Issue.TargetPath.IsEmpty()) Issue.TargetPath = TargetPath;
            Failures.Append(Issue);
        }
        if (Failures.IsEmpty())
        {
            Failures = FDiagnostic::Failure(FallbackCode.IsEmpty() ? TEXT("validation_diagnostic_missing") : FallbackCode,
                TEXT("Validation failed without reporting a specific diagnostic."), Stage);
            Failures.SourceKind = TEXT("native_result");
        }
        if (Failures.TargetPath.IsEmpty()) Failures.TargetPath = TargetPath;
        return Failures;
    }



	FDiagnostic CandidateFailure(const FString& Code, const FString& Name,
		const FString& TargetPath, const FString& Context, const FString& QueryCommand,
		const FDiagnostic& Diagnostic)
	{
		FDiagnostic Failure = Diagnostic;
		if (Failure.IsEmpty()) Failure = FDiagnostic::Failure(Code, FString(), TEXT("candidate_resolve"));
		else if (!Code.IsEmpty() && Code != Failure.Code)
		{
			Failure.Code = Code;
			Failure.PublicCause = MakeShared<FDiagnostic>(Diagnostic);
			Failure.PublicMessage = FString::Printf(TEXT("Candidate '%s' could not be selected in %s."), *Name, *Context);
		}
		if (Failure.CandidateName.IsEmpty()) Failure.CandidateName = Name;
		if (Failure.TargetPath.IsEmpty()) Failure.TargetPath = TargetPath;
		if (Failure.Stage.IsEmpty()) Failure.Stage = TEXT("candidate_resolve");
		if (Diagnostic.IsEmpty())
			Failure.PublicMessage = FString::Printf(TEXT("Candidate '%s' could not be selected in %s."), *Name, *Context);
		Failure.SuggestedFix = FString::Printf(TEXT("Query %s for this target and context%s."),
			*QueryCommand, Failure.CandidateChoices.IsValid() ? TEXT(", or use the returned choose fields") : TEXT(""));
		return Failure;
	}







	FString SeverityToString(const ESeverity Severity)
	{
		switch (Severity)
		{
		case ESeverity::Info:
			return TEXT("info");
		case ESeverity::Warning:
			return TEXT("warning");
		case ESeverity::Error:
		default:
			return TEXT("error");
		}
	}

	FDiagnostic MakeCompileFailure(const FString& Code, const FString& TargetPath,
		const TArray<TSharedPtr<FJsonValue>>& Messages)
	{
		FDiagnostic Failure;
		Failure.Code = Code;
		Failure.Stage = TEXT("compile");
		Failure.SourceKind = TEXT("native_result");
		Failure.TargetPath = TargetPath;
		Failure.PublicMessage = TEXT("Compilation failed.");
		for (const auto& Message : Messages)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Text;
			if (!Message.IsValid() || !Message->TryGetObject(Object) || !Object
				|| !Object->IsValid() || !(*Object)->TryGetStringField(TEXT("message"), Text)
				|| Text.IsEmpty()) continue;
			FDiagnostic Item;
			Item.Code = Code;
			Item.Stage = Failure.Stage;
			Item.TargetPath = TargetPath;
			Item.PublicMessage = MoveTemp(Text);
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Item, **Object);
			FString DeclaredSourceKind;
			(*Object)->TryGetStringField(TEXT("source_kind"), DeclaredSourceKind);
			if (Item.SourceKind.IsEmpty()) Item.SourceKind = DeclaredSourceKind;
			if (Item.SemanticPointer.IsEmpty())
				(*Object)->TryGetStringField(TEXT("semantic_pointer"), Item.SemanticPointer);
			if (Item.Source.JsonPath.IsEmpty() && DeclaredSourceKind != TEXT("native_result"))
				(*Object)->TryGetStringField(TEXT("json_pointer"), Item.Source.JsonPath);
			const TArray<TSharedPtr<FJsonValue>>* Related = nullptr;
			if ((*Object)->TryGetArrayField(TEXT("related_sources"), Related)) Item.RelatedSources = *Related;
			Failure.Diagnostics.Add(MoveTemp(Item));
		}
		return Failure;
	}

	void AppendCompilerMessageSources(const FTokenizedMessage& Message,
		const UObject* CompileTarget, FJsonObject& OutMessage)
	{
		TArray<FString> Pointers;
		for (const auto& Token : Message.GetMessageTokens())
		{
			if (Token->GetType() != EMessageToken::EdGraph) continue;
			const auto GraphToken = StaticCastSharedRef<FEdGraphToken>(Token);
			const UEdGraphNode* Node = Cast<UEdGraphNode>(GraphToken->GetGraphObject());
			if (!Node || !Node->GetGraph() || !Node->NodeGuid.IsValid()
				|| !Node->GetGraph()->GraphGuid.IsValid()
				|| (CompileTarget && !Node->IsIn(CompileTarget))) continue;
			Pointers.AddUnique(TEXT("/graphs/") + Node->GetGraph()->GraphGuid.ToString()
				+ TEXT("/nodes/") + Node->NodeGuid.ToString());
		}
		// These selectors identify native graph objects, not authored AST locations.
		OutMessage.SetStringField(TEXT("source_kind"), TEXT("native_result"));
		if (Pointers.IsEmpty()) return;
		OutMessage.SetStringField(TEXT("semantic_pointer"), Pointers[0]);
		if (Pointers.Num() > 1)
		{
			TArray<TSharedPtr<FJsonValue>> Related;
			for (int32 Index = 1; Index < Pointers.Num(); ++Index)
			{
				auto Source = MakeShared<FJsonObject>();
				Source->SetStringField(TEXT("source_kind"), TEXT("native_result"));
				Source->SetStringField(TEXT("semantic_pointer"), Pointers[Index]);
				Related.Add(MakeShared<FJsonValueObject>(Source));
			}
			OutMessage.SetArrayField(TEXT("related_sources"), MoveTemp(Related));
		}
	}

	FDiagnostic MakeCompileFailure(const FString& Code, const FString& TargetPath,
		const FCompilerResultsLog& CompilerLog, const UObject* CompileTarget)
	{
		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const auto& Message : CompilerLog.Messages)
		{
			if (Message->GetSeverity() != EMessageSeverity::Error) continue;
			const auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("message"), Message->ToText().ToString());
			AppendCompilerMessageSources(*Message, CompileTarget, *Item);
			Messages.Add(MakeShared<FJsonValueObject>(Item));
		}
		return MakeCompileFailure(Code, TargetPath, Messages);
	}








}
