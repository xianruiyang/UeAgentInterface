// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeRWLock.h"

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

		FString EscapeKeyPart(FString Value)
		{
			Value.ReplaceInline(TEXT("%"), TEXT("%25"), ESearchCase::CaseSensitive);
			Value.ReplaceInline(TEXT("|"), TEXT("%7C"), ESearchCase::CaseSensitive);
			Value.ReplaceInline(TEXT("&"), TEXT("%26"), ESearchCase::CaseSensitive);
			Value.ReplaceInline(TEXT("="), TEXT("%3D"), ESearchCase::CaseSensitive);
			Value.ReplaceInline(TEXT("\r"), TEXT("%0D"), ESearchCase::CaseSensitive);
			Value.ReplaceInline(TEXT("\n"), TEXT("%0A"), ESearchCase::CaseSensitive);
			Value.ReplaceInline(TEXT("\t"), TEXT("%09"), ESearchCase::CaseSensitive);
			return Value;
		}

		FString UnescapeKeyPart(FString Value)
		{
			Value.ReplaceInline(TEXT("%09"), TEXT("\t"), ESearchCase::IgnoreCase);
			Value.ReplaceInline(TEXT("%0A"), TEXT("\n"), ESearchCase::IgnoreCase);
			Value.ReplaceInline(TEXT("%0D"), TEXT("\r"), ESearchCase::IgnoreCase);
			Value.ReplaceInline(TEXT("%3D"), TEXT("="), ESearchCase::IgnoreCase);
			Value.ReplaceInline(TEXT("%26"), TEXT("&"), ESearchCase::IgnoreCase);
			Value.ReplaceInline(TEXT("%7C"), TEXT("|"), ESearchCase::IgnoreCase);
			Value.ReplaceInline(TEXT("%25"), TEXT("%"), ESearchCase::IgnoreCase);
			return Value;
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
		return GetValidationDiagnostic().IsEmpty();
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
		if (const FModelDescriptor* Existing = Models.Find(Canonical.ModelId))
		{
			OutError = IdentityFailure(TEXT("conflicting_identity_model"), TEXT("The identity model differs from its existing registration."), TEXT("contract"), Canonical.ModelId, FString());
			OutError.SourceKind = TEXT("native_result");
			if (!ModelsEqual(*Existing, Canonical, &OutError))
			{
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		Models.Add(Canonical.ModelId, MoveTemp(Canonical));
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FRegistry::FindModel(const FString& ModelId, FModelDescriptor& OutDescriptor) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FModelDescriptor* Found = Models.Find(NormalizeId(ModelId)))
		{
			OutDescriptor = *Found;
			return true;
		}
		return false;
	}

	TArray<FModelDescriptor> FRegistry::ListModels() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FModelDescriptor> Result;
		Models.GenerateValueArray(Result);
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

	bool BuildCanonicalIdentity(
		const FString& ModelId,
		const FString& TargetKind,
		const TMap<FString, FString>& Selectors,
		FCanonicalIdentity& OutIdentity,
		FDiagnostic& OutError,
		const FRegistry* Registry)
	{
		OutIdentity = FCanonicalIdentity();
		const FRegistry& ResolvedRegistry = Registry ? *Registry : FRegistry::Get();
		FModelDescriptor Model;
		if (!ResolvedRegistry.FindModel(ModelId, Model))
		{
			OutError = IdentityFailure(TEXT("target_identity_model_unknown"), TEXT("The requested identity model is not registered."), TEXT("target_identity"), ModelId, TEXT("identity_model"));
			OutError.ExpectedValue = DescriptorValue(FString(TEXT("registered identity model")));
			OutError.ActualValue = DescriptorValue(ModelId);
			return false;
		}
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
		for (const TPair<FString, FString>& Pair : Selectors)
		{
			const FString SelectorName = NormalizeId(Pair.Key);
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
			FString SelectorValue = Pair.Value;
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
			CanonicalSelectors.Add(SelectorName, MoveTemp(SelectorValue));
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

		FString Key = EscapeKeyPart(Model.ModelId) + TEXT("|")
			+ EscapeKeyPart(CanonicalTargetKind) + TEXT("|")
			+ EscapeKeyPart(SelectedPattern->PatternId) + TEXT("|");
		for (int32 Index = 0; Index < SelectedPattern->Selectors.Num(); ++Index)
		{
			const FString& SelectorName = SelectedPattern->Selectors[Index];
			const FSelectorDescriptor* SelectorDescriptor = FindSelector(Model, SelectorName);
			FString KeyValue = CanonicalSelectors.FindChecked(SelectorName);
			if (!SelectorDescriptor || !SelectorDescriptor->bCaseSensitive) KeyValue.ToLowerInline();
			if (Index > 0) Key += TEXT("&");
			Key += EscapeKeyPart(SelectorName) + TEXT("=") + EscapeKeyPart(KeyValue);
		}

		OutIdentity.ModelId = Model.ModelId;
		OutIdentity.TargetKind = CanonicalTargetKind;
		OutIdentity.KeyPattern = SelectedPattern->PatternId;
		OutIdentity.CanonicalKey = MoveTemp(Key);
		OutIdentity.Selectors = MoveTemp(CanonicalSelectors);
		OutError.Reset();
		return true;
	}

	bool ParseCanonicalKey(
		const FString& CanonicalKey,
		FCanonicalIdentity& OutIdentity,
		FDiagnostic& OutError,
		const FRegistry* Registry)
	{
		TArray<FString> Parts;
		CanonicalKey.ParseIntoArray(Parts, TEXT("|"), false);
		if (Parts.Num() != 4)
		{
			OutError = IdentityFailure(TEXT("canonical_identity_key_invalid"), TEXT("The canonical identity key must contain four separated parts."), TEXT("target_identity"), FString(), TEXT("canonical_key/part_count"));
			OutError.ExpectedValue = DescriptorValue(4); OutError.ActualValue = DescriptorValue(Parts.Num());
			OutError.ActualEvidence = DescriptorValue(CanonicalKey);
			return false;
		}
		TMap<FString, FString> Selectors;
		TArray<FString> SelectorParts;
		Parts[3].ParseIntoArray(SelectorParts, TEXT("&"), true);
		for (const FString& SelectorPart : SelectorParts)
		{
			FString Name;
			FString Value;
			if (!SelectorPart.Split(TEXT("="), &Name, &Value) || Name.IsEmpty())
			{
				OutError = IdentityFailure(TEXT("canonical_identity_key_selector_invalid"), TEXT("A canonical key selector must have a non-empty name followed by an equals sign."), TEXT("target_identity"), FString(), TEXT("canonical_key/selectors/") + FString::FromInt(static_cast<int32>(&SelectorPart - SelectorParts.GetData())));
				OutError.ActualEvidence = DescriptorValue(SelectorPart);
				return false;
			}
			Selectors.Add(UnescapeKeyPart(Name), UnescapeKeyPart(Value));
		}
		if (!BuildCanonicalIdentity(UnescapeKeyPart(Parts[0]), UnescapeKeyPart(Parts[1]), Selectors, OutIdentity, OutError, Registry))
		{
			return false;
		}
		if (!OutIdentity.KeyPattern.Equals(UnescapeKeyPart(Parts[2]), ESearchCase::IgnoreCase)
			|| !OutIdentity.CanonicalKey.Equals(CanonicalKey, ESearchCase::IgnoreCase))
		{
			OutError = IdentityFailure(TEXT("canonical_identity_key_noncanonical"),
				!OutIdentity.KeyPattern.Equals(UnescapeKeyPart(Parts[2]), ESearchCase::IgnoreCase)
					? TEXT("The canonical key names a different key pattern from the resolved selectors.")
					: TEXT("The supplied identity key differs from its canonical representation."),
				TEXT("target_identity"), OutIdentity.ModelId, TEXT("canonical_key"));
			OutError.ExpectedEvidence = DescriptorValue(OutIdentity.CanonicalKey);
			OutError.ActualEvidence = DescriptorValue(CanonicalKey);
			OutIdentity = FCanonicalIdentity();
			return false;
		}
		OutError.Reset();
		return true;
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
