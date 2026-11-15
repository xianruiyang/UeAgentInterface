// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeRWLock.h"
#include "String/ParseTokens.h"

namespace UeAgentAuthoringIdentity
{
	namespace
	{
		FString NormalizeId(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ToLowerInline();
			return Value;
		}

		// Same serialized runs, but a validation consumer needs no owning key.
		// FString::Equals first compares lengths, then the C-string prefix. Keep
		// that original embedded-NUL behavior rather than inventing a new grammar.
		struct FKeyComparison
		{
			const FString& Expected;
			int32 Written = 0;
			bool bEqual = true, bTerminated = false;
			void AppendRun(FStringView Value, bool bLowerCase, int32 NullIndex)
			{
				const int32 Begin = Written;
				Written += Value.Len();
				if (!bEqual || bTerminated || Value.IsEmpty()) return;
				if (Begin > Expected.Len() || Value.Len() > Expected.Len() - Begin) { bEqual = false; return; }
				if (!bLowerCase) {
					bEqual = FCString::Strnicmp(Value.GetData(), *Expected + Begin, Value.Len()) == 0;
					bTerminated = bEqual && NullIndex != INDEX_NONE;
					return;
				}
				TCHAR Lower[128];
				for (int32 Offset = 0; Offset < Value.Len(); Offset += UE_ARRAY_COUNT(Lower)) {
					const int32 Count = FMath::Min<int32>(UE_ARRAY_COUNT(Lower), Value.Len() - Offset);
					for (int32 Index = 0; Index < Count; ++Index) Lower[Index] = FChar::ToLower(Value[Offset + Index]);
					if (FCString::Strnicmp(Lower, *Expected + Begin + Offset, Count) != 0) { bEqual = false; return; }
					if (NullIndex >= Offset && NullIndex < Offset + Count) { bTerminated = true; return; }
				}
			}
			void AppendChars(const TCHAR* Text, int32 Count) { AppendRun(FStringView(Text, Count), false, INDEX_NONE); }
			void AppendChar(TCHAR Character) { if (Character) AppendChars(&Character, 1); }
			bool Matches() const { return bEqual && Written == Expected.Len(); }
		};

		void AppendKeyRun(FString& Out, FStringView Value, bool bLowerCase, int32 = INDEX_NONE)
		{
			const int32 Begin = Out.Len();
			Out.AppendChars(Value.GetData(), Value.Len());
			if (bLowerCase)
			{
				TCHAR* Data = Out.GetCharArray().GetData();
				for (int32 Index = Begin; Index < Out.Len(); ++Index) Data[Index] = FChar::ToLower(Data[Index]);
			}
		}

		void AppendKeyRun(FKeyComparison& Out, FStringView Value, bool bLowerCase, int32 NullIndex = INDEX_NONE)
		{
			Out.AppendRun(Value, bLowerCase, NullIndex);
		}

		template<typename Destination>
		void AppendEscapedKeyPart(Destination& Out, FStringView Value, bool bLowerCase = false)
		{
			int32 RunStart = 0, NullInputIndex = INDEX_NONE;
			bool bReplaced = false;
			for (int32 Index = 0; Index < Value.Len(); ++Index)
			{
				const TCHAR Character = Value[Index];
				if (Character == TEXT('\0') && NullInputIndex == INDEX_NONE)
				{
					// Unequal-length ReplaceInline reconstructs through C strings.
					// Keep that original NUL boundary, including a match after NUL.
					if (bReplaced) { AppendKeyRun(Out, Value.Mid(RunStart, Index - RunStart), bLowerCase); return; }
					NullInputIndex = Index;
				}
				const TCHAR* Escaped = nullptr;
				switch (Character)
				{
				case TEXT('%'): Escaped = TEXT("%25"); break;
				case TEXT('|'): Escaped = TEXT("%7C"); break;
				case TEXT('&'): Escaped = TEXT("%26"); break;
				case TEXT('='): Escaped = TEXT("%3D"); break;
				case TEXT('\r'): Escaped = TEXT("%0D"); break;
				case TEXT('\n'): Escaped = TEXT("%0A"); break;
				case TEXT('\t'): Escaped = TEXT("%09"); break;
				default: break;
				}
				if (!Escaped) continue;
				if (NullInputIndex != INDEX_NONE) { AppendKeyRun(Out, Value.Mid(RunStart, NullInputIndex - RunStart), bLowerCase); return; }
				AppendKeyRun(Out, Value.Mid(RunStart, Index - RunStart), bLowerCase);
				Out.AppendChars(Escaped, 3);
				RunStart = Index + 1;
				bReplaced = true;
			}
			AppendKeyRun(Out, Value.Mid(RunStart), bLowerCase,
				NullInputIndex == INDEX_NONE ? INDEX_NONE : NullInputIndex - RunStart);
		}

		FString UnescapeKeyPart(FStringView Value)
		{
			FString Result;
			Result.Reserve(Value.Len());
			int32 RunStart = 0, NullInputIndex = INDEX_NONE;
			bool bReplaced = false;
			for (int32 Index = 0; Index < Value.Len(); ++Index)
			{
				const TCHAR Character = Value[Index];
				if (Character == TEXT('\0') && NullInputIndex == INDEX_NONE)
				{
					if (bReplaced) { AppendKeyRun(Result, Value.Mid(RunStart, Index - RunStart), false); return Result; }
					NullInputIndex = Index;
				}
				TCHAR Decoded = TEXT('\0');
				if (Character == TEXT('%') && Index + 2 < Value.Len())
				{
					const TCHAR High = FChar::ToUpper(Value[Index + 1]);
					const TCHAR Low = FChar::ToUpper(Value[Index + 2]);
					if (High == TEXT('0'))
					{
						if (Low == TEXT('9')) Decoded = TEXT('\t');
						else if (Low == TEXT('A')) Decoded = TEXT('\n');
						else if (Low == TEXT('D')) Decoded = TEXT('\r');
					}
					else if (High == TEXT('2'))
					{
						if (Low == TEXT('5')) Decoded = TEXT('%');
						else if (Low == TEXT('6')) Decoded = TEXT('&');
					}
					else if (High == TEXT('3') && Low == TEXT('D')) Decoded = TEXT('=');
					else if (High == TEXT('7') && Low == TEXT('C')) Decoded = TEXT('|');
				}
				if (Decoded == TEXT('\0')) continue;
				if (NullInputIndex != INDEX_NONE) { AppendKeyRun(Result, Value.Mid(RunStart, NullInputIndex - RunStart), false); return Result; }
				AppendKeyRun(Result, Value.Mid(RunStart, Index - RunStart), false);
				Result.AppendChar(Decoded);
				Index += 2;
				RunStart = Index + 1;
				bReplaced = true;
			}
			AppendKeyRun(Result, Value.Mid(RunStart), false);
			return Result;
		}

		const FSelectorDescriptor* FindSelector(const FModelDescriptor& Model, const FString& SelectorName)
		{
			return Model.Selectors.FindByPredicate([&SelectorName](const FSelectorDescriptor& Selector)
			{
				return Selector.Name == SelectorName;
			});
		}

		FSelectorDescriptor Selector(
			const TCHAR* Name,
			int32 Priority,
			bool bCaseSensitive = false,
			bool bTrimValue = true)
		{
			FSelectorDescriptor Result;
			Result.Name = Name;
			Result.Priority = Priority;
			Result.bCaseSensitive = bCaseSensitive;
			Result.bTrimValue = bTrimValue;
			return Result;
		}

		FKeyPattern Pattern(const TCHAR* PatternId, int32 Priority, std::initializer_list<const TCHAR*> SelectorNames)
		{
			FKeyPattern Result;
			Result.PatternId = PatternId;
			Result.Priority = Priority;
			for (const TCHAR* SelectorName : SelectorNames)
			{
				Result.Selectors.Add(SelectorName);
			}
			return Result;
		}


		FDiagnostic IdentityFailure(const FString& Code, const TCHAR* Reason, const TCHAR* Stage,
			const FString& Model, const FString& Field)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, Stage);
			Error.CandidateName = Model;
			Error.PropertyPath = Field;
			return Error;
		}
		FDiagnostic RegistryFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Model,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TCHAR* Expected)
		{
			auto Error = IdentityFailure(Code, Reason, TEXT("contract"), Model, Field);
			Error.SourceKind = TEXT("native_result");
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			Error.ActualValue = Actual;
			return Error;
		}
		TSharedPtr<FJsonValue> DescriptorValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
		TSharedPtr<FJsonValue> DescriptorValue(int32 Value) { return MakeShared<FJsonValueNumber>(Value); }
		TSharedPtr<FJsonValue> DescriptorValue(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
		template<typename T>
		bool DescriptorEqual(const T& Expected, const T& Actual, const FString& Path, FDiagnostic* Error)
		{
			if (Expected == Actual) return true;
			if (Error) { Error->PropertyPath = Path; Error->ExpectedValue = DescriptorValue(Expected); Error->ActualValue = DescriptorValue(Actual); }
			return false;
		}
		bool DescriptorEqual(const FSelectorDescriptor& Expected, const FSelectorDescriptor& Actual, const FString& Path, FDiagnostic* Error)
		{
			return DescriptorEqual(Expected.Name, Actual.Name, Path + TEXT("/name"), Error)
				&& DescriptorEqual(Expected.Priority, Actual.Priority, Path + TEXT("/priority"), Error)
				&& DescriptorEqual(Expected.bCaseSensitive, Actual.bCaseSensitive, Path + TEXT("/case_sensitive"), Error)
				&& DescriptorEqual(Expected.bTrimValue, Actual.bTrimValue, Path + TEXT("/trim_value"), Error);
		}
		bool DescriptorEqual(const FKeyPattern& Expected, const FKeyPattern& Actual, const FString& Path, FDiagnostic* Error);
		template<typename T>
		bool DescriptorEqual(const TArray<T>& Expected, const TArray<T>& Actual, const FString& Path, FDiagnostic* Error)
		{
			if (!DescriptorEqual(Expected.Num(), Actual.Num(), Path + TEXT("/count"), Error)) return false;
			for (int32 Index = 0; Index < Expected.Num(); ++Index)
				if (!DescriptorEqual(Expected[Index], Actual[Index], Path + TEXT("/") + FString::FromInt(Index), Error)) return false;
			return true;
		}
		bool DescriptorEqual(const FKeyPattern& Expected, const FKeyPattern& Actual, const FString& Path, FDiagnostic* Error)
		{
			return DescriptorEqual(Expected.PatternId, Actual.PatternId, Path + TEXT("/pattern_id"), Error)
				&& DescriptorEqual(Expected.Priority, Actual.Priority, Path + TEXT("/priority"), Error)
				&& DescriptorEqual(Expected.Selectors, Actual.Selectors, Path + TEXT("/selectors"), Error);
		}
		bool ModelsEqual(const FModelDescriptor& Expected, const FModelDescriptor& Actual, FDiagnostic* Error)
		{
			return DescriptorEqual(Expected.ModelId, Actual.ModelId, TEXT("/model_id"), Error)
				&& DescriptorEqual(Expected.TargetKind, Actual.TargetKind, TEXT("/target_kind"), Error)
				&& DescriptorEqual(Expected.Selectors, Actual.Selectors, TEXT("/selectors"), Error)
				&& DescriptorEqual(Expected.KeyPatterns, Actual.KeyPatterns, TEXT("/key_patterns"), Error);
		}

		FModelDescriptor Normalize(FModelDescriptor Descriptor, UE::UAI::Contract::FDiagnostic& OutError)
		{
			Descriptor.ModelId = NormalizeId(MoveTemp(Descriptor.ModelId));
			Descriptor.TargetKind = NormalizeId(MoveTemp(Descriptor.TargetKind));
			TSet<FString> SeenSelectors;
			for (FSelectorDescriptor& SelectorDescriptor : Descriptor.Selectors)
			{
				SelectorDescriptor.Name = NormalizeId(MoveTemp(SelectorDescriptor.Name));
				if (SelectorDescriptor.Name.IsEmpty())
				{
					OutError = RegistryFailure(TEXT("identity_selector_name_missing"), TEXT("The identity selector name is empty."), Descriptor.ModelId, TEXT("/selectors/") + FString::FromInt(static_cast<int32>(&SelectorDescriptor - Descriptor.Selectors.GetData())) + TEXT("/name"), DescriptorValue(SelectorDescriptor.Name), TEXT("non-empty selector name"));
					return FModelDescriptor();
				}
				if (SeenSelectors.Contains(SelectorDescriptor.Name))
				{
					OutError = RegistryFailure(TEXT("identity_selector_duplicate"), TEXT("The identity model declares the same selector more than once."), Descriptor.ModelId, TEXT("/selectors/") + FString::FromInt(static_cast<int32>(&SelectorDescriptor - Descriptor.Selectors.GetData())) + TEXT("/name"), DescriptorValue(SelectorDescriptor.Name), TEXT("unique selector name"));
					return FModelDescriptor();
				}
				SeenSelectors.Add(SelectorDescriptor.Name);
			}
			Descriptor.Selectors.Sort([](const FSelectorDescriptor& Left, const FSelectorDescriptor& Right)
			{
				return Left.Priority == Right.Priority ? Left.Name < Right.Name : Left.Priority < Right.Priority;
			});

			TSet<FString> SeenPatterns;
			for (FKeyPattern& KeyPattern : Descriptor.KeyPatterns)
			{
				KeyPattern.PatternId = NormalizeId(MoveTemp(KeyPattern.PatternId));
				if (KeyPattern.PatternId.IsEmpty() || KeyPattern.Selectors.IsEmpty())
				{
					OutError = RegistryFailure(TEXT("identity_key_pattern_invalid"), TEXT("An identity key pattern requires an identifier and at least one selector."), Descriptor.ModelId, TEXT("/key_patterns/") + FString::FromInt(static_cast<int32>(&KeyPattern - Descriptor.KeyPatterns.GetData())) + (KeyPattern.PatternId.IsEmpty() ? TEXT("/pattern_id") : TEXT("/selectors")), KeyPattern.PatternId.IsEmpty() ? DescriptorValue(KeyPattern.PatternId) : DescriptorValue(KeyPattern.Selectors.Num()), TEXT("non-empty pattern identifier and selector list"));
					return FModelDescriptor();
				}
				if (SeenPatterns.Contains(KeyPattern.PatternId))
				{
					OutError = RegistryFailure(TEXT("identity_key_pattern_duplicate"), TEXT("The identity model declares the same key pattern more than once."), Descriptor.ModelId, TEXT("/key_patterns/") + FString::FromInt(static_cast<int32>(&KeyPattern - Descriptor.KeyPatterns.GetData())) + TEXT("/pattern_id"), DescriptorValue(KeyPattern.PatternId), TEXT("unique pattern identifier"));
					return FModelDescriptor();
				}
				SeenPatterns.Add(KeyPattern.PatternId);
				TSet<FString> PatternSelectors;
				for (FString& SelectorName : KeyPattern.Selectors)
				{
					SelectorName = NormalizeId(MoveTemp(SelectorName));
					if (!SeenSelectors.Contains(SelectorName))
					{
						OutError = RegistryFailure(TEXT("identity_key_selector_unknown"), TEXT("The key pattern references a selector not declared by its identity model."), Descriptor.ModelId, TEXT("/key_patterns/") + FString::FromInt(static_cast<int32>(&KeyPattern - Descriptor.KeyPatterns.GetData())) + TEXT("/selectors/") + FString::FromInt(static_cast<int32>(&SelectorName - KeyPattern.Selectors.GetData())), DescriptorValue(SelectorName), TEXT("declared selector name"));
						return FModelDescriptor();
					}
					if (PatternSelectors.Contains(SelectorName))
					{
						OutError = RegistryFailure(TEXT("identity_key_selector_duplicate"), TEXT("The key pattern repeats a selector."), Descriptor.ModelId, TEXT("/key_patterns/") + FString::FromInt(static_cast<int32>(&KeyPattern - Descriptor.KeyPatterns.GetData())) + TEXT("/selectors/") + FString::FromInt(static_cast<int32>(&SelectorName - KeyPattern.Selectors.GetData())), DescriptorValue(SelectorName), TEXT("unique selector within this pattern"));
						return FModelDescriptor();
					}
					PatternSelectors.Add(SelectorName);
				}
			}
			Descriptor.KeyPatterns.Sort([](const FKeyPattern& Left, const FKeyPattern& Right)
			{
				return Left.Priority == Right.Priority ? Left.PatternId < Right.PatternId : Left.Priority < Right.Priority;
			});
			OutError.Reset();
			return Descriptor;
		}

		bool Validate(const FModelDescriptor& Descriptor, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Descriptor.ModelId.IsEmpty()) OutError = RegistryFailure(TEXT("identity_model_id_missing"), TEXT("The identity model identifier is empty."), Descriptor.ModelId, TEXT("/model_id"), DescriptorValue(Descriptor.ModelId), TEXT("non-empty model identifier"));
			else if (Descriptor.Selectors.IsEmpty()) OutError = RegistryFailure(TEXT("identity_model_selectors_missing"), TEXT("The identity model has no selectors."), Descriptor.ModelId, TEXT("/selectors"), DescriptorValue(Descriptor.Selectors.Num()), TEXT("at least one selector"));
			else if (Descriptor.KeyPatterns.IsEmpty()) OutError = RegistryFailure(TEXT("identity_model_key_patterns_missing"), TEXT("The identity model has no key patterns."), Descriptor.ModelId, TEXT("/key_patterns"), DescriptorValue(Descriptor.KeyPatterns.Num()), TEXT("at least one key pattern"));
			else
			{
				OutError.Reset();
				return true;
			}
			return false;
		}

		bool RegisterModelChecked(FRegistry& Registry, const FModelDescriptor& Descriptor, UE::UAI::Contract::FDiagnostic& OutError)
		{
			ERegistrationResult Result = ERegistrationResult::Registered;
			return Registry.RegisterModel(Descriptor, Result, OutError);
		}
	}

	bool FSelectorDescriptor::operator==(const FSelectorDescriptor& Other) const
	{
		return DescriptorEqual(*this, Other, FString(), nullptr);
	}

	bool FKeyPattern::operator==(const FKeyPattern& Other) const
	{
		return DescriptorEqual(*this, Other, FString(), nullptr);
	}

	bool FModelDescriptor::operator==(const FModelDescriptor& Other) const
	{
		return ModelsEqual(*this, Other, nullptr);
	}

	bool FCanonicalIdentity::IsValid() const
	{
		// The diagnostic has no successful consumer. Keep its exact failure
		// constructor for callers which actually report the missing field.
		return !ModelId.IsEmpty() && !TargetKind.IsEmpty() && !KeyPattern.IsEmpty() && !CanonicalKey.IsEmpty();
	}

	FDiagnostic FCanonicalIdentity::GetValidationDiagnostic() const
	{
		const TCHAR* Field = ModelId.IsEmpty() ? TEXT("identity_model") : TargetKind.IsEmpty() ? TEXT("target_kind")
			: KeyPattern.IsEmpty() ? TEXT("key_pattern") : CanonicalKey.IsEmpty() ? TEXT("canonical_key") : nullptr;
		if (!Field) return {};
		auto Error = IdentityFailure(TEXT("canonical_identity_incomplete"), TEXT("The target identity has an empty required field."),
			TEXT("target_identity"), ModelId, Field);
		Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty identity field"));
		Error.ActualValue = MakeShared<FJsonValueString>(FString());
		return Error;
	}

	FString FCanonicalIdentity::GetSelector(const FString& Name) const
	{
		if (const FString* Value = Selectors.Find(NormalizeId(Name)))
		{
			return *Value;
		}
		return FString();
	}

	TSharedRef<FJsonObject> FCanonicalIdentity::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("identity_model"), ModelId);
		Result->SetStringField(TEXT("target_kind"), TargetKind);
		Result->SetStringField(TEXT("key_pattern"), KeyPattern);
		Result->SetStringField(TEXT("canonical_key"), CanonicalKey);
		TSharedRef<FJsonObject> SelectorObject = MakeShared<FJsonObject>();
		WriteSelectorsFlat(*this, SelectorObject);
		Result->SetObjectField(TEXT("selectors"), SelectorObject);
		return Result;
	}

	FRegistry& FRegistry::Get()
	{
		static FRegistry Registry;
		static const bool bRegistered = []()
		{
			UE::UAI::Contract::FDiagnostic Error;
			const bool bSuccess = RegisterBuiltInModels(Registry, Error);
			checkf(bSuccess, TEXT("Failed to register built-in authoring identity models: %s"), *Error.Code);
			return bSuccess;
		}();
		(void)bRegistered;
		return Registry;
	}

	bool FRegistry::RegisterModel(const FModelDescriptor& Descriptor, ERegistrationResult& OutResult, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FModelDescriptor Canonical = Normalize(Descriptor, OutError);
		if (!OutError.IsEmpty() || !Validate(Canonical, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FModelView* Existing = Models.Find(Canonical.ModelId))
		{
			OutError = IdentityFailure(TEXT("conflicting_identity_model"), TEXT("The identity model differs from its existing registration."), TEXT("contract"), Canonical.ModelId, FString());
			OutError.SourceKind = TEXT("native_result");
			if (!ModelsEqual(**Existing, Canonical, &OutError))
			{
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		const FString ModelId = Canonical.ModelId;
		Models.Add(ModelId, MakeShared<FModelDescriptor, ESPMode::ThreadSafe>(MoveTemp(Canonical)));
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FRegistry::FindModel(const FString& ModelId, FModelDescriptor& OutDescriptor) const
	{
		if (const FModelView Found = FindModelView(ModelId))
		{
			OutDescriptor = *Found;
			return true;
		}
		return false;
	}

	FModelView FRegistry::FindModelView(const FString& ModelId) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FModelView* Found = Models.Find(NormalizeId(ModelId))) return *Found;
		return nullptr;
	}

	TArray<FModelDescriptor> FRegistry::ListModels() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FModelDescriptor> Result;
		Result.Reserve(Models.Num());
		for (const auto& Pair : Models) Result.Add(*Pair.Value);
		Result.Sort([](const FModelDescriptor& Left, const FModelDescriptor& Right) { return Left.ModelId < Right.ModelId; });
		return Result;
	}

	int32 FRegistry::GetModelCount() const
	{
		FReadScopeLock Lock(RegistryLock);
		return Models.Num();
	}

	bool RegisterBuiltInModels(FRegistry& Registry, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<FModelDescriptor> Models;

		FModelDescriptor Object;
		Object.ModelId = TEXT("uobject_identity");
		Object.TargetKind = TEXT("uobject");
		Object.Selectors = { Selector(TEXT("object_path"), 0), Selector(TEXT("outer_path"), 10), Selector(TEXT("name"), 20), Selector(TEXT("class_path"), 30), Selector(TEXT("package_path"), 40) };
		Object.KeyPatterns = { Pattern(TEXT("object_path"), 0, { TEXT("object_path") }), Pattern(TEXT("outer_name_class"), 10, { TEXT("outer_path"), TEXT("name"), TEXT("class_path") }) };
		Models.Add(Object);

		FModelDescriptor Owner;
		Owner.ModelId = TEXT("owner_identity");
		Owner.TargetKind = TEXT("owner");
		Owner.Selectors = { Selector(TEXT("owner_kind"), 0), Selector(TEXT("owner_id"), 10), Selector(TEXT("object_path"), 20), Selector(TEXT("name"), 30), Selector(TEXT("class_path"), 40) };
		Owner.KeyPatterns = { Pattern(TEXT("kind_id"), 0, { TEXT("owner_kind"), TEXT("owner_id") }), Pattern(TEXT("object_path"), 10, { TEXT("object_path") }), Pattern(TEXT("kind_name"), 20, { TEXT("owner_kind"), TEXT("name") }) };
		Models.Add(Owner);

		FModelDescriptor Property;
		Property.ModelId = TEXT("property_identity");
		Property.TargetKind = TEXT("property");
		Property.Selectors = {
			Selector(TEXT("owner_path"), 0),
			Selector(TEXT("property_path"), 10, false, false),
			Selector(TEXT("notification_owner_path"), 20)
		};
		Property.KeyPatterns = { Pattern(TEXT("owner_property_path"), 0, { TEXT("owner_path"), TEXT("property_path") }) };
		Models.Add(Property);

		FModelDescriptor GraphNode;
		GraphNode.ModelId = TEXT("graph_node_identity");
		GraphNode.TargetKind = TEXT("graph_node");
		GraphNode.Selectors = { Selector(TEXT("asset_path"), 0), Selector(TEXT("graph_name"), 10), Selector(TEXT("node_guid"), 20), Selector(TEXT("node_name"), 30), Selector(TEXT("occurrence"), 40) };
		GraphNode.KeyPatterns = { Pattern(TEXT("owner_guid"), 0, { TEXT("asset_path"), TEXT("graph_name"), TEXT("node_guid") }), Pattern(TEXT("owner_name_occurrence"), 10, { TEXT("asset_path"), TEXT("graph_name"), TEXT("node_name"), TEXT("occurrence") }) };
		Models.Add(GraphNode);

		FModelDescriptor Stack;
		Stack.ModelId = TEXT("niagara_stack_identity");
		Stack.TargetKind = TEXT("niagara_stack_entry");
		Stack.Selectors = { Selector(TEXT("asset_path"), 0), Selector(TEXT("emitter_name"), 10), Selector(TEXT("usage"), 20), Selector(TEXT("module_guid"), 30), Selector(TEXT("stack_path"), 40), Selector(TEXT("occurrence"), 50) };
		Stack.KeyPatterns = { Pattern(TEXT("owner_guid"), 0, { TEXT("asset_path"), TEXT("emitter_name"), TEXT("module_guid") }), Pattern(TEXT("owner_stack_occurrence"), 10, { TEXT("asset_path"), TEXT("emitter_name"), TEXT("stack_path"), TEXT("occurrence") }) };
		Models.Add(Stack);

		FModelDescriptor Component;
		Component.ModelId = TEXT("component_identity");
		Component.TargetKind = TEXT("component");
		Component.Selectors = { Selector(TEXT("owner_path"), 0), Selector(TEXT("component_guid"), 10), Selector(TEXT("variable_name"), 20), Selector(TEXT("object_path"), 30), Selector(TEXT("class_path"), 40), Selector(TEXT("occurrence"), 50) };
		Component.KeyPatterns = { Pattern(TEXT("owner_guid"), 0, { TEXT("owner_path"), TEXT("component_guid") }), Pattern(TEXT("owner_variable"), 10, { TEXT("owner_path"), TEXT("variable_name") }), Pattern(TEXT("object_path"), 20, { TEXT("object_path") }) };
		Models.Add(Component);

		FModelDescriptor FileRole;
		FileRole.ModelId = TEXT("file_role_identity");
		FileRole.TargetKind = TEXT("file_role");
		FileRole.Selectors = { Selector(TEXT("root_id"), 0), Selector(TEXT("relative_path"), 10), Selector(TEXT("role"), 20) };
		FileRole.KeyPatterns = { Pattern(TEXT("root_path_role"), 0, { TEXT("root_id"), TEXT("relative_path"), TEXT("role") }), Pattern(TEXT("path_role"), 10, { TEXT("relative_path"), TEXT("role") }) };
		Models.Add(FileRole);

		FModelDescriptor Catalog;
		Catalog.ModelId = TEXT("catalog_candidate_identity");
		Catalog.Selectors = {
			Selector(TEXT("catalog_type"), 0), Selector(TEXT("full_name"), 10),
			Selector(TEXT("stable_id"), 20), Selector(TEXT("name"), 30),
			Selector(TEXT("display_name"), 40), Selector(TEXT("search_key"), 50),
			Selector(TEXT("object_path"), 60), Selector(TEXT("package_path"), 70),
			Selector(TEXT("path"), 80), Selector(TEXT("node_class"), 90),
			Selector(TEXT("function_owner_class"), 100), Selector(TEXT("function_name"), 110),
			Selector(TEXT("event_class"), 120), Selector(TEXT("event_name"), 130),
			Selector(TEXT("variable_name"), 140), Selector(TEXT("node_type"), 150),
			Selector(TEXT("member_parent_class"), 160), Selector(TEXT("component_class"), 170),
			Selector(TEXT("component_name"), 175), Selector(TEXT("widget_class"), 180),
			Selector(TEXT("actor_class"), 190),
			Selector(TEXT("native_action_signature"), 200),
			Selector(TEXT("asset_override"), 210),
			Selector(TEXT("actor_factory"), 220),
			Selector(TEXT("placement_category"), 230),
			Selector(TEXT("expression_class"), 240),
			Selector(TEXT("function_path"), 250),
			Selector(TEXT("operation_name"), 260),
			Selector(TEXT("class_binding"), 270),
			Selector(TEXT("macro_graph"), 280),
			Selector(TEXT("native_action_binding"), 290),
			Selector(TEXT("input_key"), 300),
			Selector(TEXT("input_action"), 310),
			Selector(TEXT("input_axis"), 320)
		};
		Catalog.KeyPatterns = {
			// Native spawner signatures do not always include configured class,
			// asset or member bindings. The catalog owner captures those semantics
			// separately; their stable fingerprint also participates in identity.
			Pattern(TEXT("catalog_native_action_bound"), -10,
				{ TEXT("catalog_type"), TEXT("native_action_signature"), TEXT("native_action_binding") }),
			Pattern(TEXT("catalog_native_action"), 0,
				{ TEXT("catalog_type"), TEXT("native_action_signature") }),
			// Reflection-backed component events have no native spawner signature.
			// Their readable event name is shared by distinct component bindings.
			Pattern(TEXT("catalog_component_event"), 5,
				{ TEXT("catalog_type"), TEXT("component_name"), TEXT("event_class"), TEXT("event_name") }),
			Pattern(TEXT("catalog_full_name"), 10,
				{ TEXT("catalog_type"), TEXT("full_name") })
		};
		Models.Add(Catalog);

		for (const FModelDescriptor& Model : Models)
		{
			if (!RegisterModelChecked(Registry, Model, OutError)) return false;
		}
		OutError.Reset();
		return true;
	}

	// Const public inputs keep their original copy/normalization contract.
	// The parser owns its decoded map and has no later reader, so that same
	// normalization algorithm can transfer the strings at their last consumer.
	static const FString& IdentitySelectorInput(const FString& Value) { return Value; }
	static FString&& IdentitySelectorInput(FString& Value) { return MoveTemp(Value); }
	static FString IdentitySelectorInput(FStringView Value) { return FString(Value); }

	static void ReserveCanonicalKey(FString& Key, const FModelDescriptor& Model, const FString& TargetKind,
		const FKeyPattern& Pattern, const TMap<FString, FString>& Selectors)
	{
		int32 Length = Model.ModelId.Len() + TargetKind.Len() + Pattern.PatternId.Len() + 3;
		for (const FString& Name : Pattern.Selectors) Length += Name.Len() + Selectors.FindChecked(Name).Len() + 2;
		Key.Reserve(Length);
	}
	static void ReserveCanonicalKey(FKeyComparison&, const FModelDescriptor&, const FString&,
		const FKeyPattern&, const TMap<FString, FString>&) {}

	template<typename Destination>
	static void WriteCanonicalKey(Destination& Key, const FModelDescriptor& Model, const FString& TargetKind,
		const FKeyPattern& Pattern, const TMap<FString, FString>& Selectors)
	{
		ReserveCanonicalKey(Key, Model, TargetKind, Pattern, Selectors);
		AppendEscapedKeyPart(Key, Model.ModelId); Key.AppendChar(TEXT('|'));
		AppendEscapedKeyPart(Key, TargetKind); Key.AppendChar(TEXT('|'));
		AppendEscapedKeyPart(Key, Pattern.PatternId); Key.AppendChar(TEXT('|'));
		for (int32 Index = 0; Index < Pattern.Selectors.Num(); ++Index) {
			const FString& Name = Pattern.Selectors[Index];
			const FSelectorDescriptor* Descriptor = FindSelector(Model, Name);
			if (Index > 0) Key.AppendChar(TEXT('&'));
			AppendEscapedKeyPart(Key, Name); Key.AppendChar(TEXT('='));
			AppendEscapedKeyPart(Key, Selectors.FindChecked(Name), !Descriptor || !Descriptor->bCaseSensitive);
		}
	}

	static bool FailNoncanonicalKey(const FString& Supplied, const FString& Expected, const FString& ModelId,
		bool bPatternMatches, FDiagnostic& OutError)
	{
		OutError = IdentityFailure(TEXT("canonical_identity_key_noncanonical"),
			!bPatternMatches ? TEXT("The canonical key names a different key pattern from the resolved selectors.")
				: TEXT("The supplied identity key differs from its canonical representation."),
			TEXT("target_identity"), ModelId, TEXT("canonical_key"));
		OutError.ExpectedEvidence = DescriptorValue(Expected);
		OutError.ActualEvidence = DescriptorValue(Supplied);
		return false;
	}

	struct FKeyValidation
	{
		const FString& Supplied;
		const FString& Pattern;
		const FModelView& QueryModel;
		FString& OutModelId;
		FString& OutTargetKind;
	};

	struct FKeyGenerationComparison {
		const FString& Expected;
		bool& Matches;
		FString& ActualOnMismatch;
	};

	template<typename SelectorMap>
	static bool BuildCanonicalIdentityImpl(
		const FString& ModelId,
		const FString& TargetKind,
		SelectorMap& Selectors,
		FCanonicalIdentity* OutIdentity,
		FDiagnostic& OutError,
		const FRegistry* Registry,
		FKeyValidation* Validation = nullptr, FKeyGenerationComparison* GenerationComparison = nullptr)
	{
		if (OutIdentity) *OutIdentity = FCanonicalIdentity();
		const FRegistry& ResolvedRegistry = Registry ? *Registry : FRegistry::Get();
		const FModelView ModelView = Validation && NormalizeId(ModelId) == Validation->QueryModel->ModelId
			? Validation->QueryModel : ResolvedRegistry.FindModelView(ModelId);
		if (!ModelView)
		{
			OutError = IdentityFailure(TEXT("target_identity_model_unknown"), TEXT("The requested identity model is not registered."), TEXT("target_identity"), ModelId, TEXT("identity_model"));
			OutError.ExpectedValue = DescriptorValue(FString(TEXT("registered identity model")));
			OutError.ActualValue = DescriptorValue(ModelId);
			return false;
		}
		const FModelDescriptor& Model = *ModelView;
		const FString CanonicalTargetKind = NormalizeId(TargetKind);
		if (CanonicalTargetKind.IsEmpty())
		{
			OutError = IdentityFailure(TEXT("target_kind_missing"), TEXT("The target kind is empty."), TEXT("target_identity"), Model.ModelId, TEXT("target_kind"));
			OutError.ExpectedValue = DescriptorValue(FString(TEXT("non-empty target kind")));
			OutError.ActualValue = DescriptorValue(TargetKind);
			return false;
		}
		if (!Model.TargetKind.IsEmpty() && Model.TargetKind != CanonicalTargetKind)
		{
			OutError = IdentityFailure(TEXT("target_identity_model_mismatch"), TEXT("The target kind does not match the identity model."), TEXT("target_identity"), Model.ModelId, TEXT("target_kind"));
			OutError.ExpectedValue = DescriptorValue(Model.TargetKind);
			OutError.ActualValue = DescriptorValue(CanonicalTargetKind);
			return false;
		}

		TMap<FString, FString> CanonicalSelectors;
		for (auto& Pair : Selectors)
		{
			FString SelectorName = NormalizeId(IdentitySelectorInput(Pair.Key));
			if (SelectorName.IsEmpty()) continue;
			const FSelectorDescriptor* SelectorDescriptor = FindSelector(Model, SelectorName);
			if (!SelectorDescriptor)
			{
				OutError = IdentityFailure(TEXT("target_selector_unknown"), TEXT("The selector is not declared by this identity model."), TEXT("target_identity"), Model.ModelId, SelectorName);
			TArray<TSharedPtr<FJsonValue>> Allowed;
			for (const auto& Declared : Model.Selectors) Allowed.Add(DescriptorValue(Declared.Name));
			OutError.ExpectedValue = MakeShared<FJsonValueArray>(Allowed);
			OutError.ActualValue = DescriptorValue(SelectorName);
				return false;
			}
			FString SelectorValue = IdentitySelectorInput(Pair.Value);
			if (SelectorDescriptor->bTrimValue) SelectorValue.TrimStartAndEndInline();
			if (SelectorValue.IsEmpty()) continue;
			if (const FString* Existing = CanonicalSelectors.Find(SelectorName))
			{
				const ESearchCase::Type SearchCase = SelectorDescriptor->bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase;
				if (!Existing->Equals(SelectorValue, SearchCase))
				{
					OutError = IdentityFailure(TEXT("identity_selector_conflict"), TEXT("The same normalized selector was supplied with conflicting values."), TEXT("target_identity"), Model.ModelId, SelectorName);
					OutError.ExpectedEvidence = DescriptorValue(*Existing);
					OutError.ActualEvidence = DescriptorValue(SelectorValue);
					return false;
				}
				continue;
			}
			CanonicalSelectors.Add(MoveTemp(SelectorName), MoveTemp(SelectorValue));
		}

		const FKeyPattern* SelectedPattern = Model.KeyPatterns.FindByPredicate([&CanonicalSelectors](const FKeyPattern& KeyPattern)
		{
			return KeyPattern.Selectors.ContainsByPredicate([&CanonicalSelectors](const FString& SelectorName)
			{
				return !CanonicalSelectors.Contains(SelectorName);
			}) == false;
		});
		if (!SelectedPattern)
		{
			OutError = IdentityFailure(TEXT("identity_key_unresolved"), TEXT("The supplied selectors do not satisfy any identity key pattern."), TEXT("target_identity"), Model.ModelId, TEXT("selectors"));
			TArray<TSharedPtr<FJsonValue>> Alternatives, Supplied;
			for (const auto& Pattern : Model.KeyPatterns)
			{
				TArray<TSharedPtr<FJsonValue>> Missing;
				for (const auto& Name : Pattern.Selectors) if (!CanonicalSelectors.Contains(Name)) Missing.Add(DescriptorValue(Name));
				Alternatives.Add(MakeShared<FJsonValueArray>(Missing));
			}
			TArray<FString> Names; CanonicalSelectors.GenerateKeyArray(Names); Names.Sort();
			for (const auto& Name : Names) Supplied.Add(DescriptorValue(Name));
			OutError.ExpectedValue = MakeShared<FJsonValueArray>(Alternatives);
			OutError.ActualValue = MakeShared<FJsonValueArray>(Supplied);
			return false;
		}

		if (GenerationComparison) {
			FKeyComparison Comparison{GenerationComparison->Expected};
			WriteCanonicalKey(Comparison, Model, CanonicalTargetKind, *SelectedPattern, CanonicalSelectors);
			GenerationComparison->Matches = Comparison.Matches();
			if (!GenerationComparison->Matches)
				WriteCanonicalKey(GenerationComparison->ActualOnMismatch, Model, CanonicalTargetKind, *SelectedPattern, CanonicalSelectors);
		} else if (Validation) {
			FKeyComparison Comparison{Validation->Supplied};
			WriteCanonicalKey(Comparison, Model, CanonicalTargetKind, *SelectedPattern, CanonicalSelectors);
			const bool bPatternMatches = SelectedPattern->PatternId.Equals(Validation->Pattern, ESearchCase::IgnoreCase);
			if (!bPatternMatches || !Comparison.Matches()) {
				FString Expected;
				WriteCanonicalKey(Expected, Model, CanonicalTargetKind, *SelectedPattern, CanonicalSelectors);
				return FailNoncanonicalKey(Validation->Supplied, Expected, Model.ModelId, bPatternMatches, OutError);
			}
			Validation->OutModelId = Model.ModelId;
			Validation->OutTargetKind = CanonicalTargetKind;
		} else {
			FString Key;
			WriteCanonicalKey(Key, Model, CanonicalTargetKind, *SelectedPattern, CanonicalSelectors);
			OutIdentity->ModelId = Model.ModelId;
			OutIdentity->TargetKind = CanonicalTargetKind;
			OutIdentity->KeyPattern = SelectedPattern->PatternId;
			OutIdentity->CanonicalKey = MoveTemp(Key);
			OutIdentity->Selectors = MoveTemp(CanonicalSelectors);
		}
		OutError.Reset();
		return true;
	}

	bool BuildCanonicalIdentity(const FString& ModelId, const FString& TargetKind,
		const TMap<FString, FString>& Selectors, FCanonicalIdentity& OutIdentity,
		FDiagnostic& OutError, const FRegistry* Registry)
	{
		return BuildCanonicalIdentityImpl(ModelId, TargetKind, Selectors, &OutIdentity, OutError, Registry);
	}

	bool BuildCanonicalIdentityFromViews(const FString& ModelId, const FString& TargetKind,
		TConstArrayView<TPair<FStringView, FStringView>> Selectors, FCanonicalIdentity& OutIdentity,
		FDiagnostic& OutError, const FRegistry* Registry)
	{
		// A property caller can borrow an input from its output's old selector
		// storage. Consume every view before replacing that owning destination.
		FCanonicalIdentity Built;
		const bool bBuilt = BuildCanonicalIdentityImpl(ModelId, TargetKind, Selectors, &Built, OutError, Registry);
		OutIdentity = MoveTemp(Built); return bBuilt;
	}

	bool CompareCanonicalIdentityFromViews(const FString& ModelId, const FString& TargetKind,
		TConstArrayView<TPair<FStringView, FStringView>> Selectors, const FString& ExpectedKey,
		bool& OutMatches, FString& OutActualKey, FDiagnostic& OutError, const FRegistry* Registry)
	{
		bool bMatches = false; FString Actual;
		FKeyGenerationComparison Comparison{ExpectedKey, bMatches, Actual};
		const bool bBuilt = BuildCanonicalIdentityImpl(ModelId, TargetKind, Selectors, nullptr, OutError, Registry, nullptr, &Comparison);
		OutMatches = bMatches; OutActualKey = MoveTemp(Actual); return bBuilt;
	}

	template<typename Consumer>
	static bool ParseCanonicalKeyParts(
		const FString& CanonicalKey,
		FDiagnostic& OutError,
		Consumer&& Consume)
	{
		TArray<FStringView, TInlineAllocator<4>> Parts;
		// FString::ParseIntoArray uses this same Engine primitive. Retain its
		// actual extents/empty-token rules without owning every split substring.
		UE::String::ParseTokens(FStringView(CanonicalKey), FStringView(TEXT("|")),
			[&](FStringView Part) { Parts.Add(Part); }, UE::String::EParseTokensOptions::IgnoreCase);
		if (Parts.Num() != 4)
		{
			OutError = IdentityFailure(TEXT("canonical_identity_key_invalid"), TEXT("The canonical identity key must contain four separated parts."), TEXT("target_identity"), FString(), TEXT("canonical_key/part_count"));
			OutError.ExpectedValue = DescriptorValue(4); OutError.ActualValue = DescriptorValue(Parts.Num());
			OutError.ActualEvidence = DescriptorValue(CanonicalKey);
			return false;
		}
		TMap<FString, FString> Selectors;
		TArray<FStringView, TInlineAllocator<8>> SelectorParts;
		UE::String::ParseTokens(Parts[3], FStringView(TEXT("&")),
			[&](FStringView Part) { SelectorParts.Add(Part); },
			UE::String::EParseTokensOptions::IgnoreCase | UE::String::EParseTokensOptions::SkipEmpty);
		for (int32 SelectorIndex = 0; SelectorIndex < SelectorParts.Num(); ++SelectorIndex)
		{
			const FStringView SelectorPart = SelectorParts[SelectorIndex];
			// The same bounded primitive used by FString::Split/Find preserves
			// its NUL and first-separator behavior on the borrowed token.
			const TCHAR* Separator = FCString::Strnistr(SelectorPart.GetData(), SelectorPart.Len(), TEXT("="), 1);
			const int32 Split = Separator ? static_cast<int32>(Separator - SelectorPart.GetData()) : INDEX_NONE;
			if (Split <= 0)
			{
				OutError = IdentityFailure(TEXT("canonical_identity_key_selector_invalid"), TEXT("A canonical key selector must have a non-empty name followed by an equals sign."), TEXT("target_identity"), FString(), TEXT("canonical_key/selectors/") + FString::FromInt(SelectorIndex));
				OutError.ActualEvidence = DescriptorValue(FString(SelectorPart));
				return false;
			}
			Selectors.Add(UnescapeKeyPart(SelectorPart.Left(Split)), UnescapeKeyPart(SelectorPart.RightChop(Split + 1)));
		}
		// Decode all borrowed header parts before a destination may reset the
		// source owner (the public full parser permits aliased key/output).
		const FString Pattern = UnescapeKeyPart(Parts[2]);
		const FString ModelId = UnescapeKeyPart(Parts[0]);
		const FString TargetKind = UnescapeKeyPart(Parts[1]);
		return Consume(ModelId, TargetKind, Pattern, Selectors);
	}

	bool ParseCanonicalKey(const FString& CanonicalKey, FCanonicalIdentity& OutIdentity,
		FDiagnostic& OutError, const FRegistry* Registry)
	{
		return ParseCanonicalKeyParts(CanonicalKey, OutError, [&](const FString& ModelId, const FString& Kind,
			const FString& Pattern, TMap<FString, FString>& Selectors) {
			if (!BuildCanonicalIdentityImpl(ModelId, Kind, Selectors, &OutIdentity, OutError, Registry)) return false;
			const bool bPatternMatches = OutIdentity.KeyPattern.Equals(Pattern, ESearchCase::IgnoreCase);
			if (!bPatternMatches || !OutIdentity.CanonicalKey.Equals(CanonicalKey, ESearchCase::IgnoreCase)) {
				FailNoncanonicalKey(CanonicalKey, OutIdentity.CanonicalKey, OutIdentity.ModelId, bPatternMatches, OutError);
				OutIdentity = FCanonicalIdentity(); return false;
			}
			OutError.Reset(); return true;
		});
	}

	bool ValidateCanonicalKey(const FString& CanonicalKey, const FRegistry& Registry, const FModelView& QueryModel,
		FString& OutModelId, FString& OutTargetKind, FDiagnostic& OutError)
	{
		OutModelId.Reset(); OutTargetKind.Reset();
		if (!QueryModel) {
			OutError = IdentityFailure(TEXT("target_identity_model_unavailable"), TEXT("Canonical key validation requires the selected registry model."),
				TEXT("target_identity"), FString(), TEXT("identity_model"));
			return false;
		}
		return ParseCanonicalKeyParts(CanonicalKey, OutError, [&](const FString& ModelId, const FString& Kind,
			const FString& Pattern, TMap<FString, FString>& Selectors) {
			FKeyValidation Validation{CanonicalKey, Pattern, QueryModel, OutModelId, OutTargetKind};
			return BuildCanonicalIdentityImpl(ModelId, Kind, Selectors, nullptr, OutError, &Registry, &Validation);
		});
	}

	void WriteSelectorsFlat(const FCanonicalIdentity& Identity, const TSharedRef<FJsonObject>& Object)
	{
		TArray<FString> Names;
		Identity.Selectors.GetKeys(Names);
		Names.Sort();
		for (const FString& Name : Names)
		{
			Object->SetStringField(Name, Identity.Selectors.FindChecked(Name));
		}
	}
}
