// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

#include "Dom/JsonObject.h"
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

		FModelDescriptor Normalize(FModelDescriptor Descriptor, FString& OutError)
		{
			Descriptor.ModelId = NormalizeId(MoveTemp(Descriptor.ModelId));
			Descriptor.TargetKind = NormalizeId(MoveTemp(Descriptor.TargetKind));
			TSet<FString> SeenSelectors;
			for (FSelectorDescriptor& SelectorDescriptor : Descriptor.Selectors)
			{
				SelectorDescriptor.Name = NormalizeId(MoveTemp(SelectorDescriptor.Name));
				if (SelectorDescriptor.Name.IsEmpty())
				{
					OutError = TEXT("identity_selector_name_missing");
					return FModelDescriptor();
				}
				if (SeenSelectors.Contains(SelectorDescriptor.Name))
				{
					OutError = FString::Printf(TEXT("identity_selector_duplicate_%s"), *SelectorDescriptor.Name);
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
					OutError = TEXT("identity_key_pattern_invalid");
					return FModelDescriptor();
				}
				if (SeenPatterns.Contains(KeyPattern.PatternId))
				{
					OutError = FString::Printf(TEXT("identity_key_pattern_duplicate_%s"), *KeyPattern.PatternId);
					return FModelDescriptor();
				}
				SeenPatterns.Add(KeyPattern.PatternId);
				TSet<FString> PatternSelectors;
				for (FString& SelectorName : KeyPattern.Selectors)
				{
					SelectorName = NormalizeId(MoveTemp(SelectorName));
					if (!SeenSelectors.Contains(SelectorName))
					{
						OutError = FString::Printf(TEXT("identity_key_selector_unknown_%s"), *SelectorName);
						return FModelDescriptor();
					}
					if (PatternSelectors.Contains(SelectorName))
					{
						OutError = FString::Printf(TEXT("identity_key_selector_duplicate_%s"), *SelectorName);
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

		bool Validate(const FModelDescriptor& Descriptor, FString& OutError)
		{
			if (Descriptor.ModelId.IsEmpty()) OutError = TEXT("identity_model_id_missing");
			else if (Descriptor.Selectors.IsEmpty()) OutError = TEXT("identity_model_selectors_missing");
			else if (Descriptor.KeyPatterns.IsEmpty()) OutError = TEXT("identity_model_key_patterns_missing");
			else
			{
				OutError.Reset();
				return true;
			}
			return false;
		}

		bool RegisterModelChecked(FRegistry& Registry, const FModelDescriptor& Descriptor, FString& OutError)
		{
			ERegistrationResult Result = ERegistrationResult::Registered;
			return Registry.RegisterModel(Descriptor, Result, OutError);
		}
	}

	bool FSelectorDescriptor::operator==(const FSelectorDescriptor& Other) const
	{
		return Name == Other.Name
			&& Priority == Other.Priority
			&& bCaseSensitive == Other.bCaseSensitive
			&& bTrimValue == Other.bTrimValue;
	}

	bool FKeyPattern::operator==(const FKeyPattern& Other) const
	{
		return PatternId == Other.PatternId && Priority == Other.Priority && Selectors == Other.Selectors;
	}

	bool FModelDescriptor::operator==(const FModelDescriptor& Other) const
	{
		return ModelId == Other.ModelId
			&& TargetKind == Other.TargetKind
			&& Selectors == Other.Selectors
			&& KeyPatterns == Other.KeyPatterns;
	}

	bool FCanonicalIdentity::IsValid() const
	{
		return !ModelId.IsEmpty() && !TargetKind.IsEmpty() && !KeyPattern.IsEmpty() && !CanonicalKey.IsEmpty();
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
			FString Error;
			const bool bSuccess = RegisterBuiltInModels(Registry, Error);
			checkf(bSuccess, TEXT("Failed to register built-in authoring identity models: %s"), *Error);
			return bSuccess;
		}();
		(void)bRegistered;
		return Registry;
	}

	bool FRegistry::RegisterModel(const FModelDescriptor& Descriptor, ERegistrationResult& OutResult, FString& OutError)
	{
		FModelDescriptor Canonical = Normalize(Descriptor, OutError);
		if (!OutError.IsEmpty() || !Validate(Canonical, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FModelDescriptor* Existing = Models.Find(Canonical.ModelId))
		{
			if (*Existing != Canonical)
			{
				OutError = FString::Printf(TEXT("conflicting_identity_model_%s"), *Canonical.ModelId);
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

	bool RegisterBuiltInModels(FRegistry& Registry, FString& OutError)
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
			Selector(TEXT("widget_class"), 180), Selector(TEXT("actor_class"), 190),
			Selector(TEXT("native_action_signature"), 200),
			Selector(TEXT("asset_override"), 210),
			Selector(TEXT("actor_factory"), 220),
			Selector(TEXT("placement_category"), 230),
			Selector(TEXT("expression_class"), 240),
			Selector(TEXT("function_path"), 250),
			Selector(TEXT("operation_name"), 260)
		};
		Catalog.KeyPatterns = {
			Pattern(TEXT("catalog_native_action"), 0,
				{ TEXT("catalog_type"), TEXT("native_action_signature") }),
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
		FString& OutError,
		const FRegistry* Registry)
	{
		OutIdentity = FCanonicalIdentity();
		const FRegistry& ResolvedRegistry = Registry ? *Registry : FRegistry::Get();
		FModelDescriptor Model;
		if (!ResolvedRegistry.FindModel(ModelId, Model))
		{
			OutError = FString::Printf(TEXT("target_identity_model_unknown_%s"), *NormalizeId(ModelId));
			return false;
		}
		const FString CanonicalTargetKind = NormalizeId(TargetKind);
		if (CanonicalTargetKind.IsEmpty())
		{
			OutError = TEXT("target_kind_missing");
			return false;
		}
		if (!Model.TargetKind.IsEmpty() && Model.TargetKind != CanonicalTargetKind)
		{
			OutError = FString::Printf(TEXT("target_identity_model_mismatch_%s_%s"), *Model.ModelId, *CanonicalTargetKind);
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
				OutError = FString::Printf(TEXT("target_selector_unknown_%s_%s"), *Model.ModelId, *SelectorName);
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
					OutError = FString::Printf(TEXT("identity_selector_conflict_%s"), *SelectorName);
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
			OutError = FString::Printf(TEXT("identity_key_unresolved_%s"), *Model.ModelId);
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
		FString& OutError,
		const FRegistry* Registry)
	{
		TArray<FString> Parts;
		CanonicalKey.ParseIntoArray(Parts, TEXT("|"), false);
		if (Parts.Num() != 4)
		{
			OutError = TEXT("canonical_identity_key_invalid");
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
				OutError = TEXT("canonical_identity_key_selector_invalid");
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
			OutError = TEXT("canonical_identity_key_noncanonical");
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
